/*
 * Persistent session - single-stream flush-and-refill engine
 *
 * Separates connection lifetime from media lifetime: the binary connects once
 * and streams a single, persistent PCM input (the process stdin) over that
 * connection for the whole process lifetime. Seek/next never re-connect and
 * never re-open the input; instead the LIVE stream is flushed and refilled in
 * place. The connection outlives each track (bounded by the idle timeout).
 *
 * One reader thread drains the input fd into one ring buffer for the whole
 * session. The audio loop only ever calls ap2_session_read()/poll(). Start
 * times are COMMANDED and VERIFIED: playback (and each warm re-anchor)
 * begins on an explicit START, and the transport is the sole authority on
 * which instants are feasible. A feasible START_UNIX_MS is scheduled EXACTLY
 * (inserting silence as the lane requires); an infeasible one is corrected
 * FORWARD to the earliest feasible instant — audio always flows, never
 * silently misplaced or refused — and the ack reports the true instant so
 * the caller can verify, log, and re-align a group by re-STARTing every
 * member at the largest reported instant. START_UNIX_MS=0 means "as soon as
 * possible": the transport picks and the ack reports the pick.
 *
 *   START(start time)  -> first call: full session start; a call after a FLUSH
 *                         re-bases the frozen anchor and resumes from the ring
 *   FLUSH              -> stop sending, RTSP-FLUSH the receiver, discard the
 *                         ring, drain the input to EAGAIN; stream goes
 *                         idle-primed (keeps buffering, sends nothing) until the
 *                         next START. Emits `[STATUS] flushed`.
 *   STANDBY            -> stop playback, keep the connection warm
 *   END                -> real teardown (DISCONNECT / idle timeout)
 *
 * The input is never re-opened across a FLUSH: the caller (MA) restarts only
 * the per-seek transcoder feeding the same persistent fd, so sequence numbers
 * and audio nonces are never reset within the connection and crypto state stays
 * valid across the boundary. The media timeline re-bases per START.
 *
 * Command pipe verbs (newline-terminated KEY=VALUE, existing verbs unchanged):
 *   START_UNIX_MS=<ms|0>      audible start instant; 0 = ASAP at the minimum
 *                             warm lead
 *   ACTION=START|FLUSH|STANDBY|DISCONNECT
 *
 * Status lines (stderr):
 *   [STATUS] playing elapsed_ms=<ms>   (elapsed since the current START anchor)
 *   [STATUS] started requested_unix_ms=<ms> at_unix_ms=<ms>
 *                                      (START scheduled; at == requested when
 *                                       the instant was feasible, the
 *                                       corrected-forward earliest instant
 *                                       otherwise — the caller compares the
 *                                       two, logs any mismatch loudly, and
 *                                       can re-align a group by re-STARTing
 *                                       every member at max(at). requested=0
 *                                       means the transport picked.)
 *   [STATUS] flushed                   (FLUSH completed; stream idle-primed)
 *   [STATUS] eof                       (input stream ended)
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
    AP2_SESSION_IDLE = 0,     /* connected; not sending (pre-start/post-flush) */
    AP2_SESSION_PLAYING,      /* streaming the ring to the device */
    AP2_SESSION_STANDBY,      /* stopped on request; connection kept warm */
    AP2_SESSION_ENDED,        /* DISCONNECT requested or idle timeout hit */
} ap2_session_state_t;

struct ap2_session_s;

/* Outcome of a commanded START at the transport. */
typedef enum {
    AP2_COMMIT_OK = 0,        /* scheduled; *at_unix_ms holds the instant */
    AP2_COMMIT_FAILED,        /* terminal transport failure */
} ap2_commit_result_t;

