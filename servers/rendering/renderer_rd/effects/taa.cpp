/**************************************************************************/
/*  taa.cpp                                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "taa.h"
#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

TAA::TAA() {
	Vector<String> taa_modes;
	taa_modes.push_back("\n#define MODE_TAA_RESOLVE");
	taa_modes.push_back("\n#define MODE_TAA_RESOLVE\n#define RT_HISTORY_METADATA_ENABLED");
	taa_shader.initialize(taa_modes);
	shader_version = taa_shader.version_create();
	for (uint32_t i = 0; i < PIPELINE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(taa_shader.version_get_shader(shader_version, i));
	}
}

TAA::~TAA() {
	taa_shader.version_free(shader_version);
}

void TAA::resolve(RID p_frame, RID p_temp, RID p_depth, RID p_velocity, RID p_prev_velocity, RID p_history, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, RID p_rt_taa_reactivity, RID p_rt_signal_confidence, RID p_rt_history_moments, RID p_rt_prev_history_moments, RID p_rt_history_metadata, RID p_rt_prev_history_metadata, Size2 p_resolution, float p_z_near, float p_z_far, bool p_raytracing_denoise, float p_raytracing_history_weight, float p_taa_disocclusion_threshold, float p_taa_history_weight, float p_taa_sharpness) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	ERR_FAIL_NULL(texture_storage);

	RID default_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	float base_variance = 1.1f;
	float base_variance_min = 0.75f;
	float base_variance_max = 1.00f;
	float variance_scale = 1080.0f / p_resolution.height; // 1080p taken as baseline for calculation, as this is most commonly used resolution

	TAAResolvePushConstant push_constant;
	memset(&push_constant, 0, sizeof(TAAResolvePushConstant));
	push_constant.resolution_width = p_resolution.width;
	push_constant.resolution_height = p_resolution.height;
	push_constant.disocclusion_threshold = CLAMP(p_taa_disocclusion_threshold, 0.0f, 8.0f); // If velocity changes by less than this amount of texels we can retain the accumulation buffer.
	push_constant.variance_dynamic = CLAMP(base_variance * variance_scale, base_variance_min, base_variance_max); // Variance dynamically scales based on resolution
	push_constant.raytracing_denoise = p_raytracing_denoise ? 1.0f : 0.0f;
	push_constant.rt_history_validity_enabled = (p_rt_history_validity.is_valid() && p_rt_prev_history_validity.is_valid()) ? 1.0f : 0.0f;
	push_constant.rt_history_id_enabled = (p_rt_history_id.is_valid() && p_rt_prev_history_id.is_valid()) ? 1.0f : 0.0f;
	push_constant.history_weight = CLAMP(p_taa_history_weight, 0.0f, 0.99f);
	push_constant.sharpness = CLAMP(p_taa_sharpness, 0.0f, 1.0f);
	push_constant.rt_history_filter_strength = 0.0f;
	push_constant.rt_taa_reactivity_enabled = p_rt_taa_reactivity.is_valid() ? 1.0f : 0.0f;
	push_constant.rt_history_metadata_enabled = (p_rt_history_moments.is_valid() && p_rt_prev_history_moments.is_valid() && p_rt_history_metadata.is_valid() && p_rt_prev_history_metadata.is_valid()) ? 1.0f : 0.0f;
	push_constant.rt_signal_confidence_enabled = p_rt_signal_confidence.is_valid() ? 1.0f : 0.0f;
	if (p_raytracing_denoise) {
		push_constant.history_weight = CLAMP(p_raytracing_history_weight, 0.0f, 0.999f);
		push_constant.sharpness = 0.0f;
		const float low_resolution_baseline = 720.0f;
		const float low_resolution_range = 360.0f;
		const float min_resolution = MIN(p_resolution.width, p_resolution.height);
		push_constant.rt_history_filter_strength = CLAMP((low_resolution_baseline - min_resolution) / low_resolution_range, 0.0f, 1.0f);
	}

	RID rt_history_validity = p_rt_history_validity.is_valid() ? p_rt_history_validity : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID rt_prev_history_validity = p_rt_prev_history_validity.is_valid() ? p_rt_prev_history_validity : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID rt_history_id = p_rt_history_id.is_valid() ? p_rt_history_id : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID rt_prev_history_id = p_rt_prev_history_id.is_valid() ? p_rt_prev_history_id : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RID rt_taa_reactivity = p_rt_taa_reactivity.is_valid() ? p_rt_taa_reactivity : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RID rt_signal_confidence = p_rt_signal_confidence.is_valid() ? p_rt_signal_confidence : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

	const bool use_rt_history_metadata = push_constant.rt_history_metadata_enabled > 0.5f;
	const PipelineMode pipeline_mode = use_rt_history_metadata ? PIPELINE_RT_HISTORY_METADATA : PIPELINE_STANDARD;
	RID shader = taa_shader.version_get_shader(shader_version, pipeline_mode);
	ERR_FAIL_COND(shader.is_null());

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[pipeline_mode]);

	RD::Uniform u_frame_source(RD::UNIFORM_TYPE_IMAGE, 0, { p_frame });
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { default_sampler, p_depth });
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_IMAGE, 2, { p_velocity });
	RD::Uniform u_prev_velocity(RD::UNIFORM_TYPE_IMAGE, 3, { p_prev_velocity });
	RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { default_sampler, p_history });
	RD::Uniform u_frame_dest(RD::UNIFORM_TYPE_IMAGE, 5, { p_temp });
	RD::Uniform u_rt_history_validity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, { nearest_sampler, rt_history_validity });
	RD::Uniform u_rt_prev_history_validity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, { nearest_sampler, rt_prev_history_validity });
	RD::Uniform u_rt_history_id(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, { nearest_sampler, rt_history_id });
	RD::Uniform u_rt_prev_history_id(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, { nearest_sampler, rt_prev_history_id });
	RD::Uniform u_rt_taa_reactivity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, { nearest_sampler, rt_taa_reactivity });

	if (use_rt_history_metadata) {
		RD::Uniform u_rt_signal_confidence(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, { nearest_sampler, rt_signal_confidence });
		RD::Uniform u_rt_prev_history_moments(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, { nearest_sampler, p_rt_prev_history_moments });
		RD::Uniform u_rt_prev_history_metadata(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, { nearest_sampler, p_rt_prev_history_metadata });
		RD::Uniform u_rt_history_moments_out(RD::UNIFORM_TYPE_IMAGE, 14, { p_rt_history_moments });
		RD::Uniform u_rt_history_metadata_out(RD::UNIFORM_TYPE_IMAGE, 15, { p_rt_history_metadata });
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_frame_source, u_depth, u_velocity, u_prev_velocity, u_history, u_frame_dest, u_rt_history_validity, u_rt_prev_history_validity, u_rt_history_id, u_rt_prev_history_id, u_rt_taa_reactivity, u_rt_signal_confidence, u_rt_prev_history_moments, u_rt_prev_history_metadata, u_rt_history_moments_out, u_rt_history_metadata_out), 0);
	} else {
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_frame_source, u_depth, u_velocity, u_prev_velocity, u_history, u_frame_dest, u_rt_history_validity, u_rt_prev_history_validity, u_rt_history_id, u_rt_prev_history_id, u_rt_taa_reactivity), 0);
	}
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(TAAResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_resolution.width, p_resolution.height, 1);
	RD::get_singleton()->compute_list_end();
}

void TAA::process(Ref<RenderSceneBuffersRD> p_render_buffers, RD::DataFormat p_format, float p_z_near, float p_z_far, bool p_raytracing_denoise, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, float p_raytracing_history_weight, RID p_rt_taa_reactivity) {
	CopyEffects *copy_effects = CopyEffects::get_singleton();

	uint32_t view_count = p_render_buffers->get_view_count();
	Size2i internal_size = p_render_buffers->get_internal_size();

	bool just_allocated = false;
	if (!p_render_buffers->has_texture(SNAME("taa"), SNAME("history"))) {
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

		p_render_buffers->create_texture(SNAME("taa"), SNAME("history"), p_format, usage_bits);
		p_render_buffers->create_texture(SNAME("taa"), SNAME("temp"), p_format, usage_bits);

		p_render_buffers->create_texture(SNAME("taa"), SNAME("prev_velocity"), RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits);

		just_allocated = true;
	}

	RD::get_singleton()->draw_command_begin_label("TAA");

	for (uint32_t v = 0; v < view_count; v++) {
		// Get our (cached) slices
		RID internal_texture = p_render_buffers->get_internal_texture(v);
		RID velocity_buffer = p_render_buffers->get_velocity_buffer(false, v);
		RID taa_history = p_render_buffers->get_texture_slice(SNAME("taa"), SNAME("history"), v, 0);
		RID taa_prev_velocity = p_render_buffers->get_texture_slice(SNAME("taa"), SNAME("prev_velocity"), v, 0);

		if (!just_allocated) {
			RID depth_texture = p_render_buffers->get_depth_texture(v);
			RID taa_temp = p_render_buffers->get_texture_slice(SNAME("taa"), SNAME("temp"), v, 0);

			RID rt_history_validity_slice = p_rt_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity"), v, 0) : RID();
			RID rt_prev_history_validity_slice = p_rt_prev_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity_prev"), v, 0) : RID();
			RID rt_history_id_slice = p_rt_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id"), v, 0) : RID();
			RID rt_prev_history_id_slice = p_rt_prev_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id_prev"), v, 0) : RID();
			RID rt_taa_reactivity_slice = p_rt_taa_reactivity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_taa_reactivity"), v, 0) : RID();

			resolve(internal_texture, taa_temp, depth_texture, velocity_buffer, taa_prev_velocity, taa_history, rt_history_validity_slice, rt_prev_history_validity_slice, rt_history_id_slice, rt_prev_history_id_slice, rt_taa_reactivity_slice, RID(), RID(), RID(), RID(), RID(), Size2(internal_size.x, internal_size.y), p_z_near, p_z_far, p_raytracing_denoise, p_raytracing_history_weight, p_render_buffers->get_taa_disocclusion_threshold(), p_render_buffers->get_taa_history_weight(), p_render_buffers->get_taa_sharpness());
			copy_effects->copy_to_rect(taa_temp, internal_texture, Rect2(0, 0, internal_size.x, internal_size.y));
		}

		copy_effects->copy_to_rect(internal_texture, taa_history, Rect2(0, 0, internal_size.x, internal_size.y));
		copy_effects->copy_to_rect(velocity_buffer, taa_prev_velocity, Rect2(0, 0, internal_size.x, internal_size.y));
		if (p_rt_history_validity.is_valid() && p_rt_prev_history_validity.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_validity, p_rt_prev_history_validity, Vector3(), Vector3(), Vector3(internal_size.x, internal_size.y, 1), 0, 0, v, v);
		}
		if (p_rt_history_id.is_valid() && p_rt_prev_history_id.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_id, p_rt_prev_history_id, Vector3(), Vector3(), Vector3(internal_size.x, internal_size.y, 1), 0, 0, v, v);
		}
	}

	RD::get_singleton()->draw_command_end_label();
}

void TAA::process_texture(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_source_context, const StringName &p_source_texture, const StringName &p_history_context, RD::DataFormat p_format, RID p_velocity_texture, float p_z_near, float p_z_far, bool p_raytracing_denoise, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, float p_raytracing_history_weight, const Size2i &p_process_size, RID p_depth_texture, RID p_rt_taa_reactivity) {
	process_texture_with_rt_history(p_render_buffers, p_source_context, p_source_texture, p_history_context, p_format, p_velocity_texture, p_z_near, p_z_far, p_raytracing_denoise, p_rt_history_validity, p_rt_prev_history_validity, p_rt_history_id, p_rt_prev_history_id, p_raytracing_history_weight, p_process_size, p_depth_texture, p_rt_taa_reactivity, SNAME("rt_history_validity"), SNAME("rt_history_validity_prev"), SNAME("rt_history_id"), SNAME("rt_history_id_prev"), StringName());
}

void TAA::process_texture_with_rt_history(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_source_context, const StringName &p_source_texture, const StringName &p_history_context, RD::DataFormat p_format, RID p_velocity_texture, float p_z_near, float p_z_far, bool p_raytracing_denoise, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, float p_raytracing_history_weight, const Size2i &p_process_size, RID p_depth_texture, RID p_rt_taa_reactivity, const StringName &p_rt_history_validity_texture, const StringName &p_rt_prev_history_validity_texture, const StringName &p_rt_history_id_texture, const StringName &p_rt_prev_history_id_texture, const StringName &p_rt_taa_reactivity_texture, RID p_rt_signal_confidence, RID p_rt_history_moments, RID p_rt_prev_history_moments, RID p_rt_history_metadata, RID p_rt_prev_history_metadata, const StringName &p_rt_signal_confidence_texture, const StringName &p_rt_history_moments_texture, const StringName &p_rt_prev_history_moments_texture, const StringName &p_rt_history_metadata_texture, const StringName &p_rt_prev_history_metadata_texture) {
	CopyEffects *copy_effects = CopyEffects::get_singleton();

	uint32_t view_count = p_render_buffers->get_view_count();
	Size2i process_size = p_process_size == Size2i() ? p_render_buffers->get_internal_size() : p_process_size;

	bool just_allocated = false;
	if (p_render_buffers->has_texture(p_history_context, SNAME("history")) &&
			p_render_buffers->get_texture_slice_size(p_history_context, SNAME("history"), 0) != process_size) {
		p_render_buffers->clear_context(p_history_context);
	}
	if (!p_render_buffers->has_texture(p_history_context, SNAME("history"))) {
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

		p_render_buffers->create_texture(p_history_context, SNAME("history"), p_format, usage_bits, RD::TEXTURE_SAMPLES_1, process_size);
		p_render_buffers->create_texture(p_history_context, SNAME("temp"), p_format, usage_bits, RD::TEXTURE_SAMPLES_1, process_size);
		p_render_buffers->create_texture(p_history_context, SNAME("prev_velocity"), RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, process_size);

		just_allocated = true;
	}

	RD::get_singleton()->draw_command_begin_label("TAA");

	for (uint32_t v = 0; v < view_count; v++) {
		RID frame_texture = p_render_buffers->get_texture_slice(p_source_context, p_source_texture, v, 0);
		RID velocity_buffer = p_velocity_texture;
		RID taa_history = p_render_buffers->get_texture_slice(p_history_context, SNAME("history"), v, 0);
		RID taa_prev_velocity = p_render_buffers->get_texture_slice(p_history_context, SNAME("prev_velocity"), v, 0);

		if (!just_allocated) {
			RID depth_texture = p_depth_texture.is_valid() ? p_depth_texture : p_render_buffers->get_depth_texture(v);
			RID taa_temp = p_render_buffers->get_texture_slice(p_history_context, SNAME("temp"), v, 0);

			RID rt_history_validity_slice = (p_rt_history_validity.is_valid() && p_rt_history_validity_texture != StringName()) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_history_validity_texture, v, 0) : RID();
			RID rt_prev_history_validity_slice = (p_rt_prev_history_validity.is_valid() && p_rt_prev_history_validity_texture != StringName()) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_prev_history_validity_texture, v, 0) : RID();
			RID rt_history_id_slice = (p_rt_history_id.is_valid() && p_rt_history_id_texture != StringName()) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_history_id_texture, v, 0) : RID();
			RID rt_prev_history_id_slice = (p_rt_prev_history_id.is_valid() && p_rt_prev_history_id_texture != StringName()) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_prev_history_id_texture, v, 0) : RID();
			RID rt_taa_reactivity_slice;
			if (p_rt_taa_reactivity.is_valid()) {
				if (p_rt_taa_reactivity_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_taa_reactivity_texture)) {
					rt_taa_reactivity_slice = p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_taa_reactivity_texture, v, 0);
				} else {
					RID default_rt_taa_reactivity = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_taa_reactivity")) ? p_render_buffers->get_texture(SNAME("forward_clustered"), SNAME("rt_taa_reactivity")) : RID();
					rt_taa_reactivity_slice = default_rt_taa_reactivity == p_rt_taa_reactivity ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_taa_reactivity"), v, 0) : p_rt_taa_reactivity;
				}
			}
			RID rt_signal_confidence_slice = (p_rt_signal_confidence.is_valid() && p_rt_signal_confidence_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_signal_confidence_texture)) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_signal_confidence_texture, v, 0) : p_rt_signal_confidence;
			RID rt_history_moments_slice = (p_rt_history_moments.is_valid() && p_rt_history_moments_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_history_moments_texture)) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_history_moments_texture, v, 0) : p_rt_history_moments;
			RID rt_prev_history_moments_slice = (p_rt_prev_history_moments.is_valid() && p_rt_prev_history_moments_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_prev_history_moments_texture)) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_prev_history_moments_texture, v, 0) : p_rt_prev_history_moments;
			RID rt_history_metadata_slice = (p_rt_history_metadata.is_valid() && p_rt_history_metadata_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_history_metadata_texture)) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_history_metadata_texture, v, 0) : p_rt_history_metadata;
			RID rt_prev_history_metadata_slice = (p_rt_prev_history_metadata.is_valid() && p_rt_prev_history_metadata_texture != StringName() && p_render_buffers->has_texture(SNAME("forward_clustered"), p_rt_prev_history_metadata_texture)) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), p_rt_prev_history_metadata_texture, v, 0) : p_rt_prev_history_metadata;

			resolve(frame_texture, taa_temp, depth_texture, velocity_buffer, taa_prev_velocity, taa_history, rt_history_validity_slice, rt_prev_history_validity_slice, rt_history_id_slice, rt_prev_history_id_slice, rt_taa_reactivity_slice, rt_signal_confidence_slice, rt_history_moments_slice, rt_prev_history_moments_slice, rt_history_metadata_slice, rt_prev_history_metadata_slice, Size2(process_size.x, process_size.y), p_z_near, p_z_far, p_raytracing_denoise, p_raytracing_history_weight, p_render_buffers->get_taa_disocclusion_threshold(), p_render_buffers->get_taa_history_weight(), p_render_buffers->get_taa_sharpness());
			copy_effects->copy_to_rect(taa_temp, frame_texture, Rect2(0, 0, process_size.x, process_size.y));
		}

		copy_effects->copy_to_rect(frame_texture, taa_history, Rect2(0, 0, process_size.x, process_size.y));
		copy_effects->copy_to_rect(velocity_buffer, taa_prev_velocity, Rect2(0, 0, process_size.x, process_size.y));
		if (p_rt_history_validity.is_valid() && p_rt_prev_history_validity.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_validity, p_rt_prev_history_validity, Vector3(), Vector3(), Vector3(process_size.x, process_size.y, 1), 0, 0, v, v);
		}
		if (p_rt_history_id.is_valid() && p_rt_prev_history_id.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_id, p_rt_prev_history_id, Vector3(), Vector3(), Vector3(process_size.x, process_size.y, 1), 0, 0, v, v);
		}
		if (p_rt_history_moments.is_valid() && p_rt_prev_history_moments.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_moments, p_rt_prev_history_moments, Vector3(), Vector3(), Vector3(process_size.x, process_size.y, 1), 0, 0, v, v);
		}
		if (p_rt_history_metadata.is_valid() && p_rt_prev_history_metadata.is_valid()) {
			RD::get_singleton()->texture_copy(p_rt_history_metadata, p_rt_prev_history_metadata, Vector3(), Vector3(), Vector3(process_size.x, process_size.y, 1), 0, 0, v, v);
		}
	}

	RD::get_singleton()->draw_command_end_label();
}
