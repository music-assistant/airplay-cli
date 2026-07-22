/*
 * AirPlay 2 MediaRemote (MRP) SENDER - Header
 *
 * Pushes now-playing state to an Apple device over the AirPlay 2 remote-control
 * data channel (MRP tunnelled over AirPlay 2, the transport tvOS >= 15 uses in
 * place of the retired standalone MRP port). Two goals:
 *   1. make tvOS render the on-screen now-playing UI for our audio stream, the
 *      way it does for an iPhone sender (RTSP SET_PARAMETER metadata is
 *      200-accepted but never drawn);
 *   2. keep the system now-playing session alive so tvOS does not drop to
 *      standby mid-stream.
 *
 * Design and protocol notes live in DESIGN.md §8.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_MRP_H_
#define __AP2_MRP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ap2_remote.h"

struct ap2_mrp_ctx;

/* Receive validated event-channel MediaRemote commands. Set before attaching
 * the event socket; the callback remains fixed for that session. */
void ap2_mrp_set_remote_command_callback(
    struct ap2_mrp_ctx *m, ap2_remote_command_cb_t callback, void *userdata);

typedef enum {
    AP2_MRP_PLAYBACK_PLAYING = 1,
    AP2_MRP_PLAYBACK_PAUSED = 2,
    AP2_MRP_PLAYBACK_STOPPED = 3,
} ap2_mrp_playback_state_t;

/*
 * Internal staging-allocation guard, not an Apple TV receiver limit.
 *
 * The actual tvOS artwork-size cutoff is not yet measured. Keep enough room
 * for the controlled 43-150 KiB hardware matrix while bounding retained local
 * input and binary-plist construction.
 */
#define AP2_MRP_ARTWORK_STAGING_MAX_BYTES (1024 * 1024)

typedef enum {
    AP2_MRP_ARTWORK_NOT_APPLICABLE = 0,
    AP2_MRP_ARTWORK_ACCEPTED,
    AP2_MRP_ARTWORK_INVALID_ARGUMENT,
    AP2_MRP_ARTWORK_UNSUPPORTED_TYPE,
    AP2_MRP_ARTWORK_STAGING_LIMIT,
    AP2_MRP_ARTWORK_INVALID_JPEG_ENVELOPE,
    AP2_MRP_ARTWORK_NO_MEMORY,
} ap2_mrp_artwork_result_t;

typedef struct {
    ap2_mrp_artwork_result_t result;
    size_t bytes;
    uint16_t width;
    uint16_t height;
    uint8_t precision;
    uint8_t components;
    uint8_t sof_marker;
    bool progressive;
} ap2_mrp_artwork_info_t;

/* Remote-control (MRP) data-channel stream SETUP constants (DESIGN.md §8,
 * pyatv ap2_session.py _setup_data_channel). The audio session issues a SETUP
 * carrying a stream of this type on its pair-verified RTSP channel; the
 * receiver answers with a dataPort for ap2_mrp_attach(). Shared here so the
 * SETUP request (ap2_client.c) and the channel code use one definition. */
#define AP2_MRP_STREAM_TYPE_REMOTE_CONTROL  130
#define AP2_MRP_STREAM_CONTROL_TYPE         2
#define AP2_MRP_CLIENT_TYPE_UUID            "1910A70F-DBC0-4242-AF95-115DB30604E1"

/*
 * Create an MRP sender context.
 *
 * Two attachment models (DESIGN.md §8):
 *  - piggyback: the audio session (ap2_client.c) issues the extra stream SETUP
 *    (type 130) on its already-verified RTSP channel and hands the returned
 *    dataPort to ap2_mrp_attach(). The channel keys derive from THAT session's
 *    pair-verify shared secret, passed here as reuse_shared_secret.
 *  - sidecar (metadata-only display mode): ap2_mrp_start() later drives its own
 *    connection + pair-verify + remote-control-only session; not implemented in
 *    the current implementation.
 *
 * :param host: device IP address.
 * :param port: device AirPlay control port (typically 7000).
 * :param auth_credentials: HAP credentials hex (192 chars), same value as the
 *                          audio session's --auth. tvOS requires verified
 *                          credentials; transient PIN-3939 pairing is not
 *                          accepted by Apple TV (DESIGN.md §8).
 * :param dacp_id: DACP identifier; doubles as our stable MRP device identifier
 *                 and (sidecar model) the pair-verify client identity.
 * :param device_name: sender display name to advertise (DEVICE_INFO).
 * :param session_uuid: UUID of the AirPlay audio session this state belongs to.
 * :param group_uuid: AirPlay group UUID advertised in the session SETUP.
 * :param reuse_shared_secret: 32-byte pairing shared secret of the verified
 *                             session the type-130 stream was SETUP on
 *                             (piggyback model). NULL for the sidecar model.
 */
