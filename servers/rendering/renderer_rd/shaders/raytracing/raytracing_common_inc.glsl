// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant (bits 0-20: flags, 21-28: samples, 29-31: bounces).
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define RT_FLAG_DLSS_RR_ENABLED (1u << 1)
#define RT_FLAG_FOG_ENABLED (1u << 2)
#define RT_FLAG_STRC_ENABLED (1u << 4)
#define RT_FLAG_STRC_PROBE_UPDATE (1u << 5)

#define RT_SAMPLE_COUNT_SHIFT 21u
#define RT_SAMPLE_COUNT_MASK 0xFFu
#define RT_GET_SAMPLE_COUNT() max(1u, (RT_FLAGS >> RT_SAMPLE_COUNT_SHIFT) & RT_SAMPLE_COUNT_MASK)

#define RT_MAX_BOUNCES_SHIFT 29u
#define RT_MAX_BOUNCES_MASK 0x7u
#define RT_GET_MAX_BOUNCES() (((RT_FLAGS >> RT_MAX_BOUNCES_SHIFT) & RT_MAX_BOUNCES_MASK) + 1u)

// Cull back faces by default; double-sided instances override via CULL_DISABLE flag.
#define RT_RAY_FLAGS gl_RayFlagsCullBackFacingTrianglesEXT

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 14, std430) readonly buffer GlobalShaderUniformData {
	vec4 data[256];
}
global_shader_uniforms;

layout(set = 0, binding = 6, std140) uniform RaytracingParams {
	vec4 rt_params[10];
	mat4 prev_vp_unjittered;
	mat4 curr_vp_unjittered;
	mat4 inv_projection_unjittered;
	vec4 rt_view_rect;
	vec4 rt_prev_view_rect;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
}

bool rt_strc_probe_update_mode() {
	return (RT_FLAGS & RT_FLAG_STRC_PROBE_UPDATE) != 0u;
}

uint rt_strc_static_visual_layer_mask() {
	return uint(get_rt_param(RT_PARAM_RTGI_STRC_STATIC_VISUAL_LAYERS)) & 0xfffffu;
}

uint rt_strc_dynamic_visual_layer_mask() {
	return (uint(get_rt_param(RT_PARAM_RTGI_STRC_DYNAMIC_VISUAL_LAYERS)) & ~rt_strc_static_visual_layer_mask()) & 0xfffffu;
}

uint rt_strc_visual_layer_mask() {
	return rt_strc_static_visual_layer_mask() | rt_strc_dynamic_visual_layer_mask();
}

bool rt_strc_visual_layer_visible(uint p_layer_mask) {
	return (p_layer_mask & rt_strc_visual_layer_mask()) != 0u;
}

bool rt_strc_visual_layer_dynamic(uint p_layer_mask) {
	return (p_layer_mask & rt_strc_dynamic_visual_layer_mask()) != 0u;
}

#ifndef RT_STAGE_ANY_HIT

vec3 rt_clamp_luminance(vec3 color, float max_luma) {
	float luma = rt_luminance(color);
	if (luma <= max_luma || luma <= 1e-6) {
		return color;
	}
	return color * (max_luma / luma);
}

vec3 rt_clamp_path_contribution(vec3 contribution, float roughness, float metalness, bool indirect_path, bool secondary_emissive_or_miss) {
	float strength = clamp(get_rt_param(RT_PARAM_RAY_FIREFLY_SUPPRESSION), 0.0, 1.0);
	float max_radiance = get_rt_param(RT_PARAM_RAY_MAX_RADIANCE);
	if (strength <= 0.001 || max_radiance <= 0.0 || get_rt_param(RT_PARAM_VIS_MODE) != 0.0) {
		return sanitize_payload_vec3(contribution);
	}

	float specular_risk = max(1.0 - clamp(roughness, 0.0, 1.0), clamp(metalness, 0.0, 1.0));
	float path_risk = max(specular_risk, indirect_path ? 0.55 : 0.0);
	path_risk = max(path_risk, secondary_emissive_or_miss ? 0.75 : 0.0);
	float limit = max(0.001, max_radiance) * mix(1.05, 0.32, clamp(path_risk, 0.0, 1.0));
	limit *= indirect_path ? 0.72 : 1.0;
	limit *= secondary_emissive_or_miss ? 0.58 : 1.0;
	float luma = rt_luminance(contribution);
	float clamp_active = smoothstep(limit * mix(1.18, 0.82, clamp(path_risk, 0.0, 1.0)), limit * 2.1 + 0.001, luma) * strength;
	return sanitize_payload_vec3(mix(contribution, rt_clamp_luminance(contribution, limit), clamp_active));
}

