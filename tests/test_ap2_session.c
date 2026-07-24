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
#include <sys/stat.h>
#include <unistd.h>

#include "ap2_io.h"
#include "ap2_session.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define MAX_GENERATIONS 64

typedef struct {
    atomic_int ready[MAX_GENERATIONS];
    atomic_int primed[MAX_GENERATIONS];
    atomic_int eof[MAX_GENERATIONS];
    atomic_int commits;
    atomic_int quiesces;
    atomic_int resumes;
} test_state_t;

static test_state_t *status_state;
static atomic_int injected_poll_step;
static atomic_bool inject_no_writer_events;

int ap2_session_test_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (atomic_load(&inject_no_writer_events)) {
        int step = atomic_fetch_add(&injected_poll_step, 1);
        if (step == 0) {
            usleep((useconds_t)timeout * 1000);
            for (nfds_t i = 0; i < nfds; i++) fds[i].revents = 0;
            return 0;
        }
        if (step == 1) {
            for (nfds_t i = 0; i < nfds; i++) fds[i].revents = POLLHUP;
            atomic_store(&inject_no_writer_events, false);
            return 1;
        }
    }
    return poll(fds, nfds, timeout);
}

static void quiesce(void *transport)
{
    test_state_t *state = transport;
    atomic_fetch_add(&state->quiesces, 1);
}

static bool commit(void *transport, uint64_t start_unix_ms)
{
    test_state_t *state = transport;
    (void)start_unix_ms;
    atomic_fetch_add(&state->commits, 1);
    return true;
}

static void resume(void *transport)
{
    test_state_t *state = transport;
    atomic_fetch_add(&state->resumes, 1);
}

static void stop(void *transport)
{
    (void)transport;
}

static void status(const char *line)
{
    unsigned long long generation;
    if (sscanf(line, "[STATUS] ready generation=%llu", &generation) == 1 &&
        generation < MAX_GENERATIONS) {
        atomic_fetch_add(&status_state->ready[generation], 1);
    } else if (sscanf(line, "[STATUS] primed generation=%llu", &generation) ==
                   1 &&
               generation < MAX_GENERATIONS) {
        atomic_fetch_add(&status_state->primed[generation], 1);
    } else if (sscanf(line, "[STATUS] input_eof generation=%llu", &generation) ==
                   1 &&
               generation < MAX_GENERATIONS) {
        atomic_fetch_add(&status_state->eof[generation], 1);
    }
}

static struct ap2_session_s *create_session(test_state_t *state)
{
    for (size_t i = 0; i < MAX_GENERATIONS; i++) {
        atomic_init(&state->ready[i], 0);
        atomic_init(&state->primed[i], 0);
        atomic_init(&state->eof[i], 0);
    }
    atomic_init(&state->commits, 0);
    atomic_init(&state->quiesces, 0);
    atomic_init(&state->resumes, 0);
    status_state = state;
    ap2_session_ops_t ops = {
        .quiesce = quiesce,
        .commit = commit,
        .resume = resume,
        .stop = stop,
        .status = status,
        .transport = state,
    };
    return ap2_session_create(&ops, 1000, 1, 0);
}

static void make_fifo(char *path, size_t size, const char *name)
{
    int written = snprintf(path, size, "/tmp/%s.XXXXXX", name);
    assert(written > 0 && (size_t)written < size);
    int placeholder = mkstemp(path);
    assert(placeholder >= 0);
    assert(close(placeholder) == 0);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
}

static bool wait_for_count(atomic_int *value, int expected, int timeout_ms)
{
    uint64_t deadline = ap2_io_monotonic_ms() + (uint64_t)timeout_ms;
    while (atomic_load(value) < expected &&
           ap2_io_monotonic_ms() < deadline) {
        usleep(1000);
    }
    return atomic_load(value) >= expected;
}

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t size;
    int delay_ms;
    int open_errno;
    bool ok;
} writer_arg_t;

static void *delayed_writer(void *opaque)
{
    writer_arg_t *arg = opaque;
    usleep((useconds_t)arg->delay_ms * 1000);
    int fd = open(arg->path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        arg->open_errno = errno;
        return NULL;
    }
    arg->ok = write(fd, arg->data, arg->size) == (ssize_t)arg->size;
    if (close(fd) != 0) arg->ok = false;
    return NULL;
}

static void read_generation(struct ap2_session_s *session,
                            const uint8_t *expected, size_t size)
{
    uint8_t received[256];
    assert(size <= sizeof(received));
    size_t offset = 0;
    while (offset < size) {
        int n = ap2_session_read(session, received + offset,
                                 (int)(size - offset), 1000);
        assert(n > 0);
        offset += (size_t)n;
    }
    assert(memcmp(received, expected, size) == 0);
    assert(ap2_session_read(session, received, sizeof(received), 1000) == -1);
}

