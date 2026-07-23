/*
 * Persistent session - generation state machine (see ap2_session.h)
 *
 * Threading model: PREPARE spawns one reader thread per generation that
 * drains the generation's input into its own ring. The command-pipe thread
 * drives prepare/start/flush/standby; START performs the transport commit
 * (RTSP work is serialized inside the transport) and swaps the staged slot
 * in as active under the session lock. The audio loop only ever calls
 * ap2_session_read()/poll(), so a generation switch is invisible to it
 * beyond the data changing.
 *
 * Slots are heap objects with stable addresses for their reader threads;
 * generation switches swap pointers, never slot contents. A retired slot is
 * freed only after its reader has been joined.
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ap2_io.h"
#include "ap2_session.h"
#include "cross_log.h"

extern log_level *loglevel;

#define SRING_MIN_BYTES   (1u << 20)   /* 1 MiB floor */
#define SRING_SECONDS     4            /* ring depth in media time */

/* One generation's input ring, filled by its reader thread. */
typedef struct {
    uint8_t *data;
    size_t cap, rd, wr, fill;
    bool eof;        /* writer closed (or read error) */
    bool abort;      /* stop the reader and discard */
} sring_t;

typedef struct {
    ap2_generation_t gen;
    char *path_owned;         /* strdup'd audio_path (gen.audio_path aliases) */
    sring_t ring;
    pthread_t thread;
    bool thread_started;
    bool ready_sent;
    bool primed_sent;
} slot_t;

struct ap2_session_s {
    ap2_session_ops_t ops;
    unsigned byte_rate;
    size_t prefill_bytes;
    int idle_timeout_ms;

    pthread_mutex_t lock;
    pthread_cond_t can_read;   /* active ring gained data / state changed */
    pthread_cond_t can_write;  /* a ring gained space / abort requested */

