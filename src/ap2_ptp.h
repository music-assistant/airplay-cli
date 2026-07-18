/*
 * AirPlay 2 PTP - Header
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_PTP_H_
#define __AP2_PTP_H_

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

struct ap2_ptp_ctx;

/* Create PTP/timing context. */
struct ap2_ptp_ctx *ap2_ptp_create(void);

/* Destroy PTP context. */
void ap2_ptp_destroy(struct ap2_ptp_ctx *ctx);

/*
 * Set PTP-to-local clock offset in nanoseconds.
 * Provided by the MA provider which runs a centralized PTP client.
 * Positive offset means PTP clock is ahead of local clock.
 */
void ap2_ptp_set_offset(struct ap2_ptp_ctx *ctx, int64_t offset_ns);

/*
 * Start NTP timing responder on a UDP socket.
 * Responds to NTP timing requests from the AirPlay device.
 * Applies PTP offset if set.
 *
 * :param device_ip: IP address of the AirPlay device (for logging).
 * :returns: true on success.
 */
bool ap2_ptp_start(struct ap2_ptp_ctx *ctx, const char *device_ip);

/* Stop timing responder. */
void ap2_ptp_stop(struct ap2_ptp_ctx *ctx);

/* Get the UDP port the timing responder is listening on. */
int ap2_ptp_get_timing_port(struct ap2_ptp_ctx *ctx);

/* Get current time as NTP 64-bit timestamp, adjusted by PTP offset. */
uint64_t ap2_ptp_get_time(struct ap2_ptp_ctx *ctx);

/* Convert a local NTP timestamp to the device's PTP clock domain. */
uint64_t ap2_ptp_local_to_device(struct ap2_ptp_ctx *ctx, uint64_t local_ntp);

/* ---- PTP grandmaster engine (IEEE 1588 two-step master) ---- */

/*
 * Set the 64-bit PTP clock identity this engine advertises. Must be called
 * before ap2_ptp_engine_start() so the identity in the emitted PTP messages
 * matches the ClockID advertised in the session SETUP timingPeerInfo. When
 * unset, the engine derives an EUI-64 identity from the host MAC address.
 */
void ap2_ptp_set_clock_id(struct ap2_ptp_ctx *ctx, uint64_t clock_id);

/* The 64-bit PTP clock identity used by the engine (grandmasterIdentity). */
uint64_t ap2_ptp_clock_id(struct ap2_ptp_ctx *ctx);

/*
 * Set the timing peer IP list (typically [receiver_ip, our_ip]) learned from
 * SETPEERS / timingPeerInfo. Used for logging and, when unicast mirroring is
 * enabled, as additional unicast destinations. Strings are copied.
 */
void ap2_ptp_set_peers(struct ap2_ptp_ctx *ctx, const char *const *ips, int count);

/*
 * Start the PTP grandmaster engine: bind UDP 319 (event) and 320 (general) and
 * spawn a thread that emits Announce/Sync/Follow_Up on the PTP multicast group
 * and answers Delay_Req with Delay_Resp.
 *
 * :param bind_addr: local interface to join/egress multicast on (INADDR_ANY for default).
 * :param device_ip: receiver IP (for logging).
 * :returns: true on success; false if 319/320 cannot be bound (e.g. lacking
 *           privilege) — the caller should then fall back to NTP timing.
 */
bool ap2_ptp_engine_start(struct ap2_ptp_ctx *ctx, struct in_addr bind_addr,
                          const char *device_ip);

/* True if the PTP grandmaster engine is running (vs the NTP responder). */
bool ap2_ptp_engine_active(struct ap2_ptp_ctx *ctx);

/*
 * Current PTP master time in nanoseconds. This is the timebase advertised via
 * the PTP Sync/Follow_Up messages; the realtime sync (control) packets must
 * carry wall-clock timestamps from this same source.
 */
uint64_t ap2_ptp_now_ns(struct ap2_ptp_ctx *ctx);

#endif /* __AP2_PTP_H_ */
