#include "ap2_feedback.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "ap2_io.h"

static struct timespec ap2_after_ms(uint64_t delay_ms)
{
    struct timespec deadline;
#ifdef __APPLE__
    deadline.tv_sec = 0;
    deadline.tv_nsec = 0;
#else
    clock_gettime(CLOCK_MONOTONIC, &deadline);
#endif
    deadline.tv_sec += (time_t)(delay_ms / 1000);
    deadline.tv_nsec += (long)(delay_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void *ap2_periodic_worker_main(void *arg)
{
    ap2_periodic_worker_t *worker = arg;
    uint64_t next_tick = ap2_io_monotonic_ms() + worker->interval_ms;

    pthread_mutex_lock(&worker->lock);
    while (!worker->stop) {
        uint64_t now = ap2_io_monotonic_ms();
        if (now < next_tick) {
            uint64_t delay = next_tick - now;
            if (delay > 100) delay = 100;
            struct timespec deadline = ap2_after_ms(delay);
#ifdef __APPLE__
            pthread_cond_timedwait_relative_np(
                &worker->wake, &worker->lock, &deadline);
#else
            pthread_cond_timedwait(&worker->wake, &worker->lock, &deadline);
#endif
            continue;
        }

        pthread_mutex_unlock(&worker->lock);
        bool keep_running = worker->callback(worker->arg);
        pthread_mutex_lock(&worker->lock);
        if (!keep_running) worker->stop = true;

        now = ap2_io_monotonic_ms();
        next_tick += worker->interval_ms;
        if (next_tick <= now) next_tick = now + worker->interval_ms;
    }
    pthread_mutex_unlock(&worker->lock);
    return NULL;
}

bool ap2_periodic_worker_init(ap2_periodic_worker_t *worker,
                              unsigned interval_ms,
                              ap2_periodic_callback_t callback, void *arg)
{
    if (!worker || !interval_ms || !callback) return false;
    memset(worker, 0, sizeof(*worker));
    worker->interval_ms = interval_ms;
    worker->callback = callback;
    worker->arg = arg;
    if (pthread_mutex_init(&worker->lock, NULL) != 0) return false;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&worker->lock);
        return false;
    }
#ifndef __APPLE__
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&worker->lock);
        return false;
    }
#endif
    if (pthread_cond_init(&worker->wake, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&worker->lock);
        return false;
    }
    pthread_condattr_destroy(&attr);
    return true;
}

bool ap2_periodic_worker_start(ap2_periodic_worker_t *worker)
{
    if (!worker || worker->started) return false;
    worker->stop = false;
    int error = pthread_create(&worker->thread, NULL,
                               ap2_periodic_worker_main, worker);
    if (error != 0) {
        errno = error;
        return false;
    }
    worker->started = true;
    return true;
}

void ap2_periodic_worker_stop(ap2_periodic_worker_t *worker)
{
    if (!worker || !worker->started) return;
    pthread_mutex_lock(&worker->lock);
    worker->stop = true;
    pthread_cond_broadcast(&worker->wake);
    pthread_mutex_unlock(&worker->lock);
    pthread_join(worker->thread, NULL);
    worker->started = false;
}

void ap2_periodic_worker_destroy(ap2_periodic_worker_t *worker)
{
    if (!worker) return;
    ap2_periodic_worker_stop(worker);
    pthread_cond_destroy(&worker->wake);
    pthread_mutex_destroy(&worker->lock);
}
