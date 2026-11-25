/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "raop_rtsp.h"

#include <errno.h>
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
/* The client whose connection ended without the receiver closing it (a reset,
 * or our own TEARDOWN): a later EOF on it is not the receiver's doing. */
static _Atomic(struct rtspcl_s *) g_ended = NULL;

static void *teardown_main(void *arg)
{
    teardown_job_t *job = arg;
    job->accepted = rtspcl_teardown(job->rtsp);
    atomic_store(&job->done, true);
    return NULL;
}

struct rtspcl_s *raop_rtsp_create(const char *useragent)
{
    struct rtspcl_s *rtsp = rtspcl_create(useragent);
    /* A fresh client can reuse the address of an ended one. */
    struct rtspcl_s *ended = rtsp;
    atomic_compare_exchange_strong(&g_ended, &ended, NULL);
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
    if (fd < 0 || atomic_load(&g_ended) == rtsp) return false;

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
    /* Ends a TEARDOWN still waiting for its turn or its answer, and nothing
     * more goes out on the connection: libraop's exit-time FLUSH and TEARDOWN
     * fail on it.
     * The client itself is left to libraop, whose exit path joins the threads
     * that also use it (the keepalive) before it disconnects. */
    atomic_store(&g_ended, rtsp);
    shutdown(fd, SHUT_RDWR);
    if (started) pthread_join(thread, NULL);
    return job.accepted;
}

bool raop_rtsp_peer_closed(struct rtspcl_s *rtsp)
{
    int fd = rtsp ? rtspcl_get_serv_sock(rtsp) : -1;
    if (fd < 0 || atomic_load(&g_ended) == rtsp) return false;
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        /* A reset is reported once and later reads see EOF, so it is
         * remembered to keep it a failure rather than the receiver closing. */
        atomic_store(&g_ended, rtsp);
        return false;
    }
    return n == 0;
}
