/*
 * AirPlay 2 Client - Dual-mode streaming
 *
 * Supports two flows based on device capabilities:
 *
 * 1. RAOP-compatible (no --auth): auth-setup + RAOP ANNOUNCE/SETUP
 *    Used for: Sonos, third-party devices without stored credentials
 *    Limitations: 16-bit only
 *
 * 2. Native AP2 (with --auth): HAP pair-verify + encrypted RTSP + streams SETUP
 *    Used for: Apple TV, HomePod, Sonos with stored credentials
 *    Supports: 24-bit/48kHz ALAC, encrypted audio
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
#include <pthread.h>

#include <openssl/rand.h>
#include <openssl/evp.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/src/raop_client.h"
#include "alac_wrapper.h"
#include "cross_util.h"
#include "cross_log.h"
#include "ap2_client.h"
#include "ap2_hap.h"
#include "ap2_plist.h"
#include "ap2_bplist.h"
#include "ap2_ptp.h"

extern log_level *loglevel;

#define AP2_FRAMES_PER_CHUNK 352
#define AP2_CHACHA_TAG_SIZE  16

typedef enum {
    FLOW_RAOP_COMPAT = 0,
    FLOW_NATIVE_AP2,
} ap2_flow_t;

struct ap2cl_s {
    /* Configuration */
    ap2_device_info_t device;
    ap2_audio_format_t format;
    ap2_state_t state;
    int latency_ms;
    int volume;
    ap2_flow_t flow;

    /* Identifiers */
    char *dacp_id;
    char *active_remote;
    char *iface;
    char *secret;
    char *password;
    char *et;
    char *md;
    char *am;
    char *auth_credentials;  /* HAP credentials hex (192 chars) */

    /* RAOP-compat flow: libraop client */
    struct raopcl_s *raopcl;

    /* Native AP2 flow */
    int sock_fd;                  /* TCP connection */
    struct ap2_hap_ctx *hap;      /* HAP encryption context */
    struct ap2_ptp_ctx *ptp;      /* Timing */
    int data_sock;                /* UDP audio */
    int ctrl_sock;                /* UDP control */
    struct sockaddr_in data_addr;
    struct sockaddr_in ctrl_addr;
    struct alac_codec_s *alac;    /* ALAC encoder for native flow */
    uint8_t audio_key[32];        /* ChaCha20 key for audio encryption */
    uint16_t seq_number;
    uint32_t rtp_timestamp;
    uint32_t ssrc;
    uint64_t head_ts;
    bool first_packet;
    uint64_t audio_nonce_counter;
    char session_url[128];
    char session_uuid[40];
    uint32_t session_id;
    int cseq;

    /* Auth-setup state (RAOP-compat) */
    bool auth_setup_done;
};

/* ---- Auth-setup (RAOP-compat flow) ---- */

static bool ap2_auth_setup(int sock_fd)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    EVP_PKEY *eph_key = NULL;
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &eph_key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    uint8_t pub[32];
    size_t pub_len = 32;
    EVP_PKEY_get_raw_public_key(eph_key, pub, &pub_len);
    EVP_PKEY_free(eph_key);

    uint8_t body[33];
    body[0] = 0x01;
    memcpy(body + 1, pub, 32);

    char req[256];
    int hdr_len = snprintf(req, sizeof(req),
        "POST /auth-setup RTSP/1.0\r\nCSeq: 0\r\n"
        "Content-Type: application/octet-stream\r\nContent-Length: 33\r\n\r\n");

    if (write(sock_fd, req, hdr_len) != hdr_len || write(sock_fd, body, 33) != 33)
        return false;

    uint8_t resp[2048];
    int total = 0;
    struct timeval tv = {.tv_sec = 5};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (total < (int)sizeof(resp) - 1) {
        int n = read(sock_fd, resp + total, sizeof(resp) - total - 1);
        if (n <= 0) break;
        total += n;
        resp[total] = '\0';
        char *end = strstr((char *)resp, "\r\n\r\n");
        if (end) {
            int hdr = (end - (char *)resp) + 4;
            int cl = 0;
            char *clh = strcasestr((char *)resp, "Content-Length:");
            if (clh) cl = atoi(clh + 15);
            if (total >= hdr + cl) break;
        }
    }

    int status = 0;
    sscanf((char *)resp, "%*s %d", &status);
    if (status != 200) return false;

    LOG_INFO("[AP2] auth-setup completed");
    return true;
}

/* ---- Native AP2 RTSP I/O ---- */

