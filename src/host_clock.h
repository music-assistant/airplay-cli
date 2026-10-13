/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HOST_CLOCK_H
#define HOST_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* CLOCK_REALTIME until an offset is set, then the raw clock plus that offset,
 * which NTP slews and steps never move. On macOS the raw clock is
 * CLOCK_UPTIME_RAW so callers can align it with mach_absolute_time. */
uint64_t host_clock_now_ns(void);
void host_clock_set_offset_ns(int64_t offset_ns);
bool host_clock_offset_ns(int64_t *offset_ns);

#endif
