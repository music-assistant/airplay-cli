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
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#include <stdatomic.h>

#include "../libraop/crosstools/src/platform.h"
#include "../libraop/crosstools/src/cross_thread.h"
#include "../libraop/crosstools/src/cross_net.h"
#include "../libraop/crosstools/src/cross_ssl.h"
#include "../libraop/src/raop_client.h"
#include "cross_util.h"
#include "cross_log.h"
#include "ap2_client.h"
#include "ap2_session.h"
#include "ap2_io.h"
#include "ap2_ptp.h"
#include "ap2_hap.h"
#include "raop_session.h"
#include "artwork.h"

#define VERSION "0.4.0"
/* Overridden at build time from the git tag for tagged releases (see Makefile). */
#ifndef CLIAIRPLAY_VERSION
#define CLIAIRPLAY_VERSION VERSION
#endif
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
    bool no_ptp;            /* force NTP timing even when SupportsPTP is advertised */
    int splice_depth_ms;    /* >0: override the splice receiver-queue depth */
    bool ptp_shared;        /* prefer a shared PTP daemon clock (multi-room) */

    /* Audio format */
    int sample_rate;
    int bit_depth;
    int channels;
} cli_config_t;

/* Globals */
static atomic_bool g_running = true;
static _Atomic playback_status_t g_status = STATUS_STOPPED;
static pthread_t g_cmdpipe_thread;
static bool g_cmdpipe_started = false;
static int g_cmdpipe_fd = -1;
/* Command-line reassembly buffer: sized for the longest single command
 * (an ARTWORK= imageproxy URL or full metadata line stays well under this). */
static char g_cmdpipe_buf[8192];
static size_t g_cmdpipe_used = 0;

/* RAOP context (when using RAOP protocol) */
static _Atomic(struct raopcl_s *) g_raopcl = NULL;

/* AP2 context (when using AirPlay 2 protocol) */
static _Atomic(struct ap2cl_s *) g_ap2cl = NULL;

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

/* One line, one write — see ap2_io_status_line() for the atomicity the emitters
 * here rely on. The implementation lives in ap2_io.c so the AP2 client's own
 * status lines go out through the same single write. */
static void status_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ap2_io_status_vline(fmt, args);
    va_end(args);
}

/* Status messages parsed by Music Assistant's _stderr_reader() */
static void status_connected(void)
{
    status_print("[STATUS] connected");
}

/* Persistent-session engine: one stdin-backed PCM stream, flushed and refilled
 * in place across seeks; playback (and each warm re-anchor) begins on START. */
#define SESSION_IDLE_TIMEOUT_MS   120000  /* orphan safety net */
static struct ap2_session_s *g_session = NULL;
static pthread_mutex_t g_audio_send_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_first_start_done = false;
static uint64_t g_pend_start_unix_ms = 0;
static bool g_pend_start_join = false;

static void session_quiesce(void *transport)
{
    (void)transport;
    pthread_mutex_lock(&g_audio_send_lock);
}