struct ap2_mrp_ctx *ap2_mrp_create(const char *host, int port,
                                    const char *auth_credentials,
                                    const char *dacp_id,
                                    const char *device_name,
                                    const char *session_uuid,
                                    const char *group_uuid,
                                    const uint8_t *reuse_shared_secret);

/* Destroy the context and release all resources. */
void ap2_mrp_destroy(struct ap2_mrp_ctx *m);

/*
 * Attach to a remote-control data channel negotiated by the audio session
 * (piggyback model). Derives the DataStream channel keys from the shared
 * secret given at create time, opens the TCP connection to the receiver's
 * dataPort, and pushes the opening messages (DEVICE_INFO,
 * SET_CONNECTION_STATE, SET_NOW_PLAYING_CLIENT, initial SET_STATE).
 *
 * :param data_port: dataPort from the type-130 stream SETUP response.
 * :param seed: the random seed sent in the type-130 SETUP request (the
 *              DataStream HKDF salt is "DataStream-Salt" + decimal seed).
 * :returns: true once the data channel is established.
 */
bool ap2_mrp_attach(struct ap2_mrp_ctx *m, int data_port, uint64_t seed);

/*
 * Attach the session event socket. Ownership transfers to the MRP context on
 * success. This derives the independent Events-Salt keys and services encrypted
 * reverse HTTP requests with 200 responses from ap2_mrp_tick().
 */
bool ap2_mrp_attach_events(struct ap2_mrp_ctx *m, int event_sock);

/*
 * Bootstrap a standalone remote-control-only session (sidecar model, for the
 * metadata-only display mode): own TCP connection, pair-verify, session SETUP
 * with isRemoteControlOnly=true, event channel, RECORD, type-130 data channel.
 * Not implemented yet — returns false.
 */
bool ap2_mrp_start(struct ap2_mrp_ctx *m);

/* Push SET_CONNECTION_STATE=Disconnected and tear the channel down. */
void ap2_mrp_stop(struct ap2_mrp_ctx *m);

/*
 * Set the now-playing track metadata. Values are copied; NULL is treated as
 * empty. Takes effect on the next feedback-worker state push.
 *
 * :param duration_ms: track duration in milliseconds (0 = unknown/live).
 */
bool ap2_mrp_set_metadata(struct ap2_mrp_ctx *m, const char *title,
                          const char *artist, const char *album,
                          int duration_ms);

/*
 * Set the now-playing artwork.
 *
 * Stages any bounded image/jpeg with a basic SOI/terminal-EOI envelope.
 * Dimensions, precision, SOF marker, components, and progressive shape are
 * best-effort telemetry only and never acceptance criteria. The bound is
 * internal staging/allocation policy; it does not claim a receiver byte,
 * dimension, component, or JPEG-profile limit. Rejected input clears prior
 * MRP artwork so a new track cannot retain stale cover art.
 *
 * :param mime: image MIME type; must be "image/jpeg".
 * :param data: image bytes (copied).
 * :param len: image byte count.
 * :param info: optional probe result and best-effort telemetry.
 */
bool ap2_mrp_set_artwork(struct ap2_mrp_ctx *m, const char *mime,
                         const uint8_t *data, int len,
                         ap2_mrp_artwork_info_t *info);

/* Clear retained artwork (used when a replacement local file cannot load). */
void ap2_mrp_clear_artwork(struct ap2_mrp_ctx *m);

/* Probe the staging envelope and best-effort metadata without mutating state. */
ap2_mrp_artwork_result_t
ap2_mrp_probe_artwork(const char *mime, const uint8_t *data, size_t len,
                      ap2_mrp_artwork_info_t *info);

/* Stable diagnostic token for an artwork result. */
const char *ap2_mrp_artwork_result_name(ap2_mrp_artwork_result_t result);

/*
 * Set playback progress and state.
 *
 * :param elapsed_ms: elapsed position in milliseconds.
 * :param duration_ms: track duration in milliseconds (0 = unknown/live).
 * :param playing: true = Playing, false = Paused.
 */
