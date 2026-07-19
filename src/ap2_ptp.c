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
#include "ap2_ptp_shm.h"

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
#define PTP_MSG_SIGNALING   0xC

/* Unicast-negotiation TLVs (IEEE-1588 16.1). Apple receivers request unicast
 * Announce/Sync/Delay_Resp from the sender's clock via Signaling. */
#define PTP_TLV_REQUEST_UNICAST 0x0004
#define PTP_TLV_GRANT_UNICAST   0x0005

/* flagField, expressed as (octet6 << 8) | octet7 */
#define PTP_FLAG_TWO_STEP       0x0200
#define PTP_FLAG_UNICAST        0x0400
#define PTP_FLAG_PTP_TIMESCALE  0x0008

/* Best-master-clock advertisement. Lower Priority1 wins BMCA; ~248 is
 * competitive against the values observed from Apple grandmasters so we tend
 * to be selected as the group's grandmaster. */
/* The grandmaster dataset iOS senders announce (from owntone's libairptp, which
 * mirrors captured iOS traffic): priority 128, clockClass 6 (GPS-locked),
 * accuracy 0x21 (100ns), variance 0x436A, timeSource GPS. Receivers compare
 * this against their own clock (Sonos: 248/248/0xFE/0x436A), so it must
 * out-rank them for the session anchor to be valid. */
#define PTP_PRIORITY1       128
#define PTP_PRIORITY2       128
#define PTP_CLOCK_CLASS     6
#define PTP_CLOCK_ACCURACY  0x21
#define PTP_LOG_VARIANCE    0x436A
#define PTP_TIME_SOURCE     0x20            /* GPS */

#define PTP_ANNOUNCE_INTERVAL_NS  1000000000ULL   /* 1 s   -> logInterval 0  */
#define PTP_SYNC_INTERVAL_NS       125000000ULL   /* 125 ms -> logInterval -3 */
#define PTP_LOG_ANNOUNCE_INTERVAL  0
#define PTP_LOG_SYNC_INTERVAL      (-3)

#define PTP_MAX_PEERS       8

/* ---- BMCA / slave tuning ---- */

/* Offset smoothing: fold each new local->master sample as an EMA with gain
 * 1/PTP_OFFSET_EMA_DIV, but SNAP straight to the sample when it steps by more
 * than PTP_OFFSET_SNAP_NS (first lock, or a clock reset) so we track rather
 * than crawl. The offset is dominated by userspace-timestamping jitter of a
 * few tens of microseconds, well under the SNAP threshold, so the EMA filters
 * that jitter while the SNAP catches genuine steps. */
#define PTP_OFFSET_EMA_DIV     8
#define PTP_OFFSET_SNAP_NS     1000000LL          /* 1 ms */

/* Revert to grandmaster if the elected peer stops announcing for this long
 * (3x the 1 s announce interval, so a couple of dropped Announces don't flap). */
#define PTP_PEER_SILENCE_NS    3000000000ULL      /* 3 s */

/* An IEEE-1588 best-master dataset: the grandmaster attributes carried in an
 * Announce (or synthesised for ourselves), enough to run the dataset
 * comparison and to name the elected timeline. */
struct ptp_dataset {
    uint8_t  priority1;                    /* grandmasterPriority1 */
    uint8_t  clock_class;                  /* grandmasterClockQuality.clockClass */
    uint8_t  clock_accuracy;               /* grandmasterClockQuality.clockAccuracy */
    uint16_t offset_scaled_log_variance;   /* grandmasterClockQuality.offsetScaledLogVariance */
    uint8_t  priority2;                    /* grandmasterPriority2 */
    uint64_t grandmaster_identity;         /* grandmasterIdentity */
    uint16_t steps_removed;                /* stepsRemoved */
    uint64_t source_port_identity;         /* sourcePortIdentity clockId of the Announce sender */
};

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
    uint16_t signaling_seq;
    char *peers[PTP_MAX_PEERS];
    int npeers;
    /* When true, timing messages are ALSO sent unicast to each peer. Multicast
     * is the standard/default transport (this stays false); the peer list and
     * this switch keep unicast one flip away for on-device experimentation. */
    bool unicast_mirror;

    /* ---- BMCA / slave state (guarded by lock) ---- */
    pthread_mutex_t lock;
    bool is_grandmaster;            /* true: we drive the timeline; false: slaved to a peer */
    /* The AirPlay sender is the session's timing authority: receivers only
     * consider masters from the SETPEERS timing-peer list (i.e. us) and cannot
     * follow their own clock, so surrendering the timeline mutes them. With
     * hold_master (the default) a competing Announce is recorded but never
     * wins the election; the BMCA/slave machinery below stays available for
     * diagnostics and the synthetic harness. */
    bool hold_master;
    bool hold_notice_logged;        /* one INFO line per session about an ignored peer GM */
    bool unicast_granted;           /* a peer negotiated unicast PTP via Signaling */
    uint64_t master_clock_id;       /* elected GM identity (peer's grandmasterIdentity when slaving) */
    int64_t master_offset_ns;       /* smoothed local->master offset (add to local for master time) */
    bool have_offset;               /* at least one offset sample folded in */
    bool have_peer;                 /* at least one peer Announce parsed */
    struct ptp_dataset best_peer;   /* most recent competing dataset */
    uint64_t last_peer_announce_ns; /* local ns of the most recent peer Announce (silence detect) */
    /* Two-step Sync/Follow_Up pairing (single outstanding, keyed by sequenceId). */
    bool pending_sync_valid;
    uint16_t pending_sync_seq;
    uint64_t pending_sync_rx_ns;    /* t2: local reception time of the Sync */
    int64_t pending_sync_corr_ns;   /* correctionField carried on the Sync */

    /* ---- shared daemon clock (streaming side, --ptp-shared) ---- */
    /* When shm_active, the master getters below read the elected clock from the
     * daemon's shared memory instead of this engine (which is not running in a
     * streaming process); ap2_client.c is oblivious to the source. */
    bool shm_active;
    struct ap2_ptp_shm_reader shm_reader;
    char *shared_ip;                /* receiver IP registered with the daemon (for unregister) */
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
    /* majorSdoId (transportSpecific) = 1: AirPlay receivers run the gPTP
     * (IEEE 802.1AS) profile over UDP and silently DISCARD any PTP message
     * with majorSdoId 0, so a plain-1588 first byte hides our clock from
     * them entirely (confirmed against a Sonos packet capture). */
    b[0] = 0x10 | (msg_type & 0x0F);
    b[1] = PTP_VERSION & 0x0F;       /* reserved=0 | versionPTP=2 */
    b[2] = (msg_len >> 8) & 0xFF;
    b[3] = msg_len & 0xFF;
    b[4] = PTP_DOMAIN;
    b[6] = (flags >> 8) & 0xFF;      /* flagField octet 6 */
    b[7] = flags & 0xFF;             /* flagField octet 7 */
    /* correctionField (8..15) and messageTypeSpecific (16..19) stay zero */
    for (int i = 0; i < 8; i++) b[20 + i] = (clock_id >> (56 - 8 * i)) & 0xFF;
    b[28] = 0x80; b[29] = 0x05;      /* sourcePortIdentity.portNumber, as iOS */
    b[30] = (seq >> 8) & 0xFF;
    b[31] = seq & 0xFF;
    b[32] = control;
    b[33] = (uint8_t)log_interval;
}

