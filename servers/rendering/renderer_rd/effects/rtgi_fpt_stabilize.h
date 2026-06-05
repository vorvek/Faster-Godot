/**************************************************************************/
/*  rtgi_fpt_stabilize.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/rtgi_fpt_stabilize.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// RTGI Full Path Tracing primary-direct temporal stabilizer (FPT-fast, no-upscaler path).
//
// FPT-fast shades the directly-visible surface with a per-frame stochastic NEE direct term, so the
// primary-direct color in RB_TEX_RAYTRACING boils frame to frame even with a frozen camera. This
// effect temporally accumulates that color BEFORE the composite so the final image is stable.
//
// It is the per-pixel analogue of RTGIGIResolve's TEMPORAL accumulate, and mirrors that effect's
// structure: a single compute shader, OWN ping-pong buffers with a read_index flip at the top of
// each run (no per-frame copy, no RenderBuffer-owned textures), a realloc-guarded ensure_resources(),
// a getter for the front buffer, and free_resources(). It is only ever invoked from the FPT branch,
// so Hybrid is byte-identical.
//
// History rejection is a SAME-SURFACE depth + normal gate from the CURRENT-frame RT guides (NOT the
// rt_history_id / *_history_*_prev buffers, which the FPT primary-direct does not maintain), and the
// blend resists large dark drops so a sub-native primary mis-visibility cannot latch into a black
// blotch.
//
// SCOPE: invoked ONLY when no jittered temporal upscaler (FSR2/XeSS/TAA) is active. Pre-averaging the
// primary is incompatible with such an upscaler's own jittered accumulation, so the caller gates the
// dispatch on the upscaler mode; under a jittered upscaler the composite uses the raw RT color.
class RTGIFPTStabilize {
public:
	// Per-frame tunables. Defaults match the GI resolve's temporal accumulate (n_cap 16, 5% relative
	// depth tolerance) with a firefly clamp width and a same-surface normal cosine for the primary signal.
	struct StabilizeParams {
		float n_cap = 16.0f; // history responsiveness (the 1/n blend caps n here).
		float firefly_k = 1.5f; // YCoCg neighborhood clamp width, in sigmas.
		float normal_threshold = 0.9f; // min dot(cur_n, prev_n) to accept history.
		float depth_rel_tol = 0.05f; // relative view-z tolerance to accept history.
	};

	RTGIFPTStabilize();
	~RTGIFPTStabilize();

	// Allocate (or reallocate on rt-size change) the ping-pong stabilized-color buffers. Returns true
	// when a (re)allocation happened (the next run_stabilize forces a reset frame internally).
	bool ensure_resources(const Size2i &p_rt_size);

	// Record the stabilize dispatch: reproject the previous accumulated color with the rt-size velocity
	// guide, accept it on a same-surface depth + normal gate from the current guides, firefly-clamp the
	// current sample, and blend (1/n, n-capped, dark-drop-resistant), writing the front buffer.
	// p_frame_index 0 (or p_force_reset, e.g. a camera cut) skips the reproject for a pure seed.
	void run_stabilize(RID p_cur_color, RID p_velocity, RID p_history_validity,
			RID p_viewz_hitdist, RID p_normal_roughness,
			const Size2i &p_rt_size, uint32_t p_frame_index, bool p_force_reset,
			const StabilizeParams &p_params);

	bool has_stable() const { return resources_valid && stable[read_index].is_valid(); }
	// The stabilized color the composite blits in place of the raw FPT primary color.
	RID get_stable() const { return stable[read_index]; }

	void free_resources();

private:
	// Matches the std430 `Params` push-constant block in rtgi_fpt_stabilize.glsl EXACTLY (12 x 4 B =
	// 48 B, a multiple of 16, so the std430-rounded size matches and the dispatch is not rejected).
	struct PushConstant {
		uint32_t screen_w;
		uint32_t screen_h;
		uint32_t frame_index;
		uint32_t reset;
		float n_cap;
		float firefly_k;
		float normal_threshold;
		float depth_rel_tol;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
		uint32_t pad3;
	};

	RtgiFptStabilizeShaderRD shader;
	RID shader_version;
	RID pipeline;

	// OWN ping-pong stabilized-color buffers (RGBA16F at rt_size). run_stabilize flips read_index at
	// the top of the frame, so AFTER the flip [read_index] is THIS frame's output and [1 - read_index]
	// is the previous frame's accumulated result (the reproject history). get_stable() returns
	// [read_index], which next frame becomes the history.
	RID stable[2];
	uint32_t read_index = 0;

	Size2i cached_rt_size;
	bool resources_valid = false;
	// Set true on (re)allocation; consumed by the next run_stabilize as a one-frame reset (the freshly
	// cleared history must not be reprojected). OR-ed with the caller's p_force_reset.
	bool needs_reset = false;

	void _allocate(const Size2i &p_rt_size);
};

} // namespace RendererRD
