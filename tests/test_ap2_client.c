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

/* A standby then a warm seek (ap2cl_flush + ap2cl_resume) each send exactly one
 * RTSP FLUSH and reuse the same native session — no reconnect, no crypto reset. */
static void test_native_flush_resume_reuses_rtsp_session(void)
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
    /* Warm seek: discard the receiver buffer, then re-anchor the timeline. */
    assert(ap2cl_flush(client));
    assert(ap2cl_resume(client, 1700000005000ULL));
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
    test_native_flush_resume_reuses_rtsp_session();

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
