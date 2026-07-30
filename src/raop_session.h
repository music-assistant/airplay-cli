#ifndef RAOP_SESSION_H
#define RAOP_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "ap2_session.h"

struct raopcl_s;

/* Commit the first START on an existing RAOP connection: a feasible audible
 * first-sample unix time is scheduled exactly; an infeasible one (or 0) is
 * corrected forward to now + the minimum RAOP lead, with the true instant
 * reported in *at_unix_ms so the caller can verify and log. A currently
 * streaming stream is flushed before re-anchoring. */
ap2_commit_result_t raop_session_commit(struct raopcl_s *client,
                                        uint64_t start_unix_ms,
                                        uint64_t *at_unix_ms);

/* Re-anchor a flushed stream to a new audible start (the START after a FLUSH)
 * under the same contract. Unlike commit it does not stop/flush again — the
 * receiver buffer was already discarded by raop_session_flush — so no
 * crypto/sequence state is reset. */
ap2_commit_result_t raop_session_start_at(struct raopcl_s *client,
                                          uint64_t start_unix_ms,
                                          uint64_t *at_unix_ms);

/* Discard the receiver's buffered audio in place for a warm seek, keeping the
 * connection; the next START re-anchors via raop_session_start_at. */
bool raop_session_flush(struct raopcl_s *client);

/* Silence playback while preserving the RAOP connection for a later START. */
bool raop_session_standby(struct raopcl_s *client);

/* Pause and retain the current stream's backlog for ACTION=PLAY. */
bool raop_session_pause(struct raopcl_s *client);

/* Resume a paused stream without discarding libraop's retained backlog. */
bool raop_session_resume(struct raopcl_s *client);

#endif /* RAOP_SESSION_H */
