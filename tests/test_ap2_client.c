#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ap2_bplist.h"
#include "ap2_client.h"
#include "ap2_io.h"
#include "ap2_session.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

void ap2cl_test_lock_mrp(struct ap2cl_s *p);
void ap2cl_test_unlock_mrp(struct ap2cl_s *p);
void ap2cl_test_attach_rtsp_socket(struct ap2cl_s *p, int fd);
void ap2cl_test_detach_rtsp_socket(struct ap2cl_s *p);

typedef struct {
    atomic_bool ready;
    atomic_bool primed;
    int commit_count;
    int stop_count;
    uint64_t start_unix_ms;
} session_test_state_t;

static session_test_state_t *session_status_state;

static bool session_test_commit(void *transport, uint64_t start_unix_ms)
{
    session_test_state_t *state = transport;
    state->commit_count++;
    state->start_unix_ms = start_unix_ms;
    return true;
}

static void session_test_quiesce(void *transport)
{
    (void)transport;
}

static void session_test_resume(void *transport)
{
    (void)transport;
}

static void session_test_stop(void *transport)
{
    session_test_state_t *state = transport;
    state->stop_count++;
}

static void session_test_status(const char *line)
{
    if (strstr(line, "[STATUS] ready generation=0"))
        atomic_store(&session_status_state->ready, true);
    if (strstr(line, "[STATUS] primed generation=0"))
        atomic_store(&session_status_state->primed, true);
}

static void test_generation_zero_requires_prepare_then_start(void)
{
    static const uint8_t audio[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    char path[] = "/tmp/cliairplay-session-test.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, audio, sizeof(audio)) == (ssize_t)sizeof(audio));
    assert(close(fd) == 0);

    session_test_state_t state = {0};
    atomic_init(&state.ready, false);
    atomic_init(&state.primed, false);
    session_status_state = &state;
    ap2_session_ops_t ops = {
        .quiesce = session_test_quiesce,
        .commit = session_test_commit,
        .resume = session_test_resume,
        .stop = session_test_stop,
        .status = session_test_status,
        .transport = &state,
    };
    struct ap2_session_s *session =
        ap2_session_create(&ops, 1000, 500, 20);
    assert(session);

    ap2_generation_t generation = {
        .number = 0,
        .audio_path = path,
        .position_ms = 0,
    };
    assert(ap2_session_prepare(session, &generation));
    for (int i = 0;
         i < 200 && (!atomic_load(&state.ready) ||
                     !atomic_load(&state.primed));
         i++)
        usleep(5000);
    assert(atomic_load(&state.ready));
    assert(atomic_load(&state.primed));
    assert(state.commit_count == 0);
    assert(ap2_session_state(session) == AP2_SESSION_PREPARING);

    uint8_t received[sizeof(audio)] = {0};
    assert(ap2_session_read(
               session, received, sizeof(received), 10) == 0);
    const uint64_t audible_start = 1770000000123ULL;
    assert(ap2_session_start(session, 0, audible_start));
    assert(state.commit_count == 1);
    assert(state.start_unix_ms == audible_start);
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);

    char staged_path[] = "/tmp/cliairplay-staged-fifo.XXXXXX";
    int staged_placeholder = mkstemp(staged_path);
    assert(staged_placeholder >= 0);
    assert(close(staged_placeholder) == 0);
    assert(unlink(staged_path) == 0);
    assert(mkfifo(staged_path, 0600) == 0);
    ap2_generation_t staged_generation = {
        .number = 1,
        .audio_path = staged_path,
    };
    assert(ap2_session_prepare(session, &staged_generation));
    assert(ap2_session_flush(session, 1));
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);
    assert(unlink(staged_path) == 0);

    assert(ap2_session_read(
               session, received, sizeof(received), 10) ==
           (int)sizeof(received));
    assert(memcmp(received, audio, sizeof(audio)) == 0);
    assert(ap2_session_read(
               session, received, sizeof(received), 100) == -1);
    assert(ap2_session_state(session) == AP2_SESSION_IDLE);
    usleep(30000);
    ap2_session_poll(session);
    assert(ap2_session_state(session) == AP2_SESSION_ENDED);

    ap2_session_destroy(session);
    session_status_state = NULL;
    assert(unlink(path) == 0);
}