    ap2_session_state_t state;
    slot_t *active;
    slot_t *staged;
    uint64_t played_bytes;     /* consumed from the ACTIVE generation */
    uint64_t idle_since_ms;    /* monotonic ms when we last went idle */
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

/* ---- generation reader thread ---- */

typedef struct {
    struct ap2_session_s *s;
    slot_t *slot;
} reader_arg_t;

static void *reader_thread(void *arg)
{
    reader_arg_t a = *(reader_arg_t *)arg;
    free(arg);
    struct ap2_session_s *s = a.s;
    slot_t *slot = a.slot;

    int fd = open(slot->path_owned, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        pthread_mutex_lock(&s->lock);
        slot->ring.eof = true;
        pthread_cond_broadcast(&s->can_read);
        pthread_mutex_unlock(&s->lock);
        LOG_ERROR("[SESSION] cannot open %s: %s", slot->path_owned,
                  strerror(errno));
        return NULL;
    }
    struct stat input_stat;
    if (fstat(fd, &input_stat) < 0) {
        int open_errno = errno;
        pthread_mutex_lock(&s->lock);
        slot->ring.eof = true;
        emit(s, "[STATUS] input_error generation=%llu errno=%d (%s)",
             (unsigned long long)slot->gen.number, open_errno,
             strerror(open_errno));
        pthread_cond_broadcast(&s->can_read);
        pthread_mutex_unlock(&s->lock);
        close(fd);
        return NULL;
    }
    bool writer_seen = !S_ISFIFO(input_stat.st_mode);

    pthread_mutex_lock(&s->lock);
    if (!slot->ring.abort && !slot->ready_sent) {
        slot->ready_sent = true;
        emit(s, "[STATUS] ready generation=%llu",
             (unsigned long long)slot->gen.number);
    }
    pthread_mutex_unlock(&s->lock);

    /* The read below unblocks when the writer closes its end (MA kills the
     * generation's ffmpeg on replace), which is the normal retirement path;
     * abort covers the never-started cases. */
    uint8_t buf[16384];
    for (;;) {
        pthread_mutex_lock(&s->lock);
        bool abort = slot->ring.abort;
        pthread_mutex_unlock(&s->lock);
        if (abort) break;

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int polled = poll(&pfd, 1, 250);
        if (polled < 0 && errno == EINTR) continue;
        if (polled == 0) {
            /* A FIFO with a connected but idle writer has no poll events. */
            writer_seen = true;
            continue;
        }
        if (!writer_seen && (pfd.revents & POLLHUP) &&
            !(pfd.revents & POLLIN)) {
            /* No writer has connected yet. Avoid treating that as input EOF. */
            usleep(10000);
            continue;
        }
        if (pfd.revents & POLLIN) writer_seen = true;

        ssize_t n = polled < 0 ? -1 : read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;  /* interrupted syscall, not an error — retry the read */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        int read_errno = errno;
        pthread_mutex_lock(&s->lock);
        if (slot->ring.abort) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        if (n <= 0) {
            slot->ring.eof = true;
            if (n == 0 && slot->ring.fill > 0 && !slot->primed_sent) {
                slot->primed_sent = true;
                emit(s, "[STATUS] primed generation=%llu prefill_ms=%llu",
                     (unsigned long long)slot->gen.number,
                     (unsigned long long)(slot->ring.fill * 1000ULL /
                                          s->byte_rate));
            }
            /* A clean EOF (0) is the normal retirement path; a negative return
             * is a real read error (e.g. a broken pipe) and is surfaced
             * distinctly instead of being hidden as end-of-input. */
            if (n < 0)
                emit(s, "[STATUS] input_error generation=%llu errno=%d (%s)",
                     (unsigned long long)slot->gen.number, read_errno,
                     strerror(read_errno));
            else
                emit(s, "[STATUS] input_eof generation=%llu",
                     (unsigned long long)slot->gen.number);
            pthread_cond_broadcast(&s->can_read);
            pthread_mutex_unlock(&s->lock);
            break;
        }
        size_t off = 0;
        while (off < (size_t)n && !slot->ring.abort) {
            size_t pushed = sring_push(&slot->ring, buf + off, (size_t)n - off);
            off += pushed;
            if (pushed) pthread_cond_broadcast(&s->can_read);
            if (!slot->primed_sent && slot->ring.fill >= s->prefill_bytes) {
                slot->primed_sent = true;
                emit(s, "[STATUS] primed generation=%llu prefill_ms=%llu",
                     (unsigned long long)slot->gen.number,
                     (unsigned long long)(slot->ring.fill * 1000ULL /
                                          s->byte_rate));
            }
            if (off < (size_t)n)
                pthread_cond_wait(&s->can_write, &s->lock);
        }
        pthread_mutex_unlock(&s->lock);
    }
    close(fd);
    return NULL;
}

/* Stop a detached slot's reader, join it and free the slot. The slot must
 * already be unlinked from the session. Lock must NOT be held. */
static void retire_slot(struct ap2_session_s *s, slot_t *slot)
{
    if (!slot) return;
    pthread_mutex_lock(&s->lock);
    slot->ring.abort = true;
    pthread_cond_broadcast(&s->can_write);
    bool started = slot->thread_started;
    pthread_mutex_unlock(&s->lock);

    if (started) {
        pthread_join(slot->thread, NULL);
    }
    free(slot->ring.data);
    free(slot->path_owned);
    free(slot);
}

/* Detach a slot pointer from the session under the lock. */
static slot_t *detach(struct ap2_session_s *s, slot_t **ref)
{
    pthread_mutex_lock(&s->lock);
    slot_t *slot = *ref;
    *ref = NULL;
    pthread_mutex_unlock(&s->lock);
    return slot;
}

/* ---- public API ---- */

struct ap2_session_s *ap2_session_create(const ap2_session_ops_t *ops,
                                         unsigned byte_rate, int prefill_ms,
                                         int idle_timeout_ms)
{
    if (!ops || !ops->quiesce || !ops->commit || !ops->resume ||
        !ops->stop || !ops->status || !byte_rate)
        return NULL;
    struct ap2_session_s *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ops = *ops;
    s->byte_rate = byte_rate;
    s->prefill_bytes = (size_t)byte_rate * (prefill_ms > 0 ? prefill_ms : 1)
                       / 1000;
    s->idle_timeout_ms = idle_timeout_ms;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->can_read, NULL);
    pthread_cond_init(&s->can_write, NULL);
    s->state = AP2_SESSION_IDLE;
    s->idle_since_ms = ap2_io_monotonic_ms();
    return s;
}

