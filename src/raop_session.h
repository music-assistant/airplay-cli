#ifndef RAOP_SESSION_H
#define RAOP_SESSION_H

#include <stdbool.h>
#include <stdint.h>

struct raopcl_s;

/* Commit a commanded generation on an existing RAOP connection. The command
 * carries the audible first-sample unix time; 0 or stale values use a safe
 * ASAP lead. A currently streaming generation is flushed before re-anchoring. */
bool raop_session_commit(struct raopcl_s *client, uint64_t start_unix_ms);

/* Silence playback while preserving the RAOP connection for a later START. */
bool raop_session_standby(struct raopcl_s *client);

/* Pause and retain the current generation's backlog for ACTION=PLAY. */
bool raop_session_pause(struct raopcl_s *client);

/* Resume a paused generation without discarding libraop's retained backlog. */
bool raop_session_resume(struct raopcl_s *client);

#endif /* RAOP_SESSION_H */
