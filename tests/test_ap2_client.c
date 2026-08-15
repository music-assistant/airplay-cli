#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "ap2_bplist.h"
#include "ap2_client.h"
#include "ap2_io.h"
#include "cross_log.h"

#include "mrp_jpeg_fixture.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

void ap2cl_test_set_mrp(struct ap2cl_s *p, struct ap2_mrp_ctx *m);
void ap2cl_test_lock_mrp(struct ap2cl_s *p);
void ap2cl_test_unlock_mrp(struct ap2cl_s *p);
void ap2cl_test_attach_rtsp_socket(struct ap2cl_s *p, int fd);
void ap2cl_test_prime_feedback_misses(struct ap2cl_s *p);
unsigned ap2cl_test_feedback_miss_budget(struct ap2cl_s *p);
void ap2cl_test_detach_rtsp_socket(struct ap2cl_s *p);
bool ap2cl_test_first_packet(struct ap2cl_s *p);
void ap2cl_test_set_first_packet(struct ap2cl_s *p, bool first_packet);
void ap2cl_test_set_splice(struct ap2cl_s *p, bool enable);
bool ap2cl_test_splice_default(const char *txt, const char *am);
void ap2cl_test_set_anchor_valid(struct ap2cl_s *p, bool valid);
bool ap2cl_test_anchor_valid(struct ap2cl_s *p);
uint64_t ap2cl_test_head_ts(struct ap2cl_s *p);
void ap2cl_test_set_head_ts(struct ap2cl_s *p, uint64_t head);
uint64_t ap2cl_test_timeline_reanchors(struct ap2cl_s *p);
uint32_t ap2cl_test_rtp_timestamp(struct ap2cl_s *p);
int ap2cl_test_start_rtx(struct ap2cl_s *p);
void ap2cl_test_stop_rtx(struct ap2cl_s *p);
void ap2cl_test_rtx_store(struct ap2cl_s *p, uint16_t seq,
                          const uint8_t *pkt, int len);
unsigned long long ap2cl_test_rtx_answered(struct ap2cl_s *p);
unsigned long long ap2cl_test_rtx_expired(struct ap2cl_s *p);
void ap2cl_test_set_use_ptp(struct ap2cl_s *p, bool enable);
void ap2cl_test_set_apple_model(struct ap2cl_s *p, bool apple);
bool ap2cl_test_apple_model_default(const char *txt, const char *am);
bool ap2cl_test_env_enabled(const char *name, bool unset_default);
bool ap2cl_test_mrp_type130_enabled(void);
void ap2cl_test_inject_clock_exchange(struct ap2cl_s *p, uint32_t count,
                                      uint64_t first_ms, uint64_t third_ms);
void ap2cl_test_clear_clock_exchange(struct ap2cl_s *p);
void ap2cl_test_set_session_state(struct ap2cl_s *p, ap2_state_t state);
void ap2cl_test_set_clock_marks(struct ap2cl_s *p, uint64_t connected_ms,
                                uint64_t last_streak_ms);
void ap2cl_test_bump_audio_sent(struct ap2cl_s *p);
void ap2cl_test_set_dev_latency_max(struct ap2cl_s *p, uint32_t frames);
uint64_t ap2cl_test_pacing_window_frames(struct ap2cl_s *p);
ap2_connect_error_t ap2cl_test_auth_error_kind(struct ap2cl_s *p);

typedef struct {
    int fd;
    int request_count;
    int expected_requests;
    int response_status;
    bool ok;
} rtsp_peer_t;

static void *run_rtsp_flush_peer(void *arg)
{
    rtsp_peer_t *peer = arg;
    peer->ok = true;
    for (int request = 0; request < peer->expected_requests; request++) {
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
        const char *reason = peer->response_status == 200 ? "OK" : "Error";
        int response_len = snprintf(
            response, sizeof(response),
            "RTSP/1.0 %d %s\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n",
            peer->response_status, reason, cseq);
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
        .expected_requests = 2,
        .response_status = 200,
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
    assert(ap2cl_start(client, 1700000000000ULL, NULL) == AP2_COMMIT_OK);
    /* The test bypasses native connect, so model a completed first send before
     * exercising the warm restart. */
    ap2cl_test_set_first_packet(client, false);

    ap2cl_standby(client);
    assert(ap2cl_state(client) == AP2_CONNECTED);
    /* Warm seek: discard the receiver buffer, then re-anchor the timeline. */
    assert(ap2cl_flush(client));
    assert(ap2cl_resume(client, 1700000005000ULL, NULL) == AP2_COMMIT_OK);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_test_first_packet(client));

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    assert(peer.request_count == 2);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
}

