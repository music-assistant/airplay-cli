#include "raop_session.h"

#include "raop_client.h"

#define RAOP_SESSION_MIN_START_LEAD_MS 200

static uint64_t commanded_audible_ntp(uint64_t start_unix_ms)
{
    uint64_t start_ntp = start_unix_ms
        ? ((start_unix_ms / 1000) << 32) |
              (((start_unix_ms % 1000) << 32) / 1000)
        : 0;
    uint64_t min_start =
        raopcl_get_ntp(NULL) + MS2NTP(RAOP_SESSION_MIN_START_LEAD_MS);
    return start_ntp < min_start ? min_start : start_ntp;
}

bool raop_session_commit(struct raopcl_s *client, uint64_t start_unix_ms)
{
    if (!client) return false;

    raop_state_t state = raopcl_state(client);
    if (state != RAOP_STREAMING && state != RAOP_FLUSHED) return false;
    /* Generation switches discard old backlog. pause() would preserve one
     * latency window and replay it before the new generation. */
    raopcl_stop(client);
    if (state == RAOP_STREAMING) {
        if (!raopcl_flush(client)) return false;
    }

    uint64_t audible_ntp = commanded_audible_ntp(start_unix_ms);
    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp);
}

bool raop_session_start_at(struct raopcl_s *client, uint64_t start_unix_ms)
{
    if (!client) return false;
    raop_state_t state = raopcl_state(client);
    if (state != RAOP_STREAMING && state != RAOP_FLUSHED) return false;

    uint64_t audible_ntp = commanded_audible_ntp(start_unix_ms);
    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp);
}

bool raop_session_flush(struct raopcl_s *client)
{
    if (!client) return false;
    raop_state_t state = raopcl_state(client);
    if (state != RAOP_STREAMING && state != RAOP_FLUSHED) return false;
    raopcl_stop(client);
    if (state == RAOP_FLUSHED) return true;
    return raopcl_flush(client);
}

bool raop_session_standby(struct raopcl_s *client)
{
    return raop_session_flush(client);
}

bool raop_session_pause(struct raopcl_s *client)
{
    if (!client) return false;
    raop_state_t state = raopcl_state(client);
    if (state == RAOP_FLUSHED) return true;
    if (state != RAOP_STREAMING) return false;
    raopcl_pause(client);
    return raopcl_flush(client);
}

bool raop_session_resume(struct raopcl_s *client)
{
    if (!client) return false;
    raop_state_t state = raopcl_state(client);
    if (state != RAOP_FLUSHED && state != RAOP_STREAMING) return false;

    uint64_t audible_ntp = commanded_audible_ntp(0);
    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp);
}
