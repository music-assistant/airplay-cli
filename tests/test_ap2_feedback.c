#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ap2_client.h"
#include "ap2_io.h"
#include "cross_log.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

typedef struct {
    int fd;
    atomic_bool stop;
    atomic_bool failed;
    atomic_bool command_seen;
    atomic_int request_count;
    atomic_int feedback_count;
    int cseq[16];
} fake_peer_t;

static int read_request(int fd, char request[2048])
{
    size_t used = 0;
    uint64_t deadline = ap2_io_monotonic_ms() + 200;
    while (used < 2047) {
        ssize_t n = ap2_io_read_deadline(
            fd, (uint8_t *)request + used, 2047 - used, deadline);
        if (n <= 0) return errno == ETIMEDOUT ? 0 : -1;
        used += (size_t)n;
        request[used] = '\0';
        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end) {
            size_t header_len = (size_t)(header_end - request) + 4;
            int content_len = 0;
            char *content_length = strstr(request, "\r\nContent-Length: ");
            if (content_length &&
                sscanf(content_length + 18, "%d", &content_len) != 1)
                return -1;
            if (content_len < 0 ||
                header_len + (size_t)content_len >= 2048)
                return -1;
            if (used >= header_len + (size_t)content_len) return 1;
        }
    }
    return -1;
}

static bool answer_request(fake_peer_t *peer)
{
    char request[2048] = {0};
    int read_status = read_request(peer->fd, request);
    if (read_status <= 0) return read_status == 0;
    char *cseq_header = strstr(request, "\r\nCSeq: ");
    int cseq = -1;
    if (!cseq_header || sscanf(cseq_header + 8, "%d", &cseq) != 1)
        return false;

    int index = atomic_load(&peer->request_count);
    if (index >= (int)(sizeof(peer->cseq) / sizeof(peer->cseq[0])) ||
        (index > 0 && cseq != peer->cseq[index - 1] + 1))
        return false;
    peer->cseq[index] = cseq;
    bool feedback = strstr(request, "POST /feedback RTSP/1.0\r\n") != NULL;
    bool command = strstr(request, "POST /command RTSP/1.0\r\n") != NULL;
    if (!feedback && !command) return false;

    char response[128];
    int response_len = snprintf(
        response, sizeof(response),
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n",
        cseq);
    if (response_len <= 0 ||
        !ap2_io_write_all_deadline(
            peer->fd, (const uint8_t *)response, response_len,
            ap2_io_monotonic_ms() + 1000))
        return false;

    if (feedback) atomic_fetch_add(&peer->feedback_count, 1);
    if (command) atomic_store(&peer->command_seen, true);
    atomic_fetch_add(&peer->request_count, 1);
    return true;
}

static void *fake_peer_thread(void *arg)
{
    fake_peer_t *peer = arg;
    while (!atomic_load(&peer->stop)) {
        if (!answer_request(peer)) {
            atomic_store(&peer->failed, true);
            break;
        }
    }
    return NULL;
}

int main(void)
{
    uint8_t oversized_response[128];
    int oversized_len = snprintf(
        (char *)oversized_response, sizeof(oversized_response),
        "RTSP/1.0 200 OK\r\nCSeq: 7\r\n"
        "Content-Length: 2147483647\r\n\r\n");
    CHECK(oversized_len > 0);
    CHECK(ap2cl_test_parse_response(
              oversized_response, oversized_len, 7) == 0);

    int sockets[2];
    int event_sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, event_sockets) == 0);

    ap2_device_info_t device = {
        .name = "fake receiver",
        .hostname = "fake.local",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "",
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    char credentials[193];
    memset(credentials, 'A', sizeof(credentials) - 1);
    credentials[sizeof(credentials) - 1] = '\0';
    struct ap2cl_s *client = ap2cl_create(
        &device, &format, credentials, NULL, "1A2B3D4EA1B2C3D4",
        "123456789", 2000, 50);
    CHECK(client != NULL);
    uint8_t shared_secret[32];
    for (int i = 0; i < 32; i++) shared_secret[i] = (uint8_t)i;
    CHECK(ap2cl_test_attach_mrp(
        client, event_sockets[0], shared_secret));

    fake_peer_t peer = {.fd = sockets[1]};
    pthread_t peer_thread;
    CHECK(pthread_create(&peer_thread, NULL, fake_peer_thread, &peer) == 0);

    /* No command pipe exists in this test. Native session maintenance must
     * still issue feedback, while a concurrent metadata transaction receives
     * the next CSeq instead of interleaving with the keepalive. */
    CHECK(ap2cl_test_start_feedback_worker(client, sockets[0], 25));
    uint64_t play_started = ap2_io_monotonic_ms();
    ap2cl_play(client);
    CHECK(ap2_io_monotonic_ms() - play_started < 100);
    usleep(90000);
    CHECK(ap2cl_test_post_command(client) == 200);
    for (int i = 0; i < 100 && atomic_load(&peer.feedback_count) < 3; i++)
        usleep(20000);
    CHECK(atomic_load(&peer.feedback_count) >= 3);
    CHECK(atomic_load(&peer.command_seen));
    CHECK(ap2cl_control_healthy(client));

    ap2cl_test_stop_feedback_worker(client);
    atomic_store(&peer.stop, true);
    CHECK(pthread_join(peer_thread, NULL) == 0);
    CHECK(!atomic_load(&peer.failed));
    CHECK(atomic_load(&peer.request_count) >= 4);
    close(sockets[0]);
    close(sockets[1]);
    close(event_sockets[0]);
    close(event_sockets[1]);
    CHECK(ap2cl_destroy(client));
    puts("ap2 feedback worker test passed");
    return 0;
}
