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
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/src/raop_client.h"
#include "cross_log.h"
#include "ap2_ptp.h"

extern log_level *loglevel;

#define NTP_EPOCH_DELTA 0x83AA7E80

/* ---- PTP (IEEE 1588-2008) grandmaster constants ---- */

#define PTP_MCAST_ADDR      "224.0.1.129"   /* primary PTP multicast group */
#define PTP_EVENT_PORT      319             /* Sync, Delay_Req */
#define PTP_GENERAL_PORT    320             /* Announce, Follow_Up, Delay_Resp */
#define PTP_DOMAIN          0
#define PTP_VERSION         2
#define PTP_HDR_LEN         34

/* messageType (low nibble of byte 0) */
#define PTP_MSG_SYNC        0x0
#define PTP_MSG_DELAY_REQ   0x1
#define PTP_MSG_FOLLOW_UP   0x8
#define PTP_MSG_DELAY_RESP  0x9
#define PTP_MSG_ANNOUNCE    0xB

/* flagField, expressed as (octet6 << 8) | octet7 */
#define PTP_FLAG_TWO_STEP       0x0200
#define PTP_FLAG_UNICAST        0x0400
#define PTP_FLAG_PTP_TIMESCALE  0x0008

/* Best-master-clock advertisement. Lower Priority1 wins BMCA; ~248 is
 * competitive against the values observed from Apple grandmasters so we tend
 * to be selected as the group's grandmaster. */
#define PTP_PRIORITY1       248
#define PTP_PRIORITY2       248
#define PTP_CLOCK_CLASS     248             /* application default, not traceable */
#define PTP_CLOCK_ACCURACY  0xFE            /* unknown */
#define PTP_LOG_VARIANCE    0xFFFF

#define PTP_ANNOUNCE_INTERVAL_NS  1000000000ULL   /* 1 s   -> logInterval 0  */
#define PTP_SYNC_INTERVAL_NS       125000000ULL   /* 125 ms -> logInterval -3 */
#define PTP_LOG_ANNOUNCE_INTERVAL  0
#define PTP_LOG_SYNC_INTERVAL      (-3)

#define PTP_MAX_PEERS       8

struct ap2_ptp_ctx {
    /* PTP-to-local offset in nanoseconds (set by provider) */
    int64_t offset_ns;

    /* NTP timing responder */
    int timing_sock;
    pthread_t timing_thread;
    bool running;
    char *device_ip;

    /* PTP grandmaster engine */
    uint64_t clock_id;
    bool clock_id_set;
    int event_sock;              /* UDP 319 */
    int general_sock;            /* UDP 320 */
    pthread_t ptp_thread;
    bool ptp_running;
    bool engine_active;
    struct in_addr bind_addr;    /* multicast egress/join interface (INADDR_ANY = default) */
    struct in_addr mcast_addr;
    uint16_t sync_seq;
    uint16_t announce_seq;
    char *peers[PTP_MAX_PEERS];
    int npeers;
    /* When true, timing messages are ALSO sent unicast to each peer. Multicast
     * is the standard/default transport (this stays false); the peer list and
     * this switch keep unicast one flip away for on-device experimentation. */
    bool unicast_mirror;
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

/* ---- PTP grandmaster engine ---- */

/* Write the 10-byte PTP timestamp: 48-bit seconds (BE) + 32-bit nanoseconds (BE). */
static void ptp_write_ts(uint8_t *b, uint64_t ns)
{
    uint64_t sec = ns / 1000000000ULL;
    uint32_t nsec = (uint32_t)(ns % 1000000000ULL);
    b[0] = (sec >> 40) & 0xFF;
    b[1] = (sec >> 32) & 0xFF;
    b[2] = (sec >> 24) & 0xFF;
    b[3] = (sec >> 16) & 0xFF;
    b[4] = (sec >> 8) & 0xFF;
    b[5] = sec & 0xFF;
    b[6] = (nsec >> 24) & 0xFF;
    b[7] = (nsec >> 16) & 0xFF;
    b[8] = (nsec >> 8) & 0xFF;
    b[9] = nsec & 0xFF;
}

/* Write the common 34-byte PTPv2 message header. */
static void ptp_write_hdr(uint8_t *b, int msg_type, uint16_t msg_len, uint16_t flags,
                          uint64_t clock_id, uint16_t seq, uint8_t control, int8_t log_interval)
{
    memset(b, 0, PTP_HDR_LEN);
    b[0] = msg_type & 0x0F;          /* transportSpecific=0 | messageType */
    b[1] = PTP_VERSION & 0x0F;       /* reserved=0 | versionPTP=2 */
    b[2] = (msg_len >> 8) & 0xFF;
    b[3] = msg_len & 0xFF;
    b[4] = PTP_DOMAIN;
    b[6] = (flags >> 8) & 0xFF;      /* flagField octet 6 */
    b[7] = flags & 0xFF;             /* flagField octet 7 */
    /* correctionField (8..15) and messageTypeSpecific (16..19) stay zero */
    for (int i = 0; i < 8; i++) b[20 + i] = (clock_id >> (56 - 8 * i)) & 0xFF;
    b[28] = 0x00; b[29] = 0x01;      /* sourcePortIdentity.portNumber = 1 */
    b[30] = (seq >> 8) & 0xFF;
    b[31] = seq & 0xFF;
    b[32] = control;
    b[33] = (uint8_t)log_interval;
}

/* Send a PTP message to the multicast group (and, if enabled, unicast peers). */
static void ptp_send(struct ap2_ptp_ctx *ctx, int sock, uint16_t port,
                     const uint8_t *buf, int len)
{
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(port)};
    dst.sin_addr = ctx->mcast_addr;
    sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));

    if (ctx->unicast_mirror) {
        for (int i = 0; i < ctx->npeers; i++) {
            struct sockaddr_in u = {.sin_family = AF_INET, .sin_port = htons(port)};
            if (inet_pton(AF_INET, ctx->peers[i], &u.sin_addr) == 1)
                sendto(sock, buf, len, 0, (struct sockaddr *)&u, sizeof(u));
        }
    }
}