static int ap2_rtsp_send(struct ap2cl_s *p, const char *method, const char *uri,
                          const uint8_t *body, int body_len, const char *ct,
                          uint8_t **resp_body, int *resp_len)
{
    char hdr[1024];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: AirPlay/670.6.2\r\n"
        "DACP-ID: %s\r\nActive-Remote: %s\r\n%s%s%s"
        "Content-Length: %d\r\n\r\n",
        method, uri, p->cseq++,
        p->dacp_id ? p->dacp_id : "0",
        p->active_remote ? p->active_remote : "0",
        ct ? "Content-Type: " : "", ct ? ct : "", ct ? "\r\n" : "",
        body_len);

    uint8_t *msg = NULL;
    int msg_len = 0;

    if (p->hap) {
        /* Encrypt RTSP via HAP framing */
        int raw_len = hdr_len + body_len;
        uint8_t *raw = malloc(raw_len);
        memcpy(raw, hdr, hdr_len);
        if (body && body_len > 0) memcpy(raw + hdr_len, body, body_len);
        msg_len = ap2_hap_encrypt(p->hap, raw, raw_len, &msg);
        free(raw);
        if (msg_len <= 0) { free(msg); return 0; }
    } else {
        msg_len = hdr_len + body_len;
        msg = malloc(msg_len);
        memcpy(msg, hdr, hdr_len);
        if (body && body_len > 0) memcpy(msg + hdr_len, body, body_len);
    }

    if (write(p->sock_fd, msg, msg_len) != msg_len) { free(msg); return 0; }
    free(msg);

    /* Read encrypted response.
     * HAP framing: [2-byte LE length][encrypted chunk + 16-byte tag]
     * We accumulate raw bytes, then decrypt complete frames. */
    uint8_t buf[16384];
    int total = 0;
    struct timeval tv = {.tv_sec = 8};
    setsockopt(p->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!p->hap) {
        /* Unencrypted: simple read */
        while (total < (int)sizeof(buf) - 1) {
            int n = read(p->sock_fd, buf + total, sizeof(buf) - total);
            if (n <= 0) break;
            total += n;
            char *end = strstr((char *)buf, "\r\n\r\n");
            if (end) {
                int h_len = (end - (char *)buf) + 4;
                int cl = 0;
                char *clh = strcasestr((char *)buf, "Content-Length:");
                if (clh) cl = atoi(clh + 15);
                if (total >= h_len + cl) break;
            }
        }
        if (total > 0 && strstr((char *)buf, "\r\n\r\n")) {
            int h_len = (strstr((char *)buf, "\r\n\r\n") - (char *)buf) + 4;
            int status = 0;
            sscanf((char *)buf, "%*s %d", &status);
            *resp_len = total - h_len;
            *resp_body = (*resp_len > 0) ? malloc(*resp_len) : NULL;
            if (*resp_body) memcpy(*resp_body, buf + h_len, *resp_len);
            return status;
        }
        *resp_body = NULL; *resp_len = 0;
        return 0;
    }

    /* Encrypted: read all HAP frames, then decrypt */
    while (total < (int)sizeof(buf) - 1) {
        int n = read(p->sock_fd, buf + total, sizeof(buf) - total);
        if (n <= 0) break;
        total += n;

        /* Check if we have at least one complete HAP frame to peek at */
        if (total >= 2) {
            int frame_len = buf[0] | (buf[1] << 8);
            if (total >= 2 + frame_len + 16) {
                /* We have at least one frame. Try decrypting all available frames. */
                uint8_t *dec = NULL;
                /* Save nonce counter in case we need to retry */
                uint64_t saved_counter = ap2_hap_save_read_counter(p->hap);
                int dec_len = ap2_hap_decrypt(p->hap, buf, total, &dec);
                if (dec_len > 0 && dec) {
                    /* Check if decrypted data contains a complete RTSP response */
                    if (strstr((char *)dec, "\r\n\r\n")) {
                        int h_len = (strstr((char *)dec, "\r\n\r\n") - (char *)dec) + 4;
                        int cl = 0;
                        char *clh = strcasestr((char *)dec, "Content-Length:");
                        if (clh) cl = atoi(clh + 15);
                        if (dec_len >= h_len + cl) {
                            int status = 0;
                            sscanf((char *)dec, "%*s %d", &status);
                            *resp_len = dec_len - h_len;
                            *resp_body = (*resp_len > 0) ? malloc(*resp_len) : NULL;
                            if (*resp_body) memcpy(*resp_body, dec + h_len, *resp_len);
                            free(dec);
                            return status;
                        }
                    }
                    free(dec);
                    /* Incomplete RTSP response, need more HAP frames */
                    /* Restore nonce counter since we'll re-decrypt with more data */
                    ap2_hap_restore_read_counter(p->hap, saved_counter);
                } else {
                    free(dec);
                    /* Decrypt failed - might need more data or broken frame */
                    ap2_hap_restore_read_counter(p->hap, saved_counter);
                }
            }
        }
    }

    LOG_ERROR("[AP2] Encrypted response read failed (total=%d bytes)", total);
    if (total > 0) {
        int dump = total < 64 ? total : 64;
        char hex[200];
        for (int i = 0; i < dump; i++) sprintf(hex + i*3, "%02x ", buf[i]);
        hex[dump*3] = '\0';
        LOG_ERROR("[AP2] Raw: %s", hex);
        /* First 2 bytes = HAP frame length */
        if (total >= 2) {
            int fl = buf[0] | (buf[1] << 8);
            LOG_ERROR("[AP2] HAP frame_len=%d, have=%d, need=%d", fl, total, 2 + fl + 16);
        }
    }
    *resp_body = NULL; *resp_len = 0;
    return 0;
}