static void test_native_flush_rejects_receiver_error(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    rtsp_peer_t peer = {
        .fd = sockets[1],
        .expected_requests = 1,
        .response_status = 500,
    };
    pthread_t peer_thread;
    assert(pthread_create(
               &peer_thread, NULL, run_rtsp_flush_peer, &peer) == 0);

    ap2_device_info_t device = {
        .name = "flush failure test",
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
    assert(ap2cl_start(client, 1700000000000ULL, NULL) == AP2_COMMIT_OK);
    assert(!ap2cl_flush(client));

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    assert(peer.request_count == 1);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
}

static bool read_rtsp_request_cseq(int fd, const char *method, int *cseq)
{
    char buffer[2048] = {0};
    size_t fill = 0;
    while (!strstr(buffer, "\r\n\r\n")) {
        ssize_t n = read(fd, buffer + fill, sizeof(buffer) - fill - 1);
        if (n <= 0) return false;
        fill += (size_t)n;
        if (fill >= sizeof(buffer) - 1) return false;
    }
    char *cseq_header = strstr(buffer, "\r\nCSeq: ");
    if (strncmp(buffer, method, strlen(method)) != 0 || !cseq_header ||
        sscanf(cseq_header, "\r\nCSeq: %d", cseq) != 1)
        return false;
    return true;
}

typedef struct {
    int fd;
    bool ok;
} feedback_peer_t;

static void *run_feedback_miss_peer(void *arg)
{
    feedback_peer_t *peer = arg;
    peer->ok = false;
    /* Beat 1: swallow the request so the sender's read budget expires. */
    int missed_cseq = 0;
    if (!read_rtsp_request_cseq(peer->fd, "POST /feedback ", &missed_cseq))
        return NULL;
    /* Beat 2: answer the missed beat late, then the current one — the
     * sender must skip the stale response and accept the fresh one. */
    int current_cseq = 0;
    if (!read_rtsp_request_cseq(peer->fd, "POST /feedback ", &current_cseq) ||
        current_cseq != missed_cseq + 1)
        return NULL;
    char responses[192];
    int responses_len = snprintf(
        responses, sizeof(responses),
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n"
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n",
        missed_cseq, current_cseq);
    if (write(peer->fd, responses, (size_t)responses_len) != responses_len)
        return NULL;
    peer->ok = true;
    return NULL;
}

/* One missed keepalive beat is tolerated (the channel stays alive) and the
 * next successful beat — whose read must first skip the missed beat's late
 * response — resets the consecutive-miss counter. */
static void test_feedback_miss_tolerated_then_recovered(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    feedback_peer_t peer = {
        .fd = sockets[1],
    };
    pthread_t peer_thread;
    assert(pthread_create(
               &peer_thread, NULL, run_feedback_miss_peer, &peer) == 0);

    ap2_device_info_t device = {
        .name = "feedback test",
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

    /* The unanswered beat fails after its read budget but must not kill the
     * channel (single-strike behaviour would report unhealthy here). */
    assert(!ap2cl_feedback(client));
    assert(ap2cl_control_healthy(client));

    /* The next beat rides over the stale response and resets the counter:
     * were the miss still counted, two more misses would flip health, so a
     * succeeded beat proving health is the observable reset. */
    assert(ap2cl_feedback(client));
    assert(ap2cl_control_healthy(client));

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client feedback miss tolerance test passed");
}

static void *run_farewell_teardown_peer(void *arg)
{
    feedback_peer_t *peer = arg;
    peer->ok = false;
    /* Swallow the beat that spends the budget. */
    int missed_cseq = 0;
    if (!read_rtsp_request_cseq(peer->fd, "POST /feedback ", &missed_cseq))
        return NULL;
    /* The goodbye has to arrive on the channel the sender is abandoning, as
     * the next request in that channel's sequence. */
    int teardown_cseq = 0;
    if (!read_rtsp_request_cseq(peer->fd, "TEARDOWN rtsp://test/session ",
                                &teardown_cseq) ||
        teardown_cseq != missed_cseq + 1)
        return NULL;
    peer->ok = true;
    return NULL;
}

/* Spending the miss budget kills the channel, and the receiver must still be
 * told: without the farewell TEARDOWN it holds the session armed and pops
 * audibly on the starved queue until something else displaces it.
 *
 * The budget is primed rather than spent beat by beat, so the test costs one
 * feedback timeout instead of the whole ladder and does not mirror the budget
 * constant — a raised budget would otherwise leave the peer waiting for a
 * goodbye the sender never sends, hanging the suite instead of failing it. */
static void test_dead_channel_writes_farewell_teardown(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    feedback_peer_t peer = {
        .fd = sockets[1],
    };
    pthread_t peer_thread;
    assert(pthread_create(
               &peer_thread, NULL, run_farewell_teardown_peer, &peer) == 0);

    ap2_device_info_t device = {
        .name = "farewell test",
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
    ap2cl_test_prime_feedback_misses(client);

    assert(!ap2cl_feedback(client));
    assert(!ap2cl_control_healthy(client));

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client farewell teardown test passed");
}

static struct ap2cl_s *create_feedback_test_client(char *name)
{
    ap2_device_info_t device = {
        .name = name,
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
    return client;
}

/* The keepalive miss budget is fixed per session from the environment at
 * creation: a bare in-range count is taken, anything else keeps the default
 * so a typo cannot disable channel-death detection. */
static void test_feedback_miss_budget_from_env(void)
{
    assert(unsetenv("CLIAIRPLAY_FEEDBACK_MISSES") == 0);
    char name_default[] = "budget default";
    struct ap2cl_s *client = create_feedback_test_client(name_default);
    assert(ap2cl_test_feedback_miss_budget(client) == 3);
    assert(ap2cl_destroy(client));

    assert(setenv("CLIAIRPLAY_FEEDBACK_MISSES", "8", 1) == 0);
    char name_raised[] = "budget raised";
    client = create_feedback_test_client(name_raised);
    assert(ap2cl_test_feedback_miss_budget(client) == 8);
    assert(ap2cl_destroy(client));

    static const char *const rejected[] = { "0", "31", "abc", "8s", "" };
    char name_rejected[] = "budget rejected";
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        assert(setenv("CLIAIRPLAY_FEEDBACK_MISSES", rejected[i], 1) == 0);
        client = create_feedback_test_client(name_rejected);
        assert(ap2cl_test_feedback_miss_budget(client) == 3);
        assert(ap2cl_destroy(client));
    }
    assert(unsetenv("CLIAIRPLAY_FEEDBACK_MISSES") == 0);
    puts("ap2_client feedback miss budget env tests passed");
}

/* A budget of one restores single-strike behaviour: the very first missed
 * beat kills the channel (and still says goodbye), with nothing primed. */
static void test_feedback_miss_budget_of_one_kills_first_miss(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    feedback_peer_t peer = {
        .fd = sockets[1],
    };
    pthread_t peer_thread;
    assert(pthread_create(
               &peer_thread, NULL, run_farewell_teardown_peer, &peer) == 0);

    assert(setenv("CLIAIRPLAY_FEEDBACK_MISSES", "1", 1) == 0);
    char name_one[] = "budget of one";
    struct ap2cl_s *client = create_feedback_test_client(name_one);
    assert(unsetenv("CLIAIRPLAY_FEEDBACK_MISSES") == 0);
    assert(ap2cl_test_feedback_miss_budget(client) == 1);
    ap2cl_force_native(client);
    ap2cl_test_attach_rtsp_socket(client, sockets[0]);

    assert(!ap2cl_feedback(client));
    assert(!ap2cl_control_healthy(client));

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer.ok);
    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client feedback miss budget of one test passed");
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

/* Real /feedback response bodies, byte-for-byte as a receiver sends them:
 * { "streams": [] } — the receiver considers no stream active, the signature
 * of one that ACKs everything and renders nothing. */
static const uint8_t FEEDBACK_IDLE[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,                       /* root dict */
    0x57, 's','t','r','e','a','m','s',      /* 7-char key */
    0xA0,                                   /* empty array */
    0x08, 0x0B, 0x13,                       /* offset table */
    0, 0, 0, 0, 0, 0, 1, 1,                 /* trailer: ofs/ref sizes */
    0, 0, 0, 0, 0, 0, 0, 3,                 /* num objects */
    0, 0, 0, 0, 0, 0, 0, 0,                 /* top object */
    0, 0, 0, 0, 0, 0, 0, 0x14,              /* offset-table offset */
};

/* { "streams": [ { "type": 96 } ] } — one stream is live. */
static const uint8_t FEEDBACK_ACTIVE[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,
    0x57, 's','t','r','e','a','m','s',
    0xA1, 0x03,                             /* array of one */
    0xD1, 0x04, 0x05,                       /* stream dict */
    0x54, 't','y','p','e',
    0x10, 0x60,                             /* int 96 (realtime) */
    0x08, 0x0B, 0x13, 0x15, 0x18, 0x1D,
    0, 0, 0, 0, 0, 0, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 6,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x1F,
};

static void test_feedback_stream_counts(void)
{
    size_t streams = 99;
    assert(ap2_bplist_find_array_count(
               FEEDBACK_IDLE, sizeof(FEEDBACK_IDLE), "streams", &streams) == 1);
    assert(streams == 0);
    assert(ap2_bplist_find_array_count(
               FEEDBACK_ACTIVE, sizeof(FEEDBACK_ACTIVE), "streams",
               &streams) == 1);
    assert(streams == 1);

    /* An absent key is "the receiver reports no stream status", which must
     * stay distinct from an empty list; so must a non-array value. */
    assert(ap2_bplist_find_array_count(
               FEEDBACK_IDLE, sizeof(FEEDBACK_IDLE), "stream", &streams) == 0);
    assert(ap2_bplist_find_array_count(
               INFO_LEGACY, sizeof(INFO_LEGACY), "streams", &streams) == 0);
    assert(ap2_bplist_find_array_count(
               INFO_LEGACY, sizeof(INFO_LEGACY), "bufferStream", &streams) == 0);

    /* Truncated input must fail cleanly at every length, never crash. */
    for (size_t len = 0; len < sizeof(FEEDBACK_ACTIVE); len++) {
        assert(ap2_bplist_find_array_count(
                   FEEDBACK_ACTIVE, len, "streams", &streams) == 0);
    }
    puts("ap2_bplist /feedback stream-count tests passed");
}

/* A password-protected receiver keeps its native AirPlay 2 route: the password
 * doubles as the transient pairing secret. Without one it still falls back to
 * the RAOP-compatible flow, and every password-less decision is unchanged. */
static void test_route_with_password(void)
{
    /* features bit 48 (AirPlay 2 + pairing) => "0x0,0x10000" */
    const char *txt = "features=0x0,0x10000 flags=0x4";

    ap2_route_t pw_supplied = ap2_resolve_route(
        AP2_PROTO_AUTO, txt, "true", false, true, 16, false, false, false);
    assert(!pw_supplied.use_raop && pw_supplied.native && pw_supplied.transient);
    assert(strcmp(pw_supplied.reason, "native AP2, password, realtime") == 0);

    ap2_route_t pw_missing = ap2_resolve_route(
        AP2_PROTO_AUTO, txt, "true", false, false, 16, false, false, false);
    assert(!pw_missing.use_raop && !pw_missing.native);

    /* No password advertised: the plain transient route, password or not. */
    ap2_route_t no_flag = ap2_resolve_route(
        AP2_PROTO_AUTO, txt, NULL, false, false, 16, false, false, false);
    assert(no_flag.native && no_flag.transient);
    assert(strcmp(no_flag.reason, "native AP2, transient, realtime") == 0);

    /* A PIN or legacy-pairing flag still wins over a supplied password. */
    ap2_route_t pin_required = ap2_resolve_route(
        AP2_PROTO_AUTO, "features=0x0,0x10000 flags=0x8", "true", false, true,
        16, false, false, false);
    assert(!pin_required.native);

    /* Stored credentials keep pair-verify, whatever the password says. */
    ap2_route_t with_creds = ap2_resolve_route(
        AP2_PROTO_AUTO, txt, "true", true, true, 16, false, false, false);
    assert(with_creds.native && !with_creds.transient);
    assert(strcmp(with_creds.reason, "native AP2, pair-verify, realtime") == 0);

    /* A device that is not AirPlay 2 at all stays on legacy RAOP. */
    ap2_route_t legacy = ap2_resolve_route(
        AP2_PROTO_AUTO, "features=0x0,0x0", "true", false, true, 16, false,
        false, false);
    assert(legacy.use_raop);
    puts("ap2_client route password selection tests passed");
}

/* Buffered (type 103) auto-selects on a native PTP route when the receiver
 * advertises SupportsBufferedAudio (bit 40); --buffered forces it there, and
 * CLIAIRPLAY_BUFFERED overrides both ways. The deny-list ships empty — adding
 * an entry must consciously update the pins here. */
static void test_buffered_route_resolution(void)
{
    unsetenv("CLIAIRPLAY_BUFFERED");

    /* LinkPlay-class TXT: bits 40 (buffered) + 41 (PTP) + pairing. */
    const char *linkplay = "features=0x445F8A00,0x1C340";
    ap2_route_t r = ap2_resolve_route(AP2_PROTO_AUTO, linkplay, NULL, false,
                                      false, 16, false, false, false);
    assert(r.native && r.ptp);
    assert(ap2_buffered_route(&r, linkplay, NULL, false));

    /* The kill switch outranks auto and force alike; =1 forces it on. */
    setenv("CLIAIRPLAY_BUFFERED", "0", 1);
    assert(!ap2_buffered_route(&r, linkplay, NULL, false));
    assert(!ap2_buffered_route(&r, linkplay, NULL, true));
    setenv("CLIAIRPLAY_BUFFERED", "1", 1);
    const char *no_bit40 = "features=0x445F8A00,0x10200";
    ap2_route_t plain = ap2_resolve_route(AP2_PROTO_AUTO, no_bit40, NULL,
                                          false, false, 16, false, false,
                                          false);
    assert(plain.native && plain.ptp);
    assert(ap2_buffered_route(&plain, no_bit40, NULL, false));
    unsetenv("CLIAIRPLAY_BUFFERED");

    /* Without the bit, auto stays realtime; --buffered still forces. */
    assert(!ap2_buffered_route(&plain, no_bit40, NULL, false));
    assert(ap2_buffered_route(&plain, no_bit40, NULL, true));

    /* Bit 40 without PTP (bit 41) cannot anchor: never buffered. */
    const char *no_ptp = "features=0x445F8A00,0x10100";
    ap2_route_t unanchored = ap2_resolve_route(AP2_PROTO_AUTO, no_ptp, NULL,
                                               false, false, 16, false, false,
                                               false);
    assert(unanchored.native && !unanchored.ptp);
    assert(!ap2_buffered_route(&unanchored, no_ptp, NULL, true));

    /* A RAOP route carries no buffered stream, forced or not. */
    ap2_route_t raop = ap2_resolve_route(AP2_PROTO_AUTO, "features=0x0,0x0",
                                         NULL, false, false, 16, false, false,
                                         false);
    assert(raop.use_raop);
    assert(!ap2_buffered_route(&raop, "features=0x0,0x0", NULL, true));

    /* Fleet pins: third-party bit-40 classes auto-route buffered; Apple
     * models never do (hardware-measured: an Apple TV ACKs the type-103
     * SETUP but never probes our clock, so its anchor can never clear —
     * they keep the ear-validated realtime splice lane). --buffered still
     * forces them on for experiments. */
    const char *atv = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE";
    ap2_route_t atv_route = ap2_resolve_route(AP2_PROTO_AUTO, atv, NULL, true,
                                              false, 16, false, false, false);
    assert(!ap2_buffered_route(&atv_route, atv, NULL, false));
    assert(ap2_buffered_route(&atv_route, atv, NULL, true));
    assert(!ap2_buffered_route(&atv_route, NULL, "AudioAccessory5,1", false));
    const char *sonos = "model=Era 100 features=0x445F8A00,0x801C340";
    ap2_route_t sonos_route = ap2_resolve_route(AP2_PROTO_AUTO, sonos, NULL,
                                                false, false, 16, false,
                                                false, false);
    assert(ap2_buffered_route(&sonos_route, sonos, NULL, false));

    puts("ap2_client buffered route resolution tests passed");
}

/* A 401/403 is classified from what we actually presented, because the two
 * verdicts send the caller to different remedies: AUTH_REQUIRED becomes MA's
 * "password required" prompt, AUTH_FAILED tells the user to fix the secret.
 * Stored HAP credentials count as a presented secret — a receiver that rotated
 * them (tvOS re-pairing) rejects pair-verify, and prompting for a password the
 * device never had leaves the user with nothing to enter. */
static void test_auth_error_kind(void)
{
    ap2_device_info_t device = {
        .name = "auth kind test",
        .address = "127.0.0.1",
        .port = 7000,
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    /* Only credentials of the exact HAP length are stored by ap2cl_create. */
    char creds[192 + 1]; /* 192-hex HAP credential + NUL */
    memset(creds, 'a', sizeof(creds) - 1);
    creds[sizeof(creds) - 1] = '\0';

    struct ap2cl_s *bare = ap2cl_create(
        &device, &format, NULL, NULL, NULL, NULL, 2000, 100);
    assert(bare);
    assert(ap2cl_test_auth_error_kind(bare) == AP2_CONNECT_ERROR_AUTH_REQUIRED);
    assert(ap2cl_destroy(bare));

    /* An empty password is no password: it is stored but can never be
     * presented, so the "supply one" verdict still stands. */
    struct ap2cl_s *empty = ap2cl_create(
        &device, &format, NULL, "", NULL, NULL, 2000, 100);
    assert(empty);
    assert(ap2cl_test_auth_error_kind(empty) == AP2_CONNECT_ERROR_AUTH_REQUIRED);
    assert(ap2cl_destroy(empty));

    struct ap2cl_s *with_password = ap2cl_create(
        &device, &format, NULL, "device-password", NULL, NULL, 2000, 100);
    assert(with_password);
    assert(ap2cl_test_auth_error_kind(with_password) ==
           AP2_CONNECT_ERROR_AUTH_FAILED);
    assert(ap2cl_destroy(with_password));

    /* Credentials present, no password: revoked pairing, not a missing one. */
    struct ap2cl_s *with_creds = ap2cl_create(
        &device, &format, creds, NULL, NULL, NULL, 2000, 100);
    assert(with_creds);
    assert(ap2cl_test_auth_error_kind(with_creds) ==
           AP2_CONNECT_ERROR_AUTH_FAILED);
    assert(ap2cl_destroy(with_creds));

    struct ap2cl_s *both = ap2cl_create(
        &device, &format, creds, "device-password", NULL, NULL, 2000, 100);
    assert(both);
    assert(ap2cl_test_auth_error_kind(both) == AP2_CONNECT_ERROR_AUTH_FAILED);
    assert(ap2cl_destroy(both));

    puts("ap2_client auth error classification tests passed");
}

/* Both MediaRemote toggles parse their value, so the spellings a user reaches
 * for to switch something off do that from either default — the /command path
 * that ships on, and the type-130 channel that ships off. */
static void test_env_toggle_parsing(void)
{
    static const char *const off[] = { "0", "false", "off" };
    static const char *const on[] = { "1", "yes", "" };

    /* Unset keeps each toggle's own default: /command on, type-130 off. */
    assert(unsetenv("CLIAIRPLAY_MRP") == 0);
    assert(unsetenv("CLIAIRPLAY_MRP_TYPE130") == 0);
    assert(ap2cl_test_env_enabled("CLIAIRPLAY_MRP", true));
    assert(!ap2cl_test_mrp_type130_enabled());

    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        assert(setenv("CLIAIRPLAY_MRP", off[i], 1) == 0);
        assert(setenv("CLIAIRPLAY_MRP_TYPE130", off[i], 1) == 0);
        assert(!ap2cl_test_env_enabled("CLIAIRPLAY_MRP", true));
        assert(!ap2cl_test_mrp_type130_enabled());
    }
    /* The documented CLIAIRPLAY_MRP_TYPE130=1 spelling enables (DESIGN.md §8),
     * and no other value is read as off. */
    for (size_t i = 0; i < sizeof(on) / sizeof(on[0]); i++) {
        assert(setenv("CLIAIRPLAY_MRP", on[i], 1) == 0);
        assert(setenv("CLIAIRPLAY_MRP_TYPE130", on[i], 1) == 0);
        assert(ap2cl_test_env_enabled("CLIAIRPLAY_MRP", true));
        assert(ap2cl_test_mrp_type130_enabled());
    }
    assert(unsetenv("CLIAIRPLAY_MRP") == 0);
    assert(unsetenv("CLIAIRPLAY_MRP_TYPE130") == 0);
    puts("ap2_client env toggle parsing tests passed");
}

/* The splice timeline is the default for every native session; the deny-list
 * (classic flush + re-anchor fallback) ships empty. The 2026-07-31 fleet A/B
 * cleared the third-party park on the splice mechanism — adding a deny entry
 * must consciously update this pin. */
static void test_splice_default_resolution(void)
{
    /* Apple receivers (the mechanism is REQUIRED there). */
    assert(ap2cl_test_splice_default(
        "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE", NULL));
    assert(ap2cl_test_splice_default(NULL, "AudioAccessory5,1"));
    /* Third-party receivers stay on the default. */
    assert(ap2cl_test_splice_default(
        "model=Era 100 features=0x445F8A00,0x801C340", NULL));
    assert(ap2cl_test_splice_default(
        "model=WiiM Pro Receiver features=0x445F8A00,0x1C340", NULL));
    assert(ap2cl_test_splice_default(
        "model=HW-LS60D features=0xC05F8A00,0x1C340", NULL));
    assert(ap2cl_test_splice_default(NULL, "Era 100"));
    /* No TXT at all still defaults to the splice timeline. */
    assert(ap2cl_test_splice_default(NULL, NULL));
    puts("ap2_client splice default resolution tests passed");
}

/* The splice timeline never sends a flush verb and never rebases: the
 * warm boundary is a forward-only stamp skip on one immutable anchor line.
 * The RTSP socket must stay silent across flush + standby, and resume must
 * keep the sequence/timestamp relation contiguous. */
static void test_splice_timeline_warm_path(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);

    ap2_device_info_t device = {
        .name = "splice test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE",
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
    ap2cl_test_set_splice(client, true);
    assert(ap2cl_start(client, 0, NULL) == AP2_COMMIT_OK);
    ap2cl_test_set_first_packet(client, false);
    ap2cl_test_set_anchor_valid(client, true);

    uint64_t head_before = ap2cl_test_head_ts(client);
    uint32_t rtp_before = ap2cl_test_rtp_timestamp(client);

    /* Warm seek: flush keeps the receiver queue and the anchor line... */
    assert(ap2cl_flush(client));
    assert(ap2cl_test_anchor_valid(client));
    /* ...and resume never touches the stamps (a jump is an audible noise
     * burst): the gap to a future commanded instant becomes silence padding
     * the audio loop sends as ordinary contiguous chunks. */
    uint64_t start_ms = ((uint64_t)time(NULL) + 3) * 1000ULL;
    assert(ap2cl_resume(client, start_ms, NULL) == AP2_COMMIT_OK);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_test_anchor_valid(client));
    assert(!ap2cl_test_first_packet(client));
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);
    /* ~3 s to the commanded instant at 44.1 kHz, minus the queued head. */
    uint32_t pad = ap2cl_splice_pad_frames(client);
    assert(pad > 44100 && pad < 4 * 44100);
    ap2cl_splice_pad_consume(client, pad);
    assert(ap2cl_splice_pad_frames(client) == 0);

    /* Park: no flush verb either, and the client stays ARMED — a queue
     * underrun while the session stays armed is itself an audible noise
     * trigger on Apple receivers (the pop at a group pause press, which
     * parks members through standby), so the audio loop must keep feeding
     * the line silence; only the published playback state stops. */
    ap2cl_standby(client);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_splice_hot(client));
    assert(ap2cl_test_anchor_valid(client));
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);
    /* The resume splices on the hot line: stamps continue, no re-anchor. */
    assert(ap2cl_resume(client, 0, NULL) == AP2_COMMIT_OK);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(!ap2cl_test_first_packet(client));
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);

    /* The whole warm path put nothing on the RTSP socket. */
    char scratch[16];
    assert(recv(sockets[1], scratch, sizeof(scratch), 0) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
}

