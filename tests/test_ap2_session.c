#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ap2_io.h"
#include "ap2_session.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

/* ap2_session.c is compiled with -Dpoll=ap2_session_test_poll; the single-stream
 * model no longer needs poll injection, so just delegate to the real poll. */
int ap2_session_test_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return poll(fds, nfds, timeout);
}

typedef struct {
    atomic_int commits;
    atomic_int flushes;
    atomic_int quiesces;
    atomic_int resumes;
    atomic_int stops;
    atomic_int flushed_status;
    atomic_int audio_status;
    atomic_int idle_timeouts;
    uint64_t last_start_unix_ms;
} test_state_t;

static test_state_t *status_state;

static void quiesce(void *transport)
{
    atomic_fetch_add(&((test_state_t *)transport)->quiesces, 1);
}

static void flush(void *transport)
{
    atomic_fetch_add(&((test_state_t *)transport)->flushes, 1);
}

static bool commit(void *transport, uint64_t start_unix_ms)
{
    test_state_t *state = transport;
    state->last_start_unix_ms = start_unix_ms;
    atomic_fetch_add(&state->commits, 1);
    return true;
}

static void resume(void *transport)
{
    atomic_fetch_add(&((test_state_t *)transport)->resumes, 1);
}

static void stop(void *transport)
{
    atomic_fetch_add(&((test_state_t *)transport)->stops, 1);
}

static void status(const char *line)
{
    if (strcmp(line, "[STATUS] flushed") == 0)
        atomic_fetch_add(&status_state->flushed_status, 1);
    else if (strncmp(line, "[STATUS] audio ", 15) == 0)
        atomic_fetch_add(&status_state->audio_status, 1);
    else if (strcmp(line, "[STATUS] idle_timeout") == 0)
        atomic_fetch_add(&status_state->idle_timeouts, 1);
}

static void state_init(test_state_t *state)
{
    memset(state, 0, sizeof(*state));
    atomic_init(&state->commits, 0);
    atomic_init(&state->flushes, 0);
    atomic_init(&state->quiesces, 0);
    atomic_init(&state->resumes, 0);
    atomic_init(&state->stops, 0);
    atomic_init(&state->flushed_status, 0);
    atomic_init(&state->audio_status, 0);
    atomic_init(&state->idle_timeouts, 0);
    status_state = state;
}

/* Create a pipe-backed session: the engine reads (a dup of) the pipe read end,
 * the test writes the audio into the returned write end. */
static struct ap2_session_s *create_session(test_state_t *state,
                                            int idle_timeout_ms, int *write_fd)
{
    state_init(state);
    ap2_session_ops_t ops = {
        .quiesce = quiesce,
        .flush = flush,
        .commit = commit,
        .resume = resume,
        .stop = stop,
        .status = status,
        .transport = state,
    };
    int input[2];
    assert(pipe(input) == 0);
    struct ap2_session_s *session =
        ap2_session_create(&ops, 1000, idle_timeout_ms, input[0]);
    assert(session);
    /* The engine owns its own dup of the read end. */
    assert(close(input[0]) == 0);
    *write_fd = input[1];
    return session;
}

static void feed(int write_fd, const uint8_t *data, size_t size)
{
    assert(write(write_fd, data, size) == (ssize_t)size);
}

static void read_exact(struct ap2_session_s *session, const uint8_t *expected,
                       size_t size)
{
    uint8_t got[256];
    assert(size <= sizeof(got));
    size_t off = 0;
    uint64_t deadline = ap2_io_monotonic_ms() + 2000;
    while (off < size && ap2_io_monotonic_ms() < deadline) {
        int n = ap2_session_read(session, got + off, (int)(size - off), 200);
        assert(n >= 0);   /* -1 (input EOF) / -2 (ended) must not happen here */
        off += (size_t)n;
    }
    assert(off == size);
    assert(memcmp(got, expected, size) == 0);
}

/* The reader thread emits [STATUS] audio asynchronously; poll for the count. */
static bool wait_for_count(atomic_int *counter, int want, int timeout_ms)
{
    uint64_t deadline = ap2_io_monotonic_ms() + (uint64_t)timeout_ms;
    while (ap2_io_monotonic_ms() < deadline) {
        if (atomic_load(counter) >= want) return true;
        usleep(5000);
    }
    return atomic_load(counter) >= want;
}

static void test_start_streams_the_input(void)
{
    static const uint8_t audio[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 0, &write_fd);

    /* Before START the engine is idle and sends nothing. */
    assert(ap2_session_state(session) == AP2_SESSION_IDLE);
    assert(ap2_session_epoch(session) == 0);
    feed(write_fd, audio, sizeof(audio));

    const uint64_t audible = 1770000000123ULL;
    assert(ap2_session_start(session, audible));
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);
    assert(ap2_session_epoch(session) == 1);
    assert(atomic_load(&state.commits) == 1);
    assert(atomic_load(&state.quiesces) == 1);
    assert(atomic_load(&state.resumes) == 1);
    assert(state.last_start_unix_ms == audible);

    read_exact(session, audio, sizeof(audio));

    ap2_session_destroy(session);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

