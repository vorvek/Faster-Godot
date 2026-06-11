#[compute]

#version 450

#VERSION_DEFINES

// Screen Probe Gather (SPG) placement compute shader.
//
// Places one screen probe per spacing_f x spacing_f tile of the primary-visibility
// G-buffer (PLACE): one thread per probe finds the nearest valid G-buffer pixel in
// its tile, reconstructs the WORLD position + normal + screen motion, and writes the
// probe header. This shader runs ONLY the PLACE pass (mode 0). The temporal
// accumulate (motion-reproject + 1/n blend of the gather ray results into the
// radiance atlas) lives in its OWN shader, rtgi_spg_accumulate.glsl: that
// pass binds a different set of resources (the radiance ping-pong + ray-result
// SSBO), so a SEPARATE set-0 layout / pipeline keeps each pass binding exactly what
// it uses (mirrors how the WRC update shader binds all of its declared bindings on
// every dispatch). The spatial filter also lives there.
//
// Coordinate-space contract (verified against rtgi_wrc_gi_consumer.glsl):
//   * The depth buffer (RB_TEX_DEPTH) holds the CORRECTED [0,1] reverse-Z device depth;
//     the cleared far/sky value is 0.0. View position is inv_projection * vec4(2*uv-1,
//     depth, 1) (homogeneous divide) -- inv_projection is the inverse of the CORRECTED
//     projection (set_depth_correction + TAA jitter, the SceneData UBO convention), so it
//     already encodes the reverse-Z/y-flip mapping; no z remap.
//   * The normal-roughness G-buffer normal is in VIEW space (normalize(rgb*2-1));
//     we rotate it VIEW->WORLD via mat3(inv_view).
//   * inv_view == the camera's view->world transform, so world pos is
//     inv_view * view_pos (full affine) and world normal is mat3(inv_view) * n.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Shared SPG hemi-oct basis math (local +Z = anchor normal), shared with the
// gather / accumulate passes.
#include "../raytracing/rtgi_spg_inc.glsl"
// Full-sphere octahedral encode (vec3_to_oct) for the probe header normal.
#include "../oct_inc.glsl"

// Mode selector (matches SPG_MODE_PLACE in rtgi_screen_probe_gather.cpp). This
// shader runs only PLACE; the temporal accumulate and the spatial filter live in
// a separate shader (rtgi_spg_accumulate.glsl).
#define SPG_MODE_PLACE 0u

layout(push_constant, std430) uniform Params {
	uint mode; // 0 = PLACE (this shader has no other modes).
	uint grid_w;
	uint grid_h;
	uint oct_res;
	uint spacing_f;
	uint frame_index;
	uint pad0;
	uint pad1;
}
params;

// Raw scene depth (reverse-Z hyperbolic), view-space normal-roughness, and screen
// motion. Bound as sampler2D for parity with the WRC consumer's set layout, but
// PLACE only does integer texelFetch()es.
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler2D velocity_buffer;
// Per-probe headers PLACE writes. header_plane: .xyz = world_pos, .w = linear_depth
// (<= 0 invalid). header_aux: .xy = octahedral world normal, .zw = screen motion (pixels,
// prev - cur; the UV velocity buffer scaled by the screen size at store time).
layout(set = 0, binding = 3, rgba32f) uniform restrict writeonly image2D header_plane_image;
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D header_aux_image;

// Two mat4s (128 bytes) hit RenderingDevice's 128-byte push-constant cap, so the
// PLACE camera matrices live in a UBO (mirrors the WRC consumer's GiDebugParams).
// Layout matches RTGIScreenProbeGather::PlaceUBO exactly.
layout(set = 0, binding = 5, std140) uniform PlaceParams {
	mat4 inv_projection; // clip -> view.
	mat4 inv_view; // view -> world (camera transform).
	// PREVIOUS-frame world -> clip (prev_cam_projection * prev_cam_view). Static geometry writes no
	// motion vector (the velocity buffer keeps its (-1,-1) clear sentinel there), so for the sentinel
	// the anchor's true screen motion is the CAMERA's: reproject the anchor world pos through this.
	mat4 prev_view_projection;
	int screen_width;
	int screen_height;
	int pad0;
	int pad1;
}
place;

// Reconstruct VIEW-space position from the raw depth buffer at integer pixel `pos`.
// Mirrors rtgi_wrc_gi_consumer.glsl::reconstruct_view_position: build a clip-space
// point from the pixel's NDC xy + raw depth z and run it through inv_projection.
vec3 reconstruct_view_position(ivec2 pos, float raw_depth) {
	vec4 clip;
	clip.xy = (2.0 * (vec2(pos) + vec2(0.5)) / vec2(place.screen_width, place.screen_height)) - 1.0;
	clip.z = raw_depth;
	clip.w = 1.0;
	vec4 view = place.inv_projection * clip;
	return view.xyz / view.w;
}

