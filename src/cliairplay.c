/*
 * cliairplay - Unified AirPlay streaming CLI
 *
 * Supports both RAOP (AirPlay 1) and AirPlay 2 protocols through a single binary.
 * Based on libraop for RAOP protocol.
 *
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Copyright (C) Philippe <philippe_44@outlook.com>
 * Copyright (C) 2024-2026 Music Assistant Contributors
 *
 * See LICENSE
 */

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <poll.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_thread.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/crosstools/src/cross_ssl.h"
#include "../libraop/src/raop_client.h"
#include "cross_util.h"
#include "cross_log.h"
#include "ap2_client.h"
#include "ap2_rtsp.h"

#define VERSION "0.1.0"
#define AP2_FRAMES_PER_CHUNK 352

/* Protocol selection */
typedef enum {
    PROTO_RAOP = 1,
    PROTO_AIRPLAY2 = 2,
} protocol_t;

/* Playback status */
typedef enum {
    STATUS_STOPPED = 0,
    STATUS_PAUSED,
    STATUS_PLAYING,
} playback_status_t;

/* CLI configuration parsed from arguments */
typedef struct {
    /* Common settings */
    protocol_t protocol;
    char *host;
    int port;
    int volume;
    int latency_ms;
    int debug_level;
    char *dacp_id;
    char *active_remote;
    char *cmdpipe;
    char *udn;
    uint64_t ntp_start;
    int wait_ms;
    char *audio_source;  /* filename or "-" for stdin */

    /* RAOP-specific */
    bool encrypt;
    bool alac;
    char *secret;
    char *password;
    char *et;
    char *md;
    char *am;
    char *pk;
    char *pw;
    char *iface;

    /* AirPlay 2-specific */
    char *auth;       /* HAP credentials (hex) */
    char *ap2_name;
    char *ap2_hostname;
    char *ap2_txt;    /* mDNS TXT records */
    int64_t ptp_offset_ns;  /* PTP clock offset in nanoseconds */

    /* Audio format */
    int sample_rate;
    int bit_depth;
    int channels;
} cli_config_t;

/* Globals */
static bool g_running = true;
static playback_status_t g_status = STATUS_STOPPED;
static pthread_t g_cmdpipe_thread;
static int g_cmdpipe_fd = -1;
static char g_cmdpipe_buf[512];

/* RAOP context (when using RAOP protocol) */
static struct raopcl_s *g_raopcl = NULL;

/* AP2 context (when using AirPlay 2 protocol) */
static struct ap2cl_s *g_ap2cl = NULL;

/* Debug levels */
log_level util_loglevel;
log_level raop_loglevel;
log_level main_log;
log_level *loglevel = &main_log;

static struct debug_s {
    int main, raop, util;
} debug_levels[] = {
    {lSILENCE, lSILENCE, lSILENCE},
    {lERROR, lERROR, lERROR},
    {lINFO, lERROR, lERROR},
    {lINFO, lINFO, lERROR},
    {lDEBUG, lERROR, lERROR},
    {lDEBUG, lINFO, lERROR},
    {lDEBUG, lDEBUG, lERROR},
    {lSDEBUG, lINFO, lERROR},
    {lSDEBUG, lDEBUG, lERROR},
    {lSDEBUG, lSDEBUG, lERROR},
};

#define NUM_DEBUG_LEVELS (sizeof(debug_levels) / sizeof(struct debug_s))

/* ---- Normalized status output ---- */

static void status_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

/* Status messages parsed by Music Assistant's _stderr_reader() */
static void status_connected(void)
{
    status_print("[STATUS] connected");
}

static void status_playing(uint64_t elapsed_ms)
{
    status_print("[STATUS] playing elapsed_ms=%" PRIu64, elapsed_ms);
}

static void status_paused(uint64_t elapsed_ms)
{
    status_print("[STATUS] paused elapsed_ms=%" PRIu64, elapsed_ms);
}

static void status_eof(void)
{
    status_print("[STATUS] eof");
}

static void status_error(const char *msg)
{
    status_print("[ERROR] %s", msg);
}

