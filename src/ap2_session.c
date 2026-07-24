/*
 * Persistent session - single-stream flush-and-refill engine (see ap2_session.h)
 *
 * Threading model: one reader thread drains the persistent input fd into one
 * ring for the whole session. The command-pipe thread drives start/flush/
 * standby; the audio loop only ever calls ap2_session_read()/poll(). A FLUSH
 * must drain the input fd without racing the reader on the same descriptor, so
 * the cmdpipe thread parks the reader (drain_request -> reader_paused handshake)
 * before it resets the ring and drains the fd, then releases it onto the empty
 * ring to buffer the next track. The reader never blocks (the fd is
 * non-blocking and poll uses a short timeout), so the handshake is bounded and
 * the cmdpipe thread is never wedged.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ap2_io.h"
#include "ap2_session.h"
#include "cross_log.h"

extern log_level *loglevel;

#define SRING_MIN_BYTES   (1u << 20)   /* 1 MiB floor */
#define SRING_SECONDS     4            /* ring depth in media time */
#define READER_POLL_MS    30           /* input poll slice; bounds pause latency */

/* The single input ring, filled by the reader thread. */
typedef struct {
    uint8_t *data;
    size_t cap, rd, wr, fill;
    bool eof;        /* input closed (or read error): end of stream */
} sring_t;

struct ap2_session_s {
    ap2_session_ops_t ops;
    unsigned byte_rate;
    int idle_timeout_ms;
    int input_fd;              /* owned dup of the caller's PCM descriptor */

    pthread_mutex_t lock;
    pthread_cond_t can_read;   /* ring gained data / state changed */
    pthread_cond_t can_write;  /* ring gained space / abort / drain requested */
    pthread_cond_t reader_paused_cond;  /* reader entered the paused state */
    pthread_cond_t reader_resume_cond;  /* drain finished; reader may continue */

    pthread_t reader;
    bool reader_started;
    bool reader_paused;        /* reader parked for a drain */
    bool drain_request;        /* cmdpipe asked the reader to park */
    bool abort;                /* tear the reader down */

    ap2_session_state_t state;
    uint64_t epoch;            /* bumped on every START */
    uint64_t idle_since_ms;    /* monotonic ms when we last went idle */
    bool audio_seen;           /* first input bytes since create/FLUSH arrived */
    sring_t ring;
};

/* ---- ring helpers (called with s->lock held) ---- */

static bool sring_init(sring_t *r, unsigned byte_rate)
{
    size_t cap = (size_t)byte_rate * SRING_SECONDS;
    if (cap < SRING_MIN_BYTES) cap = SRING_MIN_BYTES;
    memset(r, 0, sizeof(*r));
    r->data = malloc(cap);
    if (!r->data) return false;
    r->cap = cap;
    return true;
}

static void sring_reset(sring_t *r)
{
    r->rd = r->wr = r->fill = 0;
    r->eof = false;
}

static size_t sring_pop(sring_t *r, uint8_t *buf, size_t want)
{
    size_t n = want < r->fill ? want : r->fill;
    size_t first = r->cap - r->rd;
    if (first > n) first = n;
    memcpy(buf, r->data + r->rd, first);
    memcpy(buf + first, r->data, n - first);
    r->rd = (r->rd + n) % r->cap;
    r->fill -= n;
    return n;
}

static size_t sring_push(sring_t *r, const uint8_t *buf, size_t len)
{
    size_t space = r->cap - r->fill;
    size_t n = len < space ? len : space;
    size_t first = r->cap - r->wr;
    if (first > n) first = n;
    memcpy(r->data + r->wr, buf, first);
    memcpy(r->data, buf + first, n - first);
    r->wr = (r->wr + n) % r->cap;
    r->fill += n;
    return n;
}

/* ---- status emission (short callback; safe under the session lock) ---- */

static void emit(struct ap2_session_s *s, const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s->ops.status(line);
}

/* ---- input reader thread (single, persistent) ---- */

/* Read+discard from the fd until it would block. Called by the cmdpipe thread
 * under s->lock with the reader parked, so it has exclusive fd access. */