/* Send a PTP message. iOS senders unicast timing straight to each timing peer
 * (never an open multicast election), so with a peer list the message goes
 * unicast to every peer; without one (daemon idle, pre-SETPEERS) it falls back
 * to the multicast group. */
static void ptp_send(struct ap2_ptp_ctx *ctx, int sock, uint16_t port,
                     const uint8_t *buf, int len)
{
    bool sent = false;
    if (ctx->unicast_mirror) {
        for (int i = 0; i < ctx->npeers; i++) {
            struct sockaddr_in u = {.sin_family = AF_INET, .sin_port = htons(port)};
            if (inet_pton(AF_INET, ctx->peers[i], &u.sin_addr) == 1) {
                sendto(sock, buf, len, 0, (struct sockaddr *)&u, sizeof(u));
                sent = true;
            }
        }
    }
    if (!sent) {
        struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(port)};
        dst.sin_addr = ctx->mcast_addr;
        sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
}

/* Periodic Signaling (iOS sends one per second to its timing peers): two
 * Apple ORGANIZATION_EXTENSION TLVs (OUI 00:0D:93, subtypes 1 and 5) whose
 * payload starts 00 00 03 01, remainder zero. 106 bytes total. */
static void ptp_send_signaling(struct ap2_ptp_ctx *ctx)
{
    uint16_t len = PTP_HDR_LEN + 10 + 26 + 36;
    uint8_t b[PTP_HDR_LEN + 10 + 26 + 36];
    memset(b, 0, sizeof(b));
    ptp_write_hdr(b, PTP_MSG_SIGNALING, len,
                  PTP_FLAG_UNICAST | PTP_FLAG_PTP_TIMESCALE,
                  ctx->clock_id, ctx->signaling_seq, 0x05, -128);
    /* targetPortIdentity left zero (wildcard) */
    uint8_t *tlv = b + PTP_HDR_LEN + 10;
    tlv[0] = 0x00; tlv[1] = 0x03; tlv[2] = 0x00; tlv[3] = 0x16;   /* len 22 */
    tlv[4] = 0x00; tlv[5] = 0x0D; tlv[6] = 0x93;
    tlv[7] = 0x00; tlv[8] = 0x00; tlv[9] = 0x01;
    tlv[10] = 0x00; tlv[11] = 0x00; tlv[12] = 0x03; tlv[13] = 0x01;
    tlv += 26;
    tlv[0] = 0x00; tlv[1] = 0x03; tlv[2] = 0x00; tlv[3] = 0x20;   /* len 32 */
    tlv[4] = 0x00; tlv[5] = 0x0D; tlv[6] = 0x93;
    tlv[7] = 0x00; tlv[8] = 0x00; tlv[9] = 0x05;
    tlv[10] = 0x00; tlv[11] = 0x00; tlv[12] = 0x03; tlv[13] = 0x01;
    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, b, len);
    ctx->signaling_seq++;
}

