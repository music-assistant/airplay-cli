/*
 * AirPlay 2 RTSP - Header
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_RTSP_H_
#define __AP2_RTSP_H_

#include <stdbool.h>
#include <stdint.h>

struct ap2_rtsp_ctx;

/* Create RTSP session context. */
struct ap2_rtsp_ctx *ap2_rtsp_create(const char *host, int port,
                                      const char *auth_credentials,
                                      const char *dacp_id,
                                      const char *active_remote,
                                      int latency_ms);

/* Destroy RTSP session. */
void ap2_rtsp_destroy(struct ap2_rtsp_ctx *ctx);

/* Connect to device: TCP + /info + pair-verify. */
bool ap2_rtsp_connect(struct ap2_rtsp_ctx *ctx);

/* SETUP session (timing protocol negotiation). */
bool ap2_rtsp_setup_session(struct ap2_rtsp_ctx *ctx, bool use_ptp);

/* SETUP audio stream (format, encryption key, ports). */
bool ap2_rtsp_setup_stream(struct ap2_rtsp_ctx *ctx, int sample_rate,
                            int bit_depth, int channels);

/* RECORD - begin audio streaming. */
bool ap2_rtsp_record(struct ap2_rtsp_ctx *ctx);

/* SET_PARAMETER - set volume (dB: -144 to 0). */
bool ap2_rtsp_set_volume(struct ap2_rtsp_ctx *ctx, float volume_db);

/* TEARDOWN - end session. */
bool ap2_rtsp_teardown(struct ap2_rtsp_ctx *ctx);

/* POST /feedback - keepalive. */
bool ap2_rtsp_feedback(struct ap2_rtsp_ctx *ctx);

/* Getters for session-negotiated values */
int ap2_rtsp_get_data_port(struct ap2_rtsp_ctx *ctx);
int ap2_rtsp_get_control_port(struct ap2_rtsp_ctx *ctx);
int ap2_rtsp_get_timing_port(struct ap2_rtsp_ctx *ctx);
const uint8_t *ap2_rtsp_get_audio_key(struct ap2_rtsp_ctx *ctx);

#endif /* __AP2_RTSP_H_ */