static void session_resume(void *transport)
{
    (void)transport;
    pthread_mutex_unlock(&g_audio_send_lock);
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

/* How far the projected readiness instant must move to count as material. The
 * projection is arithmetic on two live readings, so it jitters even while the
 * receiver behaves: the shared-daemon snapshot freezes between poller rounds
 * while now advances, and the in-process engine mixes two clocks each truncated
 * to milliseconds. Comparing exactly reports that jitter as news every sample;
 * nothing a caller plans against moves by this little. */
#define CLOCK_READY_JITTER_MS 50

/* Receiver-clock readiness, pushed from connect so the caller never has to
 * guess a join headroom: a receiver begins probing our clock about a second
 * after it connects, on its own schedule and with no anchor announced, and is
 * seated roughly AP2_CLOCK_LOCK_MS after that — a property of the device, not
 * of any fixed lead. The first line goes out unconditionally right after
 * [STATUS] connected, whatever the state, so a caller can always tell the
 * difference between "not ready yet" and "this binary will never tell me".
 * After that only a MATERIAL change is reported — the state, or the projected
 * instant the caller would plan against moving further than
 * CLOCK_READY_JITTER_MS from the one last reported; the exchange count rides
 * along as telemetry but never triggers a line on its own — rate-limited to one
 * sample per AP2_CLOCK_VERIFY_POLL_MS, ending at the first state=ready. FLUSH
 * and START re-arm it for the next cycle, comparison baseline included.
 * state=stalled does NOT end it: a receiver that begins probing late still
 * reports probing and then ready. */
static bool g_clock_ready_reported = false;   /* emitted at least once this cycle */
static bool g_clock_ready_done = false;       /* terminal state=ready reported */
static uint64_t g_clock_ready_next_ntp = 0;
static ap2_clock_state_t g_clock_ready_state = AP2_CLOCK_COLD;
static uint64_t g_clock_ready_at_ms = 0;

static void clock_ready_rearm(void)
{
    g_clock_ready_reported = false;
    g_clock_ready_done = false;
    g_clock_ready_next_ntp = 0;
    /* The comparison baseline goes with them. A cycle judged against the
     * previous one's reading gets its own first transition wrong: a session
     * already stalled when it is re-armed reads stalled->stalled, so it repeats
     * the state without the diagnosis that explains what a stall means. */
    g_clock_ready_state = AP2_CLOCK_COLD;
    g_clock_ready_at_ms = 0;
    /* Reporting went terminal at the last ready and stopped asking, so under
     * the in-process engine — which has no poller to follow the receiver
     * meanwhile — the stall mark is as old as that quiet stretch. Restart the
     * window here so the first reading after the re-arm measures the receiver,
     * not the silence of nobody having looked. */
    ap2cl_clock_watch_restart(g_ap2cl);
}

static const char *clock_state_name(ap2_clock_state_t state)
{
    switch (state) {
    case AP2_CLOCK_PROBING: return "probing";
    case AP2_CLOCK_READY:   return "ready";
    case AP2_CLOCK_STALLED: return "stalled";
    default:                return "cold";
    }
}

static void clock_ready_tick(void)
{
    if (g_clock_ready_done) return;
    uint64_t now = raopcl_get_ntp(NULL);
    if (g_clock_ready_reported && now < g_clock_ready_next_ntp) return;
    g_clock_ready_next_ntp = now + MS2NTP(AP2_CLOCK_VERIFY_POLL_MS);

    ap2_clock_readiness_t r;
    ap2cl_clock_readiness(g_ap2cl, &r);
    /* Material change only: the state, or a projected instant that moved
     * further than the reading's own jitter. Measured against what was last
     * reported rather than against the last sample, so a slow drift still
     * surfaces once it has grown past the tolerance. The terminal state=ready
     * is out of the tolerance's reach either way — it is a state change, and
     * once reported the latch below ends the reporting above — and the line is
     * always formatted from this reading, never from the latched values. */
    uint64_t moved = r.ready_at_unix_ms > g_clock_ready_at_ms
        ? r.ready_at_unix_ms - g_clock_ready_at_ms
        : g_clock_ready_at_ms - r.ready_at_unix_ms;
    if (g_clock_ready_reported && r.state == g_clock_ready_state &&
        moved <= CLOCK_READY_JITTER_MS)
        return;
    /* Read before the latch moves, so the diagnosis below is logged on the
     * transition into the stall and not on every sample that stays in it. */
    bool stalling = r.state == AP2_CLOCK_STALLED &&
                    g_clock_ready_state != AP2_CLOCK_STALLED;
    g_clock_ready_reported = true;
    g_clock_ready_state = r.state;
    g_clock_ready_at_ms = r.ready_at_unix_ms;
    g_clock_ready_done = r.state == AP2_CLOCK_READY;
    status_print("[STATUS] clock_ready mode=%s state=%s streak_ms=%" PRIu64
                 " exchanges=%u ready_in_ms=%" PRIu64
                 " ready_at_unix_ms=%" PRIu64,
                 ap2cl_uses_ptp(g_ap2cl) ? "ptp" : "ntp",
                 clock_state_name(r.state),
                 r.streak_ms, r.exchanges, r.ready_in_ms, r.ready_at_unix_ms);
    if (stalling)
        LOG_WARN("[AP2] the receiver has not answered our PTP clock: it is "
                 "not slaved to us, can seat no render position and will play "
                 "silence however cleanly the audio paces — check that the "
                 "speaker can reach UDP 319/320 on this host");
}

/* The join's [STATUS] started, withheld at commit so the receiver-clock
 * verification can answer it with the instant the receiver can actually seat
 * (see ap2cl_start_ack_deferred). Exactly one ack leaves per START: the commit
 * either prints it outright or latches it here, and every way the verification
 * can end — a result, a disarm, a superseding commit, the loop ending — funnels
 * through the single emission in start_ack_flush(). All of it runs under
 * g_audio_send_lock, which the commit holds through the session engine's
 * quiesce bracket. */
static bool g_start_ack_pending = false;
static uint64_t g_start_ack_requested_ms = 0;
static uint64_t g_start_ack_at_ms = 0;
/* Set by session_commit, read by the START handler that drove it — both on the
 * command thread, so it never races the verification emitting the same ack. */
static bool g_start_ack_withheld = false;

static void status_started(uint64_t requested_ms, uint64_t at_ms)
{
    status_print("[STATUS] started requested_unix_ms=%" PRIu64
                 " at_unix_ms=%" PRIu64, requested_ms, at_ms);
}

/* Emit a withheld ack with the instant that stands. A no-op when none is held,
 * so every path out of the verification can call it unconditionally. */
static void start_ack_flush(void)
{
    if (!g_start_ack_pending) return;
    g_start_ack_pending = false;
    status_started(g_start_ack_requested_ms, g_start_ack_at_ms);
}

/* Corrected-join content cut, tracked from the correction until the debt is
 * settled. `anchor_corrected` reports the cut the correction ASKED for —
 * arithmetic on two instants, computed before a single byte is examined — so
 * the amount actually taken off the queued content is reported separately once
 * the last byte is discarded. The two disagree only when the cut ends short
 * (the input ran out, or a new commit superseded the corrected timeline). */
static bool g_content_cut_active = false;
static uint64_t g_content_cut_requested_ms = 0;
static uint32_t g_content_cut_dropped = 0;
static uint64_t g_content_cut_started_ntp = 0;

static void content_cut_report(unsigned input_byte_rate)
{
    if (!g_content_cut_active) return;
    g_content_cut_active = false;
    uint64_t cut_ms = input_byte_rate
        ? (uint64_t)g_content_cut_dropped * 1000ULL / input_byte_rate : 0;
    status_print("[STATUS] content_cut requested_ms=%" PRIu64 " cut_ms=%" PRIu64
                 " cut_bytes=%u drain_ms=%" PRIu64,
                 g_content_cut_requested_ms, cut_ms, g_content_cut_dropped,
                 (uint64_t)NTP2MS(raopcl_get_ntp(NULL) -
                                  g_content_cut_started_ntp));
}

/* Advance the receiver-clock verification of a cold-clock START (armed by
 * the client when it commits before the receiver's first timing probe) and
 * surface the outcome. A join whose ack was withheld is acked here with the
 * instant the verification settled on, and takes no content cut: the caller
 * maps its content onto the acked instant itself. Where the ack already went
 * out, a correction instead reports the new instant and the cut that keeps the
 * member on the group timeline; a verification only confirms the ack sent.
 * Called from the audio loop under g_audio_send_lock. */
static void clock_verify_tick(void)
{
    ap2_clock_verify_event_t ev;
    ap2_clock_verify_result_t result = ap2cl_clock_verify_poll(g_ap2cl, &ev);
    if (result == AP2_CLOCK_VERIFY_IDLE) return;
    switch (result) {
    case AP2_CLOCK_VERIFY_CORRECTED:
        if (!ev.start_ack) {
            status_print("[STATUS] anchor_corrected requested_unix_ms=%" PRIu64
                         " from_unix_ms=%" PRIu64 " at_unix_ms=%" PRIu64
                         " content_cut_ms=%" PRIu64,
                         ev.requested_unix_ms, ev.from_unix_ms, ev.at_unix_ms,
                         ev.content_cut_ms);
            g_content_cut_active = true;
            g_content_cut_requested_ms = ev.content_cut_ms;
            g_content_cut_dropped = 0;
            g_content_cut_started_ntp = raopcl_get_ntp(NULL);
        }
        break;
    case AP2_CLOCK_VERIFY_VERIFIED:
        status_print("[STATUS] clock_verified margin_ms=%" PRId64,
                     ev.margin_ms);
        break;
    default:
        break;
    }
    if (ev.start_ack) {
        g_start_ack_at_ms = ev.at_unix_ms;
        start_ack_flush();
    }
}

/* Machine-readable failure codes Music Assistant matches on. */
#define ERROR_CODE_AUTH_REQUIRED  "auth_required"
#define ERROR_CODE_AUTH_FAILED    "auth_failed"
#define ERROR_CODE_CONNECT_FAILED "connect_failed"
#define ERROR_CODE_START_FAILED   "start_failed"
#define ERROR_CODE_FLUSH_FAILED   "flush_failed"
#define ERROR_CODE_STANDBY_FAILED "standby_failed"
#define ERROR_CODE_PLAY_FAILED    "play_failed"
#define ERROR_CODE_PAUSE_FAILED   "pause_failed"
#define ERROR_CODE_STOP_FAILED    "stop_failed"

/*
 * Report a failure as one machine-readable line followed by the
 * human-readable one:
 *   [STATUS] error code=<slug> http=<int> detail="<text>"
 *   [ERROR] <msg>
 * The detail is squeezed onto a single line and stripped of the double quotes
 * that delimit it, so the contract holds for any device-supplied text.
 */
static void status_error_ex(const char *code, int http, const char *detail,
                            const char *msg)
{
    char clean[256];
    size_t out = 0;
    for (const char *c = detail ? detail : ""; *c && out + 1 < sizeof(clean); c++) {
        char ch = *c;
        if (ch == '"') ch = '\'';
        else if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        clean[out++] = ch;
    }
    clean[out] = '\0';
    status_print("[STATUS] error code=%s http=%d detail=\"%s\"",
                 code, http, clean);
    status_error(msg);
}

/* Whether a transport command still has a session that could act on it. The
 * pointer outlives the session it points at: DISCONNECT and the idle timeout
 * mark it ENDED and only the teardown frees it, and every command entry point
 * refuses an ended session. So the pointer alone would report a command the
 * session refused where in truth there was no longer a session to refuse it.
 * ap2_session_state() answers ENDED for NULL too. */
static bool session_is_live(void)
{
    return ap2_session_state(g_session) != AP2_SESSION_ENDED;
}

static void remote_command_event(
    ap2_remote_command_t command, void *userdata)
{
    (void)userdata;
    const char *name = ap2_remote_command_name(command);
    if (!name) return;
    printf("[EVENT] remote command=%s\n", name);
    fflush(stdout);
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

static void mrp_artwork_status_report(const ap2_mrp_artwork_info_t *info,
                                      int command_status)
{
    if (!info || info->result == AP2_MRP_ARTWORK_NOT_APPLICABLE ||
        info->result == AP2_MRP_ARTWORK_UNCHANGED)
        return;
    if (info->result == AP2_MRP_ARTWORK_ACCEPTED) {
        status_print("[STATUS] mrp artwork=posted status=%d bytes=%zu "
                     "width=%u height=%u precision=%u sof=0x%02x "
                     "components=%u progressive=%d staging_max_bytes=%d",
                     command_status, info->bytes, info->width, info->height,
                     info->precision, info->sof_marker, info->components,
                     info->progressive,
                     AP2_MRP_ARTWORK_STAGING_MAX_BYTES);
        return;
    }
    status_print("[STATUS] mrp artwork=rejected reason=%s bytes=%zu "
                 "width=%u height=%u precision=%u sof=0x%02x components=%u "
                 "progressive=%d clear_status=%d staging_max_bytes=%d",
                 ap2_mrp_artwork_result_name(info->result), info->bytes,
                 info->width, info->height, info->precision, info->sof_marker,
                 info->components, info->progressive, command_status,
                 AP2_MRP_ARTWORK_STAGING_MAX_BYTES);
}

static void mrp_artwork_reject_local(const char *reason)
{
    ap2_mrp_push_result_t push;
    if (!g_ap2cl || !ap2cl_clear_mrp_artwork(g_ap2cl, &push)) return;
    status_print("[STATUS] mrp artwork=rejected reason=%s clear_status=%d",
                 reason, push.nowplaying_status);
    mrp_status_report(push.overall_status);
}

/* Also emit the older human-readable status line; the [STATUS] lines from
 * status_connected/status_playing are what the MA provider parses. */
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
 * Takes the upper 3 bytes of each 32-bit LE sample (bytes 1,2,3 = bits 8-31).
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

/* The session only returns a short read when stdin reaches EOF. Complete the
 * final transport packet with silence, dropping any incomplete PCM frame. */
static int pad_final_pcm_chunk(uint8_t *buf, int bytes, int chunk_bytes,
                               int bytes_per_frame)
{
    if (bytes >= chunk_bytes) return bytes;
    int aligned = bytes - bytes % bytes_per_frame;
    if (aligned != bytes)
        LOG_WARN("Discarding %d trailing incomplete PCM bytes", bytes - aligned);
    memset(buf + aligned, 0, (size_t)(chunk_bytes - aligned));
    return chunk_bytes;
}

/* ---- Command pipe handler ---- */

static struct {
    char *title;    /* owned (strdup'd); NULL until first set */
    char *artist;
    char *album;
    char *item_id;  /* sender's stable per-track identity (ITEMID key) */
    int duration;
    int progress;
} g_metadata;

/* Store an owned copy: the value points into the cmdpipe read buffer, which
 * is overwritten by the next read. */
static void metadata_set(char **field, const char *value)
{
    char *copy = strdup(value ? value : "");
    if (!copy) return;
    free(*field);
    *field = copy;
}

static const char *metadata_str(const char *value)
{
    return value ? value : "";
}

static void handle_command(const char *key, const char *value, cli_config_t *cfg)
{
    if (strcmp(key, "TITLE") == 0) {
        metadata_set(&g_metadata.title, value);
    } else if (strcmp(key, "ARTIST") == 0) {
        metadata_set(&g_metadata.artist, value);
    } else if (strcmp(key, "ALBUM") == 0) {
        metadata_set(&g_metadata.album, value);
    } else if (strcmp(key, "ITEMID") == 0) {
        metadata_set(&g_metadata.item_id, value);
    } else if (strcmp(key, "DURATION") == 0) {
        g_metadata.duration = atoi(value);
    } else if (strcmp(key, "PROGRESS") == 0) {
        g_metadata.progress = atoi(value);
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_progress_ms(g_raopcl, g_metadata.progress * 1000, g_metadata.duration * 1000);
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_progress(g_ap2cl, g_metadata.progress, g_metadata.duration);
            mrp_status_report(ap2cl_mrp_push_progress(g_ap2cl));
        }
    } else if (strcmp(key, "ARTWORK") == 0) {
        uint8_t *image = NULL;
        size_t image_size = 0;
        char content_type[32];
        char error[160];
        bool loaded = artwork_load(value, &image, &image_size, content_type,
                                   error, sizeof(error));
        if (!loaded) {
            LOG_WARN("Cannot load artwork: %s", error);
            if (cfg->protocol == PROTO_AIRPLAY2)
                mrp_artwork_reject_local("invalid_artwork");
        } else {
            LOG_INFO("Loaded artwork (%zu bytes, %s)", image_size, content_type);
            if (cfg->protocol == PROTO_RAOP && g_raopcl) {
                raopcl_set_artwork(g_raopcl, content_type,
                                   (int)image_size, (char *)image);
            } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
                ap2_mrp_artwork_info_t mrp_info;
                ap2_mrp_push_result_t push;
                bool dmap_ok = ap2cl_set_artwork(
                    g_ap2cl, content_type, (int)image_size,
                    (const char *)image, &mrp_info, &push);
                if (!dmap_ok)
                    LOG_WARN("Receiver rejected DMAP artwork (%zu bytes, %s)",
                             image_size, content_type);
                mrp_artwork_status_report(
                    &mrp_info, push.nowplaying_status);
                mrp_status_report(push.overall_status);
            }
            free(image);
        }
    } else if (strcmp(key, "VOLUME") == 0) {
        int vol = atoi(value);
        LOG_INFO("Setting volume to: %d", vol);
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_volume(g_raopcl, raopcl_float_volume(vol));
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_volume(g_ap2cl, vol);
        }
    } else if (strcmp(key, "START_UNIX_MS") == 0) {
        g_pend_start_unix_ms = strtoull(value, NULL, 10);
    } else if (strcmp(key, "START_JOIN") == 0) {
        g_pend_start_join = atoi(value) != 0;
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "START") == 0) {
        /* The verified start contract: the ack always reports the true
         * scheduled instant, so the caller compares it with the request,
         * logs any correction loudly, and re-aligns a group by re-STARTing
         * every member at the largest reported instant. */
        uint64_t at_ms = 0;
        ap2_commit_result_t started = g_session
            ? ap2_session_start(g_session, g_pend_start_unix_ms, &at_ms)
            : AP2_COMMIT_FAILED;
        if (started == AP2_COMMIT_OK) {
            /* A cold-clock join withholds its ack for the verification to
             * answer with the instant the receiver can seat. The commit
             * latched that decision, so read it rather than the pending flag:
             * the verification can resolve — and emit — before this line. */
            if (!g_start_ack_withheld)
                status_started(g_pend_start_unix_ms, at_ms);
        } else {
            /* No instant was scheduled, so the caller must not map content
             * onto one. Coded so it can abort its pending ack wait at once
             * instead of waiting out the timeout that also means "old
             * binary" — with the ack withheld, the wait is seconds long. */
            status_error_ex(ERROR_CODE_START_FAILED, 0,
                            session_is_live() ? "session did not schedule the start"
                                              : "no live session to start",
                            "START failed");
        }
        g_pend_start_unix_ms = 0;
        g_pend_start_join = false;
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "FLUSH") == 0) {
        if (!g_session || !ap2_session_flush(g_session))
            status_error_ex(ERROR_CODE_FLUSH_FAILED, 0,
                            session_is_live() ? "session rejected the flush"
                                              : "no live session to flush",
                            "FLUSH failed");
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "STANDBY") == 0) {
        if (!g_session || !ap2_session_standby(g_session))
            status_error_ex(ERROR_CODE_STANDBY_FAILED, 0,
                            session_is_live() ? "session rejected the standby"
                                              : "no live session to stand by",
                            "STANDBY failed");
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "DISCONNECT") == 0) {
        if (g_session) ap2_session_end(g_session);
        g_status = STATUS_STOPPED;
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PAUSE") == 0) {
        if (g_status == STATUS_PLAYING) {
            pthread_mutex_lock(&g_audio_send_lock);
            if (g_status == STATUS_PLAYING) {
                if (cfg->protocol == PROTO_RAOP && g_raopcl) {
                    if (!raop_session_pause(g_raopcl))
                        status_error_ex(ERROR_CODE_PAUSE_FAILED, 0,
                                        "the RAOP transport rejected the pause",
                                        "RAOP pause failed");
                } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
                    ap2cl_pause(g_ap2cl);
                }
                g_status = STATUS_PAUSED;
                status_paused(0);
            }
            pthread_mutex_unlock(&g_audio_send_lock);
        }
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PLAY") == 0) {
        pthread_mutex_lock(&g_audio_send_lock);
        bool play_ok = true;
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            if (!raop_session_resume(g_raopcl)) {
                status_error_ex(ERROR_CODE_PLAY_FAILED, 0,
                                "the RAOP transport rejected the resume",
                                "RAOP play failed");
                play_ok = false;
            }
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_play(g_ap2cl);
        } else {
            /* No client for the resolved protocol: the command reached nothing,
             * and the status stays where it was. Coded so the caller does not
             * read the silence as a resume it can start counting from. */
            status_error_ex(ERROR_CODE_PLAY_FAILED, 0,
                            "no transport client to resume",
                            "PLAY failed");
            play_ok = false;
        }
        /* No status emission here: reporting elapsed_ms=0 would snap the
         * sender's position display backwards. The periodic reporter (gated on
         * STATUS_PLAYING) re-reports the true elapsed within a second. */
        if (play_ok) g_status = STATUS_PLAYING;
        pthread_mutex_unlock(&g_audio_send_lock);
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "STOP") == 0) {
        pthread_mutex_lock(&g_audio_send_lock);
        g_status = STATUS_STOPPED;
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_stop(g_raopcl);
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_stop(g_ap2cl);
        } else {
            /* Neither transport call reports a result, so the one failure that
             * can be seen here is having no client to make the call on. The
             * stopped line still goes out below: the caller treats STOP as
             * terminal and tears the process down either way. */
            status_error_ex(ERROR_CODE_STOP_FAILED, 0,
                            "no transport client to stop",
                            "STOP failed");
        }
        status_print("[STATUS] stopped");
        pthread_mutex_unlock(&g_audio_send_lock);
    } else if (strcmp(key, "ACTION") == 0 && strcmp(value, "SENDMETA") == 0) {
        if (cfg->protocol == PROTO_RAOP && g_raopcl) {
            raopcl_set_daap(g_raopcl, 4, "minm", 's', metadata_str(g_metadata.title),
                            "asar", 's', metadata_str(g_metadata.artist),
                            "asal", 's', metadata_str(g_metadata.album),
                            "astn", 'i', 1);
        } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
            ap2cl_set_metadata(g_ap2cl, metadata_str(g_metadata.title),
                               metadata_str(g_metadata.artist),
                               metadata_str(g_metadata.album), g_metadata.duration,
                               metadata_str(g_metadata.item_id));
            mrp_status_report(ap2cl_mrp_push(g_ap2cl));
        }
    }
}

