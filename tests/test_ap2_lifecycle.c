#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cross_log.h"
#include "ap2_client.h"
#include "cli_lifecycle.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;
log_level util_loglevel = lSILENCE;
log_level raop_loglevel = lSILENCE;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

typedef struct {
    cli_lifecycle_t lifecycle;
    _Atomic(void *) published;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool api_entered;
    bool release_api;
    bool worker_exited;
    bool destroy_called;
    bool destroyed_before_exit;
    bool use_after_destroy;
    bool retire_result;
} cli_retire_test_t;

static void *blocked_command_worker(void *arg)
{
    cli_retire_test_t *test = arg;
    while (cli_lifecycle_is_running(&test->lifecycle)) {
        cli_retire_test_t *client = atomic_load_explicit(
            &test->published, memory_order_acquire);
        if (!client) {
            sched_yield();
            continue;
        }

        pthread_mutex_lock(&test->lock);
        test->api_entered = true;
        pthread_cond_broadcast(&test->changed);
        while (!test->release_api)
            pthread_cond_wait(&test->changed, &test->lock);
        if (test->destroy_called) test->use_after_destroy = true;
        pthread_mutex_unlock(&test->lock);
        break;
    }

    pthread_mutex_lock(&test->lock);
    test->worker_exited = true;
    pthread_cond_broadcast(&test->changed);
    pthread_mutex_unlock(&test->lock);
    return NULL;
}

static void fake_client_destroy(void *arg)
{
    cli_retire_test_t *test = arg;
    pthread_mutex_lock(&test->lock);
    if (!test->worker_exited) test->destroyed_before_exit = true;
    test->destroy_called = true;
    pthread_cond_broadcast(&test->changed);
    pthread_mutex_unlock(&test->lock);
}

static void *retire_client_worker(void *arg)
{
    cli_retire_test_t *test = arg;
    test->retire_result = cli_lifecycle_retire_client(
        &test->lifecycle, &test->published, test, fake_client_destroy);
    return NULL;
}

static bool wait_for_retirement_start(cli_retire_test_t *test)
{
    for (int i = 0; i < 1000000; i++) {
        if (!cli_lifecycle_is_running(&test->lifecycle) &&
            atomic_load_explicit(
                &test->published, memory_order_acquire) == NULL)
            return true;
        sched_yield();
    }
    return false;
}