/* ---- Native AP2 connect sequence ---- */

static bool ap2_native_connect(struct ap2cl_s *p)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", p->device.port);

    if (getaddrinfo(p->device.address, port_str, &hints, &res) != 0) return false;
    p->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (p->sock_fd < 0 || connect(p->sock_fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (p->sock_fd >= 0) { close(p->sock_fd); p->sock_fd = -1; }
        return false;
    }
    freeaddrinfo(res);

    /* Get local address for session URL */
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(p->sock_fd, (struct sockaddr *)&local, &len);
    char local_addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, local_addr, sizeof(local_addr));
    RAND_bytes((uint8_t *)&p->session_id, 4);
    snprintf(p->session_url, sizeof(p->session_url), "rtsp://%s/%u", local_addr, p->session_id);

    /* Generate session UUID */
    uint8_t uuid_bytes[16];
    RAND_bytes(uuid_bytes, 16);
    snprintf(p->session_uuid, sizeof(p->session_uuid),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
             uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
             uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);

    /* 1. GET /info */
    uint8_t *resp = NULL; int resp_len = 0;
    int status = ap2_rtsp_send(p, "GET", "/info", NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    if (status != 200) { LOG_ERROR("[AP2] /info failed: %d", status); return false; }

    /* 2. HAP pair-verify */
    p->hap = ap2_hap_create(p->auth_credentials);
    if (!p->hap) { LOG_ERROR("[AP2] Invalid credentials"); return false; }

    /* Set client_id from DACP ID as UPPERCASE ASCII string.
     * Must match what was sent during pair-setup. MA's pair-setup uses the
     * DACP ID as a 16-char uppercase hex string encoded as bytes. */
    if (p->dacp_id) {
        /* Uppercase the string */
        char upper_dacp[32];
        int len = strlen(p->dacp_id);
        if (len > 30) len = 30;
        for (int i = 0; i < len; i++) {
            char c = p->dacp_id[i];
            upper_dacp[i] = (c >= 'a' && c <= 'f') ? (c - 'a' + 'A') : c;
        }
        upper_dacp[len] = '\0';
        ap2_hap_set_client_id(p->hap, (const uint8_t *)upper_dacp, len);
    }

    LOG_INFO("[AP2] Performing HAP pair-verify...");
    if (!ap2_hap_pair_verify(p->hap, p->sock_fd)) {
        LOG_ERROR("[AP2] HAP pair-verify failed");
        ap2_hap_destroy(p->hap);
        p->hap = NULL;
        return false;
    }
    LOG_INFO("[AP2] Channel encrypted");

    /* 3. Start NTP timing responder */
    p->ptp = ap2_ptp_create();
    ap2_ptp_start(p->ptp, p->device.address);
    int timing_port = ap2_ptp_get_timing_port(p->ptp);

    /* 4. Session SETUP (encrypted) */
    struct ap2_plist *sp = ap2_plist_create();

    /* deviceID must be colon-formatted uppercase hex from the 16-char DACP ID
     * e.g. "B2763ADADB414A27" -> "B2:76:3A:DA:DB:41:4A:27" */
    if (p->dacp_id && strlen(p->dacp_id) >= 16) {
        char dev_colon[24];
        int di = 0;
        for (int i = 0; i < 16 && di < (int)sizeof(dev_colon) - 1; i++) {
            char c = p->dacp_id[i];
            if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
            dev_colon[di++] = c;
            if (i % 2 == 1 && i < 15) dev_colon[di++] = ':';
        }
        dev_colon[di] = '\0';
        ap2_plist_add_string(sp, "deviceID", dev_colon);
    }

    ap2_plist_add_string(sp, "sessionUUID", p->session_uuid);
    ap2_plist_add_int(sp, "timingPort", timing_port);
    ap2_plist_add_string(sp, "timingProtocol", "NTP");

    uint8_t *plist_data; int plist_len;
    plist_len = ap2_plist_serialize(sp, &plist_data);
    ap2_plist_free(sp);

    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);
    if (status != 200) {
        LOG_ERROR("[AP2] Session SETUP failed: %d", status);
        free(resp);
        return false;
    }
    LOG_INFO("[AP2] Session SETUP OK");

    /* Extract eventPort from session response */
    int event_port = 0;
    if (resp && resp_len > 0) {
        for (int i = 0; i < resp_len - 10; i++) {
            if (i >= 9 && memcmp(resp + i, "eventPort", 9) == 0) {
                for (int j = i; j < resp_len - 3 && j < i + 50; j++) {
                    uint8_t marker = resp[j];
                    if (marker == 0x11 && j + 2 < resp_len) {
                        int val = (resp[j+1] << 8) | resp[j+2];
                        if (val >= 1024 && val <= 65535) { event_port = val; break; }
                    } else if (marker == 0x12 && j + 4 < resp_len) {
                        int val = (resp[j+1] << 24) | (resp[j+2] << 16) |
                                  (resp[j+3] << 8) | resp[j+4];
                        if (val >= 1024 && val <= 65535) { event_port = val; break; }
                    }
                }
                break;
            }
        }
    }
    free(resp);

    /* Open events connection (reverse TCP to device's eventPort) */
    if (event_port > 0) {
        LOG_INFO("[AP2] Opening events connection to %s:%d", p->device.address, event_port);
        int events_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (events_sock >= 0) {
            struct sockaddr_in ev_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(event_port),
            };
            inet_pton(AF_INET, p->device.address, &ev_addr.sin_addr);
            struct timeval etv = {.tv_sec = 3};
            setsockopt(events_sock, SOL_SOCKET, SO_RCVTIMEO, &etv, sizeof(etv));
            if (connect(events_sock, (struct sockaddr *)&ev_addr, sizeof(ev_addr)) == 0) {
                LOG_INFO("[AP2] Events connection OK");
                /* Keep the socket open - we don't need to read from it, just keep it alive */
                /* TODO: store in ctx for proper cleanup */
            } else {
                LOG_WARN("[AP2] Events connect failed");
                close(events_sock);
            }
        }
    }

    /* 5. Stream SETUP with streams array (BEFORE RECORD per Apple TV expectations) */
    RAND_bytes(p->audio_key, 32);
    /* SSRC must match streamConnectionID that we register in stream SETUP,
     * otherwise the AP2 receiver can't identify which stream the RTP packet belongs to */
    p->ssrc = p->session_id;

    /* Open UDP sockets */
    p->data_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY};
    bind(p->data_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    struct sockaddr_in ds_local;
    len = sizeof(ds_local);
    getsockname(p->data_sock, (struct sockaddr *)&ds_local, &len);
    int local_data_port = ntohs(ds_local.sin_port);

    p->ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    bind(p->ctrl_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    struct sockaddr_in cs_local;
    len = sizeof(cs_local);
    getsockname(p->ctrl_sock, (struct sockaddr *)&cs_local, &len);
    int local_ctrl_port = ntohs(cs_local.sin_port);

    /* Determine audio format */
    uint64_t audio_format;
    if (p->format.bit_depth > 16 && p->format.sample_rate >= 48000)
        audio_format = 0x200000;  /* ALAC 48000/24/2 */
    else if (p->format.bit_depth > 16)
        audio_format = 0x80000;   /* ALAC 44100/24/2 */
    else if (p->format.sample_rate >= 48000)
        audio_format = 0x100000;  /* ALAC 48000/16/2 */
    else
        audio_format = 0x40000;   /* ALAC 44100/16/2 */

    struct ap2_plist *ssp = ap2_plist_create();
    ap2_plist_stream_begin(ssp);
    ap2_plist_stream_add_int(ssp, "audioFormat", audio_format);
    ap2_plist_stream_add_string(ssp, "audioMode", "default");
    ap2_plist_stream_add_int(ssp, "controlPort", local_ctrl_port);
    ap2_plist_stream_add_int(ssp, "ct", 2);  /* ALAC */
    ap2_plist_stream_add_int(ssp, "dataPort", local_data_port);
    ap2_plist_stream_add_bool(ssp, "isMedia", true);
    ap2_plist_stream_add_int(ssp, "latencyMax", 88200);
    ap2_plist_stream_add_int(ssp, "latencyMin", 11025);
    ap2_plist_stream_add_data(ssp, "shk", p->audio_key, 32);
    ap2_plist_stream_add_int(ssp, "spf", 352);
    ap2_plist_stream_add_int(ssp, "sr", p->format.sample_rate);
    ap2_plist_stream_add_int(ssp, "streamConnectionID", p->session_id);
    ap2_plist_stream_add_bool(ssp, "supportsDynamicStreamID", false);
    ap2_plist_stream_add_int(ssp, "type", 96);
    ap2_plist_stream_end(ssp);

    plist_len = ap2_plist_serialize(ssp, &plist_data);
    ap2_plist_free(ssp);

    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "SETUP", p->session_url, plist_data, plist_len,
                            "application/x-apple-binary-plist", &resp, &resp_len);
    free(plist_data);

    if (status != 200) {
        LOG_ERROR("[AP2] Stream SETUP failed: %d", status);
        free(resp);
        return false;
    }

    /* Parse response for remote ports.
     * The response is a nested binary plist: {"streams": [{"dataPort": N, "controlPort": N}]}
     * We scan the raw bytes for the known ASCII key names followed by
     * integer object markers (0x10=1B, 0x11=2B, 0x12=4B, 0x13=8B).
     * This is a pragmatic parse that doesn't require a full plist library. */
    if (resp && resp_len > 0) {
        int remote_data = 0, remote_ctrl = 0;

        /* The plist format stores each string key and each int value as separate
         * objects at offsets given in an offset table at the end of the plist.
         * Values are then referenced by index from within their parent container.
         * Since the integers we want (dataPort, controlPort) are stored somewhere
         * in the plist as objects of form (0x10..0x13)(value), we search for
         * the key strings and then check nearby integer objects.
         * A simpler and more reliable approach: scan all integer objects in
         * the plist for values in the valid port range (1024-65535) where the
         * preceding bytes spell "dataPort"/"controlPort". */

        /*
         * In binary plists, strings and integers are stored in separate
         * object areas. Instead of trying to scan contextually, we scan
         * the entire response body for all integer objects (0x11/0x12
         * markers) in port range and heuristically match them to keys
         * based on the order they appear alongside "dataPort"/"controlPort"
         * string markers.
         *
         * Pragmatic approach: find the positions of "dataPort" and "controlPort"
         * strings, then find the integer values at roughly the correct
         * offsets. The plist offset table correlates them.
         *
         * Even simpler: collect all unique valid port integers and the
         * positions of our key strings, then use the response port that
         * the device assigned. For now we just ensure both data and ctrl
         * ports differ by preferring integers that haven't been used yet.
         */
        int found_ports[8] = {0};
        int num_ports = 0;
        for (int i = 0; i < resp_len - 4; i++) {
            uint8_t marker = resp[i];
            int val = 0;
            if (marker == 0x11 && i + 2 < resp_len) {
                val = (resp[i+1] << 8) | resp[i+2];
            } else if (marker == 0x12 && i + 4 < resp_len) {
                val = (resp[i+1] << 24) | (resp[i+2] << 16) |
                      (resp[i+3] << 8) | resp[i+4];
            }
            if (val >= 1024 && val <= 65535 && num_ports < 8) {
                /* Check if already found */
                bool dup = false;
                for (int k = 0; k < num_ports; k++) {
                    if (found_ports[k] == val) { dup = true; break; }
                }
                if (!dup) found_ports[num_ports++] = val;
            }
        }

        LOG_DEBUG("[AP2] Found %d unique ports in response", num_ports);
        /* Device returned dataPort and controlPort are typically in the response.
         * Pick the first two valid ports that aren't our local ports. */
        for (int i = 0; i < num_ports; i++) {
            if (found_ports[i] != local_data_port &&
                found_ports[i] != local_ctrl_port &&
                found_ports[i] != ap2_ptp_get_timing_port(p->ptp)) {
                if (remote_data == 0) {
                    remote_data = found_ports[i];
                } else if (remote_ctrl == 0 && found_ports[i] != remote_data) {
                    remote_ctrl = found_ports[i];
                    break;
                }
            }
        }

        if (remote_data > 0) {
            memset(&p->data_addr, 0, sizeof(p->data_addr));
            p->data_addr.sin_family = AF_INET;
            p->data_addr.sin_port = htons(remote_data);
            inet_pton(AF_INET, p->device.address, &p->data_addr.sin_addr);
            LOG_INFO("[AP2] Remote data port: %d", remote_data);
        }
        if (remote_ctrl > 0) {
            memset(&p->ctrl_addr, 0, sizeof(p->ctrl_addr));
            p->ctrl_addr.sin_family = AF_INET;
            p->ctrl_addr.sin_port = htons(remote_ctrl);
            inet_pton(AF_INET, p->device.address, &p->ctrl_addr.sin_addr);
            LOG_INFO("[AP2] Remote control port: %d", remote_ctrl);
        }

        if (remote_data == 0) {
            LOG_WARN("[AP2] Could not parse remote data port from response");
        }
    }
    LOG_INFO("[AP2] Stream SETUP OK");
    free(resp);

    /* 6. RECORD to begin streaming */
    resp = NULL; resp_len = 0;
    status = ap2_rtsp_send(p, "RECORD", p->session_url, NULL, 0, NULL, &resp, &resp_len);
    free(resp);
    if (status != 200) {
        LOG_WARN("[AP2] RECORD returned %d", status);
    } else {
        LOG_INFO("[AP2] RECORD OK");
    }

    /* Create ALAC encoder */
    p->alac = alac_create_encoder(AP2_FRAMES_PER_CHUNK,
                                   p->format.sample_rate,
                                   p->format.bit_depth,
                                   p->format.channels);

    p->first_packet = true;
    p->state = AP2_CONNECTED;
    LOG_INFO("[AP2] Native AP2 session ready");
    return true;
}

