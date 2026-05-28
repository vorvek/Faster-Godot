/**************************************************************************/
/*  rtgi_spatiotemporal_radiance_cache.h                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/rtgi_spatiotemporal_radiance_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_RTGI_STRC SNAME("rtgi_strc")
#define RB_TEX_RTGI_STRC_IRRADIANCE SNAME("irradiance")
#define RB_TEX_RTGI_STRC_IRRADIANCE_NEXT SNAME("irradiance_next")
#define RB_TEX_RTGI_STRC_DISTANCE SNAME("distance")
#define RB_TEX_RTGI_STRC_DISTANCE_NEXT SNAME("distance_next")
#define RB_TEX_RTGI_STRC_METADATA SNAME("metadata")
#define RB_TEX_RTGI_STRC_METADATA_NEXT SNAME("metadata_next")
#define RB_TEX_RTGI_STRC_RADIANCE_DEBUG SNAME("radiance_debug")
#define RB_TEX_RTGI_STRC_CONFIDENCE_DEBUG SNAME("confidence_debug")
#define RB_TEX_RTGI_STRC_UPDATES_DEBUG SNAME("updates_debug")
#define RB_TEX_RTGI_STRC_VISIBILITY_DEBUG SNAME("visibility_debug")
#define RB_TEX_RTGI_STRC_AGE_DEBUG SNAME("age_debug")
#define RB_TEX_RTGI_STRC_VARIANCE_DEBUG SNAME("variance_debug")
#define RB_TEX_RTGI_STRC_REJECTION_DEBUG SNAME("rejection_debug")

namespace RendererRD {

class RTGISpatioTemporalRadianceCache {
public:
	RTGISpatioTemporalRadianceCache();
	~RTGISpatioTemporalRadianceCache();

	bool ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_cascade_count, uint32_t p_grid_size, uint32_t p_rays_per_frame, uint64_t p_signature);
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_cascade_count, uint32_t p_grid_size, uint32_t p_rays_per_frame, float p_temporal_weight, uint32_t p_frame_index, const Vector3i *p_cascade_scroll, bool p_scroll_valid, uint32_t p_view = 0);

	RID get_ray_result_buffer() const { return ray_result_buffer; }
	uint32_t get_ray_result_capacity() const { return ray_result_capacity; }

private:
	struct PushConstant {
		uint32_t ray_count;
		uint32_t grid_size;
		uint32_t cascade_count;
		uint32_t frame_index;
		float temporal_weight;
		uint32_t mode;
		uint32_t scroll_valid;
		uint32_t pad0;
		int32_t cascade_scroll[4][4];
	};

	RtgiSpatiotemporalRadianceCacheShaderRD shader;
	RID shader_version;
	RID pipeline;

	RID ray_result_buffer;
	uint32_t ray_result_capacity = 0;
	uint64_t resource_signature = 0;

	bool _ensure_ray_result_buffer(uint32_t p_rays_per_frame);
	Size2i _atlas_size(uint32_t p_cascade_count, uint32_t p_grid_size) const;
};

} // namespace RendererRD
