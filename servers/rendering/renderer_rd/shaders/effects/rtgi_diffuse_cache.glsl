#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 8
#define MAX_RADIANCE 32768.0
#define CACHE_SLOT_COUNT 4
#define SPG_PROBE_SPACING 8
#define SPG_DIRECTION_RESOLUTION 4
#define SPG_REFINED_SUBDIVS 2
#define SPG_REFINED_DIRECTION_RESOLUTION 2
#define SPG_REFINED_CELL_SIZE (SPG_REFINED_SUBDIVS * SPG_REFINED_DIRECTION_RESOLUTION)
#define SURFACE_CACHE_ASSOCIATIVITY 4
#define SPG_REJECT_NONE 0.0
#define SPG_REJECT_LOW_CONFIDENCE 1.0
#define SPG_REJECT_STATS 2.0
#define SPG_REJECT_NORMAL 3.0
#define SPG_REJECT_DEPTH 4.0
#define SPG_REJECT_HISTORY 5.0
#define SPG_REJECT_SURFACE 6.0
#define SPG_REJECT_VISIBILITY 7.0
#define SPG_REJECT_HEMISPHERE 8.0
#define SPG_REJECT_RADIANCE 9.0
#define SPG_REJECT_LOW_QUALITY 10.0
#define SPG_REJECT_SCALE 10.0
#define SURFACE_CACHE_SOURCE_NONE 0.0
#define SURFACE_CACHE_SOURCE_RECEIVER 1.0
#define SURFACE_CACHE_SOURCE_BASE_SPG 2.0
#define SURFACE_CACHE_SOURCE_REFINED_SPG 3.0
#define SURFACE_CACHE_SOURCE_VISIBLE_CURRENT 4.0
#define SURFACE_CACHE_SOURCE_DIRECT 5.0
#define SURFACE_CACHE_SOURCE_EMISSIVE 6.0
#define SURFACE_CACHE_SOURCE_SKY 7.0
#define SURFACE_CACHE_SOURCE_STRC 8.0
#define SURFACE_CACHE_SOURCE_MIXED 9.0
#define SURFACE_CACHE_SOURCE_MAX 9.0

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	vec2 cache_resolution;
	float max_history;
	uint mode;
	uvec2 spg_probe_resolution;
	mat4 inv_view_projection;
	vec4 camera_origin_base_spacing;
	uvec4 strc_params;
}
params;

layout(set = 0, binding = 0, rgba16f) readonly uniform image2D source_image;
layout(set = 0, binding = 1) uniform sampler2D previous_radiance;
layout(set = 0, binding = 2) uniform sampler2D previous_meta;
layout(set = 0, binding = 3) uniform sampler2D previous_stats;
layout(set = 0, binding = 4) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 5) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 6) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 7) uniform sampler2D history_validity_buffer;
layout(set = 0, binding = 8) uniform sampler2D prev_history_validity_buffer;
layout(set = 0, binding = 9) uniform sampler2D history_id_buffer;
layout(set = 0, binding = 10) uniform sampler2D prev_history_id_buffer;
layout(set = 0, binding = 11) uniform sampler2D signal_confidence_buffer;
layout(set = 0, binding = 12, rgba16f) uniform image2D output_image;
layout(set = 0, binding = 13, rgba16f) uniform image2D next_radiance_image;
layout(set = 0, binding = 14, rgba16f) uniform image2D next_meta_image;
layout(set = 0, binding = 15, rgba16f) uniform image2D next_stats_image;
layout(set = 0, binding = 16, rgba8) uniform image2D diagnostic_image;
layout(set = 0, binding = 17, r8) uniform image2D age_image;
layout(set = 0, binding = 18, r8) uniform image2D rejection_image;
layout(set = 0, binding = 19) uniform sampler2D previous_cache_history_id;
layout(set = 0, binding = 20, rgba8) uniform image2D next_cache_history_id_image;
layout(set = 0, binding = 21) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 22, rgba8) uniform image2D cache_signal_confidence_image;
layout(set = 0, binding = 23) uniform sampler2D receiver_surface_id_buffer;
layout(set = 0, binding = 24) uniform sampler2D prev_receiver_surface_id_buffer;
layout(set = 0, binding = 25) uniform sampler2D depth_buffer;
layout(set = 0, binding = 26) uniform sampler2D strc_irradiance_buffer;
layout(set = 0, binding = 27) uniform sampler2D strc_distance_buffer;
layout(set = 0, binding = 28) uniform sampler2D strc_metadata_buffer;
layout(set = 0, binding = 29) uniform sampler2D primary_diffuse_direction_buffer;
layout(set = 0, binding = 30) uniform sampler2D previous_spg_radiance;
layout(set = 0, binding = 31) uniform sampler2D previous_spg_meta;
layout(set = 0, binding = 32) uniform sampler2D previous_spg_history_id;
layout(set = 0, binding = 33, rgba16f) uniform image2D next_spg_radiance_image;
layout(set = 0, binding = 34, rgba16f) uniform image2D next_spg_meta_image;
layout(set = 0, binding = 35, rgba8) uniform image2D next_spg_history_id_image;
layout(set = 0, binding = 36) uniform sampler2D previous_spg_stats;
layout(set = 0, binding = 37, rgba16f) uniform image2D next_spg_stats_image;
layout(set = 0, binding = 38) uniform sampler2D previous_spg_visibility;
layout(set = 0, binding = 39, rgba16f) uniform image2D next_spg_visibility_image;
layout(set = 0, binding = 40, rgba8) uniform image2D spg_rejection_image;
layout(set = 0, binding = 41) uniform sampler2D previous_spg_refinement_mask;
layout(set = 0, binding = 42, rgba8) uniform image2D next_spg_refinement_mask_image;
layout(set = 0, binding = 43) uniform sampler2D previous_spg_refined_radiance;
layout(set = 0, binding = 44) uniform sampler2D previous_spg_refined_meta;
layout(set = 0, binding = 45) uniform sampler2D previous_spg_refined_stats;
layout(set = 0, binding = 46) uniform sampler2D previous_spg_refined_visibility;
layout(set = 0, binding = 47) uniform sampler2D previous_spg_refined_history_id;
layout(set = 0, binding = 48, rgba16f) uniform image2D next_spg_refined_radiance_image;
layout(set = 0, binding = 49, rgba16f) uniform image2D next_spg_refined_meta_image;
layout(set = 0, binding = 50, rgba16f) uniform image2D next_spg_refined_stats_image;
layout(set = 0, binding = 51, rgba16f) uniform image2D next_spg_refined_visibility_image;
layout(set = 0, binding = 52, rgba8) uniform image2D next_spg_refined_history_id_image;
layout(set = 0, binding = 53) uniform sampler2D previous_surface_radiance;
layout(set = 0, binding = 54) uniform sampler2D previous_surface_meta;
layout(set = 0, binding = 55) uniform sampler2D previous_surface_stats;
layout(set = 0, binding = 56) uniform sampler2D previous_surface_history_id;
layout(set = 0, binding = 57, rgba16f) uniform image2D next_surface_radiance_image;
layout(set = 0, binding = 58, rgba8) uniform image2D next_surface_meta_image;
layout(set = 0, binding = 59, rgba16f) uniform image2D next_surface_stats_image;
layout(set = 0, binding = 60, rgba8) uniform image2D next_surface_history_id_image;
layout(set = 0, binding = 61, r32ui) readonly uniform uimage2D current_surface_key_image;
layout(set = 0, binding = 62, r32ui) readonly uniform uimage2D previous_cache_surface_key_image;
layout(set = 0, binding = 63, r32ui) uniform uimage2D next_cache_surface_key_image;
layout(set = 0, binding = 64, r32ui) uniform uimage2D next_surface_claim_image;
layout(set = 0, binding = 65, r32ui) readonly uniform uimage2D surface_feedback_key_image;
layout(set = 0, binding = 66) uniform sampler2D surface_feedback_radiance;
layout(set = 0, binding = 67) uniform sampler2D surface_feedback_meta;
layout(set = 0, binding = 68) uniform sampler2D surface_feedback_stats;
layout(set = 0, binding = 69, rgba8) uniform image2D surface_feedback_diagnostic_image;
layout(set = 0, binding = 70) uniform sampler2D secondary_cache_source_feedback;
layout(set = 0, binding = 71) uniform sampler2D secondary_cache_rejection_feedback;

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(MAX_RADIANCE), isinf(color));
	return clamp(color, vec3(0.0), vec3(MAX_RADIANCE));
}

float luminance(vec3 color) {
	return max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0);
}

float surface_cache_source_bucket(float source_class) {
	return clamp(source_class / SURFACE_CACHE_SOURCE_MAX, 0.0, 1.0);
}

float surface_cache_source_from_bucket(float bucket) {
	return clamp(floor(clamp(bucket, 0.0, 1.0) * SURFACE_CACHE_SOURCE_MAX + 0.5), SURFACE_CACHE_SOURCE_NONE, SURFACE_CACHE_SOURCE_MAX);
}

float surface_cache_source_quality(float source_class) {
	if (source_class == SURFACE_CACHE_SOURCE_RECEIVER) {
		return 0.82;
	}
	if (source_class == SURFACE_CACHE_SOURCE_BASE_SPG) {
		return 0.62;
	}
	if (source_class == SURFACE_CACHE_SOURCE_REFINED_SPG) {
		return 0.74;
	}
	if (source_class == SURFACE_CACHE_SOURCE_VISIBLE_CURRENT) {
		return 0.92;
	}
	if (source_class == SURFACE_CACHE_SOURCE_DIRECT) {
		return 0.88;
	}
	if (source_class == SURFACE_CACHE_SOURCE_EMISSIVE) {
		return 0.78;
	}
	if (source_class == SURFACE_CACHE_SOURCE_SKY) {
		return 0.70;
	}
	if (source_class == SURFACE_CACHE_SOURCE_STRC) {
		return 0.42;
	}
	if (source_class == SURFACE_CACHE_SOURCE_MIXED) {
		return 0.70;
	}
	return 0.35;
}

vec3 clamp_luminance(vec3 color, float max_luma) {
	float luma = luminance(color);
	return luma > max_luma ? color * (max_luma / max(luma, 1e-4)) : color;
}

vec3 decode_normal(vec4 normal_roughness) {
	return normalize(normal_roughness.xyz * 2.0 - 1.0);
}

vec3 safe_albedo(vec3 albedo) {
	return max(albedo, vec3(0.08));
}

float diffuse_demodulation_weight(vec4 albedo_metalness) {
	float metalness = clamp(albedo_metalness.a, 0.0, 1.0);
	float albedo_luma = luminance(albedo_metalness.rgb);
	return (1.0 - metalness) * smoothstep(0.015, 0.12, albedo_luma);
}

vec3 demodulate_diffuse_radiance(vec3 radiance, vec4 albedo_metalness) {
	float weight = diffuse_demodulation_weight(albedo_metalness);
	vec3 modulation = mix(vec3(1.0), safe_albedo(albedo_metalness.rgb), weight);
	return sanitize_color(radiance / modulation);
}

vec3 remodulate_diffuse_lighting(vec3 lighting, vec4 albedo_metalness) {
	float weight = diffuse_demodulation_weight(albedo_metalness);
	vec3 modulation = mix(vec3(1.0), safe_albedo(albedo_metalness.rgb), weight);
	return sanitize_color(lighting * modulation);
}

bool history_id_matches(vec4 a, vec4 b) {
	return max(max(abs(a.x - b.x), abs(a.y - b.y)), max(abs(a.z - b.z), abs(a.w - b.w))) < (0.5 / 255.0);
}

bool packed_id_valid(vec4 id) {
	return dot(id, id) > 1e-6;
}

uint unpack_packed_id(vec4 id) {
	uvec4 bytes = uvec4(round(clamp(id, 0.0, 1.0) * 255.0));
	return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}

vec4 pack_u32_rgba8(uint id) {
	uvec4 bytes = uvec4(id & 0xFFu, (id >> 8u) & 0xFFu, (id >> 16u) & 0xFFu, (id >> 24u) & 0xFFu);
	return vec4(bytes) * (1.0 / 255.0);
}

