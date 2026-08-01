/*
 * AirPlay 2 Client - Dual-mode streaming
 *
 * Supports two flows based on device capabilities:
 *
 * 1. RAOP-compatible (no --auth): auth-setup + RAOP ANNOUNCE/SETUP
 *    Used for: Sonos, third-party devices without stored credentials
 *    Limitations: 16-bit only
 *
 * 2. Native AP2: HAP pairing + encrypted RTSP + streams SETUP
 *    With --auth: HAP pair-verify with stored credentials (Apple TV, HomePod)
 *    Without --auth: transient pair-setup (Sonos, most third-party receivers)
 *    Supports: 24-bit/48kHz ALAC, encrypted audio
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <inttypes.h>

#include <openssl/rand.h>
#include <openssl/evp.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/src/raop_client.h"
#include "alac_wrapper.h"
#include "cross_util.h"
#include "cross_log.h"
#include "ap2_mrp.h"
#include "ap2_client.h"
#include "ap2_hap.h"
#include "ap2_io.h"
#include "ap2_plist.h"
#include "ap2_bplist.h"
#include "ap2_ptp.h"
#include "raop_session.h"
#include "ap2_timeline.h"

extern log_level *loglevel;

#define AP2_FRAMES_PER_CHUNK 352
#define AP2_CHACHA_TAG_SIZE  16

/* audioFormat codes (bit positions shared with the /info format tables). */
#define AP2_FMT_ALAC_44100_16_2 (1ULL << 18)
#define AP2_FMT_ALAC_44100_24_2 (1ULL << 19)
#define AP2_FMT_ALAC_48000_16_2 (1ULL << 20)
#define AP2_FMT_ALAC_48000_24_2 (1ULL << 21)
#define AP2_RTSP_SETUP_TIMEOUT_MS    8000
#define AP2_RTSP_CONTROL_TIMEOUT_MS  2000
#define AP2_RTSP_FEEDBACK_TIMEOUT_MS 2000
#define AP2_RTSP_METADATA_TIMEOUT_MS 5000
#define AP2_RTSP_ARTWORK_TIMEOUT_MS  15000
#define AP2_FEEDBACK_INTERVAL_MS     2000
/* A single missed keepalive beat must not kill an otherwise healthy session:
 * receivers ride out multi-second local network blackouts (wifi roams, DFS
 * scans) with their buffered audio intact. Consecutive timeout-shaped misses
 * are tolerated up to this count (the final one kills), so a genuinely dead
 * channel is still detected within roughly misses x (interval + timeout). */
#define AP2_FEEDBACK_MAX_CONSECUTIVE_MISSES 3
/* Consecutive feedback ticks the receiver may report an empty stream list
 * while we believe we are streaming before it counts as a fault. The list is
 * legitimately empty for a moment around SETUP/RECORD and while a receiver
 * re-seats a warm splice, so only a sustained streak means the stream really
 * is gone. */
#define AP2_FEEDBACK_IDLE_STREAM_TICKS 3
#define AP2_EVENT_POLL_INTERVAL_MS   100
#define AP2_RTSP_RX_BUF_SIZE         16384
#define AP2_UDP_SEND_TIMEOUT_MS      20
/* Minimum lead for a commanded start: receivers accepted re-anchors down to
 * 150 ms in the flush-ladder measurements; 250 ms leaves margin for the
 * commit round-trips and scheduling jitter. The caller only commands a start
 * after the connection and the audio feed are both confirmed, so no setup
 * slack is needed on top. */
#define AP2_MIN_WARM_LEAD_MS         250
/* Receiver clock readiness, measured from its PTP probe streak (Delay_Req/
 * Pdelay_Req RX tracked per peer by the timing engine or the shared daemon).
 * A third-party receiver seats its render position once, with whatever
 * servo state it has at the anchor instant, and keeps that seat for the
 * whole session — so its anchor must clear the full measured servo-lock
 * window from the streak start (Sonos Era 100: 1.7-2.3 s). Apple receivers
 * converge a still-settling servo onto the anchor and are ready shortly
 * after their third exchange. */
#define AP2_CLOCK_LOCK_MS            2300
#define AP2_CLOCK_SETTLE_MS          250
#define AP2_CLOCK_SEAT_EXCHANGES     3
/* Nothing to measure for this long, counted from the last streak seen or from
 * connect while none ever has been, means the receiver is not answering our
 * clock at all — over four times the 1.078 s first-probe latency measured on a
 * Samsung Music Frame, and well past the full AP2_CLOCK_LOCK_MS servo window
 * on top of it, so a merely slow receiver cannot trip it. That margin is the
 * whole point: this is a diagnosis threshold, not a deadline anything plans
 * against, so it answers "is this receiver definitively not answering" and is
 * deliberately slower than any caller's own wait for a readiness projection. */
#define AP2_CLOCK_STALL_MS           5000
/* Verification needs room to act before the initial fill starts releasing
 * real frames at anchor - pacing depth: anchors closer than the depth plus
 * one poll round plus slack can never be corrected, so they are not armed. */
#define AP2_CLOCK_VERIFY_MIN_WINDOW_MS (AP2_SPLICE_PACING_MS + 500)
/* Receiver queue depth on the splice timeline (§ splice below): the
 * depth IS the audible latency of a warm splice, so it stays shallow. */
#define AP2_SPLICE_PACING_MS         600
/* Delivery pacing depth (§ pacing below). A frame handed over more than the
 * receiver's buffer ahead of its deadline overflows that buffer and is
 * dropped, so delivery runs at most the reported latencyMax less a margin
 * ahead. Receivers that report no latencyMax get the standard AirPlay buffer
 * as the assumed depth, leaving the same margin. Both are held in ms and
 * scaled to the stream rate at use: the frame counts differ per rate. */
#define AP2_PACING_MARGIN_MS         250
#define AP2_PACING_DEFAULT_BUFFER_MS 2000
/* Raw bytes kept from a failed exchange for the auth diagnostics dump: enough
 * for a full header block plus the start of a TLV/plist body. */
#define AP2_DIAG_RESPONSE_MAX        1536
#define AP2_DIAG_BODY_MAX            512
/* Retransmit history. Receivers ask for lost audio on the RTP control port
 * (type 0x55 request / 0x56 response, marker bit set). A request is only worth
 * answering while the packet is still ahead of the receiver's play point, so
 * the ring only has to outlast the send window (1.75 s at its default): 512
 * packets is 4.1 s at 352 frames/packet and 44.1 kHz, comfortably more. */
#define AP2_RTX_RING_SLOTS           512
#define AP2_RTX_MAX_PKT              2048
#define AP2_RTX_CTRL_POLL_MS         200
/* Initial-fill pacing. The window gate alone lets the whole 1.75 s send window
 * go out back-to-back at stream start (measured: ~99 packets in 7 ms), which
 * overruns a receiver's socket buffer and costs a contiguous run of packets it
 * then has to ask back. Spacing releases ~1 ms apart fills the window in about
 * 220 ms — still far ahead of any commanded start lead, but paced enough that
 * the receiver keeps up. Steady state needs one packet per ~8 ms, so this
 * never throttles normal playback. */
#define AP2_FILL_MIN_PACKET_GAP_US   1000

typedef enum {
    FLOW_RAOP_COMPAT = 0,
    FLOW_NATIVE_AP2,
} ap2_flow_t;

/* One sent audio packet kept for retransmission, stored on the wire exactly as
 * it went out (header, ciphertext, tag and nonce suffix) so a resend is a byte
 * copy — re-encrypting would need the original nonce anyway. */
struct ap2_rtx_slot {
    uint16_t seq;
    uint16_t len;
    bool valid;
    uint8_t data[AP2_RTX_MAX_PKT];
};

struct ap2cl_s {
    /* Configuration */
    ap2_device_info_t device;
    ap2_audio_format_t format;
    ap2_state_t state;
    int latency_ms;
    uint32_t dev_latency_min;      /* receiver-reported buffering window (frames) */
    uint32_t dev_latency_max;
    int dev_render_ms;             /* receiver-reported arrival->render latency (ms) */
    int volume;
    ap2_flow_t flow;

    /* Identifiers */
    char *dacp_id;
    char *active_remote;
    char *iface;
    char *publish_ip;         /* address we advertise to the device (multi-homed) */
    struct in_addr bind_addr; /* resolved local bind address (INADDR_ANY if none) */
    char *secret;
    char *password;
    char *et;
    char *md;
    char *am;
    char *auth_credentials;  /* HAP credentials hex (192 chars) */

    /* RAOP-compat flow: libraop client */
    struct raopcl_s *raopcl;

    /* Native AP2 flow */
    int sock_fd;                  /* TCP connection */
    /* The RTSP socket carries whole request/response cycles from multiple
     * threads (streaming thread: TEARDOWN;
     * cmdpipe thread: SET_PARAMETER volume/metadata; feedback worker: keepalive).
     * This lock serializes the cycles so a response cannot be attributed to
     * the wrong request and the HAP nonce sequence stays intact. */
    pthread_mutex_t rtsp_lock;
    atomic_bool rtsp_dead;
    atomic_bool media_healthy;
    bool rtsp_established;
    /* Raw bytes received for a response whose exchange was abandoned by a
     * tolerated /feedback miss. Re-seeded into the next exchange's read so
     * the TCP byte stream (and the HAP read-nonce sequence) stays intact
     * when that response was mid-flight at the deadline. Only touched under
     * rtsp_lock (the exchange serializer). */
    uint8_t rtsp_carry[AP2_RTSP_RX_BUF_SIZE];
    int rtsp_carry_len;
    pthread_t feedback_thread;
    atomic_bool feedback_stop;
    bool feedback_thread_started;
    /* Consecutive /feedback bodies that listed no active stream while the
     * client believed it was streaming. Only the feedback worker touches it. */
    unsigned feedback_idle_streams;
    struct ap2_hap_ctx *hap;      /* HAP encryption context */
    struct ap2_ptp_ctx *ptp;      /* Timing */
    /* MRP now-playing over POST /command (path A, see DESIGN.md §8):
     * state carrier + body builder; only on pair-verified native sessions.
     * mrp_lock protects mutable state/snapshots only. mrp_publish_lock orders
     * mutations with network publication without affecting health reads. */
    struct ap2_mrp_ctx *mrp;
    pthread_mutex_t mrp_lock;
    pthread_mutex_t mrp_publish_lock;
    atomic_int mrp_event_health;
    bool mrp_device_registered;
    bool mrp_extended_registered;
    bool mrp_progress_push_full;  /* receiver rejected the "update" policy;
                                     timeline pushes use the full replace */
    int mrp_last_playback_state;
    ap2_remote_command_cb_t remote_command_cb;
    void *remote_command_userdata;
    int data_sock;                /* UDP audio */
    int ctrl_sock;                /* UDP control */
    int events_sock;              /* reverse TCP events connection (kept open) */
    struct sockaddr_in data_addr;
    struct sockaddr_in ctrl_addr;
    struct alac_codec_s *alac;    /* ALAC encoder for native flow */
    uint8_t audio_key[32];        /* ChaCha20 key for audio encryption */
    uint16_t seq_number;
    uint32_t rtp_timestamp;
    uint32_t ssrc;
    uint64_t head_ts;
    bool first_packet;
    uint64_t audio_packets_sent;
    uint64_t audio_packets_dropped;
    uint64_t sync_packets_sent;
    uint64_t sync_packets_dropped;
    atomic_uint feedback_failures;
    uint64_t timeline_reanchors;
    bool splice_timeline;         /* discard-free warm path on one immutable
                                     anchor line — the native default; false
                                     only for deny-listed receivers (see the
                                     splice comments at flush/resume/standby) */
    uint32_t splice_pad_frames;   /* silence frames still to send before the
                                     next real sample so it lands exactly on
                                     the commanded splice instant */
    uint32_t content_skip_bytes;  /* queued input to discard after a corrected
                                     join, so the retained content lands on
                                     the group timeline (see
                                     ap2cl_content_skip_bytes) */
    bool content_paused;          /* splice pause: the content is paused but
                                     the wire stays fed (state remains
                                     AP2_STREAMING), so the MRP playback state
                                     needs its own truth */
    bool content_stopped;         /* splice standby: the content is stopped
                                     (parked) on the same armed-line shape as
                                     content_paused, published as stopped */
    uint64_t reanchor_shifted_frames; /* cumulative shift since the last start/
                                        * resume, for [STATUS] REANCHOR */
    /* Clock verification of a start committed before the receiver's first
     * timing probe (cold clock). clock_verify_lock guards this block plus
     * the ctrl poller's snapshot; the anchor rebase a correction performs
     * runs on the audio-loop thread under its send lock, serialized against
     * a concurrent commit by this lock (commits disarm before touching the
     * anchor). */
    pthread_mutex_t clock_verify_lock;
    bool clock_verify_armed;
    bool clock_verify_enforce;     /* join start: correct, don't just observe */
    bool start_ack_deferred;       /* the join's [STATUS] started is withheld
                                      for this verification to answer, so the
                                      correction owes no content cut (see
                                      ap2cl_clock_verify_poll) */
    bool start_join_pending;       /* latched by ap2cl_set_start_join */
    bool apple_model;              /* model=/am= names an Apple receiver */
    uint64_t clock_verify_requested_unix_ms;
    uint64_t clock_verify_anchor_unix_ms;
    uint64_t clock_verify_packets_at_arm;
    uint64_t clock_verify_next_poll_ntp;
    bool clock_verify_have_exchange;
    struct ap2_ptp_exchange clock_verify_exchange;
    uint64_t clock_connected_unix_ms; /* session connect instant, which the
                                         receiver's probe window is measured
                                         from (0 until the session is up) */
    uint64_t clock_last_streak_unix_ms; /* last instant a streak was actually
                                           observed, kept current by the
                                           poller; takes over from the connect
                                           instant once it is set */
    /* Shared-daemon mode queries the streak over UDP (blocking); this poller
     * keeps the snapshot fresh so the audio loop never blocks on it. */
    pthread_t clock_verify_thread;
    bool clock_verify_thread_started;
    atomic_bool clock_verify_thread_stop;
    atomic_bool clock_poll_wanted;  /* readiness reporting (ap2cl_clock_readiness)
                                       also reads the snapshot, so in shared mode
                                       the poller runs from connect and outlives
                                       every verification that uses it */
    /* Retransmit history and the control-port reader that serves it. The ring
     * is written by the audio thread and read by the reader thread. */
    struct ap2_rtx_slot *rtx_ring;
    pthread_mutex_t rtx_lock;
    pthread_t rtx_thread;
    atomic_bool rtx_stop;
    bool rtx_thread_started;
    atomic_ullong rtx_requested;   /* packets asked for */
    atomic_ullong rtx_answered;    /* packets resent */
    atomic_ullong rtx_expired;     /* asked for but no longer in the ring */
    /* Initial-fill pacing: monotonic microseconds of the last release. */
    uint64_t pace_last_release_us;

    /* Frozen realtime anchor line (PTP): the rtp<->wall mapping is fixed once
     * at stream start and every periodic time-announce extrapolates along it.
     * Re-deriving the anchor from the send head each time makes consecutive
     * anchors disagree (the head races ahead during the initial buffer fill
     * and wobbles with pipe pacing), and each inconsistent re-anchor makes
     * the receiver re-seat its timeline and drop its buffer. */
    bool rt_anchor_valid;
    uint64_t rt_anchor_wall0;      /* master-clock ns of the anchor point */
    uint32_t rt_anchor_pos0;       /* rtp timestamp at the anchor point */
    uint64_t start_ntp;            /* shared group start (NTP fixed-point), 0 = none */
    _Atomic uint32_t rtp_offset;   /* per-process timeline offset (see start_at) */
    uint64_t audio_nonce_counter;
    /* Requested audioFormat and the /info-advertised per-stream format tables
     * (bit masks in the audioFormat bit space; "known" = the table was present,
     * "extended" = from supportedAudioFormatsExtended vs the legacy mask). */
    uint64_t audio_format;
    uint64_t realtime_formats;
    uint64_t buffered_formats;
    bool realtime_formats_known;
    bool buffered_formats_known;
    bool realtime_formats_extended;
    bool buffered_formats_extended;
    char session_url[128];
    char session_uuid[40];
    char group_uuid[40];
    uint32_t session_id;
    int cseq;

    /* Outcome of the last connect attempt, surfaced through
     * ap2cl_connect_error() so the caller can report why it failed. */
    ap2_connect_error_t connect_error;
    int connect_http_status;
    char connect_detail[192];
    /* Raw bytes of the most recent non-200 RTSP response, kept for the auth
     * diagnostics dump. Written only from inside an RTSP exchange, which the
     * rtsp_lock serializes. */
    uint8_t last_error_response[AP2_DIAG_RESPONSE_MAX];
    int last_error_response_len;

    /* PTP timing selection */
    bool use_ptp;      /* resolved: PTP grandmaster timing active this session */
    bool ptp_forced;   /* ap2cl_set_ptp() was called (overrides auto-detect) */
    bool ptp_enabled;  /* value passed to ap2cl_set_ptp() */
    bool ptp_shared;   /* prefer a shared PTP daemon clock (multi-room) if present */
};

static int ap2_mrp_send_playback_state(struct ap2cl_s *p,
                                       ap2_mrp_playback_state_t state,
                                       bool force);
static void ap2_mrp_publish_playback(struct ap2cl_s *p,
                                     ap2_mrp_playback_state_t state,
                                     bool force);
static bool ap2_set_nonblocking(int fd, const char *name);
static void ap2_raop_session_cleanup(struct ap2cl_s *p);
static void ap2_clock_verify_disarm(struct ap2cl_s *p);
static void ap2_clock_verify_reap_poller(struct ap2cl_s *p);
static void ap2_clock_poller_ensure(struct ap2cl_s *p);
static uint64_t ap2_ntp_to_unix_ms(uint64_t ntp);

static void ap2_remote_command_received(
    ap2_remote_command_t command, void *userdata)
{
    struct ap2cl_s *p = userdata;
    if (p && p->remote_command_cb)
        p->remote_command_cb(command, p->remote_command_userdata);
}

/* ---- Native AP2 failure reporting ---- */

/* True for the statuses a receiver uses to refuse an unauthenticated or
 * wrongly-authenticated request. */
static bool ap2_status_is_auth(int status)
{
    return status == 401 || status == 403;
}

/* An auth-shaped rejection means "supply a password" when we presented none,
 * and "this password is wrong" when we did. */
static ap2_connect_error_t ap2_auth_error_kind(struct ap2cl_s *p)
{
    return (p->password && *p->password) ? AP2_CONNECT_ERROR_AUTH_FAILED
                                         : AP2_CONNECT_ERROR_AUTH_REQUIRED;
}

static void ap2_set_connect_error(struct ap2cl_s *p, ap2_connect_error_t kind,
                                  int http_status, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void ap2_set_connect_error(struct ap2cl_s *p, ap2_connect_error_t kind,
                                  int http_status, const char *fmt, ...)
{
    p->connect_error = kind;
    p->connect_http_status = http_status;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->connect_detail, sizeof(p->connect_detail), fmt, args);
    va_end(args);
}

/* Keep the raw bytes of a non-200 response for the diagnostics dump; a 200
 * clears them so a later failure can never dump a stale exchange. */
static void ap2_capture_response(struct ap2cl_s *p, const uint8_t *msg,
                                 size_t len, int status)
{
    if (status == 200) {
        p->last_error_response_len = 0;
        return;
    }
    size_t keep = len < sizeof(p->last_error_response)
                      ? len : sizeof(p->last_error_response);
    memcpy(p->last_error_response, msg, keep);
    p->last_error_response_len = (int)keep;
}

/*
 * Log what the receiver answered on a failed native exchange: the status line,
 * every header and a bounded body dump. For a 401 the questions that matter
 * are whether a WWW-Authenticate challenge came with it and what the body
 * carries, so the challenge is also returned for the caller's error detail.
 */
static void ap2_log_failed_response(struct ap2cl_s *p, const char *label,
                                    int status, char *challenge,
                                    size_t challenge_size)
{
    if (challenge && challenge_size) snprintf(challenge, challenge_size, "%s", "(absent)");
    if (p->last_error_response_len <= 0) {
        LOG_ERROR("[AP2] %s -> %d (no response body captured)", label, status);
        return;
    }
    size_t len = (size_t)p->last_error_response_len;
    char dump[1600];
    ap2_io_format_response_dump(p->last_error_response, len,
                                AP2_DIAG_BODY_MAX, dump, sizeof(dump));
    LOG_ERROR("[AP2] %s -> %d response: %s", label, status, dump);
    /* the lookup clears the buffer on a miss, so restore the sentinel then */
    if (challenge && challenge_size &&
        !ap2_io_header_value(p->last_error_response, len, "WWW-Authenticate",
                             challenge, challenge_size))
        snprintf(challenge, challenge_size, "%s", "(absent)");
}

/* Report a non-200 on the native connect path: dump the exchange and classify
 * it, so the caller can tell a rejected password from a broken device. */
static void ap2_report_failed_exchange(struct ap2cl_s *p, const char *label,
                                       int status)
{
    char challenge[96];
    ap2_log_failed_response(p, label, status, challenge, sizeof(challenge));
    ap2_set_connect_error(p,
                          ap2_status_is_auth(status)
                              ? ap2_auth_error_kind(p)
                              : AP2_CONNECT_ERROR_GENERIC,
                          status, "%s -> %d; WWW-Authenticate: %s", label,
                          status, challenge);
}

/* ---- Native AP2 RTSP I/O ---- */

static void ap2_mark_rtsp_dead(struct ap2cl_s *p, const char *method,
                               const char *uri, const char *phase,
                               uint64_t elapsed_ms)
{
    int saved_errno = errno;
    unsigned prior_misses = atomic_load(&p->feedback_failures);
    if (ap2_io_feedback_miss_tolerated(uri, saved_errno, prior_misses,
                                       AP2_FEEDBACK_MAX_CONSECUTIVE_MISSES)) {
        /* The device may just be riding out a short local network blackout
         * with its buffered audio intact. Leave the channel open — the next
         * exchange skips this beat's late response by CSeq — and only give
         * up after the consecutive-miss budget is spent. */
        LOG_WARN("[AP2] POST /feedback keepalive miss %u/%d (%s after %" PRIu64
                 "ms: %s); tolerating transient failure",
                 prior_misses + 1, AP2_FEEDBACK_MAX_CONSECUTIVE_MISSES, phase,
                 elapsed_ms, strerror(saved_errno));
        errno = saved_errno;
        return;
    }
    if ((p->rtsp_established || p->hap) &&
        !atomic_exchange(&p->rtsp_dead, true)) {
        LOG_ERROR("[AP2] RTSP channel failed during %s %s %s after %" PRIu64
                  "ms: %s; terminating native session",
                  method, uri, phase, elapsed_ms, strerror(saved_errno));
        shutdown(p->sock_fd, SHUT_RDWR);
    }
    errno = saved_errno;
}

static int ap2_rtsp_timeout_ms(struct ap2cl_s *p, const char *method,
                               const char *uri, const char *content_type)
{
    if (!p->rtsp_established) return AP2_RTSP_SETUP_TIMEOUT_MS;
    if (!strcmp(uri, "/feedback")) return AP2_RTSP_FEEDBACK_TIMEOUT_MS;
    if (content_type && !strncasecmp(content_type, "image/", 6))
        return AP2_RTSP_ARTWORK_TIMEOUT_MS;
    if (!strcmp(uri, "/command") || !strcmp(method, "SET_PARAMETER"))
        return AP2_RTSP_METADATA_TIMEOUT_MS;
    return AP2_RTSP_CONTROL_TIMEOUT_MS;
}

