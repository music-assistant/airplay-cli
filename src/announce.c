/*
 * Announcement mixing over the outgoing PCM stream (see announce.h).
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "announce.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define Q15_UNITY 32768

static int clip_stage_frames(announce_t *a, int max_frames,
                             unsigned frame_bytes);
static void mix_slice_s16(announce_t *a, int16_t *music, const int16_t *clip,
                          int frames, unsigned channels);
static void mix_slice_s32(announce_t *a, int32_t *music, const int32_t *clip,
                          int frames, unsigned channels);

void announce_init(announce_t *a, unsigned sample_rate, unsigned channels,
                   unsigned bytes_per_sample)
{
    memset(a, 0, sizeof(*a));
    a->sample_rate = sample_rate;
    a->channels = channels;
    a->bytes_per_sample = bytes_per_sample;
    a->fd = -1;
}

bool announce_arm(announce_t *a, const char *path, uint64_t at_unix_ms,
                  double duck_db, char *error, size_t error_len)
{
    unsigned frame_bytes = a->channels * a->bytes_per_sample;
    if (!a->sample_rate || !frame_bytes) {
        snprintf(error, error_len, "announce module not initialised");
        return false;
    }
    /* The caller reports an active clip's cancellation before re-arming; this
     * is only the leak guard for a caller that did not. */
    if (a->active) announce_cancel(a);

    /* O_NONBLOCK so a FIFO with no writer cannot block the open (and with it
     * the caller's audio lock) before the regular-file check below rejects
     * it; reads from the regular files this accepts are unaffected. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(error, error_len, "cannot open the announcement clip: %s",
                 strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(error, error_len,
                 "the announcement clip is not a regular file");
        return false;
    }
    uint64_t clip_bytes =
        (uint64_t)st.st_size - (uint64_t)st.st_size % frame_bytes;
    if (!clip_bytes) {
        close(fd);
        snprintf(error, error_len, "the announcement clip is empty");
        return false;
    }

    a->active = true;
    a->started = false;
    a->fd = fd;
    a->clip_bytes = clip_bytes;
    a->clip_bytes_read = 0;
    a->clip_frames = clip_bytes / frame_bytes;
    a->at_unix_ms = at_unix_ms;
    a->duration_ms =
        clip_bytes * 1000ULL / ((uint64_t)a->sample_rate * frame_bytes);
    a->mixed_frames = 0;
    a->ramp_frames =
        (uint32_t)((uint64_t)ANNOUNCE_RAMP_MS * a->sample_rate / 1000);
    a->duck_q15 = announce_gain_q15_from_db(duck_db);
    return true;
}

bool announce_active(const announce_t *a)
{
    return a->active;
}

bool announce_cancel(announce_t *a)
{
    if (!a->active) return false;
    if (a->fd >= 0) close(a->fd);
    a->fd = -1;
    a->active = false;
    a->started = false;
    return true;
}

void announce_mix_chunk(announce_t *a, uint8_t *buf, int frames,
                        uint64_t head_unix_ms, announce_mix_result_t *result)
{
    memset(result, 0, sizeof(*result));
    if (!a->active || frames <= 0 || !head_unix_ms) return;

    int offset = 0;
    if (!a->started) {
        /* Map the commanded instant onto this chunk's audible window. An
         * instant at or behind the chunk head (including 0) starts at its
         * first frame — the earliest unsent one — and the reported instant is
         * the corrected truth. The clip always plays from byte 0. */
        if (a->at_unix_ms > head_unix_ms) {
            uint64_t delta_ms = a->at_unix_ms - head_unix_ms;
            /* An instant so far out that the frame math would wrap can only
             * be a corrupt command; keep the clip waiting instead of starting
             * it on a wrapped offset. */
            if (delta_ms > UINT64_MAX / a->sample_rate) return;
            uint64_t off = delta_ms * a->sample_rate / 1000;
            if (off >= (uint64_t)frames) return;   /* not reached yet */
            offset = (int)off;
        }
        a->started = true;
        result->started = true;
        result->at_unix_ms =
            head_unix_ms + (uint64_t)offset * 1000 / a->sample_rate;
        result->duration_ms = a->duration_ms;
    }

    unsigned frame_bytes = a->channels * a->bytes_per_sample;
    while (offset < frames) {
        int slice = clip_stage_frames(a, frames - offset, frame_bytes);
        bool have_clip = slice > 0;
        if (!have_clip) {
            /* Clip exhausted: the tail ramp keeps scaling the music with no
             * clip data added until the envelope returns to unity. */
            uint64_t total = a->clip_frames + a->ramp_frames;
            if (a->mixed_frames >= total) break;
            uint64_t left = total - a->mixed_frames;
            slice = frames - offset;
            if ((uint64_t)slice > left) slice = (int)left;
        }
        uint8_t *dst = buf + (size_t)offset * frame_bytes;
        const uint8_t *clip = have_clip ? a->staging.bytes : NULL;
        if (a->bytes_per_sample == 2)
            mix_slice_s16(a, (int16_t *)dst, (const int16_t *)clip, slice,
                          a->channels);
        else
            mix_slice_s32(a, (int32_t *)dst, (const int32_t *)clip, slice,
                          a->channels);
        offset += slice;
    }

    if (a->mixed_frames >= a->clip_frames + a->ramp_frames) {
        announce_cancel(a);
        result->done = true;
    }
}