/* Some receivers (notably Sonos) will not emit audio until they have received
 * metadata. Push the current values (or a placeholder title) at the first
 * commanded START, before any PCM is sent. */
static void send_initial_metadata(const cli_config_t *cfg)
{
    const char *title = (g_metadata.title && *g_metadata.title) ? g_metadata.title : "cliairplay";
    if (cfg->protocol == PROTO_RAOP && g_raopcl) {
        raopcl_set_daap(g_raopcl, 4, "minm", 's', title,
                        "asar", 's', metadata_str(g_metadata.artist),
                        "asal", 's', metadata_str(g_metadata.album), "astn", 'i', 1);
    } else if (cfg->protocol == PROTO_AIRPLAY2 && g_ap2cl) {
        ap2cl_set_metadata(g_ap2cl, title, metadata_str(g_metadata.artist),
                           metadata_str(g_metadata.album), g_metadata.duration,
                           metadata_str(g_metadata.item_id));
        mrp_status_report(ap2cl_mrp_push(g_ap2cl));
    }
}

/* ---- Session engine transport callbacks ---- */

static ap2_commit_result_t session_commit(void *transport,
                                          uint64_t start_unix_ms,
                                          uint64_t *at_unix_ms)
{
    cli_config_t *cfg = transport;
    ap2_commit_result_t committed;
    /* One ack per START: a previous join's withheld ack is answered with the
     * instant that stood, before this commit replaces the timeline it
     * described. */
    start_ack_flush();
    g_start_ack_withheld = false;
    /* Report readiness afresh for the next cycle: this start consumed the
     * reading the caller planned it against. */
    clock_ready_rearm();
    /* The first START begins the session; a START after a FLUSH re-anchors the
     * flushed stream (no second receiver flush, no crypto/sequence reset). An
     * infeasible instant is corrected forward by the transport, which reports
     * the true scheduled instant so the ack can carry it to the caller. */
    if (cfg->protocol == PROTO_RAOP) {
        struct raopcl_s *client = g_raopcl;
        if (!client) return AP2_COMMIT_FAILED;
        committed = !g_first_start_done
            ? raop_session_commit(client, start_unix_ms, at_unix_ms)
            : raop_session_start_at(client, start_unix_ms, at_unix_ms);
    } else {
        struct ap2cl_s *client = g_ap2cl;
        if (!client) return AP2_COMMIT_FAILED;
        ap2cl_set_start_join(client, g_pend_start_join);
        committed = !g_first_start_done
            ? ap2cl_start(client, start_unix_ms, at_unix_ms)
            : ap2cl_resume(client, start_unix_ms, at_unix_ms);
    }
    if (committed != AP2_COMMIT_OK) return committed;

    /* Latch a withheld ack here, inside the session engine's quiesce bracket:
     * the audio loop is held off, so the verification cannot resolve between
     * the commit arming it and the ack being owed. */
    if (cfg->protocol != PROTO_RAOP && ap2cl_start_ack_deferred(g_ap2cl)) {
        g_start_ack_withheld = true;
        g_start_ack_pending = true;
        g_start_ack_requested_ms = start_unix_ms;
        g_start_ack_at_ms = at_unix_ms ? *at_unix_ms : start_unix_ms;
    }

    /* Metadata-gated receivers must receive a placeholder before audio. */
    if (!g_first_start_done) send_initial_metadata(cfg);
    g_first_start_done = true;
    g_status = STATUS_PLAYING;
    return AP2_COMMIT_OK;
}

