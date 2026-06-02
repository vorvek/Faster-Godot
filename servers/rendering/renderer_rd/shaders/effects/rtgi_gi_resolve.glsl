#[compute]

#version 450

#VERSION_DEFINES

// RTGI GI Resolve compute shader (A3-T0).
//
// The PRODUCTION per-pixel CONSUMER of the SPG/WRC probes (promoted from the A2
// debug-only consumer rtgi_spg_gi_consumer.glsl). It runs under the radiance_probes
// pipeline AFTER the Screen Probe Gather and resolves the screen GI into its own
// ping-pong buffers, BEFORE any beauty composite (the composite lands in T4/T5).
//
// Modes (match RESOLVE_MODE_* in rtgi_gi_resolve.cpp):
//   * INTEGRATE (0): per pixel -> 4 surrounding SPG probes, plane-weighted, cosine-
//     integrate the hemisphere-oct radiance against the surface normal (confidence-
//     weighted normalizer), bilinearly blend, fall back to the WRC irradiance when no
//     probe qualifies. Writes LIGHTING-SPACE A (the confidence-weighted cosine-average
//     of incident radiance) to diffuse_gi_write -- NO albedo, NO extra 1/PI (the demod
//     is PI-free at storage; the surface adds L_o = albedo * A). spec_gi_write is 0 (T1).
//   * TEMPORAL (1) / SPATIAL (2): declared for numbering stability; implemented in T2/T3.
//   * DEBUG_GI (3): output the resolve's RAW lighting-space output -> out = diffuse_gi.rgb +
//     spec_gi.rgb (spec is 0 in T0), written RAW (linear) for the furnace gate. NO albedo:
//     remodulation by the pixel albedo belongs at the COMPOSITE (T4/T5), where the full
//     G-buffer albedo exists (the forced depth-prepass debug-view path does not populate
//     rt_albedo_metalness). On the furnace A ~= L (albedo-independent), matching the A2
//     SPG-GI gate (rtgi_spg_gi_consumer.glsl, which likewise outputs raw incident radiance).
//
// Coordinate-space contract (verified against rtgi_spg_gi_consumer.glsl +
// rtgi_screen_probe_gather.glsl):
//   * depth_buffer holds RAW reverse-Z hyperbolic depth; the cleared far/sky value is
//     0.0. View position is inv_projection * vec4(2*uv-1, depth, 1) (homogeneous divide).
//   * The normal-roughness G-buffer normal is in VIEW space (normalize(rgb*2-1)); we
//     rotate it VIEW->WORLD via mat3(inv_view).
//   * inv_view == the camera's view->world transform, so world pos is inv_view * view_pos
//     (full affine) and world normal is mat3(inv_view) * view_normal. The pixel linear
//     depth used for the probe plane match is -view_pos.z (positive in front), matching
//     header_plane.w (the probe's stored linear depth).

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Shared SPG hemi-oct basis math (local +Z = anchor normal): spg_build_basis,
// spg_hemioct_decode, spg_local_to_world. Same include the PLACE/gather/accumulate
// passes use, so the local->world tile orientation here matches how the atlas was written.
#include "../raytracing/rtgi_spg_inc.glsl"
// The WRC query API (cosine-integrated irradiance) for the no-probe fallback, plus the
// octahedral + clipmap math it needs. Binding-agnostic: takes atlas samplers + WrcParams
// as plain args, so this shader owns its own descriptor-set layout.
#include "../raytracing/rtgi_wrc_inc.glsl"
// Full-sphere octahedral decode (oct_to_vec3) for the SPG probe header normal, which the
// PLACE pass stored via vec3_to_oct of the anchor WORLD normal.
#include "../oct_inc.glsl"

// Mode selectors (match RESOLVE_MODE_* in rtgi_gi_resolve.cpp).
#define RESOLVE_MODE_INTEGRATE 0u
#define RESOLVE_MODE_TEMPORAL 1u
#define RESOLVE_MODE_SPATIAL 2u
#define RESOLVE_MODE_DEBUG_GI 3u

// Push constant: 16 x 4 B = 64 B (a multiple of 16, so the std430-rounded size matches
// the C++ PushConstant exactly -- a mismatch silently rejects every dispatch). Matches
// RTGIGIResolve::PushConstant field-for-field.
layout(push_constant, std430) uniform Params {
	uint mode;
	uint frame_index;
	uint screen_w;
	uint screen_h;
	uint spatial_iter;
	uint cur_iter;
	uint spg_grid_w;
	uint spg_grid_h;
	uint spg_oct_res;
	uint spg_spacing_f;
	float temporal_n_cap;
	float rough_cutoff; // INTEGRATE: roughness cutoff (deferred to T1). Unused by DEBUG_GI.
	uint rough_enabled;
	uint wrc_grid;
	uint wrc_cascade_count;
	float wrc_base_spacing;
}
pc;

