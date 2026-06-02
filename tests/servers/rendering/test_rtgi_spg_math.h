/**************************************************************************/
/*  test_rtgi_spg_math.h                                                  */
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
#include "tests/test_macros.h"
#include "servers/rendering/renderer_rd/effects/rtgi_spg_math.h"

namespace TestRtgiSpgMath {
TEST_CASE("[RTGI SPG] hemi-oct encode/decode round-trips upper-hemisphere unit vectors") {
	Vector3 dirs[] = { Vector3(0, 0, 1), Vector3(1, 0, 0).normalized(), Vector3(0, 1, 0).normalized(),
		Vector3(-1, 0, 0.001f).normalized(), Vector3(0.3, -0.7, 0.6).normalized(), Vector3(0.6, 0.6, 0.5).normalized() };
	for (Vector3 d : dirs) {
		Vector2 oct = RtgiSpg::hemioct_encode(d);
		Vector3 r = RtgiSpg::hemioct_decode(oct);
		CHECK(r.distance_to(d) < 1e-5);
		CHECK(oct.x >= 0.0); CHECK(oct.x <= 1.0);
		CHECK(oct.y >= 0.0); CHECK(oct.y <= 1.0);
	}
}
TEST_CASE("[RTGI SPG] build_basis returns an orthonormal frame with n as +Z") {
	Vector3 normals[] = { Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0.3, 0.7, -0.6).normalized() };
	for (Vector3 n : normals) {
		Vector3 t, b;
		RtgiSpg::build_basis(n, t, b);
		CHECK(Math::abs(t.dot(b)) < 1e-5);
		CHECK(Math::abs(t.dot(n)) < 1e-5);
		CHECK(Math::abs(b.dot(n)) < 1e-5);
		CHECK(Math::abs(t.length() - 1.0f) < 1e-5);
		Vector3 w = RtgiSpg::local_to_world(Vector3(0, 0, 1), t, b, n);
		CHECK(w.distance_to(n) < 1e-5);
	}
}
TEST_CASE("[RTGI SPG] atlas tile origins are non-overlapping per probe") {
	Point2i a = RtgiSpg::atlas_tile_origin(0, 0, 8);
	Point2i b = RtgiSpg::atlas_tile_origin(1, 0, 8);
	Point2i c = RtgiSpg::atlas_tile_origin(0, 1, 8);
	CHECK(a == Point2i(0, 0)); CHECK(b == Point2i(8, 0)); CHECK(c == Point2i(0, 8));
}
}
