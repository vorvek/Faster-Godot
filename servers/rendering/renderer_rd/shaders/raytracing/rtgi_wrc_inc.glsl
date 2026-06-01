// World Radiance Cache (WRC) GPU-side interface.
//
// This include is the GLSL counterpart of the CPU math contract in
//   servers/rendering/renderer_rd/effects/rtgi_wrc_math.h
// and provides the query API consumed by the screen-probe gather / probe-update
// raygen (Task 5), the resolve shader (Task 6) and the ray-hit fallback.
//
// The WRC is a camera-centered cascaded clipmap of octahedral-radiance probes:
//  - Cascade `k` has uniform probe spacing `base_spacing * 2^k`.
//  - Each cascade is a `grid x grid x grid` box of probes centered on the camera,
//    half-extent `base_spacing * 2^k * grid * 0.5`.
//  - Each probe stores an `oct_res x oct_res` octahedral map. A direction index
//    `0..oct_res*oct_res-1` addresses one texel of that map.
//
// BINDING-AGNOSTIC: this include declares NO `layout(set=,binding=)` resources.
// The atlas samplers and the parameter block are passed in as explicit GLSL
// parameters so each consuming shader owns its own descriptor-set layout while
// sharing one correct query implementation.
//
// Expected atlas texture formats (allocated by the consuming shaders / RD effect):
//  - radiance_atlas : RGBA16F. Octahedral directional *radiance* per probe tile,
//                     packed by atlas_coord(). .rgb = radiance, .a = per-texel
//                     confidence/weight in [0,1] (0 == never written).
//  - distance_atlas : RG16F. Per-direction distance moments (mean, mean^2) used
//                     for Chebyshev visibility. Same tile layout as radiance.
//                     A zero .x (mean distance 0) marks an unwritten texel.

#ifndef RTGI_WRC_INC_H
#define RTGI_WRC_INC_H

// ---------------------------------------------------------------------------
//  Parameter block (mirror of RtgiWrc::ClipmapParams + per-frame scalars)
// ---------------------------------------------------------------------------

// Mirrors RtgiWrc::ClipmapParams plus the per-frame camera position and a couple
// of tunables the gather needs. Consumers fill this from their uniform/push data.
struct WrcParams {
	int cascade_count; // RtgiWrc::ClipmapParams::cascade_count
	int grid; // RtgiWrc::ClipmapParams::grid (probes per axis)
	int oct_res; // RtgiWrc::ClipmapParams::oct_res (texels per tile axis)
	float base_spacing; // RtgiWrc::ClipmapParams::base_spacing (cascade 0 probe spacing)
	vec3 camera_pos; // Clipmap center (camera world position) this frame.
	// Self-occlusion bias for Chebyshev visibility, expressed as a fraction of
	// the cascade probe spacing (so it scales with cascade). ~0.5 is a good start.
	float occlusion_bias_spacing;
	// Lower bound on the Chebyshev variance term, in *world units squared*, to
	// keep nearly-flat (zero-variance) visibility from hard-clipping to a step.
	float min_variance;
};

// ---------------------------------------------------------------------------
//  Octahedral math -- PORT OF rtgi_wrc_math.h (Y-up fold).
//
//  IMPORTANT: This intentionally uses the header's *Y-up* octahedral fold
//  (the second oct coordinate is the world Z axis; the fold is gated on the
//  world Y/up axis). This is DELIBERATELY DIFFERENT from oct_inc.glsl, which
//  uses a Z-axis fold (gated on n.z) and the GLSL built-in sign() (0 -> 0).
//  Do NOT swap in oct_inc.glsl here: the WRC probe maps are world-up-aligned
//  and the round-trip contract is pinned by the Task-2 doctest unit tests
//  (tests/servers/rendering/test_rtgi_wrc_math.h). The two functions below
//  round-trip unit vectors to < 1e-5, matching the C++ header.
// ---------------------------------------------------------------------------

// Mirrors RtgiWrc::wrc_sign_not_zero: +1 for non-negative inputs (including 0),
// -1 for negative inputs. This is *not* GLSL sign() (which returns 0 at 0); the
// header relies on signNotZero(0) == +1 so the fold stays a bijection on seams.
float wrc_sign_not_zero(float v) {
	return (v >= 0.0) ? 1.0 : -1.0;
}

