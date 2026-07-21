#include <assert.h>
#include <stdint.h>

#include "ap2_timeline.h"

int main(void)
{
    ap2_timeline_recovery_t plan;

    /* A healthy lead must not be disturbed. */
    assert(!ap2_timeline_plan_recovery(
        200000, 205000, 5000, 100000, 11025, 77175, &plan));

    /* A late contiguous RTP sample is moved into the future without changing
     * its RTP timestamp; the offset absorbs the wall-time shift. */
    assert(ap2_timeline_plan_recovery(
        100000, 105000, 5000, 120000, 11025, 77175, &plan));
    assert(plan.head == 197175);
    assert(plan.shifted_frames == 97175);
    assert((uint32_t)plan.head + plan.offset == 105000);

    /* The same invariant holds across uint32 wraparound. */
    uint64_t wrapped_head = 0xFFFFFF00ULL;
    uint32_t wrapped_offset = 0x00000200U;
    uint32_t wrapped_rtp = (uint32_t)wrapped_head + wrapped_offset;
    assert(ap2_timeline_plan_recovery(
        wrapped_head, wrapped_rtp, wrapped_offset,
        wrapped_head + 20000, 11025, 77175, &plan));
    assert((uint32_t)plan.head + plan.offset == wrapped_rtp);

    /* Refuse recovery when the caller's existing invariant is already broken. */
    assert(!ap2_timeline_plan_recovery(
        100000, 999, 5000, 120000, 11025, 77175, &plan));

    /* Repeated starvation recovery preserves the RTP/head invariant and moves
     * the PTP anchor by exactly the same media duration on every re-anchor. */
    uint64_t head = 100000;
    uint32_t offset = 5000;
    uint32_t rtp = 105000;
    uint64_t anchor_ns = 7000000000ULL;
    for (int i = 0; i < 5; i++) {
        assert(ap2_timeline_plan_recovery(
            head, rtp, offset, head + 20000, 11025, 77175, &plan));
        anchor_ns += ap2_timeline_frames_to_ns(
            plan.shifted_frames, 44100);
        head = plan.head;
        offset = plan.offset;
        assert((uint32_t)head + offset == rtp);
    }
    assert(anchor_ns > 7000000000ULL);
    return 0;
}