/* ---- Native AP2 audio send ---- */

/* Send AP2 NTP sync packet (20 bytes) to control port.
 * Format (from owntone rtp_common.c sync_packet_ntp_make):
 *   bytes 0-1:  header (0x90d4 first, 0x80d4 subsequent)
 *   bytes 2-3:  fixed 0x0007
 *   bytes 4-7:  RTP timestamp - latency (network order)
 *   bytes 8-11: NTP seconds (wall clock, network order)
 *   bytes 12-15: NTP fraction (network order)
 *   bytes 16-19: current RTP timestamp (network order)
 * Sent unencrypted on the control UDP socket. */
static void ap2_send_sync_packet(struct ap2cl_s *p, bool first)
{
    if (p->ctrl_sock < 0) return;

    uint8_t pkt[20];
    pkt[0] = first ? 0x90 : 0x80;
    pkt[1] = 0xd4;
    pkt[2] = 0x00;
    pkt[3] = 0x07;

    uint32_t latency_frames = MS2TS(p->latency_ms, p->format.sample_rate);
    uint32_t ts_latency = p->rtp_timestamp >= latency_frames
                          ? p->rtp_timestamp - latency_frames : 0;
    uint32_t ts_be = htonl(ts_latency);
    memcpy(pkt + 4, &ts_be, 4);

    /* NTP time */
    uint64_t ntp = raopcl_get_ntp(NULL);
    uint32_t ntp_sec = htonl((uint32_t)(ntp >> 32));
    uint32_t ntp_frac = htonl((uint32_t)(ntp & 0xFFFFFFFF));
    memcpy(pkt + 8, &ntp_sec, 4);
    memcpy(pkt + 12, &ntp_frac, 4);

    uint32_t cur_ts_be = htonl(p->rtp_timestamp);
    memcpy(pkt + 16, &cur_ts_be, 4);

    sendto(p->ctrl_sock, pkt, sizeof(pkt), 0,
           (struct sockaddr *)&p->ctrl_addr, sizeof(p->ctrl_addr));
}

