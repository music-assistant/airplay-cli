/*
 * AirPlay 2 Native Session
 *
 * Manages the full AP2 connection lifecycle:
 * - TCP connect + /info capability detection
 * - HAP pair-verify with stored credentials (Apple devices)
 * - Encrypted RTSP channel via HAP framing
 * - Binary plist SETUP with proper streams array
 * - Fallback to RAOP-compatible flow when possible
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
#include "ap2_session.h"
#include "ap2_hap.h"

extern log_level *loglevel;

#define MAX_RTSP_BUF 16384
#define SESSION_URL_SIZE 128

struct ap2_session {
    /* Connection */
    int sock_fd;
    char *host;
    int port;
    int cseq;

    /* Session identity */
    char session_url[SESSION_URL_SIZE];
    uint32_t session_id;
    char session_uuid[40];

    /* Authentication */
    struct ap2_hap_ctx *hap;
    bool encrypted;    /* True after successful pair-verify + key derivation */

    /* Device capabilities (from /info) */
    ap2_device_caps_t caps;
    ap2_flow_mode_t flow;

    /* DACP identifiers */
    char *dacp_id;
    char *active_remote;

    /* Negotiated ports */
    int event_port;
    int timing_port;
    int data_port;
    int control_port;
};

/* ---- TCP I/O helpers ---- */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Read a complete RTSP/HTTP response. Caller frees *body. */
static int read_response(int fd, uint8_t *buf, int buf_size,
                          int *status, uint8_t **body, int *body_len)
{
    int total = 0;
    while (total < buf_size - 1) {
        int n = read(fd, buf + total, buf_size - total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *end = strstr((char *)buf, "\r\n\r\n");
        if (end) {
            int hdr_len = (end - (char *)buf) + 4;
            int cl = 0;
            char *clh = strcasestr((char *)buf, "Content-Length:");
            if (clh) cl = atoi(clh + 15);
            if (total >= hdr_len + cl) {
                *status = 0;
                sscanf((char *)buf, "%*s %d", status);
                *body_len = total - hdr_len;
                if (*body_len > 0) {
                    *body = malloc(*body_len);
                    memcpy(*body, buf + hdr_len, *body_len);
                } else {
                    *body = NULL;
                }
                return hdr_len;
            }
        }
    }
    *status = 0; *body = NULL; *body_len = 0;
    return 0;
}

/* ---- Raw RTSP send (unencrypted) ---- */

static int send_rtsp_raw(struct ap2_session *s, const char *method, const char *uri,
                          const uint8_t *body, int body_len, const char *content_type,
                          uint8_t **resp_body, int *resp_len)
{
    char hdr[1024];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "User-Agent: AirPlay/670.6.2\r\n"
        "DACP-ID: %s\r\n"
        "Active-Remote: %s\r\n"
        "%s%s%s"
        "Content-Length: %d\r\n"
        "\r\n",
        method, uri, s->cseq++,
        s->dacp_id ? s->dacp_id : "0",
        s->active_remote ? s->active_remote : "0",
        content_type ? "Content-Type: " : "",
        content_type ? content_type : "",
        content_type ? "\r\n" : "",
        body_len);

    if (write(s->sock_fd, hdr, hdr_len) != hdr_len) return 0;
    if (body && body_len > 0 && write(s->sock_fd, body, body_len) != body_len) return 0;

    uint8_t buf[MAX_RTSP_BUF];
    int status;
    read_response(s->sock_fd, buf, sizeof(buf), &status, resp_body, resp_len);
    return status;
}

/* ---- HAP-encrypted RTSP send ---- */