static void drain_input_fd(int fd)
{
    uint8_t scratch[16384];
    /* Cap the loop so a misbehaving writer that never stops can never wedge the
     * cmdpipe thread; MA stops writing old bytes before FLUSH, so a well-behaved
     * drain terminates on EAGAIN long before this. */
    for (int guard = 0; guard < 100000; guard++) {
        ssize_t n = read(fd, scratch, sizeof(scratch));
        if (n <= 0) return;   /* EAGAIN / EOF / error: nothing more to discard */
    }
}

static void *reader_thread(void *arg)
{
    struct ap2_session_s *s = arg;
    uint8_t buf[16384];

    for (;;) {
        pthread_mutex_lock(&s->lock);
        /* Park on a drain request: the cmdpipe thread needs exclusive fd access
         * to drain it, so we release the lock and wait until the drain is done. */
        while (s->drain_request && !s->abort) {
            s->reader_paused = true;
            pthread_cond_broadcast(&s->reader_paused_cond);
            pthread_cond_wait(&s->reader_resume_cond, &s->lock);
        }
        s->reader_paused = false;
        bool abort = s->abort;
        bool at_eof = s->ring.eof;
        pthread_mutex_unlock(&s->lock);
        if (abort) break;

        if (at_eof) {
            /* Input closed (teardown): stay alive to honour pause/abort, but do
             * not busy-poll a closed descriptor. */
            usleep(20000);
            continue;
        }

        struct pollfd pfd = {.fd = s->input_fd, .events = POLLIN};
        int polled = poll(&pfd, 1, READER_POLL_MS);
        if (polled < 0 && errno == EINTR) continue;

        /* A drain may have been requested while we polled; park before touching
         * the fd so the cmdpipe thread keeps exclusive access. */
        pthread_mutex_lock(&s->lock);
        bool pause_now = s->drain_request;
        pthread_mutex_unlock(&s->lock);
        if (pause_now) continue;
        if (polled <= 0) continue;   /* timeout: no data yet */

        ssize_t n = read(s->input_fd, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) {
            /* A clean EOF (0) is the normal teardown path; a negative return is
             * a read error. Both end the stream. */
            int read_errno = errno;
            pthread_mutex_lock(&s->lock);
            s->ring.eof = true;
            pthread_cond_broadcast(&s->can_read);
            pthread_mutex_unlock(&s->lock);
            if (n < 0)
                LOG_ERROR("[SESSION] input read error: %s", strerror(read_errno));
            continue;
        }

        size_t off = 0;
        pthread_mutex_lock(&s->lock);
        while (off < (size_t)n && !s->abort && !s->drain_request) {
            size_t pushed = sring_push(&s->ring, buf + off, (size_t)n - off);
            off += pushed;
            if (pushed) {
                pthread_cond_broadcast(&s->can_read);
                if (!s->audio_seen) {
                    /* One-shot per start cycle: the caller uses this to know
                     * the new track's audio is flowing before it commands a
                     * START, so it can anchor with a short lead instead of
                     * blind margin for source/transcoder spin-up. */
                    s->audio_seen = true;
                    emit(s, "[STATUS] audio buffered_ms=%llu",
                         (unsigned long long)(s->ring.fill * 1000ULL /
                                              s->byte_rate));
                }
            }
            if (off < (size_t)n)
                pthread_cond_wait(&s->can_write, &s->lock);
        }
        pthread_mutex_unlock(&s->lock);
        /* If a drain interrupted the push, the unwritten tail is pre-flush audio
         * and is dropped on purpose. */
    }
    return NULL;
}

/* ---- public API ---- */

