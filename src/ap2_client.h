/*
 * AirPlay 2 Client - Header
 *
 * Provides AirPlay 2 (AP2) streaming capability including:
 * - HomeKit (HAP) pair-verify authentication
 * - ChaCha20-Poly1305 encrypted channels (control, timing, audio)
 * - PTP clock synchronization
 * - 24-bit/48kHz ALAC support
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_CLIENT_H_
#define __AP2_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#include "ap2_io.h"
#include "ap2_mrp.h"
#include "ap2_session.h"

struct ap2cl_s;

typedef struct {
    int overall_status;
    int nowplaying_status;
} ap2_mrp_push_result_t;

static inline ap2_mrp_push_result_t ap2_mrp_push_result_empty(void)
{
    ap2_mrp_push_result_t result;
    result.overall_status = -1;
    result.nowplaying_status = 0;
    return result;
}

/* AP2 session states */
typedef enum {
    AP2_DOWN = 0,
    AP2_CONNECTING,
    AP2_CONNECTED,
    AP2_STREAMING,
    AP2_PAUSED,
} ap2_state_t;

/* AP2 audio format configuration */
typedef struct {
    int sample_rate;    /* 44100 or 48000 */
    int bit_depth;      /* 16 or 24 */
    int channels;       /* 2 (stereo) */
} ap2_audio_format_t;

/* The requested audioFormat and the /info-advertised per-stream format tables
 * (bit masks in the audioFormat bit space). The tables are advisory: hardware
 * both understates and overstates them (DESIGN.md §11), so callers must treat
 * them as evidence, not proof. */
typedef struct {
    uint64_t requested;          /* audioFormat selected for this session */
    uint64_t realtime_formats;   /* advertised audioStream mask */
    uint64_t buffered_formats;   /* advertised bufferStream mask */
    bool realtime_known;
    bool buffered_known;
} ap2_format_capabilities_t;

/* AP2 device info (from mDNS TXT records) */
typedef struct {
    char *name;         /* Device display name */
    char *hostname;     /* Device hostname */
    char *address;      /* Device IP address */
    int port;           /* Device port */
    char *txt_records;  /* mDNS TXT records as "key=value key=value ..." */
} ap2_device_info_t;

/* User protocol preference (from --protocol). AUTO derives the route from the
 * mDNS TXT; RAOP/AIRPLAY2 force the protocol. */
typedef enum {
    AP2_PROTO_AUTO = 0,
    AP2_PROTO_RAOP,
    AP2_PROTO_AIRPLAY2,
} ap2_proto_pref_t;

/* Resolved streaming route: the concrete decision the caller acts on. */
typedef struct {
    bool use_raop;      /* true => legacy RAOP (libraop); false => AirPlay 2 */
    bool native;        /* AirPlay 2: native flow (else RAOP-compatible) */
    bool transient;     /* native AP2: transient pairing (else pair-verify) */
    bool ptp;           /* native AP2: PTP grandmaster timing (else NTP) */
    uint64_t features;  /* parsed mDNS features bitmask (for logging) */
    uint64_t flags;     /* parsed mDNS status flags bitmask (for logging) */
    const char *reason; /* short human-readable summary of the decision */
} ap2_route_t;

/* Parse the 64-bit features bitmask from a TXT blob: features=0xLOW,0xHIGH (or
 * ft=) => (HIGH<<32)|LOW. Returns 0 when absent/unparseable. */
uint64_t ap2_txt_features(const char *txt);

/* Parse the 64-bit status flags bitmask from a TXT blob (flags=/sf=), or 0. */
uint64_t ap2_txt_flags(const char *txt);

/*
 * Resolve the streaming route from the discovery TXT and the caller's overrides.
 *
 * :param pref: user --protocol preference (AUTO derives from TXT).
 * :param txt: full _airplay._tcp TXT ("key=value key=value ...") or NULL.
 * :param pw: mDNS pw field ("true" when a device password is required), or NULL.
 * :param have_credentials: stored HAP credentials are available (--auth).
 * :param have_password: a non-empty device password was supplied (--password).
 * :param bit_depth: requested output bit depth (informational; hi-res rides
 *                   the realtime stream).
 * :param force_native: --ap2-native was given (forces the native AP2 flow).
 * :param ptp_forced: --ptp was given (overrides the SupportsPTP auto-detect).
 * :param ptp_enabled: the value passed to --ptp.
 */