/* Discard the receiver's buffered audio in place for a warm seek and mark the
 * stream not-playing; the next START re-anchors and resumes sending. */
static bool session_flush_op(void *transport)
{
    cli_config_t *cfg = transport;
    bool flushed = false;
    if (cfg->protocol == PROTO_RAOP) {
        flushed = g_raopcl && raop_session_flush(g_raopcl);
    } else if (g_ap2cl) {
        flushed = ap2cl_flush(g_ap2cl);
    }
    /* A failed RTSP round-trip can leave the receiver partially flushed.
     * Keep sends paused while the caller falls back to a cold restart. */
    if (g_status == STATUS_PLAYING) g_status = STATUS_PAUSED;
    /* The next track's start plans against a fresh readiness reading. */
    clock_ready_rearm();
    return flushed;
}

static void session_stop_op(void *transport)
{
    cli_config_t *cfg = transport;
    if (cfg->protocol == PROTO_RAOP) {
        if (g_raopcl && !raop_session_standby(g_raopcl))
            status_error_ex(ERROR_CODE_STANDBY_FAILED, 0,
                            "the RAOP transport rejected the standby",
                            "RAOP standby failed");
    } else if (g_ap2cl) {
        ap2cl_standby(g_ap2cl);
    }
    if (g_status == STATUS_PLAYING) g_status = STATUS_PAUSED;
}

/* Audible instant of the delivery head frozen by a FLUSH (splice timeline
 * only): rides the flushed ack so MA anchors the warm START beyond it. */
static uint64_t session_warm_head_unix_ms(void *transport)
{
    cli_config_t *cfg = transport;
    if (cfg->protocol == PROTO_RAOP) return 0;
    return g_ap2cl ? ap2cl_splice_head_unix_ms(g_ap2cl) : 0;
}

static void session_status_line(const char *line)
{
    status_print("%s", line);
}

static void *cmdpipe_reader_thread(void *arg)
{
    cli_config_t *cfg = (cli_config_t *)arg;
    uint64_t last_keepalive = raopcl_get_ntp(NULL);

    LOG_INFO("Command pipe ready: %s", cfg->cmdpipe);

    while (g_running) {
        struct pollfd pfds = {.fd = g_cmdpipe_fd, .events = POLLIN};
        /* The timeout bounds teardown, not command latency: an arriving command
         * wakes the poll at once, but STOP, DISCONNECT and the idle timeout all
         * tear down through join_command_thread(), which cannot return before
         * this wait expires. By then the client is AP2_DOWN, so the audio loop's
         * silence keepalive has already stopped and nothing feeds the wire until
         * TEARDOWN goes out — a receiver whose queue underruns while its session
         * is still armed pops audibly. 100 ms keeps that gap far inside the
         * 600 ms queue depth. The wait's only other job is pacing libraop's 20 s
         * RAOP keepalive below, which ten wakeups a second serve as well as one. */
        int n = poll(&pfds, 1, 100);
        if (!g_running) break;

        uint64_t now = raopcl_get_ntp(NULL);
        if (cfg->protocol == PROTO_RAOP && g_raopcl && now - last_keepalive >= MS2NTP(20000)) {
            raopcl_keepalive(g_raopcl);
            last_keepalive = now;
        }
        if (n <= 0 || !(pfds.revents & POLLIN)) continue;

        /* Append to the reassembly buffer and execute only complete
         * (newline-terminated) lines: one command can arrive split across
         * reads, and executing a partial line corrupts it. The unterminated
         * tail stays buffered for the next read. */
        if (g_cmdpipe_used == sizeof(g_cmdpipe_buf) - 1) {
            LOG_ERROR("Command pipe line exceeds %zu bytes; discarding",
                      sizeof(g_cmdpipe_buf) - 1);
            g_cmdpipe_used = 0;
        }
        ssize_t bytes_read = read(g_cmdpipe_fd, g_cmdpipe_buf + g_cmdpipe_used,
                                  sizeof(g_cmdpipe_buf) - 1 - g_cmdpipe_used);
        if (bytes_read > 0) {
            g_cmdpipe_used += (size_t)bytes_read;
            g_cmdpipe_buf[g_cmdpipe_used] = '\0';
            char *line = g_cmdpipe_buf;
            char *end;
            while ((end = memchr(line, '\n',
                                 g_cmdpipe_buf + g_cmdpipe_used - line)) != NULL) {
                if (!g_running) break;
                *end = '\0';
                char *separator = strchr(line, '=');
                if (separator && separator != line) {
                    *separator = '\0';
                    handle_command(line, separator + 1, cfg);
                }
                line = end + 1;
            }
            size_t remaining = g_cmdpipe_used - (size_t)(line - g_cmdpipe_buf);
            memmove(g_cmdpipe_buf, line, remaining);
            g_cmdpipe_used = remaining;
            g_cmdpipe_buf[g_cmdpipe_used] = '\0';
        } else if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_ERROR("Error reading from command pipe: %s", strerror(errno));
            usleep(250 * 1000);
        }
    }
    return NULL;
}

static void request_command_stop(void)
{
    atomic_store(&g_running, false);
}

static bool join_command_thread(void)
{
    if (!g_cmdpipe_started) return true;
    if (pthread_equal(pthread_self(), g_cmdpipe_thread)) {
        LOG_ERROR("Refusing to join command pipe thread from itself");
        return false;
    }
    int result = pthread_join(g_cmdpipe_thread, NULL);
    if (result != 0) {
        LOG_ERROR("Failed to join command pipe thread: %s", strerror(result));
        return false;
    }
    g_cmdpipe_started = false;
    return true;
}

static bool start_command_thread(cli_config_t *cfg)
{
    if (!cfg->cmdpipe || g_cmdpipe_started) return true;
    g_cmdpipe_fd = open(cfg->cmdpipe, O_RDWR | O_NONBLOCK);
    if (g_cmdpipe_fd == -1) {
        LOG_ERROR("Failed to open command pipe: %s (errno=%d)",
                  cfg->cmdpipe, errno);
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot open the command pipe",
                        "Failed to open command pipe");
        return false;
    }
    int result = pthread_create(
        &g_cmdpipe_thread, NULL, cmdpipe_reader_thread, cfg);
    if (result != 0) {
        LOG_ERROR("Failed to start command pipe thread: %s", strerror(result));
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot start the command pipe thread",
                        "Failed to start command pipe thread");
        close(g_cmdpipe_fd);
        g_cmdpipe_fd = -1;
        return false;
    }
    g_cmdpipe_started = true;
    return true;
}

static bool start_stream_session(cli_config_t *cfg, int input_bpf,
                                 int frames_per_chunk)
{
    ap2_session_ops_t ops = {
        .quiesce = session_quiesce,
        .flush = session_flush_op,
        .commit = session_commit,
        .resume = session_resume,
        .stop = session_stop_op,
        .status = session_status_line,
        .warm_head_unix_ms = session_warm_head_unix_ms,
        .transport = cfg,
    };
    g_session = ap2_session_create(
        &ops, (unsigned)cfg->sample_rate * (unsigned)input_bpf,
        (unsigned)frames_per_chunk * (unsigned)input_bpf,
        SESSION_IDLE_TIMEOUT_MS, STDIN_FILENO);
    if (!g_session) {
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot create the session engine",
                        "Cannot create session engine");
        return false;
    }
    g_status = STATUS_PAUSED;
    if (!start_command_thread(cfg)) {
        ap2_session_destroy(g_session);
        g_session = NULL;
        return false;
    }
    return true;
}

/* ---- RAOP playback loop ---- */

