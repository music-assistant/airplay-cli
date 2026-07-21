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
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

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
#include "ap2_timeline.h"
#include "ap2_feedback.h"

extern log_level *loglevel;

#define AP2_FRAMES_PER_CHUNK 352
#define AP2_CHACHA_TAG_SIZE  16
#define AP2_RTSP_SETUP_TIMEOUT_MS     8000
#define AP2_RTSP_FEEDBACK_TIMEOUT_MS  1500
#define AP2_RTSP_CONTROL_TIMEOUT_MS   3000
#define AP2_RTSP_METADATA_TIMEOUT_MS  5000
#define AP2_FEEDBACK_INTERVAL_MS      2000
#define AP2_UDP_SEND_TIMEOUT_MS       20
#define AP2_BUFFERED_WRITE_TIMEOUT_MS 8000
#define AP2_HAP_FRAME_MAX             1024
#define AP2_MRP_PENDING_REGISTER      (1u << 0)
#define AP2_MRP_PENDING_PUSH          (1u << 1)
#define AP2_MRP_PENDING_PLAYBACK      (1u << 2)

typedef enum {
    FLOW_RAOP_COMPAT = 0,
    FLOW_NATIVE_AP2,
} ap2_flow_t;

struct ap2cl_s {
    /* Configuration */
    ap2_device_info_t device;
    ap2_audio_format_t format;
    _Atomic ap2_state_t state;
    int latency_ms;
    uint32_t dev_latency_min;      /* receiver-reported buffering window (frames) */
    uint32_t dev_latency_max;
    int dev_render_ms;             /* receiver-reported arrival->render latency (ms) */
    int volume;
    ap2_flow_t flow;
    pthread_mutex_t media_lock;
    atomic_bool media_transition;

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
     * threads (streaming thread: SETRATEANCHORTIME/FLUSHBUFFERED/TEARDOWN;
     * command thread: SET_PARAMETER volume/metadata; maintenance worker:
     * /feedback keepalive).
     * This lock serializes the cycles so a response cannot be attributed to
     * the wrong request and the HAP nonce sequence stays intact. */
    pthread_mutex_t rtsp_lock;
    atomic_bool rtsp_dead;
    atomic_bool rtsp_established;
    atomic_bool media_healthy;
    ap2_periodic_worker_t feedback_worker;
    ap2_periodic_worker_t mrp_worker;
    bool workers_initialized;
    atomic_bool workers_stopping;
    atomic_bool feedback_waiting;
    int feedback_interval_ms;
    uint64_t feedback_last_ms;
    uint64_t feedback_next_ms;
    unsigned int feedback_failures;
    struct ap2_hap_ctx *hap;      /* HAP encryption context */
    struct ap2_ptp_ctx *ptp;      /* Timing */
    /* MRP now-playing over POST /command (path A, see DESIGN.md §8):
     * state carrier + body builder; only on pair-verified native sessions. */
    struct ap2_mrp_ctx *mrp;
    pthread_mutex_t mrp_lock;
    atomic_int mrp_event_status;
    atomic_uint mrp_pending;
    bool mrp_device_registered;
    bool mrp_extended_registered;
    int mrp_last_playback_state;
    int data_sock;                /* UDP audio */
    int ctrl_sock;                /* UDP control */
    int events_sock;              /* reverse TCP events connection (kept open) */
    struct sockaddr_in data_addr;
    struct sockaddr_in ctrl_addr;
    struct alac_codec_s *alac;    /* ALAC encoder for native flow */
    uint8_t audio_key[32];        /* ChaCha20 key for audio encryption */
    uint16_t seq_number;
    _Atomic uint32_t rtp_timestamp;
    uint32_t ssrc;
    uint64_t head_ts;
    bool first_packet;
    uint64_t audio_packets_sent;
    uint64_t audio_packets_dropped;
    uint64_t sync_packets_sent;
    uint64_t sync_packets_dropped;
    uint64_t timeline_reanchors;

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
    _Atomic uint32_t rtp_offset;   /* effective timeline offset (see start_at) */
    uint64_t audio_nonce_counter;
    char session_url[128];
    char session_uuid[40];
    char group_uuid[40];
    uint32_t session_id;
    int cseq;

    /* PTP timing selection */
    bool use_ptp;      /* resolved: PTP grandmaster timing active this session */
    bool ptp_forced;   /* ap2cl_set_ptp() was called (overrides auto-detect) */
    bool ptp_enabled;  /* value passed to ap2cl_set_ptp() */
    bool ptp_shared;   /* prefer a shared PTP daemon clock (multi-room) if present */

    /* Buffered audio (type 103): RTP is pushed over a TCP connection to the
     * receiver's dataPort instead of the realtime UDP data socket. */
    bool buffered;         /* buffered stream requested */
    bool use_buffered;     /* resolved: buffered active this session (needs PTP) */
    int buffered_sock;     /* TCP connection to the receiver's dataPort */
    bool anchored;         /* SETRATEANCHORTIME has been sent */
};

static int ap2_mrp_send_playback_state(struct ap2cl_s *p,
                                       ap2_mrp_playback_state_t state,
                                       bool force);
static int ap2cl_mrp_register_locked(struct ap2cl_s *p);
static int ap2cl_mrp_push_locked(struct ap2cl_s *p);

static void ap2_mrp_process_pending(struct ap2cl_s *p, unsigned int pending)
{
    if (!pending || atomic_load(&p->rtsp_dead)) return;
    if (pending & AP2_MRP_PENDING_PUSH) {
        ap2cl_mrp_push_locked(p);
    } else {
        if (pending & AP2_MRP_PENDING_REGISTER)
            ap2cl_mrp_register_locked(p);
        if (pending & AP2_MRP_PENDING_PLAYBACK) {
            ap2_mrp_playback_state_t state =
                p->state == AP2_STREAMING ? AP2_MRP_PLAYBACK_PLAYING :
                p->state == AP2_PAUSED ? AP2_MRP_PLAYBACK_PAUSED :
                                         AP2_MRP_PLAYBACK_STOPPED;
            ap2_mrp_send_playback_state(p, state, true);
        }
    }
}

/* ---- Native AP2 RTSP I/O ---- */



static int ap2_rtsp_timeout_ms(struct ap2cl_s *p, const char *method,
                               const char *uri, const char *content_type,
                               int body_len)
{
    if (!p->rtsp_established) return AP2_RTSP_SETUP_TIMEOUT_MS;
    if (strcmp(method, "POST") == 0 && strcmp(uri, "/feedback") == 0)
        return AP2_RTSP_FEEDBACK_TIMEOUT_MS;
    if ((strcmp(method, "POST") == 0 && strcmp(uri, "/command") == 0) ||
        (strcmp(method, "SET_PARAMETER") == 0 && content_type &&
         (strstr(content_type, "image/") == content_type ||
          strcmp(content_type, "application/x-dmap-tagged") == 0)) ||
        body_len > 4096)
        return AP2_RTSP_METADATA_TIMEOUT_MS;
    return AP2_RTSP_CONTROL_TIMEOUT_MS;
}

static void ap2_mark_rtsp_dead(struct ap2cl_s *p, const char *method,
                               const char *uri, const char *phase,
                               uint64_t elapsed_ms)
{
    int saved_errno = errno;
    if ((p->rtsp_established || p->hap) &&
        !atomic_exchange(&p->rtsp_dead, true)) {
        LOG_ERROR("[AP2] RTSP channel failed during %s %s %s after %" PRIu64
                  "ms: %s; terminating native session",
                  method, uri, phase, elapsed_ms, strerror(saved_errno));
        shutdown(p->sock_fd, SHUT_RDWR);
    }
    errno = saved_errno;
}

static int ap2_parse_rtsp_response(const uint8_t *data, int len,
                                   int expected_cseq,
                                   int *status, int *header_len,
                                   int *content_len)
{
    if (!data || len <= 0) return 0;
    ap2_rtsp_response_t response;
    int parsed = ap2_io_parse_rtsp_response(
        data, (size_t)len, &response);
    if (parsed <= 0) return parsed;
    if (response.message_len != (size_t)len ||
        response.cseq != expected_cseq ||
        response.header_len > INT_MAX ||
        response.body_len > INT_MAX)
        return -1;
    *status = response.status;
    *header_len = (int)response.header_len;
    *content_len = (int)response.body_len;
    return 1;
}

static int ap2_hap_frames_complete(const uint8_t *data, int len)
{
    int offset = 0;
    while (offset < len) {
        if (len - offset < 2) return 0;
        int frame_len = data[offset] | (data[offset + 1] << 8);
        if (frame_len <= 0 || frame_len > AP2_HAP_FRAME_MAX) return -1;
        int wire_len = 2 + frame_len + AP2_CHACHA_TAG_SIZE;
        if (len - offset < wire_len) return 0;
        offset += wire_len;
    }
    return 1;
}

