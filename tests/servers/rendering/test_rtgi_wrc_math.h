/**************************************************************************/
/*  test_rtgi_wrc_math.h                                                  */
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

#include "servers/rendering/renderer_rd/effects/rtgi_wrc_math.h"

namespace TestRtgiWrcMath {
TEST_CASE("[RTGI WRC] octahedral encode/decode round-trips unit vectors") {
	Vector3 dirs[] = { Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0.3, 0.7, -0.6).normalized() };
	for (Vector3 d : dirs) {
		Vector2 oct = RtgiWrc::dir_to_oct(d);
		Vector3 r = RtgiWrc::oct_to_dir(oct);
		CHECK(r.distance_to(d) < 1e-5);
		CHECK(oct.x >= 0.0);
		CHECK(oct.x <= 1.0);
		CHECK(oct.y >= 0.0);
		CHECK(oct.y <= 1.0);
	}
}
TEST_CASE("[RTGI WRC] cascade selection picks the tightest cascade containing the point") {
	RtgiWrc::ClipmapParams p;
	p.cascade_count = 3;
	p.base_spacing = 1.0f;
	p.grid = 32;
	p.oct_res = 8;
	CHECK(RtgiWrc::select_cascade(p, Vector3(5, 0, 0), Vector3()) == 0); // |5| < 16 (c0 half-extent)
	CHECK(RtgiWrc::select_cascade(p, Vector3(20, 0, 0), Vector3()) == 1); // 16 < 20 < 32 (c1)
	CHECK(RtgiWrc::select_cascade(p, Vector3(40, 0, 0), Vector3()) == 2); // 32 < 40 < 64 (c2)
}
TEST_CASE("[RTGI WRC] integer recenter delta is the probe-space camera motion") {
	RtgiWrc::ClipmapParams p;
	p.base_spacing = 1.0f;
	p.grid = 32;
	p.cascade_count = 3;
	p.oct_res = 8;
	CHECK(RtgiWrc::recenter_delta(p, 0, Vector3(0, 0, 0), Vector3(3.2, 0, 0)) == Vector3i(3, 0, 0));
	CHECK(RtgiWrc::recenter_delta(p, 1, Vector3(0, 0, 0), Vector3(3.2, 0, 0)) == Vector3i(1, 0, 0)); // c1 spacing=2 -> floor(3.2/2)=1
}
TEST_CASE("[RTGI WRC] atlas coord maps (cascade,probe,dir) into a unique non-overlapping texel") {
	RtgiWrc::ClipmapParams p;
	p.grid = 32;
	p.oct_res = 8;
	p.cascade_count = 3;
	p.base_spacing = 1.0f;
	Point2i a = RtgiWrc::atlas_coord(p, 0, Vector3i(0, 0, 0), 0);
	Point2i b = RtgiWrc::atlas_coord(p, 0, Vector3i(0, 0, 0), 1);
	Point2i c = RtgiWrc::atlas_coord(p, 0, Vector3i(1, 0, 0), 0);
	CHECK(a != b);
	CHECK(a != c);
	CHECK(b != c);
	CHECK(a.x >= 0);
	CHECK(a.y >= 0);
}
}
