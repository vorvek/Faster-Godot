/**************************************************************************/
/*  test_audio_rb_resampler.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/audio/audio_rb_resampler.h"

#include "tests/test_macros.h"

namespace TestAudioRBResampler {

constexpr int MIX_FRAC_BITS = 13;
constexpr int MIX_FRAC_LEN = 1 << MIX_FRAC_BITS;
constexpr int MIX_FRAC_MASK = MIX_FRAC_LEN - 1;

static AudioFrame get_expected_stereo_frame(const AudioRBResampler &p_resampler, uint32_t p_offset) {
	const uint32_t pos = p_offset >> MIX_FRAC_BITS;
	const uint32_t pos_next = (pos + 1) & p_resampler.rb_mask;
	const float frac = float(p_offset & MIX_FRAC_MASK) / float(MIX_FRAC_LEN);

	const float left = p_resampler.rb[(pos << 1) + 0];
	const float right = p_resampler.rb[(pos << 1) + 1];
	const float next_left = p_resampler.rb[(pos_next << 1) + 0];
	const float next_right = p_resampler.rb[(pos_next << 1) + 1];

	return AudioFrame(
			left + (next_left - left) * frac,
			right + (next_right - right) * frac);
}

TEST_CASE("[AudioRBResampler] Stereo resampling matches linear interpolation") {
	AudioRBResampler resampler;
	CHECK_EQ(resampler.setup(2, 48000, 44100, 0, 64), OK);

	float *write_buffer = resampler.get_write_buffer();
	for (uint32_t i = 0; i < 64; i++) {
		write_buffer[(i << 1) + 0] = 0.25f + float(i) * 1.5f;
		write_buffer[(i << 1) + 1] = -2.0f + float(i) * 0.75f;
	}
	resampler.write(64);

	constexpr int output_frames = 24;
	AudioFrame output[output_frames];
	CHECK(resampler.mix(output, output_frames));

	const uint32_t offset_mask = (uint32_t(1) << (resampler.rb_bits + MIX_FRAC_BITS)) - 1;
	const uint32_t increment = (resampler.src_mix_rate * MIX_FRAC_LEN) / resampler.target_mix_rate;
	uint32_t expected_offset = 0;
	for (int i = 0; i < output_frames; i++) {
		expected_offset = (expected_offset + increment) & offset_mask;
		const AudioFrame expected = get_expected_stereo_frame(resampler, expected_offset);
		CHECK(output[i].left == doctest::Approx(expected.left).epsilon(0.00001));
		CHECK(output[i].right == doctest::Approx(expected.right).epsilon(0.00001));
	}
}

TEST_CASE("[AudioRBResampler] Stereo resampling preserves ring wrap handling") {
	AudioRBResampler resampler;
	CHECK_EQ(resampler.setup(2, 44100, 44100, 0, 8), OK);

	for (uint32_t i = 0; i < resampler.rb_len; i++) {
		resampler.rb[(i << 1) + 0] = float(i * 10 + 1);
		resampler.rb[(i << 1) + 1] = float(i * 10 + 2);
	}

	resampler.offset = int32_t((resampler.rb_len - 3) << MIX_FRAC_BITS);
	resampler.rb_read_pos.set(int(resampler.rb_len - 3));
	resampler.rb_write_pos.set(4);

	constexpr int output_frames = 4;
	AudioFrame output[output_frames];
	CHECK(resampler.mix(output, output_frames));

	const uint32_t offset_mask = (uint32_t(1) << (resampler.rb_bits + MIX_FRAC_BITS)) - 1;
	uint32_t expected_offset = uint32_t((resampler.rb_len - 3) << MIX_FRAC_BITS);
	for (int i = 0; i < output_frames; i++) {
		expected_offset = (expected_offset + MIX_FRAC_LEN) & offset_mask;
		const AudioFrame expected = get_expected_stereo_frame(resampler, expected_offset);
		CHECK_EQ(output[i].left, expected.left);
		CHECK_EQ(output[i].right, expected.right);
	}
}

} // namespace TestAudioRBResampler
