#[compute]

#version 450

#VERSION_DEFINES

// World Radiance Cache (WRC) GI debug consumer.
//
// First real per-pixel CONSUMER of the World Radiance Cache: for each screen
// pixel it reconstructs the WORLD-space surface position from the depth buffer,
// decodes the WORLD-space surface normal from the normal-roughness G-buffer, and
// samples the WRC cache's cosine-integrated irradiance via the shared query in
// ../raytracing/rtgi_wrc_inc.glsl. The RAW irradiance (the query's return value)
// is written straight to the output image -- NOT multiplied by albedo and NOT
// tonemapped -- so the Task-7b furnace oracle gate can compare the sphere-pixel
// irradiance directly against the known environment radiance L (linear units).
//
// This is the shader that compile-gates rtgi_wrc_inc.glsl for the first time.
//
// Coordinate-space contract (verified against environment/gi.glsl +
// scene_data_inc.glsl + storage_rd/render_scene_data_rd.cpp):
//   * The depth buffer (RB_TEX_DEPTH) holds RAW reverse-Z hyperbolic depth; the
//     far plane / cleared sky value is 0.0 (depth_pass clears to 0.0f). View
//     position is inv_projection * vec4(2*uv-1, depth, 1) (homogeneous divide),
//     exactly like gi.glsl::reconstruct_position's full-matrix branch -- the
//     inverse projection already encodes the reverse-Z mapping, so no z remap.
//   * The normal-roughness G-buffer normal is in VIEW space (the scene shader
//     writes (read_view_matrix * normal); gi.glsl rotates it to world via
//     cam_transform). We decode normalize(rgb*2-1) then rotate VIEW->WORLD.
//   * inv_view_matrix == the camera's cam_transform (view->world), so world pos
//     is inv_view_matrix * view_pos (full affine, incl. translation) and world
//     normal is mat3(inv_view_matrix) * view_normal. camera_pos (the clipmap
//     center the atlas was built around) == inv_view_matrix's translation.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// The WRC query API (cosine-integrated irradiance / cone radiance). Binding-
// agnostic: it takes the atlas samplers + a WrcParams block as plain arguments,
// so this shader owns its own descriptor-set layout. This include also pulls in
// the octahedral + clipmap math the query needs.
#include "../raytracing/rtgi_wrc_inc.glsl"

// Atlases owned by the RTGIWorldRadianceCache effect. RGBA16F radiance (.rgb =
// directional radiance, .a = per-texel confidence) and RG16F distance moments
// (mean, mean^2). Sampled with a linear sampler (the query does bilinear taps).
layout(set = 0, binding = 0) uniform sampler2D radiance_atlas;
layout(set = 0, binding = 1) uniform sampler2D distance_atlas;
// Raw scene depth (reverse-Z hyperbolic) and view-space normal-roughness.
layout(set = 0, binding = 2) uniform sampler2D depth_buffer;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_buffer;
// Linear, un-tonemapped irradiance output (RGBA16F). .rgb = irradiance, .a holds
// the WRC query confidence for optional inspection (the gate reads .rgb).
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D dest_image;

// Two mat4s (128 bytes) plus scalars exceed RenderingDevice's 128-byte
// push-constant cap (MAX_PUSH_CONSTANT_SIZE), so these params live in a UBO
// (uncapped) bound at the next free set-0 binding after the output image.
layout(set = 0, binding = 5, std140) uniform GiDebugParams {
	// WrcParams scalars (mirror RtgiWrc::ClipmapParams + tunables). Filled from
	// the cache's cached_params + the camera position so the query addresses the
	// SAME clipmap the atlas was built around.
	int cascade_count;
	int grid;
	int oct_res;
	float base_spacing;

	float occlusion_bias_spacing;
	float min_variance;
	int screen_width;
	int screen_height;

	// camera_pos (clipmap center) packed as 3 floats + pad to keep std140 happy
	// (vec3 is 16-byte aligned; the trailing scalar packs into its 4th slot).
	vec3 camera_pos;
	uint pad0;

	// Full inverse projection (clip -> view) and view -> world (camera transform).
	// std140: each mat4 is 16-byte aligned (the scalar block above totals 48 B,
	// a multiple of 16) and occupies 64 bytes.
	mat4 inv_projection;
	mat4 inv_view; // 4x4; only the upper-left 3x3 + translation column are used.
}
params;

// Reconstruct VIEW-space position from the raw depth buffer at integer pixel
// `pos`. Mirrors gi.glsl::reconstruct_position (full-projection branch): build a
// clip-space point from the pixel's NDC xy + raw depth z and run it through the
// inverse projection. Returns the homogeneous-divided view-space position.
vec3 reconstruct_view_position(ivec2 pos, float raw_depth) {
	vec4 clip;
	clip.xy = (2.0 * (vec2(pos) + vec2(0.5)) / vec2(params.screen_width, params.screen_height)) - 1.0;
	clip.z = raw_depth;
	clip.w = 1.0;
	vec4 view = params.inv_projection * clip;
	return view.xyz / view.w;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= params.screen_width || pos.y >= params.screen_height) {
		return;
	}

	// Raw reverse-Z depth. The cleared far/sky value is 0.0 -> no geometry, the
	// gate expects vec3(0) for sky pixels (and the WRC has nothing to contribute
	// at infinity anyway).
	float raw_depth = texelFetch(depth_buffer, pos, 0).r;
	if (raw_depth <= 0.0) {
		imageStore(dest_image, pos, vec4(0.0));
		return;
	}

	// View-space normal from the G-buffer (normalize(rgb*2-1)); a zero/degenerate
	// normal (cleared texel with no geometry) means "no surface" -> output 0.
	vec3 enc = texelFetch(normal_roughness_buffer, pos, 0).xyz;
	vec3 view_normal = enc * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.0001) {
		imageStore(dest_image, pos, vec4(0.0));
		return;
	}
	view_normal = normalize(view_normal);

	// VIEW -> WORLD. inv_view is the camera's view->world transform, so a position
	// uses the full affine (rotation + translation) and a normal uses the 3x3.
	vec3 view_pos = reconstruct_view_position(pos, raw_depth);
	vec3 world_pos = (params.inv_view * vec4(view_pos, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(params.inv_view) * view_normal);

	// Fill the binding-agnostic WrcParams from the cached clipmap params. These
	// MUST match the ClipmapParams the atlas tiles were sized/written from, or the
	// tile addressing in the query mis-locates the probe.
	WrcParams wp;
	wp.cascade_count = params.cascade_count;
	wp.grid = params.grid;
	wp.oct_res = params.oct_res;
	wp.base_spacing = params.base_spacing;
	wp.camera_pos = params.camera_pos;
	wp.occlusion_bias_spacing = params.occlusion_bias_spacing;
	wp.min_variance = params.min_variance;

	float confidence = 0.0;
	vec3 irradiance = rtgi_wrc_sample_irradiance(radiance_atlas, distance_atlas, wp, world_pos, world_normal, confidence);

	// RAW irradiance, linear, un-tonemapped. Albedo / beauty compositing is NOT
	// applied here (the harness gate handles albedo*L). .a carries confidence.
	imageStore(dest_image, pos, vec4(irradiance, confidence));
}
