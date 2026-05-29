/**************************************************************************/
/*  rtgi_diffuse_cache.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_diffuse_cache.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGIDiffuseCache::RTGIDiffuseCache() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIDiffuseCache::~RTGIDiffuseCache() {
	shader.version_free(shader_version);
}

Size2i RTGIDiffuseCache::_cache_size(const Size2i &p_output_size, uint32_t p_max_cache_entries) const {
	ERR_FAIL_COND_V(p_output_size.x <= 0 || p_output_size.y <= 0, Size2i());

	const uint32_t max_entries = MIN(4194304u, MAX(4096u, p_max_cache_entries));
	const uint64_t output_pixels = (uint64_t)p_output_size.x * (uint64_t)p_output_size.y;
	if (output_pixels <= max_entries) {
		return p_output_size;
	}

	const double scale = Math::sqrt((double)max_entries / (double)output_pixels);
	Size2i cache_size(
			MAX(1, (int32_t)Math::floor((double)p_output_size.x * scale)),
			MAX(1, (int32_t)Math::floor((double)p_output_size.y * scale)));
	while ((uint64_t)cache_size.x * (uint64_t)cache_size.y > max_entries) {
		if (cache_size.x >= cache_size.y && cache_size.x > 1) {
			cache_size.x--;
		} else if (cache_size.y > 1) {
			cache_size.y--;
		} else {
			break;
		}
	}
	return cache_size;
}

bool RTGIDiffuseCache::_ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_output_size, const Size2i &p_cache_size) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_output_size.x <= 0 || p_output_size.y <= 0, false);
	ERR_FAIL_COND_V(p_cache_size.x <= 0 || p_cache_size.y <= 0, false);

	const bool has_radiance = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE);
	const bool has_output = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT);

	bool needs_clear = false;
	if (has_radiance &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, 0) != p_cache_size) {
		needs_clear = true;
	}
	if (has_output &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, 0) != p_output_size) {
		needs_clear = true;
	}

	const bool has_complete_context = has_radiance &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT) &&
			has_output &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION);

	if ((has_radiance || has_output) && !has_complete_context) {
		needs_clear = true;
	}

	if (needs_clear) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI_DIFFUSE_CACHE);
	}

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE)) {
		return false;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	RID meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	RID stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID diagnostic = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID age = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID rejection = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);

	RD::get_singleton()->texture_clear(radiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(meta, Color(0.5, 0.5, 1.0, 65504.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(stats, Color(65504.0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(diagnostic, Color(0, 0, 0, 1), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(age, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(rejection, Color(1, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());

	return true;
}

void RTGIDiffuseCache::process(Ref<RenderSceneBuffersRD> p_render_buffers,
		RID p_diffuse_radiance,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_viewz_hitdist,
		RID p_history_validity,
		RID p_prev_history_validity,
		RID p_history_id,
		RID p_prev_history_id,
		RID p_signal_confidence,
		const Size2i &p_process_size,
		uint32_t p_max_cache_entries,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_diffuse_radiance.is_valid() || !p_velocity.is_valid() || !p_normal_roughness.is_valid() || !p_viewz_hitdist.is_valid());
	ERR_FAIL_COND(!p_history_validity.is_valid() || !p_prev_history_validity.is_valid() || !p_history_id.is_valid() || !p_prev_history_id.is_valid() || !p_signal_confidence.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	const Size2i cache_size = _cache_size(p_process_size, p_max_cache_entries);
	const bool reset_history = _ensure_buffers(p_render_buffers, p_process_size, cache_size);

	RID radiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, p_view, 0);
	RID radiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT, p_view, 0);
	RID meta = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META, p_view, 0);
	RID meta_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT, p_view, 0);
	RID stats = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS, p_view, 0);
	RID stats_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT, p_view, 0);
	RID output = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, p_view, 0);
	RID raw = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW, p_view, 0);
	RID diagnostic = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC, p_view, 0);
	RID age = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE, p_view, 0);
	RID rejection = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION, p_view, 0);

	RD::get_singleton()->texture_copy(p_diffuse_radiance, raw, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	RID velocity_slice = p_velocity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_velocity"), p_view, 0) : RID();
	RID normal_roughness_slice = p_normal_roughness.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_normal_roughness"), p_view, 0) : RID();
	RID viewz_hitdist_slice = p_viewz_hitdist.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_viewz_hitdist"), p_view, 0) : RID();
	RID history_validity_slice = p_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity"), p_view, 0) : RID();
	RID prev_history_validity_slice = p_prev_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity_prev"), p_view, 0) : RID();
	RID history_id_slice = p_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id"), p_view, 0) : RID();
	RID prev_history_id_slice = p_prev_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id_prev"), p_view, 0) : RID();
	RID signal_confidence_slice = p_signal_confidence.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_signal_confidence"), p_view, 0) : RID();

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, raw));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, velocity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, normal_roughness_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, viewz_hitdist_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, history_validity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, prev_history_validity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, history_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, prev_history_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ nearest_sampler, signal_confidence_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, radiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, meta_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, stats_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, diagnostic));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, age));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, rejection));

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	push_constant.cache_width = (float)cache_size.x;
	push_constant.cache_height = (float)cache_size.y;
	push_constant.max_history = reset_history ? 1.0f : 48.0f;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 0u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, cache_size.x, cache_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(radiance_next, radiance, Vector3(), Vector3(), Vector3(cache_size.x, cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(meta_next, meta, Vector3(), Vector3(), Vector3(cache_size.x, cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(stats_next, stats, Vector3(), Vector3(), Vector3(cache_size.x, cache_size.y, 1), 0, 0, 0, 0);

	compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 1u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_process_size.x, p_process_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(output, p_diffuse_radiance, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, p_view);
}