static int run_raop(cli_config_t *cfg)
{
    struct in_addr host_addr, player_addr;
    struct hostent *hostent;
    uint32_t netmask;
    char *iface = NULL;
    raop_crypto_t crypto = RAOP_CLEAR;

    /* Resolve local interface */
    host_addr = get_interface(cfg->iface, &iface, &netmask);
    LOG_INFO("Binding to %s [%s] with mask 0x%08x", inet_ntoa(host_addr), iface ? iface : "?", ntohl(netmask));
    NFREE(iface);

    /* Resolve player */
    hostent = gethostbyname(cfg->host);
    if (!hostent) {
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot resolve the device hostname",
                        "Cannot resolve hostname");
        return 1;
    }
    memcpy(&player_addr.s_addr, hostent->h_addr_list[0], hostent->h_length);

    /* Check AppleTV auth requirement */
    if (cfg->am && strcasestr(cfg->am, "appletv") && cfg->pk && *cfg->pk && (!cfg->secret || !*cfg->secret)) {
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "AppleTV pairing secret (--secret) is missing",
                        "AppleTV requires authentication (need secret)");
        return 1;
    }

    /* Encryption setup */
    if ((cfg->encrypt) && cfg->et && strchr(cfg->et, '1'))
        crypto = RAOP_RSA;

    /* Arm RTSP digest authentication whenever a password was supplied: the
     * mDNS pw flag only says the device advertises one, and libraop uses the
     * password solely when the device actually challenges. A required password
     * that was not supplied is rejected before any connect attempt (main). */
    char *password = (cfg->password && *cfg->password) ? cfg->password : NULL;

    int latency = MS2TS(cfg->latency_ms, cfg->sample_rate);

    /* Codec selection: default to compressed ALAC, which virtually every RAOP
     * receiver advertises and which saves LAN bandwidth. Fall back to uncompressed
     * only when the device's mDNS cn field is present and does not list ALAC (1),
     * or when --raw is forced. */
    bool use_alac = true;
    if (cfg->cn && *cfg->cn && !strchr(cfg->cn, '1')) use_alac = false;
    if (cfg->raw) use_alac = false;
    LOG_INFO("RAOP codec: %s", use_alac ? "ALAC (compressed)" : "ALAC-raw (uncompressed)");

    /* Create RAOP client */
    struct raopcl_s *client = raopcl_create(
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
    if (!client) {
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot initialise the RAOP client",
                        "Cannot init RAOP client");
        return 1;
    }

    /* Connect */
    LOG_INFO("Connecting to %s:%d via RAOP", inet_ntoa(player_addr), cfg->port);
    if (!raopcl_connect(client, player_addr, cfg->port, cfg->volume > 0)) {
        /* libraop does not expose the RTSP status of the failure, so a rejected
         * password is not distinguishable from an unreachable device here. */
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0, "RAOP connect failed",
                        "Cannot connect to AirPlay device");
        request_command_stop();
        atomic_store(&g_raopcl, NULL);
        if (!join_command_thread()) return 1;
        raopcl_destroy(client);
        return 1;
    }
    atomic_store(&g_raopcl, client);

    latency = raopcl_latency(g_raopcl);
    int input_bpf = (cfg->bit_depth <= 16 ? 2 : 4) * cfg->channels;
    int alac_bpf = (cfg->bit_depth <= 16 ? 2 : 3) * cfg->channels;
    if (!start_stream_session(
            cfg, input_bpf, DEFAULT_FRAMES_PER_CHUNK))
        return 1;
    status_connected_legacy(inet_ntoa(player_addr), cfg->port,
                            (int)TS2MS(latency, raopcl_sample_rate(g_raopcl)));
    /* Legacy RAOP has no PTP clock to measure, but the line still goes out so
     * a caller that gates a join on it is answered rather than left waiting. */
    pthread_mutex_lock(&g_audio_send_lock);
    clock_ready_tick();
    pthread_mutex_unlock(&g_audio_send_lock);

    /* Commanded PCM format:
     * For 16-bit: s16le (2 bytes per sample, 4 bytes per frame stereo)
     * For 24-bit: s32le (4 bytes per sample, 8 bytes per frame stereo)
     *   ALAC encoder expects s24le (3 bytes/sample), so we truncate s32le→s24le.
     */
    uint8_t *buf = malloc(DEFAULT_FRAMES_PER_CHUNK * input_bpf);
    uint8_t *alac_buf = (cfg->bit_depth > 16) ? malloc(DEFAULT_FRAMES_PER_CHUNK * alac_bpf) : NULL;
    if (!buf || (cfg->bit_depth > 16 && !alac_buf)) {
        status_error("Cannot allocate RAOP audio buffer");
        free(buf);
        free(alac_buf);
        request_command_stop();
        join_command_thread();
        ap2_session_destroy(g_session);
        g_session = NULL;
        return 1;
    }

    uint64_t last = 0, frames = 0;
    uint64_t current_epoch = ap2_session_epoch(g_session);
    bool playback_failed = false;
    bool input_ended = false;
    bool eof_reported = false;
    uint64_t eof_time = 0;

    /* Main audio loop */
    while (g_status != STATUS_STOPPED) {
        uint64_t now = raopcl_get_ntp(NULL);
        ap2_session_poll(g_session);
        if (ap2_session_state(g_session) == AP2_SESSION_ENDED)
            break;

        if (!raopcl_is_connected(g_raopcl) || !raopcl_is_sane(g_raopcl)) {
            status_error("RAOP control or media channel failed");
            playback_failed = true;
            break;
        }

        /* Each START bumps the epoch; elapsed restarts at the new anchor. */
        uint64_t epoch = ap2_session_epoch(g_session);
        if (epoch != current_epoch) {
            current_epoch = epoch;
            frames = 0;
            input_ended = false;
            eof_reported = false;
            eof_time = 0;
        }

        /* Periodic status reporting (only while playing, so a pause does not keep
           emitting a "playing" status that would revive the sender's play state) */
        if (g_status == STATUS_PLAYING && !input_ended &&
            now - last > MS2NTP(1000)) {
            last = now;
            if (frames > (uint64_t)raopcl_latency(g_raopcl)) {
                uint32_t elapsed = TS2MS(frames - raopcl_latency(g_raopcl), raopcl_sample_rate(g_raopcl));
                status_elapsed_legacy(elapsed, frames, g_raopcl);
            }
        }

        if (input_ended) {
            bool drained = !raopcl_is_playing(g_raopcl) ||
                           now - eof_time > MS2NTP(
                               TS2MS(raopcl_latency(g_raopcl),
                                     raopcl_sample_rate(g_raopcl)) + 2000);
            if (drained && !eof_reported) {
                eof_reported = true;
                status_eof();
            }
            usleep(drained ? 50000 : 10000);
            continue;
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING) {
            pthread_mutex_lock(&g_audio_send_lock);
            if (g_status != STATUS_PLAYING ||
                !raopcl_accept_frames(g_raopcl)) {
                pthread_mutex_unlock(&g_audio_send_lock);
                usleep(1000);
                continue;
            }
            epoch = ap2_session_epoch(g_session);
            if (epoch != current_epoch) {
                current_epoch = epoch;
                frames = 0;
                input_ended = false;
                eof_reported = false;
                eof_time = 0;
            }
            int n = ap2_session_read(
                g_session, buf, DEFAULT_FRAMES_PER_CHUNK * input_bpf, 250);
            if (n == -2) {
                pthread_mutex_unlock(&g_audio_send_lock);
                break;
            }
            if (n == -1) {
                LOG_INFO("End of RAOP input stream, draining...");
                input_ended = true;
                eof_time = raopcl_get_ntp(NULL);
                pthread_mutex_unlock(&g_audio_send_lock);
                continue;
            }
            if (n == 0) {
                pthread_mutex_unlock(&g_audio_send_lock);
                continue;
            }
            n = pad_final_pcm_chunk(
                buf, n, DEFAULT_FRAMES_PER_CHUNK * input_bpf, input_bpf);

            int audio_frames = n / input_bpf;
            uint8_t *send_buf = buf;
            /* For 24-bit: truncate s32le input to s24le (packed 3 bytes) for ALAC encoder */
            if (alac_buf && cfg->bit_depth > 16) {
                truncate_32to24(buf, n, alac_buf);
                send_buf = alac_buf;
            }
            uint64_t playtime;
            if (!raopcl_send_chunk(
                    g_raopcl, send_buf, audio_frames, &playtime)) {
                status_error("RAOP audio send failed");
                playback_failed = true;
                pthread_mutex_unlock(&g_audio_send_lock);
                break;
            }
            frames += audio_frames;
            pthread_mutex_unlock(&g_audio_send_lock);
        } else {
            usleep(1000);
        }
    }

    request_command_stop();
    bool command_joined = join_command_thread();
    ap2_session_destroy(g_session);
    g_session = NULL;
    free(buf);
    free(alac_buf);
    return playback_failed || !command_joined ? 1 : 0;
}

/* ---- AirPlay 2 playback loop ---- */

/* One chunk of encoded silence on the live splice line — the keepalive for
 * every armed window without content (idle-primed FLUSH->START gap, content
 * pause, post-EOF drain/idle). The wire stays bitstream-continuous and the
 * receiver's queue never underruns. Returns false on a fatal send failure. */