/* Also emit legacy format for backward compatibility during transition */
static void status_connected_legacy(const char *host, int port, int latency_ms)
{
    LOG_INFO("connected to %s on port %d, player latency is %d ms", host, port, latency_ms);
    status_connected();
}

static void status_elapsed_legacy(uint64_t elapsed_ms, uint64_t frames, struct raopcl_s *raopcl)
{
    LOG_INFO("elapsed milliseconds: %" PRIu64, elapsed_ms);
    status_playing(elapsed_ms);
}

/* ---- Helper functions ---- */

static bool starts_with(const char *pre, const char *str)
{
    size_t lenpre = strlen(pre);
    size_t lenstr = strlen(str);
    return lenstr >= lenpre && memcmp(pre, str, lenpre) == 0;
}

/*
 * Truncate s32le samples to s24le (packed 3 bytes) for ALAC encoder.
 * Input: 4 bytes per sample (s32le), Output: 3 bytes per sample (s24le)
 * Takes the lower 3 bytes of each 32-bit LE sample (bytes 0,1,2 = bits 0-23).
 * Returns number of output bytes.
 */
static int truncate_32to24(const uint8_t *in, int in_bytes, uint8_t *out)
{
    int samples = in_bytes / 4;
    for (int i = 0; i < samples; i++) {
        /* s32le: byte0=LSB, byte3=MSB. Take bytes 1,2,3 for upper 24 bits. */
        out[i * 3 + 0] = in[i * 4 + 1];
        out[i * 3 + 1] = in[i * 4 + 2];
        out[i * 3 + 2] = in[i * 4 + 3];
    }
    return samples * 3;
}

/* ---- Command pipe handler ---- */

static struct {
    char *title;
    char *artist;
    char *album;
    int duration;
    int progress;
} g_metadata = {"", "", "", 0, 0};

static void handle_command(const char *key, const char *value, cli_config_t *cfg)
{
    if (strcmp(key, "TITLE") == 0) {
        g_metadata.title = value ? (char*)value : "";
    } else if (strcmp(key, "ARTIST") == 0) {
        g_metadata.artist = value ? (char*)value : "";
    } else if (strcmp(key, "ALBUM") == 0) {
        g_metadata.album = value ? (char*)value : "";
    } else if (strcmp(key, "DURATION") == 0) {
        g_metadata.duration = atoi(value);
    } else if (strcmp(key, "PROGRESS") == 0) {
        g_metadata.progress = atoi(value);
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_progress_ms(g_raopcl, g_metadata.progress * 1000, g_metadata.duration * 1000);
        }
    } else if (strcmp(key, "ARTWORK") == 0) {
        if (access(value, F_OK) == 0) {
            /* Local file artwork */
            FILE *artfile = fopen(value, "r");
            if (artfile) {
                fseek(artfile, 0L, SEEK_END);
                long numbytes = ftell(artfile);
                fseek(artfile, 0L, SEEK_SET);
                char *buffer = (char *)calloc(numbytes, sizeof(char));
                fread(buffer, sizeof(char), numbytes, artfile);
                fclose(artfile);
                if (cfg->protocol == PROTO_RAOP && g_raopcl) {
                    raopcl_set_artwork(g_raopcl, "image/jpg", numbytes, buffer);
                }
                free(buffer);
            }
        } else {
            LOG_DEBUG("Artwork URL not supported in binary, skipping: %s", value);
        }
    } else if (strcmp(key, "VOLUME") == 0) {
        int vol = atoi(value);
        LOG_INFO("Setting volume to: %d", vol);
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_volume(g_raopcl, raopcl_float_volume(vol));
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_volume(g_ap2cl, vol);
        }
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PAUSE") == 0) {
        if (g_status == STATUS_PLAYING) {
            if (cfg->protocol == PROTO_RAOP && g_raopcl) {
                raopcl_pause(g_raopcl);
                raopcl_flush(g_raopcl);
            } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
                ap2cl_pause(g_ap2cl);
            }
            g_status = STATUS_PAUSED;
            status_paused(0);
        }
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PLAY") == 0) {
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            int latency = raopcl_latency(g_raopcl);
            uint64_t now = raopcl_get_ntp(NULL);
            uint64_t start_at = now + MS2NTP(200) - TS2NTP(latency, raopcl_sample_rate(g_raopcl));
            raopcl_start_at(g_raopcl, start_at);
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_play(g_ap2cl);
        }
        g_status = STATUS_PLAYING;
        status_playing(0);
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "STOP") == 0) {
        g_status = STATUS_STOPPED;
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_stop(g_raopcl);
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_stop(g_ap2cl);
        }
        status_print("[STATUS] stopped");
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "SENDMETA") == 0) {
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_daap(g_raopcl, 4, "minm", 's', g_metadata.title,
                            "asar", 's', g_metadata.artist,
                            "asal", 's', g_metadata.album,
                            "astn", 'i', 1);
        }
    }
}