static bool test_command_thread_retirement(void)
{
    cli_retire_test_t test = {
        .lifecycle = CLI_LIFECYCLE_INITIALIZER,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    atomic_init(&test.published, &test);

    CHECK(cli_lifecycle_start_command_thread(
              &test.lifecycle, blocked_command_worker, &test) == 0);
    pthread_mutex_lock(&test.lock);
    while (!test.api_entered)
        pthread_cond_wait(&test.changed, &test.lock);
    pthread_mutex_unlock(&test.lock);

    pthread_t retire_thread;
    CHECK(pthread_create(
              &retire_thread, NULL, retire_client_worker, &test) == 0);
    CHECK(wait_for_retirement_start(&test));

    pthread_mutex_lock(&test.lock);
    CHECK(!test.destroy_called);
    CHECK(!test.worker_exited);
    test.release_api = true;
    pthread_cond_broadcast(&test.changed);
    pthread_mutex_unlock(&test.lock);

    CHECK(pthread_join(retire_thread, NULL) == 0);
    CHECK(test.retire_result);
    CHECK(test.worker_exited);
    CHECK(test.destroy_called);
    CHECK(!test.destroyed_before_exit);
    CHECK(!test.use_after_destroy);
    pthread_cond_destroy(&test.changed);
    pthread_mutex_destroy(&test.lock);
    return true;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int creates;
    int attach_calls;
    int destroys;
    int status_calls;
    int serial_contentions;
    bool attach_entered;
    bool attach_active;
    bool release_attach;
    bool attach_result;
    bool status_entered;
    bool status_active;
    bool release_status;
    bool status_result;
    bool destroy_during_attach;
    bool destroy_during_status;
} mrp_hooks_t;

static void test_mrp_serial_contended(void *opaque)
{
    mrp_hooks_t *hooks = opaque;
    pthread_mutex_lock(&hooks->lock);
    hooks->serial_contentions++;
    pthread_cond_broadcast(&hooks->changed);
    pthread_mutex_unlock(&hooks->lock);
}

static void test_mrp_created(struct ap2_mrp_ctx *mrp, void *opaque)
{
    (void)mrp;
    mrp_hooks_t *hooks = opaque;
    pthread_mutex_lock(&hooks->lock);
    hooks->creates++;
    pthread_cond_broadcast(&hooks->changed);
    pthread_mutex_unlock(&hooks->lock);
}

static bool test_mrp_attach(struct ap2_mrp_ctx *mrp, int data_port,
                            uint64_t seed, void *opaque)
{
    (void)mrp;
    (void)data_port;
    (void)seed;
    mrp_hooks_t *hooks = opaque;
    pthread_mutex_lock(&hooks->lock);
    hooks->attach_calls++;
    hooks->attach_active = true;
    hooks->attach_entered = true;
    pthread_cond_broadcast(&hooks->changed);
    while (!hooks->release_attach)
        pthread_cond_wait(&hooks->changed, &hooks->lock);
    bool result = hooks->attach_result;
    hooks->attach_active = false;
    pthread_mutex_unlock(&hooks->lock);
    return result;
}

static void test_mrp_destroying(struct ap2_mrp_ctx *mrp, void *opaque)
{
    (void)mrp;
    mrp_hooks_t *hooks = opaque;
    pthread_mutex_lock(&hooks->lock);
    hooks->destroys++;
    if (hooks->attach_active) hooks->destroy_during_attach = true;
    if (hooks->status_active) hooks->destroy_during_status = true;
    pthread_cond_broadcast(&hooks->changed);
    pthread_mutex_unlock(&hooks->lock);
}

static bool test_mrp_is_connected(struct ap2_mrp_ctx *mrp, void *opaque)
{
    (void)mrp;
    mrp_hooks_t *hooks = opaque;
    pthread_mutex_lock(&hooks->lock);
    hooks->status_calls++;
    hooks->status_active = true;
    hooks->status_entered = true;
    pthread_cond_broadcast(&hooks->changed);
    while (!hooks->release_status)
        pthread_cond_wait(&hooks->changed, &hooks->lock);
    bool result = hooks->status_result;
    hooks->status_active = false;
    pthread_mutex_unlock(&hooks->lock);
    return result;
}

static struct ap2cl_s *create_test_client(void)
{
    static char auth[193];
    memset(auth, 'A', sizeof(auth) - 1);
    auth[sizeof(auth) - 1] = '\0';
    ap2_device_info_t device = {
        .name = "Lifecycle test",
        .hostname = "localhost",
        .address = "127.0.0.1",
        .port = 7000,
        .txt_records = "",
    };
    ap2_audio_format_t format = {
        .sample_rate = 44100,
        .bit_depth = 16,
        .channels = 2,
    };
    return ap2cl_create(
        &device, &format, auth, NULL, "0011223344556677", "1", 2000, 50);
}

static ap2cl_mrp_test_hooks_t make_hooks(mrp_hooks_t *hooks)
{
    ap2cl_mrp_test_hooks_t result = {
        .created = test_mrp_created,
        .attach = test_mrp_attach,
        .destroying = test_mrp_destroying,
        .is_connected = test_mrp_is_connected,
        .serial_contended = test_mrp_serial_contended,
        .opaque = hooks,
    };
    return result;
}

static bool test_connect_gate_and_normal_operation(void)
{
    mrp_hooks_t hooks = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .release_status = true,
        .status_result = true,
    };
    ap2cl_mrp_test_hooks_t test_hooks = make_hooks(&hooks);
    test_hooks.attach = NULL;
    ap2cl_test_set_mrp_hooks(&test_hooks);
    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    ap2cl_test_set_mrp_phase(client, false);

    static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
    ap2_mrp_artwork_info_t info;
    ap2_mrp_push_result_t artwork_push;
    CHECK(!ap2cl_set_metadata(client, "Connecting", "", "", 60));
    CHECK(!ap2cl_set_progress(client, 1, 60));
    CHECK(!ap2cl_set_artwork(
              client, "image/jpeg", (int)sizeof(jpeg), (const char *)jpeg,
              &info, &artwork_push));
    CHECK(info.result == AP2_MRP_ARTWORK_NOT_APPLICABLE);
    CHECK(artwork_push.overall_status == -1);
    CHECK(artwork_push.nowplaying_status == 0);
    CHECK(!ap2cl_clear_mrp_artwork(client, &artwork_push));
    CHECK(ap2cl_mrp_register(client) == -1);
    ap2_mrp_push_result_t push = ap2cl_mrp_push_ex(client);
    CHECK(push.overall_status == -1 && push.nowplaying_status == 0);
    CHECK(ap2cl_mrp_channel_status(client) == 0);
    CHECK(hooks.creates == 0);
    CHECK(hooks.status_calls == 0);

    ap2cl_test_set_mrp_phase(client, true);
    CHECK(!ap2cl_set_metadata(client, "Connected", "Artist", "Album", 60));
    CHECK(ap2cl_test_has_mrp(client));
    CHECK(hooks.creates == 1);
    CHECK(ap2cl_mrp_channel_status(client) == 1);
    CHECK(hooks.status_calls == 1);

    ap2cl_test_teardown_mrp(client);
    CHECK(hooks.destroys == 1);
    ap2cl_test_set_mrp_hooks(NULL);
    CHECK(ap2cl_destroy(client));
    pthread_cond_destroy(&hooks.changed);
    pthread_mutex_destroy(&hooks.lock);
    return true;
}