ap2_route_t ap2_resolve_route(ap2_proto_pref_t pref, const char *txt, const char *pw,
                              bool have_credentials, bool have_password,
                              int bit_depth, bool force_native,
                              bool ptp_forced, bool ptp_enabled);

/* Why the last connect attempt failed, for the caller's structured report. */
typedef enum {
    AP2_CONNECT_ERROR_NONE = 0,
    AP2_CONNECT_ERROR_GENERIC,        /* anything not auth-related */
    AP2_CONNECT_ERROR_AUTH_REQUIRED,  /* the device wants a password we lack */
    AP2_CONNECT_ERROR_AUTH_FAILED,    /* the device rejected what we presented */
} ap2_connect_error_t;

/*
 * Outcome of the last ap2cl_connect().
 *
 * :param p: client context.
 * :param http_status: receives the most informative HTTP status seen (0 when
 *                     none applies), or NULL.
 * :param detail: receives a short single-line description owned by the client,
 *                or NULL.
 */
ap2_connect_error_t ap2cl_connect_error(struct ap2cl_s *p, int *http_status,
                                        const char **detail);

/*
 * Create a new AirPlay 2 client context.
 *
 * :param device: Device info (name, hostname, address, port, TXT records).
 * :param format: Audio format (sample rate, bit depth, channels).
 * :param auth: HAP credentials hex string (192 chars) from pairing, or NULL.
 * :param password: Device password, or NULL.
 * :param dacp_id: DACP identifier for remote control.
 * :param active_remote: Active-Remote identifier.
 * :param lead_ms: Anchor-to-render lead in milliseconds.
 * :param volume: Initial volume (0-100), or -1 for no initial set.
 */
struct ap2cl_s *ap2cl_create(
    ap2_device_info_t *device,
    ap2_audio_format_t *format,
    const char *auth,
    const char *password,
    const char *dacp_id,
    const char *active_remote,
    int lead_ms,
    int volume
);

/* Destroy the AP2 client and free all resources. */
bool ap2cl_destroy(struct ap2cl_s *p);

/* Connect to the AirPlay 2 device and establish encrypted session. */
bool ap2cl_connect(struct ap2cl_s *p);

/* Disconnect from the device. */
bool ap2cl_disconnect(struct ap2cl_s *p);

/* Start playback at a commanded unix-epoch millisecond instant. A feasible
 * instant is scheduled exactly; an infeasible nonzero one is corrected
 * forward to the earliest feasible instant plus one lead of retry slack (the
 * floor moves with the wall clock), and 0 takes the earliest instant
 * directly. *at_unix_ms always receives the true scheduled instant so the
 * caller can verify and log any correction. */
ap2_commit_result_t ap2cl_start(struct ap2cl_s *p, uint64_t start_unix_ms,
                                uint64_t *at_unix_ms);

/*
 * Warm-seek a live session in place. FLUSH then START (below) are the two
 * halves of the old combined warm flush:
 *
 * ap2cl_flush discards the receiver's buffered audio (RTSP FLUSH / libraop
 * flush) and stops sending, keeping the session and its sequence/nonce state.
 * (Splice timeline: no receiver flush; the queue plays out instead.)
 *
 * ap2cl_resume schedules the next pending sample audible at start_unix_ms
 * under the same contract as ap2cl_start: the splice timeline pads silence up
 * to the instant, the stock path re-bases the frozen anchor onto it, and an
 * infeasible nonzero instant is corrected forward to the earliest feasible
 * one (the splice head, or now + the minimum warm lead) plus one lead of
 * retry slack, with the truth reported in *at_unix_ms — never silently
 * misplaced or refused. A start of 0 picks the earliest instant directly.
 */
bool ap2cl_flush(struct ap2cl_s *p);
ap2_commit_result_t ap2cl_resume(struct ap2cl_s *p, uint64_t start_unix_ms,
                                 uint64_t *at_unix_ms);

/* Park the stream but keep the session warm: the splice timeline stops the
 * content while the armed line keeps carrying silence (an underrun while
 * armed is an audible noise trigger); the stock path discards buffered audio
 * and drops to CONNECTED awaiting the next warm flush. Both publish the
 * stopped playback state. */
