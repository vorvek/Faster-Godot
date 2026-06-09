#[compute]

#version 450

#VERSION_DEFINES

// Screen Probe Gather (SPG) temporal-accumulate + spatial-filter compute shader
// (A2-T3 + A2-T4).
//
// Folds this frame's gather ray results (the RTGISPGRayResult SSBO produced by the
// T2 raygen) into the per-probe octahedral radiance atlas with a sample-counted 1/n
// temporal blend, motion-reprojected from the previous frame, then same-surface 3x3
// spatial-filters that atlas into a separate output. Three modes selected by the
// `mode` push-constant drive THREE dispatches recorded back-to-back (barriers
// between) by RTGIScreenProbeGather::run_accumulate():
//
//   mode 1 -- REPROJECT: one thread per radiance-atlas texel. Carry the previous
//             frame's radiance for this texel forward into the current atlas, using
//             the anchor's screen motion to find the previous probe cell, a
//             plane-match (depth + world-pos + normal) to reject disocclusions, and
//             a RE-ORIENT-ON-READ that re-expresses THIS texel's world direction in
//             the previous probe's tangent frame (the two probes generally have
//             different normals, so the same atlas texel maps to a different world
//             direction in each). A miss / disocclusion resets the texel to count 0.
//
//   mode 2 -- BLEND: one thread per gather ray. Each ray carries (probe_linear,
//             dir_index) -> exactly one atlas texel; blend its incident radiance into
//             the reprojected history with weight 1/n (n = sample count, recovered
//             from the confidence channel .a as conf * n_cap). A never-written /
//             just-reset texel (conf 0) takes the new sample outright (w == 1).
//
//   mode 3 -- SPATIAL: one thread per radiance-atlas texel. Smooth this texel against
//             the SAME texel-direction sampled from the 3x3 neighbor probes, but only
//             across SAME-SURFACE neighbors (the same plane-match REPROJECT uses) so
//             radiance never leaks across a silhouette, with the same RE-ORIENT-ON-READ
//             into each neighbor's frame. Writes a SEPARATE atlas (radiance_filtered)
//             so the unfiltered atlas stays intact as next frame's history; radius 0 ->
//             a straight passthrough copy.
//
// PING-PONG: the frame swap happens in run_placement (read_index flips there). This
// pass reads radiance_prev/header_*_prev = the (1 - read_index) set, writes
// radiance_cur = the read_index set (REPROJECT + BLEND), and writes radiance_filtered
// (SPATIAL); it performs NO swap. radiance_filtered is what get_radiance_filtered()
// returns to A3 + the debug blit.
//
// This is the per-probe-local-hemioct analogue of the WRC update shader's mode-0
// (full-atlas carry) + mode-1 (per-ray 1/n accumulate) pair; the structure (full
// dispatch over texels, then a 1D dispatch over rays with the y!=0 guard, barrier
// between) is mirrored from rtgi_world_radiance_cache.glsl.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Shared SPG hemi-oct basis math (local +Z = anchor normal): spg_build_basis,
// spg_hemioct_encode/decode, spg_local_to_world, spg_world_to_local.
#include "../raytracing/rtgi_spg_inc.glsl"
// Full-sphere octahedral decode (oct_to_vec3) for the probe header normal.
#include "../oct_inc.glsl"
// WRC query (rtgi_wrc_sample_radiance) for the cold-start seed. All symbols are
// wrc_-prefixed -- no clash with rtgi_spg_inc.glsl / oct_inc.glsl above.
#include "../raytracing/rtgi_wrc_inc.glsl"

// Mode selectors. PLACE (0) lives in rtgi_screen_probe_gather.glsl; this shader runs
// REPROJECT (1) + BLEND (2) + SPATIAL (3). The numeric values match
// RTGIScreenProbeGather's SPG_MODE_* defines in the cpp.
#define SPG_MODE_REPROJECT 1u
#define SPG_MODE_BLEND 2u
#define SPG_MODE_SPATIAL 3u