static void test_timeout_then_hup_keeps_fifo_reader(void)
{
    static const uint8_t audio[] = {0x10, 0x20, 0x30, 0x40};
    char path[128];
    make_fifo(path, sizeof(path), "cliairplay-fifo-race");

    test_state_t state;
    struct ap2_session_s *session = create_session(&state);
    assert(session);
    atomic_store(&injected_poll_step, 0);
    atomic_store(&inject_no_writer_events, true);

    ap2_generation_t generation = {
        .number = 0,
        .audio_path = path,
    };
    writer_arg_t writer = {
        .path = path,
        .data = audio,
        .size = sizeof(audio),
        .delay_ms = 1000,
    };
    pthread_t writer_thread;
    assert(pthread_create(&writer_thread, NULL, delayed_writer, &writer) == 0);

    uint64_t prepare_started = ap2_io_monotonic_ms();
    assert(ap2_session_prepare(session, &generation));
    assert(ap2_io_monotonic_ms() - prepare_started < 500);
    assert(wait_for_count(&state.ready[0], 1, 500));
    usleep(600000);
    assert(atomic_load(&state.ready[0]) == 1);
    assert(atomic_load(&state.eof[0]) == 0);

    assert(pthread_join(writer_thread, NULL) == 0);
    assert(writer.ok);
    assert(wait_for_count(&state.primed[0], 1, 1000));
    assert(wait_for_count(&state.eof[0], 1, 1000));
    assert(atomic_load(&state.ready[0]) == 1);
    assert(atomic_load(&state.primed[0]) == 1);
    assert(atomic_load(&state.eof[0]) == 1);

    assert(ap2_session_start(session, 0, 1700000000000ULL));
    read_generation(session, audio, sizeof(audio));
    assert(atomic_load(&state.quiesces) == 1);
    assert(atomic_load(&state.commits) == 1);
    assert(atomic_load(&state.resumes) == 1);

    ap2_session_destroy(session);
    status_state = NULL;
    assert(unlink(path) == 0);
}

static void test_repeated_delayed_fifo_generations(void)
{
    static const int delays_ms[] = {20, 300, 1000, 450, 30, 1100, 275, 800};
    test_state_t state;
    struct ap2_session_s *session = create_session(&state);
    assert(session);

    for (size_t i = 0; i < sizeof(delays_ms) / sizeof(delays_ms[0]); i++) {
        char path[128];
        make_fifo(path, sizeof(path), "cliairplay-fifo-stress");
        uint8_t audio[64];
        size_t audio_size = i == 0 ? 1 : sizeof(audio);
        memset(audio, (int)(i + 1), audio_size);
        ap2_generation_t generation = {
            .number = i,
            .audio_path = path,
            .position_ms = i * 1000,
        };
        writer_arg_t writer = {
            .path = path,
            .data = audio,
            .size = audio_size,
            .delay_ms = delays_ms[i],
        };
        pthread_t writer_thread;
        assert(pthread_create(
                   &writer_thread, NULL, delayed_writer, &writer) == 0);
        assert(ap2_session_prepare(session, &generation));
        assert(wait_for_count(&state.ready[i], 1, 200));
        assert(pthread_join(writer_thread, NULL) == 0);
        assert(writer.ok);
        assert(wait_for_count(&state.primed[i], 1, 1000));
        assert(wait_for_count(&state.eof[i], 1, 1000));
        assert(ap2_session_start(
            session, i, 1700000000000ULL + i * 5000));
        read_generation(session, audio, audio_size);
        assert(atomic_load(&state.ready[i]) == 1);
        assert(atomic_load(&state.primed[i]) == 1);
        assert(atomic_load(&state.eof[i]) == 1);
        assert(unlink(path) == 0);
    }

    assert(atomic_load(&state.commits) ==
           (int)(sizeof(delays_ms) / sizeof(delays_ms[0])));
    ap2_session_destroy(session);
    status_state = NULL;
}

static void test_empty_regular_input_and_writerless_abort(void)
{
    char empty_path[] = "/tmp/cliairplay-empty.XXXXXX";
    int empty_fd = mkstemp(empty_path);
    assert(empty_fd >= 0);
    assert(close(empty_fd) == 0);

    test_state_t state;
    struct ap2_session_s *session = create_session(&state);
    assert(session);
    ap2_generation_t empty = {
        .number = 0,
        .audio_path = empty_path,
    };
    assert(ap2_session_prepare(session, &empty));
    assert(wait_for_count(&state.ready[0], 1, 200));
    assert(wait_for_count(&state.eof[0], 1, 200));
    assert(ap2_session_start(session, 0, 1700000000000ULL));
    uint8_t byte;
    assert(ap2_session_read(session, &byte, 1, 200) == -1);

    char fifo_path[128];
    make_fifo(fifo_path, sizeof(fifo_path), "cliairplay-fifo-abort");
    ap2_generation_t waiting = {
        .number = 1,
        .audio_path = fifo_path,
    };
    assert(ap2_session_prepare(session, &waiting));
    assert(wait_for_count(&state.ready[1], 1, 200));
    uint64_t flush_started = ap2_io_monotonic_ms();
    assert(ap2_session_flush(session, 1));
    assert(ap2_io_monotonic_ms() - flush_started < 500);

    ap2_session_destroy(session);
    status_state = NULL;
    assert(unlink(fifo_path) == 0);
    assert(unlink(empty_path) == 0);
}

int main(void)
{
    test_timeout_then_hup_keeps_fifo_reader();
    test_repeated_delayed_fifo_generations();
    test_empty_regular_input_and_writerless_abort();
    puts("AP2 session FIFO lifecycle tests passed");
    return 0;
}