static void ptp_send_announce(struct ap2_ptp_ctx *ctx)
{
    /* iOS-shaped Announce: flags UNICAST|TIMESCALE, originTimestamp 0,
     * currentUtcOffset 0, control 0, and a PATH_TRACE TLV carrying the clock
     * id (receivers running the gPTP profile expect it; 76 bytes total). */
    uint8_t b[96];
    uint16_t len = PTP_HDR_LEN + 30 + 12;
    ptp_write_hdr(b, PTP_MSG_ANNOUNCE, len, PTP_FLAG_UNICAST | PTP_FLAG_PTP_TIMESCALE,
                  ctx->clock_id, ctx->announce_seq, 0x00, PTP_LOG_ANNOUNCE_INTERVAL);
    uint8_t *body = b + PTP_HDR_LEN;
    memset(body, 0, 10);                          /* originTimestamp = 0 (as iOS) */
    body[10] = 0; body[11] = 0;                   /* currentUtcOffset = 0 (as iOS) */
    body[12] = 0;                                 /* reserved */
    body[13] = PTP_PRIORITY1;
    body[14] = PTP_CLOCK_CLASS;
    body[15] = PTP_CLOCK_ACCURACY;
    body[16] = (PTP_LOG_VARIANCE >> 8) & 0xFF;
    body[17] = PTP_LOG_VARIANCE & 0xFF;
    body[18] = PTP_PRIORITY2;
    for (int i = 0; i < 8; i++) body[19 + i] = (ctx->clock_id >> (56 - 8 * i)) & 0xFF;
    body[27] = 0; body[28] = 0;                   /* stepsRemoved = 0 */
    body[29] = PTP_TIME_SOURCE;
    /* PATH_TRACE TLV: type 0x0008, length 8, value = grandmaster clock id */
    body[30] = 0x00; body[31] = 0x08;
    body[32] = 0x00; body[33] = 0x08;
    for (int i = 0; i < 8; i++) body[34 + i] = (ctx->clock_id >> (56 - 8 * i)) & 0xFF;

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
    ptp_write_hdr(s, PTP_MSG_SYNC, len,
                  PTP_FLAG_UNICAST | PTP_FLAG_PTP_TIMESCALE | PTP_FLAG_TWO_STEP,
                  ctx->clock_id, seq, 0x00, PTP_LOG_SYNC_INTERVAL);
    memset(s + PTP_HDR_LEN, 0, 10);               /* originTimestamp = 0 (two-step) */
    ptp_send(ctx, ctx->event_sock, PTP_EVENT_PORT, s, len);
    uint64_t egress = ap2_ptp_now_ns(ctx);        /* best-effort software egress time */

    /* Follow_Up carries the precise timestamp plus the two TLVs iOS appends:
     * the 802.1AS Follow_Up information TLV (zeroed rate/phase fields) and an
     * Apple TLV repeating the clock id. 96 bytes total. */
    uint16_t flen = PTP_HDR_LEN + 10 + 32 + 20;
    uint8_t f[PTP_HDR_LEN + 10 + 32 + 20];
    memset(f, 0, sizeof(f));
    ptp_write_hdr(f, PTP_MSG_FOLLOW_UP, flen,
                  PTP_FLAG_UNICAST | PTP_FLAG_PTP_TIMESCALE,
                  ctx->clock_id, seq, 0x00, PTP_LOG_SYNC_INTERVAL);
    ptp_write_ts(f + PTP_HDR_LEN, egress);        /* preciseOriginTimestamp */
    uint8_t *tlv = f + PTP_HDR_LEN + 10;
    /* 802.1AS Follow_Up information: ORG_EXT, len 28, OUI 00:80:C2 subtype 1 */
    tlv[0] = 0x00; tlv[1] = 0x03; tlv[2] = 0x00; tlv[3] = 0x1C;
    tlv[4] = 0x00; tlv[5] = 0x80; tlv[6] = 0xC2;
    tlv[7] = 0x00; tlv[8] = 0x00; tlv[9] = 0x01;  /* remaining 22 bytes zero */
    tlv += 32;
    /* Apple clock-id TLV: ORG_EXT, len 16, OUI 00:0D:93 subtype 4 + clock id */
    tlv[0] = 0x00; tlv[1] = 0x03; tlv[2] = 0x00; tlv[3] = 0x10;
    tlv[4] = 0x00; tlv[5] = 0x0D; tlv[6] = 0x93;
    tlv[7] = 0x00; tlv[8] = 0x00; tlv[9] = 0x04;
    for (int i = 0; i < 8; i++) tlv[10 + i] = (ctx->clock_id >> (56 - 8 * i)) & 0xFF;
    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, f, flen);

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
    ptp_write_hdr(b, PTP_MSG_DELAY_RESP, len,
                  PTP_FLAG_UNICAST | PTP_FLAG_PTP_TIMESCALE | PTP_FLAG_TWO_STEP,
                  ctx->clock_id, seq, 0x00, PTP_LOG_SYNC_INTERVAL);
    ptp_write_ts(b + PTP_HDR_LEN, rx_ns);              /* receiveTimestamp */
    memcpy(b + PTP_HDR_LEN + 10, req + 20, 10);        /* requestingPortIdentity */
    ptp_send(ctx, ctx->general_sock, PTP_GENERAL_PORT, b, len);
    LOG_DEBUG("[PTP] TX Delay_Resp seq=%u rx=%" PRIu64 "ns", seq, rx_ns);
}

/*
 * Handle a Signaling message: grant REQUEST_UNICAST_TRANSMISSION TLVs.
 *
 * Apple receivers negotiate unicast PTP — they ask the sender's clock for
 * unicast Announce/Sync/Delay_Resp instead of relying on multicast. We reply
 * with one GRANT TLV per request (echoing message type, rate and duration,
 * renewal invited) and serve the peer through the unicast mirror.
 */
static void ptp_handle_signaling(struct ap2_ptp_ctx *ctx, const uint8_t *buf, int n,
                                 const struct sockaddr_in *src, const char *srcip)
{
    /* header (34) + targetPortIdentity (10), then TLVs */
    int off = PTP_HDR_LEN + 10;
    if (n < off + 4) return;

    uint8_t reply[PTP_HDR_LEN + 10 + 8 * 12];
    uint8_t *tlv_out = reply + PTP_HDR_LEN + 10;
    int granted = 0;
    char kinds[64] = "";

    while (off + 4 <= n && granted < 8) {
        uint16_t tlv_type = (buf[off] << 8) | buf[off + 1];
        uint16_t tlv_len = (buf[off + 2] << 8) | buf[off + 3];
        const uint8_t *val = buf + off + 4;
        if (off + 4 + tlv_len > n) break;

        if (tlv_type == PTP_TLV_REQUEST_UNICAST && tlv_len >= 6) {
            uint8_t req_msg_type = val[0] >> 4;
            int8_t log_period = (int8_t)val[1];
            uint32_t duration = ((uint32_t)val[2] << 24) | ((uint32_t)val[3] << 16) |
                                ((uint32_t)val[4] << 8) | val[5];
            if (duration == 0) duration = 300;

            tlv_out[0] = (PTP_TLV_GRANT_UNICAST >> 8) & 0xFF;
            tlv_out[1] = PTP_TLV_GRANT_UNICAST & 0xFF;
            tlv_out[2] = 0; tlv_out[3] = 8;
            tlv_out[4] = (uint8_t)(req_msg_type << 4);
            tlv_out[5] = (uint8_t)log_period;
            tlv_out[6] = (duration >> 24) & 0xFF; tlv_out[7] = (duration >> 16) & 0xFF;
            tlv_out[8] = (duration >> 8) & 0xFF;  tlv_out[9] = duration & 0xFF;
            tlv_out[10] = 0;
            tlv_out[11] = 1;                       /* renewal invited */
            tlv_out += 12;
            granted++;

            size_t kl = strlen(kinds);
            snprintf(kinds + kl, sizeof(kinds) - kl, "%s0x%x", kl ? "," : "", req_msg_type);
        }
        off += 4 + tlv_len;
    }
    if (!granted) return;

    uint16_t seq = (buf[30] << 8) | buf[31];
    uint16_t len = (uint16_t)(PTP_HDR_LEN + 10 + granted * 12);
    ptp_write_hdr(reply, PTP_MSG_SIGNALING, len, PTP_FLAG_UNICAST, ctx->clock_id,
                  seq, 0x05, 0x7F);
    memcpy(reply + PTP_HDR_LEN, buf + 20, 10);     /* target = requester's port identity */

    struct sockaddr_in dst = *src;
    dst.sin_port = htons(PTP_GENERAL_PORT);
    sendto(ctx->general_sock, reply, len, 0, (struct sockaddr *)&dst, sizeof(dst));

    pthread_mutex_lock(&ctx->lock);
    bool first = !ctx->unicast_granted;
    ctx->unicast_granted = true;
    ctx->unicast_mirror = true;                    /* serve the peer unicast from now on */
    pthread_mutex_unlock(&ctx->lock);
    if (first)
        LOG_INFO("[PTP] Granted unicast transmission to %s (msgTypes %s) — serving "
                 "Announce/Sync unicast", srcip, kinds);
    else
        LOG_DEBUG("[PTP] Renewed unicast grant for %s (msgTypes %s)", srcip, kinds);
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

/* ---- BMCA + slave (peer-driven timeline) ---- */

/* Decode a big-endian 64-bit value (clock identity / port identity). */
static uint64_t ptp_read_u64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

/* Decode a 10-byte PTP timestamp (48-bit seconds BE + 32-bit nanoseconds BE). */
static uint64_t ptp_read_ts(const uint8_t *b)
{
    uint64_t sec = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) |
                   ((uint64_t)b[2] << 24) | ((uint64_t)b[3] << 16) |
                   ((uint64_t)b[4] << 8)  |  (uint64_t)b[5];
    uint32_t nsec = ((uint32_t)b[6] << 24) | ((uint32_t)b[7] << 16) |
                    ((uint32_t)b[8] << 8)  |  (uint32_t)b[9];
    return sec * 1000000000ULL + (uint64_t)nsec;
}