static void *cmdpipe_reader_thread(void *arg)
{
    cli_config_t *cfg = (cli_config_t *)arg;
    uint64_t last_keepalive = raopcl_get_ntp(NULL);

    g_cmdpipe_fd = open(cfg->cmdpipe, O_RDWR | O_NONBLOCK);
    if (g_cmdpipe_fd == -1) {
        LOG_ERROR("Failed to open command pipe: %s (errno=%d)", cfg->cmdpipe, errno);
        return NULL;
    }
    LOG_INFO("Command pipe ready: %s", cfg->cmdpipe);

    while (g_running) {
        struct pollfd pfds = {.fd = g_cmdpipe_fd, .events = POLLIN};
        int n = poll(&pfds, 1, 1000);
        if (!g_running) break;

        uint64_t now = raopcl_get_ntp(NULL);
        if (cfg->protocol == PROTO_RAOP && g_raopcl && now - last_keepalive >= MS2NTP(20000)) {
            raopcl_keepalive(g_raopcl);
            last_keepalive = now;
        }

        if (n <= 0 || !(pfds.revents & POLLIN)) continue;

        ssize_t bytes_read = read(g_cmdpipe_fd, g_cmdpipe_buf, sizeof(g_cmdpipe_buf) - 1);
        if (bytes_read > 0) {
            g_cmdpipe_buf[bytes_read] = '\0';
            char *save_ptr1, *save_ptr2;
            char *line = strtok_r(g_cmdpipe_buf, "\n", &save_ptr1);
            while (line != NULL) {
                if (!g_running) break;
                char *key = strtok_r(line, "=", &save_ptr2);
                if (!key || strlen(key) == 0) goto next_line;
                char *value = strtok_r(NULL, "", &save_ptr2);
                if (!value) value = "";
                handle_command(key, value, cfg);
next_line:
                line = strtok_r(NULL, "\n", &save_ptr1);
            }
            memset(g_cmdpipe_buf, 0, sizeof(g_cmdpipe_buf));
        } else if (bytes_read < 0) {
            LOG_ERROR("Error reading from command pipe: %s", strerror(errno));
            usleep(250 * 1000);
        }
    }
    return NULL;
}

/* ---- RAOP playback loop ---- */