static bool ap2_send_silence_chunk(const cli_config_t *cfg, uint8_t *buf,
                                   uint8_t *alac_buf, int input_bpf)
{
    memset(buf, 0, (size_t)AP2_FRAMES_PER_CHUNK * input_bpf);
    uint8_t *send = buf;
    if (alac_buf && cfg->bit_depth > 16) {
        truncate_32to24(buf, AP2_FRAMES_PER_CHUNK * input_bpf, alac_buf);
        send = alac_buf;
    }
    return ap2cl_send_chunk(g_ap2cl, send, AP2_FRAMES_PER_CHUNK) !=
           AP2_SEND_FATAL;
}

static int run_airplay2(cli_config_t *cfg)
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

    struct ap2cl_s *client = ap2cl_create(
        &device, &format, cfg->auth, cfg->password,
        cfg->dacp_id, cfg->active_remote, cfg->latency_ms, cfg->volume);
    if (!client) {
        status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                        "cannot create the AirPlay 2 client",
                        "Cannot create AirPlay 2 client");
        return 1;
    }

    /* Pass through mDNS properties and interface for RAOP-compatible flow */
    ap2cl_set_raop_props(client, cfg->iface, cfg->secret,
                          cfg->et, cfg->md, cfg->am);
    /* Apply the resolved AirPlay 2 route (see ap2_resolve_route). force_native
     * is a no-op when stored credentials already select the native flow. */
    if (cfg->route.native)
        ap2cl_force_native(client);
    if (cfg->publish_ip)
        ap2cl_set_publish_ip(client, cfg->publish_ip);
    ap2cl_set_ptp(client, cfg->route.ptp);
    if (cfg->splice_depth_ms > 0)
        ap2cl_set_splice_depth_ms(client, cfg->splice_depth_ms);
    ap2cl_set_ptp_shared(client, cfg->ptp_shared);
    ap2cl_set_remote_command_callback(
        client, remote_command_event, NULL);

    /* Connect: auth-setup + RAOP ANNOUNCE/SETUP/RECORD */
    LOG_INFO("Connecting to %s:%d via AirPlay 2", cfg->host, cfg->port);
    if (!ap2cl_connect(client)) {
        int http = 0;
        const char *detail = NULL;
        ap2_connect_error_t kind = ap2cl_connect_error(client, &http, &detail);
        const char *code = ERROR_CODE_CONNECT_FAILED;
        if (kind == AP2_CONNECT_ERROR_AUTH_REQUIRED) code = ERROR_CODE_AUTH_REQUIRED;
        else if (kind == AP2_CONNECT_ERROR_AUTH_FAILED) code = ERROR_CODE_AUTH_FAILED;
        status_error_ex(code, http, detail,
                        "Cannot connect to AirPlay 2 device");
        request_command_stop();
        atomic_store(&g_ap2cl, NULL);
        if (!join_command_thread()) return 1;
        ap2cl_destroy(client);
        return 1;
    }
    atomic_store(&g_ap2cl, client);

    /* Report the MRP now-playing path so MA can log which one is active. The
     * type-130 data channel (path B) is opt-in; -1 means it was not attempted
     * (default) or the session is not an Apple/pair-verified target. */
    {
        int mrp_ch = ap2cl_mrp_channel_status(g_ap2cl);
        if (mrp_ch >= 0) {
            printf("[STATUS] mrp path=channel status=%d\n", mrp_ch);
            fflush(stdout);
        }
    }

    /* Register the sender identity before the first now-playing update. The
     * supported commands, playback state and NowPlayingClient follow that first
     * update inside ap2cl_mrp_push(), matching Apple's AirPlaySender ordering. */
    ap2cl_mrp_register(g_ap2cl);

    /* Surface the effective lead and the receiver-reported buffering window so
     * the caller (MA) can plan group starts from real device capabilities. */
    {
        int lead_ms = 0;
        uint32_t dev_min = 0, dev_max = 0;
        ap2cl_latency_info(g_ap2cl, &lead_ms, &dev_min, &dev_max);
        /* warm_lead_ms: minimum lead a warm commanded START needs for exact
         * placement on the splice timeline (0 = no constraint). MA anchors
         * group seeks beyond the largest member value. */
        printf("[STATUS] latency lead_ms=%d device_min_frames=%u device_max_frames=%u "
               "device_render_ms=%d warm_lead_ms=%d\n",
               lead_ms, dev_min, dev_max, ap2cl_render_latency_ms(g_ap2cl),
               ap2cl_warm_lead_ms(g_ap2cl));
        fflush(stdout);
    }

    /* Surface the /info-advertised format tables so the caller can pick the
     * best format per device (e.g. auto-selecting 24-bit) instead of relying
     * on a static configuration. Masks use the audioFormat bit space; known=0
     * means the device did not publish that table. */
    {
        ap2_format_capabilities_t caps;
        ap2cl_format_capabilities(g_ap2cl, &caps);
        printf("[STATUS] capabilities requested=0x%llx realtime_formats=0x%llx "
               "realtime_known=%d buffered_formats=0x%llx buffered_known=%d\n",
               (unsigned long long)caps.requested,
               (unsigned long long)caps.realtime_formats, caps.realtime_known ? 1 : 0,
               (unsigned long long)caps.buffered_formats, caps.buffered_known ? 1 : 0);
        fflush(stdout);
    }

    /* Set volume */
    if (cfg->volume > 0) {
        ap2cl_set_volume(g_ap2cl, cfg->volume);
    }

    /* The reader begins draining stdin into the ring immediately; the command
     * pipe thread anchors playback on the first START after connection setup. */
    int ap2_input_bpf = (cfg->bit_depth <= 16 ? 2 : 4) * cfg->channels;
    int ap2_alac_bpf = (cfg->bit_depth <= 16 ? 2 : 3) * cfg->channels;
    unsigned ap2_input_byte_rate =
        (unsigned)cfg->sample_rate * (unsigned)ap2_input_bpf;
    if (!start_stream_session(
            cfg, ap2_input_bpf, AP2_FRAMES_PER_CHUNK))
        return 1;
    status_connected();
    /* The caller gates its join on this line, so it goes out with the ack that
     * the session is up — before any START, whatever the clock state. Under
     * the send lock like every other reader of the latch: the command thread
     * is live from here and re-arms it on FLUSH and START. */
    pthread_mutex_lock(&g_audio_send_lock);
    clock_ready_tick();
    pthread_mutex_unlock(&g_audio_send_lock);

    uint64_t last = 0, frames = 0;
    uint64_t current_epoch = ap2_session_epoch(g_session);
    uint8_t *buf = malloc(AP2_FRAMES_PER_CHUNK * ap2_input_bpf);
    uint8_t *ap2_alac_buf = (cfg->bit_depth > 16) ? malloc(AP2_FRAMES_PER_CHUNK * ap2_alac_bpf) : NULL;
    bool playback_failed = false;
    bool starving = false;
    uint64_t starvation_started = 0;
    bool input_ended = false;     /* the input stream was fully consumed */
    bool eof_reported = false;
    uint64_t eof_time = 0;

    /* Main audio loop */
    while (g_status != STATUS_STOPPED) {
        uint64_t now = raopcl_get_ntp(NULL);
        ap2_session_poll(g_session);
        if (ap2_session_state(g_session) == AP2_SESSION_ENDED)
            break;   /* DISCONNECT or idle timeout */

        if (!ap2cl_is_connected(g_ap2cl) ||
            !ap2cl_control_healthy(g_ap2cl)) {
            status_error("AirPlay 2 control channel failed");
            playback_failed = true;
            break;
        }

        /* Each START bumps the epoch; elapsed restarts at the new anchor. */
        uint64_t epoch = ap2_session_epoch(g_session);
        if (epoch != current_epoch) {
            current_epoch = epoch;
            frames = 0;
            input_ended = false;
            eof_reported = false;
            eof_time = 0;
        }

        /* Receiver-clock verification of a cold-clock START, driven on every
         * iteration whatever the playback status and whatever the input is
         * doing: a join's ack is withheld until this resolves, so a pause, a
         * park, or an input that ends before the anchor must not strand it.
         * Once the verification can no longer answer — it resolved, or a
         * commit, flush or park disarmed it — the withheld ack falls due here.
         * The send lock is held because a correction rebases the anchor line
         * the send path reads. */
        pthread_mutex_lock(&g_audio_send_lock);
        clock_verify_tick();
        if (!ap2cl_clock_verify_armed(g_ap2cl)) start_ack_flush();
        clock_ready_tick();
        pthread_mutex_unlock(&g_audio_send_lock);

        /* Periodic status reporting (only while playing, so a pause does not keep
           emitting a "playing" status that would revive the sender's play state) */
        if (g_status == STATUS_PLAYING && !input_ended && now - last > MS2NTP(1000)) {
            last = now;
            /* Delivery runs ahead of audibility by the pacing depth (shallow
             * on the splice timeline, the latency lead otherwise). */
            uint32_t latency_frames = ap2cl_audible_lag_frames(g_ap2cl);
            if (frames > latency_frames) {
                uint32_t elapsed = TS2MS(frames - latency_frames, cfg->sample_rate);
                status_playing(elapsed);
            }
        }

        if (input_ended) {
            /* Input consumed: the content drains on schedule and eof is
             * reported once; then idle awaiting the next START or the idle
             * timeout. An armed splice line keeps carrying silence through
             * the drain and the idle wait: a receiver whose queue underruns
             * while the session stays armed pops audibly (the end-of-playback
             * burst on Apple receivers), while a later teardown with silence
             * still queued is clean. */
            bool drained = !ap2cl_is_playing(g_ap2cl) ||
                           now - eof_time > MS2NTP(cfg->latency_ms + 2000);
            if (drained && !eof_reported) {
                eof_reported = true;
                status_eof();
            }
            if (cfg->protocol == PROTO_AIRPLAY2 &&
                ap2cl_splice_hot(g_ap2cl)) {
                pthread_mutex_lock(&g_audio_send_lock);
                if (ap2cl_splice_hot(g_ap2cl) &&
                    ap2cl_accept_frames(g_ap2cl) &&
                    !ap2_send_silence_chunk(cfg, buf, ap2_alac_buf,
                                            ap2_input_bpf)) {
                    status_error("AirPlay 2 realtime send failed");
                    playback_failed = true;
                    pthread_mutex_unlock(&g_audio_send_lock);
                    break;
                }
                pthread_mutex_unlock(&g_audio_send_lock);
                usleep(1000);
            } else {
                usleep(drained ? 50000 : 10000);
            }
            continue;
        }

        /* Send audio chunk */
        if (g_status == STATUS_PLAYING) {
            pthread_mutex_lock(&g_audio_send_lock);
            if (g_status != STATUS_PLAYING) {
                pthread_mutex_unlock(&g_audio_send_lock);
                usleep(1000);
                continue;
            }
            /* Corrected-join content debt: discard the queued input the
             * correction advanced past, so the first retained sample is the
             * one the group timeline schedules at the corrected instant.
             * Serviced above the pacing gate — discarded bytes never reach
             * the wire, so the gate has no business throttling them and the
             * debt pays down across the whole quiet stretch the correction
             * opens instead of the last window before the anchor. Nothing is
             * sent until it is settled: every retained sample belongs after
             * the corrected instant. */
            uint32_t debt = ap2cl_content_skip_bytes(g_ap2cl);
            if (debt) {
                int dropped = ap2_session_discard(g_session, (int)debt, 250);
                if (dropped == -2) {
                    pthread_mutex_unlock(&g_audio_send_lock);
                    break;
                }
                if (dropped > 0) {
                    g_content_cut_dropped += (uint32_t)dropped;
                    ap2cl_content_skip_consume(g_ap2cl, (uint32_t)dropped);
                }
                if (dropped == -1) {
                    /* The input ended inside the cut: what is left of the debt
                     * has no content to take, so the cut settles short here
                     * rather than outliving the stream it belonged to. */
                    LOG_INFO("End of AirPlay 2 input stream, draining buffer...");
                    input_ended = true;
                    eof_time = raopcl_get_ntp(NULL);
                    ap2cl_content_skip_consume(g_ap2cl, debt);
                    content_cut_report(ap2_input_byte_rate);
                }
                pthread_mutex_unlock(&g_audio_send_lock);
                continue;
            }
            content_cut_report(ap2_input_byte_rate);
            if (!ap2cl_accept_frames(g_ap2cl)) {
                pthread_mutex_unlock(&g_audio_send_lock);
                usleep(1000);
                continue;
            }
            epoch = ap2_session_epoch(g_session);
            if (epoch != current_epoch) {
                current_epoch = epoch;
                frames = 0;
                input_ended = false;
                eof_reported = false;
                eof_time = 0;
            }
            /* Delivery-stall guard (splice timeline): a stall longer than
             * the pacing depth — process freeze, network dropout — leaves
             * the head behind the wall clock with input still queued, which
             * the zero-read starvation recovery below can never see. Pad the
             * timeline forward before reading, or this iteration would send
             * real content on past timestamps (an audible noise trigger on
             * Apple receivers). Gated like that recovery: a frozen head
             * outside session-PLAYING is a parked timeline, not a stall. */
            if (ap2_session_state(g_session) == AP2_SESSION_PLAYING)
                ap2cl_recover_delivery_gap(g_ap2cl);
            /* Splice pad (splice timeline): silence owed to the wire
             * before the next real sample, sent as ordinary encoded chunks so
             * sequence numbers and timestamps stay contiguous (any stamp jump
             * is an audible noise burst). A partial pad occupies the head of
             * the chunk and the first real samples complete it, so the splice
             * lands sample-exact on the commanded instant. */
            uint32_t pad = ap2cl_splice_pad_frames(g_ap2cl);
            uint32_t pad_frames_now = pad < AP2_FRAMES_PER_CHUNK
                                          ? pad : AP2_FRAMES_PER_CHUNK;
            int pad_bytes = (int)pad_frames_now * ap2_input_bpf;
            int want = AP2_FRAMES_PER_CHUNK * ap2_input_bpf - pad_bytes;
            int n = 0;
            if (want > 0) {
                n = ap2_session_read(g_session, buf + pad_bytes, want, 250);
                if (n == -2) {
                    pthread_mutex_unlock(&g_audio_send_lock);
                    break;
                }
                if (n == -1) {
                    LOG_INFO("End of AirPlay 2 input stream, draining buffer...");
                    input_ended = true;
                    eof_time = raopcl_get_ntp(NULL);
                    pthread_mutex_unlock(&g_audio_send_lock);
                    continue;
                }
                if (n == 0) {
                    /* A zero read while idle-primed (post-FLUSH awaiting
                     * START) or in standby is by design, not starvation: the
                     * timeline is frozen and a recovery re-anchor here would
                     * announce anchor jumps to a receiver still rendering its
                     * buffered audio. Only a stall of a genuinely playing
                     * stream is an input gap. */
                    if (ap2_session_state(g_session) != AP2_SESSION_PLAYING) {
                        pthread_mutex_unlock(&g_audio_send_lock);
                        continue;
                    }
                    if (!starving) {
                        starving = true;
                        starvation_started = raopcl_get_ntp(NULL);
                        LOG_WARN("[AP2] PCM input starved; waiting in 250 ms intervals");
                        ap2cl_log_diagnostics(g_ap2cl);
                    }
                    ap2cl_recover_input_gap(g_ap2cl);
                    pthread_mutex_unlock(&g_audio_send_lock);
                    continue;
                }
                if (starving) {
                    uint32_t stalled_ms =
                        (uint32_t)NTP2MS(raopcl_get_ntp(NULL) -
                                         starvation_started);
                    LOG_INFO("[AP2] PCM input recovered after %u ms", stalled_ms);
                    ap2cl_log_diagnostics(g_ap2cl);
                    starving = false;
                }
            }
            if (pad_bytes)
                memset(buf, 0, (size_t)pad_bytes);
            n = pad_final_pcm_chunk(
                buf, pad_bytes + n, AP2_FRAMES_PER_CHUNK * ap2_input_bpf,
                ap2_input_bpf);

            int af = n / ap2_input_bpf;
            uint8_t *send = buf;
            if (ap2_alac_buf && cfg->bit_depth > 16) {
                truncate_32to24(buf, n, ap2_alac_buf);
                send = ap2_alac_buf;
            }
            ap2_send_result_t result =
                ap2cl_send_chunk(g_ap2cl, send, af);
            if (result == AP2_SEND_FATAL) {
                status_error("AirPlay 2 realtime send failed");
                playback_failed = true;
                pthread_mutex_unlock(&g_audio_send_lock);
                break;
            }
            ap2cl_splice_pad_consume(g_ap2cl, pad_frames_now);
            /* Pad silence is not media content: elapsed counts only the real
             * samples so the position base stays on the new track's start. */
            frames += af - (int)pad_frames_now;
            pthread_mutex_unlock(&g_audio_send_lock);
        } else {
            /* Paused, parked in standby, or connected and awaiting the
             * commanded first START. On the splice timeline every armed
             * window keeps sending silence: the idle-primed gap between a
             * FLUSH and the next START (a slow next-track spin-up must not
             * lapse the line — the track-transition blip), a content pause,
             * and a standby park (MA's group pause parks members through
             * standby — a receiver whose queue underruns while the session
             * stays armed pops at the pause press, measured by ear A/B on
             * an Apple TV 4K, 2026-07-31). Contiguous silence keeps every
             * boundary a hot splice; the resume pad still lands the new
             * content on the commanded instant. Stop and teardown end the
             * feed — a teardown with audio still queued is clean — and the
             * session engine's idle timeout still ends a forgotten park. */
            if (g_first_start_done && cfg->protocol == PROTO_AIRPLAY2 &&
                ap2cl_splice_hot(g_ap2cl)) {
                pthread_mutex_lock(&g_audio_send_lock);
                if (g_status == STATUS_PAUSED &&
                    ap2cl_splice_hot(g_ap2cl) &&
                    ap2cl_accept_frames(g_ap2cl) &&
                    !ap2_send_silence_chunk(cfg, buf, ap2_alac_buf,
                                            ap2_input_bpf)) {
                    status_error("AirPlay 2 realtime send failed");
                    playback_failed = true;
                    pthread_mutex_unlock(&g_audio_send_lock);
                    break;
                }
                pthread_mutex_unlock(&g_audio_send_lock);
            }
            usleep(1000);
        }
    }

    /* The loop ends on teardown, and a join whose verification never resolved
     * is still owed its one ack. A cut caught mid-drain is settled short by
     * the teardown, so it reports what it managed to take. */
    start_ack_flush();
    content_cut_report(ap2_input_byte_rate);
    request_command_stop();
    bool command_joined = join_command_thread();
    ap2_session_destroy(g_session);
    g_session = NULL;
    free(buf);
    free(ap2_alac_buf);
    return playback_failed || !command_joined ? 1 : 0;
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

    /* Advertise the caller's identity as the grandmaster when given, so the
     * daemon and the per-stream sessions present one clock identity. Without
     * --dacp the default engine identity is kept (existing callers). */
    unsigned long long clock_id = 0;
    if (cfg->dacp_id && sscanf(cfg->dacp_id, "%16llx", &clock_id) == 1) {
        LOG_INFO("[PTP] daemon grandmaster identity %016llx (from --dacp)", clock_id);
    } else {
        clock_id = 0;
    }

    return ap2_ptp_run_daemon(bind_addr, (uint64_t)clock_id, &g_ptp_daemon_stop);
}

