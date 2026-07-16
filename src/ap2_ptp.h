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

#endif /* __AP2_PTP_H_ */
