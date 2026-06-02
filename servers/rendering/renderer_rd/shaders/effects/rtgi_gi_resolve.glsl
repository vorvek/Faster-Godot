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

// Push constant: 20 x 4 B = 80 B (a multiple of 16, so the std430-rounded size matches
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
	float rough_cutoff; // INTEGRATE: roughness cutoff (T1 rough-spec gate). Unused by DEBUG_GI.
	uint rough_enabled; // INTEGRATE: 0 disables the rough-spec channel (writes spec 0).
	uint wrc_grid;
	uint wrc_cascade_count;
	float wrc_base_spacing;
	// DEBUG_GI channel select (T1): 0 = diffuse_gi only, 1 = spec_gi only, else = combined.
	// Unused by INTEGRATE. The 3 pad uints keep the block a 16-byte multiple (80 B).
	uint debug_channel;
	uint pad0;
	uint pad1;
	uint pad2;
}
pc;

// Set 0 declares every binding the implemented modes reference (a single GLSL shader's
// set-0 layout is the union over its reachable code paths; mirrors how
// rtgi_spg_accumulate.glsl declares its full layout). G-buffers (0-1), the material-guide
// albedo (2; 3 reserved for velocity in T2), SPG atlas + headers (4-6), WRC atlases (7-8),
// the GI write images (9-10), the GI read samplers (11-12), the debug dest image (13), the
// params UBO (14), and the material-guide ORM (15). INTEGRATE writes 9-10 and reads
// 0-2 + 4-8 + 15; DEBUG_GI reads 11-12 and writes 13.
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
// Material-guide albedo (A3-T1): rgb = base albedo, a = alpha. Populated by the "RTGI Material
// Guide Prepass" (RB_TEX_RT_GUIDE_ALBEDO). INTEGRATE reads it for the rough-spec F0 mix; the
// composite (T4/T5) reuses it for the diffuse remod. NOT the dead rt_albedo_metalness.
layout(set = 0, binding = 2) uniform sampler2D guide_albedo;
// NOTE: binding 3 is RESERVED for the velocity buffer (consumed by the TEMPORAL mode in
// T2). It is deliberately NOT declared in T0/T1: a sampler referenced by no reachable code
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
// A, .a = confidence. spec_gi_write: .rgb = rough-spec radiance (A3-T1; BRDF applied), .a = variance.
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

// Material-guide ORM (A3-T1): r = ao, g = roughness, b = metallic, a = sss (the packing the
// material guide / scene_forward_clustered.glsl writes). Populated by the "RTGI Material Guide
// Prepass" (RB_TEX_RT_GUIDE_ORM). INTEGRATE reads g (roughness) + b (metallic) for the
// rough-spec cone + F0; declared at binding 15 (after the UBO) to keep the existing 0-14
// numbering stable.
layout(set = 0, binding = 15) uniform sampler2D guide_orm;

// Split-sum environment-BRDF DFG approximation (Karis' analytic fit), returning the
// (scale, bias) pair so the specular term is F0 * dfg.x + dfg.y. This is the SAME analytic
// fit the engine already relies on in brdf_inc.glsl (DLSSRR_envBRDFApprox /
// energyConservingDiffuseFactor both use these exact c0/c1 coefficients); here we expose the
// raw (scale, bias) WITHOUT the DLSS-RR x2 scale (which DLSSRR_envBRDFApprox bakes in and is
// wrong for a standard split-sum -- see the energyConservingDiffuseFactor comment). The
// engine's other split-sum consumer, prefiltered_dfg() in scene_forward_clustered_inc.glsl,
// is a precomputed-LUT sampler that needs its own texture+UBO bindings, so it is not usable
// from this self-contained effect; this analytic fit matches it closely.
// Source: Brian Karis, "Physically Based Shading on Mobile"
// (https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile).
vec2 resolve_env_brdf_dfg(float NoV, float roughness) {
	const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	return vec2(-1.04, 1.04) * a004 + r.zw; // x = scale, y = bias.
}