vec3 rt_clamp_throughput(vec3 throughput, float roughness, float metalness, uint total_bounces) {
	float strength = clamp(get_rt_param(RT_PARAM_RAY_FIREFLY_SUPPRESSION), 0.0, 1.0);
	float max_radiance = get_rt_param(RT_PARAM_RAY_MAX_RADIANCE);
	if (strength <= 0.001 || max_radiance <= 0.0 || get_rt_param(RT_PARAM_VIS_MODE) != 0.0) {
		return sanitize_payload_vec3(throughput);
	}
	float specular_risk = max(1.0 - clamp(roughness, 0.0, 1.0), clamp(metalness, 0.0, 1.0));
	float bounce_risk = smoothstep(0.0, 3.0, float(total_bounces));
	float limit = max(1.0, max_radiance * mix(0.55, 0.20, max(specular_risk, bounce_risk)));
	float luma = rt_luminance(throughput);
	float clamp_active = smoothstep(limit, limit * 2.0 + 0.001, luma) * strength;
	return sanitize_payload_vec3(mix(throughput, rt_clamp_luminance(throughput, limit), clamp_active));
}

/// Project a world-space point to UV through an unjittered VP.
vec2 project_uv(vec3 world_pos, mat4 vp) {
	vec4 clip = vp * vec4(world_pos, 1.0);
	return clip.xy / clip.w * 0.5 + 0.5;
}

bool project_uv_checked(vec3 world_pos, mat4 vp, out vec2 uv) {
	vec4 clip = vp * vec4(world_pos, 1.0);
	if (any(isnan(clip)) || any(isinf(clip)) || clip.w <= 1e-5) {
		uv = vec2(0.0);
		return false;
	}
	uv = clip.xy / clip.w * 0.5 + 0.5;
	return !any(isnan(uv)) && !any(isinf(uv));
}

vec2 rt_visible_size() {
	return max(rt_view_rect.zw, vec2(1.0));
}

vec2 rt_extent() {
	return max(rt_prev_view_rect.zw, vec2(1.0));
}

vec2 rt_current_origin() {
	return rt_view_rect.xy;
}

vec2 rt_previous_origin() {
	return rt_prev_view_rect.xy;
}

vec2 rt_current_visible_uv(ivec2 pixel) {
	return (vec2(pixel) + vec2(0.5) - rt_current_origin()) / rt_visible_size();
}

vec2 rt_visible_to_texture_uv(vec2 visible_uv, vec2 origin) {
	return (visible_uv * rt_visible_size() + origin) / rt_extent();
}

#ifdef DLSS_RR_ENABLED
layout(set = 0, binding = 9, rgba8) uniform image2D dlss_rr_diffuse_albedo;
layout(set = 0, binding = 10, rgba16f) uniform image2D dlss_rr_specular_albedo;
layout(set = 0, binding = 11, rgba8_snorm) uniform image2D dlss_rr_normal_roughness;
layout(set = 0, binding = 12, r16f) uniform image2D dlss_rr_specular_hit_dist;
#endif

