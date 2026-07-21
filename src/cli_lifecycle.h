#ifndef __CLI_LIFECYCLE_H_
#define __CLI_LIFECYCLE_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef void (*cli_lifecycle_destroy_fn)(void *client);

typedef struct {
    atomic_bool running;
    pthread_t command_thread;
    bool command_thread_started;
} cli_lifecycle_t;

#define CLI_LIFECYCLE_INITIALIZER {           \
    .running = true,                           \
    .command_thread_started = false,           \
}

static inline bool cli_lifecycle_is_running(const cli_lifecycle_t *lifecycle)
{
    return atomic_load_explicit(&lifecycle->running, memory_order_acquire);
}

static inline void cli_lifecycle_request_stop(cli_lifecycle_t *lifecycle)
{
    atomic_store_explicit(&lifecycle->running, false, memory_order_release);
}

static inline int cli_lifecycle_start_command_thread(
    cli_lifecycle_t *lifecycle, void *(*entry)(void *), void *arg)
{
    int result = pthread_create(&lifecycle->command_thread, NULL, entry, arg);
    if (result == 0) lifecycle->command_thread_started = true;
    return result;
}

static inline bool cli_lifecycle_stop_command_thread(
    cli_lifecycle_t *lifecycle)
{
    cli_lifecycle_request_stop(lifecycle);
    if (!lifecycle->command_thread_started) return true;
    if (pthread_equal(pthread_self(), lifecycle->command_thread)) return false;
    if (pthread_join(lifecycle->command_thread, NULL) != 0) return false;
    lifecycle->command_thread_started = false;
    return true;
}

/*
 * Retirement order is deliberate: stop admission, unpublish the client, join
 * the sole command thread, then destroy. A command that already loaded the
 * pointer may finish, but destruction cannot race it. A self-retirement never
 * joins or destroys and therefore cannot deadlock or free its active client.
 */
static inline bool cli_lifecycle_retire_client(
    cli_lifecycle_t *lifecycle, _Atomic(void *) *published_client,
    void *owned_client, cli_lifecycle_destroy_fn destroy_client)
{
    cli_lifecycle_request_stop(lifecycle);
    void *published = atomic_exchange_explicit(
        published_client, NULL, memory_order_acq_rel);
    if (!cli_lifecycle_stop_command_thread(lifecycle)) return false;
    if (published && owned_client && published != owned_client) return false;

    void *client = owned_client ? owned_client : published;
    if (client && destroy_client) destroy_client(client);
    return true;
}

#endif /* __CLI_LIFECYCLE_H_ */
