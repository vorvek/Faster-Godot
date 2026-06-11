/**************************************************************************/
/*  rtgi_world_radiance_cache.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/effects/rtgi_wrc_math.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_world_radiance_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_wrc_gi_consumer.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// Per-frame scalars + clipmap recenter deltas for the World Radiance Cache.
// The clipmap is camera-centered; `scroll_delta[k]` is the integer probe-space
// shift of cascade `k` since last frame (see RtgiWrc::recenter_delta). The
// zero-filled defaults keep the scroll/accumulate dispatches no-ops until the
// caller fills the real per-frame values.
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
// Structure: a single compute shader
// with a `mode` push-constant driving scroll (mode 0) and accumulate (mode 1)
// dispatches over the atlas, with ping-pong read/write atlases swapped per
// update. The probe rays themselves are traced by the WRC raygen dispatch
// (scene_raytracing_raygen.glsl); the accumulate folds its ray results in.
class RTGIWorldRadianceCache {
public:
	RTGIWorldRadianceCache();
	~RTGIWorldRadianceCache();

	// Allocate (or reallocate on param change) the ping-pong atlases sized from
	// `params`. `view_count` is accepted for signature parity with other effects;
	// the WRC is world-space so the atlases are not per-view. Returns true when a
	// (re)allocation happened.
	bool ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, const RtgiWrc::ClipmapParams &p_params, int p_view_count);

	// Allocate (or reallocate on growth) the per-frame probe-update ray-result
	// SSBO consumed by the WRC accumulate kernel. One entry per ray ==
	// one (probe, direction) octahedral texel; layout mirrors STRC's
	// RTGISTRCProbeRayResult (3 x vec4 = 48 bytes). Returns true on (re)alloc.
	bool ensure_ray_result_buffer(uint32_t p_rays_per_frame);
	RID get_ray_result_buffer() const { return ray_result_buffer; }

	// Record the scroll (mode 0) + accumulate (mode 1) compute dispatches and
	// swap the ping-pong atlases. `p_tlas` / `p_scene_uniform_set` are accepted
	// for signature parity but unused: the probe rays are traced by the WRC
	// raygen dispatch, which fills the ray-result SSBO the accumulate consumes.
	void update(RID p_tlas, RID p_scene_uniform_set, const WRCFrameParams &p_frame_params);

	// Current read (front) atlases. Valid only after ensure_resources().
	RID get_radiance_atlas() const { return radiance_atlas[read_index]; }
	RID get_distance_atlas() const { return distance_atlas[read_index]; }

	// WRC-GI debug view: a VALIDATION per-pixel CONSUMER of the cache.
	// For each screen pixel it reconstructs the WORLD position from `p_depth`
	// (corrected reverse-Z device depth) + the depth-corrected camera matrices,
	// decodes the WORLD normal from
	// `p_normal_roughness` (a VIEW-space G-buffer normal rotated to world), and
	// samples rtgi_wrc_sample_irradiance() against the cache's cosine-integrated
	// irradiance. The RAW linear irradiance is written to a debug image then
	// blitted (un-tonemapped) to `p_dest_fb`, so the furnace gate can read
	// measurable linear values. `p_params` MUST be the same ClipmapParams the
	// atlases were built from; `p_camera_pos` is the clipmap center (cam origin).
	// `p_strength` is retained for the debug UBO layout; callers always pass 1.0
	// since the artistic strength knob was removed.
	void render_gi_debug(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_depth, RID p_normal_roughness, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const RtgiWrc::ClipmapParams &p_params, const Vector3 &p_camera_pos, float p_strength, RID p_dest_fb, const Size2i &p_size);

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

	// WRC-GI debug consumer: its own full-screen compute shader that
	// reads depth + normal-roughness and samples the cache's irradiance. Set up in
	// the constructor exactly like the update `shader` above.
	//
	// These params are passed via a UBO (set 0, binding 5) rather than a push
	// constant: the two mat4s alone are 128 bytes, which already hits
	// RenderingDevice's MAX_PUSH_CONSTANT_SIZE (128) cap, and the scalars push the
	// total to 176 bytes. UBOs are uncapped. The member layout below EXACTLY
	// matches the std140 `GiDebugParams` block in rtgi_wrc_gi_consumer.glsl:
	// scalars 0..48 (camera_pos vec3 @ 32 + strength @ 44), then two 16-byte-aligned
	// mat4s at offsets 48 and 112 (total 176 bytes).
	struct GiDebugUBO {
		int32_t cascade_count;
		int32_t grid;
		int32_t oct_res;
		float base_spacing;

		float occlusion_bias_spacing;
		float min_variance;
		int32_t screen_width;
		int32_t screen_height;

		float camera_pos[3];
		float strength; // packs into camera_pos's std140 4th slot (was pad0); WRC artistic multiplier.

		float inv_projection[16];
		float inv_view[16];
	};

	RtgiWrcGiConsumerShaderRD gi_debug_shader;
	RID gi_debug_shader_version;
	RID gi_debug_pipeline;

	// Uniform buffer carrying GiDebugUBO (the consumer's params; see above for why
	// a UBO and not a push constant). Created lazily on first render_gi_debug() and
	// bound at set 0, binding 5; freed in free_resources().
	RID gi_debug_ubo;

	// Lazily (re)allocated RGBA16F image the consumer writes the raw linear
	// irradiance into, sized to the consumed G-buffer (internal) size; blitted to
	// the destination framebuffer afterwards.
	RID gi_debug_image;
	Size2i gi_debug_image_size;
	void _ensure_gi_debug_image(const Size2i &p_size);

	// Ping-pong atlases owned directly by the effect. `read_index` selects the
	// front (read) atlas; `1 - read_index` is the back (write) atlas.
	RID radiance_atlas[2]; // RGBA16F, .rgb = radiance, .a = confidence.
	RID distance_atlas[2]; // RG16F, distance moments (mean, mean^2).
	RID metadata_atlas[2]; // RGBA8, per-probe metadata (age/flags).
	uint32_t read_index = 0;

	// Per-frame probe-update ray results (binding 107 in the raytracing uniform
	// set). Written by the WRC probe-update raygen, consumed by the accumulate
	// kernel. Reallocated only when the requested ray count exceeds capacity.
	RID ray_result_buffer;
	uint32_t ray_result_capacity = 0;

	RtgiWrc::ClipmapParams cached_params;
	Size2i atlas_size;
	bool resources_valid = false;

	Size2i _atlas_size(const RtgiWrc::ClipmapParams &p_params) const;
	void _allocate_atlases(const RtgiWrc::ClipmapParams &p_params);
};

} // namespace RendererRD
