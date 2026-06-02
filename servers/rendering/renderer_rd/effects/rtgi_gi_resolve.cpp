/**************************************************************************/
/*  rtgi_gi_resolve.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_gi_resolve.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Mode selectors. These EXACTLY match the RESOLVE_MODE_* defines in rtgi_gi_resolve.glsl.
// T0 implements INTEGRATE (0) + DEBUG_GI (3); TEMPORAL (1) + SPATIAL (2) are declared so
// the numbering is stable but their kernels land in T2/T3.
#define RESOLVE_MODE_INTEGRATE 0u
#define RESOLVE_MODE_TEMPORAL 1u
#define RESOLVE_MODE_SPATIAL 2u
#define RESOLVE_MODE_DEBUG_GI 3u

RTGIGIResolve::RTGIGIResolve() {
	// Single compute shader with a `mode` push-constant (mirrors the SPG PLACE shader
	// setup). One set-0 layout drives every mode (INTEGRATE writes the GI buffers,
	// DEBUG_GI reads them and writes the debug image), so a single
	// uniform set / pipeline is sufficient. This is the first compile of
	// rtgi_gi_resolve.glsl (and thus of its rtgi_spg_inc.glsl / rtgi_wrc_inc.glsl /
	// oct_inc.glsl include usage in a resolve context), so a GLSL error there trips here.
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIGIResolve::~RTGIGIResolve() {
	free_resources();
	shader.version_free(shader_version);
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
		const GiResolveFrameParams &p_frame, const Projection &p_inv_proj, const Transform3D &p_inv_view) {
	// resources_valid is set only at the end of _allocate() (which free_resources()es
	// first), so it guarantees both ping-pong buffer sets were allocated together;
	// spot-checking diffuse_gi[read_index] is therefore sufficient.
	if (!resources_valid || !diffuse_gi[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_velocity.is_null());
	ERR_FAIL_COND(p_guide_albedo.is_null());
	ERR_FAIL_COND(p_guide_orm.is_null());
	ERR_FAIL_COND(p_spg_radiance.is_null());
	ERR_FAIL_COND(p_spg_header_plane.is_null());
	ERR_FAIL_COND(p_spg_header_aux.is_null());
	ERR_FAIL_COND(p_wrc_radiance.is_null());
	ERR_FAIL_COND(p_wrc_distance.is_null());

	const Size2i size = cached_render_size;

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

	// Set 0 declares EVERY binding any mode of rtgi_gi_resolve.glsl uses (one GLSL
	// shader's set-0 layout must declare all of them): G-buffers (0-1), the material-guide
	// albedo (2; binding 3 is reserved for velocity in T2 and is NOT declared by the
	// shader), SPG atlas + headers (4-6), WRC atlases (7-8), the GI write images (9-10),
	// the GI read samplers (11-12, used by DEBUG_GI), the debug dest image (13), the params
	// UBO (14), and the material-guide ORM (15). INTEGRATE writes 9-10 and reads 0-2 + 4-8
	// + 15; the bindings it does not touch (11-13) are pointed at the neutral gi_debug_image
	// / a pure-read texture below so no GI buffer is both written and sampled in this set.
	// ONE uniform set drives both modes. The set binds exactly
	// {0,1,2,4,5,6,7,8,9,10,11,12,13,14,15} (no binding 3).
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
	// Binding 3 (velocity) is reserved for T2 and NOT declared by the shader, so it is not
	// bound here; p_velocity is accepted in the signature but not bound yet.
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(p_guide_albedo);
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
	// DEBUG_GI read samplers (11-12) + debug dest image (13): all UNUSED by INTEGRATE.
	// Bind 11-12 to a pure-read G-buffer (p_depth) and 13 to gi_debug_image, so that NO
	// texture is bound as BOTH a sampler and a storage image in this set: the diffuse/spec
	// buffers INTEGRATE writes (9-10) appear only as images, p_depth only as samplers, and
	// gi_debug_image only as the (untouched) image 13. That avoids a same-resource
	// read+write within one descriptor set.
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 11;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 12;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
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
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 15;
		u.append_id(linear_sampler);
		u.append_id(p_guide_orm);
		uniforms.push_back(u);
	}

	GiResolveUBO ubo;
	memset(&ubo, 0, sizeof(GiResolveUBO));
	MaterialStorage::store_camera(p_inv_proj, ubo.inv_projection);
	MaterialStorage::store_transform(p_inv_view, ubo.inv_view);
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

	RD::get_singleton()->draw_command_begin_label("RTGI Resolve Integrate");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	// One thread per screen pixel.
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();
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
	// (0,1,2,4-8,15) point at diffuse_gi[read_index] (a read texture, like 11) and the neutral
	// IMAGE slots (9-10) point at gi_debug_image (the write target, like 13), so NO texture is
	// bound as both a sampler and a storage image in this set: read textures (diffuse/spec)
	// appear only as samplers, gi_debug_image only as images. That avoids a same-resource
	// read+write within one descriptor set. The set provides exactly
	// {0,1,2,4,5,6,7,8,9,10,11,12,13,14,15} -- the same layout run_resolve provides.
	LocalVector<RD::Uniform> uniforms;
	for (uint32_t b = 0; b <= 8; b++) {
		// Samplers 0-8 (incl. binding 2, the material-guide albedo) all point at the
		// resolved-diffuse read texture (neutral; DEBUG_GI does not read them). Binding 3
		// (velocity, reserved for T2) is NOT in the reflected layout, so it must be skipped
		// here or the uniform set would not match the shader.
		if (b == 3u) {
			continue;
		}
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
	// read+write, since the only images in this set are gi_debug_image at 9/10/13).
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 15;
		u.append_id(linear_sampler);
		u.append_id(diffuse_gi[read_index]);
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
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
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