/* A delivery stall longer than the pacing depth (process freeze, network
 * dropout) leaves the head behind the wall clock with input still queued.
 * The guard must splice-pad the timeline forward — silence on the immutable
 * line, no stamp jump, no RTSP verb — and report the shift as a REANCHOR,
 * instead of letting the loop burst real frames on past timestamps (an
 * audible noise trigger on Apple receivers). A healthy head, a pending pad
 * and a non-splice stream must each leave it silent; a parked (standby)
 * window never consults it — the audio loop gates the guard on the session
 * engine's PLAYING state while its keepalive keeps the armed line fed. */
static void test_splice_delivery_gap_recovery(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);

    ap2_device_info_t device = {
        .name = "delivery gap test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE",
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
    ap2cl_test_set_splice(client, true);
    assert(ap2cl_start(client, 0, NULL) == AP2_COMMIT_OK);
    ap2cl_test_set_first_packet(client, false);
    ap2cl_test_set_anchor_valid(client, true);

    /* Fresh start: the head sits just under now + the feasibility floor by
     * construction. The guard runs every send iteration, so it must read
     * that as healthy, not as a lapse. */
    assert(!ap2cl_recover_delivery_gap(client));
    assert(ap2cl_splice_pad_frames(client) == 0);
    assert(ap2cl_test_timeline_reanchors(client) == 0);

    /* Stall: drag the head 2 s back (a freeze longer than the 600 ms pacing
     * depth leaves it ~1.75 s behind the wall clock), then capture stderr
     * around the guard to pin the machine-readable REANCHOR line. */
    uint64_t lapsed = ap2cl_test_head_ts(client) - 2 * 44100;
    ap2cl_test_set_head_ts(client, lapsed);
    uint32_t rtp_before = ap2cl_test_rtp_timestamp(client);
    fflush(stderr);
    int saved_stderr = dup(2);
    int capture[2];
    assert(saved_stderr >= 0 && pipe(capture) == 0);
    assert(dup2(capture[1], 2) == 2);
    assert(close(capture[1]) == 0);
    bool padded = ap2cl_recover_delivery_gap(client);
    fflush(stderr);
    assert(dup2(saved_stderr, 2) == 2);
    assert(close(saved_stderr) == 0);
    char captured[512] = {0};
    ssize_t captured_len =
        read(capture[0], captured, sizeof(captured) - 1);
    assert(close(capture[0]) == 0);

    assert(padded);
    /* ~1.75 s of lapse plus the 600 ms recovery lead. The wall clock keeps
     * advancing between start and guard, so the upper bound is a loose
     * sanity rail (a doubled shift would be ~207k frames — and the refire
     * check below pins stacking exactly); the lower bound is the
     * deterministic minimum. */
    uint32_t pad = ap2cl_splice_pad_frames(client);
    assert(pad > 103000 && pad < 176400);
    assert(ap2cl_test_timeline_reanchors(client) == 1);
    /* Bitstream continuity: the pad is queued, the stamps never move and the
     * anchor line stays frozen. */
    assert(ap2cl_test_head_ts(client) == lapsed);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);
    assert(ap2cl_test_anchor_valid(client));
    assert(captured_len > 0);
    assert(strstr(captured, "[STATUS] REANCHOR shifted_frames="));
    assert(strstr(captured, "sample_rate=44100"));

    /* While the queued pad drains, the timeline is already recovered: an
     * immediate re-check must not stack a second shift on top. */
    assert(!ap2cl_recover_delivery_gap(client));
    assert(ap2cl_splice_pad_frames(client) == pad);
    assert(ap2cl_test_timeline_reanchors(client) == 1);

    /* Pad fully sent (the head advances as the loop delivers it): healthy. */
    ap2cl_test_set_head_ts(client, lapsed + pad);
    ap2cl_splice_pad_consume(client, pad);
    assert(!ap2cl_recover_delivery_gap(client));
    assert(ap2cl_test_timeline_reanchors(client) == 1);

    /* The stock (deny-listed) timeline keeps today's delivery behavior. */
    ap2cl_test_set_head_ts(client, lapsed);
    ap2cl_test_set_splice(client, false);
    assert(!ap2cl_recover_delivery_gap(client));
    assert(ap2cl_splice_pad_frames(client) == 0);
    ap2cl_test_set_splice(client, true);

    /* A standby park keeps the session armed so the audio loop can keep the
     * line fed with silence — an underrun while armed is itself an audible
     * noise trigger. The guard is never consulted while parked (the loop
     * gates it on the session engine's PLAYING state). */
    ap2cl_standby(client);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_splice_hot(client));
    assert(ap2cl_test_timeline_reanchors(client) == 1);

    /* The whole recovery put nothing on the RTSP socket. */
    char scratch[16];
    assert(recv(sockets[1], scratch, sizeof(scratch), 0) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client splice delivery-gap recovery tests passed");
}