static bool ap2_native_send_chunk(struct ap2cl_s *p, uint8_t *sample, int frames)
{
    if (p->data_sock < 0 || !p->alac) return false;

    /* Send initial sync packet before the very first audio packet */
    if (p->first_packet) {
        ap2_send_sync_packet(p, true);
    } else if ((p->seq_number % 100) == 0) {
        /* Periodic sync packets every ~100 chunks (~0.8 seconds at 352fpp/44.1kHz) */
        ap2_send_sync_packet(p, false);
    }

    /* ALAC encode */
    uint8_t *encoded = NULL;
    int enc_size = 0;
    pcm_to_alac(p->alac, sample, frames, &encoded, &enc_size);
    if (!encoded || enc_size <= 0) { free(encoded); return false; }

    /* Build RTP header (12 bytes) */
    uint8_t rtp_hdr[12];
    rtp_hdr[0] = 0x80;
    rtp_hdr[1] = p->first_packet ? 0xE0 : 0x60;
    rtp_hdr[2] = (p->seq_number >> 8) & 0xFF;
    rtp_hdr[3] = p->seq_number & 0xFF;
    uint32_t ts_be = htonl(p->rtp_timestamp);
    memcpy(rtp_hdr + 4, &ts_be, 4);
    uint32_t ssrc_be = htonl(p->ssrc);
    memcpy(rtp_hdr + 8, &ssrc_be, 4);
    p->first_packet = false;

    /* Encrypt ALAC payload with ChaCha20-Poly1305 (AP2 audio format).
     * Key:   First 32 bytes of X25519 shared_secret from pair-verify.
     * Nonce: 4 zero bytes + 2-byte sequence number (network byte order from RTP hdr) + 6 zero bytes.
     *        Note: owntone copies seqnum as raw bytes, which matches the network-order seqnum
     *        already in the RTP header.
     * AAD:   8 bytes from RTP header (timestamp + SSRC, bytes 4-11).
     * Output appends: encrypted payload + 16-byte tag.
     * Note: the trailing seqnum suffix some docs mention is NOT appended in the actual wire format.
     */
    const uint8_t *audio_key = ap2_hap_get_shared_secret(p->hap);
    if (!audio_key) {
        LOG_ERROR("[AP2] No shared secret for audio encryption");
        free(encoded);
        return false;
    }

    uint8_t nonce[12];
    memset(nonce, 0, 12);
    /* Copy 2-byte network-order seqnum to offset 4 (as owntone does) */
    nonce[4] = rtp_hdr[2];
    nonce[5] = rtp_hdr[3];

    int pkt_size = 12 + enc_size + AP2_CHACHA_TAG_SIZE;
    uint8_t *pkt = malloc(pkt_size);
    memcpy(pkt, rtp_hdr, 12);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len;
    EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, audio_key, nonce);
    EVP_EncryptUpdate(ctx, NULL, &len, rtp_hdr + 4, 8);  /* AAD = timestamp + ssrc */
    EVP_EncryptUpdate(ctx, pkt + 12, &len, encoded, enc_size);
    int ct_len = len;
    EVP_EncryptFinal_ex(ctx, pkt + 12 + ct_len, &len);
    ct_len += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, AP2_CHACHA_TAG_SIZE, pkt + 12 + ct_len);
    EVP_CIPHER_CTX_free(ctx);
    free(encoded);

    int actual_pkt_size = 12 + ct_len + AP2_CHACHA_TAG_SIZE;
    ssize_t sent = sendto(p->data_sock, pkt, actual_pkt_size, 0,
                           (struct sockaddr *)&p->data_addr, sizeof(p->data_addr));
    free(pkt);

    p->seq_number++;
    p->rtp_timestamp += frames;
    p->head_ts += frames;

    return sent > 0;
}