static int ap2_rtsp_send_ex_unlocked(struct ap2cl_s *p, const char *method, const char *uri,
                          const uint8_t *body, int body_len, const char *ct,
                          const char *extra_hdr,
                          uint8_t **resp_body, int *resp_len,
                          uint64_t started_ms, uint64_t deadline_ms,
                          bool *request_started)
{
    if (atomic_load(&p->rtsp_dead)) {
        *resp_body = NULL;
        *resp_len = 0;
        return 0;
    }
    if (ap2_io_monotonic_ms() >= deadline_ms) {
        *resp_body = NULL;
        *resp_len = 0;
        errno = ETIMEDOUT;
        return -ETIMEDOUT;
    }
    if (request_started) *request_started = true;
    int cseq = p->cseq++;
    char hdr[1024];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: AirPlay/670.6.2\r\n"
        "DACP-ID: %s\r\nActive-Remote: %s\r\n%s%s%s%s"
        "Content-Length: %d\r\n\r\n",
        method, uri, cseq,
        p->dacp_id ? p->dacp_id : "0",
        p->active_remote ? p->active_remote : "0",
        ct ? "Content-Type: " : "", ct ? ct : "", ct ? "\r\n" : "",
        extra_hdr ? extra_hdr : "",
        body_len);
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(hdr)) {
        errno = EMSGSIZE;
        ap2_mark_rtsp_dead(p, method, uri, "construct", 0);
        return 0;
    }

    uint8_t *msg = NULL;
    int msg_len = 0;

    if (p->hap) {
        /* Encrypt RTSP via HAP framing */
        int raw_len = hdr_len + body_len;
        uint8_t *raw = malloc(raw_len);
        if (!raw) {
            errno = ENOMEM;
            ap2_mark_rtsp_dead(p, method, uri, "allocate", 0);
            return 0;
        }
        memcpy(raw, hdr, hdr_len);
        if (body && body_len > 0) memcpy(raw + hdr_len, body, body_len);
        msg_len = ap2_hap_encrypt(p->hap, raw, raw_len, &msg);
        free(raw);
        if (msg_len <= 0) {
            free(msg);
            errno = EPROTO;
            ap2_mark_rtsp_dead(p, method, uri, "encrypt", 0);
            return 0;
        }
    } else {
        msg_len = hdr_len + body_len;
        msg = malloc(msg_len);
        if (!msg) {
            errno = ENOMEM;
            ap2_mark_rtsp_dead(p, method, uri, "allocate", 0);
            return 0;
        }
        memcpy(msg, hdr, hdr_len);
        if (body && body_len > 0) memcpy(msg + hdr_len, body, body_len);
    }

    LOG_DEBUG("[AP2] RTSP TX cseq=%d %s %s body=%d wire=%d timeout=%dms",
              cseq, method, uri, body_len, msg_len,
              (int)(deadline_ms - started_ms));
    if (!ap2_io_write_all_deadline(p->sock_fd, msg, msg_len, deadline_ms)) {
        /* mark first: it keys off errno from the failed write, which a
         * free() in between is not guaranteed to preserve */
        ap2_mark_rtsp_dead(p, method, uri, "write",
                           ap2_io_monotonic_ms() - started_ms);
        free(msg);
        return 0;
    }
    free(msg);

    /* Read encrypted response.
     * HAP framing: [2-byte LE length][encrypted chunk + 16-byte tag]
     * We accumulate raw bytes, then decrypt complete frames.
     * The accumulation is seeded with bytes carried over from an exchange
     * abandoned by a tolerated /feedback miss, so a response that was
     * mid-flight at that deadline keeps the byte stream intact; responses
     * to such abandoned requests are then skipped by CSeq below. */
    uint8_t buf[AP2_RTSP_RX_BUF_SIZE] = {0};
    int total = 0;
    if (p->rtsp_carry_len > 0) {
        memcpy(buf, p->rtsp_carry, (size_t)p->rtsp_carry_len);
        total = p->rtsp_carry_len;
        p->rtsp_carry_len = 0;
    }
    if (!p->hap) {
        while (total < (int)sizeof(buf)) {
            int n = (int)ap2_io_read_deadline(
                p->sock_fd, buf + total, sizeof(buf) - (size_t)total,
                deadline_ms);
            if (n <= 0) break;
            total += n;
            ap2_rtsp_response_t parsed;
            size_t match_offset = 0;
            unsigned discarded = 0;
            int match_status = ap2_io_match_rtsp_response(
                buf, (size_t)total, cseq, &parsed, &match_offset, &discarded);
            if (match_status < 0) {
                errno = EPROTO;
                break;
            }
            if (match_status > 0) {
                if (discarded)
                    LOG_INFO("[AP2] %s %s: discarded %u stale response(s) "
                             "from tolerated keepalive misses",
                             method, uri, discarded);
                ap2_capture_response(p, buf + match_offset, parsed.message_len,
                                     parsed.status);
                *resp_len = (int)parsed.body_len;
                *resp_body = parsed.body_len ? malloc(parsed.body_len) : NULL;
                if (parsed.body_len && !*resp_body) {
                    errno = ENOMEM;
                    break;
                }
                if (*resp_body)
                    memcpy(*resp_body, buf + match_offset + parsed.header_len,
                           parsed.body_len);
                return parsed.status;
            }
        }
        if (total >= (int)sizeof(buf)) errno = EMSGSIZE;
        ap2_mark_rtsp_dead(p, method, uri, "read",
                           ap2_io_monotonic_ms() - started_ms);
        if (!atomic_load(&p->rtsp_dead) && total > 0) {
            memcpy(p->rtsp_carry, buf, (size_t)total);
            p->rtsp_carry_len = total;
        }
        *resp_body = NULL;
        *resp_len = 0;
        return 0;
    }

    while (total < (int)sizeof(buf)) {
        int n = (int)ap2_io_read_deadline(
            p->sock_fd, buf + total, sizeof(buf) - (size_t)total,
            deadline_ms);
        if (n <= 0) break;
        total += n;
        if (total < 2) continue;

        int frame_len = buf[0] | (buf[1] << 8);
        if (total < 2 + frame_len + AP2_CHACHA_TAG_SIZE) continue;

        uint8_t *dec = NULL;
        uint64_t saved_counter = ap2_hap_save_read_counter(p->hap);
        int dec_len = ap2_hap_decrypt(p->hap, buf, total, &dec);
        if (dec_len > 0 && dec) {
            ap2_rtsp_response_t parsed;
            size_t match_offset = 0;
            unsigned discarded = 0;
            int match_status = ap2_io_match_rtsp_response(
                dec, (size_t)dec_len, cseq, &parsed, &match_offset,
                &discarded);
            if (match_status < 0) {
                free(dec);
                errno = EPROTO;
                break;
            }
            if (match_status > 0) {
                if (discarded)
                    LOG_INFO("[AP2] %s %s: discarded %u stale response(s) "
                             "from tolerated keepalive misses",
                             method, uri, discarded);
                ap2_capture_response(p, dec + match_offset, parsed.message_len,
                                     parsed.status);
                *resp_len = (int)parsed.body_len;
                *resp_body = parsed.body_len ? malloc(parsed.body_len) : NULL;
                if (parsed.body_len && !*resp_body) {
                    free(dec);
                    errno = ENOMEM;
                    break;
                }
                if (*resp_body)
                    memcpy(*resp_body, dec + match_offset + parsed.header_len,
                           parsed.body_len);
                int status = parsed.status;
                free(dec);
                return status;
            }
            free(dec);
            ap2_hap_restore_read_counter(p->hap, saved_counter);
        } else {
            free(dec);
            ap2_hap_restore_read_counter(p->hap, saved_counter);
        }
    }

    if (total >= (int)sizeof(buf)) errno = EMSGSIZE;
    ap2_mark_rtsp_dead(p, method, uri, "read",
                       ap2_io_monotonic_ms() - started_ms);
    if (!atomic_load(&p->rtsp_dead) && total > 0) {
        memcpy(p->rtsp_carry, buf, (size_t)total);
        p->rtsp_carry_len = total;
    }
    *resp_body = NULL;
    *resp_len = 0;
    return 0;
}

/* Serialize the full request/response cycle (see the rtsp_lock field note);
 * taking the lock here covers every RTSP caller, whichever thread it runs on. */
static int ap2_rtsp_send_ex_tracked(
    struct ap2cl_s *p, const char *method, const char *uri,
    const uint8_t *body, int body_len, const char *ct, const char *extra_hdr,
    uint8_t **resp_body, int *resp_len, bool *request_started)
{
    if (request_started) *request_started = false;
    int timeout_ms = ap2_rtsp_timeout_ms(p, method, uri, ct);
    uint64_t started_ms = ap2_io_monotonic_ms();
    uint64_t deadline_ms = started_ms + (uint64_t)timeout_ms;
    int lock_status =
        ap2_io_mutex_lock_deadline(&p->rtsp_lock, deadline_ms);
    uint64_t waited_ms = ap2_io_monotonic_ms() - started_ms;
    if (lock_status <= 0) {
        int lock_errno = errno;
        *resp_body = NULL;
        *resp_len = 0;
        if (lock_status == 0) {
            LOG_SDEBUG("[AP2] RTSP %s %s control lock timed out after "
                      "%" PRIu64 "ms (budget=%dms)",
                      method, uri, waited_ms, timeout_ms);
            errno = ETIMEDOUT;
            return -ETIMEDOUT;
        }
        LOG_ERROR("[AP2] RTSP %s %s control lock failed after %" PRIu64
                  "ms: %s",
                  method, uri, waited_ms, strerror(lock_errno));
        errno = lock_errno;
        return -lock_errno;
    }
    LOG_SDEBUG("[AP2] RTSP %s %s acquired control lock after %" PRIu64
               "ms (budget=%dms)",
               method, uri, waited_ms, timeout_ms);
    int status = ap2_rtsp_send_ex_unlocked(p, method, uri, body, body_len, ct,
                                           extra_hdr, resp_body, resp_len,
                                           started_ms, deadline_ms,
                                           request_started);
    pthread_mutex_unlock(&p->rtsp_lock);
    return status;
}

static int ap2_rtsp_send_ex(struct ap2cl_s *p, const char *method,
                            const char *uri, const uint8_t *body, int body_len,
                            const char *ct, const char *extra_hdr,
                            uint8_t **resp_body, int *resp_len)
{
    return ap2_rtsp_send_ex_tracked(
        p, method, uri, body, body_len, ct, extra_hdr,
        resp_body, resp_len, NULL);
}

/* Convenience wrapper: send an RTSP request with no extra headers. */
static int ap2_rtsp_send(struct ap2cl_s *p, const char *method, const char *uri,
                         const uint8_t *body, int body_len, const char *ct,
                         uint8_t **resp_body, int *resp_len)
{
    return ap2_rtsp_send_ex(p, method, uri, body, body_len, ct, NULL,
                            resp_body, resp_len);
}

static int ap2_rtsp_send_tracked(
    struct ap2cl_s *p, const char *method, const char *uri,
    const uint8_t *body, int body_len, const char *ct,
    uint8_t **resp_body, int *resp_len, bool *request_started)
{
    return ap2_rtsp_send_ex_tracked(
        p, method, uri, body, body_len, ct, NULL,
        resp_body, resp_len, request_started);
}

/* Keep a sent packet available for retransmission. Called on the audio thread
 * right after the packet goes out; the ring is indexed by sequence number so a
 * request is a direct lookup and old entries retire on their own. */
static void ap2_rtx_store(struct ap2cl_s *p, uint16_t seq,
                          const uint8_t *pkt, int len)
{
    if (!p->rtx_ring || len <= 0 || len > AP2_RTX_MAX_PKT) return;
    struct ap2_rtx_slot *slot = &p->rtx_ring[seq % AP2_RTX_RING_SLOTS];
    pthread_mutex_lock(&p->rtx_lock);
    slot->seq = seq;
    slot->len = (uint16_t)len;
    slot->valid = true;
    memcpy(slot->data, pkt, (size_t)len);
    pthread_mutex_unlock(&p->rtx_lock);
}

/* Answer one requested sequence number. The response is the classic RAOP
 * resend: a 4-byte control header (marker | type 0x56, echoing the request's
 * own sequence) wrapping the original packet verbatim. */
static bool ap2_rtx_resend(struct ap2cl_s *p, uint16_t seq, uint16_t req_seq,
                           const struct sockaddr_in *to)
{
    if (!p->rtx_ring) return false;
    uint8_t out[4 + AP2_RTX_MAX_PKT];
    int len = 0;
    pthread_mutex_lock(&p->rtx_lock);
    struct ap2_rtx_slot *slot = &p->rtx_ring[seq % AP2_RTX_RING_SLOTS];
    if (slot->valid && slot->seq == seq) {
        len = slot->len;
        memcpy(out + 4, slot->data, (size_t)len);
    }
    pthread_mutex_unlock(&p->rtx_lock);
    if (!len) {
        atomic_fetch_add(&p->rtx_expired, 1);
        return false;
    }
    out[0] = 0x80;
    out[1] = 0xd6;
    out[2] = (uint8_t)(req_seq >> 8);
    out[3] = (uint8_t)(req_seq & 0xFF);
    /* Answer where the request came from rather than the configured control
     * address: it is the same endpoint for a well-behaved receiver, and it
     * keeps working if the device asks from a different source port. */
    ap2_send_result_t r = ap2_io_send_datagram_deadline(
        p->ctrl_sock, out, (size_t)(len + 4),
        (const struct sockaddr *)to, sizeof(*to),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    if (r != AP2_SEND_SENT) return false;
    atomic_fetch_add(&p->rtx_answered, 1);
    return true;
}

/* Serve the RTP control port: receivers report lost audio there and wait for
 * it to come back. Left unread the requests simply pile up in the socket
 * buffer and the receiver keeps a permanent hole in its stream. */
static void *ap2_rtx_thread_main(void *arg)
{
    struct ap2cl_s *p = arg;
    uint64_t last_report = ap2_io_monotonic_ms();
    LOG_DEBUG("[AP2] retransmit responder started (ring=%d packets)",
              AP2_RTX_RING_SLOTS);
    while (!atomic_load(&p->rtx_stop)) {
        struct pollfd pfd = {.fd = p->ctrl_sock, .events = POLLIN};
        int pr = poll(&pfd, 1, AP2_RTX_CTRL_POLL_MS);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 256; i++) {
                uint8_t buf[512];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                /* MSG_DONTWAIT rather than relying on the socket's flags: the
                 * loop must always fall back to the poll so a stop request is
                 * seen within one poll interval. */
                ssize_t n = recvfrom(p->ctrl_sock, buf, sizeof(buf),
                                     MSG_DONTWAIT,
                                     (struct sockaddr *)&from, &from_len);
                if (n < 8) break;
                /* Type 0x55 with the marker bit: [2:4] the request's own
                 * sequence, [4:6] first missing RTP sequence, [6:8] count. */
                if ((buf[1] & 0x7f) != 0x55) continue;
                uint16_t req_seq = (uint16_t)((buf[2] << 8) | buf[3]);
                uint16_t first = (uint16_t)((buf[4] << 8) | buf[5]);
                uint16_t count = (uint16_t)((buf[6] << 8) | buf[7]);
                if (!count || count > AP2_RTX_RING_SLOTS)
                    count = count ? AP2_RTX_RING_SLOTS : 1;
                atomic_fetch_add(&p->rtx_requested, count);
                for (uint16_t k = 0; k < count; k++)
                    ap2_rtx_resend(p, (uint16_t)(first + k), req_seq, &from);
            }
        }
        uint64_t now = ap2_io_monotonic_ms();
        unsigned long long asked = atomic_load(&p->rtx_requested);
        if (asked && now - last_report >= 5000) {
            last_report = now;
            LOG_DEBUG("[AP2] retransmit: %llu requested, %llu resent, "
                      "%llu already retired",
                      asked, atomic_load(&p->rtx_answered),
                      atomic_load(&p->rtx_expired));
        }
    }
    LOG_DEBUG("[AP2] retransmit responder stopped (%llu requested, %llu resent, "
              "%llu already retired)",
              atomic_load(&p->rtx_requested), atomic_load(&p->rtx_answered),
              atomic_load(&p->rtx_expired));
    return NULL;
}

static bool ap2_rtx_start(struct ap2cl_s *p)
{
    if (p->ctrl_sock < 0) return false;
    p->rtx_ring = calloc(AP2_RTX_RING_SLOTS, sizeof(*p->rtx_ring));
    if (!p->rtx_ring) {
        LOG_ERROR("[AP2] Cannot allocate retransmit ring");
        return false;
    }
    atomic_store(&p->rtx_stop, false);
    int err = pthread_create(&p->rtx_thread, NULL, ap2_rtx_thread_main, p);
    if (err) {
        LOG_ERROR("[AP2] Cannot start retransmit responder: %s", strerror(err));
        free(p->rtx_ring);
        p->rtx_ring = NULL;
        return false;
    }
    p->rtx_thread_started = true;
    return true;
}

static void ap2_rtx_stop(struct ap2cl_s *p)
{
    if (!p) return;
    if (p->rtx_thread_started) {
        atomic_store(&p->rtx_stop, true);
        pthread_join(p->rtx_thread, NULL);
        p->rtx_thread_started = false;
    }
    free(p->rtx_ring);
    p->rtx_ring = NULL;
}

static void ap2_service_mrp_input(struct ap2cl_s *p)
{
    if (!p || atomic_load(&p->rtsp_dead)) return;
    pthread_mutex_lock(&p->mrp_lock);
    struct ap2_mrp_ctx *mrp = p->mrp;
    pthread_mutex_unlock(&p->mrp_lock);
    if (!mrp) return;

    ap2_mrp_tick(mrp);
    pthread_mutex_lock(&p->mrp_lock);
    if (p->mrp == mrp)
        atomic_store(&p->mrp_event_health, ap2_mrp_event_status(mrp));
    pthread_mutex_unlock(&p->mrp_lock);
}

static void *ap2_feedback_thread_main(void *arg)
{
    struct ap2cl_s *p = arg;
    uint64_t last_tick_ms = ap2_io_monotonic_ms();
    uint64_t next_tick_ms = last_tick_ms + AP2_FEEDBACK_INTERVAL_MS;

    LOG_DEBUG("[AP2] feedback worker started (interval=%dms)",
              AP2_FEEDBACK_INTERVAL_MS);
    while (!atomic_load(&p->feedback_stop) &&
           !atomic_load(&p->rtsp_dead)) {
        ap2_service_mrp_input(p);
        uint64_t now_ms = ap2_io_monotonic_ms();
        if (now_ms < next_tick_ms) {
            uint64_t sleep_ms = next_tick_ms - now_ms;
            if (sleep_ms > AP2_EVENT_POLL_INTERVAL_MS)
                sleep_ms = AP2_EVENT_POLL_INTERVAL_MS;
            usleep((useconds_t)sleep_ms * 1000);
            continue;
        }

        uint64_t gap_ms = now_ms - last_tick_ms;
        LOG_DEBUG("[AP2] feedback tick gap=%" PRIu64 "ms", gap_ms);
        if (gap_ms > AP2_FEEDBACK_INTERVAL_MS + 500)
            LOG_WARN("[AP2] feedback cadence delayed: %" PRIu64 "ms since prior tick",
                     gap_ms);
        ap2cl_feedback(p);
        last_tick_ms = ap2_io_monotonic_ms();
        next_tick_ms += AP2_FEEDBACK_INTERVAL_MS;
        if (next_tick_ms <= last_tick_ms)
            next_tick_ms = last_tick_ms + AP2_FEEDBACK_INTERVAL_MS;
    }
    LOG_DEBUG("[AP2] feedback worker stopped (dead=%d)",
              atomic_load(&p->rtsp_dead) ? 1 : 0);
    return NULL;
}

static bool ap2_feedback_start(struct ap2cl_s *p)
{
    atomic_store(&p->feedback_stop, false);
    int err = pthread_create(&p->feedback_thread, NULL,
                             ap2_feedback_thread_main, p);
    if (err != 0) {
        LOG_ERROR("[AP2] Cannot start feedback worker: %s", strerror(err));
        return false;
    }
    p->feedback_thread_started = true;
    return true;
}

static void ap2_feedback_stop(struct ap2cl_s *p)
{
    if (!p || !p->feedback_thread_started) return;
    atomic_store(&p->feedback_stop, true);
    pthread_join(p->feedback_thread, NULL);
    p->feedback_thread_started = false;
}

/* ---- Native AP2 connect helpers ---- */

