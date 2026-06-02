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
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_spg_accumulate.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// Screen Probe Gather (SPG) RD effect: places one screen probe per F x F tile of
// the primary-visibility G-buffer and (in later tasks) gathers incident radiance
// into a per-probe octahedral atlas. Unlike the World Radiance Cache (which owns
// world-space ping-pong atlases), the SPG grid is SCREEN-space: it is rebuilt
// every frame from the depth + normal-roughness + velocity G-buffers, so its
// textures are sized from the render (internal) resolution.
//
// Structure mirrors RTGIWorldRadianceCache: ping-pong RID arrays + read_index, a
// realloc-guarded ensure_resources(), and an _allocate() that texture_create()s +
// texture_clear()s the grid textures. The PLACE pass (mode 0) lives in
// rtgi_screen_probe_gather.glsl; the temporal accumulate (REPROJECT + BLEND) plus the
// same-surface 3x3 spatial filter (SPATIAL, A2-T4) live in their OWN shader
// rtgi_spg_accumulate.glsl with a distinct set-0 layout / pipeline (it binds the
// radiance ping-pong + ray-result SSBO + the filtered-atlas output, which PLACE never
// touches, so separate shaders keep each pass binding exactly what it uses).
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

	// Record the temporal-accumulate + spatial-filter dispatches (A2-T3 + A2-T4) on a
	// single compute list (barriers between): a REPROJECT pass over the whole radiance
	// atlas (motion-reproject + plane-match + re-orient-on-read of the previous frame's
	// radiance), then a BLEND pass over this frame's gather rays (sample-counted 1/n
	// accumulate into the reprojected history), then a SPATIAL pass (same-surface 3x3
	// neighbor-probe smoothing of the just-accumulated atlas into radiance_filtered).
	// Reads the PREVIOUS (1 - read_index) atlas/headers and writes the CURRENT
	// (read_index) atlas + radiance_filtered; performs NO ping-pong swap (the frame
	// swap is done in run_placement). Must be called AFTER the gather has filled the
	// ray-result SSBO for this frame. A no-op if resources are invalid.
	void run_accumulate(const SpgFrameParams &p_frame);

	// Current read (front) grid textures. Valid only after ensure_resources().
	RID get_header_plane() const { return header_plane[read_index]; } // RGBA32F: xyz = world_pos, w = linear_depth (<= 0 invalid).
	RID get_header_aux() const { return header_aux[read_index]; } // RGBA16F: xy = oct_normal, zw = screen_motion.
	RID get_radiance_atlas() const { return radiance_atlas[read_index]; } // RGBA16F: rgb = radiance, a = confidence.

	// SPATIAL output (A2-T4); A3 + the debug-integrate read this. Falls back to the
	// unfiltered current atlas until radiance_filtered is allocated.
	RID get_radiance_filtered() const { return radiance_filtered.is_valid() ? radiance_filtered : radiance_atlas[read_index]; }

	// Probe-grid dimensions (ceil(render_size / spacing_f)); also the header texture
	// size. Valid only after ensure_resources(). The single source of truth for the
	// gather ray count + dispatch grid, so the SSBO / atlas / dispatch cannot drift.
	Size2i get_grid_size() const { return grid_size; }

	void free_resources();

private:
	// PLACE-pass push constant (8 x uint32 = 32 B). Matches the std430 `Params` block
	// in rtgi_screen_probe_gather.glsl EXACTLY.
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

	// Accumulate-pass push constant: 9 x uint32 + 1 x float = 40 B raw, padded with two
	// uint32 to 48 B (std430 rounds a push-constant block to a multiple of 16, so the
	// pipeline requires the rounded 48 B). Matches the std430 `Params` block in rtgi_spg_accumulate.glsl
	// EXACTLY: atlas_width/atlas_height drive the REPROJECT bounds guard; rays_this_frame
	// the BLEND dispatch; temporal_n_cap the 1/n sample-count weight.
	struct AccumPushConstant {
		uint32_t mode;
		uint32_t grid_w;
		uint32_t grid_h;
		uint32_t oct_res;
		uint32_t spacing_f;
		uint32_t frame_index;
		uint32_t atlas_width;
		uint32_t atlas_height;
		uint32_t rays_this_frame;
		float temporal_n_cap;
		// std430 rounds a push-constant block up to a multiple of 16 bytes, so the
		// shader's pipeline requires 48 B (40 rounded up). The first trailing slot
		// (formerly pad0) carries the SPATIAL filter radius (A2-T4); pad1 is the second
		// pad slot. Keep sizeof(AccumPushConstant) == 48 so the dispatch matches.
		uint32_t spatial_radius;
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

	// Temporal-accumulate shader/pipeline (A2-T3). A SEPARATE shader from PLACE: its
	// set-0 layout (radiance ping-pong images + prev/cur headers + ray-result SSBO)
	// differs from PLACE's (G-buffer samplers + header writes + camera UBO), and a
	// single GLSL shader cannot declare two different set-0 layouts. Set up in the
	// constructor exactly like `shader`/`pipeline` above.
	RtgiSpgAccumulateShaderRD accum_shader;
	RID accum_shader_version;
	RID accum_pipeline;

	// Uniform buffer carrying PlaceUBO (the PLACE pass's camera matrices; see above
	// for why a UBO and not a push constant). Created lazily on first run_placement()
	// and bound for the dispatch; freed in free_resources().
	RID place_ubo;

	// Ping-pong grid textures owned directly by the effect. `read_index` selects THIS
	// frame's (current) set; `1 - read_index` is the previous frame's. The frame swap
	// happens at the START of run_placement (read_index flips there), so within a
	// frame: placement writes header[read_index]; the gather reads header[read_index]
	// (via get_header_*()); the accumulate reads radiance/header[1 - read_index]
	// (previous) and writes radiance[read_index] (current); get_radiance_atlas() ==
	// radiance[read_index] feeds the debug blit. The accumulate performs NO swap.
	RID radiance_atlas[2]; // RGBA16F, .rgb = radiance, .a = confidence.
	RID header_plane[2]; // RGBA32F, .xyz = world_pos, .w = linear_depth (<= 0 invalid).
	RID header_aux[2]; // RGBA16F, .xy = octahedral world normal, .zw = screen motion.
	uint32_t read_index = 0;

	// SPATIAL output (A2-T4): the SPATIAL pass of run_accumulate writes the same-surface
	// 3x3-filtered radiance here (RGBA16F, same format/size as radiance_atlas). NOT
	// ping-ponged -- it is a pure per-frame derivative of the current atlas that A3 +
	// the debug-integrate consume via get_radiance_filtered().
	RID radiance_filtered;

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
