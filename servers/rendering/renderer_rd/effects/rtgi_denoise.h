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
			const Size2i &p_process_size,
			uint32_t p_view = 0,
			int p_iterations = 4);

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

private:
	enum Mode {
		MODE_TEMPORAL,
		MODE_VARIANCE_PREFILTER,
		MODE_ATROUS,
		MODE_COMPOSITE,
		MODE_BLOTCH_STABILIZE,
		MODE_VOLUMETRIC_FOG,
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
		float visible_origin_width;
		float visible_origin_height;
		float visible_size_width;
		float visible_size_height;
		float fog_inv_length;
		float fog_detail_spread;
		float fog_sky_affect;
		float fog_legacy_blending;
	};

	RtgiDenoiseShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	bool _ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const StringName &p_scope, const Size2i &p_size);
	void _dispatch_temporal(const PushConstant &p_push_constant, RID p_source, RID p_velocity, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_history, RID p_moments, RID p_prev_normal_roughness, RID p_prev_viewz_hitdist, RID p_prev_albedo_metalness, RID p_history_validity, RID p_prev_history_validity, RID p_history_id, RID p_prev_history_id, RID p_temporal_out, RID p_moments_out, RID p_variance_out, RID p_rejection_out, RID p_reactivity_out, RID p_history_length_out);
	void _dispatch_variance_prefilter(const PushConstant &p_push_constant, RID p_temporal, RID p_normal_roughness, RID p_viewz_hitdist, RID p_variance, RID p_reactivity, RID p_prefilter_out);
	void _dispatch_atrous(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_output);
	void _dispatch_composite(const PushConstant &p_push_constant, RID p_filtered, RID p_temporal, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_reactivity, RID p_output);
	void _dispatch_blotch_stabilize(const PushConstant &p_push_constant, RID p_input, RID p_normal_roughness, RID p_albedo_metalness, RID p_viewz_hitdist, RID p_velocity, RID p_reactivity, RID p_output);
	void _dispatch_volumetric_fog(const PushConstant &p_push_constant, RID p_color, RID p_viewz_hitdist, RID p_fog_map);
};

} // namespace RendererRD
