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

// Mode selectors. PLACE (0) is the placement shader; REPROJECT (1) + BLEND (2) are
// the accumulate shader's two modes. These EXACTLY match the SPG_MODE_* defines in
// the respective GLSL shaders.
#define SPG_MODE_PLACE 0u
#define SPG_MODE_REPROJECT 1u
#define SPG_MODE_BLEND 2u

RTGIScreenProbeGather::RTGIScreenProbeGather() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	// Temporal-accumulate shader (separate set-0 layout from PLACE; see the header).
	accum_shader.initialize({ "" });
	accum_shader_version = accum_shader.version_create();
	accum_pipeline = RD::get_singleton()->compute_pipeline_create(accum_shader.version_get_shader(accum_shader_version, 0));
}

RTGIScreenProbeGather::~RTGIScreenProbeGather() {
	free_resources();
	shader.version_free(shader_version);
	accum_shader.version_free(accum_shader_version);
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
	// THE FRAME SWAP: flip read_index here, BEFORE writing any headers, so this
	// frame's placement/gather treat read_index as "current" and the accumulate reads
	// 1 - read_index as "previous". Both ping-pong sets are allocated together, so the
	// swapped index is valid the very first frame (it just reads a cleared prev set).
	read_index = 1u - read_index;

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

	// Placement writes THIS frame's (current) header set. read_index was already
	// flipped at the top of this function (the frame swap), so get_header_*() returns
	// what PLACE writes here, and the accumulate reads the now-previous (1 - read_index)
	// set against it.
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

void RTGIScreenProbeGather::run_accumulate(const SpgFrameParams &p_frame) {
	// resources_valid is set only at the end of _allocate() (which free_resources()es
	// first), so it guarantees all six ping-pong textures + headers were allocated
	// together; spot-checking radiance_atlas[read_index] is therefore sufficient.
	if (!resources_valid || !radiance_atlas[read_index].is_valid()) {
		return;
	}
	// The ray-result SSBO is bound unconditionally (the BLEND mode reads it) and is
	// sized to >= 1 entry by ensure_ray_result_buffer(), so a missing buffer means
	// run_accumulate() was called before that ran -- bail rather than build an
	// incomplete descriptor set (mirrors the WRC update guard).
	ERR_FAIL_COND(!ray_result_buffer.is_valid());

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rd = accum_shader.version_get_shader(accum_shader_version, 0);

	// read_index is THIS frame's (current) set; 1 - read_index is the previous frame's
	// (the swap happened in run_placement). The accumulate reads the previous radiance
	// + headers and writes the current radiance atlas. NO swap here.
	const uint32_t prev_index = 1u - read_index;

	// Set 0 mirrors rtgi_spg_accumulate.glsl's layout: radiance_prev(0, readonly) +
	// radiance_cur(1) + header_plane_cur(2) + header_aux_cur(3) + header_plane_prev(4)
	// + header_aux_prev(5) as storage images, and the gather ray-result SSBO(6).
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, radiance_atlas[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, radiance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, header_plane[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, header_aux[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, header_plane[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, header_aux[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, ray_result_buffer));

	AccumPushConstant push_constant;
	memset(&push_constant, 0, sizeof(AccumPushConstant));
	push_constant.grid_w = (uint32_t)grid_size.x;
	push_constant.grid_h = (uint32_t)grid_size.y;
	push_constant.oct_res = (uint32_t)MAX(cached_params.oct_res, 1);
	push_constant.spacing_f = (uint32_t)MAX(cached_params.spacing_f, 1);
	push_constant.frame_index = p_frame.frame_index;
	push_constant.atlas_width = (uint32_t)atlas_size.x;
	push_constant.atlas_height = (uint32_t)atlas_size.y;
	push_constant.rays_this_frame = p_frame.rays_this_frame;
	push_constant.temporal_n_cap = MAX(cached_params.temporal_n_cap, 1.0f);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, accum_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);

	// REPROJECT: one thread per radiance-atlas texel. Always dispatched -- with zero
	// motion it is the identity carry (prev_probe == probe, same normal -> re-orient
	// is identity) the BLEND then accumulates onto; on a disocclusion/miss it resets
	// the texel to count 0 so BLEND takes a fresh sample.
	push_constant.mode = SPG_MODE_REPROJECT;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(AccumPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);

	// BLEND: one thread per gather ray (1D dispatch). Each ray maps to a distinct
	// (probe, dir) atlas texel and folds its radiance into the reprojected history
	// with a 1/n weight. Skipped when no rays were traced this frame (REPROJECT still
	// produced the carried-forward atlas the debug blit reads).
	if (push_constant.rays_this_frame > 0u) {
		push_constant.mode = SPG_MODE_BLEND;
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(AccumPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, MAX(push_constant.rays_this_frame, 1u), 1, 1);
	}
	RD::get_singleton()->compute_list_end();
	// NO ping-pong swap: the frame swap is done in run_placement.
}
