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
 * Design and protocol notes live in MRP-DESIGN.md. This is a compiling
 * skeleton: the wire primitives (protobuf, framing, channel crypto) are real;
 * the socket/pair-verify/thread wiring is stubbed for a later pass.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_MRP_H_
#define __AP2_MRP_H_

#include <stdbool.h>
#include <stdint.h>

struct ap2_mrp_ctx;

/* Remote-control (MRP) data-channel stream SETUP constants (MRP-DESIGN.md §2,
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
 * Two attachment models (MRP-DESIGN.md §5):
 *  - piggyback: the audio session (ap2_client.c) issues the extra stream SETUP
 *    (type 130) on its already-verified RTSP channel and hands the returned
 *    dataPort to ap2_mrp_attach(). The channel keys derive from THAT session's
 *    pair-verify shared secret, passed here as reuse_shared_secret.
 *  - sidecar (metadata-only display mode): ap2_mrp_start() later drives its own
 *    connection + pair-verify + remote-control-only session; not implemented in
 *    the skeleton.
 *
 * :param host: device IP address.
 * :param port: device AirPlay control port (typically 7000).
 * :param auth_credentials: HAP credentials hex (192 chars), same value as the
 *                          audio session's --auth. tvOS requires verified
 *                          credentials; transient PIN-3939 pairing is not
 *                          accepted by Apple TV (MRP-DESIGN.md §7).
 * :param dacp_id: DACP identifier; doubles as our stable MRP device identifier
 *                 and (sidecar model) the pair-verify client identity.
 * :param device_name: sender display name to advertise (DEVICE_INFO).
 * :param reuse_shared_secret: 32-byte pairing shared secret of the verified
 *                             session the type-130 stream was SETUP on
 *                             (piggyback model). NULL for the sidecar model.
 */
struct ap2_mrp_ctx *ap2_mrp_create(const char *host, int port,
                                    const char *auth_credentials,
                                    const char *dacp_id,
                                    const char *device_name,
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
 * Bootstrap a standalone remote-control-only session (sidecar model, for the
 * metadata-only display mode): own TCP connection, pair-verify, session SETUP
 * with isRemoteControlOnly=true, event channel, RECORD, type-130 data channel.
 * NOT implemented in the skeleton — returns false; see MRP-DESIGN.md §9.
 */
bool ap2_mrp_start(struct ap2_mrp_ctx *m);

/* Push SET_CONNECTION_STATE=Disconnected and tear the channel down. */
void ap2_mrp_stop(struct ap2_mrp_ctx *m);

/*
 * Set the now-playing track metadata. Values are copied; NULL is treated as
 * empty. Takes effect on the next state push (immediate when connected).
 *
 * :param duration_ms: track duration in milliseconds (0 = unknown/live).
 */
bool ap2_mrp_set_metadata(struct ap2_mrp_ctx *m, const char *title,
                          const char *artist, const char *album,
                          int duration_ms);

/*
 * Set the now-playing artwork.
 *
 * :param mime: image MIME type (e.g. "image/jpeg").
 * :param data: image bytes (copied).
 * :param len: image byte count.
 */
bool ap2_mrp_set_artwork(struct ap2_mrp_ctx *m, const char *mime,
                         const uint8_t *data, int len);

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
 * Takes effect on the next state push (immediate when connected).
 *
 * :param playing: true = Playing, false = Paused.
 */
bool ap2_mrp_set_playing(struct ap2_mrp_ctx *m, bool playing);

/*
 * Keep-alive tick: drain any incoming frames (answering sync/heartbeat
 * requests) and re-push the current now-playing state to hold the system
 * now-playing session open (standby prevention). Call about every 2 s from the
 * caller's existing loop; harmless when not connected.
 */
void ap2_mrp_tick(struct ap2_mrp_ctx *m);

/* True once the data channel is established. */
bool ap2_mrp_is_connected(struct ap2_mrp_ctx *m);

/*
 * Build a binary-plist body for POST /command on the MAIN encrypted RTSP
 * channel (push path A in MRP-DESIGN.md §4: real iOS senders push MediaRemote
 * now-playing state this way; the envelope needs capture confirmation before
 * the wiring pass). Uses the current metadata/artwork/progress state.
 *
 * :param out: receives the serialized plist (caller frees).
 * :param out_len: receives the byte count.
 * :returns: true on success.
 */
bool ap2_mrp_build_nowplaying_command(struct ap2_mrp_ctx *m,
                                      uint8_t **out, int *out_len);

/*
 * Build the origin-registration bodies a real iPhone POSTs to /command before
 * pushing now-playing (MRP-DESIGN.md §10), for the same encrypted RTSP channel:
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

#endif /* __AP2_MRP_H_ */