static int ap2_rtsp_send_ex_unlocked(
    struct ap2cl_s *p, const char *method, const char *uri,
    const uint8_t *body, int body_len, const char *ct,
    const char *extra_hdr, uint8_t **resp_body, int *resp_len)
{
    if (atomic_load(&p->rtsp_dead)) return 0;
    int cseq = p->cseq++;
    char hdr[1024];
    int hdr_len = snprintf(
        hdr, sizeof(hdr),
        "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: AirPlay/670.6.2\r\n"
        "DACP-ID: %s\r\nActive-Remote: %s\r\n%s%s%s%s"
        "Content-Length: %d\r\n\r\n",
        method, uri, cseq,
        p->dacp_id ? p->dacp_id : "0",
        p->active_remote ? p->active_remote : "0",
        ct ? "Content-Type: " : "", ct ? ct : "", ct ? "\r\n" : "",
        extra_hdr ? extra_hdr : "", body_len);
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(hdr)) {
        errno = EMSGSIZE;
        return 0;
    }

    uint8_t *msg = NULL;
    int msg_len;
    if (p->hap) {
        int raw_len = hdr_len + body_len;
        uint8_t *raw = malloc((size_t)raw_len);
        if (!raw) return 0;
        memcpy(raw, hdr, (size_t)hdr_len);
        if (body && body_len > 0)
            memcpy(raw + hdr_len, body, (size_t)body_len);
        msg_len = ap2_hap_encrypt(p->hap, raw, raw_len, &msg);
        free(raw);
        if (msg_len <= 0 || !msg) {
            free(msg);
            errno = EIO;
            ap2_mark_rtsp_dead(p, method, uri, "encryption", 0);
            return 0;
        }
    } else {
        msg_len = hdr_len + body_len;
        msg = malloc((size_t)msg_len);
        if (!msg) return 0;
        memcpy(msg, hdr, (size_t)hdr_len);
        if (body && body_len > 0)
            memcpy(msg + hdr_len, body, (size_t)body_len);
    }

    int timeout_ms = ap2_rtsp_timeout_ms(p, method, uri, ct, body_len);
    uint64_t started_ms = ap2_io_monotonic_ms();
    uint64_t deadline_ms = started_ms + (uint64_t)timeout_ms;
    LOG_SDEBUG("[AP2DIAG] RTSP TX cseq=%d %s %s body=%d wire=%d "
               "timeout=%dms", cseq, method, uri, body_len, msg_len, timeout_ms);
    if (!ap2_io_write_all_deadline(p->sock_fd, msg, msg_len, deadline_ms)) {
        free(msg);
        ap2_mark_rtsp_dead(p, method, uri, "write",
                           ap2_io_monotonic_ms() - started_ms);
        return 0;
    }
    free(msg);

    uint8_t buf[16384] = {0};
    int total = 0;
    if (!p->hap) {
        while (total < (int)sizeof(buf) - 1) {
            int n = (int)ap2_io_read_deadline(
                p->sock_fd, buf + total,
                sizeof(buf) - 1 - (size_t)total, deadline_ms);
            if (n <= 0) break;
            total += n;
            int status, hlen, cl;
            int parsed = ap2_parse_rtsp_response(
                buf, total, cseq, &status, &hlen, &cl);
            if (parsed < 0) {
                errno = EPROTO;
                break;
            }
            if (parsed > 0) {
                *resp_len = cl;
                *resp_body = cl > 0 ? malloc((size_t)cl) : NULL;
                if (cl > 0 && !*resp_body) {
                    errno = ENOMEM;
                    break;
                }
                if (*resp_body) memcpy(*resp_body, buf + hlen, (size_t)cl);
                return status;
            }
        }
    } else {
        while (total < (int)sizeof(buf) - 1) {
            int n = (int)ap2_io_read_deadline(
                p->sock_fd, buf + total,
                sizeof(buf) - 1 - (size_t)total, deadline_ms);
            if (n <= 0) break;
            total += n;
            int frames_complete = ap2_hap_frames_complete(buf, total);
            if (frames_complete == 0) continue;
            if (frames_complete < 0) {
                errno = EPROTO;
                break;
            }

            uint64_t saved_counter = ap2_hap_save_read_counter(p->hap);
            uint8_t *dec = NULL;
            int dec_len = ap2_hap_decrypt(p->hap, buf, total, &dec);
            if (dec_len > 0 && dec) {
                uint8_t *terminated = realloc(dec, (size_t)dec_len + 1);
                if (!terminated) {
                    free(dec);
                    ap2_hap_restore_read_counter(p->hap, saved_counter);
                    errno = ENOMEM;
                    break;
                }
                dec = terminated;
                int status, hlen, cl;
                int parsed = ap2_parse_rtsp_response(
                    dec, dec_len, cseq, &status, &hlen, &cl);
                if (parsed > 0) {
                    *resp_len = cl;
                    *resp_body = cl > 0 ? malloc((size_t)cl) : NULL;
                    if (cl > 0 && !*resp_body) {
                        free(dec);
                        ap2_hap_restore_read_counter(p->hap, saved_counter);
                        errno = ENOMEM;
                        break;
                    }
                    if (*resp_body)
                        memcpy(*resp_body, dec + hlen, (size_t)cl);
                    free(dec);
                    return status;
                }
                free(dec);
                ap2_hap_restore_read_counter(p->hap, saved_counter);
                if (parsed < 0) {
                    errno = EPROTO;
                    break;
                }
            } else {
                free(dec);
                errno = EPROTO;
                break;
            }
        }
    }

    ap2_mark_rtsp_dead(p, method, uri, "read",
                       ap2_io_monotonic_ms() - started_ms);
    *resp_body = NULL;
    *resp_len = 0;
    return 0;
}

/* CSeq assignment, HAP nonce advancement, request write and response read are
 * one serialized transaction. */
static int ap2_rtsp_send_ex(
    struct ap2cl_s *p, const char *method, const char *uri,
    const uint8_t *body, int body_len, const char *ct,
    const char *extra_hdr, uint8_t **resp_body, int *resp_len)
{
    if (resp_body) *resp_body = NULL;
    if (resp_len) *resp_len = 0;
    uint64_t wait_started = ap2_io_monotonic_ms();
    pthread_mutex_lock(&p->rtsp_lock);
    uint64_t waited_ms = ap2_io_monotonic_ms() - wait_started;
    if (waited_ms >= 250)
        LOG_SDEBUG("[AP2DIAG] RTSP %s %s waited %" PRIu64
                   "ms for control lock", method, uri, waited_ms);
    int status = ap2_rtsp_send_ex_unlocked(
        p, method, uri, body, body_len, ct, extra_hdr,
        resp_body, resp_len);
    pthread_mutex_unlock(&p->rtsp_lock);
    return status;
}

/* Convenience wrapper: send an RTSP request with no extra headers. */
static int ap2_rtsp_send(struct ap2cl_s *p, const char *method, const char *uri,
                         const uint8_t *body, int body_len, const char *ct,
                         uint8_t **resp_body, int *resp_len)
{
    return ap2_rtsp_send_ex(p, method, uri, body, body_len, ct, NULL,
                            resp_body, resp_len);
}

static bool ap2_feedback_worker_tick(void *arg)
{
    struct ap2cl_s *p = arg;
    if (atomic_load(&p->workers_stopping) ||
        atomic_load(&p->rtsp_dead))
        return false;

    uint64_t now_ms = ap2_io_monotonic_ms();
    if (now_ms >= p->feedback_next_ms) {
        uint64_t gap_ms = now_ms - p->feedback_last_ms;
        LOG_SDEBUG("[AP2DIAG] feedback tick gap=%" PRIu64 "ms", gap_ms);
        if (gap_ms > (uint64_t)p->feedback_interval_ms + 500)
            LOG_WARN("[AP2] feedback cadence delayed: %" PRIu64 "ms",
                     gap_ms);
        atomic_store(&p->feedback_waiting, true);
        bool feedback_ok = ap2cl_feedback(p);
        atomic_store(&p->feedback_waiting, false);
        if (feedback_ok) {
            p->feedback_failures = 0;
        } else if (!atomic_load(&p->rtsp_dead) &&
                   ++p->feedback_failures >= 3) {
            errno = EPROTO;
            ap2_mark_rtsp_dead(p, "POST", "/feedback",
                               "repeated non-200 response",
                               ap2_io_monotonic_ms() - p->feedback_last_ms);
        }
        p->feedback_last_ms = ap2_io_monotonic_ms();
        p->feedback_next_ms += (uint64_t)p->feedback_interval_ms;
        if (p->feedback_next_ms <= p->feedback_last_ms)
            p->feedback_next_ms =
                p->feedback_last_ms + (uint64_t)p->feedback_interval_ms;
    }

    /* This worker is the sole owner of reverse-event/DataStream ticking.
     * Run it after the keepalive so a busy event stream cannot delay the
     * receiver's feedback deadline. */
    pthread_mutex_lock(&p->mrp_lock);
    int event_status = -1;
    if (p->mrp) {
        ap2_mrp_tick(p->mrp);
        event_status = ap2_mrp_event_status(p->mrp);
    }
    atomic_store(&p->mrp_event_status, event_status);
    pthread_mutex_unlock(&p->mrp_lock);
    return !atomic_load(&p->rtsp_dead);
}

static bool ap2_mrp_worker_tick(void *arg)
{
    struct ap2cl_s *p = arg;
    if (atomic_load(&p->workers_stopping) ||
        atomic_load(&p->rtsp_dead))
        return false;
    ap2_mrp_process_pending(p, atomic_exchange(&p->mrp_pending, 0));
    return !atomic_load(&p->rtsp_dead);
}

static bool ap2_feedback_start(struct ap2cl_s *p)
{
    if (!p->workers_initialized) return false;
    atomic_store(&p->workers_stopping, false);
    p->feedback_failures = 0;
    p->feedback_last_ms = ap2_io_monotonic_ms();
    p->feedback_next_ms =
        p->feedback_last_ms + (uint64_t)p->feedback_interval_ms;
    if (!ap2_periodic_worker_start(&p->feedback_worker)) {
        LOG_ERROR("[AP2] Cannot start feedback worker: %s", strerror(errno));
        return false;
    }
    if (!ap2_periodic_worker_start(&p->mrp_worker)) {
        LOG_ERROR("[AP2] Cannot start MRP publication worker: %s",
                  strerror(errno));
        atomic_store(&p->workers_stopping, true);
        ap2_periodic_worker_stop(&p->feedback_worker);
        return false;
    }
    LOG_DEBUG("[AP2] maintenance workers started (feedback=%dms)",
              p->feedback_interval_ms);
    return true;
}

static void ap2_feedback_stop(struct ap2cl_s *p)
{
    if (!p || !p->workers_initialized) return;
    atomic_store(&p->workers_stopping, true);
    ap2_periodic_worker_stop(&p->mrp_worker);
    ap2_periodic_worker_stop(&p->feedback_worker);
}

#ifdef AP2_TESTING
bool ap2cl_test_start_feedback_worker(struct ap2cl_s *p, int socket_fd,
                                      int interval_ms)
{
    if (!p || socket_fd < 0 || interval_ms <= 0) return false;
    p->flow = FLOW_NATIVE_AP2;
    p->sock_fd = socket_fd;
    p->rtsp_established = true;
    p->state = AP2_CONNECTED;
    p->feedback_interval_ms = interval_ms;
    atomic_store(&p->rtsp_dead, false);
    return ap2_feedback_start(p);
}

