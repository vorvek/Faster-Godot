/**************************************************************************/
/*  rtgi_screen_probe_gather.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_screen_probe_gather.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Mode selectors. PLACE (0) is the placement shader; REPROJECT (1) + BLEND (2) +
// SPATIAL (3) are the accumulate shader's three modes. These EXACTLY match the
// SPG_MODE_* defines in the respective GLSL shaders.
#define SPG_MODE_PLACE 0u
#define SPG_MODE_REPROJECT 1u
#define SPG_MODE_BLEND 2u
#define SPG_MODE_SPATIAL 3u

RTGIScreenProbeGather::RTGIScreenProbeGather() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	// Temporal-accumulate shader (separate set-0 layout from PLACE; see the header).
	accum_shader.initialize({ "" });
	accum_shader_version = accum_shader.version_create();
	accum_pipeline = RD::get_singleton()->compute_pipeline_create(accum_shader.version_get_shader(accum_shader_version, 0));

	// SPG-GI debug consumer shader (A2-T5; separate set-0 layout again -- see header).
	// First compile of rtgi_spg_gi_consumer.glsl (and thus of its rtgi_spg_inc.glsl /
	// oct_inc.glsl include usage in a consumer context), so a GLSL error there trips here.
	gi_debug_shader.initialize({ "" });
	gi_debug_shader_version = gi_debug_shader.version_create();
	gi_debug_pipeline = RD::get_singleton()->compute_pipeline_create(gi_debug_shader.version_get_shader(gi_debug_shader_version, 0));
}

RTGIScreenProbeGather::~RTGIScreenProbeGather() {
	free_resources();
	shader.version_free(shader_version);
	accum_shader.version_free(accum_shader_version);
	gi_debug_shader.version_free(gi_debug_shader_version);
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
	if (radiance_filtered.is_valid()) {
		RD::get_singleton()->free_rid(radiance_filtered);
		radiance_filtered = RID();
	}
	if (place_ubo.is_valid()) {
		RD::get_singleton()->free_rid(place_ubo);
		place_ubo = RID();
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

	// SPATIAL output (A2-T4): same format/size/usage as the radiance atlas, but a single
	// (non-ping-pong) texture -- the SPATIAL pass of run_accumulate fully rewrites every
	// texel each frame from the current atlas, so no history set is needed.
	radiance_filtered = RD::get_singleton()->texture_create(radiance_format, RD::TextureView());
	RD::get_singleton()->set_resource_name(radiance_filtered, "RTGI SPG Radiance Filtered");
	RD::get_singleton()->texture_clear(radiance_filtered, Color(0, 0, 0, 0), 0, 1, 0, 1);

	read_index = 0;
	resources_valid = true;

	// Log total VRAM. RGBA16F = 8 B/texel, RGBA32F = 16 B/texel; radiance atlas and
	// both headers are ping-ponged (x2), plus one non-ping-pong filtered atlas (A2-T4).
	const uint64_t atlas_texels = uint64_t(atlas_size.x) * uint64_t(atlas_size.y);
	const uint64_t header_texels = uint64_t(grid_size.x) * uint64_t(grid_size.y);
	const uint64_t bytes_per_set = atlas_texels * 8ULL + header_texels * (16ULL + 8ULL); // radiance(16F)=8; per probe: header_plane(32F)=16 + header_aux(16F)=8.
	const uint64_t total_bytes = bytes_per_set * 2ULL + atlas_texels * 8ULL; // x2 ping-pong sets + filtered atlas(16F)=8.
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

void RTGIScreenProbeGather::run_placement(Ref<RenderSceneBuffersRD> p_rb, RID p_depth, RID p_normal_roughness, RID p_velocity, const SpgFrameParams &p_frame, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const Size2i &p_render_size, const Projection &p_prev_cam_projection, const Transform3D &p_prev_cam_transform) {
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
	// Previous-frame world -> clip (prev_proj * prev_view) so PLACE can camera-reproject the static
	// geometry the velocity buffer leaves at the (-1,-1) sentinel. Jittered, matching the velocity-
	// buffer convention so static and dynamic anchors reproject consistently.
	const Projection prev_view_projection = p_prev_cam_projection * Projection(p_prev_cam_transform.affine_inverse());
	MaterialStorage::store_camera(prev_view_projection, ubo.prev_view_projection);
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

void RTGIScreenProbeGather::run_accumulate(const SpgFrameParams &p_frame, const WrcSeedInputs &p_wrc_seed) {
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
	// + header_aux_prev(5) as storage images, the gather ray-result SSBO(6), and the
	// SPATIAL output radiance_filtered(7). A single GLSL shader's set-0 layout must
	// declare every binding any dispatch binds, so ONE uniform set (0-7) drives all
	// three modes -- REPROJECT/BLEND just don't write binding 7; SPATIAL writes it.
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, radiance_atlas[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, radiance_atlas[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, header_plane[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, header_aux[read_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, header_plane[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, header_aux[prev_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, ray_result_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, radiance_filtered));

	// WRC atlases for the cold-start seed (binding 8/9). Bound as sampler+texture
	// (bilinear, clamp), matching the SPG gather's WRC taps. Fall back to a default-black
	// texture when the WRC is unavailable so the descriptor set is always complete; the
	// shader keys the actual seed on seed_samples (set to 0 below in that case).
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID default_black = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	const bool wrc_available = p_wrc_seed.radiance_atlas.is_valid() && p_wrc_seed.distance_atlas.is_valid();
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 8;
		u.append_id(linear_sampler);
		u.append_id(wrc_available ? p_wrc_seed.radiance_atlas : default_black);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 9;
		u.append_id(linear_sampler);
		u.append_id(wrc_available ? p_wrc_seed.distance_atlas : default_black);
		uniforms.push_back(u);
	}

	// Env override for live tuning (read once), mirroring the FPT_TAA_* knobs. Set
	// RTGI_SPG_WRC_SEED_SAMPLES before launch to sweep seed strength without rebuilding.
	static const float s_seed_override = []() -> float {
		if (OS::get_singleton()->has_environment("RTGI_SPG_WRC_SEED_SAMPLES")) {
			return OS::get_singleton()->get_environment("RTGI_SPG_WRC_SEED_SAMPLES").to_float();
		}
		return -1.0f; // sentinel: not set.
	}();
	const float seed_samples = wrc_available ? ((s_seed_override >= 0.0f) ? s_seed_override : p_wrc_seed.seed_samples) : 0.0f;

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
	// SPATIAL filter radius (A2-T4). Clamped >= 0 here; the shader additionally caps it
	// to a sane neighbor reach. Radius 0 -> SPATIAL is a straight copy of the atlas.
	push_constant.spatial_radius = (uint32_t)MAX(cached_params.spatial_radius, 0);
	push_constant.wrc_cascade_count = (uint32_t)MAX(p_wrc_seed.cascade_count, 1);
	push_constant.wrc_grid = (uint32_t)MAX(p_wrc_seed.grid, 1);
	push_constant.wrc_oct_res = (uint32_t)MAX(p_wrc_seed.oct_res, 1);
	push_constant.wrc_base_spacing = MAX(p_wrc_seed.base_spacing, 0.25f);
	push_constant.wrc_cam_x = p_wrc_seed.camera_pos.x;
	push_constant.wrc_cam_y = p_wrc_seed.camera_pos.y;
	push_constant.wrc_cam_z = p_wrc_seed.camera_pos.z;
	push_constant.seed_samples = seed_samples;

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
	// produced the carried-forward atlas the SPATIAL pass + debug blit read).
	if (push_constant.rays_this_frame > 0u) {
		push_constant.mode = SPG_MODE_BLEND;
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(AccumPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, MAX(push_constant.rays_this_frame, 1u), 1, 1);
		// Barrier so SPATIAL reads the fully-blended radiance_cur (not a partial write).
		// Only needed when BLEND ran; with no rays the REPROJECT barrier above already
		// fenced radiance_cur for the SPATIAL read.
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	// SPATIAL (A2-T4): one thread per radiance-atlas texel. Smooth each probe's octahedron
	// against its 3x3 same-surface neighbor probes (plane-matched, re-oriented on read) of
	// the just-accumulated radiance_cur into radiance_filtered. ALWAYS dispatched -- with
	// spatial_radius 0 the shader copies radiance_cur straight through, so radiance_filtered
	// is a valid consumer target every frame (A3 + the debug-integrate read it).
	push_constant.mode = SPG_MODE_SPATIAL;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(AccumPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);

	RD::get_singleton()->compute_list_end();
	// NO ping-pong swap: the frame swap is done in run_placement.
}

void RTGIScreenProbeGather::_ensure_gi_debug_image(const Size2i &p_size) {
	const Size2i wanted = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));
	if (gi_debug_image.is_valid() && gi_debug_image_size == wanted) {
		return;
	}
	if (gi_debug_image.is_valid()) {
		RD::get_singleton()->free_rid(gi_debug_image);
		gi_debug_image = RID();
	}

	RD::TextureFormat tf;
	// RGBA16F to hold linear, un-tonemapped incident radiance (.rgb) + a coverage flag
	// (.a) without clipping; STORAGE (the consumer writes it) + SAMPLING (the blit reads
	// it through copy_to_fb_rect).
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = wanted.x;
	tf.height = wanted.y;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	gi_debug_image = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(gi_debug_image, "RTGI SPG GI Debug Image");
	gi_debug_image_size = wanted;
}

void RTGIScreenProbeGather::render_gi_debug(Ref<RenderSceneBuffersRD> p_rb, RID p_depth, RID p_normal_roughness, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const Size2i &p_size, float p_strength, RID p_dest_fb) {
	// get_radiance_filtered() falls back to the current atlas until radiance_filtered
	// allocates, so guarding resources_valid + the headers covers every texture this
	// consumer binds (all allocated together in _allocate).
	if (!resources_valid || !header_plane[read_index].is_valid() || !header_aux[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_depth.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_dest_fb.is_null());

	const Size2i size = Size2i(MAX(p_size.x, 1), MAX(p_size.y, 1));
	_ensure_gi_debug_image(size);

	// The consumer's params live in a UBO (bound at set 0, binding 6), not a push
	// constant: SpgGiUBO is 160 bytes and the two mat4s alone already hit the 128-byte
	// MAX_PUSH_CONSTANT_SIZE cap. Created once, reused every frame.
	if (gi_debug_ubo.is_null()) {
		gi_debug_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(SpgGiUBO));
		RD::get_singleton()->set_resource_name(gi_debug_ubo, "RTGI SPG GI Debug Params UBO");
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = gi_debug_shader.version_get_shader(gi_debug_shader_version, 0);
	// Linear sampler, clamp-to-edge. The consumer only does integer texelFetch()es, but
	// the atlas/headers/G-buffers are bound as SAMPLER_WITH_TEXTURE (mirroring the WRC
	// consumer + PLACE), so a sampler is required; clamp-to-edge is safe for the unused
	// filtering.
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// Set 0: SPATIAL-filtered radiance atlas + the two probe headers (sampler+texture),
	// depth + normal-roughness G-buffers (sampler+texture), the output storage image,
	// and the params UBO. Sourcing get_radiance_filtered() matches the SPG_RADIANCE blit
	// + what A3 will consume.
	LocalVector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(linear_sampler);
		u.append_id(get_radiance_filtered());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(linear_sampler);
		u.append_id(header_plane[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(linear_sampler);
		u.append_id(header_aux[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 3;
		u.append_id(linear_sampler);
		u.append_id(p_depth);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 4;
		u.append_id(linear_sampler);
		u.append_id(p_normal_roughness);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 5;
		u.append_id(gi_debug_image);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 6;
		u.append_id(gi_debug_ubo);
		uniforms.push_back(u);
	}

	SpgGiUBO ubo;
	memset(&ubo, 0, sizeof(SpgGiUBO));
	MaterialStorage::store_camera(p_inv_projection, ubo.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, ubo.inv_view);
	ubo.screen_width = size.x;
	ubo.screen_height = size.y;
	ubo.grid_w = grid_size.x;
	ubo.grid_h = grid_size.y;
	ubo.spacing_f = MAX(cached_params.spacing_f, 1);
	ubo.oct_res = MAX(cached_params.oct_res, 1);
	ubo.strength = p_strength;
	RD::get_singleton()->buffer_update(gi_debug_ubo, 0, sizeof(SpgGiUBO), &ubo);

	RD::get_singleton()->draw_command_begin_label("RTGI SPG GI Debug");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, gi_debug_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	// No push constant: the params are supplied via gi_debug_ubo (binding 6), bound in
	// the uniform set above and updated via buffer_update() this frame.
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	// Blit the raw linear incident radiance to the destination framebuffer. No sRGB, no
	// LOG_LUMINANCE: keep values linear/measurable for the A2-T6 furnace gate (mirrors
	// the WRC-GI consumer blit + the SPG_RADIANCE raw-linear blit).
	CopyEffects *copy_effects = CopyEffects::get_singleton();
	ERR_FAIL_NULL(copy_effects);
	const bool multiview = p_rb.is_valid() && p_rb->get_view_count() > 1;
	copy_effects->copy_to_fb_rect(gi_debug_image, p_dest_fb, Rect2i(Point2i(), size), false, false, false, false, RID(), multiview, false, false, false, Rect2(), 1.0, true, CopyEffects::COPY_TO_FB_FLAG_MODE_NONE);
}
