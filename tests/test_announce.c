#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "announce.h"

#define Q15 32768

static char clip_path[512];

static void write_clip(const void *data, size_t len)
{
    FILE *f = fopen(clip_path, "wb");
    assert(f);
    if (len) assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

/* One constant-valued s16 stereo clip of `frames` frames. */
static void write_clip_s16(int16_t value, int frames, int channels)
{
    int samples = frames * channels;
    int16_t *data = malloc((size_t)samples * sizeof(*data));
    assert(data);
    for (int i = 0; i < samples; i++) data[i] = value;
    write_clip(data, (size_t)samples * sizeof(*data));
    free(data);
}

static void write_clip_s32(int32_t value, int frames, int channels)
{
    int samples = frames * channels;
    int32_t *data = malloc((size_t)samples * sizeof(*data));
    assert(data);
    for (int i = 0; i < samples; i++) data[i] = value;
    write_clip(data, (size_t)samples * sizeof(*data));
    free(data);
}

static void fill_s16(int16_t *buf, int frames, int channels, int16_t v)
{
    for (int i = 0; i < frames * channels; i++) buf[i] = v;
}

static void fill_s32(int32_t *buf, int frames, int channels, int32_t v)
{
    for (int i = 0; i < frames * channels; i++) buf[i] = v;
}

static void test_gain_from_db(void)
{
    assert(announce_gain_q15_from_db(0.0) == Q15);
    assert(announce_gain_q15_from_db(3.0) == Q15);   /* clamped to unity */
    assert(announce_gain_q15_from_db(-12.0) ==
           (int32_t)lround(pow(10.0, -12.0 / 20.0) * Q15));
    assert(announce_gain_q15_from_db(-60.0) == 0);   /* full mute */
    assert(announce_gain_q15_from_db(-80.0) == 0);
    /* monotonic in between */
    assert(announce_gain_q15_from_db(-6.0) > announce_gain_q15_from_db(-12.0));
}

static void test_envelope_shape(void)
{
    announce_t a;
    memset(&a, 0, sizeof(a));
    a.ramp_frames = 100;
    a.clip_frames = 500;
    a.duck_q15 = 8192;

    /* down ramp: unity at the clip's first sample, linear to the duck */
    assert(announce_music_gain_q15(&a, 0) == Q15);
    assert(announce_music_gain_q15(&a, 50) == 20480);
    /* hold */
    assert(announce_music_gain_q15(&a, 100) == 8192);
    assert(announce_music_gain_q15(&a, 499) == 8192);
    /* tail ramp: duck at the clip end, linear back to unity */
    assert(announce_music_gain_q15(&a, 500) == 8192);
    assert(announce_music_gain_q15(&a, 550) == 20480);
    assert(announce_music_gain_q15(&a, 600) == Q15);
    assert(announce_music_gain_q15(&a, 10000) == Q15);

    /* a clip shorter than the ramp never reaches the duck; its tail ramps
     * back from the gain the down ramp actually got to */
    a.clip_frames = 40;
    assert(announce_music_gain_q15(&a, 20) == 32768 - 4915);
    int32_t at_end = 32768 - 9830;
    assert(announce_music_gain_q15(&a, 40) == at_end);
    assert(announce_music_gain_q15(&a, 90) == at_end + (Q15 - at_end) * 50 / 100);
    assert(announce_music_gain_q15(&a, 140) == Q15);
}

/* s16, unity duck: clip added sample-for-sample, offset mapping, chunk
 * crossing, tail, done. rate 1000 makes 1 frame == 1 ms. */
static void test_s16_mix_and_mapping(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    write_clip_s16(1000, 100, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 1000020, 0.0, err, sizeof(err)));
    assert(announce_active(&a));

    int16_t buf[50 * 2];
    announce_mix_result_t r;

    /* commanded instant beyond this chunk: untouched, not started */
    fill_s16(buf, 50, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 50, 999900, &r);
    assert(!r.started && !r.done);
    for (int i = 0; i < 50 * 2; i++) assert(buf[i] == 100);

    /* chunk covering the instant: mixing begins at the exact frame offset */
    fill_s16(buf, 50, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 50, 1000000, &r);
    assert(r.started);
    assert(r.at_unix_ms == 1000020);
    assert(r.duration_ms == 100);
    assert(!r.done);
    for (int i = 0; i < 20 * 2; i++) assert(buf[i] == 100);
    for (int i = 20 * 2; i < 50 * 2; i++) assert(buf[i] == 1100);

    /* clip continues across the chunk boundary */
    fill_s16(buf, 50, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 50, 1000050, &r);
    assert(!r.started && !r.done);
    for (int i = 0; i < 50 * 2; i++) assert(buf[i] == 1100);

    /* clip EOF mid-chunk: the tail (unity duck) leaves the music untouched */
    fill_s16(buf, 50, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 50, 1000100, &r);
    assert(!r.done);
    for (int i = 0; i < 20 * 2; i++) assert(buf[i] == 1100);
    for (int i = 20 * 2; i < 50 * 2; i++) assert(buf[i] == 100);

    /* drain the 200-frame tail; done fires when its last frame is mixed */
    int done_calls = 0;
    for (uint64_t head = 1000150; head <= 1000300; head += 50) {
        fill_s16(buf, 50, 2, 100);
        announce_mix_chunk(&a, (uint8_t *)buf, 50, head, &r);
        assert(!r.started);
        if (r.done) done_calls++;
    }
    assert(done_calls == 1);
    assert(!announce_active(&a));

    /* mixing after done is a no-op */
    fill_s16(buf, 50, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 50, 1000350, &r);
    assert(!r.started && !r.done);
    for (int i = 0; i < 50 * 2; i++) assert(buf[i] == 100);
}