/* ---- Usage ---- */

static void print_usage(const char *name)
{
    printf("cliairplay v%s - Unified AirPlay streaming CLI\n\n", CLIAIRPLAY_VERSION);
    printf("Usage: %s [options] --cmdpipe <path> <host_ip>\n\n", name);
    printf("Protocol selection:\n");
    printf("  --protocol <auto|raop|airplay2>  Protocol to use (default: auto).\n");
    printf("                             auto picks RAOP vs AirPlay 2 from the mDNS\n");
    printf("                             features in --txt; raop/airplay2 force it.\n\n");
    printf("Common options:\n");
    printf("  --port <port>              Device port (default: 5000)\n");
    printf("  --volume <0-100>           Initial volume level\n");
    printf("  --latency <ms>             Playback lead / buffer in ms (default: 2000,\n");
    printf("                             clamped into the device-reported window)\n");
    printf("  --dacp <id>                DACP ID\n");
    printf("  --activeremote <id>        Active Remote ID\n");
    printf("  --cmdpipe <path>           Required named pipe for commands and metadata\n");
    printf("                             (audio is the process stdin, not this pipe)\n");
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
    printf("  --password <password>      Device password: RAOP digest auth, and the\n");
    printf("                             AirPlay 2 transient pairing secret\n");
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
    printf("  --no-ptp                   Force NTP timing even when the device\n");
    printf("                             advertises SupportsPTP (wins over --ptp)\n");
    printf("  --buffer-depth-ms <ms>     Receiver queue depth on the splice timeline\n");
    printf("                             (default 600; deeper feeds multiroom masters,\n");
    printf("                             at the cost of seek responsiveness)\n");
    printf("  --ptp-shared               Prefer a shared PTP daemon clock (multi-room):\n");
    printf("                             read the elected clock from shared memory and do\n");
    printf("                             not bind 319/320 when a daemon is present; else\n");
    printf("                             fall back to the in-process engine\n");
    printf("  --ptp-daemon               Run ONLY the shared PTP clock: bind 319/320 once,\n");
    printf("                             publish the elected master to shared memory, and\n");
    printf("                             serve the control channel until signaled. One per\n");
    printf("                             host; needs root. Takes no host/audio args.\n\n");
    printf("Examples:\n");
    printf("  # RAOP command-only session:\n");
    printf("  %s --protocol raop --cmdpipe /tmp/raop-cap 192.168.1.50\n\n", name);
    printf("  # AirPlay 2 command-only session:\n");
    printf("  %s --protocol airplay2 --auth <creds> --name \"HomePod\" \\\n", name);
    printf("    --cmdpipe /tmp/ap2-cap 192.168.1.50\n");
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

    static struct option long_options[] = {
        {"protocol",     required_argument, 0, 'P'},
        {"port",         required_argument, 0, 'p'},
        {"volume",       required_argument, 0, 'v'},
        {"latency",      required_argument, 0, 'l'},
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
        {"no-ptp",       no_argument,       0, 1014},
        {"buffer-depth-ms", required_argument, 0, 1015},
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
        case 1014: cfg.no_ptp = true; break;
        case 1015: cfg.splice_depth_ms = atoi(optarg); break;
        case 1011: ptp_daemon_mode = true; break;
        case 1013: pair_setup_mode = true; break;
        case 1012: cfg.ptp_shared = true; break;
        case 1002:
            printf("cliairplay v%s check\n", CLIAIRPLAY_VERSION);
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

    /* The only streaming positional argument is the target host. */
    if (optind < argc) cfg.host = argv[optind++];

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

    /* PTP daemon mode: no host; run the shared clock until signaled.
     * MA starts one of these per host for multi-room AirPlay 2 (see README). */
    if (ptp_daemon_mode) {
        int rc = run_ptp_daemon(&cfg);
        netsock_close();
        cross_ssl_free();
        return rc;
    }

    /* Validate the common positional argument before route resolution. */
    if (!cfg.host) {
        print_usage(argv[0]);
        return 1;
    }
    if (optind < argc) {
        status_error(
            "Streaming audio must be provided on stdin, not argv");
        return 1;
    }

    /* Resolve the streaming route from the discovery TXT and any overrides.
     * --protocol auto (the default) picks RAOP vs AirPlay 2 from the mDNS
     * features; explicit raop/airplay2 force the protocol; --ap2-native and
     * --ptp are forcing overrides. This is the single decision point:
     * cfg.protocol becomes the concrete protocol used for dispatch and
     * cfg.route carries the AirPlay 2 sub-decisions applied in run_airplay2(). */
    bool have_creds = cfg.auth && strlen(cfg.auth) == 192;
    bool have_password = cfg.password && *cfg.password;
    /* --no-ptp wins over --ptp and the SupportsPTP auto-detect. Diagnostic
     * escape hatch only: no device is routed here by default (receivers that
     * looked PTP-broken turned out to need a deeper splice queue instead). */
    bool ptp_forced = cfg.ptp || cfg.no_ptp;
    bool ptp_enabled = cfg.ptp && !cfg.no_ptp;
    cfg.route = ap2_resolve_route(cfg.proto_pref, cfg.ap2_txt, cfg.pw, have_creds,
                                  have_password, cfg.bit_depth, cfg.force_native,
                                  ptp_forced, ptp_enabled);
    cfg.protocol = cfg.route.use_raop ? PROTO_RAOP : PROTO_AIRPLAY2;
    LOG_INFO("[AP2] auto-selected: %s; timing=%s; features=0x%llx; flags=0x%llx; bitdepth=%d",
             cfg.route.reason,
             cfg.route.use_raop ? "n/a" : (cfg.route.ptp ? "PTP" : "NTP"),
             (unsigned long long)cfg.route.features,
             (unsigned long long)cfg.route.flags,
             cfg.bit_depth);
    /* Machine-parseable route report on stdout so the caller (MA) can log and
     * surface which route this stream actually took. The literal buffered=0
     * keeps the line shape stable for existing parsers; buffered playback
     * itself was investigated and removed (DESIGN.md). */
    printf("[STATUS] route protocol=%s flow=%s timing=%s buffered=0\n",
           cfg.route.use_raop ? "raop" : "airplay2",
           cfg.route.use_raop ? "legacy" : (cfg.route.native ? "native" : "raop-compat"),
           (!cfg.route.use_raop && cfg.route.ptp) ? "ptp" : "ntp");
    fflush(stdout);

    /* The device advertises that it needs a password and we hold neither one
     * nor stored credentials: every route would fail its handshake, so report
     * what is missing instead of spending a connect attempt on it. */
    if (cfg.pw && !strcasecmp(cfg.pw, "true") && !have_password && !have_creds) {
        status_error_ex(ERROR_CODE_AUTH_REQUIRED, 0,
                        "device requires a password; none was supplied",
                        "Password required but not supplied");
        return 1;
    }

    if (!cfg.cmdpipe) {
        status_error("Streaming requires --cmdpipe");
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Setup command pipe */
    struct stat st;
    if (stat(cfg.cmdpipe, &st) != 0) {
        if (mkfifo(cfg.cmdpipe, 0666) != 0) {
            status_error_ex(ERROR_CODE_CONNECT_FAILED, 0,
                            "cannot create the command pipe",
                            "Failed to create command pipe");
            return 1;
        }
    }

    int result = cfg.protocol == PROTO_RAOP
        ? run_raop(&cfg) : run_airplay2(&cfg);

    /* Cleanup */
    request_command_stop();
    if (!join_command_thread()) return 1;
    struct ap2cl_s *ap2_client = atomic_exchange(&g_ap2cl, NULL);
    struct raopcl_s *raop_client = atomic_exchange(&g_raopcl, NULL);
    if (cfg.cmdpipe) {
        if (g_cmdpipe_fd >= 0) close(g_cmdpipe_fd);
        unlink(cfg.cmdpipe);
    }
    if (ap2_client) ap2cl_destroy(ap2_client);
    if (raop_client) {
        raopcl_disconnect(raop_client);
        raopcl_destroy(raop_client);
    }
    free(g_metadata.title);
    free(g_metadata.artist);
    free(g_metadata.album);
    g_metadata.title = g_metadata.artist = g_metadata.album = NULL;
    netsock_close();
    cross_ssl_free();

    return result;
}