uint mix_u32(uint h, uint v) {
	h ^= v + 0x9e3779b9u + (h << 6u) + (h >> 2u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h;
}

ivec2 surface_cache_pos_from_key_variant(uint key, ivec2 surface_size, int variant) {
	uint h = mix_u32(0x73726363u ^ uint(variant) * 0x9e3779b9u, key);
	uint texel_count = uint(max(surface_size.x * surface_size.y, 1));
	uint index = h % texel_count;
	return ivec2(int(index % uint(surface_size.x)), int(index / uint(surface_size.x)));
}

ivec2 surface_cache_pos_from_key(uint key, ivec2 surface_size) {
	return surface_cache_pos_from_key_variant(key, surface_size, 0);
}

ivec2 surface_cache_pos_from_id(vec4 id, ivec2 surface_size) {
	return surface_cache_pos_from_key(unpack_packed_id(id), surface_size);
}

uint surface_cache_claim_from_quality(uint key, float quality) {
	uint quality_bucket = uint(clamp(quality, 0.0, 1.0) * 255.0);
	uint tie_breaker = mix_u32(0x73636c6du, key) & 0x00FFFFFFu;
	return (quality_bucket << 24u) | tie_breaker;
}

vec4 receiver_surface_id_at(ivec2 pos) {
	vec4 id = texelFetch(receiver_surface_id_buffer, pos, 0);
	return packed_id_valid(id) ? id : texelFetch(history_id_buffer, pos, 0);
}

vec4 prev_receiver_surface_id_at(ivec2 pos) {
	vec4 id = texelFetch(prev_receiver_surface_id_buffer, pos, 0);
	return packed_id_valid(id) ? id : texelFetch(prev_history_id_buffer, pos, 0);
}

float relative_delta(float a, float b, float floor_value) {
	return abs(a - b) / max(max(a, b), floor_value);
}

float velocity_pixels_at(ivec2 pos) {
	return length(texelFetch(velocity_buffer, pos, 0).xy * params.resolution);
}

float diffuse_cache_motion_reuse(float motion_pixels) {
	return mix(1.0, 0.45, smoothstep(0.75, 8.0, motion_pixels));
}

float signal_clamp_risk(vec4 confidence_signal) {
	return clamp(confidence_signal.r * 0.35 + confidence_signal.g * 0.55, 0.0, 1.0);
}

vec2 diffuse_cache_vec3_to_oct(vec3 v) {
	v /= max(abs(v.x) + abs(v.y) + abs(v.z), 1e-6);
	vec2 oct = v.xy;
	if (v.z < 0.0) {
		oct = (1.0 - abs(oct.yx)) * sign(oct.xy);
	}
	return oct;
}

uint diffuse_cache_strc_direction_index(vec3 direction) {
	vec2 oct = diffuse_cache_vec3_to_oct(normalize(direction)) * 0.5 + 0.5;
	uvec2 texel = uvec2(clamp(floor(oct * 8.0), vec2(0.0), vec2(7.0)));
	return texel.x + texel.y * 8u;
}

uint diffuse_cache_strc_probe_index(uint cascade, uvec3 probe_coord, uint grid_size) {
	uint grid = max(grid_size, 1u);
	return cascade * grid * grid * grid + probe_coord.x + probe_coord.y * grid + probe_coord.z * grid * grid;
}

ivec2 diffuse_cache_strc_atlas_coord(uint probe_index, uint dir_index, uint grid_size, uint cascade_count) {
	uint grid = max(grid_size, 1u);
	uint probes_per_cascade = grid * grid * grid;
	uint cascade = min(probe_index / probes_per_cascade, max(cascade_count, 1u) - 1u);
	uint probe = probe_index - cascade * probes_per_cascade;
	uint px = probe % grid;
	uint py = (probe / grid) % grid;
	uint pz = probe / (grid * grid);
	uint dx = dir_index & 7u;
	uint dy = (dir_index >> 3u) & 7u;
	return ivec2(int(px * 8u + dx), int(((cascade * grid + pz) * grid + py) * 8u + dy));
}

vec3 diffuse_cache_strc_probe_world_position(vec3 cascade_center, float spacing, uvec3 probe_coord, uint grid_size) {
	vec3 probe_local = (vec3(probe_coord) + vec3(0.5)) - vec3(float(grid_size) * 0.5);
	return cascade_center + probe_local * spacing;
}

float diffuse_cache_strc_distance_visibility(vec4 moments, float receiver_distance, float spacing) {
	float mean_distance = clamp(moments.x, 0.0, 65504.0);
	float variance = max(moments.y, 0.0);
	if (mean_distance >= 65503.0) {
		return 1.0;
	}
	float bias = max(spacing * 0.10, 0.04);
	if (receiver_distance <= mean_distance + bias) {
		return 1.0;
	}
	float delta = receiver_distance - mean_distance;
	float chebyshev = variance / max(variance + delta * delta, 1e-4);
	return clamp(chebyshev * chebyshev, 0.0, 1.0);
}

float diffuse_cache_strc_source_quality(uint source_mask) {
	float quality = 0.0;
	if ((source_mask & 1u) != 0u) {
		quality = max(quality, 1.0);
	}
	if ((source_mask & 2u) != 0u) {
		quality = max(quality, 0.95);
	}
	if ((source_mask & 4u) != 0u) {
		quality = max(quality, 0.80);
	}
	if ((source_mask & 8u) != 0u) {
		quality = max(quality, 0.60);
	}
	return quality;
}

bool diffuse_cache_world_pos_from_depth(ivec2 pos, out vec3 world_pos) {
	world_pos = vec3(0.0);
	float depth = texelFetch(depth_buffer, pos, 0).r;
	if (depth <= 0.000001) {
		return false;
	}
	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	vec4 world_h = params.inv_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	if (any(isnan(world_h)) || any(isinf(world_h)) || abs(world_h.w) <= 1e-6) {
		return false;
	}
	world_pos = world_h.xyz / world_h.w;
	return !(any(isnan(world_pos)) || any(isinf(world_pos)));
}

bool diffuse_cache_sample_strc(vec3 world_pos, vec3 normal, out vec3 lighting, out float confidence) {
	lighting = vec3(0.0);
	confidence = 0.0;
	if (params.strc_params.z == 0u) {
		return false;
	}

	uint grid = clamp(params.strc_params.x, 12u, 32u);
	uint cascade_count = clamp(params.strc_params.y, 1u, 4u);
	float base_spacing = max(params.camera_origin_base_spacing.w, 0.25);
	vec3 camera_origin = params.camera_origin_base_spacing.xyz;
	vec3 normal_n = normalize(normal);
	uint normal_dir_index = diffuse_cache_strc_direction_index(normal_n);
	vec3 irradiance_sum = vec3(0.0);
	float weight_sum = 0.0;

	for (uint cascade = 0u; cascade < 4u; cascade++) {
		if (cascade >= cascade_count) {
			break;
		}
		float spacing = base_spacing * exp2(float(cascade));
		vec3 cascade_center = floor(camera_origin / spacing) * spacing;
		vec3 probe_space = (world_pos - cascade_center) / spacing + vec3(float(grid) * 0.5) - vec3(0.5);
		ivec3 base_probe = ivec3(floor(probe_space));
		if (all(greaterThanEqual(base_probe, ivec3(0))) && all(lessThan(base_probe, ivec3(int(grid) - 1)))) {
			vec3 frac_probe = clamp(fract(probe_space), vec3(0.0), vec3(1.0));
			for (uint corner = 0u; corner < 8u; corner++) {
				uvec3 corner_bits = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
				ivec3 probe_i = base_probe + ivec3(corner_bits);
				uvec3 probe_coord = uvec3(probe_i);
				vec3 corner_weight_v = mix(vec3(1.0) - frac_probe, frac_probe, vec3(corner_bits));
				float trilinear_weight = corner_weight_v.x * corner_weight_v.y * corner_weight_v.z;
				if (trilinear_weight <= 0.0) {
					continue;
				}

				vec3 probe_world = diffuse_cache_strc_probe_world_position(cascade_center, spacing, probe_coord, grid);
				vec3 probe_to_point = world_pos - probe_world;
				float receiver_distance = length(probe_to_point);
				vec3 probe_to_point_dir = receiver_distance > 1e-4 ? probe_to_point / receiver_distance : normal_n;
				vec3 point_to_probe_dir = -probe_to_point_dir;
				float normal_weight = mix(0.08, 1.0, pow(max(dot(normal_n, point_to_probe_dir), 0.0), 2.0));
				uint probe_index = diffuse_cache_strc_probe_index(cascade, probe_coord, grid);

				uint visibility_dir_index = diffuse_cache_strc_direction_index(probe_to_point_dir);
				vec4 distance_sample = texelFetch(strc_distance_buffer, diffuse_cache_strc_atlas_coord(probe_index, visibility_dir_index, grid, cascade_count), 0);
				float visibility = diffuse_cache_strc_distance_visibility(distance_sample, receiver_distance, spacing);
				if (visibility <= 0.001) {
					continue;
				}

				ivec2 irradiance_coord = diffuse_cache_strc_atlas_coord(probe_index, normal_dir_index, grid, cascade_count);
				vec4 irradiance_sample = texelFetch(strc_irradiance_buffer, irradiance_coord, 0);
				vec4 metadata_sample = texelFetch(strc_metadata_buffer, irradiance_coord, 0);
				float variance = clamp(sqrt(max(distance_sample.y, 0.0)) / max(distance_sample.x, 0.25), 0.0, 1.0);
				float variance_confidence = 1.0 - smoothstep(0.20, 0.80, variance);
				float dynamic_confidence = 1.0 - clamp(metadata_sample.y, 0.0, 1.0) * 0.85;
				float age_confidence = 1.0 - smoothstep(128.0, 768.0, clamp(metadata_sample.x, 0.0, 65504.0));
				uint source_mask = uint(clamp(metadata_sample.z, 0.0, 15.0) + 0.5);
				float source_quality = diffuse_cache_strc_source_quality(source_mask);
				float sample_weight = trilinear_weight * normal_weight * visibility * variance_confidence * dynamic_confidence * age_confidence * source_quality * clamp(irradiance_sample.a, 0.0, 1.0);
				if (sample_weight <= 0.0) {
					continue;
				}

				irradiance_sum += sanitize_color(irradiance_sample.rgb) * sample_weight;
				weight_sum += sample_weight;
			}
			break;
		}
	}

	if (weight_sum <= 1e-5) {
		return false;
	}

	lighting = sanitize_color(irradiance_sum / weight_sum);
	confidence = clamp(weight_sum, 0.0, 1.0);
	return luminance(lighting) > 0.0005 && confidence > 0.015;
}

float diffuse_cache_current_confidence(float current_valid, float guide_valid, float signal_risk) {
	float risk_confidence = mix(0.22, 1.0, sqrt(clamp(1.0 - signal_risk, 0.0, 1.0)));
	return current_valid * guide_valid * risk_confidence;
}

ivec2 output_size_i() {
	return ivec2(params.resolution);
}

ivec2 cache_size_i() {
	return ivec2(params.cache_resolution);
}

ivec2 cache_slot_pos(ivec2 cache_pos, int slot) {
	return ivec2(cache_pos.x * CACHE_SLOT_COUNT + clamp(slot, 0, CACHE_SLOT_COUNT - 1), cache_pos.y);
}

vec2 cache_slot_offset(int slot) {
	if (slot == 0) {
		return vec2(0.25, 0.25);
	}
	if (slot == 1) {
		return vec2(0.75, 0.25);
	}
	if (slot == 2) {
		return vec2(0.25, 0.75);
	}
	return vec2(0.75, 0.75);
}

ivec2 output_pos_to_cache_pos(ivec2 pos) {
	ivec2 cache_size = cache_size_i();
	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	return clamp(ivec2(floor(uv * params.cache_resolution)), ivec2(0), cache_size - ivec2(1));
}

ivec2 cache_pos_to_output_center(ivec2 cache_pos, int slot) {
	ivec2 output_size = output_size_i();
	vec2 uv = (vec2(cache_pos) + cache_slot_offset(slot)) / params.cache_resolution;
	return clamp(ivec2(floor(uv * params.resolution)), ivec2(0), output_size - ivec2(1));
}

ivec2 spg_probe_size_i() {
	return ivec2(max(params.spg_probe_resolution, uvec2(1u)));
}

ivec2 spg_atlas_size_i() {
	return spg_probe_size_i() * SPG_DIRECTION_RESOLUTION;
}

ivec2 spg_atlas_pos(ivec2 probe_pos, ivec2 dir_tile) {
	return probe_pos * SPG_DIRECTION_RESOLUTION + clamp(dir_tile, ivec2(0), ivec2(SPG_DIRECTION_RESOLUTION - 1));
}

ivec2 spg_refined_atlas_size_i() {
	return spg_probe_size_i() * SPG_REFINED_CELL_SIZE;
}

ivec2 spg_refined_atlas_pos(ivec2 probe_pos, ivec2 sub_tile, ivec2 dir_tile) {
	ivec2 local = clamp(sub_tile, ivec2(0), ivec2(SPG_REFINED_SUBDIVS - 1)) * SPG_REFINED_DIRECTION_RESOLUTION + clamp(dir_tile, ivec2(0), ivec2(SPG_REFINED_DIRECTION_RESOLUTION - 1));
	return probe_pos * SPG_REFINED_CELL_SIZE + local;
}

vec3 spg_oct_to_vec3(vec2 oct) {
	vec2 f = oct * 2.0 - 1.0;
	vec3 v = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = clamp(-v.z, 0.0, 1.0);
	v.xy += vec2(v.x >= 0.0 ? -t : t, v.y >= 0.0 ? -t : t);
	return normalize(v);
}

vec3 spg_direction_from_tile(ivec2 dir_tile) {
	vec2 oct = (vec2(clamp(dir_tile, ivec2(0), ivec2(SPG_DIRECTION_RESOLUTION - 1))) + vec2(0.5)) / float(SPG_DIRECTION_RESOLUTION);
	return spg_oct_to_vec3(oct);
}

vec3 spg_refined_direction_from_tile(ivec2 dir_tile) {
	vec2 oct = (vec2(clamp(dir_tile, ivec2(0), ivec2(SPG_REFINED_DIRECTION_RESOLUTION - 1))) + vec2(0.5)) / float(SPG_REFINED_DIRECTION_RESOLUTION);
	return spg_oct_to_vec3(oct);
}

ivec2 spg_direction_tile(vec3 direction) {
	vec2 oct = diffuse_cache_vec3_to_oct(normalize(direction)) * 0.5 + 0.5;
	return ivec2(clamp(floor(oct * float(SPG_DIRECTION_RESOLUTION)), vec2(0.0), vec2(float(SPG_DIRECTION_RESOLUTION - 1))));
}

ivec2 spg_refined_direction_tile(vec3 direction) {
	vec2 oct = diffuse_cache_vec3_to_oct(normalize(direction)) * 0.5 + 0.5;
	return ivec2(clamp(floor(oct * float(SPG_REFINED_DIRECTION_RESOLUTION)), vec2(0.0), vec2(float(SPG_REFINED_DIRECTION_RESOLUTION - 1))));
}

float spg_direction_support(vec3 sample_dir, vec3 bin_dir) {
	float alignment = max(dot(normalize(sample_dir), normalize(bin_dir)), 0.0);
	return smoothstep(0.28, 0.94, alignment);
}

float spg_surface_compatibility(vec4 anchor_id, vec3 anchor_normal, float anchor_viewz, float anchor_roughness, vec4 sample_id, vec3 sample_normal, float sample_viewz, float sample_roughness) {
	bool exact_surface = packed_id_valid(anchor_id) && packed_id_valid(sample_id) && history_id_matches(anchor_id, sample_id);
	float roughness = max(anchor_roughness, sample_roughness);
	float normal_threshold = exact_surface ? mix(0.82, 0.20, roughness) : mix(0.95, 0.72, roughness);
	float normal_dot = dot(anchor_normal, sample_normal);
	if (normal_dot < normal_threshold) {
		return 0.0;
	}

	float depth_threshold = exact_surface ? mix(0.08, 0.24, roughness) : mix(0.035, 0.12, roughness);
	float depth_delta = relative_delta(anchor_viewz, sample_viewz, 0.25);
	if (depth_delta > depth_threshold) {
		return 0.0;
	}

	float rough_delta = abs(anchor_roughness - sample_roughness);
	if (!exact_surface && rough_delta > 0.38) {
		return 0.0;
	}

	float normal_weight = smoothstep(normal_threshold, 0.995, normal_dot);
	float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
	float rough_weight = exact_surface ? 1.0 : 1.0 - smoothstep(0.18, 0.38, rough_delta);
	return normal_weight * depth_weight * rough_weight * (exact_surface ? 1.0 : 0.24);
}

float spg_visibility_hit_weight(vec4 visibility_sample, float current_hitdist, float roughness) {
	float visibility_confidence = clamp(visibility_sample.y * visibility_sample.w, 0.0, 1.0);
	if (visibility_confidence <= 0.02 || current_hitdist >= 60000.0 || visibility_sample.x >= 60000.0) {
		return 1.0;
	}

	float hit_delta = relative_delta(current_hitdist, visibility_sample.x, 0.25);
	float hit_threshold = mix(0.28, 1.60, clamp(roughness, 0.0, 1.0));
	float hit_weight = 1.0 - smoothstep(hit_threshold * 0.35, hit_threshold, hit_delta);
	return mix(1.0, mix(0.35, 1.0, hit_weight), visibility_confidence);
}

void spg_record_rejection(float reason, float strength, inout float best_reason, inout float best_strength) {
	if (strength > best_strength) {
		best_reason = reason;
		best_strength = strength;
	}
}

vec3 clamp_current_diffuse_outlier(ivec2 pos, vec3 current, vec4 current_normal_roughness, vec4 current_viewz_hitdist, vec4 confidence_signal, out float clamp_activity) {
	clamp_activity = 0.0;
	current = sanitize_color(current);
	float current_luma = luminance(current);
	if (current_luma <= 0.03 || current_viewz_hitdist.x >= 60000.0) {
		return current;
	}

	float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
	float current_risk = signal_clamp_risk(confidence_signal);
	vec3 current_normal = decode_normal(current_normal_roughness);
	vec4 current_cache_id = receiver_surface_id_at(pos);
	const ivec2 offsets[8] = ivec2[](
			ivec2(-1, -1),
			ivec2(0, -1),
			ivec2(1, -1),
			ivec2(-1, 0),
			ivec2(1, 0),
			ivec2(-1, 1),
			ivec2(0, 1),
			ivec2(1, 1));

	float weight_sum = 0.0;
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	float max_support_luma = 0.0;
	float support_count = 0.0;

	for (int i = 0; i < 8; i++) {
		ivec2 candidate_pos = pos + offsets[i];
		if (candidate_pos.x < 0 || candidate_pos.y < 0 || candidate_pos.x >= int(params.resolution.x) || candidate_pos.y >= int(params.resolution.y)) {
			continue;
		}
		if (texelFetch(history_validity_buffer, candidate_pos, 0).r < 0.5) {
			continue;
		}
		if (!history_id_matches(current_cache_id, receiver_surface_id_at(candidate_pos))) {
			continue;
		}

		vec4 candidate_normal_roughness = texelFetch(normal_roughness_buffer, candidate_pos, 0);
		vec4 candidate_viewz_hitdist = texelFetch(viewz_hitdist_buffer, candidate_pos, 0);
		if (candidate_viewz_hitdist.x >= 60000.0) {
			continue;
		}

		float normal_dot = dot(current_normal, decode_normal(candidate_normal_roughness));
		float normal_threshold = mix(0.82, 0.10, roughness);
		if (normal_dot < normal_threshold) {
			continue;
		}

		float depth_delta = relative_delta(current_viewz_hitdist.x, candidate_viewz_hitdist.x, 0.25);
		float hit_delta = relative_delta(current_viewz_hitdist.y, candidate_viewz_hitdist.y, 0.25);
		float depth_threshold = mix(0.055, 0.22, roughness);
		float hit_threshold = mix(0.16, 1.00, roughness);
		if (depth_delta > depth_threshold || hit_delta > hit_threshold) {
			continue;
		}

		float normal_weight = smoothstep(normal_threshold, 0.995, normal_dot);
		float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
		float hit_weight = 1.0 - smoothstep(hit_threshold * 0.35, hit_threshold, hit_delta);
		float candidate_risk = signal_clamp_risk(texelFetch(signal_confidence_buffer, candidate_pos, 0));
		float signal_weight = mix(1.0, 0.35, candidate_risk);
		float weight = normal_weight * depth_weight * hit_weight * signal_weight;
		if (weight <= 0.02) {
			continue;
		}

		vec4 candidate_albedo = texelFetch(albedo_metalness_buffer, candidate_pos, 0);
		float candidate_luma = luminance(demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, candidate_pos).rgb), candidate_albedo));
		weight_sum += weight;
		luma_sum += candidate_luma * weight;
		luma_sq_sum += candidate_luma * candidate_luma * weight;
		max_support_luma = max(max_support_luma, candidate_luma);
		support_count += weight > 0.12 ? 1.0 : 0.0;
	}

	if (weight_sum < 0.90 || support_count < 2.0) {
		return current;
	}

	float local_mean = luma_sum / max(weight_sum, 1e-5);
	float local_variance = max(luma_sq_sum / max(weight_sum, 1e-5) - local_mean * local_mean, 0.0);
	float local_sigma = sqrt(local_variance);
	float isolated_delta = relative_delta(current_luma, local_mean, 0.08);
	float unsupported_spike = smoothstep(0.22, 0.75, isolated_delta) * (1.0 - smoothstep(0.80, 1.30, max_support_luma / max(current_luma, 1e-4)));
	float risk_tightening = smoothstep(0.05, 0.65, current_risk);
	float clamp_strength = max(risk_tightening, unsupported_spike * smoothstep(1.10, 2.50, current_luma / max(local_mean + local_sigma + 0.05, 0.05)));
	if (clamp_strength <= 0.01) {
		return current;
	}

	float slack = mix(0.08, 0.16, roughness);
	float statistical_limit = local_mean + local_sigma * mix(2.75, 1.25, clamp_strength) + slack;
	float support_limit = max_support_luma * mix(1.55, 1.12, clamp_strength) + slack;
	float max_current_luma = max(max(statistical_limit, support_limit), 0.035);
	if (current_luma <= max_current_luma) {
		return current;
	}

	float clamp_blend = smoothstep(max_current_luma * 1.02, max_current_luma * 1.75 + 0.05, current_luma) * max(clamp_strength, current_risk);
	if (clamp_blend <= 0.03) {
		return current;
	}

	clamp_activity = mix(0.35, 0.70, risk_tightening);
	return clamp_luminance(current, mix(current_luma, max_current_luma, clamp(clamp_blend, 0.0, 1.0)));
}

