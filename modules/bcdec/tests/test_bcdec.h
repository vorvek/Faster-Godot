/**************************************************************************/
/*  test_bcdec.h                                                          */
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

#include "../image_decompress_bcdec.h"

#include "tests/test_macros.h"

#include "thirdparty/misc/bcdec.h"

namespace TestBCDec {

static void write_u16(uint8_t *p_dst, uint16_t p_value) {
	memcpy(p_dst, &p_value, sizeof(p_value));
}

static void write_u32(uint8_t *p_dst, uint32_t p_value) {
	memcpy(p_dst, &p_value, sizeof(p_value));
}

static Vector<uint8_t> make_bc1_blocks(int p_width, int p_height) {
	const int block_count_x = (p_width + 3) / 4;
	const int block_count_y = (p_height + 3) / 4;

	Vector<uint8_t> data;
	data.resize(block_count_x * block_count_y * BCDEC_BC1_BLOCK_SIZE);

	uint8_t *data_w = data.ptrw();
	for (int block = 0; block < block_count_x * block_count_y; block++) {
		uint8_t *dst_block = data_w + block * BCDEC_BC1_BLOCK_SIZE;
		const bool opaque_block = (block % 2) == 0;
		write_u16(dst_block, opaque_block ? 0xF800 : 0x001F);
		write_u16(dst_block + 2, opaque_block ? 0x07E0 : 0xFFFF);
		write_u32(dst_block + 4, 0xE4E4E4E4u ^ (uint32_t(block) * 0x1B1B1B1Bu));
	}

	return data;
}

static Vector<uint8_t> decode_bc1_scalar(int p_width, int p_height, const Vector<uint8_t> &p_data) {
	const int block_count_x = (p_width + 3) / 4;
	const int block_count_y = (p_height + 3) / 4;

	Vector<uint8_t> decoded;
	decoded.resize(p_width * p_height * 4);
	uint8_t *decoded_w = decoded.ptrw();
	const uint8_t *data_r = p_data.ptr();

	uint8_t block_pixels[4 * 4 * 4];
	for (int block_y = 0; block_y < block_count_y; block_y++) {
		for (int block_x = 0; block_x < block_count_x; block_x++) {
			const int block_index = block_y * block_count_x + block_x;
			bcdec_bc1(data_r + block_index * BCDEC_BC1_BLOCK_SIZE, block_pixels, 4 * 4);

			const int copy_width = (p_width - block_x * 4) < 4 ? (p_width - block_x * 4) : 4;
			const int copy_height = (p_height - block_y * 4) < 4 ? (p_height - block_y * 4) : 4;
			for (int y = 0; y < copy_height; y++) {
				memcpy(decoded_w + ((block_y * 4 + y) * p_width + block_x * 4) * 4, block_pixels + y * 4 * 4, copy_width * 4);
			}
		}
	}

	return decoded;
}

static void check_bc1_decompression_matches_scalar(int p_width, int p_height) {
	Vector<uint8_t> compressed = make_bc1_blocks(p_width, p_height);
	Vector<uint8_t> expected = decode_bc1_scalar(p_width, p_height, compressed);

	Ref<Image> image = memnew(Image(p_width, p_height, false, Image::FORMAT_DXT1, compressed));
	image_decompress_bcdec(image.ptr());

	CHECK(image->get_format() == Image::FORMAT_RGBA8);
	CHECK_MESSAGE(
			image->get_data() == expected,
			"BC1 decompression should match the scalar bcdec block decoder exactly.");
}

TEST_CASE("[BCDec] BC1 AVX2 batch decompression matches scalar decoder") {
	check_bc1_decompression_matches_scalar(16, 8);
	check_bc1_decompression_matches_scalar(20, 8);
	check_bc1_decompression_matches_scalar(18, 7);
}

} // namespace TestBCDec