static void test_idle_fifo_does_not_block_shutdown(void)
{
    char path[] = "/tmp/cliairplay-session-fifo.XXXXXX";
    int placeholder = mkstemp(path);
    assert(placeholder >= 0);
    assert(close(placeholder) == 0);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);

    session_test_state_t state = {0};
    atomic_init(&state.ready, false);
    atomic_init(&state.primed, false);
    session_status_state = &state;
    ap2_session_ops_t ops = {
        .quiesce = session_test_quiesce,
        .commit = session_test_commit,
        .resume = session_test_resume,
        .stop = session_test_stop,
        .status = session_test_status,
        .transport = &state,
    };
    struct ap2_session_s *session =
        ap2_session_create(&ops, 1000, 500, 0);
    assert(session);
    ap2_generation_t generation = {
        .number = 0,
        .audio_path = path,
    };
    assert(ap2_session_prepare(session, &generation));

    uint64_t started = ap2_io_monotonic_ms();
    assert(ap2_session_flush(session, 0));
    assert(ap2_io_monotonic_ms() - started < 1000);
    assert(ap2_session_state(session) == AP2_SESSION_IDLE);
    ap2_session_destroy(session);

    atomic_store(&state.ready, false);
    session = ap2_session_create(&ops, 1000, 500, 0);
    assert(session);
    assert(ap2_session_prepare(session, &generation));
    int writer = open(path, O_WRONLY);
    assert(writer >= 0);
    for (int i = 0; i < 200 && !atomic_load(&state.ready); i++)
        usleep(5000);
    assert(atomic_load(&state.ready));

    started = ap2_io_monotonic_ms();
    ap2_session_destroy(session);
    assert(ap2_io_monotonic_ms() - started < 1000);
    session_status_state = NULL;
    assert(close(writer) == 0);
    assert(unlink(path) == 0);
}