float variance_ratio_from_stats(vec4 stats_sample) {
	float mean_value = max(stats_sample.y, 0.0);
	float second_value = max(stats_sample.z, 0.0);
	float variance_value = max(second_value - mean_value * mean_value, 0.0);
	return sqrt(variance_value) / max(mean_value, 0.08);
}

vec3 integrate_current_probe_cell(ivec2 pos, vec3 center, vec4 center_normal_roughness, vec4 center_viewz_hitdist, vec4 center_cache_id, out float support, out float coherence) {
	vec2 cell_size = max(params.resolution / max(params.cache_resolution, vec2(1.0)), vec2(1.0));
	int radius = int(clamp(ceil(max(cell_size.x, cell_size.y) * 0.65), 1.0, 3.0));
	vec3 center_normal = decode_normal(center_normal_roughness);
	float roughness = clamp(center_normal_roughness.a, 0.0, 1.0);
	float normal_threshold = mix(0.76, 0.06, roughness);
	float depth_threshold = mix(0.07, 0.26, roughness);
	float hit_threshold = mix(0.18, 1.15, roughness);

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float spatial_sum = 0.0;
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	for (int y = -3; y <= 3; y++) {
		for (int x = -3; x <= 3; x++) {
			if (abs(x) > radius || abs(y) > radius) {
				continue;
			}
			ivec2 tap_pos = pos + ivec2(x, y);
			if (tap_pos.x < 0 || tap_pos.y < 0 || tap_pos.x >= int(params.resolution.x) || tap_pos.y >= int(params.resolution.y)) {
				continue;
			}
			vec2 delta = vec2(x, y);
			float spatial_weight = exp2(-dot(delta, delta) / max(float(radius * radius) * 0.75, 1.0));
			spatial_sum += spatial_weight;

			if (texelFetch(history_validity_buffer, tap_pos, 0).r < 0.5) {
				continue;
			}
			vec4 tap_viewz_hitdist = texelFetch(viewz_hitdist_buffer, tap_pos, 0);
			if (tap_viewz_hitdist.x >= 60000.0) {
				continue;
			}
			vec4 tap_normal_roughness = texelFetch(normal_roughness_buffer, tap_pos, 0);
			float normal_dot = dot(center_normal, decode_normal(tap_normal_roughness));
			if (normal_dot < normal_threshold) {
				continue;
			}
			float depth_delta = relative_delta(center_viewz_hitdist.x, tap_viewz_hitdist.x, 0.25);
			float hit_delta = relative_delta(center_viewz_hitdist.y, tap_viewz_hitdist.y, 0.25);
			if (depth_delta > depth_threshold || hit_delta > hit_threshold) {
				continue;
			}

			float normal_weight = smoothstep(normal_threshold, 0.995, normal_dot);
			float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
			float hit_weight = 1.0 - smoothstep(hit_threshold * 0.35, hit_threshold, hit_delta);
			float history_weight = history_id_matches(center_cache_id, receiver_surface_id_at(tap_pos)) ? 1.0 : 0.54;
			float signal_weight = mix(1.0, 0.45, signal_clamp_risk(texelFetch(signal_confidence_buffer, tap_pos, 0)));
			float weight = spatial_weight * normal_weight * depth_weight * hit_weight * history_weight * signal_weight;
			if (weight <= 0.02) {
				continue;
			}

			vec4 tap_albedo_metalness = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			vec3 tap = demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, tap_pos).rgb), tap_albedo_metalness);
			float tap_luma = luminance(tap);
			sum += tap * weight;
			luma_sum += tap_luma * weight;
			luma_sq_sum += tap_luma * tap_luma * weight;
			weight_sum += weight;
		}
	}

	if (weight_sum <= 1e-5) {
		support = 0.0;
		coherence = 0.0;
		return center;
	}

	vec3 averaged = sanitize_color(sum / weight_sum);
	float mean_luma = luma_sum / weight_sum;
	float variance = max(luma_sq_sum / weight_sum - mean_luma * mean_luma, 0.0);
	float sigma_ratio = sqrt(variance) / max(mean_luma, 0.08);
	support = clamp(weight_sum / max(spatial_sum, 1e-5), 0.0, 1.0);
	coherence = 1.0 - smoothstep(0.20, 0.95, sigma_ratio);
	return averaged;
}

bool load_previous_cache_sample(ivec2 prev_cache_pos, int preferred_slot, vec4 current_cache_id, out vec4 previous_radiance_sample, out vec4 previous_meta_sample, out vec4 previous_stats_sample) {
	previous_radiance_sample = vec4(0.0);
	previous_meta_sample = vec4(0.5, 0.5, 1.0, 65504.0);
	previous_stats_sample = vec4(65504.0, 0.0, 0.0, 0.0);
	ivec2 cache_size = cache_size_i();
	if (prev_cache_pos.x < 0 || prev_cache_pos.y < 0 || prev_cache_pos.x >= cache_size.x || prev_cache_pos.y >= cache_size.y) {
		return false;
	}

	float best_score = 0.0;
	for (int slot = 0; slot < CACHE_SLOT_COUNT; slot++) {
		ivec2 sample_pos = cache_slot_pos(prev_cache_pos, slot);
		vec4 sample_id = texelFetch(previous_cache_history_id, sample_pos, 0);
		if (!history_id_matches(current_cache_id, sample_id)) {
			continue;
		}

		vec4 radiance_sample = texelFetch(previous_radiance, sample_pos, 0);
		vec4 stats_sample = texelFetch(previous_stats, sample_pos, 0);
		float confidence = radiance_sample.a;
		float age = stats_sample.w;
		if (confidence <= 0.04 || age < 1.0) {
			continue;
		}

		float slot_bonus = slot == preferred_slot ? 0.25 : 0.0;
		float score = confidence + smoothstep(1.0, 18.0, age) * 0.35 + slot_bonus;
		if (score > best_score) {
			best_score = score;
			previous_radiance_sample = radiance_sample;
			previous_meta_sample = texelFetch(previous_meta, sample_pos, 0);
			previous_stats_sample = stats_sample;
		}
	}

	return best_score > 0.0;
}

void update_cache_slot(ivec2 cache_pos, int slot) {
	ivec2 storage_pos = cache_slot_pos(cache_pos, slot);
	ivec2 pos = cache_pos_to_output_center(cache_pos, slot);
	vec4 current_albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec3 current_radiance = sanitize_color(imageLoad(source_image, pos).rgb);
	vec3 current = demodulate_diffuse_radiance(current_radiance, current_albedo_metalness);
	float raw_current_luma = luminance(current);
	vec4 current_normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 current_viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
	vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
	float clamp_activity = 0.0;
	current = clamp_current_diffuse_outlier(pos, current, current_normal_roughness, current_viewz_hitdist, confidence_signal, clamp_activity);
	float current_luma = luminance(current);
	float clamp_luma_delta = max(raw_current_luma - current_luma, 0.0);
	float current_valid = texelFetch(history_validity_buffer, pos, 0).r >= 0.5 ? 1.0 : 0.0;
	float guide_valid = current_viewz_hitdist.x < 60000.0 ? 1.0 : 0.0;
	float clamp_risk = signal_clamp_risk(confidence_signal);
	float current_confidence = diffuse_cache_current_confidence(current_valid, guide_valid, clamp_risk);
	float current_roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
	vec4 current_history_id = texelFetch(history_id_buffer, pos, 0);
	vec4 current_cache_id = receiver_surface_id_at(pos);
	uint current_surface_key = imageLoad(current_surface_key_image, pos).r;
	if (current_confidence > 0.05) {
		float cell_support = 0.0;
		float cell_coherence = 0.0;
		vec3 cell_integrated = integrate_current_probe_cell(pos, current, current_normal_roughness, current_viewz_hitdist, current_cache_id, cell_support, cell_coherence);
		float cell_blend = smoothstep(0.18, 0.62, cell_support) * cell_coherence * mix(0.62, 1.0, current_roughness) * (1.0 - clamp_risk * 0.35);
		cell_blend = min(cell_blend, 0.38);
		current = sanitize_color(mix(current, cell_integrated, cell_blend));
		current_luma = luminance(current);
		current_confidence = clamp(current_confidence + cell_blend * 0.10, 0.0, 1.0);
	}

	vec3 filtered = current;
	float out_age = 1.0;
	float out_confidence = current_confidence * 0.5;
	float rejection = 0.0;

	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	vec2 prev_uv = uv + texelFetch(velocity_buffer, pos, 0).xy;
	ivec2 prev_pos = ivec2(floor(prev_uv * params.resolution));
	ivec2 prev_cache_pos = clamp(ivec2(floor(prev_uv * params.cache_resolution)), ivec2(0), cache_size_i() - ivec2(1));

	bool reusable = true;
	if (current_confidence <= 0.05) {
		reusable = false;
		rejection = 1.0;
	}
	if (reusable && (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThanEqual(prev_uv, vec2(1.0))) ||
						  prev_pos.x < 0 || prev_pos.y < 0 || prev_pos.x >= int(params.resolution.x) || prev_pos.y >= int(params.resolution.y))) {
		reusable = false;
		rejection = 2.0;
	}
	if (reusable && texelFetch(prev_history_validity_buffer, prev_pos, 0).r < 0.5) {
		reusable = false;
		rejection = 3.0;
	}
	if (reusable &&
			!history_id_matches(current_history_id, texelFetch(prev_history_id_buffer, prev_pos, 0)) &&
			!history_id_matches(current_cache_id, prev_receiver_surface_id_at(prev_pos))) {
		reusable = false;
		rejection = 4.0;
	}

	vec4 previous_radiance_sample = vec4(0.0);
	vec4 previous_meta_sample = vec4(0.5, 0.5, 1.0, 65504.0);
	vec4 previous_stats_sample = vec4(65504.0, 0.0, 0.0, 0.0);
	if (reusable && !load_previous_cache_sample(prev_cache_pos, slot, current_cache_id, previous_radiance_sample, previous_meta_sample, previous_stats_sample)) {
		reusable = false;
		rejection = 9.0;
	}
	float previous_age = previous_stats_sample.w;
	float previous_confidence = previous_radiance_sample.a;
	vec3 previous = sanitize_color(previous_radiance_sample.rgb);
	float previous_luma = luminance(previous);
	float previous_mean = max(previous_stats_sample.y, 0.0);
	float previous_second = max(previous_stats_sample.z, 0.0);
	float previous_variance = max(previous_second - previous_mean * previous_mean, 0.0);
	float previous_sigma = sqrt(previous_variance);

	if (reusable) {
		vec3 current_normal = decode_normal(current_normal_roughness);
		vec3 previous_normal = decode_normal(previous_meta_sample);
		float normal_dot = dot(current_normal, previous_normal);
		float roughness = current_normal_roughness.a;
		float normal_threshold = mix(0.80, 0.05, clamp(roughness, 0.0, 1.0));
		if (normal_dot < normal_threshold) {
			reusable = false;
			rejection = 5.0;
		}
	}
	if (reusable) {
		float depth_delta = relative_delta(current_viewz_hitdist.x, previous_meta_sample.w, 0.25);
		float hit_delta = relative_delta(current_viewz_hitdist.y, previous_stats_sample.x, 0.25);
		float roughness = current_normal_roughness.a;
		float depth_threshold = mix(0.08, 0.28, clamp(roughness, 0.0, 1.0));
		float hit_threshold = mix(0.20, 1.25, clamp(roughness, 0.0, 1.0));
		if (depth_delta > depth_threshold || hit_delta > hit_threshold) {
			reusable = false;
			rejection = 6.0;
		}
	}
	if (reusable) {
		float radiance_delta = relative_delta(current_luma, previous_luma, 0.08);
		if (radiance_delta > 2.25) {
			reusable = false;
			rejection = 7.0;
		}
	}
	if (reusable) {
		if (previous_age < 1.0 || previous_confidence <= 0.04 || variance_ratio_from_stats(previous_stats_sample) > 1.65) {
			reusable = false;
			rejection = 8.0;
		}
	}

	if (reusable) {
		float radiance_delta = relative_delta(current_luma, previous_luma, 0.08);
		float roughness = current_normal_roughness.a;
		float normal_threshold = mix(0.80, 0.05, clamp(roughness, 0.0, 1.0));
		float normal_weight = smoothstep(normal_threshold, 0.98, dot(decode_normal(current_normal_roughness), decode_normal(previous_meta_sample)));
		float delta_weight = 1.0 - smoothstep(0.75, 2.25, radiance_delta);
		float variance_ratio = previous_sigma / max(previous_mean, 0.08);
		float variance_weight = 1.0 - smoothstep(0.35, 1.65, variance_ratio);
		float age_weight = smoothstep(1.0, 18.0, previous_age);
		float confidence = clamp(min(current_confidence, previous_confidence) * normal_weight * delta_weight * mix(0.60, 1.0, variance_weight), 0.0, 1.0);
		float history_weight = min(0.90, mix(0.42, 0.86, age_weight) * confidence);
		history_weight *= diffuse_cache_motion_reuse(velocity_pixels_at(pos));
		float previous_brighter = max(previous_luma - current_luma, 0.0) / max(max(previous_luma, current_luma), 0.08);
		float current_brighter = max(current_luma - previous_luma, 0.0) / max(max(previous_luma, current_luma), 0.08);
		float brighten_guard = 1.0 - smoothstep(0.10, 0.58, previous_brighter);
		float spike_reuse_boost = mix(1.0, 1.12, smoothstep(0.12, 0.75, current_brighter) * variance_weight * age_weight);
		history_weight = min(0.90, history_weight * brighten_guard * spike_reuse_boost);
		float max_previous_luma = max(current_luma * 2.35 + 0.05, previous_mean + previous_sigma * 2.00 + 0.04);
		vec3 clamped_previous = clamp_luminance(previous, max_previous_luma);
		float previous_clamp_delta = max(previous_luma - luminance(clamped_previous), 0.0);
		clamp_luma_delta = max(clamp_luma_delta, previous_clamp_delta);
		filtered = sanitize_color(mix(current, clamped_previous, history_weight));
		out_age = min(previous_age + 1.0, params.max_history);
		out_confidence = clamp(mix(current_confidence, previous_confidence, history_weight) + 0.05, 0.0, 1.0);
	}

	float filtered_luma = luminance(filtered);
	float moment_weight = reusable ? min(0.94, 0.48 + out_age * 0.035) * out_confidence : 0.0;
	float filtered_second = filtered_luma * filtered_luma + clamp_luma_delta * clamp_luma_delta;
	float next_mean = mix(filtered_luma, previous_mean, moment_weight);
	float next_second = max(mix(filtered_second, previous_second, moment_weight), next_mean * next_mean + clamp_luma_delta * clamp_luma_delta * mix(0.35, 0.08, moment_weight));

	imageStore(next_radiance_image, storage_pos, vec4(filtered, out_confidence));
	imageStore(next_meta_image, storage_pos, vec4(current_normal_roughness.xyz, current_viewz_hitdist.x));
	imageStore(next_stats_image, storage_pos, vec4(current_viewz_hitdist.y, next_mean, next_second, out_age));
	imageStore(next_cache_history_id_image, storage_pos, current_cache_id);
	imageStore(next_cache_surface_key_image, storage_pos, uvec4((current_confidence > 0.05 && current_surface_key != 0u) ? current_surface_key : 0u, 0u, 0u, 0u));
}

void update_cache_entry(ivec2 cache_pos) {
	for (int slot = 0; slot < CACHE_SLOT_COUNT; slot++) {
		update_cache_slot(cache_pos, slot);
	}
}

