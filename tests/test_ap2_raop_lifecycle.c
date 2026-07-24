#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cross_log.h"
#include "raop_client.h"
#include "ap2_client.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

#define MAX_CLIENTS 8
#define MAX_EVENTS 32

struct raopcl_s {
    int id;
    raop_state_t state;
    uint32_t latency;
    uint32_t sample_rate;
};

typedef enum {
    EVENT_CREATE,
    EVENT_CONNECT,
    EVENT_DISCONNECT,
    EVENT_DESTROY,
} event_type_t;

typedef struct {
    event_type_t type;
    int client_id;
} event_t;

static struct {
    struct raopcl_s clients[MAX_CLIENTS];
    event_t events[MAX_EVENTS];
    bool connect_results[MAX_CLIENTS];
    size_t event_count;
    size_t connect_result_count;
    int creates;
    int connect_calls;
    int disconnects;
    int destroys;
    int destroy_counts[MAX_CLIENTS];
    int stops;
    int flushes;
    int starts;
    struct raopcl_s *last_transport_client;
} mock;

static void record_event(event_type_t type, int client_id)
{
    if (mock.event_count < MAX_EVENTS) {
        mock.events[mock.event_count].type = type;
        mock.events[mock.event_count].client_id = client_id;
    }
    mock.event_count++;
}

static void reset_mock(void)
{
    memset(&mock, 0, sizeof(mock));
}

struct raopcl_s *ap2_test_raopcl_create(
    struct in_addr host, uint16_t port_base, uint16_t port_range,
    char *dacp_id, char *active_remote,
    raop_codec_t codec, int frame_len, int latency_frames,
    raop_crypto_t crypto, bool auth, char *secret, char *password,
    char *et, char *md,
    int sample_rate, int sample_size, int channels, float volume)
{
    (void)host;
    (void)port_base;
    (void)port_range;
    (void)dacp_id;
    (void)active_remote;
    (void)codec;
    (void)frame_len;
    (void)latency_frames;
    (void)crypto;
    (void)auth;
    (void)secret;
    (void)password;
    (void)et;
    (void)md;
    (void)sample_rate;
    (void)sample_size;
    (void)channels;
    (void)volume;

    int id = ++mock.creates;
    if (id >= MAX_CLIENTS) return NULL;
    mock.clients[id].id = id;
    mock.clients[id].state = RAOP_DOWN;
    mock.clients[id].latency = 11025;
    mock.clients[id].sample_rate = 44100;
    record_event(EVENT_CREATE, id);
    return &mock.clients[id];
}

bool ap2_test_raopcl_connect(
    struct raopcl_s *client, struct in_addr host,
    uint16_t port, bool set_volume)
{
    (void)host;
    (void)port;
    (void)set_volume;

    record_event(EVENT_CONNECT, client->id);
    int call = mock.connect_calls++;
    bool connected = call < (int)mock.connect_result_count
        ? mock.connect_results[call] : false;
    if (connected) client->state = RAOP_FLUSHED;
    return connected;
}

bool ap2_test_raopcl_disconnect(struct raopcl_s *client)
{
    mock.disconnects++;
    record_event(EVENT_DISCONNECT, client->id);
    return true;
}

bool ap2_test_raopcl_destroy(struct raopcl_s *client)
{
    mock.destroys++;
    mock.destroy_counts[client->id]++;
    record_event(EVENT_DESTROY, client->id);
    return true;
}

raop_state_t ap2_test_raopcl_state(struct raopcl_s *client)
{
    return client->state;
}

void ap2_test_raopcl_stop(struct raopcl_s *client)
{
    mock.stops++;
    mock.last_transport_client = client;
}

bool ap2_test_raopcl_flush(struct raopcl_s *client)
{
    mock.flushes++;
    mock.last_transport_client = client;
    client->state = RAOP_FLUSHED;
    return true;
}

bool ap2_test_raopcl_start_at(
    struct raopcl_s *client, uint64_t start_time)
{
    (void)start_time;
    mock.starts++;
    mock.last_transport_client = client;
    client->state = RAOP_STREAMING;
    return true;
}

uint64_t ap2_test_raopcl_get_ntp(struct ntp_s *ntp)
{
    (void)ntp;
    return ((uint64_t)1700000000) << 32;
}

uint32_t ap2_test_raopcl_latency(struct raopcl_s *client)
{
    return client->latency;
}

uint32_t ap2_test_raopcl_sample_rate(struct raopcl_s *client)
{
    return client->sample_rate;
}

