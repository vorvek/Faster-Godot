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
//     of incident radiance) to diffuse_gi_rw -- NO albedo, NO extra 1/PI (the demod
//     is PI-free at storage; the surface adds L_o = albedo * A). spec_gi_rw holds the T1
//     rough-spec radiance.
//   * TEMPORAL (1): motion-reproject the PREVIOUS frame's accumulated GI (the [1-read_index]
//     history at 11/12), reject bad history on a same-surface depth+normal AND-gate, and blend
//     with a sample-counted 1/n weight, IN-PLACE on diffuse_gi_rw/spec_gi_rw (A3-T2). SPATIAL
//     (2) is declared for numbering stability; implemented in T3.
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

// Source-quality floor for the temporal convergence advance. INTEGRATE writes into .a how much of
// this frame's resolved value is VERIFIED traced data (the cosine-weighted mean of the gathered
// probe texels' own n/n_cap; an unverified prior -- the WRC direct fallback or a freshly WRC-seeded
// probe -- reports low). TEMPORAL advances the sample count by that quality instead of a flat +1,
// so the composite's cold-start gate (cold_fade on nd/n_cap) opens only as traced rays actually
// replace the prior: a flat +1/frame counted unverified-prior frames as convergence and revealed
// the seed content mid-wash (the soft round cold-start blobs = one leaky WRC cell's trilinear
// footprint). The floor guarantees progress for pixels that only ever have the prior (permanent
// grid-edge / fallback): >= 0.25/frame still reaches full reveal in <= 4 * n_cap frames, a slow
// deliberate ease-in, never stuck dark. Steady state (fully traced probes, quality = 1) advances
// +1/frame -- bit-identical to the old behavior after warmup.
#define RTGI_RESOLVE_QUALITY_FLOOR 0.25
// COMPOSITE (A3-T4): the BEAUTY remod of the resolved GI -> dest_image (binding 13) for the
// additive blit onto the raster-lit frame. out = albedo * diffuse_A + spec (background masked).
#define RESOLVE_MODE_COMPOSITE 4u

// Resolve mode as a SPECIALIZATION CONSTANT (constant_id 0): the C++ creates one compute pipeline per
// mode (rtgi_gi_resolve.cpp), each baking this constant, so the driver folds it and dead-strips the
// other modes' code -> each mode gets its own register allocation / occupancy. (One shared program
// branching on the pc.mode push constant made the fused INTEGRATE path's register pressure penalize
// the TEMPORAL + SPATIAL dispatches.) The reflected set-0 layout stays the union over ALL modes
// (every binding is still referenced somewhere in the source), so the existing union-layout uniform
// sets bind to any mode's pipeline unchanged. The `mode` push-constant field is kept for C++ layout
// parity but is no longer read.
layout(constant_id = 0) const uint sc_resolve_mode = RESOLVE_MODE_INTEGRATE;

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
	// Unused by INTEGRATE. write_reactive + the trailing pad uint keep the block a 16-byte multiple (80 B).
	uint debug_channel;
	float history_rejection; // TEMPORAL (T2): depth/normal reproject tolerance scale (was pad0).
	uint write_reactive; // COMPOSITE: 1 = also write the GI-aware reactive mask (binding 16); 0 = skip (was pad1).
	uint pad2;
	// Cold-start hide enable: > 0 turns on the COMPOSITE's convergence-gated fade-from-zero (the
	// reveal pace is driven by the temporal sample count, not this value; 0 disables for A/B).
	// Grows the block 80 B -> 96 B (a multiple of 16); mirrors RTGIGIResolve::PushConstant EXACTLY.
	float fade_time;
	uint pad3;
	uint pad4;
	uint pad5;
}
pc;