void update_directional_screen_probe(ivec2 atlas_pos) {
	ivec2 atlas_size = spg_atlas_size_i();
	if (atlas_pos.x < 0 || atlas_pos.y < 0 || atlas_pos.x >= atlas_size.x || atlas_pos.y >= atlas_size.y) {
		return;
	}

	ivec2 probe_pos = atlas_pos / SPG_DIRECTION_RESOLUTION;
	ivec2 dir_tile = atlas_pos - probe_pos * SPG_DIRECTION_RESOLUTION;
	ivec2 probe_base = probe_pos * SPG_PROBE_SPACING;
	vec3 bin_dir = spg_direction_from_tile(dir_tile);

	vec3 radiance_sum = vec3(0.0);
	vec3 normal_sum = vec3(0.0);
	float viewz_sum = 0.0;
	float viewz_sq_sum = 0.0;
	float hitdist_sum = 0.0;
	float hitdist_sq_sum = 0.0;
	float hitdist_weight_sum = 0.0;
	float weight_sum = 0.0;
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	float best_weight = 0.0;
	vec4 best_id = vec4(0.0);
	vec4 anchor_id = vec4(0.0);
	vec3 anchor_normal = vec3(0.0, 0.0, 1.0);
	float anchor_viewz = 65504.0;
	float anchor_roughness = 1.0;
	float anchor_score = 0.0;

	for (int y = 0; y < SPG_PROBE_SPACING; y++) {
		for (int x = 0; x < SPG_PROBE_SPACING; x++) {
			ivec2 pos = probe_base + ivec2(x, y);
			if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
				continue;
			}
			if (texelFetch(history_validity_buffer, pos, 0).r < 0.5) {
				continue;
			}

			vec4 direction_sample = texelFetch(primary_diffuse_direction_buffer, pos, 0);
			if (direction_sample.a <= 0.02 || dot(direction_sample.xyz, direction_sample.xyz) <= 0.01) {
				continue;
			}

			vec3 sample_dir = normalize(direction_sample.xyz);
			float direction_weight = spg_direction_support(sample_dir, bin_dir);
			if (direction_weight <= 0.01) {
				continue;
			}

			vec4 viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
			if (viewz_hitdist.x >= 60000.0) {
				continue;
			}

			vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
			vec3 normal = decode_normal(normal_roughness);
			float hemisphere_weight = smoothstep(-0.05, 0.35, dot(normal, sample_dir));
			if (hemisphere_weight <= 0.01) {
				continue;
			}

			vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
			float signal_weight = mix(1.0, 0.42, signal_clamp_risk(confidence_signal));
			float roughness = clamp(normal_roughness.a, 0.0, 1.0);
			float rough_weight = smoothstep(0.48, 0.92, roughness);
			float sample_score = direction_weight * hemisphere_weight * direction_sample.a * signal_weight * rough_weight;
			if (sample_score > anchor_score) {
				anchor_score = sample_score;
				anchor_id = receiver_surface_id_at(pos);
				anchor_normal = normal;
				anchor_viewz = viewz_hitdist.x;
				anchor_roughness = roughness;
			}
		}
	}

	for (int y = 0; y < SPG_PROBE_SPACING; y++) {
		for (int x = 0; x < SPG_PROBE_SPACING; x++) {
			if (anchor_score <= 0.005) {
				continue;
			}
			ivec2 pos = probe_base + ivec2(x, y);
			if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
				continue;
			}
			if (texelFetch(history_validity_buffer, pos, 0).r < 0.5) {
				continue;
			}

			vec4 direction_sample = texelFetch(primary_diffuse_direction_buffer, pos, 0);
			if (direction_sample.a <= 0.02 || dot(direction_sample.xyz, direction_sample.xyz) <= 0.01) {
				continue;
			}

			vec3 sample_dir = normalize(direction_sample.xyz);
			float direction_weight = spg_direction_support(sample_dir, bin_dir);
			if (direction_weight <= 0.01) {
				continue;
			}

			vec4 viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
			if (viewz_hitdist.x >= 60000.0) {
				continue;
			}

			vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
			vec3 normal = decode_normal(normal_roughness);
			float hemisphere_weight = smoothstep(-0.05, 0.35, dot(normal, sample_dir));
			if (hemisphere_weight <= 0.01) {
				continue;
			}

			vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
			float signal_weight = mix(1.0, 0.42, signal_clamp_risk(confidence_signal));
			float roughness = clamp(normal_roughness.a, 0.0, 1.0);
			float rough_weight = smoothstep(0.48, 0.92, roughness);
			float sample_weight = direction_weight * hemisphere_weight * direction_sample.a * signal_weight * rough_weight;
			if (sample_weight <= 0.005) {
				continue;
			}

			vec4 sample_id = receiver_surface_id_at(pos);
			float surface_weight = spg_surface_compatibility(anchor_id, anchor_normal, anchor_viewz, anchor_roughness, sample_id, normal, viewz_hitdist.x, roughness);
			if (surface_weight <= 0.0) {
				continue;
			}
			sample_weight *= surface_weight;
			if (sample_weight <= 0.005) {
				continue;
			}

			vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
			vec3 radiance = demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, pos).rgb), albedo_metalness);
			float luma = luminance(radiance);
			radiance_sum += radiance * sample_weight;
			normal_sum += normal * sample_weight;
			viewz_sum += viewz_hitdist.x * sample_weight;
			viewz_sq_sum += viewz_hitdist.x * viewz_hitdist.x * sample_weight;
			if (viewz_hitdist.y > 0.0 && viewz_hitdist.y < 60000.0) {
				hitdist_sum += viewz_hitdist.y * sample_weight;
				hitdist_sq_sum += viewz_hitdist.y * viewz_hitdist.y * sample_weight;
				hitdist_weight_sum += sample_weight;
			}
			luma_sum += luma * sample_weight;
			luma_sq_sum += luma * luma * sample_weight;
			weight_sum += sample_weight;
			if (sample_weight > best_weight) {
				best_weight = sample_weight;
				best_id = sample_id;
			}
		}
	}

	vec4 previous_radiance_sample = texelFetch(previous_spg_radiance, atlas_pos, 0);
	vec4 previous_meta_sample = texelFetch(previous_spg_meta, atlas_pos, 0);
	vec4 previous_id_sample = texelFetch(previous_spg_history_id, atlas_pos, 0);
	vec4 previous_stats_sample = texelFetch(previous_spg_stats, atlas_pos, 0);
	vec4 previous_visibility_sample = texelFetch(previous_spg_visibility, atlas_pos, 0);
	float previous_confidence = clamp(previous_radiance_sample.a, 0.0, 1.0);

	if (weight_sum <= 0.01) {
		float fade = 0.62;
		if (previous_confidence <= 0.025) {
			imageStore(next_spg_radiance_image, atlas_pos, vec4(0.0));
			imageStore(next_spg_meta_image, atlas_pos, vec4(0.5, 0.5, 1.0, 65504.0));
			imageStore(next_spg_history_id_image, atlas_pos, vec4(0.0));
			imageStore(next_spg_stats_image, atlas_pos, vec4(0.0));
			imageStore(next_spg_visibility_image, atlas_pos, vec4(65504.0, 0.0, 65504.0, 0.0));
			return;
		}
		imageStore(next_spg_radiance_image, atlas_pos, vec4(sanitize_color(previous_radiance_sample.rgb), previous_confidence * fade));
		imageStore(next_spg_meta_image, atlas_pos, previous_meta_sample);
		imageStore(next_spg_history_id_image, atlas_pos, previous_id_sample);
		imageStore(next_spg_stats_image, atlas_pos, vec4(max(previous_stats_sample.x - 1.0, 0.0), previous_stats_sample.yzw * fade));
		imageStore(next_spg_visibility_image, atlas_pos, vec4(previous_visibility_sample.xyz, previous_visibility_sample.w * fade));
		return;
	}

	vec3 current = sanitize_color(radiance_sum / max(weight_sum, 1e-5));
	vec3 current_normal = normalize(normal_sum / max(weight_sum, 1e-5));
	float current_viewz = viewz_sum / max(weight_sum, 1e-5);
	float mean_luma = luma_sum / max(weight_sum, 1e-5);
	float variance = max(luma_sq_sum / max(weight_sum, 1e-5) - mean_luma * mean_luma, 0.0);
	float viewz_variance = max(viewz_sq_sum / max(weight_sum, 1e-5) - current_viewz * current_viewz, 0.0);
	float viewz_sigma_ratio = sqrt(viewz_variance) / max(current_viewz, 0.25);
	float current_hitdist = hitdist_weight_sum > 0.01 ? hitdist_sum / max(hitdist_weight_sum, 1e-5) : 65504.0;
	float hitdist_variance = hitdist_weight_sum > 0.01 ? max(hitdist_sq_sum / max(hitdist_weight_sum, 1e-5) - current_hitdist * current_hitdist, 0.0) : 0.0;
	float hitdist_sigma_ratio = current_hitdist < 60000.0 ? sqrt(hitdist_variance) / max(current_hitdist, 0.25) : 1.0;
	float hitdist_quality = current_hitdist < 60000.0 ? 1.0 - smoothstep(0.08, 0.75, hitdist_sigma_ratio) : 0.0;
	float visibility_support = clamp(hitdist_weight_sum / max(weight_sum, 1e-5), 0.0, 1.0);
	float coherence = 1.0 - smoothstep(0.28, 1.05, sqrt(variance) / max(mean_luma, 0.08));
	float support = clamp(weight_sum * 0.22, 0.0, 1.0);
	float plane_quality = 1.0 - smoothstep(0.015, 0.140, viewz_sigma_ratio);
	float current_confidence = clamp(support * mix(0.45, 1.0, coherence) * mix(0.35, 1.0, plane_quality), 0.0, 1.0);

	vec3 filtered = current;
	float out_confidence = current_confidence;
	float out_age = 1.0;
	float out_hitdist = current_hitdist;
	float out_hitdist_quality = hitdist_quality;
	float out_visibility_support = visibility_support;
	bool reusable = previous_confidence > 0.04 && packed_id_valid(previous_id_sample);
	if (reusable) {
		float normal_dot = dot(current_normal, decode_normal(previous_meta_sample));
		float depth_delta = relative_delta(current_viewz, previous_meta_sample.w, 0.25);
		float variance_ratio = sqrt(variance) / max(mean_luma, 0.08);
		float id_weight = history_id_matches(best_id, previous_id_sample) ? 1.0 : 0.35;
		float normal_weight = smoothstep(0.62, 0.98, normal_dot);
		float depth_weight = 1.0 - smoothstep(0.06, 0.26, depth_delta);
		float variance_weight = 1.0 - smoothstep(0.28, 1.05, variance_ratio);
		float previous_quality = clamp(previous_stats_sample.y * previous_stats_sample.z * previous_stats_sample.w, 0.0, 1.0);
		float history_age_weight = smoothstep(1.0, 12.0, previous_stats_sample.x);
		float history_weight = clamp(previous_confidence * normal_weight * depth_weight * variance_weight * id_weight * previous_quality * mix(0.45, 1.0, history_age_weight) * 0.58, 0.0, 0.62);
		float max_previous_luma = max(mean_luma * 2.25 + 0.05, 0.08);
		filtered = sanitize_color(mix(current, clamp_luminance(previous_radiance_sample.rgb, max_previous_luma), history_weight));
		out_confidence = clamp(mix(current_confidence, previous_confidence, history_weight) + 0.025, 0.0, 1.0);
		out_age = min(previous_stats_sample.x + 1.0, params.max_history);
		if (current_hitdist < 60000.0 && previous_visibility_sample.x < 60000.0 && previous_visibility_sample.w > 0.02) {
			out_hitdist = mix(current_hitdist, previous_visibility_sample.x, history_weight);
			out_hitdist_quality = mix(hitdist_quality, previous_visibility_sample.y, history_weight);
			out_visibility_support = mix(visibility_support, previous_visibility_sample.w, history_weight);
		}
	}

	imageStore(next_spg_radiance_image, atlas_pos, vec4(filtered, out_confidence));
	imageStore(next_spg_meta_image, atlas_pos, vec4(current_normal * 0.5 + 0.5, current_viewz));
	imageStore(next_spg_history_id_image, atlas_pos, best_id);
	imageStore(next_spg_stats_image, atlas_pos, vec4(out_age, support, coherence, plane_quality));
	imageStore(next_spg_visibility_image, atlas_pos, vec4(out_hitdist, out_hitdist_quality, current_viewz, out_visibility_support));
}

void update_spg_refinement_mask(ivec2 probe_pos) {
	ivec2 probe_size = spg_probe_size_i();
	if (probe_pos.x < 0 || probe_pos.y < 0 || probe_pos.x >= probe_size.x || probe_pos.y >= probe_size.y) {
		return;
	}

	vec4 previous_mask = texelFetch(previous_spg_refinement_mask, probe_pos, 0);
	float confidence_sum = 0.0;
	float support_sum = 0.0;
	float plane_risk_sum = 0.0;
	float coherence_risk_sum = 0.0;
	float visibility_risk_sum = 0.0;
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	float dir_weight_sum = 0.0;
	for (int dy = 0; dy < SPG_DIRECTION_RESOLUTION; dy++) {
		for (int dx = 0; dx < SPG_DIRECTION_RESOLUTION; dx++) {
			ivec2 atlas_pos = spg_atlas_pos(probe_pos, ivec2(dx, dy));
			vec4 radiance_sample = texelFetch(previous_spg_radiance, atlas_pos, 0);
			vec4 stats_sample = texelFetch(previous_spg_stats, atlas_pos, 0);
			vec4 visibility_sample = texelFetch(previous_spg_visibility, atlas_pos, 0);
			float confidence = clamp(radiance_sample.a, 0.0, 1.0);
			float support = clamp(stats_sample.y, 0.0, 1.0);
			float age_weight = smoothstep(1.0, 8.0, stats_sample.x);
			float weight = confidence * mix(0.35, 1.0, support) * mix(0.50, 1.0, age_weight);
			float luma = luminance(sanitize_color(radiance_sample.rgb));
			confidence_sum += confidence;
			support_sum += support;
			plane_risk_sum += (1.0 - clamp(stats_sample.w, 0.0, 1.0)) * weight;
			coherence_risk_sum += (1.0 - clamp(stats_sample.z, 0.0, 1.0)) * weight;
			visibility_risk_sum += (1.0 - clamp(visibility_sample.y * visibility_sample.w, 0.0, 1.0)) * weight;
			luma_sum += luma * weight;
			luma_sq_sum += luma * luma * weight;
			dir_weight_sum += weight;
		}
	}

	float direction_count = float(SPG_DIRECTION_RESOLUTION * SPG_DIRECTION_RESOLUTION);
	float avg_confidence = confidence_sum / max(direction_count, 1.0);
	float avg_support = support_sum / max(direction_count, 1.0);
	float inv_dir_weight = 1.0 / max(dir_weight_sum, 1e-5);
	float mean_luma = luma_sum * inv_dir_weight;
	float luma_variance = max(luma_sq_sum * inv_dir_weight - mean_luma * mean_luma, 0.0);
	float luma_risk = smoothstep(0.18, 0.95, sqrt(luma_variance) / max(mean_luma, 0.08));
	float plane_risk = plane_risk_sum * inv_dir_weight;
	float coherence_risk = coherence_risk_sum * inv_dir_weight;
	float visibility_risk = visibility_risk_sum * inv_dir_weight;

	ivec2 probe_base = probe_pos * SPG_PROBE_SPACING;
	vec3 anchor_normal = vec3(0.0, 0.0, 1.0);
	float anchor_viewz = 65504.0;
	float anchor_weight = 0.0;
	float normal_spread = 0.0;
	float depth_spread = 0.0;
	float screen_trace_success_sum = 0.0;
	float screen_trace_rejection_sum = 0.0;
	float screen_trace_spg_sum = 0.0;
	float screen_trace_surface_sum = 0.0;
	float screen_trace_strc_sum = 0.0;
	for (int y = 0; y < SPG_PROBE_SPACING; y++) {
		for (int x = 0; x < SPG_PROBE_SPACING; x++) {
			ivec2 pos = probe_base + ivec2(x, y);
			if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
				continue;
			}

			vec4 secondary_source = texelFetch(secondary_cache_source_feedback, pos, 0);
			if (secondary_source.z >= 0.5 && secondary_source.x > 0.001) {
				float source_weight = smoothstep(0.010, 0.22, clamp(secondary_source.y, 0.0, 1.0));
				uint source_class = uint(clamp(floor(clamp(secondary_source.x, 0.0, 1.0) * 5.0 + 0.5), 0.0, 5.0));
				screen_trace_success_sum += source_weight;
				screen_trace_strc_sum += source_class == 2u ? source_weight : 0.0;
				screen_trace_spg_sum += (source_class == 3u || source_class == 4u) ? source_weight : 0.0;
				screen_trace_surface_sum += source_class == 5u ? source_weight : 0.0;
			}

			vec4 secondary_rejection = texelFetch(secondary_cache_rejection_feedback, pos, 0);
			if (secondary_rejection.y >= 0.5 && secondary_rejection.x > 0.001) {
				uint rejection_reason = uint(clamp(floor(clamp(secondary_rejection.x, 0.0, 1.0) * 9.0 + 0.5), 0.0, 9.0));
				float rejection_weight = max(clamp(secondary_rejection.z, 0.0, 1.0), 0.08);
				float rejection_priority = 0.35;
				if (rejection_reason == 7u) {
					rejection_priority = 1.0;
				} else if (rejection_reason == 8u) {
					rejection_priority = 0.85;
				} else if (rejection_reason == 4u || rejection_reason == 5u) {
					rejection_priority = 0.70;
				} else if (rejection_reason == 9u) {
					rejection_priority = 0.62;
				}
				screen_trace_rejection_sum += rejection_weight * rejection_priority;
			}

			vec4 viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
			if (viewz_hitdist.x >= 60000.0 || texelFetch(history_validity_buffer, pos, 0).r < 0.5) {
				continue;
			}
			vec3 normal = decode_normal(texelFetch(normal_roughness_buffer, pos, 0));
			float sample_weight = 1.0;
			if (anchor_weight <= 0.0) {
				anchor_normal = normal;
				anchor_viewz = viewz_hitdist.x;
			}
			normal_spread = max(normal_spread, 1.0 - max(dot(anchor_normal, normal), 0.0));
			depth_spread = max(depth_spread, relative_delta(anchor_viewz, viewz_hitdist.x, 0.25));
			anchor_weight += sample_weight;
		}
	}

	float geometry_risk = max(smoothstep(0.08, 0.42, normal_spread), smoothstep(0.025, 0.18, depth_spread));
	geometry_risk = max(geometry_risk, plane_risk);
	float radiance_visibility_risk = max(max(luma_risk, coherence_risk), visibility_risk);
	float probe_pixel_count = float(SPG_PROBE_SPACING * SPG_PROBE_SPACING);
	float screen_trace_success_request = smoothstep(0.035, 0.28, screen_trace_success_sum / max(probe_pixel_count, 1.0));
	float screen_trace_rejection_request = smoothstep(0.020, 0.18, screen_trace_rejection_sum / max(probe_pixel_count, 1.0));
	float screen_trace_spg_request = smoothstep(0.020, 0.16, screen_trace_spg_sum / max(probe_pixel_count, 1.0));
	float screen_trace_surface_request = smoothstep(0.012, 0.10, screen_trace_surface_sum / max(probe_pixel_count, 1.0));
	float screen_trace_strc_request = smoothstep(0.025, 0.18, screen_trace_strc_sum / max(probe_pixel_count, 1.0));
	float explicit_feedback_request = max(max(screen_trace_rejection_request, screen_trace_success_request * 0.52), max(screen_trace_spg_request * 0.78, max(screen_trace_surface_request * 0.62, screen_trace_strc_request * 0.48)));
	radiance_visibility_risk = max(radiance_visibility_risk, max(screen_trace_rejection_request, max(screen_trace_spg_request, screen_trace_surface_request) * 0.72));
	float undersampling_risk = max(1.0 - smoothstep(0.08, 0.40, avg_support), 1.0 - smoothstep(0.05, 0.25, avg_confidence));
	float request = max(max(geometry_risk, radiance_visibility_risk), undersampling_risk * 0.55);
	request *= smoothstep(0.08, 0.22, max(avg_confidence, avg_support));
	request = max(request, explicit_feedback_request);
	request = max(request, previous_mask.r * 0.74);
	float history = max(request, previous_mask.a * 0.88);
	imageStore(next_spg_refinement_mask_image, probe_pos, clamp(vec4(request, geometry_risk, radiance_visibility_risk, history), vec4(0.0), vec4(1.0)));
}

