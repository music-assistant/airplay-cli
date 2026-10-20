/*
 * AirPlay 2 PTP - the shared daemon's receiver registrations
 *
 * Every streaming process registers its receiver with `cliairplay --ptp-daemon`
 * (see the control channel in ap2_ptp_shm.h). A registration is keyed by the
 * receiver and the registering process, so streams to the same receiver come
 * and go independently, and the daemon can drop the registrations of a process
 * that died without unregistering.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_PTP_PEERS_H_
#define __AP2_PTP_PEERS_H_

#include <stdbool.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ap2_ptp_shm.h"

/* Room for every receiver to be registered by two streams at once (e.g. a
 * stream restarting before the old one is reaped). */
#define AP2_PTP_PEERS_MAX_REGS (2 * AP2_PTP_MAX_PEERS)

struct ap2_ptp_peer_reg {
    struct in_addr addr;
    char ip[INET_ADDRSTRLEN];
    pid_t owner;                      /* registering process; 0 = none, never reaped */
    bool follow;                      /* follow this receiver's own clock */
};

struct ap2_ptp_peers {
    struct ap2_ptp_peer_reg e[AP2_PTP_PEERS_MAX_REGS];
    int n;
};

/*
 * Register `ip` for `owner`, or update `follow` of an existing registration of
 * the same (ip, owner). Registrations without an owner always add an entry.
 *
 * :param p: registration table.
 * :param ip: receiver IPv4 address.
 * :param owner: registering process id; 0 when the client sent none.
 * :param follow: follow the receiver's own clock.
 * :returns: false when ip is invalid, the table is full, or a new receiver
 *           would exceed AP2_PTP_MAX_PEERS distinct receivers.
 */
bool ap2_ptp_peers_add(struct ap2_ptp_peers *p, const char *ip, pid_t owner, bool follow);

/*
 * Remove one registration of `ip`.
 *
 * :param p: registration table.
 * :param ip: receiver IPv4 address.
 * :param owner: remove the registration of this process; 0 removes the first
 *               registration of ip regardless of owner.
 * :returns: true if a registration was removed.
 */
bool ap2_ptp_peers_remove(struct ap2_ptp_peers *p, const char *ip, pid_t owner);

/*
 * Remove every registration whose owner is no longer alive. Registrations
 * without an owner are kept.
 *
 * :param p: registration table.
 * :param alive: reports whether a process id still exists.
 * :param on_reap: called for each removed registration before it goes; may be NULL.
 * :returns: the number of registrations removed.
 */
int ap2_ptp_peers_reap(struct ap2_ptp_peers *p, bool (*alive)(pid_t pid),
                       void (*on_reap)(const struct ap2_ptp_peer_reg *reg));

/*
 * List the distinct registered receivers.
 *
 * :param p: registration table.
 * :param ips: receives up to max receiver addresses, pointing into p.
 * :param follow: follow[i] is true when any registration of ips[i] follows it.
 * :param max: capacity of ips and follow.
 * :returns: the number of receivers written.
 */
int ap2_ptp_peers_snapshot(const struct ap2_ptp_peers *p, const char **ips, bool *follow,
                           int max);

#endif /* __AP2_PTP_PEERS_H_ */