/* ---- RAOP-compat connect ---- */

static bool ap2_raop_compat_connect(struct ap2cl_s *p)
{
    /* Auth-setup on temporary connection */
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", p->device.port);

    if (getaddrinfo(p->device.address, port_str, &hints, &res) != 0) return false;
    int auth_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (auth_sock < 0 || connect(auth_sock, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (auth_sock >= 0) close(auth_sock);
        return false;
    }
    freeaddrinfo(res);

    const char *info_req = "GET /info RTSP/1.0\r\nCSeq: 0\r\nUser-Agent: AirPlay/670.6.2\r\nContent-Length: 0\r\n\r\n";
    write(auth_sock, info_req, strlen(info_req));
    uint8_t discard[4096];
    struct timeval tv = {.tv_sec = 2};
    setsockopt(auth_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    read(auth_sock, discard, sizeof(discard));

    if (!ap2_auth_setup(auth_sock)) {
        LOG_WARN("[AP2] auth-setup failed, proceeding without");
    } else {
        p->auth_setup_done = true;
    }
    close(auth_sock);

    /* Use libraop RAOP client */
    struct in_addr host_addr, player_addr;
    uint32_t netmask;
    char *iface = NULL;
    host_addr = get_interface(p->iface, &iface, &netmask);
    NFREE(iface);

    struct hostent *hostent = gethostbyname(p->device.address);
    if (!hostent) return false;
    memcpy(&player_addr.s_addr, hostent->h_addr_list[0], hostent->h_length);

    int latency_frames = MS2TS(p->latency_ms, p->format.sample_rate);
    bool mfi_auth = p->am && strcasestr(p->am, "airport");

    p->raopcl = raopcl_create(
        host_addr, 0, 0,
        p->dacp_id ? p->dacp_id : "1A2B3D4EA1B2C3D4",
        p->active_remote ? p->active_remote : "0",
        RAOP_ALAC, DEFAULT_FRAMES_PER_CHUNK, latency_frames,
        RAOP_CLEAR, mfi_auth,
        p->secret ? p->secret : "", p->password,
        p->et ? p->et : "0,4", p->md ? p->md : "0,1,2",
        p->format.sample_rate, p->format.bit_depth, p->format.channels,
        p->volume > 0 ? raopcl_float_volume(p->volume) : -144.0f);

    if (!p->raopcl) return false;

    if (!raopcl_connect(p->raopcl, player_addr, p->device.port, p->volume > 0)) {
        raopcl_destroy(p->raopcl);
        p->raopcl = NULL;
        return false;
    }

    p->state = AP2_CONNECTED;
    return true;
}

/* ---- Public API ---- */

struct ap2cl_s *ap2cl_create(
    ap2_device_info_t *device, ap2_audio_format_t *format,
    const char *auth, const char *password,
    const char *dacp_id, const char *active_remote,
    int latency_ms, int volume)
{
    struct ap2cl_s *p = calloc(1, sizeof(struct ap2cl_s));
    if (!p) return NULL;

    p->device = *device;
    p->format = *format;
    p->state = AP2_DOWN;
    p->latency_ms = latency_ms;
    p->volume = volume;
    p->sock_fd = -1;
    p->data_sock = -1;
    p->ctrl_sock = -1;
    if (dacp_id) p->dacp_id = strdup(dacp_id);
    if (active_remote) p->active_remote = strdup(active_remote);
    if (password) p->password = strdup(password);
    if (auth && strlen(auth) == 192) {
        p->auth_credentials = strdup(auth);
        p->flow = FLOW_NATIVE_AP2;
    } else {
        p->flow = FLOW_RAOP_COMPAT;
    }

    LOG_INFO("[AP2] Created client for %s (%s:%d) [%s]",
             device->name ? device->name : "unknown",
             device->address, device->port,
             p->flow == FLOW_NATIVE_AP2 ? "native AP2" : "RAOP-compat");
    return p;
}

bool ap2cl_destroy(struct ap2cl_s *p)
{
    if (!p) return false;
    if (p->raopcl) { raopcl_disconnect(p->raopcl); raopcl_destroy(p->raopcl); }
    if (p->hap) ap2_hap_destroy(p->hap);
    if (p->ptp) ap2_ptp_destroy(p->ptp);
    if (p->alac) alac_delete_encoder(p->alac);
    if (p->sock_fd >= 0) close(p->sock_fd);
    if (p->data_sock >= 0) close(p->data_sock);
    if (p->ctrl_sock >= 0) close(p->ctrl_sock);
    free(p->dacp_id); free(p->active_remote); free(p->iface);
    free(p->secret); free(p->password); free(p->et); free(p->md); free(p->am);
    free(p->auth_credentials);
    free(p);
    return true;
}

void ap2cl_set_raop_props(struct ap2cl_s *p,
                           const char *iface, const char *secret,
                           const char *et, const char *md, const char *am)
{
    if (!p) return;
    if (iface) { free(p->iface); p->iface = strdup(iface); }
    if (secret) { free(p->secret); p->secret = strdup(secret); }
    if (et) { free(p->et); p->et = strdup(et); }
    if (md) { free(p->md); p->md = strdup(md); }
    if (am) { free(p->am); p->am = strdup(am); }
}

bool ap2cl_connect(struct ap2cl_s *p)
{
    if (!p) return false;

    if (p->flow == FLOW_NATIVE_AP2) {
        LOG_INFO("[AP2] Connecting via native AP2 flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_native_connect(p);
    } else {
        LOG_INFO("[AP2] Connecting via RAOP-compatible flow to %s:%d",
                 p->device.address, p->device.port);
        return ap2_raop_compat_connect(p);
    }
}

bool ap2cl_disconnect(struct ap2cl_s *p)
{
    if (!p) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        if (p->sock_fd >= 0) {
            uint8_t *resp = NULL; int resp_len = 0;
            ap2_rtsp_send(p, "TEARDOWN", p->session_url, NULL, 0, NULL, &resp, &resp_len);
            free(resp);
            close(p->sock_fd); p->sock_fd = -1;
        }
    } else if (p->raopcl) {
        raopcl_disconnect(p->raopcl);
    }
    p->state = AP2_DOWN;
    return true;
}

bool ap2cl_start_at(struct ap2cl_s *p, uint64_t ntp_start)
{
    if (!p) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        p->head_ts = NTP2TS(ntp_start, p->format.sample_rate);
        p->rtp_timestamp = (uint32_t)p->head_ts;
        p->state = AP2_STREAMING;
        return true;
    }
    if (!p->raopcl) return false;
    int latency = raopcl_latency(p->raopcl);
    raopcl_start_at(p->raopcl, ntp_start - TS2NTP(latency, p->format.sample_rate));
    p->state = AP2_STREAMING;
    return true;
}

bool ap2cl_send_chunk(struct ap2cl_s *p, uint8_t *sample, int frames)
{
    if (!p || p->state != AP2_STREAMING) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        return ap2_native_send_chunk(p, sample, frames);
    }
    if (!p->raopcl) return false;
    uint64_t playtime;
    return raopcl_send_chunk(p->raopcl, sample, frames, &playtime);
}