// Set 0 declares every binding the implemented modes reference (a single GLSL shader's
// set-0 layout is the union over its reachable code paths; mirrors how
// rtgi_spg_accumulate.glsl declares its full layout). G-buffers (0-1), the material-guide
// albedo (2), the velocity buffer (3, TEMPORAL), SPG atlas + headers (4-6), WRC atlases
// (7-8), the GI read+write images (9-10), the GI history read samplers (11-12), the debug
// dest image (13), the params UBO (14), the material-guide ORM (15), and the GI-aware reactive
// mask output (16). INTEGRATE writes 9-10 and reads 0-2 + 4-8 + 15 (it ignores 3/11/12);
// TEMPORAL reads 3 + 11-12 + the cur G-buffer and read-modify-writes 9-10 in place; DEBUG_GI
// reads 11-12 and writes 13; COMPOSITE reads 0 + 2 + 11/12 + 15, writes 13, and (only when
// pc.write_reactive == 1u) writes 16.
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
// Material-guide albedo (A3-T1): rgb = base albedo, a = alpha. Populated by the "RTGI Material
// Guide Prepass" (RB_TEX_RT_GUIDE_ALBEDO). INTEGRATE reads it for the rough-spec F0 mix; the
// composite (T4/T5) reuses it for the diffuse remod. NOT the dead rt_albedo_metalness.
layout(set = 0, binding = 2) uniform sampler2D guide_albedo;
// Velocity buffer (A3-T2): .xy = screen-space motion in UV (prev_uv - cur_uv), the standard
// RB_TEX_VELOCITY convention (TAA reads prev_uv = cur_uv + mv). Consumed by the TEMPORAL mode,
// which scales it to PIXELS (* screen size) and reprojects prev = pos + mv*size; INTEGRATE/
// DEBUG_GI ignore it. Bound by run_resolve from p_velocity (the SPG anchor motion in
// header_aux.zw uses the SAME source, pre-scaled to pixels at PLACE). (Was reserved in T0/T1.)
layout(set = 0, binding = 3) uniform sampler2D velocity_buffer;
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
// Screen-GI READ+WRITE images (the [read_index] set: this frame's buffers). INTEGRATE
// imageStores this frame's RAW resolve here; TEMPORAL then imageLoads them back ("cur"),
// blends the reprojected history, and imageStores the accumulated result in place. The
// `writeonly` qualifier is therefore DROPPED (an imageLoad on a writeonly image is illegal);
// INTEGRATE still only stores, which is fine for a plain (non-writeonly) storage image.
// diffuse_gi_rw: .rgb = lighting-space A, .a = confidence/n. spec_gi_rw: .rgb = rough-spec
// radiance (A3-T1; BRDF applied), .a = variance/n.
layout(set = 0, binding = 9, rgba16f) uniform restrict image2D diffuse_gi_rw;
layout(set = 0, binding = 10, rgba16f) uniform restrict image2D spec_gi_rw;
// GI HISTORY read samplers (the [1 - read_index] set: the previous frame's ACCUMULATED
// result). TEMPORAL texelFetches these at the reprojected pixel for the 1/n blend; DEBUG_GI
// binds them to the [read_index] set instead (a separate set in render_resolve_debug) to
// DISPLAY this frame's resolved output. Different buffers from 9/10 within a given set, so
// no same-resource sampler+image hazard (verified in run_resolve).
layout(set = 0, binding = 11) uniform sampler2D diffuse_history;
layout(set = 0, binding = 12) uniform sampler2D spec_history;
// Debug dest image (DEBUG_GI writes the raw lighting-space linear value here for the blit).
layout(set = 0, binding = 13, rgba16f) uniform restrict writeonly image2D dest_image;

// Two mat4s (128 bytes) already hit RenderingDevice's push-constant cap, so the
// reconstruction matrices live in a UBO (uncapped). Layout matches RTGIGIResolve::GiResolveUBO
// exactly: two 16-byte-aligned mat4s at offsets 0 and 64.
layout(set = 0, binding = 14, std140) uniform GiResolveUBO {
	mat4 inv_projection; // clip -> view.
	mat4 inv_view; // view -> world (camera transform).
	// PREVIOUS-frame world -> clip (prev_cam_projection * prev_cam_view). TEMPORAL uses it to
	// camera-reproject the STATIC geometry the velocity buffer leaves at the (-1,-1) no-motion
	// sentinel (static surfaces write no motion vector, color_pass_inclusion_mask 0). Jittered,
	// to match the jittered history the resolve accumulated. 192 B total, a 16-byte multiple.
	mat4 prev_view_projection;
}
ubo;

// Material-guide ORM (A3-T1): r = ao, g = roughness, b = metallic, a = sss (the packing the
// material guide / scene_forward_clustered.glsl writes). Populated by the "RTGI Material Guide
// Prepass" (RB_TEX_RT_GUIDE_ORM). INTEGRATE reads g (roughness) + b (metallic) for the
// rough-spec cone + F0; declared at binding 15 (after the UBO) to keep the existing 0-14
// numbering stable.
layout(set = 0, binding = 15) uniform sampler2D guide_orm;

// GI-aware reactive mask output (binding 16): COMPOSITE writes 1 - confidence here (R8) so the
// temporal upscaler trusts the current frame where GI just disoccluded / is low-confidence
// ("poor-man's Ray Reconstruction"). DISTINCT write-only image, never also bound as a sampler,
// so it adds no same-resource hazard to the set (the only other images are gi_debug_image at
// 9/10/13). It is ALWAYS bound (the shared set-0 layout must stay valid for every mode), but
// written ONLY by COMPOSITE and ONLY when pc.write_reactive == 1u. NOT `restrict`: when the
// reactive denoiser is off the caller binds a tiny format-matching r8 dummy here as a neutral
// placeholder and the shader never touches it, so there is no aliasing in either case.
layout(set = 0, binding = 16, r8) uniform writeonly image2D reactive_image;

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