// Binding 14 is reserved for GlobalShaderUniformData (declared above).
// Samplers occupy 16-27 (see raytracing_samplers_inc.glsl). Motion transforms
// use binding 32, so RT output side channels continue at 33.
layout(set = 0, binding = 28, rg16f) uniform image2D rt_velocity_image;
layout(set = 0, binding = 29, r8) uniform image2D rt_history_validity_image;
layout(set = 0, binding = 30, rgba8) uniform image2D rt_history_id_image;
layout(set = 0, binding = 31, rg16f) uniform image2D rt_visible_velocity_image;
layout(set = 0, binding = 15, r32f) uniform image2D rt_depth_image;
layout(set = 0, binding = 33, rgba16f) uniform image2D rt_normal_roughness_image;
layout(set = 0, binding = 34, rgba16f) uniform image2D rt_albedo_metalness_image;
layout(set = 0, binding = 35, rgba16f) uniform image2D rt_viewz_hitdist_image;
layout(set = 0, binding = 36, rgba16f) uniform image2D rt_diffuse_radiance_image;
layout(set = 0, binding = 37, rgba16f) uniform image2D rt_specular_radiance_image;
layout(set = 0, binding = 38, rgba16f) uniform image2D rt_specular_guide_image;
layout(set = 0, binding = 60, rgba16f) uniform image2D rt_specular_reflection_direction_image;
layout(set = 0, binding = 63, rgba16f) uniform image2D rt_specular_reprojection_image;
layout(set = 0, binding = 39, rgba16f) uniform image2D rt_signal_direct_light_image;
layout(set = 0, binding = 40, rgba16f) uniform image2D rt_signal_emissive_image;
layout(set = 0, binding = 41, rgba16f) uniform image2D rt_signal_indirect_image;
layout(set = 0, binding = 42, rgba16f) uniform image2D rt_signal_sky_image;
layout(set = 0, binding = 43, rgba16f) uniform image2D rt_signal_confidence_image;
layout(set = 0, binding = 45, rgba16f) uniform image2D rt_source_candidate_image;
layout(set = 0, binding = 46, rgba16f) uniform image2D rt_source_candidate_prev_image;
layout(set = 0, binding = 47, r32ui) uniform uimage2D rt_source_candidate_key_image;
layout(set = 0, binding = 48, r32ui) uniform uimage2D rt_source_candidate_key_prev_image;
layout(set = 0, binding = 49, rgba16f) uniform image2D rt_source_history_image;
layout(set = 0, binding = 50, rgba16f) uniform image2D rt_source_temporal_delta_image;
layout(set = 0, binding = 51, r8) uniform image2D rt_prev_history_validity_image;
layout(set = 0, binding = 52, rgba8) uniform image2D rt_prev_history_id_image;
layout(set = 0, binding = 53, rgba16f) uniform image2D rt_source_normal_roughness_prev_image;
layout(set = 0, binding = 54, rgba16f) uniform image2D rt_source_viewz_hitdist_prev_image;
layout(set = 0, binding = 55, rgba16f) uniform image2D rt_source_rejection_image;
layout(set = 0, binding = 56, rgba16f) uniform image2D rt_source_direct_candidate_image;
layout(set = 0, binding = 57, rgba16f) uniform image2D rt_source_direct_candidate_prev_image;
layout(set = 0, binding = 58, r32ui) uniform uimage2D rt_source_direct_candidate_key_image;
layout(set = 0, binding = 59, r32ui) uniform uimage2D rt_source_direct_candidate_key_prev_image;

#define RT_VIS_MODE_SPECULAR_REFLECTION_DIRECTION 24
#define RT_VIS_MODE_SPECULAR_REFLECTED_HIT_DISTANCE 25
#define RT_VIS_MODE_SPECULAR_REFLECTED_HIT_NORMAL 26

#define RT_SOURCE_CLASS_DIRECT 1u
#define RT_SOURCE_CLASS_EMISSIVE 2u
#define RT_SOURCE_CLASS_INDIRECT 3u
#define RT_SOURCE_CLASS_SKY 4u
#define RT_SOURCE_CLASS_SHIFT 28u

#define RT_SOURCE_REJECT_NONE 0u
#define RT_SOURCE_REJECT_PREV_UV 1u
#define RT_SOURCE_REJECT_CURRENT_HISTORY 2u
#define RT_SOURCE_REJECT_PREVIOUS_HISTORY 3u
#define RT_SOURCE_REJECT_HISTORY_ID 4u
#define RT_SOURCE_REJECT_DEPTH 5u
#define RT_SOURCE_REJECT_NORMAL 6u
#define RT_SOURCE_REJECT_HIT_DISTANCE 7u
#define RT_SOURCE_REJECT_SOURCE_CLASS 8u
#define RT_SOURCE_REJECT_SOURCE_ID 9u
#define RT_SOURCE_REJECT_PDF_WEIGHT 10u
#define RT_SOURCE_REJECT_WEIGHT_RATIO 12u
#define RT_SOURCE_REJECT_LOW_CONFIDENCE 13u

uint rt_source_make_key(uint source_class, uint source_id) {
	return (source_class << RT_SOURCE_CLASS_SHIFT) | (source_id & 0x0FFFFFFFu);
}

uint rt_source_unpack_history_id(vec4 packed_id) {
	uvec4 bytes = uvec4(round(clamp(packed_id, 0.0, 1.0) * 255.0));
	return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}

float rt_source_ratio(float a, float b) {
	return min(a, b) / max(max(a, b), 1e-5);
}