layout(push_constant, std430) uniform Params {
	uint mode; // 1 = REPROJECT, 2 = BLEND, 3 = SPATIAL.
	uint grid_w;
	uint grid_h;
	uint oct_res;
	uint spacing_f;
	uint frame_index;
	uint atlas_width;
	uint atlas_height;
	uint rays_this_frame;
	float temporal_n_cap;
	uint spatial_radius; // SPATIAL (A2-T4) 3x3 neighbor reach; 0 -> straight copy. (Was pad0.)
	uint pad1; // std430 rounds the push-constant block to a multiple of 16 (40 -> 48 B);
	// WRC cold-start seed: WrcParams scalars + the effective seed sample count. Block
	// grows 48 B -> 80 B (still a multiple of 16). Mirrors AccumPushConstant in
	// rtgi_screen_probe_gather.h EXACTLY (a size mismatch is a black screen).
	uint wrc_cascade_count;
	uint wrc_grid;
	uint wrc_oct_res;
	float wrc_base_spacing;
	float wrc_cam_x;
	float wrc_cam_y;
	float wrc_cam_z;
	float seed_samples;
}
params;

// Radiance ping-pong (RGBA16F: .rgb = radiance, .a = sample count n / n_cap).
// radiance_prev is the previous frame's atlas (read-only); radiance_cur is this
// frame's atlas (written by REPROJECT, then read-modified-written by BLEND).
layout(set = 0, binding = 0, rgba16f) uniform restrict readonly image2D radiance_prev;
layout(set = 0, binding = 1, rgba16f) uniform restrict image2D radiance_cur;
// Current-frame headers (this frame's placement). plane: .xyz world-pos, .w
// linear-depth (<= 0 invalid). aux: .xy oct-normal (vec3_to_oct), .zw screen-motion px.
layout(set = 0, binding = 2, rgba32f) uniform restrict readonly image2D header_plane_cur;
layout(set = 0, binding = 3, rgba16f) uniform restrict readonly image2D header_aux_cur;
// Previous-frame headers (last frame's placement), used to plane-match the
// reprojected probe cell and to re-orient this texel's world dir into its frame.
layout(set = 0, binding = 4, rgba32f) uniform restrict readonly image2D header_plane_prev;
layout(set = 0, binding = 5, rgba16f) uniform restrict readonly image2D header_aux_prev;

// Per-frame gather ray results produced by the T2 SPG raygen. One entry per ray;
// matches RTGISPGRayResult in raytracing_common_inc.glsl (2 x vec4 = 32 B) and
// RTGIScreenProbeGather::ensure_ray_result_buffer.
//   radiance_distance : .rgb = incident radiance, .a = hit distance (-1 = WRC-sourced).
//   probe_dir         : .x = probe_linear, .y = dir_index, .zw = pad.
struct RTGISPGRayResult {
	vec4 radiance_distance;
	vec4 probe_dir;
};

layout(set = 0, binding = 6, std430) readonly buffer SPGRayResults {
	RTGISPGRayResult results[];
};

// SPATIAL output (A2-T4): the same-surface 3x3-filtered radiance atlas. Written only by
// the SPATIAL mode (REPROJECT/BLEND leave it untouched); consumed by A3 + the debug
// integrate via get_radiance_filtered(). Same RGBA16F layout as radiance_cur.
layout(set = 0, binding = 7, rgba16f) uniform restrict writeonly image2D radiance_filtered;

// WRC atlases for the cold-start seed (REPROJECT reset path only). sampler2D
// (bilinear, clamp), exactly like the SPG gather's WRC taps. When the WRC is
// unavailable the C++ binds a default-black texture and sets seed_samples = 0,
// so these reads are inert.
layout(set = 0, binding = 8) uniform sampler2D wrc_radiance_atlas;
layout(set = 0, binding = 9) uniform sampler2D wrc_distance_atlas;