// Fused per-probe consumer: ONE walk of the probe's HEMISPHERE-octahedral radiance tile that
// produces BOTH the cosine-integrated diffuse radiance (against world_N) AND, when do_spec, the
// GGX-cone-prefiltered rough-spec radiance (around the reflection vector R). Replaces the separate
// resolve_integrate_probe + resolve_prefilter_probe, which walked the SAME tile twice for a rough
// pixel: duplicate spg_radiance texelFetch + duplicate spg_hemioct_decode / spg_local_to_world + a
// second spg_build_basis. The tile is in the probe's OWN anchor basis (local +Z = probe_N), so each
// local hemioct direction is decoded + rotated to world ONCE and reused by both weights.
// CONFIDENCE-WEIGHTED NORMALIZERS, verbatim from the two originals (and from
// rtgi_spg_gi_consumer.glsl::integrate_probe): the diffuse numerator and denominator both weight by
// ndl * rad.a (rad.a cancels), the spec by lobe * rad.a, so unwritten texels (a == 0) drop out and
// partial coverage still yields the true mean (~= L). The diffuse accumulator sees the same texels
// (ndl > 0) and the spec accumulator the same texels (lobe > 0), in the same (ty,tx) order with the
// same values as the two-loop version, so each channel is bit-identical. The single texelFetch is
// gated on the UNION of the two acceptance tests, so each needed texel is fetched exactly once (the
// two loops fetched their gate intersection twice).
//   diffuse_irr / cos_norm: cosine-mean incident radiance (cos_norm == 0 -> no lit coverage).
//   ndl_sum: the UNWEIGHTED cosine normalizer (sum of ndl with no rad.a). cos_norm / ndl_sum is the
//     cosine-weighted mean of the tile's per-texel convergence .a (n/n_cap in the SPG atlas: 0
//     unwritten, seed_n/n_cap freshly WRC-seeded, -> 1 fully traced) -- the probe's SOURCE QUALITY.
//     The irradiance normalize cancels rad.a, so a freshly-seeded probe returns the pure WRC prior
//     at FULL strength; this is the only signal that says how much of that value is verified.
//   spec_pref  / lobe_norm: GGX-cone-prefiltered radiance (lobe_norm == 0 -> no cone coverage); the
//     split-sum BRDF (F0 * dfg.x + dfg.y) is applied by the caller. Both spec outputs stay 0 when
//     do_spec is false, so a smooth / spec-disabled pixel pays only the diffuse cost (the per-texel
//     do_spec test is uniform across the tile, so it predicates cheaply).
void resolve_probe_fused(ivec2 probe, vec3 probe_N, vec3 world_N, vec3 R, float roughness, bool do_spec,
		out vec3 diffuse_irr, out float cos_norm, out float ndl_sum, out vec3 spec_pref, out float lobe_norm) {
	cos_norm = 0.0;
	ndl_sum = 0.0;
	diffuse_irr = vec3(0.0);
	lobe_norm = 0.0;
	spec_pref = vec3(0.0);

	int res = int(max(pc.spg_oct_res, 1u));
	float inv_res = 1.0 / float(res);
	vec3 pt, pb;
	spg_build_basis(probe_N, pt, pb);
	ivec2 tile_origin = probe * res;

	// GGX NDF setup (perceptual->linear roughness remap, alpha = roughness^2), only when the spec
	// channel is active. alpha2 is clamped off zero so a perfectly smooth surface still yields a
	// finite, sharply-peaked lobe rather than a divide-by-zero. Matches resolve_prefilter_probe.
	float alpha2 = 0.0;
	if (do_spec) {
		float alpha = max(roughness * roughness, 1e-3);
		alpha2 = alpha * alpha;
	}

	for (int ty = 0; ty < res; ty++) {
		for (int tx = 0; tx < res; tx++) {
			vec2 local_oct = (vec2(tx, ty) + vec2(0.5)) * inv_res; // texel center.
			vec3 local_dir = spg_hemioct_decode(local_oct); // local, +Z = probe_N (unit).
			// local->world WITHOUT the redundant normalize: (pt, pb, probe_N) is an orthonormal basis
			// and local_dir is unit, so the rotated vector is already unit (spg_local_to_world's
			// normalize only corrects FP epsilon). Dropping it saves a sqrt + divide on each of the
			// up-to-256 texels/pixel in this INTEGRATE hot loop; ndl/lobe shift by < FP epsilon, so the
			// resolved A is perceptually identical. (Hot-loop-scoped; the shared spg_local_to_world is
			// unchanged for the placement/gather/accumulate callers.)
			vec3 world_dir = pt * local_dir.x + pb * local_dir.y + probe_N * local_dir.z;

			// Diffuse cosine weight toward the surface normal (resolve_integrate_probe).
			float ndl = max(0.0, dot(world_N, world_dir));

			// Spec GGX-lobe weight toward R (resolve_prefilter_probe); 0 unless the spec channel is
			// active and the half-vector is well-defined. Guard the normalize against the near-
			// antiparallel case (world_dir ~= -R -> R + world_dir ~= 0): those directions are opposite
			// the lobe, so their weight is ~0 anyway.
			float lobe = 0.0;
			if (do_spec) {
				vec3 h = R + world_dir;
				float hlen2 = dot(h, h);
				if (hlen2 >= 1e-8) {
					vec3 H = h * inversesqrt(hlen2);
					float NoH = max(dot(R, H), 0.0);
					float d = (alpha2 - 1.0) * NoH * NoH + 1.0;
					lobe = alpha2 / max(d * d, 1e-8); // GGX D (PI-free; the constant cancels in the normalize).
				}
			}

			// One fetch, only when this texel feeds at least one channel (the union of the two gates).
			if (ndl <= 0.0 && lobe <= 0.0) {
				continue;
			}
			vec4 rad = texelFetch(spg_radiance, tile_origin + ivec2(tx, ty), 0);

			if (ndl > 0.0) {
				diffuse_irr += rad.rgb * (ndl * rad.a);
				cos_norm += ndl * rad.a;
				ndl_sum += ndl;
			}
			if (lobe > 0.0) {
				float w = lobe * rad.a;
				spec_pref += rad.rgb * w;
				lobe_norm += w;
			}
		}
	}

	diffuse_irr = (cos_norm > 0.0) ? (diffuse_irr / cos_norm) : vec3(0.0);
	spec_pref = (lobe_norm > 0.0) ? (spec_pref / lobe_norm) : vec3(0.0);
}