bool ap2cl_test_attach_mrp(struct ap2cl_s *p, int event_socket,
                           const uint8_t shared_secret[32])
{
    if (!p || event_socket < 0 || !shared_secret) return false;
    pthread_mutex_lock(&p->mrp_lock);
    p->mrp = ap2_mrp_create(
        "127.0.0.1", 7000, p->auth_credentials, p->dacp_id,
        "test sender", "11111111-2222-4333-8444-555555555555",
        "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", shared_secret);
    bool attached = p->mrp && ap2_mrp_attach_events(p->mrp, event_socket);
    if (!attached && p->mrp) {
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
    }
    atomic_store(&p->mrp_event_status, attached ? 1 : -1);
    pthread_mutex_unlock(&p->mrp_lock);
    return attached;
}

void ap2cl_test_stop_feedback_worker(struct ap2cl_s *p)
{
    if (!p) return;
    ap2_feedback_stop(p);
    p->sock_fd = -1;
    p->rtsp_established = false;
    atomic_store(&p->state, AP2_DOWN);
}

int ap2cl_test_post_command(struct ap2cl_s *p)
{
    if (!p) return 0;
    uint8_t *response = NULL;
    int response_len = 0;
    int status = ap2_rtsp_send(
        p, "POST", "/command", NULL, 0, NULL, &response, &response_len);
    free(response);
    return status;
}

#endif

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

ap2_route_t ap2_resolve_route(ap2_proto_pref_t pref, const char *txt, const char *pw,
                              bool have_credentials, int bit_depth,
                              bool force_native, bool force_buffered,
                              bool ptp_forced, bool ptp_enabled)
{
    ap2_route_t r = {0};
    r.features = ap2_txt_features(txt);
    r.flags = ap2_txt_flags(txt);
    bool has_pw = (pw && !strcasecmp(pw, "true"));

    /* 1. Protocol: AirPlay 2 vs legacy RAOP. The --ap2-native / --buffered
     * overrides pull us onto AirPlay 2 regardless of the advertised features. */
    bool is_ap2;
    if (pref == AP2_PROTO_RAOP) {
        is_ap2 = false;
    } else if (pref == AP2_PROTO_AIRPLAY2) {
        is_ap2 = true;
    } else { /* AUTO */
        is_ap2 = AP2_FEAT(r.features, AP2_FEAT_UNIFIED_MEDIA) ||
                 AP2_FEAT(r.features, AP2_FEAT_COREUTILS);
    }
    if (force_native || force_buffered) is_ap2 = true;

    if (!is_ap2) {
        r.use_raop = true;
        r.reason = "legacy RAOP";
        return r;
    }

    /* 2. Native AP2 vs RAOP-compatible flow. Stored credentials or an explicit
     * override select native; in AUTO, transient-pairable devices (pairing bits,
     * no PIN/legacy flag, no password) go native too. Explicit --protocol
     * airplay2 keeps the proven RAOP-compat default unless native is forced. */
    bool native, transient = false;
    if (have_credentials) {
        native = true;            /* pair-verify with stored keys (Apple TV/HomePod) */
    } else if (force_native || force_buffered) {
        native = true;
        transient = true;         /* no creds -> transient pairing */
    } else if (pref == AP2_PROTO_AUTO &&
               (AP2_FEAT(r.features, AP2_FEAT_HK_PAIRING) ||
                AP2_FEAT(r.features, AP2_FEAT_COREUTILS)) &&
               !(r.flags & (AP2_SF_PIN_REQUIRED | AP2_SF_LEGACY_PAIRING)) && !has_pw) {
        native = true;
        transient = true;
    } else {
        native = false;           /* RAOP-compatible fallback */
    }

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

    /* 4. Buffered (type 103): only when forced by --buffered. Hi-res (24-bit)
     * rides the REALTIME stream — verified audible on device — so it must
     * never steer the route onto the buffered path (whose playback anchoring
     * is unresolved on Apple TV). Buffered anchoring needs PTP. */
    (void)bit_depth;
    r.buffered = force_buffered;
    if (r.buffered) r.ptp = true;

    r.reason = transient
                   ? (r.buffered ? "native AP2, transient, buffered"
                                 : "native AP2, transient, realtime")
                   : (r.buffered ? "native AP2, pair-verify, buffered"
                                 : "native AP2, pair-verify, realtime");
    return r;
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
    pthread_mutex_lock(&p->mrp_lock);
    if (!p->mrp)
        p->mrp = ap2_mrp_create(p->device.address, p->device.port,
                                p->auth_credentials, p->dacp_id, p->device.name,
                                p->session_uuid, p->group_uuid,
                                ap2_hap_get_shared_secret(p->hap));
    if (!p->mrp) goto done;
    if (!ap2_mrp_attach(p->mrp, data_port, seed)) {
        LOG_WARN("[MRP] data-channel attach failed; continuing with /command");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        atomic_store(&p->mrp_event_status, -1);
        goto done;
    }
    if (!ap2_mrp_attach_events(p->mrp, p->events_sock)) {
        LOG_WARN("[MRP] event-channel attach failed; MediaRemote disabled");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        atomic_store(&p->mrp_event_status, -1);
        goto done;
    }
    p->events_sock = -1;
    atomic_store(&p->mrp_event_status, 1);
    LOG_INFO("[MRP] remote-control data channel established (now-playing active)");
done:
    pthread_mutex_unlock(&p->mrp_lock);
}

/* ---- Native AP2 connect sequence ---- */

static bool ap2_native_connect(struct ap2cl_s *p)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", p->device.port);

    /* Resolve the local bind address once (multi-homed hosts): used for the
     * RTSP TCP socket and the RTP data/control UDP sockets below. */
    p->bind_addr.s_addr = INADDR_ANY;
    if (p->iface) {
        char *ifname = NULL;
        uint32_t netmask;
        p->bind_addr = get_interface(p->iface, &ifname, &netmask);
        NFREE(ifname);
    }

    if (getaddrinfo(p->device.address, port_str, &hints, &res) != 0) return false;
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
    atomic_store(&p->rtsp_dead, false);
    atomic_store(&p->media_healthy, true);
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(p->sock_fd, SOL_SOCKET, SO_NOSIGPIPE,
               &no_sigpipe, sizeof(no_sigpipe));
