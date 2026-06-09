/**************************************************************************/
/*  rtgi_gi_resolve.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_gi_resolve.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Mode selectors. These EXACTLY match the RESOLVE_MODE_* defines in rtgi_gi_resolve.glsl.
// T0 implements INTEGRATE (0) + DEBUG_GI (3); TEMPORAL (1) + SPATIAL (2) are declared so
// the numbering is stable but their kernels land in T2/T3.
#define RESOLVE_MODE_INTEGRATE 0u
#define RESOLVE_MODE_TEMPORAL 1u
#define RESOLVE_MODE_SPATIAL 2u
#define RESOLVE_MODE_DEBUG_GI 3u
// COMPOSITE (A3-T4): BEAUTY remod (albedo * diffuse_A + spec) -> gi_debug_image for the
// additive blit onto the raster-lit frame. EXACTLY matches RESOLVE_MODE_COMPOSITE in the GLSL.
#define RESOLVE_MODE_COMPOSITE 4u
// The per-mode pipeline array (pipelines[]) is indexed by these values; RESOLVE_MODE_COUNT must cover
// 0..COMPOSITE (asserted in the constructor).

// Live-tuning override for the cold-start hide enable (> 0 on, 0 off; the reveal pace itself is
// convergence-driven), read once. -1 = unset (use the per-preset cold_start_fade_time). Mirrors
// the FPT_TAA_* / RTGI_SPG_WRC_SEED_SAMPLES env knobs.
static float rtgi_gi_fade_time_override() {
	static const float s_override = []() -> float {
		if (OS::get_singleton()->has_environment("RTGI_GI_FADE_TIME")) {
			return OS::get_singleton()->get_environment("RTGI_GI_FADE_TIME").to_float();
		}
		return -1.0f;
	}();
	return s_override;
}

RTGIGIResolve::RTGIGIResolve() {
	// Single compute shader with a `mode` push-constant (mirrors the SPG PLACE shader
	// setup). One set-0 layout drives every mode (INTEGRATE writes the GI buffers,
	// DEBUG_GI reads them and writes the debug image), so a single
	// uniform set / pipeline is sufficient. This is the first compile of
	// rtgi_gi_resolve.glsl (and thus of its rtgi_spg_inc.glsl / rtgi_wrc_inc.glsl /
	// oct_inc.glsl include usage in a resolve context), so a GLSL error there trips here.
	// Keep the mode count in lockstep with the RESOLVE_MODE_* range (member access, so it must live in
	// a member function, not file scope).
	static_assert(RESOLVE_MODE_COUNT == RESOLVE_MODE_COMPOSITE + 1u, "RESOLVE_MODE_COUNT must match the RESOLVE_MODE_* range");
	shader.initialize({ "" });
	shader_version = shader.version_create();
	// One pipeline per resolve mode, each baking sc_resolve_mode (constant_id 0) to the mode value so
	// the driver dead-strips the other modes' code and gives each its own register allocation. The
	// shader + set-0 layout are identical across them (the spec constant only gates main()'s dispatch).
	RID resolve_shader_rd = shader.version_get_shader(shader_version, 0);
	for (uint32_t mode = 0; mode < RESOLVE_MODE_COUNT; mode++) {
		if (mode == RESOLVE_MODE_SPATIAL) {
			continue; // built from the standalone spatial shader below.
		}
		RD::PipelineSpecializationConstant sc;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
		sc.constant_id = 0; // sc_resolve_mode
		sc.int_value = mode;
		Vector<RD::PipelineSpecializationConstant> specialization_constants;
		specialization_constants.push_back(sc);
		pipelines[mode] = RD::get_singleton()->compute_pipeline_create(resolve_shader_rd, specialization_constants);
	}
	// SPATIAL: standalone single-mode shader (owns the LDS). No spec constant.
	spatial_shader.initialize({ "" });
	spatial_shader_version = spatial_shader.version_create();
	pipelines[RESOLVE_MODE_SPATIAL] = RD::get_singleton()->compute_pipeline_create(spatial_shader.version_get_shader(spatial_shader_version, 0));

	// Standalone volumetric-fog composite shader/pipeline (FPT re-applies the froxel fog the
	// raster opaque pass would have applied; see composite_volumetric_fog). Its own small 3-binding
	// set-0 layout, separate from the resolve's unified layout above.
	fog_shader.initialize({ "" });
	fog_shader_version = fog_shader.version_create();
	fog_pipeline = RD::get_singleton()->compute_pipeline_create(fog_shader.version_get_shader(fog_shader_version, 0));
}

RTGIGIResolve::~RTGIGIResolve() {
	free_resources();
	shader.version_free(shader_version);
	spatial_shader.version_free(spatial_shader_version);
	fog_shader.version_free(fog_shader_version);
}

void RTGIGIResolve::composite_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_source_context, const StringName &p_source_texture, RID p_viewz_hitdist, RID p_fog_map, const Size2i &p_process_size, const Vector2i &p_visible_origin, const Size2i &p_visible_size, float p_fog_length, float p_fog_detail_spread, float p_fog_sky_affect, bool p_legacy_blending, uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(p_visible_size.x <= 0 || p_visible_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_source_texture));
	ERR_FAIL_COND(!p_viewz_hitdist.is_valid() || !p_fog_map.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	RID source = p_render_buffers->get_texture_slice(p_source_context, p_source_texture, p_view, 0);

	FogPushConstant push_constant;
	memset(&push_constant, 0, sizeof(FogPushConstant));
	push_constant.resolution[0] = (float)p_process_size.x;
	push_constant.resolution[1] = (float)p_process_size.y;
	push_constant.visible_origin[0] = (float)p_visible_origin.x;
	push_constant.visible_origin[1] = (float)p_visible_origin.y;
	push_constant.visible_size[0] = (float)p_visible_size.x;
	push_constant.visible_size[1] = (float)p_visible_size.y;
	push_constant.fog_inv_length = p_fog_length > 0.0f ? 1.0f / p_fog_length : 1.0f;
	push_constant.fog_detail_spread = p_fog_detail_spread > 0.0f ? 1.0f / p_fog_detail_spread : 1.0f;
	push_constant.fog_sky_affect = CLAMP(p_fog_sky_affect, 0.0f, 1.0f);
	push_constant.fog_legacy_blending = p_legacy_blending ? 1.0f : 0.0f;

	_dispatch_volumetric_fog(push_constant, source, p_viewz_hitdist, p_fog_map);
}

void RTGIGIResolve::_dispatch_volumetric_fog(const FogPushConstant &p_push_constant, RID p_color, RID p_viewz_hitdist, RID p_fog_map) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = fog_shader.version_get_shader(fog_shader_version, 0);

	RD::Uniform u_color(RD::UNIFORM_TYPE_IMAGE, 0, p_color);
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_fog_map(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear_sampler, p_fog_map }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, fog_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_color, u_viewz_hitdist, u_fog_map), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(FogPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution[0], (uint32_t)p_push_constant.resolution[1], 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIGIResolve::free_resources() {
	for (uint32_t i = 0; i < 2; i++) {
		if (diffuse_gi[i].is_valid()) {
			RD::get_singleton()->free_rid(diffuse_gi[i]);
			diffuse_gi[i] = RID();
		}
		if (spec_gi[i].is_valid()) {
			RD::get_singleton()->free_rid(spec_gi[i]);
			spec_gi[i] = RID();
		}
	}
	if (gi_debug_image.is_valid()) {
		RD::get_singleton()->free_rid(gi_debug_image);
		gi_debug_image = RID();
	}
	if (reactive_dummy.is_valid()) {
		RD::get_singleton()->free_rid(reactive_dummy);
		reactive_dummy = RID();
	}
	if (resolve_ubo.is_valid()) {
		RD::get_singleton()->free_rid(resolve_ubo);
		resolve_ubo = RID();
	}
	read_index = 0;
	cached_render_size = Size2i();
	resources_valid = false;
}

void RTGIGIResolve::_allocate(const Size2i &p_render_size) {
	free_resources();

	const int render_w = MAX(p_render_size.x, 1);
	const int render_h = MAX(p_render_size.y, 1);
	cached_render_size = Size2i(render_w, render_h);

	// RGBA16F at the INTERNAL render size, STORAGE (INTEGRATE/TEMPORAL/SPATIAL write
	// it) + SAMPLING (DEBUG_GI + the later beauty composite read it) + COPY both ways
	// (parity with the SPG atlases). Both diffuse + spec are ping-ponged so T2's
	// temporal accumulate can read the previous frame's resolved GI.
	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RD::TextureFormat gi_format;
	gi_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	gi_format.width = render_w;
	gi_format.height = render_h;
	gi_format.usage_bits = usage_bits;

	for (uint32_t i = 0; i < 2; i++) {
		diffuse_gi[i] = RD::get_singleton()->texture_create(gi_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(diffuse_gi[i], i == 0 ? "RTGI Resolve Diffuse GI A" : "RTGI Resolve Diffuse GI B");
		spec_gi[i] = RD::get_singleton()->texture_create(gi_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(spec_gi[i], i == 0 ? "RTGI Resolve Spec GI A" : "RTGI Resolve Spec GI B");

		// Clear to "unresolved": rgb 0, confidence/variance 0.
		RD::get_singleton()->texture_clear(diffuse_gi[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::get_singleton()->texture_clear(spec_gi[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	// Dedicated scratch / debug-dest image (also the neutral binding for the unified
	// layout; see the header). Same RGBA16F format/size as the GI buffers.
	gi_debug_image = RD::get_singleton()->texture_create(gi_format, RD::TextureView());
	RD::get_singleton()->set_resource_name(gi_debug_image, "RTGI Resolve Debug Image");
	RD::get_singleton()->texture_clear(gi_debug_image, Color(0, 0, 0, 0), 0, 1, 0, 1);

	// 1x1 R8 STORAGE dummy for the reactive-mask slot (binding 16) when the reactive denoiser is off.
	// Format-matching (r8) so the storage-image view matches the shader's declared format; never
	// accessed by any mode (see the header). Tiny (1x1) since it is a pure placeholder.
	RD::TextureFormat reactive_dummy_format;
	reactive_dummy_format.format = RD::DATA_FORMAT_R8_UNORM;
	reactive_dummy_format.width = 1;
	reactive_dummy_format.height = 1;
	reactive_dummy_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	reactive_dummy = RD::get_singleton()->texture_create(reactive_dummy_format, RD::TextureView());
	RD::get_singleton()->set_resource_name(reactive_dummy, "RTGI Resolve Reactive Dummy");
	RD::get_singleton()->texture_clear(reactive_dummy, Color(0, 0, 0, 0), 0, 1, 0, 1);

	read_index = 0;
	resources_valid = true;

	// Log total VRAM. RGBA16F = 8 B/texel; diffuse + spec each ping-ponged (x2) + the
	// scratch/debug image (x1).
	const uint64_t texels = uint64_t(render_w) * uint64_t(render_h);
	const uint64_t total_bytes = texels * 8ULL * 5ULL; // (2 buffers x 2 sets + 1 scratch) x 8 B.
	print_verbose(vformat("RTGI Resolve: allocated screen-GI buffers %dx%d using %.2f MiB VRAM.",
			render_w, render_h, double(total_bytes) / (1024.0 * 1024.0)));
}

bool RTGIGIResolve::ensure_resources(Ref<RenderSceneBuffersRD> p_rb, const GiResolveParams &p_params, const Size2i &p_render_size) {
	ERR_FAIL_COND_V(p_rb.is_null(), false);

	const int render_w = MAX(p_render_size.x, 1);
	const int render_h = MAX(p_render_size.y, 1);
	const Size2i wanted = Size2i(render_w, render_h);

	// Realloc only when the internal render size changes (it drives the buffer sizes).
	if (resources_valid && cached_render_size == wanted && diffuse_gi[0].is_valid()) {
		// Keep cached_params current even on a no-realloc frame so this frame's passes
		// read the right tunables (single source of truth, mirrors the SPG).
		cached_params = p_params;
		return false;
	}

	_allocate(wanted);
	cached_params = p_params;
	return true;
}

void RTGIGIResolve::run_resolve(RID p_depth, RID p_normal_roughness, RID p_velocity,
		RID p_guide_albedo, RID p_guide_orm,
		RID p_spg_radiance, RID p_spg_header_plane, RID p_spg_header_aux,
		RID p_wrc_radiance, RID p_wrc_distance,
		const GiResolveFrameParams &p_frame, const Projection &p_inv_proj, const Transform3D &p_inv_view,
		const Projection &p_prev_cam_projection, const Transform3D &p_prev_cam_transform) {
	// THE FRAME SWAP (A3-T2): flip read_index ONCE at the TOP of the frame (mirrors
	// RTGIScreenProbeGather::run_placement). AFTER the flip [read_index] is THIS frame's set
	// (INTEGRATE writes it, TEMPORAL accumulates in place) and [1 - read_index] is the previous
	// frame's accumulated result (the history TEMPORAL reprojects + blends). Both ping-pong sets
	// are allocated together, so either index is valid -- guarding [read_index] after the flip is
	// sufficient. get_diffuse_gi()/get_spec_gi() return [read_index], which becomes next frame's
	// history. NO further swap happens this frame.
	read_index = 1u - read_index;

	// resources_valid is set only at the end of _allocate() (which free_resources()es
	// first), so it guarantees both ping-pong buffer sets were allocated together;
	// spot-checking diffuse_gi[read_index] is therefore sufficient.
	if (!resources_valid || !diffuse_gi[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_velocity.is_null());
	// NO hard-abort on the material-guide textures (A-fix): the DIFFUSE INTEGRATE writes a
	// LIGHTING-SPACE A that uses NEITHER guide_albedo (binding 2) NOR guide_orm (binding 15) --
	// they feed ONLY the do_spec-gated rough-spec F0/roughness/metallic (rtgi_gi_resolve.glsl
	// ~:370/470). A null guide must therefore NOT kill the diffuse resolve, and -- critically --
	// must NOT leave read_index flipped-with-nothing-written (a temporal-history desync, since the
	// flip already happened at the top of this frame). When a guide is null we bind the neutral
	// white default below so the dispatch stays valid and the spec-F0 degrades gracefully; the
	// diffuse path always runs and writes A, keeping the read_index flip consistent.
	ERR_FAIL_COND(p_spg_radiance.is_null());
	ERR_FAIL_COND(p_spg_header_plane.is_null());
	ERR_FAIL_COND(p_spg_header_aux.is_null());
	ERR_FAIL_COND(p_wrc_radiance.is_null());
	ERR_FAIL_COND(p_wrc_distance.is_null());

	const Size2i size = cached_render_size;

	// [read_index] is THIS frame's set (INTEGRATE writes it, TEMPORAL accumulates in place);
	// [prev_index] is the previous frame's accumulated result (TEMPORAL's reproject history).
	const uint32_t prev_index = 1u - read_index;

	// The reconstruction matrices live in a UBO (the two mat4s alone hit the 128-byte
	// MAX_PUSH_CONSTANT_SIZE cap). Created once, reused every frame.
	if (resolve_ubo.is_null()) {
		resolve_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(GiResolveUBO));
		RD::get_singleton()->set_resource_name(resolve_ubo, "RTGI Resolve Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = shader.version_get_shader(shader_version, 0);
	// Linear sampler, clamp-to-edge. INTEGRATE only does integer texelFetch()es over the
	// SPG atlas/headers + G-buffers, but they are bound as SAMPLER_WITH_TEXTURE (mirroring
	// the SPG consumer); the WRC fallback query does bilinear taps + already clamps its
	// per-tile UVs, so clamp-to-edge is correct.
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// Neutral guide fallback (A-fix): the guide-albedo (binding 2) and guide-orm (binding 15) are
	// consumed ONLY by the do_spec-gated rough-spec path; the diffuse A is guide-independent. If a
	// guide RID is null this frame (its material-guide texture was freed between the prepass and
	// this consume) bind the default WHITE texture so the dispatch stays valid -- the guides here
	// are plain sampler2D (single-view; depth/normal at 0/1 are likewise bound view-0), so the 2D
	// WHITE default matches the declared layout. White ORM/albedo degrade the spec-F0 gracefully
	// (mix(0.04, white, metalness)); the diffuse resolve is unaffected. Keeping the real guide bound
	// when it IS valid leaves behavior identical to before.
	RID guide_default = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID guide_albedo_rid = p_guide_albedo.is_valid() ? p_guide_albedo : guide_default;
	RID guide_orm_rid = p_guide_orm.is_valid() ? p_guide_orm : guide_default;

	// Set 0 declares EVERY binding any mode of rtgi_gi_resolve.glsl uses (one GLSL
	// shader's set-0 layout must declare all of them): G-buffers (0-1), the material-guide
	// albedo (2), the velocity buffer (3, A3-T2), SPG atlas + headers (4-6), WRC atlases
	// (7-8), the GI read+write images (9-10 = [read_index]), the GI HISTORY read samplers
	// (11-12 = [prev_index]), the debug dest image (13), the params UBO (14), the
	// material-guide ORM (15), and the reactive-mask image (16). ONE shared uniform set drives
	// BOTH INTEGRATE and TEMPORAL: INTEGRATE writes 9-10 and reads 0-2 + 4-8 + 15 (it ignores
	// 3/11/12/16); TEMPORAL reads 3 + 11-12 and read-modify-writes 9-10 in place. The set binds
	// exactly {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}. NO same-resource sampler+image hazard:
	// [read_index] appears ONLY at 9/10 (image), [prev_index] ONLY at 11/12 (sampler), and
	// they are DIFFERENT buffers (the ping-pong sets); the debug dest image 13 is the
	// untouched gi_debug_image; the reactive slot 16 is the untouched reactive_dummy (these
	// modes never write the reactive mask). (The DEBUG_GI set in render_resolve_debug is
	// separate: it binds 11/12 = [read_index] to DISPLAY this frame's resolved output.)
	LocalVector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(linear_sampler);
		u.append_id(p_normal_roughness);
		uniforms.push_back(u);
	}
	// Binding 2: material-guide albedo (A3-T1; INTEGRATE reads it for the rough-spec F0 mix).
	// guide_albedo_rid falls back to the WHITE default when p_guide_albedo is null (see above).
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(guide_albedo_rid);
		uniforms.push_back(u);
	}
	// Binding 3: velocity buffer (A3-T2; TEMPORAL reprojects the history with it). Now declared
	// by the shader, so it is bound here from p_velocity. INTEGRATE ignores it.
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 3;
		u.append_id(linear_sampler);
		u.append_id(p_velocity);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 4;
		u.append_id(linear_sampler);
		u.append_id(p_spg_radiance);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 5;
		u.append_id(linear_sampler);
		u.append_id(p_spg_header_plane);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 6;
		u.append_id(linear_sampler);
		u.append_id(p_spg_header_aux);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 7;
		u.append_id(linear_sampler);
		u.append_id(p_wrc_radiance);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 8;
		u.append_id(linear_sampler);
		u.append_id(p_wrc_distance);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 9;
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 10;
		u.append_id(spec_gi[read_index]);
		uniforms.push_back(u);
	}
	// GI HISTORY read samplers (11-12): the PREVIOUS frame's accumulated result = the
	// [prev_index] ping-pong set. TEMPORAL texelFetches these at the reprojected pixel for the
	// 1/n blend; INTEGRATE ignores them. They are DIFFERENT buffers from the [read_index] images
	// at 9/10, so NO texture is bound as BOTH a sampler and a storage image in this set: the
	// [read_index] buffers appear only as images (9/10), the [prev_index] buffers only as
	// samplers (11/12), and gi_debug_image only as the (untouched) image 13. That avoids a
	// same-resource read+write within one descriptor set.
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 11;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[prev_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 12;
		u.append_id(linear_sampler);
		u.append_id(spec_gi[prev_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 13;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 14;
		u.append_id(resolve_ubo);
		uniforms.push_back(u);
	}
	// Binding 15: material-guide ORM (A3-T1; INTEGRATE reads g = roughness, b = metallic).
	// guide_orm_rid falls back to the WHITE default when p_guide_orm is null (see above).
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 15;
		u.append_id(linear_sampler);
		u.append_id(guide_orm_rid);
		uniforms.push_back(u);
	}
	// Binding 16: the GI-aware reactive mask (write-only). INTEGRATE/TEMPORAL/SPATIAL never write it
	// (only COMPOSITE does), so the shared set-0 layout binds the neutral r8 reactive_dummy here -- a
	// format-matching STORAGE image, never written through this slot, so it adds no same-resource
	// read+write hazard. The spatial per-iteration set copies `uniforms`, so this slot propagates there
	// unchanged (binding 16 stays reactive_dummy across the a-trous loop).
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 16;
		u.append_id(reactive_dummy);
		uniforms.push_back(u);
	}
	GiResolveUBO ubo;
	memset(&ubo, 0, sizeof(GiResolveUBO));
	MaterialStorage::store_camera(p_inv_proj, ubo.inv_projection);
	MaterialStorage::store_transform(p_inv_view, ubo.inv_view);
	// Previous-frame world -> clip = prev_proj * prev_view (prev_view = inverse of the prev camera
	// transform). TEMPORAL reprojects static geometry (the velocity sentinel) through this so the
	// static world accumulates history. Built jittered, matching the jittered history the resolve
	// stored.
	const Projection prev_view_projection = p_prev_cam_projection * Projection(p_prev_cam_transform.affine_inverse());
	MaterialStorage::store_camera(prev_view_projection, ubo.prev_view_projection);
	RD::get_singleton()->buffer_update(resolve_ubo, 0, sizeof(GiResolveUBO), &ubo);

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.mode = RESOLVE_MODE_INTEGRATE;
	push_constant.frame_index = p_frame.frame_index;
	push_constant.screen_w = (uint32_t)size.x;
	push_constant.screen_h = (uint32_t)size.y;
	push_constant.spatial_iter = (uint32_t)MAX(cached_params.spatial_iterations, 0);
	push_constant.cur_iter = 0u;
	push_constant.spg_grid_w = p_frame.spg_grid_w;
	push_constant.spg_grid_h = p_frame.spg_grid_h;
	push_constant.spg_oct_res = MAX(p_frame.spg_oct_res, 1u);
	push_constant.spg_spacing_f = MAX(p_frame.spg_spacing_f, 1u);
	push_constant.temporal_n_cap = MAX(cached_params.temporal_n_cap, 1.0f);
	push_constant.rough_cutoff = cached_params.rough_spec_roughness_cutoff;
	push_constant.rough_enabled = cached_params.rough_spec_enabled ? 1u : 0u;
	push_constant.wrc_grid = MAX(p_frame.wrc_grid, 1u);
	push_constant.wrc_cascade_count = MAX(p_frame.wrc_cascade_count, 1u);
	push_constant.wrc_base_spacing = p_frame.wrc_base_spacing;
	// TEMPORAL (A3-T2) reproject tolerance scale; carried for both dispatches (INTEGRATE ignores
	// it). temporal_n_cap (set above) is the history responsiveness, also shared by both.
	push_constant.history_rejection = cached_params.history_rejection;
	const float fade_override = rtgi_gi_fade_time_override();
	push_constant.fade_time = MAX(fade_override >= 0.0f ? fade_override : cached_params.cold_start_fade_time, 0.0f);

	// INTEGRATE -> barrier -> TEMPORAL, recorded back-to-back on ONE compute list with the SAME
	// shared uniform set (mirrors how RTGIScreenProbeGather::run_accumulate records REPROJECT ->
	// barrier -> BLEND). INTEGRATE writes this frame's RAW resolve into the [read_index] images
	// (9/10); the barrier fences those writes; TEMPORAL then reads them back in place, blends the
	// reprojected [prev_index] history (11/12), and stores the accumulated result to 9/10.
	RD::get_singleton()->draw_command_begin_label("RTGI Resolve Integrate + Temporal");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[RESOLVE_MODE_INTEGRATE]);
	RID resolve_set = uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, resolve_set, 0);

	// INTEGRATE: one thread per screen pixel -> this frame's RAW resolved GI.
	// Finer GPU-profiler bracket (A3-T8): the next RENDER_TIMESTAMP ("RTGI Resolve Temporal")
	// closes this region, so INTEGRATE is a distinct profiler area (mirrors gi.cpp's use of
	// RENDER_TIMESTAMP around its SDFGI compute sub-stages).
	RENDER_TIMESTAMP("RTGI Resolve Integrate");
	push_constant.mode = RESOLVE_MODE_INTEGRATE;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);

	// Barrier so TEMPORAL reads the fully-written INTEGRATE output (not a partial write) when it
	// imageLoads the [read_index] images in place.
	RD::get_singleton()->compute_list_add_barrier(compute_list);

	// TEMPORAL: one thread per screen pixel -> motion-reprojected history accumulate, in place.
	// Finer GPU-profiler bracket (A3-T8): this RENDER_TIMESTAMP ends the INTEGRATE region above;
	// the SPATIAL label (or compute_list_end when SPATIAL is skipped) closes this one.
	RENDER_TIMESTAMP("RTGI Resolve Temporal");
	// The barrier above re-bound the INTEGRATE pipeline (compute_list_add_barrier restores the prior
	// pipeline + sets); switch to the TEMPORAL pipeline and re-bind the (unchanged) resolve set.
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[RESOLVE_MODE_TEMPORAL]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, resolve_set, 0);
	push_constant.mode = RESOLVE_MODE_TEMPORAL;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);

	// SPATIAL (A3-T3): ONE joint (diffuse + spec) edge-aware a-trous filter, run
	// `spatial_iterations` times after TEMPORAL (0 = skip; 1 = default; 2 = escalation). The
	// a-trous reads NEIGHBORS, so it CANNOT run in place -- each iteration reads a SOURCE set and
	// writes a DISTINCT DEST set, ping-ponging between [read_index] (the TEMPORAL output / running
	// result) and [prev_index] (the previous frame's history, already consumed by TEMPORAL this
	// frame, so it is FREE scratch -- next frame's INTEGRATE overwrites it after the flip).
	//
	// BINDING: per iteration the SOURCE goes on the read samplers (11/12) and the DEST on the write
	// images (9/10), reusing the existing set-0 layout. SOURCE != DEST every iteration, so within
	// each per-iteration set no texture is bound as BOTH a sampler and a storage image (the only
	// same-resource hazard the validator flags); the velocity / guide / SPG / WRC bindings stay
	// bound (SPATIAL reads depth/normal/ORM + the GI buffers, and ignores the rest).
	//
	// PARITY: starting iter 0 src=[read]->dst=[prev], the result alternates dst = [prev],[read],
	// [prev],... so an ODD iteration count ends in [prev], an EVEN one in [read]. The getters (and
	// next frame's history) read [read_index], so for an odd count we texture_copy [prev]->[read]
	// after the compute list (a GPU blit; the buffers carry CAN_COPY_FROM/TO). The default (1) is
	// odd. A third scratch pair is avoided -- [prev] suffices.
	const uint32_t spatial_iterations = (uint32_t)MAX(cached_params.spatial_iterations, 0);
	// Finer GPU-profiler bracket (A3-T8): ONE label covers all the SPATIAL iterations as a single
	// profiler area (mirrors gi.cpp, which labels an iterative jump-flood pass once before its loop).
	// The RENDER_TIMESTAMP is issued INSIDE the loop on iteration 0, right after that iteration's
	// barrier, NOT here before the loop: capturing a timestamp on an active compute list is only
	// legal while its dispatch_count is 0 (RenderingDevice::capture_timestamp ERR_FAILs otherwise,
	// at rendering_device.cpp), and the TEMPORAL dispatch above left dispatch_count > 0. The iter-0
	// barrier resets it to 0, so the timestamp is legal there. When spatial_iterations == 0 the loop
	// never runs, so no SPATIAL region is opened and compute_list_end closes the TEMPORAL region.
	uint32_t spatial_src = read_index; // iter 0 reads the TEMPORAL output in [read_index].
	for (uint32_t it = 0; it < spatial_iterations; it++) {
		const uint32_t spatial_dst = 1u - spatial_src;

		// Barrier so this iteration reads the FULLY-written previous pass (TEMPORAL for iter 0, the
		// prior a-trous step otherwise) rather than a partial write. The barrier also resets the
		// compute list's dispatch_count to 0 (internally it ends and re-begins the list), which is
		// what makes the iter-0 SPATIAL timestamp below legal.
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		// Open the SPATIAL profiler region once, on iteration 0, AFTER the barrier above (see the
		// note before the loop): the timestamp closes the TEMPORAL region and brackets the a-trous
		// iterations. It must follow a barrier so dispatch_count == 0 when it is captured, otherwise
		// RenderingDevice::capture_timestamp ERR_FAILs and the pass time is dropped.
		if (it == 0) {
			RENDER_TIMESTAMP("RTGI Resolve Spatial");
		}

		// Per-iteration set: identical to the run_resolve layout except the 4 GI bindings. Copy the
		// common uniforms (0-8, 13-16 untouched, incl. the neutral reactive slot 16) and override 9/10
		// (DEST images) + 11/12 (SOURCE samplers). The pushed order is binding-sequential, so
		// uniforms[b].binding == b for b<=16.
		// The SPATIAL shader is standalone with a 7-binding subset layout (0,1,9,10,11,12,14), so build
		// the set from just those (sub-selected from `uniforms`, which is binding-indexed), with this
		// iteration's ping-pong override on 9/10 (DEST) + 11/12 (SOURCE).
		LocalVector<RD::Uniform> spatial_uniforms;
		spatial_uniforms.push_back(uniforms[0]); // depth_buffer
		spatial_uniforms.push_back(uniforms[1]); // normal_roughness_buffer
		RD::Uniform su_diff_dst = uniforms[9];
		su_diff_dst.clear_ids();
		su_diff_dst.append_id(diffuse_gi[spatial_dst]);
		spatial_uniforms.push_back(su_diff_dst);
		RD::Uniform su_spec_dst = uniforms[10];
		su_spec_dst.clear_ids();
		su_spec_dst.append_id(spec_gi[spatial_dst]);
		spatial_uniforms.push_back(su_spec_dst);
		RD::Uniform su_diff_src = uniforms[11];
		su_diff_src.clear_ids();
		su_diff_src.append_id(linear_sampler);
		su_diff_src.append_id(diffuse_gi[spatial_src]);
		spatial_uniforms.push_back(su_diff_src);
		RD::Uniform su_spec_src = uniforms[12];
		su_spec_src.clear_ids();
		su_spec_src.append_id(linear_sampler);
		su_spec_src.append_id(spec_gi[spatial_src]);
		spatial_uniforms.push_back(su_spec_src);
		spatial_uniforms.push_back(uniforms[14]); // GiResolveUBO

		// The per-iteration barrier above re-bound the prior pipeline (TEMPORAL on iter 0); switch to
		// the standalone SPATIAL pipeline + its own (smaller) set.
		RID spatial_shader_rd = spatial_shader.version_get_shader(spatial_shader_version, 0);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[RESOLVE_MODE_SPATIAL]);
		RID spatial_set = uniform_set_cache->get_cache_vec(spatial_shader_rd, 0, spatial_uniforms);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, spatial_set, 0);

		push_constant.mode = RESOLVE_MODE_SPATIAL;
		push_constant.cur_iter = it; // a-trous step = 1 << cur_iter (1, 2, 4, ...).
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);

		spatial_src = spatial_dst; // next iteration reads what this one wrote.
	}

	RD::get_singleton()->compute_list_end();

	// PARITY copy: if the a-trous loop left the final result in [prev_index] (odd iteration count),
	// blit it back to [read_index] so the getters / next frame's history see it. spatial_src now
	// holds the buffer index of the final write; copy only when it is NOT [read_index].
	if (spatial_iterations > 0 && spatial_src != read_index) {
		RD::get_singleton()->texture_copy(diffuse_gi[spatial_src], diffuse_gi[read_index], Vector3(), Vector3(), Vector3(size.x, size.y, 1), 0, 0, 0, 0);
		RD::get_singleton()->texture_copy(spec_gi[spatial_src], spec_gi[read_index], Vector3(), Vector3(), Vector3(size.x, size.y, 1), 0, 0, 0, 0);
	}

	RD::get_singleton()->draw_command_end_label();
}

void RTGIGIResolve::render_resolve_debug(Ref<RenderSceneBuffersRD> p_rb, const Size2i &p_size, RID p_dest_fb, uint32_t p_debug_channel) {
	if (!resources_valid || !diffuse_gi[read_index].is_valid() || !spec_gi[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_dest_fb.is_null());

	const Size2i size = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));

	// DEBUG_GI writes the raw lighting-space resolve output into the dedicated debug image
	// (binding 13), then blits it. gi_debug_image is allocated alongside the GI buffers (same
	// RGBA16F format/size) and is the resolve analogue of the WRC/SPG gi_debug_image.
	ERR_FAIL_COND(!gi_debug_image.is_valid());

	if (resolve_ubo.is_null()) {
		resolve_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(GiResolveUBO));
		RD::get_singleton()->set_resource_name(resolve_ubo, "RTGI Resolve Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = shader.version_get_shader(shader_version, 0);
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// Same full set-0 layout as run_resolve (one GLSL shader, one layout). DEBUG_GI reads
	// binding 11 (diffuse), 12 (spec) and writes 13 (debug dest). The neutral SAMPLER slots
	// (0,1,2,3,4-8,15) point at diffuse_gi[read_index] (a read texture, like 11), the neutral
	// IMAGE slots (9-10) point at gi_debug_image (the write target, like 13), and the reactive
	// slot (16) points at the untouched r8 reactive_dummy, so NO texture is bound as both a
	// sampler and a storage image in this set: read textures (diffuse/spec) appear only as
	// samplers, gi_debug_image only as images. That avoids a same-resource read+write within one
	// descriptor set. The set provides exactly {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16} -- the
	// same layout run_resolve provides.
	LocalVector<RD::Uniform> uniforms;
	for (uint32_t b = 0; b <= 8; b++) {
		// Samplers 0-8 (incl. binding 2 the material-guide albedo and binding 3 the velocity
		// buffer) all point at the resolved-diffuse read texture (neutral; DEBUG_GI does not read
		// them). Binding 3 is now DECLARED by the shader (A3-T2), so it MUST be provided here too
		// or the uniform set would not match the reflected layout -- it is bound neutrally like the
		// rest (a read texture on a sampler slot is safe).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = b;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 9;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 10;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 11;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 12;
		u.append_id(linear_sampler);
		u.append_id(spec_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 13;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 14;
		u.append_id(resolve_ubo);
		uniforms.push_back(u);
	}
	// Binding 15 (material-guide ORM): neutral here -- DEBUG_GI does not read it, so it points
	// at the resolved-diffuse read texture like the other neutral samplers (never a same-resource
	// read+write, since the only images in this set are gi_debug_image at 9/10/13/16).
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 15;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	// Binding 16 (reactive mask): neutral here -- DEBUG_GI does not write it, so it binds the
	// format-matching r8 reactive_dummy. write_reactive stays 0 (memset), so the slot is never touched.
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 16;
		u.append_id(reactive_dummy);
		uniforms.push_back(u);
	}
	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.mode = RESOLVE_MODE_DEBUG_GI;
	push_constant.screen_w = (uint32_t)size.x;
	push_constant.screen_h = (uint32_t)size.y;
	// Channel select (A3-T1): 0 = diffuse-only (RESOLVE_GI), 1 = spec-only (RESOLVE_SPEC),
	// else = combined. rough_cutoff/rough_enabled stay 0 (memset): DEBUG_GI does not read them
	// (only INTEGRATE uses them for the rough-spec gate); the fields stay in the PushConstant.
	push_constant.debug_channel = p_debug_channel;

	RD::get_singleton()->draw_command_begin_label("RTGI Resolve GI Debug");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[RESOLVE_MODE_DEBUG_GI]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	// Blit the raw lighting-space resolve output (diffuse + spec) linear value to the
	// destination framebuffer. No sRGB, no LOG_LUMINANCE: keep values linear/measurable for
	// the furnace gate (mirrors the WRC/SPG GI consumer raw-linear blit).
	CopyEffects *copy_effects = CopyEffects::get_singleton();
	ERR_FAIL_NULL(copy_effects);
	const bool multiview = p_rb.is_valid() && p_rb->get_view_count() > 1;
	copy_effects->copy_to_fb_rect(gi_debug_image, p_dest_fb, Rect2i(Point2i(), size), false, false, false, false, RID(), multiview, false, false, false, Rect2(), 1.0, true, CopyEffects::COPY_TO_FB_FLAG_MODE_NONE);
}

void RTGIGIResolve::render_composite(RID p_depth, RID p_normal_roughness, RID p_guide_albedo, RID p_guide_orm, RID p_wrc_radiance, RID p_wrc_distance, const GiResolveFrameParams &p_frame, const Size2i &p_size, RID p_dest_color_fb, uint32_t p_view_count, RID p_fpt_primary_color, RID p_reactive_out) {
	if (!resources_valid || !diffuse_gi[read_index].is_valid() || !spec_gi[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_dest_color_fb.is_null());
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_guide_albedo.is_null());
	ERR_FAIL_COND(p_guide_orm.is_null()); // COMPOSITE now reads ORM.b (metallic) for the diffuse remod.
	// COMPOSITE writes the BEAUTY remod (albedo * diffuse_A + spec) into the dedicated scratch
	// image (binding 13), then ADDITIVELY blends it onto the raster-lit color FB. gi_debug_image is
	// the same RGBA16F render-size scratch render_resolve_debug uses.
	ERR_FAIL_COND(!gi_debug_image.is_valid());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_wrc_radiance.is_null());
	ERR_FAIL_COND(p_wrc_distance.is_null());

	const Size2i size = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));

	if (resolve_ubo.is_null()) {
		resolve_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(GiResolveUBO));
		RD::get_singleton()->set_resource_name(resolve_ubo, "RTGI Resolve Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = shader.version_get_shader(shader_version, 0);
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// Same full set-0 layout as render_resolve_debug (one GLSL shader, one layout), with the SAME
	// hazard discipline: the images in this set are gi_debug_image (at 9/10/13) and the reactive mask
	// (at 16), and every read texture is bound only on a sampler -- so no texture is bound as BOTH a
	// sampler and a storage image. COMPOSITE reads binding 0 (depth, the background mask), 2 (guide
	// albedo, the diffuse remod), 15 (guide ORM, the metallic for the diffuse split), 11
	// (diffuse_gi[read_index]) and 12 (spec_gi[read_index] = THIS frame's resolved GI), writes 13, and
	// (only when the reactive denoiser is selected) writes 16. The departures from the debug layout are
	// exactly: binding 0 = the REAL depth, binding 2 = the REAL guide albedo, binding 15 = the REAL
	// guide ORM (the diffuse remod multiplies A by albedo * (1 - ORM.b) so metals get no Lambertian
	// diffuse), and binding 16 = the REAL reactive mask when p_reactive_out is valid. The reactive image
	// at 16 is a DISTINCT write-only image (RB_TEX_RTGI_REACTIVE), never also bound as a sampler, so it
	// adds no same-resource read+write hazard; when p_reactive_out is RID() it binds the neutral
	// format-matching reactive_dummy and write_reactive stays 0, so binding 16 is never touched. After the
	// cold-start lean binding 1 is the REAL normal-roughness and 7/8 are the REAL WRC atlases (read by the
	// lean); only bindings 3-6 stay neutral (point at diffuse_gi[read_index], a read texture) since
	// COMPOSITE ignores them. The set provides exactly {0..17} (binding 17 = the age image).
	LocalVector<RD::Uniform> uniforms;
	{
		// Binding 0: the REAL depth buffer (COMPOSITE reads it to mask the background/sky).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
		uniforms.push_back(u);
	}
	{
		// Binding 1: the REAL normal-roughness (the cold-start lean reconstructs world_N from it).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(linear_sampler);
		u.append_id(p_normal_roughness);
		uniforms.push_back(u);
	}
	{
		// Binding 2: the REAL material-guide albedo (COMPOSITE remods the resolved diffuse A by it).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(p_guide_albedo);
		uniforms.push_back(u);
	}
	for (uint32_t b = 3; b <= 6; b++) {
		// Bindings 3-6 (velocity, SPG atlas/headers): neutral -- COMPOSITE ignores them, so they point
		// at the resolved-diffuse read texture (a read texture on a sampler slot is safe).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = b;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		// Binding 7: the REAL WRC radiance atlas (the cold-start lean queries rtgi_wrc_sample_irradiance).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 7;
		u.append_id(linear_sampler);
		u.append_id(p_wrc_radiance);
		uniforms.push_back(u);
	}
	{
		// Binding 8: the REAL WRC distance atlas (Chebyshev visibility for the lean's WRC query).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 8;
		u.append_id(linear_sampler);
		u.append_id(p_wrc_distance);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 9;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 10;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		// Binding 11 = the [read_index] diffuse GI (THIS frame's resolved lighting-space A). Bound to
		// the read set just like render_resolve_debug, so COMPOSITE reads this frame's output.
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 11;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		// Binding 12 = the [read_index] spec GI (THIS frame's resolved rough-spec radiance).
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 12;
		u.append_id(linear_sampler);
		u.append_id(spec_gi[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 13;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 14;
		u.append_id(resolve_ubo);
		uniforms.push_back(u);
	}
	{
		// Binding 15: the REAL material-guide ORM. COMPOSITE reads ORM.b (metallic) to build the
		// diffuse albedo (albedo * (1 - metalness)) for the diffuse remod -- metals have no Lambertian
		// diffuse, so their albedo (which is the spec F0) must not modulate the diffuse irradiance.
		// (render_resolve_debug keeps this slot neutral; only COMPOSITE needs the real ORM.)
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 15;
		u.append_id(linear_sampler);
		u.append_id(p_guide_orm);
		uniforms.push_back(u);
	}
	// Binding 16: the GI-aware reactive mask output. When p_reactive_out is valid (the Reactive
	// denoiser is selected) COMPOSITE writes 1 - confidence there for the temporal upscaler; otherwise
	// bind the neutral format-matching r8 reactive_dummy and leave write_reactive 0 so the shader never
	// touches it -- keeping the composite output byte-identical.
	const bool reactive_enabled = p_reactive_out.is_valid();
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 16;
		u.append_id(reactive_enabled ? p_reactive_out : reactive_dummy);
		uniforms.push_back(u);
	}
	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.mode = RESOLVE_MODE_COMPOSITE;
	push_constant.screen_w = (uint32_t)size.x;
	push_constant.screen_h = (uint32_t)size.y;
	push_constant.write_reactive = reactive_enabled ? 1u : 0u;
	// WRC clipmap scalars (run_resolve fills these for INTEGRATE/TEMPORAL; COMPOSITE builds its own
	// push, so fill them here too). fade_time (the cold-start hide enable) comes from the cached params.
	push_constant.spg_oct_res = MAX(p_frame.spg_oct_res, 1u);
	push_constant.wrc_grid = MAX(p_frame.wrc_grid, 1u);
	push_constant.wrc_cascade_count = MAX(p_frame.wrc_cascade_count, 1u);
	push_constant.wrc_base_spacing = p_frame.wrc_base_spacing;
	const float fade_override = rtgi_gi_fade_time_override();
	push_constant.fade_time = MAX(fade_override >= 0.0f ? fade_override : cached_params.cold_start_fade_time, 0.0f);

	RD::get_singleton()->draw_command_begin_label("RTGI Composite Hybrid");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[RESOLVE_MODE_COMPOSITE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();

	CopyEffects *copy_effects = CopyEffects::get_singleton();
	ERR_FAIL_NULL(copy_effects);
	if (p_fpt_primary_color.is_valid()) {
		// A4 FPT REPLACE: opaque = per-pixel path-traced primary-direct + probe indirect. First
		// overwrite the dest with the primary-direct color (rt_get_texture, written by the FPT
		// primary-direct dispatch this frame) -- discarding the raster opaque, which FPT does not
		// use -- then additively add the resolved indirect (gi_debug_image, masked to 0 on the
		// background so the primary-direct's sky-on-miss survives there). The draw graph orders the
		// blit after the primary-direct dispatch's write (RAW on rt_get_texture).
		copy_effects->copy_to_fb_rect(p_fpt_primary_color, p_dest_color_fb, Rect2i(Point2i(), size));
		copy_effects->additive_blend(p_dest_color_fb, gi_debug_image, p_view_count);
	} else {
		// Hybrid: ADDITIVELY blend the composited indirect GI onto the raster-lit color FB:
		// merge_specular's SPECULAR_MERGE_ADDITIVE_ADD path (dest.rgb += source.rgb), linear
		// additive. This lands the indirect-at-primary on top of the DIRECT light the opaque pass
		// already wrote. Background pixels were masked to 0 in the shader, so the raster sky/clear
		// color is untouched.
		copy_effects->additive_blend(p_dest_color_fb, gi_debug_image, p_view_count);
	}

	RD::get_singleton()->draw_command_end_label();
}
