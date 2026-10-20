#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ap2_ptp.h"
#include "ap2_ptp_shm.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define MAX_DESTS 128

/* Datagrams are counted per destination: a daemon serving many peers sends
 * hundreds per second, so a flat list would overflow within a test. */
static pthread_mutex_t sends_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { struct sockaddr_in addr; int count; } dests[MAX_DESTS];
static int ndests;

/* ap2_ptp.c is compiled with -Dbind/-Dsendto: bind to an ephemeral port instead
 * of privileged 319/320, and record every datagram instead of sending it. */
int ap2_ptp_test_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_in a;
    memcpy(&a, addr, sizeof(a));
    a.sin_port = 0;
    return bind(fd, (const struct sockaddr *)&a, len);
}

ssize_t ap2_ptp_test_sendto(int fd, const void *buf, size_t len, int flags,
                            const struct sockaddr *dst, socklen_t dst_len)
{
    (void)fd; (void)buf; (void)flags; (void)dst_len;
    struct sockaddr_in d;
    memcpy(&d, dst, sizeof(d));
    pthread_mutex_lock(&sends_lock);
    int i = 0;
    for (; i < ndests; i++)
        if (dests[i].addr.sin_addr.s_addr == d.sin_addr.s_addr &&
            dests[i].addr.sin_port == d.sin_port) break;
    if (i == ndests && ndests < MAX_DESTS) {
        dests[ndests].addr = d;
        dests[ndests].count = 0;
        ndests++;
    }
    if (i < ndests) dests[i].count++;
    pthread_mutex_unlock(&sends_lock);
    return (ssize_t)len;
}

void get_mac(uint8_t mac[])
{
    static const uint8_t fixed[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(mac, fixed, sizeof(fixed));
}

static int sends_count(void)
{
    pthread_mutex_lock(&sends_lock);
    int n = 0;
    for (int i = 0; i < ndests; i++) n += dests[i].count;
    pthread_mutex_unlock(&sends_lock);
    return n;
}

static void sends_reset(void)
{
    pthread_mutex_lock(&sends_lock);
    ndests = 0;
    pthread_mutex_unlock(&sends_lock);
}

/* Timing datagrams (PTP ports) recorded for `ip`; the daemon's control acks
 * go to a loopback port and are not counted. */
static int sends_to(const char *ip)
{
    struct in_addr want;
    assert(inet_pton(AF_INET, ip, &want) == 1);
    pthread_mutex_lock(&sends_lock);
    int n = 0;
    for (int i = 0; i < ndests; i++) {
        uint16_t port = ntohs(dests[i].addr.sin_port);
        if (dests[i].addr.sin_addr.s_addr == want.s_addr && (port == 319 || port == 320))
            n += dests[i].count;
    }
    pthread_mutex_unlock(&sends_lock);
    return n;
}

static bool wait_for_sends_to(const char *ip, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (sends_to(ip) > 0) return true;
        usleep(10000);
    }
    return false;
}

static bool wait_for_sends(int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (sends_count() > 0) return true;
        usleep(10000);
    }
    return false;
}

/* Every recorded datagram went to `ip` on a PTP port; none to the multicast group. */
static void assert_all_sends_to(const char *ip)
{
    struct in_addr want;
    assert(inet_pton(AF_INET, ip, &want) == 1);
    pthread_mutex_lock(&sends_lock);
    for (int i = 0; i < ndests; i++) {
        assert(dests[i].addr.sin_addr.s_addr == want.s_addr);
        uint16_t port = ntohs(dests[i].addr.sin_port);
        assert(port == 319 || port == 320);
    }
    pthread_mutex_unlock(&sends_lock);
}

static struct in_addr any_iface(void)
{
    struct in_addr a = {.s_addr = INADDR_ANY};
    return a;
}

static void test_engine_without_peers_stays_silent(void)
{
    sends_reset();
    struct ap2_ptp_ctx *ctx = ap2_ptp_create();
    assert(ctx);
    assert(ap2_ptp_engine_start(ctx, any_iface(), NULL));

    /* Several Sync intervals (125 ms) and the first Announce go by unsent. */
    usleep(400000);
    assert(sends_count() == 0);

    /* The same running engine starts serving a peer as soon as it has one. */
    const char *peers[1] = {"192.0.2.20"};
    ap2_ptp_set_peers(ctx, peers, 1);
    assert(wait_for_sends(2000));
    assert_all_sends_to("192.0.2.20");

    ap2_ptp_destroy(ctx);
}

static void test_engine_start_serves_device_before_setpeers(void)
{
    sends_reset();
    struct ap2_ptp_ctx *ctx = ap2_ptp_create();
    assert(ctx);
    assert(ap2_ptp_engine_start(ctx, any_iface(), "192.0.2.10"));

    assert(wait_for_sends(2000));
    assert_all_sends_to("192.0.2.10");

    ap2_ptp_destroy(ctx);
}

/* ---- shared daemon (--ptp-daemon) driven over its control channel ---- */

static volatile bool daemon_stop_flag;

static void *daemon_thread(void *arg)
{
    (void)arg;
    ap2_ptp_run_daemon(any_iface(), 0, &daemon_stop_flag);
    return NULL;
}

/* Start the daemon and wait until it has published its first clock sample
 * (a fresh shm reads as an all-zero sample, so the count has to move), which
 * happens only after the control socket is bound. */