// Energy-conserving multi-scatter specular albedo (Fdez-Aguera 2019, "A Multiple-Scattering
// Microfacet Model for Real-Time Image-Based Lighting"). Returns FssEss + FmsEms -- the
// single-scatter split-sum specular albedo PLUS the multi-scatter energy compensation that
// single-scatter GGX loses (large for rough high-F0/metal surfaces). The plain
// resolve_env_brdf_dfg term (F0 * scale + bias) is FssEss ALONE and under-lights rough metals;
// this lifts it back to energy conservation. Mirrors brdf_inc.glsl::energyConservingDiffuseFactor
// (same (scale,bias) AB fit, same Favg = F0 + (1-F0)/21, same max(...,1e-4) guard) but returns
// the SPECULAR part rather than its diffuse complement (1 - FssEss - FmsEms) -- C8-consistent.
// In the resolve F0 = mix(0.04, albedo, metalness) >= 0.04 always, so shadowedF90(F0) == 1.0
// (per the brdf_inc.glsl comment); the F90/bias term is therefore just AB.y (no shadowedF90 port).
vec3 resolve_env_brdf_specular(vec3 F0, float roughness, float NoV) {
	vec2 AB = resolve_env_brdf_dfg(NoV, roughness); // AB.x = scale, AB.y = bias (same Karis fit).
	vec3 FssEss = F0 * AB.x + AB.y; // single-scatter specular albedo (F90 == 1.0 here).
	float Ess = AB.x + AB.y; // white-furnace specular albedo (F0 = 1).
	float Ems = 1.0 - Ess; // energy missed by single scatter.
	vec3 Favg = F0 + (1.0 - F0) * (1.0 / 21.0);
	vec3 FmsEms = (Ems * FssEss * Favg) / max(1.0 - Favg * Ems, 1e-4);
	return FssEss + FmsEms; // energy-conserving (multi-scatter) specular albedo.
}

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

// Cone-prefilter one SPG probe's HEMISPHERE-octahedral radiance tile around the reflection
// direction `R` (A3-T1 rough-spec). Same tile addressing + local->world rotation as
// resolve_integrate_probe, but the per-texel weight is a GGX-lobe term toward R instead of a
// cosine toward the surface normal: a roughness-driven cone (narrow lobe at low roughness,
// widening toward the diffuse hemisphere at roughness 1). The lobe weight is the GGX NDF of
// the half-vector between R and the sampled direction, evaluated with alpha = roughness^2
// (Disney/UE remap). CONFIDENCE-WEIGHTED NORMALIZER (matches the diffuse integrate): both the
// numerator and the normalizer weight by lobe * rad.a, so rad.a cancels and unwritten texels
// (a == 0) are excluded. Returns the prefiltered radiance; `lobe_norm` reports the summed
// weight (0 == no usable texels). The split-sum BRDF (F0 * dfg.x + dfg.y) is applied by the
// caller -- this returns radiance only.
vec3 resolve_prefilter_probe(ivec2 probe, vec3 probe_N, vec3 R, float roughness, out float lobe_norm) {
	lobe_norm = 0.0;
	vec3 pref = vec3(0.0);

	// GGX NDF with the perceptual->linear roughness remap (alpha = roughness^2). alpha2 is
	// clamped off zero so a perfectly smooth surface still yields a finite, sharply-peaked
	// lobe rather than a divide-by-zero.
	float alpha = max(roughness * roughness, 1e-3);
	float alpha2 = alpha * alpha;

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
			// GGX lobe weight toward R: the half-vector between the reflection direction and the
			// sampled direction, scored by the GGX NDF (D). NoH = dot(R, H) since the lobe is
			// centered on R. Directions outside the lobe (or pointing away from R) get ~0 weight.
			// Guard the half-vector normalize against the near-antiparallel case (world_dir ~= -R,
			// where R + world_dir ~= 0 -> normalize NaN): such directions are opposite the lobe,
			// so skip them (their GGX weight is ~0 anyway).
			vec3 h = R + world_dir;
			float hlen2 = dot(h, h);
			if (hlen2 < 1e-8) {
				continue;
			}
			vec3 H = h * inversesqrt(hlen2);
			float NoH = max(dot(R, H), 0.0);
			float d = (alpha2 - 1.0) * NoH * NoH + 1.0;
			float lobe = alpha2 / max(d * d, 1e-8); // GGX D (PI-free; the constant cancels in the normalize).
			if (lobe <= 0.0) {
				continue;
			}
			vec4 rad = texelFetch(spg_radiance, tile_origin + ivec2(tx, ty), 0);
			float w = lobe * rad.a;
			pref += rad.rgb * w;
			lobe_norm += w;
		}
	}
	return (lobe_norm > 0.0) ? (pref / lobe_norm) : vec3(0.0);
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

	// Rough-spec view + reflection vectors (A3-T1). The camera world position is inv_view's
	// translation column (the same clipmap center the WRC params use). V points from the
	// surface toward the eye; R is the mirror reflection of -V about the surface normal.
	vec3 cam_pos = ubo.inv_view[3].xyz;
	vec3 V = normalize(cam_pos - world_pos);
	vec3 R = reflect(-V, world_N);

	// Per-pixel material guide (A3-T1): roughness (ORM.g) + metallic (ORM.b) drive the
	// rough-spec cone + F0; albedo (guide RGB) is the metallic tint of F0. The dead
	// rt_albedo_metalness is NOT used -- these come from the material-guide prepass.
	vec4 orm = texelFetch(guide_orm, pos, 0); // r=ao, g=roughness, b=metallic, a=sss.
	float rough = orm.g;
	float metalness = orm.b;
	vec3 albedo = texelFetch(guide_albedo, pos, 0).rgb;
	// Gate the rough-spec channel: only the rough domain (roughness >= cutoff) is resolved from
	// the diffuse-style probe octahedra; the sharp domain (below cutoff) is a separate mirror /
	// ray-traced reflection path (T-later) and stays 0 here. Also honors the global enable.
	bool do_spec = (pc.rough_enabled != 0u) && (rough >= pc.rough_cutoff);

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
	// Rough-spec prefiltered-radiance accumulator, bilinearly blended over the SAME qualifying
	// probes as the diffuse A (its own normalizer `spec_wsum` so a probe with diffuse coverage
	// but no spec-cone coverage does not bias the spec blend).
	vec3 spec_rad = vec3(0.0);
	float spec_wsum = 0.0;

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

			// Rough-spec cone-prefilter from the SAME probe tile, around R (A3-T1). Shares this
			// probe's plane/normal validation + bilinear weight; only accumulated when the spec
			// channel is active and the cone found usable (confidence-weighted) texels.
			if (do_spec) {
				float lobe_norm;
				vec3 probe_spec = resolve_prefilter_probe(probe, probe_N, R, rough, lobe_norm);
				if (lobe_norm > 0.0) {
					spec_rad += probe_spec * bw;
					spec_wsum += bw;
				}
			}
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

	// Rough-spec channel (A3-T1): apply the ENERGY-CONSERVING multi-scatter specular BRDF to the
	// prefiltered radiance. spec_gi is RADIANCE space (NO demod) -- the env-BRDF factor already
	// carries the BRDF, so the composite adds spec_gi directly (unlike diffuse A, which the
	// composite remods by albedo). The factor is FssEss + FmsEms (Fdez-Aguera 2019 multi-scatter),
	// not just the single-scatter F0*scale + bias: single-scatter GGX under-lights rough high-F0
	// (metal) surfaces, so this mirrors brdf_inc.glsl's energyConservingDiffuseFactor's specular
	// part (C8-consistent). For a rough metal under uniform Le it recovers ~= albedo * Le.
	vec3 spec = vec3(0.0);
	if (do_spec && spec_wsum > 0.0) {
		vec3 pref = spec_rad / spec_wsum; // prefiltered incident radiance around R.
		float NoV = max(dot(world_N, V), 1e-4);
		vec3 F0 = mix(vec3(0.04), albedo, metalness); // dielectric 4% -> albedo-tinted for metals.
		spec = pref * resolve_env_brdf_specular(F0, rough, NoV); // FssEss + FmsEms.
	}
	// .a = variance/confidence proxy (1.0 for now; T2/T3 fill a real variance).
	imageStore(spec_gi_write, pos, vec4(spec, 1.0));
}