// Clamp a colour to finite, non-negative values so a NaN/Inf ray result cannot
// poison the accumulated radiance (RGBA16F max is 65504). Mirrors wrc_sanitize_color.
vec3 spg_sanitize_color(vec3 c) {
	c = mix(c, vec3(0.0), isnan(c));
	c = mix(c, vec3(65504.0), isinf(c));
	return clamp(c, vec3(0.0), vec3(65504.0));
}

// REPROJECT: one thread per radiance-atlas texel. Carry the matching previous-frame
// texel's radiance forward (re-oriented into this probe's frame), or reset on a
// disocclusion / no-history / out-of-hemisphere miss.
void spg_reproject_main(ivec2 texel) {
	uint oct_res = max(params.oct_res, 1u);
	uint spacing = max(params.spacing_f, 1u);
	int grid_w = int(params.grid_w);
	int grid_h = int(params.grid_h);
	// texel -> (probe, in-tile dir). Bounds-guard against grid_w/grid_h.
	ivec2 probe = texel / int(oct_res);
	ivec2 in_tile = texel - probe * int(oct_res);
	if (probe.x >= grid_w || probe.y >= grid_h) {
		return;
	}
	vec4 cur_plane = imageLoad(header_plane_cur, probe);
	if (cur_plane.w <= 0.0) {
		imageStore(radiance_cur, texel, vec4(0.0)); // invalid probe.
		return;
	}
	vec4 cur_aux = imageLoad(header_aux_cur, probe);
	vec3 cur_n = oct_to_vec3(cur_aux.xy * 2.0 - 1.0);
	// This texel's world direction (CUR probe local hemioct -> world).
	vec2 local_oct = (vec2(in_tile) + 0.5) / float(oct_res);
	vec3 local_dir = spg_hemioct_decode(local_oct);
	vec3 ct, cb;
	spg_build_basis(cur_n, ct, cb);
	vec3 world_dir = spg_local_to_world(local_dir, ct, cb, cur_n);
	// Reproject this probe to its previous-frame screen cell via the anchor motion.
	// Convention: motion_px (header_aux.zw) is the anchor velocity in screen PIXELS, prev - cur
	// (the velocity buffer stores prev_uv - cur_uv; PLACE scaled UV -> pixels), so
	// prev_uv = cur_uv + motion and the previous cell is probe + motion_px / spacing. On the
	// static furnace motion == 0 (identity carry). (The previous `probe - ...` reading was the
	// wrong sign AND fed raw UV, so it rounded to 0 and never reprojected -> indirect smear.)
	vec2 motion_px = cur_aux.zw;
	ivec2 prev_probe = probe + ivec2(round(motion_px / float(spacing)));
	vec4 carried = vec4(0.0); // default: disocclusion / no history -> reset (count 0).
	bool from_history = false; // set true only when a valid reprojected texel is read.
	if (all(greaterThanEqual(prev_probe, ivec2(0))) && prev_probe.x < grid_w && prev_probe.y < grid_h) {
		vec4 prev_plane = imageLoad(header_plane_prev, prev_probe);
		if (prev_plane.w > 0.0) {
			vec4 prev_aux = imageLoad(header_aux_prev, prev_probe);
			vec3 prev_n = oct_to_vec3(prev_aux.xy * 2.0 - 1.0);
			// Plane-match: same surface (linear-depth rel-tol + world-pos dist + normal dot).
			bool match = abs(prev_plane.w - cur_plane.w) <= 0.05 * cur_plane.w &&
					distance(prev_plane.xyz, cur_plane.xyz) <= 2.0 * cur_plane.w * 0.01 + 0.25 &&
					dot(prev_n, cur_n) >= 0.906; // ~25 deg.
			if (match) {
				// RE-ORIENT ON READ: this texel's world_dir expressed in the PREV probe's
				// frame -> prev hemioct texel. Only valid if it stays in the prev hemisphere.
				vec3 pt, pb;
				spg_build_basis(prev_n, pt, pb);
				vec3 prev_local = spg_world_to_local(world_dir, pt, pb, prev_n);
				if (prev_local.z > 0.0) {
					vec2 prev_oct = spg_hemioct_encode(prev_local);
					ivec2 prev_texel = prev_probe * int(oct_res) + clamp(ivec2(prev_oct * float(oct_res)), ivec2(0), ivec2(int(oct_res) - 1));
					carried = imageLoad(radiance_prev, prev_texel); // .rgb radiance, .a = n/n_cap.
					from_history = true;
				}
			}
		}
	}
	// Cold-start seed: when reprojection produced no history (off-grid, plane-match fail,
	// or the re-oriented direction left the previous hemisphere), seed this texel from the
	// WRC instead of leaving it at count 0. The WRC is a coarse, persistent spatial+angular
	// average, so the probe is born smooth and sharpens into its own traced detail over
	// ~seed_samples frames via the BLEND 1/n weight, rather than showing a single noisy ray
	// (the camera-cut blotch). Mirrors Lumen's "fall back to the World Space Radiance Cache
	// when reprojection fails". Only runs on the reset path, so successful reprojections
	// cost nothing.
	if (!from_history && params.seed_samples > 0.0 && params.wrc_oct_res > 0u) {
		WrcParams wp;
		wp.cascade_count = int(max(params.wrc_cascade_count, 1u));
		wp.grid = int(max(params.wrc_grid, 1u));
		wp.oct_res = int(max(params.wrc_oct_res, 1u));
		wp.base_spacing = max(params.wrc_base_spacing, 0.25);
		wp.camera_pos = vec3(params.wrc_cam_x, params.wrc_cam_y, params.wrc_cam_z);
		wp.occlusion_bias_spacing = 0.5;
		wp.min_variance = 0.0001;
		// SPG per-direction cone (oct_res = the local SPG oct_res from line ~129), the same
		// cone the gather uses; intentionally NOT the WRC oct_res.
		float cone = 3.14159265 / float(oct_res);
		float wrc_conf = 0.0;
		vec3 seed_rgb = rtgi_wrc_sample_radiance(wrc_radiance_atlas, wrc_distance_atlas, wp, cur_plane.xyz, world_dir, cone, wrc_conf);
		// Seed only when the WRC actually has data here; otherwise keep the count-0 reset so
		// we never hold a black value at nonzero confidence for seed_samples frames.
		if (wrc_conf > 0.0) {
			float n_cap = max(params.temporal_n_cap, 1.0);
			float seed_n = clamp(params.seed_samples, 0.0, n_cap - 1.0);
			carried = vec4(spg_sanitize_color(seed_rgb), seed_n / n_cap);
		}
	}
	imageStore(radiance_cur, texel, carried);
}

