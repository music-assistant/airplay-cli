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

typedef struct {
    int id;
    int disconnects;
    int destroys;
} fake_raop_t;

typedef struct {
    fake_raop_t clients[MAX_CLIENTS];
    bool connect_results[MAX_CLIENTS];
    int connect_result_count;
    int creates;
    int connects;
    bool double_destroy;
    char events[MAX_EVENTS][8];
    int event_count;
} mock_state_t;

static mock_state_t mock;

static fake_raop_t *fake_raop(struct raopcl_s *client)
{
    return (fake_raop_t *)client;
}

static void record_event(char kind, int id)
{
    if (mock.event_count >= MAX_EVENTS) return;
    snprintf(mock.events[mock.event_count], sizeof(mock.events[0]),
             "%c%d", kind, id);
    mock.event_count++;
}

static void reset_mock(const bool *results, int count)
{
    memset(&mock, 0, sizeof(mock));
    memcpy(mock.connect_results, results, (size_t)count * sizeof(results[0]));
    mock.connect_result_count = count;
}

struct raopcl_s *test_raopcl_create(
    struct in_addr host, uint16_t port_base, uint16_t port_range,
    char *dacp_id, char *active_remote,
    raop_codec_t codec, int frame_len, int latency_frames,
    raop_crypto_t crypto, bool auth, char *secret, char *password,
    char *et, char *md,
    int sample_rate, int sample_size, int channels, float volume)
{
    (void)host; (void)port_base; (void)port_range;
    (void)dacp_id; (void)active_remote; (void)codec; (void)frame_len;
    (void)latency_frames; (void)crypto; (void)auth; (void)secret;
    (void)password; (void)et; (void)md; (void)sample_rate;
    (void)sample_size; (void)channels; (void)volume;
    if (mock.creates >= MAX_CLIENTS) return NULL;
    fake_raop_t *client = &mock.clients[mock.creates];
    client->id = ++mock.creates;
    record_event('C', client->id);
    return (struct raopcl_s *)client;
}

bool test_raopcl_connect(
    struct raopcl_s *client, struct in_addr host,
    uint16_t destport, bool set_volume)
{
    (void)host; (void)destport; (void)set_volume;
    fake_raop_t *fake = fake_raop(client);
    record_event('K', fake->id);
    if (mock.connects >= mock.connect_result_count) return false;
    return mock.connect_results[mock.connects++];
}

bool test_raopcl_disconnect(struct raopcl_s *client)
{
    fake_raop_t *fake = fake_raop(client);
    fake->disconnects++;
    record_event('X', fake->id);
    return true;
}

bool test_raopcl_destroy(struct raopcl_s *client)
{
    fake_raop_t *fake = fake_raop(client);
    if (fake->destroys) mock.double_destroy = true;
    fake->destroys++;
    record_event('D', fake->id);
    return true;
}

static struct ap2cl_s *create_client(void)
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
        &device, &format, NULL, NULL,
        "0011223344556677", "1", 2000, -1);
}

static bool expect_events(const char *const *expected, int count)
{
    CHECK(mock.event_count == count);
    for (int i = 0; i < count; i++)
        CHECK(strcmp(mock.events[i], expected[i]) == 0);
    return true;
}

static bool test_disconnect_reconnect_failure_cleanup(void)
{
    static const bool results[] = {true, true, false};
    static const char *const expected[] = {
        "C1", "K1", "X1", "D1",
        "C2", "K2", "X2", "D2", "C3", "K3", "D3",
    };
    reset_mock(results, 3);
    struct ap2cl_s *client = create_client();
    CHECK(client != NULL);

    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_state(client) == AP2_CONNECTED);
    CHECK(ap2cl_disconnect(client));
    CHECK(ap2cl_state(client) == AP2_DOWN);
    CHECK(mock.clients[0].disconnects == 1);
    CHECK(mock.clients[0].destroys == 1);

    int events_after_disconnect = mock.event_count;
    CHECK(ap2cl_disconnect(client));
    CHECK(mock.event_count == events_after_disconnect);

    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_state(client) == AP2_CONNECTED);
    CHECK(!ap2cl_connect(client));
    CHECK(ap2cl_state(client) == AP2_DOWN);
    CHECK(expect_events(
        expected, (int)(sizeof(expected) / sizeof(expected[0]))));
    CHECK(mock.creates == 3);
    CHECK(mock.clients[0].destroys == 1);
    CHECK(mock.clients[1].disconnects == 1);
    CHECK(mock.clients[1].destroys == 1);
    CHECK(mock.clients[2].disconnects == 0);
    CHECK(mock.clients[2].destroys == 1);
    CHECK(!mock.double_destroy);

    int events_before_destroy = mock.event_count;
    CHECK(ap2cl_destroy(client));
    CHECK(mock.event_count == events_before_destroy);
    CHECK(!mock.double_destroy);
    return true;
}

static bool test_connected_final_cleanup(void)
{
    static const bool results[] = {true};
    static const char *const expected[] = {"C1", "K1", "X1", "D1"};
    reset_mock(results, 1);
    struct ap2cl_s *client = create_client();
    CHECK(client != NULL);
    CHECK(ap2cl_connect(client));
    CHECK(ap2cl_destroy(client));
    CHECK(expect_events(
        expected, (int)(sizeof(expected) / sizeof(expected[0]))));
    CHECK(mock.clients[0].disconnects == 1);
    CHECK(mock.clients[0].destroys == 1);
    CHECK(!mock.double_destroy);
    return true;
}

int main(void)
{
    if (!test_disconnect_reconnect_failure_cleanup() ||
        !test_connected_final_cleanup())
        return 1;
    puts("RAOP-compatible reconnect lifecycle tests passed");
    return 0;
}
