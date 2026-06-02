/**************************************************************************/
/*  rtgi_gi_resolve.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/shaders/effects/rtgi_gi_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// RTGI GI Resolve RD effect: the production per-pixel CONSUMER of the SPG/WRC
// probes (A3). It promotes the A2 thin debug-integrate (rtgi_spg_gi_consumer.glsl)
// into a real module with its OWN ping-pong screen-GI buffers and a debug view,
// run under the radiance_probes pipeline AFTER the Screen Probe Gather. Unlike the
// debug-only consumer, INTEGRATE writes a LIGHTING-SPACE diffuse buffer (the
// confidence-weighted cosine-average A of incident radiance, with NO albedo and NO
// extra 1/PI -- the demod is PI-free at storage, so L_o = albedo * A); the debug
// view shows that RAW lighting-space output (no albedo) -- the per-surface remod by
// albedo lands at the composite (T4/T5). The spec buffer is written 0
// in T0 (T1 fills the rough-spec radiance). The temporal (T2) + spatial (T3) modes
// are declared here but NOT implemented in T0.
//
// Structure mirrors RTGIScreenProbeGather: a single compute shader with a `mode`
// push-constant, realloc-guarded ensure_resources(), an _allocate() that
// texture_create()s + texture_clear()s the ping-pong buffers, getters for the
// front buffers, and a free_resources(). The world-pos reconstruction matrices are
// too big for the push constant, so they travel in a GiResolveUBO (std140), like
// the WRC consumer's GiDebugUBO.
class RTGIGIResolve {
public:
	// Per-frame resolve tunables (resolved from the per-preset Project Settings in
	// T7; for now the defaults below are used). spatial_iterations drives the a-trous
	// passes (T3), temporal_n_cap the history responsiveness (T2), and the rough-spec
	// fields the deferred sharp-reflections domain (T1).
	struct GiResolveParams {
		int spatial_iterations = 1; // a-trous iters (0 = off; 1 default; 2 = escalation).
		float temporal_n_cap = 16.0f; // history responsiveness.
		float rough_spec_roughness_cutoff = 0.5f; // below = sharp-reflections domain (deferred).
		bool rough_spec_enabled = true;
		float history_rejection = 1.0f; // depth/normal/mesh-id tolerance scale.
	};

	// Per-frame scalars handed to a single resolve dispatch. Mirrors how the A2
	// consumer's GiDebugUBO sourced the WRC clipmap params for the fallback sample +
	// the SPG grid geometry for probe addressing; the tunables are carried on
	// `params` (refreshed every frame by ensure_resources, the single source of truth).
	struct GiResolveFrameParams {
		uint32_t frame_index = 0;
		GiResolveParams params;
		// WRC clipmap params for the fallback sample (mirror the A2 consumer's GiDebugUBO source):
		uint32_t wrc_grid = 16;
		uint32_t wrc_cascade_count = 4;
		float wrc_base_spacing = 0.0f;
		// SPG grid geometry for probe addressing:
		uint32_t spg_grid_w = 0;
		uint32_t spg_grid_h = 0;
		uint32_t spg_oct_res = 8;
		uint32_t spg_spacing_f = 16;
	};

	RTGIGIResolve();
	~RTGIGIResolve();

	// Allocate (or reallocate on render-size change) the ping-pong screen-GI buffers
	// sized from the internal render size. `p_rb` is accepted for signature parity
	// with the other effects (the resolve owns its own textures). `p_params` is
	// cached so the passes read this frame's tunables. Returns true when a
	// (re)allocation happened.
	bool ensure_resources(Ref<RenderSceneBuffersRD> p_rb, const GiResolveParams &p_params, const Size2i &p_render_size);

	// Record the INTEGRATE dispatch: per pixel it reconstructs the WORLD position +
	// normal from the depth + normal-roughness G-buffers, locates the 4 surrounding
	// SPG probes, cosine-integrates each probe's hemioct tile against the surface
	// normal (confidence-weighted normalizer), bilinearly blends them, falls back to
	// the WRC irradiance when no probe qualifies, and writes the LIGHTING-SPACE A to
	// the diffuse buffer (spec buffer 0 for now). TEMPORAL/SPATIAL (T2/T3) will
	// ping-pong the GI buffers; T0 implements INTEGRATE only. `p_inv_proj` is the
	// clip->view inverse projection; `p_inv_view` the view->world transform.
	void run_resolve(RID p_depth, RID p_normal_roughness, RID p_velocity,
			RID p_spg_radiance, RID p_spg_header_plane, RID p_spg_header_aux,
			RID p_wrc_radiance, RID p_wrc_distance,
			const GiResolveFrameParams &p_frame, const Projection &p_inv_proj, const Transform3D &p_inv_view);

	RID get_diffuse_gi() const { return diffuse_gi[read_index]; } // RGBA16F: rgb = lighting-space A, a = confidence/variance.
	RID get_spec_gi() const { return spec_gi[read_index]; } // RGBA16F: rgb = rough-spec radiance, a = variance.

	// RTGI-RESOLVE-GI debug view (A3-T0): the VALIDATION per-pixel view of the resolved
	// screen GI (the resolve analogue of RTGIScreenProbeGather::render_gi_debug). It
	// outputs the resolve's RAW lighting-space output -- out = diffuse_gi.rgb +
	// spec_gi.rgb (spec 0 in T0) -- then blits the RAW linear value (no albedo, no
	// tonemap) so the furnace gate can read measurable linear values. NO albedo
	// remodulation here: the per-surface remod (L_o = albedo * A) is applied at the
	// composite (T4/T5), where the full G-buffer albedo exists (this view forces a
	// depth-prepass that does not populate rt_albedo_metalness). On the furnace A ~= L
	// (albedo-independent), matching the SPG-GI gate. `p_size` is the consumed G-buffer
	// (internal) size.
	void render_resolve_debug(Ref<RenderSceneBuffersRD> p_rb, const Size2i &p_size, RID p_dest_fb);

	void free_resources();

private:
	// Mode selectors live in the .cpp (RESOLVE_MODE_*). The push constant is 16 x 4 B =
	// 64 B (a multiple of 16, so the std430-rounded size matches and the dispatch is not
	// silently rejected). The world-pos reconstruction matrices (inv_proj + inv_view)
	// are too big for the push, so they travel in GiResolveUBO below. Matches the std430
	// `Params` block in rtgi_gi_resolve.glsl EXACTLY.
	struct PushConstant {
		uint32_t mode;
		uint32_t frame_index;
		uint32_t screen_w;
		uint32_t screen_h;
		uint32_t spatial_iter;
		uint32_t cur_iter;
		uint32_t spg_grid_w;
		uint32_t spg_grid_h;
		uint32_t spg_oct_res;
		uint32_t spg_spacing_f;
		float temporal_n_cap;
		float rough_cutoff;
		uint32_t rough_enabled;
		uint32_t wrc_grid;
		uint32_t wrc_cascade_count;
		float wrc_base_spacing;
	};

	// World-pos reconstruction needs inv_proj + inv_view (two mat4s = 128 bytes, which
	// alone hit RenderingDevice's 128-byte push-constant cap), so they live in a UBO
	// (mirrors RTGIWorldRadianceCache::GiDebugUBO). The layout below EXACTLY matches the
	// std140 `GiResolveUBO` block in rtgi_gi_resolve.glsl: two 16-byte-aligned mat4s at
	// offsets 0 and 64 (each 64 bytes), total 128 bytes (a multiple of 16).
	struct GiResolveUBO {
		float inv_projection[16]; // offset 0: clip -> view.
		float inv_view[16]; // offset 64: view -> world (camera transform).
	};

	RtgiGiResolveShaderRD shader;
	RID shader_version;
	RID pipeline; // INTEGRATE/TEMPORAL/SPATIAL/DEBUG_GI all share this shader (one set-0 layout).

	// Uniform buffer carrying GiResolveUBO (the resolve's reconstruction matrices; see
	// above for why a UBO and not a push constant). Created lazily on first run_resolve()
	// and bound for the dispatch; freed in free_resources().
	RID resolve_ubo;

	// Ping-pong screen-GI buffers owned directly by the effect. `read_index` selects
	// THIS frame's (front) set; `1 - read_index` is the previous frame's. INTEGRATE
	// writes diffuse_gi[read_index] + spec_gi[read_index]; TEMPORAL/SPATIAL (T2/T3)
	// ping-pong these. get_diffuse_gi()/get_spec_gi() return the front (read) set.
	RID diffuse_gi[2]; // RGBA16F: rgb = lighting-space A, a = confidence/variance.
	RID spec_gi[2]; // RGBA16F: rgb = rough-spec radiance, a = variance.
	uint32_t read_index = 0;

	// Dedicated scratch / debug-dest image (RGBA16F, render size), allocated alongside
	// the GI buffers. render_resolve_debug writes the raw lighting-space resolve output
	// here for the blit (the resolve analogue of the WRC/SPG gi_debug_image). It also fills the
	// neutral IMAGE bindings of the unified set-0 layout the active mode does not write,
	// so each dispatch keeps every binding valid while ensuring no single texture is bound
	// as BOTH a sampler and a storage image in one descriptor set (which would be a
	// same-resource read+write). Never actually read+written by either mode.
	RID gi_debug_image;

	GiResolveParams cached_params;
	Size2i cached_render_size;
	bool resources_valid = false;

	void _allocate(const Size2i &p_render_size);
};

} // namespace RendererRD
