/*
 * AirPlay 2 PTP - the shared daemon's receiver registrations
 *
 * See ap2_ptp_peers.h.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ap2_ptp_peers.h"

static int peers_find(const struct ap2_ptp_peers *p, struct in_addr addr, int from);
static int peers_distinct(const struct ap2_ptp_peers *p);
static void peers_drop(struct ap2_ptp_peers *p, int i);

bool ap2_ptp_peers_add(struct ap2_ptp_peers *p, const char *ip, pid_t owner, bool follow)
{
    struct in_addr addr;
    if (!ip || inet_pton(AF_INET, ip, &addr) != 1) return false;
    /* An owning process registers its receiver once; registrations without
     * an owner cannot be told apart, so each one counts (refcount semantics). */
    for (int i = peers_find(p, addr, 0); owner && i >= 0; i = peers_find(p, addr, i + 1)) {
        if (p->e[i].owner == owner) { p->e[i].follow = follow; return true; }
    }
    if (p->n >= AP2_PTP_PEERS_MAX_REGS) return false;
    if (peers_find(p, addr, 0) < 0 && peers_distinct(p) >= AP2_PTP_MAX_PEERS) return false;
    struct ap2_ptp_peer_reg *r = &p->e[p->n++];
    r->addr = addr;
    inet_ntop(AF_INET, &addr, r->ip, sizeof(r->ip));
    r->owner = owner;
    r->follow = follow;
    return true;
}

bool ap2_ptp_peers_remove(struct ap2_ptp_peers *p, const char *ip, pid_t owner)
{
    struct in_addr addr;
    if (!ip || inet_pton(AF_INET, ip, &addr) != 1) return false;
    for (int i = peers_find(p, addr, 0); i >= 0; i = peers_find(p, addr, i + 1)) {
        if (owner && p->e[i].owner != owner) continue;
        peers_drop(p, i);
        return true;
    }
    return false;
}

int ap2_ptp_peers_reap(struct ap2_ptp_peers *p, bool (*alive)(pid_t pid),
                       void (*on_reap)(const struct ap2_ptp_peer_reg *reg))
{
    int removed = 0;
    for (int i = 0; i < p->n;) {
        if (!p->e[i].owner || alive(p->e[i].owner)) { i++; continue; }
        if (on_reap) on_reap(&p->e[i]);
        peers_drop(p, i);
        removed++;
    }
    return removed;
}

int ap2_ptp_peers_snapshot(const struct ap2_ptp_peers *p, const char **ips, bool *follow,
                           int max)
{
    int n = 0;
    for (int i = 0; i < p->n && n < max; i++) {
        if (peers_find(p, p->e[i].addr, 0) != i) continue;
        bool f = false;
        for (int j = i; j >= 0; j = peers_find(p, p->e[i].addr, j + 1)) f |= p->e[j].follow;
        ips[n] = p->e[i].ip;
        follow[n++] = f;
    }
    return n;
}

/* Index of the first registration of addr at or after `from`, or -1. */
static int peers_find(const struct ap2_ptp_peers *p, struct in_addr addr, int from)
{
    for (int i = from; i < p->n; i++)
        if (p->e[i].addr.s_addr == addr.s_addr) return i;
    return -1;
}

static int peers_distinct(const struct ap2_ptp_peers *p)
{
    int c = 0;
    for (int i = 0; i < p->n; i++)
        if (peers_find(p, p->e[i].addr, 0) == i) c++;
    return c;
}

static void peers_drop(struct ap2_ptp_peers *p, int i)
{
    p->e[i] = p->e[--p->n];
}