vec2 wrc_sign_not_zero(vec2 v) {
	return vec2(wrc_sign_not_zero(v.x), wrc_sign_not_zero(v.y));
}

// Encode a unit direction into octahedral [0,1]^2. Mirrors RtgiWrc::dir_to_oct.
// Axis assignment: x <-> oct.x, world-up Y <-> fold axis, z <-> oct.y.
vec2 wrc_dir_to_oct(vec3 dir) {
	float l1 = abs(dir.x) + abs(dir.y) + abs(dir.z);
	// Guard against a degenerate zero vector; any finite scale is fine here.
	float inv_l1 = (l1 > 0.0) ? (1.0 / l1) : 0.0;
	float ox = dir.x * inv_l1;
	float oy = dir.z * inv_l1;
	if (dir.y < 0.0) {
		float fx = (1.0 - abs(oy)) * wrc_sign_not_zero(ox);
		float fy = (1.0 - abs(ox)) * wrc_sign_not_zero(oy);
		ox = fx;
		oy = fy;
	}
	// Remap [-1,1] -> [0,1].
	return vec2(ox * 0.5 + 0.5, oy * 0.5 + 0.5);
}

// Decode an octahedral [0,1]^2 coordinate into a unit direction.
// Inverse of wrc_dir_to_oct(); mirrors RtgiWrc::oct_to_dir.
vec3 wrc_oct_to_dir(vec2 oct) {
	// Remap [0,1] -> [-1,1].
	float ex = oct.x * 2.0 - 1.0;
	float ey = oct.y * 2.0 - 1.0;
	float vx = ex;
	float vy = 1.0 - abs(ex) - abs(ey);
	float vz = ey;
	// Unfold the lower hemisphere (vy < 0).
	if (vy < 0.0) {
		float fx = (1.0 - abs(ey)) * wrc_sign_not_zero(ex);
		float fz = (1.0 - abs(ex)) * wrc_sign_not_zero(ey);
		vx = fx;
		vz = fz;
	}
	// Axis assignment matches wrc_dir_to_oct: x<->x, y<->up, z<->y.
	vec3 v = vec3(vx, vy, vz);
	float len = length(v);
	// Guard normalize-of-zero (cannot happen for valid oct inputs, but be safe).
	return (len > 0.0) ? (v / len) : vec3(0.0, 1.0, 0.0);
}

// ---------------------------------------------------------------------------
//  Clipmap helpers -- PORT OF rtgi_wrc_math.h.
// ---------------------------------------------------------------------------

// Probe spacing of cascade `k`: base_spacing * 2^k. Mirrors the `1 << k` factor.
float wrc_cascade_spacing(WrcParams p, int cascade) {
	return p.base_spacing * float(1 << cascade);
}

// Half-extent (world units) of cascade `k`'s cubic box.
// Mirrors RtgiWrc::cascade_half_extent.
float wrc_cascade_half_extent(WrcParams p, int cascade) {
	return wrc_cascade_spacing(p, cascade) * float(p.grid) * 0.5;
}

// Select the smallest cascade whose cubic box contains `world_pos` relative to
// the camera, using max-axis (Chebyshev) distance. Clamps to the last cascade.
// Mirrors RtgiWrc::select_cascade.
int wrc_select_cascade(WrcParams p, vec3 world_pos) {
	vec3 d = abs(world_pos - p.camera_pos);
	float chebyshev = max(d.x, max(d.y, d.z));
	int last = max(p.cascade_count - 1, 0);
	for (int k = 0; k < p.cascade_count; k++) {
		if (chebyshev <= wrc_cascade_half_extent(p, k)) {
			return k;
		}
	}
	return last;
}

// Number of probe tiles per atlas row. Mirrors RtgiWrc::atlas_tiles_per_row:
// ceil(sqrt(cascade_count * grid^3)).
int wrc_atlas_tiles_per_row(WrcParams p) {
	int total_tiles = p.cascade_count * p.grid * p.grid * p.grid;
	int n = int(ceil(sqrt(float(max(total_tiles, 1)))));
	return max(n, 1);
}