// PLACE: one thread per probe (gx, gy). Search outward from the tile center for the
// nearest pixel with valid geometry (raw_depth > 0); the first hit anchors the
// probe. A probe whose entire tile is sky writes an invalid header (.w <= 0).
void spg_place_main(ivec2 probe) {
	uint spacing = max(params.spacing_f, 1u);
	// Tile origin + center. The grid is ceil(screen / spacing), so the last
	// column/row tiles may extend past the screen; texelFetch is clamped below.
	ivec2 tile_origin = probe * int(spacing);
	ivec2 center = tile_origin + ivec2(int(spacing) / 2);
	ivec2 res = ivec2(place.screen_width, place.screen_height);

	// Spiral-ish outward search by Chebyshev radius from the tile center, capped at
	// spacing/2 so a probe only ever anchors inside its own tile. The first pixel
	// with raw_depth > 0 (real geometry; the cleared sky value is 0) wins.
	int max_radius = (int(spacing) + 1) / 2; // +1 so an odd spacing still reaches the tile's far edge.
	ivec2 found = ivec2(-1);
	float found_depth = 0.0;
	for (int r = 0; r <= max_radius && found.x < 0; r++) {
		for (int dy = -r; dy <= r && found.x < 0; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				// Only the ring at Chebyshev distance r (interior already scanned).
				if (max(abs(dx), abs(dy)) != r) {
					continue;
				}
				ivec2 p = center + ivec2(dx, dy);
				if (p.x < 0 || p.y < 0 || p.x >= res.x || p.y >= res.y) {
					continue;
				}
				float d = texelFetch(depth_buffer, p, 0).r;
				if (d > 0.0) {
					found = p;
					found_depth = d;
					break;
				}
			}
		}
	}

	if (found.x < 0) {
		// Empty (all-sky) tile: write an invalid probe so consumers skip it.
		imageStore(header_plane_image, probe, vec4(0.0, 0.0, 0.0, -1.0));
		imageStore(header_aux_image, probe, vec4(0.0));
		return;
	}

	// Reconstruct the anchor's WORLD position + WORLD normal + screen motion.
	vec3 view_pos = reconstruct_view_position(found, found_depth);
	vec3 world_pos = (place.inv_view * vec4(view_pos, 1.0)).xyz;
	float linear_depth = -view_pos.z; // view -Z is forward; linear depth is positive.

	vec3 enc = texelFetch(normal_roughness_buffer, found, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	// A depth-valid pixel can still carry a degenerate / unwritten normal; normalize()
	// of it yields NaN/garbage and vec3_to_oct would silently encode a wrong direction
	// (the WRC consumer guards this identically). Mark the probe invalid instead.
	if (dot(view_normal, view_normal) < 0.0001) {
		imageStore(header_plane_image, probe, vec4(0.0, 0.0, 0.0, -1.0));
		imageStore(header_aux_image, probe, vec4(0.0));
		return;
	}
	view_normal = normalize(view_normal);
	vec3 world_normal = normalize(mat3(place.inv_view) * view_normal);

	// Anchor screen motion. The velocity buffer (RB_TEX_VELOCITY) stores UV-space motion
	// (prev_uv - cur_uv; see scene_forward_clustered.glsl + rt_store_primary_velocity), so scale
	// by the screen size to store header_aux.zw in PIXELS, which is what the SPG accumulate
	// REPROJECT consumes (prev_cell = cell + motion_px / spacing). Storing the raw UV here is the
	// bug that made the probe reproject round to 0 and smear the indirect under motion.
	// STATIC geometry, however, writes NO motion vector (color_pass_inclusion_mask 0), so its
	// velocity stays at the (-1,-1) clear sentinel -> a bogus ~full-screen motion that fails the
	// reproject every frame, leaving the probe permanently reset to the WRC cold-start seed (it
	// never accumulates/sharpens, and the seed runs full-cost forever). For the sentinel, the
	// anchor's true screen motion is the CAMERA's: reproject its world pos through the previous
	// view-projection. Dynamic anchors (real velocity) keep the velocity-buffer motion.
	vec2 vel = texelFetch(velocity_buffer, found, 0).xy;
	vec2 motion;
	if (vel.x <= -1.0 && vel.y <= -1.0) {
		vec4 prev_clip = place.prev_view_projection * vec4(world_pos, 1.0);
		if (prev_clip.w > 0.0) {
			vec2 prev_px = ((prev_clip.xy / prev_clip.w) * 0.5 + 0.5) * vec2(place.screen_width, place.screen_height);
			motion = prev_px - (vec2(found) + vec2(0.5)); // prev - cur, in pixels (the stored convention).
		} else {
			motion = vec2(0.0); // anchor behind the previous camera: identity carry.
		}
	} else {
		motion = vel * vec2(place.screen_width, place.screen_height);
	}

	imageStore(header_plane_image, probe, vec4(world_pos, linear_depth));
	imageStore(header_aux_image, probe, vec4(vec3_to_oct(world_normal), motion));
}

void main() {
	if (params.mode == SPG_MODE_PLACE) {
		ivec2 probe = ivec2(gl_GlobalInvocationID.xy);
		if (uint(probe.x) >= params.grid_w || uint(probe.y) >= params.grid_h) {
			return;
		}
		spg_place_main(probe);
		return;
	}

	// This shader only runs PLACE; the temporal accumulate and the spatial
	// filter live in rtgi_spg_accumulate.glsl.
	return;
}