void ap2cl_standby(struct ap2cl_s *p);

/* Send a chunk of PCM audio data.
 * A transient realtime UDP drop advances the media timeline and returns
 * AP2_SEND_DROPPED; hard encode/control/session failures return AP2_SEND_FATAL.
 * :param sample: Raw PCM audio bytes (interleaved, little-endian).
 * :param frames: Number of audio frames in the sample buffer.
 */
ap2_send_result_t ap2cl_send_chunk(struct ap2cl_s *p, uint8_t *sample,
                                   int frames);

/* Check if the client is ready to accept more audio frames. */
bool ap2cl_accept_frames(struct ap2cl_s *p);

/* Re-anchor a realtime stream after PCM starvation exhausts its pacing lead. */
bool ap2cl_recover_input_gap(struct ap2cl_s *p);

/* Splice-pad a realtime stream whose delivery head lapsed behind the wall
 * clock (a delivery stall longer than the pacing depth, with input still
 * queued) so the loop never sends frames on past timestamps. Splice timeline
 * only; call before reading each chunk. Returns true when it padded. */
bool ap2cl_recover_delivery_gap(struct ap2cl_s *p);

/* Outcome of one clock-verification poll (see ap2cl_clock_verify_poll). */
typedef enum {
    AP2_CLOCK_VERIFY_IDLE = 0,     /* nothing armed, or nothing new yet */
    AP2_CLOCK_VERIFY_VERIFIED,     /* receiver clock ready before the anchor */
    AP2_CLOCK_VERIFY_CORRECTED,    /* anchor moved forward to readiness */
    AP2_CLOCK_VERIFY_UNVERIFIED,   /* window closed without a probe streak */
} ap2_clock_verify_result_t;

typedef struct {
    uint64_t requested_unix_ms;    /* instant commanded on the START (0 = picked) */
    uint64_t from_unix_ms;         /* previously scheduled audible instant */
    uint64_t at_unix_ms;           /* now-scheduled audible instant */
    int64_t margin_ms;             /* readiness margin before the anchor (verified) */
    uint64_t content_cut_ms;       /* content advanced past the correction so the
                                      join still lands on the group timeline */
    bool start_ack;                /* this event carries the join's withheld
                                      [STATUS] started (see
                                      ap2cl_start_ack_deferred); then no content
                                      was cut, the two being exclusive */
} ap2_clock_verify_event_t;

/*
 * Advance the clock verification of a start that was committed before the
 * receiver's first timing probe (a freshly connected receiver takes about a
 * second to start probing, so a prompt start commits ahead of it). Armed
 * automatically by such
 * commits; the audio loop calls this every iteration under its send lock and
 * reports VERIFIED/CORRECTED events on the status channel. On a join-marked
 * start (ap2cl_set_start_join) a correction moves the still-unsent anchor
 * line forward to the receiver's verified readiness instant, so *ev carries
 * the new truth for the caller's ack; other starts only observe and report.
 */
ap2_clock_verify_result_t ap2cl_clock_verify_poll(struct ap2cl_s *p,
                                                  ap2_clock_verify_event_t *ev);

/* One sample of the receiver's probe streak: the rate at which the client
 * refreshes it, and at which a caller reporting readiness should sample. */
#define AP2_CLOCK_VERIFY_POLL_MS     250

/* How far the receiver's clock has come since the session connected. */
typedef enum {
    AP2_CLOCK_COLD = 0,   /* no timing probe recorded yet */
    AP2_CLOCK_PROBING,    /* probing; readiness is projected, not reached */
    AP2_CLOCK_READY,      /* the projected readiness instant has passed */
    AP2_CLOCK_STALLED,    /* still no probe long past when one was due */
} ap2_clock_state_t;

typedef struct {
    ap2_clock_state_t state;
    uint64_t streak_ms;        /* age of the streak's first probe */
    uint32_t exchanges;        /* probes in the current uninterrupted streak */
    uint64_t ready_in_ms;      /* 0 once ready; meaningless while cold */
    uint64_t ready_at_unix_ms; /* projected readiness instant; 0 while cold */
} ap2_clock_readiness_t;

/* True when the session's EFFECTIVE timing is PTP. False for NTP-timed
 * sessions — including a PTP session that fell back because 319/320 could not
 * be bound — where clock readiness is not measurable. */
