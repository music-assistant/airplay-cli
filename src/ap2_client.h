/*
 * AirPlay 2 Client - Header
 *
 * Provides AirPlay 2 (AP2) streaming capability including:
 * - HomeKit (HAP) pair-verify authentication
 * - ChaCha20-Poly1305 encrypted channels (control, timing, audio)
 * - PTP clock synchronization
 * - Buffered audio streaming
 * - 24-bit/48kHz ALAC support
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_CLIENT_H_
#define __AP2_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#include "ap2_io.h"
#include "ap2_mrp.h"

struct ap2cl_s;

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
    bool buffered;      /* native AP2: buffered stream, type 103 (else realtime 96) */
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
 * :param bit_depth: requested output bit depth (informational; hi-res rides
 *                   the realtime stream, not buffered).
 * :param force_native: --ap2-native was given (forces the native AP2 flow).
 * :param force_buffered: --buffered was given (forces buffered + native + PTP).
 * :param ptp_forced: --ptp was given (overrides the SupportsPTP auto-detect).
 * :param ptp_enabled: the value passed to --ptp.
 */
ap2_route_t ap2_resolve_route(ap2_proto_pref_t pref, const char *txt, const char *pw,
                              bool have_credentials, int bit_depth,
                              bool force_native, bool force_buffered,
                              bool ptp_forced, bool ptp_enabled);

/*
 * Create a new AirPlay 2 client context.
 *
 * :param device: Device info (name, hostname, address, port, TXT records).
 * :param format: Audio format (sample rate, bit depth, channels).
 * :param auth: HAP credentials hex string (192 chars) from pairing, or NULL.
 * :param password: Device password, or NULL.
 * :param dacp_id: DACP identifier for remote control.
 * :param active_remote: Active-Remote identifier.
 * :param latency_ms: Output buffer duration in milliseconds.
 * :param volume: Initial volume (0-100), or -1 for no initial set.
 */
struct ap2cl_s *ap2cl_create(
    ap2_device_info_t *device,
    ap2_audio_format_t *format,
    const char *auth,
    const char *password,
    const char *dacp_id,
    const char *active_remote,
    int latency_ms,
    int volume
);

/* Destroy the AP2 client and free all resources. */
bool ap2cl_destroy(struct ap2cl_s *p);

/* Connect to the AirPlay 2 device and establish encrypted session. */
bool ap2cl_connect(struct ap2cl_s *p);

/* Disconnect from the device. */
bool ap2cl_disconnect(struct ap2cl_s *p);

/* Start playback at the given NTP timestamp. */
bool ap2cl_start_at(struct ap2cl_s *p, uint64_t ntp_start);

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

/* Set metadata (DAAP format). */
bool ap2cl_set_metadata(struct ap2cl_s *p, const char *title, const char *artist,
                        const char *album, int duration);

/*
 * Set artwork on the existing DMAP path and, when applicable, stage it for
 * MediaRemote. DMAP receives the original detected image type and bytes even
 * when strict MRP validation rejects them.
 */
bool ap2cl_set_artwork(struct ap2cl_s *p, const char *content_type, int size,
                       const char *data, ap2_mrp_artwork_info_t *mrp_info);

/* Clear only the MediaRemote artwork state. Returns false when MRP is inactive. */
bool ap2cl_clear_mrp_artwork(struct ap2cl_s *p);

/* Set playback progress. */
bool ap2cl_set_progress(struct ap2cl_s *p, int elapsed_s, int duration_s);

/* Push the current now-playing state and any pending MediaRemote extended
 * metadata over POST /command. Enabled for pair-verified native sessions;
 * CLIAIRPLAY_MRP=0 disables it. Returns the HTTP status, 0 on registration
 * failure, or -1 when the push does not apply to this session. */
int ap2cl_mrp_push(struct ap2cl_s *p);

/* Exact HTTP status from updateMRNowPlayingInfo in the most recent push. */
int ap2cl_mrp_last_nowplaying_status(struct ap2cl_s *p);

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

/* Select the buffered audio stream (type 103, RTP over TCP + SETRATEANCHORTIME)
 * for the native AP2 flow. Requires PTP timing; when PTP cannot be established
 * the client falls back to the realtime (type 96) stream. Must be called before
 * ap2cl_connect(). */
void ap2cl_set_buffered(struct ap2cl_s *p, bool enable);

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

/* Device-reported arrival->render latency in ms (0 when unreported). Members
 * reporting it are scheduled earlier by this amount so their acoustic output
 * aligns with the group. */
int ap2cl_render_latency_ms(struct ap2cl_s *p);

/* Get current state. */
ap2_state_t ap2cl_state(struct ap2cl_s *p);

/* Check if connected. */
bool ap2cl_is_connected(struct ap2cl_s *p);

/* Check if playing (has buffered or streaming data). */
bool ap2cl_is_playing(struct ap2cl_s *p);

#endif /* __AP2_CLIENT_H_ */