/* Format a random RFC-4122-shaped UUID string (uppercase, 36 chars + NUL). */
static void ap2_gen_uuid(char out[37])
{
    uint8_t u[16];
    RAND_bytes(u, 16);
    snprintf(out, 37,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Parse up to 8 bytes from a hex identifier string (e.g. the 16-char DACP ID).
 * Returns the number of bytes parsed. */
static int ap2_dacp_bytes(const char *dacp, uint8_t out[8])
{
    int n = 0;
    if (!dacp) return 0;
    int len = (int)strlen(dacp);
    for (int i = 0; i + 1 < len && n < 8; i += 2) {
        unsigned int v;
        if (sscanf(dacp + i, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* Format bytes as an uppercase colon-separated hex string ("1A:2B:..."). */
static void ap2_colon_hex(const uint8_t *b, int n, char *out)
{
    int di = 0;
    for (int i = 0; i < n; i++) {
        di += sprintf(out + di, "%02X", b[i]);
        if (i < n - 1) out[di++] = ':';
    }
    out[di] = '\0';
}

/* mDNS features bitmask bits (features = (HIGH<<32)|LOW). */
#define AP2_FEAT(f, n)          (((f) >> (n)) & 1ULL)
#define AP2_FEAT_UNIFIED_MEDIA  38  /* SupportsUnifiedMediaControl -> AirPlay 2 */
#define AP2_FEAT_BUFFERED       40  /* SupportsBufferedAudio (type 103) */
#define AP2_FEAT_PTP            41  /* SupportsPTP */
#define AP2_FEAT_HK_PAIRING     46  /* SupportsHKPairingAndAccessControl */
#define AP2_FEAT_COREUTILS      48  /* SupportsCoreUtilsPairingAndEncryption -> AirPlay 2 */

/* mDNS status flags (sf/flags) bits. */
#define AP2_SF_PIN_REQUIRED     0x8ULL
#define AP2_SF_LEGACY_PAIRING   0x200ULL

/* Parse a "key=0x..[,0x..]" hex field out of a TXT blob into a 64-bit value.
 * key1/key2 are the two accepted spellings (e.g. "features=" and "ft="). The
 * features field carries two comma-separated 32-bit halves (LOW,HIGH) that fold
 * into (HIGH<<32)|LOW; single-value fields (flags/sf) parse the first token. */
static uint64_t ap2_txt_hex_field(const char *txt, const char *key1, const char *key2)
{
    if (!txt) return 0;
    const char *f = strstr(txt, key1);
    if (f) f += strlen(key1);
    else if (key2 && (f = strstr(txt, key2))) f += strlen(key2);
    else return 0;

    unsigned long long low = 0, high = 0;
    int n = sscanf(f, "%llx,%llx", &low, &high);
    if (n == 2) return ((uint64_t)high << 32) | (uint32_t)low;
    if (n == 1) return (uint64_t)low;
    return 0;
}

uint64_t ap2_txt_features(const char *txt)
{
    return ap2_txt_hex_field(txt, "features=", "ft=");
}

uint64_t ap2_txt_flags(const char *txt)
{
    return ap2_txt_hex_field(txt, "flags=", "sf=");
}

/* True if the mDNS TXT blob advertises SupportsPTP (features bit 41). */
static bool ap2_features_has_ptp(const char *txt)
{
    return AP2_FEAT(ap2_txt_features(txt), AP2_FEAT_PTP) != 0;
}

/* Splice-timeline deny-list: receivers that need the classic flush +
 * re-anchor warm path instead of the splice timeline (§DESIGN.md). The
 * splice timeline is the default for every native session: Apple's own
 * receivers REQUIRE it (their realtime lane emits a short noise burst on any
 * buffer discard, anchor re-announce, or late-frame delivery — measured A/B
 * on an Apple TV 4K, tvOS 27, 2026-07-30; the only clean warm transitions
 * were a natural drain and a bitstream-continuous splice), and the
 * 2026-07-31 fleet A/B cleared the third-party park (Sonos Era 100 pair and
 * solo, Sonos Bookshelf, WiiM Pro, Edifier MS50A, Samsung HW-LS60D:
 * cold/seek/next/pause/park/keepalive/late-join/group runs, delivery-stall
 * ladders) on the same mechanism. Add a `model=` (_airplay TXT) / `am=`
 * (_raop) prefix here only when a receiver measures splice-hostile on
 * hardware. */
static bool ap2_splice_denied(const char *txt, const char *am)
{
    static const char *const prefixes[] = { NULL };
    const char *model = txt ? strstr(txt, "model=") : NULL;
    if (model) model += strlen("model=");
    for (size_t i = 0; prefixes[i]; i++) {
        size_t len = strlen(prefixes[i]);
        if (model && strncmp(model, prefixes[i], len) == 0) return true;
        if (am && strncmp(am, prefixes[i], len) == 0) return true;
    }
    return false;
}

/* Apple receivers, from the `model=` (_airplay TXT) / `am=` (_raop) field.
 * They converge a still-settling PTP servo onto a committed anchor (solo
 * Apple TV starts render exactly on anchors well inside the servo-lock
 * window), so clock readiness grants them the fast observed bound; a
 * third-party receiver keeps whatever seat it takes at the anchor instant. */
static bool ap2_apple_model(const char *txt, const char *am)
{
    static const char *const prefixes[] = {
        "AppleTV", "AudioAccessory", "iPhone", "iPad", "iPod", "Mac", NULL,
    };
    const char *model = txt ? strstr(txt, "model=") : NULL;
    if (model) model += strlen("model=");
    for (size_t i = 0; prefixes[i]; i++) {
        size_t len = strlen(prefixes[i]);
        if (model && strncmp(model, prefixes[i], len) == 0) return true;
        if (am && strncmp(am, prefixes[i], len) == 0) return true;
    }
    return false;
}

ap2_route_t ap2_resolve_route(ap2_proto_pref_t pref, const char *txt, const char *pw,
                              bool have_credentials, bool have_password,
                              int bit_depth, bool force_native,
                              bool ptp_forced, bool ptp_enabled)
{
    ap2_route_t r = {0};
    r.features = ap2_txt_features(txt);
    r.flags = ap2_txt_flags(txt);
    bool has_pw = (pw && !strcasecmp(pw, "true"));

    /* 1. Protocol: AirPlay 2 vs legacy RAOP. The --ap2-native override pulls
     * us onto AirPlay 2 regardless of the advertised features. */
    bool is_ap2;
    if (pref == AP2_PROTO_RAOP) {
        is_ap2 = false;
    } else if (pref == AP2_PROTO_AIRPLAY2) {
        is_ap2 = true;
    } else { /* AUTO */
        is_ap2 = AP2_FEAT(r.features, AP2_FEAT_UNIFIED_MEDIA) ||
                 AP2_FEAT(r.features, AP2_FEAT_COREUTILS);
    }
    if (force_native) is_ap2 = true;

    if (!is_ap2) {
        r.use_raop = true;
        r.reason = "legacy RAOP";
        return r;
    }

    /* 2. Native AP2 vs RAOP-compatible flow. Stored credentials or --ap2-native
     * select native outright; otherwise a transient-pairable device goes native,
     * a supplied password doubling as its SRP secret. --protocol airplay2 also
     * pairs when the features say nothing at all — absent, unparseable and a
     * literal 0x0,0x0 all test as 0 here, deliberately — because that flag is
     * the caller naming an AirPlay-2-only receiver, which has no _raop service
     * behind it. Everything else (features without the pairing bits, or a
     * blocker native cannot clear either) stays on RAOP-compat. */
    bool pairable = AP2_FEAT(r.features, AP2_FEAT_HK_PAIRING) ||
                    AP2_FEAT(r.features, AP2_FEAT_COREUTILS) ||
                    (pref == AP2_PROTO_AIRPLAY2 && r.features == 0);
    bool pairing_blocked = (r.flags & (AP2_SF_PIN_REQUIRED | AP2_SF_LEGACY_PAIRING)) != 0 ||
                           (has_pw && !have_password);

    bool native = have_credentials || force_native || (pairable && !pairing_blocked);
    bool transient = native && !have_credentials;   /* stored keys => pair-verify */

    if (!native) {
        r.use_raop = false;
        r.native = false;
        r.reason = "AirPlay 2 (RAOP-compat)";
        return r;
    }

    r.use_raop = false;
    r.native = true;
    r.transient = transient;

    /* 3. Timing: PTP grandmaster when forced, else the SupportsPTP feature bit. */
    r.ptp = ptp_forced ? ptp_enabled : (AP2_FEAT(r.features, AP2_FEAT_PTP) != 0);

    /* Stream type is always realtime (type 96); it carries 16- and 24-bit —
     * verified audible on device. Buffered (type 103) was investigated and
     * removed: see DESIGN.md for the findings and rationale. */
    (void)bit_depth;

    if (transient)
        r.reason = have_password ? "native AP2, password, realtime"
                                 : "native AP2, transient, realtime";
    else
        r.reason = "native AP2, pair-verify, realtime";
    return r;
}

static uint64_t ap2_audio_format_code(const ap2_audio_format_t *format)
{
    if (format->bit_depth > 16 && format->sample_rate >= 48000)
        return AP2_FMT_ALAC_48000_24_2;
    if (format->bit_depth > 16)
        return AP2_FMT_ALAC_44100_24_2;
    if (format->sample_rate >= 48000)
        return AP2_FMT_ALAC_48000_16_2;
    return AP2_FMT_ALAC_44100_16_2;
}

/* Read one stream type's advertised format table from the GET /info reply:
 * supportedAudioFormatsExtended.<stream> (array of bit indices) when present,
 * else the legacy supportedFormats.<stream> mask. The tables are advisory —
 * hardware both understates (Apple TV renders unadvertised 24-bit) and
 * overstates (a SETUP 200 can still render silence); see DESIGN.md §11. */
static void ap2_parse_format_capability(const uint8_t *data, size_t len,
                                        const char *stream_key,
                                        uint64_t *formats, bool *known,
                                        bool *extended)
{
    *formats = 0;
    *known = false;
    *extended = false;
    if (ap2_bplist_find_dict_uint_array_mask(data, len,
                                              "supportedAudioFormatsExtended",
                                              stream_key, formats)) {
        *known = true;
        *extended = true;
        return;
    }
    if (ap2_bplist_find_dict_uint(data, len, "supportedFormats",
                                  stream_key, formats))
        *known = true;
}

/* Build one timing-peer dict {ID, DeviceType, ClockID, SupportsClockPort..., Addresses:[addr]}. */
static ap2_pl_node *ap2_make_timing_peer(const char *id, uint64_t clock_id, const char *addr)
{
    ap2_pl_node *d = ap2_pl_dict();
    ap2_pl_dict_set(d, "ID", ap2_pl_string(id));
    ap2_pl_dict_set(d, "DeviceType", ap2_pl_int(0));
    ap2_pl_dict_set(d, "ClockID", ap2_pl_int((int64_t)clock_id));
    ap2_pl_dict_set(d, "SupportsClockPortMatchingOverride", ap2_pl_bool(false));
    ap2_pl_node *addrs = ap2_pl_array();
    ap2_pl_array_append(addrs, ap2_pl_string(addr));
    ap2_pl_dict_set(d, "Addresses", addrs);
    return d;
}

/* Issue the type-130 remote-control (MRP) data-channel stream SETUP on the
 * already-verified RTSP session, then attach the MRP sender to the returned
 * dataPort (DESIGN.md §8, the "combined/piggyback" model). This is
 * best-effort decoration: any failure (SETUP non-200, no dataPort, connect or
 * handshake failure) logs and leaves p->mrp NULL / audio untouched. Reachable
 * only on a pair-verified session (Apple devices) — the DataStream keys derive
 * from this session's pair-verify shared secret. */
static void ap2_native_setup_mrp(struct ap2cl_s *p)
{
    if (!p->hap || !p->auth_credentials) return;   /* pair-verified only */

    /* Random seed < 2^63: our plist writer stores signed int64 and Apple's own
     * plist ints are signed at that width. The DataStream HKDF salt is
     * "DataStream-Salt" + this seed in decimal (ap2_mrp_attach). */
    uint64_t seed = 0;
    RAND_bytes((uint8_t *)&seed, sizeof(seed));
    seed &= 0x7FFFFFFFFFFFFFFFULL;

    char channel_uuid[37], client_uuid[37];
    ap2_gen_uuid(channel_uuid);
    ap2_gen_uuid(client_uuid);

    struct ap2_plist *ssp = ap2_plist_create();
    ap2_plist_stream_begin(ssp);
    ap2_plist_stream_add_int(ssp, "type", AP2_MRP_STREAM_TYPE_REMOTE_CONTROL);
    ap2_plist_stream_add_int(ssp, "controlType", AP2_MRP_STREAM_CONTROL_TYPE);
    ap2_plist_stream_add_string(ssp, "channelID", channel_uuid);
    ap2_plist_stream_add_int(ssp, "seed", (int64_t)seed);
    ap2_plist_stream_add_string(ssp, "clientUUID", client_uuid);
    ap2_plist_stream_add_string(ssp, "clientTypeUUID", AP2_MRP_CLIENT_TYPE_UUID);
    ap2_plist_stream_add_bool(ssp, "wantsDedicatedSocket", true);
    ap2_plist_stream_end(ssp);

    uint8_t *plist_data = NULL;
    int plist_len = ap2_plist_serialize(ssp, &plist_data);
    ap2_plist_free(ssp);

    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                               "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);

    if (status != 200) {
        LOG_WARN("[MRP] type-130 data-channel SETUP -> %d (no now-playing channel)",
                 status);
        free(resp);
        return;
    }

    int data_port = 0;
    uint64_t v;
    if (resp && resp_len > 0 &&
        ap2_bplist_find_uint(resp, (size_t)resp_len, "dataPort", &v) &&
        v >= 1024 && v <= 65535)
        data_port = (int)v;
    free(resp);

    if (data_port <= 0) {
        LOG_WARN("[MRP] type-130 SETUP returned no dataPort; no now-playing channel");
        return;
    }
    LOG_INFO("[MRP] type-130 data-channel SETUP OK (dataPort=%d, seed=%llu)",
             data_port, (unsigned long long)seed);

    /* Bind the sender to THIS session's pair-verify shared secret and open the
     * channel (TCP connect + DEVICE_INFO-first handshake). */
    if (!p->mrp)
        p->mrp = ap2_mrp_create(p->device.address, p->device.port,
                                p->auth_credentials, p->dacp_id, p->device.name,
                                p->session_uuid, p->group_uuid,
                                ap2_hap_get_shared_secret(p->hap));
    if (!p->mrp) return;
    ap2_mrp_set_remote_command_callback(
        p->mrp, ap2_remote_command_received, p);
    if (!ap2_mrp_attach_events(p->mrp, p->events_sock)) {
        LOG_WARN("[MRP] event-channel attach failed; degrading to no-MRP");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        return;
    }
    p->events_sock = -1;

    if (!ap2_mrp_attach(p->mrp, data_port, seed)) {
        LOG_WARN("[MRP] data-channel attach failed; degrading to no-MRP");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        return;
    }
    LOG_INFO("[MRP] remote-control data channel established (now-playing active)");
}

/* ---- Native AP2 connect sequence ---- */

/* Open the RTSP control connection to the device (bound to the configured
 * interface on multi-homed hosts). */
static bool ap2_native_open_socket(struct ap2cl_s *p)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", p->device.port);

    if (getaddrinfo(p->device.address, port_str, &hints, &res) != 0) return false;
    p->rtsp_carry_len = 0;
    p->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (p->sock_fd >= 0 && p->bind_addr.s_addr != INADDR_ANY) {
        struct sockaddr_in la = {.sin_family = AF_INET, .sin_addr = p->bind_addr};
        if (bind(p->sock_fd, (struct sockaddr *)&la, sizeof(la)) != 0)
            LOG_WARN("[AP2] Cannot bind RTSP socket to %s", inet_ntoa(p->bind_addr));
    }
    if (p->sock_fd < 0 || connect(p->sock_fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (p->sock_fd >= 0) { close(p->sock_fd); p->sock_fd = -1; }
        return false;
    }
    freeaddrinfo(res);
    return true;
}

/* GET /info on the (still unencrypted) control connection and read the
 * advertised per-stream format tables from the reply. */
static bool ap2_native_get_info(struct ap2cl_s *p)
{
    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "GET", "/info", NULL, 0, NULL, &resp, &resp_len);
    if (status != 200) {
        LOG_ERROR("[AP2] /info failed: %d", status);
        ap2_report_failed_exchange(p, "GET /info", status);
        free(resp);
        return false;
    }
    p->audio_format = ap2_audio_format_code(&p->format);
    if (resp && resp_len > 0) {
        ap2_parse_format_capability(resp, (size_t)resp_len, "audioStream",
                                    &p->realtime_formats,
                                    &p->realtime_formats_known,
                                    &p->realtime_formats_extended);
        ap2_parse_format_capability(resp, (size_t)resp_len, "bufferStream",
                                    &p->buffered_formats,
                                    &p->buffered_formats_known,
                                    &p->buffered_formats_extended);
    }
    LOG_INFO("[AP2] Formats requested=0x%" PRIx64
             " realtime=%s0x%" PRIx64 " buffered=%s0x%" PRIx64,
             p->audio_format,
             p->realtime_formats_known
                 ? (p->realtime_formats_extended ? "extended:" : "mask:")
                 : "unknown:",
             p->realtime_formats,
             p->buffered_formats_known
                 ? (p->buffered_formats_extended ? "extended:" : "mask:")
                 : "unknown:",
             p->buffered_formats);
    free(resp);
    return true;
}

/* Start the next pairing attempt from a clean connection: a receiver that
 * rejected one pairing leg leaves its state machine wedged on that socket. */
static bool ap2_native_reset_connection(struct ap2cl_s *p)
{
    if (p->sock_fd >= 0) { close(p->sock_fd); p->sock_fd = -1; }
    if (!ap2_native_open_socket(p)) {
        LOG_ERROR("[AP2] Cannot reopen the control connection for the next "
                  "pairing attempt");
        return false;
    }
    return ap2_native_get_info(p);
}

/* One transient pair-setup leg; secret NULL selects the fixed transient PIN. */
static bool ap2_native_transient(struct ap2cl_s *p, const char *secret,
                                 ap2_hap_error_t *err)
{
    p->hap = ap2_hap_create(NULL);
    if (!p->hap) {
        LOG_ERROR("[AP2] Cannot create HAP context");
        return false;
    }
    LOG_INFO("[AP2] Performing HAP transient pair-setup...");
    if (ap2_hap_pair_setup_transient(p->hap, p->sock_fd, secret, err))
        return true;
    LOG_ERROR("[AP2] HAP transient pair-setup failed");
    ap2_hap_destroy(p->hap);
    p->hap = NULL;
    return false;
}

/* One pair-verify leg with the stored HAP credentials. */
static bool ap2_native_pair_verify(struct ap2cl_s *p, ap2_hap_error_t *err)
{
    p->hap = ap2_hap_create(p->auth_credentials);
    if (!p->hap) {
        LOG_ERROR("[AP2] Invalid credentials");
        return false;
    }

    /* Set client_id from DACP ID as UPPERCASE ASCII string.
     * Must match what was sent during pair-setup. MA's pair-setup uses the
     * DACP ID as a 16-char uppercase hex string encoded as bytes. */
    if (p->dacp_id) {
        /* Uppercase the string */
        char upper_dacp[32];
        int len = strlen(p->dacp_id);
        if (len > 30) len = 30;
        for (int i = 0; i < len; i++) {
            char c = p->dacp_id[i];
            upper_dacp[i] = (c >= 'a' && c <= 'f') ? (c - 'a' + 'A') : c;
        }
        upper_dacp[len] = '\0';
        ap2_hap_set_client_id(p->hap, (const uint8_t *)upper_dacp, len);
    }

    LOG_INFO("[AP2] Performing HAP pair-verify...");
    if (ap2_hap_pair_verify(p->hap, p->sock_fd, err)) return true;
    LOG_ERROR("[AP2] HAP pair-verify failed");
    ap2_hap_destroy(p->hap);
    p->hap = NULL;
    return false;
}

/*
 * HAP handshake for the native flow, leaving p->hap holding the session keys.
 *
 * Without a device password this is exactly one leg: pair-verify with stored
 * credentials (Apple TV/HomePod), transient pair-setup with the fixed PIN
 * otherwise (Sonos and most third-party receivers).
 *
 * A device password makes it a ladder, because receivers disagree on what a
 * password means for AirPlay 2: some substitute it for the fixed PIN as the
 * transient SRP secret, others disable transient pairing entirely once a
 * password is set and expect the stored HomeKit credentials instead. The
 * password leg goes first and each rejected leg is retried on a fresh
 * connection; the reported status is the password leg's, which is the one
 * that describes the device's password handling.
 */
static bool ap2_native_pair(struct ap2cl_s *p)
{
    ap2_hap_error_t err = {0};
    const bool have_password = p->password && *p->password;

    if (!have_password) {
        /* A TLV-level rejection arrives with HTTP 200, so the auth-shaped test
         * must consider the HAP result kind as well as the HTTP status. */
        if (p->auth_credentials) {
            if (ap2_native_pair_verify(p, &err)) return true;
            ap2_set_connect_error(p,
                                  (err.result == AP2_HAP_ERR_AUTH ||
                                   ap2_status_is_auth(err.http_status))
                                      ? ap2_auth_error_kind(p)
                                      : AP2_CONNECT_ERROR_GENERIC,
                                  err.http_status, "HAP pair-verify failed");
            return false;
        }
        if (ap2_native_transient(p, NULL, &err)) return true;
        ap2_set_connect_error(p,
                              (err.result == AP2_HAP_ERR_AUTH ||
                               ap2_status_is_auth(err.http_status))
                                  ? ap2_auth_error_kind(p)
                                  : AP2_CONNECT_ERROR_GENERIC,
                              err.http_status, "HAP transient pair-setup failed");
        return false;
    }

    LOG_INFO("[AP2] Pairing leg 1/2: transient pair-setup with the device password");
    if (ap2_native_transient(p, p->password, &err)) {
        LOG_INFO("[AP2] Device accepted the password as the transient secret");
        return true;
    }
    int password_status = err.http_status;
    LOG_WARN("[AP2] Password pair-setup rejected (cause=%d http=%d tlv=%d)",
             (int)err.result, err.http_status, err.tlv_error);
    if (err.result == AP2_HAP_ERR_TRANSPORT) {
        /* Nothing was rejected — the connection died mid-handshake. */
        ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, password_status,
                              "pair-setup with the device password failed on the wire");
        return false;
    }

    if (p->auth_credentials) {
        LOG_INFO("[AP2] Pairing leg 2/2: stored credentials (pair-verify)");
        if (!ap2_native_reset_connection(p)) {
            ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, password_status,
                                  "cannot reconnect for the pair-verify fallback");
            return false;
        }
        ap2_hap_error_t verify_err = {0};
        if (ap2_native_pair_verify(p, &verify_err)) {
            LOG_INFO("[AP2] Stored credentials accepted after the password leg "
                     "was rejected");
            return true;
        }
        ap2_set_connect_error(p, AP2_CONNECT_ERROR_AUTH_FAILED,
                              password_status ? password_status
                                              : verify_err.http_status,
                              "device rejected both the password and the stored "
                              "credentials");
        return false;
    }

    /* No credentials to fall back on. The password may simply not be this
     * receiver's SRP secret (configured, but not actually required), so give
     * the fixed transient PIN its normal chance before giving up. */
    LOG_INFO("[AP2] Pairing leg 2/2: transient pair-setup with the fixed PIN");
    if (!ap2_native_reset_connection(p)) {
        ap2_set_connect_error(p, AP2_CONNECT_ERROR_AUTH_FAILED, password_status,
                              "device rejected the password and the connection "
                              "could not be retried");
        return false;
    }
    ap2_hap_error_t pin_err = {0};
    if (ap2_native_transient(p, NULL, &pin_err)) {
        LOG_WARN("[AP2] Device paired with the fixed transient PIN; the "
                 "configured password was not used");
        return true;
    }
    ap2_set_connect_error(p, AP2_CONNECT_ERROR_AUTH_FAILED,
                          password_status ? password_status : pin_err.http_status,
                          "device rejected the supplied password");
    return false;
}

static bool ap2_native_connect(struct ap2cl_s *p)
{
    /* Default outcome for every failure path below; the auth-aware sites
     * refine it. Cleared once the session is up. */
    ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, 0,
                          "native AirPlay 2 connect failed");
    p->last_error_response_len = 0;

    /* Resolve the local bind address once (multi-homed hosts): used for the
     * RTSP TCP socket and the RTP data/control UDP sockets below. */
    p->bind_addr.s_addr = INADDR_ANY;
    if (p->iface) {
        char *ifname = NULL;
        uint32_t netmask;
        p->bind_addr = get_interface(p->iface, &ifname, &netmask);
        NFREE(ifname);
    }

    if (!ap2_native_open_socket(p)) return false;

    /* Get local address for session URL */
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(p->sock_fd, (struct sockaddr *)&local, &len);
    char local_addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, local_addr, sizeof(local_addr));
    RAND_bytes((uint8_t *)&p->session_id, 4);
    snprintf(p->session_url, sizeof(p->session_url), "rtsp://%s/%u", local_addr, p->session_id);

    /* Address we advertise to the device (timingPeerInfo.Addresses, SETPEERS):
     * explicit --publish-ip, else the bound interface, else the RTSP local addr. */
    char our_addr[INET_ADDRSTRLEN];
    if (p->publish_ip && *p->publish_ip) {
        snprintf(our_addr, sizeof(our_addr), "%s", p->publish_ip);
    } else if (p->bind_addr.s_addr != INADDR_ANY) {
        inet_ntop(AF_INET, &p->bind_addr, our_addr, sizeof(our_addr));
    } else {
        snprintf(our_addr, sizeof(our_addr), "%s", local_addr);
    }

    /* Generate session UUID */
    uint8_t uuid_bytes[16];
    RAND_bytes(uuid_bytes, 16);
    snprintf(p->session_uuid, sizeof(p->session_uuid),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
             uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
             uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);
    ap2_gen_uuid(p->group_uuid);

    /* 1. GET /info */
    if (!ap2_native_get_info(p)) return false;

    /* 2. HAP pairing (see ap2_native_pair for the leg order). */
    if (!ap2_native_pair(p)) return false;
    LOG_INFO("[AP2] Channel encrypted");

    /* Diagnostic: post-pairing /info — the encrypted-channel reply is the
     * full form (e.g. audioLatencies). Dump-only; env-gated. */
    if (getenv("CLIAIRPLAY_DUMP_INFO2")) {
        uint8_t *iresp = NULL; int iresp_len = 0;
        int istatus = ap2_rtsp_send(p, "GET", "/info", NULL, 0, NULL, &iresp, &iresp_len);
        if (istatus == 200 && iresp && iresp_len > 0) {
            FILE *f = fopen(getenv("CLIAIRPLAY_DUMP_INFO2"), "wb");
            if (f) { fwrite(iresp, 1, iresp_len, f); fclose(f); }
            LOG_INFO("[AP2] post-pairing /info dumped (%d bytes)", iresp_len);
        }
        free(iresp);
    }

    /* Our AirPlay identity, derived from the (16-hex) DACP ID: an 8-byte
     * colon deviceID, a 6-byte colon macAddress, and the 64-bit PTP clock
     * identity. Keeping these in one place ensures the PTP grandmasterIdentity
     * matches the ClockID we advertise in the session SETUP. */
    uint8_t id_bytes[8] = {0};
    int id_n = ap2_dacp_bytes(p->dacp_id, id_bytes);
    bool id_valid = (id_n >= 8);
    uint64_t clock_id = 0;
    for (int i = 0; i < 8; i++) clock_id = (clock_id << 8) | id_bytes[i];
    char dev_colon[24]; ap2_colon_hex(id_bytes, 8, dev_colon);
    char mac_colon[18]; ap2_colon_hex(id_bytes, 6, mac_colon);

    /* 3. Timing. PTP grandmaster when selected (forced by flag, else
     * SupportsPTP feature bit); the legacy NTP responder otherwise. If PTP is
     * wanted but 319/320 can't be bound (privilege), fall back to NTP. */
    bool want_ptp = p->ptp_forced ? p->ptp_enabled
                                  : ap2_features_has_ptp(p->device.txt_records);
    p->ptp = ap2_ptp_create();
    int timing_port = 0;
    if (want_ptp) {
        ap2_ptp_set_clock_id(p->ptp, clock_id);
        /* Multi-room: when a shared PTP daemon owns 319/320 on this host, attach
         * its elected clock read-only and register our receiver with it, rather
         * than running our own engine (only one process per host can bind
         * 319/320). Without --ptp-shared, or with no live daemon, fall through to
         * the in-process engine — the single-device path, byte-for-byte. */
        if (p->ptp_shared && ap2_ptp_attach_shared(p->ptp)) {
            p->use_ptp = true;
            ap2_ptp_shared_register(p->ptp, p->device.address);
            ap2_ptp_engine_settle(p->ptp, 400);
        } else if (ap2_ptp_engine_start(p->ptp, p->bind_addr, p->device.address)) {
            p->use_ptp = true;
            /* Let BMCA hear any competing Announce and resolve the grandmaster
             * before we build the SETUP, so the timeline ClockID below is the
             * elected master's (ours if we win, the receiver's if it does). */
            ap2_ptp_engine_settle(p->ptp, 400);
        } else {
            ap2_ptp_start(p->ptp, p->device.address);
            timing_port = ap2_ptp_get_timing_port(p->ptp);
        }
    } else {
        ap2_ptp_start(p->ptp, p->device.address);
        timing_port = ap2_ptp_get_timing_port(p->ptp);
    }

    /* 4. Session SETUP (encrypted). PTP and NTP use different session dicts. */
    uint8_t *plist_data = NULL; int plist_len = 0;
    if (p->use_ptp) {
        char peer_id[37];
        ap2_gen_uuid(peer_id);
        const char *name = (p->device.name && *p->device.name) ? p->device.name : "cliairplay";

        /* Advertise OUR clock as the session timeline (the engine holds
         * grandmaster): receivers only follow masters from the timing-peer
         * list — us — and cannot anchor to their own clock. The media anchor
         * below is expressed against this same clock domain. */
        uint64_t timeline_id = ap2_ptp_master_clock_id(p->ptp);

        ap2_pl_node *root = ap2_pl_dict();
        ap2_pl_dict_set(root, "timingProtocol", ap2_pl_string("PTP"));
        ap2_pl_dict_set(root, "deviceID", ap2_pl_string(dev_colon));
        ap2_pl_dict_set(root, "sessionUUID", ap2_pl_string(p->session_uuid));
        ap2_pl_dict_set(root, "name", ap2_pl_string(name));
        ap2_pl_dict_set(root, "macAddress", ap2_pl_string(mac_colon));
        ap2_pl_dict_set(root, "groupUUID", ap2_pl_string(p->group_uuid));
        ap2_pl_dict_set(root, "groupContainsGroupLeader", ap2_pl_bool(false));
        ap2_pl_dict_set(root, "timingPeerInfo",
                        ap2_make_timing_peer(peer_id, timeline_id, our_addr));
        ap2_pl_node *peer_list = ap2_pl_array();
        ap2_pl_array_append(peer_list, ap2_make_timing_peer(peer_id, timeline_id, our_addr));
        ap2_pl_dict_set(root, "timingPeerList", peer_list);

        plist_len = ap2_pl_serialize(root, &plist_data);
        ap2_pl_free(root);
        LOG_INFO("[AP2] PTP session SETUP (%d bytes, timelineID=%016llx%s, addr=%s)",
                 plist_len, (unsigned long long)timeline_id,
                 timeline_id == clock_id ? " [we are GM]" : " [slaving to peer]", our_addr);
    } else {
        struct ap2_plist *sp = ap2_plist_create();
        if (id_valid) ap2_plist_add_string(sp, "deviceID", dev_colon);
        ap2_plist_add_string(sp, "sessionUUID", p->session_uuid);
        ap2_plist_add_int(sp, "timingPort", timing_port);
        ap2_plist_add_string(sp, "timingProtocol", "NTP");
        plist_len = ap2_plist_serialize(sp, &plist_data);
        ap2_plist_free(sp);
    }

    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);
    if (status != 200) {
        LOG_ERROR("[AP2] Session SETUP failed: %d", status);
        ap2_report_failed_exchange(p, "session SETUP", status);
        free(resp);
        return false;
    }
    LOG_INFO("[AP2] Session SETUP OK (%s timing)", p->use_ptp ? "PTP" : "NTP");

    /* Extract eventPort from the session response (by key, real traversal) */
    int event_port = 0;
    if (resp && resp_len > 0) {
        uint64_t v;
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "eventPort", &v) &&
            v >= 1024 && v <= 65535)
            event_port = (int)v;
    }
    free(resp);

    /* Open events connection (reverse TCP to device's eventPort) */
    if (event_port > 0) {
        LOG_INFO("[AP2] Opening events connection to %s:%d", p->device.address, event_port);
        int events_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (events_sock >= 0) {
            struct sockaddr_in ev_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(event_port),
            };
            inet_pton(AF_INET, p->device.address, &ev_addr.sin_addr);
            struct timeval etv = {.tv_sec = 3};
            setsockopt(events_sock, SOL_SOCKET, SO_RCVTIMEO, &etv, sizeof(etv));
            setsockopt(events_sock, SOL_SOCKET, SO_SNDTIMEO, &etv, sizeof(etv));
            if (connect(events_sock, (struct sockaddr *)&ev_addr, sizeof(ev_addr)) == 0) {
                LOG_INFO("[AP2] Events connection OK");
                int one = 1;
                setsockopt(events_sock, IPPROTO_TCP, TCP_NODELAY,
                           &one, sizeof(one));
                p->events_sock = events_sock;
            } else {
                LOG_WARN("[AP2] Events connect failed");
                close(events_sock);
            }
        }
    }

    /* 5. RECORD (on the session URL, below), then stream SETUP with the
     * streams array. RECORD before the stream SETUP matches the order used
     * by Apple senders and owntone, and is REQUIRED by Samsung AirPlay 2
     * receivers (e.g. Music Frame HW-LS60D), which 200-ACK the entire session
     * but render silence otherwise; verified on hardware 2026-07-24 (A/B/A on
     * the Frame; no regression on Apple TV 4K or Sonos Era, warm seek
     * intact). */
    /* The shk audio key must be the first 32 bytes of the pairing shared
     * secret: that is also the key the audio sender encrypts with, and the
     * receiver uses shk to decrypt the RTP payloads. */
    memcpy(p->audio_key, ap2_hap_get_shared_secret(p->hap), 32);
    /* For NTP sessions the SSRC matches the streamConnectionID we register in
     * stream SETUP; for PTP sessions the RTP SSRC is zero (the stream is keyed
     * by the PTP clock identity instead). */
    p->ssrc = p->use_ptp ? 0 : p->session_id;

    /* Open UDP sockets */
    p->data_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->data_sock < 0 || !ap2_set_nonblocking(p->data_sock, "RTP data"))
        return false;
    struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_addr = p->bind_addr};
    if (bind(p->data_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        LOG_ERROR("[AP2] Cannot bind RTP data socket: %s", strerror(errno));
        return false;
    }
    struct sockaddr_in ds_local;
    len = sizeof(ds_local);
    getsockname(p->data_sock, (struct sockaddr *)&ds_local, &len);
    int local_data_port = ntohs(ds_local.sin_port);

    p->ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->ctrl_sock < 0 || !ap2_set_nonblocking(p->ctrl_sock, "RTP control"))
        return false;
    if (bind(p->ctrl_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        LOG_ERROR("[AP2] Cannot bind RTP control socket: %s", strerror(errno));
        return false;
    }
    struct sockaddr_in cs_local;
    len = sizeof(cs_local);
    getsockname(p->ctrl_sock, (struct sockaddr *)&cs_local, &len);
    int local_ctrl_port = ntohs(cs_local.sin_port);

    /* Determine audio format */
    uint64_t audio_format = ap2_audio_format_code(&p->format);

    /* Realtime (type 96) streams the audio over UDP and carries our data port
     * in the SETUP. */
    int stream_type = 96;

    struct ap2_plist *ssp = ap2_plist_create();
    ap2_plist_stream_begin(ssp);
    ap2_plist_stream_add_int(ssp, "audioFormat", audio_format);
    ap2_plist_stream_add_string(ssp, "audioMode", "default");
    ap2_plist_stream_add_int(ssp, "controlPort", local_ctrl_port);
    ap2_plist_stream_add_int(ssp, "ct", 2);  /* ALAC */
    ap2_plist_stream_add_int(ssp, "dataPort", local_data_port);
    ap2_plist_stream_add_bool(ssp, "isMedia", true);
    ap2_plist_stream_add_int(ssp, "latencyMax", 88200);
    ap2_plist_stream_add_int(ssp, "latencyMin", 11025);
    ap2_plist_stream_add_data(ssp, "shk", p->audio_key, 32);
    ap2_plist_stream_add_int(ssp, "spf", 352);
    ap2_plist_stream_add_int(ssp, "sr", p->format.sample_rate);
    ap2_plist_stream_add_int(ssp, "streamConnectionID", p->session_id);
    ap2_plist_stream_add_bool(ssp, "supportsDynamicStreamID", false);
    ap2_plist_stream_add_int(ssp, "type", stream_type);
    ap2_plist_stream_end(ssp);

    plist_len = ap2_plist_serialize(ssp, &plist_data);
    ap2_plist_free(ssp);

    /* RECORD on the session URL, before the stream SETUP (see the note
     * above): required for Samsung AirPlay 2 receivers to actually render
     * audio. */
    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "RECORD", p->session_url, NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    if (status <= 0) {
        free(plist_data);
        return false;
    } else if (status != 200) {
        LOG_WARN("[AP2] RECORD returned %d", status);
    } else {
        LOG_INFO("[AP2] RECORD OK");
    }

    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);

    if (status != 200) {
        LOG_ERROR("[AP2] Stream SETUP failed: %d", status);
        ap2_report_failed_exchange(p, "stream SETUP", status);
        free(resp);
        return false;
    }

    if (getenv("CLIAIRPLAY_DUMP_SETUP") && resp && resp_len > 0) {
        FILE *f = fopen(getenv("CLIAIRPLAY_DUMP_SETUP"), "wb");
        if (f) { fwrite(resp, 1, resp_len, f); fclose(f); }
        LOG_INFO("[AP2] stream SETUP response dumped (%d bytes)", resp_len);
    }

    /* Parse the remote ports from the response plist,
     * {"streams": [{"dataPort": N, "controlPort": N, ...}]}, by KEY with real
     * offset-table traversal. Positional guessing must never be used here: a
     * receiver's plist writer typically serializes controlPort before dataPort
     * (alphabetical), so "first port found = data" sends the audio to the
     * receiver's control port and mutes it. */
    if (resp && resp_len > 0) {
        int remote_data = 0, remote_ctrl = 0;
        uint64_t v;
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "dataPort", &v) &&
            v >= 1024 && v <= 65535)
            remote_data = (int)v;
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "controlPort", &v) &&
            v >= 1024 && v <= 65535)
            remote_ctrl = (int)v;
        LOG_DEBUG("[AP2] Stream response ports: data=%d control=%d", remote_data, remote_ctrl);

        /* Downstream pipeline delay the device itself reports (Apple TV: its
         * decode+HDMI+display chain, ~100ms). Parsed for information only and
         * surfaced to the caller (MA); NOT applied to scheduling. Receivers
         * already self-compensate their own render latency, so applying the
         * reported value over-compensates and makes those devices play early.
         * Real downstream latency (TV / AV receiver / amplifier) is per-
         * household and set manually via the player's latency adjustment. */
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "arrivalToRenderLatencyMs", &v) &&
            v <= 2000) {
            p->dev_render_ms = (int)v;
            LOG_INFO("[AP2] Device reports arrival->render latency %dms "
                     "(informational; not applied - downstream TV/AV latency is "
                     "set via the player's latency adjustment)", p->dev_render_ms);
        }

        /* The receiver reports its buffering window (frames). Clamp our lead
         * into it so the configured latency can never violate the device;
         * the effective value is surfaced so the caller (MA) can plan group
         * starts from real device capabilities instead of a config guess. */
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "latencyMin", &v) && v > 0)
            p->dev_latency_min = (uint32_t)v;
        if (ap2_bplist_find_uint(resp, (size_t)resp_len, "latencyMax", &v) && v > 0)
            p->dev_latency_max = (uint32_t)v;
        if (p->dev_latency_min || p->dev_latency_max) {
            int lat_frames = MS2TS(p->latency_ms, p->format.sample_rate);
            int min_f = p->dev_latency_min ? (int)p->dev_latency_min : 0;
            int max_f = p->dev_latency_max ? (int)p->dev_latency_max : lat_frames;
            int clamped = lat_frames < min_f ? min_f : (lat_frames > max_f ? max_f : lat_frames);
            int clamped_ms = clamped * 1000 / p->format.sample_rate;
            if (clamped_ms != p->latency_ms) {
                LOG_INFO("[AP2] Device latency window %d..%d frames: adjusting lead "
                         "%dms -> %dms", min_f, max_f, p->latency_ms, clamped_ms);
                p->latency_ms = clamped_ms;
            }
            LOG_INFO("[AP2] Device latency: min=%u max=%u frames, effective lead %dms",
                     p->dev_latency_min, p->dev_latency_max, p->latency_ms);
        }

        if (remote_data > 0) {
            memset(&p->data_addr, 0, sizeof(p->data_addr));
            p->data_addr.sin_family = AF_INET;
            p->data_addr.sin_port = htons(remote_data);
            inet_pton(AF_INET, p->device.address, &p->data_addr.sin_addr);
            LOG_INFO("[AP2] Remote data port: %d", remote_data);
        }
        if (remote_ctrl > 0) {
            memset(&p->ctrl_addr, 0, sizeof(p->ctrl_addr));
            p->ctrl_addr.sin_family = AF_INET;
            p->ctrl_addr.sin_port = htons(remote_ctrl);
            inet_pton(AF_INET, p->device.address, &p->ctrl_addr.sin_addr);
            LOG_INFO("[AP2] Remote control port: %d", remote_ctrl);
        }

        if (remote_data == 0) {
            LOG_WARN("[AP2] Could not parse remote data port from response");
        }
    }
    LOG_INFO("[AP2] Stream SETUP OK");
    free(resp);

    /* 6. SETPEERS (PTP only): a bare binary-plist array of IP strings
     * [receiver, us] so the receiver knows the timing group members. */
    if (p->use_ptp) {
        ap2_pl_node *arr = ap2_pl_array();
        ap2_pl_array_append(arr, ap2_pl_string(p->device.address));
        ap2_pl_array_append(arr, ap2_pl_string(our_addr));
        uint8_t *sp_data = NULL;
        int sp_len = ap2_pl_serialize(arr, &sp_data);
        ap2_pl_free(arr);

        resp = NULL; resp_len = 0;
        int sp_status = ap2_rtsp_send(p, "SETPEERS", p->session_url, sp_data, sp_len,
                                       "application/x-apple-binary-plist", &resp, &resp_len);
        free(sp_data);
        free(resp);
        LOG_INFO("[AP2] SETPEERS [%s, %s] -> %d", p->device.address, our_addr, sp_status);
        if (sp_status <= 0) return false;

        const char *peers[2] = { p->device.address, our_addr };
        ap2_ptp_set_peers(p->ptp, peers, 2);
        /* Kick timing at the fresh peer so it measures our clock right away
         * (no-op without a shared daemon; the in-process engine kicks itself
         * from ap2_ptp_set_peers). */
        ap2_ptp_shared_kick(p->ptp);
    }

    /* 6b. MRP now-playing. Ground-truth capture (DESIGN.md §8) shows a real
     * sender delivers now-playing over POST /command, NOT the type-130 channel
     * (its type-130 SETUPs never completed a data connection), and pushing state
     * over type-130 competes with /command. So the type-130 channel is OFF by
     * default; now-playing rides /command (ap2cl_mrp_register + ap2cl_mrp_push).
     * Set CLIAIRPLAY_MRP_TYPE130=1 to re-enable the channel (future remote-
     * control RX). Best-effort; audio is up regardless. */
    if (getenv("CLIAIRPLAY_MRP_TYPE130"))
        ap2_native_setup_mrp(p);

    /* Create ALAC encoder */
    p->alac = alac_create_encoder(AP2_FRAMES_PER_CHUNK,
                                   p->format.sample_rate,
                                   p->format.bit_depth,
                                   p->format.channels);

    p->first_packet = true;
    if (atomic_load(&p->rtsp_dead)) return false;
    p->rtsp_established = true;
    p->state = AP2_CONNECTED;
    /* Readiness is measurable from here: a cold receiver starts probing our
     * clock about a second after connect, on its own schedule and with no
     * anchor announced (measured on a Samsung Music Frame, 187 probes over
     * 24 s with no START ever sent). Keep the shared-daemon snapshot fresh
     * from now on so ap2cl_clock_readiness can report it without blocking,
     * and stamp the instant its stall window runs from. The streak mark starts
     * clear: one left by an earlier session on this client describes a receiver
     * that is no longer the one being measured. */
    atomic_store(&p->clock_poll_wanted, true);
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_connected_unix_ms = ap2_ntp_to_unix_ms(raopcl_get_ntp(NULL));
    p->clock_last_streak_unix_ms = 0;
    pthread_mutex_unlock(&p->clock_verify_lock);
    ap2_clock_poller_ensure(p);
    if (!ap2_feedback_start(p)) {
        atomic_store(&p->rtsp_dead, true);
        return false;
    }
    /* Non-fatal: without the responder the stream still plays, it just cannot
     * repair receiver-side loss. */
    ap2_rtx_start(p);
    ap2_set_connect_error(p, AP2_CONNECT_ERROR_NONE, 0, "%s", "");
    LOG_INFO("[AP2] Native AP2 session ready");
    return true;
}

