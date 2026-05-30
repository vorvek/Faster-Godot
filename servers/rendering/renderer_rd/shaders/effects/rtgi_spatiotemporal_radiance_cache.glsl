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

const float STRC_MAX_DISTANCE = 65504.0;
const float STRC_MAX_AGE = 65504.0;
const float STRC_REJECT_NONE = 0.0;
const float STRC_REJECT_LOW_CONFIDENCE = 1.0;
const float STRC_REJECT_DYNAMIC = 2.0;
const float STRC_REJECT_VARIANCE = 3.0;
const float STRC_REJECT_PROBE_OCCUPIED = 4.0;
const float STRC_REJECT_STALE = 5.0;
const float STRC_REJECT_SCROLL = 6.0;
const float STRC_REJECT_BLACK_RADIANCE = 7.0;
const float STRC_REJECT_NO_SOURCE = 8.0;
const uint STRC_SOURCE_MASK_DIRECT = 1u;
const uint STRC_SOURCE_MASK_EMISSIVE = 2u;
const uint STRC_SOURCE_MASK_SKY = 4u;
const uint STRC_SOURCE_MASK_INDIRECT = 8u;

struct ProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
	vec4 metadata;
};

layout(set = 0, binding = 0, std430) readonly buffer ProbeRayResultBuffer {
	ProbeRayResult results[];
}
probe_results;

layout(set = 0, binding = 1, rgba16f) uniform image2D irradiance_image;
layout(set = 0, binding = 2, rgba16f) uniform image2D distance_image;
layout(set = 0, binding = 3, rgba16f) uniform image2D metadata_image;
layout(set = 0, binding = 4, rgba16f) uniform image2D radiance_debug_image;
layout(set = 0, binding = 5, r8) uniform image2D confidence_debug_image;
layout(set = 0, binding = 6, r8) uniform image2D updates_debug_image;
layout(set = 0, binding = 7, r8) uniform image2D visibility_debug_image;
layout(set = 0, binding = 8, r8) uniform image2D age_debug_image;
layout(set = 0, binding = 9, r8) uniform image2D variance_debug_image;
layout(set = 0, binding = 10, rgba16f) uniform image2D rejection_debug_image;
layout(set = 0, binding = 11, rgba16f) readonly uniform image2D previous_irradiance_image;
layout(set = 0, binding = 12, rgba16f) readonly uniform image2D previous_distance_image;
layout(set = 0, binding = 13, rgba16f) readonly uniform image2D previous_metadata_image;

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(65504.0), isinf(color));
	return clamp(color, vec3(0.0), vec3(65504.0));
}

float sanitize_cache_scalar(float value, float max_value) {
	value = isnan(value) ? 0.0 : value;
	value = isinf(value) ? max_value : value;
	return clamp(value, 0.0, max_value);
}

vec4 rejection_debug_color(float reason) {
	if (reason < 0.5) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	if (reason < 1.5) {
		return vec4(0.95, 0.10, 0.05, 1.0);
	}
	if (reason < 2.5) {
		return vec4(1.0, 0.75, 0.05, 1.0);
	}
	if (reason < 3.5) {
		return vec4(1.0, 0.35, 0.0, 1.0);
	}
	if (reason < 4.5) {
		return vec4(0.85, 0.10, 1.0, 1.0);
	}
	if (reason < 5.5) {
		return vec4(0.05, 0.30, 1.0, 1.0);
	}
	if (reason < 6.5) {
		return vec4(1.0, 0.0, 0.0, 1.0);
	}
	if (reason < 7.5) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	return vec4(0.35, 0.0, 0.85, 1.0);
}

float variance_ratio(vec4 distance_value) {
	float mean_distance = max(distance_value.x, 0.25);
	return clamp(sqrt(max(distance_value.y, 0.0)) / mean_distance, 0.0, 1.0);
}

float source_mask_quality(uint source_mask) {
	float quality = 0.0;
	if ((source_mask & STRC_SOURCE_MASK_DIRECT) != 0u) {
		quality = max(quality, 1.0);
	}
	if ((source_mask & STRC_SOURCE_MASK_EMISSIVE) != 0u) {
		quality = max(quality, 0.95);
	}
	if ((source_mask & STRC_SOURCE_MASK_SKY) != 0u) {
		quality = max(quality, 0.80);
	}
	if ((source_mask & STRC_SOURCE_MASK_INDIRECT) != 0u) {
		quality = max(quality, 0.60);
	}
	return quality;
}