// DEBUG_GI: output the resolve's RAW output -- the channel selected by pc.debug_channel
// (diffuse-space A, the rough-spec radiance, or their sum) -- written RAW (linear) for the
// blit. The .a keeps the diffuse confidence for inspection. NO albedo remodulation: the per-surface remod by
// albedo (L_o = albedo * A) is applied at the COMPOSITE (Hybrid/FPT in T4/T5), which runs in
// the full render where the G-buffer albedo genuinely exists -- the forced depth-prepass that
// this debug view triggers does NOT populate rt_albedo_metalness (it reads 0, which would
// black out the whole view). This mirrors rtgi_spg_gi_consumer.glsl's debug view, which also
// outputs raw incident radiance. On the furnace A ~= L (albedo-independent), so the energy
// gate passes. Reads BOTH the diffuse (11) and spec (12) GI buffers so both stay referenced.
void resolve_debug_gi_main(ivec2 pos) {
	// Read BOTH the diffuse (11) and spec (12) GI buffers so both stay referenced regardless of
	// the selected channel (an unreferenced sampler would be stripped from the reflected layout).
	vec4 diffuse = texelFetch(diffuse_gi_read, pos, 0);
	vec3 spec = texelFetch(spec_gi_read, pos, 0).rgb;
	// Channel select (A3-T1): 0 = diffuse-only (RESOLVE_GI view), 1 = spec-only (RESOLVE_SPEC
	// view), else = combined. Diffuse is lighting-space A (no albedo); spec is radiance-space
	// (BRDF already applied). Written RAW (linear) for the furnace gate.
	vec3 result;
	if (pc.debug_channel == 0u) {
		result = diffuse.rgb;
	} else if (pc.debug_channel == 1u) {
		result = spec;
	} else {
		result = diffuse.rgb + spec;
	}
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
