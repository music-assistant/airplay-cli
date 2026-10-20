#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ap2_ptp_peers.h"

static int snapshot(const struct ap2_ptp_peers *p, const char **ips, bool *follow)
{
    return ap2_ptp_peers_snapshot(p, ips, follow, AP2_PTP_MAX_PEERS);
}

static bool listed(const char **ips, int n, const char *ip)
{
    for (int i = 0; i < n; i++)
        if (strcmp(ips[i], ip) == 0) return true;
    return false;
}

static bool owner_alive_unless_1(pid_t pid)
{
    return pid != 1;
}

static void test_removed_registrations_free_their_slots(void)
{
    struct ap2_ptp_peers p = {0};
    char ip[INET_ADDRSTRLEN];
    for (int i = 1; i <= 8; i++) {
        snprintf(ip, sizeof(ip), "192.0.2.%d", i);
        assert(ap2_ptp_peers_add(&p, ip, 1, false));
    }
    for (int i = 1; i <= 8; i++) {
        snprintf(ip, sizeof(ip), "192.0.2.%d", i);
        assert(ap2_ptp_peers_remove(&p, ip, 1));
    }
    for (int i = 9; i <= 16; i++) {
        snprintf(ip, sizeof(ip), "192.0.2.%d", i);
        assert(ap2_ptp_peers_add(&p, ip, 1, false));
    }
    assert(p.n == 8);
    const char *ips[AP2_PTP_MAX_PEERS];
    bool follow[AP2_PTP_MAX_PEERS];
    int n = snapshot(&p, ips, follow);
    assert(n == 8);
    for (int i = 9; i <= 16; i++) {
        snprintf(ip, sizeof(ip), "192.0.2.%d", i);
        assert(listed(ips, n, ip));
    }
}

static void test_receiver_of_two_owners_is_one_peer(void)
{
    struct ap2_ptp_peers p = {0};
    const char *ips[AP2_PTP_MAX_PEERS];
    bool follow[AP2_PTP_MAX_PEERS];
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 1, false));
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 2, false));
    assert(snapshot(&p, ips, follow) == 1);
    assert(ap2_ptp_peers_remove(&p, "192.0.2.1", 1));
    assert(snapshot(&p, ips, follow) == 1);
    assert(ap2_ptp_peers_remove(&p, "192.0.2.1", 2));
    assert(snapshot(&p, ips, follow) == 0);
}

static void test_unowned_registrations_are_refcounted(void)
{
    struct ap2_ptp_peers p = {0};
    const char *ips[AP2_PTP_MAX_PEERS];
    bool follow[AP2_PTP_MAX_PEERS];
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 0, false));
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 0, false));
    assert(p.n == 2 && snapshot(&p, ips, follow) == 1);
    assert(ap2_ptp_peers_remove(&p, "192.0.2.1", 0));
    assert(snapshot(&p, ips, follow) == 1);
    assert(ap2_ptp_peers_remove(&p, "192.0.2.1", 0));
    assert(snapshot(&p, ips, follow) == 0);
}

static void test_reap_drops_only_dead_owners(void)
{
    struct ap2_ptp_peers p = {0};
    const char *ips[AP2_PTP_MAX_PEERS];
    bool follow[AP2_PTP_MAX_PEERS];
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 1, false));
    assert(ap2_ptp_peers_add(&p, "192.0.2.2", 2, false));
    assert(ap2_ptp_peers_add(&p, "192.0.2.3", 0, false));
    assert(ap2_ptp_peers_reap(&p, owner_alive_unless_1, NULL) == 1);
    int n = snapshot(&p, ips, follow);
    assert(n == 2);
    assert(listed(ips, n, "192.0.2.2"));
    assert(listed(ips, n, "192.0.2.3"));
}

static void test_follow_is_set_while_any_registration_follows(void)
{
    struct ap2_ptp_peers p = {0};
    const char *ips[AP2_PTP_MAX_PEERS];
    bool follow[AP2_PTP_MAX_PEERS];
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 1, false));
    assert(ap2_ptp_peers_add(&p, "192.0.2.1", 2, true));
    assert(snapshot(&p, ips, follow) == 1 && follow[0]);
    assert(ap2_ptp_peers_remove(&p, "192.0.2.1", 2));
    assert(snapshot(&p, ips, follow) == 1 && !follow[0]);
}

int main(void)
{
    test_removed_registrations_free_their_slots();
    test_receiver_of_two_owners_is_one_peer();
    test_unowned_registrations_are_refcounted();
    test_reap_drops_only_dead_owners();
    test_follow_is_set_while_any_registration_follows();
    printf("test_ap2_ptp_peers: all tests passed\n");
    return 0;
}