static void ptp_send_announce(struct ap2_ptp_ctx *ctx)
{
    uint8_t b[64];
    uint16_t len = PTP_HDR_LEN + 30;
    ptp_write_hdr(b, PTP_MSG_ANNOUNCE, len, PTP_FLAG_PTP_TIMESCALE, ctx->clock_id,
                  ctx->announce_seq, 0x05, PTP_LOG_ANNOUNCE_INTERVAL);
    uint8_t *body = b + PTP_HDR_LEN;
    ptp_write_ts(body, ap2_ptp_now_ns(ctx));      /* originTimestamp */
    body[10] = 0; body[11] = 37;                  /* currentUtcOffset = 37 (TAI-UTC) */
    body[12] = 0;                                 /* reserved */
    body[13] = PTP_PRIORITY1;
    body[14] = PTP_CLOCK_CLASS;
    body[15] = PTP_CLOCK_ACCURACY;
    body[16] = (PTP_LOG_VARIANCE >> 8) & 0xFF;
    body[17] = PTP_LOG_VARIANCE & 0xFF;
    body[18] = PTP_PRIORITY2;
    for (int i = 0; i < 8; i++) body[19 + i] = (ctx->clock_id >> (56 - 8 * i)) & 0xFF;
    body[27] = 0; body[28] = 0;                   /* stepsRemoved = 0 */
    body[29] = 0xA0;                              /* timeSource = INTERNAL_OSCILLATOR */

    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, b, len);
    LOG_DEBUG("[PTP] TX Announce seq=%u gm=%016" PRIx64 " prio1=%d", ctx->announce_seq,
              ctx->clock_id, PTP_PRIORITY1);
    ctx->announce_seq++;
}

/* Two-step master: emit Sync (empty originTimestamp) then Follow_Up carrying
 * the precise egress time. Both share one sequenceId. */
static void ptp_send_sync(struct ap2_ptp_ctx *ctx)
{
    uint16_t seq = ctx->sync_seq;
    uint16_t len = PTP_HDR_LEN + 10;

    uint8_t s[PTP_HDR_LEN + 10];
    ptp_write_hdr(s, PTP_MSG_SYNC, len, PTP_FLAG_TWO_STEP, ctx->clock_id, seq,
                  0x00, PTP_LOG_SYNC_INTERVAL);
    memset(s + PTP_HDR_LEN, 0, 10);               /* originTimestamp = 0 (two-step) */
    ptp_send(ctx, ctx->event_sock, PTP_EVENT_PORT, s, len);
    uint64_t egress = ap2_ptp_now_ns(ctx);        /* best-effort software egress time */

    uint8_t f[PTP_HDR_LEN + 10];
    ptp_write_hdr(f, PTP_MSG_FOLLOW_UP, len, 0, ctx->clock_id, seq,
                  0x02, PTP_LOG_SYNC_INTERVAL);
    ptp_write_ts(f + PTP_HDR_LEN, egress);        /* preciseOriginTimestamp */
    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, f, len);

    LOG_DEBUG("[PTP] TX Sync+Follow_Up seq=%u t=%" PRIu64 "ns", seq, egress);
    ctx->sync_seq++;
}

