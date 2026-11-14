/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RAOP_RTSP_H
#define RAOP_RTSP_H

#include <stdbool.h>

/* The RTSP client libraop keeps private inside a raopcl. raop_client.c is
 * built with rtspcl_create renamed to raop_rtsp_create (see the Makefile), so
 * each raopcl_create() records the RTSP client it makes, for its owner to
 * claim with raop_rtsp_claim() right after. The handle lives until
 * raopcl_destroy(). */
struct rtspcl_s;

/* rtspcl_create() for raop_client.c: creates the client and records it. */
struct rtspcl_s *raop_rtsp_create(char *useragent);

/* Take the RTSP client recorded by the last raopcl_create(), or NULL. */
struct rtspcl_s *raop_rtsp_claim(void);

/* End the receiver session now: send TEARDOWN, wait at most a second for the
 * answer, then close the connection. raopcl_disconnect() later sends nothing
 * further on it. Returns whether the receiver accepted the TEARDOWN. */
bool raop_rtsp_teardown(struct rtspcl_s *rtsp);

/* Whether the receiver closed the RTSP connection in an orderly way (EOF),
 * as opposed to it being open or reset. */
bool raop_rtsp_peer_closed(struct rtspcl_s *rtsp);

#endif /* RAOP_RTSP_H */