static int send_rtsp_encrypted(struct ap2_session *s, const char *method, const char *uri,
                                const uint8_t *body, int body_len, const char *content_type,
                                uint8_t **resp_body, int *resp_len)
{
    if (!s->hap) return send_rtsp_raw(s, method, uri, body, body_len, content_type, resp_body, resp_len);

    /* Build the RTSP message as a single buffer */
    char hdr[1024];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "User-Agent: AirPlay/670.6.2\r\n"
        "DACP-ID: %s\r\n"
        "Active-Remote: %s\r\n"
        "%s%s%s"
        "Content-Length: %d\r\n"
        "\r\n",
        method, uri, s->cseq++,
        s->dacp_id ? s->dacp_id : "0",
        s->active_remote ? s->active_remote : "0",
        content_type ? "Content-Type: " : "",
        content_type ? content_type : "",
        content_type ? "\r\n" : "",
        body_len);

    int msg_len = hdr_len + body_len;
    uint8_t *msg = malloc(msg_len);
    memcpy(msg, hdr, hdr_len);
    if (body && body_len > 0) memcpy(msg + hdr_len, body, body_len);

    /* Encrypt via HAP framing */
    uint8_t *encrypted = NULL;
    int enc_len = ap2_hap_encrypt(s->hap, msg, msg_len, &encrypted);
    free(msg);

    if (enc_len <= 0 || !encrypted) {
        LOG_ERROR("[AP2-S] HAP encrypt failed");
        free(encrypted);
        return 0;
    }

    if (write(s->sock_fd, encrypted, enc_len) != enc_len) {
        free(encrypted);
        return 0;
    }
    free(encrypted);

    /* Read encrypted response */
    uint8_t enc_buf[MAX_RTSP_BUF];
    int total = 0;
    struct timeval tv = {.tv_sec = 8};
    setsockopt(s->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read HAP frames until we have a complete RTSP response */
    while (total < MAX_RTSP_BUF - 1) {
        int n = read(s->sock_fd, enc_buf + total, sizeof(enc_buf) - total);
        if (n <= 0) break;
        total += n;
        /* Try to decrypt to see if we have enough */
        uint8_t *dec = NULL;
        int dec_len = ap2_hap_decrypt(s->hap, enc_buf, total, &dec);
        if (dec_len > 0 && dec) {
            /* Check if we have a complete RTSP response */
            if (strstr((char *)dec, "\r\n\r\n")) {
                int status = 0;
                sscanf((char *)dec, "%*s %d", &status);
                char *hdr_end = strstr((char *)dec, "\r\n\r\n");
                int h_len = (hdr_end - (char *)dec) + 4;
                *resp_len = dec_len - h_len;
                if (*resp_len > 0) {
                    *resp_body = malloc(*resp_len);
                    memcpy(*resp_body, dec + h_len, *resp_len);
                } else {
                    *resp_body = NULL;
                }
                free(dec);
                return status;
            }
            free(dec);
        }
    }

    *resp_body = NULL; *resp_len = 0;
    return 0;
}

/* ---- Public API ---- */

struct ap2_session *ap2_session_create(const char *host, int port,
                                       const char *auth_credentials,
                                       const char *dacp_id,
                                       const char *active_remote)
{
    struct ap2_session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->sock_fd = -1;
    s->host = strdup(host);
    s->port = port;
    if (dacp_id) s->dacp_id = strdup(dacp_id);
    if (active_remote) s->active_remote = strdup(active_remote);

    /* Create HAP context if credentials provided */
    if (auth_credentials && strlen(auth_credentials) == 192) {
        s->hap = ap2_hap_create(auth_credentials);
    }

    /* Generate session identity */
    RAND_bytes((uint8_t *)&s->session_id, 4);
    uint8_t uuid_bytes[16];
    RAND_bytes(uuid_bytes, 16);
    snprintf(s->session_uuid, sizeof(s->session_uuid),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
             uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
             uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);

    return s;
}

void ap2_session_destroy(struct ap2_session *s)
{
    if (!s) return;
    if (s->sock_fd >= 0) close(s->sock_fd);
    if (s->hap) ap2_hap_destroy(s->hap);
    free(s->host);
    free(s->dacp_id);
    free(s->active_remote);
    free(s);
}

