/*
 * Persistent session - generation state machine
 *
 * Separates connection lifetime from media lifetime: the binary connects once
 * and plays a sequence of numbered media GENERATIONS over that connection, so
 * seek/next/resume never pay the protocol setup cost again. Every generation is
 * supplied through the command pipe, including generation 0; the connection
 * outlives each generation (bounded by the idle timeout) awaiting PREPARE.
 *
 * Start times are COMMANDED, not configured: every generation (including
 * generation 0) starts on an explicit START, which the caller sends after
 * seeing `connected` and `primed`. The caller therefore never has to guess a
 * safe setup lead in advance - it picks the start once the expensive work has
 * already succeeded, and a group start is simply the same START_UNIX_MS sent
 * to every primed member. Starts of 0 or in the past clamp to now + the
 * minimum warm lead.
 *
 *   PREPARE(gen, audio pipe, position)  ->  ready  ->  primed
 *   START(gen, start time)              ->  flush old + swap + anchor -> playing
 *   STANDBY                             ->  stop playback, keep the connection warm
 *   DISCONNECT                          ->  real teardown
 *
 * The next generation's audio arrives on its OWN pipe and is drained into a
 * staging ring while the current generation keeps playing, so source and DSP
 * preparation cost no audible time. The commit is the measured-fast warm
 * flush: RTSP FLUSH (+/- 5 ms) plus a re-based frozen anchor line, working
 * down to a 150 ms lead on tested receivers. Sequence numbers and audio
 * nonces are never reset within the connection; the media timeline re-bases
 * per generation.
 *
 * Command pipe verbs (newline-terminated KEY=VALUE, existing verbs unchanged):
 *   GENERATION=<n>            select the generation the next ACTION applies to
 *   AUDIO=<path>              FIFO carrying this generation's PCM (PREPARE)
 *   POSITION_MS=<ms>          media position of the pipe's first byte
 *   START_UNIX_MS=<ms|0>      audible start instant; 0 = ASAP at the minimum
 *                             warm lead
 *   ACTION=PREPARE|START|FLUSH|STANDBY|DISCONNECT
 *
 * Status lines (stderr, generation-tagged):
 *   [STATUS] ready generation=<n>
 *   [STATUS] primed generation=<n> prefill_ms=<ms>
 *   [STATUS] playing generation=<n> elapsed_ms=<ms>
 *   [STATUS] input_eof generation=<n>
 *   [STATUS] eof generation=<n>
 *   [STATUS] idle_timeout
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_SESSION_H_
#define __AP2_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

/* Overall session-mode state. */
typedef enum {
    AP2_SESSION_IDLE = 0,     /* connected; no generation playing or staged */
    AP2_SESSION_PREPARING,    /* staging ring filling for the next generation */
    AP2_SESSION_PLAYING,      /* a generation is streaming */
    AP2_SESSION_STANDBY,      /* stopped on request; connection kept warm */
    AP2_SESSION_ENDED,        /* DISCONNECT requested or idle timeout hit */
} ap2_session_state_t;

/* One staged/active generation. Audio comes from opening `audio_path`, normally
 * a FIFO created by the caller; "-" duplicates process stdin and is limited to
 * one active generation because duplicated descriptors share the same stream. */
typedef struct {
    uint64_t number;          /* caller-assigned generation number */
    const char *audio_path;   /* FIFO delivering this generation's PCM */
    uint64_t position_ms;     /* media position of byte zero (progress base) */
} ap2_generation_t;

struct ap2_session_s;

/*
 * Callbacks the session engine invokes from its own threads. All are
 * required. `commit` performs the transport work of a generation switch on a
 * LIVE connection: discard receiver-buffered audio and re-base the timeline
 * so the next written frame is audible at `start_unix_ms` (or now + the
 * minimum warm lead when 0). Returns false when the transport failed
 * terminally (the caller then ends the session so MA can fall back cold).
 */
typedef struct {
    void (*quiesce)(void *transport);    /* pause audio sends across commit */
    bool (*commit)(void *transport, uint64_t start_unix_ms);
    void (*resume)(void *transport);     /* resume sends after active swap */
    void (*stop)(void *transport);      /* silence the receiver, keep session */
    void (*status)(const char *line);   /* emit one [STATUS] line */
    void *transport;
} ap2_session_ops_t;

/*
 * Create the session engine.
 *
 * :param ops: transport quiesce/commit/resume + status emit callbacks.
 * :param byte_rate: PCM byte rate of the input format (ring sizing/timing).
 * :param prefill_ms: staging ring fill level that emits `primed`.
 * :param idle_timeout_ms: end the session after this long without a playing
 *                         or preparing generation (orphan safety net).
 */
struct ap2_session_s *ap2_session_create(const ap2_session_ops_t *ops,
                                         unsigned byte_rate, int prefill_ms,
                                         int idle_timeout_ms);
void ap2_session_destroy(struct ap2_session_s *s);

/* Command-pipe entry points, serialized by the single cmdpipe thread. Invalid
 * transitions are rejected with a [STATUS]/log line and return false. */
bool ap2_session_prepare(struct ap2_session_s *s, const ap2_generation_t *gen);
bool ap2_session_start(struct ap2_session_s *s, uint64_t generation,
                       uint64_t start_unix_ms);
bool ap2_session_flush(struct ap2_session_s *s, uint64_t generation);
bool ap2_session_standby(struct ap2_session_s *s);
void ap2_session_end(struct ap2_session_s *s);

/* Audio-loop entry points. `read` returns PCM from the ACTIVE generation's
 * ring (blocking up to timeout_ms; 0 on generation boundary/underrun, -1 on
 * end-of-generation, -2 when the session ended). The loop calls `poll` once
 * per iteration so the engine can run timeouts and complete commits. */
int ap2_session_read(struct ap2_session_s *s, uint8_t *buf, int len,
                     int timeout_ms);
void ap2_session_poll(struct ap2_session_s *s);

ap2_session_state_t ap2_session_state(struct ap2_session_s *s);
uint64_t ap2_session_active_generation(struct ap2_session_s *s);
/* Media position of the active generation (position_ms + played ms). */
uint64_t ap2_session_position_ms(struct ap2_session_s *s);

#endif /* __AP2_SESSION_H_ */
