/**************************************************************************/
/*  rtgi_denoise.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/rtgi_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_RTGI_DENOISE SNAME("rtgi_denoise")
#define RB_SCOPE_RTGI_DENOISE_DIFFUSE SNAME("rtgi_denoise_diffuse")
#define RB_SCOPE_RTGI_DENOISE_SPECULAR SNAME("rtgi_denoise_specular")
#define RB_SCOPE_RTGI_DENOISE_COMPOSITE SNAME("rtgi_denoise_composite")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION SNAME("rtgi_signal_decomposition")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_DIFFUSE SNAME("rtgi_signal_decomposition_diffuse")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_DIRECT SNAME("rtgi_signal_decomposition_direct")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_EMISSIVE SNAME("rtgi_signal_decomposition_emissive")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_INDIRECT SNAME("rtgi_signal_decomposition_indirect")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_SKY SNAME("rtgi_signal_decomposition_sky")
#define RB_SCOPE_RTGI_SIGNAL_DECOMPOSITION_SPECULAR SNAME("rtgi_signal_decomposition_specular")
#define RB_TEX_RTGI_DENOISE_HISTORY SNAME("history")
#define RB_TEX_RTGI_DENOISE_NOISY SNAME("noisy")
#define RB_TEX_RTGI_DENOISE_MOMENTS SNAME("moments")
#define RB_TEX_RTGI_DENOISE_TEMP_A SNAME("temp_a")
#define RB_TEX_RTGI_DENOISE_TEMP_B SNAME("temp_b")
#define RB_TEX_RTGI_DENOISE_TEMP_C SNAME("temp_c")
#define RB_TEX_RTGI_DENOISE_VARIANCE SNAME("variance")
#define RB_TEX_RTGI_DENOISE_HISTORY_LENGTH SNAME("history_length")
#define RB_TEX_RTGI_DENOISE_REJECTION SNAME("rejection")
#define RB_TEX_RTGI_DENOISE_REACTIVITY SNAME("reactivity")
#define RB_TEX_RTGI_DENOISE_PREV_NORMAL_ROUGHNESS SNAME("prev_normal_roughness")
#define RB_TEX_RTGI_DENOISE_PREV_VIEWZ_HITDIST SNAME("prev_viewz_hitdist")
#define RB_TEX_RTGI_DENOISE_PREV_ALBEDO_METALNESS SNAME("prev_albedo_metalness")
#define RB_TEX_RTGI_DENOISE_PREV_SPECULAR_GUIDE SNAME("prev_specular_guide")

namespace RendererRD {

class RTGIDenoise {
public:
	RTGIDenoise();
	~RTGIDenoise();

	void process(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			uint32_t p_view = 0,
			int p_iterations = 4);

	void process_signal(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			uint32_t p_view = 0,
			int p_iterations = 4);

	void capture_noisy(Ref<RenderSceneBuffersRD> p_render_buffers,
			const StringName &p_source_context,
			const StringName &p_source_texture,
			const StringName &p_denoise_scope,
			const Size2i &p_process_size,
			uint32_t p_view = 0);

	void composite_split(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			uint32_t p_view = 0);

	void composite_reconstructed_split(Ref<RenderSceneBuffersRD> p_render_buffers,
			const StringName &p_source_context,
			const StringName &p_diffuse_texture,
			const StringName &p_specular_texture,
			RID p_target_albedo,
			RID p_target_normal,
			RID p_target_orm,
			const StringName &p_output_texture,
			const Size2i &p_process_size,
			uint32_t p_view = 0);

	void composite_signal_decomposition(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			float p_detail_preservation,
			const Size2i &p_process_size,
			uint32_t p_view = 0);

	void composite_volumetric_fog(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			uint32_t p_view = 0);

	void compose_taa_reactivity(Ref<RenderSceneBuffersRD> p_render_buffers,
			const Vector<StringName> &p_denoise_scopes,
			RID p_velocity,
			RID p_history_validity,
			RID p_output,
			const Size2i &p_process_size,
			uint32_t p_view = 0);

	void reconstruct(Ref<RenderSceneBuffersRD> p_render_buffers,
			const StringName &p_source_context,
			const StringName &p_source_texture,
			const StringName &p_output_context,
			const StringName &p_output_texture,
			const StringName &p_intermediate_context,
			const StringName &p_intermediate_texture,
			const StringName &p_reactivity_context,
			const StringName &p_reactivity_texture,
			const StringName &p_signal_confidence_context,
			const StringName &p_signal_confidence_texture,
			const StringName &p_guide_mismatch_context,
			const StringName &p_guide_mismatch_texture,
			const StringName &p_fill_source_context,
			const StringName &p_fill_source_texture,
			const Vector<StringName> &p_denoise_scopes,
			RID p_taa_reactivity,
			RID p_signal_confidence,
			RID p_cache_fill_radiance,
			RID p_cache_fill_signal_confidence,
			RID p_source_depth,
			RID p_source_normal_roughness,
			RID p_source_albedo_metalness,
			RID p_source_viewz_hitdist,
			RID p_target_depth,
			RID p_target_normal_roughness,
			RID p_target_albedo,
			RID p_target_normal,
			RID p_target_orm,
			const Vector2i &p_source_visible_origin,
			const Size2i &p_source_visible_size,
			const Vector2 &p_source_jitter,
			const Size2i &p_output_size,
			bool p_use_target_guides,
			bool p_diffuse_irradiance_reconstruction,
			uint32_t p_view = 0);

	void reconstruct_history(Ref<RenderSceneBuffersRD> p_render_buffers,
			RID p_source_history_validity,
			RID p_source_history_id,
			const StringName &p_output_history_validity_texture,
			const StringName &p_output_history_id_texture,
			const Vector2i &p_source_visible_origin,
			const Size2i &p_source_visible_size,
			const Vector2 &p_source_jitter,
			const Size2i &p_output_size,
			uint32_t p_view = 0);

private:
	enum Mode {
		MODE_RECONSTRUCT,
		MODE_RECONSTRUCT_REFINE,
		MODE_TEMPORAL,
		MODE_VARIANCE_PREFILTER,
		MODE_ATROUS,
		MODE_COMPOSITE,
		MODE_BLOTCH_STABILIZE,
		MODE_SPLIT_COMPOSITE,
		MODE_SIGNAL_DECOMPOSITION_COMPOSITE,
		MODE_VOLUMETRIC_FOG,
		MODE_TAA_REACTIVITY,
		MODE_RECONSTRUCT_COMPOSITE_SPLIT,
		MODE_RECONSTRUCT_HISTORY,
		MODE_MAX
	};

	struct PushConstant {
		float resolution_width;
		float resolution_height;
		float history_weight;
		float max_history;
		float denoise_strength;
		int32_t step_size;
		int32_t pass_index;
		float phi_color;
		float phi_normal;
		float phi_depth;
		float variance_boost;
		float radiance_space_history;
		float firefly_suppression;
		float detail_preservation;
		float visible_origin_width;
		float visible_origin_height;
		float visible_size_width;
		float visible_size_height;
		float fog_inv_length;
		float fog_detail_spread;
		float fog_sky_affect;
		float fog_legacy_blending;
		float specular_guide_enabled;
		float history_clip_sigma;
		float diagnostic_scope_count;
		float target_material_guide_enabled;
		float source_jitter_x;
		float source_jitter_y;
		float diffuse_irradiance_reconstruction;
		float cache_fill_enabled;
		float pad4;
		float pad5;
	};

	RtgiDenoiseShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	bool _ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_scope, const Size2i &p_size);
	void _dispatch_temporal(const PushConstant &p_push_constant, RID p_source, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_specular_guide, RID p_prev_specular_guide, RID p_specular_reprojection, RID p_history, RID p_moments, RID p_prev_normal_roughness, RID p_prev_viewz_hitdist, RID p_prev_albedo_metalness, RID p_history_validity, RID p_prev_history_validity, RID p_history_id, RID p_prev_history_id, RID p_temporal_out, RID p_moments_out, RID p_variance_out, RID p_rejection_out, RID p_reactivity_out, RID p_history_length_out);
	void _dispatch_variance_prefilter(const PushConstant &p_push_constant, RID p_temporal, RID p_normal_roughness, RID p_viewz_hitdist, RID p_variance, RID p_reactivity, RID p_prefilter_out);
	void _dispatch_atrous(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_specular_guide, RID p_history_id, RID p_output);
	void _dispatch_composite(const PushConstant &p_push_constant, RID p_filtered, RID p_temporal, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_reactivity, RID p_specular_guide, RID p_history_id, RID p_output);
	void _dispatch_blotch_stabilize(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_output);
	void _dispatch_split_composite(const PushConstant &p_push_constant, RID p_diffuse, RID p_specular, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_specular_guide, RID p_output);
	void _dispatch_reconstruct_composite_split(const PushConstant &p_push_constant, RID p_diffuse, RID p_specular, RID p_target_albedo, RID p_target_normal, RID p_target_orm, RID p_output);
	void _dispatch_signal_decomposition_composite(const PushConstant &p_push_constant, RID p_direct, RID p_emissive, RID p_indirect, RID p_sky, RID p_specular, RID p_diffuse, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_specular_guide, RID p_output);
	void _dispatch_volumetric_fog(const PushConstant &p_push_constant, RID p_color, RID p_viewz_hitdist, RID p_fog_map);
	void _dispatch_taa_reactivity(const PushConstant &p_push_constant, RID p_velocity, RID p_history_validity, const RID *p_variance, const RID *p_history_length, const RID *p_rejection, const RID *p_reactivity, RID p_output);
	void _dispatch_reconstruct(Mode p_mode, const PushConstant &p_push_constant, RID p_source, RID p_source_depth, RID p_source_normal_roughness, RID p_source_albedo_metalness, RID p_source_viewz_hitdist, RID p_target_depth, RID p_target_normal_roughness, RID p_target_albedo, RID p_target_normal, RID p_target_orm, RID p_taa_reactivity, RID p_signal_confidence, RID p_cache_fill_radiance, RID p_cache_fill_signal_confidence, const RID *p_variance, const RID *p_history_length, const RID *p_rejection, const RID *p_reactivity, RID p_output, RID p_reactivity_output, RID p_signal_confidence_output, RID p_guide_mismatch_output, RID p_fill_source_output);
	void _dispatch_reconstruct_history(const PushConstant &p_push_constant, RID p_source_history_validity, RID p_source_history_id, RID p_output_history_validity, RID p_output_history_id);
};

} // namespace RendererRD