void update_refined_directional_screen_probe(ivec2 atlas_pos) {
	ivec2 atlas_size = spg_refined_atlas_size_i();
	if (atlas_pos.x < 0 || atlas_pos.y < 0 || atlas_pos.x >= atlas_size.x || atlas_pos.y >= atlas_size.y) {
		return;
	}

	ivec2 probe_pos = atlas_pos / SPG_REFINED_CELL_SIZE;
	ivec2 local_pos = atlas_pos - probe_pos * SPG_REFINED_CELL_SIZE;
	ivec2 sub_tile = local_pos / SPG_REFINED_DIRECTION_RESOLUTION;
	ivec2 dir_tile = local_pos - sub_tile * SPG_REFINED_DIRECTION_RESOLUTION;
	vec4 mask = texelFetch(previous_spg_refinement_mask, probe_pos, 0);
	vec4 previous_radiance_sample = texelFetch(previous_spg_refined_radiance, atlas_pos, 0);
	vec4 previous_meta_sample = texelFetch(previous_spg_refined_meta, atlas_pos, 0);
	vec4 previous_stats_sample = texelFetch(previous_spg_refined_stats, atlas_pos, 0);
	vec4 previous_visibility_sample = texelFetch(previous_spg_refined_visibility, atlas_pos, 0);
	vec4 previous_id_sample = texelFetch(previous_spg_refined_history_id, atlas_pos, 0);
	float previous_confidence = clamp(previous_radiance_sample.a, 0.0, 1.0);

	if (max(mask.r, mask.a * 0.70) < 0.18) {
		float fade = 0.50;
		imageStore(next_spg_refined_radiance_image, atlas_pos, vec4(previous_radiance_sample.rgb, previous_confidence * fade));
		imageStore(next_spg_refined_meta_image, atlas_pos, previous_confidence > 0.02 ? previous_meta_sample : vec4(0.5, 0.5, 1.0, 65504.0));
		imageStore(next_spg_refined_stats_image, atlas_pos, vec4(max(previous_stats_sample.x - 1.0, 0.0), previous_stats_sample.yzw * fade));
		imageStore(next_spg_refined_visibility_image, atlas_pos, vec4(previous_visibility_sample.xyz, previous_visibility_sample.w * fade));
		imageStore(next_spg_refined_history_id_image, atlas_pos, previous_confidence > 0.02 ? previous_id_sample : vec4(0.0));
		return;
	}

	int sub_spacing = max(SPG_PROBE_SPACING / SPG_REFINED_SUBDIVS, 1);
	ivec2 probe_base = probe_pos * SPG_PROBE_SPACING + sub_tile * sub_spacing;
	vec3 bin_dir = spg_refined_direction_from_tile(dir_tile);
	vec3 radiance_sum = vec3(0.0);
	vec3 normal_sum = vec3(0.0);
	float viewz_sum = 0.0;
	float viewz_sq_sum = 0.0;
	float hitdist_sum = 0.0;
	float hitdist_sq_sum = 0.0;
	float hitdist_weight_sum = 0.0;
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	float weight_sum = 0.0;
	float best_weight = 0.0;
	vec4 best_id = vec4(0.0);

	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			ivec2 pos = probe_base + ivec2(x, y);
			if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y) || texelFetch(history_validity_buffer, pos, 0).r < 0.5) {
				continue;
			}
			vec4 direction_sample = texelFetch(primary_diffuse_direction_buffer, pos, 0);
			if (direction_sample.a <= 0.02 || dot(direction_sample.xyz, direction_sample.xyz) <= 0.01) {
				continue;
			}
			vec3 sample_dir = normalize(direction_sample.xyz);
			float direction_weight = smoothstep(0.02, 0.78, max(dot(sample_dir, bin_dir), 0.0));
			if (direction_weight <= 0.01) {
				continue;
			}
			vec4 viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
			if (viewz_hitdist.x >= 60000.0) {
				continue;
			}
			vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
			vec3 normal = decode_normal(normal_roughness);
			float roughness = clamp(normal_roughness.a, 0.0, 1.0);
			float hemisphere_weight = smoothstep(-0.05, 0.35, dot(normal, sample_dir));
			float rough_weight = smoothstep(0.48, 0.92, roughness);
			float sample_weight = direction_weight * hemisphere_weight * direction_sample.a * rough_weight * mix(0.55, 1.0, mask.r);
			if (sample_weight <= 0.005) {
				continue;
			}
			vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
			vec3 radiance = demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, pos).rgb), albedo_metalness);
			float luma = luminance(radiance);
			radiance_sum += radiance * sample_weight;
			normal_sum += normal * sample_weight;
			viewz_sum += viewz_hitdist.x * sample_weight;
			viewz_sq_sum += viewz_hitdist.x * viewz_hitdist.x * sample_weight;
			if (viewz_hitdist.y > 0.0 && viewz_hitdist.y < 60000.0) {
				hitdist_sum += viewz_hitdist.y * sample_weight;
				hitdist_sq_sum += viewz_hitdist.y * viewz_hitdist.y * sample_weight;
				hitdist_weight_sum += sample_weight;
			}
			luma_sum += luma * sample_weight;
			luma_sq_sum += luma * luma * sample_weight;
			weight_sum += sample_weight;
			if (sample_weight > best_weight) {
				best_weight = sample_weight;
				best_id = receiver_surface_id_at(pos);
			}
		}
	}

	if (weight_sum <= 0.01) {
		float fade = 0.58;
		imageStore(next_spg_refined_radiance_image, atlas_pos, vec4(sanitize_color(previous_radiance_sample.rgb), previous_confidence * fade));
		imageStore(next_spg_refined_meta_image, atlas_pos, previous_confidence > 0.02 ? previous_meta_sample : vec4(0.5, 0.5, 1.0, 65504.0));
		imageStore(next_spg_refined_stats_image, atlas_pos, vec4(max(previous_stats_sample.x - 1.0, 0.0), previous_stats_sample.yzw * fade));
		imageStore(next_spg_refined_visibility_image, atlas_pos, vec4(previous_visibility_sample.xyz, previous_visibility_sample.w * fade));
		imageStore(next_spg_refined_history_id_image, atlas_pos, previous_confidence > 0.02 ? previous_id_sample : vec4(0.0));
		return;
	}

	vec3 current = sanitize_color(radiance_sum / max(weight_sum, 1e-5));
	vec3 current_normal = normalize(normal_sum / max(weight_sum, 1e-5));
	float current_viewz = viewz_sum / max(weight_sum, 1e-5);
	float mean_luma = luma_sum / max(weight_sum, 1e-5);
	float variance = max(luma_sq_sum / max(weight_sum, 1e-5) - mean_luma * mean_luma, 0.0);
	float viewz_variance = max(viewz_sq_sum / max(weight_sum, 1e-5) - current_viewz * current_viewz, 0.0);
	float plane_quality = 1.0 - smoothstep(0.015, 0.120, sqrt(viewz_variance) / max(current_viewz, 0.25));
	float support = clamp(weight_sum * 0.38, 0.0, 1.0);
	float coherence = 1.0 - smoothstep(0.25, 0.95, sqrt(variance) / max(mean_luma, 0.08));
	float current_confidence = clamp(support * mix(0.40, 1.0, coherence) * mix(0.35, 1.0, plane_quality) * mix(0.35, 1.0, mask.r), 0.0, 1.0);
	float current_hitdist = hitdist_weight_sum > 0.01 ? hitdist_sum / max(hitdist_weight_sum, 1e-5) : 65504.0;
	float hitdist_variance = hitdist_weight_sum > 0.01 ? max(hitdist_sq_sum / max(hitdist_weight_sum, 1e-5) - current_hitdist * current_hitdist, 0.0) : 0.0;
	float hitdist_quality = current_hitdist < 60000.0 ? 1.0 - smoothstep(0.08, 0.75, sqrt(hitdist_variance) / max(current_hitdist, 0.25)) : 0.0;
	float visibility_support = clamp(hitdist_weight_sum / max(weight_sum, 1e-5), 0.0, 1.0);
	vec3 filtered = current;
	float out_confidence = current_confidence;
	float out_age = 1.0;

	if (previous_confidence > 0.04 && packed_id_valid(previous_id_sample)) {
		float normal_weight = smoothstep(0.65, 0.98, dot(current_normal, decode_normal(previous_meta_sample)));
		float depth_weight = 1.0 - smoothstep(0.04, 0.20, relative_delta(current_viewz, previous_meta_sample.w, 0.25));
		float previous_quality = clamp(previous_stats_sample.y * previous_stats_sample.z * previous_stats_sample.w, 0.0, 1.0);
		float history_weight = clamp(previous_confidence * normal_weight * depth_weight * previous_quality * 0.48, 0.0, 0.52);
		filtered = sanitize_color(mix(current, clamp_luminance(previous_radiance_sample.rgb, max(mean_luma * 2.15 + 0.05, 0.08)), history_weight));
		out_confidence = clamp(mix(current_confidence, previous_confidence, history_weight) + 0.015, 0.0, 1.0);
		out_age = min(previous_stats_sample.x + 1.0, params.max_history);
	}

	imageStore(next_spg_refined_radiance_image, atlas_pos, vec4(filtered, out_confidence));
	imageStore(next_spg_refined_meta_image, atlas_pos, vec4(current_normal * 0.5 + 0.5, current_viewz));
	imageStore(next_spg_refined_stats_image, atlas_pos, vec4(out_age, support, coherence, plane_quality));
	imageStore(next_spg_refined_visibility_image, atlas_pos, vec4(current_hitdist, hitdist_quality, current_viewz, visibility_support));
	imageStore(next_spg_refined_history_id_image, atlas_pos, best_id);
}

bool load_reconstruction_candidate(ivec2 candidate_pos, int slot, vec4 current_cache_id, vec3 current, float current_luma, vec4 current_normal_roughness, vec4 current_viewz_hitdist, out vec4 candidate_radiance, out vec4 candidate_meta, out vec4 candidate_stats, out float candidate_quality) {
	candidate_quality = 0.0;
	ivec2 cache_size = cache_size_i();
	if (candidate_pos.x < 0 || candidate_pos.y < 0 || candidate_pos.x >= cache_size.x || candidate_pos.y >= cache_size.y) {
		return false;
	}

	ivec2 storage_pos = cache_slot_pos(candidate_pos, slot);
	vec4 candidate_id = texelFetch(previous_cache_history_id, storage_pos, 0);
	bool exact_history = packed_id_valid(current_cache_id) && packed_id_valid(candidate_id) && history_id_matches(current_cache_id, candidate_id);
	candidate_radiance = texelFetch(previous_radiance, storage_pos, 0);
	candidate_meta = texelFetch(previous_meta, storage_pos, 0);
	candidate_stats = texelFetch(previous_stats, storage_pos, 0);
	float candidate_age = candidate_stats.w;
	float candidate_confidence = candidate_radiance.a;
	if (candidate_age < 1.0 || candidate_confidence <= 0.04) {
		return false;
	}
	if (!exact_history && (candidate_age < 5.0 || candidate_confidence <= 0.10)) {
		return false;
	}

	float roughness = current_normal_roughness.a;
	vec3 current_normal = decode_normal(current_normal_roughness);
	vec3 candidate_normal = decode_normal(candidate_meta);
	float exact_normal_threshold = mix(0.72, 0.02, clamp(roughness, 0.0, 1.0));
	float relaxed_normal_threshold = mix(0.97, 0.84, clamp(roughness, 0.0, 1.0));
	float normal_threshold = exact_history ? exact_normal_threshold : relaxed_normal_threshold;
	float normal_dot = dot(current_normal, candidate_normal);
	if (normal_dot < normal_threshold) {
		return false;
	}

	float depth_delta = relative_delta(current_viewz_hitdist.x, candidate_meta.w, 0.25);
	float hit_delta = relative_delta(current_viewz_hitdist.y, candidate_stats.x, 0.25);
	float depth_threshold = mix(0.10, 0.35, clamp(roughness, 0.0, 1.0));
	float hit_threshold = mix(0.28, 1.50, clamp(roughness, 0.0, 1.0));
	if (!exact_history) {
		depth_threshold *= 0.45;
		hit_threshold *= 0.45;
	}
	if (depth_delta > depth_threshold || hit_delta > hit_threshold) {
		return false;
	}

	vec3 candidate = sanitize_color(candidate_radiance.rgb);
	float candidate_luma = luminance(candidate);
	float radiance_delta = relative_delta(current_luma, candidate_luma, 0.08);
	float variance_ratio = variance_ratio_from_stats(candidate_stats);
	float radiance_delta_limit = exact_history ? 2.50 : 1.10;
	float variance_limit = exact_history ? 1.85 : 0.95;
	if (radiance_delta > radiance_delta_limit || variance_ratio > variance_limit) {
		return false;
	}

	float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
	float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
	float hit_weight = 1.0 - smoothstep(hit_threshold * 0.35, hit_threshold, hit_delta);
	float delta_weight = 1.0 - smoothstep(radiance_delta_limit * 0.30, radiance_delta_limit, radiance_delta);
	float variance_weight = 1.0 - smoothstep(variance_limit * 0.20, variance_limit, variance_ratio);
	float age_weight = exact_history ? smoothstep(1.0, 18.0, candidate_age) : smoothstep(5.0, 24.0, candidate_age);
	candidate_quality = candidate_confidence * normal_weight * depth_weight * hit_weight * delta_weight * variance_weight * mix(0.50, 1.0, age_weight);
	candidate_quality *= exact_history ? 1.0 : 0.32;
	return candidate_quality > (exact_history ? 0.04 : 0.025);
}