bool ap2_session_connect(struct ap2_session *s)
{
    if (!s) return false;

    s->sock_fd = tcp_connect(s->host, s->port);
    if (s->sock_fd < 0) {
        LOG_ERROR("[AP2-S] Cannot connect to %s:%d", s->host, s->port);
        return false;
    }

    /* Build session URL using local address */
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s->sock_fd, (struct sockaddr *)&local, &len);
    char local_addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, local_addr, sizeof(local_addr));
    snprintf(s->session_url, sizeof(s->session_url), "rtsp://%s/%u", local_addr, s->session_id);

    /* GET /info to detect capabilities */
    uint8_t *body = NULL;
    int body_len = 0;
    int status = send_rtsp_raw(s, "GET", "/info", NULL, 0, NULL, &body, &body_len);
    if (status != 200) {
        LOG_ERROR("[AP2-S] /info failed: %d", status);
        free(body);
        return false;
    }

    /* Parse /info response - it's a binary plist */
    if (body && body_len > 0) {
        /* Use ap2_bplist to parse */
        /* For now just log that we got it. The Python-side already parses
         * /info when it decides which protocol to use. The C side gets
         * the protocol choice from the --protocol flag. */
        LOG_INFO("[AP2-S] Got /info response (%d bytes)", body_len);
    }
    free(body);

    /* Determine flow based on whether we have HAP credentials */
    if (s->hap) {
        /* Apple device with stored credentials -> native AP2 */
        s->flow = AP2_FLOW_NATIVE_REALTIME;
        s->caps.requires_hap_pairing = true;
        LOG_INFO("[AP2-S] Will use native AP2 flow (HAP pair-verify)");
    } else {
        /* No credentials -> try RAOP-compatible flow */
        s->flow = AP2_FLOW_RAOP_COMPAT;
        LOG_INFO("[AP2-S] Will use RAOP-compatible flow (auth-setup)");
    }

    return true;
}

ap2_device_caps_t *ap2_session_get_caps(struct ap2_session *s) { return s ? &s->caps : NULL; }
ap2_flow_mode_t ap2_session_get_flow(struct ap2_session *s) { return s ? s->flow : AP2_FLOW_RAOP_COMPAT; }

bool ap2_session_pair_verify(struct ap2_session *s)
{
    if (!s || !s->hap) return false;

    LOG_INFO("[AP2-S] Performing HAP pair-verify...");
    if (!ap2_hap_pair_verify(s->hap, s->sock_fd)) {
        LOG_ERROR("[AP2-S] HAP pair-verify failed");
        return false;
    }

    s->encrypted = true;
    LOG_INFO("[AP2-S] Channel encrypted via HAP");
    return true;
}

int ap2_session_rtsp(struct ap2_session *s, const char *method, const char *uri,
                     const uint8_t *body, int body_len, const char *content_type,
                     uint8_t **resp, int *resp_len)
{
    if (!s) return 0;
    if (s->encrypted) {
        return send_rtsp_encrypted(s, method, uri, body, body_len, content_type, resp, resp_len);
    }
    return send_rtsp_raw(s, method, uri, body, body_len, content_type, resp, resp_len);
}