/* A splice pause stops the content, never the wire: the client must stay in
 * AP2_STREAMING with the line hot (the audio loop keeps feeding silence), so
 * a receiver whose queue would otherwise underrun while the session stays
 * armed never pops (the pause-press burst, ear-measured on an Apple TV 4K).
 * The un-pause splices back in on the same line — no stamp move, no
 * re-anchor. The stock timeline keeps its parked pause. */
static void test_splice_pause_keeps_line_hot(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);

    ap2_device_info_t device = {
        .name = "pause keepalive test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE",
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
    ap2cl_test_set_splice(client, true);
    assert(ap2cl_start(client, 0, NULL) == AP2_COMMIT_OK);
    ap2cl_test_set_first_packet(client, false);
    ap2cl_test_set_anchor_valid(client, true);
    uint64_t head_before = ap2cl_test_head_ts(client);
    uint32_t rtp_before = ap2cl_test_rtp_timestamp(client);

    ap2cl_pause(client);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_is_playing(client));
    assert(ap2cl_splice_hot(client));
    assert(ap2cl_test_anchor_valid(client));
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);
    assert(ap2cl_splice_pad_frames(client) == 0);

    ap2cl_play(client);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(!ap2cl_test_first_packet(client));
    assert(ap2cl_test_anchor_valid(client));
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_before);
    /* A hot un-pause may owe silence up to the minimum lead, never more —
     * and never moves a stamp. */
    assert(ap2cl_splice_pad_frames(client) < 22050);

    /* The stock (deny-listed) timeline keeps the parked pause. */
    ap2cl_test_set_splice(client, false);
    ap2cl_pause(client);
    assert(ap2cl_state(client) == AP2_PAUSED);
    assert(!ap2cl_is_playing(client));
    assert(!ap2cl_test_anchor_valid(client));

    /* Neither pause shape touches the RTSP socket. */
    char scratch[16];
    assert(recv(sockets[1], scratch, sizeof(scratch), 0) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client splice pause keepalive tests passed");
}

/* The stock (non-splice) pause/resume pair. No receiver reaches this path
 * today — ap2_splice_denied's prefix list is empty, so every native session
 * takes the splice timeline — so this pins the behavior for work that would
 * route a deep-queue receiver onto it. */