bool ap2cl_uses_ptp(struct ap2cl_s *p);

/*
 * Sample the receiver's clock readiness, so a caller can pick a join anchor it
 * can actually meet instead of guessing a headroom. A receiver begins probing
 * our clock about a second after it connects, on its own schedule and without
 * an anchor having been announced; readiness follows from the probe streak
 * (see ap2_clock_ready_from). A PTP session with nothing to measure for
 * AP2_CLOCK_STALL_MS — counted from the last streak this call saw, or from
 * connect while it has seen none — reports AP2_CLOCK_STALLED: that receiver is
 * not slaved to our clock, so it can seat no render position and stays silent
 * however cleanly the audio paces. Under the shared daemon the snapshot poller
 * keeps that mark current for the whole session; the in-process engine has no
 * poller, so the mark only moves while a caller is asking. A caller that stops
 * asking must therefore call ap2cl_clock_watch_restart before it resumes, or
 * the first reading afterwards measures a window nobody was watching.
 * Stalled is not terminal — a receiver that begins probing late reports
 * probing and then ready as usual. Safe to call from the audio loop: it never
 * blocks on the network. Recompute per use — ready_at_unix_ms is a projection
 * against the wall clock and must not be cached.
 */
void ap2cl_clock_readiness(struct ap2cl_s *p, ap2_clock_readiness_t *out);
/*
 * Restart the stall window at now, so the receiver gets the full
 * AP2_CLOCK_STALL_MS grace from the moment readiness reporting resumes.
 * Call it wherever reporting re-arms after having gone terminal: the mark
 * left behind is as old as the quiet stretch, and measuring a stall against
 * it would condemn a healthy receiver on its first reading.
 */
void ap2cl_clock_watch_restart(struct ap2cl_s *p);

/* True while a cold-clock verification can still produce a result. It goes
 * false on the resolving poll and on every disarm (commit, flush, park,
 * teardown), which is how a caller holding a withheld ack knows the
 * verification will never answer it and emits it itself. */
bool ap2cl_clock_verify_armed(struct ap2cl_s *p);

/* True when the commit just made withheld the join's [STATUS] started for its
 * verification to answer: a cold-clock join committed through ap2cl_start can
 * still have its anchor moved, and an ack carrying the moved instant lets the
 * caller map its content straight onto it — which is why no content cut is
 * taken on that path. Cleared by the resolving poll and by every disarm. */
bool ap2cl_start_ack_deferred(struct ap2cl_s *p);

/* Mark the next commanded start as a late join onto an already-live group
 * timeline: receiver clock readiness is then ENFORCED — a cold-clock anchor
 * the receiver cannot be ready for is corrected forward once the probe
 * streak appears. Cleared by every commit. A group/solo ORIGIN start must
 * not set it: its receivers self-seat a fresh session cleanly, and moving
 * one member of a group origin would desync the group. */
void ap2cl_set_start_join(struct ap2cl_s *p, bool join);

/* Emit native pacing, packet-drop, and anchor diagnostics at debug level 10. */
void ap2cl_log_diagnostics(struct ap2cl_s *p);

/* False after terminal RTSP/media/event failures. */
bool ap2cl_control_healthy(struct ap2cl_s *p);

/* Pause playback. */
void ap2cl_pause(struct ap2cl_s *p);

/* Resume playback. */
void ap2cl_play(struct ap2cl_s *p);

/* Stop playback. */
void ap2cl_stop(struct ap2cl_s *p);

/* Session keepalive: POST /feedback over the encrypted RTSP channel, as real
 * Apple senders do every ~2 s (long sessions can otherwise hit receiver-side
 * idle timeouts). Native AP2 flow only; a no-op returning false otherwise.
 * Returns true when the receiver answered 200. */
bool ap2cl_feedback(struct ap2cl_s *p);

/* Set volume (0-100). */
bool ap2cl_set_volume(struct ap2cl_s *p, int volume);

/*
 * Set the track metadata and artwork as one bundle: DMAP metadata (and, when
 * artwork is given, DMAP artwork) on the transport in use, plus exactly one
 * serialized MediaRemote replace push carrying item, timeline and artwork
 * together — the shape a real Apple sender emits at a track change. item_id
 * is the sender's stable per-track identity ("" = none): MediaRemote keys
 * its now-playing item on it, so later tag refinements for the same item
 * update in place instead of presenting as a new track. Byte-identical
 * artwork keeps its ArtworkIdentifier across a track change; NULL artwork
 * drops retained MRP artwork when the track changed. mrp_info and mrp_push
 * receive the request-scoped artwork verdict and push statuses.
 */