static struct ap2cl_s *create_test_client(void)
{
    ap2_device_info_t device = {
        .name = "RAOP lifecycle test",
        .hostname = "localhost",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "",
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    return ap2cl_create(
        &device, &format, NULL, NULL, "0011223344556677", "1", 2000, 50);
}

static bool check_events(const event_t *expected, size_t count)
{
    CHECK(mock.event_count == count);
    for (size_t i = 0; i < count; i++) {
        CHECK(mock.events[i].type == expected[i].type);
        CHECK(mock.events[i].client_id == expected[i].client_id);
    }
    return true;
}

static bool test_disconnect_reconnect_and_failure_cleanup(void)
{
    reset_mock();
    mock.connect_results[0] = true;
    mock.connect_results[1] = true;
    mock.connect_results[2] = false;
    mock.connect_result_count = 3;

    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_disconnect(client));
    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_disconnect(client));
    CHECK(!ap2cl_connect(client));
    CHECK(ap2cl_destroy(client));

    static const event_t expected[] = {
        {EVENT_CREATE, 1},
        {EVENT_CONNECT, 1},
        {EVENT_DISCONNECT, 1},
        {EVENT_DESTROY, 1},
        {EVENT_CREATE, 2},
        {EVENT_CONNECT, 2},
        {EVENT_DISCONNECT, 2},
        {EVENT_DESTROY, 2},
        {EVENT_CREATE, 3},
        {EVENT_CONNECT, 3},
        {EVENT_DESTROY, 3},
    };
    CHECK(check_events(expected, sizeof(expected) / sizeof(expected[0])));
    CHECK(mock.creates == 3);
    CHECK(mock.destroys == 3);
    CHECK(mock.destroy_counts[1] == 1);
    CHECK(mock.destroy_counts[2] == 1);
    CHECK(mock.destroy_counts[3] == 1);
    return true;
}

static bool test_final_cleanup_order(void)
{
    reset_mock();
    mock.connect_results[0] = true;
    mock.connect_result_count = 1;

    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_destroy(client));

    static const event_t expected[] = {
        {EVENT_CREATE, 1},
        {EVENT_CONNECT, 1},
        {EVENT_DISCONNECT, 1},
        {EVENT_DESTROY, 1},
    };
    CHECK(check_events(expected, sizeof(expected) / sizeof(expected[0])));
    CHECK(mock.creates == 1);
    CHECK(mock.disconnects == 1);
    CHECK(mock.destroys == 1);
    CHECK(mock.destroy_counts[1] == 1);
    return true;
}

static bool test_standby_reuses_raop_compatible_connection(void)
{
    reset_mock();
    mock.connect_results[0] = true;
    mock.connect_result_count = 1;

    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    CHECK(ap2cl_connect(client));
    struct raopcl_s *transport_client = &mock.clients[1];
    CHECK(ap2cl_start(client, 1700000003000ULL));
    CHECK(ap2cl_state(client) == AP2_STREAMING);
    CHECK(mock.starts == 1);

    ap2cl_standby(client);
    CHECK(ap2cl_state(client) == AP2_CONNECTED);
    CHECK(mock.last_transport_client == transport_client);
    CHECK(mock.stops == 1);
    CHECK(mock.flushes == 1);
    CHECK(mock.disconnects == 0);
    CHECK(mock.destroys == 0);

    /* A warm seek is now FLUSH (discard receiver buffer) + START (re-anchor):
     * flush stops the flushed stream again, resume re-arms start-at. No extra
     * receiver flush, so mock.flushes stays 1. */
    CHECK(ap2cl_flush(client));
    CHECK(ap2cl_resume(client, 1700000008000ULL));
    CHECK(ap2cl_state(client) == AP2_STREAMING);
    CHECK(mock.last_transport_client == transport_client);
    CHECK(mock.stops == 2);
    CHECK(mock.flushes == 1);
    CHECK(mock.starts == 2);
    CHECK(mock.disconnects == 0);
    CHECK(mock.destroys == 0);

    CHECK(ap2cl_destroy(client));
    CHECK(mock.disconnects == 1);
    CHECK(mock.destroys == 1);
    return true;
}

int main(void)
{
    if (!test_disconnect_reconnect_and_failure_cleanup() ||
        !test_final_cleanup_order() ||
        !test_standby_reuses_raop_compatible_connection())
        return 1;
    puts("AP2 RAOP reconnect lifecycle tests passed");
    return 0;
}
