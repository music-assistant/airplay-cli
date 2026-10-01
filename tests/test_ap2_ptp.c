#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ap2_ptp.h"
#include "cross_log.h"

static log_level test_log_level = lSILENCE;
log_level *loglevel = &test_log_level;

#define MAX_SENDS 256

static pthread_mutex_t sends_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sockaddr_in sends[MAX_SENDS];
static int nsends;

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
    pthread_mutex_lock(&sends_lock);
    if (nsends < MAX_SENDS) memcpy(&sends[nsends++], dst, sizeof(sends[0]));
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
    int n = nsends;
    pthread_mutex_unlock(&sends_lock);
    return n;
}

static void sends_reset(void)
{
    pthread_mutex_lock(&sends_lock);
    nsends = 0;
    pthread_mutex_unlock(&sends_lock);
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
    for (int i = 0; i < nsends; i++) {
        assert(sends[i].sin_addr.s_addr == want.s_addr);
        uint16_t port = ntohs(sends[i].sin_port);
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

int main(void)
{
    test_engine_without_peers_stays_silent();
    test_engine_start_serves_device_before_setpeers();
    printf("test_ap2_ptp: all tests passed\n");
    return 0;
}
