/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "raop_rtsp.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>

#include "platform.h"
#include "cross_log.h"
#include "cross_util.h"
#include "rtsp_client.h"
#include "ap2_io.h"

/* Defined by rtsp_client.c but left out of its header. */
int rtspcl_get_serv_sock(struct rtspcl_s *p);
bool rtspcl_teardown(struct rtspcl_s *p);

#define RAOP_RTSP_TEARDOWN_TIMEOUT_MS 1000

extern log_level *loglevel;

typedef struct {
    struct rtspcl_s *rtsp;
    atomic_bool done;
    bool accepted;
} teardown_job_t;

static _Atomic(struct rtspcl_s *) g_created = NULL;

static void *teardown_main(void *arg)
{
    teardown_job_t *job = arg;
    job->accepted = rtspcl_teardown(job->rtsp);
    atomic_store(&job->done, true);
    return NULL;
}

struct rtspcl_s *raop_rtsp_create(char *useragent)
{
    struct rtspcl_s *rtsp = rtspcl_create(useragent);
    atomic_store(&g_created, rtsp);
    return rtsp;
}

struct rtspcl_s *raop_rtsp_claim(void)
{
    return atomic_exchange(&g_created, NULL);
}

bool raop_rtsp_teardown(struct rtspcl_s *rtsp)
{
    int fd = rtsp ? rtspcl_get_serv_sock(rtsp) : -1;
    if (fd < 0) return false;

    /* libraop waits up to 10 s for the answer, so the request runs on its own
     * thread and is given up on after the timeout. */
    teardown_job_t job = {.rtsp = rtsp};
    atomic_init(&job.done, false);
    pthread_t thread;
    bool started = pthread_create(&thread, NULL, teardown_main, &job) == 0;
    if (started) {
        uint64_t deadline =
            ap2_io_monotonic_ms() + RAOP_RTSP_TEARDOWN_TIMEOUT_MS;
        while (!atomic_load(&job.done) && ap2_io_monotonic_ms() < deadline)
            usleep(1000);
    } else {
        LOG_ERROR("Cannot start the RTSP TEARDOWN thread");
    }
    /* Ends a TEARDOWN still waiting for its answer, and leaves nothing for
     * rtspcl_disconnect() to send a second one on: it only closes the socket
     * and clears the handle, which turns libraop's own exit-time FLUSH and
     * TEARDOWN into no-ops. */
    shutdown(fd, SHUT_RDWR);
    if (started) pthread_join(thread, NULL);
    rtspcl_disconnect(rtsp);
    return job.accepted;
}

bool raop_rtsp_peer_closed(struct rtspcl_s *rtsp)
{
    int fd = rtsp ? rtspcl_get_serv_sock(rtsp) : -1;
    char byte;
    return fd >= 0 && recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}