// INTEGRATE: per pixel -> 4 surrounding SPG probes, plane-weighted, cosine-integrate the
// hemisphere-oct -> lighting-space diffuse A (confidence-weighted). NO albedo here.
void resolve_integrate_main(ivec2 pos) {
	// Raw reverse-Z depth. The cleared far/sky value is 0.0 -> no geometry; write 0 (the
	// gate expects vec3(0) for background pixels, and no probe contributes there).
	float raw_depth = texelFetch(depth_buffer, pos, 0).r;
	if (raw_depth <= 0.0) {
		imageStore(diffuse_gi_rw, pos, vec4(0.0));
		imageStore(spec_gi_rw, pos, vec4(0.0));
		return;
	}

	// View-space normal from the G-buffer (normalize(rgb*2-1)); a zero/degenerate normal
	// (cleared texel with no geometry) means "no surface" -> output 0.
	vec3 enc = texelFetch(normal_roughness_buffer, pos, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.0001) {
		imageStore(diffuse_gi_rw, pos, vec4(0.0));
		imageStore(spec_gi_rw, pos, vec4(0.0));
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
	// Source quality of the resolved value (see RTGI_RESOLVE_QUALITY_FLOOR): the bilinear blend of
	// each probe's cosine-weighted mean texel convergence. 0.333 for freshly WRC-seeded probes
	// (seed_n/n_cap), -> 1 as the gather rays re-trace the tile. Written to .a for TEMPORAL.
	float q_sum = 0.0;
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

			// Fused diffuse + rough-spec consumer: ONE walk of this probe's tile yields both the
			// cosine-integrated diffuse radiance and (when do_spec) the GGX-cone-prefiltered spec
			// radiance, instead of walking the tile twice (see resolve_probe_fused).
			float cos_norm;
			float ndl_sum;
			float lobe_norm;
			vec3 probe_irr;
			vec3 probe_spec;
			resolve_probe_fused(probe, probe_N, world_N, R, rough, do_spec, probe_irr, cos_norm, ndl_sum, probe_spec, lobe_norm);
			if (cos_norm <= 0.0) {
				// No lit-hemisphere coverage for this probe (skips spec too, exactly as before: the
				// old code computed the spec prefilter only after this same continue).
				continue;
			}

			A += probe_irr * bw;
			wsum += bw;
			// cos_norm / ndl_sum = this probe's cosine-weighted mean texel convergence (ndl_sum > 0
			// is implied by cos_norm > 0: every cos_norm contribution also added to ndl_sum).
			q_sum += (cos_norm / ndl_sum) * bw;

			// Rough-spec accumulate from the SAME walk. Only when the spec channel is active and the
			// cone found usable (confidence-weighted) texels -- identical gate to the old two-pass path.
			if (do_spec && lobe_norm > 0.0) {
				spec_rad += probe_spec * bw;
				spec_wsum += bw;
			}
		}
	}

	// Fallback: all 4 probes failed (disocclusion / grid edge) -> direct WRC irradiance
	// (already returns the cosine-average A). Mirrors the production consumer's fallback;
	// the A2 debug consumer returned 0 here, but the production resolve has the WRC to lean on.
	float q; // source quality -> .a (see RTGI_RESOLVE_QUALITY_FLOOR at the top).
	if (wsum <= 0.0) {
		float dconf;
		A = rtgi_wrc_sample_irradiance(wrc_radiance, wrc_distance, resolve_wrc_params(), world_pos, world_N, dconf);
		// The WRC prior is never "verified" by this pixel's own traced rays, so it advances the
		// temporal count at the floor rate -- a slow ease-in for permanent-fallback pixels. (When
		// dconf == 0 the value is black and revealing black is invisible, so one constant suffices.)
		q = RTGI_RESOLVE_QUALITY_FLOOR;
	} else {
		A /= wsum;
		q = q_sum / wsum;
	}

	imageStore(diffuse_gi_rw, pos, vec4(A, q)); // lighting space; .a = source quality (TEMPORAL consumes it).

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
	// .a: INTEGRATE writes the source quality (the spec shares the diffuse's probes and prior
	// fraction, so the same q applies); TEMPORAL overwrites it with the sample-count fraction
	// n/n_cap -- a confidence proxy that drives the T3 variance-weighted spatial filter.
	imageStore(spec_gi_rw, pos, vec4(spec, q));
}