void ap2_session_destroy(struct ap2_session_s *s)
{
    if (!s) return;
    ap2_session_end(s);
    retire_slot(s, detach(s, &s->staged));
    retire_slot(s, detach(s, &s->active));
    pthread_cond_destroy(&s->can_read);
    pthread_cond_destroy(&s->can_write);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

bool ap2_session_prepare(struct ap2_session_s *s, const ap2_generation_t *gen)
{
    if (!s || !gen || !gen->audio_path) return false;
    /* A superseded staged generation is discarded: the caller only ever wants
     * the newest one (rapid seek/seek/seek collapses naturally). */
    retire_slot(s, detach(s, &s->staged));

    slot_t *slot = calloc(1, sizeof(*slot));
    if (!slot) return false;
    slot->gen = *gen;
    slot->path_owned = gen->audio_path ? strdup(gen->audio_path) : NULL;
    slot->gen.audio_path = slot->path_owned;
    if (!sring_init(&slot->ring, s->byte_rate)) {
        free(slot->path_owned);
        free(slot);
        return false;
    }

    pthread_mutex_lock(&s->lock);
    if (s->state == AP2_SESSION_ENDED) {
        pthread_mutex_unlock(&s->lock);
        free(slot->ring.data);
        free(slot->path_owned);
        free(slot);
        return false;
    }
    s->staged = slot;
    if (s->state == AP2_SESSION_IDLE || s->state == AP2_SESSION_STANDBY)
        s->state = AP2_SESSION_PREPARING;
    pthread_mutex_unlock(&s->lock);

    reader_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        retire_slot(s, detach(s, &s->staged));
        return false;
    }
    arg->s = s;
    arg->slot = slot;
    if (pthread_create(&slot->thread, NULL, reader_thread, arg) != 0) {
        free(arg);
        retire_slot(s, detach(s, &s->staged));
        return false;
    }
    pthread_mutex_lock(&s->lock);
    slot->thread_started = true;
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool ap2_session_start(struct ap2_session_s *s, uint64_t generation,
                       uint64_t start_unix_ms)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool ok = s->staged && s->staged->gen.number == generation &&
              s->state != AP2_SESSION_ENDED;
    pthread_mutex_unlock(&s->lock);
    if (!ok) {
        LOG_ERROR("[SESSION] START for generation %llu does not match the "
                  "staged generation",
                  (unsigned long long)generation);
        return false;
    }

    /* Stop the audio loop between its current chunk and the transport FLUSH;
     * keep it stopped until the staged slot is active. */
    s->ops.quiesce(s->ops.transport);
    pthread_mutex_lock(&s->lock);
    ok = s->staged && s->staged->gen.number == generation &&
         s->state != AP2_SESSION_ENDED;
    pthread_mutex_unlock(&s->lock);
    if (!ok) {
        s->ops.resume(s->ops.transport);
        return false;
    }

    /* Transport commit outside the lock: it does RTSP round-trips. The staged
     * reader keeps filling meanwhile; audio sends remain quiesced. */
    if (!s->ops.commit(s->ops.transport, start_unix_ms)) {
        s->ops.resume(s->ops.transport);
        LOG_ERROR("[SESSION] transport commit failed for generation %llu",
                  (unsigned long long)generation);
        return false;
    }

    pthread_mutex_lock(&s->lock);
    /* Command actions are serialized by the cmdpipe thread. Re-validate after
     * the unlocked transport round-trip as a defensive API invariant. */
    if (!s->staged || s->staged->gen.number != generation ||
        s->state == AP2_SESSION_ENDED) {
        pthread_mutex_unlock(&s->lock);
        s->ops.resume(s->ops.transport);
        LOG_WARN("[SESSION] generation %llu superseded before activation",
                 (unsigned long long)generation);
        return false;
    }
    slot_t *old = s->active;
    s->active = s->staged;
    s->staged = NULL;
    s->played_bytes = 0;
    s->state = AP2_SESSION_PLAYING;
    pthread_cond_broadcast(&s->can_read);
    pthread_cond_broadcast(&s->can_write);
    pthread_mutex_unlock(&s->lock);
    s->ops.resume(s->ops.transport);

    retire_slot(s, old);
    return true;
}