void store_debug(ivec2 coord, vec4 irradiance, vec4 distance_value, vec4 metadata, float update_signal) {
	float confidence = clamp(irradiance.a, 0.0, 1.0);
	float dynamic_confidence = clamp(metadata.y, 0.0, 1.0);
	float variance = variance_ratio(distance_value);
	float age = clamp(metadata.x / 1024.0, 0.0, 1.0);
	float visibility = confidence * (1.0 - variance) * (1.0 - dynamic_confidence * 0.85);

	imageStore(radiance_debug_image, coord, vec4(sanitize_color(irradiance.rgb), 1.0));
	imageStore(confidence_debug_image, coord, vec4(confidence, 0.0, 0.0, 1.0));
	imageStore(updates_debug_image, coord, vec4(clamp(update_signal, 0.0, 1.0), 0.0, 0.0, 1.0));
	imageStore(visibility_debug_image, coord, vec4(clamp(visibility, 0.0, 1.0), 0.0, 0.0, 1.0));
	imageStore(age_debug_image, coord, vec4(age, 0.0, 0.0, 1.0));
	imageStore(variance_debug_image, coord, vec4(variance, 0.0, 0.0, 1.0));
	imageStore(rejection_debug_image, coord, rejection_debug_color(metadata.w));
}

void clear_cache_texel(ivec2 coord, float rejection_reason) {
	vec4 irradiance = vec4(0.0);
	vec4 distance_value = vec4(STRC_MAX_DISTANCE, 0.0, 0.0, 0.0);
	vec4 metadata = vec4(STRC_MAX_AGE, 0.0, 0.0, rejection_reason);
	imageStore(irradiance_image, coord, irradiance);
	imageStore(distance_image, coord, distance_value);
	imageStore(metadata_image, coord, metadata);
	store_debug(coord, irradiance, distance_value, metadata, 0.0);
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

uint strc_cascade_weight(uint cascade, uint cascade_count) {
	return 1u << (max(cascade_count, 1u) - cascade - 1u);
}

uint select_update_index(uint ray_index, uint ray_count, uint grid, uint cascade_count, uint frame_index) {
	uint active_cascades = clamp(cascade_count, 1u, 4u);
	uint texels_per_cascade = grid * grid * grid * 64u;
	if (active_cascades == 1u) {
		return texels_per_cascade > 0u ? (frame_index * max(ray_count, 1u) + ray_index) % texels_per_cascade : 0u;
	}

	uint slot_count = (1u << active_cascades) - 1u;
	uint slot = ray_index % slot_count;
	uint slot_start = 0u;
	uint cascade = 0u;
	uint selected_weight = strc_cascade_weight(0u, active_cascades);
	for (uint i = 0u; i < 4u; i++) {
		if (i >= active_cascades) {
			break;
		}
		uint weight = strc_cascade_weight(i, active_cascades);
		if (slot < slot_start + weight) {
			cascade = i;
			selected_weight = weight;
			break;
		}
		slot_start += weight;
	}

	uint slots_per_round = max((ray_count + slot_count - 1u) / slot_count, 1u);
	uint cascade_budget = max(slots_per_round * selected_weight, 1u);
	uint local_ray = (ray_index / slot_count) * selected_weight + (slot - slot_start);
	uint local_index = texels_per_cascade > 0u ? (frame_index * cascade_budget + local_ray) % texels_per_cascade : 0u;
	return cascade * texels_per_cascade + local_index;
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
		clear_cache_texel(coord, STRC_REJECT_SCROLL);
		return;
	}

	ivec3 old_probe = ivec3(int(probe_x), int(probe_y), int(probe_z)) + params.cascade_scroll[cascade].xyz;
	if (any(lessThan(old_probe, ivec3(0))) || any(greaterThanEqual(old_probe, ivec3(int(grid))))) {
		clear_cache_texel(coord, STRC_REJECT_SCROLL);
		return;
	}

	ivec2 old_coord = atlas_coord_from_components(cascade, uvec3(old_probe), uvec2(dir_x, dir_y));
	vec4 irradiance = imageLoad(previous_irradiance_image, old_coord);
	vec4 distance_value = imageLoad(previous_distance_image, old_coord);
	vec4 metadata = imageLoad(previous_metadata_image, old_coord);
	metadata.x = min(sanitize_cache_scalar(metadata.x, STRC_MAX_AGE) + 1.0, STRC_MAX_AGE);
	metadata.y = clamp(metadata.y * params.temporal_weight, 0.0, 1.0);
	if (metadata.x > 4096.0 && metadata.w < 0.5) {
		metadata.w = STRC_REJECT_STALE;
	}
	imageStore(irradiance_image, coord, irradiance);
	imageStore(distance_image, coord, distance_value);
	imageStore(metadata_image, coord, metadata);
	store_debug(coord, irradiance, distance_value, metadata, 0.0);
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
	ProbeRayResult result = probe_results.results[ray_index];
	uint update_index = texel_count > 0u ? min(uint(max(result.metadata.w, 0.0) + 0.5), texel_count - 1u) : 0u;
	uint probe_index = update_index >> 6u;
	uint dir_index = update_index & 63u;
	ivec2 coord = atlas_coord_from_probe_dir(probe_index, dir_index);

	vec3 radiance = sanitize_color(result.radiance_distance.rgb);
	float radiance_luma = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
	float hit_distance = sanitize_cache_scalar(result.radiance_distance.a, STRC_MAX_DISTANCE);
	float result_confidence = clamp(result.normal_confidence.a, 0.0, 1.0);
	float dynamic_hit = clamp(result.metadata.x, 0.0, 1.0);
	uint source_mask = uint(clamp(result.metadata.z, 0.0, 15.0) + 0.5);
	float source_quality = source_mask_quality(source_mask);
	bool radiance_valid = radiance_luma > 0.0005 && result_confidence > 0.001;
	bool source_valid = source_mask != 0u && source_quality > 0.0;
	bool probe_occupied = hit_distance < 0.12 && hit_distance < STRC_MAX_DISTANCE - 1.0;
	float occupied_probe = smoothstep(0.015, 0.12, hit_distance);
	float new_confidence = (radiance_valid && source_valid && !probe_occupied) ? result_confidence * mix(occupied_probe, 1.0, step(STRC_MAX_DISTANCE - 1.0, hit_distance)) : 0.0;

	vec4 previous = imageLoad(irradiance_image, coord);
	vec4 previous_distance = imageLoad(distance_image, coord);
	vec4 previous_metadata = imageLoad(metadata_image, coord);
	float prev_confidence = clamp(previous.a, 0.0, 1.0);
	float dynamic_decay = mix(1.0, 0.35, dynamic_hit);
	float temporal_weight = clamp(params.temporal_weight * dynamic_decay, 0.0, 0.995);
	float previous_mean = sanitize_cache_scalar(previous_distance.x, STRC_MAX_DISTANCE);
	float previous_variance = max(previous_distance.y, 0.0);
	vec3 blended_radiance = vec3(0.0);
	float blended_confidence = 0.0;
	float blended_distance = previous_mean;
	float blended_variance = previous_variance;
	float dynamic_confidence = 0.0;

	float rejection_reason = STRC_REJECT_NONE;
	if (new_confidence <= 0.001) {
		blended_radiance = previous.rgb;
		blended_confidence = clamp(prev_confidence * temporal_weight, 0.0, 1.0);
		dynamic_confidence = clamp(max(dynamic_hit, previous_metadata.y * temporal_weight), 0.0, 1.0);
		source_mask = uint(clamp(previous_metadata.z, 0.0, 15.0) + 0.5);
		if (!radiance_valid) {
			rejection_reason = STRC_REJECT_BLACK_RADIANCE;
		} else if (!source_valid) {
			rejection_reason = STRC_REJECT_NO_SOURCE;
		} else if (probe_occupied) {
			rejection_reason = STRC_REJECT_PROBE_OCCUPIED;
		} else {
			rejection_reason = STRC_REJECT_LOW_CONFIDENCE;
		}
	} else {
		float blend_prev = clamp(temporal_weight * prev_confidence, 0.0, temporal_weight);
		float blend_new = 1.0 - blend_prev;
		blended_radiance = mix(radiance, previous.rgb, blend_prev);
		blended_confidence = clamp(max(new_confidence, prev_confidence * temporal_weight), 0.0, 1.0);
		blended_distance = previous_mean * blend_prev + hit_distance * blend_new;
		blended_variance = blend_prev * (previous_variance + (previous_mean - blended_distance) * (previous_mean - blended_distance)) + blend_new * ((hit_distance - blended_distance) * (hit_distance - blended_distance));
		blended_variance = clamp(blended_variance, 0.0, STRC_MAX_DISTANCE);
		dynamic_confidence = clamp(max(dynamic_hit, previous_metadata.y * temporal_weight), 0.0, 1.0);
		if (dynamic_hit > 0.5) {
			rejection_reason = STRC_REJECT_DYNAMIC;
		} else if (variance_ratio(vec4(blended_distance, blended_variance, 0.0, 0.0)) > 0.45) {
			rejection_reason = STRC_REJECT_VARIANCE;
		}
	}

	vec4 distance_value = vec4(blended_distance, blended_variance, blended_confidence, dynamic_confidence);
	float next_age = new_confidence > 0.001 ? 0.0 : min(previous_metadata.x + 1.0, STRC_MAX_AGE);
	vec4 metadata = vec4(next_age, dynamic_confidence, float(source_mask), rejection_reason);

	imageStore(irradiance_image, coord, vec4(blended_radiance, blended_confidence));
	imageStore(distance_image, coord, distance_value);
	imageStore(metadata_image, coord, metadata);
	store_debug(coord, vec4(blended_radiance, blended_confidence), distance_value, metadata, max(new_confidence, dynamic_hit * 0.5));
}