static int run_raop(cli_config_t *cfg, int infile)
{
    struct in_addr host_addr, player_addr;
    struct hostent *hostent;
    uint32_t netmask;
    char *iface = NULL;
    raop_crypto_t crypto = RAOP_CLEAR;
    int latency;
    uint64_t start = 0, last = 0, frames = 0;
    uint8_t *buf;
    bool got_eof = false;

    /* Resolve local interface */
    host_addr = get_interface(cfg->iface, &iface, &netmask);
    LOG_INFO("Binding to %s [%s] with mask 0x%08x", inet_ntoa(host_addr), iface ? iface : "?", ntohl(netmask));
    NFREE(iface);

    /* Resolve player */
    hostent = gethostbyname(cfg->host);
    if (!hostent) {
        status_error("Cannot resolve hostname");
        return 1;
    }
    memcpy(&player_addr.s_addr, hostent->h_addr_list[0], hostent->h_length);

    /* Check AppleTV auth requirement */
    if (cfg->am && strcasestr(cfg->am, "appletv") && cfg->pk && *cfg->pk && (!cfg->secret || !*cfg->secret)) {
        status_error("AppleTV requires authentication (need secret)");
        return 1;
    }

    /* Encryption setup */
    if ((cfg->encrypt) && cfg->et && strchr(cfg->et, '1'))
        crypto = RAOP_RSA;

    /* Handle device password */
    char *password = NULL;
    if (cfg->pw && !strcasecmp(cfg->pw, "true")) {
        if (cfg->password && *cfg->password)
            password = cfg->password;
        else {
            status_error("Password required but not supplied");
            return 1;
        }
    }

    latency = MS2TS(cfg->latency_ms, cfg->sample_rate);

    /* Create RAOP client */
    g_raopcl = raopcl_create(
        host_addr, 0, 0, cfg->dacp_id, cfg->active_remote,
        cfg->alac ? RAOP_ALAC : RAOP_ALAC_RAW,
        DEFAULT_FRAMES_PER_CHUNK, latency, crypto,
        (cfg->am && strcasestr(cfg->am, "airport")),  /* auth */
        cfg->secret ? cfg->secret : "",
        password,
        cfg->et ? cfg->et : "0,4",
        cfg->md ? cfg->md : "0,1,2",
        cfg->sample_rate, cfg->bit_depth, cfg->channels,
        cfg->volume > 0 ? raopcl_float_volume(cfg->volume) : -144.0f
    );
    if (!g_raopcl) {
        status_error("Cannot init RAOP client");
        return 1;
    }

    /* Connect */
    LOG_INFO("Connecting to %s:%d via RAOP", inet_ntoa(player_addr), cfg->port);
    if (!raopcl_connect(g_raopcl, player_addr, cfg->port, cfg->volume > 0)) {
        status_error("Cannot connect to AirPlay device");
        raopcl_destroy(g_raopcl);
        g_raopcl = NULL;
        return 1;
    }

    latency = raopcl_latency(g_raopcl);
    status_connected_legacy(inet_ntoa(player_addr), cfg->port,
                            (int)TS2MS(latency, raopcl_sample_rate(g_raopcl)));

    /* Schedule start time */
    if (cfg->ntp_start || cfg->wait_ms) {
        uint64_t now = raopcl_get_ntp(NULL);
        uint64_t start_at = (cfg->ntp_start ? cfg->ntp_start : now)
                            + MS2NTP(cfg->wait_ms)
                            - TS2NTP(latency, raopcl_sample_rate(g_raopcl));
        raopcl_start_at(g_raopcl, start_at);
    }

    g_status = STATUS_PLAYING;
    start = raopcl_get_ntp(NULL);
    /* Stdin audio format:
     * For 16-bit: s16le (2 bytes per sample, 4 bytes per frame stereo)
     * For 24-bit: s32le (4 bytes per sample, 8 bytes per frame stereo)
     *   ALAC encoder expects s24le (3 bytes/sample), so we truncate s32le→s24le.
     */
    int input_bpf = (cfg->bit_depth <= 16 ? 2 : 4) * cfg->channels;
    int alac_bpf = (cfg->bit_depth <= 16 ? 2 : 3) * cfg->channels;  /* what ALAC encoder expects */
    buf = malloc(DEFAULT_FRAMES_PER_CHUNK * input_bpf);
    uint8_t *alac_buf = (cfg->bit_depth > 16) ? malloc(DEFAULT_FRAMES_PER_CHUNK * alac_bpf) : NULL;

    /* Main audio loop */
    while (g_status != STATUS_STOPPED && (!got_eof || raopcl_is_playing(g_raopcl))) {
        uint64_t now = raopcl_get_ntp(NULL);

        /* Periodic status reporting */
        if (now - last > MS2NTP(1000)) {
            last = now;
            if (frames > (uint64_t)raopcl_latency(g_raopcl)) {
                uint32_t elapsed = TS2MS(frames - raopcl_latency(g_raopcl), raopcl_sample_rate(g_raopcl));
                status_elapsed_legacy(elapsed, frames, g_raopcl);
            }
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING && raopcl_accept_frames(g_raopcl)) {
            int n = read(infile, buf, DEFAULT_FRAMES_PER_CHUNK * input_bpf);
            if (n < 0) {
                status_error("Error reading from audio source");
                break;
            } else if (n == 0) {
                if (!got_eof) {
                    LOG_INFO("End of audio stream, draining buffer...");
                    got_eof = true;
                }
                continue;
            }

            int audio_frames = n / input_bpf;
            uint8_t *send_buf = buf;
            /* For 24-bit: truncate s32le input to s24le (packed 3 bytes) for ALAC encoder */
            if (alac_buf && cfg->bit_depth > 16) {
                truncate_32to24(buf, n, alac_buf);
                send_buf = alac_buf;
            }
            uint64_t playtime;
            raopcl_send_chunk(g_raopcl, send_buf, audio_frames, &playtime);
            frames += audio_frames;
        } else {
            usleep(1000);
        }
    }

    status_eof();
    g_running = false;
    free(buf);
    raopcl_disconnect(g_raopcl);
    raopcl_destroy(g_raopcl);
    g_raopcl = NULL;
    return 0;
}

