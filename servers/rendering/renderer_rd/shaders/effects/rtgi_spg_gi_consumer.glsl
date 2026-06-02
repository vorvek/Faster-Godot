#[compute]

#version 450

#VERSION_DEFINES

// Screen Probe Gather (SPG) GI debug consumer (A2-T5).
//
// VALIDATION-ONLY per-pixel CONSUMER of the SPG per-probe radiance atlas: for each
// screen pixel it reconstructs the WORLD-space surface position from the depth
// buffer, decodes the WORLD-space surface normal from the normal-roughness
// G-buffer, locates the 4 surrounding screen probes, cosine-integrates each probe's
// HEMISPHERE-octahedral radiance tile against the surface normal, and bilinearly
// blends them (weighted by plane/normal compatibility). The RAW cosine-mean
// incident radiance is written straight to the output image -- NOT multiplied by
// albedo and NOT tonemapped -- so the A2-T6 furnace oracle gate can compare the
// sphere-pixel value directly against the known environment radiance L (linear).
//
// This is the SCREEN-PROBE analogue of rtgi_wrc_gi_consumer.glsl. It is NOT the
// production per-pixel resolve (that is A3): there is no demod/remod and no
// composite into beauty. It exists only so the furnace metric has a consumer.
//
// Coordinate-space contract (verified against rtgi_wrc_gi_consumer.glsl +
// rtgi_screen_probe_gather.glsl):
//   * The depth buffer (RB_TEX_DEPTH) holds RAW reverse-Z hyperbolic depth; the
//     cleared far/sky value is 0.0. View position is inv_projection * vec4(2*uv-1,
//     depth, 1) (homogeneous divide) -- the inverse projection already encodes the
//     reverse-Z mapping, so no z remap.
//   * The normal-roughness G-buffer normal is in VIEW space (normalize(rgb*2-1));
//     we rotate it VIEW->WORLD via mat3(inv_view).
//   * inv_view == the camera's view->world transform, so world pos is inv_view *
//     view_pos (full affine, incl. translation) and world normal is
//     mat3(inv_view) * view_normal. Linear depth used for the probe plane match is
//     -view_pos.z (positive in front of the camera), matching header_plane.w which
//     the PLACE pass stored as the probe's linear depth.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Shared SPG hemi-oct basis math (local +Z = anchor normal): spg_build_basis,
// spg_hemioct_decode, spg_local_to_world. Same include the PLACE/gather passes use,
// so the local->world tile orientation here matches how the atlas was written.
#include "../raytracing/rtgi_spg_inc.glsl"
// Full-sphere octahedral decode (oct_to_vec3) for the probe header normal, which the
// PLACE pass stored via vec3_to_oct of the anchor WORLD normal.
#include "../oct_inc.glsl"

// SPG textures owned by the RTGIScreenProbeGather effect. radiance_filtered is the
// SPATIAL-filtered per-probe octahedral atlas (RGBA16F, grid_w*oct_res x
// grid_h*oct_res): each probe owns an oct_res x oct_res HEMISPHERE-octahedral tile
// (local +Z = the probe's anchor normal), .rgb = incident radiance, .a = confidence
// (n/n_cap, 0 = unwritten). header_plane: .xyz = anchor WORLD position, .w = linear
// depth (<= 0 invalid). header_aux: .xy = oct-normal (vec3_to_oct of the anchor
// WORLD normal), .zw = motion. All sampled via integer texelFetch (point), so the
// linear sampler bound to them is only there to satisfy SAMPLER_WITH_TEXTURE.
layout(set = 0, binding = 0) uniform sampler2D radiance_filtered;
layout(set = 0, binding = 1) uniform sampler2D header_plane;
layout(set = 0, binding = 2) uniform sampler2D header_aux;
// Raw scene depth (reverse-Z hyperbolic) and view-space normal-roughness.
layout(set = 0, binding = 3) uniform sampler2D depth_buffer;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_buffer;
// Linear, un-tonemapped incident-radiance output (RGBA16F). .rgb = cosine-mean
// radiance, .a = 1 when probes contributed / 0.5 when none did (for inspection; the
// gate reads .rgb).
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D dest_image;

