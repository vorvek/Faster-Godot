/**************************************************************************/
/*  rtgi_spg_math.h                                                       */
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

// Screen-Probe Gather (SPG) pure-math contract. Probes are placed one per F x F
// screen tile; each stores an O x O HEMISPHERE-octahedral incident-radiance map
// oriented to the probe's anchor normal (tangent frame). This differs from the
// WRC's world-up full-sphere oct (rtgi_wrc_math.h): SPG folds only the upper
// hemisphere (n.z >= 0 in the local frame) so no texels are wasted.
namespace RtgiSpg {

// Branchless orthonormal basis from a unit normal (Duff et al. 2017,
// "Building an Orthonormal Basis, Revisited"). n maps to the local +Z axis.
static _ALWAYS_INLINE_ void build_basis(Vector3 p_n, Vector3 &r_t, Vector3 &r_b) {
	const float s = (p_n.z >= 0.0f) ? 1.0f : -1.0f;
	const float a = -1.0f / (s + p_n.z);
	const float bb = p_n.x * p_n.y * a;
	r_t = Vector3(1.0f + s * p_n.x * p_n.x * a, s * bb, -s * p_n.x);
	r_b = Vector3(bb, s + p_n.y * p_n.y * a, -p_n.y);
}

// Hemi-octahedral encode (Cigolle et al. 2014). Input v in the LOCAL frame with
// v.z >= 0. Returns oct coords in [0,1]^2.
static _ALWAYS_INLINE_ Vector2 hemioct_encode(Vector3 p_v) {
	const float denom = Math::abs(p_v.x) + Math::abs(p_v.y) + MAX(p_v.z, 0.0f);
	const float inv = (denom > 0.0f) ? (1.0f / denom) : 0.0f;
	const float px = p_v.x * inv;
	const float py = p_v.y * inv;
	// Rotate+scale the center diamond to the unit square, then remap [-1,1]->[0,1].
	return Vector2((px + py) * 0.5f + 0.5f, (px - py) * 0.5f + 0.5f);
}

// Inverse of hemioct_encode. Returns a unit vector with z >= 0 in the local frame.
static _ALWAYS_INLINE_ Vector3 hemioct_decode(Vector2 p_oct) {
	const float ex = p_oct.x * 2.0f - 1.0f;
	const float ey = p_oct.y * 2.0f - 1.0f;
	const Vector2 t = Vector2((ex + ey), (ex - ey)) * 0.5f;
	Vector3 v(t.x, t.y, 1.0f - Math::abs(t.x) - Math::abs(t.y));
	return v.normalized();
}

// Local-hemisphere dir -> world dir, given the anchor normal's basis.
static _ALWAYS_INLINE_ Vector3 local_to_world(Vector3 p_local, Vector3 p_t, Vector3 p_b, Vector3 p_n) {
	return (p_t * p_local.x + p_b * p_local.y + p_n * p_local.z).normalized();
}

// World dir -> local-hemisphere coords, given the anchor normal's orthonormal
// basis (inverse of local_to_world; the basis is orthonormal so transpose = inverse).
static _ALWAYS_INLINE_ Vector3 world_to_local(Vector3 p_w, Vector3 p_t, Vector3 p_b, Vector3 p_n) {
	return Vector3(p_w.dot(p_t), p_w.dot(p_b), p_w.dot(p_n));
}

// Bottom-left atlas texel of probe (gx, gy)'s O x O tile (probes laid out as a
// 2D screen grid; row-major). The in-tile texel offset is added by the caller.
static _ALWAYS_INLINE_ Point2i atlas_tile_origin(int p_gx, int p_gy, int p_oct_res) {
	return Point2i(p_gx * p_oct_res, p_gy * p_oct_res);
}

// Octahedral coord of the CENTER of texel p_dir_index within an O x O tile, [0,1]^2.
static _ALWAYS_INLINE_ Vector2 oct_texel_center(int p_dir_index, int p_oct_res) {
	const int tx = p_dir_index % p_oct_res;
	const int ty = p_dir_index / p_oct_res;
	const float inv = 1.0f / float(p_oct_res);
	return Vector2((float(tx) + 0.5f) * inv, (float(ty) + 0.5f) * inv);
}

} // namespace RtgiSpg