// Set 0 declares every binding the implemented modes reference (a single GLSL shader's
// set-0 layout is the union over its reachable code paths; mirrors how
// rtgi_spg_accumulate.glsl declares its full layout). G-buffers (0-1; 2 free, 3 reserved
// for velocity in T2), SPG atlas + headers (4-6), WRC atlases (7-8), the GI write images
// (9-10), the GI read samplers (11-12), the debug dest image (13), and the params UBO
// (14). INTEGRATE writes 9-10 and reads 0-1 + 4-8; DEBUG_GI reads 11-12 and writes 13.
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
// NOTE: binding 2 is now FREE (the albedo sampler was removed: no reachable mode references
// it -- INTEGRATE never did, and DEBUG_GI no longer remodulates by albedo; a declared-but-
// unreferenced sampler is stripped from the reflected set layout and would mismatch the C++
// uniform set). The composite (T4) re-uses binding 2 for the G-buffer albedo at remod time.
// NOTE: binding 3 is RESERVED for the velocity buffer (consumed by the TEMPORAL mode in
// T2). It is deliberately NOT declared in T0: a sampler referenced by no reachable code
// path is stripped from the reflected set layout, which would make the C++ uniform set
// (which must match the reflected layout) reject a binding-3 uniform. T2 adds the
// declaration + its texelFetch and binds it. The C++ run_resolve already carries p_velocity.
// SPG SPATIAL-filtered per-probe radiance atlas (grid_w*oct_res x grid_h*oct_res): each
// probe owns an oct_res x oct_res HEMISPHERE-octahedral tile (local +Z = anchor normal),
// .rgb = incident radiance, .a = confidence. header_plane: .xyz = anchor WORLD position,
// .w = linear depth (<= 0 invalid). header_aux: .xy = oct-normal (vec3_to_oct of the
// anchor WORLD normal), .zw = motion.
layout(set = 0, binding = 4) uniform sampler2D spg_radiance;
layout(set = 0, binding = 5) uniform sampler2D spg_header_plane;
layout(set = 0, binding = 6) uniform sampler2D spg_header_aux;
// WRC atlases for the fallback irradiance query (RGBA16F radiance, RG16F distance moments).
layout(set = 0, binding = 7) uniform sampler2D wrc_radiance;
layout(set = 0, binding = 8) uniform sampler2D wrc_distance;
// Screen-GI output images (INTEGRATE writes these). diffuse_gi_write: .rgb = lighting-space
// A, .a = confidence. spec_gi_write: .rgb = rough-spec radiance (0 in T0), .a = variance.
layout(set = 0, binding = 9, rgba16f) uniform restrict writeonly image2D diffuse_gi_write;
layout(set = 0, binding = 10, rgba16f) uniform restrict writeonly image2D spec_gi_write;
// Resolved screen-GI read samplers (DEBUG_GI reads these).
layout(set = 0, binding = 11) uniform sampler2D diffuse_gi_read;
layout(set = 0, binding = 12) uniform sampler2D spec_gi_read;
// Debug dest image (DEBUG_GI writes the raw lighting-space linear value here for the blit).
layout(set = 0, binding = 13, rgba16f) uniform restrict writeonly image2D dest_image;

// Two mat4s (128 bytes) already hit RenderingDevice's push-constant cap, so the
// reconstruction matrices live in a UBO (uncapped). Layout matches RTGIGIResolve::GiResolveUBO
// exactly: two 16-byte-aligned mat4s at offsets 0 and 64.
layout(set = 0, binding = 14, std140) uniform GiResolveUBO {
	mat4 inv_projection; // clip -> view.
	mat4 inv_view; // view -> world (camera transform).
}
ubo;

// Reconstruct VIEW-space position from the raw depth buffer at integer pixel `pos`.
// Mirrors rtgi_spg_gi_consumer.glsl::reconstruct_view_position: build a clip-space point
// from the pixel's NDC xy + raw depth z and run it through inv_projection.
vec3 resolve_reconstruct_view_position(ivec2 pos, float raw_depth) {
	vec4 clip;
	clip.xy = (2.0 * (vec2(pos) + vec2(0.5)) / vec2(pc.screen_w, pc.screen_h)) - 1.0;
	clip.z = raw_depth;
	clip.w = 1.0;
	vec4 view = ubo.inv_projection * clip;
	return view.xyz / view.w;
}

