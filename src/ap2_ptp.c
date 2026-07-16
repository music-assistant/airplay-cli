/*
 * AirPlay 2 PTP - Precision Time Protocol support
 *
 * AirPlay 2 devices (especially Apple HomePod, Apple TV) use PTP (IEEE 1588)
 * for clock synchronization instead of NTP.
 *
 * Architecture:
 *   The PTP clock synchronization runs centralized in the Music Assistant
 *   provider (Python side). The binary receives the PTP-to-local clock
 *   offset via --ptp-offset <nanoseconds> and adjusts timestamps accordingly.
 *
 *   For devices that use NTP timing (most third-party), no offset is needed.
 *   For PTP devices, the provider computes the offset by running a PTP client
 *   (or NQPTP daemon) and passes it to each cliairplay instance.
 *
 * This file also implements a minimal NTP timing responder for devices that
 * send NTP timing requests on the timing UDP port (required for RAOP-compat flow).
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/src/raop_client.h"
#include "cross_log.h"
#include "ap2_ptp.h"

extern log_level *loglevel;

#define NTP_EPOCH_DELTA 0x83AA7E80

struct ap2_ptp_ctx {
    /* PTP-to-local offset in nanoseconds (set by provider) */
    int64_t offset_ns;

    /* NTP timing responder */
    int timing_sock;
    pthread_t timing_thread;
    bool running;
    char *device_ip;
};

/* ---- NTP timing responder ---- */

static void get_ntp_time(uint32_t *sec, uint32_t *frac)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = (uint32_t)(ts.tv_sec + NTP_EPOCH_DELTA);
    *frac = (uint32_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ULL);
}

static void *timing_thread_func(void *arg)
{
    struct ap2_ptp_ctx *ctx = (struct ap2_ptp_ctx *)arg;
    uint8_t buf[256];
    struct sockaddr_in addr;
    socklen_t addr_len;

    struct timeval tv = {.tv_sec = 1};
    setsockopt(ctx->timing_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (ctx->running) {
        addr_len = sizeof(addr);
        int n = recvfrom(ctx->timing_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&addr, &addr_len);
        if (n < 32 || !ctx->running) continue;

        /* Check for timing request: byte[1] == 0xD2 */
        if (buf[1] != 0xD2) continue;

        /* Build timing response */
        uint8_t resp[32];
        memset(resp, 0, 32);
        resp[0] = 0x80;
        resp[1] = 0xD3;  /* timing response */

        /* Reference time = request's send time (bytes 24-31) */
        memcpy(resp + 8, buf + 24, 8);

        /* Receive time = now */
        uint32_t sec, frac;
        get_ntp_time(&sec, &frac);

        /* Apply PTP offset if set */
        if (ctx->offset_ns != 0) {
            int64_t ns = (int64_t)sec * 1000000000LL +
                         (int64_t)frac * 1000000000LL / (1LL << 32);
            ns += ctx->offset_ns;
            sec = (uint32_t)(ns / 1000000000LL);
            frac = (uint32_t)(((uint64_t)(ns % 1000000000LL) << 32) / 1000000000ULL);
        }

        resp[16] = (sec >> 24) & 0xFF;
        resp[17] = (sec >> 16) & 0xFF;
        resp[18] = (sec >> 8) & 0xFF;
        resp[19] = sec & 0xFF;
        resp[20] = (frac >> 24) & 0xFF;
        resp[21] = (frac >> 16) & 0xFF;
        resp[22] = (frac >> 8) & 0xFF;
        resp[23] = frac & 0xFF;

        /* Send time = same as receive time (we respond immediately) */
        memcpy(resp + 24, resp + 16, 8);

        sendto(ctx->timing_sock, resp, 32, 0,
               (struct sockaddr *)&addr, addr_len);
    }

    return NULL;
}

/* ---- Public API ---- */

struct ap2_ptp_ctx *ap2_ptp_create(void)
{
    struct ap2_ptp_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->timing_sock = -1;
    return ctx;
}

void ap2_ptp_destroy(struct ap2_ptp_ctx *ctx)
{
    if (!ctx) return;
    ap2_ptp_stop(ctx);
    free(ctx->device_ip);
    free(ctx);
}

void ap2_ptp_set_offset(struct ap2_ptp_ctx *ctx, int64_t offset_ns)
{
    if (ctx) ctx->offset_ns = offset_ns;
}

bool ap2_ptp_start(struct ap2_ptp_ctx *ctx, const char *device_ip)
{
    if (!ctx) return false;
    if (device_ip) ctx->device_ip = strdup(device_ip);

    /* Open UDP socket for NTP timing responses */
    ctx->timing_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->timing_sock < 0) {
        LOG_ERROR("[PTP] Cannot create timing socket");
        return false;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = 0,  /* OS assigns port */
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(ctx->timing_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("[PTP] Cannot bind timing socket");
        close(ctx->timing_sock);
        ctx->timing_sock = -1;
        return false;
    }

    ctx->running = true;
    pthread_create(&ctx->timing_thread, NULL, timing_thread_func, ctx);

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(ctx->timing_sock, (struct sockaddr *)&local, &len);
    LOG_INFO("[PTP] Timing responder on port %d (offset=%" PRId64 "ns)",
             ntohs(local.sin_port), ctx->offset_ns);

    return true;
}

void ap2_ptp_stop(struct ap2_ptp_ctx *ctx)
{
    if (!ctx || !ctx->running) return;
    ctx->running = false;
    if (ctx->timing_sock >= 0) {
        close(ctx->timing_sock);
        ctx->timing_sock = -1;
    }
    pthread_join(ctx->timing_thread, NULL);
}

int ap2_ptp_get_timing_port(struct ap2_ptp_ctx *ctx)
{
    if (!ctx || ctx->timing_sock < 0) return 0;
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(ctx->timing_sock, (struct sockaddr *)&local, &len);
    return ntohs(local.sin_port);
}

uint64_t ap2_ptp_get_time(struct ap2_ptp_ctx *ctx)
{
    uint32_t sec, frac;
    get_ntp_time(&sec, &frac);

    if (ctx && ctx->offset_ns != 0) {
        int64_t ns = (int64_t)(sec - NTP_EPOCH_DELTA) * 1000000000LL +
                     (int64_t)frac * 1000000000LL / (1LL << 32);
        ns += ctx->offset_ns;
        sec = (uint32_t)(ns / 1000000000LL) + NTP_EPOCH_DELTA;
        frac = (uint32_t)(((uint64_t)(ns % 1000000000LL) << 32) / 1000000000ULL);
    }

    return ((uint64_t)sec << 32) | frac;
}

uint64_t ap2_ptp_local_to_device(struct ap2_ptp_ctx *ctx, uint64_t local_ntp)
{
    if (!ctx || ctx->offset_ns == 0) return local_ntp;

    /* Convert NTP to nanoseconds, apply offset, convert back */
    uint32_t sec = (uint32_t)(local_ntp >> 32);
    uint32_t frac = (uint32_t)(local_ntp & 0xFFFFFFFF);
    int64_t ns = (int64_t)(sec - NTP_EPOCH_DELTA) * 1000000000LL +
                 (int64_t)frac * 1000000000LL / (1LL << 32);
    ns += ctx->offset_ns;
    sec = (uint32_t)(ns / 1000000000LL) + NTP_EPOCH_DELTA;
    frac = (uint32_t)(((uint64_t)(ns % 1000000000LL) << 32) / 1000000000ULL);
    return ((uint64_t)sec << 32) | frac;
}