/* Answer a received Delay_Req with a Delay_Resp echoing the requester's
 * port identity and sequenceId, and our receive timestamp. */
static void ptp_send_delay_resp(struct ap2_ptp_ctx *ctx, const uint8_t *req, uint64_t rx_ns)
{
    uint16_t seq = (req[30] << 8) | req[31];
    uint16_t len = PTP_HDR_LEN + 10 + 10;
    uint8_t b[PTP_HDR_LEN + 10 + 10];
    ptp_write_hdr(b, PTP_MSG_DELAY_RESP, len, 0, ctx->clock_id, seq, 0x03, 0x7F);
    ptp_write_ts(b + PTP_HDR_LEN, rx_ns);              /* receiveTimestamp */
    memcpy(b + PTP_HDR_LEN + 10, req + 20, 10);        /* requestingPortIdentity */
    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, b, len);
    LOG_DEBUG("[PTP] TX Delay_Resp seq=%u rx=%" PRIu64 "ns", seq, rx_ns);
}

static int ptp_open_socket(struct ap2_ptp_ctx *ctx, uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    /* SO_REUSEADDR is the usual requirement for binding a multicast port; we do
     * NOT set SO_REUSEPORT, so a second cliairplay that also tries PTP fails to
     * bind here and falls back to NTP rather than silently co-binding as a
     * competing grandmaster (only one process owns 319/320 for now). */
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Bind INADDR_ANY:port. Binding a specific unicast address would stop the
     * kernel from delivering datagrams sent to the multicast group on BSD and
     * Linux; interface selection is done via the join and IP_MULTICAST_IF. */
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr = ctx->mcast_addr;
    mreq.imr_interface = ctx->bind_addr;     /* INADDR_ANY -> kernel default */
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        LOG_WARN("[PTP] IP_ADD_MEMBERSHIP failed on %u: %s", port, strerror(errno));

    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ctx->bind_addr, sizeof(ctx->bind_addr));
    unsigned char ttl = 1;      /* link-local timing domain */
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return s;
}

static void *ptp_thread_func(void *arg)
{
    struct ap2_ptp_ctx *ctx = (struct ap2_ptp_ctx *)arg;
    uint64_t last_announce = 0, last_sync = 0;

    while (ctx->ptp_running) {
        uint64_t now = ap2_ptp_now_ns(ctx);
        if (now - last_announce >= PTP_ANNOUNCE_INTERVAL_NS) {
            ptp_send_announce(ctx);
            last_announce = now;
        }
        if (now - last_sync >= PTP_SYNC_INTERVAL_NS) {
            ptp_send_sync(ctx);
            last_sync = now;
        }

        struct pollfd pfds[2] = {
            {.fd = ctx->event_sock, .events = POLLIN},
            {.fd = ctx->general_sock, .events = POLLIN},
        };
        int pr = poll(pfds, 2, 20);
        if (pr <= 0) continue;

        for (int i = 0; i < 2; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            uint8_t buf[256];
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            int n = recvfrom(pfds[i].fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
            uint64_t rx = ap2_ptp_now_ns(ctx);
            if (n < PTP_HDR_LEN) continue;

            int msg_type = buf[0] & 0x0F;
            char srcip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &src.sin_addr, srcip, sizeof(srcip));
            switch (msg_type) {
            case PTP_MSG_DELAY_REQ:
                LOG_DEBUG("[PTP] RX Delay_Req from %s", srcip);
                if (n >= PTP_HDR_LEN + 10) ptp_send_delay_resp(ctx, buf, rx);
                break;
            case PTP_MSG_ANNOUNCE:
                LOG_DEBUG("[PTP] RX Announce from %s (competing master)", srcip);
                break;
            case PTP_MSG_SYNC:
                LOG_DEBUG("[PTP] RX Sync from %s", srcip);
                break;
            default:
                LOG_DEBUG("[PTP] RX msgType=0x%x from %s", msg_type, srcip);
                break;
            }
        }
    }
    return NULL;
}

/* ---- Public API ---- */

struct ap2_ptp_ctx *ap2_ptp_create(void)
{
    struct ap2_ptp_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->timing_sock = -1;
    ctx->event_sock = -1;
    ctx->general_sock = -1;
    return ctx;
}

