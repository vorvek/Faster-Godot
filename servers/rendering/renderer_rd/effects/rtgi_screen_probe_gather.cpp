/**************************************************************************/
/*  rtgi_screen_probe_gather.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_screen_probe_gather.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// PLACE-pass mode selector (matches SPG_MODE_PLACE in the shader). ACCUM / SPATIAL
// are added by later tasks; only PLACE is dispatched in T1.
#define SPG_MODE_PLACE 0u

RTGIScreenProbeGather::RTGIScreenProbeGather() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIScreenProbeGather::~RTGIScreenProbeGather() {
	free_resources();
	shader.version_free(shader_version);
}

bool RTGIScreenProbeGather::ensure_ray_result_buffer(uint32_t p_rays_per_frame) {
	const uint32_t capacity = MAX(1u, p_rays_per_frame);
	if (ray_result_buffer.is_valid() && ray_result_capacity >= capacity) {
		return false;
	}
	if (ray_result_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ray_result_buffer);
		ray_result_buffer = RID();
	}

	// 2 x vec4 = 32 bytes per entry. Consumed by the SPG gather kernel (T2); the
	// stride is pinned here so the GLSL ray-result struct and T2's gather match.
	const uint32_t result_stride = sizeof(float) * 8u;
	ray_result_buffer = RD::get_singleton()->storage_buffer_create(uint64_t(capacity) * result_stride);
	RD::get_singleton()->set_resource_name(ray_result_buffer, "RTGI SPG Gather Ray Results");
	ray_result_capacity = capacity;
	return true;
}

void RTGIScreenProbeGather::free_resources() {
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
		if (header_plane[i].is_valid()) {
			RD::get_singleton()->free_rid(header_plane[i]);
			header_plane[i] = RID();
		}
		if (header_aux[i].is_valid()) {
			RD::get_singleton()->free_rid(header_aux[i]);
			header_aux[i] = RID();
		}
	}
	if (place_ubo.is_valid()) {
		RD::get_singleton()->free_rid(place_ubo);
		place_ubo = RID();
	}
	read_index = 0;
	atlas_size = Size2i();
	grid_size = Size2i();
	cached_render_size = Size2i();
	resources_valid = false;
}

void RTGIScreenProbeGather::_allocate(const SpgParams &p_params, const Size2i &p_render_size) {
	free_resources();

	const int spacing_f = MAX(p_params.spacing_f, 1);
	const int oct_res = MAX(p_params.oct_res, 1);
	const int render_w = MAX(p_render_size.x, 1);
	const int render_h = MAX(p_render_size.y, 1);

	// One probe per spacing_f x spacing_f tile; ceil so an edge tile shorter than a
	// full F still gets a probe (mirrors the Math::ceil grid math at the dispatch
	// site). The header textures are one texel per probe; the radiance atlas packs
	// each probe's oct_res x oct_res octahedral tile.
	const int grid_w = (render_w + spacing_f - 1) / spacing_f;
	const int grid_h = (render_h + spacing_f - 1) / spacing_f;
	grid_size = Size2i(MAX(grid_w, 1), MAX(grid_h, 1));
	atlas_size = Size2i(grid_size.x * oct_res, grid_size.y * oct_res);
	cached_params = p_params;
	cached_render_size = Size2i(render_w, render_h);

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RD::TextureFormat radiance_format;
	radiance_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	radiance_format.width = atlas_size.x;
	radiance_format.height = atlas_size.y;
	radiance_format.usage_bits = usage_bits;

	// Header plane carries the WORLD position (.xyz) + linear depth (.w); full f32
	// so world-space positions across a large scene stay precise (a half-float .w
	// linear depth would quantize badly at distance).
	RD::TextureFormat header_plane_format;
	header_plane_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	header_plane_format.width = grid_size.x;
	header_plane_format.height = grid_size.y;
	header_plane_format.usage_bits = usage_bits;

	// Header aux carries the octahedral world normal (.xy) + screen motion (.zw);
	// half-float is ample for an oct-encoded unit normal and pixel-space motion.
	RD::TextureFormat header_aux_format;
	header_aux_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	header_aux_format.width = grid_size.x;
	header_aux_format.height = grid_size.y;
	header_aux_format.usage_bits = usage_bits;

	for (uint32_t i = 0; i < 2; i++) {
		radiance_atlas[i] = RD::get_singleton()->texture_create(radiance_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(radiance_atlas[i], i == 0 ? "RTGI SPG Radiance Atlas A" : "RTGI SPG Radiance Atlas B");
		header_plane[i] = RD::get_singleton()->texture_create(header_plane_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(header_plane[i], i == 0 ? "RTGI SPG Header Plane A" : "RTGI SPG Header Plane B");
		header_aux[i] = RD::get_singleton()->texture_create(header_aux_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(header_aux[i], i == 0 ? "RTGI SPG Header Aux A" : "RTGI SPG Header Aux B");

		// Clear to "unwritten": radiance/confidence 0, header_plane .w = 0 (<= 0
		// marks an invalid probe per the placement contract), header_aux 0.
		RD::get_singleton()->texture_clear(radiance_atlas[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::get_singleton()->texture_clear(header_plane[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::get_singleton()->texture_clear(header_aux[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	read_index = 0;
	resources_valid = true;

	// Log total VRAM. RGBA16F = 8 B/texel, RGBA32F = 16 B/texel; radiance atlas and
	// both headers are ping-ponged (x2).
	const uint64_t atlas_texels = uint64_t(atlas_size.x) * uint64_t(atlas_size.y);
	const uint64_t header_texels = uint64_t(grid_size.x) * uint64_t(grid_size.y);
	const uint64_t bytes_per_set = atlas_texels * 8ULL + header_texels * (16ULL + 8ULL); // radiance(16F)=8; per probe: header_plane(32F)=16 + header_aux(16F)=8.
	const uint64_t total_bytes = bytes_per_set * 2ULL;
	print_verbose(vformat("RTGI SPG: allocated screen-probe grid %dx%d probes (spacing=%d oct_res=%d, atlas %dx%d) using %.2f MiB VRAM.",
			grid_size.x, grid_size.y, spacing_f, oct_res, atlas_size.x, atlas_size.y,
			double(total_bytes) / (1024.0 * 1024.0)));
}

bool RTGIScreenProbeGather::ensure_resources(Ref<RenderSceneBuffersRD> p_rb, const SpgParams &p_params, const Size2i &p_render_size) {
	ERR_FAIL_COND_V(p_rb.is_null(), false);

	const int spacing_f = MAX(p_params.spacing_f, 1);
	const int render_w = MAX(p_render_size.x, 1);
	const int render_h = MAX(p_render_size.y, 1);
	const Size2i wanted_grid = Size2i(
			MAX((render_w + spacing_f - 1) / spacing_f, 1),
			MAX((render_h + spacing_f - 1) / spacing_f, 1));

	// Realloc only when the grid dimensions or the octahedral resolution change
	// (those drive the texture sizes). A pure spacing change that leaves the ceil'd
	// grid identical does not need new textures.
	const bool layout_changed = grid_size != wanted_grid || cached_params.oct_res != p_params.oct_res;
	if (resources_valid && !layout_changed && radiance_atlas[0].is_valid()) {
		// Keep cached_params/render_size current even on a no-realloc frame so the
		// gather/spatial passes (later tasks) read this frame's tunables.
		cached_params = p_params;
		cached_render_size = Size2i(render_w, render_h);
		return false;
	}

	_allocate(p_params, Size2i(render_w, render_h));
	return true;
}

void RTGIScreenProbeGather::run_placement(Ref<RenderSceneBuffersRD> p_rb, RID p_depth, RID p_normal_roughness, RID p_velocity, const SpgFrameParams &p_frame, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const Size2i &p_render_size) {
	if (!resources_valid || !header_plane[read_index].is_valid() || !header_aux[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_velocity.is_null());

	const Size2i size = Size2i(MAX(p_render_size.x, 1), MAX(p_render_size.y, 1));

	// The PLACE matrices live in a UBO (the two mat4s alone hit the 128-byte
	// MAX_PUSH_CONSTANT_SIZE cap). Created once, reused every frame.
	if (place_ubo.is_null()) {
		place_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(PlaceUBO));
		RD::get_singleton()->set_resource_name(place_ubo, "RTGI SPG Place Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = shader.version_get_shader(shader_version, 0);
	// Linear sampler, clamp-to-edge. PLACE only does integer texelFetch()es, but the
	// G-buffers are bound as SAMPLER_WITH_TEXTURE (mirroring render_gi_debug), so a
	// sampler is required; clamp-to-edge is the safe choice for the unused filtering.
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// T1 writes the FRONT (read) set directly: the ping-pong swap is introduced by
	// the T3 accumulate, so for now write index == read_index and get_header_*()
	// returns what PLACE just wrote this frame.
	const uint32_t write_index = read_index;

	// Set 0: depth + normal-roughness + velocity G-buffers (sampler+texture), the
	// two header storage images PLACE writes, and the PLACE params UBO.
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
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(p_velocity);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 3;
		u.append_id(header_plane[write_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 4;
		u.append_id(header_aux[write_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 5;
		u.append_id(place_ubo);
		uniforms.push_back(u);
	}

	PlaceUBO ubo;
	memset(&ubo, 0, sizeof(PlaceUBO));
	MaterialStorage::store_camera(p_inv_projection, ubo.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, ubo.inv_view);
	ubo.screen_width = size.x;
	ubo.screen_height = size.y;
	RD::get_singleton()->buffer_update(place_ubo, 0, sizeof(PlaceUBO), &ubo);

	const uint32_t grid_w = (uint32_t)grid_size.x;
	const uint32_t grid_h = (uint32_t)grid_size.y;

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.mode = SPG_MODE_PLACE;
	push_constant.grid_w = grid_w;
	push_constant.grid_h = grid_h;
	push_constant.oct_res = (uint32_t)MAX(cached_params.oct_res, 1);
	push_constant.spacing_f = (uint32_t)MAX(cached_params.spacing_f, 1);
	push_constant.frame_index = p_frame.frame_index;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	// One thread per probe (gx, gy).
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, grid_w, grid_h, 1);
	RD::get_singleton()->compute_list_end();
}