static void test_stock_pause_resume_reanchors(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);

    ap2_device_info_t device = {
        .name = "stock pause test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE",
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
    ap2cl_test_set_splice(client, false);
    assert(ap2cl_start(client, 0, NULL) == AP2_COMMIT_OK);
    ap2cl_test_set_first_packet(client, false);
    ap2cl_test_set_anchor_valid(client, true);

    /* A pause the receiver's queue outlasts: the head is still ahead of the
     * wall clock, so the buffered audio covers the whole gap and delivery
     * resumes on the frozen line. Moving it here would jump the stamps under
     * audio the receiver is still rendering. */
    uint64_t head_hot = ap2cl_test_head_ts(client);
    uint32_t rtp_hot = ap2cl_test_rtp_timestamp(client);
    ap2cl_pause(client);
    assert(ap2cl_state(client) == AP2_PAUSED);
    assert(!ap2cl_is_playing(client));
    assert(!ap2cl_test_anchor_valid(client));
    assert(ap2cl_test_head_ts(client) == head_hot);

    ap2cl_play(client);
    assert(ap2cl_state(client) == AP2_STREAMING);
    assert(ap2cl_is_playing(client));
    assert(ap2cl_test_head_ts(client) == head_hot);
    assert(ap2cl_test_rtp_timestamp(client) == rtp_hot);
    assert(!ap2cl_test_first_packet(client));

    /* A pause long enough to drain the receiver: the head lapses behind the
     * wall clock. Resuming on it would hand the pending content elapsed
     * timestamps — the pacing gate passes them (it only holds frames that are
     * too EARLY) and neither recovery helper covers this path, so the
     * un-pause has to re-anchor. Drag the head 5 s back to stand in for the
     * pause span. */
    ap2cl_pause(client);
    assert(ap2cl_state(client) == AP2_PAUSED);
    uint64_t lapsed = ap2cl_test_head_ts(client) - 5 * 44100;
    ap2cl_test_set_head_ts(client, lapsed);
    uint32_t rtp_lapsed = ap2cl_test_rtp_timestamp(client);
    uint64_t reanchors_before = ap2cl_test_timeline_reanchors(client);

    ap2cl_play(client);
    assert(ap2cl_state(client) == AP2_STREAMING);

    /* The line moved forward by the lapse plus the minimum lead. The wall
     * clock advances between the two calls, so the bounds are sanity rails
     * around the injected 5 s; the stamp delta below is exact. */
    uint64_t head_after = ap2cl_test_head_ts(client);
    uint64_t shift = head_after - lapsed;
    assert(shift > 4 * 44100 && shift < 7 * 44100);
    /* The wire timestamp tracks the head 1:1 (both carry the same per-process
     * offset), so the receiver re-seats on one consistent line. */
    assert(ap2cl_test_rtp_timestamp(client) - rtp_lapsed == (uint32_t)shift);
    /* The next packet carries the RTP marker and a fresh sync, and the anchor
     * is re-derived from the new start. */
    assert(ap2cl_test_first_packet(client));
    assert(!ap2cl_test_anchor_valid(client));
    /* A commanded un-pause is not drift: it must not land in the REANCHOR
     * accounting Music Assistant tracks. */
    assert(ap2cl_test_timeline_reanchors(client) == reanchors_before);
    /* The stock path owes no splice pad. */
    assert(ap2cl_splice_pad_frames(client) == 0);

    /* The whole pair put nothing on the RTSP socket. */
    char scratch[16];
    assert(recv(sockets[1], scratch, sizeof(scratch), 0) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    puts("ap2_client stock pause/resume re-anchor tests passed");
}

static bool bytes_contain(const uint8_t *data, size_t data_len,
                          const void *needle, size_t needle_len)
{
    if (!data || !needle || needle_len == 0 || needle_len > data_len)
        return false;
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static bool bytes_contain_string(const uint8_t *data, size_t data_len,
                                 const char *needle)
{
    return bytes_contain(data, data_len, needle, strlen(needle));
}

/* Whether a serialized bplist carries a real with value 0.0 (marker 0x23 and
 * eight zero bytes). The 32-byte trailer is excluded: a small top-object
 * reference there is the one place the pattern could occur by coincidence. */
static bool bplist_contains_zero_real(const uint8_t *data, size_t len)
{
    static const uint8_t zero_real[9] = {0x23, 0, 0, 0, 0, 0, 0, 0, 0};
    if (len <= 32) return false;
    return bytes_contain(data, len - 32, zero_real, sizeof(zero_real));
}

/* Locate the ArtworkIdentifier value in a now-playing body: the only
 * 16-character ASCII string object (marker 0x5F 0x10 0x10, 16 hex chars). */
static bool find_artwork_identifier(const uint8_t *data, size_t len,
                                    char out[17])
{
    for (size_t i = 0; i + 19 <= len; i++) {
        if (data[i] != 0x5F || data[i + 1] != 0x10 || data[i + 2] != 0x10)
            continue;
        bool hex = true;
        for (size_t j = 0; j < 16 && hex; j++) {
            uint8_t c = data[i + 3 + j];
            hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (!hex) continue;
        memcpy(out, data + i + 3, 16);
        out[16] = '\0';
        return true;
    }
    return false;
}

#define COMMAND_PEER_MAX_REQUESTS 24
#define COMMAND_PEER_BODY_MAX 8192

typedef struct {
    char method[16];
    char uri[64];
    uint8_t body[COMMAND_PEER_BODY_MAX];
    int body_len;
} command_peer_request_t;

typedef struct {
    int fd;
    int expected_requests;
    int request_count;
    command_peer_request_t requests[COMMAND_PEER_MAX_REQUESTS];
    bool ok;
} command_peer_t;

/* Receiver end for the MediaRemote publication tests: answer every RTSP
 * request with 200 while capturing method, URI and body for inspection. */
static void *run_command_peer(void *arg)
{
    command_peer_t *peer = arg;
    peer->ok = true;
    /* A client that regresses into sending fewer requests must fail the
     * suite, not hang it in read()/pthread_join. */
    struct timeval rcv_timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(peer->fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout,
               sizeof(rcv_timeout));
    for (int i = 0; i < peer->expected_requests; i++) {
        uint8_t buffer[COMMAND_PEER_BODY_MAX + 2048];
        size_t fill = 0;
        char *body_start = NULL;
        while (!body_start) {
            if (fill >= sizeof(buffer) - 1) {
                peer->ok = false;
                return NULL;
            }
            ssize_t n = read(peer->fd, buffer + fill,
                             sizeof(buffer) - 1 - fill);
            if (n <= 0) {
                peer->ok = false;
                return NULL;
            }
            fill += (size_t)n;
            buffer[fill] = '\0';
            body_start = strstr((char *)buffer, "\r\n\r\n");
        }
        body_start += 4;
        size_t header_len = (size_t)((uint8_t *)body_start - buffer);
        int cseq = 0;
        long content_length = 0;
        char *cseq_header = strstr((char *)buffer, "\r\nCSeq: ");
        char *length_header = strstr((char *)buffer, "\r\nContent-Length: ");
        if (!cseq_header || sscanf(cseq_header, "\r\nCSeq: %d", &cseq) != 1) {
            peer->ok = false;
            return NULL;
        }
        if (length_header)
            content_length = strtol(length_header + 18, NULL, 10);
        if (content_length < 0 || content_length > COMMAND_PEER_BODY_MAX ||
            header_len + (size_t)content_length > sizeof(buffer) - 1) {
            peer->ok = false;
            return NULL;
        }
        while (fill < header_len + (size_t)content_length) {
            ssize_t n = read(peer->fd, buffer + fill,
                             header_len + (size_t)content_length - fill);
            if (n <= 0) {
                peer->ok = false;
                return NULL;
            }
            fill += (size_t)n;
        }
        command_peer_request_t *req = &peer->requests[peer->request_count];
        if (sscanf((char *)buffer, "%15s %63s", req->method, req->uri) != 2) {
            peer->ok = false;
            return NULL;
        }
        memcpy(req->body, buffer + header_len, (size_t)content_length);
        req->body_len = (int)content_length;
        peer->request_count++;
        char response[96];
        int response_len = snprintf(
            response, sizeof(response),
            "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n", cseq);
        if (write(peer->fd, response, (size_t)response_len) != response_len) {
            peer->ok = false;
            return NULL;
        }
    }
    return NULL;
}

static bool request_has(const command_peer_request_t *req, const char *needle)
{
    return bytes_contain_string(req->body, (size_t)req->body_len, needle);
}

/* MediaRemote publication over a scripted RTSP peer: the track bundle reaches
 * the receiver as ONE now-playing replace push carrying the artwork; pause
 * and resume each push a timeline (mergePolicy update) body BEFORE their
 * updateMRPlaybackState with no duplicate state post; stop stays state-only;
 * a progress set while content-paused reports rate 0; and a track change with
 * byte-identical artwork keeps the ArtworkIdentifier while still re-sending
 * the DMAP copy for receivers that consume only that path. */
static void test_mrp_bundle_and_pause_publish(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    command_peer_t *peer = calloc(1, sizeof(*peer));
    assert(peer);
    peer->fd = sockets[1];
    peer->expected_requests = 17;
    pthread_t peer_thread;
    assert(pthread_create(&peer_thread, NULL, run_command_peer, peer) == 0);

    ap2_device_info_t device = {
        .name = "mrp bundle test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE",
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
    ap2cl_test_set_splice(client, true);
    assert(ap2cl_start(client, 0, NULL) == AP2_COMMIT_OK);
    ap2cl_test_set_first_packet(client, false);
    ap2cl_test_set_anchor_valid(client, true);

    /* The test bypasses pairing, so hand the client a ready MRP context; the
     * client owns it from here (ap2cl_destroy frees it). */
    struct ap2_mrp_ctx *mrp = ap2_mrp_create(
        "127.0.0.1", 7000, NULL, "0011223344556677", "bundle sender",
        "11111111-1111-1111-1111-111111111111",
        "22222222-2222-2222-2222-222222222222", NULL);
    assert(mrp);
    ap2cl_test_set_mrp(client, mrp);

    /* Track bundle: DMAP metadata, DMAP artwork, then registration and ONE
     * now-playing replace push chained with the extended registration
     * (requests 1-7). */
    ap2_mrp_artwork_info_t info;
    ap2_mrp_push_result_t push;
    bool track_changed = false;
    assert(ap2cl_set_metadata_ex(client, "Track One", "Artist", "Album", 180,
                                 "item-1", "image/jpeg", k_baseline_jpeg,
                                 (int)sizeof(k_baseline_jpeg), &track_changed,
                                 &info, &push));
    assert(track_changed);
    assert(info.result == AP2_MRP_ARTWORK_ACCEPTED);
    assert(push.overall_status == 200 && push.nowplaying_status == 200);

    /* A progress set while playing stages state without wire traffic. */
    assert(ap2cl_set_progress(client, 42, 180));

    /* Splice pause: timeline push then playback state (requests 8-9). */
    ap2cl_pause(client);

    /* A progress set while content-paused must report rate 0: the splice
     * pause keeps the client AP2_STREAMING, so the content flags own the
     * published rate. */
    assert(ap2cl_set_progress(client, 42, 180));
    uint8_t *paused_progress = NULL;
    int paused_progress_len = 0;
    ap2cl_test_lock_mrp(client);
    assert(ap2_mrp_build_nowplaying_progress_command(
        mrp, &paused_progress, &paused_progress_len));
    ap2cl_test_unlock_mrp(client);
    assert(bplist_contains_zero_real(paused_progress,
                                     (size_t)paused_progress_len));
    free(paused_progress);

    /* Resume: timeline push then playback state (requests 10-11). A REDUNDANT
     * play (state already Playing) must still post the forced state re-assert
     * after its timeline push (requests 12-13) — the chained send dedupes on
     * an unchanged state, so only the kept force reaches the wire. Then stop:
     * playback state only (request 14). */
    ap2cl_play(client);
    ap2cl_play(client);
    ap2cl_stop(client);

    /* Next track with byte-identical artwork: one more bundle (15-17). */
    assert(ap2cl_set_metadata_ex(client, "Track Two", "Artist", "Album", 200,
                                 "item-2", "image/jpeg", k_baseline_jpeg,
                                 (int)sizeof(k_baseline_jpeg), &track_changed,
                                 &info, &push));
    assert(track_changed);
    assert(info.result == AP2_MRP_ARTWORK_UNCHANGED);
    assert(push.overall_status == 200 && push.nowplaying_status == 200);

    assert(pthread_join(peer_thread, NULL) == 0);
    assert(peer->ok);
    assert(peer->request_count == 17);

    /* Bundle 1: DMAP metadata + artwork ride SET_PARAMETER first. */
    assert(strcmp(peer->requests[0].method, "SET_PARAMETER") == 0);
    assert(request_has(&peer->requests[0], "mlit"));
    assert(strcmp(peer->requests[1].method, "SET_PARAMETER") == 0);
    assert(bytes_contain(peer->requests[1].body,
                         (size_t)peer->requests[1].body_len,
                         k_baseline_jpeg, sizeof(k_baseline_jpeg)));
    /* Registration, then the single replace push carrying the artwork. */
    assert(strcmp(peer->requests[2].method, "POST") == 0);
    assert(strcmp(peer->requests[2].uri, "/command") == 0);
    assert(request_has(&peer->requests[3], "updateMRNowPlayingInfo"));
    assert(request_has(&peer->requests[3], "replace"));
    assert(request_has(&peer->requests[3],
                       "kMRMediaRemoteNowPlayingInfoArtworkData"));
    assert(bytes_contain(peer->requests[3].body,
                         (size_t)peer->requests[3].body_len,
                         k_baseline_jpeg, sizeof(k_baseline_jpeg)));
    assert(request_has(&peer->requests[4], "updateMRSupportedCommands"));
    assert(request_has(&peer->requests[5], "updateMRPlaybackState"));
    assert(request_has(&peer->requests[6], "updateMRNowPlayingClient"));

    /* Pause: a lean timeline body (no replace, no artwork keys, rate 0)
     * BEFORE updateMRPlaybackState, and exactly one state post. */
    assert(request_has(&peer->requests[7], "updateMRNowPlayingInfo"));
    assert(!request_has(&peer->requests[7], "replace"));
    assert(!request_has(&peer->requests[7],
                        "kMRMediaRemoteNowPlayingInfoArtwork"));
    assert(request_has(&peer->requests[7],
                       "kMRMediaRemoteNowPlayingInfoElapsedTime"));
    assert(bplist_contains_zero_real(peer->requests[7].body,
                                     (size_t)peer->requests[7].body_len));
    assert(request_has(&peer->requests[8], "updateMRPlaybackState"));
    assert(!request_has(&peer->requests[8], "updateMRNowPlayingInfo"));

    /* Resume: the same shape at rate 1 (elapsed is 42.x, so no zero real). */
    assert(request_has(&peer->requests[9], "updateMRNowPlayingInfo"));
    assert(!request_has(&peer->requests[9], "replace"));
    assert(!bplist_contains_zero_real(peer->requests[9].body,
                                      (size_t)peer->requests[9].body_len));
    assert(request_has(&peer->requests[10], "updateMRPlaybackState"));

    /* Redundant play: timeline update, then the FORCED state re-assert the
     * caller asked for — the chained send dedupes on the unchanged state, so
     * without the kept force nothing would reach the wire. */
    assert(request_has(&peer->requests[11], "updateMRNowPlayingInfo"));
    assert(!request_has(&peer->requests[11], "replace"));
    assert(request_has(&peer->requests[12], "updateMRPlaybackState"));
    assert(!request_has(&peer->requests[12], "updateMRNowPlayingInfo"));

    /* Stop: state only, no now-playing body at teardown. */
    assert(request_has(&peer->requests[13], "updateMRPlaybackState"));
    assert(!request_has(&peer->requests[13], "updateMRNowPlayingInfo"));

    /* Bundle 2: the DMAP copy is re-sent for the new track even though the
     * bytes are identical, while the MRP push keeps the ArtworkIdentifier
     * (an identifier flip alone re-renders the receiver's UI) under a fresh
     * item UniqueIdentifier. */
    assert(strcmp(peer->requests[14].method, "SET_PARAMETER") == 0);
    assert(request_has(&peer->requests[14], "mlit"));
    assert(strcmp(peer->requests[15].method, "SET_PARAMETER") == 0);
    assert(bytes_contain(peer->requests[15].body,
                         (size_t)peer->requests[15].body_len,
                         k_baseline_jpeg, sizeof(k_baseline_jpeg)));
    assert(request_has(&peer->requests[16], "updateMRNowPlayingInfo"));
    assert(request_has(&peer->requests[16],
                       "kMRMediaRemoteNowPlayingInfoArtworkData"));
    char id_first[17];
    char id_second[17];
    assert(find_artwork_identifier(peer->requests[3].body,
                                   (size_t)peer->requests[3].body_len,
                                   id_first));
    assert(find_artwork_identifier(peer->requests[16].body,
                                   (size_t)peer->requests[16].body_len,
                                   id_second));
    assert(strcmp(id_first, id_second) == 0);
    uint64_t uid_first = 0;
    uint64_t uid_second = 0;
    assert(ap2_bplist_find_uint(
        peer->requests[3].body, (size_t)peer->requests[3].body_len,
        "kMRMediaRemoteNowPlayingInfoUniqueIdentifier", &uid_first));
    assert(ap2_bplist_find_uint(
        peer->requests[16].body, (size_t)peer->requests[16].body_len,
        "kMRMediaRemoteNowPlayingInfoUniqueIdentifier", &uid_second));
    assert(uid_first != 0 && uid_second != 0 && uid_first != uid_second);

    /* Exactly five now-playing posts in total: one per bundle, one per
     * pause/resume/redundant-play transition — nothing doubled. */
    int nowplaying_posts = 0;
    for (int i = 0; i < peer->request_count; i++) {
        if (request_has(&peer->requests[i], "updateMRNowPlayingInfo"))
            nowplaying_posts++;
    }
    assert(nowplaying_posts == 5);

    ap2cl_test_detach_rtsp_socket(client);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    assert(ap2cl_destroy(client));
    free(peer);
    puts("ap2_client MRP bundle and pause publication tests passed");
}

/* An explicit --protocol airplay2 reaches the native flow on exactly the same
 * terms as auto, so an AirPlay-2-only receiver (no _raop service to fall back
 * on) is not routed into a RAOP-compatible flow it cannot answer. Only a real
 * pairing blocker drops it back to RAOP-compat. */
static void test_route_explicit_airplay2(void)
{
    /* features bit 48 (AirPlay 2 + pairing) => "0x0,0x10000" */
    const char *txt = "features=0x0,0x10000 flags=0x4";

    ap2_route_t transient = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, txt, NULL, false, false, 16, false, false, false);
    assert(!transient.use_raop && transient.native && transient.transient);
    assert(strcmp(transient.reason, "native AP2, transient, realtime") == 0);

    /* Pairing bit 46 alone: auto would not even call this device AirPlay 2, but
     * the explicit flag does, and it pairs transiently just the same. */
    ap2_route_t hk_only = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=0x0,0x4000", NULL, false, false, 16,
        false, false, false);
    assert(hk_only.native && hk_only.transient);

    /* A receiver that advertises no features at all is the case the explicit
     * flag exists for: trust the caller and pair transiently. */
    ap2_route_t featureless = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "model=Generic1,1 srcvers=770.8.1", NULL, false,
        false, 16, false, false, false);
    assert(featureless.native && featureless.transient);
    ap2_route_t no_txt = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, NULL, NULL, false, false, 16, false, false, false);
    assert(no_txt.native && no_txt.transient);
    /* A features field that enumerates nothing, or one we cannot parse, says
     * exactly as much as no field at all — all three take the same route. */
    ap2_route_t zero_features = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=0x0,0x0", NULL, false, false, 16, false,
        false, false);
    assert(zero_features.native && zero_features.transient);
    ap2_route_t unparseable = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=nonsense", NULL, false, false, 16, false,
        false, false);
    assert(unparseable.native && unparseable.transient);

    /* That trust is explicit-only: auto still reads the same TXT as legacy. */
    ap2_route_t featureless_auto = ap2_resolve_route(
        AP2_PROTO_AUTO, "model=Generic1,1 srcvers=770.8.1", NULL, false, false,
        16, false, false, false);
    assert(featureless_auto.use_raop);

    /* PIN or legacy pairing: native cannot work either, so RAOP-compat stays
     * the last attempt worth making. */
    ap2_route_t pin_required = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=0x0,0x10000 flags=0x8", NULL, false,
        false, 16, false, false, false);
    assert(!pin_required.use_raop && !pin_required.native);
    assert(strcmp(pin_required.reason, "AirPlay 2 (RAOP-compat)") == 0);

    ap2_route_t legacy_pairing = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=0x0,0x10000 flags=0x200", NULL, false,
        false, 16, false, false, false);
    assert(!legacy_pairing.use_raop && !legacy_pairing.native);

    /* A required password we do not hold blocks transient pairing; supplying
     * one unblocks it, exactly as under auto. */
    ap2_route_t pw_missing = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, txt, "true", false, false, 16, false, false, false);
    assert(!pw_missing.native);
    ap2_route_t pw_supplied = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, txt, "true", false, true, 16, false, false, false);
    assert(pw_supplied.native && pw_supplied.transient);
    assert(strcmp(pw_supplied.reason, "native AP2, password, realtime") == 0);

    /* Stored credentials still select pair-verify over transient pairing. */
    ap2_route_t with_creds = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, txt, NULL, true, false, 16, false, false, false);
    assert(with_creds.native && !with_creds.transient);
    assert(strcmp(with_creds.reason, "native AP2, pair-verify, realtime") == 0);

    /* An AirPlay 2 receiver without the pairing bits keeps the proven
     * RAOP-compatible flow (features bit 38 only). */
    ap2_route_t compat_only = ap2_resolve_route(
        AP2_PROTO_AIRPLAY2, "features=0x0,0x40", NULL, false, false, 16, false,
        false, false);
    assert(!compat_only.use_raop && !compat_only.native);

    /* --protocol raop is untouched by any of this. */
    ap2_route_t forced_raop = ap2_resolve_route(
        AP2_PROTO_RAOP, txt, NULL, false, false, 16, false, false, false);
    assert(forced_raop.use_raop);
    puts("ap2_client explicit airplay2 route tests passed");
}

