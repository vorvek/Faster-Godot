/**************************************************************************/
/*  rtgi_screen_probe_gather.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_screen_probe_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// Screen Probe Gather (SPG) RD effect: places one screen probe per F x F tile of
// the primary-visibility G-buffer and (in later tasks) gathers incident radiance
// into a per-probe octahedral atlas. Unlike the World Radiance Cache (which owns
// world-space ping-pong atlases), the SPG grid is SCREEN-space: it is rebuilt
// every frame from the depth + normal-roughness + velocity G-buffers, so its
// textures are sized from the render (internal) resolution.
//
// Structure mirrors RTGIWorldRadianceCache exactly: a single compute shader with
// a `mode` push-constant, ping-pong RID arrays + read_index, a realloc-guarded
// ensure_resources(), and an _allocate() that texture_create()s + texture_clear()s
// the grid textures. THIS task (A2-T1) ships only the PLACE pass; the gather
// (T2), accumulate (T3) and spatial-filter (T4) modes come later.
class RTGIScreenProbeGather {
public:
	// Per-quality tunables (resolved from per-preset Project Settings in T7; for
	// now the defaults below are used). `spacing_f` is the tile edge in pixels
	// (one probe per spacing_f x spacing_f tile); `oct_res` is the per-probe
	// octahedral tile resolution; the remainder drive the gather/accumulate/spatial
	// passes wired in later tasks.
	struct SpgParams {
		int spacing_f = 16;
		int oct_res = 8;
		int dirs_per_probe_per_frame = 4;
		float temporal_n_cap = 16.0f;
		int spatial_radius = 1;
		float rt_fallback_confidence = 0.5f;
	};

	// Per-frame scalars handed to a single SPG dispatch. `grid_w` / `grid_h` are
	// the probe-grid dimensions (ceil(render_size / spacing_f)); `rays_this_frame`
	// is consumed by the gather pass (T2).
	struct SpgFrameParams {
		uint32_t grid_w = 0;
		uint32_t grid_h = 0;
		uint32_t frame_index = 0;
		uint32_t rays_this_frame = 0;
		// (Tunables are NOT duplicated here: ensure_resources() refreshes cached_params
		// every frame, so all passes read those as the single source of truth.)
	};

	RTGIScreenProbeGather();
	~RTGIScreenProbeGather();

	// Allocate (or reallocate on grid / oct_res change) the ping-pong grid textures
	// sized from `p_params` + `p_render_size`. Returns true when a (re)allocation
	// happened. `p_rb` is accepted for signature parity with the other effects (the
	// SPG owns its own textures rather than the render-buffers scope).
	bool ensure_resources(Ref<RenderSceneBuffersRD> p_rb, const SpgParams &p_params, const Size2i &p_render_size);

	// Allocate (or grow) the per-frame gather ray-result SSBO consumed by the SPG
	// gather kernel (T2). 32-byte stride == 2 x vec4 per ray. Grow-only. Returns
	// true on (re)alloc.
	bool ensure_ray_result_buffer(uint32_t p_rays_per_frame);
	RID get_ray_result_buffer() const { return ray_result_buffer; }

	// Record the PLACE dispatch: one thread per probe (gx, gy) finds the nearest
	// valid G-buffer pixel in its tile, reconstructs the WORLD position + normal +
	// screen motion, and writes the probe header. `p_velocity` may be a default
	// black texture (motion reads 0) on a static scene. `p_inv_projection` is the
	// clip->view inverse projection; `p_cam_transform` is the view->world transform.
	void run_placement(Ref<RenderSceneBuffersRD> p_rb, RID p_depth, RID p_normal_roughness, RID p_velocity, const SpgFrameParams &p_frame, const Projection &p_inv_projection, const Transform3D &p_cam_transform, const Size2i &p_render_size);

	// Current read (front) grid textures. Valid only after ensure_resources().
	RID get_header_plane() const { return header_plane[read_index]; } // RGBA32F: xyz = world_pos, w = linear_depth (<= 0 invalid).
	RID get_header_aux() const { return header_aux[read_index]; } // RGBA16F: xy = oct_normal, zw = screen_motion.
	RID get_radiance_atlas() const { return radiance_atlas[read_index]; } // RGBA16F: rgb = radiance, a = confidence.

	// Probe-grid dimensions (ceil(render_size / spacing_f)); also the header texture
	// size. Valid only after ensure_resources(). The single source of truth for the
	// gather ray count + dispatch grid, so the SSBO / atlas / dispatch cannot drift.
	Size2i get_grid_size() const { return grid_size; }

	void free_resources();

private:
	struct PushConstant {
		uint32_t mode;
		uint32_t grid_w;
		uint32_t grid_h;
		uint32_t oct_res;
		uint32_t spacing_f;
		uint32_t frame_index;
		uint32_t pad0;
		uint32_t pad1;
	};

	// PLACE needs the two camera matrices (clip->view + view->world) = 128 bytes of
	// std140 mat4s, which alone hit RenderingDevice's 128-byte push-constant cap, so
	// they live in a UBO (mirrors RTGIWorldRadianceCache::GiDebugUBO). The layout
	// below EXACTLY matches the std140 `PlaceParams` block in the SPG shader: two
	// 16-byte-aligned mat4s (offsets 0 and 64) then four trailing ints (offset 128).
	struct PlaceUBO {
		float inv_projection[16];
		float inv_view[16];
		int screen_width;
		int screen_height;
		int pad0;
		int pad1;
	};

	RtgiScreenProbeGatherShaderRD shader;
	RID shader_version;
	RID pipeline;

	// Uniform buffer carrying PlaceUBO (the PLACE pass's camera matrices; see above
	// for why a UBO and not a push constant). Created lazily on first run_placement()
	// and bound for the dispatch; freed in free_resources().
	RID place_ubo;

	// Ping-pong grid textures owned directly by the effect. `read_index` selects the
	// front (read) set; `1 - read_index` is the back (write) set. The ping-pong swap
	// is introduced by the T3 accumulate; T1's PLACE writes the front set directly
	// (write index == read_index) so get_header_*() returns what PLACE just wrote.
	RID radiance_atlas[2]; // RGBA16F, .rgb = radiance, .a = confidence.
	RID header_plane[2]; // RGBA32F, .xyz = world_pos, .w = linear_depth (<= 0 invalid).
	RID header_aux[2]; // RGBA16F, .xy = octahedral world normal, .zw = screen motion.
	uint32_t read_index = 0;

	// Per-frame gather ray results consumed by the SPG gather kernel (T2). 32-byte
	// stride (2 x vec4). Reallocated only when the requested ray count exceeds
	// capacity (grow-only).
	RID ray_result_buffer;
	uint32_t ray_result_capacity = 0;

	SpgParams cached_params;
	Size2i cached_render_size;
	Size2i atlas_size; // radiance atlas size = (grid_w * oct_res, grid_h * oct_res).
	Size2i grid_size; // probe-grid size = (grid_w, grid_h); also the header texture size.
	bool resources_valid = false;

	void _allocate(const SpgParams &p_params, const Size2i &p_render_size);
};

} // namespace RendererRD