struct RTEmissiveCandidate {
	float object_to_world[12];
	uint geometry_index;
	uint flags;
	float selection_weight;
	float _pad;
};

layout(set = 0, binding = 44, std430) readonly buffer RTEmissiveCandidateBuffer {
	RTEmissiveCandidate rt_emissive_candidates[];
};

struct RTGISTRCProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
};

layout(set = 0, binding = 64, rgba16f) uniform image2D rt_strc_irradiance_image;
layout(set = 0, binding = 65, rgba16f) uniform image2D rt_strc_distance_image;
layout(set = 0, binding = 66, std430) buffer RTGISTRCProbeRayResultBuffer {
	RTGISTRCProbeRayResult rt_strc_probe_ray_results[];
};

bool rt_strc_enabled() {
	return (RT_FLAGS & RT_FLAG_STRC_ENABLED) != 0u && get_rt_param(RT_PARAM_RTGI_STRC_ENABLED) > 0.5 && get_rt_param(RT_PARAM_RTGI_STRC_STRENGTH) > 0.001;
}

vec3 rt_camera_world_origin() {
	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	return inv_view[3].xyz;
}

ivec2 rt_strc_atlas_coord(uint probe_index, uint dir_index, uint grid_size) {
	uint grid = max(grid_size, 1u);
	uint probes_per_cascade = grid * grid * grid;
	uint cascade_count = max(uint(get_rt_param(RT_PARAM_RTGI_STRC_CASCADE_COUNT)), 1u);
	uint cascade = min(probe_index / probes_per_cascade, cascade_count - 1u);
	uint probe = probe_index - cascade * probes_per_cascade;
	uint px = probe % grid;
	uint py = (probe / grid) % grid;
	uint pz = probe / (grid * grid);
	uint dx = dir_index & 7u;
	uint dy = (dir_index >> 3u) & 7u;
	return ivec2(int(px * 8u + dx), int(((cascade * grid + pz) * grid + py) * 8u + dy));
}

vec3 rt_strc_sample_irradiance(vec3 world_pos, vec3 normal, out float confidence) {
	confidence = 0.0;
	if (!rt_strc_enabled() || rt_strc_probe_update_mode()) {
		return vec3(0.0);
	}

	uint grid = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_GRID_SIZE)), 12u, 32u);
	uint cascade_count = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_CASCADE_COUNT)), 1u, 4u);
	float base_spacing = max(get_rt_param(RT_PARAM_RTGI_STRC_BASE_PROBE_SPACING), 0.25);
	vec3 camera_origin = rt_camera_world_origin();
	uint best_probe = 0u;
	float best_weight = 0.0;

	for (uint cascade = 0u; cascade < 4u; cascade++) {
		if (cascade >= cascade_count) {
			break;
		}
		float spacing = base_spacing * exp2(float(cascade));
		vec3 cascade_center = floor(camera_origin / spacing) * spacing;
		vec3 grid_pos = (world_pos - cascade_center) / spacing + vec3(float(grid) * 0.5);
		ivec3 probe_coord = ivec3(floor(grid_pos));
		if (all(greaterThanEqual(probe_coord, ivec3(1))) && all(lessThan(probe_coord, ivec3(int(grid) - 1)))) {
			vec3 cell = abs(fract(grid_pos) - vec3(0.5));
			best_weight = 1.0 - clamp(max(max(cell.x, cell.y), cell.z) * 2.0, 0.0, 1.0);
			uint probe_in_cascade = uint(probe_coord.x) + uint(probe_coord.y) * grid + uint(probe_coord.z) * grid * grid;
			best_probe = cascade * grid * grid * grid + probe_in_cascade;
			break;
		}
	}

	if (best_weight <= 0.0) {
		return vec3(0.0);
	}

	vec2 oct = vec3_to_oct(normal) * 0.5 + 0.5;
	uvec2 texel = uvec2(clamp(floor(oct * 8.0), vec2(0.0), vec2(7.0)));
	uint dir_index = texel.x + texel.y * 8u;
	vec4 sample_value = imageLoad(rt_strc_irradiance_image, rt_strc_atlas_coord(best_probe, dir_index, grid));
	confidence = clamp(sample_value.a * best_weight, 0.0, 1.0);
	return sanitize_payload_vec3(sample_value.rgb);
}