#endif

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
    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "GET", "/info", NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    if (status != 200) { LOG_ERROR("[AP2] /info failed: %d", status); return false; }

    /* 2. HAP pairing: pair-verify with stored credentials (Apple TV/HomePod),
     * transient pair-setup otherwise (Sonos and most third-party receivers) */
    if (p->auth_credentials) {
        p->hap = ap2_hap_create(p->auth_credentials);
        if (!p->hap) { LOG_ERROR("[AP2] Invalid credentials"); return false; }

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
        if (!ap2_hap_pair_verify(p->hap, p->sock_fd)) {
            LOG_ERROR("[AP2] HAP pair-verify failed");
            ap2_hap_destroy(p->hap);
            p->hap = NULL;
            return false;
        }
    } else {
        p->hap = ap2_hap_create(NULL);
        if (!p->hap) { LOG_ERROR("[AP2] Cannot create HAP context"); return false; }

        LOG_INFO("[AP2] Performing HAP transient pair-setup...");
        if (!ap2_hap_pair_setup_transient(p->hap, p->sock_fd)) {
            LOG_ERROR("[AP2] HAP transient pair-setup failed");
            ap2_hap_destroy(p->hap);
            p->hap = NULL;
            return false;
        }
    }
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

    /* Buffered audio (type 103) anchors playback against the PTP timeline via
     * SETRATEANCHORTIME, so it is only viable with an active grandmaster. Fall
     * back to the realtime stream when PTP could not be established. */
    p->use_buffered = p->buffered && p->use_ptp;
    if (p->buffered && !p->use_buffered)
        LOG_WARN("[AP2] Buffered audio requested but PTP is unavailable; "
                 "falling back to realtime (type 96)");

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

    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);
    if (status != 200) {
        LOG_ERROR("[AP2] Session SETUP failed: %d", status);
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
                if (ap2_io_set_nonblocking(events_sock)) {
                    p->events_sock = events_sock;
                } else {
                    LOG_WARN("[AP2] Cannot make events socket nonblocking: %s",
                             strerror(errno));
                    close(events_sock);
                }
            } else {
                LOG_WARN("[AP2] Events connect failed");
                close(events_sock);
            }
        }
    }

    /* 5. Stream SETUP with streams array (BEFORE RECORD per Apple TV expectations) */
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
    struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_addr = p->bind_addr};
    if (p->data_sock < 0 ||
        bind(p->data_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        !ap2_io_set_nonblocking(p->data_sock)) {
        LOG_ERROR("[AP2] Cannot create non-blocking RTP data socket: %s",
                  strerror(errno));
        if (p->data_sock >= 0) { close(p->data_sock); p->data_sock = -1; }
        return false;
    }
    struct sockaddr_in ds_local;
    len = sizeof(ds_local);
    getsockname(p->data_sock, (struct sockaddr *)&ds_local, &len);
    int local_data_port = ntohs(ds_local.sin_port);

    p->ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->ctrl_sock < 0 ||
        bind(p->ctrl_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
        !ap2_io_set_nonblocking(p->ctrl_sock)) {
        LOG_ERROR("[AP2] Cannot create non-blocking RTP control socket: %s",
                  strerror(errno));
        if (p->ctrl_sock >= 0) { close(p->ctrl_sock); p->ctrl_sock = -1; }
        return false;
    }
    struct sockaddr_in cs_local;
    len = sizeof(cs_local);
    getsockname(p->ctrl_sock, (struct sockaddr *)&cs_local, &len);
    int local_ctrl_port = ntohs(cs_local.sin_port);

    /* Determine audio format */
    uint64_t audio_format;
    if (p->format.bit_depth > 16 && p->format.sample_rate >= 48000)
        audio_format = 0x200000;  /* ALAC 48000/24/2 */
    else if (p->format.bit_depth > 16)
        audio_format = 0x80000;   /* ALAC 44100/24/2 */
    else if (p->format.sample_rate >= 48000)
        audio_format = 0x100000;  /* ALAC 48000/16/2 */
    else
        audio_format = 0x40000;   /* ALAC 44100/16/2 */

    /* Realtime (type 96) streams the audio over UDP and carries our data port in
     * the SETUP; buffered (type 103) pushes RTP over a TCP connection to the
     * receiver's dataPort, so we advertise no dataPort and let the receiver
     * assign the TCP listener it returns in the response. */
    int stream_type = p->use_buffered ? 103 : 96;

    struct ap2_plist *ssp = ap2_plist_create();
    ap2_plist_stream_begin(ssp);
    ap2_plist_stream_add_int(ssp, "audioFormat", audio_format);
    ap2_plist_stream_add_string(ssp, "audioMode", "default");
    ap2_plist_stream_add_int(ssp, "controlPort", local_ctrl_port);
    ap2_plist_stream_add_int(ssp, "ct", 2);  /* ALAC */
    if (!p->use_buffered)
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

    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);

    if (status != 200) {
        LOG_ERROR("[AP2] Stream SETUP failed: %d", status);
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

    /* 6. RECORD to begin streaming */
    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "RECORD", p->session_url, NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    if (status != 200) {
        LOG_WARN("[AP2] RECORD returned %d", status);
    } else {
        LOG_INFO("[AP2] RECORD OK");
    }

    /* 6b. SETPEERS (PTP only): a bare binary-plist array of IP strings
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

        const char *peers[2] = { p->device.address, our_addr };
        ap2_ptp_set_peers(p->ptp, peers, 2);
    }

    /* 6c. Buffered audio: open the TCP data connection to the receiver's
     * dataPort (parsed above into p->data_addr). Audio is pushed here as
     * length-prefixed RTP; the realtime UDP data socket stays unused. */
    if (p->use_buffered) {
        if (p->data_addr.sin_port == 0) {
            LOG_ERROR("[AP2] Buffered stream SETUP returned no dataPort");
            return false;
        }
        p->buffered_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (p->buffered_sock >= 0 && p->bind_addr.s_addr != INADDR_ANY) {
            struct sockaddr_in la = {.sin_family = AF_INET, .sin_addr = p->bind_addr};
            if (bind(p->buffered_sock, (struct sockaddr *)&la, sizeof(la)) != 0)
                LOG_WARN("[AP2] Cannot bind buffered TCP socket to %s",
                         inet_ntoa(p->bind_addr));
        }
        if (p->buffered_sock >= 0) {
            /* Keep kernel timeouts as a second line of defense; audio writes
             * also use a cumulative userspace deadline. */
            struct timeval stv = {.tv_sec = 8};
            setsockopt(p->buffered_sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
            setsockopt(p->buffered_sock, SOL_SOCKET, SO_RCVTIMEO, &stv, sizeof(stv));
        }
        if (p->buffered_sock < 0 ||
            connect(p->buffered_sock, (struct sockaddr *)&p->data_addr,
                    sizeof(p->data_addr)) != 0) {
            LOG_ERROR("[AP2] Buffered data TCP connect to %s:%d failed: %s",
                      inet_ntoa(p->data_addr.sin_addr),
                      ntohs(p->data_addr.sin_port), strerror(errno));
            if (p->buffered_sock >= 0) { close(p->buffered_sock); p->buffered_sock = -1; }
            return false;
        }
        int one = 1;
        setsockopt(p->buffered_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        LOG_INFO("[AP2] Buffered data TCP connected to %s:%d",
                 inet_ntoa(p->data_addr.sin_addr), ntohs(p->data_addr.sin_port));
    }

    /* 6d. MRP now-playing. Ground-truth capture (DESIGN.md §8) shows a real
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
    if (!p->alac) {
        LOG_ERROR("[AP2] Cannot create ALAC encoder");
        return false;
    }

    p->first_packet = true;
    if (atomic_load(&p->rtsp_dead)) return false;
    p->rtsp_established = true;
    p->state = AP2_CONNECTED;
    if (!ap2_feedback_start(p)) {
        atomic_store(&p->rtsp_dead, true);
        return false;
    }
    LOG_INFO("[AP2] Native AP2 session ready");
    return true;
}

/* ---- Native AP2 audio send ---- */

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
        errno = ENOTCONN;
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

    ap2_io_datagram_result_t io_result = ap2_io_send_datagram_deadline(
        p->ctrl_sock, pkt, sizeof(pkt),
        (const struct sockaddr *)&p->ctrl_addr, sizeof(p->ctrl_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    ap2_send_result_t result =
        io_result == AP2_IO_DATAGRAM_SENT ? AP2_SEND_SENT :
        io_result == AP2_IO_DATAGRAM_DROPPED ? AP2_SEND_DROPPED :
                                               AP2_SEND_FATAL;
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
        errno = ENOTCONN;
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
    }
    ap2_timeline_ptp_anchor_t anchor;
    if (!ap2_timeline_ptp_anchor(
            wall, p->rt_anchor_wall0, p->rt_anchor_pos0,
            p->format.sample_rate, p->latency_ms, &anchor)) {
        atomic_store(&p->media_healthy, false);
        errno = EINVAL;
        return AP2_SEND_FATAL;
    }

    uint32_t be = htonl(anchor.frame_1);
    memcpy(pkt + 4, &be, 4);

    uint32_t wall_hi = htonl((uint32_t)(wall >> 32));
    uint32_t wall_lo = htonl((uint32_t)(wall & 0xFFFFFFFF));
    memcpy(pkt + 8, &wall_hi, 4);
    memcpy(pkt + 12, &wall_lo, 4);

    be = htonl(anchor.frame_2);
    memcpy(pkt + 16, &be, 4);

    uint64_t cid = ap2_ptp_master_clock_id(p->ptp);
    uint32_t cid_hi = htonl((uint32_t)(cid >> 32));
    uint32_t cid_lo = htonl((uint32_t)(cid & 0xFFFFFFFF));
    memcpy(pkt + 20, &cid_hi, 4);
    memcpy(pkt + 24, &cid_lo, 4);

    ap2_io_datagram_result_t io_result = ap2_io_send_datagram_deadline(
        p->ctrl_sock, pkt, sizeof(pkt),
        (const struct sockaddr *)&p->ctrl_addr, sizeof(p->ctrl_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    ap2_send_result_t result =
        io_result == AP2_IO_DATAGRAM_SENT ? AP2_SEND_SENT :
        io_result == AP2_IO_DATAGRAM_DROPPED ? AP2_SEND_DROPPED :
                                               AP2_SEND_FATAL;
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
    LOG_DEBUG("[AP2] TX PTP sync %s play_pos=%u wall=%" PRIu64 "ns",
              first ? "(initial)" : "", anchor.frame_1 - 11035, wall);
    return result;
}

/* ---- Native AP2 buffered audio (type 103) ---- */

static int ap2_encrypt_audio_payload(
    const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t *aad, int aad_len,
    const uint8_t *plaintext, int plaintext_len,
    uint8_t *ciphertext, uint8_t tag[AP2_CHACHA_TAG_SIZE])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int total = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len) == 1 &&
        EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) == 1;
    if (ok) {
        total = len;
        ok = EVP_EncryptFinal_ex(ctx, ciphertext + total, &len) == 1;
        total += len;
    }
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(
                 ctx, EVP_CTRL_AEAD_GET_TAG, AP2_CHACHA_TAG_SIZE, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? total : -1;
}

static ap2_send_result_t ap2_buffered_write_frame(
    struct ap2cl_s *p, const uint8_t *frame, size_t frame_len)
{
    size_t offset = 0;
    uint64_t deadline =
        ap2_io_monotonic_ms() + AP2_BUFFERED_WRITE_TIMEOUT_MS;
    while (offset < frame_len) {
        if (!offset && atomic_load(&p->media_transition)) {
            errno = EAGAIN;
            return AP2_SEND_FATAL;
        }

        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t written = send(
            p->buffered_sock, frame + offset, frame_len - offset, flags);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
            uint64_t now = ap2_io_monotonic_ms();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return AP2_SEND_FATAL;
            }
            uint64_t poll_deadline = now + 50;
            if (poll_deadline > deadline) poll_deadline = deadline;
            int ready = ap2_io_poll_fd(
                p->buffered_sock, POLLOUT, poll_deadline);
            if (ready > 0) continue;
            if (ready == 0 && poll_deadline < deadline) continue;
            return AP2_SEND_FATAL;
        }
        if (written == 0) errno = EPIPE;
        return AP2_SEND_FATAL;
    }
    return AP2_SEND_SENT;
}

/* Send the SETRATEANCHORTIME anchor. It pins an RTP sample number to a point on
 * the PTP timeline at a playback rate, so the receiver can schedule every
 * buffered sample: "sample rtpTime renders at PTP time networkTime, at rate".
 * rate=1 plays, rate=0 pauses. */
static bool ap2_send_setrateanchortime(struct ap2cl_s *p, uint32_t rtp_time,
                                       uint64_t anchor_ns, uint64_t rate)
{
    uint64_t secs = anchor_ns / 1000000000ULL;
    uint64_t rem_ns = anchor_ns % 1000000000ULL;
    /* networkTimeFrac is a fraction of a second in units of 1/2^64. Build it in
     * two 32-bit steps to avoid a 128-bit multiply: frac32 = rem_ns * 2^32 / 1e9
     * (a fraction in 1/2^32 units, <= 32 bits) placed in the high half. */
    uint64_t frac32 = ((uint64_t)rem_ns << 32) / 1000000000ULL;
    uint64_t network_time_frac = frac32 << 32;

    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "networkTimeTimelineID",
                    ap2_pl_int((int64_t)ap2_ptp_master_clock_id(p->ptp)));
    ap2_pl_dict_set(root, "networkTimeSecs", ap2_pl_int((int64_t)secs));
    ap2_pl_dict_set(root, "networkTimeFrac", ap2_pl_int((int64_t)network_time_frac));
    ap2_pl_dict_set(root, "rtpTime", ap2_pl_int((int64_t)rtp_time));
    ap2_pl_dict_set(root, "rate", ap2_pl_int((int64_t)rate));

    uint8_t *body = NULL;
    int body_len = ap2_pl_serialize(root, &body);
    ap2_pl_free(root);

    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "SETRATEANCHORTIME", p->session_url, body, body_len,
                               "application/x-apple-binary-plist", &resp, &resp_len);
    free(body);
    LOG_INFO("[AP2] SETRATEANCHORTIME rtp=%u anchor=%" PRIu64 "ns rate=%" PRIu64 " -> %d",
             rtp_time, anchor_ns, rate, status);
    if (status != 200 && resp && resp_len > 0) {
        int dump = resp_len < 64 ? resp_len : 64;
        char hex[200];
        for (int i = 0; i < dump; i++) sprintf(hex + i * 3, "%02x ", resp[i]);
        hex[dump * 3] = '\0';
        LOG_INFO("[AP2] SETRATEANCHORTIME response body (%d bytes): %s", resp_len, hex);
    }
    free(resp);
    return status == 200;
}

