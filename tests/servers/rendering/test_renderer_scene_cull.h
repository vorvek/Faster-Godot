/**************************************************************************/
/*  test_renderer_scene_cull.h                                            */
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

#include "servers/rendering/renderer_scene_cull.h"

#include "tests/test_macros.h"

namespace TestRendererSceneCull {

template <uint32_t N>
RendererSceneCull::Frustum make_frustum(const Plane (&p_planes)[N]) {
	Vector<Plane> planes;
	planes.resize(N);
	for (uint32_t i = 0; i < N; i++) {
		planes.write[i] = p_planes[i];
	}
	return RendererSceneCull::Frustum(planes);
}

void check_frustum_matches_scalar(const RendererSceneCull::Frustum &p_frustum, const AABB &p_aabb) {
	const RendererSceneCull::InstanceBounds bounds(p_aabb);
	CHECK_MESSAGE(
			bounds.in_frustum(p_frustum) == bounds.in_frustum_scalar(p_frustum),
			"InstanceBounds SIMD frustum result should match the scalar oracle.");
}

TEST_CASE("[RendererSceneCull] InstanceBounds six-plane frustum path matches scalar oracle") {
	const Plane planes[6] = {
		Plane(Vector3(1.0f, 0.5f, 0.0f), 2.0f),
		Plane(Vector3(-0.5f, 1.0f, 0.25f), 2.0f),
		Plane(Vector3(0.0f, -1.0f, 0.5f), 1.5f),
		Plane(Vector3(0.25f, 0.0f, -1.0f), 1.5f),
		Plane(Vector3(-1.0f, -0.25f, 0.0f), 2.5f),
		Plane(Vector3(0.0f, 0.25f, 1.0f), 2.5f),
	};
	const RendererSceneCull::Frustum frustum = make_frustum(planes);

	const AABB aabbs[] = {
		AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f)),
		AABB(Vector3(0.5f, -0.25f, -0.25f), Vector3(0.75f, 0.5f, 0.5f)),
		AABB(Vector3(-2.0f, 0.25f, -0.75f), Vector3(0.5f, 0.5f, 0.5f)),
		AABB(Vector3(0.25f, 1.25f, 0.25f), Vector3(0.5f, 0.5f, 0.5f)),
		AABB(Vector3(-0.25f, -0.25f, 2.25f), Vector3(0.5f, 0.5f, 0.5f)),
	};

	for (const AABB &aabb : aabbs) {
		check_frustum_matches_scalar(frustum, aabb);
	}
}

TEST_CASE("[RendererSceneCull] InstanceBounds frustum boundary remains an outside rejection") {
	const Plane planes[6] = {
		Plane(Vector3(1.0f, 0.0f, 0.0f), 1.0f),
		Plane(Vector3(-1.0f, 0.0f, 0.0f), 1.0f),
		Plane(Vector3(0.0f, 1.0f, 0.0f), 1.0f),
		Plane(Vector3(0.0f, -1.0f, 0.0f), 1.0f),
		Plane(Vector3(0.0f, 0.0f, 1.0f), 1.0f),
		Plane(Vector3(0.0f, 0.0f, -1.0f), 1.0f),
	};
	const RendererSceneCull::Frustum frustum = make_frustum(planes);

	const RendererSceneCull::InstanceBounds inside_bounds(AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f)));
	const RendererSceneCull::InstanceBounds boundary_bounds(AABB(Vector3(1.0f, -0.5f, -0.5f), Vector3(0.25f, 1.0f, 1.0f)));

	CHECK(inside_bounds.in_frustum(frustum));
	CHECK_FALSE(boundary_bounds.in_frustum(frustum));
	CHECK_FALSE(boundary_bounds.in_frustum_scalar(frustum));
}

TEST_CASE("[RendererSceneCull] InstanceBounds non-six-plane frustum uses scalar-compatible fallback") {
	const Plane planes[7] = {
		Plane(Vector3(1.0f, 0.0f, 0.0f), 2.0f),
		Plane(Vector3(-1.0f, 0.0f, 0.0f), 2.0f),
		Plane(Vector3(0.0f, 1.0f, 0.0f), 2.0f),
		Plane(Vector3(0.0f, -1.0f, 0.0f), 2.0f),
		Plane(Vector3(0.0f, 0.0f, 1.0f), 2.0f),
		Plane(Vector3(0.0f, 0.0f, -1.0f), 2.0f),
		Plane(Vector3(1.0f, 1.0f, 0.0f), 2.5f),
	};
	const RendererSceneCull::Frustum frustum = make_frustum(planes);

	check_frustum_matches_scalar(frustum, AABB(Vector3(-0.25f, -0.25f, -0.25f), Vector3(0.5f, 0.5f, 0.5f)));
	check_frustum_matches_scalar(frustum, AABB(Vector3(1.5f, 1.5f, 0.0f), Vector3(0.5f, 0.5f, 0.5f)));
}

} // namespace TestRendererSceneCull
