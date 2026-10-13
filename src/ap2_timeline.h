/*
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AP2_TIMELINE_H_
#define __AP2_TIMELINE_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t head;
    uint64_t shifted_frames;
    uint32_t offset;
} ap2_timeline_recovery_t;

static inline uint64_t ap2_timeline_frames_to_ns(uint64_t frames,
                                                 uint32_t sample_rate)
{
    return (frames / sample_rate) * 1000000000ULL +
           ((frames % sample_rate) * 1000000000ULL) / sample_rate;
}

/*
 * Plan a recovery after PCM starvation exhausted the realtime lead.
 *
 * Audio RTP stays contiguous (no content is skipped); the scheduling head is
 * moved into the future and the effective offset moves backward by the same
 * amount, preserving the modulo-32-bit invariant:
 *
 *     rtp_timestamp == (uint32_t)head + offset
 */
static inline bool ap2_timeline_plan_recovery(
    uint64_t head, uint32_t rtp_timestamp, uint32_t offset,
    uint64_t now, uint64_t floor, uint64_t recovery_lead,
    ap2_timeline_recovery_t *plan)
{
    if (!plan || head > now + floor) return false;
    uint64_t desired = now + recovery_lead;
    if (desired <= head) return false;

    uint64_t shifted = desired - head;
    uint32_t adjusted = offset - (uint32_t)shifted;
    if ((uint32_t)desired + adjusted != rtp_timestamp) return false;

    plan->head = desired;
    plan->shifted_frames = shifted;
    plan->offset = adjusted;
    return true;
}

/*
 * Frame a receiver is rendering at now_ns on a rate-1 buffered anchor
 * (anchor_rtp renders at anchor_ns), clamped to the write head head_rtp.
 * Wrap-safe past 2^31 frames. False on unusable inputs.
 */
static inline bool ap2_timeline_audible_rtp(
    uint32_t anchor_rtp, uint64_t anchor_ns, uint64_t now_ns,
    uint32_t head_rtp, uint32_t sample_rate, uint32_t *audible)
{
    if (!audible || !sample_rate || !now_ns) return false;
    uint64_t elapsed = now_ns > anchor_ns ? now_ns - anchor_ns : 0;
    uint64_t frames = (elapsed / 1000000000ULL) * sample_rate +
                      ((elapsed % 1000000000ULL) * sample_rate) / 1000000000ULL;
    uint32_t rtp = anchor_rtp + (uint32_t)frames;
    if ((int32_t)(head_rtp - rtp) < 0) rtp = head_rtp;
    *audible = rtp;
    return true;
}

typedef struct {
    uint32_t anchor_rtp;
    uint64_t head;
    uint32_t offset;
} ap2_timeline_resume_t;

/*
 * Plan a buffered resume from the frame frozen by the pause. The rate-1
 * anchor lands lag_frames past it (capped at the write head) at resume_ts,
 * the wire head stays put, and the scheduling head counts the frames the
 * receiver still holds, keeping rtp_timestamp == (uint32_t)head + offset.
 */
static inline void ap2_timeline_plan_buffered_resume(
    uint32_t paused_rtp, uint32_t lag_frames, uint32_t head_rtp,
    uint64_t resume_ts, ap2_timeline_resume_t *plan)
{
    uint32_t queued = head_rtp - paused_rtp;
    if (lag_frames > queued) lag_frames = queued;
    plan->anchor_rtp = paused_rtp + lag_frames;
    plan->head = resume_ts + (queued - lag_frames);
    plan->offset = head_rtp - (uint32_t)plan->head;
}

#endif /* __AP2_TIMELINE_H_ */