int32_t announce_gain_q15_from_db(double duck_db)
{
    if (duck_db <= ANNOUNCE_MUTE_DB) return 0;
    if (duck_db >= 0.0) return Q15_UNITY;
    return (int32_t)lround(pow(10.0, duck_db / 20.0) * Q15_UNITY);
}

/* Music gain at frame t since the clip's first sample: linear ramp down over
 * ramp_frames, hold at duck for the clip, linear ramp back up after its last
 * sample. A clip shorter than the ramp never reaches the full duck, so its
 * tail ramps back from the gain the down ramp actually got to. */
int32_t announce_music_gain_q15(const announce_t *a, uint64_t t_frames)
{
    uint32_t ramp = a->ramp_frames;
    if (!ramp) return t_frames < a->clip_frames ? a->duck_q15 : Q15_UNITY;
    if (t_frames < a->clip_frames) {
        if (t_frames >= ramp) return a->duck_q15;
        return Q15_UNITY + (int32_t)((int64_t)(a->duck_q15 - Q15_UNITY) *
                                     (int64_t)t_frames / ramp);
    }
    uint64_t tail = t_frames - a->clip_frames;
    if (tail >= ramp) return Q15_UNITY;
    int32_t from = a->clip_frames >= ramp
        ? a->duck_q15
        : Q15_UNITY + (int32_t)((int64_t)(a->duck_q15 - Q15_UNITY) *
                                (int64_t)a->clip_frames / ramp);
    return from + (int32_t)((int64_t)(Q15_UNITY - from) * (int64_t)tail / ramp);
}

/* ---- private helpers ---- */

/* Stage the next run of clip frames (0 = clip fully consumed). A read that
 * delivers less than the fstat'd size promised ends the clip at the last
 * whole frame delivered, so the tail ramp begins from the gain reached. */
static int clip_stage_frames(announce_t *a, int max_frames,
                             unsigned frame_bytes)
{
    uint64_t left = (a->clip_bytes - a->clip_bytes_read) / frame_bytes;
    if (!left) return 0;
    int want = max_frames;
    if ((uint64_t)want > left) want = (int)left;
    int cap = (int)(ANNOUNCE_STAGING_BYTES / frame_bytes);
    if (want > cap) want = cap;
    int want_bytes = want * (int)frame_bytes;
    int got = 0;
    while (got < want_bytes) {
        ssize_t n =
            read(a->fd, a->staging.bytes + got, (size_t)(want_bytes - got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (int)n;
    }
    if (got < want_bytes) {
        got -= got % (int)frame_bytes;
        a->clip_bytes_read += (uint64_t)got;
        a->clip_bytes = a->clip_bytes_read;
        a->clip_frames = a->clip_bytes / frame_bytes;
        return got / (int)frame_bytes;
    }
    a->clip_bytes_read += (uint64_t)got;
    return want;
}

static inline int16_t clamp_s16(int32_t v)
{
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

static inline int32_t clamp_s32(int64_t v)
{
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

/* Interleaved samples are mixed channel-agnostically: one envelope gain per
 * frame, applied to every sample in it. clip == NULL mixes the tail (music
 * scaling only). */
static void mix_slice_s16(announce_t *a, int16_t *music, const int16_t *clip,
                          int frames, unsigned channels)
{
    for (int f = 0; f < frames; f++) {
        int32_t gain = announce_music_gain_q15(a, a->mixed_frames);
        for (unsigned c = 0; c < channels; c++) {
            unsigned i = (unsigned)f * channels + c;
            int32_t acc = (int32_t)(((int64_t)music[i] * gain) >> 15);
            if (clip) acc += clip[i];
            music[i] = clamp_s16(acc);
        }
        a->mixed_frames++;
    }
}

static void mix_slice_s32(announce_t *a, int32_t *music, const int32_t *clip,
                          int frames, unsigned channels)
{
    for (int f = 0; f < frames; f++) {
        int32_t gain = announce_music_gain_q15(a, a->mixed_frames);
        for (unsigned c = 0; c < channels; c++) {
            unsigned i = (unsigned)f * channels + c;
            int64_t acc = ((int64_t)music[i] * gain) >> 15;
            if (clip) acc += clip[i];
            music[i] = clamp_s32(acc);
        }
        a->mixed_frames++;
    }
}
