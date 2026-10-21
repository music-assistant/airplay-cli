#include <assert.h>
#include <stdint.h>

#include "ap2_timeline.h"

#define SR 44100U
#define NS 1000000000ULL

int main(void)
{
    ap2_timeline_recovery_t plan;

    assert(!ap2_timeline_plan_recovery(
        200000, 205000, 5000, 100000, 11025, 77175, &plan));

    assert(ap2_timeline_plan_recovery(
        100000, 105000, 5000, 120000, 11025, 77175, &plan));
    assert(plan.head == 197175);
    assert(plan.shifted_frames == 97175);
    assert((uint32_t)plan.head + plan.offset == 105000);

    uint64_t wrapped_head = 0xFFFFFF00ULL;
    uint32_t wrapped_offset = 0x00000200U;
    uint32_t wrapped_rtp = (uint32_t)wrapped_head + wrapped_offset;
    assert(ap2_timeline_plan_recovery(
        wrapped_head, wrapped_rtp, wrapped_offset,
        wrapped_head + 20000, 11025, 77175, &plan));
    assert((uint32_t)plan.head + plan.offset == wrapped_rtp);

    assert(!ap2_timeline_plan_recovery(
        100000, 999, 5000, 120000, 11025, 77175, &plan));

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

    uint32_t audible;
    ap2_timeline_resume_t resume;

    /* Pause 3 s into a line whose write head runs 1.75 s ahead: the freeze
     * lands on the audible frame, not the head, and the resume plays out the
     * queued 1.75 s instead of anchoring past it. */
    uint32_t anchor_rtp = 0x12340000U;
    anchor_ns = 50 * NS;
    uint32_t queued = SR * 7 / 4;
    uint32_t head_rtp = anchor_rtp + 3 * SR + queued;
    audible = ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                       anchor_ns + 3 * NS, head_rtp, SR);
    assert(audible == anchor_rtp + 3 * SR);

    uint64_t resume_ts = 900000000ULL;
    ap2_timeline_plan_buffered_resume(audible, head_rtp, resume_ts, &resume);
    assert(resume.anchor_rtp == audible);
    assert(resume.head == resume_ts + queued);
    assert((uint32_t)resume.head + resume.offset == head_rtp);

    /* Before the anchor instant nothing has rendered yet. */
    audible = ap2_timeline_audible_rtp(anchor_rtp, anchor_ns, anchor_ns - NS,
                                       head_rtp, SR);
    assert(audible == anchor_rtp);

    /* A drained receiver is capped at the write head; the resume anchors at
     * the head with nothing queued. */
    audible = ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                       anchor_ns + 60 * NS, head_rtp, SR);
    assert(audible == head_rtp);
    ap2_timeline_plan_buffered_resume(audible, head_rtp, resume_ts, &resume);
    assert(resume.anchor_rtp == head_rtp);
    assert(resume.head == resume_ts);
    assert((uint32_t)resume.head + resume.offset == head_rtp);

    /* RTP wraparound between anchor and head. */
    anchor_rtp = 0xFFFFF000U;
    head_rtp = anchor_rtp + SR;
    audible = ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                       anchor_ns + NS / 2, head_rtp, SR);
    assert(audible == anchor_rtp + SR / 2);
    ap2_timeline_plan_buffered_resume(audible, head_rtp, resume_ts, &resume);
    assert(resume.anchor_rtp == anchor_rtp + SR / 2);
    assert((uint32_t)resume.head + resume.offset == head_rtp);

    /* A head behind the anchor clamps to the head. */
    audible = ap2_timeline_audible_rtp(head_rtp, anchor_ns, anchor_ns + NS,
                                       anchor_rtp, SR);
    assert(audible == anchor_rtp);

    /* 14 h past the anchor (> 2^31 frames) with 1.75 s still queued. */
    uint64_t elapsed_s = 14 * 3600;
    uint64_t elapsed_frames = elapsed_s * SR;
    anchor_rtp = 0x12340000U;
    audible = (uint32_t)(anchor_rtp + elapsed_frames);
    head_rtp = audible + queued;
    assert(elapsed_frames > 0x80000000ULL);
    uint32_t got = ap2_timeline_audible_rtp(anchor_rtp, anchor_ns,
                                            anchor_ns + elapsed_s * NS,
                                            head_rtp, SR);
    assert(got == audible);
    ap2_timeline_plan_buffered_resume(got, head_rtp, resume_ts, &resume);
    assert((uint32_t)resume.head + resume.offset == head_rtp);
    return 0;
}