bool sample_refined_directional_screen_probe(ivec2 pos, vec4 current_cache_id, vec3 current, float current_luma, vec4 current_normal_roughness, vec4 current_viewz_hitdist, out vec3 spg_lighting, out float spg_confidence) {
	spg_lighting = vec3(0.0);
	spg_confidence = 0.0;
	ivec2 probe_size = spg_probe_size_i();
	ivec2 base_probe_pos = clamp(pos / SPG_PROBE_SPACING, ivec2(0), probe_size - ivec2(1));
	vec3 current_normal = decode_normal(current_normal_roughness);
	float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
	float normal_threshold = mix(0.78, 0.12, roughness);
	float depth_threshold = mix(0.055, 0.24, roughness);
	int sub_spacing = max(SPG_PROBE_SPACING / SPG_REFINED_SUBDIVS, 1);

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float best_quality = 0.0;
	for (int py = -1; py <= 1; py++) {
		for (int px = -1; px <= 1; px++) {
			ivec2 probe_pos = base_probe_pos + ivec2(px, py);
			if (probe_pos.x < 0 || probe_pos.y < 0 || probe_pos.x >= probe_size.x || probe_pos.y >= probe_size.y) {
				continue;
			}
			vec4 mask = texelFetch(previous_spg_refinement_mask, probe_pos, 0);
			float mask_weight = max(mask.r, mask.a * 0.70);
			if (mask_weight <= 0.16) {
				continue;
			}

			for (int sy = 0; sy < SPG_REFINED_SUBDIVS; sy++) {
				for (int sx = 0; sx < SPG_REFINED_SUBDIVS; sx++) {
					ivec2 sub_tile = ivec2(sx, sy);
					vec2 sub_center = vec2(probe_pos * SPG_PROBE_SPACING + sub_tile * sub_spacing) + vec2(float(sub_spacing) * 0.5);
					vec2 pixel_delta = (vec2(pos) + vec2(0.5)) - sub_center;
					float spatial_weight = exp2(-dot(pixel_delta, pixel_delta) / 12.0);
					for (int dy = 0; dy < SPG_REFINED_DIRECTION_RESOLUTION; dy++) {
						for (int dx = 0; dx < SPG_REFINED_DIRECTION_RESOLUTION; dx++) {
							ivec2 dir_tile = ivec2(dx, dy);
							ivec2 atlas_pos = spg_refined_atlas_pos(probe_pos, sub_tile, dir_tile);
							vec4 radiance_sample = texelFetch(previous_spg_refined_radiance, atlas_pos, 0);
							vec4 stats_sample = texelFetch(previous_spg_refined_stats, atlas_pos, 0);
							float confidence = clamp(radiance_sample.a, 0.0, 1.0);
							float stats_quality = clamp(smoothstep(1.0, 8.0, stats_sample.x) * stats_sample.y * stats_sample.z * stats_sample.w, 0.0, 1.0);
							if (confidence <= 0.04 || stats_quality <= 0.015) {
								continue;
							}

							vec4 meta_sample = texelFetch(previous_spg_refined_meta, atlas_pos, 0);
							vec3 sample_normal = decode_normal(meta_sample);
							float normal_dot = dot(current_normal, sample_normal);
							if (normal_dot < normal_threshold) {
								continue;
							}
							float depth_delta = relative_delta(current_viewz_hitdist.x, meta_sample.w, 0.25);
							if (depth_delta > depth_threshold) {
								continue;
							}
							vec4 sample_id = texelFetch(previous_spg_refined_history_id, atlas_pos, 0);
							bool exact_history = packed_id_valid(current_cache_id) && packed_id_valid(sample_id) && history_id_matches(current_cache_id, sample_id);
							if (!exact_history && confidence < 0.16) {
								continue;
							}
							float surface_weight = spg_surface_compatibility(current_cache_id, current_normal, current_viewz_hitdist.x, roughness, sample_id, sample_normal, meta_sample.w, roughness);
							if (surface_weight <= 0.0) {
								continue;
							}
							vec4 visibility_sample = texelFetch(previous_spg_refined_visibility, atlas_pos, 0);
							float visibility_weight = spg_visibility_hit_weight(visibility_sample, current_viewz_hitdist.y, roughness);
							if (visibility_weight <= 0.05) {
								continue;
							}
							vec3 dir = spg_refined_direction_from_tile(dir_tile);
							float hemisphere = smoothstep(-0.02, 0.62, dot(current_normal, dir));
							if (hemisphere <= 0.01) {
								continue;
							}
							vec3 sample_color = sanitize_color(radiance_sample.rgb);
							float sample_luma = luminance(sample_color);
							float radiance_delta = relative_delta(current_luma, sample_luma, 0.08);
							float delta_limit = exact_history ? 2.25 : 1.05;
							if (radiance_delta > delta_limit) {
								continue;
							}

							float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
							float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
							float delta_weight = 1.0 - smoothstep(delta_limit * 0.25, delta_limit, radiance_delta);
							float direction_weight = mix(0.18, 1.0, hemisphere);
							float history_weight = exact_history ? 1.0 : 0.30;
							float quality = confidence * stats_quality * spatial_weight * normal_weight * depth_weight * surface_weight * visibility_weight * delta_weight * direction_weight * history_weight * mask_weight;
							if (quality <= 0.016) {
								continue;
							}
							sum += sample_color * quality;
							weight_sum += quality;
							best_quality = max(best_quality, quality);
						}
					}
				}
			}
		}
	}

	if (weight_sum <= 0.035 || best_quality <= 0.030) {
		return false;
	}
	spg_lighting = sanitize_color(sum / max(weight_sum, 1e-5));
	spg_confidence = clamp(best_quality * 0.74 + min(weight_sum, 1.0) * 0.15, 0.0, 1.0);
	return luminance(spg_lighting) > 0.0005 && spg_confidence > 0.030;
}

bool sample_directional_screen_probe(ivec2 pos, vec4 current_cache_id, vec3 current, float current_luma, vec4 current_normal_roughness, vec4 current_viewz_hitdist, out vec3 spg_lighting, out float spg_confidence, out vec4 spg_rejection_info) {
	spg_lighting = vec3(0.0);
	spg_confidence = 0.0;
	spg_rejection_info = vec4(SPG_REJECT_LOW_CONFIDENCE / SPG_REJECT_SCALE, 0.0, 0.0, 0.0);
	ivec2 probe_size = spg_probe_size_i();
	ivec2 base_probe_pos = clamp(pos / SPG_PROBE_SPACING, ivec2(0), probe_size - ivec2(1));
	vec3 current_normal = decode_normal(current_normal_roughness);
	float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
	float normal_threshold = mix(0.74, 0.10, roughness);
	float depth_threshold = mix(0.08, 0.32, roughness);

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float best_quality = 0.0;
	float best_rejection_reason = SPG_REJECT_LOW_CONFIDENCE;
	float best_rejection_strength = 0.0;
	for (int py = -1; py <= 1; py++) {
		for (int px = -1; px <= 1; px++) {
			ivec2 probe_pos = base_probe_pos + ivec2(px, py);
			if (probe_pos.x < 0 || probe_pos.y < 0 || probe_pos.x >= probe_size.x || probe_pos.y >= probe_size.y) {
				continue;
			}

			vec2 probe_center = (vec2(probe_pos) + vec2(0.5)) * float(SPG_PROBE_SPACING);
			vec2 pixel_delta = (vec2(pos) + vec2(0.5)) - probe_center;
			float spatial_weight = exp2(-dot(pixel_delta, pixel_delta) / 36.0);
			for (int dy = 0; dy < SPG_DIRECTION_RESOLUTION; dy++) {
				for (int dx = 0; dx < SPG_DIRECTION_RESOLUTION; dx++) {
					ivec2 dir_tile = ivec2(dx, dy);
					ivec2 atlas_pos = spg_atlas_pos(probe_pos, dir_tile);
					vec4 radiance_sample = texelFetch(previous_spg_radiance, atlas_pos, 0);
					vec4 stats_sample = texelFetch(previous_spg_stats, atlas_pos, 0);
					vec4 visibility_sample = texelFetch(previous_spg_visibility, atlas_pos, 0);
					float confidence = clamp(radiance_sample.a, 0.0, 1.0);
					float stats_quality = clamp(smoothstep(1.0, 10.0, stats_sample.x) * stats_sample.y * stats_sample.z * stats_sample.w, 0.0, 1.0);
					if (confidence <= 0.04 || stats_quality <= 0.015) {
						spg_record_rejection(confidence <= 0.04 ? SPG_REJECT_LOW_CONFIDENCE : SPG_REJECT_STATS, max(confidence, stats_quality) * spatial_weight, best_rejection_reason, best_rejection_strength);
						continue;
					}

					vec4 meta_sample = texelFetch(previous_spg_meta, atlas_pos, 0);
					vec3 sample_normal = decode_normal(meta_sample);
					float normal_dot = dot(current_normal, sample_normal);
					if (normal_dot < normal_threshold) {
						spg_record_rejection(SPG_REJECT_NORMAL, confidence * stats_quality * spatial_weight * (1.0 - smoothstep(0.0, normal_threshold, normal_dot)), best_rejection_reason, best_rejection_strength);
						continue;
					}

					float depth_delta = relative_delta(current_viewz_hitdist.x, meta_sample.w, 0.25);
					if (depth_delta > depth_threshold) {
						spg_record_rejection(SPG_REJECT_DEPTH, confidence * stats_quality * spatial_weight * smoothstep(depth_threshold, depth_threshold * 2.0, depth_delta), best_rejection_reason, best_rejection_strength);
						continue;
					}

					vec4 sample_id = texelFetch(previous_spg_history_id, atlas_pos, 0);
					bool exact_history = packed_id_valid(current_cache_id) && packed_id_valid(sample_id) && history_id_matches(current_cache_id, sample_id);
					if (!exact_history && confidence < 0.18) {
						spg_record_rejection(SPG_REJECT_HISTORY, confidence * stats_quality * spatial_weight, best_rejection_reason, best_rejection_strength);
						continue;
					}
					float surface_weight = spg_surface_compatibility(current_cache_id, current_normal, current_viewz_hitdist.x, roughness, sample_id, sample_normal, meta_sample.w, roughness);
					if (surface_weight <= 0.0) {
						spg_record_rejection(SPG_REJECT_SURFACE, confidence * stats_quality * spatial_weight, best_rejection_reason, best_rejection_strength);
						continue;
					}
					float visibility_weight = spg_visibility_hit_weight(visibility_sample, current_viewz_hitdist.y, roughness);
					if (visibility_weight <= 0.05) {
						spg_record_rejection(SPG_REJECT_VISIBILITY, confidence * stats_quality * spatial_weight * (1.0 - visibility_weight), best_rejection_reason, best_rejection_strength);
						continue;
					}

					vec3 dir = spg_direction_from_tile(dir_tile);
					float hemisphere = smoothstep(-0.02, 0.62, dot(current_normal, dir));
					if (hemisphere <= 0.01) {
						spg_record_rejection(SPG_REJECT_HEMISPHERE, confidence * stats_quality * spatial_weight * surface_weight * visibility_weight, best_rejection_reason, best_rejection_strength);
						continue;
					}

					vec3 sample_color = sanitize_color(radiance_sample.rgb);
					float sample_luma = luminance(sample_color);
					float radiance_delta = relative_delta(current_luma, sample_luma, 0.08);
					float delta_limit = exact_history ? 2.65 : 1.20;
					if (radiance_delta > delta_limit) {
						spg_record_rejection(SPG_REJECT_RADIANCE, confidence * stats_quality * spatial_weight * smoothstep(delta_limit, delta_limit * 2.0, radiance_delta), best_rejection_reason, best_rejection_strength);
						continue;
					}

					float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
					float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
					float delta_weight = 1.0 - smoothstep(delta_limit * 0.25, delta_limit, radiance_delta);
					float history_weight = exact_history ? 1.0 : 0.32;
					float direction_weight = mix(0.20, 1.0, hemisphere);
					float quality = confidence * stats_quality * spatial_weight * normal_weight * depth_weight * surface_weight * visibility_weight * delta_weight * direction_weight * history_weight;
					if (quality <= 0.018) {
						spg_record_rejection(SPG_REJECT_LOW_QUALITY, max(quality, confidence * stats_quality * spatial_weight * 0.25), best_rejection_reason, best_rejection_strength);
						continue;
					}

					sum += sample_color * quality;
					weight_sum += quality;
					best_quality = max(best_quality, quality);
				}
			}
		}
	}

	if (weight_sum <= 0.05 || best_quality <= 0.035) {
		if (best_quality > 0.0 || weight_sum > 0.0) {
			best_rejection_reason = SPG_REJECT_LOW_QUALITY;
			best_rejection_strength = max(best_rejection_strength, max(best_quality, weight_sum));
		}
		spg_rejection_info = vec4(best_rejection_reason / SPG_REJECT_SCALE, clamp(best_rejection_strength, 0.0, 1.0), clamp(weight_sum, 0.0, 1.0), 0.0);
		return false;
	}

	spg_lighting = sanitize_color(sum / max(weight_sum, 1e-5));
	spg_confidence = clamp(best_quality * 0.85 + min(weight_sum, 1.0) * 0.18, 0.0, 1.0);
	bool accepted = luminance(spg_lighting) > 0.0005 && spg_confidence > 0.035;
	spg_rejection_info = accepted ? vec4(SPG_REJECT_NONE, clamp(best_quality, 0.0, 1.0), clamp(weight_sum, 0.0, 1.0), clamp(spg_confidence, 0.0, 1.0)) : vec4(SPG_REJECT_RADIANCE / SPG_REJECT_SCALE, clamp(best_quality, 0.0, 1.0), clamp(weight_sum, 0.0, 1.0), 0.0);
	return accepted;
}