static void test_s16_saturation(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    write_clip_s16(32000, 10, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));

    int16_t buf[10 * 2];
    announce_mix_result_t r;
    fill_s16(buf, 10, 2, 32000);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 500, &r);
    assert(r.started && r.at_unix_ms == 500);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == INT16_MAX);
    announce_cancel(&a);

    write_clip_s16(-32000, 10, 2);
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, -32000);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 500, &r);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == INT16_MIN);
    announce_cancel(&a);
}

/* Duck applied to the music: mute duck zeroes it in the hold, halves it at
 * the ramp midpoint, leaves it whole at the clip's first sample. */
static void test_s16_duck_envelope(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    write_clip_s16(1234, 500, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, -60.0, err, sizeof(err)));
    assert(a.ramp_frames == 200);

    int16_t buf[500 * 2];
    announce_mix_result_t r;
    fill_s16(buf, 500, 2, 999);
    announce_mix_chunk(&a, (uint8_t *)buf, 500, 1000, &r);
    assert(r.started);
    /* frame 0: unity gain */
    assert(buf[0] == 999 + 1234);
    /* frame 100: mid-ramp, gain 16384 -> music 999*16384>>15 = 499 */
    assert(buf[100 * 2] == 499 + 1234);
    /* frames 200..499: hold, music fully muted */
    assert(buf[200 * 2] == 1234);
    assert(buf[499 * 2] == 1234);
}

static void test_s32_mix_and_saturation(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 4);
    write_clip_s32(100000, 10, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));

    int32_t buf[10 * 2];
    announce_mix_result_t r;
    fill_s32(buf, 10, 2, 200000);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 500, &r);
    assert(r.started);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == 300000);
    announce_cancel(&a);

    /* positive rail */
    write_clip_s32(100000, 10, 2);
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s32(buf, 10, 2, INT32_MAX - 10);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 500, &r);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == INT32_MAX);
    announce_cancel(&a);

    /* negative rail */
    write_clip_s32(-100000, 10, 2);
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s32(buf, 10, 2, INT32_MIN + 10);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 500, &r);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == INT32_MIN);
    announce_cancel(&a);
}

/* An instant already behind the head (or 0) starts at the chunk's first
 * frame, and the reported instant is the corrected truth. */
