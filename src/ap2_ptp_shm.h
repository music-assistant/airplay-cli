/*
 * AirPlay 2 PTP - shared clock (daemon <-> streaming processes)
 *
 * Music Assistant spawns one cliairplay process per speaker. Only one process
 * per host can bind the privileged PTP ports (UDP 319/320), and every receiver
 * in a sync group must lock to the same grandmaster. This module lets a single
 * `cliairplay --ptp-daemon` own 319/320, run the PTP engine, and PUBLISH the
 * elected-master clock to POSIX shared memory; the per-device streaming
 * processes attach that shm read-only (with `--ptp-shared`) instead of running
 * their own engine, so all streams share one clock. It mirrors nqptp's split
 * (a singleton timing service + a shm clock), except here we are the sender.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __AP2_PTP_SHM_H_
#define __AP2_PTP_SHM_H_

#include <stdbool.h>
#include <stdint.h>

/* POSIX shared-memory object name (one per host). On Linux it materialises as
 * /dev/shm/cliairplay-ptp; the leading slash and short name keep it valid on
 * macOS too (POSIX shm names there are capped near 31 chars). */
#define AP2_PTP_SHM_NAME    "/cliairplay-ptp"

/* Shared-memory layout version. Readers refuse a mismatch so a future layout
 * change cannot be misread as the current one. */
#define AP2_PTP_SHM_VERSION 1u

/* localhost UDP control channel the daemon listens on; streaming processes
 * register/unregister their receiver IP(s) here. A DEDICATED port (not nqptp's
 * 9000) so a real nqptp serving shairport-sync RECEIVERS on the same host does
 * not collide with our sender-side daemon. */
#define AP2_PTP_CTRL_ADDR "127.0.0.1"
#define AP2_PTP_CTRL_PORT 9010

/* sample flags */
#define AP2_PTP_SHM_F_GRANDMASTER   0x1u  /* the daemon holds the timeline (offset 0) */
#define AP2_PTP_SHM_F_OFFSET_LOCKED 0x2u  /* slaving to a peer and >=1 offset sample folded */

/*
 * One published clock sample. All times are nanoseconds on CLOCK_REALTIME,
 * which the daemon and the streaming processes share (same host), so a stream
 * recovers master-domain time as (its own CLOCK_REALTIME now + offset).
 */
struct ap2_ptp_shm_sample {
    uint64_t update_count;            /* bumped once per publish; freshness/liveness */
    uint64_t master_clock_id;         /* elected grandmaster identity (timeline id) */
    uint64_t local_time;              /* local CLOCK_REALTIME ns at publish */
    int64_t  local_to_master_offset;  /* add to a local ns to get master-domain ns */
    uint64_t master_clock_start_time; /* master timebase epoch (daemon start), informational */
    uint32_t flags;                   /* AP2_PTP_SHM_F_* */
    uint32_t reserved;
};

/*
 * Double-buffered container. Lock-free write/read (nqptp's protocol): the
 * writer fills `main`, issues a full barrier, then copies to `secondary`; a
 * reader copies both with barriers and accepts the snapshot ONLY when the two
 * copies are byte-identical (no torn write straddled the read), retrying a
 * bounded number of times before falling back to its last good snapshot.
 */
struct ap2_ptp_shm {
    uint32_t version;                 /* == AP2_PTP_SHM_VERSION */
    uint32_t reserved;
    struct ap2_ptp_shm_sample main;
    struct ap2_ptp_shm_sample secondary;
};

/* ---- writer (daemon side) ---- */

struct ap2_ptp_shm_writer {
    int fd;
    struct ap2_ptp_shm *map;
    uint64_t seq;                     /* monotonic publish counter */
    bool owner;                       /* we created the object (unlink on close) */
};

/*
 * Create/attach the shm object read-write and stamp the layout version.
 *
 * :param w: writer state to initialise.
 * :returns: true on success.
 */
bool ap2_ptp_shm_writer_open(struct ap2_ptp_shm_writer *w);

/*
 * Publish one sample with the lock-free double-buffer protocol. The writer owns
 * update_count (the caller need not set it).
 */
void ap2_ptp_shm_publish(struct ap2_ptp_shm_writer *w, struct ap2_ptp_shm_sample s);

/* Unmap, close, and (if we created it) unlink the shm object. */
void ap2_ptp_shm_writer_close(struct ap2_ptp_shm_writer *w);

/* ---- reader (streaming side) ---- */

struct ap2_ptp_shm_reader {
    int fd;
    struct ap2_ptp_shm *map;
    struct ap2_ptp_shm_sample last;   /* last good snapshot (fallback on a torn read) */
    bool have_last;
};

/*
 * Attach the shm object read-only.
 *
 * :param r: reader state to initialise.
 * :returns: true if the object exists and its layout version matches.
 */
bool ap2_ptp_shm_reader_open(struct ap2_ptp_shm_reader *r);

/*
 * Read a tear-free snapshot. On a consistent read it updates and returns the
 * fresh sample; on the (rare) event that every retry is torn it falls back to
 * the last good sample.
 *
 * :param r: attached reader.
 * :param out: receives the snapshot.
 * :returns: true if any sample (fresh or cached) is available.
 */
bool ap2_ptp_shm_read(struct ap2_ptp_shm_reader *r, struct ap2_ptp_shm_sample *out);

/* Detach the reader. */
void ap2_ptp_shm_reader_close(struct ap2_ptp_shm_reader *r);

/* ---- control channel ---- */

/*
 * Control datagrams are plain text; the first non-space character selects the
 * command (arguments are space-separated):
 *   R <ip> [<ip> ...]   register (add) receiver IP(s) into the timing peer set
 *   U <ip> [<ip> ...]   unregister (remove) receiver IP(s)
 *   T <ip> [<ip> ...]   nqptp-compatible alias for register (ADD; see note)
 *   B | E | P           begin / end / pause (accepted + acked; no-op for a sender)
 *   ? | PING            liveness probe
 * The daemon replies to the datagram's source with a short ack, e.g.
 *   "OK peers=2 gm=0123456789abcdef role=grandmaster"
 *
 * NOTE: nqptp's "T" REPLACES its peer list; ours ADDS, because each streaming
 * process registers its own receiver independently and the daemon aggregates
 * them. Use R/U for the explicit refcounted add/remove; T is only for tools
 * that speak nqptp's vocabulary.
 */

/* Open the daemon-side control socket (localhost UDP, bound to AP2_PTP_CTRL_PORT).
 * :returns: fd, or -1 on error (e.g. another daemon already bound it). */
int ap2_ptp_ctrl_server_open(void);

/*
 * Client helper: send one control line to the local daemon and, if timeout_ms
 * > 0, wait that long for the ack.
 *
 * :param cmd: full command line, e.g. "R 10.0.0.5" or "?".
 * :param timeout_ms: how long to wait for the ack (0 = fire-and-forget).
 * :param ack: optional buffer for the ack text (NUL-terminated), or NULL.
 * :param ack_len: size of ack.
 * :returns: true if an ack was received (proves a daemon is live); false on
 *           timeout/error, or true immediately when timeout_ms == 0 and the
 *           datagram was sent.
 */
bool ap2_ptp_ctrl_send(const char *cmd, int timeout_ms, char *ack, int ack_len);

#endif /* __AP2_PTP_SHM_H_ */