/* Play the receiver end: ask a live responder for packets over the control
 * port and check what comes back. */
static void test_retransmit_responder(void)
{
    ap2_device_info_t device = {
        .name = "rtx", .address = "127.0.0.1", .port = 7000};
    ap2_audio_format_t format = {
        .sample_rate = 44100, .bit_depth = 16, .channels = 2};
    struct ap2cl_s *client = ap2cl_create(
        &device, &format, NULL, NULL, NULL, NULL, 2000, 100);
    assert(client);

    int ctrl_port = ap2cl_test_start_rtx(client);
    assert(ctrl_port > 0);

    /* Two packets the receiver will claim it lost, plus one never sent. */
    uint8_t pkt_a[64], pkt_b[64];
    for (size_t i = 0; i < sizeof(pkt_a); i++) { pkt_a[i] = (uint8_t)i; }
    for (size_t i = 0; i < sizeof(pkt_b); i++) { pkt_b[i] = (uint8_t)(0xF0 ^ i); }
    ap2cl_test_rtx_store(client, 1000, pkt_a, (int)sizeof(pkt_a));
    ap2cl_test_rtx_store(client, 1001, pkt_b, (int)sizeof(pkt_b));

    int peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons((uint16_t)ctrl_port);

    /* marker|0x55, request seq 7, first missing seq 1000, count 2 */
    uint8_t req[8] = {0x80, 0xd5, 0x00, 0x07, 0x03, 0xe8, 0x00, 0x02};
    assert(sendto(peer, req, sizeof(req), 0,
                  (struct sockaddr *)&to, sizeof(to)) == (ssize_t)sizeof(req));

    bool seen_a = false, seen_b = false;
    for (int i = 0; i < 2; i++) {
        uint8_t resp[512];
        ssize_t n = recv(peer, resp, sizeof(resp), 0);
        assert(n == 4 + (ssize_t)sizeof(pkt_a));
        assert(resp[0] == 0x80 && resp[1] == 0xd6);
        assert(((resp[2] << 8) | resp[3]) == 7);   /* echoes the request seq */
        if (!memcmp(resp + 4, pkt_a, sizeof(pkt_a))) seen_a = true;
        if (!memcmp(resp + 4, pkt_b, sizeof(pkt_b))) seen_b = true;
    }
    assert(seen_a && seen_b);
    /* The responder counts a resend only once the datagram is away, so the
     * second packet can reach us here before the counter catches up. */
    for (int i = 0; i < 30 && ap2cl_test_rtx_answered(client) < 2; i++)
        usleep(50000);
    assert(ap2cl_test_rtx_answered(client) == 2);

    /* A packet that has already aged out is counted, not answered. */
    uint8_t stale[8] = {0x80, 0xd5, 0x00, 0x08, 0x00, 0x2a, 0x00, 0x01};
    assert(sendto(peer, stale, sizeof(stale), 0,
                  (struct sockaddr *)&to, sizeof(to)) == (ssize_t)sizeof(stale));
    for (int i = 0; i < 30 && ap2cl_test_rtx_expired(client) == 0; i++)
        usleep(50000);
    assert(ap2cl_test_rtx_expired(client) == 1);
    assert(ap2cl_test_rtx_answered(client) == 2);

    close(peer);
    ap2cl_test_stop_rtx(client);
    ap2cl_destroy(client);
    puts("ap2_client retransmit responder tests passed");
}

static uint64_t test_now_unix_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static void test_apple_model_resolution(void)
{
    assert(ap2cl_test_apple_model_default(
        "model=AppleTV11,1 features=0x4A7FDFD5,0x3C177FDE", NULL));
    assert(ap2cl_test_apple_model_default(NULL, "AudioAccessory5,1"));
    assert(ap2cl_test_apple_model_default("model=Mac16,10", NULL));
    assert(!ap2cl_test_apple_model_default("model=Era 100", NULL));
    assert(!ap2cl_test_apple_model_default("model=HW-LS60D", NULL));
    assert(!ap2cl_test_apple_model_default(NULL, "Era 100"));
    assert(!ap2cl_test_apple_model_default(NULL, NULL));
    puts("ap2_client apple model resolution tests passed");
}

/* The pacing window is a duration, so every term scales with the stream rate:
 * the margin held back from the receiver's reported buffer, the default that
 * stands in for a buffer it never reported, and the splice depth that caps
 * both. Same frame counts at 44.1 kHz, ~8.8% more at 48 kHz. */
static void test_pacing_window_rate_scaling(void)
{
    static const struct {
        int rate;
        uint64_t unreported;    /* no latencyMax echoed: the 1.75 s default */
        uint32_t reported;      /* a 2.0 s receiver buffer, in frames */
        uint64_t windowed;      /* that buffer less the 250 ms margin */
        uint32_t under_margin;  /* a buffer no deeper than the margin */
        uint32_t shallow;       /* an 800 ms buffer: under the splice depth */
        uint64_t shallow_win;
        uint64_t splice_depth;  /* 600 ms, the cap on the splice timeline */
    } cases[] = {
        {44100, 77175, 88200, 77175, 4410, 35280, 24255, 26460},
        {48000, 84000, 96000, 84000, 4800, 38400, 26400, 28800},
    };

    ap2_device_info_t device = {
        .name = "pacing test",
        .address = "127.0.0.1",
        .port = 7000,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ap2_audio_format_t format = {
            .sample_rate = cases[i].rate,
            .bit_depth = 16,
            .channels = 2,
        };
        struct ap2cl_s *client = ap2cl_create(
            &device, &format, NULL, NULL, NULL, NULL, 2000, 100);
        assert(client);
        ap2cl_force_native(client);

        /* Receiver echoes no latencyMax (Sonos, and both Samsung sets in the
         * late-join field logs): the assumed buffer less the margin. */
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].unreported);

        /* A reported buffer: the margin held back scales with the rate too. */
        ap2cl_test_set_dev_latency_max(client, cases[i].reported);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].windowed);

        /* A buffer inside the margin leaves nothing to hold back, so the
         * default stands rather than underflowing the subtraction. */
        ap2cl_test_set_dev_latency_max(client, cases[i].under_margin);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].unreported);

        /* On the splice timeline the shallow queue depth caps the window —
         * unless the receiver reports a buffer shallower still, which then
         * caps the room a corrected late join's content skip has to drain. */
        ap2cl_test_set_splice(client, true);
        ap2cl_test_set_dev_latency_max(client, cases[i].reported);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].splice_depth);
        ap2cl_test_set_dev_latency_max(client, cases[i].shallow);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].shallow_win);

        assert(ap2cl_destroy(client));
    }
    puts("ap2_client pacing window rate scaling tests passed");
}