void rt_signal_reset(ivec2 pixel) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	imageStore(rt_signal_direct_light_image, pixel, vec4(0.0));
	imageStore(rt_signal_emissive_image, pixel, vec4(0.0));
	imageStore(rt_signal_indirect_image, pixel, vec4(0.0));
	imageStore(rt_signal_sky_image, pixel, vec4(0.0));
	imageStore(rt_signal_confidence_image, pixel, vec4(0.0, 0.0, 0.0, 1.0));
	imageStore(rt_source_candidate_image, pixel, vec4(0.0));
	imageStore(rt_source_candidate_key_image, pixel, uvec4(0u));
	imageStore(rt_source_direct_candidate_image, pixel, vec4(0.0));
	imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(0u));
	imageStore(rt_source_history_image, pixel, vec4(0.0));
	imageStore(rt_source_temporal_delta_image, pixel, vec4(0.0));
	imageStore(rt_source_rejection_image, pixel, vec4(0.0));
}

#define RT_SIGNAL_ACCUMULATE(signal_image, pixel, value) if (!rt_strc_probe_update_mode()) { imageStore(signal_image, pixel, imageLoad(signal_image, pixel) + (value)); }

bool rt_source_load_reprojected_previous(ivec2 pixel, out ivec2 previous_pixel, out vec4 previous_candidate, out uint previous_key, out uint reject_reason) {
	vec2 extent = rt_extent();
	vec2 current_texture_uv = (vec2(pixel) + vec2(0.5)) / extent;
	vec2 previous_texture_uv = current_texture_uv + imageLoad(rt_velocity_image, pixel).xy;
	if (any(lessThan(previous_texture_uv, vec2(0.0))) || any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		previous_pixel = ivec2(0);
		previous_candidate = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_pixel = ivec2(floor(previous_texture_uv * extent));
	ivec2 extent_i = ivec2(extent);
	if (any(lessThan(previous_pixel, ivec2(0))) || any(greaterThanEqual(previous_pixel, extent_i))) {
		previous_candidate = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_candidate = imageLoad(rt_source_candidate_prev_image, previous_pixel);
	previous_key = imageLoad(rt_source_candidate_key_prev_image, previous_pixel).r;
	reject_reason = RT_SOURCE_REJECT_NONE;
	return true;
}

bool rt_source_load_reprojected_previous_direct(ivec2 pixel, out ivec2 previous_pixel, out vec4 previous_candidate, out uint previous_key, out uint reject_reason) {
	vec2 extent = rt_extent();
	vec2 current_texture_uv = (vec2(pixel) + vec2(0.5)) / extent;
	vec2 previous_texture_uv = current_texture_uv + imageLoad(rt_velocity_image, pixel).xy;
	if (any(lessThan(previous_texture_uv, vec2(0.0))) || any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		previous_pixel = ivec2(0);
		previous_candidate = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_pixel = ivec2(floor(previous_texture_uv * extent));
	ivec2 extent_i = ivec2(extent);
	if (any(lessThan(previous_pixel, ivec2(0))) || any(greaterThanEqual(previous_pixel, extent_i))) {
		previous_candidate = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_candidate = imageLoad(rt_source_direct_candidate_prev_image, previous_pixel);
	previous_key = imageLoad(rt_source_direct_candidate_key_prev_image, previous_pixel).r;
	reject_reason = RT_SOURCE_REJECT_NONE;
	return true;
}

bool rt_source_direct_history_accept(ivec2 pixel, ivec2 previous_pixel, vec4 previous_candidate, uint current_key, uint previous_key, float current_normalized_weight, float current_confidence, out uint reject_reason) {
	if (imageLoad(rt_history_validity_image, pixel).r < 0.5) {
		reject_reason = RT_SOURCE_REJECT_CURRENT_HISTORY;
		return false;
	}
	if (imageLoad(rt_prev_history_validity_image, previous_pixel).r < 0.5) {
		reject_reason = RT_SOURCE_REJECT_PREVIOUS_HISTORY;
		return false;
	}

	uint current_history_id = rt_source_unpack_history_id(imageLoad(rt_history_id_image, pixel));
	uint previous_history_id = rt_source_unpack_history_id(imageLoad(rt_prev_history_id_image, previous_pixel));
	if (current_history_id == 0u || previous_history_id == 0u || current_history_id != previous_history_id) {
		reject_reason = RT_SOURCE_REJECT_HISTORY_ID;
		return false;
	}

	vec4 current_viewz_hitdist = imageLoad(rt_viewz_hitdist_image, pixel);
	vec4 previous_viewz_hitdist = imageLoad(rt_source_viewz_hitdist_prev_image, previous_pixel);
	float current_depth = current_viewz_hitdist.x;
	float previous_depth = previous_viewz_hitdist.x;
	float depth_limit = max(0.03, max(abs(current_depth), abs(previous_depth)) * 0.02);
	if (abs(current_depth - previous_depth) > depth_limit) {
		reject_reason = RT_SOURCE_REJECT_DEPTH;
		return false;
	}

	vec3 current_normal = normalize(imageLoad(rt_normal_roughness_image, pixel).xyz * 2.0 - 1.0);
	vec3 previous_normal = normalize(imageLoad(rt_source_normal_roughness_prev_image, previous_pixel).xyz * 2.0 - 1.0);
	if (dot(current_normal, previous_normal) < 0.94) {
		reject_reason = RT_SOURCE_REJECT_NORMAL;
		return false;
	}

	float current_hitdist = current_viewz_hitdist.y;
	float previous_hitdist = previous_viewz_hitdist.y;
	float hitdist_limit = max(0.10, max(abs(current_hitdist), abs(previous_hitdist)) * 0.15);
	if (abs(current_hitdist - previous_hitdist) > hitdist_limit) {
		reject_reason = RT_SOURCE_REJECT_HIT_DISTANCE;
		return false;
	}

	if ((current_key >> RT_SOURCE_CLASS_SHIFT) != RT_SOURCE_CLASS_DIRECT || (previous_key >> RT_SOURCE_CLASS_SHIFT) != RT_SOURCE_CLASS_DIRECT) {
		reject_reason = RT_SOURCE_REJECT_SOURCE_CLASS;
		return false;
	}
	if (current_key == 0u || previous_key == 0u || current_key != previous_key) {
		reject_reason = RT_SOURCE_REJECT_SOURCE_ID;
		return false;
	}
	if (previous_candidate.y < 0.75 || current_confidence < 0.75) {
		reject_reason = RT_SOURCE_REJECT_LOW_CONFIDENCE;
		return false;
	}
	if (any(isnan(previous_candidate)) || any(isinf(previous_candidate)) || isnan(current_normalized_weight) || isinf(current_normalized_weight) || previous_candidate.z <= 0.0 || current_normalized_weight <= 0.0) {
		reject_reason = RT_SOURCE_REJECT_PDF_WEIGHT;
		return false;
	}
	if (rt_source_ratio(previous_candidate.z, current_normalized_weight) < 0.25) {
		reject_reason = RT_SOURCE_REJECT_WEIGHT_RATIO;
		return false;
	}

	reject_reason = RT_SOURCE_REJECT_NONE;
	return true;
}

void rt_source_direct_candidate_record(ivec2 pixel, uint source_key, float confidence, float normalized_weight, vec3 contribution, bool stochastic_candidate_mode) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	vec3 value = sanitize_payload_vec3(contribution);
	bool valid_source = source_key != 0u && (source_key >> RT_SOURCE_CLASS_SHIFT) == RT_SOURCE_CLASS_DIRECT;
	float stored_confidence = stochastic_candidate_mode ? confidence : min(confidence, 0.5);
	float contribution_luma = valid_source ? rt_luminance(value) : 0.0;
	if (!valid_source) {
		imageStore(rt_source_direct_candidate_image, pixel, vec4(0.0));
		imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(0u));
		return;
	}

	imageStore(rt_source_direct_candidate_image, pixel, vec4(
			0.25,
			clamp(stored_confidence, 0.0, 1.0),
			clamp(normalized_weight, 0.0, 1.0),
			clamp(contribution_luma, 0.0, 1.0)));
	imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(source_key, 0u, 0u, 0u));
}

void rt_source_candidate_record(ivec2 pixel, float source_class, float confidence, float normalized_weight, vec3 contribution, float downweight, uint source_key) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	vec3 value = sanitize_payload_vec3(contribution);
	float contribution_luma = rt_luminance(value);
	vec4 previous = imageLoad(rt_source_candidate_image, pixel);
	if (contribution_luma >= previous.a) {
		uint current_class_key = source_key >> RT_SOURCE_CLASS_SHIFT;
		uint previous_key = current_class_key == RT_SOURCE_CLASS_DIRECT ? 0u : imageLoad(rt_source_candidate_key_prev_image, pixel).r;
		vec4 previous_candidate = current_class_key == RT_SOURCE_CLASS_DIRECT ? vec4(0.0) : imageLoad(rt_source_candidate_prev_image, pixel);
		uint reject_reason = RT_SOURCE_REJECT_NONE;
		bool direct_reprojected = false;
		if (current_class_key == RT_SOURCE_CLASS_DIRECT) {
			ivec2 previous_pixel = ivec2(0);
			direct_reprojected = rt_source_load_reprojected_previous_direct(pixel, previous_pixel, previous_candidate, previous_key, reject_reason);
			if (direct_reprojected) {
				uint accept_reason = RT_SOURCE_REJECT_NONE;
				if (!rt_source_direct_history_accept(pixel, previous_pixel, previous_candidate, source_key, previous_key, normalized_weight, confidence, accept_reason)) {
					reject_reason = accept_reason;
				}
			}
		}
		bool current_has_key = source_key != 0u;
		bool previous_has_key = previous_key != 0u && previous_candidate.x > 0.0;
		bool eligible = current_has_key && previous_has_key;
		uint previous_class_key = previous_key >> RT_SOURCE_CLASS_SHIFT;
		bool class_agrees = eligible && current_class_key == previous_class_key;
		bool id_agrees = class_agrees && source_key == previous_key;
		bool direct_accepted = current_class_key == RT_SOURCE_CLASS_DIRECT && id_agrees && reject_reason == RT_SOURCE_REJECT_NONE && direct_reprojected;
		uint direct_reason = direct_accepted ? RT_SOURCE_REJECT_NONE : (reject_reason != RT_SOURCE_REJECT_NONE ? reject_reason : RT_SOURCE_REJECT_SOURCE_ID);
		float temporal_delta = class_agrees ? abs(contribution_luma - previous_candidate.a) : 0.0;
		imageStore(rt_source_candidate_image, pixel, vec4(
				clamp(source_class, 0.0, 1.0),
				clamp(confidence, 0.0, 1.0),
				clamp(normalized_weight, 0.0, 1.0),
				clamp(contribution_luma, 0.0, 1.0)));
		imageStore(rt_source_candidate_key_image, pixel, uvec4(source_key, 0u, 0u, 0u));
		imageStore(rt_source_history_image, pixel, vec4(
				clamp(source_class, 0.0, 1.0),
				eligible ? 1.0 : 0.0,
				class_agrees ? 1.0 : 0.0,
				1.0));
		imageStore(rt_source_temporal_delta_image, pixel, vec4(
				eligible ? clamp(source_class, 0.0, 1.0) : 0.0,
				id_agrees ? 1.0 : 0.0,
				clamp(temporal_delta, 0.0, 1.0),
				1.0));
		if (current_class_key == RT_SOURCE_CLASS_DIRECT) {
			imageStore(rt_source_rejection_image, pixel, vec4(
					clamp(source_class, 0.0, 1.0),
					eligible ? 1.0 : 0.0,
					float(direct_reason) / 15.0,
					1.0));
		} else {
			imageStore(rt_source_rejection_image, pixel, vec4(0.0));
		}
	}
	if (downweight > 0.001) {
		vec4 confidence_signal = imageLoad(rt_signal_confidence_image, pixel);
		confidence_signal.g = max(confidence_signal.g, clamp(downweight, 0.0, 1.0));
		imageStore(rt_signal_confidence_image, pixel, confidence_signal);
	}
}