// Two mat4s (128 bytes) already hit RenderingDevice's 128-byte push-constant cap
// (MAX_PUSH_CONSTANT_SIZE), so these params live in a UBO (uncapped) bound at the
// next free set-0 binding after the output image. Layout matches
// RTGIScreenProbeGather::SpgGiUBO exactly: two 16-byte-aligned mat4s (offsets 0 and
// 64), then the trailing scalars packed per std140 (offset 128..), tail padded to a
// multiple of 16.
layout(set = 0, binding = 6, std140) uniform SpgGiParams {
	mat4 inv_projection; // clip -> view.
	mat4 inv_view; // view -> world (camera transform).
	int screen_width;
	int screen_height;
	int grid_w;
	int grid_h;
	int spacing_f;
	int oct_res;
	float strength;
	int pad0; // tail pad: 7 scalars (offset 128..156) padded to 160 (multiple of 16).
}
params;

// Reconstruct VIEW-space position from the raw depth buffer at integer pixel `pos`.
// Mirrors rtgi_wrc_gi_consumer.glsl::reconstruct_view_position: build a clip-space
// point from the pixel's NDC xy + raw depth z and run it through inv_projection.
vec3 reconstruct_view_position(ivec2 pos, float raw_depth) {
	vec4 clip;
	clip.xy = (2.0 * (vec2(pos) + vec2(0.5)) / vec2(params.screen_width, params.screen_height)) - 1.0;
	clip.z = raw_depth;
	clip.w = 1.0;
	vec4 view = params.inv_projection * clip;
	return view.xyz / view.w;
}