/* The headline test: FLUSH drains the ring AND any undelivered stdin bytes and
 * acks with [STATUS] flushed; the next START re-anchors and resumes cleanly from
 * only the post-flush audio. */
static void test_flush_drains_and_reanchors(void)
{
    static const uint8_t old_audio[] = {0xA0, 0xA1, 0xA2, 0xA3};
    static const uint8_t stale_audio[] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
    static const uint8_t new_audio[] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4};
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 0, &write_fd);

    feed(write_fd, old_audio, sizeof(old_audio));
    /* The one-shot audio signal announces the feed is flowing (pre-START). */
    assert(wait_for_count(&state.audio_status, 1, 1000));
    assert(ap2_session_start(session, 1700000000000ULL));
    read_exact(session, old_audio, sizeof(old_audio));
    assert(atomic_load(&state.audio_status) == 1);

    /* Bytes fed but never delivered before FLUSH: they sit in the ring and/or
     * the input pipe and must be discarded by the drain. */
    feed(write_fd, stale_audio, sizeof(stale_audio));

    uint64_t flush_started = ap2_io_monotonic_ms();
    assert(ap2_session_flush(session));
    assert(ap2_io_monotonic_ms() - flush_started < 1000);
    assert(atomic_load(&state.flushes) == 1);
    assert(atomic_load(&state.flushed_status) == 1);
    assert(ap2_session_state(session) == AP2_SESSION_IDLE);
    assert(ap2_session_epoch(session) == 1);   /* FLUSH does not bump the epoch */

    /* The post-flush track: START must re-anchor and deliver ONLY this audio,
     * proving the stale bytes were drained from both the ring and the input.
     * FLUSH re-armed the one-shot audio signal, so the new track's first bytes
     * announce themselves again. */
    feed(write_fd, new_audio, sizeof(new_audio));
    assert(wait_for_count(&state.audio_status, 2, 1000));
    assert(ap2_session_start(session, 1700000005000ULL));
    assert(atomic_load(&state.commits) == 2);
    assert(ap2_session_epoch(session) == 2);
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);
    read_exact(session, new_audio, sizeof(new_audio));

    ap2_session_destroy(session);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

static void test_flush_while_idle_before_start(void)
{
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 0, &write_fd);

    /* A FLUSH before the first START is valid: no commit yet, still idle. */
    assert(ap2_session_flush(session));
    assert(atomic_load(&state.flushes) == 1);
    assert(atomic_load(&state.flushed_status) == 1);
    assert(atomic_load(&state.commits) == 0);
    assert(ap2_session_state(session) == AP2_SESSION_IDLE);

    static const uint8_t audio[] = {0x01, 0x02, 0x03, 0x04};
    feed(write_fd, audio, sizeof(audio));
    assert(ap2_session_start(session, 0));
    read_exact(session, audio, sizeof(audio));

    ap2_session_destroy(session);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

static void test_standby_then_resume(void)
{
    static const uint8_t audio[] = {0x20, 0x21, 0x22, 0x23};
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 0, &write_fd);

    feed(write_fd, audio, sizeof(audio));
    assert(ap2_session_start(session, 1700000000000ULL));
    read_exact(session, audio, sizeof(audio));

    assert(ap2_session_standby(session));
    assert(atomic_load(&state.stops) == 1);
    assert(ap2_session_state(session) == AP2_SESSION_STANDBY);

    static const uint8_t more[] = {0x30, 0x31, 0x32};
    feed(write_fd, more, sizeof(more));
    assert(ap2_session_start(session, 1700000005000ULL));
    assert(atomic_load(&state.commits) == 2);
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);
    read_exact(session, more, sizeof(more));

    ap2_session_destroy(session);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

static void test_idle_timeout_ends_session(void)
{
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 20, &write_fd);

    usleep(60000);
    ap2_session_poll(session);
    assert(ap2_session_state(session) == AP2_SESSION_ENDED);
    assert(atomic_load(&state.idle_timeouts) == 1);
    /* A read on an ended session reports -2. */
    uint8_t byte;
    assert(ap2_session_read(session, &byte, 1, 50) == -2);

    ap2_session_destroy(session);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

/* The input pipe's writer stays open (as MA holds the CLI stdin open), so the
 * reader never sees EOF; destroy must still tear it down promptly. */
static void test_destroy_is_prompt_with_open_writer(void)
{
    test_state_t state;
    int write_fd;
    struct ap2_session_s *session = create_session(&state, 0, &write_fd);

    static const uint8_t audio[] = {0x40, 0x41};
    feed(write_fd, audio, sizeof(audio));
    assert(ap2_session_start(session, 1700000000000ULL));
    read_exact(session, audio, sizeof(audio));

    uint64_t started = ap2_io_monotonic_ms();
    ap2_session_destroy(session);
    assert(ap2_io_monotonic_ms() - started < 1000);
    status_state = NULL;
    assert(close(write_fd) == 0);
}

int main(void)
{
    test_start_streams_the_input();
    test_flush_drains_and_reanchors();
    test_flush_while_idle_before_start();
    test_standby_then_resume();
    test_idle_timeout_ends_session();
    test_destroy_is_prompt_with_open_writer();
    puts("AP2 session flush-and-refill tests passed");
    return 0;
}
