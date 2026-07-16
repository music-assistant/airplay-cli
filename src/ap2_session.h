/*
 * AirPlay 2 Native Session - Header
 *
 * Full AP2 session supporting both RAOP-compatible and native AP2 flows.
 * Handles device capability detection, authentication, and stream setup.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_SESSION_H_
#define __AP2_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

/* AP2 device capability flags (from /info features field) */
#define AP2_FEAT_SUPPORTS_BUFFERED_AUDIO  (1ULL << 48)
#define AP2_FEAT_SUPPORTS_PTP             (1ULL << 38)  /* TransientPairing, implies PTP */
#define AP2_FEAT_SUPPORTS_HK_PAIRING      (1ULL << 25)  /* HomeKit pair-setup required */
#define AP2_FEAT_SUPPORTS_SYSTEM_PAIRING  (1ULL << 27)

/* AP2 flow mode - determined from device capabilities */
typedef enum {
    AP2_FLOW_RAOP_COMPAT = 0,   /* auth-setup + RAOP ANNOUNCE/SETUP */
    AP2_FLOW_NATIVE_REALTIME,   /* HAP pair-verify + bplist SETUP + encrypted RTP */
    AP2_FLOW_NATIVE_BUFFERED,   /* HAP pair-verify + bplist SETUP + buffered TCP audio */
} ap2_flow_mode_t;

struct ap2_session;

/* Device info parsed from /info response */
typedef struct {
    uint64_t features;
    char device_id[32];
    char model[64];
    int status_flags;
    bool requires_hap_pairing;    /* Device needs HAP pair-verify with stored creds */
    bool supports_ptp;            /* Device needs PTP instead of NTP */
    bool supports_buffered;       /* Device supports buffered audio (type 103) */
    bool accepts_announce;        /* Device accepts RAOP ANNOUNCE flow */
} ap2_device_caps_t;

/*
 * Create a native AP2 session.
 *
 * :param host: Device IP address.
 * :param port: Device port (typically 7000).
 * :param auth_credentials: HAP credentials hex (192 chars) or NULL.
 * :param dacp_id: DACP identifier.
 * :param active_remote: Active-Remote identifier.
 */
struct ap2_session *ap2_session_create(const char *host, int port,
                                       const char *auth_credentials,
                                       const char *dacp_id,
                                       const char *active_remote);

void ap2_session_destroy(struct ap2_session *s);

/* Connect and probe device capabilities via /info. */
bool ap2_session_connect(struct ap2_session *s);

/* Get detected device capabilities. */
ap2_device_caps_t *ap2_session_get_caps(struct ap2_session *s);

/* Get the detected flow mode. */
ap2_flow_mode_t ap2_session_get_flow(struct ap2_session *s);

/* Perform HAP pair-verify (for native AP2 devices with stored credentials). */
bool ap2_session_pair_verify(struct ap2_session *s);

/* Send encrypted RTSP message. Returns status code. Response body in *resp, *resp_len. */
int ap2_session_rtsp(struct ap2_session *s, const char *method, const char *uri,
                     const uint8_t *body, int body_len, const char *content_type,
                     uint8_t **resp, int *resp_len);

/* Perform session SETUP (binary plist with timing info). */
bool ap2_session_setup(struct ap2_session *s, int timing_port, bool use_ptp);

/* Perform stream SETUP (binary plist with audio format and ports). */
bool ap2_session_setup_stream(struct ap2_session *s,
                               int sample_rate, int bit_depth, int channels,
                               int data_port, int control_port,
                               const uint8_t *shared_key);

/* RECORD - begin streaming. */
bool ap2_session_record(struct ap2_session *s);

/* TEARDOWN - end session. */
bool ap2_session_teardown(struct ap2_session *s);

/* SET_PARAMETER - volume in dB (-144 to 0). */
bool ap2_session_set_volume(struct ap2_session *s, float volume_db);

/* POST /feedback - keepalive. */
bool ap2_session_feedback(struct ap2_session *s);

/* Get negotiated remote ports. */
int ap2_session_get_data_port(struct ap2_session *s);
int ap2_session_get_control_port(struct ap2_session *s);
int ap2_session_get_event_port(struct ap2_session *s);
int ap2_session_get_timing_port(struct ap2_session *s);

/* Get the session URL (rtsp://local_addr/session_id). */
const char *ap2_session_get_url(struct ap2_session *s);

/* Get the underlying socket fd (for HAP-encrypted I/O). */
int ap2_session_get_sock(struct ap2_session *s);

#endif /* __AP2_SESSION_H_ */