/*
 * Callbacks the session engine invokes from its own threads. All are required
 * except `warm_head_unix_ms`. `commit` performs the START transport work on a
 * LIVE connection: on the first start it begins the session; after a FLUSH it
 * re-bases the timeline. The commanded instant is honored exactly when
 * feasible (padding silence or re-anchoring as the transport's lane
 * requires); an instant the transport cannot honor — behind its feasibility
 * floor — is CORRECTED FORWARD to the earliest feasible instant instead of
 * being silently played wrong or refused: audio always flows, and
 * *at_unix_ms always reports the true scheduled instant so the caller can
 * verify the contract, re-align a group with a corrective START, and log the
 * mismatch. A `start_unix_ms` of 0 asks the transport to pick (earliest).
 * AP2_COMMIT_FAILED is terminal (the caller then ends the session so MA can
 * fall back cold). `flush` discards the receiver's buffered audio in place
 * (RTSP FLUSH / libraop flush), keeping the session and timeline continuity;
 * it returns false when the receiver did not accept the flush.
 * `warm_head_unix_ms` (optional) reports the audible instant of the delivery
 * head frozen by the flush (unix ms; 0 = no constraint); it rides the
 * `[STATUS] flushed` ack as the caller's anchor hint.
 */
typedef struct {
    void (*quiesce)(void *transport);    /* pause audio sends across a command */
    bool (*flush)(void *transport);      /* RTSP-flush receiver, keep session */
    ap2_commit_result_t (*commit)(void *transport, uint64_t start_unix_ms,
                                  uint64_t *at_unix_ms);
    void (*resume)(void *transport);     /* resume sends after the command */
    void (*stop)(void *transport);       /* silence the receiver, keep session */
    void (*status)(const char *line);    /* emit one [STATUS] line */
    uint64_t (*warm_head_unix_ms)(void *transport);  /* optional (may be NULL) */
    void *transport;
} ap2_session_ops_t;

/*
 * Create the session engine and start its single input reader.
 *
 * :param ops: transport quiesce/flush/commit/resume/stop + status callbacks.
 * :param byte_rate: PCM byte rate of the input format (ring sizing/timing).
 * :param ready_bytes: buffered PCM required before the one-shot audio status.
 * :param idle_timeout_ms: end the session after this long idle (no playing
 *                         stream), an orphan safety net; 0 disables it.
 * :param input_fd: PCM input descriptor (normally the process stdin). The
 *                  engine duplicates it, drives it non-blocking, and reads it
 *                  for the whole session; the caller keeps ownership of its own
 *                  descriptor.
 */
struct ap2_session_s *ap2_session_create(const ap2_session_ops_t *ops,
                                         unsigned byte_rate,
                                         unsigned ready_bytes,
                                         int idle_timeout_ms, int input_fd);
void ap2_session_destroy(struct ap2_session_s *s);

/* Command-pipe entry points, serialized by the single cmdpipe thread. Invalid
 * transitions are rejected with a [STATUS]/log line and return false.
 * `start` forwards the commit outcome: on AP2_COMMIT_OK *at_unix_ms holds the
 * true scheduled audible instant (the commanded one, or the corrected-forward
 * earliest feasible instant — the caller compares and reacts). */
ap2_commit_result_t ap2_session_start(struct ap2_session_s *s,
                                      uint64_t start_unix_ms,
                                      uint64_t *at_unix_ms);
bool ap2_session_flush(struct ap2_session_s *s);
bool ap2_session_standby(struct ap2_session_s *s);
void ap2_session_end(struct ap2_session_s *s);

/* Audio-loop entry points. While the input remains open, `read` retains partial
 * data in the ring until exactly len bytes are available. It blocks up to
 * timeout_ms; returns 0 when that deadline passes, a final short read at EOF,
 * -1 once the input is fully consumed, and -2 when the session ended. The loop
 * calls `poll` once per iteration so the engine can run the idle timeout. */
int ap2_session_read(struct ap2_session_s *s, uint8_t *buf, int len,
                     int timeout_ms);
void ap2_session_poll(struct ap2_session_s *s);

ap2_session_state_t ap2_session_state(struct ap2_session_s *s);
/* Monotonic counter bumped on every START; the audio loop resets its
 * per-track elapsed/EOF bookkeeping when it changes. */
uint64_t ap2_session_epoch(struct ap2_session_s *s);

#endif /* __AP2_SESSION_H_ */