float rt_signal_clamp_delta(vec3 raw_value, vec3 clamped_value) {
	return max(0.0, rt_luminance(sanitize_payload_vec3(raw_value)) - rt_luminance(sanitize_payload_vec3(clamped_value)));
}

void rt_signal_add_direct(ivec2 pixel, vec3 diffuse_value, vec3 specular_value, float clamp_delta) {
	vec3 total = sanitize_payload_vec3(diffuse_value + specular_value);
	float total_luma = rt_luminance(total);
	float specular_fraction = total_luma > 1e-5 ? rt_luminance(specular_value) / total_luma : 0.0;
	RT_SIGNAL_ACCUMULATE(rt_signal_direct_light_image, pixel, vec4(total, clamp(specular_fraction, 0.0, 1.0)));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
}

void rt_signal_add_emissive(ivec2 pixel, vec3 contribution, bool secondary_emissive, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(contribution);
	RT_SIGNAL_ACCUMULATE(rt_signal_emissive_image, pixel, vec4(value, secondary_emissive ? rt_luminance(value) : 0.0));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
}

void rt_signal_add_explicit_emissive(ivec2 pixel, vec3 contribution, float pdf, float selected_weight, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(contribution);
	float risk = clamp(rt_luminance(value) / max(selected_weight, 1e-4), 0.0, 1.0);
	RT_SIGNAL_ACCUMULATE(rt_signal_emissive_image, pixel, vec4(value, rt_luminance(value)));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, max(risk, clamp(pdf * 32.0, 0.0, 1.0)), 0.0, 0.0));
}

