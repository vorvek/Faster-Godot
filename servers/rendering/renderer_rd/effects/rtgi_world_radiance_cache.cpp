/**************************************************************************/
/*  rtgi_world_radiance_cache.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_world_radiance_cache.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGIWorldRadianceCache::RTGIWorldRadianceCache() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	// WRC-GI debug consumer pipeline. Same single-variant setup as the update
	// shader above; this is the first compile of rtgi_wrc_inc.glsl (included by
	// the consumer), so a GLSL error in that header surfaces here at build time.
	gi_debug_shader.initialize({ "" });
	gi_debug_shader_version = gi_debug_shader.version_create();
	gi_debug_pipeline = RD::get_singleton()->compute_pipeline_create(gi_debug_shader.version_get_shader(gi_debug_shader_version, 0));
}

RTGIWorldRadianceCache::~RTGIWorldRadianceCache() {
	free_resources();
	shader.version_free(shader_version);
	gi_debug_shader.version_free(gi_debug_shader_version);
}

Size2i RTGIWorldRadianceCache::_atlas_size(const RtgiWrc::ClipmapParams &p_params) const {
	// Each probe owns one `oct_res x oct_res` tile. `atlas_tiles_per_row` packs
	// the cascade_count * grid^3 tiles into a roughly-square grid of tiles; the
	// per-axis tile count is ceil(sqrt(total_tiles)). Width spans a full row of
	// tiles; height spans exactly enough tile-rows to hold every tile so the
	// `linear / tiles_per_row` row index in RtgiWrc::atlas_coord stays in-bounds.
	const int oct_res = MAX(p_params.oct_res, 1);
	const int tiles_per_row = RtgiWrc::atlas_tiles_per_row(p_params);
	const int64_t total_tiles = int64_t(MAX(p_params.cascade_count, 1)) * MAX(p_params.grid, 1) * MAX(p_params.grid, 1) * MAX(p_params.grid, 1);
	const int tile_rows = int((total_tiles + tiles_per_row - 1) / tiles_per_row);
	return Size2i(tiles_per_row * oct_res, MAX(tile_rows, 1) * oct_res);
}

bool RTGIWorldRadianceCache::ensure_ray_result_buffer(uint32_t p_rays_per_frame) {
	const uint32_t capacity = MAX(1u, p_rays_per_frame);
	if (ray_result_buffer.is_valid() && ray_result_capacity >= capacity) {
		return false;
	}
	if (ray_result_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ray_result_buffer);
		ray_result_buffer = RID();
	}

	// 3 x vec4 = 48 bytes per entry, identical to STRC's RTGISTRCProbeRayResult
	// so the GLSL RTGIWRCProbeRayResult struct and Task 6's accumulate match.
	const uint32_t result_stride = sizeof(float) * 12u;
	ray_result_buffer = RD::get_singleton()->storage_buffer_create(uint64_t(capacity) * result_stride);
	RD::get_singleton()->set_resource_name(ray_result_buffer, "RTGI WRC Probe Ray Results");
	ray_result_capacity = capacity;
	return true;
}

void RTGIWorldRadianceCache::free_resources() {
	if (ray_result_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ray_result_buffer);
		ray_result_buffer = RID();
	}
	ray_result_capacity = 0;
	for (uint32_t i = 0; i < 2; i++) {
		if (radiance_atlas[i].is_valid()) {
			RD::get_singleton()->free_rid(radiance_atlas[i]);
			radiance_atlas[i] = RID();
		}
		if (distance_atlas[i].is_valid()) {
			RD::get_singleton()->free_rid(distance_atlas[i]);
			distance_atlas[i] = RID();
		}
		if (metadata_atlas[i].is_valid()) {
			RD::get_singleton()->free_rid(metadata_atlas[i]);
			metadata_atlas[i] = RID();
		}
	}
	if (gi_debug_image.is_valid()) {
		RD::get_singleton()->free_rid(gi_debug_image);
		gi_debug_image = RID();
	}
	if (gi_debug_ubo.is_valid()) {
		RD::get_singleton()->free_rid(gi_debug_ubo);
		gi_debug_ubo = RID();
	}
	gi_debug_image_size = Size2i();
	read_index = 0;
	atlas_size = Size2i();
	resources_valid = false;
}

void RTGIWorldRadianceCache::_allocate_atlases(const RtgiWrc::ClipmapParams &p_params) {
	free_resources();

	atlas_size = _atlas_size(p_params);
	cached_params = p_params;

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RD::TextureFormat radiance_format;
	radiance_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	radiance_format.width = atlas_size.x;
	radiance_format.height = atlas_size.y;
	radiance_format.usage_bits = usage_bits;

	RD::TextureFormat distance_format;
	distance_format.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	distance_format.width = atlas_size.x;
	distance_format.height = atlas_size.y;
	distance_format.usage_bits = usage_bits;

	RD::TextureFormat metadata_format;
	metadata_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	metadata_format.width = atlas_size.x;
	metadata_format.height = atlas_size.y;
	metadata_format.usage_bits = usage_bits;

	for (uint32_t i = 0; i < 2; i++) {
		radiance_atlas[i] = RD::get_singleton()->texture_create(radiance_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(radiance_atlas[i], i == 0 ? "RTGI WRC Radiance Atlas A" : "RTGI WRC Radiance Atlas B");
		distance_atlas[i] = RD::get_singleton()->texture_create(distance_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(distance_atlas[i], i == 0 ? "RTGI WRC Distance Atlas A" : "RTGI WRC Distance Atlas B");
		metadata_atlas[i] = RD::get_singleton()->texture_create(metadata_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(metadata_atlas[i], i == 0 ? "RTGI WRC Metadata Atlas A" : "RTGI WRC Metadata Atlas B");

		// Clear to "unwritten": radiance/confidence 0, distance moments 0 (a zero
		// mean marks an unwritten texel per the rtgi_wrc_inc.glsl contract).
		RD::get_singleton()->texture_clear(radiance_atlas[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::get_singleton()->texture_clear(distance_atlas[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::get_singleton()->texture_clear(metadata_atlas[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	read_index = 0;
	resources_valid = true;

	// Log total VRAM. RGBA16F = 8 B/texel, RG16F = 4 B/texel, RGBA8 = 4 B/texel;
	// each is ping-ponged (x2).
	const uint64_t texels = uint64_t(atlas_size.x) * uint64_t(atlas_size.y);
	const uint64_t bytes_per_set = texels * (8ULL + 4ULL + 4ULL);
	const uint64_t total_bytes = bytes_per_set * 2ULL;
	print_verbose(vformat("RTGI WRC: allocated radiance-cache atlases %dx%d (cascades=%d grid=%d oct_res=%d) using %.2f MiB VRAM.",
			atlas_size.x, atlas_size.y, p_params.cascade_count, p_params.grid, p_params.oct_res,
			double(total_bytes) / (1024.0 * 1024.0)));
}

bool RTGIWorldRadianceCache::ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, const RtgiWrc::ClipmapParams &p_params, int p_view_count) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);

	const Size2i wanted = _atlas_size(p_params);
	const bool params_changed = cached_params.cascade_count != p_params.cascade_count ||
			cached_params.grid != p_params.grid ||
			cached_params.oct_res != p_params.oct_res;
	if (resources_valid && !params_changed && atlas_size == wanted && radiance_atlas[0].is_valid()) {
		return false;
	}

	_allocate_atlases(p_params);
	return true;
}

void RTGIWorldRadianceCache::update(RID p_tlas, RID p_scene_uniform_set, const WRCFrameParams &p_frame_params) {
	if (!resources_valid || !radiance_atlas[0].is_valid()) {
		return;
	}
	// The ray-result SSBO is bound at set-0 binding 6, which the shader declares
	// unconditionally and the single uniform set drives BOTH dispatches (mode 0
	// recenter + mode 1 accumulate). It is sized to >= 1 entry by
	// ensure_ray_result_buffer(), so a missing buffer means update() was called
	// before that ran -- bail rather than build an incomplete descriptor set.
	ERR_FAIL_COND(!ray_result_buffer.is_valid());

	const uint32_t write_index = 1u - read_index;

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	// Bindings mirror the shader's set 0 layout: read (front) atlases at 0..2,
	// write (back) atlases at 3..5, and the probe-ray result SSBO at 6 (consumed
	// by the mode-1 accumulate kernel).
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, radiance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, distance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, metadata_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, radiance_atlas[write_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, distance_atlas[write_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, metadata_atlas[write_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, ray_result_buffer));

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.cascade_count = (uint32_t)MAX(cached_params.cascade_count, 1);
	push_constant.grid = (uint32_t)MAX(cached_params.grid, 1);
	push_constant.oct_res = (uint32_t)MAX(cached_params.oct_res, 1);
	push_constant.atlas_width = (uint32_t)atlas_size.x;
	push_constant.atlas_height = (uint32_t)atlas_size.y;
	push_constant.rays_this_frame = p_frame_params.rays_this_frame;
	push_constant.frame_index = p_frame_params.frame_index;
	push_constant.base_spacing = cached_params.base_spacing;
	push_constant.temporal_n_cap = p_frame_params.temporal_n_cap;
	push_constant.feedback_damping = p_frame_params.feedback_damping;
	push_constant.view_prioritization = p_frame_params.view_prioritization;
	push_constant.camera_pos[0] = p_frame_params.camera_pos.x;
	push_constant.camera_pos[1] = p_frame_params.camera_pos.y;
	push_constant.camera_pos[2] = p_frame_params.camera_pos.z;
	for (uint32_t k = 0; k < 4; k++) {
		push_constant.scroll_delta[k][0] = p_frame_params.scroll_delta[k][0];
		push_constant.scroll_delta[k][1] = p_frame_params.scroll_delta[k][1];
		push_constant.scroll_delta[k][2] = p_frame_params.scroll_delta[k][2];
	}

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);

	// Mode 0: scroll/recenter the cache into the write atlas over the full atlas
	// (one thread per atlas texel). Always dispatched -- with zero scroll it is the
	// identity FRONT->BACK copy the ping-pong + in-place accumulate depend on.
	push_constant.mode = 0u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);

	// Mode 1: accumulate this frame's probe rays into the write atlas. ONE thread
	// per ray result (1D dispatch over rays_this_frame), since the producer writes
	// exactly one result per ray and each carries a distinct (probe, dir) ->
	// distinct atlas texel. Skipped when no rays were traced this frame (the mode-0
	// recenter already produced a valid BACK atlas to swap in).
	if (push_constant.rays_this_frame > 0u) {
		push_constant.mode = 1u;
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, MAX(push_constant.rays_this_frame, 1u), 1, 1);
	}
	RD::get_singleton()->compute_list_end();

	// Ping-pong: the freshly written back atlases become the new front.
	read_index = write_index;
}

void RTGIWorldRadianceCache::_ensure_gi_debug_image(const Size2i &p_size) {
	const Size2i wanted = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));
	if (gi_debug_image.is_valid() && gi_debug_image_size == wanted) {
		return;
	}
	if (gi_debug_image.is_valid()) {
		RD::get_singleton()->free_rid(gi_debug_image);
		gi_debug_image = RID();
	}

	RD::TextureFormat tf;
	// RGBA16F to hold linear, un-tonemapped HDR irradiance (.rgb) + confidence (.a)
	// without clipping; STORAGE (the consumer writes it) + SAMPLING (the blit reads
	// it through copy_to_fb_rect).
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = wanted.x;
	tf.height = wanted.y;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	gi_debug_image = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(gi_debug_image, "RTGI WRC GI Debug Image");
	gi_debug_image_size = wanted;
}

void RTGIWorldRadianceCache::render_gi_debug(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_depth, RID p_normal_roughness, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const RtgiWrc::ClipmapParams &p_params, const Vector3 &p_camera_pos, float p_strength, RID p_dest_fb, const Size2i &p_size) {
	if (!resources_valid || !radiance_atlas[read_index].is_valid() || !distance_atlas[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_dest_fb.is_null());

	const Size2i size = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));
	_ensure_gi_debug_image(size);

	// The consumer's params live in a UBO (bound at set 0, binding 5), not a push
	// constant: GiDebugUBO is 176 bytes and the two mat4s alone already hit the
	// 128-byte MAX_PUSH_CONSTANT_SIZE cap. Created once, reused every frame.
	if (gi_debug_ubo.is_null()) {
		gi_debug_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(GiDebugUBO));
		RD::get_singleton()->set_resource_name(gi_debug_ubo, "RTGI WRC GI Debug Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = gi_debug_shader.version_get_shader(gi_debug_shader_version, 0);
	// Linear sampler, clamp-to-edge: the query does bilinear taps and already
	// clamps its UVs to the per-tile half-texel inset, so edge clamp is correct.
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// Set 0: radiance/distance atlases (sampler+texture), depth + normal-roughness
	// G-buffers (sampler+texture), and the output storage image.
	LocalVector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(linear_sampler);
		u.append_id(radiance_atlas[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(linear_sampler);
		u.append_id(distance_atlas[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 3;
		u.append_id(linear_sampler);
		u.append_id(p_normal_roughness);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 4;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 5;
		u.append_id(gi_debug_ubo);
		uniforms.push_back(u);
	}

	// Fill WrcParams from cached_params -- the EXACT ClipmapParams the atlas was
	// built from in ensure_resources(). The caller reconstructs the same clamped
	// defaults into p_params; assert they agree so a future divergence (params not
	// kept in lock-step) trips loudly in dev builds instead of silently
	// mis-addressing the query tiles. Using cached_params (not p_params) keeps the
	// query pinned to the atlas's actual layout regardless of the caller.
	DEV_ASSERT(p_params.cascade_count == cached_params.cascade_count &&
			p_params.grid == cached_params.grid &&
			p_params.oct_res == cached_params.oct_res);

	GiDebugUBO ubo;
	memset(&ubo, 0, sizeof(GiDebugUBO));
	ubo.cascade_count = MAX(cached_params.cascade_count, 1);
	ubo.grid = MAX(cached_params.grid, 1);
	ubo.oct_res = MAX(cached_params.oct_res, 1);
	ubo.base_spacing = cached_params.base_spacing;
	ubo.occlusion_bias_spacing = 0.5f; // sane default per rtgi_wrc_inc.glsl WrcParams docs.
	ubo.min_variance = 0.0001f;
	ubo.screen_width = size.x;
	ubo.screen_height = size.y;
	ubo.camera_pos[0] = p_camera_pos.x;
	ubo.camera_pos[1] = p_camera_pos.y;
	ubo.camera_pos[2] = p_camera_pos.z;
	ubo.strength = p_strength;
	MaterialStorage::store_camera(p_inv_projection, ubo.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, ubo.inv_view);
	RD::get_singleton()->buffer_update(gi_debug_ubo, 0, sizeof(GiDebugUBO), &ubo);

	RD::get_singleton()->draw_command_begin_label("RTGI WRC GI Debug");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, gi_debug_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	// No push constant: the params are supplied via gi_debug_ubo (binding 5),
	// bound in the uniform set above and updated via buffer_update() this frame.
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	// Blit the raw linear irradiance to the destination framebuffer. No sRGB, no
	// LOG_LUMINANCE: keep values linear/measurable for the furnace gate.
	CopyEffects *copy_effects = CopyEffects::get_singleton();
	ERR_FAIL_NULL(copy_effects);
	const bool multiview = p_render_buffers.is_valid() && p_render_buffers->get_view_count() > 1;
	copy_effects->copy_to_fb_rect(gi_debug_image, p_dest_fb, Rect2i(Point2i(), size), false, false, false, false, RID(), multiview, false, false, false, Rect2(), 1.0, true, CopyEffects::COPY_TO_FB_FLAG_MODE_NONE);
}
