/*
 * AirPlay 2 PTP - shared clock (daemon <-> streaming processes)
 *
 * See ap2_ptp_shm.h for the architecture. This file implements the POSIX
 * shared-memory clock (lock-free double buffer) and the localhost UDP control
 * channel used to register receiver IPs with the daemon.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "cross_log.h"
#include "ap2_ptp_shm.h"

extern log_level *loglevel;

/* ---- shared-memory writer (daemon side) ---- */

bool ap2_ptp_shm_writer_open(struct ap2_ptp_shm_writer *w)
{
    if (!w) return false;
    memset(w, 0, sizeof(*w));
    w->fd = -1;

    /* O_CREAT|O_RDWR: reattach a stale object left by a crashed daemon (same
     * size) rather than failing; we re-stamp the version and overwrite. */
    bool created = true;
    int fd = shm_open(AP2_PTP_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0 && errno == EEXIST) {
        created = false;
        fd = shm_open(AP2_PTP_SHM_NAME, O_CREAT | O_RDWR, 0644);
    }
    if (fd < 0) {
        LOG_ERROR("[PTP-SHM] shm_open(%s) failed: %s", AP2_PTP_SHM_NAME, strerror(errno));
        return false;
    }

    /* ftruncate is only meaningful when we just created the object; sizing an
     * existing mapping down/up is undefined on some platforms, so guard it. */
    if (created && ftruncate(fd, sizeof(struct ap2_ptp_shm)) != 0) {
        LOG_ERROR("[PTP-SHM] ftruncate failed: %s", strerror(errno));
        close(fd);
        shm_unlink(AP2_PTP_SHM_NAME);
        return false;
    }

    void *map = mmap(NULL, sizeof(struct ap2_ptp_shm), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOG_ERROR("[PTP-SHM] mmap failed: %s", strerror(errno));
        close(fd);
        if (created) shm_unlink(AP2_PTP_SHM_NAME);
        return false;
    }

    w->fd = fd;
    w->map = (struct ap2_ptp_shm *)map;
    w->owner = created;
    w->seq = 0;

    /* Stamp the layout version and zero the payload before any reader trusts it. */
    memset(w->map, 0, sizeof(*w->map));
    w->map->version = AP2_PTP_SHM_VERSION;
    __sync_synchronize();

    LOG_INFO("[PTP-SHM] Publishing clock at %s (%s, %zu bytes)", AP2_PTP_SHM_NAME,
             created ? "created" : "reattached", sizeof(struct ap2_ptp_shm));
    return true;
}

void ap2_ptp_shm_publish(struct ap2_ptp_shm_writer *w, struct ap2_ptp_shm_sample s)
{
    if (!w || !w->map) return;
    s.update_count = ++w->seq;

    /* Fill main, barrier, then copy to secondary. A reader that catches the
     * update in flight sees main != secondary and retries. */
    w->map->main = s;
    __sync_synchronize();
    w->map->secondary = s;
    __sync_synchronize();
}

void ap2_ptp_shm_writer_close(struct ap2_ptp_shm_writer *w)
{
    if (!w) return;
    if (w->map) { munmap(w->map, sizeof(struct ap2_ptp_shm)); w->map = NULL; }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    if (w->owner) shm_unlink(AP2_PTP_SHM_NAME);
}

/* ---- shared-memory reader (streaming side) ---- */

bool ap2_ptp_shm_reader_open(struct ap2_ptp_shm_reader *r)
{
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    r->fd = -1;

    int fd = shm_open(AP2_PTP_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) return false;   /* no daemon */

    void *map = mmap(NULL, sizeof(struct ap2_ptp_shm), PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }

    struct ap2_ptp_shm *shm = (struct ap2_ptp_shm *)map;
    uint32_t ver = shm->version;
    __sync_synchronize();
    if (ver != AP2_PTP_SHM_VERSION) {
        LOG_WARN("[PTP-SHM] version mismatch (shm=%u, expected=%u); ignoring daemon clock",
                 ver, AP2_PTP_SHM_VERSION);
        munmap(map, sizeof(struct ap2_ptp_shm));
        close(fd);
        return false;
    }

    r->fd = fd;
    r->map = shm;
    return true;
}

bool ap2_ptp_shm_read(struct ap2_ptp_shm_reader *r, struct ap2_ptp_shm_sample *out)
{
    if (!r || !out) return false;
    if (r->map) {
        for (int i = 0; i < 10; i++) {
            struct ap2_ptp_shm_sample a, b;
            __sync_synchronize();
            memcpy(&a, &r->map->main, sizeof(a));
            __sync_synchronize();
            memcpy(&b, &r->map->secondary, sizeof(b));
            __sync_synchronize();
            if (memcmp(&a, &b, sizeof(a)) == 0) {
                r->last = a;
                r->have_last = true;
                *out = a;
                return true;
            }
        }
    }
    if (r->have_last) { *out = r->last; return true; }
    return false;
}

void ap2_ptp_shm_reader_close(struct ap2_ptp_shm_reader *r)
{
    if (!r) return;
    if (r->map) { munmap(r->map, sizeof(struct ap2_ptp_shm)); r->map = NULL; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    r->have_last = false;
}

/* ---- control channel ---- */

int ap2_ptp_ctrl_server_open(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(AP2_PTP_CTRL_PORT),
    };
    a.sin_addr.s_addr = inet_addr(AP2_PTP_CTRL_ADDR);   /* loopback only */
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        LOG_ERROR("[PTP-SHM] control bind %s:%d failed: %s", AP2_PTP_CTRL_ADDR,
                  AP2_PTP_CTRL_PORT, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

bool ap2_ptp_ctrl_send(const char *cmd, int timeout_ms, char *ack, int ack_len)
{
    if (!cmd) return false;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;

    /* Bind to an ephemeral loopback port so the daemon can reply to us. */
    struct sockaddr_in la = {.sin_family = AF_INET};
    la.sin_addr.s_addr = inet_addr(AP2_PTP_CTRL_ADDR);
    bind(s, (struct sockaddr *)&la, sizeof(la));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(AP2_PTP_CTRL_PORT),
    };
    dst.sin_addr.s_addr = inet_addr(AP2_PTP_CTRL_ADDR);

    if (sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        close(s);
        return false;
    }
    if (timeout_ms <= 0) { close(s); return true; }

    struct pollfd pfd = {.fd = s, .events = POLLIN};
    bool got = false;
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
        char buf[256];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n >= 0) {
            buf[n] = '\0';
            got = true;
            if (ack && ack_len > 0) {
                snprintf(ack, ack_len, "%s", buf);
            }
        }
    }
    close(s);
    return got;
}