// Bottom-left atlas texel of the tile owned by (cascade, probe). The per-texel
// offset is added by the caller. Mirrors the tile placement in
// RtgiWrc::atlas_coord (the direction-index -> in-tile offset is handled
// separately so we can sample arbitrary fractional oct coordinates).
ivec2 wrc_atlas_tile_origin(WrcParams p, int cascade, ivec3 probe) {
	int grid = p.grid;
	// linear = ((cascade * grid + z) * grid + y) * grid + x   (row-major).
	int linear = ((cascade * grid + probe.z) * grid + probe.y) * grid + probe.x;
	int tiles_per_row = wrc_atlas_tiles_per_row(p);
	int tile_col = linear % tiles_per_row;
	int tile_row = linear / tiles_per_row;
	return ivec2(tile_col * p.oct_res, tile_row * p.oct_res);
}

// Atlas UV (in [0,1] over the whole atlas) for octahedral coordinate `oct`
// (itself in [0,1]^2) inside the tile owned by (cascade, probe). The half-texel
// inset keeps `oct` addressing texel centers, matching oct_texel_center() on the
// CPU; the [inset, 1-inset] clamp stops bilinear taps from bleeding into the
// neighbouring tile across the octahedron seam.
vec2 wrc_atlas_uv(WrcParams p, int cascade, ivec3 probe, vec2 oct, vec2 inv_atlas_size) {
	ivec2 origin = wrc_atlas_tile_origin(p, cascade, probe);
	float res = float(p.oct_res);
	float inset = 0.5 / res; // half a texel, in tile-local [0,1].
	vec2 t = clamp(oct, vec2(inset), vec2(1.0 - inset));
	vec2 texel = vec2(origin) + t * res; // Absolute texel coordinate (center-addressed via `t`).
	return texel * inv_atlas_size;
}

// ---------------------------------------------------------------------------
//  Visibility (Chebyshev / variance shadow maps).
// ---------------------------------------------------------------------------

