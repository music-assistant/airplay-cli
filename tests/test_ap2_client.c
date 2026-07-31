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
bool ap2cl_test_first_packet(struct ap2cl_s *p);
void ap2cl_test_set_first_packet(struct ap2cl_s *p, bool first_packet);

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
    assert(ap2cl_start(client, 1700000000000ULL));
    /* The test bypasses native connect, so model a completed first send before
     * exercising the warm restart. */
    ap2cl_test_set_first_packet(client, false);

    ap2cl_standby(client);
    assert(ap2cl_state(client) == AP2_CONNECTED);
    /* Warm seek: discard the receiver buffer, then re-anchor the timeline. */
    assert(ap2cl_flush(client));
    assert(ap2cl_resume(client, 1700000005000ULL));
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
    assert(ap2cl_start(client, 1700000000000ULL));
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

int main(void)
{
    test_route_with_password();
    test_route_explicit_airplay2();
    test_info_format_tables();
    test_native_flush_resume_reuses_rtsp_session();
    test_native_flush_rejects_receiver_error();
    test_feedback_miss_tolerated_then_recovered();

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
