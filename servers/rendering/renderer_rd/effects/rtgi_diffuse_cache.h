/**************************************************************************/
/*  rtgi_diffuse_cache.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_diffuse_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_RTGI_DIFFUSE_CACHE SNAME("rtgi_diffuse_cache")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE SNAME("radiance")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT SNAME("radiance_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_META SNAME("meta")
#define RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT SNAME("meta_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_STATS SNAME("stats")
#define RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT SNAME("stats_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID SNAME("history_id")
#define RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID_NEXT SNAME("history_id_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT SNAME("output")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RAW SNAME("raw")
#define RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC SNAME("diagnostic")
#define RB_TEX_RTGI_DIFFUSE_CACHE_AGE SNAME("age")
#define RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION SNAME("rejection")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SIGNAL_CONFIDENCE SNAME("signal_confidence")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE SNAME("spg_radiance")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE_NEXT SNAME("spg_radiance_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META SNAME("spg_meta")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META_NEXT SNAME("spg_meta_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS SNAME("spg_stats")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS_NEXT SNAME("spg_stats_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY SNAME("spg_visibility")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY_NEXT SNAME("spg_visibility_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REJECTION SNAME("spg_rejection")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK SNAME("spg_refinement_mask")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK_NEXT SNAME("spg_refinement_mask_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE SNAME("spg_refined_radiance")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE_NEXT SNAME("spg_refined_radiance_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META SNAME("spg_refined_meta")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META_NEXT SNAME("spg_refined_meta_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS SNAME("spg_refined_stats")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS_NEXT SNAME("spg_refined_stats_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY SNAME("spg_refined_visibility")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY_NEXT SNAME("spg_refined_visibility_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID SNAME("spg_refined_history_id")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID_NEXT SNAME("spg_refined_history_id_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID SNAME("spg_history_id")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID_NEXT SNAME("spg_history_id_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE SNAME("surface_radiance")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE_NEXT SNAME("surface_radiance_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META SNAME("surface_meta")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META_NEXT SNAME("surface_meta_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS SNAME("surface_stats")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS_NEXT SNAME("surface_stats_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID SNAME("surface_history_id")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID_NEXT SNAME("surface_history_id_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY SNAME("slot_surface_key")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY_NEXT SNAME("slot_surface_key_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_CLAIM SNAME("surface_claim")

namespace RendererRD {

class RTGIDiffuseCache {
public:
	RTGIDiffuseCache();
	~RTGIDiffuseCache();

	bool ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers,
			const Size2i &p_process_size,
			uint32_t p_max_cache_entries);

	void process(Ref<RenderSceneBuffersRD> p_render_buffers,
			RID p_diffuse_radiance,
			RID p_albedo_metalness,
			RID p_velocity,
			RID p_normal_roughness,
			RID p_viewz_hitdist,
			RID p_history_validity,
			RID p_prev_history_validity,
			RID p_history_id,
			RID p_prev_history_id,
			RID p_receiver_surface_id,
			RID p_prev_receiver_surface_id,
			RID p_signal_confidence,
			RID p_primary_diffuse_direction,
			const Vector3 &p_camera_origin,
			const Projection &p_inv_view_projection,
			bool p_strc_enabled,
			uint32_t p_strc_cascade_count,
			uint32_t p_strc_grid_size,
			float p_strc_base_probe_spacing,
			const Size2i &p_process_size,
			uint32_t p_max_cache_entries,
			bool p_capture_raw_debug,
			uint32_t p_view = 0);

private:
	struct PushConstant {
		float resolution_width;
		float resolution_height;
		float cache_width;
		float cache_height;
		float max_history;
		uint32_t mode;
		uint32_t spg_probe_width;
		uint32_t spg_probe_height;
		float inv_view_projection[16];
		float camera_origin[3];
		float strc_base_probe_spacing;
		uint32_t strc_grid_size;
		uint32_t strc_cascade_count;
		uint32_t strc_enabled;
		uint32_t pad2;
	};

	RtgiDiffuseCacheShaderRD shader;
	RID shader_version;
	RID pipeline;

	Size2i _cache_size(const Size2i &p_output_size, uint32_t p_max_cache_entries) const;
	bool _ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_output_size, const Size2i &p_cache_size);
};

} // namespace RendererRD