void reconstruct_output_pixel(ivec2 pos) {
	vec4 current_albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec3 current = demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, pos).rgb), current_albedo_metalness);
	vec4 current_normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 current_viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
	vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
	float clamp_activity = 0.0;
	current = clamp_current_diffuse_outlier(pos, current, current_normal_roughness, current_viewz_hitdist, confidence_signal, clamp_activity);
	float current_luma = luminance(current);
	float current_valid = texelFetch(history_validity_buffer, pos, 0).r >= 0.5 ? 1.0 : 0.0;
	float guide_valid = current_viewz_hitdist.x < 60000.0 ? 1.0 : 0.0;
	float clamp_risk = signal_clamp_risk(confidence_signal);
	float current_confidence = diffuse_cache_current_confidence(current_valid, guide_valid, clamp_risk);
	vec4 current_cache_id = receiver_surface_id_at(pos);
	vec3 world_pos = vec3(0.0);
	vec3 strc_lighting = vec3(0.0);
	float strc_confidence = 0.0;
	bool strc_valid = guide_valid > 0.5 &&
			diffuse_cache_world_pos_from_depth(pos, world_pos) &&
			diffuse_cache_sample_strc(world_pos, decode_normal(current_normal_roughness), strc_lighting, strc_confidence);

	ivec2 base_cache_pos = output_pos_to_cache_pos(pos);
	vec3 cache_radiance_sum = vec3(0.0);
	float cache_confidence_sum = 0.0;
	float cache_mean_sum = 0.0;
	float cache_second_sum = 0.0;
	float cache_age_sum = 0.0;
	float cache_luma_sum = 0.0;
	float cache_luma_sq_sum = 0.0;
	float cache_weight_sum = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec4 candidate_radiance;
			vec4 candidate_meta;
			vec4 candidate_stats;
			float candidate_quality = 0.0;
			ivec2 candidate_pos = base_cache_pos + ivec2(x, y);
			for (int slot = 0; slot < CACHE_SLOT_COUNT; slot++) {
				if (load_reconstruction_candidate(candidate_pos, slot, current_cache_id, current, current_luma, current_normal_roughness, current_viewz_hitdist, candidate_radiance, candidate_meta, candidate_stats, candidate_quality)) {
					float spatial_weight = exp2(-dot(vec2(x, y), vec2(x, y)) * 0.55);
					float candidate_weight = candidate_quality * spatial_weight;
					vec3 candidate_color = sanitize_color(candidate_radiance.rgb);
					float candidate_luma = luminance(candidate_color);
					cache_radiance_sum += candidate_color * candidate_weight;
					cache_confidence_sum += candidate_radiance.a * candidate_weight;
					cache_mean_sum += candidate_stats.y * candidate_weight;
					cache_second_sum += candidate_stats.z * candidate_weight;
					cache_age_sum += candidate_stats.w * candidate_weight;
					cache_luma_sum += candidate_luma * candidate_weight;
					cache_luma_sq_sum += candidate_luma * candidate_luma * candidate_weight;
					cache_weight_sum += candidate_weight;
				}
			}
		}
	}

	vec3 filtered = current;
	float out_age = 1.0;
	float out_confidence = current_confidence * 0.5;
	float hit = 0.0;
	float rejection = 0.0;
	float cache_stability = 0.0;
	if (current_confidence <= 0.05) {
		rejection = 1.0;
	} else if (cache_weight_sum <= 0.0) {
		rejection = 8.0;
	} else {
		hit = 1.0;
		vec3 cached = sanitize_color(cache_radiance_sum / max(cache_weight_sum, 1e-5));
		float cached_luma = luminance(cached);
		float cached_age = cache_age_sum / max(cache_weight_sum, 1e-5);
		float cached_confidence = cache_confidence_sum / max(cache_weight_sum, 1e-5);
		float cached_mean = max(cache_mean_sum / max(cache_weight_sum, 1e-5), 0.0);
		float cached_second = max(cache_second_sum / max(cache_weight_sum, 1e-5), 0.0);
		float cached_variance = max(cached_second - cached_mean * cached_mean, 0.0);
		float cached_sigma = sqrt(cached_variance);
		float radiance_delta = relative_delta(current_luma, cached_luma, 0.08);
		float delta_weight = 1.0 - smoothstep(0.75, 2.50, radiance_delta);
		float variance_ratio = cached_sigma / max(cached_mean, 0.08);
		float variance_weight = 1.0 - smoothstep(0.35, 1.85, variance_ratio);
		float age_weight = smoothstep(1.0, 18.0, cached_age);
		float cache_support = smoothstep(0.10, 0.38, cache_weight_sum);
		float cache_luma_mean = cache_luma_sum / max(cache_weight_sum, 1e-5);
		float cache_luma_variance = max(cache_luma_sq_sum / max(cache_weight_sum, 1e-5) - cache_luma_mean * cache_luma_mean, 0.0);
		float cache_coherence_ratio = sqrt(cache_luma_variance) / max(cache_luma_mean, 0.08);
		float cache_coherence = 1.0 - smoothstep(0.18, 0.85, cache_coherence_ratio);
		float history_weight = min(0.82, mix(0.32, 0.78, age_weight) * min(current_confidence, cached_confidence) * delta_weight * mix(0.55, 1.0, variance_weight) * cache_support * cache_coherence);
		history_weight *= diffuse_cache_motion_reuse(velocity_pixels_at(pos));
		float max_cached_luma = max(current_luma * 2.35 + 0.05, cached_mean + cached_sigma * 2.0 + 0.04);
		filtered = sanitize_color(mix(current, clamp_luminance(cached, max_cached_luma), history_weight));
		out_age = min(cached_age, params.max_history);
		out_confidence = clamp(mix(current_confidence, cached_confidence, history_weight) + 0.03, 0.0, 1.0);
		cache_stability = clamp(out_confidence * cache_support * cache_coherence * variance_weight * age_weight, 0.0, 1.0);
	}

	vec3 spg_lighting = vec3(0.0);
	float spg_confidence = 0.0;
	vec4 spg_rejection_info = guide_valid > 0.5 ? vec4(SPG_REJECT_LOW_CONFIDENCE / SPG_REJECT_SCALE, 0.0, 0.0, 0.0) : vec4(SPG_REJECT_DEPTH / SPG_REJECT_SCALE, 1.0, 0.0, 0.0);
	vec3 refined_spg_lighting = vec3(0.0);
	float refined_spg_confidence = 0.0;
	bool refined_spg_used = false;
	if (guide_valid > 0.5 && sample_refined_directional_screen_probe(pos, current_cache_id, current, current_luma, current_normal_roughness, current_viewz_hitdist, refined_spg_lighting, refined_spg_confidence)) {
		float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
		float rough_diffuse = smoothstep(0.46, 0.92, roughness);
		float miss_weight = hit > 0.5 ? 0.42 : 1.0;
		float instability_weight = smoothstep(0.08, 0.90, 1.0 - cache_stability);
		float current_noise_weight = mix(1.0, 0.60, current_confidence);
		float refined_blend = clamp(refined_spg_confidence * rough_diffuse * miss_weight * mix(0.45, 1.0, instability_weight) * current_noise_weight, 0.0, hit > 0.5 ? 0.08 : 0.16);
		if (refined_blend > 0.01) {
			float filtered_luma = luminance(filtered);
			float max_refined_luma = max(filtered_luma * 2.25 + 0.05, current_luma * 2.60 + 0.05);
			filtered = sanitize_color(mix(filtered, clamp_luminance(refined_spg_lighting, max_refined_luma), refined_blend));
			out_confidence = clamp(max(out_confidence, refined_blend * 0.90 + refined_spg_confidence * 0.08), 0.0, 1.0);
			cache_stability = clamp(max(cache_stability, refined_blend * 0.82), 0.0, 1.0);
			hit = max(hit, refined_blend > 0.04 ? 0.62 : 0.30);
			refined_spg_used = true;
			spg_rejection_info = vec4(SPG_REJECT_NONE, clamp(refined_blend, 0.0, 1.0), clamp(refined_spg_confidence, 0.0, 1.0), clamp(refined_spg_confidence, 0.0, 1.0));
			if (rejection > 0.0 && luminance(refined_spg_lighting) > 0.0005) {
				rejection = min(rejection, 5.5);
			}
		}
	}
	vec4 base_spg_rejection_info = spg_rejection_info;
	if (guide_valid > 0.5 && sample_directional_screen_probe(pos, current_cache_id, current, current_luma, current_normal_roughness, current_viewz_hitdist, spg_lighting, spg_confidence, base_spg_rejection_info)) {
		spg_rejection_info = base_spg_rejection_info;
		float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
		float rough_diffuse = smoothstep(0.46, 0.92, roughness);
		float miss_weight = hit > 0.5 ? 0.48 : 1.0;
		float instability_weight = smoothstep(0.08, 0.90, 1.0 - cache_stability);
		float current_noise_weight = mix(1.0, 0.58, current_confidence);
		float spg_blend = clamp(spg_confidence * rough_diffuse * miss_weight * mix(0.42, 1.0, instability_weight) * current_noise_weight, 0.0, hit > 0.5 ? 0.18 : 0.34);
		if (spg_blend > 0.01) {
			float filtered_luma = luminance(filtered);
			float max_spg_luma = max(filtered_luma * 2.45 + 0.05, current_luma * 2.85 + 0.05);
			filtered = sanitize_color(mix(filtered, clamp_luminance(spg_lighting, max_spg_luma), spg_blend));
			out_confidence = clamp(max(out_confidence, spg_blend * 0.88 + spg_confidence * 0.08), 0.0, 1.0);
			cache_stability = clamp(max(cache_stability, spg_blend * 0.76), 0.0, 1.0);
			hit = max(hit, spg_blend > 0.05 ? 0.55 : 0.25);
			if (rejection > 0.0 && luminance(spg_lighting) > 0.0005) {
				rejection = min(rejection, 6.0);
			}
		}
	} else if (!refined_spg_used) {
		spg_rejection_info = base_spg_rejection_info;
	}

	if (strc_valid) {
		float roughness = clamp(current_normal_roughness.a, 0.0, 1.0);
		float rough_diffuse = smoothstep(0.42, 0.90, roughness);
		float miss_weight = hit > 0.5 ? 0.45 : 1.0;
		float instability_weight = smoothstep(0.10, 0.85, 1.0 - cache_stability);
		float current_noise_weight = mix(1.0, 0.55, current_confidence);
		float strc_blend = clamp(strc_confidence * rough_diffuse * miss_weight * instability_weight * current_noise_weight, 0.0, hit > 0.5 ? 0.16 : 0.28);
		if (strc_blend > 0.01) {
			float strc_luma = luminance(strc_lighting);
			float filtered_luma = luminance(filtered);
			float max_strc_luma = max(filtered_luma * 2.50 + 0.05, current_luma * 2.85 + 0.05);
			vec3 clamped_strc = clamp_luminance(strc_lighting, max_strc_luma);
			filtered = sanitize_color(mix(filtered, clamped_strc, strc_blend));
			out_confidence = clamp(max(out_confidence, strc_blend * 0.85 + strc_confidence * 0.10), 0.0, 1.0);
			cache_stability = clamp(max(cache_stability, strc_blend * 0.70), 0.0, 1.0);
			hit = max(hit, strc_blend > 0.04 ? 0.35 : hit);
			if (rejection > 0.0 && strc_luma > 0.0005) {
				rejection = min(rejection, 7.0);
			}
		}
	}

	float rejection_risk = rejection > 0.0 ? smoothstep(0.0, 1.0, rejection / 9.0) : 0.0;
	float cache_risk = max(rejection_risk, (1.0 - cache_stability) * mix(0.35, 0.70, 1.0 - current_confidence));
	vec4 reconstruction_confidence = confidence_signal;
	reconstruction_confidence.a = clamp(max(confidence_signal.a * current_confidence * 0.55, cache_stability), 0.0, 1.0);
	reconstruction_confidence.g = max(reconstruction_confidence.g, clamp(cache_risk * 0.72, 0.0, 0.72));

	imageStore(output_image, pos, vec4(remodulate_diffuse_lighting(filtered, current_albedo_metalness), 1.0));
	imageStore(diagnostic_image, pos, vec4(hit, out_confidence, clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0), rejection / 9.0));
	imageStore(age_image, pos, vec4(clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0)));
	imageStore(rejection_image, pos, vec4(rejection / 9.0));
	imageStore(spg_rejection_image, pos, clamp(spg_rejection_info, vec4(0.0), vec4(1.0)));
	imageStore(cache_signal_confidence_image, pos, clamp(reconstruction_confidence, vec4(0.0), vec4(1.0)));
}

void age_surface_cache_entry(ivec2 pos) {
	vec4 surface_id = texelFetch(previous_surface_history_id, pos, 0);
	vec4 radiance_sample = texelFetch(previous_surface_radiance, pos, 0);
	vec4 meta_sample = texelFetch(previous_surface_meta, pos, 0);
	vec4 stats_sample = texelFetch(previous_surface_stats, pos, 0);
	if (!packed_id_valid(surface_id) || radiance_sample.a <= 0.003) {
		imageStore(next_surface_radiance_image, pos, vec4(0.0));
		imageStore(next_surface_meta_image, pos, vec4(0.5, 0.5, 1.0, surface_cache_source_bucket(SURFACE_CACHE_SOURCE_NONE)));
		imageStore(next_surface_stats_image, pos, vec4(0.0));
		imageStore(next_surface_history_id_image, pos, vec4(0.0));
		imageStore(next_surface_claim_image, pos, uvec4(0u));
		return;
	}

	float stale_frames = min(stats_sample.w + 1.0, 65504.0);
	float stale_fade = 1.0 - smoothstep(48.0, 128.0, stale_frames);
	float confidence = clamp(radiance_sample.a * mix(0.995, 0.970, smoothstep(12.0, 64.0, stale_frames)) * stale_fade, 0.0, 1.0);
	if (confidence <= 0.003) {
		imageStore(next_surface_radiance_image, pos, vec4(0.0));
		imageStore(next_surface_meta_image, pos, vec4(0.5, 0.5, 1.0, surface_cache_source_bucket(SURFACE_CACHE_SOURCE_NONE)));
		imageStore(next_surface_stats_image, pos, vec4(0.0));
		imageStore(next_surface_history_id_image, pos, vec4(0.0));
		imageStore(next_surface_claim_image, pos, uvec4(0u));
		return;
	}

	imageStore(next_surface_radiance_image, pos, vec4(sanitize_color(radiance_sample.rgb), confidence));
	imageStore(next_surface_meta_image, pos, meta_sample);
	imageStore(next_surface_stats_image, pos, vec4(stats_sample.xyz, stale_frames));
	imageStore(next_surface_history_id_image, pos, surface_id);
	imageStore(next_surface_claim_image, pos, uvec4(surface_cache_claim_from_quality(unpack_packed_id(surface_id), confidence * max(stats_sample.x, 0.05)), 0u, 0u, 0u));
}

#define SURFACE_FEEDBACK_STATUS_NONE 0.0
#define SURFACE_FEEDBACK_STATUS_SELECTED 1.0
#define SURFACE_FEEDBACK_STATUS_INVALID 2.0
#define SURFACE_FEEDBACK_STATUS_NO_RADIANCE 3.0
#define SURFACE_FEEDBACK_STATUS_BUDGET_STARVED 4.0
#define SURFACE_FEEDBACK_STATUS_COLLISION 5.0
#define SURFACE_FEEDBACK_STATUS_ATOMIC_SKIPPED 6.0
#define SURFACE_FEEDBACK_STATUS_LOW_CONFIDENCE 7.0
#define SURFACE_FEEDBACK_STATUS_LOW_QUALITY 8.0
#define SURFACE_FEEDBACK_STATUS_STALE_REFRESH 9.0
#define SURFACE_FEEDBACK_STATUS_DYNAMIC_INELIGIBLE 10.0
#define SURFACE_FEEDBACK_STATUS_MAX 10.0

void store_surface_feedback_diagnostic(ivec2 pos, float status, float detail, float source_quality, float source_class) {
	imageStore(surface_feedback_diagnostic_image, pos, vec4(
			clamp(status / SURFACE_FEEDBACK_STATUS_MAX, 0.0, 1.0),
			surface_cache_source_bucket(source_class),
			clamp(source_quality, 0.0, 1.0),
			clamp(detail, 0.0, 1.0)));
}

uint surface_feedback_budget(ivec2 feedback_size) {
	uint total = uint(max(feedback_size.x * feedback_size.y, 1));
	return min(total, clamp(total / 8u, 4096u, 131072u));
}

bool surface_feedback_selected_by_budget(uint surface_key, ivec2 feedback_pos, float priority, out float budget_detail) {
	ivec2 feedback_size = ivec2(max(params.resolution, vec2(1.0)));
	uint total = uint(max(feedback_size.x * feedback_size.y, 1));
	uint base_budget = surface_feedback_budget(feedback_size);
	float priority_bias = mix(0.70, 2.20, clamp(priority, 0.0, 1.0));
	uint effective_budget = min(total, max(1u, uint(float(base_budget) * priority_bias)));
	uint h = mix_u32(surface_key, uint(feedback_pos.x) ^ (uint(feedback_pos.y) << 16u));
	uint draw = h % total;
	budget_detail = clamp(float(effective_budget) / float(total), 0.0, 1.0);
	return draw < effective_budget;
}

bool choose_surface_cache_write_pos(uint surface_key, vec4 surface_id, float source_quality, out ivec2 surface_pos, out bool same_surface, out vec4 previous_radiance_sample, out vec4 previous_stats_sample, out float write_status) {
	ivec2 surface_size = imageSize(next_surface_radiance_image);
	surface_pos = ivec2(0);
	same_surface = false;
	previous_radiance_sample = vec4(0.0);
	previous_stats_sample = vec4(0.0);
	write_status = SURFACE_FEEDBACK_STATUS_COLLISION;
	if (surface_size.x <= 0 || surface_size.y <= 0) {
		return false;
	}

	float best_score = -1.0;
	for (int variant = 0; variant < SURFACE_CACHE_ASSOCIATIVITY; variant++) {
		ivec2 candidate_pos = surface_cache_pos_from_key_variant(surface_key, surface_size, variant);
		vec4 candidate_id = texelFetch(previous_surface_history_id, candidate_pos, 0);
		vec4 candidate_radiance = texelFetch(previous_surface_radiance, candidate_pos, 0);
		vec4 candidate_stats = texelFetch(previous_surface_stats, candidate_pos, 0);
		bool candidate_same = packed_id_valid(candidate_id) && history_id_matches(candidate_id, surface_id);
		bool candidate_empty = !packed_id_valid(candidate_id) || candidate_radiance.a <= 0.003;
		float candidate_score = -1.0;
		if (candidate_same) {
			candidate_score = 4.0;
		} else if (candidate_empty) {
			candidate_score = 3.0;
		} else if (candidate_stats.w >= 24.0) {
			candidate_score = 2.0 + clamp(candidate_stats.w / 128.0, 0.0, 0.75);
		} else if (candidate_radiance.a <= source_quality * 1.15) {
			candidate_score = 1.0 + clamp(source_quality - candidate_radiance.a, 0.0, 0.75);
		}
		candidate_score += float(SURFACE_CACHE_ASSOCIATIVITY - variant) * 0.001;
		if (candidate_score > best_score) {
			best_score = candidate_score;
			surface_pos = candidate_pos;
			same_surface = candidate_same;
			previous_radiance_sample = candidate_radiance;
			previous_stats_sample = candidate_stats;
			write_status = candidate_same || candidate_empty ? SURFACE_FEEDBACK_STATUS_SELECTED : (candidate_stats.w >= 24.0 ? SURFACE_FEEDBACK_STATUS_STALE_REFRESH : SURFACE_FEEDBACK_STATUS_COLLISION);
		}
	}

	return best_score >= 0.0;
}

