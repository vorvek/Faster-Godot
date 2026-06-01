/**************************************************************************/
/*  rtgi_world_radiance_cache.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/effects/rtgi_wrc_math.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_world_radiance_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// Per-frame scalars + clipmap recenter deltas for the World Radiance Cache.
// The clipmap is camera-centered; `scroll_delta[k]` is the integer probe-space
// shift of cascade `k` since last frame (see RtgiWrc::recenter_delta). The real
// per-frame cascade/scroll math is wired in Task 5; Task 4 fills defaults so the
// scroll/accumulate dispatches run as no-ops.
struct WRCFrameParams {
	Vector3 camera_pos;
	int32_t scroll_delta[4][3];
	uint32_t rays_this_frame = 0;
	float temporal_n_cap = 64.0f;
	float feedback_damping = 0.0f;
	float view_prioritization = 0.0f;
	uint32_t frame_index = 0;
};

// World Radiance Cache RD effect: a camera-centered cascaded clipmap of
// octahedral-radiance probes packed into a roughly-square atlas (see
// RtgiWrc::atlas_tiles_per_row / atlas_coord). This effect OWNS its atlas
// textures directly (RID members + ping-pong index) rather than the
// render-buffers scope, since the cache is world-space / view-independent.
//
// Structure mirrors RTGISpatioTemporalRadianceCache: a single compute shader
// with a `mode` push-constant driving scroll (mode 0) and accumulate (mode 1)
// dispatches over the atlas, with ping-pong read/write atlases swapped per
// update. Task 4 ships the structure with EMPTY kernels; the probe-ray update
// (Task 5) and the real accumulate/recenter kernels (Task 6) come later.
class RTGIWorldRadianceCache {
public:
	RTGIWorldRadianceCache();
	~RTGIWorldRadianceCache();

	// Allocate (or reallocate on param change) the ping-pong atlases sized from
	// `params`. `view_count` is accepted for signature parity with other effects;
	// the WRC is world-space so the atlases are not per-view. Returns true when a
	// (re)allocation happened.
	bool ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, const RtgiWrc::ClipmapParams &p_params, int p_view_count);

	// Record the scroll (mode 0) + accumulate (mode 1) compute dispatches and
	// swap the ping-pong atlases. `p_tlas` / `p_scene_uniform_set` are unused by
	// the Task 4 empty kernels (no probe tracing yet) and reserved for Task 5.
	void update(RID p_tlas, RID p_scene_uniform_set, const WRCFrameParams &p_frame_params);

	// Current read (front) atlases. Valid only after ensure_resources().
	RID get_radiance_atlas() const { return radiance_atlas[read_index]; }
	RID get_distance_atlas() const { return distance_atlas[read_index]; }

	void free_resources();

private:
	struct PushConstant {
		uint32_t mode;
		uint32_t cascade_count;
		uint32_t grid;
		uint32_t oct_res;
		uint32_t atlas_width;
		uint32_t atlas_height;
		uint32_t rays_this_frame;
		uint32_t frame_index;
		float base_spacing;
		float temporal_n_cap;
		float feedback_damping;
		float view_prioritization;
		float camera_pos[3];
		uint32_t pad0;
		int32_t scroll_delta[4][4];
	};

	RtgiWorldRadianceCacheShaderRD shader;
	RID shader_version;
	RID pipeline;

	// Ping-pong atlases owned directly by the effect. `read_index` selects the
	// front (read) atlas; `1 - read_index` is the back (write) atlas.
	RID radiance_atlas[2]; // RGBA16F, .rgb = radiance, .a = confidence.
	RID distance_atlas[2]; // RG16F, distance moments (mean, mean^2).
	RID metadata_atlas[2]; // RGBA8, per-probe metadata (age/flags).
	uint32_t read_index = 0;

	RtgiWrc::ClipmapParams cached_params;
	Size2i atlas_size;
	bool resources_valid = false;

	Size2i _atlas_size(const RtgiWrc::ClipmapParams &p_params) const;
	void _allocate_atlases(const RtgiWrc::ClipmapParams &p_params);
};

} // namespace RendererRD