/* ---- AirPlay 2 playback loop ---- */

static int run_airplay2(cli_config_t *cfg, int infile)
{
    ap2_device_info_t device = {
        .name = cfg->ap2_name,
        .hostname = cfg->ap2_hostname,
        .address = cfg->host,
        .port = cfg->port,
        .txt_records = cfg->ap2_txt,
    };
    ap2_audio_format_t format = {
        .sample_rate = cfg->sample_rate,
        .bit_depth = cfg->bit_depth,
        .channels = cfg->channels,
    };

    g_ap2cl = ap2cl_create(&device, &format,
                            cfg->auth, cfg->password,
                            cfg->dacp_id, cfg->active_remote,
                            cfg->latency_ms, cfg->volume);
    if (!g_ap2cl) {
        status_error("Cannot create AirPlay 2 client");
        return 1;
    }

    /* Pass through mDNS properties and interface for RAOP-compatible flow */
    ap2cl_set_raop_props(g_ap2cl, cfg->iface, cfg->secret,
                          cfg->et, cfg->md, cfg->am);

    /* Connect: auth-setup + RAOP ANNOUNCE/SETUP/RECORD */
    LOG_INFO("Connecting to %s:%d via AirPlay 2", cfg->host, cfg->port);
    if (!ap2cl_connect(g_ap2cl)) {
        status_error("Cannot connect to AirPlay 2 device");
        ap2cl_destroy(g_ap2cl);
        g_ap2cl = NULL;
        return 1;
    }

    status_connected();

    /* Set volume */
    if (cfg->volume > 0) {
        ap2cl_set_volume(g_ap2cl, cfg->volume);
    }

    /* Schedule start time */
    if (cfg->ntp_start) {
        ap2cl_start_at(g_ap2cl, cfg->ntp_start);
    } else {
        uint64_t now = raopcl_get_ntp(NULL);
        uint64_t start_at = now + MS2NTP(cfg->wait_ms > 0 ? cfg->wait_ms : 200);
        ap2cl_start_at(g_ap2cl, start_at);
    }

    g_status = STATUS_PLAYING;
    uint64_t last = 0, frames = 0;
    int ap2_input_bpf = (cfg->bit_depth <= 16 ? 2 : 4) * cfg->channels;
    int ap2_alac_bpf = (cfg->bit_depth <= 16 ? 2 : 3) * cfg->channels;
    uint8_t *buf = malloc(AP2_FRAMES_PER_CHUNK * ap2_input_bpf);
    uint8_t *ap2_alac_buf = (cfg->bit_depth > 16) ? malloc(AP2_FRAMES_PER_CHUNK * ap2_alac_bpf) : NULL;
    bool got_eof = false;

    uint64_t eof_time = 0;

    /* Main audio loop */
    while (g_status != STATUS_STOPPED && (!got_eof || ap2cl_is_playing(g_ap2cl))) {
        uint64_t now = raopcl_get_ntp(NULL);

        /* After EOF, drain for at most latency + 2 seconds then stop */
        if (got_eof && eof_time && now - eof_time > MS2NTP(cfg->latency_ms + 2000)) {
            LOG_INFO("Drain timeout reached, ending stream");
            break;
        }

        /* Periodic status reporting */
        if (now - last > MS2NTP(1000)) {
            last = now;
            uint32_t latency_frames = MS2TS(cfg->latency_ms, cfg->sample_rate);
            if (frames > latency_frames) {
                uint32_t elapsed = TS2MS(frames - latency_frames, cfg->sample_rate);
                status_playing(elapsed);
            }
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING && ap2cl_accept_frames(g_ap2cl)) {
            int n = read(infile, buf, AP2_FRAMES_PER_CHUNK * ap2_input_bpf);
            if (n < 0) {
                status_error("Error reading from audio source");
                break;
            } else if (n == 0) {
                if (!got_eof) {
                    LOG_INFO("End of audio stream, draining buffer...");
                    got_eof = true;
                    eof_time = raopcl_get_ntp(NULL);
                }
                continue;
            }

            int af = n / ap2_input_bpf;
            uint8_t *send = buf;
            if (ap2_alac_buf && cfg->bit_depth > 16) {
                truncate_32to24(buf, n, ap2_alac_buf);
                send = ap2_alac_buf;
            }
            ap2cl_send_chunk(g_ap2cl, send, af);
            frames += af;
        } else {
            usleep(1000);
        }
    }

    status_eof();
    g_running = false;
    free(buf);
    ap2cl_destroy(g_ap2cl);
    g_ap2cl = NULL;
    return 0;
}