// Chebyshev one-tailed inequality: upper bound on the probability that the
// surface at `dist_to_surface` is *visible* from the probe along `dir`, given
// the stored distance moments (mean, mean^2). `spacing` scales the self-
// occlusion bias and the variance floor so the test behaves consistently across
// cascades. Returns 1.0 when the surface is at/closer than the mean (lit).
float wrc_chebyshev(vec2 moments, float dist_to_surface, float spacing, WrcParams p) {
	float mean = moments.x;
	// Unwritten texel (mean == 0): no information -> treat as not visible so the
	// corner is rejected and the aggregate confidence reflects the gap.
	if (mean <= 0.0) {
		return 0.0;
	}
	// Bias the comparison distance toward the probe to suppress self-occlusion
	// (scaled by probe spacing so larger cascades use a larger absolute bias).
	float biased = dist_to_surface - p.occlusion_bias_spacing * spacing;
	if (biased <= mean) {
		return 1.0;
	}
	float variance = moments.y - mean * mean;
	// Clamp the variance: never negative (fp moment error) and never below a
	// spacing-scaled floor so flat regions don't hard-clip to a binary step.
	float var_floor = p.min_variance * spacing * spacing;
	variance = max(variance, var_floor);
	float d = biased - mean;
	float p_max = variance / (variance + d * d);
	return clamp(p_max, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
//  Probe-corner gather.
// ---------------------------------------------------------------------------

// Floor probe coordinate + fractional position of `world_pos` inside cascade
// `cascade`'s grid. The clipmap grid is centered on the camera: probe (0..grid-1)
// spans [camera - half_extent, camera + half_extent]. Returns the lower corner
// probe index in `base_probe` and the in-cell fraction in `frac` (both clamped
// so the 8 corners stay inside [0, grid-1]).
void wrc_locate(WrcParams p, int cascade, vec3 world_pos, out ivec3 base_probe, out vec3 frac) {
	float spacing = wrc_cascade_spacing(p, cascade);
	float half_extent = wrc_cascade_half_extent(p, cascade);
	// Position in grid space where probe centers sit on integer coordinates.
	// grid_pos = (world - (camera - half_extent)) / spacing - 0.5
	// (the -0.5 puts the cell corners on integers so floor() picks the lower
	// neighbouring probe and `frac` is the trilinear weight to the upper one).
	vec3 grid_pos = (world_pos - (p.camera_pos - vec3(half_extent))) / spacing - vec3(0.5);
	vec3 base_f = floor(grid_pos);
	frac = clamp(grid_pos - base_f, vec3(0.0), vec3(1.0));
	ivec3 b = ivec3(base_f);
	// Clamp so the upper corner (b+1) also stays in range.
	base_probe = clamp(b, ivec3(0), ivec3(p.grid - 2));
	// If we clamped, fold the residual into frac so we still interpolate sanely
	// at the clipmap boundary instead of snapping.
	frac = clamp(frac + (vec3(b) - vec3(base_probe)), vec3(0.0), vec3(1.0));
}

// Trilinear weight of corner `c` (each component 0 or 1) given fraction `frac`.
float wrc_corner_weight(ivec3 c, vec3 frac) {
	vec3 w = mix(vec3(1.0) - frac, frac, vec3(c));
	return w.x * w.y * w.z;
}

// ---------------------------------------------------------------------------
//  Public query API (binding-agnostic).
// ---------------------------------------------------------------------------

// Cosine-weighted irradiance from the WRC at `world_pos` for surface normal `N`.
//
// Selects a cascade by distance, then trilinearly blends the 8 surrounding
// probes. For each probe it cosine-integrates the octahedral radiance map
// against `N` (sum over all oct_res^2 texels of radiance * max(0, dot(N, dir)) *
// texel_solid_angle, normalized by the summed cosine*solid_angle). Corners are
// weighted by trilinear weight * Chebyshev visibility * stored confidence.
//
// `confidence` (out, [0,1]) aggregates how much trustworthy data backed the
// result. Returns vec3(0) with confidence 0 when no usable data was found.
vec3 rtgi_wrc_sample_irradiance(
		sampler2D radiance_atlas,
		sampler2D distance_atlas,
		WrcParams p,
		vec3 world_pos,
		vec3 N,
		out float confidence) {
	confidence = 0.0;

	vec2 atlas_size = vec2(textureSize(radiance_atlas, 0));
	if (atlas_size.x <= 0.0 || atlas_size.y <= 0.0) {
		return vec3(0.0);
	}
	vec2 inv_atlas_size = 1.0 / atlas_size;

	float n_len = length(N);
	if (n_len <= 0.0) {
		return vec3(0.0); // Guard normalize-of-zero.
	}
	vec3 nN = N / n_len;

	int cascade = wrc_select_cascade(p, world_pos);
	float spacing = wrc_cascade_spacing(p, cascade);

	ivec3 base_probe;
	vec3 frac;
	wrc_locate(p, cascade, world_pos, base_probe, frac);

	int res = p.oct_res;
	float inv_res = 1.0 / float(res);
	// Uniform-grid solid-angle approximation: the octahedron maps the whole
	// sphere (4*PI sr) over res^2 texels. The per-texel solid angle cancels in
	// the cosine-weight normalization below, so we fold it into a constant and
	// rely on the normalization; it is kept here for clarity / future weighting.
	// (Constant across texels -> cancels, so we omit it from the running sums.)

	vec3 irradiance_sum = vec3(0.0);
	float weight_sum = 0.0; // sum of corner weights actually used.
	float confidence_sum = 0.0;

	// Iterate the 8 corners of the trilinear cell.
	for (int ci = 0; ci < 8; ci++) {
		ivec3 c = ivec3(ci & 1, (ci >> 1) & 1, (ci >> 2) & 1);
		float tw = wrc_corner_weight(c, frac);
		if (tw <= 0.0) {
			continue;
		}
		ivec3 probe = base_probe + c;

		// Direction from this probe's center to the shading point, for the
		// Chebyshev visibility test. Probe center world position:
		float half_extent = wrc_cascade_half_extent(p, cascade);
		vec3 probe_pos = (p.camera_pos - vec3(half_extent)) + (vec3(probe) + vec3(0.5)) * spacing;
		vec3 to_surface = world_pos - probe_pos;
		float dist_to_surface = length(to_surface);
		vec3 vis_dir = (dist_to_surface > 0.0) ? (to_surface / dist_to_surface) : nN;

		// Cosine-integrate this probe's octahedral radiance against N.
		vec3 probe_irr = vec3(0.0);
		float cos_norm = 0.0; // summed cosine weights (solid angle cancels).
		float probe_conf = 0.0;
		float conf_norm = 0.0;
		for (int ty = 0; ty < res; ty++) {
			for (int tx = 0; tx < res; tx++) {
				vec2 texel_oct = (vec2(tx, ty) + vec2(0.5)) * inv_res; // texel center.
				vec3 texel_dir = wrc_oct_to_dir(texel_oct);
				float ndl = max(0.0, dot(nN, texel_dir));
				if (ndl <= 0.0) {
					continue;
				}
				vec2 uv = wrc_atlas_uv(p, cascade, probe, texel_oct, inv_atlas_size);
				vec4 rad = textureLod(radiance_atlas, uv, 0.0);
				// .a holds per-texel confidence; weight radiance by it so
				// never-written texels (a==0) don't poison the integral.
				probe_irr += rad.rgb * (ndl * rad.a);
				cos_norm += ndl;
				probe_conf += rad.a * ndl;
				conf_norm += ndl;
			}
		}
		if (cos_norm <= 0.0 || conf_norm <= 0.0) {
			continue; // No lit hemisphere coverage for this corner.
		}
		probe_irr /= cos_norm; // Cosine-weighted average radiance (== irradiance/PI scale-free).
		probe_conf /= conf_norm; // Mean per-texel confidence over the lit hemisphere.

		// Chebyshev visibility of the shading point from this probe.
		vec2 vis_oct = wrc_dir_to_oct(vis_dir);
		vec2 vis_uv = wrc_atlas_uv(p, cascade, probe, vis_oct, inv_atlas_size);
		vec2 moments = textureLod(distance_atlas, vis_uv, 0.0).xy;
		float vis = wrc_chebyshev(moments, dist_to_surface, spacing, p);

		float w = tw * vis * probe_conf;
		if (w <= 0.0) {
			continue;
		}
		irradiance_sum += probe_irr * w;
		weight_sum += w;
		// Aggregate confidence is the trilinear-weighted mean of per-corner
		// (visibility * stored confidence); independent of the radiance weight
		// so a dim-but-trusted probe still reports high confidence.
		confidence_sum += tw * (vis * probe_conf);
	}

	if (weight_sum <= 0.0) {
		confidence = 0.0;
		return vec3(0.0); // Empty-weight guard.
	}
	confidence = clamp(confidence_sum, 0.0, 1.0); // trilinear weights sum to 1.
	return irradiance_sum / weight_sum;
}

// Directional radiance from the WRC at `world_pos` along `dir`, integrated over
// a small cone of half-angle ~`cone` (radians). Same cascade/corner selection
// and weighting as the irradiance query, but instead of a cosine hemisphere
// integral it averages the few octahedral texels nearest `dir` (a box of texels
// whose angular size tracks `cone`), giving a glossy/cone-traced radiance.
//
// `confidence` (out, [0,1]) as above. Returns vec3(0)/confidence 0 when empty.
vec3 rtgi_wrc_sample_radiance(
		sampler2D radiance_atlas,
		sampler2D distance_atlas,
		WrcParams p,
		vec3 world_pos,
		vec3 dir,
		float cone,
		out float confidence) {
	confidence = 0.0;

	vec2 atlas_size = vec2(textureSize(radiance_atlas, 0));
	if (atlas_size.x <= 0.0 || atlas_size.y <= 0.0) {
		return vec3(0.0);
	}
	vec2 inv_atlas_size = 1.0 / atlas_size;

	float d_len = length(dir);
	if (d_len <= 0.0) {
		return vec3(0.0); // Guard normalize-of-zero.
	}
	vec3 nD = dir / d_len;

	int cascade = wrc_select_cascade(p, world_pos);
	float spacing = wrc_cascade_spacing(p, cascade);

	ivec3 base_probe;
	vec3 frac;
	wrc_locate(p, cascade, world_pos, base_probe, frac);

	int res = p.oct_res;
	float inv_res = 1.0 / float(res);

	// Cone -> texel radius. One oct texel subtends roughly (2/oct_res) of the
	// [-1,1] oct domain, ~ (PI / oct_res) radians near the center of a face.
	// Map the requested cone half-angle to a texel radius, clamped to [0, res].
	float texels_per_rad = float(res) / 3.14159265; // ~ inverse of per-texel angle.
	int radius = int(clamp(ceil(cone * texels_per_rad), 1.0, float(res)));

	// Center texel of `dir` in tile-local texel coordinates.
	vec2 center_oct = wrc_dir_to_oct(nD);
	ivec2 center_texel = ivec2(floor(center_oct * float(res)));
	center_texel = clamp(center_texel, ivec2(0), ivec2(res - 1));

	vec3 radiance_sum = vec3(0.0);
	float weight_sum = 0.0;
	float confidence_sum = 0.0;

	for (int ci = 0; ci < 8; ci++) {
		ivec3 c = ivec3(ci & 1, (ci >> 1) & 1, (ci >> 2) & 1);
		float tw = wrc_corner_weight(c, frac);
		if (tw <= 0.0) {
			continue;
		}
		ivec3 probe = base_probe + c;

		float half_extent = wrc_cascade_half_extent(p, cascade);
		vec3 probe_pos = (p.camera_pos - vec3(half_extent)) + (vec3(probe) + vec3(0.5)) * spacing;
		vec3 to_surface = world_pos - probe_pos;
		float dist_to_surface = length(to_surface);
		vec3 vis_dir = (dist_to_surface > 0.0) ? (to_surface / dist_to_surface) : nD;

		// Average the texels nearest `dir`, weighting each by its angular
		// closeness to `dir` and its stored per-texel confidence.
		vec3 probe_rad = vec3(0.0);
		float rad_norm = 0.0;
		float probe_conf = 0.0;
		float conf_norm = 0.0;
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				ivec2 t = center_texel + ivec2(dx, dy);
				// Clamp to the tile; the seam-aware UV clamp keeps taps in-tile,
				// and clamping the index avoids re-deriving the mirrored neighbour.
				t = clamp(t, ivec2(0), ivec2(res - 1));
				vec2 texel_oct = (vec2(t) + vec2(0.5)) * inv_res;
				vec3 texel_dir = wrc_oct_to_dir(texel_oct);
				float aligned = max(0.0, dot(nD, texel_dir));
				if (aligned <= 0.0) {
					continue;
				}
				// Sharpen the angular falloff with the cone (smaller cone ->
				// tighter lobe). cos^k with k from the cone size.
				float k = max(1.0, texels_per_rad / max(float(radius), 1.0));
				float aw = pow(aligned, k);
				vec2 uv = wrc_atlas_uv(p, cascade, probe, texel_oct, inv_atlas_size);
				vec4 rad = textureLod(radiance_atlas, uv, 0.0);
				probe_rad += rad.rgb * (aw * rad.a);
				rad_norm += aw * rad.a;
				probe_conf += rad.a * aw;
				conf_norm += aw;
			}
		}
		if (rad_norm <= 0.0 || conf_norm <= 0.0) {
			continue;
		}
		probe_rad /= rad_norm;
		probe_conf /= conf_norm;

		vec2 vis_oct = wrc_dir_to_oct(vis_dir);
		vec2 vis_uv = wrc_atlas_uv(p, cascade, probe, vis_oct, inv_atlas_size);
		vec2 moments = textureLod(distance_atlas, vis_uv, 0.0).xy;
		float vis = wrc_chebyshev(moments, dist_to_surface, spacing, p);

		float w = tw * vis * probe_conf;
		if (w <= 0.0) {
			continue;
		}
		radiance_sum += probe_rad * w;
		weight_sum += w;
		confidence_sum += tw * (vis * probe_conf);
	}

	if (weight_sum <= 0.0) {
		confidence = 0.0;
		return vec3(0.0);
	}
	confidence = clamp(confidence_sum, 0.0, 1.0);
	return radiance_sum / weight_sum;
}

#endif // RTGI_WRC_INC_H
