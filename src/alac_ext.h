/*
 * Extended ALAC encoder wrapper that properly handles 24-bit audio.
 *
 * The libraop alac_wrapper hardcodes mFormatFlags=1 (16-bit) regardless
 * of the sample_size parameter. This extension creates the encoder with
 * the correct format flags for 24-bit (flags=3).
 *
 * Copyright (C) 2024-2026 Music Assistant Contributors
 * See LICENSE
 */

#ifndef __ALAC_EXT_H_
#define __ALAC_EXT_H_

#include <stdint.h>
#include <stdbool.h>

struct alac_codec_s;

/*
 * Create ALAC encoder with correct bit depth support.
 * Unlike alac_create_encoder, this sets mFormatFlags correctly for 24-bit.
 */
struct alac_codec_s *alac_create_encoder_ext(int max_frames, int sample_rate,
                                              int sample_size, int channels);

#endif