struct ap2_session_s *ap2_session_create(const ap2_session_ops_t *ops,
                                         unsigned byte_rate, int idle_timeout_ms,
                                         int input_fd)
{
    if (!ops || !ops->quiesce || !ops->flush || !ops->commit || !ops->resume ||
        !ops->stop || !ops->status || !byte_rate || input_fd < 0)
        return NULL;
    struct ap2_session_s *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ops = *ops;
    s->byte_rate = byte_rate;
    s->idle_timeout_ms = idle_timeout_ms;
    s->input_fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->can_read, NULL);
    pthread_cond_init(&s->can_write, NULL);
    pthread_cond_init(&s->reader_paused_cond, NULL);
    pthread_cond_init(&s->reader_resume_cond, NULL);
    s->state = AP2_SESSION_IDLE;
    s->idle_since_ms = ap2_io_monotonic_ms();
    if (!sring_init(&s->ring, byte_rate)) {
        ap2_session_destroy(s);
        return NULL;
    }

    /* Own a private, non-blocking dup of the caller's descriptor: the reader
     * polls for readiness and must never block, and the caller keeps its own
     * reference (killing the per-seek transcoder must not close our input). */
    s->input_fd = dup(input_fd);
    if (s->input_fd < 0) {
        LOG_ERROR("[SESSION] cannot dup input fd: %s", strerror(errno));
        ap2_session_destroy(s);
        return NULL;
    }
    int fl = fcntl(s->input_fd, F_GETFL, 0);
    if (fl != -1)
        (void)fcntl(s->input_fd, F_SETFL, fl | O_NONBLOCK);

    if (pthread_create(&s->reader, NULL, reader_thread, s) != 0) {
        LOG_ERROR("[SESSION] cannot start input reader");
        ap2_session_destroy(s);
        return NULL;
    }
    s->reader_started = true;
    return s;
}

void ap2_session_destroy(struct ap2_session_s *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->state = AP2_SESSION_ENDED;
    s->abort = true;
    /* Wake the reader out of any wait (ring-full, paused, or state change). */
    pthread_cond_broadcast(&s->can_read);
    pthread_cond_broadcast(&s->can_write);
    pthread_cond_broadcast(&s->reader_resume_cond);
    pthread_mutex_unlock(&s->lock);
    if (s->reader_started)
        pthread_join(s->reader, NULL);
    if (s->input_fd >= 0) close(s->input_fd);
    pthread_cond_destroy(&s->can_read);
    pthread_cond_destroy(&s->can_write);
    pthread_cond_destroy(&s->reader_paused_cond);
    pthread_cond_destroy(&s->reader_resume_cond);
    pthread_mutex_destroy(&s->lock);
    free(s->ring.data);
    free(s);
}

bool ap2_session_start(struct ap2_session_s *s, uint64_t start_unix_ms)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool ended = s->state == AP2_SESSION_ENDED;
    pthread_mutex_unlock(&s->lock);
    if (ended) return false;

    /* Stop the audio loop between chunks, do the transport work, then swap to
     * PLAYING. On the first start commit begins the session; after a FLUSH it
     * re-bases the frozen anchor. */
    s->ops.quiesce(s->ops.transport);
    if (!s->ops.commit(s->ops.transport, start_unix_ms)) {
        s->ops.resume(s->ops.transport);
        LOG_ERROR("[SESSION] transport commit failed");
        return false;
    }
    pthread_mutex_lock(&s->lock);
    if (s->state == AP2_SESSION_ENDED) {
        pthread_mutex_unlock(&s->lock);
        s->ops.resume(s->ops.transport);
        return false;
    }
    s->ring.eof = false;   /* a fresh START clears any stale end-of-stream */
    s->epoch++;
    s->state = AP2_SESSION_PLAYING;
    pthread_cond_broadcast(&s->can_read);
    pthread_cond_broadcast(&s->can_write);
    pthread_mutex_unlock(&s->lock);
    s->ops.resume(s->ops.transport);
    return true;
}

bool ap2_session_flush(struct ap2_session_s *s)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool ended = s->state == AP2_SESSION_ENDED;
    pthread_mutex_unlock(&s->lock);
    if (ended) return false;

    /* Stop sending, then discard the receiver's buffered audio in place. */
    s->ops.quiesce(s->ops.transport);
    s->ops.flush(s->ops.transport);

    pthread_mutex_lock(&s->lock);
    /* Park the reader so we can drain the input fd without racing it, then
     * discard our own ring and drain the fd to EAGAIN. MA has already stopped
     * writing old bytes, so this removes exactly the pre-flush audio. */
    s->drain_request = true;
    pthread_cond_broadcast(&s->can_write);
    while (s->reader_started && !s->reader_paused && !s->abort)
        pthread_cond_wait(&s->reader_paused_cond, &s->lock);
    sring_reset(&s->ring);
    drain_input_fd(s->input_fd);
    /* Re-arm the one-shot audio signal: the next bytes to arrive belong to
     * the new track and announce that its feed is flowing. */
    s->audio_seen = false;
    /* Release the reader onto the now-empty ring: it buffers the next track
     * while we send nothing until the next START (idle-primed). */
    s->drain_request = false;
    if (s->state != AP2_SESSION_ENDED) {
        s->state = AP2_SESSION_IDLE;
        s->idle_since_ms = ap2_io_monotonic_ms();
    }
    pthread_cond_broadcast(&s->reader_resume_cond);
    pthread_cond_broadcast(&s->can_read);
    pthread_mutex_unlock(&s->lock);

    s->ops.resume(s->ops.transport);
    emit(s, "[STATUS] flushed");
    return true;
}