bool ap2cl_accept_frames(struct ap2cl_s *p)
{
    if (!p || p->state != AP2_STREAMING) return false;
    if (p->flow == FLOW_NATIVE_AP2) {
        uint64_t now_ntp = raopcl_get_ntp(NULL);
        uint64_t now_ts = NTP2TS(now_ntp, p->format.sample_rate);
        uint64_t latency_frames = MS2TS(p->latency_ms, p->format.sample_rate);
        return (now_ts + latency_frames) >= p->head_ts;
    }
    if (!p->raopcl) return false;
    return raopcl_accept_frames(p->raopcl);
}

void ap2cl_pause(struct ap2cl_s *p)
{
    if (!p) return;
    if (p->raopcl) { raopcl_pause(p->raopcl); raopcl_flush(p->raopcl); }
    p->state = AP2_PAUSED;
}

void ap2cl_play(struct ap2cl_s *p)
{
    if (!p) return;
    if (p->raopcl) {
        int lat = raopcl_latency(p->raopcl);
        uint64_t now = raopcl_get_ntp(NULL);
        raopcl_start_at(p->raopcl, now + MS2NTP(200) - TS2NTP(lat, raopcl_sample_rate(p->raopcl)));
    }
    p->state = AP2_STREAMING;
}

void ap2cl_stop(struct ap2cl_s *p)
{
    if (!p) return;
    if (p->raopcl) raopcl_stop(p->raopcl);
    p->state = AP2_DOWN;
}

