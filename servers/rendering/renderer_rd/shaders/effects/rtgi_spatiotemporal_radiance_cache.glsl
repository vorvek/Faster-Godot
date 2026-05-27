#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 64

layout(local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint ray_count;
	uint grid_size;
	uint cascade_count;
	uint frame_index;
	float temporal_weight;
	uint mode;
	uint scroll_valid;
	uint pad0;
	ivec4 cascade_scroll[4];
}
params;

struct ProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
};

layout(set = 0, binding = 0, std430) readonly buffer ProbeRayResultBuffer {
	ProbeRayResult results[];
}
probe_results;

layout(set = 0, binding = 1, rgba16f) uniform image2D irradiance_image;
layout(set = 0, binding = 2, rgba16f) uniform image2D distance_image;
layout(set = 0, binding = 3, rgba16f) uniform image2D radiance_debug_image;
layout(set = 0, binding = 4, r8) uniform image2D confidence_debug_image;
layout(set = 0, binding = 5, r8) uniform image2D updates_debug_image;
layout(set = 0, binding = 6, rgba16f) readonly uniform image2D previous_irradiance_image;
layout(set = 0, binding = 7, rgba16f) readonly uniform image2D previous_distance_image;

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(65504.0), isinf(color));
	return clamp(color, vec3(0.0), vec3(65504.0));
}

ivec2 atlas_coord_from_probe_dir(uint probe_index, uint dir_index) {
	uint grid = max(params.grid_size, 1u);
	uint probes_per_cascade = grid * grid * grid;
	uint cascade = min(probe_index / probes_per_cascade, max(params.cascade_count, 1u) - 1u);
	uint probe = probe_index - cascade * probes_per_cascade;
	uint px = probe % grid;
	uint py = (probe / grid) % grid;
	uint pz = probe / (grid * grid);
	uint dx = dir_index & 7u;
	uint dy = (dir_index >> 3u) & 7u;
	return ivec2(int(px * 8u + dx), int(((cascade * grid + pz) * grid + py) * 8u + dy));
}

ivec2 atlas_coord_from_components(uint cascade, uvec3 probe_coord, uvec2 dir_coord) {
	uint grid = max(params.grid_size, 1u);
	return ivec2(int(probe_coord.x * 8u + dir_coord.x), int(((cascade * grid + probe_coord.z) * grid + probe_coord.y) * 8u + dir_coord.y));
}

void scroll_cache_main() {
	uvec2 coord_u = gl_GlobalInvocationID.xy;
	uint grid = max(params.grid_size, 1u);
	uint atlas_width = grid * 8u;
	uint atlas_height = max(params.cascade_count, 1u) * grid * grid * 8u;
	if (coord_u.x >= atlas_width || coord_u.y >= atlas_height) {
		return;
	}

	uint probe_x = coord_u.x >> 3u;
	uint dir_x = coord_u.x & 7u;
	uint probe_row = coord_u.y >> 3u;
	uint dir_y = coord_u.y & 7u;
	uint cascade = min(probe_row / (grid * grid), max(params.cascade_count, 1u) - 1u);
	uint row_in_cascade = probe_row - cascade * grid * grid;
	uint probe_y = row_in_cascade % grid;
	uint probe_z = row_in_cascade / grid;
	ivec2 coord = ivec2(coord_u);

	if (params.scroll_valid == 0u) {
		imageStore(irradiance_image, coord, vec4(0.0));
		imageStore(distance_image, coord, vec4(65504.0, 0.0, 0.0, 0.0));
		imageStore(radiance_debug_image, coord, vec4(0.0));
		imageStore(confidence_debug_image, coord, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	ivec3 old_probe = ivec3(int(probe_x), int(probe_y), int(probe_z)) + params.cascade_scroll[cascade].xyz;
	if (any(lessThan(old_probe, ivec3(0))) || any(greaterThanEqual(old_probe, ivec3(int(grid))))) {
		imageStore(irradiance_image, coord, vec4(0.0));
		imageStore(distance_image, coord, vec4(65504.0, 0.0, 0.0, 0.0));
		imageStore(radiance_debug_image, coord, vec4(0.0));
		imageStore(confidence_debug_image, coord, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	ivec2 old_coord = atlas_coord_from_components(cascade, uvec3(old_probe), uvec2(dir_x, dir_y));
	vec4 irradiance = imageLoad(previous_irradiance_image, old_coord);
	vec4 distance = imageLoad(previous_distance_image, old_coord);
	imageStore(irradiance_image, coord, irradiance);
	imageStore(distance_image, coord, distance);
	imageStore(radiance_debug_image, coord, vec4(irradiance.rgb, 1.0));
	imageStore(confidence_debug_image, coord, vec4(clamp(irradiance.a, 0.0, 1.0), 0.0, 0.0, 1.0));
}

void main() {
	if (params.mode == 0u) {
		scroll_cache_main();
		return;
	}

	uint ray_index = gl_GlobalInvocationID.x;
	if (ray_index >= params.ray_count) {
		return;
	}

	uint grid = max(params.grid_size, 1u);
	uint probe_count = max(params.cascade_count, 1u) * grid * grid * grid;
	uint texel_count = probe_count * 64u;
	uint update_index = texel_count > 0u ? (params.frame_index * max(params.ray_count, 1u) + ray_index) % texel_count : 0u;
	uint probe_index = update_index >> 6u;
	uint dir_index = update_index & 63u;
	ivec2 coord = atlas_coord_from_probe_dir(probe_index, dir_index);

	ProbeRayResult result = probe_results.results[ray_index];
	vec3 radiance = sanitize_color(result.radiance_distance.rgb);
	float hit_distance = clamp(result.radiance_distance.a, 0.0, 65504.0);
	float new_confidence = clamp(result.normal_confidence.a, 0.0, 1.0);

	vec4 previous = imageLoad(irradiance_image, coord);
	vec4 previous_distance = imageLoad(distance_image, coord);
	float prev_confidence = clamp(previous.a, 0.0, 1.0);
	float blend_prev = new_confidence > 0.0 ? clamp(params.temporal_weight * prev_confidence, 0.0, params.temporal_weight) : 1.0;

	vec3 blended_radiance = mix(radiance, previous.rgb, blend_prev);
	float blended_confidence = clamp(max(new_confidence, prev_confidence * params.temporal_weight), 0.0, 1.0);
	float blended_distance = mix(hit_distance, previous_distance.x, blend_prev);
	float distance_variance = mix(0.0, previous_distance.y, blend_prev);

	imageStore(irradiance_image, coord, vec4(blended_radiance, blended_confidence));
	imageStore(distance_image, coord, vec4(blended_distance, distance_variance, new_confidence, 0.0));
	imageStore(radiance_debug_image, coord, vec4(blended_radiance, 1.0));
	imageStore(confidence_debug_image, coord, vec4(blended_confidence, 0.0, 0.0, 1.0));
	imageStore(updates_debug_image, coord, vec4(new_confidence, 0.0, 0.0, 1.0));
}