/* Decode the header correctionField (bytes 8..15): a signed value in units of
 * 2^-16 ns. Return whole nanoseconds (sub-ns residue dropped). */
static int64_t ptp_read_correction_ns(const uint8_t *hdr)
{
    uint64_t raw = ptp_read_u64(hdr + 8);
    return (int64_t)raw / 65536;   /* /2^16, symmetric truncation, keeps sign */
}

/* Parse an Announce body into a best-master dataset. */
static bool ptp_parse_announce(const uint8_t *buf, int n, struct ptp_dataset *ds)
{
    if (n < PTP_HDR_LEN + 30) return false;
    const uint8_t *body = buf + PTP_HDR_LEN;
    ds->priority1 = body[13];
    ds->clock_class = body[14];
    ds->clock_accuracy = body[15];
    ds->offset_scaled_log_variance = ((uint16_t)body[16] << 8) | body[17];
    ds->priority2 = body[18];
    ds->grandmaster_identity = ptp_read_u64(body + 19);
    ds->steps_removed = ((uint16_t)body[27] << 8) | body[28];
    ds->source_port_identity = ptp_read_u64(buf + 20);
    return true;
}

/* Our own advertised dataset (mirrors what ptp_send_announce emits). */
static void ptp_own_dataset(const struct ap2_ptp_ctx *ctx, struct ptp_dataset *ds)
{
    ds->priority1 = PTP_PRIORITY1;
    ds->clock_class = PTP_CLOCK_CLASS;
    ds->clock_accuracy = PTP_CLOCK_ACCURACY;
    ds->offset_scaled_log_variance = PTP_LOG_VARIANCE;
    ds->priority2 = PTP_PRIORITY2;
    ds->grandmaster_identity = ctx->clock_id;
    ds->steps_removed = 0;
    ds->source_port_identity = ctx->clock_id;
}

/*
 * IEEE 1588-2008 dataset comparison (§9.3.2.5). Returns <0 if A is the better
 * master, >0 if B is the better master, 0 if equivalent. When the two name
 * different grandmasters (our case: our clock vs a receiver's) the
 * priority/quality vector decides, identity breaking a final tie. The
 * same-grandmaster branch is a reduced topology tiebreak (fewer stepsRemoved,
 * then lower announcing identity) — sufficient for a two-node sender/receiver
 * group and never reached while our and the peer's identities differ.
 */
static int ptp_dataset_compare(const struct ptp_dataset *a, const struct ptp_dataset *b)
{
    if (a->grandmaster_identity != b->grandmaster_identity) {
        if (a->priority1 != b->priority1)
            return a->priority1 < b->priority1 ? -1 : 1;
        if (a->clock_class != b->clock_class)
            return a->clock_class < b->clock_class ? -1 : 1;
        if (a->clock_accuracy != b->clock_accuracy)
            return a->clock_accuracy < b->clock_accuracy ? -1 : 1;
        if (a->offset_scaled_log_variance != b->offset_scaled_log_variance)
            return a->offset_scaled_log_variance < b->offset_scaled_log_variance ? -1 : 1;
        if (a->priority2 != b->priority2)
            return a->priority2 < b->priority2 ? -1 : 1;
        return a->grandmaster_identity < b->grandmaster_identity ? -1 : 1;
    }
    if (a->steps_removed != b->steps_removed)
        return a->steps_removed < b->steps_removed ? -1 : 1;
    if (a->source_port_identity != b->source_port_identity)
        return a->source_port_identity < b->source_port_identity ? -1 : 1;
    return 0;
}

/* Fold one local->master offset sample in: snap on a step, EMA on jitter. */
static void ptp_apply_offset_sample(struct ap2_ptp_ctx *ctx, int64_t raw_offset)
{
    pthread_mutex_lock(&ctx->lock);
    if (!ctx->have_offset) {
        ctx->master_offset_ns = raw_offset;
        ctx->have_offset = true;
    } else {
        int64_t delta = raw_offset - ctx->master_offset_ns;
        if (delta > PTP_OFFSET_SNAP_NS || delta < -PTP_OFFSET_SNAP_NS)
            ctx->master_offset_ns = raw_offset;
        else
            ctx->master_offset_ns += delta / PTP_OFFSET_EMA_DIV;
    }
    pthread_mutex_unlock(&ctx->lock);
}