void inject_receiver_surface_cache_entry(ivec2 receiver_pos) {
	int receiver_slot = int(clamp(receiver_pos.x % CACHE_SLOT_COUNT, 0, CACHE_SLOT_COUNT - 1));
	ivec2 receiver_cache_pos = ivec2(receiver_pos.x / CACHE_SLOT_COUNT, receiver_pos.y);
	ivec2 receiver_output_pos = cache_pos_to_output_center(receiver_cache_pos, receiver_slot);
	if (receiver_output_pos.x < 0 || receiver_output_pos.y < 0 || receiver_output_pos.x >= int(params.resolution.x) || receiver_output_pos.y >= int(params.resolution.y)) {
		return;
	}

	uint surface_key = imageLoad(previous_cache_surface_key_image, receiver_pos).r;
	if (surface_key == 0u) {
		return;
	}
	vec4 surface_id = pack_u32_rgba8(surface_key);

	vec4 radiance_sample = texelFetch(previous_radiance, receiver_pos, 0);
	vec4 stats_sample = texelFetch(previous_stats, receiver_pos, 0);
	vec4 receiver_normal_roughness = texelFetch(normal_roughness_buffer, receiver_output_pos, 0);
	vec4 receiver_viewz_hitdist = texelFetch(viewz_hitdist_buffer, receiver_output_pos, 0);
	vec4 receiver_cache_id = receiver_surface_id_at(receiver_output_pos);
	vec4 receiver_slot_id = texelFetch(previous_cache_history_id, receiver_pos, 0);
	float confidence = clamp(radiance_sample.a, 0.0, 1.0);
	float receiver_age = stats_sample.w;
	if (confidence <= 0.055 || receiver_age < 2.0) {
		return;
	}

	float variance_ratio = sqrt(max(stats_sample.z - stats_sample.y * stats_sample.y, 0.0)) / max(stats_sample.y, 0.08);
	float variance_weight = 1.0 - smoothstep(0.30, 1.35, variance_ratio);
	float age_weight = smoothstep(2.0, 18.0, receiver_age);
	float radiance_weight = smoothstep(0.00035, 0.0055, luminance(radiance_sample.rgb));
	float source_quality = clamp(confidence * variance_weight * mix(0.35, 1.0, age_weight) * radiance_weight, 0.0, 1.0);
	if (source_quality <= 0.020) {
		return;
	}

	ivec2 surface_pos = ivec2(0);
	vec4 previous_radiance_sample = vec4(0.0);
	vec4 previous_stats_sample = vec4(0.0);
	bool same_surface = false;
	float write_status = SURFACE_FEEDBACK_STATUS_SELECTED;
	if (!choose_surface_cache_write_pos(surface_key, surface_id, source_quality, surface_pos, same_surface, previous_radiance_sample, previous_stats_sample, write_status)) {
		return;
	}

	uint candidate_claim = surface_cache_claim_from_quality(surface_key, source_quality);
	uint previous_claim = imageAtomicMax(next_surface_claim_image, surface_pos, candidate_claim);
	if (previous_claim > candidate_claim) {
		return;
	}

	vec3 surface_radiance = sanitize_color(radiance_sample.rgb);
	float source_provenance_quality = surface_cache_source_quality(SURFACE_CACHE_SOURCE_RECEIVER);
	float surface_source_class = SURFACE_CACHE_SOURCE_RECEIVER;
	float receiver_luma = luminance(surface_radiance);
	if (receiver_viewz_hitdist.x < 60000.0 && receiver_luma > 0.0002) {
		uint current_visible_surface_key = imageLoad(current_surface_key_image, receiver_output_pos).r;
		bool visible_identity_agrees = current_visible_surface_key == surface_key && packed_id_valid(receiver_slot_id) && history_id_matches(receiver_slot_id, receiver_cache_id);
		vec4 visible_albedo_metalness = texelFetch(albedo_metalness_buffer, receiver_output_pos, 0);
		vec3 visible_current = demodulate_diffuse_radiance(sanitize_color(imageLoad(source_image, receiver_output_pos).rgb), visible_albedo_metalness);
		vec4 visible_confidence_signal = texelFetch(signal_confidence_buffer, receiver_output_pos, 0);
		float visible_clamp_activity = 0.0;
		visible_current = clamp_current_diffuse_outlier(receiver_output_pos, visible_current, receiver_normal_roughness, receiver_viewz_hitdist, visible_confidence_signal, visible_clamp_activity);
		float visible_luma = luminance(visible_current);
		float visible_radiance_weight = smoothstep(0.00035, 0.0055, visible_luma);
		float visible_delta = relative_delta(visible_luma, receiver_luma, 0.08);
		float visible_signal_risk = max(signal_clamp_risk(visible_confidence_signal), visible_clamp_activity);
		float visible_signal_weight = 1.0 - smoothstep(0.10, 0.55, visible_signal_risk);
		float visible_delta_weight = 1.0 - smoothstep(0.45, 1.45, visible_delta);
		float visible_metal_weight = 1.0 - smoothstep(0.20, 0.58, clamp(visible_albedo_metalness.a, 0.0, 1.0));
		float visible_valid = texelFetch(history_validity_buffer, receiver_output_pos, 0).r >= 0.5 ? 1.0 : 0.0;
		float visible_diffuse_weight = smoothstep(0.35, 0.72, clamp(receiver_normal_roughness.a, 0.0, 1.0)) * smoothstep(0.020, 0.12, luminance(visible_albedo_metalness.rgb));
		float visible_quality = clamp(confidence * variance_weight * age_weight * visible_radiance_weight * visible_signal_weight * visible_delta_weight * visible_metal_weight * visible_valid * visible_diffuse_weight, 0.0, 1.0);
		bool visible_current_agrees = visible_identity_agrees && receiver_age >= 5.0 && variance_ratio < 0.72 && visible_delta < 1.25;
		if (visible_current_agrees && visible_quality > max(0.060, source_quality * 0.78)) {
			float visible_blend = clamp(0.38 + visible_quality * 0.34, 0.34, 0.72);
			float receiver_sigma = sqrt(max(stats_sample.z - stats_sample.y * stats_sample.y, 0.0));
			float max_visible_luma = max(receiver_luma * 2.20 + 0.045, max(stats_sample.y, receiver_luma) + receiver_sigma * 2.0 + 0.045);
			vec3 clamped_visible = clamp_luminance(visible_current, max_visible_luma);
			surface_radiance = sanitize_color(mix(surface_radiance, clamped_visible, visible_blend));
			source_quality = max(source_quality, visible_quality);
			confidence = max(confidence, clamp(visible_quality * 0.82 + confidence * 0.16, 0.0, 0.94));
			variance_weight = max(variance_weight, clamp(visible_delta_weight * 0.72 + visible_signal_weight * 0.18, 0.0, 1.0));
			surface_source_class = SURFACE_CACHE_SOURCE_VISIBLE_CURRENT;
			source_provenance_quality = surface_cache_source_quality(surface_source_class);
		}

		vec3 refined_spg_lighting = vec3(0.0);
		float refined_spg_confidence = 0.0;
		if (surface_source_class == SURFACE_CACHE_SOURCE_RECEIVER &&
				sample_refined_directional_screen_probe(receiver_output_pos, receiver_cache_id, surface_radiance, receiver_luma, receiver_normal_roughness, receiver_viewz_hitdist, refined_spg_lighting, refined_spg_confidence)) {
			float refined_radiance_weight = smoothstep(0.00035, 0.0055, luminance(refined_spg_lighting));
			float refined_quality = clamp(refined_spg_confidence * refined_radiance_weight * mix(0.45, 1.0, source_quality), 0.0, 1.0);
			if (refined_quality > source_quality * 1.08 || receiver_age < 4.0) {
				float refined_blend = clamp(0.32 + refined_quality * 0.36, 0.28, 0.62);
				surface_radiance = sanitize_color(mix(surface_radiance, refined_spg_lighting, refined_blend));
				source_quality = max(source_quality, refined_quality * 0.86);
				confidence = max(confidence, clamp(refined_spg_confidence * 0.72, 0.0, 0.72));
				variance_weight = max(variance_weight, clamp(refined_radiance_weight * 0.60 + 0.22, 0.0, 1.0));
				surface_source_class = SURFACE_CACHE_SOURCE_REFINED_SPG;
				source_provenance_quality = surface_cache_source_quality(surface_source_class);
			}
		}

		vec3 base_spg_lighting = vec3(0.0);
		float base_spg_confidence = 0.0;
		vec4 base_spg_rejection_info = vec4(0.0);
		if (surface_source_class == SURFACE_CACHE_SOURCE_RECEIVER &&
				sample_directional_screen_probe(receiver_output_pos, receiver_cache_id, surface_radiance, receiver_luma, receiver_normal_roughness, receiver_viewz_hitdist, base_spg_lighting, base_spg_confidence, base_spg_rejection_info)) {
			float base_radiance_weight = smoothstep(0.00035, 0.0055, luminance(base_spg_lighting));
			float base_quality = clamp(base_spg_confidence * base_radiance_weight * mix(0.38, 0.86, source_quality), 0.0, 1.0);
			if (base_quality > source_quality * 1.12 || receiver_age < 3.0) {
				float base_blend = clamp(0.24 + base_quality * 0.30, 0.22, 0.54);
				surface_radiance = sanitize_color(mix(surface_radiance, base_spg_lighting, base_blend));
				source_quality = max(source_quality, base_quality * 0.78);
				confidence = max(confidence, clamp(base_spg_confidence * 0.64, 0.0, 0.64));
				variance_weight = max(variance_weight, clamp(base_radiance_weight * 0.55 + 0.18, 0.0, 1.0));
				surface_source_class = SURFACE_CACHE_SOURCE_BASE_SPG;
				source_provenance_quality = surface_cache_source_quality(surface_source_class);
			}
		}
	}

	float surface_confidence = clamp(source_quality * 0.82 + confidence * 0.18, 0.0, 1.0);
	float support = clamp(source_quality * 0.92 + source_provenance_quality * 0.08, 0.0, 1.0);
	float surface_source_bucket = surface_cache_source_bucket(surface_source_class);
	if (same_surface && previous_radiance_sample.a > 0.01) {
		float refresh_weight = clamp(0.22 + source_quality * 0.42 + smoothstep(20.0, 72.0, previous_stats_sample.w) * 0.22, 0.18, 0.84);
		float previous_luma = luminance(previous_radiance_sample.rgb);
		float current_luma = luminance(surface_radiance);
		float max_current_luma = max(previous_luma * 2.25 + 0.04, current_luma * 1.35 + 0.04);
		surface_radiance = sanitize_color(mix(previous_radiance_sample.rgb, clamp_luminance(surface_radiance, max_current_luma), refresh_weight));
		surface_confidence = clamp(max(previous_radiance_sample.a * 0.92, surface_confidence), 0.0, 1.0);
		support = clamp(max(previous_stats_sample.x * 0.88, source_quality), 0.0, 1.0);
		float previous_source_class = surface_cache_source_from_bucket(previous_stats_sample.z);
		if (surface_cache_source_quality(previous_source_class) > source_provenance_quality + 0.05) {
			surface_source_bucket = previous_stats_sample.z;
		}
	}

	imageStore(next_surface_radiance_image, surface_pos, vec4(surface_radiance, surface_confidence));
	imageStore(next_surface_meta_image, surface_pos, vec4(receiver_normal_roughness.xyz, clamp(receiver_normal_roughness.a, 0.0, 1.0)));
	imageStore(next_surface_stats_image, surface_pos, vec4(support, variance_weight, surface_source_bucket, 0.0));
	imageStore(next_surface_history_id_image, surface_pos, surface_id);
}

void inject_feedback_surface_cache_entry(ivec2 feedback_pos) {
	uint surface_key = imageLoad(surface_feedback_key_image, feedback_pos).r;
	if (surface_key == 0u) {
		float reject_reason = round(texelFetch(surface_feedback_stats, feedback_pos, 0).w * 2.0);
		if (reject_reason == 1.0) {
			store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_DYNAMIC_INELIGIBLE, 1.0, 0.0, 0.0);
		} else if (reject_reason > 1.0) {
			store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_INVALID, 1.0, 0.0, 0.0);
		} else {
			store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_NONE, 0.0, 0.0, 0.0);
		}
		return;
	}

	vec4 radiance_sample = texelFetch(surface_feedback_radiance, feedback_pos, 0);
	vec4 meta_sample = texelFetch(surface_feedback_meta, feedback_pos, 0);
	vec4 stats_sample = texelFetch(surface_feedback_stats, feedback_pos, 0);
	float confidence = clamp(radiance_sample.a, 0.0, 1.0);
	float source_quality = clamp(stats_sample.x, 0.0, 1.0);
	float support_hint = clamp(stats_sample.y, 0.0, 1.0);
	float radiance_weight = clamp(stats_sample.z, 0.0, 1.0);
	float source_class = surface_cache_source_from_bucket(stats_sample.w);
	float source_provenance_quality = surface_cache_source_quality(source_class);
	float radiance_luma = luminance(radiance_sample.rgb);
	if (confidence <= 0.012) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_LOW_CONFIDENCE, confidence / 0.012, source_quality, source_class);
		return;
	}
	if (source_quality <= 0.010) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_LOW_QUALITY, source_quality / 0.010, source_quality, source_class);
		return;
	}
	if (radiance_luma <= 0.0002) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_NO_RADIANCE, radiance_luma / 0.0002, source_quality, source_class);
		return;
	}

	float budget_detail = 0.0;
	float feedback_priority = clamp(source_quality * 0.60 + confidence * 0.20 + radiance_weight * 0.05 + source_provenance_quality * 0.15, 0.0, 1.0);
	if (!surface_feedback_selected_by_budget(surface_key, feedback_pos, feedback_priority, budget_detail)) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_BUDGET_STARVED, budget_detail, source_quality, source_class);
		return;
	}

	vec4 surface_id = pack_u32_rgba8(surface_key);
	ivec2 surface_pos = ivec2(0);
	vec4 previous_radiance_sample = vec4(0.0);
	vec4 previous_stats_sample = vec4(0.0);
	bool same_surface = false;
	float write_status = SURFACE_FEEDBACK_STATUS_SELECTED;
	if (!choose_surface_cache_write_pos(surface_key, surface_id, source_quality * mix(0.78, 0.96, source_provenance_quality), surface_pos, same_surface, previous_radiance_sample, previous_stats_sample, write_status)) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_COLLISION, 0.0, source_quality, source_class);
		return;
	}

	float claim_quality = clamp(source_quality * 0.62 + confidence * 0.08 + source_provenance_quality * 0.10, 0.0, 1.0);
	uint candidate_claim = surface_cache_claim_from_quality(surface_key, claim_quality);
	uint previous_claim = imageAtomicMax(next_surface_claim_image, surface_pos, candidate_claim);
	if (previous_claim > candidate_claim) {
		store_surface_feedback_diagnostic(feedback_pos, SURFACE_FEEDBACK_STATUS_ATOMIC_SKIPPED, clamp(float(previous_claim >> 24u) / 255.0, 0.0, 1.0), source_quality, source_class);
		return;
	}

	vec3 surface_radiance = sanitize_color(radiance_sample.rgb);
	float surface_confidence = clamp(confidence * 0.46 + source_quality * 0.22 + source_provenance_quality * 0.08, 0.0, mix(0.58, 0.80, source_provenance_quality));
	float support = clamp(source_quality * 0.62 + support_hint * 0.18 + source_provenance_quality * 0.08, 0.0, 1.0);
	float variance_quality = clamp(radiance_weight * 0.65 + 0.18, 0.0, 1.0);
	float surface_source_bucket = surface_cache_source_bucket(source_class);
	if (same_surface && previous_radiance_sample.a > 0.01) {
		float refresh_weight = clamp(0.28 + source_quality * 0.34 + smoothstep(20.0, 72.0, previous_stats_sample.w) * 0.18, 0.22, 0.72);
		float previous_luma = luminance(previous_radiance_sample.rgb);
		float current_luma = luminance(surface_radiance);
		float max_current_luma = max(previous_luma * 2.0 + 0.035, current_luma * 1.25 + 0.035);
		surface_radiance = sanitize_color(mix(previous_radiance_sample.rgb, clamp_luminance(surface_radiance, max_current_luma), refresh_weight));
		surface_confidence = clamp(max(previous_radiance_sample.a * 0.90, surface_confidence), 0.0, 0.82);
		support = clamp(max(previous_stats_sample.x * 0.82, support), 0.0, 1.0);
		variance_quality = clamp(max(previous_stats_sample.y * 0.72, variance_quality), 0.0, 1.0);
		float previous_source_class = surface_cache_source_from_bucket(previous_stats_sample.z);
		if (surface_cache_source_quality(previous_source_class) > source_provenance_quality + 0.05) {
			surface_source_bucket = previous_stats_sample.z;
		}
	}

	imageStore(next_surface_radiance_image, surface_pos, vec4(surface_radiance, surface_confidence));
	imageStore(next_surface_meta_image, surface_pos, meta_sample);
	imageStore(next_surface_stats_image, surface_pos, vec4(support, variance_quality, surface_source_bucket, 0.0));
	imageStore(next_surface_history_id_image, surface_pos, surface_id);
	store_surface_feedback_diagnostic(feedback_pos, write_status, budget_detail, source_quality, source_class);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (params.mode == 0u) {
		if (pos.x >= int(params.cache_resolution.x) || pos.y >= int(params.cache_resolution.y)) {
			return;
		}
		update_cache_entry(pos);
	} else if (params.mode == 2u) {
		ivec2 atlas_size = spg_atlas_size_i();
		if (pos.x >= atlas_size.x || pos.y >= atlas_size.y) {
			return;
		}
		update_directional_screen_probe(pos);
	} else if (params.mode == 3u) {
		ivec2 probe_size = spg_probe_size_i();
		if (pos.x >= probe_size.x || pos.y >= probe_size.y) {
			return;
		}
		update_spg_refinement_mask(pos);
	} else if (params.mode == 4u) {
		ivec2 atlas_size = spg_refined_atlas_size_i();
		if (pos.x >= atlas_size.x || pos.y >= atlas_size.y) {
			return;
		}
		update_refined_directional_screen_probe(pos);
	} else if (params.mode == 5u) {
		ivec2 surface_size = imageSize(next_surface_radiance_image);
		if (pos.x >= surface_size.x || pos.y >= surface_size.y) {
			return;
		}
		age_surface_cache_entry(pos);
	} else if (params.mode == 6u) {
		ivec2 persistent_size = ivec2(int(params.cache_resolution.x) * CACHE_SLOT_COUNT, int(params.cache_resolution.y));
		if (pos.x >= persistent_size.x || pos.y >= persistent_size.y) {
			return;
		}
		inject_receiver_surface_cache_entry(pos);
	} else if (params.mode == 7u) {
		if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
			return;
		}
		inject_feedback_surface_cache_entry(pos);
	} else {
		if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
			return;
		}
		reconstruct_output_pixel(pos);
	}
}
