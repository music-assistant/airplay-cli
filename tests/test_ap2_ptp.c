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
#include "host_clock.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define MAX_DESTS 128

/* Datagrams are counted per destination: a daemon serving many peers sends
 * hundreds per second, so a flat list would overflow within a test. */
static pthread_mutex_t sends_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { struct sockaddr_in addr; int count; } dests[MAX_DESTS];
static int ndests;

/* The port the engine's most recently bound socket really listens on, so a
 * test can play a receiver's timing messages into it. */
static uint16_t engine_port;

/* ap2_ptp.c is compiled with -Dbind/-Dsendto: bind to an ephemeral port instead
 * of privileged 319/320, and record every datagram instead of sending it. */
int ap2_ptp_test_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_in a;
    memcpy(&a, addr, sizeof(a));
    a.sin_port = 0;
    int rc = bind(fd, (const struct sockaddr *)&a, len);
    socklen_t bound_len = sizeof(a);
    if (rc == 0 && getsockname(fd, (struct sockaddr *)&a, &bound_len) == 0)
        engine_port = ntohs(a.sin_port);
    return rc;
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

static void assert_silent_to(const char *ip, int window_ms)
{
    sends_reset();
    usleep((useconds_t)window_ms * 1000);
    assert(sends_to(ip) == 0);
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

/* A receiver the engine serves stops being served the moment its own clock is
 * followed, on an engine that is already running. */
static void test_engine_follows_receiver_mid_session(void)
{
    sends_reset();
    struct ap2_ptp_ctx *ctx = ap2_ptp_create();
    assert(ctx);
    assert(ap2_ptp_engine_start(ctx, any_iface(), "192.0.2.30"));
    assert(wait_for_sends_to("192.0.2.30", 2000));

    assert(ap2_ptp_follow_receiver(ctx, "192.0.2.30"));
    /* A datagram already past the peer snapshot may still go out. */
    usleep(100000);
    assert_silent_to("192.0.2.30", 600);

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

/* A stream that registers its receiver again as followed turns that one
 * receiver over to its own clock; the others stay served. */
static void test_daemon_follows_receiver_on_reregistration(void)
{
    pthread_t t = daemon_start();
    pid_t me = getpid();
    ctrl("R 192.0.2.50 pid=%d", (int)me);
    ctrl("R 192.0.2.51 pid=%d", (int)me);
    assert(wait_for_sends_to("192.0.2.50", 2000));

    ctrl("R 192.0.2.50 F pid=%d", (int)me);
    usleep(100000);
    assert_silent_to("192.0.2.50", 600);
    assert(sends_to("192.0.2.51") > 0);
    daemon_stop(t);
}

/* A timing message as a receiver at 127.0.0.1 sends it: `type` with the
 * receiver's port identity, and for an Announce the grandmaster it names. */
static void receiver_send(int fd, int type, uint64_t grandmaster)
{
    uint8_t msg[64] = {0};
    int len = type == 0xB ? 64 : 44;
    msg[0] = 0x10 | type;
    msg[1] = 2;
    msg[3] = (uint8_t)len;
    for (int i = 0; i < 8; i++) msg[20 + i] = 0xA0 + i;
    for (int i = 0; i < 8; i++) msg[34 + 19 + i] = (grandmaster >> (56 - 8 * i)) & 0xFF;
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(engine_port)};
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&dst, sizeof(dst)) == len);
}

/* Probe the engine's clock and wait for its answer, which also proves it has
 * taken in everything the receiver sent before the probe. */
static void receiver_probe(int fd)
{
    int answered = sends_to("127.0.0.1");
    receiver_send(fd, 0x1, 0);
    for (int waited = 0; sends_to("127.0.0.1") == answered; waited += 5) {
        assert(waited < 2000);
        usleep(5000);
    }
}

/* Run `step` with the engine's log captured; returns what it logged. */
static const char *captured_log(void (*step)(int fd), int fd)
{
    static char log[1024];
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    FILE *capture = tmpfile();
    assert(saved >= 0 && capture);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    test_log_level = lINFO;
    step(fd);
    test_log_level = lSILENCE;
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    size_t n = fread(log, 1, sizeof(log) - 1, capture);
    log[n] = '\0';
    fclose(capture);
    return log;
}

static void announce_own_grandmaster(int fd)
{
    receiver_send(fd, 0xB, 0x1122334455667788ULL);
    receiver_probe(fd);
}

static void announce_our_grandmaster(int fd)
{
    /* The fixed test MAC gives the engine this clock identity. */
    receiver_send(fd, 0xB, 0x020000FFFE000001ULL);
    receiver_probe(fd);
}

/* A receiver that probes our clock yet keeps announcing a grandmaster of its
 * own, seconds apart within one probe streak, is logged once per streak. The
 * burst of Announces every receiver sends as a session starts is not, and
 * neither is a receiver that names our clock as its grandmaster.
 * Runs last: it leaves the host clock on the raw clock, stepped forward. */
static void test_engine_notes_receiver_announcing_own_grandmaster(void)
{
    const char *noted = "keeps announcing its own grandmaster";
    host_clock_set_offset_ns(0);
    sends_reset();
    struct ap2_ptp_ctx *ctx = ap2_ptp_create();
    assert(ctx);
    assert(ap2_ptp_engine_start(ctx, any_iface(), NULL));
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    /* The session-start burst: several Announces within a moment. */
    receiver_probe(fd);
    assert(!strstr(captured_log(announce_own_grandmaster, fd), noted));
    assert(!strstr(captured_log(announce_own_grandmaster, fd), noted));

    /* Naming our clock later in the streak is a receiver that took it. */
    host_clock_set_offset_ns(2600000000LL);
    receiver_probe(fd);
    host_clock_set_offset_ns(5200000000LL);
    assert(!strstr(captured_log(announce_our_grandmaster, fd), noted));

    /* Its own grandmaster again, this long after the first: noted, once. */
    const char *log = captured_log(announce_own_grandmaster, fd);
    assert(strstr(log, noted) && strstr(log, "gm=1122334455667788"));
    host_clock_set_offset_ns(7800000000LL);
    assert(!strstr(captured_log(announce_own_grandmaster, fd), noted));

    close(fd);
    ap2_ptp_destroy(ctx);
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
        {"engine_follows_receiver_mid_session", test_engine_follows_receiver_mid_session},
        {"daemon_serves_new_receiver_after_old_ones_unregister",
         test_daemon_serves_new_receiver_after_old_ones_unregister},
        {"daemon_serves_nine_receivers_at_once", test_daemon_serves_nine_receivers_at_once},
        {"daemon_reaps_receivers_of_dead_clients", test_daemon_reaps_receivers_of_dead_clients},
        {"daemon_follows_receiver_on_reregistration",
         test_daemon_follows_receiver_on_reregistration},
        {"daemon_replaces_stale_smaller_shm", test_daemon_replaces_stale_smaller_shm},
        {"engine_notes_receiver_announcing_own_grandmaster",
         test_engine_notes_receiver_announcing_own_grandmaster},
    };
    /* An optional argument runs one test by name. */
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) continue;
        tests[i].fn();
    }
    printf("test_ap2_ptp: all tests passed\n");
    return 0;
}
