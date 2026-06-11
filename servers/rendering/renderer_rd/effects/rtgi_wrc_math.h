/**************************************************************************/
/*  rtgi_wrc_math.h                                                       */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"

// World Radiance Cache (WRC) pure-math contract.
//
// The WRC is a camera-centered cascaded clipmap of octahedral-radiance probes.
// This header is header-only and depends only on Godot core math types so it
// can be reasoned about on the CPU (cascade/scroll selection by the RD effect)
// and mirrored faithfully in GLSL (rtgi_wrc_inc.glsl). Every routine here is intentionally
// branch-light and free of STL so the GLSL port is a near-literal translation.
//
// Coordinate conventions:
//  - A cascade `k` has a uniform probe spacing of `base_spacing * 2^k`.
//  - Each cascade is a cubic box of `grid x grid x grid` probes centered on the
//    camera, giving a half-extent of `base_spacing * 2^k * grid * 0.5`.
//  - Each probe stores an `oct_res x oct_res` octahedral map of radiance; a
//    direction index `0..oct_res*oct_res-1` addresses one texel of that map.

namespace RtgiWrc {

struct ClipmapParams {
	int cascade_count = 4;
	int grid = 32;
	int oct_res = 8;
	float base_spacing = 1.0f;
};

// Branch-light copysign used by the octahedral fold. Mirrors the reference
// "signNotZero" from Cigolle et al.: the result is +1 for non-negative inputs
// and -1 for negative inputs (note: 0 maps to +1, matching the survey code).
// This is deliberately *not* GLSL `sign()` (which returns 0 at 0) so the fold
// stays a bijection on the octahedron seams; the GLSL port should use the same
// `signNotZero` helper rather than the built-in `sign`.
static _ALWAYS_INLINE_ float wrc_sign_not_zero(float p_v) {
	return (p_v >= 0.0f) ? 1.0f : -1.0f;
}

// Encode a unit direction into octahedral [0,1]^2.
// Standard octahedral mapping (Cigolle et al., "A Survey of Efficient
// Representations for Independent Unit Vectors"): project onto the L1-ball
// octahedron, fold the lower hemisphere across the diagonals, then bias/scale
// the resulting [-1,1]^2 coordinate into [0,1]^2.
static _ALWAYS_INLINE_ Vector2 dir_to_oct(Vector3 p_dir) {
	const float l1 = Math::abs(p_dir.x) + Math::abs(p_dir.y) + Math::abs(p_dir.z);
	// Guard against a degenerate zero vector; any finite scale is fine here.
	const float inv_l1 = (l1 > 0.0f) ? (1.0f / l1) : 0.0f;
	float ox = p_dir.x * inv_l1;
	float oy = p_dir.z * inv_l1;
	if (p_dir.y < 0.0f) {
		const float fx = (1.0f - Math::abs(oy)) * wrc_sign_not_zero(ox);
		const float fy = (1.0f - Math::abs(ox)) * wrc_sign_not_zero(oy);
		ox = fx;
		oy = fy;
	}
	// Remap [-1,1] -> [0,1].
	return Vector2(ox * 0.5f + 0.5f, oy * 0.5f + 0.5f);
}

// Decode an octahedral [0,1]^2 coordinate back into a unit direction.
// Inverse of dir_to_oct(); round-trips to < 1e-5 for unit inputs.
static _ALWAYS_INLINE_ Vector3 oct_to_dir(Vector2 p_oct) {
	// Remap [0,1] -> [-1,1].
	const float ex = p_oct.x * 2.0f - 1.0f;
	const float ey = p_oct.y * 2.0f - 1.0f;
	float vx = ex;
	float vy = 1.0f - Math::abs(ex) - Math::abs(ey);
	float vz = ey;
	// Unfold the lower hemisphere (vy < 0).
	if (vy < 0.0f) {
		const float fx = (1.0f - Math::abs(ey)) * wrc_sign_not_zero(ex);
		const float fz = (1.0f - Math::abs(ex)) * wrc_sign_not_zero(ey);
		vx = fx;
		vz = fz;
	}
	// Note axis assignment matches dir_to_oct: x<->x, y<->up, z<->y.
	return Vector3(vx, vy, vz).normalized();
}

// Half-extent (in world units) of cascade `k`'s cubic clipmap box.
static _ALWAYS_INLINE_ float cascade_half_extent(const ClipmapParams &p_params, int p_cascade) {
	const float spacing = p_params.base_spacing * float(int64_t(1) << p_cascade);
	return spacing * float(p_params.grid) * 0.5f;
}

// Select the smallest cascade whose cubic box contains `p_world` relative to
// `p_camera`, measured with the max-axis (Chebyshev) distance. Clamps to the
// last cascade when the point lies beyond every box.
static _ALWAYS_INLINE_ int select_cascade(const ClipmapParams &p_params, Vector3 p_world, Vector3 p_camera) {
	const Vector3 d = (p_world - p_camera).abs();
	const float chebyshev = MAX(d.x, MAX(d.y, d.z));
	const int last = MAX(p_params.cascade_count - 1, 0);
	for (int k = 0; k < p_params.cascade_count; k++) {
		if (chebyshev <= cascade_half_extent(p_params, k)) {
			return k;
		}
	}
	return last;
}

// Integer probe-space scroll for `p_cascade` between two camera positions.
// Returns floor(cur/spacing) - floor(prev/spacing) per axis, matching the
// existing STRC recenter semantics (the clipmap only ever shifts by whole
// probes, keeping cached probes texel-aligned across frames).
static _ALWAYS_INLINE_ Vector3i recenter_delta(const ClipmapParams &p_params, int p_cascade, Vector3 p_prev_camera, Vector3 p_cur_camera) {
	const float spacing = p_params.base_spacing * float(int64_t(1) << p_cascade);
	const float inv_spacing = (spacing != 0.0f) ? (1.0f / spacing) : 0.0f;
	Vector3i out;
	for (int axis = 0; axis < 3; axis++) {
		const int cur = int(Math::floor(p_cur_camera[axis] * inv_spacing));
		const int prev = int(Math::floor(p_prev_camera[axis] * inv_spacing));
		out[axis] = cur - prev;
	}
	return out;
}

// Number of probe tiles laid out along one axis of the atlas. The atlas packs
// `cascade_count * grid^3` square tiles into a roughly-square grid of tiles, so
// the per-axis tile count is ceil(sqrt(total_tiles)).
static _ALWAYS_INLINE_ int atlas_tiles_per_row(const ClipmapParams &p_params) {
	const int total_tiles = p_params.cascade_count * p_params.grid * p_params.grid * p_params.grid;
	int n = int(Math::ceil(Math::sqrt(double(MAX(total_tiles, 1)))));
	return MAX(n, 1);
}

// Map (cascade, 3D probe coord, direction index) to a unique atlas texel.
//
// Layout: each probe owns one `oct_res x oct_res` tile. Probes are linearized
//   linear = ((cascade * grid + z) * grid + y) * grid + x
// and tiles are placed row-major in a `tiles_per_row` grid of tiles. The
// direction index addresses the texel inside the tile as
//   (dir_index % oct_res, dir_index / oct_res).
// Distinct (cascade, probe) pairs land in distinct tiles (no overlap); distinct
// dir indices within a tile land in distinct texels.
static _ALWAYS_INLINE_ Point2i atlas_coord(const ClipmapParams &p_params, int p_cascade, Vector3i p_probe, int p_dir_index) {
	const int grid = p_params.grid;
	const int oct_res = p_params.oct_res;
	const int64_t linear = (((int64_t(p_cascade) * grid + p_probe.z) * grid + p_probe.y) * grid + p_probe.x);
	const int tiles_per_row = atlas_tiles_per_row(p_params);
	const int tile_col = int(linear % tiles_per_row);
	const int tile_row = int(linear / tiles_per_row);
	const int texel_x = p_dir_index % oct_res;
	const int texel_y = p_dir_index / oct_res;
	return Point2i(tile_col * oct_res + texel_x, tile_row * oct_res + texel_y);
}

// Octahedral coordinate of the *center* of texel `p_dir_index` within a tile,
// expressed in [0,1]^2 (half-texel offset so we sample texel centers, not
// corners). Shared by cosine_weight() and the future GLSL irradiance gather.
static _ALWAYS_INLINE_ Vector2 oct_texel_center(const ClipmapParams &p_params, int p_dir_index) {
	const int oct_res = p_params.oct_res;
	const int tx = p_dir_index % oct_res;
	const int ty = p_dir_index / oct_res;
	const float inv_res = 1.0f / float(oct_res);
	return Vector2((float(tx) + 0.5f) * inv_res, (float(ty) + 0.5f) * inv_res);
}

// Cosine (clamped Lambert) weight for direction `p_dir_index` against a surface
// normal. Used later for cosine-weighted irradiance integration over a probe's
// octahedral map. Provided here so the C++ and GLSL paths share one definition.
static _ALWAYS_INLINE_ float cosine_weight(const ClipmapParams &p_params, int p_dir_index, Vector3 p_normal) {
	const Vector3 dir = oct_to_dir(oct_texel_center(p_params, p_dir_index));
	return MAX(0.0f, float(p_normal.dot(dir)));
}

} // namespace RtgiWrc
