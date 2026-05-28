/**************************************************************************/
/*  rtgi_spatiotemporal_radiance_cache.cpp                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_spatiotemporal_radiance_cache.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGISpatioTemporalRadianceCache::RTGISpatioTemporalRadianceCache() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGISpatioTemporalRadianceCache::~RTGISpatioTemporalRadianceCache() {
	if (ray_result_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ray_result_buffer);
		ray_result_buffer = RID();
	}
	shader.version_free(shader_version);
}

Size2i RTGISpatioTemporalRadianceCache::_atlas_size(uint32_t p_cascade_count, uint32_t p_grid_size) const {
	const uint32_t grid = CLAMP(p_grid_size, 12u, 32u);
	const uint32_t cascades = CLAMP(p_cascade_count, 1u, 4u);
	return Size2i((int32_t)(grid * 8u), (int32_t)(grid * grid * cascades * 8u));
}

bool RTGISpatioTemporalRadianceCache::_ensure_ray_result_buffer(uint32_t p_rays_per_frame) {
	const uint32_t capacity = MAX(1u, p_rays_per_frame);
	if (ray_result_buffer.is_valid() && ray_result_capacity >= capacity) {
		return false;
	}
	if (ray_result_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ray_result_buffer);
		ray_result_buffer = RID();
	}

	const uint32_t result_stride = sizeof(float) * 12u;
	ray_result_buffer = RD::get_singleton()->storage_buffer_create(uint64_t(capacity) * result_stride);
	RD::get_singleton()->set_resource_name(ray_result_buffer, "RTGI STRC Probe Ray Results");
	ray_result_capacity = capacity;
	return true;
}

bool RTGISpatioTemporalRadianceCache::ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_cascade_count, uint32_t p_grid_size, uint32_t p_rays_per_frame, uint64_t p_signature) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);

	const Size2i atlas_size = _atlas_size(p_cascade_count, p_grid_size);
	const bool size_mismatch = p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE) &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE, 0) != atlas_size;
	const bool missing = !p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE_NEXT) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE_NEXT) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA_NEXT) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_RADIANCE_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_CONFIDENCE_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_UPDATES_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VISIBILITY_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_AGE_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VARIANCE_DEBUG) ||
			!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_REJECTION_DEBUG);

	bool reset = false;
	if (size_mismatch || missing || resource_signature != p_signature) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI_STRC);
		resource_signature = p_signature;
		reset = true;
	}

	_ensure_ray_result_buffer(p_rays_per_frame);

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE)) {
		return reset;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID irradiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID distance = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID metadata = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID radiance_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_RADIANCE_DEBUG, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID confidence_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_CONFIDENCE_DEBUG, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID updates_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_UPDATES_DEBUG, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID visibility_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VISIBILITY_DEBUG, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID age_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_AGE_DEBUG, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID variance_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VARIANCE_DEBUG, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);
	RID rejection_debug = p_render_buffers->create_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_REJECTION_DEBUG, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, atlas_size);

	RD::get_singleton()->texture_clear(irradiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(distance, Color(65504.0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(metadata, Color(65504.0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(radiance_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(confidence_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(updates_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(visibility_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(age_debug, Color(1, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(variance_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(rejection_debug, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());

	return true;
}

void RTGISpatioTemporalRadianceCache::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_cascade_count, uint32_t p_grid_size, uint32_t p_rays_per_frame, float p_temporal_weight, uint32_t p_frame_index, const Vector3i *p_cascade_scroll, bool p_scroll_valid, uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(!ray_result_buffer.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());
	ERR_FAIL_COND(!p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE));

	const Size2i atlas_size = _atlas_size(p_cascade_count, p_grid_size);
	RID irradiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE, p_view, 0);
	RID irradiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE_NEXT, p_view, 0);
	RID distance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE, p_view, 0);
	RID distance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE_NEXT, p_view, 0);
	RID metadata = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA, p_view, 0);
	RID metadata_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA_NEXT, p_view, 0);
	RID radiance_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_RADIANCE_DEBUG, p_view, 0);
	RID confidence_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_CONFIDENCE_DEBUG, p_view, 0);
	RID updates_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_UPDATES_DEBUG, p_view, 0);
	RID visibility_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VISIBILITY_DEBUG, p_view, 0);
	RID age_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_AGE_DEBUG, p_view, 0);
	RID variance_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_VARIANCE_DEBUG, p_view, 0);
	RID rejection_debug = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_REJECTION_DEBUG, p_view, 0);

	RD::get_singleton()->texture_clear(updates_debug, Color(0, 0, 0, 0), 0, 1, 0, 1);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, ray_result_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, irradiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, distance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, metadata_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, radiance_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, confidence_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, updates_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, visibility_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, age_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, variance_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rejection_debug));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, irradiance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, distance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, metadata));

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.ray_count = MIN(p_rays_per_frame, ray_result_capacity);
	push_constant.grid_size = CLAMP(p_grid_size, 12u, 32u);
	push_constant.cascade_count = CLAMP(p_cascade_count, 1u, 4u);
	push_constant.frame_index = p_frame_index;
	push_constant.temporal_weight = CLAMP(p_temporal_weight, 0.0f, 0.995f);
	push_constant.scroll_valid = p_scroll_valid ? 1u : 0u;
	for (uint32_t i = 0; i < 4; i++) {
		const Vector3i scroll = p_cascade_scroll ? p_cascade_scroll[i] : Vector3i();
		push_constant.cascade_scroll[i][0] = scroll.x;
		push_constant.cascade_scroll[i][1] = scroll.y;
		push_constant.cascade_scroll[i][2] = scroll.z;
	}

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 0u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, atlas_size.x, atlas_size.y, 1);
	push_constant.mode = 1u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, push_constant.ray_count, 1, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(irradiance_next, irradiance, Vector3(), Vector3(), Vector3(atlas_size.x, atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(distance_next, distance, Vector3(), Vector3(), Vector3(atlas_size.x, atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(metadata_next, metadata, Vector3(), Vector3(), Vector3(atlas_size.x, atlas_size.y, 1), 0, 0, 0, 0);
}