// Build the binding-agnostic WrcParams for the fallback query. Mirrors the A2 WRC
// consumer's GiDebugUBO source: cascade/grid/base_spacing from the push, the clipmap
// center (camera_pos) from inv_view's translation column (== cam_transform.origin, the
// clipmap center the atlas was built around), and the sane default occlusion-bias /
// min-variance the WRC consumer used. The WRC tile oct_res is not carried separately in
// the T0 push, so it reuses spg_oct_res (both atlases use the same octahedral resolution
// under the project defaults); revisit if the two ever diverge.
WrcParams resolve_wrc_params() {
	WrcParams wp;
	wp.cascade_count = int(max(pc.wrc_cascade_count, 1u));
	wp.grid = int(max(pc.wrc_grid, 1u));
	wp.oct_res = int(max(pc.spg_oct_res, 1u));
	wp.base_spacing = pc.wrc_base_spacing;
	wp.camera_pos = ubo.inv_view[3].xyz;
	wp.occlusion_bias_spacing = 0.5; // sane default per rtgi_wrc_inc.glsl WrcParams docs.
	wp.min_variance = 0.0001;
	return wp;
}

// Cosine-integrate one SPG probe's HEMISPHERE-octahedral radiance tile against the
// world-space surface normal `world_N`. The tile is oriented in the probe's OWN anchor
// basis (local +Z = probe_N), so each local hemioct direction is rotated to world via
// that basis before the dot with world_N. CONFIDENCE-WEIGHTED NORMALIZER (verbatim from
// rtgi_spg_gi_consumer.glsl::integrate_probe): numerator and denominator both weight by
// ndl * rad.a so rad.a cancels -- unwritten texels (a == 0) are excluded and partial
// coverage still yields the true cosine-mean radiance (~= L). Reports lit-hemisphere
// coverage in `cos_norm` (0 == no usable texels).
vec3 resolve_integrate_probe(ivec2 probe, vec3 probe_N, vec3 world_N, out float cos_norm) {
	cos_norm = 0.0;
	vec3 irr = vec3(0.0);

	int res = int(max(pc.spg_oct_res, 1u));
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
			vec4 rad = texelFetch(spg_radiance, tile_origin + ivec2(tx, ty), 0);
			irr += rad.rgb * (ndl * rad.a);
			cos_norm += ndl * rad.a;
		}
	}
	return (cos_norm > 0.0) ? (irr / cos_norm) : vec3(0.0);
}