static pthread_t daemon_start(void)
{
    sends_reset();
    daemon_stop_flag = false;
    pthread_t t;
    assert(pthread_create(&t, NULL, daemon_thread, NULL) == 0);
    struct ap2_ptp_shm_reader r;
    struct ap2_ptp_shm_sample s;
    bool up = false;
    for (int waited = 0; !up && waited < 3000; waited += 10) {
        usleep(10000);
        if (!ap2_ptp_shm_reader_open(&r)) continue;
        up = ap2_ptp_shm_read(&r, &s) && s.update_count > 0;
        ap2_ptp_shm_reader_close(&r);
    }
    assert(up);
    return t;
}

static void daemon_stop(pthread_t t)
{
    daemon_stop_flag = true;
    assert(pthread_join(t, NULL) == 0);
}

/* The test build records the daemon's acks instead of delivering them, so
 * commands are fire-and-forget; the daemon drains its socket within a tick. */
static void ctrl(const char *fmt, ...)
{
    char cmd[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    assert(ap2_ptp_ctrl_send(cmd, 0, NULL, 0));
    usleep(20000);
}

static void assert_silent_to(const char *ip, int window_ms)
{
    sends_reset();
    usleep((useconds_t)window_ms * 1000);
    assert(sends_to(ip) == 0);
}

/* Receivers unregistered by earlier sessions free their slots: after eight
 * distinct receivers have come and gone, a ninth is still served. */
static void test_daemon_serves_new_receiver_after_old_ones_unregister(void)
{
    pthread_t t = daemon_start();
    pid_t me = getpid();
    for (int i = 1; i <= 8; i++) ctrl("R 192.0.2.%d pid=%d", i, (int)me);
    for (int i = 1; i <= 8; i++) ctrl("U 192.0.2.%d pid=%d", i, (int)me);
    ctrl("R 192.0.2.9 pid=%d", (int)me);
    assert(wait_for_sends_to("192.0.2.9", 2000));
    assert_silent_to("192.0.2.1", 600);
    daemon_stop(t);
}

/* A sync group larger than eight receivers: every member is served. */
static void test_daemon_serves_nine_receivers_at_once(void)
{
    pthread_t t = daemon_start();
    for (int i = 1; i <= 9; i++) ctrl("R 192.0.2.%d pid=%d", i, (int)getpid());
    for (int i = 1; i <= 9; i++) {
        char ip[INET_ADDRSTRLEN];
        snprintf(ip, sizeof(ip), "192.0.2.%d", i);
        assert(wait_for_sends_to(ip, 2000));
    }
    daemon_stop(t);
}

/* A stream killed before it could unregister (SIGKILL from the server) must
 * not keep its receiver in the peer set: the daemon reaps dead owners. A live
 * owner's receiver stays served. */
static void test_daemon_reaps_receivers_of_dead_clients(void)
{
    pthread_t t = daemon_start();
    ctrl("R 192.0.2.41 pid=%d", (int)getpid());

    /* The child registers its receiver and stays alive, without ever
     * unregistering, until its receiver is being served; then it exits. */
    int hold[2];
    assert(pipe(hold) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(hold[1]);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "R 192.0.2.40 pid=%d", (int)getpid());
        ap2_ptp_ctrl_send(cmd, 0, NULL, 0);
        char c;
        while (read(hold[0], &c, 1) < 0) {}
        _exit(0);
    }
    close(hold[0]);
    assert(wait_for_sends_to("192.0.2.40", 2000));
    close(hold[1]);
    assert(waitpid(child, NULL, 0) == child);

    /* Reaping is periodic (1 s); well past that the dead owner's receiver is
     * silent while the live owner's is still served. */
    usleep(2500000);
    assert_silent_to("192.0.2.40", 600);
    assert(sends_to("192.0.2.41") > 0);
    daemon_stop(t);
}

/* A stale shm object of an older, smaller layout (a daemon that was killed)
 * is replaced, not reused, so the new layout never writes past its end. */
static void test_daemon_replaces_stale_smaller_shm(void)
{
    shm_unlink(AP2_PTP_SHM_NAME);
    int fd = shm_open(AP2_PTP_SHM_NAME, O_CREAT | O_RDWR, 0644);
    assert(fd >= 0 && ftruncate(fd, 64) == 0);
    close(fd);

    pthread_t t = daemon_start();
    struct stat st;
    fd = shm_open(AP2_PTP_SHM_NAME, O_RDONLY, 0);
    assert(fd >= 0 && fstat(fd, &st) == 0);
    close(fd);
    /* Platforms that round object sizes up to a page report more than asked. */
    assert(st.st_size >= (off_t)sizeof(struct ap2_ptp_shm));
    daemon_stop(t);
}

struct named_test { const char *name; void (*fn)(void); };

int main(int argc, char **argv)
{
    static const struct named_test tests[] = {
        {"engine_without_peers_stays_silent", test_engine_without_peers_stays_silent},
        {"engine_start_serves_device_before_setpeers",
         test_engine_start_serves_device_before_setpeers},
        {"daemon_serves_new_receiver_after_old_ones_unregister",
         test_daemon_serves_new_receiver_after_old_ones_unregister},
        {"daemon_serves_nine_receivers_at_once", test_daemon_serves_nine_receivers_at_once},
        {"daemon_reaps_receivers_of_dead_clients", test_daemon_reaps_receivers_of_dead_clients},
        {"daemon_replaces_stale_smaller_shm", test_daemon_replaces_stale_smaller_shm},
    };
    /* An optional argument runs one test by name. */
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) continue;
        tests[i].fn();
    }
    printf("test_ap2_ptp: all tests passed\n");
    return 0;
}
