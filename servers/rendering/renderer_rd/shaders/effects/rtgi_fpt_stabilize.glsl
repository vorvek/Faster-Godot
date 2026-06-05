#[compute]

#version 450

#VERSION_DEFINES

// RTGI Full Path Tracing primary-direct temporal stabilizer (no-upscaler path).
//
// FPT-fast shades the directly-visible surface with a stochastic NEE direct term (soft-shadow
// cone + shadow-ray RNG re-seeded every frame), so the primary-direct color in RB_TEX_RAYTRACING
// boils frame to frame even with a frozen camera. This pass temporally accumulates it BEFORE the
// composite so the final image is stable. It is the per-pixel analogue of the proven GI accumulate
// rtgi_gi_resolve.glsl::resolve_temporal_main, run at rt_size on the RT guides the FPT primary
// already stored (rtgi_store_raster_guides):
//   * REPROJECT the previous accumulated color with the rt-size velocity guide
//     (RB_TEX_RT_VELOCITY = prev_texture_uv - cur_texture_uv, so prev_uv = cur_uv + mv).
//   * ACCEPT the reprojected history on a SAME-SURFACE depth + normal gate read from the CURRENT
//     guides (RB_TEX_RT_VIEWZ_HITDIST.x = abs(view_z), RB_TEX_RT_NORMAL_ROUGHNESS = world normal),
//     exactly like resolve_history_valid. It does NOT use the rt_history_id / *_history_*_prev
//     buffers (the FPT primary takes no ReSTIR reuse, so those prev buffers are unmaintained for
//     these pixels and a gate keyed on them rejects every pixel).
//   * FIREFLY-CLAMP the current sample to its 3x3 YCoCg neighborhood mean +/- k*sigma.
//   * BLEND sample-counted 1/n (n-capped), but with DARK-DROP RESISTANCE: a near-black current
//     sample far below the converged history (a sub-native, resolution_scale < 1, primary-direct
//     mis-visibility "miss" on a grazing surface) gets its blend weight scaled down by how large
//     the relative drop is, so a transient miss cannot pull a converged-lit pixel to black (which
//     the naive 1/n blend latched into persistent black blotches). A small or gradual darkening
//     still blends near-normally, and a surface that is dark from its first sample (a real static
//     shadow) has no drop to resist, so legitimate shadows are unaffected.
//
// This stabilizer is invoked ONLY when no jittered temporal upscaler (FSR2/XeSS/TAA) is active:
// pre-averaging the primary is incompatible with such an upscaler's own jittered accumulation (it
// converges the deterministic raster primary but not a pre-averaged path-traced one). The caller
// gates it; here it is a plain temporal accumulate. FPT-only; Hybrid never runs it, and the deep-
// path reference oracle is excluded upstream.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Binding 0: the CURRENT-frame FPT primary-direct color (RB_TEX_RAYTRACING) at rt_size.
layout(set = 0, binding = 0) uniform sampler2D cur_color;
// Binding 1: the PREVIOUS frame's accumulated color (the [1 - read_index] ping-pong buffer).
layout(set = 0, binding = 1) uniform sampler2D prev_stable;
// Binding 2: THIS frame's stabilized output (the [read_index] ping-pong buffer). rgb = accumulated
// color, a = n / n_cap (the sample-count fraction next frame reads back).
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D stable_out;
// Binding 3: rt-size velocity guide (RB_TEX_RT_VELOCITY). .xy = prev_texture_uv - cur_texture_uv.
layout(set = 0, binding = 3) uniform sampler2D velocity_buffer;
// Binding 4: rt-size surface mask (RB_TEX_RT_HISTORY_VALIDITY). .r >= 0.5 on a shaded FPT surface.
layout(set = 0, binding = 4) uniform sampler2D history_validity;
// Binding 5: rt-size view-z + hit distance (RB_TEX_RT_VIEWZ_HITDIST). .x = abs(view_pos.z).
layout(set = 0, binding = 5) uniform sampler2D viewz_hitdist;
// Binding 6: rt-size normal-roughness (RB_TEX_RT_NORMAL_ROUGHNESS). .xyz = world_normal*0.5+0.5.
layout(set = 0, binding = 6) uniform sampler2D normal_roughness;

layout(push_constant, std430) uniform Params {
	uint screen_w; // rt_size.x
	uint screen_h; // rt_size.y
	uint frame_index; // 0 => no history yet (pure seed); reproject only when > 0.
	uint reset; // 1 => discard history this frame (realloc / camera-cut): pure seed.
	float n_cap; // history responsiveness (the 1/n blend caps n here).
	float firefly_k; // YCoCg neighborhood clamp width, in sigmas.
	float normal_threshold; // min dot(cur_n, prev_n) to accept history.
	float depth_rel_tol; // relative view-z tolerance to accept history.
	uint pad0;
	uint pad1;
	uint pad2;
	uint pad3;
}
pc;

// Reversible RGB<->YCoCg (the standard integer-free lift). The neighborhood firefly clamp runs in
// YCoCg because luma (Y) carries the firefly energy and the chroma axes bound color. Co/Cg signed.
vec3 rgb_to_ycocg(vec3 c) {
	float y = dot(c, vec3(0.25, 0.5, 0.25));
	float co = dot(c, vec3(0.5, 0.0, -0.5));
	float cg = dot(c, vec3(-0.25, 0.5, -0.25));
	return vec3(y, co, cg);
}

vec3 ycocg_to_rgb(vec3 c) {
	float t = c.x - c.z; // y - cg
	return vec3(t + c.y, c.x + c.z, t - c.y);
}