bool ap2cl_set_volume(struct ap2cl_s *p, int volume)
{
    if (!p) return false;
    p->volume = volume;
    if (p->flow == FLOW_NATIVE_AP2 && p->sock_fd >= 0) {
        float vol_db = volume <= 0 ? -144.0f : -30.0f + (volume / 100.0f) * 30.0f;
        char body[32];
        int blen = snprintf(body, sizeof(body), "volume: %.6f\r\n", vol_db);
        uint8_t *resp = NULL; int resp_len = 0;
        ap2_rtsp_send(p, "SET_PARAMETER", p->session_url,
                       (uint8_t *)body, blen, "text/parameters", &resp, &resp_len);
        free(resp);
        return true;
    }
    if (p->raopcl) return raopcl_set_volume(p->raopcl, raopcl_float_volume(volume));
    return false;
}

bool ap2cl_set_metadata(struct ap2cl_s *p, const char *title, const char *artist,
                        const char *album, int duration)
{
    if (!p) return false;
    if (p->raopcl)
        return raopcl_set_daap(p->raopcl, 4, "minm", 's', title,
                                "asar", 's', artist, "asal", 's', album, "astn", 'i', 1);
    return false;
}

bool ap2cl_set_artwork(struct ap2cl_s *p, const char *content_type, int size, const char *data)
{
    if (!p || !p->raopcl) return false;
    return raopcl_set_artwork(p->raopcl, (char *)content_type, size, (char *)data);
}

bool ap2cl_set_progress(struct ap2cl_s *p, int elapsed_s, int duration_s)
{
    if (!p || !p->raopcl) return false;
    return raopcl_set_progress_ms(p->raopcl, elapsed_s * 1000, duration_s * 1000);
}

ap2_state_t ap2cl_state(struct ap2cl_s *p) { return p ? p->state : AP2_DOWN; }
bool ap2cl_is_connected(struct ap2cl_s *p) { return p && p->state >= AP2_CONNECTED; }
bool ap2cl_is_playing(struct ap2cl_s *p) { return p && p->state == AP2_STREAMING; }