/* Discard buffered audio on stop/flush. flushUntilSeq/TS mark the end of the
 * range to drop; the receiver stops rendering and clears its buffer. */
static bool ap2_send_flushbuffered(struct ap2cl_s *p)
{
    ap2_pl_node *root = ap2_pl_dict();
    ap2_pl_dict_set(root, "flushUntilSeq", ap2_pl_int((int64_t)p->seq_number));
    ap2_pl_dict_set(root, "flushUntilTS", ap2_pl_int((int64_t)p->rtp_timestamp));

    uint8_t *body = NULL;
    int body_len = ap2_pl_serialize(root, &body);
    ap2_pl_free(root);

    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "FLUSHBUFFERED", p->session_url, body, body_len,
                               "application/x-apple-binary-plist", &resp, &resp_len);
    free(body);
    free(resp);
    LOG_INFO("[AP2] FLUSHBUFFERED untilSeq=%u untilTS=%u -> %d",
             p->seq_number, p->rtp_timestamp, status);
    return status == 200;
}

/* Push one encoded chunk over the buffered TCP data connection. The wire frame
 * is a 2-byte big-endian length prefix followed by the encrypted RTP packet
 * [12B hdr][ciphertext][16B tag][8B nonce]. TCP provides reliability and, via
 * backpressure on the blocking write, flow control (no resend logic). */
static ap2_send_result_t ap2_buffered_send_chunk(
    struct ap2cl_s *p, uint8_t *sample, int frames)
{
    if (p->buffered_sock < 0 || !p->alac) {
        errno = ENOTCONN;
        return AP2_SEND_FATAL;
    }

    uint8_t *encoded = NULL;
    int enc_size = 0;
    pcm_to_alac(p->alac, sample, frames, &encoded, &enc_size);
    if (!encoded || enc_size <= 0) {
        LOG_ERROR("[AP2] ALAC encoder failed");
        free(encoded);
        errno = EIO;
        return AP2_SEND_FATAL;
    }

    const uint8_t *audio_key = ap2_hap_get_shared_secret(p->hap);
    if (!audio_key) {
        LOG_ERROR("[AP2] No shared secret for audio encryption");
        free(encoded);
        errno = EACCES;
        return AP2_SEND_FATAL;
    }

    /* RTP header (12 bytes). Payload type = stream type (103), marker bit set on
     * the first packet, mirroring the realtime header shape. */
    uint8_t rtp_hdr[12];
    rtp_hdr[0] = 0x80;
    rtp_hdr[1] = p->first_packet ? 0xE7 : 0x67;   /* 0x67 = PT 103 */
    rtp_hdr[2] = (p->seq_number >> 8) & 0xFF;
    rtp_hdr[3] = p->seq_number & 0xFF;
    uint32_t ts_be = htonl(p->rtp_timestamp);
    memcpy(rtp_hdr + 4, &ts_be, 4);
    uint32_t ssrc_be = htonl(p->ssrc);
    memcpy(rtp_hdr + 8, &ssrc_be, 4);

    /* Nonce = 4 zero bytes + an 8-byte little-endian per-packet counter placed at
     * offset 4 (matching the realtime nonce layout). The same 8 counter bytes are
     * appended to the packet so the receiver reconstructs the nonce explicitly.
     * AAD = RTP header bytes 4..11 (timestamp + ssrc), as in the realtime path. */
    uint64_t counter = p->audio_nonce_counter;
    uint8_t nonce[12];
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)((counter >> (8 * i)) & 0xFF);

    int cap = 2 + 12 + enc_size + AP2_CHACHA_TAG_SIZE + 8;
    uint8_t *frame = malloc(cap);
    if (!frame) {
        LOG_ERROR("[AP2] Cannot allocate buffered audio frame");
        free(encoded);
        errno = ENOMEM;
        return AP2_SEND_FATAL;
    }
    uint8_t *pkt = frame + 2;   /* payload starts after the 2-byte length prefix */
    memcpy(pkt, rtp_hdr, 12);

    uint8_t tag[AP2_CHACHA_TAG_SIZE];
    int ct_len = ap2_encrypt_audio_payload(
        audio_key, nonce, rtp_hdr + 4, 8, encoded, enc_size, pkt + 12, tag);
    free(encoded);
    if (ct_len < 0) {
        LOG_ERROR("[AP2] Buffered audio encryption failed");
        free(frame);
        errno = EIO;
        return AP2_SEND_FATAL;
    }
    memcpy(pkt + 12 + ct_len, tag, sizeof(tag));

    int payload_len = 12 + ct_len + AP2_CHACHA_TAG_SIZE;
    memcpy(pkt + payload_len, nonce + 4, 8);   /* trailing nonce (nonce[4..11]) */
    payload_len += 8;

    /* The 2-byte big-endian prefix is the TOTAL frame length INCLUDING itself:
     * the receiver reads it, then reads (length - 2) more bytes for the RTP
     * packet. */
    int total = 2 + payload_len;
    frame[0] = (uint8_t)((total >> 8) & 0xFF);
    frame[1] = (uint8_t)(total & 0xFF);

    ap2_send_result_t write_result = ap2_buffered_write_frame(
        p, frame, (size_t)total);
    free(frame);

    if (write_result == AP2_SEND_SENT) {
        p->audio_nonce_counter++;
        p->first_packet = false;
        p->seq_number++;
        p->rtp_timestamp += frames;
        p->head_ts += frames;
    } else if (errno != EAGAIN) {
        LOG_ERROR("[AP2] Buffered TCP write failed: %s", strerror(errno));
        shutdown(p->buffered_sock, SHUT_RDWR);
        close(p->buffered_sock);
        p->buffered_sock = -1;
    }
    return write_result;
}

static ap2_send_result_t ap2_native_send_chunk(
    struct ap2cl_s *p, uint8_t *sample, int frames)
{
    if (p->use_buffered) return ap2_buffered_send_chunk(p, sample, frames);
    if (p->data_sock < 0 || !p->alac) {
        errno = ENOTCONN;
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
        LOG_ERROR("[AP2] ALAC encoder failed");
        free(encoded);
        errno = EIO;
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
        errno = EACCES;
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
        errno = ENOMEM;
        return AP2_SEND_FATAL;
    }
    memcpy(pkt, rtp_hdr, 12);

    uint8_t tag[AP2_CHACHA_TAG_SIZE];
    int ct_len = ap2_encrypt_audio_payload(
        audio_key, nonce, rtp_hdr + 4, 8, encoded, enc_size, pkt + 12, tag);
    free(encoded);
    if (ct_len < 0) {
        LOG_ERROR("[AP2] Realtime audio encryption failed");
        free(pkt);
        errno = EIO;
        return AP2_SEND_FATAL;
    }
    memcpy(pkt + 12 + ct_len, tag, sizeof(tag));

    /* Append the 8-byte per-packet nonce (the low 8 bytes of the 12-byte nonce)
     * so the receiver can reconstruct it. AP2 realtime wire format is
     * [12B RTP hdr][ciphertext][16B tag][8B nonce]; without the suffix every
     * packet fails its auth tag and is silently dropped. */
    memcpy(pkt + 12 + ct_len + AP2_CHACHA_TAG_SIZE, nonce + 4, 8);
    int actual_pkt_size = 12 + ct_len + AP2_CHACHA_TAG_SIZE + 8;
    ap2_io_datagram_result_t send_result = ap2_io_send_datagram_deadline(
        p->data_sock, pkt, (size_t)actual_pkt_size,
        (struct sockaddr *)&p->data_addr, sizeof(p->data_addr),
        ap2_io_monotonic_ms() + AP2_UDP_SEND_TIMEOUT_MS);
    free(pkt);

    if (send_result == AP2_IO_DATAGRAM_SENT) {
        p->audio_packets_sent++;
    } else if (send_result == AP2_IO_DATAGRAM_DROPPED) {
        p->audio_packets_dropped++;
        if (p->audio_packets_dropped <= 3 ||
            (p->audio_packets_dropped % 100) == 0)
            LOG_WARN("[AP2] RTP send transiently dropped seq=%u rtptime=%u: %s",
                     p->seq_number, p->rtp_timestamp, strerror(errno));
    } else {
        LOG_ERROR("[AP2] RTP send failed seq=%u rtptime=%u bytes=%d: %s",
                  p->seq_number, p->rtp_timestamp, actual_pkt_size,
                  strerror(errno));
        return AP2_SEND_FATAL;
    }
    /* Realtime RTP follows the wall timeline even when the local UDP queue
     * drops a packet; advancing exposes a sequence gap instead of replaying an
     * old timestamp late. */
    p->first_packet = false;
    p->seq_number++;
    p->rtp_timestamp += frames;
    p->head_ts += frames;

    return send_result == AP2_IO_DATAGRAM_SENT
               ? AP2_SEND_SENT : AP2_SEND_DROPPED;
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