// Cosine-integrate one probe's HEMISPHERE-octahedral radiance tile against the
// world-space surface normal `world_N`. The tile is oriented in the probe's OWN
// anchor basis (local +Z = probe_N), so each local hemioct direction is rotated to
// world via that basis before the dot with world_N. Returns the cosine-weighted
// average incident radiance and reports lit-hemisphere coverage in `cos_norm` (0 ==
// no usable texels).
//
// CONFIDENCE-WEIGHTED NORMALIZER (mirrors rtgi_wrc_sample_irradiance): the
// normalizer accumulates ndl * rad.a, matching the ndl * rad.a numerator, so rad.a
// cancels in the divide -- unwritten texels (a == 0) are excluded and partial /
// under-converged coverage still yields the true cosine-mean radiance (~= L on the
// furnace) rather than a value dimmed by the average confidence. (Normalizing by
// plain ndl, as the WRC bug did pre-4884a49, dims the GI.)
vec3 integrate_probe(ivec2 probe, vec3 probe_N, vec3 world_N, out float cos_norm) {
	cos_norm = 0.0;
	vec3 irr = vec3(0.0);

	int res = params.oct_res;
	float inv_res = 1.0 / float(res);
	vec3 pt, pb;
	spg_build_basis(probe_N, pt, pb);
	ivec2 tile_origin = probe * res;

	for (int ty = 0; ty < res; ty++) {
		for (int tx = 0; tx < res; tx++) {
			vec2 local_oct = (vec2(tx, ty) + vec2(0.5)) * inv_res; // texel center.
			vec3 local_dir = spg_hemioct_decode(local_oct); // local, +Z = probe_N.
			vec3 world_dir = spg_local_to_world(local_dir, pt, pb, probe_N);
			float ndl = max(0.0, dot(world_N, world_dir));
			if (ndl <= 0.0) {
				continue;
			}
			vec4 rad = texelFetch(radiance_filtered, tile_origin + ivec2(tx, ty), 0);
			// .a holds per-texel confidence; weight radiance by it so never-written
			// texels (a == 0) don't poison the integral, and divide by the SAME
			// confidence-weighted cosine sum so rad.a cancels (coverage-robust).
			irr += rad.rgb * (ndl * rad.a);
			cos_norm += ndl * rad.a;
		}
	}
	return (cos_norm > 0.0) ? (irr / cos_norm) : vec3(0.0);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= params.screen_width || pos.y >= params.screen_height) {
		return;
	}

	// Raw reverse-Z depth. The cleared far/sky value is 0.0 -> no geometry; the gate
	// expects vec3(0) for background pixels (no probes contribute there anyway).
	float raw_depth = texelFetch(depth_buffer, pos, 0).r;
	if (raw_depth <= 0.0) {
		imageStore(dest_image, pos, vec4(0.0));
		return;
	}

	// View-space normal from the G-buffer (normalize(rgb*2-1)); a zero/degenerate
	// normal (cleared texel with no geometry) means "no surface" -> output 0.
	vec3 enc = texelFetch(normal_roughness_buffer, pos, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.0001) {
		imageStore(dest_image, pos, vec4(0.0));
		return;
	}
	view_normal = normalize(view_normal);

	// VIEW -> WORLD. inv_view is the camera's view->world transform, so a position
	// uses the full affine (rotation + translation) and a normal uses the 3x3. The
	// probe plane match below compares header_plane.w (the probe's stored linear
	// depth) against this pixel's linear depth = -view_pos.z (positive in front).
	vec3 view_pos = reconstruct_view_position(pos, raw_depth);
	float pixel_linear_depth = -view_pos.z;
	vec3 world_N = normalize(mat3(params.inv_view) * view_normal);

	// Locate the 4 surrounding probes. Probe (gx, gy) anchors near tile center
	// (tile_origin + spacing/2), so a pixel's continuous probe-grid coordinate is
	// pf = (pos + 0.5) / spacing - 0.5; the lower-left corner is floor(pf) and `frac`
	// is the bilinear weight toward the upper-right. Clamp the base so base..base+1
	// stay in [0, grid-1] (mirrors the WRC's wrc_locate corner clamp).
	int grid_w = max(params.grid_w, 1);
	int grid_h = max(params.grid_h, 1);
	vec2 pf = (vec2(pos) + vec2(0.5)) / float(max(params.spacing_f, 1)) - vec2(0.5);
	ivec2 base = ivec2(floor(pf));
	vec2 frac = pf - vec2(base);
	base = clamp(base, ivec2(0), ivec2(grid_w - 2, grid_h - 2));
	base = max(base, ivec2(0)); // grid may be 1 wide/tall: keep base in [0, grid-1].

	vec3 out_irr = vec3(0.0);
	float wsum = 0.0;

	for (int cy = 0; cy < 2; cy++) {
		for (int cx = 0; cx < 2; cx++) {
			ivec2 probe = clamp(base + ivec2(cx, cy), ivec2(0), ivec2(grid_w - 1, grid_h - 1));

			vec4 plane = texelFetch(header_plane, probe, 0);
			if (plane.w <= 0.0) {
				continue; // Invalid probe (its whole tile was sky).
			}
			vec4 aux = texelFetch(header_aux, probe, 0);
			vec3 probe_N = oct_to_vec3(aux.xy * 2.0 - 1.0);

			// Bilinear weight of this corner from `frac`.
			float bw = ((cx == 0) ? (1.0 - frac.x) : frac.x) * ((cy == 0) ? (1.0 - frac.y) : frac.y);
			if (bw <= 0.0) {
				continue;
			}

			// Plane compatibility: reject probes on a different surface so a pixel only
			// gathers from probes that actually see its plane (mirrors the SPATIAL /
			// accumulate same-surface tests). Depth match is relative (scales with
			// distance); normal match is a hemisphere-ish cosine threshold.
			float depth_diff = abs(plane.w - pixel_linear_depth);
			if (depth_diff > 0.1 * plane.w) {
				continue;
			}
			if (dot(probe_N, world_N) < 0.5) {
				continue;
			}

			float cos_norm;
			vec3 probe_irr = integrate_probe(probe, probe_N, world_N, cos_norm);
			if (cos_norm <= 0.0) {
				continue; // No lit-hemisphere coverage for this probe.
			}

			out_irr += probe_irr * bw;
			wsum += bw;
		}
	}

	// Cosine-mean incident radiance (~= L on the furnace), bilinearly blended over the
	// compatible probes. When NO probe qualified (disocclusion / edge of grid) fall back
	// to vec3(0): a v1 best-probe fallback is intentionally omitted (zero is correct for
	// the furnace background and honest about missing data); .a flags the no-coverage
	// case for inspection. Scaled by the artistic strength knob (1.0 = raw, what the gate
	// reads).
	vec3 result = (wsum > 0.0) ? (out_irr / wsum) : vec3(0.0);
	imageStore(dest_image, pos, vec4(result * params.strength, (wsum > 0.0) ? 1.0 : 0.5));
}