/* Run BMCA against a freshly received peer Announce and (re)assign our role. */
static void ptp_handle_announce(struct ap2_ptp_ctx *ctx, const uint8_t *buf, int n,
                                uint64_t rx_ns, const char *srcip)
{
    struct ptp_dataset peer;
    if (!ptp_parse_announce(buf, n, &peer)) return;

    struct ptp_dataset own;
    pthread_mutex_lock(&ctx->lock);
    ptp_own_dataset(ctx, &own);
    bool first_peer = !ctx->have_peer;
    ctx->best_peer = peer;
    ctx->have_peer = true;
    ctx->last_peer_announce_ns = rx_ns;
    bool was_gm = ctx->is_grandmaster;
    /* We keep the timeline unless the peer is strictly better (cmp > 0). */
    bool now_gm = ptp_dataset_compare(&own, &peer) <= 0;
    if (ctx->hold_master && !now_gm) {
        /* Sender stays the timing authority: note the competing GM once, keep
         * announcing our own timeline. */
        if (!ctx->hold_notice_logged) {
            ctx->hold_notice_logged = true;
            pthread_mutex_unlock(&ctx->lock);
            LOG_INFO("[PTP] Peer %s announces a better dataset (gm=%016" PRIx64
                     "); holding grandmaster — the sender owns the session timeline",
                     srcip, peer.grandmaster_identity);
            pthread_mutex_lock(&ctx->lock);
        }
        now_gm = true;
    }
    ctx->is_grandmaster = now_gm;
    ctx->master_clock_id = now_gm ? ctx->clock_id : peer.grandmaster_identity;
    if (!now_gm && was_gm) {
        /* Just started slaving: drop any stale lock so the offset re-acquires
         * against the new master rather than smoothing from zero. */
        ctx->have_offset = false;
        ctx->pending_sync_valid = false;
    }
    pthread_mutex_unlock(&ctx->lock);

    if (first_peer) {
        LOG_INFO("[PTP] Peer %s clock dataset: gm=%016" PRIx64 " prio1=%u class=%u "
                 "accuracy=0x%02x variance=0x%04x prio2=%u steps=%u (ours: prio1=%u)",
                 srcip, peer.grandmaster_identity, peer.priority1, peer.clock_class,
                 peer.clock_accuracy, peer.offset_scaled_log_variance, peer.priority2,
                 peer.steps_removed, own.priority1);
    }
    if (was_gm != now_gm) {
        LOG_INFO("[PTP] BMCA role -> %s (peer %s gm=%016" PRIx64 " prio1=%u/class=%u; "
                 "ours prio1=%u/class=%u)", now_gm ? "GRANDMASTER" : "SLAVE", srcip,
                 peer.grandmaster_identity, peer.priority1, peer.clock_class,
                 own.priority1, own.clock_class);
    } else {
        LOG_DEBUG("[PTP] RX Announce from %s gm=%016" PRIx64 " prio1=%u (role unchanged: %s)",
                  srcip, peer.grandmaster_identity, peer.priority1,
                  now_gm ? "GM" : "slave");
    }
}

/*
 * Slave path, Sync. Two-step: stash reception time (t2) + correctionField keyed
 * by sequenceId to pair with the Follow_Up. One-step: originTimestamp is in the
 * Sync itself, so compute the offset immediately.
 */
