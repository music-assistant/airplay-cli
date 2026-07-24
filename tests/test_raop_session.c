#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "raop_client.h"
#include "raop_session.h"

struct raopcl_s {
    raop_state_t state;
    uint32_t latency;
    uint32_t sample_rate;
};

static uint64_t mock_now;
static struct raopcl_s *last_client;
static uint64_t last_start;
static int pause_calls;
static int stop_calls;
static int flush_calls;
static int start_calls;
static bool flush_result;

uint64_t raopcl_get_ntp(struct ntp_s *ntp)
{
    (void)ntp;
    return mock_now;
}

raop_state_t raopcl_state(struct raopcl_s *client)
{
    return client->state;
}

uint32_t raopcl_latency(struct raopcl_s *client)
{
    return client->latency;
}

uint32_t raopcl_sample_rate(struct raopcl_s *client)
{
    return client->sample_rate;
}

void raopcl_pause(struct raopcl_s *client)
{
    last_client = client;
    pause_calls++;
}

void raopcl_stop(struct raopcl_s *client)
{
    last_client = client;
    stop_calls++;
}

bool raopcl_flush(struct raopcl_s *client)
{
    last_client = client;
    flush_calls++;
    if (flush_result) client->state = RAOP_FLUSHED;
    return flush_result;
}

bool raopcl_start_at(struct raopcl_s *client, uint64_t start_time)
{
    last_client = client;
    last_start = start_time;
    start_calls++;
    return true;
}

static uint64_t unix_ms_to_ntp(uint64_t ms)
{
    return ((ms / 1000) << 32) | (((ms % 1000) << 32) / 1000);
}

static void reset_mocks(void)
{
    last_client = NULL;
    last_start = 0;
    pause_calls = 0;
    stop_calls = 0;
    flush_calls = 0;
    start_calls = 0;
    flush_result = true;
}

int main(void)
{
    struct raopcl_s client = {
        .state = RAOP_FLUSHED,
        .latency = 11025,
        .sample_rate = 44100,
    };
    const uint64_t now_ms = 1700000000000ULL;
    const uint64_t future_ms = now_ms + 3000;
    mock_now = unix_ms_to_ntp(now_ms);
    reset_mocks();

    assert(raop_session_commit(&client, future_ms));
    assert(last_client == &client);
    assert(pause_calls == 0);
    assert(stop_calls == 1);
    assert(flush_calls == 0);
    assert(start_calls == 1);
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           unix_ms_to_ntp(future_ms));

    client.state = RAOP_STREAMING;
    const uint64_t replacement_ms = future_ms + 5000;
    assert(raop_session_commit(&client, replacement_ms));
    assert(last_client == &client);
    assert(pause_calls == 0);
    assert(stop_calls == 2);
    assert(flush_calls == 1);
    assert(start_calls == 2);
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           unix_ms_to_ntp(replacement_ms));

    client.state = RAOP_FLUSHED;
    assert(raop_session_commit(&client, 0));
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           mock_now + MS2NTP(200));

    client.state = RAOP_STREAMING;
    assert(raop_session_standby(&client));
    assert(client.state == RAOP_FLUSHED);
    assert(stop_calls == 4);
    int stops_after_standby = stop_calls;
    int flushes_after_standby = flush_calls;
    assert(raop_session_standby(&client));
    assert(stop_calls == stops_after_standby + 1);
    assert(flush_calls == flushes_after_standby);

    /* Standby keeps the same connected libraop client reusable. A later
     * commanded START only re-anchors it; no reconnect API is involved. */
    const uint64_t post_standby_ms = replacement_ms + 5000;
    assert(raop_session_commit(&client, post_standby_ms));
    assert(last_client == &client);
    assert(start_calls == 4);
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           unix_ms_to_ntp(post_standby_ms));

    client.state = RAOP_STREAMING;
    assert(raop_session_pause(&client));
    assert(pause_calls == 1);
    assert(client.state == RAOP_FLUSHED);
    int stops_before_resume = stop_calls;
    assert(raop_session_resume(&client));
    assert(stop_calls == stops_before_resume);
    assert(start_calls == 5);
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           mock_now + MS2NTP(200));

    /* raop_session_flush discards the receiver buffer in place: stop, and flush
     * only when currently streaming (a warm seek's FLUSH half). */
    reset_mocks();
    client.state = RAOP_STREAMING;
    assert(raop_session_flush(&client));
    assert(stop_calls == 1);
    assert(flush_calls == 1);
    assert(client.state == RAOP_FLUSHED);
    assert(raop_session_flush(&client));   /* already flushed: stop only */
    assert(stop_calls == 2);
    assert(flush_calls == 1);

    /* raop_session_start_at re-anchors a flushed stream without stopping or
     * flushing again (the START half after a FLUSH). */
    reset_mocks();
    client.state = RAOP_FLUSHED;
    const uint64_t resume_ms = 1700000123000ULL;
    assert(raop_session_start_at(&client, resume_ms));
    assert(stop_calls == 0);
    assert(flush_calls == 0);
    assert(start_calls == 1);
    assert(last_start + TS2NTP(client.latency, client.sample_rate) ==
           unix_ms_to_ntp(resume_ms));

    client.state = RAOP_DOWN;
    assert(!raop_session_commit(&client, future_ms));
    assert(!raop_session_standby(&client));
    assert(!raop_session_flush(&client));
    assert(!raop_session_start_at(&client, future_ms));
    assert(!raop_session_pause(&client));
    assert(!raop_session_resume(&client));

    puts("RAOP command-only session tests passed");
    return 0;
}