float luma(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Clamp negatives to 0, replace Inf/NaN with 0 (a poisoned sample must not stick in the persistent
// ping-pong buffer), clamp to the RGBA16F max. Mirrors rtgi_gi_resolve.glsl's sanitize idiom.
vec3 sanitize_color(vec3 c) {
	c = max(c, vec3(0.0));
	c = mix(c, vec3(0.0), isnan(c));
	c = mix(c, vec3(0.0), isinf(c));
	return min(c, vec3(65504.0));
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(int(pc.screen_w), int(pc.screen_h));
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}

	vec3 cur = texelFetch(cur_color, pixel, 0).rgb;

	// Sky / miss / empty pixel: deterministic sky-on-miss color, no surface mask, nothing to
	// accumulate. Pass through with n == 0 so it never seeds a surface history.
	float cur_valid = texelFetch(history_validity, pixel, 0).r;
	if (cur_valid < 0.5) {
		imageStore(stable_out, pixel, vec4(sanitize_color(cur), 0.0));
		return;
	}

	// FIREFLY CLAMP: bound the current sample to its 3x3 YCoCg neighborhood mean +/- k*sigma. A
	// stochastic NEE spike is far outside the local mean and is pulled back before it can enter the
	// accumulator. Edge texels clamp the tap coordinate.
	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 t = clamp(pixel + ivec2(dx, dy), ivec2(0), size - ivec2(1));
			vec3 yc = rgb_to_ycocg(texelFetch(cur_color, t, 0).rgb);
			m1 += yc;
			m2 += yc * yc;
		}
	}
	const float inv_n = 1.0 / 9.0;
	vec3 mean = m1 * inv_n;
	vec3 sigma = sqrt(max(m2 * inv_n - mean * mean, vec3(0.0)));
	vec3 cur_yc = rgb_to_ycocg(cur);
	float k = max(pc.firefly_k, 0.0);
	vec3 clamped_yc = clamp(cur_yc, mean - k * sigma, mean + k * sigma);
	vec3 clamped_cur = sanitize_color(ycocg_to_rgb(clamped_yc));

	// REPROJECT: prev_uv = cur_uv + velocity. On a frozen camera velocity == 0 -> prev == pixel.
	vec2 cur_uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
	vec2 mv = texelFetch(velocity_buffer, pixel, 0).xy;
	ivec2 prev_pixel = ivec2(floor((cur_uv + mv) * vec2(size)));

	bool accept = false;
	float hist_n = 0.0;
	vec3 hist_rgb = vec3(0.0);

	// frame_index 0 (or a reset) has no usable history; the bounds guard keeps the reprojected fetch
	// on-screen (off-screen history is a disocclusion -> reseed).
	if (pc.frame_index > 0u && pc.reset == 0u &&
			all(greaterThanEqual(prev_pixel, ivec2(0))) && all(lessThan(prev_pixel, size))) {
		// SAME-SURFACE gate (depth AND normal) from the CURRENT-frame guides at both pixels (this fork
		// keeps no prev-frame G-buffer; the GI resolve uses the same current-guide reproject validity).
		// The reprojected pixel must also be a shaded surface this frame, not sky.
		if (texelFetch(history_validity, prev_pixel, 0).r >= 0.5) {
			float cur_z = texelFetch(viewz_hitdist, pixel, 0).x;
			float prev_z = texelFetch(viewz_hitdist, prev_pixel, 0).x;
			float ztol = max(pc.depth_rel_tol, 0.0) * max(cur_z, 1e-4);
			vec3 cur_n = normalize(texelFetch(normal_roughness, pixel, 0).xyz * 2.0 - 1.0);
			vec3 prev_n = normalize(texelFetch(normal_roughness, prev_pixel, 0).xyz * 2.0 - 1.0);
			if (abs(cur_z - prev_z) <= ztol && dot(cur_n, prev_n) >= pc.normal_threshold) {
				vec4 h = texelFetch(prev_stable, prev_pixel, 0);
				hist_rgb = h.rgb;
				hist_n = h.a * pc.n_cap; // recover the stored sample count (.a == n / n_cap).
				accept = true;
			}
		}
	}

	float n_cap = max(pc.n_cap, 1.0);
	float nn = min(hist_n + 1.0, n_cap);
	float weight = 1.0 / max(nn, 1.0);

	// DARK-DROP REJECTION: drop a near-TOTAL black dropout from the blend this frame. A sub-native
	// (resolution_scale < 1) primary-direct MISS reads ~0 on a grazing surface the half-res sample
	// slipped off; without this the 1/n blend latches that black into a persistent blotch (worse at
	// high n_cap). The cut is RELATIVE and EXTREME -- current below 2% of a clearly-lit history -- so
	// normal stochastic noise (firefly-clamped to within ~1.5 sigma of the neighborhood mean) never
	// triggers it, the delta is unaffected, and a legitimately dark pixel (history at/near black) is
	// exempt. On trigger the converged history is kept (weight 0); n still advances so a real later
	// darkening converges normally. Only applied to accepted history (a fresh pixel takes cur outright).
	if (accept) {
		float hist_luma = luma(hist_rgb);
		float cur_luma = luma(clamped_cur);
		if (hist_luma > 1e-3 && cur_luma < 0.02 * hist_luma) {
			weight = 0.0;
		}
	}

	vec3 outc = accept ? mix(hist_rgb, clamped_cur, weight) : clamped_cur;
	outc = sanitize_color(outc);
	imageStore(stable_out, pixel, vec4(outc, nn / n_cap));
}
