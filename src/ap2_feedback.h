#ifndef __AP2_FEEDBACK_H_
#define __AP2_FEEDBACK_H_

#include <stdbool.h>
#include <pthread.h>

typedef bool (*ap2_periodic_callback_t)(void *arg);

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    ap2_periodic_callback_t callback;
    void *arg;
    unsigned interval_ms;
    bool stop;
    bool started;
} ap2_periodic_worker_t;

bool ap2_periodic_worker_init(ap2_periodic_worker_t *worker,
                              unsigned interval_ms,
                              ap2_periodic_callback_t callback, void *arg);
bool ap2_periodic_worker_start(ap2_periodic_worker_t *worker);
void ap2_periodic_worker_stop(ap2_periodic_worker_t *worker);
void ap2_periodic_worker_destroy(ap2_periodic_worker_t *worker);

#endif