// INTEGRATE: per pixel -> 4 surrounding SPG probes, plane-weighted, cosine-integrate the
// hemisphere-oct -> lighting-space diffuse A (confidence-weighted). NO albedo here.
void resolve_integrate_main(ivec2 pos) {
	// Raw reverse-Z depth. The cleared far/sky value is 0.0 -> no geometry; write 0 (the
	// gate expects vec3(0) for background pixels, and no probe contributes there).
	float raw_depth = texelFetch(depth_buffer, pos, 0).r;
	if (raw_depth <= 0.0) {
		imageStore(diffuse_gi_write, pos, vec4(0.0));
		imageStore(spec_gi_write, pos, vec4(0.0));
		return;
	}

	// View-space normal from the G-buffer (normalize(rgb*2-1)); a zero/degenerate normal
	// (cleared texel with no geometry) means "no surface" -> output 0.
	vec3 enc = texelFetch(normal_roughness_buffer, pos, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.0001) {
		imageStore(diffuse_gi_write, pos, vec4(0.0));
		imageStore(spec_gi_write, pos, vec4(0.0));
		return;
	}
	view_normal = normalize(view_normal);

	// VIEW -> WORLD. inv_view is the camera's view->world transform, so a position uses the
	// full affine (rotation + translation) and a normal uses the 3x3.
	vec3 view_pos = resolve_reconstruct_view_position(pos, raw_depth);
	vec3 world_pos = (ubo.inv_view * vec4(view_pos, 1.0)).xyz;
	float pixel_linear_depth = -view_pos.z;
	vec3 world_N = normalize(mat3(ubo.inv_view) * view_normal);

	// Locate the 4 surrounding probes. Probe (gx, gy) anchors near tile center
	// (tile_origin + spacing/2), so a pixel's continuous probe-grid coordinate is
	// pf = (pos + 0.5) / spacing - 0.5; the lower-left corner is floor(pf) and `frac` is
	// the bilinear weight toward the upper-right. Clamp the base so base..base+1 stay in
	// [0, grid-1] (verbatim from rtgi_spg_gi_consumer.glsl).
	int grid_w = int(max(pc.spg_grid_w, 1u));
	int grid_h = int(max(pc.spg_grid_h, 1u));
	vec2 pf = (vec2(pos) + vec2(0.5)) / float(max(pc.spg_spacing_f, 1u)) - vec2(0.5);
	ivec2 base = ivec2(floor(pf));
	vec2 frac = pf - vec2(base);
	base = clamp(base, ivec2(0), ivec2(grid_w - 2, grid_h - 2));
	base = max(base, ivec2(0)); // grid may be 1 wide/tall: keep base in [0, grid-1].

	vec3 A = vec3(0.0);
	float wsum = 0.0;

	for (int cy = 0; cy < 2; cy++) {
		for (int cx = 0; cx < 2; cx++) {
			ivec2 probe = clamp(base + ivec2(cx, cy), ivec2(0), ivec2(grid_w - 1, grid_h - 1));

			vec4 plane = texelFetch(spg_header_plane, probe, 0);
			if (plane.w <= 0.0) {
				continue; // Invalid probe (its whole tile was sky).
			}
			vec4 aux = texelFetch(spg_header_aux, probe, 0);
			vec3 probe_N = oct_to_vec3(aux.xy * 2.0 - 1.0);

			// Bilinear weight of this corner from `frac`.
			float bw = ((cx == 0) ? (1.0 - frac.x) : frac.x) * ((cy == 0) ? (1.0 - frac.y) : frac.y);
			if (bw <= 0.0) {
				continue;
			}

			// Plane compatibility: reject probes on a different surface (relative depth +
			// hemisphere-ish normal cosine), verbatim from the A2 consumer.
			float depth_diff = abs(plane.w - pixel_linear_depth);
			if (depth_diff > 0.1 * plane.w) {
				continue;
			}
			if (dot(probe_N, world_N) < 0.5) {
				continue;
			}

			float cos_norm;
			vec3 probe_irr = resolve_integrate_probe(probe, probe_N, world_N, cos_norm);
			if (cos_norm <= 0.0) {
				continue; // No lit-hemisphere coverage for this probe.
			}

			A += probe_irr * bw;
			wsum += bw;
		}
	}

	// Fallback: all 4 probes failed (disocclusion / grid edge) -> direct WRC irradiance
	// (already returns the cosine-average A). Mirrors the production consumer's fallback;
	// the A2 debug consumer returned 0 here, but the production resolve has the WRC to lean on.
	if (wsum <= 0.0) {
		float dconf;
		A = rtgi_wrc_sample_irradiance(wrc_radiance, wrc_distance, resolve_wrc_params(), world_pos, world_N, dconf);
	} else {
		A /= wsum;
	}

	imageStore(diffuse_gi_write, pos, vec4(A, 1.0)); // lighting space; .a = confidence (1.0 = resolved).
	imageStore(spec_gi_write, pos, vec4(0.0)); // T1 fills rough-spec.
}

// DEBUG_GI: output the resolve's RAW lighting-space output -- the lighting-space diffuse A
// plus the rough-spec radiance (0 in T0) -- written RAW (linear) for the blit. The .a keeps
// the diffuse confidence for inspection. NO albedo remodulation: the per-surface remod by
// albedo (L_o = albedo * A) is applied at the COMPOSITE (Hybrid/FPT in T4/T5), which runs in
// the full render where the G-buffer albedo genuinely exists -- the forced depth-prepass that
// this debug view triggers does NOT populate rt_albedo_metalness (it reads 0, which would
// black out the whole view). This mirrors rtgi_spg_gi_consumer.glsl's debug view, which also
// outputs raw incident radiance. On the furnace A ~= L (albedo-independent), so the energy
// gate passes. Reads BOTH the diffuse (11) and spec (12) GI buffers so both stay referenced.
void resolve_debug_gi_main(ivec2 pos) {
	vec4 diffuse = texelFetch(diffuse_gi_read, pos, 0);
	vec3 spec = texelFetch(spec_gi_read, pos, 0).rgb;
	vec3 result = diffuse.rgb + spec; // raw lighting-space resolve output (spec 0 in T0).
	imageStore(dest_image, pos, vec4(result, diffuse.a)); // .a = diffuse confidence (inspection).
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= int(pc.screen_w) || pos.y >= int(pc.screen_h)) {
		return;
	}

	if (pc.mode == RESOLVE_MODE_INTEGRATE) {
		resolve_integrate_main(pos);
		return;
	}
	if (pc.mode == RESOLVE_MODE_DEBUG_GI) {
		resolve_debug_gi_main(pos);
		return;
	}

	// TEMPORAL (1) / SPATIAL (2) land in T2/T3; this shader runs only INTEGRATE + DEBUG_GI.
	return;
}
