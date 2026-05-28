/**************************************************************************/
/*  rtgi_diffuse_cache.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/rtgi_diffuse_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_RTGI_DIFFUSE_CACHE SNAME("rtgi_diffuse_cache")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE SNAME("radiance")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT SNAME("radiance_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_META SNAME("meta")
#define RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT SNAME("meta_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_STATS SNAME("stats")
#define RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT SNAME("stats_next")
#define RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT SNAME("output")
#define RB_TEX_RTGI_DIFFUSE_CACHE_RAW SNAME("raw")
#define RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC SNAME("diagnostic")
#define RB_TEX_RTGI_DIFFUSE_CACHE_AGE SNAME("age")
#define RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION SNAME("rejection")

namespace RendererRD {

class RTGIDiffuseCache {
public:
	RTGIDiffuseCache();
	~RTGIDiffuseCache();

	void process(Ref<RenderSceneBuffersRD> p_render_buffers,
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
			uint32_t p_view = 0);

private:
	struct PushConstant {
		float resolution_width;
		float resolution_height;
		float cache_width;
		float cache_height;
		float max_history;
		uint32_t mode;
		uint32_t pad0;
		uint32_t pad1;
	};

	RtgiDiffuseCacheShaderRD shader;
	RID shader_version;
	RID pipeline;

	Size2i _cache_size(const Size2i &p_output_size, uint32_t p_max_cache_entries) const;
	bool _ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_output_size, const Size2i &p_cache_size);
};

} // namespace RendererRD
