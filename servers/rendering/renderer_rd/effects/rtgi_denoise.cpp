/**************************************************************************/
/*  rtgi_denoise.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_denoise.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGIDenoise::RTGIDenoise() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TEMPORAL");
	modes.push_back("\n#define MODE_VARIANCE_PREFILTER");
	modes.push_back("\n#define MODE_ATROUS");
	modes.push_back("\n#define MODE_COMPOSITE");
	modes.push_back("\n#define MODE_BLOTCH_STABILIZE");
	modes.push_back("\n#define MODE_SPLIT_COMPOSITE");
	modes.push_back("\n#define MODE_FIDELITYFX_COMPOSITE");
	modes.push_back("\n#define MODE_VOLUMETRIC_FOG");

	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

RTGIDenoise::~RTGIDenoise() {
	shader.version_free(shader_version);
}

bool RTGIDenoise::_ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_scope, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, false);

	if (p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY) &&
			p_render_buffers->get_texture_slice_size(p_scope, RB_TEX_RTGI_DENOISE_HISTORY, 0) != p_size) {
		p_render_buffers->clear_context(p_scope);
	}
	if (p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY) &&
			!p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_REACTIVITY)) {
		p_render_buffers->clear_context(p_scope);
	}
	if (p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY) &&
			!p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_PREV_SPECULAR_GUIDE)) {
		p_render_buffers->clear_context(p_scope);
	}

	if (p_render_buffers->has_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY)) {
		return false;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID history = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID noisy = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_NOISY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID moments = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_MOMENTS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_TEMP_A, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_TEMP_B, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_TEMP_C, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID variance = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_VARIANCE, RD::DATA_FORMAT_R16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID history_length = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_HISTORY_LENGTH, RD::DATA_FORMAT_R16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID rejection = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_REJECTION, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID reactivity = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_REACTIVITY, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID prev_normal_roughness = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_PREV_NORMAL_ROUGHNESS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID prev_viewz_hitdist = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_PREV_VIEWZ_HITDIST, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID prev_albedo_metalness = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_PREV_ALBEDO_METALNESS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);
	RID prev_specular_guide = p_render_buffers->create_texture(p_scope, RB_TEX_RTGI_DENOISE_PREV_SPECULAR_GUIDE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_size);

	RD::get_singleton()->texture_clear(history, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(noisy, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(moments, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(variance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(history_length, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(rejection, Color(1, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(reactivity, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(prev_normal_roughness, Color(0.5, 0.5, 1.0, 1.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(prev_viewz_hitdist, Color(65504.0, 65504.0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(prev_albedo_metalness, Color(1, 1, 1, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(prev_specular_guide, Color(1, 65504.0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());

	return true;
}

void RTGIDenoise::_dispatch_temporal(const PushConstant &p_push_constant, RID p_source, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_specular_guide, RID p_prev_specular_guide, RID p_specular_reprojection, RID p_history, RID p_moments, RID p_prev_normal_roughness, RID p_prev_viewz_hitdist, RID p_prev_albedo_metalness, RID p_history_validity, RID p_prev_history_validity, RID p_history_id, RID p_prev_history_id, RID p_temporal_out, RID p_moments_out, RID p_variance_out, RID p_rejection_out, RID p_reactivity_out, RID p_history_length_out) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_TEMPORAL);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_source));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_velocity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_normal_roughness })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_albedo_metalness })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_viewz_hitdist })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ linear_sampler, p_history })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ linear_sampler, p_moments })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_prev_normal_roughness })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_prev_viewz_hitdist })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, p_prev_albedo_metalness })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, p_history_validity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ nearest_sampler, p_prev_history_validity })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ nearest_sampler, p_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, Vector<RID>({ nearest_sampler, p_prev_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, p_temporal_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, p_moments_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, p_variance_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, p_rejection_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, p_reactivity_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, p_history_length_out));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 20, Vector<RID>({ nearest_sampler, p_specular_guide })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 21, Vector<RID>({ nearest_sampler, p_prev_specular_guide })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 22, Vector<RID>({ nearest_sampler, p_specular_reprojection })));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_variance_prefilter(const PushConstant &p_push_constant, RID p_temporal, RID p_normal_roughness, RID p_viewz_hitdist, RID p_variance, RID p_reactivity, RID p_prefilter_out) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_VARIANCE_PREFILTER);

	RD::Uniform u_temporal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_temporal }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_variance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_variance }));
	RD::Uniform u_reactivity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_reactivity }));
	RD::Uniform u_prefilter_out(RD::UNIFORM_TYPE_IMAGE, 5, p_prefilter_out);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_VARIANCE_PREFILTER]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_temporal, u_normal_roughness, u_viewz_hitdist, u_variance, u_reactivity, u_prefilter_out), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_atrous(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_specular_guide, RID p_history_id, RID p_output) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_ATROUS);

	RD::Uniform u_input(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_input }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_albedo_metalness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_albedo_metalness }));
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_velocity }));
	RD::Uniform u_reactivity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_reactivity }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 6, p_output);
	RD::Uniform u_specular_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_specular_guide }));
	RD::Uniform u_history_id(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_history_id }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_ATROUS]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_input, u_normal_roughness, u_albedo_metalness, u_viewz_hitdist, u_velocity, u_reactivity, u_output, u_specular_guide, u_history_id), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_composite(const PushConstant &p_push_constant, RID p_filtered, RID p_temporal, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_reactivity, RID p_specular_guide, RID p_history_id, RID p_output) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_COMPOSITE);

	RD::Uniform u_filtered(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_filtered }));
	RD::Uniform u_temporal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_temporal }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_albedo_metalness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_albedo_metalness }));
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_reactivity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_reactivity }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 6, p_output);
	RD::Uniform u_specular_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_specular_guide }));
	RD::Uniform u_history_id(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_history_id }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_COMPOSITE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_filtered, u_temporal, u_normal_roughness, u_albedo_metalness, u_viewz_hitdist, u_reactivity, u_output, u_specular_guide, u_history_id), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_blotch_stabilize(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_output) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_BLOTCH_STABILIZE);

	RD::Uniform u_input(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_input }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_albedo_metalness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_albedo_metalness }));
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_velocity }));
	RD::Uniform u_reactivity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_reactivity }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 6, p_output);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_BLOTCH_STABILIZE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_input, u_normal_roughness, u_albedo_metalness, u_viewz_hitdist, u_velocity, u_reactivity, u_output), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_split_composite(const PushConstant &p_push_constant, RID p_diffuse, RID p_specular, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_specular_guide, RID p_output) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_SPLIT_COMPOSITE);

	RD::Uniform u_diffuse(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_diffuse }));
	RD::Uniform u_specular(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_specular }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 2, p_output);
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_velocity }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_albedo_metalness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_albedo_metalness }));
	RD::Uniform u_specular_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_specular_guide }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_SPLIT_COMPOSITE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_diffuse, u_specular, u_output, u_velocity, u_normal_roughness, u_albedo_metalness, u_specular_guide), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_fidelityfx_composite(const PushConstant &p_push_constant, RID p_direct, RID p_emissive, RID p_indirect, RID p_sky, RID p_specular, RID p_diffuse, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_specular_guide, RID p_output) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_FIDELITYFX_COMPOSITE);

	RD::Uniform u_direct(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_direct }));
	RD::Uniform u_emissive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_emissive }));
	RD::Uniform u_indirect(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_indirect }));
	RD::Uniform u_sky(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_sky }));
	RD::Uniform u_specular(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_specular }));
	RD::Uniform u_diffuse(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_diffuse }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 6, p_output);
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_velocity }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_normal_roughness }));
	RD::Uniform u_albedo_metalness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, p_albedo_metalness }));
	RD::Uniform u_specular_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, p_specular_guide }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_FIDELITYFX_COMPOSITE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_direct, u_emissive, u_indirect, u_sky, u_specular, u_diffuse, u_output, u_velocity, u_normal_roughness, u_albedo_metalness, u_specular_guide), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::_dispatch_volumetric_fog(const PushConstant &p_push_constant, RID p_color, RID p_viewz_hitdist, RID p_fog_map) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, MODE_VOLUMETRIC_FOG);

	RD::Uniform u_color(RD::UNIFORM_TYPE_IMAGE, 0, p_color);
	RD::Uniform u_viewz_hitdist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_viewz_hitdist }));
	RD::Uniform u_fog_map(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear_sampler, p_fog_map }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_VOLUMETRIC_FOG]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rd, 0, u_color, u_viewz_hitdist, u_fog_map), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, (uint32_t)p_push_constant.resolution_width, (uint32_t)p_push_constant.resolution_height, 1);
	RD::get_singleton()->compute_list_end();
}

void RTGIDenoise::process(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_source_texture,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_albedo_metalness,
		RID p_viewz_hitdist,
		RID p_history_validity,
		RID p_prev_history_validity,
		RID p_history_id,
		RID p_prev_history_id,
		float p_history_weight,
		float p_denoise_strength,
		float p_firefly_suppression,
		float p_detail_preservation,
		const Size2i &p_process_size,
		uint32_t p_view,
		int p_iterations) {
	process_signal(p_render_buffers, p_source_context, p_source_texture, RB_SCOPE_RTGI_DENOISE, p_velocity, p_normal_roughness, p_albedo_metalness, p_viewz_hitdist, RID(), RID(), p_history_validity, p_prev_history_validity, p_history_id, p_prev_history_id, p_history_weight, p_denoise_strength, p_firefly_suppression, p_detail_preservation, false, true, true, p_process_size, p_view, p_iterations);
}

void RTGIDenoise::process_signal(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_source_texture,
		const StringName &p_denoise_scope,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_albedo_metalness,
		RID p_viewz_hitdist,
		RID p_specular_guide,
		RID p_specular_reprojection,
		RID p_history_validity,
		RID p_prev_history_validity,
		RID p_history_id,
		RID p_prev_history_id,
		float p_history_weight,
		float p_denoise_strength,
		float p_firefly_suppression,
		float p_detail_preservation,
		bool p_radiance_space_history,
		bool p_enable_blotch_stabilize,
		bool p_update_shared_history,
		const Size2i &p_process_size,
		uint32_t p_view,
		int p_iterations) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_source_texture));
	ERR_FAIL_COND(!p_velocity.is_valid() || !p_normal_roughness.is_valid() || !p_albedo_metalness.is_valid() || !p_viewz_hitdist.is_valid());
	ERR_FAIL_COND(!p_history_validity.is_valid() || !p_prev_history_validity.is_valid() || !p_history_id.is_valid() || !p_prev_history_id.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());
	const bool specular_guide_enabled = p_specular_guide.is_valid();
	RID specular_guide = specular_guide_enabled ? p_specular_guide : p_normal_roughness;
	RID specular_reprojection = p_specular_reprojection.is_valid() ? p_specular_reprojection : p_velocity;

	const StringName denoise_scope = p_denoise_scope;
	const bool reset_history = _ensure_buffers(p_render_buffers, denoise_scope, p_process_size);

	RID source = p_render_buffers->get_texture_slice(p_source_context, p_source_texture, p_view, 0);
	RID history = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_HISTORY, p_view, 0);
	RID noisy = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_NOISY, p_view, 0);
	RID moments = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_MOMENTS, p_view, 0);
	RID temp_a = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_TEMP_A, p_view, 0);
	RID temp_b = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_TEMP_B, p_view, 0);
	RID temp_c = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_TEMP_C, p_view, 0);
	RID variance = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_VARIANCE, p_view, 0);
	RID history_length = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_HISTORY_LENGTH, p_view, 0);
	RID rejection = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_REJECTION, p_view, 0);
	RID reactivity = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_REACTIVITY, p_view, 0);
	RID prev_normal_roughness = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_PREV_NORMAL_ROUGHNESS, p_view, 0);
	RID prev_viewz_hitdist = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_PREV_VIEWZ_HITDIST, p_view, 0);
	RID prev_albedo_metalness = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_PREV_ALBEDO_METALNESS, p_view, 0);
	RID prev_specular_guide = p_render_buffers->get_texture_slice(denoise_scope, RB_TEX_RTGI_DENOISE_PREV_SPECULAR_GUIDE, p_view, 0);

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	const float requested_history_weight = CLAMP(p_history_weight, 0.0f, 0.99f);
	push_constant.history_weight = reset_history ? 0.0f : requested_history_weight;
	push_constant.denoise_strength = CLAMP(p_denoise_strength, 0.0f, 1.0f);
	push_constant.max_history = Math::lerp(1.0f, 96.0f, requested_history_weight);
	push_constant.step_size = 1;
	push_constant.phi_color = 4.0f;
	push_constant.phi_normal = 16.0f;
	push_constant.phi_depth = 0.045f;
	push_constant.variance_boost = 2.0f;
	push_constant.radiance_space_history = p_radiance_space_history ? 1.0f : 0.0f;
	push_constant.firefly_suppression = CLAMP(p_firefly_suppression, 0.0f, 1.0f);
	push_constant.detail_preservation = CLAMP(p_detail_preservation, 0.0f, 1.0f);
	push_constant.specular_guide_enabled = specular_guide_enabled ? 1.0f : 0.0f;
	push_constant.history_clip_sigma = 1.8f;

	RD::get_singleton()->texture_copy(source, noisy, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);

	_dispatch_temporal(push_constant, source, p_velocity, p_normal_roughness, p_albedo_metalness, p_viewz_hitdist, specular_guide, prev_specular_guide, specular_reprojection, history, moments, prev_normal_roughness, prev_viewz_hitdist, prev_albedo_metalness, p_history_validity, p_prev_history_validity, p_history_id, p_prev_history_id, temp_c, temp_a, variance, rejection, reactivity, history_length);
	RD::get_singleton()->texture_copy(temp_a, moments, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(temp_c, history, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
	_dispatch_variance_prefilter(push_constant, temp_c, p_normal_roughness, p_viewz_hitdist, variance, reactivity, temp_b);

	RID read = temp_b;
	RID write = temp_a;
	const int max_iterations = CLAMP(p_iterations, 1, 5);
	int iterations = CLAMP((int)Math::round(Math::lerp(2.0, 5.0, (double)push_constant.denoise_strength)), 1, max_iterations);
	for (int i = 0; i < iterations; i++) {
		push_constant.pass_index = i;
		push_constant.step_size = 1 << i;
		push_constant.phi_color = 3.5f + (float)i * 0.75f;
		push_constant.phi_depth = 0.035f + (float)i * 0.015f;
		_dispatch_atrous(push_constant, read, p_normal_roughness, p_albedo_metalness, p_viewz_hitdist, p_velocity, reactivity, specular_guide, p_history_id, write);

		RID swap = read;
		read = write;
		write = swap;
	}

	_dispatch_composite(push_constant, read, temp_c, p_normal_roughness, p_albedo_metalness, p_viewz_hitdist, reactivity, specular_guide, p_history_id, source);
	if (p_enable_blotch_stabilize && push_constant.denoise_strength > 0.01f) {
		_dispatch_blotch_stabilize(push_constant, source, p_normal_roughness, p_albedo_metalness, p_viewz_hitdist, p_velocity, reactivity, temp_a);
		RD::get_singleton()->texture_copy(temp_a, source, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
	}

	RD::get_singleton()->texture_copy(p_normal_roughness, prev_normal_roughness, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);
	RD::get_singleton()->texture_copy(p_viewz_hitdist, prev_viewz_hitdist, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);
	RD::get_singleton()->texture_copy(p_albedo_metalness, prev_albedo_metalness, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);
	if (specular_guide_enabled) {
		RD::get_singleton()->texture_copy(specular_guide, prev_specular_guide, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);
	}
	if (p_update_shared_history) {
		RD::get_singleton()->texture_copy(p_history_validity, p_prev_history_validity, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, p_view);
		RD::get_singleton()->texture_copy(p_history_id, p_prev_history_id, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, p_view);
	}
}

void RTGIDenoise::capture_noisy(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_source_texture,
		const StringName &p_denoise_scope,
		const Size2i &p_process_size,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_source_texture));
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	if (p_render_buffers->has_texture(p_denoise_scope, RB_TEX_RTGI_DENOISE_NOISY) &&
			p_render_buffers->get_texture_slice_size(p_denoise_scope, RB_TEX_RTGI_DENOISE_NOISY, 0) != p_process_size) {
		p_render_buffers->clear_context(p_denoise_scope);
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (!p_render_buffers->has_texture(p_denoise_scope, RB_TEX_RTGI_DENOISE_NOISY)) {
		p_render_buffers->create_texture(p_denoise_scope, RB_TEX_RTGI_DENOISE_NOISY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_process_size);
	}

	RID source = p_render_buffers->get_texture_slice(p_source_context, p_source_texture, p_view, 0);
	RID noisy = p_render_buffers->get_texture_slice(p_denoise_scope, RB_TEX_RTGI_DENOISE_NOISY, p_view, 0);
	RD::get_singleton()->texture_copy(source, noisy, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, 0);
}

void RTGIDenoise::composite_split(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_diffuse_texture,
		const StringName &p_specular_texture,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_albedo_metalness,
		RID p_specular_guide,
		const StringName &p_output_texture,
		float p_denoise_strength,
		float p_firefly_suppression,
		float p_detail_preservation,
		const Size2i &p_process_size,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_diffuse_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_specular_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_output_texture));
	ERR_FAIL_COND(!p_velocity.is_valid());
	ERR_FAIL_COND(!p_normal_roughness.is_valid() || !p_albedo_metalness.is_valid() || !p_specular_guide.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	RID diffuse = p_render_buffers->get_texture_slice(p_source_context, p_diffuse_texture, p_view, 0);
	RID specular = p_render_buffers->get_texture_slice(p_source_context, p_specular_texture, p_view, 0);
	RID output = p_render_buffers->get_texture_slice(p_source_context, p_output_texture, p_view, 0);

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	push_constant.denoise_strength = CLAMP(p_denoise_strength, 0.0f, 1.0f);
	push_constant.firefly_suppression = CLAMP(p_firefly_suppression, 0.0f, 1.0f);
	push_constant.detail_preservation = CLAMP(p_detail_preservation, 0.0f, 1.0f);

	_dispatch_split_composite(push_constant, diffuse, specular, p_velocity, p_normal_roughness, p_albedo_metalness, p_specular_guide, output);
}

void RTGIDenoise::composite_fidelityfx(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_direct_texture,
		const StringName &p_emissive_texture,
		const StringName &p_indirect_texture,
		const StringName &p_sky_texture,
		const StringName &p_specular_texture,
		const StringName &p_diffuse_texture,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_albedo_metalness,
		RID p_specular_guide,
		const StringName &p_output_texture,
		float p_denoise_strength,
		float p_firefly_suppression,
		const Size2i &p_process_size,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_direct_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_emissive_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_indirect_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_sky_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_specular_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_diffuse_texture));
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_output_texture));
	ERR_FAIL_COND(!p_velocity.is_valid());
	ERR_FAIL_COND(!p_normal_roughness.is_valid() || !p_albedo_metalness.is_valid() || !p_specular_guide.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	RID direct = p_render_buffers->get_texture_slice(p_source_context, p_direct_texture, p_view, 0);
	RID emissive = p_render_buffers->get_texture_slice(p_source_context, p_emissive_texture, p_view, 0);
	RID indirect = p_render_buffers->get_texture_slice(p_source_context, p_indirect_texture, p_view, 0);
	RID sky = p_render_buffers->get_texture_slice(p_source_context, p_sky_texture, p_view, 0);
	RID specular = p_render_buffers->get_texture_slice(p_source_context, p_specular_texture, p_view, 0);
	RID diffuse = p_render_buffers->get_texture_slice(p_source_context, p_diffuse_texture, p_view, 0);
	RID output = p_render_buffers->get_texture_slice(p_source_context, p_output_texture, p_view, 0);

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	push_constant.denoise_strength = CLAMP(p_denoise_strength, 0.0f, 1.0f);
	push_constant.firefly_suppression = CLAMP(p_firefly_suppression, 0.0f, 1.0f);

	_dispatch_fidelityfx_composite(push_constant, direct, emissive, indirect, sky, specular, diffuse, p_velocity, p_normal_roughness, p_albedo_metalness, p_specular_guide, output);
}

void RTGIDenoise::composite_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_source_texture,
		RID p_viewz_hitdist,
		RID p_fog_map,
		const Size2i &p_process_size,
		const Vector2i &p_visible_origin,
		const Size2i &p_visible_size,
		float p_fog_length,
		float p_fog_detail_spread,
		float p_fog_sky_affect,
		bool p_legacy_blending,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(p_visible_size.x <= 0 || p_visible_size.y <= 0);
	ERR_FAIL_COND(!p_render_buffers->has_texture(p_source_context, p_source_texture));
	ERR_FAIL_COND(!p_viewz_hitdist.is_valid() || !p_fog_map.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	RID source = p_render_buffers->get_texture_slice(p_source_context, p_source_texture, p_view, 0);

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	push_constant.visible_origin_width = (float)p_visible_origin.x;
	push_constant.visible_origin_height = (float)p_visible_origin.y;
	push_constant.visible_size_width = (float)p_visible_size.x;
	push_constant.visible_size_height = (float)p_visible_size.y;
	push_constant.fog_inv_length = p_fog_length > 0.0f ? 1.0f / p_fog_length : 1.0f;
	push_constant.fog_detail_spread = p_fog_detail_spread > 0.0f ? 1.0f / p_fog_detail_spread : 1.0f;
	push_constant.fog_sky_affect = CLAMP(p_fog_sky_affect, 0.0f, 1.0f);
	push_constant.fog_legacy_blending = p_legacy_blending ? 1.0f : 0.0f;

	_dispatch_volumetric_fog(push_constant, source, p_viewz_hitdist, p_fog_map);
}
