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
#include <errno.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_thread.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/crosstools/src/cross_ssl.h"
#include "../libraop/src/raop_client.h"
#include "cross_util.h"
#include "cross_log.h"
#include "ap2_client.h"
#include "ap2_rtsp.h"
#include "ap2_ptp.h"
#include "ap2_hap.h"

#define VERSION "0.1.0"
#define AP2_FRAMES_PER_CHUNK 352

/* Protocol selection (resolved, concrete protocol used for dispatch). */
typedef enum {
    PROTO_AUTO = 0,
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
    protocol_t protocol;         /* resolved concrete protocol (RAOP/AIRPLAY2) */
    ap2_proto_pref_t proto_pref; /* user --protocol preference (auto/raop/airplay2) */
    ap2_route_t route;           /* resolved route (AirPlay 2 sub-decisions) */
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
    char *audio_source;  /* filename or "-" for stdin */

    /* RAOP-specific */
    bool encrypt;
    bool raw;          /* force uncompressed audio instead of compressed ALAC */
    char *secret;
    char *password;
    char *et;
    char *md;
    char *am;
    char *pk;
    char *pw;
    char *cn;          /* mDNS cn field: codec/compression types the device supports */
    char *iface;

    /* AirPlay 2-specific */
    char *auth;       /* HAP credentials (hex) */
    char *ap2_name;
    char *ap2_hostname;
    char *ap2_txt;    /* mDNS TXT records */
    bool force_native;      /* force native AP2 flow (transient pairing) */
    char *publish_ip;       /* address advertised to devices (multi-homed hosts) */
    bool ptp;               /* force PTP grandmaster timing for native AP2 */
    bool ptp_shared;        /* prefer a shared PTP daemon clock (multi-room) */
    bool buffered;          /* force buffered audio stream (type 103) */

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

/* Path-A MRP experiment: surface the POST /command response on stdout when it
 * changes, so the caller can log whether the device accepts the push. */
static void mrp_status_report(int status)
{
    static int last;
    if (status <= 0 || status == last) return;
    last = status;
    printf("[STATUS] mrp path=command status=%d\n", status);
    fflush(stdout);
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

/* ---- Eager input buffering ---- */

/*
 * A dedicated reader drains the audio source into this ring as fast as the
 * source can deliver, decoupled from network pacing. The caller (MA) can
 * therefore fill the pipeline well before a scheduled group start instead of
 * being throttled to the send schedule, so playback start cannot underrun on
 * slow source startup. When the ring is full the reader blocks, which
 * backpressures the pipe as before.
 */
#define INPUT_RING_BYTES (4u * 1024 * 1024)  /* allocation; admission is capped by playtime */

static struct {
    uint8_t *data;
    size_t rd, wr, fill;
    size_t max_fill;
    bool eof;
    bool started;
    int fd;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t can_read;
    pthread_cond_t can_write;
} g_inring = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER,
               .can_read = PTHREAD_COND_INITIALIZER,
               .can_write = PTHREAD_COND_INITIALIZER };

static void *input_ring_thread(void *arg)
{
    (void)arg;
    uint8_t chunk[65536];
    while (g_running) {
        ssize_t n = read(g_inring.fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&g_inring.lock);
        if (n <= 0) {
            g_inring.eof = true;
            pthread_cond_broadcast(&g_inring.can_read);
            pthread_mutex_unlock(&g_inring.lock);
            break;
        }
        size_t off = 0;
        while (off < (size_t)n && g_running) {
            while (g_inring.fill >= g_inring.max_fill && g_running)
                pthread_cond_wait(&g_inring.can_write, &g_inring.lock);
            size_t space = g_inring.max_fill - g_inring.fill;
            size_t chunk_len = (size_t)n - off;
            if (chunk_len > space) chunk_len = space;
            size_t to_end = INPUT_RING_BYTES - g_inring.wr;
            if (chunk_len > to_end) chunk_len = to_end;
            memcpy(g_inring.data + g_inring.wr, chunk + off, chunk_len);
            g_inring.wr = (g_inring.wr + chunk_len) % INPUT_RING_BYTES;
            g_inring.fill += chunk_len;
            off += chunk_len;
            pthread_cond_broadcast(&g_inring.can_read);
        }
        pthread_mutex_unlock(&g_inring.lock);
    }
    return NULL;
}

static void input_ring_start(int fd, unsigned byte_rate, int cap_ms)
{
    g_inring.data = malloc(INPUT_RING_BYTES);
    g_inring.fd = fd;
    /* Cap admission by buffered PLAYTIME, not allocation size: an over-deep
     * ring lets the feeder run tens of seconds ahead of the audible position,
     * which breaks anything mapping feed position to wall clock (sync-group
     * late joiners join silent until the clock catches up). The cap covers
     * the pre-start prefill (latency) plus margin. */
    uint64_t cap = (uint64_t)byte_rate * (uint64_t)cap_ms / 1000;
    g_inring.max_fill = (cap && cap < INPUT_RING_BYTES) ? (size_t)cap : INPUT_RING_BYTES;
    g_inring.started = true;
    pthread_create(&g_inring.thread, NULL, input_ring_thread, NULL);
}

/* Read up to `want` bytes; blocks until data or EOF. Returns 0 only at EOF. */
static int input_ring_read(uint8_t *buf, size_t want)
{
    pthread_mutex_lock(&g_inring.lock);
    while (g_inring.fill == 0 && !g_inring.eof && g_running)
        pthread_cond_wait(&g_inring.can_read, &g_inring.lock);
    size_t n = g_inring.fill < want ? g_inring.fill : want;
    size_t got = 0;
    while (got < n) {
        size_t to_end = INPUT_RING_BYTES - g_inring.rd;
        size_t chunk_len = n - got;
        if (chunk_len > to_end) chunk_len = to_end;
        memcpy(buf + got, g_inring.data + g_inring.rd, chunk_len);
        g_inring.rd = (g_inring.rd + chunk_len) % INPUT_RING_BYTES;
        got += chunk_len;
    }
    g_inring.fill -= n;
    if (n) pthread_cond_broadcast(&g_inring.can_write);
    pthread_mutex_unlock(&g_inring.lock);
    return (int)n;
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
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_progress(g_ap2cl, g_metadata.progress, g_metadata.duration);
            mrp_status_report(ap2cl_mrp_push(g_ap2cl));
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
                } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
                    ap2cl_set_artwork(g_ap2cl, "image/jpeg", numbytes, buffer);
                    mrp_status_report(ap2cl_mrp_push(g_ap2cl));
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
        /* No status emission here: reporting elapsed_ms=0 would snap the
         * sender's position display backwards. The periodic reporter (gated on
         * STATUS_PLAYING) re-reports the true elapsed within a second. */
        g_status = STATUS_PLAYING;
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
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_metadata(g_ap2cl, g_metadata.title, g_metadata.artist,
                               g_metadata.album, g_metadata.duration);
            mrp_status_report(ap2cl_mrp_push(g_ap2cl));
        }
    }
}