void ap2_ptp_destroy(struct ap2_ptp_ctx *ctx)
{
    if (!ctx) return;
    ap2_ptp_stop(ctx);
    for (int i = 0; i < ctx->npeers; i++) free(ctx->peers[i]);
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
    if (!ctx) return;

    /* NTP responder */
    if (ctx->running) {
        ctx->running = false;
        if (ctx->timing_sock >= 0) {
            close(ctx->timing_sock);
            ctx->timing_sock = -1;
        }
        pthread_join(ctx->timing_thread, NULL);
    }

    /* PTP grandmaster engine */
    if (ctx->ptp_running) {
        ctx->ptp_running = false;
        pthread_join(ctx->ptp_thread, NULL);
        if (ctx->event_sock >= 0) { close(ctx->event_sock); ctx->event_sock = -1; }
        if (ctx->general_sock >= 0) { close(ctx->general_sock); ctx->general_sock = -1; }
        ctx->engine_active = false;
    }
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

/* ---- PTP grandmaster engine: public API ---- */

void ap2_ptp_set_clock_id(struct ap2_ptp_ctx *ctx, uint64_t clock_id)
{
    if (!ctx) return;
    ctx->clock_id = clock_id;
    ctx->clock_id_set = true;
}

uint64_t ap2_ptp_clock_id(struct ap2_ptp_ctx *ctx)
{
    return ctx ? ctx->clock_id : 0;
}

uint64_t ap2_ptp_now_ns(struct ap2_ptp_ctx *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void ap2_ptp_set_peers(struct ap2_ptp_ctx *ctx, const char *const *ips, int count)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->npeers; i++) { free(ctx->peers[i]); ctx->peers[i] = NULL; }
    ctx->npeers = 0;
    for (int i = 0; i < count && ctx->npeers < PTP_MAX_PEERS; i++) {
        if (ips[i]) ctx->peers[ctx->npeers++] = strdup(ips[i]);
    }
    for (int i = 0; i < ctx->npeers; i++)
        LOG_DEBUG("[PTP] peer[%d] = %s", i, ctx->peers[i]);
}

bool ap2_ptp_engine_active(struct ap2_ptp_ctx *ctx)
{
    return ctx && ctx->engine_active;
}

bool ap2_ptp_engine_start(struct ap2_ptp_ctx *ctx, struct in_addr bind_addr,
                          const char *device_ip)
{
    if (!ctx) return false;
    if (device_ip && !ctx->device_ip) ctx->device_ip = strdup(device_ip);
    ctx->bind_addr = bind_addr;
    ctx->mcast_addr.s_addr = inet_addr(PTP_MCAST_ADDR);

    /* Derive a stable EUI-64 clock identity from the host MAC unless the caller
     * pinned one (so it matches the ClockID advertised in the session SETUP). */
    if (!ctx->clock_id_set) {
        uint8_t mac[6];
        get_mac(mac);
        ctx->clock_id = ((uint64_t)mac[0] << 56) | ((uint64_t)mac[1] << 48) |
                        ((uint64_t)mac[2] << 40) | ((uint64_t)0xFF << 32) |
                        ((uint64_t)0xFE << 24) | ((uint64_t)mac[3] << 16) |
                        ((uint64_t)mac[4] << 8) | (uint64_t)mac[5];
        ctx->clock_id_set = true;
    }

    ctx->event_sock = ptp_open_socket(ctx, PTP_EVENT_PORT);
    if (ctx->event_sock < 0) {
        LOG_ERROR("[PTP] Cannot bind UDP %d: %s (privileged port; run as root for PTP). "
                  "Falling back to NTP timing.", PTP_EVENT_PORT, strerror(errno));
        return false;
    }
    ctx->general_sock = ptp_open_socket(ctx, PTP_GENERAL_PORT);
    if (ctx->general_sock < 0) {
        LOG_ERROR("[PTP] Cannot bind UDP %d: %s (privileged port; run as root for PTP). "
                  "Falling back to NTP timing.", PTP_GENERAL_PORT, strerror(errno));
        close(ctx->event_sock);
        ctx->event_sock = -1;
        return false;
    }

    ctx->ptp_running = true;
    ctx->engine_active = true;
    if (pthread_create(&ctx->ptp_thread, NULL, ptp_thread_func, ctx) != 0) {
        LOG_ERROR("[PTP] Cannot start PTP thread");
        close(ctx->event_sock);
        close(ctx->general_sock);
        ctx->event_sock = ctx->general_sock = -1;
        ctx->ptp_running = false;
        ctx->engine_active = false;
        return false;
    }

    LOG_INFO("[PTP] Grandmaster started on UDP 319/320, clockID=%016" PRIx64
             ", mcast=%s, iface=%s", ctx->clock_id, PTP_MCAST_ADDR,
             ctx->bind_addr.s_addr == INADDR_ANY ? "default" : inet_ntoa(ctx->bind_addr));
    return true;
}