bool ap2cl_set_metadata_ex(struct ap2cl_s *p, const char *title,
                           const char *artist, const char *album,
                           int duration, const char *item_id,
                           const char *content_type, const uint8_t *art_data,
                           int art_len, ap2_mrp_artwork_info_t *mrp_info,
                           ap2_mrp_push_result_t *mrp_push);

/* ap2cl_set_metadata_ex without artwork or result reporting. */
bool ap2cl_set_metadata(struct ap2cl_s *p, const char *title, const char *artist,
                        const char *album, int duration, const char *item_id);

/*
 * Set artwork on the existing DMAP path and, when applicable, stage it for
 * MediaRemote. DMAP receives the original detected image type and bytes even
 * when the bounded MRP MIME/envelope probe rejects them. Staging and the
 * complete MRP push are serialized; mrp_push receives request-scoped statuses.
 */
bool ap2cl_set_artwork(struct ap2cl_s *p, const char *content_type, int size,
                       const char *data, ap2_mrp_artwork_info_t *mrp_info,
                       ap2_mrp_push_result_t *mrp_push);

/* Clear only MRP artwork and push that state as one serialized operation. */
bool ap2cl_clear_mrp_artwork(struct ap2cl_s *p,
                             ap2_mrp_push_result_t *mrp_push);

/* Set playback progress. */
bool ap2cl_set_progress(struct ap2cl_s *p, int elapsed_s, int duration_s);

/* Push the current now-playing state and any pending MediaRemote extended
 * metadata over POST /command. Enabled for pair-verified native sessions;
 * CLIAIRPLAY_MRP=0 disables it. Returns the HTTP status, 0 on registration
 * failure, or -1 when the push does not apply to this session. */
int ap2cl_mrp_push(struct ap2cl_s *p);

/* Same, but with the timeline-correction body shape (see
 * ap2_mrp_build_nowplaying_progress_command) for seek progress updates. */
int ap2cl_mrp_push_progress(struct ap2cl_s *p);

/* Serialized push with request-scoped overall and now-playing statuses. */
ap2_mrp_push_result_t ap2cl_mrp_push_ex(struct ap2cl_s *p);

/* Status of the type-130 MRP data channel (path B) for the [STATUS] mrp line:
 * -1 = not attempted (non-Apple / not pair-verified), 0 = attempted but not up,
 * 1 = channel established. */
int ap2cl_mrp_channel_status(struct ap2cl_s *p);

/* Perform the /command DEVICE_INFO origin registration before now-playing.
 * Extended metadata is sent immediately after the first now-playing update.
 * Returns 1 (2xx), 0, or -1 (not applicable). */
int ap2cl_mrp_register(struct ap2cl_s *p);

/* Set RAOP-compatible properties (mDNS fields, interface, credentials).
 * Must be called before ap2cl_connect(). */
void ap2cl_set_raop_props(struct ap2cl_s *p,
                           const char *iface, const char *secret,
                           const char *et, const char *md, const char *am);

/* Force the native AP2 flow even without stored credentials (uses transient
 * pairing). Must be called before ap2cl_connect(). */
void ap2cl_force_native(struct ap2cl_s *p);

/* Force PTP (IEEE-1588) timing for the native AP2 flow on or off, overriding
 * the automatic SupportsPTP (features bit 41) detection. Must be called before
 * ap2cl_connect(). When PTP is selected but 319/320 cannot be bound, the client
 * falls back to NTP timing. */
void ap2cl_set_ptp(struct ap2cl_s *p, bool enable);

/*
 * Override the splice-timeline receiver queue depth (default 600 ms). The
 * depth is the audible latency of every warm seek/next, but receivers that
 * feed a native multiroom pipeline (LinkPlay masters) starve below ~1 s of
 * queued audio and render silence, so their caller asks for more. Values
 * <= 0 are ignored; the effective depth stays capped by the receiver's
 * reported (or assumed) buffer window.
 */
void ap2cl_set_splice_depth_ms(struct ap2cl_s *p, int ms);