void rt_signal_add_indirect(ivec2 pixel, vec3 throughput, uint total_bounces, int brdf_type, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(throughput);
	float encoded_type = brdf_type == 2 ? 2.0 : 1.0;
	RT_SIGNAL_ACCUMULATE(rt_signal_indirect_image, pixel, vec4(value, encoded_type + float(min(total_bounces, 30u)) / 32.0));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
	rt_source_candidate_record(pixel, 0.75, 1.0, clamp(rt_luminance(value) / 4.0, 0.0, 1.0), value, 0.0, 0u);
}

void rt_signal_add_sky(ivec2 pixel, vec3 contribution, bool secondary_miss, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(contribution);
	RT_SIGNAL_ACCUMULATE(rt_signal_sky_image, pixel, vec4(value, secondary_miss ? rt_luminance(value) : 0.0));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
	rt_source_candidate_record(pixel, 1.0, 1.0, secondary_miss ? 0.5 : 1.0, value, 0.0, rt_source_make_key(RT_SOURCE_CLASS_SKY, secondary_miss ? 2u : 1u));
}

void rt_signal_set_primary_confidence(ivec2 pixel, float specular_risk, float material_id, float validity) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	vec4 confidence = imageLoad(rt_signal_confidence_image, pixel);
	confidence.b = material_id;
	confidence.a = validity;
	confidence.g = max(confidence.g, clamp(specular_risk, 0.0, 1.0));
	imageStore(rt_signal_confidence_image, pixel, confidence);
}