// BLEND: one thread per gather ray. Blend the ray's incident radiance into the
// reprojected history texel (REPROJECT already ran, with a barrier between) using a
// sample-counted 1/n weight recovered from the confidence channel.
void spg_blend_main() {
	// The workgroup is 8x8 (shared with REPROJECT's 2D atlas dispatch), but BLEND is a
	// 1D dispatch: compute_list_dispatch_threads(rays, 1, 1) still rounds the Y group
	// count up to 1, launching 8 thread-rows. Only row y==0 processes rays so each ray
	// result is consumed by EXACTLY one thread (no 8x over-accumulation / write race).
	if (gl_GlobalInvocationID.y != 0u) {
		return;
	}
	uint ray = gl_GlobalInvocationID.x;
	if (ray >= params.rays_this_frame) {
		return;
	}
	RTGISPGRayResult r = results[ray];
	uint probe_linear = uint(r.probe_dir.x + 0.5);
	uint dir_index = uint(r.probe_dir.y + 0.5);
	uint grid_w = max(params.grid_w, 1u);
	uint oct_res = max(params.oct_res, 1u);
	// Guard a stale/garbage probe index against the addressable grid so a bad index can
	// never scribble outside the atlas (mirrors the WRC accumulate's range guard).
	if (probe_linear >= grid_w * max(params.grid_h, 1u)) {
		return;
	}
	ivec2 probe = ivec2(int(probe_linear % grid_w), int(probe_linear / grid_w));
	ivec2 texel = probe * int(oct_res) + ivec2(int(dir_index % oct_res), int(dir_index / oct_res));
	vec4 cur = imageLoad(radiance_cur, texel); // reprojected history (REPROJECT ran first, barrier between).
	float n_cap = max(params.temporal_n_cap, 1.0);
	float n = clamp(cur.a, 0.0, 1.0) * n_cap;
	float n_new = min(n + 1.0, n_cap);
	float w = 1.0 / max(n_new, 1.0);
	vec3 rad_new = spg_sanitize_color(r.radiance_distance.rgb); // clamp NaN/Inf <= 65504.
	vec3 acc = mix(cur.rgb, rad_new, w);
	imageStore(radiance_cur, texel, vec4(acc, n_new / n_cap));
}