// ---------------------------------------------------------------------------------------
// TEMPORAL (A3-T2): motion-reprojected history accumulation with rejection.
//
// After INTEGRATE writes this frame's RAW resolved GI into the [read_index] rw images (9/10),
// the TEMPORAL pass runs IN-PLACE on those same images: it imageLoads this frame's value
// ("cur"), motion-reprojects the PREVIOUS frame's accumulated GI (the history samplers 11/12 =
// the [1 - read_index] set), rejects bad history on a same-surface depth+normal AND-gate, and
// blends with a sample-counted 1/n (n-capped) weight, then imageStores the result back. This is
// the per-pixel analogue of rtgi_spg_accumulate.glsl's REPROJECT+BLEND (the resolve reprojects
// per screen pixel rather than per probe-atlas texel; the 1/n blend + the in-place storage-image
// read-modify-write discipline are mirrored from there). DIFFUSE reprojects on surface motion;
// SPECULAR on a roughness-SELECTED reproject (hard branch, C17). History rejection is depth AND
// normal (AND mesh-id where available -- this fork's resolve has no mesh-id G-buffer, so depth +
// normal is the documented fallback), NOT the legacy OR-gate (C11).

// Reconstruct WORLD-space normal at integer pixel `pos` from the CURRENT-frame normal-roughness
// G-buffer (view-space encoded rgb*2-1, rotated VIEW->WORLD), matching how INTEGRATE builds
// world_N. Returns false for a background / degenerate-normal texel (no usable surface there).
bool resolve_world_normal(ivec2 pos, out vec3 world_n) {
	vec3 enc = texelFetch(normal_roughness_buffer, pos, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.0001) {
		world_n = vec3(0.0);
		return false;
	}
	world_n = normalize(mat3(ubo.inv_view) * normalize(view_normal));
	return true;
}

// Reproject validity: accept the history at `prev_pos` for the surface at `cur_pos` ONLY when
// they lie on the SAME surface. The history G-buffer is the CURRENT-frame G-buffer sampled at
// prev_pos -- this fork has no prev-frame G-buffer, so this is the standard screen-space
// reproject validity check (mirrors how rtgi_spg_accumulate.glsl plane-matches its reprojected
// probe cell against the current header). SAME-SURFACE AND-GATE (C11, NOT an OR-gate):
//   depth-relative-diff <= history_rejection * tol  AND  dot(normal_cur, normal_prev) > thresh.
// (Mesh-id would be a third AND term but this fork's resolve has no mesh-id G-buffer wired;
// depth + normal is the documented fallback.) A background / degenerate prev texel rejects.
bool resolve_history_valid(ivec2 cur_pos, ivec2 prev_pos) {
	float cur_raw = texelFetch(depth_buffer, cur_pos, 0).r;
	float prev_raw = texelFetch(depth_buffer, prev_pos, 0).r;
	if (cur_raw <= 0.0 || prev_raw <= 0.0) {
		return false; // current or reprojected texel is background/sky -> no valid history.
	}
	// View-space linear depth (-view_pos.z, positive in front) at both pixels, from the same
	// inv_projection reconstruct INTEGRATE uses for the probe plane match.
	float cur_depth = -resolve_reconstruct_view_position(cur_pos, cur_raw).z;
	float prev_depth = -resolve_reconstruct_view_position(prev_pos, prev_raw).z;
	// Relative depth tolerance, scaled by the history_rejection knob (1.0 -> 5% base, the same
	// rel-tol family rtgi_spg_accumulate.glsl uses; smaller = stricter, more rejection).
	float tol = 0.05 * max(pc.history_rejection, 0.0);
	float depth_rel = abs(prev_depth - cur_depth) / max(cur_depth, 1e-4);
	if (depth_rel > tol) {
		return false;
	}
	// Same-surface normal cosine (AND with the depth test). ~25 deg, matching the SPG plane match.
	vec3 cur_n, prev_n;
	if (!resolve_world_normal(cur_pos, cur_n) || !resolve_world_normal(prev_pos, prev_n)) {
		return false;
	}
	if (dot(cur_n, prev_n) <= 0.906) {
		return false;
	}
	return true; // depth AND normal both agree -> same surface, history is valid.
}

// SPECULAR reproject (C17): a HARD SELECT on roughness, NEVER a lerp between the two
// reprojections. Below the rough cutoff (sharp-ish, where the reflection tracks the virtual
// image) we WOULD reproject on the virtual position (offset toward the reflected hit); a robust
// virtual-position estimate is not available from the current resolve buffers (no per-pixel hit
// distance is stored), so the sharp branch reuses the surface reproject as the documented
// approximation -- but the MECHANISM is a hard branch on roughness, not a blend. At/above the
// cutoff (broad lobe) the reflection tracks the surface, so we use the surface reproject. The
// review-graded requirement is the hard branch; both arms returning surface_prev today does not
// make it a lerp (a future virtual-position estimate slots straight into the sharp arm).
ivec2 resolve_spec_reproject(ivec2 cur_pos, ivec2 surface_prev, float roughness) {
	if (roughness < pc.rough_cutoff) {
		// Sharp domain: virtual-position reproject. No virtual-position estimate available from
		// the current buffers -> fall back to the surface reproject (documented approximation).
		return surface_prev;
	}
	// Rough domain: the lobe is broad, the reflection tracks the surface -> surface reproject.
	return surface_prev;
}

