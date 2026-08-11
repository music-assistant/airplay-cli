/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ANNOUNCE_H_
#define __ANNOUNCE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Announcement mixing (ACTION=ANNOUNCE): a short PCM clip, pre-rendered by the
 * caller to exactly the session's stdin format, is mixed over the outgoing
 * music at the frame-pull point with the music ducked under it. The group
 * timeline is untouched — announcement frames are ordinary content frames.
 *
 * The module is pure state + arithmetic plus incremental reads of the clip
 * file; it emits nothing and takes no locks. The caller owns the locking and
 * reports the returned lifecycle events on its status channel.
 */

/* Music duck ramp on either side of the clip (ms): the music gain runs
 * 1.0 -> duck over this window from the clip's first sample, holds for the
 * clip, and returns duck -> 1.0 over the same window after its last sample. */
#define ANNOUNCE_RAMP_MS 200

/* Duck levels at or below this many dB mute the music outright. */
#define ANNOUNCE_MUTE_DB (-60.0)

/* Per-slice clip staging buffer: the clip is consumed incrementally, never
 * loaded whole (a hi-res clip can run past 100 MB). */
#define ANNOUNCE_STAGING_BYTES 4096

typedef struct announce_s {
    /* Session PCM format, fixed at announce_init. */
    unsigned sample_rate;
    unsigned channels;
    unsigned bytes_per_sample;   /* 2 = s16le, 4 = s32le carrier (24-bit) */

    /* Armed clip; live between announce_arm and done/cancel. */
    bool active;
    bool started;                /* first clip sample committed to a chunk */
    int fd;                      /* clip file, owned while active */
    uint64_t clip_bytes;         /* frame-aligned clip payload size */
    uint64_t clip_bytes_read;
    uint64_t clip_frames;
    uint64_t at_unix_ms;         /* requested instant; 0 = earliest feasible */
    uint64_t duration_ms;
    uint64_t mixed_frames;       /* frames mixed since the clip's first sample */
    uint32_t ramp_frames;
    int32_t duck_q15;            /* music gain during the hold (Q15) */

    union {
        uint8_t bytes[ANNOUNCE_STAGING_BYTES];
        int32_t words[ANNOUNCE_STAGING_BYTES / 4];
    } staging;
} announce_t;

/* One mix call's observable outcome, for the caller's status lines. */
typedef struct {
    bool started;            /* the clip's first sample went into this chunk */
    uint64_t at_unix_ms;     /* audible instant of that first sample */
    uint64_t duration_ms;    /* clip length (valid with started) */
    bool done;               /* clip and tail ramp fully mixed; state idle */
} announce_mix_result_t;

/* Bind the module to the session's PCM format, before any clip is armed.
 * Call on an idle instance only: a live clip's file descriptor would leak. */
void announce_init(announce_t *a, unsigned sample_rate, unsigned channels,
                   unsigned bytes_per_sample);

/*
 * Arm a clip file for mixing. The file is raw headerless PCM in exactly the
 * session format; a trailing partial frame is dropped. at_unix_ms of 0 (or one
 * already behind the delivery head) starts at the earliest unsent frame.
 * duck_db is the music gain during the clip (dB, <= 0; <= -60 mutes). On
 * failure writes a one-line reason into error and arms nothing.
 */
bool announce_arm(announce_t *a, const char *path, uint64_t at_unix_ms,
                  double duck_db, char *error, size_t error_len);

bool announce_active(const announce_t *a);

/* Drop an armed clip. Returns true when one was active (the caller reports
 * the cancellation). */
bool announce_cancel(announce_t *a);

/*
 * Mix the active clip into one finalized PCM chunk about to be sent.
 * head_unix_ms is the audible wall-clock instant of the chunk's first frame
 * (0 = unknown: the chunk is left untouched). Music samples are scaled by the
 * duck envelope, clip samples added at unity, with a saturating clamp.
 */
void announce_mix_chunk(announce_t *a, uint8_t *buf, int frames,
                        uint64_t head_unix_ms, announce_mix_result_t *result);

/* Pure gain math, exposed for the unit tests. Q15: 32768 = unity. */
int32_t announce_gain_q15_from_db(double duck_db);
int32_t announce_music_gain_q15(const announce_t *a, uint64_t t_frames);

#endif /* __ANNOUNCE_H_ */
