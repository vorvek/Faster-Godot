#[compute]

#version 450

#VERSION_DEFINES

// World Radiance Cache update compute shader.
//
// Camera-centered cascaded clipmap of octahedral-radiance probes packed into a
// roughly-square atlas (tile layout mirrors RtgiWrc::atlas_coord in
// servers/rendering/renderer_rd/effects/rtgi_wrc_math.h). A single shader runs
// two modes selected by the `mode` push-constant:
//   mode 0 -- scroll/recenter the previous-frame cache into the write atlas.
//   mode 1 -- accumulate this frame's probe-ray radiance into the write atlas.
//
// Task 4 ships EMPTY kernels (structure only): the probe-ray update (Task 5) and
// the real accumulate/recenter logic (Task 6) fill in the bodies later. The
// binding-agnostic query API in ../raytracing/rtgi_wrc_inc.glsl is for the
// downstream consumers (Task 7); it is intentionally NOT included here.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint mode; // 0 = scroll/recenter, 1 = accumulate.
	uint cascade_count;
	uint grid;
	uint oct_res;
	uint atlas_width;
	uint atlas_height;
	uint rays_this_frame;
	uint frame_index;
	float base_spacing;
	float temporal_n_cap;
	float feedback_damping;
	float view_prioritization;
	vec3 camera_pos;
	uint pad0;
	ivec4 scroll_delta[4]; // .xyz = integer probe-space recenter delta per cascade.
}
params;

// Front (read) atlases: previous-frame cache.
layout(set = 0, binding = 0, rgba16f) uniform image2D radiance_read_image;
layout(set = 0, binding = 1, rg16f) uniform image2D distance_read_image;
layout(set = 0, binding = 2, rgba8) uniform image2D metadata_read_image;
// Back (write) atlases: this-frame cache.
layout(set = 0, binding = 3, rgba16f) uniform image2D radiance_write_image;
layout(set = 0, binding = 4, rg16f) uniform image2D distance_write_image;
layout(set = 0, binding = 5, rgba8) uniform image2D metadata_write_image;

// Mode 0: scroll/recenter the clipmap into the write atlas.
// EMPTY STUB (Task 6). For now: copy-through so the write atlas holds valid data
// after the swap and the cache does not flicker to black while the kernels are
// being built out.
void wrc_scroll_main(ivec2 coord) {
	imageStore(radiance_write_image, coord, imageLoad(radiance_read_image, coord));
	imageStore(distance_write_image, coord, imageLoad(distance_read_image, coord));
	imageStore(metadata_write_image, coord, imageLoad(metadata_read_image, coord));
}

// Mode 1: accumulate this frame's probe rays into the write atlas.
// EMPTY STUB (Task 5/6): no probe-ray input bound yet, so do nothing.
void wrc_accumulate_main(ivec2 coord) {
	return;
}

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	if (uint(coord.x) >= params.atlas_width || uint(coord.y) >= params.atlas_height) {
		return;
	}

	if (params.mode == 0u) {
		wrc_scroll_main(coord);
		return;
	}

	wrc_accumulate_main(coord);
}