static void ptp_handle_sync(struct ap2_ptp_ctx *ctx, const uint8_t *buf, int n, uint64_t rx_ns)
{
    uint16_t flags = ((uint16_t)buf[6] << 8) | buf[7];
    uint16_t seq = ((uint16_t)buf[30] << 8) | buf[31];
    int64_t corr_ns = ptp_read_correction_ns(buf);

    if (flags & PTP_FLAG_TWO_STEP) {
        pthread_mutex_lock(&ctx->lock);
        ctx->pending_sync_valid = true;
        ctx->pending_sync_seq = seq;
        ctx->pending_sync_rx_ns = rx_ns;
        ctx->pending_sync_corr_ns = corr_ns;
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    if (n < PTP_HDR_LEN + 10) return;
    uint64_t t1 = ptp_read_ts(buf + PTP_HDR_LEN);
    ptp_apply_offset_sample(ctx, (int64_t)t1 + corr_ns - (int64_t)rx_ns);
    LOG_DEBUG("[PTP] slave offset (one-step) seq=%u t1=%" PRIu64 " t2=%" PRIu64,
              seq, t1, rx_ns);
}

/*
 * Slave path, Follow_Up. Pair with the stashed two-step Sync and compute
 * local->master offset = (preciseOriginTimestamp + corr_sync + corr_fup) - t2,
 * where t2 is the Sync reception time. This is nqptp's minimal one-way math:
 * propagation delay is ignored (sub-ms and equal across receivers).
 */
static void ptp_handle_follow_up(struct ap2_ptp_ctx *ctx, const uint8_t *buf, int n)
{
    if (n < PTP_HDR_LEN + 10) return;
    uint16_t seq = ((uint16_t)buf[30] << 8) | buf[31];
    uint64_t t1 = ptp_read_ts(buf + PTP_HDR_LEN);
    int64_t corr_fup = ptp_read_correction_ns(buf);

    pthread_mutex_lock(&ctx->lock);
    bool matched = ctx->pending_sync_valid && ctx->pending_sync_seq == seq;
    uint64_t t2 = ctx->pending_sync_rx_ns;
    int64_t corr_sync = ctx->pending_sync_corr_ns;
    if (matched) ctx->pending_sync_valid = false;
    pthread_mutex_unlock(&ctx->lock);
    if (!matched) return;

    int64_t offset = (int64_t)t1 + corr_sync + corr_fup - (int64_t)t2;
    ptp_apply_offset_sample(ctx, offset);
    LOG_DEBUG("[PTP] slave offset seq=%u t1=%" PRIu64 " t2=%" PRIu64 " corr=%" PRId64
              " -> off=%" PRId64 "ns", seq, t1, t2, corr_sync + corr_fup, offset);
}

static void *ptp_thread_func(void *arg)
{
    struct ap2_ptp_ctx *ctx = (struct ap2_ptp_ctx *)arg;
    uint64_t last_announce = 0, last_sync = 0;

    while (ctx->ptp_running) {
        uint64_t now = ap2_ptp_now_ns(ctx);

        /* Read our current role once per iteration and revert to grandmaster if
         * the elected peer has gone silent (dropped off the network). */
        bool reclaimed = false;
        pthread_mutex_lock(&ctx->lock);
        if (!ctx->is_grandmaster && ctx->have_peer &&
            now - ctx->last_peer_announce_ns > PTP_PEER_SILENCE_NS) {
            ctx->is_grandmaster = true;
            ctx->master_clock_id = ctx->clock_id;
            ctx->have_peer = false;
            ctx->have_offset = false;
            ctx->pending_sync_valid = false;
            reclaimed = true;
        }
        bool gm = ctx->is_grandmaster;
        pthread_mutex_unlock(&ctx->lock);
        if (reclaimed)
            LOG_INFO("[PTP] peer silent > %llu ms; reclaiming GRANDMASTER",
                     (unsigned long long)(PTP_PEER_SILENCE_NS / 1000000ULL));

        /* Emit Announce/Sync/Follow_Up only while we hold the timeline. A slaved
         * clock stays quiet so it does not compete with the elected master. */
        if (gm) {
            if (now - last_announce >= PTP_ANNOUNCE_INTERVAL_NS) {
                ptp_send_announce(ctx);
                ptp_send_signaling(ctx);      /* iOS pairs these at 1/s */
                last_announce = now;
            }
            if (now - last_sync >= PTP_SYNC_INTERVAL_NS) {
                ptp_send_sync(ctx);
                last_sync = now;
            }
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

            /* Drop our own messages (multicast loopback / unicast mirror). */
            uint64_t src_clock = 0;
            for (int k = 0; k < 8; k++) src_clock = (src_clock << 8) | buf[20 + k];
            if (src_clock == ctx->clock_id) continue;

            switch (msg_type) {
            case PTP_MSG_DELAY_REQ:
                /* Only the grandmaster answers Delay_Req. */
                if (gm && n >= PTP_HDR_LEN + 10) {
                    LOG_DEBUG("[PTP] RX Delay_Req from %s", srcip);
                    ptp_send_delay_resp(ctx, buf, rx);
                }
                break;
            case PTP_MSG_ANNOUNCE:
                ptp_handle_announce(ctx, buf, n, rx, srcip);
                break;
            case PTP_MSG_SYNC:
                /* Peer Sync only matters when we are slaving to it. */
                if (!gm) ptp_handle_sync(ctx, buf, n, rx);
                break;
            case PTP_MSG_FOLLOW_UP:
                if (!gm) ptp_handle_follow_up(ctx, buf, n);
                break;
            case PTP_MSG_SIGNALING:
                ptp_handle_signaling(ctx, buf, n, &src, srcip);
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
    pthread_mutex_init(&ctx->lock, NULL);
    /* Default to grandmaster and hold it: the sender owns the session timeline
     * (see the hold_master field note), and the getters return a sane timeline
     * before the engine ever runs. */
    ctx->is_grandmaster = true;
    ctx->hold_master = true;
    /* Apple receivers consume the session clock as UNICAST PTP sent straight
     * to them (the nqptp model — they never join an open multicast election),
     * so mirror Announce/Sync/Follow_Up unicast to every timing peer from the
     * start. Signaling REQUEST_UNICAST_TRANSMISSION is granted as well for
     * stacks that negotiate explicitly. */
    ctx->unicast_mirror = true;
    return ctx;
}

void ap2_ptp_destroy(struct ap2_ptp_ctx *ctx)
{
    if (!ctx) return;
    ap2_ptp_stop(ctx);
    if (ctx->shm_active) {
        if (ctx->shared_ip) ap2_ptp_shared_unregister(ctx, ctx->shared_ip);
        ap2_ptp_shm_reader_close(&ctx->shm_reader);
        ctx->shm_active = false;
    }
    free(ctx->shared_ip);
    for (int i = 0; i < ctx->npeers; i++) free(ctx->peers[i]);
    free(ctx->device_ip);
    pthread_mutex_destroy(&ctx->lock);
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

    /* Start as grandmaster of our own clock. With hold_master (default) we
     * keep it for the whole session; only with holding disabled can BMCA hand
     * the timeline to a peer announcing a strictly better dataset. */
    pthread_mutex_lock(&ctx->lock);
    ctx->is_grandmaster = true;
    ctx->master_clock_id = ctx->clock_id;
    ctx->master_offset_ns = 0;
    ctx->have_offset = false;
    ctx->have_peer = false;
    ctx->pending_sync_valid = false;
    pthread_mutex_unlock(&ctx->lock);

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

    LOG_INFO("[PTP] Engine started on UDP 319/320, clockID=%016" PRIx64
             ", mcast=%s, iface=%s", ctx->clock_id, PTP_MCAST_ADDR,
             ctx->bind_addr.s_addr == INADDR_ANY ? "default" : inet_ntoa(ctx->bind_addr));
    return true;
}

void ap2_ptp_engine_settle(struct ap2_ptp_ctx *ctx, int timeout_ms)
{
    if (!ctx || timeout_ms <= 0) return;

    if (ctx->shm_active) {
        /* Shared mode: wait for the daemon to advance its publish counter so we
         * build the SETUP against a fresh, live clock rather than a stale one. */
        uint64_t deadline = ap2_ptp_now_ns(ctx) + (uint64_t)timeout_ms * 1000000ULL;
        struct ap2_ptp_shm_sample first;
        bool have_first = ap2_ptp_shm_read(&ctx->shm_reader, &first);
        while (ap2_ptp_now_ns(ctx) < deadline) {
            struct ap2_ptp_shm_sample s;
            if (ap2_ptp_shm_read(&ctx->shm_reader, &s) &&
                (!have_first || s.update_count != first.update_count) &&
                ((s.flags & AP2_PTP_SHM_F_GRANDMASTER) ||
                 (s.flags & AP2_PTP_SHM_F_OFFSET_LOCKED)))
                break;
            usleep(10000);
        }
        struct ap2_ptp_shm_sample s;
        if (ap2_ptp_shm_read(&ctx->shm_reader, &s))
            LOG_INFO("[PTP] shared clock settled: timeline=%016" PRIx64 " offset=%" PRId64
                     "ns role=%s", s.master_clock_id, s.local_to_master_offset,
                     (s.flags & AP2_PTP_SHM_F_GRANDMASTER) ? "daemon-GM" : "daemon-slave");
        return;
    }

    if (!ctx->engine_active) return;

    uint64_t deadline = ap2_ptp_now_ns(ctx) + (uint64_t)timeout_ms * 1000000ULL;
    while (ap2_ptp_now_ns(ctx) < deadline) {
        pthread_mutex_lock(&ctx->lock);
        /* A coherent decision: either a peer appeared and we out-ranked it, or
         * we are slaving to it and have locked at least one offset sample. */
        bool decided = ctx->have_peer && (ctx->is_grandmaster || ctx->have_offset);
        pthread_mutex_unlock(&ctx->lock);
        if (decided) break;
        usleep(10000);   /* 10 ms poll */
    }

    pthread_mutex_lock(&ctx->lock);
    LOG_INFO("[PTP] BMCA settled: %s, timeline=%016" PRIx64 "%s",
             ctx->is_grandmaster ? "GRANDMASTER" : "SLAVE", ctx->master_clock_id,
             (!ctx->is_grandmaster && ctx->have_offset) ? " (offset locked)" :
             (ctx->have_peer ? "" : " (no peer seen)"));
    pthread_mutex_unlock(&ctx->lock);
}

uint64_t ap2_ptp_master_clock_id(struct ap2_ptp_ctx *ctx)
{
    if (!ctx) return 0;
    if (ctx->shm_active) {
        struct ap2_ptp_shm_sample s;
        if (ap2_ptp_shm_read(&ctx->shm_reader, &s)) return s.master_clock_id;
        return ctx->clock_id;   /* daemon clock briefly unreadable: our own id */
    }
    pthread_mutex_lock(&ctx->lock);
    uint64_t id = ctx->is_grandmaster ? ctx->clock_id : ctx->master_clock_id;
    pthread_mutex_unlock(&ctx->lock);
    return id;
}

uint64_t ap2_ptp_master_now_ns(struct ap2_ptp_ctx *ctx)
{
    uint64_t local = ap2_ptp_now_ns(ctx);
    if (!ctx) return local;
    if (ctx->shm_active) {
        /* The daemon publishes a local->master offset; the daemon and this
         * process share CLOCK_REALTIME, so master-now is our own now + offset. */
        struct ap2_ptp_shm_sample s;
        int64_t off = ap2_ptp_shm_read(&ctx->shm_reader, &s) ? s.local_to_master_offset : 0;
        return (uint64_t)((int64_t)local + off);
    }
    pthread_mutex_lock(&ctx->lock);
    int64_t off = ctx->is_grandmaster ? 0 : ctx->master_offset_ns;
    pthread_mutex_unlock(&ctx->lock);
    return (uint64_t)((int64_t)local + off);
}

/* ---- shared daemon clock: streaming-side attach + peer registration ---- */

bool ap2_ptp_shared_active(struct ap2_ptp_ctx *ctx)
{
    return ctx && ctx->shm_active;
}

bool ap2_ptp_attach_shared(struct ap2_ptp_ctx *ctx)
{
    if (!ctx) return false;

    /* Prove a LIVE daemon first (control ping), so a stale shm left by a crashed
     * daemon cannot fool us into shared mode. */
    char ack[256] = {0};
    if (!ap2_ptp_ctrl_send("?", 250, ack, sizeof(ack))) {
        LOG_INFO("[PTP] No PTP daemon on the control channel; using in-process engine");
        return false;
    }
    if (!ap2_ptp_shm_reader_open(&ctx->shm_reader)) {
        LOG_WARN("[PTP] Daemon answered but its shared clock is unreadable; "
                 "using in-process engine");
        return false;
    }
    struct ap2_ptp_shm_sample s;
    if (!ap2_ptp_shm_read(&ctx->shm_reader, &s)) {
        LOG_WARN("[PTP] Daemon shared clock present but no sample yet; "
                 "using in-process engine");
        ap2_ptp_shm_reader_close(&ctx->shm_reader);
        return false;
    }
    ctx->shm_active = true;
    LOG_INFO("[PTP] Attached shared daemon clock (%s); timeline=%016" PRIx64
             ", not binding 319/320", ack, s.master_clock_id);
    return true;
}

void ap2_ptp_shared_register(struct ap2_ptp_ctx *ctx, const char *ip)
{
    if (!ctx || !ctx->shm_active || !ip || !*ip) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "R %s", ip);
    char ack[128] = {0};
    bool ok = ap2_ptp_ctrl_send(cmd, 250, ack, sizeof(ack));
    if (!ctx->shared_ip) ctx->shared_ip = strdup(ip);
    LOG_INFO("[PTP] Registered receiver %s with daemon -> %s", ip, ok ? ack : "(no ack)");
}

void ap2_ptp_shared_unregister(struct ap2_ptp_ctx *ctx, const char *ip)
{
    if (!ctx || !ip || !*ip) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "U %s", ip);
    ap2_ptp_ctrl_send(cmd, 100, NULL, 0);
    LOG_DEBUG("[PTP] Unregistered receiver %s from daemon", ip);
}

/* ---- PTP daemon ---- */

/* Aggregate receiver peer set, refcounted so independent streams to the SAME
 * receiver register/unregister cleanly. */
struct ptp_peerset {
    struct { char ip[INET_ADDRSTRLEN]; int refs; } e[PTP_MAX_PEERS];
    int n;
};

static void peerset_apply(struct ptp_peerset *ps, struct ap2_ptp_ctx *ctx)
{
    const char *ips[PTP_MAX_PEERS];
    int n = 0;
    for (int i = 0; i < ps->n; i++)
        if (ps->e[i].refs > 0) ips[n++] = ps->e[i].ip;
    ap2_ptp_set_peers(ctx, ips, n);
}

static void peerset_add(struct ptp_peerset *ps, const char *ip)
{
    for (int i = 0; i < ps->n; i++)
        if (strcmp(ps->e[i].ip, ip) == 0) { ps->e[i].refs++; return; }
    if (ps->n >= PTP_MAX_PEERS) {
        LOG_WARN("[PTP] peer set full (%d); ignoring %s", PTP_MAX_PEERS, ip);
        return;
    }
    snprintf(ps->e[ps->n].ip, sizeof(ps->e[ps->n].ip), "%s", ip);
    ps->e[ps->n].refs = 1;
    ps->n++;
}

static void peerset_remove(struct ptp_peerset *ps, const char *ip)
{
    for (int i = 0; i < ps->n; i++) {
        if (strcmp(ps->e[i].ip, ip) == 0) {
            if (ps->e[i].refs > 0) ps->e[i].refs--;
            return;
        }
    }
}

static int peerset_count(const struct ptp_peerset *ps)
{
    int c = 0;
    for (int i = 0; i < ps->n; i++) if (ps->e[i].refs > 0) c++;
    return c;
}

/* Snapshot the engine's elected clock into a shm sample, atomically. */
static void daemon_fill_sample(struct ap2_ptp_ctx *ctx, uint64_t start_ns,
                               struct ap2_ptp_shm_sample *out)
{
    uint64_t local = ap2_ptp_now_ns(ctx);
    pthread_mutex_lock(&ctx->lock);
    bool gm = ctx->is_grandmaster;
    uint64_t cid = gm ? ctx->clock_id : ctx->master_clock_id;
    int64_t off = gm ? 0 : ctx->master_offset_ns;
    bool locked = gm ? true : ctx->have_offset;
    pthread_mutex_unlock(&ctx->lock);

    memset(out, 0, sizeof(*out));
    out->master_clock_id = cid;
    out->local_time = local;
    out->local_to_master_offset = off;
    out->master_clock_start_time = start_ns;
    out->flags = (gm ? AP2_PTP_SHM_F_GRANDMASTER : 0u) |
                 (locked ? AP2_PTP_SHM_F_OFFSET_LOCKED : 0u);
}

/* Read and act on one control datagram; reply with a status ack. */
static void daemon_handle_ctrl(int sock, struct ptp_peerset *ps, struct ap2_ptp_ctx *ctx)
{
    char buf[512];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &sl);
    if (n <= 0) return;
    buf[n] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " \t\r\n", &save);
    if (!tok) return;
    char cmd = tok[0];
    bool changed = false;

    switch (cmd) {
    case 'R':   /* register (add) */
    case 'T': { /* nqptp-compatible alias: ADD (see header note) */
        char *ip;
        while ((ip = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            struct in_addr tmp;
            if (inet_pton(AF_INET, ip, &tmp) == 1) { peerset_add(ps, ip); changed = true; }
        }
        break;
    }
    case 'U': { /* unregister (remove) */
        char *ip;
        while ((ip = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            peerset_remove(ps, ip);
            changed = true;
        }
        break;
    }
    case 'B': case 'E': case 'P':   /* begin/end/pause: no-ops for a sender GM */
    case '?':                        /* liveness probe */
        break;
    default:
        break;
    }

    if (changed) peerset_apply(ps, ctx);

    /* Ack with the current group state so a client can confirm the daemon is
     * live and see the elected grandmaster. */
    pthread_mutex_lock(&ctx->lock);
    bool gm = ctx->is_grandmaster;
    uint64_t cid = gm ? ctx->clock_id : ctx->master_clock_id;
    pthread_mutex_unlock(&ctx->lock);
    char ack[128];
    int an = snprintf(ack, sizeof(ack), "OK peers=%d gm=%016" PRIx64 " role=%s",
                      peerset_count(ps), cid, gm ? "grandmaster" : "slave");
    sendto(sock, ack, an, 0, (struct sockaddr *)&src, sl);
}

int ap2_ptp_run_daemon(struct in_addr bind_addr, volatile bool *stop)
{
    struct ap2_ptp_ctx *ctx = ap2_ptp_create();
    if (!ctx) { LOG_ERROR("[PTP] daemon: cannot create context"); return 1; }

    struct ap2_ptp_shm_writer w;
    if (!ap2_ptp_shm_writer_open(&w)) {
        ap2_ptp_destroy(ctx);
        return 1;
    }

    int ctrl = ap2_ptp_ctrl_server_open();
    if (ctrl < 0) {
        LOG_ERROR("[PTP] daemon: control channel unavailable (another daemon running?)");
        ap2_ptp_shm_writer_close(&w);
        ap2_ptp_destroy(ctx);
        return 1;
    }

    if (!ap2_ptp_engine_start(ctx, bind_addr, NULL)) {
        LOG_ERROR("[PTP] daemon: cannot bind UDP 319/320 (run as root / grant "
                  "CAP_NET_BIND_SERVICE). Multi-room PTP sync is unavailable.");
        close(ctrl);
        ap2_ptp_shm_writer_close(&w);
        ap2_ptp_destroy(ctx);
        return 2;
    }

    uint64_t start_ns = ap2_ptp_now_ns(ctx);
    struct ptp_peerset ps = {0};
    LOG_INFO("[PTP] daemon up: engine on 319/320, clock in %s, control on %s:%d",
             AP2_PTP_SHM_NAME, AP2_PTP_CTRL_ADDR, AP2_PTP_CTRL_PORT);

    /* Publish immediately so an early-attaching stream sees a live sample. */
    struct ap2_ptp_shm_sample sample;
    daemon_fill_sample(ctx, start_ns, &sample);
    ap2_ptp_shm_publish(&w, sample);

    while (!*stop) {
        struct pollfd pfd = {.fd = ctrl, .events = POLLIN};
        int pr = poll(&pfd, 1, 100);   /* ~10 Hz publish cadence */
        if (pr > 0 && (pfd.revents & POLLIN))
            daemon_handle_ctrl(ctrl, &ps, ctx);
        daemon_fill_sample(ctx, start_ns, &sample);
        ap2_ptp_shm_publish(&w, sample);
    }

    LOG_INFO("[PTP] daemon shutting down");
    close(ctrl);
    ap2_ptp_shm_writer_close(&w);
    ap2_ptp_destroy(ctx);
    return 0;
}