/* ---- Native AP2 audio send ---- */

static bool ap2_set_nonblocking(int fd, const char *name)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("[AP2] Cannot make %s socket nonblocking: %s",
                  name, strerror(errno));
        return false;
    }
    return true;
}

/* Send AP2 NTP sync packet (20 bytes) to control port.
 * Format (from owntone rtp_common.c sync_packet_ntp_make):
 *   bytes 0-1:  header (0x90d4 first, 0x80d4 subsequent)
 *   bytes 2-3:  fixed 0x0007
 *   bytes 4-7:  RTP timestamp - latency (network order)
 *   bytes 8-11: NTP seconds (wall clock, network order)
 *   bytes 12-15: NTP fraction (network order)
 *   bytes 16-19: current RTP timestamp (network order)
 * Sent unencrypted on the control UDP socket. */
static ap2_send_result_t ap2_send_sync_packet(struct ap2cl_s *p, bool first)
{
    if (p->ctrl_sock < 0) {
        LOG_ERROR("[AP2] NTP sync socket is unavailable");
        atomic_store(&p->media_healthy, false);
        return AP2_SEND_FATAL;
    }

    uint8_t pkt[20];
    pkt[0] = first ? 0x90 : 0x80;
    pkt[1] = 0xd4;
    pkt[2] = 0x00;
    pkt[3] = 0x07;

    uint32_t latency_frames = MS2TS(p->latency_ms, p->format.sample_rate);
    uint32_t ts_latency = p->rtp_timestamp >= latency_frames
                          ? p->rtp_timestamp - latency_frames : 0;
    uint32_t ts_be = htonl(ts_latency);
    memcpy(pkt + 4, &ts_be, 4);

    /* NTP time */
    uint64_t ntp = raopcl_get_ntp(NULL);
    uint32_t ntp_sec = htonl((uint32_t)(ntp >> 32));
    uint32_t ntp_frac = htonl((uint32_t)(ntp & 0xFFFFFFFF));
    memcpy(pkt + 8, &ntp_sec, 4);
    memcpy(pkt + 12, &ntp_frac, 4);

    uint32_t cur_ts_be = htonl(p->rtp_timestamp);
    memcpy(pkt + 16, &cur_ts_be, 4);

    ap2_send_result_t result = ap2_io_send_datagram_deadline(
        p->ctrl_sock, pkt, sizeof(pkt),
        (const struct sockaddr *)&p->ctrl_addr, sizeof(p->ctrl_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    if (result == AP2_SEND_SENT) {
        p->sync_packets_sent++;
    } else {
        p->sync_packets_dropped++;
        if (result == AP2_SEND_FATAL) {
            atomic_store(&p->media_healthy, false);
            LOG_ERROR("[AP2] NTP sync send failed: %s", strerror(errno));
        } else if (p->sync_packets_dropped <= 3 ||
                   (p->sync_packets_dropped % 100) == 0) {
            LOG_WARN("[AP2] NTP sync send transiently dropped");
        }
    }
    return result;
}

/* Send AP2 PTP sync (anchor) packet (28 bytes) to the control port.
 * Format (owntone rtp_common.c sync_packet_ptp, realtime use_ptp path):
 *   bytes 0-1:   header (0x90d7 first / 0x80d7 subsequent)
 *   bytes 2-3:   fixed 0x0006
 *   bytes 4-7:   current RTP timestamp (network order)
 *   bytes 8-15:  wall-clock time in nanoseconds on the PTP master timebase (BE64)
 *   bytes 16-19: RTP timestamp - latency (the sample currently rendering)
 *   bytes 20-27: our PTP clock identity (BE64)
 * The wall-clock ns and the clock identity come from the PTP grandmaster so the
 * receiver, slaved to that clock, can place the anchor precisely. */
static ap2_send_result_t ap2_send_sync_packet_ptp(struct ap2cl_s *p, bool first)
{
    if (p->ctrl_sock < 0) {
        LOG_ERROR("[AP2] PTP sync socket is unavailable");
        atomic_store(&p->media_healthy, false);
        return AP2_SEND_FATAL;
    }

    uint8_t pkt[28];
    pkt[0] = first ? 0x90 : 0x80;
    pkt[1] = 0xd7;
    pkt[2] = 0x00;
    pkt[3] = 0x06;

    /* Anchor semantics (from shairport-sync's receiver math): the receiver
     * schedules playback as "frame_1 - 11035 plays at the packet timestamp"
     * and derives its buffer latency from frame_2 - frame_1 (Apple senders:
     * 77175). The mapping is FROZEN at stream start — playback of the first
     * frame begins latency_ms after the anchor point — and every periodic
     * packet extrapolates along that same line, so all time-announces agree. */
    uint64_t wall = ap2_ptp_master_now_ns(p->ptp);
    if (!p->rt_anchor_valid) {
        if (p->start_ntp) {
            /* Group playback: derive the line from the SHARED start time so
             * every player maps the same sample to the same wall instant.
             * Contract (all protocol paths): the first sample is AUDIBLE
             * exactly at the start time — anchoring one lead early makes the
             * line's audible point land on it, and mixed RAOP/AP2 groups
             * align regardless of each member's lead. Valid because our
             * timeline — in-process GM or the shared daemon — is host
             * CLOCK_REALTIME, the same clock the start value comes from
             * (libraop's NTP fixed-point is UNIX-epoch: seconds<<32 | frac). */
            uint64_t unix_ns = (p->start_ntp >> 32) * 1000000000ULL
                             + (((p->start_ntp & 0xFFFFFFFFULL) * 1000000000ULL) >> 32);
            p->rt_anchor_wall0 = unix_ns - (uint64_t)p->latency_ms * 1000000ULL;
            p->rt_anchor_pos0 = (uint32_t)NTP2TS(p->start_ntp, p->format.sample_rate)
                              + atomic_load(&p->rtp_offset);
        } else {
            p->rt_anchor_wall0 = wall;
            p->rt_anchor_pos0 = p->rtp_timestamp;
        }
        p->rt_anchor_valid = true;
        /* The frozen schedule handed to the receiver. audible_in_ms is how long
         * the device still has between this announce and the instant its first
         * sample must be rendering — the budget a woken-from-standby amp has to
         * bring its output path up. */
        int64_t audible_in_ms =
            ((int64_t)(p->rt_anchor_wall0 - wall) / 1000000LL)
            + (int64_t)p->latency_ms;
        LOG_INFO("[AP2] anchor frozen: wall0=%" PRIu64 "ns pos0=%u "
                 "latency_ms=%u start_ntp=%" PRIu64 " audible_in_ms=%" PRId64,
                 p->rt_anchor_wall0, p->rt_anchor_pos0, p->latency_ms,
                 p->start_ntp, audible_in_ms);
    }
    int64_t wall_delta_ns = wall >= p->rt_anchor_wall0
                                ? (int64_t)(wall - p->rt_anchor_wall0)
                                : -(int64_t)(p->rt_anchor_wall0 - wall);
    int64_t elapsed_ns = wall_delta_ns
                       - (int64_t)p->latency_ms * 1000000LL;
    uint32_t play_pos = p->rt_anchor_pos0
                      + (uint32_t)((elapsed_ns * p->format.sample_rate) / 1000000000LL);
    uint32_t frame_1 = play_pos + 11035;
    uint32_t frame_2 = frame_1 + 77175;

    uint32_t be = htonl(frame_1);
    memcpy(pkt + 4, &be, 4);

    uint32_t wall_hi = htonl((uint32_t)(wall >> 32));
    uint32_t wall_lo = htonl((uint32_t)(wall & 0xFFFFFFFF));
    memcpy(pkt + 8, &wall_hi, 4);
    memcpy(pkt + 12, &wall_lo, 4);

    be = htonl(frame_2);
    memcpy(pkt + 16, &be, 4);

    uint64_t cid = ap2_ptp_master_clock_id(p->ptp);
    uint32_t cid_hi = htonl((uint32_t)(cid >> 32));
    uint32_t cid_lo = htonl((uint32_t)(cid & 0xFFFFFFFF));
    memcpy(pkt + 20, &cid_hi, 4);
    memcpy(pkt + 24, &cid_lo, 4);

    ap2_send_result_t result = ap2_io_send_datagram_deadline(
        p->ctrl_sock, pkt, sizeof(pkt),
        (const struct sockaddr *)&p->ctrl_addr, sizeof(p->ctrl_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    if (result == AP2_SEND_SENT) {
        p->sync_packets_sent++;
    } else {
        p->sync_packets_dropped++;
        if (result == AP2_SEND_FATAL) {
            atomic_store(&p->media_healthy, false);
            LOG_ERROR("[AP2] PTP sync send failed: %s", strerror(errno));
        } else if (p->sync_packets_dropped <= 3 ||
                   (p->sync_packets_dropped % 100) == 0) {
            LOG_WARN("[AP2] PTP sync send transiently dropped");
        }
    }
    int32_t send_ahead = (int32_t)(p->rtp_timestamp - play_pos);
    LOG_DEBUG("[AP2] TX PTP sync %s play_pos=%u rtp_head=%u ahead=%d frames wall=%" PRIu64 "ns",
              first ? "(initial)" : "", play_pos, p->rtp_timestamp,
              send_ahead, wall);
    return result;
}

/* ---- Native AP2 audio encryption ---- */

static bool ap2_encrypt_audio(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t aad[8], const uint8_t *plain,
                              int plain_len, uint8_t *cipher,
                              int *cipher_len, uint8_t tag[AP2_CHACHA_TAG_SIZE])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int len = 0;
    int total = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) > 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) > 0 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) > 0 &&
        EVP_EncryptUpdate(ctx, NULL, &len, aad, 8) > 0 &&
        EVP_EncryptUpdate(ctx, cipher, &len, plain, plain_len) > 0;
    if (ok) {
        total = len;
        ok = EVP_EncryptFinal_ex(ctx, cipher + total, &len) > 0;
        total += len;
    }
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(
                 ctx, EVP_CTRL_AEAD_GET_TAG, AP2_CHACHA_TAG_SIZE, tag) > 0;
    EVP_CIPHER_CTX_free(ctx);
    if (ok) *cipher_len = total;
    return ok;
}


static ap2_send_result_t ap2_native_send_chunk(
    struct ap2cl_s *p, uint8_t *sample, int frames)
{
    if (p->data_sock < 0 || !p->alac || !p->hap) {
        LOG_ERROR("[AP2] Realtime audio session is incomplete");
        return AP2_SEND_FATAL;
    }

    /* Send initial sync/anchor packet before the very first audio packet, then
     * periodically every ~100 chunks (~0.8s at 352fpp/44.1kHz). PTP sessions use
     * the 28-byte anchor form; NTP sessions the 20-byte form. */
    ap2_send_result_t sync_result = AP2_SEND_SENT;
    if (p->first_packet) {
        sync_result = p->use_ptp ? ap2_send_sync_packet_ptp(p, true)
                                 : ap2_send_sync_packet(p, true);
    } else if ((p->seq_number % 100) == 0) {
        sync_result = p->use_ptp ? ap2_send_sync_packet_ptp(p, false)
                                 : ap2_send_sync_packet(p, false);
    }
    if (sync_result == AP2_SEND_FATAL) return AP2_SEND_FATAL;

    /* ALAC encode */
    uint8_t *encoded = NULL;
    int enc_size = 0;
    pcm_to_alac(p->alac, sample, frames, &encoded, &enc_size);
    if (!encoded || enc_size <= 0) {
        LOG_ERROR("[AP2] ALAC encoder failed for realtime audio");
        free(encoded);
        return AP2_SEND_FATAL;
    }

    /* Build RTP header (12 bytes) */
    uint8_t rtp_hdr[12];
    rtp_hdr[0] = 0x80;
    rtp_hdr[1] = p->first_packet ? 0xE0 : 0x60;
    rtp_hdr[2] = (p->seq_number >> 8) & 0xFF;
    rtp_hdr[3] = p->seq_number & 0xFF;
    uint32_t ts_be = htonl(p->rtp_timestamp);
    memcpy(rtp_hdr + 4, &ts_be, 4);
    uint32_t ssrc_be = htonl(p->ssrc);
    memcpy(rtp_hdr + 8, &ssrc_be, 4);
    /* Encrypt the ALAC payload with ChaCha20-Poly1305 (AP2 realtime audio).
     * Key:   the 32-byte pairing audio key (shk) — the raw X25519 secret for
     *        pair-verify, SHA512(S)[:32] for transient pairing.
     * Nonce: 12 bytes, all zero except the 2-byte sequence number at [4..5].
     * AAD:   RTP header bytes 4..11 (timestamp + SSRC).
     * Wire:  [12B RTP hdr][ciphertext][16B tag][8B nonce]; the trailing 8 bytes
     *        are the low 8 nonce bytes so the receiver can reconstruct it. */
    const uint8_t *audio_key = ap2_hap_get_shared_secret(p->hap);
    if (!audio_key) {
        LOG_ERROR("[AP2] No shared secret for audio encryption");
        free(encoded);
        return AP2_SEND_FATAL;
    }

    uint8_t nonce[12];
    memset(nonce, 0, 12);
    /* seqnum at offset 4 in native (little-endian) byte order, matching owntone
     * (memcpy(nonce+4, &seqnum, 2)). The same bytes are appended to the wire. */
    uint16_t seq16 = (uint16_t)p->seq_number;
    memcpy(nonce + 4, &seq16, 2);

    int pkt_size = 12 + enc_size + AP2_CHACHA_TAG_SIZE + 8;
    uint8_t *pkt = malloc(pkt_size);
    if (!pkt) {
        LOG_ERROR("[AP2] Cannot allocate realtime RTP packet");
        free(encoded);
        return AP2_SEND_FATAL;
    }
    memcpy(pkt, rtp_hdr, 12);