    if (!p->raopcl) return false;

    if (!raopcl_connect(p->raopcl, player_addr, p->device.port, p->volume > 0)) {
        raopcl_destroy(p->raopcl);
        p->raopcl = NULL;
        return false;
    }

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
    atomic_init(&p->state, AP2_DOWN);
    p->latency_ms = latency_ms;
    p->volume = volume;
    p->sock_fd = -1;
    p->data_sock = -1;
    p->ctrl_sock = -1;
    p->events_sock = -1;
    p->buffered_sock = -1;
    pthread_mutex_init(&p->rtsp_lock, NULL);
    pthread_mutex_init(&p->mrp_lock, NULL);
    pthread_mutex_init(&p->media_lock, NULL);
    atomic_init(&p->rtsp_dead, false);
    atomic_init(&p->rtsp_established, false);
    atomic_init(&p->media_healthy, true);
    atomic_init(&p->media_transition, false);
    atomic_init(&p->workers_stopping, true);
    atomic_init(&p->feedback_waiting, false);
    atomic_init(&p->mrp_event_status, -1);
    atomic_init(&p->mrp_pending, 0);
    p->feedback_interval_ms = AP2_FEEDBACK_INTERVAL_MS;
    if (!ap2_periodic_worker_init(
            &p->feedback_worker, 100, ap2_feedback_worker_tick, p)) {
        pthread_mutex_destroy(&p->rtsp_lock);
        pthread_mutex_destroy(&p->mrp_lock);
        pthread_mutex_destroy(&p->media_lock);
        free(p);
        return NULL;
    }
    if (!ap2_periodic_worker_init(
            &p->mrp_worker, 100, ap2_mrp_worker_tick, p)) {
        ap2_periodic_worker_destroy(&p->feedback_worker);
        pthread_mutex_destroy(&p->rtsp_lock);
        pthread_mutex_destroy(&p->mrp_lock);
        pthread_mutex_destroy(&p->media_lock);
        free(p);
        return NULL;
    }
    p->workers_initialized = true;
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

bool ap2cl_destroy(struct ap2cl_s *p)
{
    if (!p) return false;
    ap2_feedback_stop(p);
    /* Preserve the on-wire shutdown sequence (final MediaRemote stopped state,
     * MRP disconnect, RTSP TEARDOWN) on the normal EOF path too. */
    if (p->state != AP2_DOWN || p->sock_fd >= 0)
        ap2cl_disconnect(p);
    if (p->raopcl) raopcl_destroy(p->raopcl);
    if (p->hap) ap2_hap_destroy(p->hap);
    if (p->ptp) ap2_ptp_destroy(p->ptp);
    pthread_mutex_lock(&p->mrp_lock);
    if (p->mrp) {
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        atomic_store(&p->mrp_event_status, -1);
    }
    pthread_mutex_unlock(&p->mrp_lock);
    if (p->alac) alac_delete_encoder(p->alac);
    /* Close the RTSP socket under the lock so an in-flight request/response
     * cycle on another thread finishes first and a later cycle sees -1
     * instead of writing into a reused descriptor. */
    pthread_mutex_lock(&p->rtsp_lock);
    if (p->sock_fd >= 0) { close(p->sock_fd); p->sock_fd = -1; }
    pthread_mutex_unlock(&p->rtsp_lock);
    if (p->workers_initialized) {
        ap2_periodic_worker_destroy(&p->mrp_worker);
        ap2_periodic_worker_destroy(&p->feedback_worker);
        p->workers_initialized = false;
    }
    pthread_mutex_destroy(&p->rtsp_lock);
    pthread_mutex_destroy(&p->mrp_lock);
    pthread_mutex_destroy(&p->media_lock);
    if (p->data_sock >= 0) close(p->data_sock);
    if (p->ctrl_sock >= 0) close(p->ctrl_sock);
    if (p->events_sock >= 0) close(p->events_sock);
    if (p->buffered_sock >= 0) close(p->buffered_sock);
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

void ap2cl_set_buffered(struct ap2cl_s *p, bool enable)
{
    if (!p) return;
    p->buffered = enable;
    LOG_INFO("[AP2] Buffered audio (type 103) %s", enable ? "requested" : "disabled");
}

bool ap2cl_connect(struct ap2cl_s *p)
{
    if (!p) return false;

    if (p->flow == FLOW_NATIVE_AP2) {
        LOG_INFO("[AP2] Connecting via native AP2 flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_native_connect(p);
    } else {
        LOG_INFO("[AP2] Connecting via RAOP-compatible flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_raop_compat_connect(p);
    }
}

bool ap2cl_disconnect(struct ap2cl_s *p)
{
    if (!p) return false;
    atomic_store(&p->media_transition, true);
    pthread_mutex_lock(&p->media_lock);
    if (p->flow == FLOW_NATIVE_AP2) {
        ap2_feedback_stop(p);
        atomic_store(&p->mrp_pending, 0);
        /* Publish the final stopped state before disconnecting MediaRemote,
         * while the encrypted RTSP session is still live. */
        pthread_mutex_lock(&p->mrp_lock);
        bool mrp_ready = p->mrp != NULL;
        if (mrp_ready) ap2_mrp_set_stopped(p->mrp);
        pthread_mutex_unlock(&p->mrp_lock);
        if (mrp_ready)
            ap2_mrp_send_playback_state(
                p, AP2_MRP_PLAYBACK_STOPPED, true);
        pthread_mutex_lock(&p->mrp_lock);
        if (p->mrp) ap2_mrp_stop(p->mrp);
        atomic_store(&p->mrp_event_status, -1);
        pthread_mutex_unlock(&p->mrp_lock);
        if (p->events_sock >= 0) {
            close(p->events_sock);
            p->events_sock = -1;
        }
        if (p->buffered_sock >= 0) {
            close(p->buffered_sock);
            p->buffered_sock = -1;
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
        p->rtsp_established = false;
    } else if (p->raopcl) {
        raopcl_disconnect(p->raopcl);
    }
    p->state = AP2_DOWN;
    pthread_mutex_unlock(&p->media_lock);
    return true;
}

bool ap2cl_start_at(struct ap2cl_s *p, uint64_t ntp_start)
{
    if (!p) return false;
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
        p->state = AP2_STREAMING;
        pthread_mutex_lock(&p->mrp_lock);
        if (p->mrp) ap2_mrp_set_playing(p->mrp, true);
        pthread_mutex_unlock(&p->mrp_lock);
        /* Announce the timeline IMMEDIATELY: with a future ntpstart the first
         * audio chunk (and the anchor coupled to it) would otherwise only go
         * out once pacing releases it, and a receiver that sees no time
         * announce shortly after RECORD can abandon the stream. The line is
         * fully determined by ntpstart, so this and the per-chunk announces
         * agree. */
        if (p->use_ptp && !p->use_buffered && p->ctrl_sock >= 0)
            if (ap2_send_sync_packet_ptp(p, true) == AP2_SEND_FATAL)
                return false;
        /* Buffered playback is scheduled entirely by the anchor: map the
         * current RTP sample to the PTP timeline at the requested start
         * instant. A strict receiver 400s the anchor until it has acquired
         * our clock (a second or two of Delay_Req), so retry.
         * NOTE (parked): buffered is only needed for hi-res 24-bit, which
         * among the tested devices only the Apple TV supports — and the Apple
         * TV will not measure our PTP clock on a buffered stream at all, so its
         * anchor never clears. Cracking that needs a capture of iOS -> Apple TV
         * buffered. Sonos does accept the anchor but offers no 24-bit, so
         * buffered has no payoff there. */
        if (p->use_buffered && !p->anchored) {
            uint64_t now_ntp = raopcl_get_ntp(NULL);
            uint64_t lead_ns = 0;
            if (ntp_start > now_ntp) {
                uint64_t d = ntp_start - now_ntp;  /* NTP fixed-point: sec<<32 | frac */
                lead_ns = (d >> 32) * 1000000000ULL +
                          (((d & 0xFFFFFFFFULL) * 1000000000ULL) >> 32);
            }
            bool anchored_ok = false;
            for (int try = 0; try < 12 && !anchored_ok; try++) {
                if (try) usleep(500000);
                uint64_t anchor_ns = ap2_ptp_master_now_ns(p->ptp) + lead_ns;
                anchored_ok = ap2_send_setrateanchortime(p, p->rtp_timestamp,
                                                         anchor_ns, 1);
            }
            p->anchored = true;
        }
        return true;
    }
    if (!p->raopcl) return false;
    int latency = raopcl_latency(p->raopcl);
    raopcl_start_at(p->raopcl, ntp_start - TS2NTP(latency, p->format.sample_rate));
    p->state = AP2_STREAMING;
    return true;
}

ap2_send_result_t ap2cl_send_chunk(struct ap2cl_s *p,
                                   uint8_t *sample, int frames)
{
    if (!p) {
        errno = EINVAL;
        return AP2_SEND_FATAL;
    }
    if (atomic_load(&p->media_transition)) {
        errno = EAGAIN;
        return AP2_SEND_FATAL;
    }

    pthread_mutex_lock(&p->media_lock);
    if (atomic_load(&p->media_transition) ||
        p->state != AP2_STREAMING) {
        pthread_mutex_unlock(&p->media_lock);
        errno = EAGAIN;
        return AP2_SEND_FATAL;
    }

    ap2_send_result_t result;
    if (p->flow == FLOW_NATIVE_AP2) {
        if (atomic_load(&p->rtsp_dead)) {
            pthread_mutex_unlock(&p->media_lock);
            errno = ENOTCONN;
            return AP2_SEND_FATAL;
        }
        result = ap2_native_send_chunk(p, sample, frames);
    } else if (!p->raopcl) {
        pthread_mutex_unlock(&p->media_lock);
        errno = ENOTCONN;
        return AP2_SEND_FATAL;
    } else {
        uint64_t playtime;
        result = raopcl_send_chunk(
                     p->raopcl, sample, frames, &playtime)
                     ? AP2_SEND_SENT : AP2_SEND_FATAL;
        if (result == AP2_SEND_FATAL) errno = EIO;
    }
    pthread_mutex_unlock(&p->media_lock);
    return result;
}

static uint64_t ap2_pacing_window_frames(struct ap2cl_s *p)
{
    return p->dev_latency_max > 11025 ? p->dev_latency_max - 11025 : 77175;
}

bool ap2cl_accept_frames(struct ap2cl_s *p)
{
    if (!p || p->state != AP2_STREAMING) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        /* Buffered pushes over TCP: the blocking write and its send timeout
         * provide flow control, so accept whenever we are streaming and let
         * backpressure pace us (rather than the realtime latency window). */
        if (p->use_buffered) return true;
        /* Pace against the ANCHOR DEADLINE, capped to the receiver's buffer.
         * Contract: frame f is AUDIBLE at its frame-clock position (the anchor
         * line starts one lead early), so f's deadline IS f. A frame delivered
         * more than the receiver's latencyMax before its deadline overflows
         * its buffer and is dropped (Sonos: 88200 = 2.0s) — release at most
         * `window` ahead: the reported window when known, else 77175 (1.75s,
         * inside every AirPlay receiver's standard 2s). Delivery therefore
         * runs up to ~window AHEAD of playback from the very first sample —
         * the receiver's buffer is filled before the scheduled start and the
         * start cannot underrun — while scheduled group starts stay safe no
         * matter how far ahead the start lies. */
        uint64_t now_ntp = raopcl_get_ntp(NULL);
        uint64_t now_ts = NTP2TS(now_ntp, p->format.sample_rate);
        uint64_t window = ap2_pacing_window_frames(p);
        return (now_ts + window) >= p->head_ts;
    }
    if (!p->raopcl) return false;
    return raopcl_accept_frames(p->raopcl);
}

bool ap2cl_recover_input_gap(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->use_buffered ||
        p->state != AP2_STREAMING ||
        atomic_load(&p->media_transition))
        return false;

    pthread_mutex_lock(&p->media_lock);
    if (p->state != AP2_STREAMING ||
        atomic_load(&p->media_transition)) {
        pthread_mutex_unlock(&p->media_lock);
        return false;
    }

    bool recovered = false;
    uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
    uint64_t floor = MS2TS(250, p->format.sample_rate);
    uint64_t window = ap2_pacing_window_frames(p);
    uint64_t latency = MS2TS(p->latency_ms, p->format.sample_rate);
    uint64_t recovery_lead = latency < window ? latency : window;
    ap2_timeline_recovery_t recovery;
    if (!ap2_timeline_plan_recovery(
            p->head_ts, p->rtp_timestamp, atomic_load(&p->rtp_offset),
            now_ts, floor, recovery_lead, &recovery)) {
        if (p->head_ts <= now_ts + floor &&
            (uint32_t)p->head_ts + atomic_load(&p->rtp_offset) !=
                p->rtp_timestamp)
            LOG_ERROR("[AP2] Cannot re-anchor: RTP/head invariant already broken");
        goto done;
    }
    uint64_t shifted_frames = recovery.shifted_frames;
    if ((uint32_t)recovery.head + recovery.offset != p->rtp_timestamp) {
        LOG_ERROR("[AP2] Cannot re-anchor: RTP/head invariant already broken");
        goto done;
    }
    p->head_ts = recovery.head;
    atomic_store(&p->rtp_offset, recovery.offset);
    if (p->use_ptp && p->rt_anchor_valid) {
        uint64_t shift_ns = ap2_timeline_frames_to_ns(
            shifted_frames, p->format.sample_rate);
        p->rt_anchor_wall0 += shift_ns;
    }
    p->timeline_reanchors++;
    ap2_send_result_t sync_result =
        p->use_ptp ? ap2_send_sync_packet_ptp(p, true)
                   : ap2_send_sync_packet(p, true);
    if (sync_result == AP2_SEND_FATAL) goto done;
    LOG_WARN("[AP2] Re-anchored after PCM starvation: shifted_frames=%" PRIu64
             " lead_frames=%" PRIu64 " count=%" PRIu64,
             shifted_frames, recovery_lead, p->timeline_reanchors);
    recovered = true;
done:
    pthread_mutex_unlock(&p->media_lock);
    return recovered;
}

void ap2cl_log_diagnostics(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || *loglevel < lSDEBUG) return;
    uint64_t now_ts = NTP2TS(raopcl_get_ntp(NULL), p->format.sample_rate);
    int64_t pacing_ahead = p->head_ts >= now_ts
                               ? (int64_t)(p->head_ts - now_ts)
                               : -(int64_t)(now_ts - p->head_ts);
    uint64_t window = ap2_pacing_window_frames(p);
    uint64_t ptp_now = p->use_ptp ? ap2_ptp_master_now_ns(p->ptp) : 0;
    uint64_t clock_id = p->use_ptp ? ap2_ptp_master_clock_id(p->ptp) : 0;
    LOG_SDEBUG("[AP2DIAG] state=%d ptp=%d buffered=%d seq=%u rtp=%u "
               "head=%" PRIu64 " pacing_ahead_frames=%" PRId64
               " window_frames=%" PRIu64 " audio_sent=%" PRIu64
               " audio_dropped=%" PRIu64 " sync_sent=%" PRIu64
               " sync_dropped=%" PRIu64 " anchor_valid=%d "
               "anchor_wall=%" PRIu64 " anchor_pos=%u ptp_now=%" PRIu64
               " clock=%016" PRIx64 " reanchors=%" PRIu64
               " rtsp_dead=%d media_healthy=%d event_status=%d pending=%u",
               p->state, p->use_ptp, p->use_buffered, p->seq_number,
               p->rtp_timestamp, p->head_ts, pacing_ahead, window,
               p->audio_packets_sent, p->audio_packets_dropped,
               p->sync_packets_sent, p->sync_packets_dropped,
               p->rt_anchor_valid, p->rt_anchor_wall0, p->rt_anchor_pos0,
               ptp_now, clock_id, p->timeline_reanchors,
               atomic_load(&p->rtsp_dead) ? 1 : 0,
               atomic_load(&p->media_healthy) ? 1 : 0,
               atomic_load(&p->mrp_event_status),
               atomic_load(&p->mrp_pending));
}

static void ap2_mrp_publish_playback_state(
    struct ap2cl_s *p, ap2_state_t client_state,
    ap2_mrp_playback_state_t state)
{
    p->state = client_state;
    pthread_mutex_lock(&p->mrp_lock);
    bool ready = p->mrp != NULL;
    if (ready) {
        if (state == AP2_MRP_PLAYBACK_STOPPED)
            ap2_mrp_set_stopped(p->mrp);
        else
            ap2_mrp_set_playing(
                 p->mrp, state == AP2_MRP_PLAYBACK_PLAYING);
    }
    pthread_mutex_unlock(&p->mrp_lock);
    if (ready)
        atomic_fetch_or(&p->mrp_pending, AP2_MRP_PENDING_PLAYBACK);
}

void ap2cl_pause(struct ap2cl_s *p)
{
    if (!p) return;
    atomic_store(&p->media_transition, true);
    pthread_mutex_lock(&p->media_lock);
    if (p->use_buffered && p->buffered_sock >= 0) {
        /* Freeze the buffered timeline in place with a rate-0 anchor. */
        ap2_send_setrateanchortime(p, p->rtp_timestamp, ap2_ptp_master_now_ns(p->ptp), 0);
        ap2_mrp_publish_playback_state(
            p, AP2_PAUSED, AP2_MRP_PLAYBACK_PAUSED);
    } else {
        if (p->raopcl) {
            raopcl_pause(p->raopcl);
            raopcl_flush(p->raopcl);
        }
        p->rt_anchor_valid = false;    /* resume re-anchors on a fresh line */
        ap2_mrp_publish_playback_state(
            p, AP2_PAUSED, AP2_MRP_PLAYBACK_PAUSED);
    }
    pthread_mutex_unlock(&p->media_lock);
    atomic_store(&p->media_transition, false);
}

void ap2cl_play(struct ap2cl_s *p)
{
    if (!p) return;
    atomic_store(&p->media_transition, true);
    pthread_mutex_lock(&p->media_lock);
    if (p->use_buffered && p->buffered_sock >= 0) {
        /* Re-anchor at rate 1 a short lead ahead to resume the buffered stream. */
        uint64_t anchor_ns = ap2_ptp_master_now_ns(p->ptp) + (uint64_t)p->latency_ms * 1000000ULL;
        ap2_send_setrateanchortime(p, p->rtp_timestamp, anchor_ns, 1);
        ap2_mrp_publish_playback_state(
            p, AP2_STREAMING, AP2_MRP_PLAYBACK_PLAYING);
    } else {
        if (p->raopcl) {
            int lat = raopcl_latency(p->raopcl);
            uint64_t now = raopcl_get_ntp(NULL);
            raopcl_start_at(
                p->raopcl,
                now + MS2NTP(200) -
                    TS2NTP(lat, raopcl_sample_rate(p->raopcl)));
        }
        ap2_mrp_publish_playback_state(
            p, AP2_STREAMING, AP2_MRP_PLAYBACK_PLAYING);
    }
    pthread_mutex_unlock(&p->media_lock);
    atomic_store(&p->media_transition, false);
}

void ap2cl_stop(struct ap2cl_s *p)
{
    if (!p) return;
    atomic_store(&p->media_transition, true);
    pthread_mutex_lock(&p->media_lock);
    if (p->use_buffered && p->buffered_sock >= 0)
        ap2_send_flushbuffered(p);
    if (p->raopcl) raopcl_stop(p->raopcl);
    ap2_mrp_publish_playback_state(
        p, AP2_DOWN, AP2_MRP_PLAYBACK_STOPPED);
    p->rt_anchor_valid = false;
    pthread_mutex_unlock(&p->media_lock);
}

bool ap2cl_feedback(struct ap2cl_s *p)
{
    /* Native flow only: the RAOP-compat flow rides libraop, whose keepalive
     * is raopcl_keepalive(). The response body carries stream status we do
     * not need; the POST itself is what resets the receiver's idle timer. */
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return false;
    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "POST", "/feedback", NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    LOG_DEBUG("[AP2] /feedback keepalive -> %d", status);
    return status == 200;
}

bool ap2cl_control_healthy(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2) return true;
    if (atomic_load(&p->rtsp_dead) ||
        !atomic_load(&p->media_healthy))
        return false;
    return atomic_load(&p->mrp_event_status) != 0;
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

/* Lazily create MediaRemote for pair-verified native sessions and attach the
 * encrypted reverse event channel before advertising any controllable state.
 * Transient-paired third-party speakers never receive these Apple-specific
 * messages. Set CLIAIRPLAY_MRP=0 to disable the path for diagnosis. */
static void ap2_mrp_ready_locked(struct ap2cl_s *p)
{
    if (p->mrp || !p->rtsp_established ||
        p->flow != FLOW_NATIVE_AP2 || !p->auth_credentials ||
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
    if (!ap2_mrp_attach_events(p->mrp, p->events_sock)) {
        LOG_WARN("[MRP] event-channel attach failed; MediaRemote disabled");
        ap2_mrp_destroy(p->mrp);
        p->mrp = NULL;
        atomic_store(&p->mrp_event_status, -1);
        return;
    }
    p->events_sock = -1;
    atomic_store(&p->mrp_event_status, 1);
    ap2_mrp_set_playing(p->mrp, p->state == AP2_STREAMING);
}

static bool ap2_mrp_ensure_ready(struct ap2cl_s *p)
{
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready_locked(p);
    bool ready = p->mrp != NULL;
    pthread_mutex_unlock(&p->mrp_lock);
    return ready;
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
                                const char *tag,
                                uint64_t *artwork_generation)
{
    uint8_t *body = NULL; int body_len = 0;
    pthread_mutex_lock(&p->mrp_lock);
    bool built = p->mrp && builder(p->mrp, &body, &body_len);
    if (built && artwork_generation)
        *artwork_generation = ap2_mrp_artwork_generation(p->mrp);
    pthread_mutex_unlock(&p->mrp_lock);
    if (!built) return -1;
    while (atomic_load(&p->feedback_waiting) &&
           !atomic_load(&p->rtsp_dead) &&
           !atomic_load(&p->workers_stopping))
        usleep(1000);
    uint8_t *resp = NULL;
    int resp_len = 0;
    int status = ap2_rtsp_send(p, "POST", "/command", body, body_len,
                               "application/x-apple-binary-plist", &resp, &resp_len);
    free(body);
    LOG_INFO("[MRP] /command %s -> %d", tag, status);
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
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0)
        return -1;
    if (!force && p->mrp_last_playback_state == state) return 200;

    int status = ap2_mrp_post_command(
        p, ap2_mrp_build_playbackstate_command,
        "[MRP] /command updateMRPlaybackState", NULL);
    if (ap2_mrp_status_ok(status))
        p->mrp_last_playback_state = state;
    return status;
}

/* Apple AirPlaySender sends the extended metadata immediately after the first
 * updateMRNowPlayingInfo: supported commands, explicit playback state, then the
 * serialized NowPlayingClient. A receiver can accept the info plist while
 * keeping it detached from its system now-playing player when these are absent. */
static int ap2_mrp_send_extended_registration(struct ap2cl_s *p)
{
    ap2_mrp_playback_state_t state =
        p->state == AP2_STREAMING ? AP2_MRP_PLAYBACK_PLAYING :
        p->state == AP2_PAUSED ? AP2_MRP_PLAYBACK_PAUSED :
                                 AP2_MRP_PLAYBACK_STOPPED;

    int st_cmd = ap2_mrp_post_command(
        p, ap2_mrp_build_supportedcommands_command,
        "[MRP] /command updateMRSupportedCommands", NULL);
    int st_state = ap2_mrp_send_playback_state(p, state, true);
    int st_client = ap2_mrp_post_command(
        p, ap2_mrp_build_nowplayingclient_command,
        "[MRP] /command updateMRNowPlayingClient", NULL);

    p->mrp_extended_registered =
        ap2_mrp_status_ok(st_cmd) && ap2_mrp_status_ok(st_state) &&
        ap2_mrp_status_ok(st_client);
    LOG_INFO("[MRP] extended metadata: commands=%d playback=%d client=%d",
             st_cmd, st_state, st_client);

    if (!ap2_mrp_status_ok(st_cmd)) return st_cmd;
    if (!ap2_mrp_status_ok(st_state)) return st_state;
    return st_client;
}

static int ap2cl_mrp_push_locked(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return -1;
    if (!ap2_mrp_ensure_ready(p)) return -1;

    if (!p->mrp_device_registered && ap2cl_mrp_register_locked(p) != 1)
        return 0;

    uint64_t artwork_generation = 0;
    int status = ap2_mrp_post_command(
        p, ap2_mrp_build_nowplaying_command,
        "[MRP] /command updateMRNowPlayingInfo", &artwork_generation);
    if (!ap2_mrp_status_ok(status)) return status;
    pthread_mutex_lock(&p->mrp_lock);
    if (p->mrp)
        ap2_mrp_mark_artwork_sent(p->mrp, artwork_generation);
    pthread_mutex_unlock(&p->mrp_lock);

    int ext_status;
    if (!p->mrp_extended_registered) {
        ext_status = ap2_mrp_send_extended_registration(p);
    } else {
        ap2_mrp_playback_state_t state =
            p->state == AP2_STREAMING ? AP2_MRP_PLAYBACK_PLAYING :
            p->state == AP2_PAUSED ? AP2_MRP_PLAYBACK_PAUSED :
                                     AP2_MRP_PLAYBACK_STOPPED;
        ext_status = ap2_mrp_send_playback_state(p, state, false);
    }
    return ap2_mrp_status_ok(ext_status) ? status : ext_status;
}

int ap2cl_mrp_push(struct ap2cl_s *p)
{
    if (!p || !ap2_mrp_ensure_ready(p)) return -1;
    atomic_fetch_or(&p->mrp_pending, AP2_MRP_PENDING_PUSH);
    return 202;
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
static int ap2cl_mrp_register_locked(struct ap2cl_s *p)
{
    if (!p || p->flow != FLOW_NATIVE_AP2 || p->sock_fd < 0) return -1;
    if (!ap2_mrp_ensure_ready(p)) return -1;
    if (p->mrp_device_registered) return 1;

    int status = ap2_mrp_post_command(
        p, ap2_mrp_build_deviceinfo_command,
        "[MRP] /command DEVICE_INFO", NULL);
    p->mrp_device_registered = ap2_mrp_status_ok(status);
    return p->mrp_device_registered ? 1 : 0;
}

int ap2cl_mrp_register(struct ap2cl_s *p)
{
    if (!p || !ap2_mrp_ensure_ready(p)) return -1;
    atomic_fetch_or(&p->mrp_pending, AP2_MRP_PENDING_REGISTER);
    return 202;
}

bool ap2cl_set_metadata(struct ap2cl_s *p, const char *title, const char *artist,
                        const char *album, int duration)
{
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready_locked(p);
    if (p->mrp)
        ap2_mrp_set_metadata(p->mrp, title, artist, album, duration * 1000);
    pthread_mutex_unlock(&p->mrp_lock);
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0)
        return ap2_native_send_metadata(p, title, artist, album);
    if (p->raopcl)
        return raopcl_set_daap(p->raopcl, 4, "minm", 's', title,
                                "asar", 's', artist, "asal", 's', album, "astn", 'i', 1);
    return false;
}

bool ap2cl_set_artwork(struct ap2cl_s *p, const char *content_type, int size, const char *data)
{
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready_locked(p);
    if (p->mrp)
        ap2_mrp_set_artwork(p->mrp, content_type, (const uint8_t *)data, size);
    pthread_mutex_unlock(&p->mrp_lock);
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0) {
        /* Same shape as the RAOP path: the image bytes as the SET_PARAMETER
         * body with its image content type, anchored via RTP-Info (receivers
         * such as Apple TV render it on their now-playing display). */
        char rtpinfo[48];
        snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: rtptime=%u\r\n", p->rtp_timestamp);
        uint8_t *resp = NULL; int resp_len = 0;
        int status = ap2_rtsp_send_ex(p, "SET_PARAMETER", p->session_url,
                                      (const uint8_t *)data, size, content_type,
                                      rtpinfo, &resp, &resp_len);
        free(resp);
        LOG_INFO("[AP2] native artwork SET_PARAMETER -> status %d (%d bytes, %s)",
                 status, size, content_type);
        return status >= 200 && status < 300;
    }
    if (!p->raopcl) return false;
    return raopcl_set_artwork(p->raopcl, (char *)content_type, size, (char *)data);
}

