/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "raop_session.h"

#include "raop_client.h"

#define RAOP_SESSION_MIN_START_LEAD_MS 200

static uint64_t unix_ms_to_ntp(uint64_t ms)
{
    return ((ms / 1000) << 32) | (((ms % 1000) << 32) / 1000);
}

static uint64_t ntp_to_unix_ms(uint64_t ntp)
{
    return (ntp >> 32) * 1000ULL + (((ntp & 0xFFFFFFFFULL) * 1000ULL) >> 32);
}

/* Resolve a commanded start: a feasible nonzero request is honored exactly; a
 * request behind the feasibility floor is corrected FORWARD to the floor
 * (audio always flows), and a request of 0 picks the floor. *at_unix_ms
 * always receives the true audible instant so the caller can verify. */
static void resolve_start(uint64_t start_unix_ms, uint64_t *audible_ntp,
                          uint64_t *at_unix_ms)
{
    uint64_t lead = MS2NTP(RAOP_SESSION_MIN_START_LEAD_MS);
    uint64_t floor = raopcl_get_ntp(NULL) + lead;
    uint64_t requested = start_unix_ms ? unix_ms_to_ntp(start_unix_ms) : 0;
    if (start_unix_ms && requested >= floor) {
        *audible_ntp = requested;
        if (at_unix_ms) *at_unix_ms = start_unix_ms;
        return;
    }
    /* Corrected forward with one extra lead of slack (the floor moves with
     * the wall clock, so a corrective retry at a bare floor would chase it);
     * a request of 0 takes the floor directly. */
    *audible_ntp = start_unix_ms ? floor + lead : floor;
    if (at_unix_ms) *at_unix_ms = ntp_to_unix_ms(*audible_ntp);
}

ap2_commit_result_t raop_session_commit(struct raopcl_s *client,
                                        uint64_t start_unix_ms,
                                        uint64_t *at_unix_ms)
{
    if (!client) return AP2_COMMIT_FAILED;

    raop_state_t state = raopcl_state(client);
    if (state != RAOP_STREAMING && state != RAOP_FLUSHED)
        return AP2_COMMIT_FAILED;

    uint64_t audible_ntp = 0;
    resolve_start(start_unix_ms, &audible_ntp, at_unix_ms);

    /* A commanded (re)start discards the old backlog. pause() would preserve
     * one latency window and replay it before the new track. */
    raopcl_stop(client);
    if (state == RAOP_STREAMING) {
        if (!raopcl_flush(client)) return AP2_COMMIT_FAILED;
    }

    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp)
               ? AP2_COMMIT_OK
               : AP2_COMMIT_FAILED;
}

ap2_commit_result_t raop_session_start_at(struct raopcl_s *client,
                                          uint64_t start_unix_ms,
                                          uint64_t *at_unix_ms)
{
    if (!client) return AP2_COMMIT_FAILED;
    /* Only valid after a flush (seek/standby): re-anchoring a live stream
     * would replay the receiver's retained backlog against the new timeline.
     * Rejecting STREAMING makes a START without a preceding FLUSH fail fast. */
    raop_state_t state = raopcl_state(client);
    if (state != RAOP_FLUSHED) return AP2_COMMIT_FAILED;

    uint64_t audible_ntp = 0;
    resolve_start(start_unix_ms, &audible_ntp, at_unix_ms);

    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp)
               ? AP2_COMMIT_OK
               : AP2_COMMIT_FAILED;
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

    uint64_t audible_ntp =
        raopcl_get_ntp(NULL) + MS2NTP(RAOP_SESSION_MIN_START_LEAD_MS);
    uint64_t latency_ntp =
        TS2NTP(raopcl_latency(client), raopcl_sample_rate(client));
    return raopcl_start_at(client, audible_ntp - latency_ntp);
}