typedef struct {
    struct ap2cl_s *client;
    atomic_bool done;
    bool result;
} client_call_t;

static void *setup_mrp_worker(void *arg)
{
    client_call_t *call = arg;
    call->result = ap2cl_test_setup_mrp(call->client);
    atomic_store(&call->done, true);
    return NULL;
}

static void *metadata_worker(void *arg)
{
    client_call_t *call = arg;
    call->result = ap2cl_set_metadata(
        call->client, "Concurrent", "Artist", "Album", 60);
    atomic_store(&call->done, true);
    return NULL;
}

static bool test_attach_failure_with_command(void)
{
    mrp_hooks_t hooks = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .attach_result = false,
        .release_status = true,
    };
    ap2cl_mrp_test_hooks_t test_hooks = make_hooks(&hooks);
    ap2cl_test_set_mrp_hooks(&test_hooks);
    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    ap2cl_test_set_mrp_phase(client, false);

    client_call_t setup = {.client = client};
    pthread_t setup_thread;
    CHECK(pthread_create(
              &setup_thread, NULL, setup_mrp_worker, &setup) == 0);
    pthread_mutex_lock(&hooks.lock);
    while (!hooks.attach_entered)
        pthread_cond_wait(&hooks.changed, &hooks.lock);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(ap2cl_test_mrp_serial_locked(client));

    client_call_t command = {.client = client};
    pthread_t command_thread;
    CHECK(pthread_create(
              &command_thread, NULL, metadata_worker, &command) == 0);
    pthread_mutex_lock(&hooks.lock);
    while (hooks.serial_contentions < 1)
        pthread_cond_wait(&hooks.changed, &hooks.lock);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(!atomic_load(&command.done));

    pthread_mutex_lock(&hooks.lock);
    hooks.release_attach = true;
    pthread_cond_broadcast(&hooks.changed);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(pthread_join(setup_thread, NULL) == 0);
    CHECK(pthread_join(command_thread, NULL) == 0);

    CHECK(!setup.result);
    CHECK(!command.result);
    CHECK(hooks.creates == 1);
    CHECK(hooks.attach_calls == 1);
    CHECK(hooks.destroys == 1);
    CHECK(!hooks.destroy_during_attach);
    CHECK(!ap2cl_test_has_mrp(client));

    ap2cl_test_set_mrp_phase(client, true);
    CHECK(!ap2cl_set_metadata(client, "Recovered", "", "", 60));
    CHECK(ap2cl_test_has_mrp(client));
    CHECK(hooks.creates == 2);
    ap2cl_test_teardown_mrp(client);
    CHECK(hooks.destroys == 2);

    ap2cl_test_set_mrp_hooks(NULL);
    CHECK(ap2cl_destroy(client));
    pthread_cond_destroy(&hooks.changed);
    pthread_mutex_destroy(&hooks.lock);
    return true;
}