static void test_at_clamp(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    char err[128];
    int16_t buf[10 * 2];
    announce_mix_result_t r;

    write_clip_s16(500, 5, 2);
    assert(announce_arm(&a, clip_path, 900000, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 1000000, &r);
    assert(r.started && r.at_unix_ms == 1000000);
    assert(buf[0] == 600);
    announce_cancel(&a);

    write_clip_s16(500, 5, 2);
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 1000000, &r);
    assert(r.started && r.at_unix_ms == 1000000);
    announce_cancel(&a);

    /* an unknown head (0) leaves the chunk untouched and the clip waiting */
    write_clip_s16(500, 5, 2);
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 0, &r);
    assert(!r.started);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == 100);
    announce_cancel(&a);

    /* an instant so far out the frame math would wrap keeps the clip
     * waiting instead of starting it on a wrapped offset */
    write_clip_s16(500, 5, 2);
    assert(announce_arm(&a, clip_path, UINT64_MAX - 1, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 1000000, &r);
    assert(!r.started);
    for (int i = 0; i < 10 * 2; i++) assert(buf[i] == 100);
    announce_cancel(&a);
}

/* started and done can land in one call when clip + tail fit one chunk. */
static void test_tiny_clip_single_chunk(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    write_clip_s16(500, 2, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    int16_t buf[300 * 2];
    announce_mix_result_t r;
    fill_s16(buf, 300, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 300, 1000, &r);
    assert(r.started && r.done);
    assert(r.duration_ms == 2);
    assert(!announce_active(&a));
    assert(buf[0] == 600 && buf[1 * 2] == 600 && buf[2 * 2] == 100);
}

/* A chunk larger than the staging buffer is consumed in slices. */
static void test_staging_boundary(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);   /* 4 B/frame; staging holds 1024 frames */
    write_clip_s16(7, 3000, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    int16_t *buf = malloc(2000 * 2 * sizeof(*buf));
    assert(buf);
    announce_mix_result_t r;

    fill_s16(buf, 2000, 2, 5);
    announce_mix_chunk(&a, (uint8_t *)buf, 2000, 1000, &r);
    assert(r.started && !r.done);
    for (int i = 0; i < 2000 * 2; i++) assert(buf[i] == 12);

    fill_s16(buf, 2000, 2, 5);
    announce_mix_chunk(&a, (uint8_t *)buf, 2000, 3000, &r);
    assert(r.done);
    for (int i = 0; i < 1000 * 2; i++) assert(buf[i] == 12);
    for (int i = 1000 * 2; i < 2000 * 2; i++) assert(buf[i] == 5);
    free(buf);
}

static void test_replace_and_cancel(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    write_clip_s16(500, 100, 2);

    char err[128];
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    int16_t buf[10 * 2];
    announce_mix_result_t r;
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 1000, &r);
    assert(r.started && announce_active(&a));

    /* cancel reports exactly once */
    assert(announce_cancel(&a));
    assert(!announce_active(&a));
    assert(!announce_cancel(&a));

    /* re-arm after cancel plays the new clip from byte 0 */
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    fill_s16(buf, 10, 2, 100);
    announce_mix_chunk(&a, (uint8_t *)buf, 10, 2000, &r);
    assert(r.started);
    assert(buf[0] == 600);
    assert(announce_cancel(&a));
}

static void test_file_validation(void)
{
    announce_t a;
    announce_init(&a, 1000, 2, 2);
    char err[128];

    assert(!announce_arm(&a, "/nonexistent/announce-clip.pcm", 0, 0.0,
                         err, sizeof(err)));
    assert(!announce_active(&a));

    write_clip("", 0);
    assert(!announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));

    /* less than one frame is empty after alignment */
    write_clip("abc", 3);
    assert(!announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));

    /* a trailing partial frame is dropped */
    uint8_t ten[10] = {0};
    write_clip(ten, sizeof(ten));
    assert(announce_arm(&a, clip_path, 0, 0.0, err, sizeof(err)));
    assert(a.clip_bytes == 8);
    assert(a.clip_frames == 2);
    assert(a.duration_ms == 2);
    assert(announce_cancel(&a));
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(clip_path, sizeof(clip_path), "%s/test_announce_clip_%d.pcm",
             tmp && *tmp ? tmp : "/tmp", (int)getpid());

    test_gain_from_db();
    test_envelope_shape();
    test_s16_mix_and_mapping();
    test_s16_saturation();
    test_s16_duck_envelope();
    test_s32_mix_and_saturation();
    test_at_clamp();
    test_tiny_clip_single_chunk();
    test_staging_boundary();
    test_replace_and_cancel();
    test_file_validation();

    remove(clip_path);
    puts("announce mixing tests passed");
    return 0;
}
