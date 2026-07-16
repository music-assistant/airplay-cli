/*
 * ALAC encoder wrapper with proper 24-bit support.
 *
 * Overrides alac_create_encoder from libcodecs with a version that
 * sets mFormatFlags correctly based on sample_size.
 *
 * The original in libcodecs hardcodes mFormatFlags=1 (16-bit).
 * This version sets: 16-bit=1, 20-bit=2, 24-bit=3, 32-bit=4.
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "ALACEncoder.h"
#include "ALACBitUtilities.h"
#include "alac_wrapper.h"

/* Must match the struct layout in libcodecs alac_wrapper.cpp */
typedef struct alac_codec_s_ext {
    AudioFormatDescription inputFormat, outputFormat;
    ALACEncoder *encoder;
    void *Decoder;
    unsigned block_size, frames_per_packet;
} alac_codec_ext_t;

/*
 * Override alac_create_encoder with proper bit depth handling.
 * This symbol takes precedence over the one in libcodecs.a because
 * object files are linked before static libraries.
 */
/* Also provide the other functions that were in the removed alac_wrapper.o */

extern "C" bool pcm_to_alac(struct alac_codec_s *codec_opaque, uint8_t *in, int frames,
                             uint8_t **out, int *size) {
    alac_codec_ext_t *codec = (alac_codec_ext_t *)codec_opaque;
    *size = frames * codec->inputFormat.mBytesPerFrame;
    *out = (uint8_t *)calloc(2 * codec->outputFormat.mFramesPerPacket *
                              codec->inputFormat.mBytesPerFrame + kALACMaxEscapeHeaderBytes, 1);
    return !codec->encoder->Encode(codec->inputFormat, codec->outputFormat, in, *out, size);
}

extern "C" bool pcm_to_alac_raw(uint8_t *sample, int frames, uint8_t **out, int *size, int bsize) {
    /* Raw ALAC framing for 16-bit stereo - copied from original */
    uint8_t *p;
    uint32_t *in = (uint32_t *)sample;
    int count;
    frames = std::min(frames, bsize);
    *out = (uint8_t *)malloc(bsize * 4 + 16);
    p = *out;
    *p++ = (1 << 5);
    *p++ = 0;
    *p++ = (1 << 4) | (1 << 1) | ((bsize & 0x80000000) >> 31);
    *p++ = ((bsize & 0x7f800000) << 1) >> 24;
    *p++ = ((bsize & 0x007f8000) << 1) >> 16;
    *p++ = ((bsize & 0x00007f80) << 1) >> 8;
    *p = ((bsize & 0x0000007f) << 1);
    *p++ |= (*in & 0x00008000) >> 15;
    count = frames - 1;
    while (count--) {
        *p++ = ((*in & 0x00007f80) >> 7);
        *p++ = ((*in & 0x0000007f) << 1) | ((*in & 0x80000000) >> 31);
        *p++ = ((*in & 0x7f800000) >> 23);
        *p++ = ((*in & 0x007f0000) >> 15) | ((*(in + 1) & 0x00008000) >> 15);
        in++;
    }
    *p++ = ((*in & 0x00007f80) >> 7);
    *p++ = ((*in & 0x0000007f) << 1) | ((*in & 0x80000000) >> 31);
    *p++ = ((*in & 0x7f800000) >> 23);
    *p++ = ((*in & 0x007f0000) >> 15);
    count = (bsize - frames) * 4;
    while (count--) *p++ = 0;
    *(p - 1) |= 1;
    *p = (7 >> 1) << 6;
    *size = p - *out + 1;
    return true;
}

extern "C" void alac_delete_encoder(struct alac_codec_s *codec_opaque) {
    alac_codec_ext_t *codec = (alac_codec_ext_t *)codec_opaque;
    if (codec) {
        delete codec->encoder;
        free(codec);
    }
}

extern "C" struct alac_codec_s *alac_create_encoder(int max_frames, int sample_rate,
                                                     int sample_size, int channels) {
    alac_codec_ext_t *codec;

    if ((codec = (alac_codec_ext_t *)malloc(sizeof(alac_codec_ext_t))) == NULL) return NULL;
    memset(codec, 0, sizeof(*codec));

    if ((codec->encoder = new ALACEncoder) == NULL) {
        free(codec);
        return NULL;
    }

    codec->inputFormat.mFormatID = kALACFormatLinearPCM;
    codec->inputFormat.mSampleRate = sample_rate;
    codec->inputFormat.mBitsPerChannel = sample_size;
    codec->inputFormat.mFramesPerPacket = 1;
    codec->inputFormat.mChannelsPerFrame = channels;
    codec->inputFormat.mBytesPerFrame = channels * (sample_size / 8);
    codec->inputFormat.mBytesPerPacket = codec->inputFormat.mBytesPerFrame;
    codec->inputFormat.mFormatFlags = kALACFormatFlagsNativeEndian | kALACFormatFlagIsSignedInteger;
    codec->inputFormat.mReserved = 0;

    codec->outputFormat.mFormatID = kALACFormatAppleLossless;
    codec->outputFormat.mSampleRate = sample_rate;
    codec->outputFormat.mFramesPerPacket = max_frames;
    codec->outputFormat.mChannelsPerFrame = channels;
    codec->outputFormat.mBytesPerPacket = 0;
    codec->outputFormat.mBytesPerFrame = 0;
    codec->outputFormat.mBitsPerChannel = 0;
    codec->outputFormat.mReserved = 0;

    /* Fix: set mFormatFlags based on actual bit depth.
     * Apple's ALAC encoder uses this to determine mBitDepth. */
    switch (sample_size) {
        case 16: codec->outputFormat.mFormatFlags = 1; break;
        case 20: codec->outputFormat.mFormatFlags = 2; break;
        case 24: codec->outputFormat.mFormatFlags = 3; break;
        case 32: codec->outputFormat.mFormatFlags = 4; break;
        default: codec->outputFormat.mFormatFlags = 1; break;
    }

    codec->encoder->SetFrameSize(codec->outputFormat.mFramesPerPacket);
    /* Disable fast mode for >16-bit to avoid potential encoder issues */
    codec->encoder->SetFastMode(sample_size <= 16);
    codec->encoder->InitializeEncoder(codec->outputFormat);

    return (struct alac_codec_s *)codec;
}