    int ct_len = 0;
    uint8_t tag[AP2_CHACHA_TAG_SIZE];
    bool encrypted_ok = ap2_encrypt_audio(
        audio_key, nonce, rtp_hdr + 4, encoded, enc_size,
        pkt + 12, &ct_len, tag);
    free(encoded);
    if (!encrypted_ok) {
        LOG_ERROR("[AP2] Realtime audio encryption failed");
        free(pkt);
        return AP2_SEND_FATAL;
    }
    memcpy(pkt + 12 + ct_len, tag, sizeof(tag));

    /* Append the 8-byte per-packet nonce (the low 8 bytes of the 12-byte nonce)
     * so the receiver can reconstruct it. AP2 realtime wire format is
     * [12B RTP hdr][ciphertext][16B tag][8B nonce]; without the suffix every
     * packet fails its auth tag and is silently dropped. */
    memcpy(pkt + 12 + ct_len + AP2_CHACHA_TAG_SIZE, nonce + 4, 8);
    int actual_pkt_size = 12 + ct_len + AP2_CHACHA_TAG_SIZE + 8;
    ap2_send_result_t result = ap2_io_send_datagram_deadline(
        p->data_sock, pkt, (size_t)actual_pkt_size,
        (const struct sockaddr *)&p->data_addr, sizeof(p->data_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    /* Keep the wire bytes before releasing them: a receiver can ask for this
     * packet back at any point inside its buffer window. */
    if (result == AP2_SEND_SENT)
        ap2_rtx_store(p, (uint16_t)p->seq_number, pkt, actual_pkt_size);
    free(pkt);

    if (result == AP2_SEND_SENT) {
        p->audio_packets_sent++;
    } else if (result == AP2_SEND_DROPPED) {
        p->audio_packets_dropped++;
        if (p->audio_packets_dropped <= 3 ||
            (p->audio_packets_dropped % 100) == 0)
            LOG_WARN("[AP2] RTP send transiently dropped seq=%u rtptime=%u",
                     p->seq_number, p->rtp_timestamp);
    } else {
        LOG_ERROR("[AP2] Realtime RTP send failed seq=%u rtptime=%u "
                  "bytes=%d: %s",
                  p->seq_number, p->rtp_timestamp, actual_pkt_size,
                  strerror(errno));
        return AP2_SEND_FATAL;
    }
    /* A transient local UDP drop exposes a sequence gap rather than retrying
     * an old timestamp late, so the media timeline always advances. Keep the
     * restart marker armed until its sync and audio packet reach the receiver. */
    if (result == AP2_SEND_SENT && sync_result == AP2_SEND_SENT)
        p->first_packet = false;
    p->seq_number++;
    p->rtp_timestamp += frames;
    p->head_ts += frames;
    if ((p->seq_number % 500) == 0)
        LOG_DEBUG("[AP2] RTP heartbeat seq=%u rtptime=%u head=%" PRIu64
                  " anchor_valid=%d",
                  p->seq_number, p->rtp_timestamp, p->head_ts,
                  p->rt_anchor_valid ? 1 : 0);

    return result;
}

/* ---- RAOP-compat connect ---- */

static bool ap2_raop_compat_connect(struct ap2cl_s *p)
{
    /* auth-setup (the MFi X25519 exchange some AP2 receivers require) is handled
     * inside libraop's raopcl_connect() via rtspcl_auth_setup() when the device's
     * et field advertises type 4 — on the real RTSP socket, not a throwaway one. */

    /* Use libraop RAOP client */
    struct in_addr host_addr, player_addr;
    uint32_t netmask;
    char *iface = NULL;
    host_addr = get_interface(p->iface, &iface, &netmask);
    NFREE(iface);

    struct hostent *hostent = gethostbyname(p->device.address);
    if (!hostent) return false;
    memcpy(&player_addr.s_addr, hostent->h_addr_list[0], hostent->h_length);

    int latency_frames = MS2TS(p->latency_ms, p->format.sample_rate);
    bool mfi_auth = p->am && strcasestr(p->am, "airport");

    p->raopcl = raopcl_create(
        host_addr, 0, 0,
        p->dacp_id ? p->dacp_id : "1A2B3D4EA1B2C3D4",
        p->active_remote ? p->active_remote : "0",
        RAOP_ALAC, DEFAULT_FRAMES_PER_CHUNK, latency_frames,
        RAOP_CLEAR, mfi_auth,
        p->secret ? p->secret : "", p->password,
        p->et ? p->et : "0,4", p->md ? p->md : "0,1,2",
        p->format.sample_rate, p->format.bit_depth, p->format.channels,
        p->volume > 0 ? raopcl_float_volume(p->volume) : -144.0f);

    if (!p->raopcl) {
        ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, 0,
                              "cannot create the RAOP-compatible client");
        return false;
    }

    if (!raopcl_connect(p->raopcl, player_addr, p->device.port, p->volume > 0)) {
        /* libraop does not expose the RTSP status of the failure, so a rejected
         * password is not distinguishable from an unreachable device here. */
        ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, 0,
                              "RAOP-compatible connect failed");
        ap2_raop_session_cleanup(p);
        return false;
    }

    ap2_set_connect_error(p, AP2_CONNECT_ERROR_NONE, 0, "%s", "");
    p->state = AP2_CONNECTED;
    return true;
}

/* ---- Public API ---- */

struct ap2cl_s *ap2cl_create(
    ap2_device_info_t *device, ap2_audio_format_t *format,
    const char *auth, const char *password,
    const char *dacp_id, const char *active_remote,
    int latency_ms, int volume)
{
    struct ap2cl_s *p = calloc(1, sizeof(struct ap2cl_s));
    if (!p) return NULL;

    p->device = *device;
    p->format = *format;
    p->state = AP2_DOWN;
    p->latency_ms = latency_ms;
    p->volume = volume;
    p->sock_fd = -1;
    p->data_sock = -1;
    p->ctrl_sock = -1;
    p->events_sock = -1;
    pthread_mutex_init(&p->rtsp_lock, NULL);
    pthread_mutex_init(&p->mrp_lock, NULL);
    pthread_mutex_init(&p->mrp_publish_lock, NULL);
    pthread_mutex_init(&p->rtx_lock, NULL);
    pthread_mutex_init(&p->clock_verify_lock, NULL);
    atomic_init(&p->clock_verify_thread_stop, true);
    atomic_init(&p->clock_poll_wanted, false);
    atomic_init(&p->rtx_stop, true);
    atomic_init(&p->rtx_requested, 0);
    atomic_init(&p->rtx_answered, 0);
    atomic_init(&p->rtx_expired, 0);
    atomic_init(&p->rtsp_dead, false);
    atomic_init(&p->media_healthy, true);
    atomic_init(&p->feedback_failures, 0);
    atomic_init(&p->mrp_event_health, -1);
    atomic_init(&p->feedback_stop, true);
    atomic_init(&p->rtp_offset, 0);
    if (dacp_id) p->dacp_id = strdup(dacp_id);
    if (active_remote) p->active_remote = strdup(active_remote);
    if (password) p->password = strdup(password);
    if (auth && strlen(auth) == 192) {
        p->auth_credentials = strdup(auth);
        p->flow = FLOW_NATIVE_AP2;
    } else {
        p->flow = FLOW_RAOP_COMPAT;
    }

    LOG_INFO("[AP2] Created client for %s (%s:%d) [%s]",
             device->name ? device->name : "unknown",
             device->address, device->port,
             p->flow == FLOW_NATIVE_AP2 ? "native AP2" : "RAOP-compat");
    return p;
}

static void ap2_raop_session_cleanup(struct ap2cl_s *p)
{
    if (p->raopcl) {
        raopcl_destroy(p->raopcl);
        p->raopcl = NULL;
    }
}

bool ap2cl_destroy(struct ap2cl_s *p)
{
    if (!p) return false;
    ap2_feedback_stop(p);
    ap2_clock_verify_disarm(p);
    ap2_clock_verify_reap_poller(p);
    /* Must join before the control socket closes: the responder polls it. */
    ap2_rtx_stop(p);
    /* Preserve the on-wire shutdown sequence (final MediaRemote stopped state,
     * MRP disconnect, RTSP TEARDOWN) on the normal EOF path too. */
    if (p->state != AP2_DOWN || p->sock_fd >= 0)
        ap2cl_disconnect(p);
    ap2_raop_session_cleanup(p);
    if (p->hap) ap2_hap_destroy(p->hap);
    if (p->ptp) ap2_ptp_destroy(p->ptp);
    if (p->mrp) ap2_mrp_destroy(p->mrp);
    if (p->alac) alac_delete_encoder(p->alac);
    /* Close the RTSP socket under the lock so an in-flight request/response
     * cycle on another thread finishes first and a later cycle sees -1
     * instead of writing into a reused descriptor. */
    pthread_mutex_lock(&p->rtsp_lock);
    if (p->sock_fd >= 0) { close(p->sock_fd); p->sock_fd = -1; }
    pthread_mutex_unlock(&p->rtsp_lock);
    pthread_mutex_destroy(&p->rtsp_lock);
    pthread_mutex_destroy(&p->mrp_lock);
    pthread_mutex_destroy(&p->mrp_publish_lock);
    pthread_mutex_destroy(&p->rtx_lock);
    pthread_mutex_destroy(&p->clock_verify_lock);
    if (p->data_sock >= 0) close(p->data_sock);
    if (p->ctrl_sock >= 0) close(p->ctrl_sock);
    if (p->events_sock >= 0) close(p->events_sock);
    free(p->dacp_id); free(p->active_remote); free(p->iface); free(p->publish_ip);
    free(p->secret); free(p->password); free(p->et); free(p->md); free(p->am);
    free(p->auth_credentials);
    free(p);
    return true;
}

void ap2cl_set_raop_props(struct ap2cl_s *p,
                           const char *iface, const char *secret,
                           const char *et, const char *md, const char *am)
{
    if (!p) return;
    if (iface) { free(p->iface); p->iface = strdup(iface); }
    if (secret) { free(p->secret); p->secret = strdup(secret); }
    if (et) { free(p->et); p->et = strdup(et); }
    if (md) { free(p->md); p->md = strdup(md); }
    if (am) { free(p->am); p->am = strdup(am); }
}

void ap2cl_force_native(struct ap2cl_s *p)
{
    if (!p) return;
    if (p->flow != FLOW_NATIVE_AP2) {
        p->flow = FLOW_NATIVE_AP2;
        LOG_INFO("[AP2] Forcing native AP2 flow (transient pairing without credentials)");
    }
}

void ap2cl_set_publish_ip(struct ap2cl_s *p, const char *ip)
{
    if (!p || !ip) return;
    free(p->publish_ip);
    p->publish_ip = strdup(ip);
}

void ap2cl_set_ptp(struct ap2cl_s *p, bool enable)
{
    if (!p) return;
    p->ptp_forced = true;
    p->ptp_enabled = enable;
    LOG_INFO("[AP2] PTP timing %s", enable ? "forced ON" : "forced OFF");
}

void ap2cl_set_ptp_shared(struct ap2cl_s *p, bool enable)
{
    if (!p) return;
    p->ptp_shared = enable;
    LOG_INFO("[AP2] Shared PTP daemon clock %s", enable ? "preferred" : "disabled");
}

void ap2cl_set_start_join(struct ap2cl_s *p, bool join)
{
    if (!p) return;
    p->start_join_pending = join;
}

void ap2cl_set_remote_command_callback(
    struct ap2cl_s *p, ap2_remote_command_cb_t callback, void *userdata)
{
    if (!p) return;
    p->remote_command_cb = callback;
    p->remote_command_userdata = userdata;
}

ap2_connect_error_t ap2cl_connect_error(struct ap2cl_s *p, int *http_status,
                                        const char **detail)
{
    if (!p) {
        if (http_status) *http_status = 0;
        if (detail) *detail = "";
        return AP2_CONNECT_ERROR_GENERIC;
    }
    if (http_status) *http_status = p->connect_http_status;
    if (detail) *detail = p->connect_detail;
    return p->connect_error;
}

bool ap2cl_connect(struct ap2cl_s *p)
{
    if (!p) return false;
    ap2_set_connect_error(p, AP2_CONNECT_ERROR_GENERIC, 0, "connect failed");
    if (p->flow == FLOW_NATIVE_AP2) {
        p->splice_timeline =
            !ap2_splice_denied(p->device.txt_records, p->am);
        p->apple_model = ap2_apple_model(p->device.txt_records, p->am);
        if (p->splice_timeline) {
            LOG_INFO("[AP2] splice timeline (discard-free warm path, "
                     "%d ms queue depth)", AP2_SPLICE_PACING_MS);
        } else {
            LOG_INFO("[AP2] deny-listed receiver: classic flush + re-anchor "
                     "warm path");
        }
        LOG_INFO("[AP2] Connecting via native AP2 flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_native_connect(p);
    } else {
        if (p->state != AP2_DOWN && p->raopcl)
            ap2cl_disconnect(p);
        ap2_raop_session_cleanup(p);
        LOG_INFO("[AP2] Connecting via RAOP-compatible flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_raop_compat_connect(p);
    }
}

bool ap2cl_disconnect(struct ap2cl_s *p)
{
    if (!p) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        ap2_feedback_stop(p);
        ap2_rtx_stop(p);
        /* Publish the final stopped state before disconnecting MediaRemote,
         * while the encrypted RTSP session is still live. */
        ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_STOPPED, true);
        pthread_mutex_lock(&p->mrp_publish_lock);
        pthread_mutex_lock(&p->mrp_lock);
        struct ap2_mrp_ctx *mrp = p->mrp;
        pthread_mutex_unlock(&p->mrp_lock);
        if (mrp) {
            ap2_mrp_stop(mrp);
            atomic_store(&p->mrp_event_health, 0);
        }
        pthread_mutex_unlock(&p->mrp_publish_lock);
        if (p->events_sock >= 0) {
            close(p->events_sock);
            p->events_sock = -1;
        }
        if (p->sock_fd >= 0) {
            uint8_t *resp = NULL; int resp_len = 0;
            ap2_rtsp_send(p, "TEARDOWN", p->session_url, NULL, 0, NULL, &resp, &resp_len);
            free(resp);
            /* Under the lock: see ap2cl_destroy. */
            pthread_mutex_lock(&p->rtsp_lock);
            close(p->sock_fd); p->sock_fd = -1;
            pthread_mutex_unlock(&p->rtsp_lock);
        }
    } else if (p->raopcl) {
        raopcl_disconnect(p->raopcl);
    }
    p->state = AP2_DOWN;
    return true;
}

static uint64_t ap2_unix_ms_to_ntp(uint64_t ms)
{
    return ((ms / 1000) << 32) | (((ms % 1000) << 32) / 1000);
}

static uint64_t ap2_ntp_to_unix_ms(uint64_t ntp)
{
    return (ntp >> 32) * 1000ULL + (((ntp & 0xFFFFFFFFULL) * 1000ULL) >> 32);
}

/* ---- Receiver clock readiness (PTP probe streak) ---- */

/* Earliest instant (unix ms) the receiver can seat an anchor, from a live
 * probe-streak snapshot. Third-party receivers get the full servo-lock
 * window from the streak start; Apple models also honor the observed fast
 * bound (third exchange + settle), whichever is earlier. */
static uint64_t ap2_clock_ready_from(const struct ap2cl_s *p,
                                     const struct ap2_ptp_exchange *ex,
                                     uint64_t now_unix_ms)
{
    uint64_t first_ms = ex->first_ms < now_unix_ms ? ex->first_ms : now_unix_ms;
    uint64_t ready = now_unix_ms - first_ms + AP2_CLOCK_LOCK_MS;
    if (p->apple_model && ex->count >= AP2_CLOCK_SEAT_EXCHANGES) {
        uint64_t third_ms =
            ex->third_ms < now_unix_ms ? ex->third_ms : now_unix_ms;
        uint64_t fast = now_unix_ms - third_ms + AP2_CLOCK_SETTLE_MS;
        if (fast < ready) ready = fast;
    }
    return ready;
}

/* Live probe streak for this session's receiver. Blocks up to the control
 * timeout in shared-daemon mode, so the audio loop must go through the
 * poller snapshot instead of calling this. */
static bool ap2_clock_query(struct ap2cl_s *p, struct ap2_ptp_exchange *ex)
{
    return ap2_ptp_peer_exchange(p->ptp, p->device.address, ex);
}

/* Raise a commit's feasibility floor to the receiver's clock readiness. With
 * no live streak the clock is cold: the floor is left alone (a receiver that
 * has just connected takes about a second to start probing, so an early
 * commit has nothing to measure yet) and *clock_cold tells the commit to arm
 * the post-commit verification instead. Callers that can wait should instead
 * follow ap2cl_clock_readiness from connect and commit once it reports ready. */
static uint64_t ap2_clock_floor(struct ap2cl_s *p, uint64_t floor_ntp,
                                bool *clock_cold)
{
    *clock_cold = false;
    if (p->flow != FLOW_NATIVE_AP2 || !p->use_ptp) return floor_ntp;
    struct ap2_ptp_exchange ex;
    if (!ap2_clock_query(p, &ex)) {
        *clock_cold = true;
        return floor_ntp;
    }
    uint64_t now_ms = ap2_ntp_to_unix_ms(raopcl_get_ntp(NULL));
    uint64_t ready_ms = ap2_clock_ready_from(p, &ex, now_ms);
    uint64_t ready_ntp = ap2_unix_ms_to_ntp(ready_ms);
    if (ready_ntp <= floor_ntp) return floor_ntp;
    LOG_INFO("[AP2] receiver clock floor: probe streak %" PRIu64 " ms old "
             "(%u exchanges), ready in %" PRIu64 " ms",
             ex.first_ms, ex.count, ready_ms - now_ms);
    return ready_ntp;
}

/* Stop the verification without an event (new commit, flush, teardown). The
 * poller is signalled here and joined by the next arm or the destroy, unless
 * readiness reporting still wants its snapshot. */
static void ap2_clock_verify_disarm(struct ap2cl_s *p)
{
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_verify_armed = false;
    /* A withheld ack falls due the moment the verification can no longer
     * answer it: the caller sees both flags drop together and emits it with
     * the instant that stands. */
    p->start_ack_deferred = false;
    pthread_mutex_unlock(&p->clock_verify_lock);
    if (!atomic_load(&p->clock_poll_wanted))
        atomic_store(&p->clock_verify_thread_stop, true);
}

static void ap2_clock_verify_reap_poller(struct ap2cl_s *p)
{
    if (!p->clock_verify_thread_started) return;
    atomic_store(&p->clock_verify_thread_stop, true);
    pthread_join(p->clock_verify_thread, NULL);
    p->clock_verify_thread_started = false;
}

/* Shared-daemon mode: keep the probe-streak snapshot fresh over the control
 * channel so the audio loop's poll never blocks on UDP. */
static void *ap2_clock_verify_poller(void *arg)
{
    struct ap2cl_s *p = arg;
    while (!atomic_load(&p->clock_verify_thread_stop)) {
        struct ap2_ptp_exchange ex;
        bool have = ap2_clock_query(p, &ex);
        uint64_t now_ms = ap2_ntp_to_unix_ms(raopcl_get_ntp(NULL));
        pthread_mutex_lock(&p->clock_verify_lock);
        bool armed = p->clock_verify_armed;
        p->clock_verify_have_exchange = have;
        if (have) {
            p->clock_verify_exchange = ex;
            /* The stall window is measured from here because this loop is the
             * only thing that follows the receiver for a whole session:
             * readiness reporting goes terminal at the first ready and stops
             * asking until a flush or start re-arms it. */
            p->clock_last_streak_unix_ms = now_ms;
        }
        pthread_mutex_unlock(&p->clock_verify_lock);
        if (!armed && !atomic_load(&p->clock_poll_wanted)) break;
        for (int i = 0; i < AP2_CLOCK_VERIFY_POLL_MS / 50 &&
                        !atomic_load(&p->clock_verify_thread_stop); i++)
            usleep(50000);
    }
    return NULL;
}

/* Run the snapshot poller in shared-daemon mode, where the streak query is a
 * blocking UDP round-trip no caller on the audio loop may make. Idempotent, and
 * serialized by the send lock: connect starts it before the command thread
 * exists, and every later call comes from a commit, which the session engine
 * brackets with that lock. */
static void ap2_clock_poller_ensure(struct ap2cl_s *p)
{
    if (!ap2_ptp_shared_active(p->ptp)) return;
    if (p->clock_verify_thread_started &&
        !atomic_load(&p->clock_verify_thread_stop))
        return;
    ap2_clock_verify_reap_poller(p);
    atomic_store(&p->clock_verify_thread_stop, false);
    if (pthread_create(&p->clock_verify_thread, NULL,
                       ap2_clock_verify_poller, p) == 0)
        p->clock_verify_thread_started = true;
    else
        atomic_store(&p->clock_verify_thread_stop, true);
}

/* Arm the post-commit verification for a cold-clock commit: the receiver's
 * probe streak had not begun when the instant was committed, so the committed
 * instant must be checked against readiness once it does.
 * Only a join-marked start (landing on an already-live group timeline) is
 * ENFORCED with a forward correction; an origin start is observed and
 * reported only — its receivers self-seat a fresh session cleanly, and
 * moving one member of a group origin desyncs the group (ear-confirmed).
 * Anchors without enough runway to act before the initial fill are not
 * armed (their fill starts immediately; nothing could be corrected). */
static void ap2_clock_verify_arm(struct ap2cl_s *p, uint64_t requested_unix_ms,
                                 uint64_t at_unix_ms, bool enforce)
{
    if (!p->splice_timeline || !p->use_ptp) return;
    uint64_t now_ms = ap2_ntp_to_unix_ms(raopcl_get_ntp(NULL));
    if (at_unix_ms < now_ms + AP2_CLOCK_VERIFY_MIN_WINDOW_MS) return;
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_verify_armed = true;
    p->clock_verify_enforce = enforce;
    p->clock_verify_requested_unix_ms = requested_unix_ms;
    p->clock_verify_anchor_unix_ms = at_unix_ms;
    p->clock_verify_packets_at_arm = p->audio_packets_sent;
    p->clock_verify_next_poll_ntp = 0;
    p->clock_verify_have_exchange = false;
    pthread_mutex_unlock(&p->clock_verify_lock);
    LOG_INFO("[AP2] clock verification armed (%s): cold receiver clock, "
             "anchor in %" PRIu64 " ms", enforce ? "join, enforcing" : "observe",
             at_unix_ms - now_ms);
    ap2_clock_poller_ensure(p);
}

/* Resolve a commanded start: a request at or beyond `floor_ntp` is honored
 * exactly; one behind it (or 0) is corrected FORWARD to the floor — audio
 * always flows, never silently misplaced or refused. *ntp_start receives the
 * audible instant and *at_unix_ms the same value in unix ms, so the caller's
 * ack always carries the truth. A correction of a nonzero request is logged
 * here; the caller re-aligns groups from the acks. */
static void ap2_resolve_start(uint64_t start_unix_ms, uint64_t floor_ntp,
                              uint64_t *ntp_start, uint64_t *at_unix_ms)
{
    uint64_t requested =
        start_unix_ms ? ap2_unix_ms_to_ntp(start_unix_ms) : 0;
    if (requested >= floor_ntp) {
        *ntp_start = requested;
        if (at_unix_ms) *at_unix_ms = start_unix_ms;
        return;
    }
    if (start_unix_ms) {
        /* Corrected forward with one extra lead of slack: the floor moves
         * with the wall clock, so a corrective retry at a bare floor would
         * chase it forever. A request of 0 takes the floor directly (no
         * retry follows a pick). */
        *ntp_start = floor_ntp + MS2NTP(AP2_MIN_WARM_LEAD_MS);
        if (at_unix_ms) *at_unix_ms = ap2_ntp_to_unix_ms(*ntp_start);
        LOG_WARN("[AP2] start %llu ms is %llu ms behind the feasibility "
                 "floor; corrected forward",
                 (unsigned long long)start_unix_ms,
                 (unsigned long long)(ap2_ntp_to_unix_ms(floor_ntp) -
                                      start_unix_ms));
        return;
    }
    *ntp_start = floor_ntp;
    if (at_unix_ms) *at_unix_ms = ap2_ntp_to_unix_ms(floor_ntp);
}

/* Drop a corrected join's content cut. A new commit, a park, or a teardown
 * legitimately supersedes the corrected timeline, so the debt goes with it —
 * but never silently: bytes still outstanding are content the audio loop
 * retained past the correction, and on a timeline that still stands they
 * render exactly that late. */
static void ap2_content_skip_reset(struct ap2cl_s *p, const char *cause)
{
    if (p->content_skip_bytes) {
        int input_bpf =
            (p->format.bit_depth <= 16 ? 2 : 4) * p->format.channels;
        uint64_t rate = (uint64_t)input_bpf * p->format.sample_rate;
        LOG_WARN("[AP2] %s drops %u bytes (%" PRIu64 " ms) of an undrained "
                 "corrected-join content cut", cause, p->content_skip_bytes,
                 rate ? (uint64_t)p->content_skip_bytes * 1000ULL / rate : 0);
    }
    p->content_skip_bytes = 0;
}

