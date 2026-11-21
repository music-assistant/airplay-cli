/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "host_clock.h"

#ifdef __APPLE__
#define RAW_CLOCK CLOCK_UPTIME_RAW
#else
#define RAW_CLOCK CLOCK_MONOTONIC_RAW
#endif

#define NS 1000000000LL
#define TEN_YEARS_NS (10LL * 365 * 86400 * NS)
#define MAX_PPB 2000000LL

static int64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * NS + ts.tv_nsec;
}

static void test_zero_rate_adds_nothing(void)
{
    assert(host_clock_rate_term_ns(0, 0) == 0);
    assert(host_clock_rate_term_ns(123456789012345LL, 0) == 0);
    assert(host_clock_rate_term_ns(TEN_YEARS_NS, 0) == 0);
    puts("host_clock zero rate test passed");
}

static void test_rate_term_values(void)
{
    /* One second of raw time gains or loses exactly ppb nanoseconds. */
    assert(host_clock_rate_term_ns(NS, 857000) == 857000);
    assert(host_clock_rate_term_ns(NS, -857000) == -857000);
    assert(host_clock_rate_term_ns(100 * NS, 1000) == 100000);
    assert(host_clock_rate_term_ns(100 * NS, -1000) == -100000);

    /* A part of a second scales by the same ratio, truncating toward zero. */
    assert(host_clock_rate_term_ns(NS / 2, 1000000) == 500000);
    assert(host_clock_rate_term_ns(NS / 2, -1000000) == -500000);
    assert(host_clock_rate_term_ns(999999999, 1000) == 999);
    assert(host_clock_rate_term_ns(999999999, -1000) == -999);
    assert(host_clock_rate_term_ns(1500000000LL, 1000) == 1500);
    puts("host_clock rate term test passed");
}

static void test_ten_years_of_uptime(void)
{
    /* Hand-computed: 315360000 s * 2000000 ppb = 630720000000000 ns. */
    assert(host_clock_rate_term_ns(TEN_YEARS_NS, MAX_PPB) == 630720000000000LL);
    assert(host_clock_rate_term_ns(TEN_YEARS_NS, -MAX_PPB) == -630720000000000LL);

    /* The remainder 999999999 ns adds 1999999.998 ns, truncated toward zero. */
    assert(host_clock_rate_term_ns(TEN_YEARS_NS + 999999999, MAX_PPB) ==
           630720000000000LL + 1999999);
    assert(host_clock_rate_term_ns(TEN_YEARS_NS + 999999999, -MAX_PPB) ==
           -630720000000000LL - 1999999);
    puts("host_clock ten years of uptime test passed");
}

#ifdef __SIZEOF_INT128__
/* The split arithmetic equals the exact product for any raw time >= 0. */
static void test_parity_with_exact_arithmetic(void)
{
    const int64_t raws[] = {0, 1, 999999999, NS, NS + 1, 123456789012345LL,
                            TEN_YEARS_NS, TEN_YEARS_NS + 999999999,
                            100LL * 365 * 86400 * NS};
    const int64_t ppbs[] = {0, 1, -1, 857000, -857000, 150000, 230000,
                            MAX_PPB, -MAX_PPB};
    for (size_t i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
        for (size_t j = 0; j < sizeof(ppbs) / sizeof(ppbs[0]); j++) {
            int64_t exact = (int64_t)(((__int128)raws[i] * ppbs[j]) / NS);
            assert(host_clock_rate_term_ns(raws[i], ppbs[j]) == exact);
        }
    }

    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 100000; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t raw = (int64_t)(state >> 17) % (TEN_YEARS_NS * 10);
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t ppb = (int64_t)((state >> 33) % (2 * MAX_PPB + 1)) - MAX_PPB;
        int64_t exact = (int64_t)(((__int128)raw * ppb) / NS);
        assert(host_clock_rate_term_ns(raw, ppb) == exact);
    }
    puts("host_clock parity with exact arithmetic test passed");
}
#endif

static void test_clock_reads(void)
{
    /* Without an offset the clock is the wall clock and the rate is ignored. */
    int64_t offset;
    assert(!host_clock_offset_ns(&offset));
    assert(host_clock_rate_ppb() == 0);
    host_clock_set_rate_ppb(MAX_PPB);
    assert(host_clock_rate_ppb() == MAX_PPB);
    int64_t gap = (int64_t)host_clock_now_ns() - clock_ns(CLOCK_REALTIME);
    assert(gap > -100000000LL && gap < 100000000LL);

    /* Rate 0 is the raw clock plus the offset, as before the rate existed. */
    host_clock_set_rate_ppb(0);
    host_clock_set_offset_ns(5 * NS);
    int64_t before = clock_ns(RAW_CLOCK) + 5 * NS;
    int64_t now = (int64_t)host_clock_now_ns();
    int64_t after = clock_ns(RAW_CLOCK) + 5 * NS;
    assert(now >= before && now <= after);

    /* With a rate the reading carries the correction of its own raw time. */
    host_clock_set_rate_ppb(-857000);
    assert(host_clock_rate_ppb() == -857000);
    int64_t raw_before = clock_ns(RAW_CLOCK);
    now = (int64_t)host_clock_now_ns();
    int64_t raw_after = clock_ns(RAW_CLOCK);
    int64_t low = raw_before + 5 * NS + host_clock_rate_term_ns(raw_before, -857000);
    int64_t high = raw_after + 5 * NS + host_clock_rate_term_ns(raw_after, -857000);
    assert(now >= low && now <= high);
    puts("host_clock reads test passed");
}

int main(void)
{
    test_zero_rate_adds_nothing();
    test_rate_term_values();
    test_ten_years_of_uptime();
#ifdef __SIZEOF_INT128__
    test_parity_with_exact_arithmetic();
#endif
    test_clock_reads();
    return 0;
}