/* An explicit depth override outranks the buffer the receiver never reported,
 * but never the one it did: a starving renderer can be given the headroom it
 * needs, while a device that named its limit still bounds the delivery. The
 * depth drives ms-domain consumers (warm lead, clock-verification windows)
 * as well as the frame-domain pacing window, so both are pinned. */
static void test_splice_depth_override_vs_reported_buffer(void)
{
    static const struct {
        int rate;
        uint64_t override_unreported; /* a 3000 ms override, no window echoed */
        uint32_t reported;            /* a 2.0 s receiver buffer, in frames */
        uint64_t override_reported;   /* that buffer less the 250 ms margin */
        uint32_t under_margin;        /* a buffer no deeper than the margin */
    } cases[] = {
        {44100, 132300, 88200, 77175, 4410},
        {48000, 144000, 96000, 84000, 4800},
    };

    ap2_device_info_t device = {
        .name = "depth override test",
        .address = "127.0.0.1",
        .port = 7000,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ap2_audio_format_t format = {
            .sample_rate = cases[i].rate,
            .bit_depth = 16,
            .channels = 2,
        };
        struct ap2cl_s *client =
            ap2cl_create(&device, &format, NULL, NULL, NULL, NULL, 2000, 100);
        assert(client);
        ap2cl_force_native(client);
        ap2cl_test_set_splice(client, true);

        /* Nothing reported: a 3000 ms override clears the 1.75 s assumed
         * window untouched, in both the frame and the ms domain. */
        ap2cl_set_splice_depth_ms(client, 3000);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].override_unreported);
        assert(ap2cl_warm_lead_ms(client) == 3000);

        /* A reported 2.0 s buffer clamps the same override to that buffer
         * less the 250 ms margin. */
        ap2cl_test_set_dev_latency_max(client, cases[i].reported);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].override_reported);
        assert(ap2cl_warm_lead_ms(client) == 1750);

        /* A report at or under the margin leaves nothing to hold back, so it
         * counts as no report and the override stands again. */
        ap2cl_test_set_dev_latency_max(client, cases[i].under_margin);
        assert(ap2cl_test_pacing_window_frames(client) == cases[i].override_unreported);

        assert(ap2cl_destroy(client));
    }
    puts("ap2_client splice depth override tests passed");
}

/* A receiver that reports no window leaves the override unbounded, so the
 * setter bounds it: past the maximum the depth is clamped rather than taken,
 * keeping it inside the caller's EOF drain budget. */
static void test_splice_depth_bounds(void)
{
    ap2_device_info_t device = {
        .name = "depth bounds test",
        .address = "127.0.0.1",
        .port = 7000,
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    struct ap2cl_s *client =
        ap2cl_create(&device, &format, NULL, NULL, NULL, NULL, 2000, 100);
    assert(client);
    ap2cl_force_native(client);
    ap2cl_test_set_splice(client, true);

    /* Nothing set yet: the compiled default, itself under the assumed window. */
    assert(ap2cl_warm_lead_ms(client) == 600);

    /* Junk and negatives stay ignored, leaving the default in place. */
    ap2cl_set_splice_depth_ms(client, 0);
    assert(ap2cl_warm_lead_ms(client) == 600);
    ap2cl_set_splice_depth_ms(client, -1750);
    assert(ap2cl_warm_lead_ms(client) == 600);

    /* The maximum itself is taken as given. */
    ap2cl_set_splice_depth_ms(client, 3000);
    assert(ap2cl_warm_lead_ms(client) == 3000);

    /* Beyond it the depth is clamped, not truncated to junk or refused: the
     * pacing window follows the clamped depth, not the value asked for. */
    ap2cl_set_splice_depth_ms(client, 60000);
    assert(ap2cl_warm_lead_ms(client) == 3000);
    assert(ap2cl_test_pacing_window_frames(client) == 132300);

    /* A clamped depth is still an override, so it outranks the assumed window
     * and a reported buffer still clamps it further. */
    ap2cl_test_set_dev_latency_max(client, 88200);
    assert(ap2cl_warm_lead_ms(client) == 1750);

    assert(ap2cl_destroy(client));
    puts("ap2_client splice depth bounds tests passed");
}

/* A start committed before the receiver's first timing probe arms the
 * post-commit verification; the poll then verifies, corrects forward, or
 * closes the window, exactly once per commit. */
static void test_clock_verified_anchor(void)
{
    ap2_device_info_t device = {
        .name = "clock test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=Era 100",
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
    ap2cl_test_set_splice(client, true);
    ap2cl_test_set_use_ptp(client, true);
    ap2_clock_verify_event_t ev;

    /* Cold clock at commit: the commanded instant stands, verification arms. */
    uint64_t start_ms = test_now_unix_ms() + 5000;
    uint64_t at = 0;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(at == start_ms);
    assert(ap2cl_clock_verify_armed(client));

    /* No probe yet and a wide-open window: nothing to report. */
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_IDLE);
    assert(ap2cl_clock_verify_armed(client));

    /* A streak whose lock window ends before the anchor verifies it. */
    ap2cl_test_inject_clock_exchange(client, 2, 100, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_VERIFIED);
    assert(ev.margin_ms > 2000 && ev.margin_ms < 3500);
    assert(!ap2cl_clock_verify_armed(client));
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_IDLE);

    /* An origin start short of readiness is observed, never moved. */
    start_ms = test_now_unix_ms() + 1500;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_clock_verify_armed(client));
    uint64_t origin_head = ap2cl_test_head_ts(client);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(ap2cl_test_head_ts(client) == origin_head);
    assert(!ap2cl_clock_verify_armed(client));

    /* A join-marked start short of readiness withholds its ack and moves the
     * still-unsent line forward onto readiness exactly — no round-trip slack
     * on top, since the caller commits nothing back — with the wire head
     * following. The event carries the ack, so the caller maps its content
     * onto the acked instant and NO cut is taken: doing both would advance the
     * content twice. The 1.5 s anchor falls 800 ms short of the 2.3 s lock
     * window the injected streak starts. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(at == start_ms);
    assert(ap2cl_clock_verify_armed(client));
    assert(ap2cl_start_ack_deferred(client));
    assert(ap2cl_content_skip_bytes(client) == 0);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_CORRECTED);
    assert(ev.start_ack);
    assert(ev.from_unix_ms == start_ms);
    assert(ev.at_unix_ms >= start_ms + 800);
    assert(ev.at_unix_ms < start_ms + 900);
    assert(ev.content_cut_ms == 0);
    assert(ap2cl_content_skip_bytes(client) == 0);
    uint64_t head_ms_at_rate =
        ap2cl_test_head_ts(client) * 1000ULL / format.sample_rate;
    assert(head_ms_at_rate + 5 > ev.at_unix_ms &&
           head_ms_at_rate < ev.at_unix_ms + 5);
    /* Answered exactly once: the flag is consumed with the result, so a second
     * poll can neither re-deliver it nor produce another event. */
    assert(!ap2cl_start_ack_deferred(client));
    assert(!ap2cl_clock_verify_armed(client));
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_IDLE);
    assert(!ev.start_ack);

    /* The fallback: a join re-anchored through resume acks at commit, so a
     * later correction takes the cut instead — the caller already mapped its
     * content to the instant it was acked. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_resume(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_clock_verify_armed(client));
    assert(!ap2cl_start_ack_deferred(client));
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_CORRECTED);
    assert(!ev.start_ack);
    assert(ev.content_cut_ms == ev.at_unix_ms - ev.from_unix_ms);
    /* 16-bit stereo: 4 bytes per input frame. */
    assert(ap2cl_content_skip_bytes(client) ==
           ev.content_cut_ms * format.sample_rate / 1000 * 4);
    ap2cl_content_skip_consume(client, 128);
    assert(ap2cl_content_skip_bytes(client) ==
           ev.content_cut_ms * format.sample_rate / 1000 * 4 - 128);

    /* A new commit clears an undrained cut. */
    assert(ap2cl_start(client, 0, &at) == AP2_COMMIT_OK);
    assert(ap2cl_content_skip_bytes(client) == 0);

    /* So does a flush, and every other reset of the corrected timeline. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_resume(client, start_ms, &at) == AP2_COMMIT_OK);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_CORRECTED);
    assert(ap2cl_content_skip_bytes(client) > 0);
    assert(ap2cl_flush(client));
    assert(ap2cl_content_skip_bytes(client) == 0);

    /* A disarm before the verification resolves drops both flags together:
     * that is how the audio loop knows the withheld ack will never be
     * answered and must emit it with the instant that stands. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_clock_verify_armed(client));
    assert(ap2cl_start_ack_deferred(client));
    assert(ap2cl_flush(client));
    assert(!ap2cl_clock_verify_armed(client));
    assert(!ap2cl_start_ack_deferred(client));

    /* An origin start never withholds its ack: it is acked at commit and the
     * verification only observes. */
    start_ms = test_now_unix_ms() + 1500;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_clock_verify_armed(client));
    assert(!ap2cl_start_ack_deferred(client));
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(!ev.start_ack);

    /* The Apple fast bound (third exchange + settle) verifies an anchor the
     * full lock window would have corrected. */
    ap2cl_test_set_apple_model(client, true);
    start_ms = test_now_unix_ms() + 1500;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    ap2cl_test_inject_clock_exchange(client, 3, 400, 200);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_VERIFIED);
    assert(!ap2cl_clock_verify_armed(client));
    ap2cl_test_set_apple_model(client, false);

    /* A join whose anchor the receiver can already seat is acked at the
     * instant it committed to — the ack is withheld for the answer, not for a
     * correction. */
    ap2cl_test_set_apple_model(client, true);
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_start_ack_deferred(client));
    ap2cl_test_inject_clock_exchange(client, 3, 400, 200);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_VERIFIED);
    assert(ev.start_ack);
    assert(ev.at_unix_ms == start_ms);
    assert(ap2cl_content_skip_bytes(client) == 0);
    ap2cl_test_set_apple_model(client, false);

    /* Audio already on the wire closes the window: without a probe the
     * anchor stands unverified... */
    start_ms = test_now_unix_ms() + 5000;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    ap2cl_test_bump_audio_sent(client);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(!ap2cl_clock_verify_armed(client));

    /* ...and even a join cannot move a line with audio already on it. That
     * terminal outcome still answers the withheld ack, with the instant that
     * stands, so the caller is never left waiting on it. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_start_ack_deferred(client));
    uint64_t head_before = ap2cl_test_head_ts(client);
    ap2cl_test_bump_audio_sent(client);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(ev.start_ack);
    assert(ev.at_unix_ms == start_ms);
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(ap2cl_content_skip_bytes(client) == 0);
    assert(!ap2cl_clock_verify_armed(client));

    /* An anchor without room to act before the fill never arms — and a join
     * that never arms never withholds its ack, or nothing would emit it. */
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, 0, &at) == AP2_COMMIT_OK);
    assert(!ap2cl_clock_verify_armed(client));
    assert(!ap2cl_start_ack_deferred(client));

    /* A flush disarms a pending verification. */
    start_ms = test_now_unix_ms() + 5000;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_clock_verify_armed(client));
    assert(ap2cl_flush(client));
    assert(!ap2cl_clock_verify_armed(client));

    ap2cl_destroy(client);
    puts("ap2_client clock verified anchor tests passed");
}