static void test_stdin_generation_uses_command_path(void)
{
    static const uint8_t audio[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    int input[2];
    assert(pipe(input) == 0);
    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    assert(close(input[0]) == 0);

    session_test_state_t state = {0};
    atomic_init(&state.ready, false);
    atomic_init(&state.primed, false);
    session_status_state = &state;
    ap2_session_ops_t ops = {
        .quiesce = session_test_quiesce,
        .commit = session_test_commit,
        .resume = session_test_resume,
        .stop = session_test_stop,
        .status = session_test_status,
        .transport = &state,
    };
    struct ap2_session_s *session =
        ap2_session_create(&ops, 1000, 500, 0);
    assert(session);
    ap2_generation_t generation = {
        .number = 0,
        .audio_path = "-",
    };
    assert(ap2_session_prepare(session, &generation));
    assert(write(input[1], audio, sizeof(audio)) == (ssize_t)sizeof(audio));
    assert(close(input[1]) == 0);
    for (int i = 0;
         i < 200 && (!atomic_load(&state.ready) ||
                     !atomic_load(&state.primed));
         i++)
        usleep(5000);
    assert(atomic_load(&state.ready));
    assert(atomic_load(&state.primed));
    assert(state.commit_count == 0);
    assert(ap2_session_start(session, 0, 1700000000000ULL));
    ap2_generation_t overlapping_stdin = {
        .number = 1,
        .audio_path = "-",
    };
    assert(!ap2_session_prepare(session, &overlapping_stdin));

    uint8_t received[sizeof(audio)] = {0};
    assert(ap2_session_read(
               session, received, sizeof(received), 100) ==
           (int)sizeof(received));
    assert(memcmp(received, audio, sizeof(audio)) == 0);
    ap2_session_destroy(session);
    session_status_state = NULL;

    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    assert(close(saved_stdin) == 0);
}

static void test_standby_keeps_session_reusable(void)
{
    static const uint8_t audio[] = {0x20, 0x21, 0x22, 0x23};
    char first_path[] = "/tmp/cliairplay-standby-first.XXXXXX";
    char second_path[] = "/tmp/cliairplay-standby-second.XXXXXX";
    int first_fd = mkstemp(first_path);
    int second_fd = mkstemp(second_path);
    assert(first_fd >= 0);
    assert(second_fd >= 0);
    assert(write(first_fd, audio, sizeof(audio)) == (ssize_t)sizeof(audio));
    assert(write(second_fd, audio, sizeof(audio)) == (ssize_t)sizeof(audio));
    assert(close(first_fd) == 0);
    assert(close(second_fd) == 0);

    session_test_state_t state = {0};
    atomic_init(&state.ready, false);
    atomic_init(&state.primed, false);
    session_status_state = &state;
    ap2_session_ops_t ops = {
        .quiesce = session_test_quiesce,
        .commit = session_test_commit,
        .resume = session_test_resume,
        .stop = session_test_stop,
        .status = session_test_status,
        .transport = &state,
    };
    struct ap2_session_s *session =
        ap2_session_create(&ops, 1000, 1, 0);
    assert(session);

    ap2_generation_t first = {
        .number = 0,
        .audio_path = first_path,
    };
    assert(ap2_session_prepare(session, &first));
    assert(ap2_session_start(session, 0, 1700000000000ULL));
    assert(ap2_session_standby(session));
    assert(state.stop_count == 1);
    assert(ap2_session_state(session) == AP2_SESSION_STANDBY);

    ap2_generation_t second = {
        .number = 1,
        .audio_path = second_path,
    };
    assert(ap2_session_prepare(session, &second));
    assert(ap2_session_start(session, 1, 1700000005000ULL));
    assert(state.commit_count == 2);
    assert(ap2_session_active_generation(session) == 1);
    assert(ap2_session_state(session) == AP2_SESSION_PLAYING);

    ap2_session_destroy(session);
    session_status_state = NULL;
    assert(unlink(first_path) == 0);
    assert(unlink(second_path) == 0);
}

typedef struct {
    int fd;
    int request_count;
    bool ok;
} rtsp_peer_t;

static void *run_rtsp_flush_peer(void *arg)
{
    rtsp_peer_t *peer = arg;
    peer->ok = true;
    for (int request = 0; request < 2; request++) {
        char buffer[2048] = {0};
        size_t fill = 0;
        while (!strstr(buffer, "\r\n\r\n")) {
            ssize_t n = read(peer->fd, buffer + fill,
                             sizeof(buffer) - fill - 1);
            if (n <= 0) {
                peer->ok = false;
                return NULL;
            }
            fill += (size_t)n;
            if (fill >= sizeof(buffer) - 1) {
                peer->ok = false;
                return NULL;
            }
        }
        int cseq = 0;
        char *cseq_header = strstr(buffer, "\r\nCSeq: ");
        if (strncmp(buffer, "FLUSH ", 6) != 0 || !cseq_header ||
            sscanf(cseq_header, "\r\nCSeq: %d", &cseq) != 1) {
            peer->ok = false;
            return NULL;
        }
        peer->request_count++;
        char response[96];
        int response_len = snprintf(
            response, sizeof(response),
            "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n",
            cseq);
        if (write(peer->fd, response, (size_t)response_len) != response_len) {
            peer->ok = false;
            return NULL;
        }
    }
    return NULL;
}

static void test_native_standby_keeps_rtsp_session_reusable(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    rtsp_peer_t peer = {
        .fd = sockets[1],
    };
    pthread_t peer_thread;
    assert(pthread_create(
               &peer_thread, NULL, run_rtsp_flush_peer, &peer) == 0);

    ap2_device_info_t device = {
        .name = "standby test",
        .address = "127.0.0.1",
        .port = 7000,
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    struct ap2cl_s *client = ap2cl_create(
        &device, &format, NULL, NULL, NULL, NULL, 2000, 100);
    assert(client);
    ap2cl_force_native(client);
    ap2cl_test_attach_rtsp_socket(client, sockets[0]);
    assert(ap2cl_start(client, 1700000000000ULL));

    ap2cl_standby(client);
    assert(ap2cl_state(client) == AP2_CONNECTED);
    assert(ap2cl_warm_flush(client, 1700000005000ULL));
    assert(ap2cl_state(client) == AP2_STREAMING);

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    assert(peer.request_count == 2);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
}

typedef struct {
    struct ap2cl_s *client;
    atomic_bool done;
    bool healthy;
} health_check_t;

static void *run_health_check(void *arg)
{
    health_check_t *check = arg;
    check->healthy = ap2cl_control_healthy(check->client);
    atomic_store(&check->done, true);
    return NULL;
}

/* Minimal /info-shaped bplists for the format-table readers.
 * { supportedAudioFormatsExtended: { audioStream: [18, 21] } } */
static const uint8_t INFO_EXTENDED[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,                       /* root dict */
    0x5F, 0x10, 0x1D,                       /* 29-char key */
    's','u','p','p','o','r','t','e','d','A','u','d','i','o','F','o','r',
    'm','a','t','s','E','x','t','e','n','d','e','d',
    0xD1, 0x03, 0x04,                       /* nested dict */
    0x5B, 'a','u','d','i','o','S','t','r','e','a','m',
    0xA2, 0x05, 0x06,                       /* array of two ints */
    0x10, 18,
    0x10, 21,
    0x08, 0x0B, 0x2B, 0x2E, 0x3A, 0x3D, 0x3F,   /* offset table */
    0, 0, 0, 0, 0, 0, 1, 1,                 /* trailer: ofs/ref sizes */
    0, 0, 0, 0, 0, 0, 0, 7,                 /* num objects */
    0, 0, 0, 0, 0, 0, 0, 0,                 /* top object */
    0, 0, 0, 0, 0, 0, 0, 65,                /* offset-table offset */
};

/* { supportedFormats: { bufferStream: 0x60000 } } */
static const uint8_t INFO_LEGACY[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,
    0x5F, 0x10, 0x10,
    's','u','p','p','o','r','t','e','d','F','o','r','m','a','t','s',
    0xD1, 0x03, 0x04,
    0x5C, 'b','u','f','f','e','r','S','t','r','e','a','m',
    0x12, 0x00, 0x06, 0x00, 0x00,           /* 4-byte int 0x60000 */
    0x08, 0x0B, 0x1E, 0x21, 0x2E,
    0, 0, 0, 0, 0, 0, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 5,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 51,
};

static void test_info_format_tables(void)
{
    uint64_t mask = 0;
    assert(ap2_bplist_find_dict_uint_array_mask(
               INFO_EXTENDED, sizeof(INFO_EXTENDED),
               "supportedAudioFormatsExtended", "audioStream", &mask) == 1);
    assert(mask == ((1ULL << 18) | (1ULL << 21)));
    assert(ap2_bplist_find_dict_uint_array_mask(
               INFO_EXTENDED, sizeof(INFO_EXTENDED),
               "supportedAudioFormatsExtended", "bufferStream", &mask) == 0);

    assert(ap2_bplist_find_dict_uint(
               INFO_LEGACY, sizeof(INFO_LEGACY),
               "supportedFormats", "bufferStream", &mask) == 1);
    assert(mask == 0x60000);
    assert(ap2_bplist_find_dict_uint(
               INFO_LEGACY, sizeof(INFO_LEGACY),
               "supportedFormats", "audioStream", &mask) == 0);
    /* The value is an int, not an array. */
    assert(ap2_bplist_find_dict_uint_array_mask(
               INFO_LEGACY, sizeof(INFO_LEGACY),
               "supportedFormats", "bufferStream", &mask) == 0);

    /* Truncated input must fail cleanly at every length, never crash. */
    for (size_t len = 0; len < sizeof(INFO_EXTENDED); len++) {
        assert(ap2_bplist_find_dict_uint_array_mask(
                   INFO_EXTENDED, len,
                   "supportedAudioFormatsExtended", "audioStream", &mask) == 0);
    }
    puts("ap2_bplist /info format-table tests passed");
}

int main(void)
{
    test_info_format_tables();
    test_generation_zero_requires_prepare_then_start();
    test_idle_fifo_does_not_block_shutdown();
    test_stdin_generation_uses_command_path();
    test_standby_keeps_session_reusable();
    test_native_standby_keeps_rtsp_session_reusable();

    ap2_device_info_t device = {
        .name = "test",
        .address = "127.0.0.1",
        .port = 7000,
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    struct ap2cl_s *client = ap2cl_create(
        &device, &format, NULL, NULL, NULL, NULL, 2000, 100);
    assert(client);
    ap2cl_force_native(client);

    health_check_t check = {
        .client = client,
    };
    atomic_init(&check.done, false);
    ap2cl_test_lock_mrp(client);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_health_check, &check) == 0);
    usleep(50000);
    assert(atomic_load(&check.done));
    assert(check.healthy);
    ap2cl_test_unlock_mrp(client);
    assert(pthread_join(thread, NULL) == 0);

    assert(ap2cl_destroy(client));
    puts("ap2_client health snapshot test passed");
    return 0;
}