bool ap2_session_flush(struct ap2_session_s *s, uint64_t generation)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool is_staged = s->staged && s->staged->gen.number == generation;
    bool is_active = s->active && s->active->gen.number == generation;
    pthread_mutex_unlock(&s->lock);

    if (is_staged) {
        retire_slot(s, detach(s, &s->staged));
        pthread_mutex_lock(&s->lock);
        if (s->state != AP2_SESSION_ENDED) {
            if (s->active &&
                (!s->active->ring.eof || s->active->ring.fill > 0)) {
                s->state = AP2_SESSION_PLAYING;
            } else {
                s->state = AP2_SESSION_IDLE;
                s->idle_since_ms = ap2_io_monotonic_ms();
            }
        }
        pthread_cond_broadcast(&s->can_read);
        pthread_mutex_unlock(&s->lock);
        return true;
    }
    if (is_active) {
        s->ops.quiesce(s->ops.transport);
        s->ops.stop(s->ops.transport);
        slot_t *old = detach(s, &s->active);
        pthread_mutex_lock(&s->lock);
        if (s->state != AP2_SESSION_ENDED) {
            s->state = s->staged ? AP2_SESSION_PREPARING : AP2_SESSION_IDLE;
            s->idle_since_ms = ap2_io_monotonic_ms();
        }
        pthread_cond_broadcast(&s->can_read);
        pthread_mutex_unlock(&s->lock);
        s->ops.resume(s->ops.transport);
        retire_slot(s, old);
        return true;
    }
    return false;
}

bool ap2_session_standby(struct ap2_session_s *s)
{
    if (!s) return false;
    s->ops.quiesce(s->ops.transport);
    s->ops.stop(s->ops.transport);
    slot_t *old = detach(s, &s->active);
    pthread_mutex_lock(&s->lock);
    if (s->state != AP2_SESSION_ENDED) {
        s->state = s->staged ? AP2_SESSION_PREPARING : AP2_SESSION_STANDBY;
        s->idle_since_ms = ap2_io_monotonic_ms();
    }
    pthread_cond_broadcast(&s->can_read);
    pthread_mutex_unlock(&s->lock);
    s->ops.resume(s->ops.transport);
    retire_slot(s, old);
    return true;
}

void ap2_session_end(struct ap2_session_s *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->state = AP2_SESSION_ENDED;
    pthread_cond_broadcast(&s->can_read);
    pthread_cond_broadcast(&s->can_write);
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
        if (s->active && s->active->ring.fill > 0) {
            size_t n = sring_pop(&s->active->ring, buf, (size_t)len);
            s->played_bytes += n;
            pthread_cond_broadcast(&s->can_write);
            pthread_mutex_unlock(&s->lock);
            return (int)n;
        }
        if (s->active && s->active->ring.eof) {
            if (s->state == AP2_SESSION_PLAYING) {
                s->state = s->staged
                    ? AP2_SESSION_PREPARING : AP2_SESSION_IDLE;
                if (s->state == AP2_SESSION_IDLE)
                    s->idle_since_ms = ap2_io_monotonic_ms();
            }
            pthread_mutex_unlock(&s->lock);
            return -1;   /* generation fully consumed */
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

uint64_t ap2_session_active_generation(struct ap2_session_s *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    uint64_t g = s->active ? s->active->gen.number : 0;
    pthread_mutex_unlock(&s->lock);
    return g;
}

uint64_t ap2_session_position_ms(struct ap2_session_s *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->lock);
    uint64_t pos = 0;
    if (s->active)
        pos = s->active->gen.position_ms +
              s->played_bytes * 1000ULL / s->byte_rate;
    pthread_mutex_unlock(&s->lock);
    return pos;
}