bool ap2cl_set_progress(struct ap2cl_s *p, int elapsed_s, int duration_s)
{
    if (!p) return false;
    pthread_mutex_lock(&p->mrp_lock);
    ap2_mrp_ready_locked(p);
    if (p->mrp)
        ap2_mrp_set_progress(p->mrp, elapsed_s * 1000, duration_s * 1000,
                             p->state == AP2_STREAMING);
    pthread_mutex_unlock(&p->mrp_lock);
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

void ap2cl_latency_info(struct ap2cl_s *p, int *lead_ms, uint32_t *dev_min, uint32_t *dev_max)
{
    if (lead_ms) *lead_ms = p ? p->latency_ms : 0;
    if (dev_min) *dev_min = p ? p->dev_latency_min : 0;
    if (dev_max) *dev_max = p ? p->dev_latency_max : 0;
}

int ap2cl_render_latency_ms(struct ap2cl_s *p) { return p ? p->dev_render_ms : 0; }

ap2_state_t ap2cl_state(struct ap2cl_s *p) { return p ? p->state : AP2_DOWN; }
bool ap2cl_is_connected(struct ap2cl_s *p) { return p && p->state >= AP2_CONNECTED; }
bool ap2cl_is_playing(struct ap2cl_s *p) { return p && p->state == AP2_STREAMING; }
