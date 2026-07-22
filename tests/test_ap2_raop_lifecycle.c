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
    return call < (int)mock.connect_result_count
        ? mock.connect_results[call] : false;
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

int main(void)
{
    if (!test_disconnect_reconnect_and_failure_cleanup() ||
        !test_final_cleanup_order())
        return 1;
    puts("AP2 RAOP reconnect lifecycle tests passed");
    return 0;
}