static void *channel_status_worker(void *arg)
{
    client_call_t *call = arg;
    call->result = ap2cl_mrp_channel_status(call->client) == 1;
    atomic_store(&call->done, true);
    return NULL;
}

static void *teardown_mrp_worker(void *arg)
{
    client_call_t *call = arg;
    ap2cl_test_teardown_mrp(call->client);
    call->result = true;
    atomic_store(&call->done, true);
    return NULL;
}

static bool test_channel_status_during_teardown(void)
{
    mrp_hooks_t hooks = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .release_attach = true,
        .attach_result = true,
        .status_result = true,
    };
    ap2cl_mrp_test_hooks_t test_hooks = make_hooks(&hooks);
    test_hooks.attach = NULL;
    ap2cl_test_set_mrp_hooks(&test_hooks);
    struct ap2cl_s *client = create_test_client();
    CHECK(client != NULL);
    ap2cl_test_set_mrp_phase(client, true);
    CHECK(!ap2cl_set_metadata(client, "Status", "", "", 60));
    CHECK(ap2cl_test_has_mrp(client));

    client_call_t status = {.client = client};
    pthread_t status_thread;
    CHECK(pthread_create(
              &status_thread, NULL, channel_status_worker, &status) == 0);
    pthread_mutex_lock(&hooks.lock);
    while (!hooks.status_entered)
        pthread_cond_wait(&hooks.changed, &hooks.lock);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(ap2cl_test_mrp_serial_locked(client));

    client_call_t teardown = {.client = client};
    pthread_t teardown_thread;
    CHECK(pthread_create(
              &teardown_thread, NULL, teardown_mrp_worker, &teardown) == 0);
    pthread_mutex_lock(&hooks.lock);
    while (hooks.serial_contentions < 1)
        pthread_cond_wait(&hooks.changed, &hooks.lock);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(!atomic_load(&teardown.done));
    pthread_mutex_lock(&hooks.lock);
    CHECK(hooks.destroys == 0);
    pthread_mutex_unlock(&hooks.lock);

    pthread_mutex_lock(&hooks.lock);
    hooks.release_status = true;
    pthread_cond_broadcast(&hooks.changed);
    pthread_mutex_unlock(&hooks.lock);
    CHECK(pthread_join(status_thread, NULL) == 0);
    CHECK(pthread_join(teardown_thread, NULL) == 0);

    CHECK(status.result);
    CHECK(teardown.result);
    CHECK(hooks.status_calls == 1);
    CHECK(hooks.destroys == 1);
    CHECK(!hooks.destroy_during_status);
    CHECK(!ap2cl_test_has_mrp(client));
    CHECK(ap2cl_mrp_channel_status(client) == 0);
    CHECK(hooks.status_calls == 1);

    ap2cl_test_set_mrp_hooks(NULL);
    CHECK(ap2cl_destroy(client));
    pthread_cond_destroy(&hooks.changed);
    pthread_mutex_destroy(&hooks.lock);
    return true;
}

int main(void)
{
    CHECK(setenv("CLIAIRPLAY_MRP_TYPE130", "1", 1) == 0);
    CHECK(unsetenv("CLIAIRPLAY_MRP") == 0);
    if (!test_command_thread_retirement() ||
        !test_connect_gate_and_normal_operation() ||
        !test_attach_failure_with_command() ||
        !test_channel_status_during_teardown())
        return 1;
    puts("AP2 command and MRP lifecycle tests passed");
    return 0;
}
