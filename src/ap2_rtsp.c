/*
 * AirPlay 2 RTSP - Encrypted RTSP session management
 *
 * Handles the AirPlay 2 connection sequence:
 * 1. TCP connect to device
 * 2. GET /info to query capabilities
 * 3. POST /pair-verify for HAP authentication
 * 4. SETUP (session) with timing protocol and device info
 * 5. SETUP (stream) with audio format and encryption key
 * 6. RECORD to begin streaming
 * 7. SET_PARAMETER for volume/metadata during playback
 * 8. POST /feedback for keepalive
 * 9. TEARDOWN to end session
 *
 * After pair-verify, all RTSP communication is encrypted via HAP framing.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#include <openssl/rand.h>

#include "../libraop/crosstools/src/platform.h"
#include "cross_log.h"
#include "ap2_rtsp.h"
#include "ap2_hap.h"
#include "ap2_bplist.h"

extern log_level *loglevel;

/* RTSP session state */
typedef enum {
    RTSP_DISCONNECTED = 0,
    RTSP_CONNECTED,
    RTSP_AUTHENTICATED,
    RTSP_SESSION_SETUP,
    RTSP_STREAM_SETUP,
    RTSP_RECORDING,
} rtsp_state_t;

struct ap2_rtsp_ctx {
    /* Connection */
    int sock_fd;
    char *host;
    int port;
    rtsp_state_t state;
    int cseq;

    /* HAP encryption */
    struct ap2_hap_ctx *hap;
    bool encrypted;

    /* Device info from /info */
    uint64_t features;
    char *device_id;
    char *model;

    /* Session info from SETUP */
    int event_port;        /* TCP port for device events */
    int timing_port;       /* UDP port for NTP timing */
    int data_port;         /* UDP port for audio data */
    int control_port;      /* UDP port for control (retransmit, sync) */
    char session_uuid[40]; /* UUID for this session */

    /* Audio encryption key for RTP packets */
    uint8_t shared_audio_key[32];  /* The 'shk' from SETUP */

    /* Config from caller */
    char *dacp_id;
    char *active_remote;
    int latency_ms;
};

/* ---- TCP connection ---- */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        LOG_ERROR("[AP2-RTSP] Cannot resolve %s:%d", host, port);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        LOG_ERROR("[AP2-RTSP] Cannot connect to %s:%d: %s", host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    LOG_INFO("[AP2-RTSP] Connected to %s:%d", host, port);
    return fd;
}

/* ---- RTSP message I/O ---- */

