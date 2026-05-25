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

bool RTGIDiffuseCache::_ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, false);

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE) &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, 0) != p_size) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI_DIFFUSE_CACHE);
	}

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE) &&
			(!p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC) ||
					!p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW) ||
					!p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE) ||
					!p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION))) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI_DIFFUSE_CACHE);
	}

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE)) {
		return false;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID diagnostic = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID age = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID rejection = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);

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
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_diffuse_radiance.is_valid() || !p_velocity.is_valid() || !p_normal_roughness.is_valid() || !p_viewz_hitdist.is_valid());
	ERR_FAIL_COND(!p_history_validity.is_valid() || !p_prev_history_validity.is_valid() || !p_history_id.is_valid() || !p_prev_history_id.is_valid() || !p_signal_confidence.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	const bool reset_history = _ensure_buffers(p_render_buffers, p_process_size);

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
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_diffuse_radiance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_velocity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_normal_roughness })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_viewz_hitdist })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_history_validity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_prev_history_validity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, p_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, p_prev_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ nearest_sampler, p_signal_confidence })));
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
	push_constant.max_history = reset_history ? 1.0f : 48.0f;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_process_size.x, p_process_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(output, p_diffuse_radiance, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, p_view);
	RD::get_singleton()->texture_copy(radiance_next, radiance, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(meta_next, meta, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(stats_next, stats, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
}