ap2_commit_result_t ap2cl_start(struct ap2cl_s *p, uint64_t start_unix_ms,
                                uint64_t *at_unix_ms)
{
    if (!p) return AP2_COMMIT_FAILED;
    ap2_clock_verify_disarm(p);
    bool join = p->start_join_pending;
    p->start_join_pending = false;
    p->content_paused = false;
    p->content_stopped = false;
    ap2_content_skip_reset(p, "START");
    uint64_t ntp_start = 0;
    bool clock_cold = false;
    uint64_t floor_ntp =
        ap2_clock_floor(p, raopcl_get_ntp(NULL) + MS2NTP(AP2_MIN_WARM_LEAD_MS),
                        &clock_cold);
    uint64_t at_local = 0;
    ap2_resolve_start(start_unix_ms, floor_ntp, &ntp_start, &at_local);
    if (at_unix_ms) *at_unix_ms = at_local;
    if (p->flow == FLOW_NATIVE_AP2) {
        /* Offset the RTP timeline per process: streams in one group share
         * ntpstart, and with identical pos0 two sessions from one host are
         * wire-identical twins (same clock id, anchor tuple, ssrc, source) —
         * Sonos household stream tracking then cross-wires them and one
         * device goes silent. The anchor line carries the same offset, so
         * the audible schedule is untouched. */
        atomic_store(&p->rtp_offset,
                     (uint32_t)(getpid() * 2654435761u) & 0x0FFFFF00u);
        /* head_ts stays in the pure scheduling domain (pacing compares it to
         * the wall frame clock); the offset is applied only to the on-wire
         * RTP timestamps and the anchor, which advance in lockstep. */
        p->head_ts = NTP2TS(ntp_start, p->format.sample_rate);
        p->rtp_timestamp = (uint32_t)p->head_ts +
                           atomic_load(&p->rtp_offset);
        p->seq_number = (uint16_t)(getpid() * 40503u);
        p->start_ntp = ntp_start;
        p->reanchor_shifted_frames = 0;
        p->state = AP2_STREAMING;
        atomic_store(&p->media_healthy, true);
        pthread_mutex_lock(&p->mrp_publish_lock);
        pthread_mutex_lock(&p->mrp_lock);
        if (p->mrp) ap2_mrp_set_playing(p->mrp, true);
        pthread_mutex_unlock(&p->mrp_lock);
        pthread_mutex_unlock(&p->mrp_publish_lock);
        /* Announce the timeline IMMEDIATELY: with a future ntpstart the first
         * audio chunk (and the anchor coupled to it) would otherwise only go
         * out once pacing releases it, and a receiver that sees no time
         * announce shortly after RECORD can abandon the stream. The line is
         * fully determined by ntpstart, so this and the per-chunk announces
         * agree. */
        if (p->use_ptp && p->ctrl_sock >= 0 &&
            ap2_send_sync_packet_ptp(p, true) == AP2_SEND_FATAL) {
            atomic_store(&p->media_healthy, false);
            return AP2_COMMIT_FAILED;
        }
        if (clock_cold) {
            ap2_clock_verify_arm(p, start_unix_ms, at_local, join);
            /* Withhold the join's ack for that verification to answer: this
             * anchor can still move, and an ack carrying the moved instant
             * lets the caller map its content straight onto it instead of
             * mapping to a guess and being corrected afterwards. Only where
             * the verification actually armed — nothing else would ever
             * emit it. */
            pthread_mutex_lock(&p->clock_verify_lock);
            p->start_ack_deferred = join && p->clock_verify_armed;
            pthread_mutex_unlock(&p->clock_verify_lock);
        }
        return AP2_COMMIT_OK;
    }
    if (!p->raopcl) return AP2_COMMIT_FAILED;
    int latency = raopcl_latency(p->raopcl);
    raopcl_start_at(p->raopcl, ntp_start - TS2NTP(latency, p->format.sample_rate));
    p->state = AP2_STREAMING;
    return AP2_COMMIT_OK;
}

/* Park the stream but keep the session warm (persistent standby). On the
 * splice timeline the CONTENT stops while the armed line keeps carrying
 * silence: a queue underrun while the session stays armed is an audible
 * noise burst on Apple receivers (measured at the group pause press, which
 * parks members through standby), and a line kept hot makes the next resume
 * a splice instead of a fresh anchor. The stock path discards buffered audio
 * with an RTSP FLUSH and drops back to CONNECTED so a later warm flush can
 * restart. Both publish the stopped playback state; the session engine's
 * idle timeout still ends a park that nothing ever resumes. */
void ap2cl_standby(struct ap2cl_s *p)
{
    if (!p || p->state == AP2_DOWN) return;
    ap2_clock_verify_disarm(p);
    p->content_paused = false;
    p->content_stopped = false;
    ap2_content_skip_reset(p, "standby");
    if (p->flow != FLOW_NATIVE_AP2) {
        if (p->raopcl) raop_session_standby(p->raopcl);
        p->state = AP2_CONNECTED;
        return;
    }
    if (p->splice_timeline) {
        /* Splice park: the client stays AP2_STREAMING and the audio loop
         * keeps the line fed with encoded silence — draining the shallow
         * queue here is the underrun noise trigger. content_stopped keeps
         * the published now-playing state truthful meanwhile; the frozen
         * anchor line survives the park for a hot-splice resume. */
        LOG_INFO("[AP2] splice standby: content stopped, keeping the line fed");
        p->content_stopped = true;
        p->splice_pad_frames = 0;
        ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_STOPPED, true);
        return;
    }
    if (!atomic_load(&p->rtsp_dead)) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "RTP-Info: seq=%u;rtptime=%u\r\n",
                 (unsigned)p->seq_number, (unsigned)p->rtp_timestamp);
        uint8_t *resp = NULL;
        int resp_len = 0;
        int status = ap2_rtsp_send_ex(p, "FLUSH", p->session_url, NULL, 0,
                                      NULL, hdr, &resp, &resp_len);
        free(resp);
        LOG_INFO("[AP2] standby FLUSH -> %d", status);
    }
    ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_STOPPED, true);
    p->rt_anchor_valid = false;
    p->state = AP2_CONNECTED;
}

/* Discard the receiver's buffered audio in place for a warm seek, keeping the
 * session and its timeline continuity. Native realtime sends the classic RTSP
 * FLUSH with the current RTP-Info; the RAOP-compat path uses libraop's flush.
 * The send gate lives in the session engine: it is idle-primed after a FLUSH
 * and calls no send until the next START commands ap2cl_resume. Sequence
 * numbers (and thus audio nonces) are never reset, so crypto state stays valid
 * across the boundary (measured accepted down to a ~150 ms lead on Sonos and
 * Apple TV once resumed). */
bool ap2cl_flush(struct ap2cl_s *p)
{
    if (!p || p->state == AP2_DOWN) return false;
    ap2_clock_verify_disarm(p);
    ap2_content_skip_reset(p, "FLUSH");

    if (p->flow != FLOW_NATIVE_AP2)
        return raop_session_flush(p->raopcl);

    if (atomic_load(&p->rtsp_dead)) return false;
    if (p->splice_timeline) {
        /* Splice warm path: never ask the receiver to discard. Its queued
         * audio (at most the shallow pacing depth) plays out while the session
         * engine swaps the content behind it; the next resume continues the
         * same timeline and anchor line. Any flush verb here — classic FLUSH
         * with any RTP-Info, or FLUSHBUFFERED — is what produces the audible
         * noise burst on Apple receivers. */
        LOG_INFO("[AP2] splice flush: keeping the receiver queue "
                 "(head seq=%u rtptime=%u)",
                 (unsigned)p->seq_number, (unsigned)p->rtp_timestamp);
        p->splice_pad_frames = 0;   /* the next resume computes a fresh pad */
        return true;
    }
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "RTP-Info: seq=%u;rtptime=%u\r\n",
             (unsigned)p->seq_number, (unsigned)p->rtp_timestamp);
    uint8_t *resp = NULL;
    int resp_len = 0;
    int status = ap2_rtsp_send_ex(p, "FLUSH", p->session_url, NULL, 0, NULL,
                                  hdr, &resp, &resp_len);
    free(resp);
    LOG_INFO("[AP2] warm FLUSH (seq=%u rtptime=%u) -> %d",
             (unsigned)p->seq_number, (unsigned)p->rtp_timestamp, status);
    if (status != 200) {
        LOG_WARN("[AP2] receiver did not accept FLUSH (%d)", status);
        return false;
    }
    /* Drop the stale anchor; ap2cl_resume freezes a fresh line at START. */
    p->rt_anchor_valid = false;
    return true;
}

/* Schedule the next pending sample audible at start_unix_ms (unix epoch ms),
 * resuming a stream after ap2cl_flush (the START after a FLUSH), under the
 * strict start contract: a nonzero instant is honored exactly or rejected
 * with the minimum feasible instant — never silently degraded. The splice
 * timeline pads silence up to the instant on its immutable anchor line; the
 * stock path re-bases the frozen anchor onto it. Sequence numbers and audio
 * nonces are untouched. */
ap2_commit_result_t ap2cl_resume(struct ap2cl_s *p, uint64_t start_unix_ms,
                                 uint64_t *at_unix_ms)
{
    if (!p || p->state == AP2_DOWN) return AP2_COMMIT_FAILED;
    ap2_clock_verify_disarm(p);
    bool join = p->start_join_pending;
    p->start_join_pending = false;
    p->content_paused = false;
    p->content_stopped = false;
    ap2_content_skip_reset(p, "resume");

    if (p->flow != FLOW_NATIVE_AP2) {
        ap2_commit_result_t r =
            raop_session_start_at(p->raopcl, start_unix_ms, at_unix_ms);
        if (r == AP2_COMMIT_OK) p->state = AP2_STREAMING;
        return r;
    }

    if (atomic_load(&p->rtsp_dead)) return AP2_COMMIT_FAILED;
    uint64_t now_ntp = raopcl_get_ntp(NULL);
    if (p->splice_timeline && p->rt_anchor_valid &&
        p->head_ts > NTP2TS(now_ntp, p->format.sample_rate)) {
        /* Splice resume (hot line: the receiver still holds queued audio).
         * The anchor line frozen at the first START is immutable for the
         * whole session, and the wire must stay bitstream-continuous —
         * a line move, a timestamp jump, and late frames are each an audible
         * noise trigger on Apple receivers (all measured). The commanded
         * start therefore selects the splice point ON the line, and the gap
         * between the frozen head and that instant is FILLED with encoded
         * silence: sequence numbers and timestamps advance normally and the
         * first real sample lands exactly on the commanded instant. Every
         * member of a sync group is handed the same accepted instant, so a
         * group splices sample-aligned by construction. The feasibility
         * floor is the head itself: anything earlier would require
         * discarding queued audio (the noise trigger), so such a request is
         * corrected FORWARD to the head and the ack carries the truth. A
         * start of 0 splices at the head — the earliest possible point. */
        uint64_t head_ntp = TS2NTP(p->head_ts, p->format.sample_rate);
        uint64_t requested =
            start_unix_ms ? ap2_unix_ms_to_ntp(start_unix_ms) : 0;
        if (start_unix_ms && requested >= head_ntp) {
            uint64_t target = NTP2TS(requested, p->format.sample_rate);
            uint64_t pad = target > p->head_ts ? target - p->head_ts : 0;
            p->splice_pad_frames = (uint32_t)pad;
            if (at_unix_ms) *at_unix_ms = start_unix_ms;
            LOG_INFO("[AP2] splice resume: padding %" PRIu64 " frames "
                     "(%" PRIu64 " ms) of silence to the commanded start",
                     pad, pad * 1000ULL / p->format.sample_rate);
        } else if (start_unix_ms) {
            /* Corrected forward — one minimum lead BEYOND the head, not AT
             * it: the idle keepalive advances the head 1:1 with the wall
             * clock, so an ack naming the head itself sends the caller's
             * corrective retry chasing a target that has always just moved
             * (measured: +212/+418/+15 ms rounds without convergence). One
             * lead of slack absorbs the command round-trip, so the retry at
             * the reported instant lands exactly. */
            uint64_t corrected_ntp =
                head_ntp + MS2NTP(AP2_MIN_WARM_LEAD_MS);
            uint64_t target = NTP2TS(corrected_ntp, p->format.sample_rate);
            p->splice_pad_frames =
                (uint32_t)(target > p->head_ts ? target - p->head_ts : 0);
            if (at_unix_ms) *at_unix_ms = ap2_ntp_to_unix_ms(corrected_ntp);
            LOG_WARN("[AP2] splice resume: start %llu ms is behind the head; "
                     "corrected forward to head + %d ms",
                     (unsigned long long)start_unix_ms, AP2_MIN_WARM_LEAD_MS);
        } else {
            p->splice_pad_frames = 0;
            if (at_unix_ms) *at_unix_ms = ap2_ntp_to_unix_ms(head_ntp);
            LOG_INFO("[AP2] splice resume at head (seq=%u rtptime=%u)",
                     (unsigned)p->seq_number, (unsigned)p->rtp_timestamp);
        }
        /* A commanded start zeroes the drift baseline on both sides (MA
         * resets its accumulated shift on START). */
        p->reanchor_shifted_frames = 0;
        p->state = AP2_STREAMING;
        atomic_store(&p->media_healthy, true);
        return AP2_COMMIT_OK;
    }
    /* A lapsed splice line (head at or behind the wall clock: the receiver
     * drained long ago — resume from a long park) falls through to the stock
     * re-anchor: a fresh line over a long-idle pipeline is the session-start
     * shape, which is clean; padding silence from a lapsed head would deliver
     * late frames, which is not. */
    p->splice_pad_frames = 0;
    if (p->state == AP2_STREAMING && !p->splice_timeline) {
        /* A re-anchor on an already-streaming stock stream (the corrective
         * round of a group start) must discard the receiver's audio from the
         * previous anchor first: without the flush the receiver keeps
         * rendering the buffered frames on the OLD line and stays offset by
         * the correction delta until the next clean anchor (measured on
         * Sonos as a persistent few-hundred-ms group desync). */
        ap2cl_flush(p);
    }
    uint64_t start_ntp = 0;
    bool clock_cold = false;
    uint64_t floor_ntp =
        ap2_clock_floor(p, now_ntp + MS2NTP(AP2_MIN_WARM_LEAD_MS), &clock_cold);
    uint64_t at_local = 0;
    ap2_resolve_start(start_unix_ms, floor_ntp, &start_ntp, &at_local);
    if (at_unix_ms) *at_unix_ms = at_local;
    /* Re-base the frozen line: head_ts moves in the scheduling domain, the
     * wire timestamp follows it plus the per-process offset, and the next
     * sync packet freezes the new line from start_ntp. */
    p->start_ntp = start_ntp;
    p->head_ts = NTP2TS(start_ntp, p->format.sample_rate);
    p->rtp_timestamp = (uint32_t)p->head_ts + atomic_load(&p->rtp_offset);
    p->reanchor_shifted_frames = 0;
    p->rt_anchor_valid = false;
    /* The first packet after a timeline discontinuity carries the RTP marker
     * and sends a fresh sync packet before its audio payload. */
    p->first_packet = true;
    p->state = AP2_STREAMING;
    atomic_store(&p->media_healthy, true);

    if (p->use_ptp && p->ctrl_sock >= 0 &&
        ap2_send_sync_packet_ptp(p, true) == AP2_SEND_FATAL) {
        atomic_store(&p->media_healthy, false);
        return AP2_COMMIT_FAILED;
    }
    if (clock_cold) ap2_clock_verify_arm(p, start_unix_ms, at_local, join);
    return AP2_COMMIT_OK;
}

ap2_send_result_t ap2cl_send_chunk(struct ap2cl_s *p, uint8_t *sample,
                                   int frames)
{
    if (!p || p->state != AP2_STREAMING) {
        LOG_ERROR("[AP2] Audio send attempted outside a streaming session");
        return AP2_SEND_FATAL;
    }
    if (p->flow == FLOW_NATIVE_AP2) {
        if (atomic_load(&p->rtsp_dead)) return AP2_SEND_FATAL;
        ap2_send_result_t result = ap2_native_send_chunk(p, sample, frames);
        if (result == AP2_SEND_FATAL)
            atomic_store(&p->media_healthy, false);
        return result;
    }
    if (!p->raopcl) return AP2_SEND_FATAL;
    uint64_t playtime;
    return raopcl_send_chunk(p->raopcl, sample, frames, &playtime)
               ? AP2_SEND_SENT
               : AP2_SEND_FATAL;
}

static uint64_t ap2_pacing_window_frames(struct ap2cl_s *p)
{
    /* Frame counts, so every term is scaled to the stream rate: a receiver
     * reports latencyMax in frames at the rate it is being fed. */
    uint64_t margin = MS2TS(AP2_PACING_MARGIN_MS, p->format.sample_rate);
    uint64_t window =
        p->dev_latency_max > margin
            ? p->dev_latency_max - margin
            : MS2TS(AP2_PACING_DEFAULT_BUFFER_MS - AP2_PACING_MARGIN_MS,
                    p->format.sample_rate);
    /* The splice timeline keeps the receiver queue shallow: with warm
     * boundaries expressed as content splices on one immutable line, the
     * queue depth IS the audible latency of every seek/next. Apple receivers
     * are the LAN-local targets owntone has fed at realtime pacing for years,
     * so the reduced dropout headroom is field-proven there. */
    if (p->splice_timeline) {
        uint64_t depth = (uint64_t)MS2TS(AP2_SPLICE_PACING_MS,
                                         p->format.sample_rate);
        return depth < window ? depth : window;
    }
    return window;
}

bool ap2cl_accept_frames(struct ap2cl_s *p)
{
    if (!p || p->state != AP2_STREAMING) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        if (atomic_load(&p->rtsp_dead)) return false;
        /* Pace against the ANCHOR DEADLINE, capped to the receiver's buffer.
         * Contract: frame f is AUDIBLE at its frame-clock position (the anchor
         * line starts one lead early), so f's deadline IS f. A frame delivered
         * more than the receiver's latencyMax before its deadline overflows
         * its buffer and is dropped (Sonos: 88200 = 2.0s) — release at most
         * `window` ahead: the reported window when known, else 1.75s (inside
         * every AirPlay receiver's standard 2s). Delivery therefore
         * runs up to ~window AHEAD of playback from the very first sample —
         * the receiver's buffer is filled before the scheduled start and the
         * start cannot underrun — while scheduled group starts stay safe no
         * matter how far ahead the start lies. */
        uint64_t now_ntp = raopcl_get_ntp(NULL);
        uint64_t now_ts = NTP2TS(now_ntp, p->format.sample_rate);
        uint64_t window = ap2_pacing_window_frames(p);
        if ((now_ts + window) < p->head_ts) return false;
        /* Space the releases. The window gate alone would hand the receiver
         * the whole window at once at stream start, which overruns its socket
         * buffer; steady state is far slower than this floor and unaffected. */
        uint64_t now_us = ap2_io_monotonic_ms() * 1000ULL;
        if (p->pace_last_release_us &&
            now_us - p->pace_last_release_us < AP2_FILL_MIN_PACKET_GAP_US)
            return false;
        p->pace_last_release_us = now_us;
        return true;
    }
    if (!p->raopcl) return false;
    return raopcl_accept_frames(p->raopcl);
}

/* Shared splice-pad recovery (input starvation and delivery stalls): queue
 * silence padding from the EFFECTIVE head — the frozen head plus any pad
 * already owed — up to now plus the recovery lead, on the same immutable
 * line. The audio loop sends the pad as ordinary encoded chunks, so the wire
 * stays bitstream-continuous through the stall (a stamp jump, a line move
 * and late frames are each an audible noise trigger). Playback shifts later
 * by the pad, reported as REANCHOR so MA keeps tracking the drift. Gating on
 * the effective head makes repeated calls idempotent while a queued pad
 * drains: once silence covering the gap is owed, the timeline is already
 * recovered and another call must not stack a second shift on top. Fires
 * only when the effective head is at or behind `lapse_ts` (each caller's
 * notion of "too late"). */
static bool ap2_splice_pad_to_lead(struct ap2cl_s *p, uint64_t now_ts,
                                   uint64_t lapse_ts, const char *cause)
{
    uint64_t window = ap2_pacing_window_frames(p);
    uint64_t latency = MS2TS(p->latency_ms, p->format.sample_rate);
    uint64_t recovery_lead = latency < window ? latency : window;
    uint64_t effective_head = p->head_ts + p->splice_pad_frames;
    if (effective_head > lapse_ts) return false;
    uint64_t target = now_ts + recovery_lead;
    if (target <= effective_head) return false;
    uint64_t pad = target - effective_head;
    p->splice_pad_frames += (uint32_t)pad;
    p->timeline_reanchors++;
    p->reanchor_shifted_frames += pad;
    LOG_WARN("[AP2] Splice-padded after %s: shifted_frames="
             "%" PRIu64 " lead_frames=%" PRIu64 " count=%" PRIu64,
             cause, pad, recovery_lead, p->timeline_reanchors);
    fprintf(stderr, "[STATUS] REANCHOR shifted_frames=%" PRIu64
            " total_shifted_frames=%" PRIu64 " sample_rate=%d\n",
            pad, p->reanchor_shifted_frames, p->format.sample_rate);
    fflush(stderr);
    return true;
}

bool ap2cl_recover_input_gap(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->state != AP2_STREAMING)
        return false;

    uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
    uint64_t floor = MS2TS(250, p->format.sample_rate);
    uint64_t window = ap2_pacing_window_frames(p);
    uint64_t latency = MS2TS(p->latency_ms, p->format.sample_rate);
    uint64_t recovery_lead = latency < window ? latency : window;

    if (p->splice_timeline) {
        /* Input is dry, so the recovery is anticipatory: pad as soon as the
         * head falls inside the minimum lead — with nothing left to send the
         * lapse is otherwise inevitable. */
        return ap2_splice_pad_to_lead(p, now_ts, now_ts + floor,
                                      "PCM starvation");
    }

    ap2_timeline_recovery_t recovery;
    if (!ap2_timeline_plan_recovery(
            p->head_ts, p->rtp_timestamp, atomic_load(&p->rtp_offset),
            now_ts, floor, recovery_lead, &recovery)) {
        if (p->head_ts <= now_ts + floor &&
            (uint32_t)p->head_ts + atomic_load(&p->rtp_offset) !=
                p->rtp_timestamp)
            LOG_ERROR("[AP2] Cannot re-anchor: RTP/head invariant already broken");
        return false;
    }

    p->head_ts = recovery.head;
    atomic_store(&p->rtp_offset, recovery.offset);
    if (p->use_ptp && p->rt_anchor_valid) {
        p->rt_anchor_wall0 += ap2_timeline_frames_to_ns(
            recovery.shifted_frames, (uint32_t)p->format.sample_rate);
    }
    p->timeline_reanchors++;
    p->reanchor_shifted_frames += recovery.shifted_frames;
    ap2_send_result_t sync_result =
        p->use_ptp ? ap2_send_sync_packet_ptp(p, true)
                   : ap2_send_sync_packet(p, true);
    if (sync_result == AP2_SEND_FATAL) return false;
    LOG_WARN("[AP2] Re-anchored after PCM starvation: shifted_frames=%" PRIu64
             " lead_frames=%" PRIu64 " count=%" PRIu64,
             recovery.shifted_frames, recovery_lead, p->timeline_reanchors);
    /* Machine-readable counterpart of the LOG_WARN above, on the same stderr
     * channel as the binary's other [STATUS] lines, so Music Assistant can
     * track re-anchor drift without scraping the human-readable log. */
    fprintf(stderr, "[STATUS] REANCHOR shifted_frames=%" PRIu64
            " total_shifted_frames=%" PRIu64 " sample_rate=%d\n",
            recovery.shifted_frames, p->reanchor_shifted_frames,
            p->format.sample_rate);
    fflush(stderr);
    return true;
}

bool ap2cl_recover_delivery_gap(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->state != AP2_STREAMING ||
        !p->splice_timeline)
        return false;

    /* DELIVERY stalled (process freeze, network dropout) longer than the
     * shallow pacing depth: the head lapsed behind the wall clock while the
     * input ring still holds audio, so the starvation recovery above never
     * sees it — and without this guard the audio loop bursts the queued
     * content on past timestamps until the head catches back up (late-frame
     * delivery, an audible noise trigger on Apple receivers; log signature
     * "TX PTP sync ... ahead=-N"). Pad the timeline forward instead, exactly
     * like the starvation recovery.
     *
     * Unlike that recovery this trigger is strictly "behind now", not the
     * anticipatory minimum-lead floor: content is still queued here, and a
     * stall shorter than the pacing depth leaves the head between now and
     * the depth — the receiver's queue never ran dry and the pending frames
     * are on time, so padding would insert an audible silence gap into
     * otherwise unbroken audio. It also keeps this guard, which runs every
     * send iteration, quiet in the moments after a start resolved to the
     * feasibility floor (head sits just under now + floor by construction). */
    uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
    return ap2_splice_pad_to_lead(p, now_ts, now_ts, "delivery stall");
}

/* Move a committed anchor line that has not carried any audio yet onto a
 * later instant. With a distant anchor the pacing gate holds every frame
 * until anchor - depth, so the receiver has only seen announces of the old
 * line and re-seats cleanly on the fresh one (the session-start shape); the
 * content mapping moves with the line, keeping the first pending sample on
 * the commanded contract. */
static void ap2_rebase_pending_anchor(struct ap2cl_s *p, uint64_t at_unix_ms)
{
    uint64_t ntp = ap2_unix_ms_to_ntp(at_unix_ms);
    p->start_ntp = ntp;
    p->head_ts = NTP2TS(ntp, p->format.sample_rate);
    p->rtp_timestamp = (uint32_t)p->head_ts + atomic_load(&p->rtp_offset);
    p->rt_anchor_valid = false;
    p->first_packet = true;
    if (p->ctrl_sock >= 0 &&
        ap2_send_sync_packet_ptp(p, true) == AP2_SEND_FATAL)
        LOG_WARN("[AP2] corrected-anchor announce failed; the next send "
                 "retries it");
}

