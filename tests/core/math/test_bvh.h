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
#include "core/math/dynamic_bvh.h"

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

TEST_CASE("[BVH] DynamicBVH convex query handles SIMD-sized plane batches") {
	const Plane planes[8] = {
		Plane(Vector3(1, 0, 0), 1),
		Plane(Vector3(-1, 0, 0), 1),
		Plane(Vector3(0, 1, 0), 1),
		Plane(Vector3(0, -1, 0), 1),
		Plane(Vector3(0, 0, 1), 1),
		Plane(Vector3(1, 1, 0).normalized(), Math::SQRT12),
		Plane(Vector3(0, 0, -1), 1),
		Plane(Vector3(-1, 1, 0).normalized(), 2),
	};
	const Vector3 points[8] = {
		Vector3(-1, -1, -1),
		Vector3(-1, -1, 1),
		Vector3(-1, 1, -1),
		Vector3(-1, 1, 1),
		Vector3(1, -1, -1),
		Vector3(1, -1, 1),
		Vector3(1, 1, -1),
		Vector3(1, 1, 1),
	};

	DynamicBVH bvh;
	int items[4] = { 0, 1, 2, 3 };
	bvh.insert(AABB(Vector3(-0.25, -0.25, -0.25), Vector3(0.5, 0.5, 0.5)), &items[0]);
	bvh.insert(AABB(Vector3(0.75, 0.15, -0.25), Vector3(0.25, 0.25, 0.5)), &items[1]);
	bvh.insert(AABB(Vector3(0.85, 0.85, -0.25), Vector3(0.1, 0.1, 0.5)), &items[2]);
	bvh.insert(AABB(Vector3(1.25, -0.25, -0.25), Vector3(0.25, 0.5, 0.5)), &items[3]);

	bool seen_six_planes[4] = {};
	auto collect_six_planes = [&seen_six_planes](void *p_data) {
		const int item = *static_cast<int *>(p_data);
		seen_six_planes[item] = true;
		return false;
	};

	bvh.convex_query(planes, 6, points, 8, collect_six_planes);

	CHECK(seen_six_planes[0]);
	CHECK(seen_six_planes[1]);
	CHECK_FALSE(seen_six_planes[2]);
	CHECK_FALSE(seen_six_planes[3]);

	bool seen_eight_planes[4] = {};
	auto collect_eight_planes = [&seen_eight_planes](void *p_data) {
		const int item = *static_cast<int *>(p_data);
		seen_eight_planes[item] = true;
		return false;
	};

	bvh.convex_query(planes, 8, points, 8, collect_eight_planes);

	CHECK(seen_eight_planes[0]);
	CHECK(seen_eight_planes[1]);
	CHECK_FALSE(seen_eight_planes[2]);
	CHECK_FALSE(seen_eight_planes[3]);
}

} // namespace TestBVH