bool ap2_session_setup(struct ap2_session *s, int timing_port, bool use_ptp)
{
    /* Session SETUP uses flat bplist (works with libraop bplist) */
    /* Build via ap2_bplist */
    #include "ap2_bplist.h"
    struct ap2_bplist *bp = ap2_bplist_create();
    ap2_bplist_add_string(bp, "timingProtocol", use_ptp ? "PTP" : "NTP");
    ap2_bplist_add_int(bp, "timingPort", timing_port);
    ap2_bplist_add_string(bp, "sessionUUID", s->session_uuid);

    uint8_t *plist_data;
    int plist_len = ap2_bplist_serialize(bp, &plist_data);
    ap2_bplist_free(bp);

    uint8_t *resp = NULL;
    int resp_len = 0;
    int status = ap2_session_rtsp(s, "SETUP", s->session_url,
                                   plist_data, plist_len,
                                   "application/x-apple-binary-plist",
                                   &resp, &resp_len);
    free(plist_data);

    if (status != 200) {
        LOG_ERROR("[AP2-S] Session SETUP failed: %d", status);
        free(resp);
        return false;
    }

    /* Parse response for ports */
    if (resp && resp_len > 0) {
        struct ap2_bplist *rp = ap2_bplist_parse(resp, resp_len);
        if (rp) {
            s->event_port = (int)ap2_bplist_get_int(rp, "eventPort");
            s->timing_port = (int)ap2_bplist_get_int(rp, "timingPort");
            LOG_INFO("[AP2-S] Session: eventPort=%d, timingPort=%d",
                     s->event_port, s->timing_port);
            ap2_bplist_free(rp);
        }
    }
    free(resp);
    return true;
}

bool ap2_session_setup_stream(struct ap2_session *s,
                               int sample_rate, int bit_depth, int channels,
                               int data_port, int control_port,
                               const uint8_t *shared_key)
{
    /*
     * Stream SETUP requires a nested binary plist:
     * {"streams": [{"type": 96, "ct": 2, ...}]}
     *
     * Since libraop's bplist can't do nested structures, we construct
     * this binary plist manually. The format is well-defined.
     *
     * TODO: For now, this only works on devices that accept RAOP-compatible
     * flow (Sonos, etc.). For Apple devices, this will be called after
     * pair-verify and HAP encryption are active, and needs proper nested plist.
     *
     * The proper fix is to either:
     * 1. Extend the bplist library to support nested arrays/dicts
     * 2. Use a proper plist library (e.g., libplist)
     * 3. Construct the exact bytes needed manually
     */

    LOG_WARN("[AP2-S] Native stream SETUP not yet fully implemented for Apple devices");
    LOG_WARN("[AP2-S] Use RAOP-compatible flow (--protocol raop) for now");

    /* For Apple devices with HAP encryption, we would send the encrypted
     * stream SETUP here. Placeholder for future implementation. */

    s->data_port = data_port;
    s->control_port = control_port;
    return false;  /* Not yet implemented for native flow */
}

bool ap2_session_record(struct ap2_session *s)
{
    if (!s) return false;
    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_session_rtsp(s, "RECORD", s->session_url,
                                   NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    return status == 200;
}

bool ap2_session_teardown(struct ap2_session *s)
{
    if (!s) return false;
    uint8_t *resp = NULL; int resp_len = 0;
    ap2_session_rtsp(s, "TEARDOWN", s->session_url, NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    return true;
}

bool ap2_session_set_volume(struct ap2_session *s, float volume_db)
{
    if (!s) return false;
    char body[32];
    int len = snprintf(body, sizeof(body), "volume: %.6f\r\n", volume_db);
    uint8_t *resp = NULL; int resp_len = 0;
    ap2_session_rtsp(s, "SET_PARAMETER", s->session_url,
                     (uint8_t *)body, len, "text/parameters", &resp, &resp_len);
    free(resp);
    return true;
}

bool ap2_session_feedback(struct ap2_session *s)
{
    if (!s) return false;
    uint8_t *resp = NULL; int resp_len = 0;
    ap2_session_rtsp(s, "POST", "/feedback", NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    return true;
}

int ap2_session_get_data_port(struct ap2_session *s) { return s ? s->data_port : 0; }
int ap2_session_get_control_port(struct ap2_session *s) { return s ? s->control_port : 0; }
int ap2_session_get_event_port(struct ap2_session *s) { return s ? s->event_port : 0; }
int ap2_session_get_timing_port(struct ap2_session *s) { return s ? s->timing_port : 0; }
const char *ap2_session_get_url(struct ap2_session *s) { return s ? s->session_url : ""; }
int ap2_session_get_sock(struct ap2_session *s) { return s ? s->sock_fd : -1; }