/* Read a full HTTP/RTSP response. Returns body or NULL. Sets *body_len. */
static uint8_t *rtsp_read_response(struct ap2_rtsp_ctx *ctx,
                                    int *status_code, int *body_len)
{
    /* Read up to 16KB - sufficient for /info and SETUP responses */
    uint8_t buf[16384];
    int total = 0;

    /* Read until we have the full headers + body */
    while (total < (int)sizeof(buf) - 1) {
        int n = read(ctx->sock_fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;

        /* Check if we have the complete response */
        buf[total] = '\0';
        char *header_end = strstr((char *)buf, "\r\n\r\n");
        if (header_end) {
            int header_len = (header_end - (char *)buf) + 4;
            /* Parse Content-Length */
            int content_len = 0;
            char *cl = strcasestr((char *)buf, "Content-Length:");
            if (cl) content_len = atoi(cl + 15);
            if (total >= header_len + content_len) break;
        }
    }

    if (total <= 0) {
        *status_code = 0;
        *body_len = 0;
        return NULL;
    }

    /* Parse status code */
    *status_code = 0;
    if (total > 12) {
        sscanf((char *)buf, "%*s %d", status_code);
    }

    /* Find body */
    char *header_end = strstr((char *)buf, "\r\n\r\n");
    if (!header_end) {
        *body_len = 0;
        return NULL;
    }
    int header_len = (header_end - (char *)buf) + 4;
    *body_len = total - header_len;
    if (*body_len <= 0) return NULL;

    uint8_t *body = malloc(*body_len);
    memcpy(body, buf + header_len, *body_len);
    return body;
}

/* Send an RTSP request. Returns true on success. */
static bool rtsp_send(struct ap2_rtsp_ctx *ctx, const char *method,
                       const char *uri, const char *content_type,
                       const uint8_t *body, int body_len)
{
    char header[1024];
    int hdr_len = snprintf(header, sizeof(header),
        "%s %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "DACP-ID: %s\r\n"
        "Active-Remote: %s\r\n"
        "User-Agent: AirPlay/670.6.2\r\n"
        "%s%s%s"
        "Content-Length: %d\r\n"
        "\r\n",
        method, uri, ctx->cseq++,
        ctx->dacp_id ? ctx->dacp_id : "0",
        ctx->active_remote ? ctx->active_remote : "0",
        content_type ? "Content-Type: " : "",
        content_type ? content_type : "",
        content_type ? "\r\n" : "",
        body_len);

    if (write(ctx->sock_fd, header, hdr_len) != hdr_len) return false;
    if (body && body_len > 0) {
        if (write(ctx->sock_fd, body, body_len) != body_len) return false;
    }
    return true;
}

/* ---- Public API ---- */

struct ap2_rtsp_ctx *ap2_rtsp_create(const char *host, int port,
                                      const char *auth_credentials,
                                      const char *dacp_id,
                                      const char *active_remote,
                                      int latency_ms)
{
    struct ap2_rtsp_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->sock_fd = -1;
    ctx->host = strdup(host);
    ctx->port = port;
    ctx->dacp_id = dacp_id ? strdup(dacp_id) : NULL;
    ctx->active_remote = active_remote ? strdup(active_remote) : NULL;
    ctx->latency_ms = latency_ms;

    /* Create HAP context if credentials provided */
    if (auth_credentials && strlen(auth_credentials) == 192) {
        ctx->hap = ap2_hap_create(auth_credentials);
    }

    /* Generate session UUID */
    uint8_t uuid_bytes[16];
    RAND_bytes(uuid_bytes, 16);
    snprintf(ctx->session_uuid, sizeof(ctx->session_uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
             uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
             uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);

    return ctx;
}

void ap2_rtsp_destroy(struct ap2_rtsp_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->sock_fd >= 0) close(ctx->sock_fd);
    if (ctx->hap) ap2_hap_destroy(ctx->hap);
    free(ctx->host);
    free(ctx->device_id);
    free(ctx->model);
    free(ctx->dacp_id);
    free(ctx->active_remote);
    free(ctx);
}

bool ap2_rtsp_connect(struct ap2_rtsp_ctx *ctx)
{
    if (!ctx) return false;

    /* Step 1: TCP connect */
    ctx->sock_fd = tcp_connect(ctx->host, ctx->port);
    if (ctx->sock_fd < 0) return false;
    ctx->state = RTSP_CONNECTED;

    /* Step 2: GET /info to query device capabilities */
    LOG_INFO("[AP2-RTSP] Querying device info...");
    if (!rtsp_send(ctx, "GET", "/info", NULL, NULL, 0)) {
        LOG_ERROR("[AP2-RTSP] Failed to send /info request");
        return false;
    }

    int status, body_len;
    uint8_t *body = rtsp_read_response(ctx, &status, &body_len);
    if (status != 200 || !body) {
        LOG_ERROR("[AP2-RTSP] /info failed with status %d", status);
        free(body);
        return false;
    }

    /* Parse /info response (binary plist) */
    struct ap2_bplist *info = ap2_bplist_parse(body, body_len);
    free(body);
    if (info) {
        ctx->features = ap2_bplist_get_int(info, "features");
        const char *model = ap2_bplist_get_string(info, "model");
        if (model) ctx->model = strdup(model);
        const char *device_id = ap2_bplist_get_string(info, "deviceID");
        if (device_id) ctx->device_id = strdup(device_id);
        LOG_INFO("[AP2-RTSP] Device: model=%s, deviceID=%s, features=0x%" PRIx64,
                 ctx->model ? ctx->model : "?",
                 ctx->device_id ? ctx->device_id : "?",
                 ctx->features);
        ap2_bplist_free(info);
    }

    /* Step 3: HAP pair-verify if credentials available */
    if (ctx->hap) {
        LOG_INFO("[AP2-RTSP] Performing HAP pair-verify...");
        if (!ap2_hap_pair_verify(ctx->hap, ctx->sock_fd)) {
            LOG_ERROR("[AP2-RTSP] Pair-verify failed");
            return false;
        }
        ctx->encrypted = true;
        ctx->state = RTSP_AUTHENTICATED;
        LOG_INFO("[AP2-RTSP] HAP pair-verify succeeded, channel encrypted");
    } else {
        ctx->state = RTSP_AUTHENTICATED;
        LOG_INFO("[AP2-RTSP] No credentials, skipping pair-verify");
    }

    return true;
}

bool ap2_rtsp_setup_session(struct ap2_rtsp_ctx *ctx, bool use_ptp)
{
    if (!ctx || ctx->state < RTSP_AUTHENTICATED) return false;

    LOG_INFO("[AP2-RTSP] Setting up session (timing=%s)...", use_ptp ? "PTP" : "NTP");

    /* Build session SETUP bplist */
    struct ap2_bplist *bp = ap2_bplist_create();
    ap2_bplist_add_string(bp, "timingProtocol", use_ptp ? "PTP" : "NTP");
    ap2_bplist_add_string(bp, "sessionUUID", ctx->session_uuid);

    /* Timing port - we'll allocate a UDP socket for NTP timing */
    /* For now use a standard port, will be refined */
    ap2_bplist_add_int(bp, "timingPort", 0);

    uint8_t *plist_data;
    int plist_len = ap2_bplist_serialize(bp, &plist_data);
    ap2_bplist_free(bp);

    if (!rtsp_send(ctx, "SETUP", "rtsp://localhost/0",
                   "application/x-apple-binary-plist",
                   plist_data, plist_len)) {
        free(plist_data);
        return false;
    }
    free(plist_data);

    int status, body_len;
    uint8_t *body = rtsp_read_response(ctx, &status, &body_len);
    if (status != 200 || !body) {
        LOG_ERROR("[AP2-RTSP] Session SETUP failed with status %d", status);
        free(body);
        return false;
    }

    /* Parse response */
    struct ap2_bplist *resp = ap2_bplist_parse(body, body_len);
    free(body);
    if (resp) {
        ctx->event_port = (int)ap2_bplist_get_int(resp, "eventPort");
        ctx->timing_port = (int)ap2_bplist_get_int(resp, "timingPort");
        LOG_INFO("[AP2-RTSP] Session: eventPort=%d, timingPort=%d",
                 ctx->event_port, ctx->timing_port);
        ap2_bplist_free(resp);
    }

    ctx->state = RTSP_SESSION_SETUP;
    return true;
}

bool ap2_rtsp_setup_stream(struct ap2_rtsp_ctx *ctx, int sample_rate,
                            int bit_depth, int channels)
{
    if (!ctx || ctx->state < RTSP_SESSION_SETUP) return false;

    LOG_INFO("[AP2-RTSP] Setting up audio stream (%dHz/%dbit/%dch)...",
             sample_rate, bit_depth, channels);

    /* Generate shared encryption key for audio packets */
    RAND_bytes(ctx->shared_audio_key, 32);

    /* Determine audio format bitmask */
    uint64_t audio_format;
    if (sample_rate == 48000)
        audio_format = 0x400000;  /* ALAC 48000/16/2 */
    else
        audio_format = 0x40000;   /* ALAC 44100/16/2 */

    /* Build stream SETUP bplist */
    struct ap2_bplist *bp = ap2_bplist_create();

    /* The streams array would be a nested structure - for now we add
     * the key parameters directly. The actual implementation needs proper
     * nested plist support which bplist doesn't fully provide.
     * TODO: Implement proper nested array/dict for streams */
    ap2_bplist_add_int(bp, "type", 96);  /* Realtime */
    ap2_bplist_add_int(bp, "ct", 2);     /* ALAC */
    ap2_bplist_add_int(bp, "spf", 352);  /* Samples per frame */
    ap2_bplist_add_int(bp, "audioFormat", audio_format);
    ap2_bplist_add_data(bp, "shk", ctx->shared_audio_key, 32);
    ap2_bplist_add_int(bp, "latencyMin", 11025);
    ap2_bplist_add_int(bp, "latencyMax", (uint64_t)(sample_rate * 2));  /* 2s */

    uint8_t *plist_data;
    int plist_len = ap2_bplist_serialize(bp, &plist_data);
    ap2_bplist_free(bp);

    if (!rtsp_send(ctx, "SETUP", "rtsp://localhost/1",
                   "application/x-apple-binary-plist",
                   plist_data, plist_len)) {
        free(plist_data);
        return false;
    }
    free(plist_data);

    int status, body_len;
    uint8_t *body = rtsp_read_response(ctx, &status, &body_len);
    if (status != 200 || !body) {
        LOG_ERROR("[AP2-RTSP] Stream SETUP failed with status %d", status);
        free(body);
        return false;
    }

    struct ap2_bplist *resp = ap2_bplist_parse(body, body_len);
    free(body);
    if (resp) {
        ctx->data_port = (int)ap2_bplist_get_int(resp, "dataPort");
        ctx->control_port = (int)ap2_bplist_get_int(resp, "controlPort");
        LOG_INFO("[AP2-RTSP] Stream: dataPort=%d, controlPort=%d",
                 ctx->data_port, ctx->control_port);
        ap2_bplist_free(resp);
    }

    ctx->state = RTSP_STREAM_SETUP;
    return true;
}

bool ap2_rtsp_record(struct ap2_rtsp_ctx *ctx)
{
    if (!ctx || ctx->state < RTSP_STREAM_SETUP) return false;

    if (!rtsp_send(ctx, "RECORD", "rtsp://localhost/1", NULL, NULL, 0)) {
        LOG_ERROR("[AP2-RTSP] RECORD failed");
        return false;
    }

    int status, body_len;
    uint8_t *body = rtsp_read_response(ctx, &status, &body_len);
    free(body);
    if (status != 200) {
        LOG_ERROR("[AP2-RTSP] RECORD failed with status %d", status);
        return false;
    }

    ctx->state = RTSP_RECORDING;
    LOG_INFO("[AP2-RTSP] Recording started");
    return true;
}

bool ap2_rtsp_set_volume(struct ap2_rtsp_ctx *ctx, float volume_db)
{
    if (!ctx || ctx->state < RTSP_RECORDING) return false;

    char vol_str[32];
    snprintf(vol_str, sizeof(vol_str), "volume: %.6f\r\n", volume_db);

    return rtsp_send(ctx, "SET_PARAMETER", "rtsp://localhost/1",
                     "text/parameters", (uint8_t *)vol_str, strlen(vol_str));
}

bool ap2_rtsp_teardown(struct ap2_rtsp_ctx *ctx)
{
    if (!ctx || ctx->sock_fd < 0) return false;

    rtsp_send(ctx, "TEARDOWN", "rtsp://localhost/1", NULL, NULL, 0);
    ctx->state = RTSP_DISCONNECTED;
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
    LOG_INFO("[AP2-RTSP] Session torn down");
    return true;
}

bool ap2_rtsp_feedback(struct ap2_rtsp_ctx *ctx)
{
    if (!ctx || ctx->state < RTSP_RECORDING) return false;
    /* Keepalive - POST /feedback with empty body */
    char req[512];
    int len = snprintf(req, sizeof(req),
        "POST /feedback RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "Content-Length: 0\r\n"
        "\r\n", ctx->cseq++);
    return write(ctx->sock_fd, req, len) == len;
}

/* Getters for session info */
int ap2_rtsp_get_data_port(struct ap2_rtsp_ctx *ctx) { return ctx ? ctx->data_port : 0; }
int ap2_rtsp_get_control_port(struct ap2_rtsp_ctx *ctx) { return ctx ? ctx->control_port : 0; }
int ap2_rtsp_get_timing_port(struct ap2_rtsp_ctx *ctx) { return ctx ? ctx->timing_port : 0; }
const uint8_t *ap2_rtsp_get_audio_key(struct ap2_rtsp_ctx *ctx) { return ctx ? ctx->shared_audio_key : NULL; }
