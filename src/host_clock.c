/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "host_clock.h"

#include <time.h>

#ifdef __APPLE__
#define HOST_CLOCK_RAW CLOCK_UPTIME_RAW
#else
#define HOST_CLOCK_RAW CLOCK_MONOTONIC_RAW
#endif

static bool g_raw;
static int64_t g_offset_ns;

static int64_t read_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void host_clock_set_offset_ns(int64_t offset_ns)
{
    g_offset_ns = offset_ns;
    g_raw = true;
}

bool host_clock_offset_ns(int64_t *offset_ns)
{
    *offset_ns = g_offset_ns;
    return g_raw;
}

uint64_t host_clock_now_ns(void)
{
    if (!g_raw) return (uint64_t)read_ns(CLOCK_REALTIME);
    return (uint64_t)(read_ns(HOST_CLOCK_RAW) + g_offset_ns);
}

/* Replaces crosstools' gettime_us, see the Makefile. */
uint64_t gettime_us(void)
{
    return host_clock_now_ns() / 1000;
}
