#include <assert.h>
#include <stdint.h>

#include "ap2_timeline.h"

#define SR 44100U
#define NS 1000000000ULL

int main(void)
{
    uint32_t audible = 0;
    ap2_timeline_resume_t plan;

    /* Pause 3 s into a line whose write head runs 1.75 s ahead: the freeze
     * lands on the audible frame, not the head, and the resume plays out the
     * queued 1.75 s instead of anchoring past it. */
    uint32_t anchor_rtp = 0x12340000U;
    uint64_t anchor_ns = 50 * NS;
    uint32_t queued = SR * 7 / 4;
    uint32_t head_rtp = anchor_rtp + 3 * SR + queued;
    assert(ap2_timeline_audible_rtp(anchor_rtp, anchor_ns, anchor_ns + 3 * NS,
                                    head_rtp, SR, &audible));
    assert(audible == anchor_rtp + 3 * SR);

    uint64_t resume_ts = 900000000ULL;
    ap2_timeline_plan_buffered_resume(audible, 352, head_rtp, resume_ts, &plan);
    assert(plan.anchor_rtp == audible + 352);
    assert(plan.head == resume_ts + queued - 352);
    assert((uint32_t)plan.head + plan.offset == head_rtp);

    /* Before the anchor instant nothing has rendered yet. */
    assert(ap2_timeline_audible_rtp(anchor_rtp, anchor_ns, anchor_ns - NS,
                                    head_rtp, SR, &audible));
    assert(audible == anchor_rtp);

    /* A drained receiver is capped at the write head; the resume anchors at
     * the head with nothing queued. */
    assert(ap2_timeline_audible_rtp(anchor_rtp, anchor_ns, anchor_ns + 60 * NS,
                                    head_rtp, SR, &audible));
    assert(audible == head_rtp);
    ap2_timeline_plan_buffered_resume(audible, 352, head_rtp, resume_ts, &plan);
    assert(plan.anchor_rtp == head_rtp);
    assert(plan.head == resume_ts);
    assert((uint32_t)plan.head + plan.offset == head_rtp);

    /* RTP wraparound between anchor and head. */
    anchor_rtp = 0xFFFFF000U;
    head_rtp = anchor_rtp + SR;
    assert(ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                    anchor_ns + NS / 2, head_rtp, SR,
                                    &audible));
    assert(audible == anchor_rtp + SR / 2);
    ap2_timeline_plan_buffered_resume(audible, 352, head_rtp, resume_ts, &plan);
    assert(plan.anchor_rtp == anchor_rtp + SR / 2 + 352);
    assert((uint32_t)plan.head + plan.offset == head_rtp);

    /* A head behind the anchor clamps to the head. */
    assert(ap2_timeline_audible_rtp(head_rtp, anchor_ns, anchor_ns + NS,
                                    anchor_rtp, SR, &audible));
    assert(audible == anchor_rtp);

    /* 14 h past the anchor (> 2^31 frames) with 1.75 s still queued. */
    uint64_t elapsed_s = 14 * 3600;
    uint64_t elapsed_frames = elapsed_s * SR;
    anchor_rtp = 0x12340000U;
    audible = (uint32_t)(anchor_rtp + elapsed_frames);
    head_rtp = audible + queued;
    uint32_t got = 0;
    assert(elapsed_frames > 0x80000000ULL);
    assert(ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                    anchor_ns + elapsed_s * NS, head_rtp, SR,
                                    &got));
    assert(got == audible);
    ap2_timeline_plan_buffered_resume(got, 352, head_rtp, resume_ts, &plan);
    assert((uint32_t)plan.head + plan.offset == head_rtp);

    /* Unusable inputs leave the caller on its fallback. */
    audible = 7;
    assert(!ap2_timeline_audible_rtp(anchor_rtp, anchor_ns, 0, head_rtp, SR,
                                     &audible));
    assert(audible == 7);
    return 0;
}
