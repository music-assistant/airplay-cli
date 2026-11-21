/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HOST_CLOCK_H
#define HOST_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* CLOCK_REALTIME until an offset is set, then the raw clock plus that offset
 * plus the rate correction, which NTP slews and steps never move. On macOS the
 * raw clock is CLOCK_UPTIME_RAW so callers can align it with mach_absolute_time. */
uint64_t host_clock_now_ns(void);
void host_clock_set_offset_ns(int64_t offset_ns);
bool host_clock_offset_ns(int64_t *offset_ns);

/* Rate correction in parts per billion (positive runs the clock faster), for
 * a raw clock that runs off the true rate. Only applies once an offset is set.
 * Set it once at startup, before any thread reads the clock. */
void host_clock_set_rate_ppb(int64_t ppb);
int64_t host_clock_rate_ppb(void);

/* What the rate correction adds to a raw clock reading of raw_ns. */
int64_t host_clock_rate_term_ns(int64_t raw_ns, int64_t ppb);

#endif