/* Prefer a shared PTP daemon clock (multi-room) for the native AP2 flow. When
 * enabled and a live `cliairplay --ptp-daemon` is present on this host, the
 * client reads the elected clock from shared memory and does NOT bind UDP
 * 319/320 or run its own engine; it registers its receiver IP with the daemon.
 * With no live daemon it falls back to the in-process engine. Must be called
 * before ap2cl_connect(). */
void ap2cl_set_ptp_shared(struct ap2cl_s *p, bool enable);

/* Set the address we advertise to the device (multi-homed hosts), used
 * wherever the protocol carries our own address (e.g. timing peer lists).
 * Defaults to the bind/source address when unset. */
void ap2cl_set_publish_ip(struct ap2cl_s *p, const char *ip);

/* Return the requested format and the /info-advertised format capabilities
 * (populated by the native flow's GET /info; zeroed otherwise). */
void ap2cl_format_capabilities(struct ap2cl_s *p,
                               ap2_format_capabilities_t *caps);

/* Set the callback for validated receiver MediaRemote commands. Must be called
 * before ap2cl_connect(); the callback remains fixed for that session. */
void ap2cl_set_remote_command_callback(
    struct ap2cl_s *p, ap2_remote_command_cb_t callback, void *userdata);

/*
 * Get the effective playback lead and the receiver-reported buffering window.
 *
 * :param p: client context.
 * :param lead_ms: receives the effective lead in ms (config clamped into the
 *                 device window), or NULL.
 * :param dev_min: receives the device's latencyMin in frames (0 = unreported), or NULL.
 * :param dev_max: receives the device's latencyMax in frames (0 = unreported), or NULL.
 */
void ap2cl_latency_info(struct ap2cl_s *p, int *lead_ms, uint32_t *dev_min, uint32_t *dev_max);

/* Device-reported arrival->render latency in ms (0 when unreported). Reported
 * for diagnostics on the [STATUS] latency line and never applied to a schedule:
 * receivers already self-compensate their own render latency. */
int ap2cl_render_latency_ms(struct ap2cl_s *p);

/* Frames between the delivery head and the audible position (elapsed
 * reporting): the shallow pacing depth on the splice timeline, the
 * latency lead otherwise. */
uint32_t ap2cl_audible_lag_frames(struct ap2cl_s *p);

/* Minimum lead (ms) a warm commanded START needs for exact placement — the
 * splice timeline's queue depth, 0 when the stock warm path has no
 * constraint. Surfaced on the [STATUS] latency line for the caller. */
int ap2cl_warm_lead_ms(struct ap2cl_s *p);

/* Audible instant (unix ms) of the current delivery head on the splice
 * timeline, carried on the flush ack so the caller anchors warm starts beyond
 * every member's queued audio. 0 = no constraint (not on the splice path). */
uint64_t ap2cl_splice_head_unix_ms(struct ap2cl_s *p);

/* Silence frames still owed to the wire before the next real sample (splice
 * timeline): the audio loop prepends them as ordinary encoded chunks so the
 * bitstream stays contiguous, and consumes the pad as it sends. */
uint32_t ap2cl_splice_pad_frames(struct ap2cl_s *p);
void ap2cl_splice_pad_consume(struct ap2cl_s *p, uint32_t frames);

/* Input bytes still to be discarded from the head of the queued content
 * (a corrected late join advances the content by exactly the correction, so
 * the member's first retained sample is the one the group timeline schedules
 * at the corrected instant): the audio loop drops them from its session
 * reads before sending, consuming as it goes. */
uint32_t ap2cl_content_skip_bytes(struct ap2cl_s *p);
void ap2cl_content_skip_consume(struct ap2cl_s *p, uint32_t bytes);

/* True while the splice timeline is live on the wire (audio sends legal);
 * gates the idle-window silence keepalive in the audio loop. */
bool ap2cl_splice_hot(struct ap2cl_s *p);

/* Get current state. */
ap2_state_t ap2cl_state(struct ap2cl_s *p);

/* Check if connected. */
bool ap2cl_is_connected(struct ap2cl_s *p);

/* Check if playing (has buffered or streaming data). */
bool ap2cl_is_playing(struct ap2cl_s *p);

#endif /* __AP2_CLIENT_H_ */
