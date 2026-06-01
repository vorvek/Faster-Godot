/**************************************************************************/
/*  rtgi_world_radiance_cache.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_world_radiance_cache.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGIWorldRadianceCache::RTGIWorldRadianceCache() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIWorldRadianceCache::~RTGIWorldRadianceCache() {
	free_resources();
	shader.version_free(shader_version);
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

	const uint32_t write_index = 1u - read_index;

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	// Bindings mirror the shader's set 0 layout: read (front) atlases at 0..2,
	// write (back) atlases at 3..5. The Task 4 kernels are no-ops, but the
	// descriptor set is wired so Tasks 5/6 only fill in kernel bodies.
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, radiance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, distance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, metadata_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, radiance_atlas[write_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, distance_atlas[write_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, metadata_atlas[write_index]));

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

	// Mode 0: scroll/recenter the cache into the write atlas over the full atlas.
	push_constant.mode = 0u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);

	// Mode 1: accumulate this frame's probe rays into the write atlas.
	push_constant.mode = 1u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);
	RD::get_singleton()->compute_list_end();

	// Ping-pong: the freshly written back atlases become the new front.
	read_index = write_index;
}