/* Readiness reported from connect, so a caller can plan a join anchor the
 * receiver can actually meet instead of guessing a headroom. */
static void test_clock_readiness_report(void)
{
    ap2_device_info_t device = {
        .name = "readiness test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=Era 100",
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
    ap2cl_test_set_splice(client, true);
    ap2cl_test_set_session_state(client, AP2_CONNECTED);
    ap2_clock_readiness_t r;

    /* NTP timing — including a PTP session that fell back on a 319/320 bind
     * failure: readiness is not measurable, and the caller is told so rather
     * than left waiting for a reading that will never come. */
    ap2cl_test_set_use_ptp(client, false);
    assert(!ap2cl_uses_ptp(client));
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);
    assert(r.streak_ms == 0 && r.exchanges == 0);
    assert(r.ready_in_ms == 0 && r.ready_at_unix_ms == 0);

    /* PTP with no probe recorded: a receiver that just connected, or one the
     * engine has no peer slot left to serve. Cold, with nothing to plan
     * against — the caller times out instead of waiting forever. */
    ap2cl_test_set_use_ptp(client, true);
    assert(ap2cl_uses_ptp(client));
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);
    assert(r.streak_ms == 0 && r.exchanges == 0);
    assert(r.ready_in_ms == 0 && r.ready_at_unix_ms == 0);

    /* A live streak projects readiness one lock window from its start, and
     * carries the same two numbers the commit-time floor logs. */
    uint64_t before = test_now_unix_ms();
    ap2cl_test_inject_clock_exchange(client, 2, 100, 0);
    ap2cl_clock_readiness(client, &r);
    uint64_t after = test_now_unix_ms();
    assert(r.state == AP2_CLOCK_PROBING);
    assert(r.streak_ms == 100 && r.exchanges == 2);
    assert(r.ready_in_ms > 2100 && r.ready_in_ms <= 2200);
    /* The projection reads the clock once inside the call, so bracketing it by
     * the call's own span pins it exactly however long the call takes. */
    assert(r.ready_at_unix_ms >= before + 2200);
    assert(r.ready_at_unix_ms <= after + 2200);

    /* A streak whose projection has passed reports ready, with no wait left. */
    ap2cl_test_inject_clock_exchange(client, 12, 3000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_READY);
    assert(r.ready_in_ms == 0);
    assert(r.ready_at_unix_ms != 0);
    assert(r.streak_ms == 3000 && r.exchanges == 12);

    /* The Apple fast bound moves readiness in: the streak still probing on a
     * generic receiver is already ready on an Apple one. */
    ap2cl_test_inject_clock_exchange(client, 3, 400, 300);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_PROBING);
    ap2cl_test_set_apple_model(client, true);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_READY);
    ap2cl_test_set_apple_model(client, false);

    /* The projection is recomputed per call, never cached. */
    uint64_t first_at = r.ready_at_unix_ms;
    usleep(30000);
    ap2cl_test_inject_clock_exchange(client, 4, 400, 300);
    ap2cl_clock_readiness(client, &r);
    assert(r.ready_at_unix_ms > first_at);

    ap2cl_destroy(client);
    puts("ap2_client clock readiness report tests passed");
}

/* A receiver that never probes is reported as such: a permanently dead
 * receiver clock is otherwise indistinguishable from the cold state every
 * healthy receiver passes through on its way to probing. */
static void test_clock_stall_detection(void)
{
    ap2_device_info_t device = {
        .name = "stall test",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "model=Era 100",
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
    ap2cl_test_set_splice(client, true);
    ap2cl_test_set_use_ptp(client, true);
    ap2cl_test_set_session_state(client, AP2_CONNECTED);
    ap2_clock_readiness_t r;

    /* No session yet: nothing has started the window the stall is measured
     * against, so there is nothing to be late for. */
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);

    /* Inside the window: the receiver measured slowest took 1.078 s to its
     * first probe, so a second in this is still the normal cold state. */
    ap2cl_test_set_clock_marks(client, test_now_unix_ms() - 1000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);
    assert(r.streak_ms == 0 && r.exchanges == 0);
    assert(r.ready_in_ms == 0 && r.ready_at_unix_ms == 0);

    /* Past it with still no probe: the receiver never slaved to our clock. */
    ap2cl_test_set_clock_marks(client, test_now_unix_ms() - 6000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_STALLED);
    assert(r.streak_ms == 0 && r.exchanges == 0);
    assert(r.ready_in_ms == 0 && r.ready_at_unix_ms == 0);

    /* An NTP-timed session has no streak to be missing, however long it has
     * been up, so it stays cold rather than raising a false alarm. */
    ap2cl_test_set_use_ptp(client, false);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);
    ap2cl_test_set_use_ptp(client, true);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_STALLED);

    /* Stalled is not terminal: a receiver that starts probing late is picked
     * up on the next sample and runs on to ready as usual. */
    ap2cl_test_inject_clock_exchange(client, 2, 100, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_PROBING);
    assert(r.streak_ms == 100 && r.exchanges == 2);
    assert(r.ready_in_ms > 2100 && r.ready_in_ms <= 2200);
    ap2cl_test_inject_clock_exchange(client, 20, 3000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_READY);
    assert(r.ready_in_ms == 0);

    /* One empty snapshot is a lost round-trip, not a receiver that went away:
     * the window runs from the streak seen a poll ago, so a session a minute
     * past connect still reports nothing to measure rather than a stall. */
    ap2cl_test_set_clock_marks(client, test_now_unix_ms() - 60000, 0);
    ap2cl_test_inject_clock_exchange(client, 40, 8000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_READY);
    ap2cl_test_clear_clock_exchange(client);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);

    /* A mark kept current — by the shared daemon's poller, or by the re-arm
     * restarting the window — makes a lost round-trip on that very cycle only
     * a blip. */
    ap2cl_test_set_clock_marks(client, test_now_unix_ms() - 600000,
                               test_now_unix_ms() - 200);
    ap2cl_test_clear_clock_exchange(client);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);

    /* Only an absence lasting the whole window is the real thing, and it is
     * measured from that last streak, not from the far older connect. */
    ap2cl_test_set_clock_marks(client, test_now_unix_ms() - 600000,
                               test_now_unix_ms() - 6000);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_STALLED);

    /* Restarting the watch is what earns a re-armed session the same grace as
     * a cold one: without it the in-process engine, which has no poller, would
     * measure the stall against a mark left behind while reporting was
     * terminal and condemn a healthy receiver on the first reading. */
    ap2cl_clock_watch_restart(client);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);

    /* Once the session is down there is no receiver to answer for: the marks
     * survive teardown, so a reading taken afterwards must not read as a stall. */
    ap2cl_test_set_session_state(client, AP2_DOWN);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);

    /* The snapshot survives teardown too, so the same has to hold with a streak
     * still planted — a torn-down session must not answer with a live one. */
    ap2cl_test_inject_clock_exchange(client, 40, 8000, 0);
    ap2cl_clock_readiness(client, &r);
    assert(r.state == AP2_CLOCK_COLD);
    assert(r.exchanges == 0 && r.ready_at_unix_ms == 0);

    ap2cl_destroy(client);
    puts("ap2_client clock stall detection tests passed");
}

int main(void)
{
    test_retransmit_responder();
    test_route_with_password();
    test_buffered_route_resolution();
    test_route_explicit_airplay2();
    test_auth_error_kind();
    test_info_format_tables();
    test_feedback_stream_counts();
    test_native_flush_resume_reuses_rtsp_session();
    test_native_flush_rejects_receiver_error();
    test_env_toggle_parsing();
    test_splice_default_resolution();
    test_splice_timeline_warm_path();
    test_splice_delivery_gap_recovery();
    test_splice_pause_keeps_line_hot();
    test_stock_pause_resume_reanchors();
    test_mrp_bundle_and_pause_publish();
    test_feedback_miss_tolerated_then_recovered();
    test_dead_channel_writes_farewell_teardown();
    test_feedback_miss_budget_from_env();
    test_feedback_miss_budget_of_one_kills_first_miss();
    test_apple_model_resolution();
    test_pacing_window_rate_scaling();
    test_splice_depth_override_vs_reported_buffer();
    test_splice_depth_bounds();
    test_clock_verified_anchor();
    test_clock_readiness_report();
    test_clock_stall_detection();

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

    /* A client that has not connected reports no failure, and the accessor
     * stays usable on a NULL client so the caller's error path cannot crash. */
    int http = 1234;
    const char *detail = NULL;
    assert(ap2cl_connect_error(client, &http, &detail) == AP2_CONNECT_ERROR_NONE);
    assert(http == 0 && detail && *detail == '\0');
    assert(ap2cl_connect_error(NULL, NULL, NULL) == AP2_CONNECT_ERROR_GENERIC);

    health_check_t check = {
        .client = client,
    };
    atomic_init(&check.done, false);
    ap2cl_test_lock_mrp(client);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_health_check, &check) == 0);
    /* Completing while the MRP lock is held is the property under test, so
     * wait for the snapshot instead of assuming a fixed span was enough. */
    for (int i = 0; i < 30 && !atomic_load(&check.done); i++)
        usleep(50000);
    assert(atomic_load(&check.done));
    assert(check.healthy);
    ap2cl_test_unlock_mrp(client);
    assert(pthread_join(thread, NULL) == 0);

    assert(ap2cl_destroy(client));
    puts("ap2_client health snapshot test passed");
    return 0;
}