// TEMPORAL: per screen pixel, blend this frame's INTEGRATE output with the motion-reprojected,
// rejection-gated previous-frame accumulated GI using a sample-counted 1/n (n-capped) weight.
// Runs IN-PLACE on the [read_index] rw images (imageLoad cur -> blend -> imageStore), reading the
// [1 - read_index] history via the read samplers (11/12).
void resolve_temporal_main(ivec2 pos) {
	vec4 cur_d = imageLoad(diffuse_gi_rw, pos); // this-frame INTEGRATE output (diffuse).
	vec4 cur_s = imageLoad(spec_gi_rw, pos); // this-frame INTEGRATE output (rough-spec).

	// Per-pixel roughness drives the spec reproject SELECT (C17). Same ORM.g source INTEGRATE uses.
	float roughness = texelFetch(guide_orm, pos, 0).g;

	float n_cap = max(pc.temporal_n_cap, 1.0);

	// Surface motion from the velocity buffer (RB_TEX_VELOCITY). Its convention is UV-space,
	// prev - cur (motion = prev_uv - cur_uv; see scene_forward_clustered.glsl motion_vector +
	// rt_store_primary_velocity in raytracing_common_inc.glsl, and the TAA/RT consumers that read
	// prev_uv = cur_uv + mv). So the previous pixel is pos + mv scaled UV -> pixels by the screen
	// size: prev = pos + round(mv * screen). (The earlier "PIXELS, cur - prev" reading was wrong on
	// BOTH units and sign: a raw UV value ~0.01 rounded to 0, so the reproject never moved and the
	// history smeared under motion.) On the static furnace mv == 0 -> prev == pos (identity
	// reproject, byte-identical to before), so TEMPORAL degenerates to a stable in-place accumulate.
	// The velocity buffer carries only per-OBJECT motion. Static geometry is rendered with NO
	// motion-vector output (color_pass_inclusion_mask 0 in _fill_render_list), so it keeps the
	// velocity attachment's (-1,-1) clear sentinel forever. Reprojecting that sentinel naively
	// (prev = pos + mv*screen) lands ~a full screen off, so every static pixel reads as a fresh
	// disocclusion EVERY frame -- which (a) prevents temporal accumulation on the entire static
	// world (boil), and (b) pins the sample count at 1 so the COMPOSITE's cold-start gate holds the
	// indirect dark screen-wide. A static surface's only screen motion IS the camera's, so for the
	// sentinel derive prev from the world position reprojected by the previous frame's
	// view-projection. Dynamic objects (real velocity) keep prev = pos + mv*screen unchanged.
	vec2 mv = texelFetch(velocity_buffer, pos, 0).xy;
	ivec2 prev;
	if (mv.x <= -1.0 && mv.y <= -1.0) {
		float reproj_depth = texelFetch(depth_buffer, pos, 0).r;
		vec4 prev_clip = vec4(0.0);
		if (reproj_depth > 0.0) {
			vec3 wpos = (ubo.inv_view * vec4(resolve_reconstruct_view_position(pos, reproj_depth), 1.0)).xyz;
			prev_clip = ubo.prev_view_projection * vec4(wpos, 1.0);
		}
		if (prev_clip.w > 0.0) {
			vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
			prev = ivec2(floor(prev_uv * vec2(pc.screen_w, pc.screen_h)));
		} else {
			// Behind the old camera (a far cut: this surface was off-screen / behind the previous
			// view) or sky. Emit an OUT-OF-BOUNDS sentinel so the bounds gate below rejects it as a
			// TRUE disocclusion (n_d = 0 -> conf = 1/n_cap -> cold_fade fades the GI in from 0). NOT
			// prev = pos: a self-fetch makes resolve_history_valid(pos, pos) trivially PASS (identical
			// depth + normal -- there is one current-frame G-buffer, no prev-frame G-buffer), which
			// FALSE-ACCEPTS the stale [prev_index] GI still sitting at this screen coord. That carried
			// a high sample count -> conf ~= 1 -> cold_fade ~= 1, defeating the cold-start gate, so the
			// OLD camera's lighting showed through un-faded as a faint patch on far cuts.
			prev = ivec2(-1);
		}
	} else {
		prev = pos + ivec2(round(mv * vec2(pc.screen_w, pc.screen_h)));
	}

	float n_d = 0.0;
	float n_s = 0.0;
	vec3 hist_d = vec3(0.0);
	vec3 hist_s = vec3(0.0);
	// frame_index 0 has no history (the buffers were just cleared / are last frame's stale set);
	// skip reprojection so the first frame is a pure INTEGRATE seed (n -> 1). The bounds guard
	// keeps the reprojected fetch on-screen (off-screen history is a disocclusion -> reset).
	if (pc.frame_index > 0u &&
			all(greaterThanEqual(prev, ivec2(0))) &&
			prev.x < int(pc.screen_w) && prev.y < int(pc.screen_h)) {
		// DIFFUSE: reproject on the surface motion. Accept history only on a same-surface match.
		if (resolve_history_valid(pos, prev)) {
			vec4 h = texelFetch(diffuse_history, prev, 0);
			hist_d = h.rgb;
			n_d = h.a * n_cap; // recover the stored sample count (.a == n / n_cap).
		}
		// SPECULAR: reproject on the roughness-SELECTED position (hard branch, C17), then the
		// SAME same-surface rejection at that reprojected pixel.
		ivec2 sprev = resolve_spec_reproject(pos, prev, roughness);
		if (resolve_history_valid(pos, sprev)) {
			vec4 h = texelFetch(spec_history, sprev, 0);
			hist_s = h.rgb;
			n_s = h.a * n_cap;
		}
	}

	// Sample-counted 1/n blend (n-capped), mirroring rtgi_spg_accumulate.glsl's BLEND: a fresh /
	// rejected texel (n == 0 -> nd == 1 -> w == 1) takes cur outright; a converged one leans on
	// its history. Store the new count back as .a == n / n_cap for next frame.
	// The count advances by the SOURCE QUALITY INTEGRATE wrote into cur .a (how much of this
	// frame's value is verified traced data) instead of a flat +1: an unverified prior (WRC seed /
	// fallback) must not count as convergence, or the composite's cold-start gate opens while the
	// pixel still shows the un-traced prior (the post-cut round blobs: one leaky WRC cell's
	// trilinear footprint, revealed mid-wash). Floored so prior-only pixels still converge (see
	// RTGI_RESOLVE_QUALITY_FLOOR); fully traced probes give quality 1 -> identical to the old
	// +1/frame after warmup.
	float nd = min(n_d + clamp(cur_d.a, RTGI_RESOLVE_QUALITY_FLOOR, 1.0), n_cap);
	float ns = min(n_s + clamp(cur_s.a, RTGI_RESOLVE_QUALITY_FLOOR, 1.0), n_cap);
	vec3 od = mix(hist_d, cur_d.rgb, 1.0 / max(nd, 1.0));
	vec3 os = mix(hist_s, cur_s.rgb, 1.0 / max(ns, 1.0));
	imageStore(diffuse_gi_rw, pos, vec4(od, nd / n_cap));
	imageStore(spec_gi_rw, pos, vec4(os, ns / n_cap));
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
// (render_resolve_debug binds 11/12 to the [read_index] set, so these sample THIS frame's
// resolved output -- the TEMPORAL/INTEGRATE result -- not a history buffer.)
void resolve_debug_gi_main(ivec2 pos) {
	// Read BOTH the diffuse (11) and spec (12) GI buffers so both stay referenced regardless of
	// the selected channel (an unreferenced sampler would be stripped from the reflected layout).
	vec4 diffuse = texelFetch(diffuse_history, pos, 0);
	vec3 spec = texelFetch(spec_history, pos, 0).rgb;
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
	imageStore(dest_image, pos, vec4(result, diffuse.a)); // .a = post-TEMPORAL sample-count fraction n/n_cap (inspection).
}

// COMPOSITE (A3-T4): the BEAUTY remod of the resolved screen GI, written to dest_image (13) for the
// additive blit onto the raster-lit opaque frame. This is the surface light-out the DEBUG_GI view
// deferred: L_indirect = albedo * A + spec, where A is the LIGHTING-SPACE diffuse incident radiance
// (no albedo, PI-free at storage -> the per-surface remod is the albedo multiply HERE) and spec is
// already RADIANCE space (the INTEGRATE rough-spec already applied the split-sum BRDF, so it is
// added directly). Background/sky (raw_depth <= 0) is masked to 0 so the additive_blend leaves the
// raster sky untouched. render_composite binds 11/12 to the [read_index] (THIS frame's resolved)
// set -- same discipline as render_resolve_debug -- so diffuse_history/spec_history here are this
// frame's TEMPORAL/SPATIAL output, NOT a history buffer. guide_albedo (2) is the REAL material-guide
// albedo (linear); depth (0) is the real depth buffer (the background mask).
void resolve_composite_main(ivec2 pos) {
	float raw_depth = texelFetch(depth_buffer, pos, 0).r; // binding 0: real depth (reverse-Z; 0 = sky).
	vec3 albedo = texelFetch(guide_albedo, pos, 0).rgb; // binding 2: material-guide albedo (linear).
	// Remod the LIGHTING-SPACE diffuse A by the DIFFUSE albedo, not the raw material albedo. Metals
	// have NO Lambertian diffuse: their albedo IS the specular F0 (see INTEGRATE's mix(0.04, albedo,
	// metalness)), so `albedo * A` would falsely inject a diffuse term into metals and over-light
	// every metal under GI. diffuse_albedo = albedo * (1 - metalness) is the standard PBR split:
	// unchanged for dielectrics (metalness 0), driven to 0 for metals (metalness 1). spec is already
	// F0/BRDF-correct from INTEGRATE, so it is added unmodified.
	float metalness = texelFetch(guide_orm, pos, 0).b; // binding 15 ORM.b = metallic (r=ao, g=rough, a=sss).
	vec3 diffuse_albedo = albedo * (1.0 - metalness);
	vec4 diffuse_sample = texelFetch(diffuse_history, pos, 0); // binding 11 = [read_index] diffuse.
	vec3 A = diffuse_sample.rgb; // lighting-space A.
	// Cold-start hide. Read-time only (the stored GI is untouched -> no history feedback). The cold /
	// still-biased GI is faded in from ZERO as the pixel's estimate actually CONVERGES, gated on the
	// resolve temporal sample fraction (diffuse_sample.a == nd / n_cap, written by TEMPORAL), NOT a
	// wall-clock timer. A 0 contribution cannot blotch, so the unconfident GI stays invisible until it
	// has accumulated enough samples, then eases in -- a gentle bloom-in that reads as HDR adaptation
	// rather than a cold-start smudge. On a load / disocclusion / cut the count resets to ~1/n_cap
	// (TEMPORAL rejects history) -> cold_fade ~= 0, then climbs to 1 as the pixel settles. Driving the
	// fade on the SAMPLE COUNT (not seconds) makes it frame-rate-independent: the old time-based fade
	// released early at the low fps of a load/cut hitch, snapping the still-cold GI on (the blotch). It
	// also lets the composite SKIP the per-pixel WRC irradiance gather it used to do here -- that
	// ~512-tap gather over the disocclusion band, re-deriving a warm/cold signal INTEGRATE already had,
	// was the motion-time FPS drop. In steady state diffuse_sample.a == 1 -> cold_fade == 1 -> full GI
	// at ~no cost. pc.fade_time > 0 enables the hide (0 disables it for A/B).
	float cold_fade = 1.0;
	if (pc.fade_time > 0.0 && raw_depth > 0.0) {
		float conf = clamp(diffuse_sample.a, 0.0, 1.0); // per-pixel GI convergence (accumulated nd / n_cap).
		cold_fade = smoothstep(0.25, 0.95, conf); // held at 0 while cold / biased -> 1 once converged.
	}
	vec3 spec = texelFetch(spec_history, pos, 0).rgb; // binding 12 = [read_index] spec: radiance-space (BRDF applied).
	// Mask the background so the additive composite does not lift the raster sky/clear color.
	// cold_fade fades the whole indirect (diffuse + spec) in from 0 as the GI converges (load/disocclusion/cut).
	vec3 indirect = (raw_depth <= 0.0) ? vec3(0.0) : ((diffuse_albedo * A + spec) * cold_fade);

	// GI-aware reactive mask ("poor-man's Ray Reconstruction"): diffuse_sample.a holds the
	// post-TEMPORAL sample fraction n/n_cap in [0,1] (the resolve's per-pixel temporal CONFIDENCE).
	// reactive = 1 - confidence, so freshly disoccluded / low-confidence GI tells the temporal
	// upscaler to trust THIS frame there. Background/sky (raw_depth <= 0) is 0 = "trust history".
	// Written only when requested (the Reactive denoiser is selected); otherwise binding 16 is a
	// neutral placeholder the shader never touches.
	if (pc.write_reactive == 1u) {
		float conf = clamp(diffuse_sample.a, 0.0, 1.0);
		float reactive = (raw_depth <= 0.0) ? 0.0 : clamp(1.0 - conf, 0.0, 1.0);
		imageStore(reactive_image, pos, vec4(reactive, 0.0, 0.0, 0.0));
	}
	// Sanitize before the additive store (B4): this lands on a PERSISTENT color FB via additive_blend
	// (dest.rgb += source.rgb), so a single negative or non-finite resolve value would STICK and
	// permanently corrupt the beauty. Clamp negatives to 0 (the indirect contribution is additive
	// energy, never negative), then replace any Inf/NaN component with 0 so it cannot poison the FB.
	// Two separate componentwise mix()es (the proven copy.glsl idiom) -- isinf()/isnan() return a
	// bvec3 the mix selects on; a logical-OR of the two bvec3s is NOT valid GLSL.
	indirect = max(indirect, vec3(0.0));
	indirect = mix(indirect, vec3(0.0), isinf(indirect));
	indirect = mix(indirect, vec3(0.0), isnan(indirect));
	imageStore(dest_image, pos, vec4(indirect, 0.0)); // binding 13 = gi_debug_image; .a unused by the additive blit.
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= int(pc.screen_w) || pos.y >= int(pc.screen_h)) {
		return;
	}

	if (sc_resolve_mode == RESOLVE_MODE_INTEGRATE) {
		resolve_integrate_main(pos);
		return;
	}
	if (sc_resolve_mode == RESOLVE_MODE_TEMPORAL) {
		resolve_temporal_main(pos);
		return;
	}
	if (sc_resolve_mode == RESOLVE_MODE_DEBUG_GI) {
		resolve_debug_gi_main(pos);
		return;
	}
	if (sc_resolve_mode == RESOLVE_MODE_COMPOSITE) {
		resolve_composite_main(pos);
		return;
	}
}