bool ap2_session_standby(struct ap2_session_s *s)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool ended = s->state == AP2_SESSION_ENDED;
    pthread_mutex_unlock(&s->lock);
    if (ended) return false;

    s->ops.quiesce(s->ops.transport);
    s->ops.stop(s->ops.transport);
    pthread_mutex_lock(&s->lock);
    if (s->state != AP2_SESSION_ENDED) {
        s->state = AP2_SESSION_STANDBY;
        s->idle_since_ms = ap2_io_monotonic_ms();
    }
    pthread_cond_broadcast(&s->can_read);
    pthread_mutex_unlock(&s->lock);
    s->ops.resume(s->ops.transport);
    return true;
}

void ap2_session_end(struct ap2_session_s *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->state = AP2_SESSION_ENDED;
    pthread_cond_broadcast(&s->can_read);
    pthread_cond_broadcast(&s->can_write);
    pthread_cond_broadcast(&s->reader_resume_cond);
    pthread_mutex_unlock(&s->lock);
}

int ap2_session_read(struct ap2_session_s *s, uint8_t *buf, int len,
                     int timeout_ms)
{
    if (!s || !buf || len <= 0) return -2;
    uint64_t deadline =
        ap2_io_monotonic_ms() + (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);

    pthread_mutex_lock(&s->lock);
    for (;;) {
        if (s->state == AP2_SESSION_ENDED) {
            pthread_mutex_unlock(&s->lock);
            return -2;
        }
        if (s->state == AP2_SESSION_PLAYING) {
            if (s->ring.fill > 0) {
                size_t n = sring_pop(&s->ring, buf, (size_t)len);
                pthread_cond_broadcast(&s->can_write);
                pthread_mutex_unlock(&s->lock);
                return (int)n;
            }
            if (s->ring.eof) {
                pthread_mutex_unlock(&s->lock);
                return -1;   /* input stream fully consumed */
            }
        }
        uint64_t now = ap2_io_monotonic_ms();
        if (now >= deadline) {
            pthread_mutex_unlock(&s->lock);
            return 0;
        }
        uint64_t wait_ms = deadline - now;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(wait_ms / 1000);
        ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->can_read, &s->lock, &ts);
    }
}

void ap2_session_poll(struct ap2_session_s *s)
{
    if (!s || s->idle_timeout_ms <= 0) return;
    pthread_mutex_lock(&s->lock);
    bool idle = s->state == AP2_SESSION_IDLE || s->state == AP2_SESSION_STANDBY;
    if (idle && ap2_io_monotonic_ms() - s->idle_since_ms >=
                    (uint64_t)s->idle_timeout_ms) {
        s->state = AP2_SESSION_ENDED;
        emit(s, "[STATUS] idle_timeout");
        pthread_cond_broadcast(&s->can_read);
        pthread_cond_broadcast(&s->can_write);
        pthread_cond_broadcast(&s->reader_resume_cond);
    }
    pthread_mutex_unlock(&s->lock);
}

ap2_session_state_t ap2_session_state(struct ap2_session_s *s)
{
    if (!s) return AP2_SESSION_ENDED;
    pthread_mutex_lock(&s->lock);
    ap2_session_state_t st = s->state;
    pthread_mutex_unlock(&s->lock);
    return st;
}

uint64_t ap2_session_epoch(struct ap2_session_s *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    uint64_t e = s->epoch;
    pthread_mutex_unlock(&s->lock);
    return e;
}