bool ap2_mrp_set_progress(struct ap2_mrp_ctx *m, int elapsed_ms,
                          int duration_ms, bool playing);

/*
 * Flip the play/pause state without a fresh elapsed position (pause/resume).
 * Advances the stored elapsed to now so the receiver's extrapolated position
 * (elapsedTime + timestamp + playbackRate) is accurate across the transition.
 * Takes effect on the next feedback-worker state push.
 *
 * :param playing: true = Playing, false = Paused.
 */
bool ap2_mrp_set_playing(struct ap2_mrp_ctx *m, bool playing);

/* Mark playback stopped. The current metadata remains available for the final
 * updateMRPlaybackState push before session teardown. */
bool ap2_mrp_set_stopped(struct ap2_mrp_ctx *m);

/*
 * Keep-alive tick: answer encrypted event-channel HTTP requests and drain any
 * type-130 frames (answering sync/heartbeat requests). Call only from the
 * feedback worker.
 */
void ap2_mrp_tick(struct ap2_mrp_ctx *m);

/*
 * Snapshot a pending/periodic type-130 state push while the caller protects
 * mutable MRP state, then send the owned payload without that state lock.
 * The feedback worker is the sole sender for these snapshots.
 */
bool ap2_mrp_prepare_state_push(struct ap2_mrp_ctx *m, uint8_t **out,
                                int *out_len, uint64_t *generation);
bool ap2_mrp_send_state_push(struct ap2_mrp_ctx *m,
                             const uint8_t *data, int len);
void ap2_mrp_complete_state_push(struct ap2_mrp_ctx *m,
                                 uint64_t generation, bool success);

/* True once the data channel is established. */
bool ap2_mrp_is_connected(struct ap2_mrp_ctx *m);

/* Reverse event-channel status: -1 unattached, 0 closed/failed, 1 active. */
int ap2_mrp_event_status(struct ap2_mrp_ctx *m);

/*
 * Build a binary-plist body for POST /command on the MAIN encrypted RTSP
 * channel (push path A in DESIGN.md §8: real iOS senders push MediaRemote
 * now-playing state this way). Uses the current metadata/artwork/progress state.
 *
 * :param out: receives the serialized plist (caller frees).
 * :param out_len: receives the byte count.
 * :returns: true on success.
 */
bool ap2_mrp_build_nowplaying_command(struct ap2_mrp_ctx *m,
                                      uint8_t **out, int *out_len);

/* Mark the one-shot artwork bytes as accepted after a successful POST. */
void ap2_mrp_mark_artwork_sent(struct ap2_mrp_ctx *m);
uint64_t ap2_mrp_artwork_generation(struct ap2_mrp_ctx *m);
void ap2_mrp_mark_artwork_sent_if_generation(
    struct ap2_mrp_ctx *m, uint64_t generation);

/*
 * Build the origin-registration bodies a real iPhone POSTs to /command before
 * pushing now-playing (DESIGN.md §8), for the same encrypted RTSP channel:
 *   - deviceinfo: {params:{data: <MRP DEVICE_INFO protobuf>}} — registers us as
 *     a now-playing origin.
 *   - supportedcommands: {type:"updateMRSupportedCommands", params:{...}} —
 *     declares the transport commands we accept (MRMediaRemoteCommand numbering).
 * Caller frees *out.
 */
bool ap2_mrp_build_deviceinfo_command(struct ap2_mrp_ctx *m,
                                      uint8_t **out, int *out_len);
bool ap2_mrp_build_supportedcommands_command(struct ap2_mrp_ctx *m,
                                             uint8_t **out, int *out_len);

/*
 * Build the remaining MediaRemote extended-metadata commands sent by Apple's
 * AirPlaySender after updateMRNowPlayingInfo:
 *   - updateMRPlaybackState: {params:{mrPlaybackState:<enum>}}
 *   - updateMRNowPlayingClient: a serialized NowPlayingClient protobuf in
 *     {params:{mrNowPlayingClient:<data>}}
 */
bool ap2_mrp_build_playbackstate_command(struct ap2_mrp_ctx *m,
                                         uint8_t **out, int *out_len);
bool ap2_mrp_build_nowplayingclient_command(struct ap2_mrp_ctx *m,
                                            uint8_t **out, int *out_len);

#endif /* __AP2_MRP_H_ */
