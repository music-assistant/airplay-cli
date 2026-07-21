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

#endif /* __AP2_TIMELINE_H_ */