// SPATIAL (A2-T4): one thread per radiance-atlas texel. Smooth this texel against the
// SAME texel-direction sampled from the 3x3 neighbor probes, but only across neighbors
// that lie on the SAME SURFACE (plane-matched on depth + world-pos + normal, the same
// tolerances REPROJECT uses) so radiance never leaks across a silhouette. Because
// neighbor probes generally have a different normal, this texel's world direction is
// re-expressed in each neighbor's tangent frame (RE-ORIENT ON READ, identical to
// REPROJECT) before sampling that neighbor's octahedron. The accumulate already ran
// (REPROJECT + BLEND, barrier before this), so radiance_cur is this frame's final
// per-probe radiance; the filtered result is written to a SEPARATE atlas
// (radiance_filtered) so the unfiltered atlas stays intact for next frame's history.
void spg_spatial_main(ivec2 texel) {
	uint oct_res_u = max(params.oct_res, 1u);
	int oct_res = int(oct_res_u);
	int grid_w = int(params.grid_w);
	int grid_h = int(params.grid_h);
	// texel -> (probe, in-tile dir). Bounds-guard against grid_w/grid_h (the atlas can be
	// wider than grid_w * oct_res only via rounding; guard mirrors REPROJECT).
	ivec2 probe = texel / oct_res;
	ivec2 in_tile = texel - probe * oct_res;
	if (probe.x >= grid_w || probe.y >= grid_h) {
		return;
	}
	vec4 cur_plane = imageLoad(header_plane_cur, probe);
	if (cur_plane.w <= 0.0) {
		imageStore(radiance_filtered, texel, vec4(0.0)); // invalid probe.
		return;
	}
	// The center texel's own (post-accumulate) radiance + confidence. Always the fallback
	// and the carrier of the output confidence (.a), so the filter never inflates n.
	vec4 center = imageLoad(radiance_cur, texel);
	if (params.spatial_radius == 0u) {
		imageStore(radiance_filtered, texel, center); // radius 0 -> straight passthrough copy.
		return;
	}
	// Cap the neighbor reach to a sane 3x3 (..5x5) window; the SPG grid is coarse, so a
	// larger radius would pull in unrelated surfaces faster than it helps.
	int r = int(min(params.spatial_radius, 2u));

	vec4 cur_aux = imageLoad(header_aux_cur, probe);
	vec3 cur_n = oct_to_vec3(cur_aux.xy * 2.0 - 1.0);
	// This texel's world direction (CUR probe local hemioct -> world); re-oriented into
	// each neighbor's frame below. Built once.
	vec2 local_oct = (vec2(in_tile) + 0.5) / float(oct_res);
	vec3 local_dir = spg_hemioct_decode(local_oct);
	vec3 ct, cb;
	spg_build_basis(cur_n, ct, cb);
	vec3 world_dir = spg_local_to_world(local_dir, ct, cb, cur_n);

	// Center contributes with weight = its own confidence: a low-confidence (freshly
	// reset) center leans more on its neighbors, a converged one dominates its own value.
	float center_conf = clamp(center.a, 0.0, 1.0);
	vec3 rgb_sum = center.rgb * center_conf;
	float w_sum = center_conf;

	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			if (dx == 0 && dy == 0) {
				continue; // center already folded in above.
			}
			ivec2 np = probe + ivec2(dx, dy);
			if (np.x < 0 || np.y < 0 || np.x >= grid_w || np.y >= grid_h) {
				continue; // off-grid.
			}
			// All neighbors are THIS frame's probes (same header set as the center) -- the
			// spatial filter is purely intra-frame, so there is no reprojection here.
			vec4 np_plane = imageLoad(header_plane_cur, np);
			if (np_plane.w <= 0.0) {
				continue; // invalid / background neighbor -> reject (no edge leak).
			}
			vec4 np_aux = imageLoad(header_aux_cur, np);
			vec3 np_n = oct_to_vec3(np_aux.xy * 2.0 - 1.0);
			// Plane-match: same surface (linear-depth rel-tol + world-pos dist + normal dot),
			// the same tolerances REPROJECT uses to reject disocclusions.
			float ndot = dot(np_n, cur_n);
			bool match = abs(np_plane.w - cur_plane.w) <= 0.05 * cur_plane.w &&
					distance(np_plane.xyz, cur_plane.xyz) <= 2.0 * cur_plane.w * 0.01 + 0.25 &&
					ndot >= 0.906; // ~25 deg.
			if (!match) {
				continue;
			}
			// RE-ORIENT ON READ: this texel's world_dir expressed in the NEIGHBOR probe's
			// frame -> neighbor hemioct texel. Only valid if it stays in the neighbor's upper
			// hemisphere (else that direction is simply not represented in the neighbor).
			vec3 nt, nb;
			spg_build_basis(np_n, nt, nb);
			vec3 np_local = spg_world_to_local(world_dir, nt, nb, np_n);
			if (np_local.z <= 0.0) {
				continue;
			}
			vec2 np_oct = spg_hemioct_encode(np_local);
			ivec2 np_texel = np * oct_res + clamp(ivec2(np_oct * float(oct_res)), ivec2(0), ivec2(oct_res - 1));
			vec4 np_rad = imageLoad(radiance_cur, np_texel); // .rgb radiance, .a = n/n_cap.
			// Weight: a plane-compatibility term (normal dot, sharpened) * a depth-similarity
			// term * the neighbor texel's confidence. A barely-matching or low-confidence
			// neighbor contributes little; a parallel, converged one contributes fully.
			float w_normal = ndot * ndot * ndot * ndot; // dot^4: falls off well before the 25 deg cutoff.
			float depth_rel = abs(np_plane.w - cur_plane.w) / max(cur_plane.w, 1e-4);
			float w_depth = max(1.0 - depth_rel / 0.05, 0.0); // 1 at equal depth -> 0 at the rel-tol edge.
			float w = w_normal * w_depth * clamp(np_rad.a, 0.0, 1.0);
			if (w > 0.0) {
				rgb_sum += np_rad.rgb * w;
				w_sum += w;
			}
		}
	}

	// Normalize. With any positive weight, output the weighted mean but KEEP the center
	// texel's confidence (.a) -- the filter smooths radiance, it does not change how
	// converged the probe is. With no positive weight (e.g. an isolated probe with a
	// zero-confidence center) fall back to the center texel unchanged.
	vec3 filtered_rgb = (w_sum > 0.0) ? (rgb_sum / w_sum) : center.rgb;
	imageStore(radiance_filtered, texel, vec4(filtered_rgb, center.a));
}

void main() {
	if (params.mode == SPG_MODE_REPROJECT) {
		ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
		if (uint(texel.x) >= params.atlas_width || uint(texel.y) >= params.atlas_height) {
			return;
		}
		spg_reproject_main(texel);
		return;
	} else if (params.mode == SPG_MODE_SPATIAL) {
		// SPATIAL is a 2D dispatch over the atlas (same grid as REPROJECT).
		ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
		if (uint(texel.x) >= params.atlas_width || uint(texel.y) >= params.atlas_height) {
			return;
		}
		spg_spatial_main(texel);
		return;
	}

	// BLEND is a 1D dispatch of rays_this_frame threads (one per ray result).
	spg_blend_main();
}