/* Some receivers (notably Sonos) will not emit any audio until they have received
 * at least one metadata command. Push the current metadata (or a minimal
 * placeholder title) once the session is established, so audio starts regardless
 * of whether — or when — the caller sends a SENDMETA command. Any later metadata
 * from the caller simply overwrites this. */
static void send_initial_metadata(const cli_config_t *cfg)
{
    const char *title = (g_metadata.title && *g_metadata.title) ? g_metadata.title : "cliairplay";
    if (cfg->protocol == PROTO_RAOP && g_raopcl) {
        raopcl_set_daap(g_raopcl, 4, "minm", 's', title,
                        "asar", 's', g_metadata.artist,
                        "asal", 's', g_metadata.album, "astn", 'i', 1);
    } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
        ap2cl_set_metadata(g_ap2cl, title, g_metadata.artist,
                           g_metadata.album, g_metadata.duration);
        mrp_status_report(ap2cl_mrp_push(g_ap2cl));
    }
}

static void *cmdpipe_reader_thread(void *arg)
{
    cli_config_t *cfg = (cli_config_t *)arg;
    uint64_t last_keepalive = raopcl_get_ntp(NULL);
    uint64_t last_feedback = last_keepalive;

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
        /* Native AP2 keepalive: real senders POST /feedback about every 2 s
         * and long sessions can hit receiver-side idle timeouts without it
         * (no-op for the RAOP-compat flow, which libraop keeps alive). */
        if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl && ap2cl_is_connected(g_ap2cl) &&
            now - last_feedback >= MS2NTP(2000)) {
            ap2cl_feedback(g_ap2cl);
            last_feedback = now;
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
    uint64_t last = 0, frames = 0;
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

    /* Codec selection: default to compressed ALAC, which virtually every RAOP
     * receiver advertises and which saves LAN bandwidth. Fall back to uncompressed
     * only when the device's mDNS cn field is present and does not list ALAC (1),
     * or when --raw is forced. */
    bool use_alac = true;
    if (cfg->cn && *cfg->cn && !strchr(cfg->cn, '1')) use_alac = false;
    if (cfg->raw) use_alac = false;
    LOG_INFO("RAOP codec: %s", use_alac ? "ALAC (compressed)" : "ALAC-raw (uncompressed)");

    /* Create RAOP client */
    g_raopcl = raopcl_create(
        host_addr, 0, 0, cfg->dacp_id, cfg->active_remote,
        use_alac ? RAOP_ALAC : RAOP_ALAC_RAW,
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
    send_initial_metadata(cfg);

    /* Schedule start time */
    if (cfg->ntp_start) {
        /* Contract: the first sample is AUDIBLE exactly at the requested start
         * (RAOP renders a frame latency after its frame-clock position, so the
         * timeline starts latency early). */
        raopcl_start_at(g_raopcl,
                        cfg->ntp_start - TS2NTP(latency, raopcl_sample_rate(g_raopcl)));
    }

    g_status = STATUS_PLAYING;
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

        /* Periodic status reporting (only while playing, so a pause does not keep
           emitting a "playing" status that would revive the sender's play state) */
        if (g_status == STATUS_PLAYING && now - last > MS2NTP(1000)) {
            last = now;
            if (frames > (uint64_t)raopcl_latency(g_raopcl)) {
                uint32_t elapsed = TS2MS(frames - raopcl_latency(g_raopcl), raopcl_sample_rate(g_raopcl));
                status_elapsed_legacy(elapsed, frames, g_raopcl);
            }
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING && raopcl_accept_frames(g_raopcl)) {
            int n = input_ring_read(buf, DEFAULT_FRAMES_PER_CHUNK * input_bpf);
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
    free(alac_buf);
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
    /* Apply the resolved AirPlay 2 route (see ap2_resolve_route). force_native
     * is a no-op when stored credentials already select the native flow. */
    if (cfg->route.native)
        ap2cl_force_native(g_ap2cl);
    if (cfg->publish_ip)
        ap2cl_set_publish_ip(g_ap2cl, cfg->publish_ip);
    ap2cl_set_ptp(g_ap2cl, cfg->route.ptp);
    ap2cl_set_ptp_shared(g_ap2cl, cfg->ptp_shared);
    ap2cl_set_buffered(g_ap2cl, cfg->route.buffered);

    /* Connect: auth-setup + RAOP ANNOUNCE/SETUP/RECORD */
    LOG_INFO("Connecting to %s:%d via AirPlay 2", cfg->host, cfg->port);
    if (!ap2cl_connect(g_ap2cl)) {
        status_error("Cannot connect to AirPlay 2 device");
        ap2cl_destroy(g_ap2cl);
        g_ap2cl = NULL;
        return 1;
    }

    status_connected();

    /* Report the MRP now-playing path so MA can log which one is active. The
     * type-130 data channel (path B) is set up during connect; -1 means the
     * session is not an Apple/pair-verified target and MRP does not apply. */
    {
        int mrp_ch = ap2cl_mrp_channel_status(g_ap2cl);
        if (mrp_ch >= 0) {
            printf("[STATUS] mrp path=channel status=%d\n", mrp_ch);
            fflush(stdout);
        }
    }

    /* Surface the effective lead and the receiver-reported buffering window so
     * the caller (MA) can plan group starts from real device capabilities. */
    {
        int lead_ms = 0;
        uint32_t dev_min = 0, dev_max = 0;
        ap2cl_latency_info(g_ap2cl, &lead_ms, &dev_min, &dev_max);
        printf("[STATUS] latency lead_ms=%d device_min_frames=%u device_max_frames=%u "
               "device_render_ms=%d\n",
               lead_ms, dev_min, dev_max, ap2cl_render_latency_ms(g_ap2cl));
        fflush(stdout);
    }

    /* Set volume */
    if (cfg->volume > 0) {
        ap2cl_set_volume(g_ap2cl, cfg->volume);
    }
    send_initial_metadata(cfg);

    /* Schedule start time */
    if (cfg->ntp_start) {
        ap2cl_start_at(g_ap2cl, cfg->ntp_start);
    } else {
        uint64_t now = raopcl_get_ntp(NULL);
        uint64_t start_at = now + MS2NTP(cfg->latency_ms);
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

        /* Periodic status reporting (only while playing, so a pause does not keep
           emitting a "playing" status that would revive the sender's play state) */
        if (g_status == STATUS_PLAYING && now - last > MS2NTP(1000)) {
            last = now;
            uint32_t latency_frames = MS2TS(cfg->latency_ms, cfg->sample_rate);
            if (frames > latency_frames) {
                uint32_t elapsed = TS2MS(frames - latency_frames, cfg->sample_rate);
                status_playing(elapsed);
            }
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING && ap2cl_accept_frames(g_ap2cl)) {
            int n = input_ring_read(buf, AP2_FRAMES_PER_CHUNK * ap2_input_bpf);
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
    free(ap2_alac_buf);
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

/* ---- HomeKit pair-setup mode ---- */

/* Read the on-screen PIN from stdin (prompt on stderr, so stdout stays clean
 * for the CREDENTIALS line the caller parses). */
static const char *pair_setup_pin_prompt(void *arg)
{
    static char pin[32];
    (void)arg;
    fprintf(stderr, "Enter the PIN shown on the device: ");
    fflush(stderr);
    if (!fgets(pin, sizeof(pin), stdin)) return NULL;
    pin[strcspn(pin, "\r\n")] = '\0';
    return pin[0] ? pin : NULL;
}

/* Run `cliairplay --pair-setup <host> --port 7000 --dacp <id>`: full HomeKit
 * pairing (PIN) producing --auth credentials on stdout. Returns exit code. */
static int run_pair_setup(cli_config_t *cfg)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(cfg->port)};
    if (inet_pton(AF_INET, cfg->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "--pair-setup needs a literal IPv4 address\n");
        return 1;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Cannot connect to %s:%d\n", cfg->host, cfg->port);
        if (sock >= 0) close(sock);
        return 1;
    }

    struct ap2_hap_ctx *hap = ap2_hap_create(NULL);
    if (!hap) { close(sock); return 1; }

    /* The pairing identifier must match the DACP id later used at stream
     * time (pair-verify signs with it), uppercased like the stream path. */
    const char *dacp = cfg->dacp_id ? cfg->dacp_id : "0";
    char upper_dacp[32];
    int len = (int)strlen(dacp);
    if (len > 30) len = 30;
    for (int i = 0; i < len; i++) {
        char c = dacp[i];
        upper_dacp[i] = (c >= 'a' && c <= 'f') ? (c - 'a' + 'A') : c;
    }
    upper_dacp[len] = '\0';
    ap2_hap_set_client_id(hap, (const uint8_t *)upper_dacp, len);

    char creds[193];
    bool ok = ap2_hap_pair_setup_pin(hap, sock, pair_setup_pin_prompt, NULL, creds);
    ap2_hap_destroy(hap);
    close(sock);

    if (!ok) {
        fprintf(stderr, "Pairing failed.\n");
        return 1;
    }
    printf("CREDENTIALS: %s\n", creds);
    fflush(stdout);
    fprintf(stderr, "Pairing successful. Pass the credentials via --auth "
                    "(with the same --dacp).\n");
    return 0;
}

/* ---- PTP daemon mode ---- */

static volatile bool g_ptp_daemon_stop = false;

static void ptp_daemon_signal_handler(int sig)
{
    (void)sig;
    g_ptp_daemon_stop = true;
}

/* Run `cliairplay --ptp-daemon`: own UDP 319/320, run the shared PTP clock, and
 * serve the control channel until signaled. Returns the process exit code. */
static int run_ptp_daemon(cli_config_t *cfg)
{
    struct in_addr bind_addr;
    bind_addr.s_addr = INADDR_ANY;
    if (cfg->iface && *cfg->iface) {
        char *ifname = NULL;
        uint32_t netmask;
        bind_addr = get_interface(cfg->iface, &ifname, &netmask);
        LOG_INFO("[PTP] daemon binding multicast to %s [%s]",
                 inet_ntoa(bind_addr), ifname ? ifname : "?");
        NFREE(ifname);
    }

    signal(SIGINT, ptp_daemon_signal_handler);
    signal(SIGTERM, ptp_daemon_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    return ap2_ptp_run_daemon(bind_addr, &g_ptp_daemon_stop);
}

/* ---- Usage ---- */

static void print_usage(const char *name)
{
    printf("cliairplay v%s - Unified AirPlay streaming CLI\n\n", VERSION);
    printf("Usage: %s [options] <host_ip> <filename ('-' for stdin)>\n\n", name);
    printf("Protocol selection:\n");
    printf("  --protocol <auto|raop|airplay2>  Protocol to use (default: auto).\n");
    printf("                             auto picks RAOP vs AirPlay 2 from the mDNS\n");
    printf("                             features in --txt; raop/airplay2 force it.\n\n");
    printf("Common options:\n");
    printf("  --port <port>              Device port (default: 5000)\n");
    printf("  --volume <0-100>           Initial volume level\n");
    printf("  --latency <ms>             Playback lead / buffer in ms (default: 2000,\n");
    printf("                             clamped into the device-reported window)\n");
    printf("  --start-unix-ms <ms>       Start at unix epoch milliseconds (preferred:\n");
    printf("                             the caller never handles NTP formats; pass the\n");
    printf("                             SAME value to every member of a sync group)\n");
    printf("  --dacp <id>                DACP ID\n");
    printf("  --activeremote <id>        Active Remote ID\n");
    printf("  --cmdpipe <path>           Named pipe for metadata/commands\n");
    printf("  --udn <name>               UDN name for mDNS\n");
    printf("  --samplerate <rate>        Sample rate (default: 44100)\n");
    printf("  --bitdepth <bits>          Bit depth: 16 or 24 (default: 16). 24-bit\n");
    printf("                             uses native AirPlay 2 ALAC (0x80000/0x200000)\n");
    printf("  --channels <n>             Channel count (default: 2)\n");
    printf("  --if <ip>                  Local interface IP to bind (multi-homed hosts)\n");
    printf("  --debug <0-9>              Debug level (default: 3)\n");
    printf("  --check                    Print check info and exit\n");
    printf("  --pair                     Legacy AppleTV RAOP pairing (--secret)\n");
    printf("  --pair-setup               HomeKit pair-setup: the device shows a PIN,\n");
    printf("                             prints --auth credentials on success. Needs\n");
    printf("                             <address>, --port and --dacp\n\n");
    printf("RAOP options:\n");
    printf("  --raw                      Force uncompressed audio (ALAC-raw)\n");
    printf("  --encrypt                  Enable audio payload encryption\n");
    printf("  --secret <secret>          AppleTV pairing secret\n");
    printf("  --password <password>      Device password\n");
    printf("  --et <value>               mDNS et field (encryption types)\n");
    printf("  --md <value>               mDNS md field (metadata types)\n");
    printf("  --am <value>               mDNS am field (model name)\n");
    printf("  --pk <value>               mDNS pk field (public key)\n");
    printf("  --pw <value>               mDNS pw field (password flag)\n");
    printf("  --cn <value>               mDNS cn field (codec types); auto-selects codec\n\n");
    printf("AirPlay 2 options:\n");
    printf("  --auth <credentials>       HAP credentials (hex string, stored pairing)\n");
    printf("  --ap2-native               Force native AP2 flow without credentials\n");
    printf("                             (transient pairing; default is RAOP-compat)\n");
    printf("  --publish-ip <ip>          Address advertised to devices (multi-homed hosts)\n");
    printf("  --name <name>              Device name\n");
    printf("  --hostname <hostname>      Device hostname\n");
    printf("  --txt <records>            mDNS TXT records (key=value pairs)\n");
    printf("  --ptp                      Force PTP grandmaster timing (native AP2;\n");
    printf("                             binds UDP 319/320, needs root; else auto by\n");
    printf("                             SupportsPTP feature bit)\n");
    printf("  --buffered                 Force the buffered audio stream (type 103,\n");
    printf("                             native AP2, RTP over TCP + PTP anchor); else\n");
    printf("                             auto when SupportsBufferedAudio + 24-bit\n");
    printf("  --ptp-shared               Prefer a shared PTP daemon clock (multi-room):\n");
    printf("                             read the elected clock from shared memory and do\n");
    printf("                             not bind 319/320 when a daemon is present; else\n");
    printf("                             fall back to the in-process engine\n");
    printf("  --ptp-daemon               Run ONLY the shared PTP clock: bind 319/320 once,\n");
    printf("                             publish the elected master to shared memory, and\n");
    printf("                             serve the control channel until signaled. One per\n");
    printf("                             host; needs root. Takes no host/audio args.\n\n");
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
        .protocol = PROTO_AUTO,
        .proto_pref = AP2_PROTO_AUTO,
        .port = 5000,
        .volume = 0,
        .latency_ms = 2000,
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
    bool ptp_daemon_mode = false;
    bool pair_setup_mode = false;
    int infile = -1;

    static struct option long_options[] = {
        {"protocol",     required_argument, 0, 'P'},
        {"port",         required_argument, 0, 'p'},
        {"volume",       required_argument, 0, 'v'},
        {"latency",      required_argument, 0, 'l'},
        {"start-unix-ms", required_argument, 0, 1014},
        {"dacp",         required_argument, 0, 'D'},
        {"activeremote", required_argument, 0, 'R'},
        {"cmdpipe",      required_argument, 0, 'C'},
        {"udn",          required_argument, 0, 'U'},
        {"samplerate",   required_argument, 0, 'r'},
        {"bitdepth",     required_argument, 0, 'b'},
        {"channels",     required_argument, 0, 'c'},
        {"debug",        required_argument, 0, 'd'},
        {"encrypt",      no_argument,       0, 'e'},
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
        {"cn",           required_argument, 0, 1005},
        {"raw",          no_argument,       0, 1006},
        {"ap2-native",   no_argument,       0, 1007},
        {"publish-ip",   required_argument, 0, 1008},
        {"ptp",          no_argument,       0, 1009},
        {"buffered",     no_argument,       0, 1010},
        {"ptp-daemon",   no_argument,       0, 1011},
        {"ptp-shared",   no_argument,       0, 1012},
        {"check",        no_argument,       0, 1002},
        {"pair",         no_argument,       0, 1003},
        {"pair-setup",   no_argument,       0, 1013},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    setvbuf(stderr, NULL, _IONBF, 0);  /* Unbuffered stderr for status output */

    int opt;
    while ((opt = getopt_long(argc, argv, "hP:p:v:l:D:R:C:U:r:b:c:d:es:x:I:E:M:A:K:W:T:n:H:t:",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'P':
            if (strcmp(optarg, "auto") == 0) cfg.proto_pref = AP2_PROTO_AUTO;
            else if (strcmp(optarg, "raop") == 0) cfg.proto_pref = AP2_PROTO_RAOP;
            else if (strcmp(optarg, "airplay2") == 0) cfg.proto_pref = AP2_PROTO_AIRPLAY2;
            else { fprintf(stderr, "Unknown protocol: %s\n", optarg); return 1; }
            break;
        case 'p': cfg.port = atoi(optarg); break;
        case 'v': cfg.volume = atoi(optarg); break;
        case 'l': cfg.latency_ms = atoi(optarg); break;
        case 1014: {
            /* Group start as plain unix epoch milliseconds; converted to the
             * NTP fixed-point (unix seconds << 32 | frac) used internally so
             * callers never handle NTP formats. */
            uint64_t ms = 0;
            sscanf(optarg, "%" PRIu64, &ms);
            cfg.ntp_start = ((ms / 1000) << 32) | (((ms % 1000) << 32) / 1000);
            break;
        }
        case 'D': cfg.dacp_id = optarg; break;
        case 'R': cfg.active_remote = optarg; break;
        case 'C': cfg.cmdpipe = optarg; break;
        case 'U': cfg.udn = optarg; break;
        case 'r': cfg.sample_rate = atoi(optarg); break;
        case 'b': cfg.bit_depth = atoi(optarg); break;
        case 'c': cfg.channels = atoi(optarg); break;
        case 'd': cfg.debug_level = atoi(optarg); break;
        case 'e': cfg.encrypt = true; break;
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
        case 1005: cfg.cn = optarg; break;
        case 1006: cfg.raw = true; break;
        case 1007: cfg.force_native = true; break;
        case 1008: cfg.publish_ip = optarg; break;
        case 1009: cfg.ptp = true; break;
        case 1010: cfg.buffered = true; break;
        case 1011: ptp_daemon_mode = true; break;
        case 1013: pair_setup_mode = true; break;
        case 1012: cfg.ptp_shared = true; break;
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

    /* Pairing mode (legacy AppleTV RAOP pairing -> --secret) */
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

    /* HomeKit pair-setup mode: PIN on the device's screen -> --auth credentials */
    if (pair_setup_mode) {
        if (!cfg.host) {
            fprintf(stderr, "--pair-setup needs the device address (and --port)\n");
            return 1;
        }
        int rc = run_pair_setup(&cfg);
        netsock_close();
        cross_ssl_free();
        return rc;
    }

    /* PTP daemon mode: no host/audio source; run the shared clock until signaled.
     * MA starts one of these per host for multi-room AirPlay 2 (see README). */
    if (ptp_daemon_mode) {
        int rc = run_ptp_daemon(&cfg);
        netsock_close();
        cross_ssl_free();
        return rc;
    }

    /* Validate required args */
    if (!cfg.host || !cfg.audio_source) {
        print_usage(argv[0]);
        return 1;
    }

    /* Resolve the streaming route from the discovery TXT and any overrides.
     * --protocol auto (the default) picks RAOP vs AirPlay 2 from the mDNS
     * features; explicit raop/airplay2 force the protocol; --ap2-native,
     * --buffered and --ptp are forcing overrides. This is the single decision
     * point: cfg.protocol becomes the concrete protocol used for dispatch and
     * cfg.route carries the AirPlay 2 sub-decisions applied in run_airplay2(). */
    bool have_creds = cfg.auth && strlen(cfg.auth) == 192;
    cfg.route = ap2_resolve_route(cfg.proto_pref, cfg.ap2_txt, cfg.pw, have_creds,
                                  cfg.bit_depth, cfg.force_native, cfg.buffered,
                                  cfg.ptp, cfg.ptp);
    cfg.protocol = cfg.route.use_raop ? PROTO_RAOP : PROTO_AIRPLAY2;
    LOG_INFO("[AP2] auto-selected: %s; timing=%s; features=0x%llx; flags=0x%llx; bitdepth=%d",
             cfg.route.reason,
             cfg.route.use_raop ? "n/a" : (cfg.route.ptp ? "PTP" : "NTP"),
             (unsigned long long)cfg.route.features,
             (unsigned long long)cfg.route.flags,
             cfg.bit_depth);
    /* Machine-parseable route report on stdout so the caller (MA) can log and
     * surface which route this stream actually took. */
    printf("[STATUS] route protocol=%s flow=%s timing=%s buffered=%d\n",
           cfg.route.use_raop ? "raop" : "airplay2",
           cfg.route.use_raop ? "legacy" : (cfg.route.native ? "native" : "raop-compat"),
           (!cfg.route.use_raop && cfg.route.ptp) ? "ptp" : "ntp",
           cfg.route.buffered ? 1 : 0);
    fflush(stdout);

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

    /* Drain the audio source eagerly from here on (see the input ring note),
     * keeping at most the pre-start prefill plus margin buffered. */
    unsigned in_byte_rate =
        (unsigned)((cfg.bit_depth <= 16 ? 2 : 4) * cfg.channels * cfg.sample_rate);
    input_ring_start(infile, in_byte_rate, cfg.latency_ms + 2000);

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
