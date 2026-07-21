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

    /* Repeated recoveries preserve contiguous RTP while shifting the PTP
     * anchor line by the same number of frames each time. */
    uint64_t head = 100000;
    uint32_t rtp = 105000;
    uint32_t offset = 5000;
    uint64_t anchor_wall = 10000000000ULL;
    uint32_t anchor_position = rtp;
    uint64_t wall = 15000000000ULL;
    ap2_timeline_ptp_anchor_t before, after;
    assert(ap2_timeline_ptp_anchor(
        wall, anchor_wall, anchor_position, 44100, 2000, &before));

    assert(ap2_timeline_plan_recovery(
        head, rtp, offset, 120000, 11025, 77175, &plan));
    head = plan.head;
    offset = plan.offset;
    anchor_wall += ap2_timeline_frames_to_ns(plan.shifted_frames, 44100);
    assert((uint32_t)head + offset == rtp);
    assert(ap2_timeline_ptp_anchor(
        wall, anchor_wall, anchor_position, 44100, 2000, &after));
    assert(before.frame_1 - after.frame_1 == plan.shifted_frames);
    assert(after.frame_2 - after.frame_1 == 77175);

    head += 352;
    rtp += 352;
    assert((uint32_t)head + offset == rtp);
    wall = 25000000000ULL;
    assert(ap2_timeline_ptp_anchor(
        wall, anchor_wall, anchor_position, 44100, 2000, &before));
    assert(ap2_timeline_plan_recovery(
        head, rtp, offset, 300000, 11025, 77175, &plan));
    head = plan.head;
    offset = plan.offset;
    anchor_wall += ap2_timeline_frames_to_ns(plan.shifted_frames, 44100);
    assert((uint32_t)head + offset == rtp);
    assert(ap2_timeline_ptp_anchor(
        wall, anchor_wall, anchor_position, 44100, 2000, &after));
    assert(before.frame_1 - after.frame_1 == plan.shifted_frames);
    assert(after.frame_2 - after.frame_1 == 77175);

    /* Periodic PTP anchors remain valid beyond the old signed-multiply
     * overflow point (about 53 hours at 48 kHz). */
    ap2_timeline_ptp_anchor_t long_stream;
    uint64_t seventy_two_hours_ns = 72ULL * 60 * 60 * 1000000000ULL;
    assert(ap2_timeline_ptp_anchor(
        10000000000ULL + 2000000000ULL + seventy_two_hours_ns,
        10000000000ULL, 1234, 48000, 2000, &long_stream));
    assert(long_stream.frame_1 ==
           1234U + (uint32_t)(72ULL * 60 * 60 * 48000) + 11035U);
    assert(long_stream.frame_2 - long_stream.frame_1 == 77175);

    /* Refuse recovery when the caller's existing invariant is already broken. */
    assert(!ap2_timeline_plan_recovery(
        100000, 999, 5000, 120000, 11025, 77175, &plan));

    /* Repeated starvation recovery preserves the RTP/head invariant and moves
     * the PTP anchor by exactly the same media duration on every re-anchor. */
    uint64_t repeat_head = 100000;
    uint32_t repeat_offset = 5000;
    uint32_t repeat_rtp = 105000;
    uint64_t anchor_ns = 7000000000ULL;
    for (int i = 0; i < 5; i++) {
        assert(ap2_timeline_plan_recovery(
            repeat_head, repeat_rtp, repeat_offset,
            repeat_head + 20000, 11025, 77175, &plan));
        anchor_ns += ap2_timeline_frames_to_ns(
            plan.shifted_frames, 44100);
        repeat_head = plan.head;
        repeat_offset = plan.offset;
        assert((uint32_t)repeat_head + repeat_offset == repeat_rtp);
    }
    assert(anchor_ns > 7000000000ULL);
    return 0;
}
