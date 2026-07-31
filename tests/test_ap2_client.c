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

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

void ap2cl_test_lock_mrp(struct ap2cl_s *p);
void ap2cl_test_unlock_mrp(struct ap2cl_s *p);
void ap2cl_test_attach_rtsp_socket(struct ap2cl_s *p, int fd);
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
void ap2cl_test_inject_clock_exchange(struct ap2cl_s *p, uint32_t count,
                                      uint64_t first_ms, uint64_t third_ms);
bool ap2cl_test_clock_verify_armed(struct ap2cl_s *p);
void ap2cl_test_bump_audio_sent(struct ap2cl_s *p);

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
    assert(ap2cl_test_clock_verify_armed(client));

    /* No probe yet and a wide-open window: nothing to report. */
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_IDLE);
    assert(ap2cl_test_clock_verify_armed(client));

    /* A streak whose lock window ends before the anchor verifies it. */
    ap2cl_test_inject_clock_exchange(client, 2, 100, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_VERIFIED);
    assert(ev.margin_ms > 2000 && ev.margin_ms < 3500);
    assert(!ap2cl_test_clock_verify_armed(client));
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_IDLE);

    /* An origin start short of readiness is observed, never moved. */
    start_ms = test_now_unix_ms() + 1500;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_test_clock_verify_armed(client));
    uint64_t origin_head = ap2cl_test_head_ts(client);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(ap2cl_test_head_ts(client) == origin_head);
    assert(!ap2cl_test_clock_verify_armed(client));

    /* A join-marked start short of readiness moves the still-unsent line
     * forward by the shortfall plus the retry slack (wire head following)
     * and advances the queued content by exactly the correction, so the
     * retained content still lands on the group timeline. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_test_clock_verify_armed(client));
    assert(ap2cl_content_skip_bytes(client) == 0);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_CORRECTED);
    assert(ev.from_unix_ms == start_ms);
    assert(ev.at_unix_ms > start_ms + 800);
    assert(ev.at_unix_ms < start_ms + 2000);
    assert(ev.content_cut_ms == ev.at_unix_ms - ev.from_unix_ms);
    uint64_t head_ms_at_rate =
        ap2cl_test_head_ts(client) * 1000ULL / format.sample_rate;
    assert(head_ms_at_rate + 5 > ev.at_unix_ms &&
           head_ms_at_rate < ev.at_unix_ms + 5);
    /* 16-bit stereo: 4 bytes per input frame. */
    assert(ap2cl_content_skip_bytes(client) ==
           ev.content_cut_ms * format.sample_rate / 1000 * 4);
    ap2cl_content_skip_consume(client, 128);
    assert(ap2cl_content_skip_bytes(client) ==
           ev.content_cut_ms * format.sample_rate / 1000 * 4 - 128);
    assert(!ap2cl_test_clock_verify_armed(client));

    /* A new commit clears an undrained cut. */
    assert(ap2cl_start(client, 0, &at) == AP2_COMMIT_OK);
    assert(ap2cl_content_skip_bytes(client) == 0);

    /* The Apple fast bound (third exchange + settle) verifies an anchor the
     * full lock window would have corrected. */
    ap2cl_test_set_apple_model(client, true);
    start_ms = test_now_unix_ms() + 1500;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    ap2cl_test_inject_clock_exchange(client, 3, 400, 200);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_VERIFIED);
    assert(!ap2cl_test_clock_verify_armed(client));
    ap2cl_test_set_apple_model(client, false);

    /* Audio already on the wire closes the window: without a probe the
     * anchor stands unverified... */
    start_ms = test_now_unix_ms() + 5000;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    ap2cl_test_bump_audio_sent(client);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(!ap2cl_test_clock_verify_armed(client));

    /* ...and even a join cannot move a line with audio already on it. */
    start_ms = test_now_unix_ms() + 1500;
    ap2cl_set_start_join(client, true);
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    uint64_t head_before = ap2cl_test_head_ts(client);
    ap2cl_test_bump_audio_sent(client);
    ap2cl_test_inject_clock_exchange(client, 1, 0, 0);
    assert(ap2cl_clock_verify_poll(client, &ev) == AP2_CLOCK_VERIFY_UNVERIFIED);
    assert(ap2cl_test_head_ts(client) == head_before);
    assert(!ap2cl_test_clock_verify_armed(client));

    /* An anchor without room to act before the fill never arms. */
    assert(ap2cl_start(client, 0, &at) == AP2_COMMIT_OK);
    assert(!ap2cl_test_clock_verify_armed(client));

    /* A flush disarms a pending verification. */
    start_ms = test_now_unix_ms() + 5000;
    assert(ap2cl_start(client, start_ms, &at) == AP2_COMMIT_OK);
    assert(ap2cl_test_clock_verify_armed(client));
    assert(ap2cl_flush(client));
    assert(!ap2cl_test_clock_verify_armed(client));

    ap2cl_destroy(client);
    puts("ap2_client clock verified anchor tests passed");
}

int main(void)
{
    test_retransmit_responder();
    test_route_with_password();
    test_route_explicit_airplay2();
    test_info_format_tables();
    test_native_flush_resume_reuses_rtsp_session();
    test_native_flush_rejects_receiver_error();
    test_splice_default_resolution();
    test_splice_timeline_warm_path();
    test_splice_delivery_gap_recovery();
    test_splice_pause_keeps_line_hot();
    test_feedback_miss_tolerated_then_recovered();
    test_apple_model_resolution();
    test_clock_verified_anchor();

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
    usleep(50000);
    assert(atomic_load(&check.done));
    assert(check.healthy);
    ap2cl_test_unlock_mrp(client);
    assert(pthread_join(thread, NULL) == 0);

    assert(ap2cl_destroy(client));
    puts("ap2_client health snapshot test passed");
    return 0;
}