/* ---- Signal handling ---- */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
    g_status = STATUS_STOPPED;
}

/* ---- Usage ---- */

static void print_usage(const char *name)
{
    printf("cliairplay v%s - Unified AirPlay streaming CLI\n\n", VERSION);
    printf("Usage: %s [options] <host_ip> <filename ('-' for stdin)>\n\n", name);
    printf("Protocol selection:\n");
    printf("  --protocol <raop|airplay2>  Protocol to use (default: raop)\n\n");
    printf("Common options:\n");
    printf("  --port <port>              Device port (default: 5000)\n");
    printf("  --volume <0-100>           Initial volume level\n");
    printf("  --latency <ms>             Output buffer duration in ms (default: 1000)\n");
    printf("  --ntpstart <ntp>           Start at NTP timestamp\n");
    printf("  --wait <ms>                Wait before starting (ms)\n");
    printf("  --dacp <id>                DACP ID\n");
    printf("  --activeremote <id>        Active Remote ID\n");
    printf("  --cmdpipe <path>           Named pipe for metadata/commands\n");
    printf("  --udn <name>               UDN name for mDNS\n");
    printf("  --samplerate <rate>        Sample rate (default: 44100)\n");
    printf("  --bitdepth <bits>          Bit depth: 16 or 24 (default: 16)\n");
    printf("  --channels <n>             Channel count (default: 2)\n");
    printf("  --debug <0-9>              Debug level (default: 3)\n");
    printf("  --ntp                      Print current NTP and exit\n");
    printf("  --check                    Print check info and exit\n");
    printf("  --pair                     Enter pairing mode\n\n");
    printf("RAOP options:\n");
    printf("  --encrypt                  Enable audio payload encryption\n");
    printf("  --alac                     Send ALAC compressed audio\n");
    printf("  --secret <secret>          AppleTV pairing secret\n");
    printf("  --password <password>      Device password\n");
    printf("  --if <ip>                  Local interface IP to bind\n");
    printf("  --et <value>               mDNS et field\n");
    printf("  --md <value>               mDNS md field\n");
    printf("  --am <value>               mDNS am field (model name)\n");
    printf("  --pk <value>               mDNS pk field\n");
    printf("  --pw <value>               mDNS pw field\n\n");
    printf("AirPlay 2 options:\n");
    printf("  --auth <credentials>       HAP credentials (hex string)\n");
    printf("  --name <name>              Device name\n");
    printf("  --hostname <hostname>      Device hostname\n");
    printf("  --txt <records>            mDNS TXT records (key=value pairs)\n");
    printf("  --ptp-offset <ns>          PTP clock offset in nanoseconds\n\n");
    printf("Examples:\n");
    printf("  # RAOP streaming from stdin:\n");
    printf("  ffmpeg -i song.flac -f s16le -ar 44100 -ac 2 - | %s 192.168.1.50 -\n\n", name);
    printf("  # AirPlay 2 streaming:\n");
    printf("  ffmpeg -i song.flac -f s16le -ar 44100 -ac 2 - | \\\n");
    printf("    %s --protocol airplay2 --auth <creds> --name \"HomePod\" 192.168.1.50 -\n", name);
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    cli_config_t cfg = {
        .protocol = PROTO_RAOP,
        .port = 5000,
        .volume = 0,
        .latency_ms = 1000,
        .debug_level = 3,
        .dacp_id = "1A2B3D4EA1B2C3D4",
        .active_remote = "ap5918800d",
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
        .et = "0,4",
        .md = "0,1,2",
        .am = "",
        .pk = "",
        .pw = "",
    };

    bool pairing_mode = false;
    int infile = -1;

    static struct option long_options[] = {
        {"protocol",     required_argument, 0, 'P'},
        {"port",         required_argument, 0, 'p'},
        {"volume",       required_argument, 0, 'v'},
        {"latency",      required_argument, 0, 'l'},
        {"ntpstart",     required_argument, 0, 'N'},
        {"wait",         required_argument, 0, 'w'},
        {"dacp",         required_argument, 0, 'D'},
        {"activeremote", required_argument, 0, 'R'},
        {"cmdpipe",      required_argument, 0, 'C'},
        {"udn",          required_argument, 0, 'U'},
        {"samplerate",   required_argument, 0, 'r'},
        {"bitdepth",     required_argument, 0, 'b'},
        {"channels",     required_argument, 0, 'c'},
        {"debug",        required_argument, 0, 'd'},
        {"encrypt",      no_argument,       0, 'e'},
        {"alac",         no_argument,       0, 'a'},
        {"secret",       required_argument, 0, 's'},
        {"password",     required_argument, 0, 'x'},
        {"if",           required_argument, 0, 'I'},
        {"et",           required_argument, 0, 'E'},
        {"md",           required_argument, 0, 'M'},
        {"am",           required_argument, 0, 'A'},
        {"pk",           required_argument, 0, 'K'},
        {"pw",           required_argument, 0, 'W'},
        {"auth",         required_argument, 0, 'T'},
        {"name",         required_argument, 0, 'n'},
        {"hostname",     required_argument, 0, 'H'},
        {"txt",          required_argument, 0, 't'},
        {"ptp-offset",   required_argument, 0, 1004},
        {"ntp",          no_argument,       0, 1001},
        {"check",        no_argument,       0, 1002},
        {"pair",         no_argument,       0, 1003},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    setvbuf(stderr, NULL, _IONBF, 0);  /* Unbuffered stderr for status output */

    int opt;
    while ((opt = getopt_long(argc, argv, "hP:p:v:l:N:w:D:R:C:U:r:b:c:d:eas:x:I:E:M:A:K:W:T:n:H:t:",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'P':
            if (strcmp(optarg, "raop") == 0) cfg.protocol = PROTO_RAOP;
            else if (strcmp(optarg, "airplay2") == 0) cfg.protocol = PROTO_AIRPLAY2;
            else { fprintf(stderr, "Unknown protocol: %s\n", optarg); return 1; }
            break;
        case 'p': cfg.port = atoi(optarg); break;
        case 'v': cfg.volume = atoi(optarg); break;
        case 'l': cfg.latency_ms = atoi(optarg); break;
        case 'N': sscanf(optarg, "%" PRIu64, &cfg.ntp_start); break;
        case 'w': cfg.wait_ms = atoi(optarg); break;
        case 'D': cfg.dacp_id = optarg; break;
        case 'R': cfg.active_remote = optarg; break;
        case 'C': cfg.cmdpipe = optarg; break;
        case 'U': cfg.udn = optarg; break;
        case 'r': cfg.sample_rate = atoi(optarg); break;
        case 'b': cfg.bit_depth = atoi(optarg); break;
        case 'c': cfg.channels = atoi(optarg); break;
        case 'd': cfg.debug_level = atoi(optarg); break;
        case 'e': cfg.encrypt = true; break;
        case 'a': cfg.alac = true; break;
        case 's': cfg.secret = optarg; break;
        case 'x': cfg.password = optarg; break;
        case 'I': cfg.iface = optarg; break;
        case 'E': cfg.et = optarg; break;
        case 'M': cfg.md = optarg; break;
        case 'A': cfg.am = optarg; break;
        case 'K': cfg.pk = optarg; break;
        case 'W': cfg.pw = optarg; break;
        case 'T': cfg.auth = optarg; break;
        case 'n': cfg.ap2_name = optarg; break;
        case 'H': cfg.ap2_hostname = optarg; break;
        case 't': cfg.ap2_txt = optarg; break;
        case 1004: sscanf(optarg, "%" PRId64, &cfg.ptp_offset_ns); break;
        case 1001: {
            uint64_t ntp = raopcl_get_ntp(NULL);
            printf("%" PRIu64 "\n", ntp);
            return 0;
        }
        case 1002:
            printf("cliairplay v%s check\n", VERSION);
            return 0;
        case 1003:
            pairing_mode = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* Remaining positional args: host and filename */
    if (optind < argc) cfg.host = argv[optind++];
    if (optind < argc) cfg.audio_source = argv[optind++];

    /* Setup debug levels */
    if (cfg.debug_level >= (int)NUM_DEBUG_LEVELS)
        cfg.debug_level = NUM_DEBUG_LEVELS - 1;
    util_loglevel = debug_levels[cfg.debug_level].util;
    raop_loglevel = debug_levels[cfg.debug_level].raop;
    main_log = debug_levels[cfg.debug_level].main;

    /* Initialize platform */
    netsock_init();
    cross_ssl_load();

    /* Pairing mode */
    if (pairing_mode) {
        char *pair_udn = NULL, *pair_secret = NULL;
        if (AppleTVpairing(NULL, &pair_udn, &pair_secret)) {
            fprintf(stderr, "\nPairing successful!\nUDN: %s\nSecret: %s\n",
                    pair_udn ? pair_udn : "(none)",
                    pair_secret ? pair_secret : "(none)");
        } else {
            fprintf(stderr, "Pairing failed.\n");
        }
        return 0;
    }

    /* Validate required args */
    if (!cfg.host || !cfg.audio_source) {
        print_usage(argv[0]);
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Open audio source */
    if (strcmp(cfg.audio_source, "-") == 0) {
        infile = fileno(stdin);
        LOG_INFO("Reading audio from stdin");
    } else {
        struct stat st;
        bool is_fifo = false;
        if (stat(cfg.audio_source, &st) == 0) {
            is_fifo = S_ISFIFO(st.st_mode);
        } else if (mkfifo(cfg.audio_source, 0666) == 0) {
            is_fifo = true;
        }
        int flags = O_RDONLY;
        if (is_fifo) flags |= O_NONBLOCK;
        if ((infile = open(cfg.audio_source, flags)) == -1) {
            status_error("Cannot open audio source");
            return 1;
        }
        if (is_fifo) {
            int file_flags = fcntl(infile, F_GETFL);
            fcntl(infile, F_SETFL, file_flags & ~O_NONBLOCK);
        }
    }

    /* Setup command pipe */
    if (cfg.cmdpipe) {
        struct stat st;
        if (stat(cfg.cmdpipe, &st) != 0) {
            if (mkfifo(cfg.cmdpipe, 0666) != 0) {
                status_error("Failed to create command pipe");
                return 1;
            }
        }
        pthread_create(&g_cmdpipe_thread, NULL, cmdpipe_reader_thread, &cfg);
    }

    /* Run the selected protocol */
    int result;
    if (cfg.protocol == PROTO_RAOP) {
        result = run_raop(&cfg, infile);
    } else {
        result = run_airplay2(&cfg, infile);
    }

    /* Cleanup */
    g_running = false;
    if (cfg.cmdpipe) {
        pthread_join(g_cmdpipe_thread, NULL);
        if (g_cmdpipe_fd >= 0) close(g_cmdpipe_fd);
        unlink(cfg.cmdpipe);
    }
    if (infile >= 0 && infile != fileno(stdin)) close(infile);
    netsock_close();
    cross_ssl_free();

    return result;
}
