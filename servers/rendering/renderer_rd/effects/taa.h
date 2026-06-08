/**************************************************************************/
/*  taa.h                                                                 */
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

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

class TAA {
public:
	TAA();
	~TAA();

	void process(Ref<RenderSceneBuffersRD> p_render_buffers, RD::DataFormat p_format, float p_z_near, float p_z_far, bool p_raytracing_denoise = false, RID p_rt_history_validity = RID(), RID p_rt_prev_history_validity = RID(), RID p_rt_history_id = RID(), RID p_rt_prev_history_id = RID(), float p_raytracing_history_weight = 0.94f, RID p_rt_taa_reactivity = RID(), RID p_taa_confidence_tex = RID(), bool p_confidence_relax = false);
	void process_texture(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_source_context, const StringName &p_source_texture, const StringName &p_history_context, RD::DataFormat p_format, RID p_velocity_texture, float p_z_near, float p_z_far, bool p_raytracing_denoise = false, RID p_rt_history_validity = RID(), RID p_rt_prev_history_validity = RID(), RID p_rt_history_id = RID(), RID p_rt_prev_history_id = RID(), float p_raytracing_history_weight = 0.94f, const Size2i &p_process_size = Size2i(), RID p_depth_texture = RID(), RID p_rt_taa_reactivity = RID());
	void process_texture_with_rt_history(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_source_context, const StringName &p_source_texture, const StringName &p_history_context, RD::DataFormat p_format, RID p_velocity_texture, float p_z_near, float p_z_far, bool p_raytracing_denoise, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, float p_raytracing_history_weight, const Size2i &p_process_size, RID p_depth_texture, RID p_rt_taa_reactivity, const StringName &p_rt_history_validity_texture, const StringName &p_rt_prev_history_validity_texture, const StringName &p_rt_history_id_texture, const StringName &p_rt_prev_history_id_texture, const StringName &p_rt_taa_reactivity_texture, RID p_rt_signal_confidence = RID(), RID p_rt_history_moments = RID(), RID p_rt_prev_history_moments = RID(), RID p_rt_history_metadata = RID(), RID p_rt_prev_history_metadata = RID(), const StringName &p_rt_signal_confidence_texture = StringName(), const StringName &p_rt_history_moments_texture = StringName(), const StringName &p_rt_prev_history_moments_texture = StringName(), const StringName &p_rt_history_metadata_texture = StringName(), const StringName &p_rt_prev_history_metadata_texture = StringName());

private:
	enum PipelineMode {
		PIPELINE_STANDARD,
		PIPELINE_RT_HISTORY_METADATA,
		PIPELINE_MAX
	};

	struct TAAResolvePushConstant {
		float resolution_width;
		float resolution_height;
		float disocclusion_threshold;
		float variance_dynamic;
		float raytracing_denoise;
		float rt_history_validity_enabled;
		float rt_history_id_enabled;
		float history_weight;
		float sharpness;
		float rt_history_filter_strength;
		float rt_taa_reactivity_enabled;
		float rt_history_metadata_enabled;
		float rt_signal_confidence_enabled;
		float taa_confidence_relax_enabled; // FPT-only: relax the neighborhood clamp / hold history on converged path-traced pixels. Was _pad0.
		float relax_agree_lo; // history-agreement gate: rel-distance(history, clamped) below this = full relax. Was _pad1.
		float relax_agree_hi; // history-agreement gate: rel-distance above this = no relax (stock clamp -> no ghost). Was _pad2.
		float relax_floor; // FPT relax: min per-frame blend on a fully-relaxed pixel = the held-pixel convergence rate. 0.02 ~= 2.5 s scene-change settle, 0.05 ~= 1 s. Sets the transient-vs-steady-calm tradeoff.
		// std430 rounds the push-constant block up to a 16-byte multiple; the 17 used floats (68 B) round to
		// 80 B, so pad the struct to 80 B to match the shader's declared push-constant size (else the dispatch
		// fails with "requires (80) bytes ... supplied (68)" and TAA writes nothing -> black frame).
		float relax_change_gain; // FPT relax: box-independent spatial change-term weight (neighborhood mean + min vs history). 0 = prior rel-only gate. Was pad0.
		float relax_dt_gain; // FPT relax: box-independent temporal-term weight (|current - history|); defaulted low. Was pad1.
		float pad2; // keeps the struct at 80 B (16-byte multiple).
	};

	TaaResolveShaderRD taa_shader;
	RID shader_version;
	RID pipelines[PIPELINE_MAX];

	void resolve(RID p_frame, RID p_temp, RID p_depth, RID p_velocity, RID p_prev_velocity, RID p_history, RID p_rt_history_validity, RID p_rt_prev_history_validity, RID p_rt_history_id, RID p_rt_prev_history_id, RID p_rt_taa_reactivity, RID p_rt_signal_confidence, RID p_rt_history_moments, RID p_rt_prev_history_moments, RID p_rt_history_metadata, RID p_rt_prev_history_metadata, Size2 p_resolution, float p_z_near, float p_z_far, bool p_raytracing_denoise, float p_raytracing_history_weight, float p_taa_disocclusion_threshold, float p_taa_history_weight, float p_taa_sharpness, bool p_confidence_relax = false);
};

} // namespace RendererRD