ap2_clock_verify_result_t ap2cl_clock_verify_poll(struct ap2cl_s *p,
                                                  ap2_clock_verify_event_t *ev)
{
    if (!p || !ev) return AP2_CLOCK_VERIFY_IDLE;
    memset(ev, 0, sizeof(*ev));
    pthread_mutex_lock(&p->clock_verify_lock);
    if (!p->clock_verify_armed) {
        pthread_mutex_unlock(&p->clock_verify_lock);
        return AP2_CLOCK_VERIFY_IDLE;
    }
    uint64_t now_ntp = raopcl_get_ntp(NULL);
    uint64_t now_ms = ap2_ntp_to_unix_ms(now_ntp);
    uint64_t anchor_ms = p->clock_verify_anchor_unix_ms;
    bool sent = p->audio_packets_sent != p->clock_verify_packets_at_arm;

    /* Probe-streak snapshot: the poller keeps it fresh in shared-daemon
     * mode (ages go stale by at most one poll round, which only pushes the
     * computed readiness later — the safe direction); the in-process engine
     * is queried directly (its own lock only, never the network). */
    struct ap2_ptp_exchange ex;
    bool have = false;
    if (p->clock_verify_have_exchange) {
        have = true;
        ex = p->clock_verify_exchange;
    } else if (!p->clock_verify_thread_started &&
               !ap2_ptp_shared_active(p->ptp) &&
               now_ntp >= p->clock_verify_next_poll_ntp) {
        p->clock_verify_next_poll_ntp =
            now_ntp + MS2NTP(AP2_CLOCK_VERIFY_POLL_MS);
        have = ap2_clock_query(p, &ex);
    }

    ap2_clock_verify_result_t result = AP2_CLOCK_VERIFY_IDLE;
    /* Read once for the whole poll. A withheld ack and a content cut are two
     * ways to answer the same correction and are mutually exclusive BY
     * CONSTRUCTION here: this single flag either sends the correction out as
     * the ack (the caller maps its content onto the acked instant itself) or
     * takes the cut (the caller already mapped to the old instant). Doing both
     * would advance the content twice and put the joiner early by exactly the
     * cut. */
    bool ack_deferred = p->start_ack_deferred;
    if (have) {
        uint64_t ready_ms = ap2_clock_ready_from(p, &ex, now_ms);
        ev->requested_unix_ms = p->clock_verify_requested_unix_ms;
        ev->from_unix_ms = anchor_ms;
        if (ready_ms <= anchor_ms) {
            ev->at_unix_ms = anchor_ms;
            ev->margin_ms = (int64_t)(anchor_ms - ready_ms);
            result = AP2_CLOCK_VERIFY_VERIFIED;
            LOG_INFO("[AP2] clock verified: probe streak began %" PRIu64
                     " ms before the anchor, readiness margin %" PRId64 " ms",
                     ex.first_ms, ev->margin_ms);
        } else if (!p->clock_verify_enforce) {
            /* Origin start: the shortfall is reported for lead tuning but
             * the anchor stands — the receiver self-seats a fresh session
             * (measured within ~20 ms), while moving one member of a group
             * origin would desync it. */
            ev->at_unix_ms = anchor_ms;
            result = AP2_CLOCK_VERIFY_UNVERIFIED;
            LOG_WARN("[AP2] receiver clock ready %" PRIu64 " ms after the "
                     "origin anchor; anchor stands (observe only)",
                     ready_ms - anchor_ms);
        } else if (!sent) {
            /* Readiness itself, with no slack on top: unlike the forward
             * corrections on the commit paths, this one asks nothing of the
             * caller — it re-bases the reported position and commits nothing
             * back — so there is no ack round-trip to stay ahead of, and every
             * millisecond of lead here is content the join has to cut. */
            ev->at_unix_ms = ready_ms;
            ap2_rebase_pending_anchor(p, ev->at_unix_ms);
            if (!ack_deferred) {
                /* Advance the queued content past the correction: the ack has
                 * already gone out on the original instant, so the caller
                 * mapped its content there and without the cut the member
                 * would render exactly that far behind the group for the whole
                 * session. Every frame is still unsent (the pacing gate holds
                 * them until anchor minus the window), and the carrier frame
                 * size mirrors the audio loop's input format. */
                ev->content_cut_ms = ev->at_unix_ms - anchor_ms;
                uint64_t cut_frames =
                    ev->content_cut_ms * p->format.sample_rate / 1000ULL;
                int input_bpf =
                    (p->format.bit_depth <= 16 ? 2 : 4) * p->format.channels;
                p->content_skip_bytes =
                    (uint32_t)(cut_frames * (uint64_t)input_bpf);
            }
            result = AP2_CLOCK_VERIFY_CORRECTED;
            LOG_WARN("[AP2] anchor %" PRIu64 " ms precedes receiver clock "
                     "readiness by %" PRIu64 " ms; corrected forward to %"
                     PRIu64 " and %s to keep the join on the group timeline",
                     anchor_ms, ready_ms - anchor_ms, ev->at_unix_ms,
                     ack_deferred ? "acked there, the caller mapping its "
                                    "content onto it"
                                  : "content advanced by the same amount");
        } else {
            /* Readiness resolved late, after real frames went out: the line
             * cannot move without a stamp jump. The receiver will seat
             * roughly this late; the caller's session-level resync is the
             * remaining remedy. */
            ev->at_unix_ms = anchor_ms;
            result = AP2_CLOCK_VERIFY_UNVERIFIED;
            LOG_WARN("[AP2] receiver clock ready %" PRIu64 " ms after the "
                     "anchor with audio already on the wire; expect a late "
                     "seat", ready_ms - anchor_ms);
        }
    } else if (sent || now_ms + AP2_CLOCK_VERIFY_POLL_MS >=
                           anchor_ms - AP2_SPLICE_PACING_MS) {
        ev->requested_unix_ms = p->clock_verify_requested_unix_ms;
        ev->from_unix_ms = anchor_ms;
        ev->at_unix_ms = anchor_ms;
        result = AP2_CLOCK_VERIFY_UNVERIFIED;
        LOG_WARN("[AP2] anchor window closed without a receiver clock probe; "
                 "anchor stands unverified");
    }
    if (result != AP2_CLOCK_VERIFY_IDLE) {
        p->clock_verify_armed = false;
        /* Every outcome answers the withheld ack — including the two that
         * leave the anchor standing — and consuming the flag here, at the one
         * point a result is produced, is what makes it exactly once. */
        ev->start_ack = ack_deferred;
        p->start_ack_deferred = false;
    }
    pthread_mutex_unlock(&p->clock_verify_lock);
    if (result != AP2_CLOCK_VERIFY_IDLE &&
        !atomic_load(&p->clock_poll_wanted))
        atomic_store(&p->clock_verify_thread_stop, true);
    return result;
}

bool ap2cl_uses_ptp(struct ap2cl_s *p)
{
    return p && p->flow == FLOW_NATIVE_AP2 && p->use_ptp;
}

void ap2cl_clock_readiness(struct ap2cl_s *p, ap2_clock_readiness_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* Without the PTP engine there is no probe streak to measure: NTP-timed
     * sessions (and the RAOP-compat flow) report cold and stay there. */
    if (!ap2cl_uses_ptp(p)) return;

    struct ap2_ptp_exchange ex;
    bool have = false;
    pthread_mutex_lock(&p->clock_verify_lock);
    if (p->clock_verify_have_exchange) {
        have = true;
        ex = p->clock_verify_exchange;
    }
    /* The stall window runs from the last streak actually seen, and only from
     * connect while none ever has been: a snapshot goes empty on a single lost
     * Q round-trip as readily as on a receiver that stopped answering, so
     * measuring from connect alone would call a healthy session stalled on one
     * dropped datagram. The poller refreshes the mark every round, so a blip
     * can never reach the window while a real loss gets the same grace as a
     * cold start. */
    uint64_t stall_from_ms =
        p->clock_last_streak_unix_ms > p->clock_connected_unix_ms
            ? p->clock_last_streak_unix_ms : p->clock_connected_unix_ms;
    pthread_mutex_unlock(&p->clock_verify_lock);
    /* The in-process engine answers from its own lock, so the audio loop may
     * ask it directly; shared-daemon mode must take the poller's snapshot,
     * whose staleness only ever pushes readiness later — the safe direction. */
    if (!have && !ap2_ptp_shared_active(p->ptp))
        have = ap2_clock_query(p, &ex);

    uint64_t now_ms = ap2_ntp_to_unix_ms(raopcl_get_ntp(NULL));
    if (!have) {
        /* Nothing to measure this long after the receiver was last heard from
         * means it is not slaved to our clock: it can seat no render position
         * and renders silence however cleanly the audio paces. Reported as its
         * own state because a cold receiver looks the same on the way there.
         * Only a live session can be judged silent: the marks outlive teardown,
         * so a reading taken once the session is down measures nothing. */
        if (p->state != AP2_DOWN && stall_from_ms &&
            now_ms >= stall_from_ms + AP2_CLOCK_STALL_MS)
            out->state = AP2_CLOCK_STALLED;
        return;
    }
    /* Carry the mark for the in-process engine, which runs no poller to keep
     * it current; in shared-daemon mode the poller has it already. */
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_last_streak_unix_ms = now_ms;
    pthread_mutex_unlock(&p->clock_verify_lock);

    uint64_t ready_ms = ap2_clock_ready_from(p, &ex, now_ms);
    out->streak_ms = ex.first_ms;
    out->exchanges = ex.count;
    out->ready_at_unix_ms = ready_ms;
    out->ready_in_ms = ready_ms > now_ms ? ready_ms - now_ms : 0;
    out->state = ready_ms > now_ms ? AP2_CLOCK_PROBING : AP2_CLOCK_READY;
}

bool ap2cl_clock_verify_armed(struct ap2cl_s *p)
{
    if (!p) return false;
    pthread_mutex_lock(&p->clock_verify_lock);
    bool armed = p->clock_verify_armed;
    pthread_mutex_unlock(&p->clock_verify_lock);
    return armed;
}

bool ap2cl_start_ack_deferred(struct ap2cl_s *p)
{
    if (!p) return false;
    pthread_mutex_lock(&p->clock_verify_lock);
    bool deferred = p->start_ack_deferred;
    pthread_mutex_unlock(&p->clock_verify_lock);
    return deferred;
}

void ap2cl_log_diagnostics(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || *loglevel < lSDEBUG) return;
    uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
    int64_t pacing_ahead = p->head_ts >= now_ts
                               ? (int64_t)(p->head_ts - now_ts)
                               : -(int64_t)(now_ts - p->head_ts);
    LOG_SDEBUG("[AP2DIAG] state=%d seq=%u rtp=%u head=%" PRIu64
               " pacing_ahead_frames=%" PRId64 " audio_sent=%" PRIu64
               " audio_dropped=%" PRIu64 " sync_sent=%" PRIu64
               " sync_dropped=%" PRIu64 " reanchors=%" PRIu64,
               p->state, p->seq_number, p->rtp_timestamp, p->head_ts,
               pacing_ahead, p->audio_packets_sent, p->audio_packets_dropped,
               p->sync_packets_sent, p->sync_packets_dropped,
               p->timeline_reanchors);
}

void ap2cl_pause(struct ap2cl_s *p)
{
    if (!p) return;
    p->content_stopped = false;
    if (p->raopcl) { raopcl_pause(p->raopcl); raopcl_flush(p->raopcl); }
    if (p->splice_timeline) {
        /* Splice pause stops the CONTENT, never the wire: an Apple receiver
         * whose queue underruns while the session stays armed emits a noise
         * burst (measured at the pause press, ear A/B 2026-07-31 — the same
         * pop whether or not the pause is announced over MRP; a teardown
         * with audio still queued is clean). The client therefore stays in
         * AP2_STREAMING and the audio loop keeps the line fed with encoded
         * silence; the un-pause splices the content back in on the hot line
         * and the immutable anchor survives by construction. content_paused
         * keeps the published now-playing state truthful meanwhile. */
        p->content_paused = true;
        ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_PAUSED, true);
        return;
    }
    /* Stock timeline: park the stream; resume re-anchors a fresh line. */
    p->rt_anchor_valid = false;
    p->state = AP2_PAUSED;
    ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_PAUSED, true);
}

void ap2cl_play(struct ap2cl_s *p)
{
    if (!p) return;
    p->content_paused = false;
    p->content_stopped = false;
    if (p->raopcl) {
        int lat = raopcl_latency(p->raopcl);
        uint64_t now = raopcl_get_ntp(NULL);
        raopcl_start_at(p->raopcl, now + MS2NTP(200) - TS2NTP(lat, raopcl_sample_rate(p->raopcl)));
    }
    if (p->flow == FLOW_NATIVE_AP2 && p->splice_timeline &&
        p->rt_anchor_valid) {
        uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
        uint64_t target =
            now_ts + MS2TS(AP2_MIN_WARM_LEAD_MS, p->format.sample_rate);
        if (p->head_ts > now_ts) {
            /* Un-pause on the hot line: fill the paused span with silence so
             * the wire stays bitstream-continuous and the remaining content
             * resumes at now plus the minimum lead — a stamp jump or late
             * frames are each an audible noise trigger. */
            if (target > p->head_ts) {
                p->splice_pad_frames = (uint32_t)(target - p->head_ts);
                LOG_INFO("[AP2] splice un-pause: padding %u silence frames",
                         (unsigned)p->splice_pad_frames);
            }
        } else {
            /* Lapsed line (receiver drained long ago): re-anchor fresh over
             * the idle pipeline, the clean session-start shape. */
            p->splice_pad_frames = 0;
            p->start_ntp = raopcl_get_ntp(NULL) + MS2NTP(AP2_MIN_WARM_LEAD_MS);
            p->head_ts = NTP2TS(p->start_ntp, p->format.sample_rate);
            p->rtp_timestamp = (uint32_t)p->head_ts +
                               atomic_load(&p->rtp_offset);
            p->rt_anchor_valid = false;
            p->first_packet = true;
            LOG_INFO("[AP2] splice un-pause after drain: fresh anchor line");
        }
    }
    p->state = AP2_STREAMING;
    ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_PLAYING, true);
}

void ap2cl_stop(struct ap2cl_s *p)
{
    if (!p) return;
    ap2_clock_verify_disarm(p);
    p->content_paused = false;
    p->content_stopped = false;
    ap2_content_skip_reset(p, "stop");
    if (p->raopcl) raopcl_stop(p->raopcl);
    ap2_mrp_publish_playback(p, AP2_MRP_PLAYBACK_STOPPED, true);
    p->rt_anchor_valid = false;
    p->state = AP2_DOWN;
}

/*
 * Read the receiver's own view of the session out of a /feedback response:
 * {"streams": [...]} lists the streams it still holds for us. An empty list
 * while we believe we are streaming means the receiver dropped the stream
 * under us and every packet we send is going nowhere — the POST answers 200
 * either way, so nothing else on this channel reports it. Receivers that omit
 * the key report no stream status at all, which is never a fault: only a
 * present-but-empty list counts.
 *
 * The list is what the receiver HOLDS, not what it renders: measured
 * 2026-08-02, a Sonos Era 100 confirmed playing (its own UPnP transport
 * reported PLAYING) and a Samsung HW-LS60D both answered every tick with the
 * identical body {"streams": [{"type": 96, "sr": 44100}]}. So a live entry
 * does not prove audio is audible, and this is not a silence detector.
 */
static void ap2_feedback_note_streams(struct ap2cl_s *p, const uint8_t *resp,
                                      int resp_len)
{
    size_t streams = 0;
    if (!resp || resp_len <= 0 ||
        !ap2_bplist_find_array_count(resp, (size_t)resp_len, "streams",
                                     &streams)) {
        p->feedback_idle_streams = 0;
        return;
    }

    LOG_DEBUG("[AP2] /feedback streams=%zu", streams);
    if (streams > 0 || p->state != AP2_STREAMING) {
        /* Only a stream coming back closes a reported episode; leaving the
         * streaming state just drops the streak, since an idle receiver
         * holding nothing is the expected shape there. */
        if (streams > 0 &&
            p->feedback_idle_streams >= AP2_FEEDBACK_IDLE_STREAM_TICKS)
            LOG_INFO("[AP2] receiver holds a stream again (streams=%zu)",
                     streams);
        p->feedback_idle_streams = 0;
        return;
    }
    /* Report the fault once per episode; the recovery above closes it. */
    if (++p->feedback_idle_streams != AP2_FEEDBACK_IDLE_STREAM_TICKS) return;
    LOG_WARN("[AP2] Receiver holds no stream after %u feedback ticks while "
             "streaming: it dropped the stream we are still sending to",
             p->feedback_idle_streams);
    /* Machine-readable counterpart on the same stderr channel as the binary's
     * other [STATUS] lines, so Music Assistant can act on a dropped stream
     * without scraping the human-readable log. */
    fprintf(stderr, "[STATUS] stream_dropped streams=0 ticks=%u interval_ms=%d\n",
            p->feedback_idle_streams, AP2_FEEDBACK_INTERVAL_MS);
    fflush(stderr);
}

bool ap2cl_feedback(struct ap2cl_s *p)
{
    /* Native flow only: the RAOP-compat flow rides libraop, whose keepalive
     * is raopcl_keepalive(). The POST itself is what resets the receiver's
     * idle timer; its body carries the receiver's active-stream list. */
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return false;
    uint8_t *resp = NULL; int resp_len = 0;
    bool request_started = false;
    int status = ap2_rtsp_send_tracked(
        p, "POST", "/feedback", NULL, 0, NULL,
        &resp, &resp_len, &request_started);
    ap2_feedback_note_streams(p, resp, resp_len);
    free(resp);
    ap2_feedback_result_t result =
        ap2_io_feedback_result(status, request_started);
    if (result == AP2_FEEDBACK_SKIPPED) {
        LOG_SDEBUG("[AP2] /feedback tick skipped: RTSP control lock budget "
                   "expired before request start");
    } else {
        LOG_DEBUG("[AP2] /feedback keepalive -> %d", status);
    }
    /* Service event/DataStream input after the receiver keepalive itself.
     * State output takes the publication lock only when immediately available,
     * so metadata work can never delay the next feedback cadence. */
    if (!atomic_load(&p->rtsp_dead)) {
        pthread_mutex_lock(&p->mrp_lock);
        struct ap2_mrp_ctx *mrp = p->mrp;
        pthread_mutex_unlock(&p->mrp_lock);

        if (mrp) {
            ap2_mrp_tick(mrp);
            pthread_mutex_lock(&p->mrp_lock);
            if (p->mrp == mrp) {
                atomic_store(&p->mrp_event_health,
                             ap2_mrp_event_status(mrp));
            }
            pthread_mutex_unlock(&p->mrp_lock);

            if (pthread_mutex_trylock(&p->mrp_publish_lock) == 0) {
                uint8_t *state = NULL;
                int state_len = 0;
                uint64_t state_generation = 0;
                pthread_mutex_lock(&p->mrp_lock);
                bool prepared =
                    p->mrp == mrp &&
                    ap2_mrp_prepare_state_push(
                        mrp, &state, &state_len, &state_generation);
                pthread_mutex_unlock(&p->mrp_lock);
                if (prepared) {
                    bool state_ok =
                        ap2_mrp_send_state_push(mrp, state, state_len);
                    pthread_mutex_lock(&p->mrp_lock);
                    if (p->mrp == mrp)
                        ap2_mrp_complete_state_push(
                            mrp, state_generation, state_ok);
                    pthread_mutex_unlock(&p->mrp_lock);
                }
                free(state);
                pthread_mutex_unlock(&p->mrp_publish_lock);
            } else {
                LOG_SDEBUG("[MRP] state push deferred behind publication");
            }
        }
    }
    if (result == AP2_FEEDBACK_SUCCEEDED) {
        unsigned prior_misses = atomic_exchange(&p->feedback_failures, 0);
        if (prior_misses)
            LOG_INFO("[AP2] /feedback keepalive recovered after %u missed "
                     "beat(s)", prior_misses);
    } else if (result == AP2_FEEDBACK_FAILED) {
        atomic_fetch_add(&p->feedback_failures, 1);
    }
    return result == AP2_FEEDBACK_SUCCEEDED;
}

bool ap2cl_control_healthy(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2) return true;
    if (atomic_load(&p->rtsp_dead) ||
        !atomic_load(&p->media_healthy) ||
        atomic_load(&p->feedback_failures) >= AP2_FEEDBACK_MAX_CONSECUTIVE_MISSES)
        return false;
    return atomic_load(&p->mrp_event_health) != 0;
}

bool ap2cl_set_volume(struct ap2cl_s *p, int volume)
{
    if (!p) return false;
    p->volume = volume;
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0) {
        /* Same percent->dB mapping as the RAOP path (libraop): linear in dB
         * over -30..0, mute at 0. This is the AirPlay convention (iOS,
         * shairport-sync use the same range), so a given slider position is
         * equally loud on every protocol path and matches other senders. */
        float vol_db = volume <= 0 ? -144.0f
                       : raopcl_float_volume(volume > 100 ? 100 : volume);
        char body[32];
        int blen = snprintf(body, sizeof(body), "volume: %.6f\r\n", vol_db);
        uint8_t *resp = NULL; int resp_len = 0;
        int status = ap2_rtsp_send(p, "SET_PARAMETER", p->session_url,
                       (uint8_t *)body, blen, "text/parameters", &resp, &resp_len);
        LOG_INFO("[AP2] native volume -> %.2f dB (SET_PARAMETER status %d)", vol_db, status);
        free(resp);
        return true;
    }
    if (p->raopcl) return raopcl_set_volume(p->raopcl, raopcl_float_volume(volume));
    return false;
}

/* Build a DMAP "mlit" metadata blob (libraop's rtspcl_set_daap wire format) and
 * push it over the native encrypted RTSP channel. Sonos withholds audio until it
 * receives track metadata, so the native flow must send this to become audible
 * (the RAOP-compat flow gets it for free via raopcl). */
static bool ap2_native_send_metadata(struct ap2cl_s *p, const char *title,
                                     const char *artist, const char *album)
{
    if (!p || p->sock_fd < 0) return false;
    if (!title)  title  = "";
    if (!artist) artist = "";
    if (!album)  album  = "";

    size_t need = 8 + 9 + (8 + strlen(title)) + (8 + strlen(artist))
                + (8 + strlen(album)) + 10;
    uint8_t *buf = malloc(need);
    if (!buf) return false;
    uint8_t *q = buf;

    memcpy(q, "mlit", 4); q += 8;                        /* size backfilled below */
    memcpy(q, "mikd", 4); q += 4;
    *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 1; *q++ = 2;    /* mikd: len 1, value 2 */

    const char *tags[3] = { "minm", "asar", "asal" };
    const char *vals[3] = { title, artist, album };
    for (int i = 0; i < 3; i++) {
        uint32_t n = (uint32_t)strlen(vals[i]);
        memcpy(q, tags[i], 4); q += 4;
        for (int b = 0; b < 4; b++) *q++ = (n >> (24 - 8 * b)) & 0xff;
        memcpy(q, vals[i], n); q += n;
    }
    memcpy(q, "astn", 4); q += 4;                        /* track number = 1 (int16) */
    *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 2; *q++ = 0; *q++ = 1;

    uint32_t sz = (uint32_t)(q - buf - 8);               /* mlit payload size */
    for (int b = 0; b < 4; b++) buf[4 + b] = (sz >> (24 - 8 * b)) & 0xff;

    /* Anchor the metadata to the current RTP position; the RAOP-compat path
     * sends this via RTP-Info and Sonos 400s a metadata request without it. */
    char rtpinfo[48];
    snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: rtptime=%u\r\n", p->rtp_timestamp);

    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send_ex(p, "SET_PARAMETER", p->session_url,
                            buf, (int)(q - buf), "application/x-dmap-tagged",
                            rtpinfo, &resp, &resp_len);
    LOG_INFO("[AP2] native metadata SET_PARAMETER -> status %d (%d bytes)",
             status, (int)(q - buf));
    free(resp);
    free(buf);
    return status >= 200 && status < 300;
}

/* Playback state to publish: the splice keepalive keeps the client
 * AP2_STREAMING through content pauses and standby parks, so the published
 * state follows the content flags, not the wire. */
static ap2_mrp_playback_state_t ap2_mrp_current_playback_state(
    struct ap2cl_s *p)
{
    if (p->content_stopped) return AP2_MRP_PLAYBACK_STOPPED;
    if (p->state == AP2_PAUSED || p->content_paused)
        return AP2_MRP_PLAYBACK_PAUSED;
    return p->state == AP2_STREAMING ? AP2_MRP_PLAYBACK_PLAYING
                                     : AP2_MRP_PLAYBACK_STOPPED;
}

/* Lazily create MediaRemote for pair-verified native sessions and attach the
 * encrypted reverse event channel before advertising any controllable state.
 * Transient-paired third-party speakers never receive these Apple-specific
 * messages. Set CLIAIRPLAY_MRP=0 to disable the path for diagnosis. */
static void ap2_mrp_ready(struct ap2cl_s *p)
{
    if (p->mrp || p->flow != FLOW_NATIVE_AP2 || !p->auth_credentials ||
        p->sock_fd < 0 || p->events_sock < 0 || !p->session_uuid[0])
        return;
    const char *setting = getenv("CLIAIRPLAY_MRP");
    if (setting && (!strcmp(setting, "0") || !strcmp(setting, "false") ||
                    !strcmp(setting, "off")))
        return;
    p->mrp = ap2_mrp_create(p->device.address, p->device.port, p->auth_credentials,
                            p->dacp_id, p->device.name,
                            p->session_uuid, p->group_uuid,
                            ap2_hap_get_shared_secret(p->hap));
    if (!p->mrp) return;
    ap2_mrp_set_remote_command_callback(
        p->mrp, ap2_remote_command_received, p);
    if (!ap2_mrp_attach_events(p->mrp, p->events_sock)) {
        LOG_WARN("[MRP] event-channel attach failed; MediaRemote disabled");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        atomic_store(&p->mrp_event_health, -1);
        return;
    }
    p->events_sock = -1;
    ap2_mrp_playback_state_t state = ap2_mrp_current_playback_state(p);
    if (state == AP2_MRP_PLAYBACK_STOPPED)
        ap2_mrp_set_stopped(p->mrp);
    else
        ap2_mrp_set_playing(p->mrp, state == AP2_MRP_PLAYBACK_PLAYING);
    atomic_store(&p->mrp_event_health, ap2_mrp_event_status(p->mrp));
}

/* Log an RTSP response body for diagnosis: a printable-text view (control bytes
 * escaped) plus a short hex prefix, and whether it is a binary plist. */