void rt_store_primary_velocity(ivec2 pixel, vec2 curr_visible_uv, vec2 prev_visible_uv) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	vec2 curr_texture_uv = rt_visible_to_texture_uv(curr_visible_uv, rt_current_origin());
	vec2 prev_texture_uv = rt_visible_to_texture_uv(prev_visible_uv, rt_previous_origin());
	imageStore(rt_velocity_image, pixel, vec4(prev_texture_uv - curr_texture_uv, 0.0, 0.0));

	if (uint(get_rt_param(RT_PARAM_MODE)) == RT_MODE_PATH_TRACED) {
		ivec2 visible_pixel = pixel - ivec2(round(rt_current_origin()));
		ivec2 visible_size_i = ivec2(round(rt_visible_size()));
		if (all(greaterThanEqual(visible_pixel, ivec2(0))) && all(lessThan(visible_pixel, visible_size_i))) {
			imageStore(rt_visible_velocity_image, visible_pixel, vec4(prev_visible_uv - curr_visible_uv, 0.0, 0.0));
		}
	}
}

void rt_store_invalid_primary_velocity(ivec2 pixel) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	imageStore(rt_velocity_image, pixel, vec4(0.0));

	if (uint(get_rt_param(RT_PARAM_MODE)) == RT_MODE_PATH_TRACED) {
		ivec2 visible_pixel = pixel - ivec2(round(rt_current_origin()));
		ivec2 visible_size_i = ivec2(round(rt_visible_size()));
		if (all(greaterThanEqual(visible_pixel, ivec2(0))) && all(lessThan(visible_pixel, visible_size_i))) {
			imageStore(rt_visible_velocity_image, visible_pixel, vec4(0.0));
		}
	}
}

#endif // !RT_STAGE_ANY_HIT

// Shared hitAttributeEXT layout for all hit-group stages.
// Vulkan requires every shader in a hit group to agree on this layout.
#if defined(RT_STAGE_CLOSEST_HIT) || defined(RT_STAGE_ANY_HIT) || defined(RT_STAGE_INTERSECTION)
struct HitAttribs {
	vec2 bary_or_uv;
#ifdef ENABLE_INTERSECTION_SHADERS
	uint packed_normal; // xyz: snorm8 normal,  w: delta.x FP16 low byte.
	uint packed_tangent; // xyz: snorm8 tangent, w: delta.x FP16 high byte.
	uint prev_pos_delta_yz; // packHalf2x16(delta.y, delta.z).
#endif
};
hitAttributeEXT HitAttribs hit_attribs;
#endif
