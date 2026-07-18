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
 * Block for up to timeout_ms while the engine listens for peer Announce
 * messages and runs BMCA, so the elected grandmaster is resolved before the
 * caller builds the PTP session SETUP. Returns early once a peer has been seen
 * and a coherent decision reached (we won, or we are slaving with an offset);
 * otherwise it waits out the timeout and leaves us as grandmaster (the default
 * when no peer competes). No-op if the engine is not active.
 */
void ap2_ptp_engine_settle(struct ap2_ptp_ctx *ctx, int timeout_ms);

/*
 * Local host time in nanoseconds (CLOCK_REALTIME). When we are the elected
 * grandmaster this IS the master timebase; the GM Announce/Sync/Follow_Up are
 * stamped from here.
 */
uint64_t ap2_ptp_now_ns(struct ap2_ptp_ctx *ctx);

/*
 * The clock identity of the currently elected grandmaster: our own clockID
 * when we win BMCA, otherwise the winning peer's grandmasterIdentity. This is
 * the timeline the media anchor (SETRATEANCHORTIME networkTimeTimelineID and
 * the realtime sync-packet clock id) must be expressed against.
 */
uint64_t ap2_ptp_master_clock_id(struct ap2_ptp_ctx *ctx);

/*
 * Current time in the elected grandmaster's clock domain, in nanoseconds:
 * local now plus the smoothed local->master offset. Equal to ap2_ptp_now_ns()
 * when we are the grandmaster (offset 0); maps local time into the peer's clock
 * when slaving. Wall-clock timestamps on the wire (sync packets, anchor) must
 * come from here so the receiver schedules against the clock it actually drives.
 *
 * When the context is attached to a shared daemon clock (ap2_ptp_attach_shared),
 * both master getters read the elected clock from shared memory instead of the
 * in-process engine, so ap2_client.c is oblivious to where the clock lives.
 */
uint64_t ap2_ptp_master_now_ns(struct ap2_ptp_ctx *ctx);

/* ---- shared daemon clock (multi-room: one engine, many streams) ---- */

/*
 * Attach this context to the clock published by `cliairplay --ptp-daemon` on
 * this host. Confirms a LIVE daemon (control-channel ping) before mapping the
 * shm read-only, so a stale shm from a crashed daemon does not fool us. When it
 * returns true the caller MUST NOT start the in-process engine or the NTP
 * responder: the master getters now read the daemon's elected clock, and the
 * streaming process never binds UDP 319/320.
 *
 * :returns: true if a live daemon clock was attached; false otherwise (the
 *           caller falls back to the in-process engine — single-device path).
 */
bool ap2_ptp_attach_shared(struct ap2_ptp_ctx *ctx);

/* True if this context is reading the shared daemon clock (vs an in-process engine). */
bool ap2_ptp_shared_active(struct ap2_ptp_ctx *ctx);

/*
 * Register/unregister a receiver IP with the local daemon over the control
 * channel, so the daemon aggregates it into the PTP timing peer set and runs
 * BMCA across all streams' receivers. No-op unless shared mode is active. The
 * registered IP is remembered and auto-unregistered on ap2_ptp_destroy().
 */
void ap2_ptp_shared_register(struct ap2_ptp_ctx *ctx, const char *ip);
void ap2_ptp_shared_unregister(struct ap2_ptp_ctx *ctx, const char *ip);

/*
 * Run the PTP daemon: own UDP 319/320, run the grandmaster+BMCA+slave engine,
 * publish the elected-master clock to shared memory, and serve the control
 * channel (streams register their receiver IPs). Blocks until *stop becomes
 * true (set from a signal handler). This is the body of `--ptp-daemon`.
 *
 * :param bind_addr: multicast egress/join interface (INADDR_ANY for default).
 * :param stop: pointer to a flag polled for shutdown.
 * :returns: 0 on clean shutdown; non-zero if the engine could not bind 319/320.
 */
int ap2_ptp_run_daemon(struct in_addr bind_addr, volatile bool *stop);

#endif /* __AP2_PTP_H_ */