static void ap2_log_response_body(const char *tag, const uint8_t *body, int len)
{
    if (!body || len <= 0) { LOG_INFO("%s response body: <empty>", tag); return; }
    int shown = len < 256 ? len : 256;
    char text[300];
    int t = 0;
    for (int i = 0; i < shown && t < (int)sizeof(text) - 1; i++) {
        uint8_t c = body[i];
        text[t++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    text[t] = '\0';
    char hex[8 * 3 + 1];
    int hn = len < 8 ? len : 8;
    for (int i = 0; i < hn; i++) sprintf(hex + i * 3, "%02x ", body[i]);
    hex[hn * 3 ? hn * 3 - 1 : 0] = '\0';
    bool is_bplist = len >= 8 && memcmp(body, "bplist00", 8) == 0;
    LOG_INFO("%s response body: %d bytes%s [%s] text=\"%s\"",
             tag, len, is_bplist ? " (bplist00)" : "", hex, text);
}

typedef bool (*ap2_mrp_command_builder_t)(struct ap2_mrp_ctx *,
                                          uint8_t **, int *);

static int ap2_mrp_post_command(struct ap2cl_s *p,
                                ap2_mrp_command_builder_t builder,
                                const char *tag)
{
    uint8_t *body = NULL; int body_len = 0;
    pthread_mutex_lock(&p->mrp_lock);
    bool built = p->mrp && builder(p->mrp, &body, &body_len);
    pthread_mutex_unlock(&p->mrp_lock);
    if (!built) return -1;
    uint8_t *resp = NULL;
    int resp_len = 0;
    int status = ap2_rtsp_send(p, "POST", "/command", body, body_len,
                               "application/x-apple-binary-plist", &resp, &resp_len);
    free(body);
    LOG_INFO("[MRP] /command %s -> %d (%d-byte body)", tag, status, body_len);
    if (status < 200 || status >= 300)
        ap2_log_response_body(tag, resp, resp_len);
    free(resp);
    return status;
}

static bool ap2_mrp_status_ok(int status)
{
    return status >= 200 && status < 300;
}

static int ap2_mrp_send_playback_state(struct ap2cl_s *p,
                                       ap2_mrp_playback_state_t state,
                                       bool force)
{
    if (!p || !p->mrp || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0)
        return -1;
    if (!force && p->mrp_last_playback_state == state) return 200;

    int status = ap2_mrp_post_command(
        p, ap2_mrp_build_playbackstate_command,
        "[MRP] /command updateMRPlaybackState");
    if (ap2_mrp_status_ok(status))
        p->mrp_last_playback_state = state;
    return status;
}

static void ap2_mrp_publish_playback(struct ap2cl_s *p,
                                     ap2_mrp_playback_state_t state,
                                     bool force)
{
    if (!p || p->flow != FLOW_NATIVE_AP2) return;
    pthread_mutex_lock(&p->mrp_publish_lock);
    pthread_mutex_lock(&p->mrp_lock);
    if (p->mrp) {
        if (state == AP2_MRP_PLAYBACK_STOPPED)
            ap2_mrp_set_stopped(p->mrp);
        else
            ap2_mrp_set_playing(
                p->mrp, state == AP2_MRP_PLAYBACK_PLAYING);
    }
    pthread_mutex_unlock(&p->mrp_lock);
    ap2_mrp_send_playback_state(p, state, force);
    pthread_mutex_unlock(&p->mrp_publish_lock);
}

/* Apple AirPlaySender sends the extended metadata immediately after the first
 * updateMRNowPlayingInfo: supported commands, explicit playback state, then the
 * serialized NowPlayingClient. A receiver can accept the info plist while
 * keeping it detached from its system now-playing player when these are absent. */
static int ap2_mrp_send_extended_registration(struct ap2cl_s *p)
{
    ap2_mrp_playback_state_t state = ap2_mrp_current_playback_state(p);

    int st_cmd = ap2_mrp_post_command(
        p, ap2_mrp_build_supportedcommands_command,
        "[MRP] /command updateMRSupportedCommands");
    int st_state = ap2_mrp_send_playback_state(p, state, true);
    int st_client = ap2_mrp_post_command(
        p, ap2_mrp_build_nowplayingclient_command,
        "[MRP] /command updateMRNowPlayingClient");

    p->mrp_extended_registered =
        ap2_mrp_status_ok(st_cmd) && ap2_mrp_status_ok(st_state) &&
        ap2_mrp_status_ok(st_client);
    LOG_INFO("[MRP] extended metadata: commands=%d playback=%d client=%d",
             st_cmd, st_state, st_client);

    if (!ap2_mrp_status_ok(st_cmd)) return st_cmd;
    if (!ap2_mrp_status_ok(st_state)) return st_state;
    return st_client;
}

static int ap2cl_mrp_register_serialized(struct ap2cl_s *p);

static ap2_mrp_push_result_t ap2cl_mrp_push_serialized_with(
    struct ap2cl_s *p, ap2_mrp_command_builder_t builder)
{
    ap2_mrp_push_result_t result = ap2_mrp_push_result_empty();
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return result;
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    bool have_mrp = p->mrp != NULL;
    pthread_mutex_unlock(&p->mrp_lock);
    if (!have_mrp) return result;

    if (!p->mrp_device_registered && ap2cl_mrp_register_serialized(p) != 1) {
        result.overall_status = 0;
        return result;
    }

    int status = ap2_mrp_post_command(
        p, builder, "[MRP] /command updateMRNowPlayingInfo");
    result.nowplaying_status = status;
    result.overall_status = status;
    if (!ap2_mrp_status_ok(status)) return result;

    int ext_status;
    if (!p->mrp_extended_registered) {
        ext_status = ap2_mrp_send_extended_registration(p);
    } else {
        ext_status = ap2_mrp_send_playback_state(
            p, ap2_mrp_current_playback_state(p), false);
    }
    if (!ap2_mrp_status_ok(ext_status)) result.overall_status = ext_status;
    return result;
}

static ap2_mrp_push_result_t ap2cl_mrp_push_serialized(struct ap2cl_s *p)
{
    return ap2cl_mrp_push_serialized_with(p, ap2_mrp_build_nowplaying_command);
}

ap2_mrp_push_result_t ap2cl_mrp_push_ex(struct ap2cl_s *p)
{
    ap2_mrp_push_result_t result = ap2_mrp_push_result_empty();
    if (!p) return result;
    pthread_mutex_lock(&p->mrp_publish_lock);
    result = ap2cl_mrp_push_serialized(p);
    pthread_mutex_unlock(&p->mrp_publish_lock);
    return result;
}

int ap2cl_mrp_push(struct ap2cl_s *p)
{
    return ap2cl_mrp_push_ex(p).overall_status;
}

int ap2cl_mrp_push_progress(struct ap2cl_s *p)
{
    ap2_mrp_push_result_t result = ap2_mrp_push_result_empty();
    if (!p) return result.overall_status;
    pthread_mutex_lock(&p->mrp_publish_lock);
    if (!p->mrp_progress_push_full) {
        result = ap2cl_mrp_push_serialized_with(
            p, ap2_mrp_build_nowplaying_progress_command);
        /* A receiver that rejects the "update" merge policy outright gets
         * the full replace shape from now on (bytes riding along); only an
         * HTTP-level rejection demotes — transport failures stay retryable
         * on the lean shape. */
        if (result.nowplaying_status >= 300) {
            LOG_WARN("[MRP] timeline update push rejected (%d); "
                     "using full now-playing pushes for this session",
                     result.nowplaying_status);
            p->mrp_progress_push_full = true;
        }
    }
    if (p->mrp_progress_push_full)
        result = ap2cl_mrp_push_serialized(p);
    pthread_mutex_unlock(&p->mrp_publish_lock);
    return result.overall_status;
}

/* MRP data-channel (path B) status for the [STATUS] mrp line:
 *   -1 = not attempted (non-Apple / not pair-verified),
 *    0 = attempted but the channel is not up,
 *    1 = channel established (now-playing active). */
int ap2cl_mrp_channel_status(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || !p->auth_credentials) return -1;
    /* The type-130 channel is opt-in (see ap2_native_connect §6d); when it is
     * not attempted, report "not applicable" so the [STATUS] path=channel line
     * is suppressed and only path=command (the active now-playing path) shows. */
    if (!getenv("CLIAIRPLAY_MRP_TYPE130")) return -1;
    pthread_mutex_lock(&p->mrp_lock);
    int status = (p->mrp && ap2_mrp_is_connected(p->mrp)) ? 1 : 0;
    pthread_mutex_unlock(&p->mrp_lock);
    return status;
}

/* Register the sender identity before the first now-playing update. The
 * remaining extended metadata follows updateMRNowPlayingInfo in
 * ap2cl_mrp_push(), matching Apple's current AirPlaySender implementation. */
static int ap2cl_mrp_register_serialized(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return -1;
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    bool have_mrp = p->mrp != NULL;
    pthread_mutex_unlock(&p->mrp_lock);
    if (!have_mrp) return -1;
    if (p->mrp_device_registered) return 1;

    int status = ap2_mrp_post_command(
        p, ap2_mrp_build_deviceinfo_command,
        "[MRP] /command DEVICE_INFO");
    p->mrp_device_registered = ap2_mrp_status_ok(status);
    return p->mrp_device_registered ? 1 : 0;
}

int ap2cl_mrp_register(struct ap2cl_s *p)
{
    if (!p) return -1;
    pthread_mutex_lock(&p->mrp_publish_lock);
    int status = ap2cl_mrp_register_serialized(p);
    pthread_mutex_unlock(&p->mrp_publish_lock);
    return status;
}

bool ap2cl_set_metadata(struct ap2cl_s *p, const char *title, const char *artist,
                        const char *album, int duration, const char *item_id)
{
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_publish_lock);
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    if (p->mrp)
        ap2_mrp_set_metadata(p->mrp, title, artist, album, duration * 1000,
                             item_id);
    pthread_mutex_unlock(&p->mrp_lock);
    pthread_mutex_unlock(&p->mrp_publish_lock);
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0)
        return ap2_native_send_metadata(p, title, artist, album);
    if (p->raopcl)
        return raopcl_set_daap(p->raopcl, 4, "minm", 's', title,
                                "asar", 's', artist, "asal", 's', album, "astn", 'i', 1);
    return false;
}

bool ap2cl_set_artwork(struct ap2cl_s *p, const char *content_type, int size,
                       const char *data, ap2_mrp_artwork_info_t *mrp_info,
                       ap2_mrp_push_result_t *mrp_push)
{
    ap2_mrp_artwork_info_t local_info;
    if (!mrp_info) mrp_info = &local_info;
    if (mrp_push) *mrp_push = ap2_mrp_push_result_empty();
    memset(mrp_info, 0, sizeof(*mrp_info));
    mrp_info->result = AP2_MRP_ARTWORK_NOT_APPLICABLE;
    mrp_info->bytes = size > 0 ? (size_t)size : 0;
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_publish_lock);
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    bool have_mrp = p->mrp != NULL;
    if (p->mrp)
        ap2_mrp_set_artwork(p->mrp, content_type, (const uint8_t *)data,
                            size, mrp_info);
    pthread_mutex_unlock(&p->mrp_lock);
    if (mrp_info->result == AP2_MRP_ARTWORK_UNCHANGED) {
        /* The receiver already holds these exact bytes under the current
         * identifier; re-sending on either path would only make it re-render
         * its Now Playing UI. */
        pthread_mutex_unlock(&p->mrp_publish_lock);
        return true;
    }
    bool dmap_ok = false;
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0) {
        /* Preserve the DMAP/SET_PARAMETER path for receivers such as Sonos;
         * pair-verified Apple sessions mirror validated artwork over MRP. */
        char rtpinfo[48];
        snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: rtptime=%u\r\n", p->rtp_timestamp);
        uint8_t *resp = NULL; int resp_len = 0;
        int status = ap2_rtsp_send_ex(p, "SET_PARAMETER", p->session_url,
                                      (const uint8_t *)data, size, content_type,
                                      rtpinfo, &resp, &resp_len);
        free(resp);
        LOG_INFO("[AP2] native artwork SET_PARAMETER -> status %d (%d bytes, %s)",
                 status, size, content_type);
        dmap_ok = status >= 200 && status < 300;
    } else if (p->raopcl) {
        dmap_ok = raopcl_set_artwork(
            p->raopcl, (char *)content_type, size, (char *)data);
    }
    if (have_mrp) {
        ap2_mrp_push_result_t result = ap2cl_mrp_push_serialized(p);
        if (mrp_push) *mrp_push = result;
    }
    pthread_mutex_unlock(&p->mrp_publish_lock);
    return dmap_ok;
}

bool ap2cl_clear_mrp_artwork(struct ap2cl_s *p,
                             ap2_mrp_push_result_t *mrp_push)
{
    if (mrp_push) *mrp_push = ap2_mrp_push_result_empty();
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_publish_lock);
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    if (!p->mrp) {
        pthread_mutex_unlock(&p->mrp_lock);
        pthread_mutex_unlock(&p->mrp_publish_lock);
        return false;
    }
    ap2_mrp_clear_artwork(p->mrp);
    pthread_mutex_unlock(&p->mrp_lock);
    ap2_mrp_push_result_t result = ap2cl_mrp_push_serialized(p);
    if (mrp_push) *mrp_push = result;
    pthread_mutex_unlock(&p->mrp_publish_lock);
    return true;
}

bool ap2cl_set_progress(struct ap2cl_s *p, int elapsed_s, int duration_s)
{
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_publish_lock);
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready(p);
    bool have_mrp = p->mrp != NULL;
    if (p->mrp)
        ap2_mrp_set_progress(p->mrp, elapsed_s * 1000, duration_s * 1000,
                             p->state == AP2_STREAMING);
    pthread_mutex_unlock(&p->mrp_lock);
    pthread_mutex_unlock(&p->mrp_publish_lock);
    if (have_mrp) {
        /* MediaRemote owns now-playing on this receiver; the timeline rides
         * the mergePolicy-"update" push that follows this call. The legacy
         * SET_PARAMETER progress line exists for receivers without the MRP
         * bridge (Sonos-class) — sending both makes an Apple TV process the
         * same seek twice, and its legacy path visibly re-renders the
         * now-playing screen. */
        return true;
    }
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0) {
        /* progress: <start>/<current>/<end>, all in the STREAM's RTP timestamp
         * units (the per-process timeline offset included, so the values match
         * the timestamps the receiver sees on the audio packets). */
        uint32_t now_w = (uint32_t)NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate)
                       + atomic_load(&p->rtp_offset);
        uint32_t start = now_w - (uint32_t)((uint64_t)elapsed_s * p->format.sample_rate);
        uint32_t end = duration_s
                       ? start + (uint32_t)((uint64_t)duration_s * p->format.sample_rate)
                       : now_w;
        char body[128];
        int blen = snprintf(body, sizeof(body), "progress: %u/%u/%u\r\n",
                            start, now_w, end);
        uint8_t *resp = NULL; int resp_len = 0;
        int status = ap2_rtsp_send(p, "SET_PARAMETER", p->session_url,
                                   (uint8_t *)body, blen, "text/parameters",
                                   &resp, &resp_len);
        free(resp);
        LOG_DEBUG("[AP2] native progress SET_PARAMETER -> status %d", status);
        return status >= 200 && status < 300;
    }
    if (!p->raopcl) return false;
    return raopcl_set_progress_ms(p->raopcl, elapsed_s * 1000, duration_s * 1000);
}

void ap2cl_format_capabilities(struct ap2cl_s *p,
                               ap2_format_capabilities_t *caps)
{
    if (!caps) return;
    memset(caps, 0, sizeof(*caps));
    if (!p) return;
    caps->requested = p->audio_format;
    caps->realtime_formats = p->realtime_formats;
    caps->buffered_formats = p->buffered_formats;
    caps->realtime_known = p->realtime_formats_known;
    caps->buffered_known = p->buffered_formats_known;
}

void ap2cl_latency_info(struct ap2cl_s *p, int *lead_ms, uint32_t *dev_min, uint32_t *dev_max)
{
    if (lead_ms) *lead_ms = p ? p->latency_ms : 0;
    if (dev_min) *dev_min = p ? p->dev_latency_min : 0;
    if (dev_max) *dev_max = p ? p->dev_latency_max : 0;
}

/* Frames between the delivery head and the audible position, for elapsed
 * reporting. On the splice timeline delivery runs one shallow pacing window
 * ahead of audibility; the stock path keeps the historical latency lead. */
uint32_t ap2cl_audible_lag_frames(struct ap2cl_s *p)
{
    if (!p) return 0;
    if (p->flow == FLOW_NATIVE_AP2 && p->splice_timeline)
        return (uint32_t)ap2_pacing_window_frames(p);
    return (uint32_t)MS2TS(p->latency_ms, p->format.sample_rate);
}

/* Minimum lead (ms) a warm commanded START needs for the new content to begin
 * exactly at the commanded instant: on the splice timeline the receiver's
 * queued audio (one pacing depth) plays out first, so the caller should
 * anchor beyond it. 0 = no constraint (stock flush+re-anchor path). */
int ap2cl_warm_lead_ms(struct ap2cl_s *p)
{
    return p && p->splice_timeline ? AP2_SPLICE_PACING_MS : 0;
}

/* Wall-clock instant (unix ms) at which the current delivery head becomes
 * audible on the splice timeline. The flush ack carries it so the caller can
 * anchor the warm START beyond EVERY member's queued audio: a commanded
 * instant at or behind a member's head splices at that member's own head
 * instead, silently breaking the shared instant (and the session's recorded
 * start time, which late joiners anchor against). 0 when the stream is not on
 * the splice timeline (no constraint). */
uint64_t ap2cl_splice_head_unix_ms(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || !p->splice_timeline || !p->head_ts)
        return 0;
    /* head_ts lives in the frame-clock domain of the unix-epoch NTP wall
     * clock, so its audible instant is the direct inverse mapping. */
    return ap2_ntp_to_unix_ms(TS2NTP(p->head_ts, p->format.sample_rate));
}

/* Silence frames the audio loop still owes the wire before the next real
 * sample (splice timeline): the pad bridges the frozen head and the commanded
 * splice instant with ordinary encoded chunks so sequence numbers and
 * timestamps stay contiguous. The loop consumes it as it sends. */
uint32_t ap2cl_splice_pad_frames(struct ap2cl_s *p)
{
    return p ? p->splice_pad_frames : 0;
}

/* True while the splice timeline is live on the wire (audio sends legal): the
 * audio loop keeps the idle-primed window between a FLUSH and the next START
 * fed with silence so the line can never lapse mid-session. */
bool ap2cl_splice_hot(struct ap2cl_s *p)
{
    return p && p->flow == FLOW_NATIVE_AP2 && p->splice_timeline &&
           p->state == AP2_STREAMING && !atomic_load(&p->rtsp_dead);
}

void ap2cl_splice_pad_consume(struct ap2cl_s *p, uint32_t frames)
{
    if (!p) return;
    p->splice_pad_frames = frames < p->splice_pad_frames
                               ? p->splice_pad_frames - frames : 0;
}

uint32_t ap2cl_content_skip_bytes(struct ap2cl_s *p)
{
    return p ? p->content_skip_bytes : 0;
}

void ap2cl_content_skip_consume(struct ap2cl_s *p, uint32_t bytes)
{
    if (!p) return;
    p->content_skip_bytes = bytes < p->content_skip_bytes
                                ? p->content_skip_bytes - bytes : 0;
}

int ap2cl_render_latency_ms(struct ap2cl_s *p) { return p ? p->dev_render_ms : 0; }

ap2_state_t ap2cl_state(struct ap2cl_s *p) { return p ? p->state : AP2_DOWN; }
bool ap2cl_is_connected(struct ap2cl_s *p)
{
    return p && p->state >= AP2_CONNECTED && !atomic_load(&p->rtsp_dead);
}

bool ap2cl_is_playing(struct ap2cl_s *p)
{
    return p && p->state == AP2_STREAMING && !atomic_load(&p->rtsp_dead);
}

#ifdef AP2_TESTING
void ap2cl_test_lock_mrp(struct ap2cl_s *p)
{
    pthread_mutex_lock(&p->mrp_lock);
}

void ap2cl_test_unlock_mrp(struct ap2cl_s *p)
{
    pthread_mutex_unlock(&p->mrp_lock);
}

void ap2cl_test_attach_rtsp_socket(struct ap2cl_s *p, int fd)
{
    p->sock_fd = fd;
    p->rtsp_established = true;
    p->rtsp_carry_len = 0;
    atomic_store(&p->rtsp_dead, false);
    snprintf(p->session_url, sizeof(p->session_url), "rtsp://test/session");
}

void ap2cl_test_detach_rtsp_socket(struct ap2cl_s *p)
{
    p->sock_fd = -1;
    p->rtsp_established = false;
    p->state = AP2_DOWN;
}

bool ap2cl_test_first_packet(struct ap2cl_s *p)
{
    return p->first_packet;
}

unsigned ap2cl_test_feedback_idle_streams(struct ap2cl_s *p)
{
    return p->feedback_idle_streams;
}

void ap2cl_test_set_first_packet(struct ap2cl_s *p, bool first_packet)
{
    p->first_packet = first_packet;
}

void ap2cl_test_set_splice(struct ap2cl_s *p, bool enable)
{
    p->splice_timeline = enable;
}

bool ap2cl_test_splice_default(const char *txt, const char *am)
{
    return !ap2_splice_denied(txt, am);
}

void ap2cl_test_set_anchor_valid(struct ap2cl_s *p, bool valid)
{
    p->rt_anchor_valid = valid;
}

bool ap2cl_test_anchor_valid(struct ap2cl_s *p)
{
    return p->rt_anchor_valid;
}

uint64_t ap2cl_test_head_ts(struct ap2cl_s *p)
{
    return p->head_ts;
}

/* Move the delivery head as a stall would find it: the wire timestamp stays
 * locked to the head (they advance together in the send path), so the
 * head<->rtp invariant survives the manipulation. */
void ap2cl_test_set_head_ts(struct ap2cl_s *p, uint64_t head)
{
    p->head_ts = head;
    p->rtp_timestamp = (uint32_t)head + atomic_load(&p->rtp_offset);
}

uint64_t ap2cl_test_timeline_reanchors(struct ap2cl_s *p)
{
    return p->timeline_reanchors;
}

uint32_t ap2cl_test_rtp_timestamp(struct ap2cl_s *p)
{
    return p->rtp_timestamp;
}

/* Bind the control socket to an ephemeral local port and start the retransmit
 * responder against it, so a test can play the receiver end. */
int ap2cl_test_start_rtx(struct ap2cl_s *p)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    p->ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->ctrl_sock < 0) return -1;
    if (bind(p->ctrl_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    socklen_t len = sizeof(addr);
    if (getsockname(p->ctrl_sock, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    if (!ap2_rtx_start(p)) return -1;
    return ntohs(addr.sin_port);
}

void ap2cl_test_stop_rtx(struct ap2cl_s *p)
{
    ap2_rtx_stop(p);
    if (p->ctrl_sock >= 0) { close(p->ctrl_sock); p->ctrl_sock = -1; }
}

void ap2cl_test_rtx_store(struct ap2cl_s *p, uint16_t seq,
                          const uint8_t *pkt, int len)
{
    ap2_rtx_store(p, seq, pkt, len);
}

unsigned long long ap2cl_test_rtx_answered(struct ap2cl_s *p)
{
    return atomic_load(&p->rtx_answered);
}

unsigned long long ap2cl_test_rtx_expired(struct ap2cl_s *p)
{
    return atomic_load(&p->rtx_expired);
}

void ap2cl_test_set_use_ptp(struct ap2cl_s *p, bool enable)
{
    p->use_ptp = enable;
}

void ap2cl_test_set_apple_model(struct ap2cl_s *p, bool apple)
{
    p->apple_model = apple;
}

bool ap2cl_test_apple_model_default(const char *txt, const char *am)
{
    return ap2_apple_model(txt, am);
}

/* Plant a probe-streak snapshot where the shared-daemon poller would put
 * one, so the verification poll consumes it on its next call. */
void ap2cl_test_inject_clock_exchange(struct ap2cl_s *p, uint32_t count,
                                      uint64_t first_ms, uint64_t third_ms)
{
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_verify_have_exchange = true;
    p->clock_verify_exchange.count = count;
    p->clock_verify_exchange.first_ms = first_ms;
    p->clock_verify_exchange.last_ms = 0;
    p->clock_verify_exchange.third_ms = third_ms;
    pthread_mutex_unlock(&p->clock_verify_lock);
}

/* Retract the planted snapshot the way a poll that came back empty does, so a
 * test can play a lost round-trip against a receiver that is still probing. */
void ap2cl_test_clear_clock_exchange(struct ap2cl_s *p)
{
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_verify_have_exchange = false;
    pthread_mutex_unlock(&p->clock_verify_lock);
}

/* Stamp the two instants the stall window is measured from — the connect a
 * real session records in ap2_native_connect, and the last streak readiness
 * itself observes — so a test can place that window wherever it needs it. */
void ap2cl_test_set_clock_marks(struct ap2cl_s *p, uint64_t connected_ms,
                                uint64_t last_streak_ms)
{
    pthread_mutex_lock(&p->clock_verify_lock);
    p->clock_connected_unix_ms = connected_ms;
    p->clock_last_streak_unix_ms = last_streak_ms;
    pthread_mutex_unlock(&p->clock_verify_lock);
}

/* Place the session state a real connect and teardown move through, so a test
 * can take a readiness reading against a live session and against a torn-down
 * one — the marks outlive teardown, the verdict must not. */
void ap2cl_test_set_session_state(struct ap2cl_s *p, ap2_state_t state)
{
    p->state = state;
}

void ap2cl_test_bump_audio_sent(struct ap2cl_s *p)
{
    p->audio_packets_sent++;
}

void ap2cl_test_set_dev_latency_max(struct ap2cl_s *p, uint32_t frames)
{
    p->dev_latency_max = frames;
}

uint64_t ap2cl_test_pacing_window_frames(struct ap2cl_s *p)
{
    return ap2_pacing_window_frames(p);
}
#endif
