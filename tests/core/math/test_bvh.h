/**************************************************************************/
/*  test_bvh.h                                                            */
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

#include "core/math/bvh.h"

#include "tests/test_macros.h"

namespace TestBVH {

struct TestPairFunction {
	static bool user_pair_check(int *p_a, int *p_b) {
		return p_a != p_b;
	}
};

struct TestCullFunction {
	static bool user_cull_check(const int *p_a, int *p_b) {
		return p_a != p_b;
	}
};

TEST_CASE("[BVH] AABB culling returns the same leaf hits across SIMD-sized leaves") {
	BVH_Manager<int, 1, false, 16, TestPairFunction, TestCullFunction> bvh;
	bvh.params_set_thread_safe(false);

	int items[10];
	for (int i = 0; i < 10; i++) {
		items[i] = i;
		bvh.create(&items[i], true, 0, 1, AABB(Vector3(i, 0, 0), Vector3(0.25, 0.25, 0.25)));
	}

	int *results[10] = {};
	const int result_count = bvh.cull_aabb(AABB(Vector3(2, -1, -1), Vector3(4, 2, 2)), results, 10, nullptr);
	CHECK(result_count == 5);

	bool seen[10] = {};
	for (int i = 0; i < result_count; i++) {
		REQUIRE(results[i] != nullptr);
		seen[*results[i]] = true;
	}
	CHECK(seen[2]);
	CHECK(seen[3]);
	CHECK(seen[4]);
	CHECK(seen[5]);
	CHECK(seen[6]);
	CHECK_FALSE(seen[1]);
	CHECK_FALSE(seen[7]);
}

} // namespace TestBVH
