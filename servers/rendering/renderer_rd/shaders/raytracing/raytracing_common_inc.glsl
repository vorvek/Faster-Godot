// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant (bits 0-20: flags, 21-28: samples, 29-31: bounces).
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define RT_FLAG_FOG_ENABLED (1u << 2)
#define RT_FLAG_STRC_ENABLED (1u << 4)
#define RT_FLAG_STRC_PROBE_UPDATE (1u << 5)
#define RT_FLAG_STRC_INTERNAL_FALLBACK (1u << 6)
#define RT_FLAG_WRC_PROBE_UPDATE (1u << 7)
#define RT_FLAG_SPG_GATHER (1u << 8)

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
	// 12 vec4s == RT_PARAM_SHADER_FLOAT_COUNT (48) floats. Grown from 10 to 12 to make
	// room for the Screen Probe Gather (SPG) params (indices 39..44); see the matching
	// RT_PARAM_SHADER_FLOAT_COUNT in scene_shader_raytracing.h + the rt_ubo static_assert.
	vec4 rt_params[12];
	mat4 prev_vp_unjittered;
mat4 curr_vp_unjittered;
mat4 inv_projection_unjittered;
vec4 rt_view_rect;
vec4 rt_prev_view_rect;
vec4 rt_jitter;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
}

bool rt_strc_probe_update_mode() {
	return (RT_FLAGS & RT_FLAG_STRC_PROBE_UPDATE) != 0u;
}

bool rt_wrc_probe_update_mode() {
	return (RT_FLAGS & RT_FLAG_WRC_PROBE_UPDATE) != 0u;
}

bool rt_spg_gather_mode() {
	return (RT_FLAGS & RT_FLAG_SPG_GATHER) != 0u;
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
	float soft_luma = max_luma + (luma - max_luma) / (1.0 + (luma - max_luma) / max_luma);
	return color * (soft_luma / luma);
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
	float limit = max(0.001, max_radiance) * mix(0.85, 0.15, clamp(path_risk, 0.0, 1.0));
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

vec2 rt_reconstruction_jitter_pixels() {
	return rt_jitter.xy;
}

vec2 rt_raygen_jitter_pixels() {
	return rt_jitter.zw;
}

vec2 rt_current_visible_uv(ivec2 pixel) {
	return (vec2(pixel) + vec2(0.5) - rt_current_origin() + rt_raygen_jitter_pixels()) / rt_visible_size();
}

vec2 rt_visible_to_texture_uv(vec2 visible_uv, vec2 origin) {
	return (visible_uv * rt_visible_size() + origin) / rt_extent();
}

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
layout(set = 0, binding = 77, rgba8) uniform image2D rt_receiver_surface_id_image;
layout(set = 0, binding = 78, rgba16f) readonly uniform image2D rtgi_diffuse_cache_radiance_image;
layout(set = 0, binding = 79, rgba16f) readonly uniform image2D rtgi_diffuse_cache_meta_image;
layout(set = 0, binding = 80, rgba16f) readonly uniform image2D rtgi_diffuse_cache_stats_image;
layout(set = 0, binding = 81, rgba8) readonly uniform image2D rtgi_diffuse_cache_history_id_image;
layout(set = 0, binding = 82, rgba16f) uniform image2D rt_primary_diffuse_direction_image;
layout(set = 0, binding = 83, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_radiance_image;
layout(set = 0, binding = 84, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_meta_image;
layout(set = 0, binding = 85, rgba8) readonly uniform image2D rtgi_diffuse_cache_spg_history_id_image;
layout(set = 0, binding = 86, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_stats_image;
layout(set = 0, binding = 87, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_visibility_image;
layout(set = 0, binding = 88, rgba8) readonly uniform image2D rtgi_diffuse_cache_spg_refinement_mask_image;
layout(set = 0, binding = 89, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_refined_radiance_image;
layout(set = 0, binding = 90, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_refined_meta_image;
layout(set = 0, binding = 91, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_refined_stats_image;
layout(set = 0, binding = 92, rgba16f) readonly uniform image2D rtgi_diffuse_cache_spg_refined_visibility_image;
layout(set = 0, binding = 93, rgba8) readonly uniform image2D rtgi_diffuse_cache_spg_refined_history_id_image;
layout(set = 0, binding = 96, r32ui) uniform uimage2D rt_surface_cache_key_image;
layout(set = 0, binding = 97, rgba16f) readonly uniform image2D rtgi_diffuse_cache_surface_radiance_image;
layout(set = 0, binding = 98, rgba8) readonly uniform image2D rtgi_diffuse_cache_surface_meta_image;
layout(set = 0, binding = 99, rgba16f) readonly uniform image2D rtgi_diffuse_cache_surface_stats_image;
layout(set = 0, binding = 100, rgba8) readonly uniform image2D rtgi_diffuse_cache_surface_history_id_image;
layout(set = 0, binding = 101, rgba8) uniform image2D rt_surface_cache_diagnostic_image;
layout(set = 0, binding = 102, rgba8) uniform image2D rt_secondary_cache_surface_image;
layout(set = 0, binding = 103, r32ui) uniform uimage2D rt_surface_cache_feedback_key_image;
layout(set = 0, binding = 104, rgba16f) uniform image2D rt_surface_cache_feedback_radiance_image;
layout(set = 0, binding = 105, rgba8) uniform image2D rt_surface_cache_feedback_meta_image;
layout(set = 0, binding = 106, rgba8) uniform image2D rt_surface_cache_feedback_stats_image;
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
layout(set = 0, binding = 94, rgba8) uniform image2D rt_secondary_cache_source_image;
layout(set = 0, binding = 95, rgba8) uniform image2D rt_secondary_cache_rejection_image;
layout(set = 0, binding = 56, rgba16f) uniform image2D rt_source_direct_candidate_image;
layout(set = 0, binding = 57, rgba16f) uniform image2D rt_source_direct_candidate_prev_image;
layout(set = 0, binding = 58, r32ui) uniform uimage2D rt_source_direct_candidate_key_image;
layout(set = 0, binding = 59, r32ui) uniform uimage2D rt_source_direct_candidate_key_prev_image;
layout(set = 0, binding = 67, rgba16f) uniform image2D rt_source_direct_reservoir_image;
layout(set = 0, binding = 68, rgba16f) uniform image2D rt_source_direct_reservoir_prev_image;
layout(set = 0, binding = 69, rgba16f) uniform image2D rt_source_direct_reservoir_lighting_image;
layout(set = 0, binding = 70, rgba16f) uniform image2D rt_source_direct_reservoir_lighting_prev_image;
layout(set = 0, binding = 71) uniform texture2D raster_depth_texture;
layout(set = 0, binding = 72) uniform texture2D raster_normal_roughness_texture;
layout(set = 0, binding = 73) uniform texture2D raster_color_texture;
layout(set = 0, binding = 74) uniform sampler raster_nearest_sampler;

#define RT_VIS_MODE_SPECULAR_REFLECTION_DIRECTION 24
#define RT_VIS_MODE_SPECULAR_REFLECTED_HIT_DISTANCE 25
#define RT_VIS_MODE_SPECULAR_REFLECTED_HIT_NORMAL 26

#define RTGI_SURFACE_KEY_REASON_VALID 0u
#define RTGI_SURFACE_KEY_REASON_EMPTY 1u
#define RTGI_SURFACE_KEY_REASON_RASTER 2u
#define RTGI_SURFACE_KEY_REASON_HISTORY_INVALID 3u
#define RTGI_SURFACE_KEY_REASON_DEFORMED 4u
#define RTGI_SURFACE_KEY_REASON_PROCEDURAL 5u
#define RTGI_SURFACE_KEY_REASON_ZERO_KEY 6u

#define RTGI_SURFACE_CACHE_QUERY_NONE 0u
#define RTGI_SURFACE_CACHE_QUERY_ACCEPTED 1u
#define RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE 2u
#define RTGI_SURFACE_CACHE_QUERY_INELIGIBLE 3u
#define RTGI_SURFACE_CACHE_QUERY_NO_KEY 4u
#define RTGI_SURFACE_CACHE_QUERY_DYNAMIC_INELIGIBLE 5u
#define RTGI_SURFACE_CACHE_QUERY_NO_PAGE 6u
#define RTGI_SURFACE_CACHE_QUERY_ID_MISMATCH 7u
#define RTGI_SURFACE_CACHE_QUERY_LOW_CONFIDENCE 8u
#define RTGI_SURFACE_CACHE_QUERY_LOW_SUPPORT 9u
#define RTGI_SURFACE_CACHE_QUERY_LOW_VARIANCE 10u
#define RTGI_SURFACE_CACHE_QUERY_STALE 11u
#define RTGI_SURFACE_CACHE_QUERY_NORMAL_MISMATCH 12u
#define RTGI_SURFACE_CACHE_QUERY_WEAK_RADIANCE 13u
#define RTGI_SURFACE_CACHE_QUERY_WEAK_QUALITY 14u

#define RT_SOURCE_CLASS_DIRECT 1u
#define RT_SOURCE_CLASS_EMISSIVE 2u
#define RT_SOURCE_CLASS_INDIRECT 3u
#define RT_SOURCE_CLASS_SKY 4u

vec4 rt_pack_u32_rgba8(uint id) {
	uvec4 bytes = uvec4(id & 0xFFu, (id >> 8u) & 0xFFu, (id >> 16u) & 0xFFu, (id >> 24u) & 0xFFu);
	return vec4(bytes) * (1.0 / 255.0);
}

uint rt_unpack_u32_rgba8(vec4 packed_id) {
	uvec4 bytes = uvec4(round(clamp(packed_id, 0.0, 1.0) * 255.0));
	return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}

uint rt_mix_u32(uint id, uint value) {
	uint h = id ^ (value + 0x9e3779b9u + (id << 6u) + (id >> 2u));
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h == 0u ? 1u : h;
}

void rtgi_store_surface_key_diagnostic(ivec2 pixel, uint key, uint reason, float static_weight) {
	float coverage = key != 0u ? 1.0 : 0.0;
	imageStore(rt_surface_cache_diagnostic_image, pixel, vec4(coverage, clamp(float(reason) / float(RTGI_SURFACE_KEY_REASON_ZERO_KEY), 0.0, 1.0), clamp(static_weight, 0.0, 1.0), 1.0));
}

vec3 rtgi_demodulate_surface_feedback(vec3 diffuse_lighting, vec3 albedo_proxy, float metalness) {
	float albedo_luma = rt_luminance(albedo_proxy);
	float weight = (1.0 - clamp(metalness, 0.0, 1.0)) * smoothstep(0.015, 0.12, albedo_luma);
	vec3 modulation = mix(vec3(1.0), clamp(albedo_proxy, vec3(0.04), vec3(1.0)), weight);
	return sanitize_payload_vec3(diffuse_lighting / modulation);
}

#define RTGI_SURFACE_SOURCE_NONE 0u
#define RTGI_SURFACE_SOURCE_RECEIVER 1u
#define RTGI_SURFACE_SOURCE_BASE_SPG 2u
#define RTGI_SURFACE_SOURCE_REFINED_SPG 3u
#define RTGI_SURFACE_SOURCE_VISIBLE_CURRENT 4u
#define RTGI_SURFACE_SOURCE_DIRECT 5u
#define RTGI_SURFACE_SOURCE_EMISSIVE 6u
#define RTGI_SURFACE_SOURCE_SKY 7u
#define RTGI_SURFACE_SOURCE_STRC 8u
#define RTGI_SURFACE_SOURCE_MIXED 9u
#define RTGI_SURFACE_SOURCE_MAX 9u

#define RTGI_SURFACE_FEEDBACK_SOURCE_DIRECT_BIT 1u
#define RTGI_SURFACE_FEEDBACK_SOURCE_EMISSIVE_BIT 2u
#define RTGI_SURFACE_FEEDBACK_SOURCE_SKY_BIT 4u
#define RTGI_SURFACE_FEEDBACK_SOURCE_STRC_BIT 8u

float rtgi_surface_source_bucket(uint source_class) {
	return clamp(float(source_class) / float(RTGI_SURFACE_SOURCE_MAX), 0.0, 1.0);
}

uint rtgi_surface_source_from_bucket(float bucket) {
	return uint(clamp(floor(clamp(bucket, 0.0, 1.0) * float(RTGI_SURFACE_SOURCE_MAX) + 0.5), float(RTGI_SURFACE_SOURCE_NONE), float(RTGI_SURFACE_SOURCE_MAX)));
}

float rtgi_surface_source_quality(uint source_class) {
	if (source_class == RTGI_SURFACE_SOURCE_RECEIVER) {
		return 0.82;
	}
	if (source_class == RTGI_SURFACE_SOURCE_BASE_SPG) {
		return 0.62;
	}
	if (source_class == RTGI_SURFACE_SOURCE_REFINED_SPG) {
		return 0.74;
	}
	if (source_class == RTGI_SURFACE_SOURCE_VISIBLE_CURRENT) {
		return 0.92;
	}
	if (source_class == RTGI_SURFACE_SOURCE_DIRECT) {
		return 0.88;
	}
	if (source_class == RTGI_SURFACE_SOURCE_EMISSIVE) {
		return 0.78;
	}
	if (source_class == RTGI_SURFACE_SOURCE_SKY) {
		return 0.70;
	}
	if (source_class == RTGI_SURFACE_SOURCE_STRC) {
		return 0.42;
	}
	if (source_class == RTGI_SURFACE_SOURCE_MIXED) {
		return 0.70;
	}
	return 0.35;
}

uint rtgi_surface_source_from_feedback_mask(uint source_mask) {
	uint source_count = 0u;
	source_count += (source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_DIRECT_BIT) != 0u ? 1u : 0u;
	source_count += (source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_EMISSIVE_BIT) != 0u ? 1u : 0u;
	source_count += (source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_SKY_BIT) != 0u ? 1u : 0u;
	source_count += (source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_STRC_BIT) != 0u ? 1u : 0u;
	if (source_count > 1u) {
		return RTGI_SURFACE_SOURCE_MIXED;
	}
	if ((source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_DIRECT_BIT) != 0u) {
		return RTGI_SURFACE_SOURCE_DIRECT;
	}
	if ((source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_EMISSIVE_BIT) != 0u) {
		return RTGI_SURFACE_SOURCE_EMISSIVE;
	}
	if ((source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_SKY_BIT) != 0u) {
		return RTGI_SURFACE_SOURCE_SKY;
	}
	if ((source_mask & RTGI_SURFACE_FEEDBACK_SOURCE_STRC_BIT) != 0u) {
		return RTGI_SURFACE_SOURCE_STRC;
	}
	return RTGI_SURFACE_SOURCE_NONE;
}

uint rtgi_surface_source_from_strc_mask(uint source_mask) {
	uint source_count = 0u;
	source_count += (source_mask & STRC_SOURCE_MASK_DIRECT) != 0u ? 1u : 0u;
	source_count += (source_mask & STRC_SOURCE_MASK_EMISSIVE) != 0u ? 1u : 0u;
	source_count += (source_mask & STRC_SOURCE_MASK_SKY) != 0u ? 1u : 0u;
	if (source_count > 1u) {
		return RTGI_SURFACE_SOURCE_MIXED;
	}
	if ((source_mask & STRC_SOURCE_MASK_DIRECT) != 0u) {
		return RTGI_SURFACE_SOURCE_DIRECT;
	}
	if ((source_mask & STRC_SOURCE_MASK_EMISSIVE) != 0u) {
		return RTGI_SURFACE_SOURCE_EMISSIVE;
	}
	if ((source_mask & STRC_SOURCE_MASK_SKY) != 0u) {
		return RTGI_SURFACE_SOURCE_SKY;
	}
	if ((source_mask & STRC_SOURCE_MASK_INDIRECT) != 0u) {
		return RTGI_SURFACE_SOURCE_STRC;
	}
	return RTGI_SURFACE_SOURCE_NONE;
}

uint rtgi_surface_feedback_mask_from_strc_mask(uint source_mask) {
	uint feedback_mask = 0u;
	if ((source_mask & STRC_SOURCE_MASK_DIRECT) != 0u) {
		feedback_mask |= RTGI_SURFACE_FEEDBACK_SOURCE_DIRECT_BIT;
	}
	if ((source_mask & STRC_SOURCE_MASK_EMISSIVE) != 0u) {
		feedback_mask |= RTGI_SURFACE_FEEDBACK_SOURCE_EMISSIVE_BIT;
	}
	if ((source_mask & STRC_SOURCE_MASK_SKY) != 0u) {
		feedback_mask |= RTGI_SURFACE_FEEDBACK_SOURCE_SKY_BIT;
	}
	if (feedback_mask == 0u && (source_mask & STRC_SOURCE_MASK_INDIRECT) != 0u) {
		feedback_mask = RTGI_SURFACE_FEEDBACK_SOURCE_STRC_BIT;
	}
	return feedback_mask;
}

void rtgi_surface_cache_feedback_reset(ivec2 pixel) {
	imageStore(rt_surface_cache_feedback_key_image, pixel, uvec4(0u));
	imageStore(rt_surface_cache_feedback_radiance_image, pixel, vec4(0.0));
	imageStore(rt_surface_cache_feedback_meta_image, pixel, vec4(0.5, 0.5, 1.0, 0.0));
	imageStore(rt_surface_cache_feedback_stats_image, pixel, vec4(0.0));
}

void rtgi_surface_cache_feedback_reject(ivec2 pixel, float reason) {
	if (rt_strc_probe_update_mode()) {
		return;
	}

	imageStore(rt_surface_cache_feedback_key_image, pixel, uvec4(0u));
	imageStore(rt_surface_cache_feedback_radiance_image, pixel, vec4(0.0));
	imageStore(rt_surface_cache_feedback_meta_image, pixel, vec4(0.5, 0.5, 1.0, 0.0));
	imageStore(rt_surface_cache_feedback_stats_image, pixel, vec4(0.0, 0.0, 0.0, clamp(reason / 2.0, 0.0, 1.0)));
}

void rtgi_surface_cache_feedback_record_source(ivec2 pixel, uint surface_key, vec3 normal, float roughness, vec3 demodulated_lighting, float confidence, float support, uint source_class) {
	if (rt_strc_probe_update_mode() || surface_key == 0u) {
		return;
	}

	if (source_class == RTGI_SURFACE_SOURCE_NONE) {
		return;
	}

	vec3 lighting = sanitize_payload_vec3(demodulated_lighting);
	lighting = rt_clamp_path_contribution(lighting, roughness, 0.0, true, true);
	float luma = rt_luminance(lighting);
	float radiance_weight = smoothstep(0.00035, 0.0055, luma);
	float source_quality = clamp(confidence * support * radiance_weight, 0.0, 1.0);
	if (source_quality <= 0.010) {
		return;
	}

	imageStore(rt_surface_cache_feedback_key_image, pixel, uvec4(surface_key, 0u, 0u, 0u));
	imageStore(rt_surface_cache_feedback_radiance_image, pixel, vec4(lighting, clamp(confidence * 0.72 + source_quality * 0.28, 0.0, 1.0)));
	imageStore(rt_surface_cache_feedback_meta_image, pixel, vec4(normalize(normal) * 0.5 + 0.5, clamp(roughness, 0.0, 1.0)));
	imageStore(rt_surface_cache_feedback_stats_image, pixel, vec4(source_quality, support, radiance_weight, rtgi_surface_source_bucket(source_class)));
}

void rtgi_surface_cache_feedback_record(ivec2 pixel, uint surface_key, vec3 normal, float roughness, vec3 demodulated_lighting, float confidence, float support, uint source_mask) {
	rtgi_surface_cache_feedback_record_source(pixel, surface_key, normal, roughness, demodulated_lighting, confidence, support, rtgi_surface_source_from_feedback_mask(source_mask));
}

uint rt_receiver_surface_id(vec3 world_pos, vec3 normal, float roughness, vec3 albedo_proxy) {
	uint h = 0x73757266u;
	ivec3 quantized_position = ivec3(floor(world_pos * 2.0 + vec3(0.5)));
	uvec3 quantized_normal = uvec3(clamp(floor(normalize(normal) * 63.0 + vec3(64.0)), vec3(0.0), vec3(127.0)));
	uvec3 quantized_albedo = uvec3(clamp(floor(clamp(albedo_proxy, vec3(0.0), vec3(1.0)) * 15.0 + vec3(0.5)), vec3(0.0), vec3(15.0)));
	uint quantized_roughness = uint(clamp(floor(clamp(roughness, 0.0, 1.0) * 31.0 + 0.5), 0.0, 31.0));
	uint normal_key = quantized_normal.x | (quantized_normal.y << 7u) | (quantized_normal.z << 14u);
	uint material_key = quantized_albedo.x | (quantized_albedo.y << 4u) | (quantized_albedo.z << 8u) | (quantized_roughness << 12u);
	h = rt_mix_u32(h, uint(quantized_position.x));
	h = rt_mix_u32(h, uint(quantized_position.y));
	h = rt_mix_u32(h, uint(quantized_position.z));
	h = rt_mix_u32(h, normal_key);
	h = rt_mix_u32(h, material_key);
	return h;
}

#define RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT 4
#define RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING 8
#define RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION 4
#define RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS 2
#define RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION 2
#define RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE (RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS * RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION)
#define RTGI_DIFFUSE_CACHE_SURFACE_ASSOCIATIVITY 4

bool rtgi_diffuse_cache_id_matches(vec4 a, vec4 b) {
	return max(max(abs(a.x - b.x), abs(a.y - b.y)), max(abs(a.z - b.z), abs(a.w - b.w))) < (0.5 / 255.0);
}

bool rtgi_diffuse_cache_id_valid(vec4 id) {
	return dot(id, id) > 1e-6;
}

float rtgi_diffuse_cache_relative_delta(float a, float b, float floor_value) {
	return abs(a - b) / max(max(abs(a), abs(b)), floor_value);
}

float rtgi_diffuse_cache_variance_ratio(vec4 stats_sample) {
	float mean_value = max(stats_sample.y, 0.0);
	float second_value = max(stats_sample.z, 0.0);
	float variance_value = max(second_value - mean_value * mean_value, 0.0);
	return sqrt(variance_value) / max(mean_value, 0.08);
}

float rtgi_diffuse_cache_spg_visibility_quality(vec4 visibility_sample) {
	if (visibility_sample.x >= 60000.0 || visibility_sample.w <= 0.02) {
		return 1.0;
	}
	return mix(0.50, 1.0, clamp(visibility_sample.y * visibility_sample.w, 0.0, 1.0));
}

ivec2 rtgi_diffuse_cache_slot_pos(ivec2 cache_pos, int slot) {
	return ivec2(cache_pos.x * RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT + clamp(slot, 0, RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT - 1), cache_pos.y);
}

ivec2 rtgi_diffuse_cache_spg_atlas_pos(ivec2 probe_pos, ivec2 dir_tile) {
	return probe_pos * RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION + clamp(dir_tile, ivec2(0), ivec2(RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION - 1));
}

ivec2 rtgi_diffuse_cache_spg_refined_atlas_pos(ivec2 probe_pos, ivec2 sub_tile, ivec2 dir_tile) {
	ivec2 local = clamp(sub_tile, ivec2(0), ivec2(RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS - 1)) * RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION + clamp(dir_tile, ivec2(0), ivec2(RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION - 1));
	return probe_pos * RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE + local;
}

ivec2 rtgi_diffuse_cache_surface_pos_from_key_variant(uint key, ivec2 surface_size, int variant) {
	uint h = rt_mix_u32(0x73726363u ^ uint(variant) * 0x9e3779b9u, key);
	uint texel_count = uint(max(surface_size.x * surface_size.y, 1));
	uint index = h % texel_count;
	return ivec2(int(index % uint(surface_size.x)), int(index / uint(surface_size.x)));
}

ivec2 rtgi_diffuse_cache_surface_pos_from_id(vec4 id, ivec2 surface_size) {
	return rtgi_diffuse_cache_surface_pos_from_key_variant(rt_unpack_u32_rgba8(id), surface_size, 0);
}

vec3 rtgi_diffuse_cache_spg_oct_to_vec3(vec2 oct) {
	vec2 f = oct * 2.0 - 1.0;
	vec3 v = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = clamp(-v.z, 0.0, 1.0);
	v.xy += vec2(v.x >= 0.0 ? -t : t, v.y >= 0.0 ? -t : t);
	return normalize(v);
}

vec3 rtgi_diffuse_cache_spg_direction(ivec2 dir_tile) {
	vec2 oct = (vec2(clamp(dir_tile, ivec2(0), ivec2(RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION - 1))) + vec2(0.5)) / float(RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION);
	return rtgi_diffuse_cache_spg_oct_to_vec3(oct);
}

vec3 rtgi_diffuse_cache_spg_refined_direction(ivec2 dir_tile) {
	vec2 oct = (vec2(clamp(dir_tile, ivec2(0), ivec2(RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION - 1))) + vec2(0.5)) / float(RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION);
	return rtgi_diffuse_cache_spg_oct_to_vec3(oct);
}

mat4 rtgi_previous_view_matrix() {
	return transpose(mat4(scene_data_block.prev_data.view_matrix[0],
			scene_data_block.prev_data.view_matrix[1],
			scene_data_block.prev_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
}

mat4 rtgi_previous_inv_view_matrix() {
	return transpose(mat4(scene_data_block.prev_data.inv_view_matrix[0],
			scene_data_block.prev_data.inv_view_matrix[1],
			scene_data_block.prev_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
}

vec3 rtgi_previous_camera_world_origin() {
	mat4 prev_inv_view = rtgi_previous_inv_view_matrix();
	return prev_inv_view[3].xyz;
}

vec2 rt_texture_to_visible_uv(vec2 texture_uv, vec2 origin) {
	return (texture_uv * rt_extent() - origin) / rt_visible_size();
}

bool rtgi_previous_world_pos_from_view_z(vec2 previous_texture_uv, float previous_view_z, out vec3 world_pos) {
	world_pos = vec3(0.0);
	if (previous_view_z >= 60000.0 || previous_view_z <= 0.0 ||
			any(lessThan(previous_texture_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		return false;
	}

	vec2 previous_visible_uv = rt_texture_to_visible_uv(previous_texture_uv, rt_previous_origin());
	if (any(lessThan(previous_visible_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_visible_uv, vec2(1.0)))) {
		return false;
	}

	vec4 view_ray_h = scene_data_block.prev_data.inv_projection_matrix * vec4(previous_visible_uv * 2.0 - 1.0, 1.0, 1.0);
	vec3 view_ray = view_ray_h.xyz;
	if (abs(view_ray.z) <= 1e-5 || any(isnan(view_ray)) || any(isinf(view_ray))) {
		return false;
	}

	vec3 previous_view_pos = view_ray * (previous_view_z / abs(view_ray.z));
	world_pos = (rtgi_previous_inv_view_matrix() * vec4(previous_view_pos, 1.0)).xyz;
	return !(any(isnan(world_pos)) || any(isinf(world_pos)));
}

bool rtgi_diffuse_cache_sample_receiver(vec3 world_pos, vec3 normal, float roughness, vec3 albedo_proxy, out vec3 cached_lighting, out float cache_weight) {
	cached_lighting = vec3(0.0);
	cache_weight = 0.0;
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			get_rt_param(RT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_SPLIT_SIGNALS) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_HISTORY_WEIGHT) <= 0.001) {
		return false;
	}

	vec2 previous_uv;
	if (!project_uv_checked(world_pos, prev_vp_unjittered, previous_uv) ||
			any(lessThan(previous_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_uv, vec2(1.0)))) {
		return false;
	}

	vec2 previous_texture_uv = rt_visible_to_texture_uv(previous_uv, rt_previous_origin());
	if (any(lessThan(previous_texture_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		return false;
	}

	ivec2 persistent_size = imageSize(rtgi_diffuse_cache_radiance_image);
	if (persistent_size.x < RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT || persistent_size.y <= 0) {
		return false;
	}
	ivec2 cache_size = ivec2(max(persistent_size.x / RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT, 1), persistent_size.y);
	ivec2 base_cache_pos = clamp(ivec2(floor(previous_texture_uv * vec2(cache_size))), ivec2(0), cache_size - ivec2(1));

	vec3 normal_n = normalize(normal);
	vec4 receiver_id = rt_pack_u32_rgba8(rt_receiver_surface_id(world_pos, normal_n, roughness, max(albedo_proxy, vec3(0.0))));
	if (!rtgi_diffuse_cache_id_valid(receiver_id)) {
		return false;
	}

	mat4 prev_view = rtgi_previous_view_matrix();
	float previous_view_z = abs((prev_view * vec4(world_pos, 1.0)).z);
	float previous_camera_distance = length(world_pos - rtgi_previous_camera_world_origin());
	float normal_threshold = mix(0.86, 0.16, clamp(roughness, 0.0, 1.0));
	float depth_threshold = mix(0.07, 0.30, clamp(roughness, 0.0, 1.0));
	float distance_threshold = mix(0.10, 0.45, clamp(roughness, 0.0, 1.0));

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float best_quality = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 cache_pos = base_cache_pos + ivec2(x, y);
			if (cache_pos.x < 0 || cache_pos.y < 0 || cache_pos.x >= cache_size.x || cache_pos.y >= cache_size.y) {
				continue;
			}
			float spatial_weight = exp2(-dot(vec2(x, y), vec2(x, y)) * 0.75);
			for (int slot = 0; slot < RTGI_DIFFUSE_CACHE_RAY_SLOT_COUNT; slot++) {
				ivec2 sample_pos = rtgi_diffuse_cache_slot_pos(cache_pos, slot);
				vec4 sample_id = imageLoad(rtgi_diffuse_cache_history_id_image, sample_pos);
				if (!rtgi_diffuse_cache_id_matches(receiver_id, sample_id)) {
					continue;
				}

				vec4 radiance_sample = imageLoad(rtgi_diffuse_cache_radiance_image, sample_pos);
				vec4 meta_sample = imageLoad(rtgi_diffuse_cache_meta_image, sample_pos);
				vec4 stats_sample = imageLoad(rtgi_diffuse_cache_stats_image, sample_pos);
				float confidence = clamp(radiance_sample.a, 0.0, 1.0);
				float age = stats_sample.w;
				if (confidence <= 0.08 || age < 1.0) {
					continue;
				}

				vec3 candidate_normal = normalize(meta_sample.xyz * 2.0 - 1.0);
				float normal_dot = dot(normal_n, candidate_normal);
				if (normal_dot < normal_threshold) {
					continue;
				}

				float depth_delta = rtgi_diffuse_cache_relative_delta(previous_view_z, meta_sample.w, 0.25);
				float distance_delta = rtgi_diffuse_cache_relative_delta(previous_camera_distance, stats_sample.x, 0.25);
				if (depth_delta > depth_threshold || distance_delta > distance_threshold) {
					continue;
				}

				float variance_ratio = rtgi_diffuse_cache_variance_ratio(stats_sample);
				if (variance_ratio > 1.45) {
					continue;
				}

				float normal_weight = smoothstep(normal_threshold, 0.995, normal_dot);
				float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
				float distance_weight = 1.0 - smoothstep(distance_threshold * 0.35, distance_threshold, distance_delta);
				float variance_weight = 1.0 - smoothstep(0.28, 1.45, variance_ratio);
				float age_weight = smoothstep(1.0, 18.0, age);
				float candidate_quality = confidence * normal_weight * depth_weight * distance_weight * variance_weight * mix(0.50, 1.0, age_weight) * spatial_weight;
				if (candidate_quality <= 0.035) {
					continue;
				}

				vec3 candidate = sanitize_payload_vec3(radiance_sample.rgb);
				sum += candidate * candidate_quality;
				weight_sum += candidate_quality;
				best_quality = max(best_quality, candidate_quality);
			}
		}
	}

	if (weight_sum <= 0.05 || best_quality <= 0.06) {
		return false;
	}

	cached_lighting = sanitize_payload_vec3(sum / max(weight_sum, 1e-5));
	cache_weight = clamp(best_quality * 0.62 + min(weight_sum, 1.0) * 0.10, 0.0, 0.46);
	return cache_weight > 0.025;
}

bool rtgi_diffuse_cache_sample_refined_directional_spg(vec3 world_pos, vec3 normal, float roughness, vec3 albedo_proxy, out vec3 cached_lighting, out float cache_weight) {
	cached_lighting = vec3(0.0);
	cache_weight = 0.0;
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			get_rt_param(RT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_SPLIT_SIGNALS) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_HISTORY_WEIGHT) <= 0.001) {
		return false;
	}

	vec2 previous_uv;
	if (!project_uv_checked(world_pos, prev_vp_unjittered, previous_uv) ||
			any(lessThan(previous_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_uv, vec2(1.0)))) {
		return false;
	}

	vec2 previous_texture_uv = rt_visible_to_texture_uv(previous_uv, rt_previous_origin());
	if (any(lessThan(previous_texture_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		return false;
	}

	ivec2 refined_atlas_size = imageSize(rtgi_diffuse_cache_spg_refined_radiance_image);
	if (refined_atlas_size.x < RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE || refined_atlas_size.y < RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE) {
		return false;
	}
	ivec2 probe_size = max(refined_atlas_size / RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE, ivec2(1));
	if (any(notEqual(probe_size * RTGI_DIFFUSE_CACHE_SPG_REFINED_CELL_SIZE, refined_atlas_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_spg_refinement_mask_image), probe_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_spg_refined_meta_image), refined_atlas_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_spg_refined_stats_image), refined_atlas_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_spg_refined_visibility_image), refined_atlas_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_spg_refined_history_id_image), refined_atlas_size))) {
		return false;
	}

	ivec2 base_probe_pos = clamp(ivec2(floor(previous_texture_uv * vec2(probe_size))), ivec2(0), probe_size - ivec2(1));
	vec3 normal_n = normalize(normal);
	vec4 receiver_id = rt_pack_u32_rgba8(rt_receiver_surface_id(world_pos, normal_n, roughness, max(albedo_proxy, vec3(0.0))));
	if (!rtgi_diffuse_cache_id_valid(receiver_id)) {
		return false;
	}

	mat4 prev_view = rtgi_previous_view_matrix();
	float previous_view_z = abs((prev_view * vec4(world_pos, 1.0)).z);
	float normal_threshold = mix(0.80, 0.14, clamp(roughness, 0.0, 1.0));
	float depth_threshold = mix(0.060, 0.26, clamp(roughness, 0.0, 1.0));
	float sub_spacing = max(float(RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING) / float(RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS), 1.0);
	vec2 previous_probe_pixels = previous_texture_uv * vec2(probe_size) * float(RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING);

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float best_quality = 0.0;
	for (int py = -1; py <= 1; py++) {
		for (int px = -1; px <= 1; px++) {
			ivec2 probe_pos = base_probe_pos + ivec2(px, py);
			if (probe_pos.x < 0 || probe_pos.y < 0 || probe_pos.x >= probe_size.x || probe_pos.y >= probe_size.y) {
				continue;
			}

			vec4 mask = imageLoad(rtgi_diffuse_cache_spg_refinement_mask_image, probe_pos);
			float mask_weight = max(mask.r, mask.a * 0.70);
			if (mask_weight <= 0.16) {
				continue;
			}

			for (int sy = 0; sy < RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS; sy++) {
				for (int sx = 0; sx < RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS; sx++) {
					ivec2 sub_tile = ivec2(sx, sy);
					vec2 sub_center_pixels = vec2(probe_pos * RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING) + vec2(sub_tile) * sub_spacing + vec2(sub_spacing * 0.5);
					vec2 sub_center_uv = sub_center_pixels / (vec2(probe_size) * float(RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING));
					vec2 pixel_delta = previous_probe_pixels - sub_center_pixels;
					float spatial_weight = exp2(-dot(pixel_delta, pixel_delta) / 12.0);

					for (int dy = 0; dy < RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION; dy++) {
						for (int dx = 0; dx < RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION; dx++) {
							ivec2 dir_tile = ivec2(dx, dy);
							ivec2 atlas_pos = rtgi_diffuse_cache_spg_refined_atlas_pos(probe_pos, sub_tile, dir_tile);
							vec4 radiance_sample = imageLoad(rtgi_diffuse_cache_spg_refined_radiance_image, atlas_pos);
							vec4 stats_sample = imageLoad(rtgi_diffuse_cache_spg_refined_stats_image, atlas_pos);
							float confidence = clamp(radiance_sample.a, 0.0, 1.0);
							float stats_quality = clamp(smoothstep(1.0, 8.0, stats_sample.x) * stats_sample.y * stats_sample.z * stats_sample.w, 0.0, 1.0);
							if (confidence <= 0.04 || stats_quality <= 0.015) {
								continue;
							}

							vec4 meta_sample = imageLoad(rtgi_diffuse_cache_spg_refined_meta_image, atlas_pos);
							vec3 candidate_normal = normalize(meta_sample.xyz * 2.0 - 1.0);
							float normal_dot = dot(normal_n, candidate_normal);
							if (normal_dot < normal_threshold) {
								continue;
							}

							float depth_delta = rtgi_diffuse_cache_relative_delta(previous_view_z, meta_sample.w, 0.25);
							if (depth_delta > depth_threshold) {
								continue;
							}

							vec3 candidate_world_pos;
							bool has_candidate_plane = rtgi_previous_world_pos_from_view_z(sub_center_uv, meta_sample.w, candidate_world_pos);
							float plane_weight = 1.0;
							if (has_candidate_plane) {
								float plane_distance = abs(dot(world_pos - candidate_world_pos, candidate_normal));
								float plane_threshold = max(0.030, previous_view_z * mix(0.0040, 0.018, clamp(roughness, 0.0, 1.0)));
								plane_weight = 1.0 - smoothstep(plane_threshold * 0.30, plane_threshold, plane_distance);
								if (plane_weight <= 0.01) {
									continue;
								}
							} else if (depth_delta > depth_threshold * 0.45) {
								continue;
							}

							vec4 sample_id = imageLoad(rtgi_diffuse_cache_spg_refined_history_id_image, atlas_pos);
							bool exact_history = rtgi_diffuse_cache_id_valid(sample_id) && rtgi_diffuse_cache_id_matches(receiver_id, sample_id);
							if (!exact_history && confidence < 0.16) {
								continue;
							}

							vec3 incoming_dir = rtgi_diffuse_cache_spg_refined_direction(dir_tile);
							float hemisphere_weight = smoothstep(-0.02, 0.62, dot(normal_n, incoming_dir));
							if (hemisphere_weight <= 0.01) {
								continue;
							}

							vec3 candidate = sanitize_payload_vec3(radiance_sample.rgb);
							float radiance_weight = smoothstep(0.0005, 0.0060, rt_luminance(candidate));
							if (radiance_weight <= 0.0) {
								continue;
							}

							vec4 visibility_sample = imageLoad(rtgi_diffuse_cache_spg_refined_visibility_image, atlas_pos);
							float visibility_weight = rtgi_diffuse_cache_spg_visibility_quality(visibility_sample);
							float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
							float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
							float history_weight = exact_history ? 1.0 : 0.30;
							float candidate_quality = confidence * stats_quality * mask_weight * spatial_weight * normal_weight * depth_weight * plane_weight * visibility_weight * mix(0.22, 1.0, hemisphere_weight) * radiance_weight * history_weight;
							if (candidate_quality <= 0.014) {
								continue;
							}

							sum += candidate * candidate_quality;
							weight_sum += candidate_quality;
							best_quality = max(best_quality, candidate_quality);
						}
					}
				}
			}
		}
	}

	if (weight_sum <= 0.035 || best_quality <= 0.030) {
		return false;
	}

	cached_lighting = sanitize_payload_vec3(sum / max(weight_sum, 1e-5));
	cache_weight = clamp(best_quality * 0.30 + min(weight_sum, 1.0) * 0.05, 0.0, 0.18);
	return cache_weight > 0.030;
}

bool rtgi_diffuse_cache_sample_directional_spg(vec3 world_pos, vec3 normal, float roughness, vec3 albedo_proxy, out vec3 cached_lighting, out float cache_weight) {
	cached_lighting = vec3(0.0);
	cache_weight = 0.0;
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			get_rt_param(RT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_SPLIT_SIGNALS) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_HISTORY_WEIGHT) <= 0.001) {
		return false;
	}

	vec2 previous_uv;
	if (!project_uv_checked(world_pos, prev_vp_unjittered, previous_uv) ||
			any(lessThan(previous_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_uv, vec2(1.0)))) {
		return false;
	}

	vec2 previous_texture_uv = rt_visible_to_texture_uv(previous_uv, rt_previous_origin());
	if (any(lessThan(previous_texture_uv, vec2(0.0))) ||
			any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		return false;
	}

	ivec2 atlas_size = imageSize(rtgi_diffuse_cache_spg_radiance_image);
	if (atlas_size.x < RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION || atlas_size.y < RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION) {
		return false;
	}
	bool has_visibility = all(equal(imageSize(rtgi_diffuse_cache_spg_visibility_image), atlas_size));
	ivec2 probe_size = max(atlas_size / RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION, ivec2(1));
	ivec2 base_probe_pos = clamp(ivec2(floor(previous_texture_uv * vec2(probe_size))), ivec2(0), probe_size - ivec2(1));

	vec3 normal_n = normalize(normal);
	vec4 receiver_id = rt_pack_u32_rgba8(rt_receiver_surface_id(world_pos, normal_n, roughness, max(albedo_proxy, vec3(0.0))));
	if (!rtgi_diffuse_cache_id_valid(receiver_id)) {
		return false;
	}

	mat4 prev_view = rtgi_previous_view_matrix();
	float previous_view_z = abs((prev_view * vec4(world_pos, 1.0)).z);
	float normal_threshold = mix(0.76, 0.12, clamp(roughness, 0.0, 1.0));
	float depth_threshold = mix(0.08, 0.34, clamp(roughness, 0.0, 1.0));

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	float best_quality = 0.0;
	for (int py = -1; py <= 1; py++) {
		for (int px = -1; px <= 1; px++) {
			ivec2 probe_pos = base_probe_pos + ivec2(px, py);
			if (probe_pos.x < 0 || probe_pos.y < 0 || probe_pos.x >= probe_size.x || probe_pos.y >= probe_size.y) {
				continue;
			}

			vec2 probe_center_uv = (vec2(probe_pos) + vec2(0.5)) / vec2(probe_size);
			vec2 probe_delta = (previous_texture_uv - probe_center_uv) * vec2(probe_size) * float(RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING);
			float spatial_weight = exp2(-dot(probe_delta, probe_delta) / 36.0);
			for (int dy = 0; dy < RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION; dy++) {
				for (int dx = 0; dx < RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION; dx++) {
					ivec2 dir_tile = ivec2(dx, dy);
					ivec2 atlas_pos = rtgi_diffuse_cache_spg_atlas_pos(probe_pos, dir_tile);
					vec4 radiance_sample = imageLoad(rtgi_diffuse_cache_spg_radiance_image, atlas_pos);
					vec4 stats_sample = imageLoad(rtgi_diffuse_cache_spg_stats_image, atlas_pos);
					vec4 visibility_sample = has_visibility ? imageLoad(rtgi_diffuse_cache_spg_visibility_image, atlas_pos) : vec4(65504.0, 0.0, 65504.0, 0.0);
					float confidence = clamp(radiance_sample.a, 0.0, 1.0);
					float stats_quality = clamp(smoothstep(1.0, 10.0, stats_sample.x) * stats_sample.y * stats_sample.z * stats_sample.w, 0.0, 1.0);
					if (confidence <= 0.05 || stats_quality <= 0.015) {
						continue;
					}

					vec4 meta_sample = imageLoad(rtgi_diffuse_cache_spg_meta_image, atlas_pos);
					vec3 candidate_normal = normalize(meta_sample.xyz * 2.0 - 1.0);
					float normal_dot = dot(normal_n, candidate_normal);
					if (normal_dot < normal_threshold) {
						continue;
					}

					float depth_delta = rtgi_diffuse_cache_relative_delta(previous_view_z, meta_sample.w, 0.25);
					if (depth_delta > depth_threshold) {
						continue;
					}

					vec3 candidate_world_pos;
					bool has_candidate_plane = rtgi_previous_world_pos_from_view_z(probe_center_uv, meta_sample.w, candidate_world_pos);
					float plane_weight = 1.0;
					if (has_candidate_plane) {
						float plane_distance = abs(dot(world_pos - candidate_world_pos, candidate_normal));
						float plane_threshold = max(0.035, previous_view_z * mix(0.0045, 0.020, clamp(roughness, 0.0, 1.0)));
						plane_weight = 1.0 - smoothstep(plane_threshold * 0.30, plane_threshold, plane_distance);
						if (plane_weight <= 0.01) {
							continue;
						}
					} else if (depth_delta > depth_threshold * 0.45) {
						continue;
					}

					vec4 sample_id = imageLoad(rtgi_diffuse_cache_spg_history_id_image, atlas_pos);
					bool exact_history = rtgi_diffuse_cache_id_valid(sample_id) && rtgi_diffuse_cache_id_matches(receiver_id, sample_id);
					if (!exact_history && confidence < 0.18) {
						continue;
					}

					vec3 incoming_dir = rtgi_diffuse_cache_spg_direction(dir_tile);
					float hemisphere_weight = smoothstep(-0.02, 0.66, dot(normal_n, incoming_dir));
					if (hemisphere_weight <= 0.01) {
						continue;
					}

					vec3 candidate = sanitize_payload_vec3(radiance_sample.rgb);
					float radiance_weight = smoothstep(0.0005, 0.0060, rt_luminance(candidate));
					if (radiance_weight <= 0.0) {
						continue;
					}

					float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
					float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
					float visibility_weight = rtgi_diffuse_cache_spg_visibility_quality(visibility_sample);
					float history_weight = exact_history ? 1.0 : 0.30;
					float candidate_quality = confidence * stats_quality * spatial_weight * normal_weight * depth_weight * plane_weight * visibility_weight * mix(0.25, 1.0, hemisphere_weight) * radiance_weight * history_weight;
					if (candidate_quality <= 0.018) {
						continue;
					}

					sum += candidate * candidate_quality;
					weight_sum += candidate_quality;
					best_quality = max(best_quality, candidate_quality);
				}
			}
		}
	}

	if (weight_sum <= 0.05 || best_quality <= 0.04) {
		return false;
	}

	cached_lighting = sanitize_payload_vec3(sum / max(weight_sum, 1e-5));
	cache_weight = clamp(best_quality * 0.34 + min(weight_sum, 1.0) * 0.06, 0.0, 0.22);
	return cache_weight > 0.035;
}

uint rtgi_surface_cache_query_from_key_reason(uint surface_key_reason) {
	if (surface_key_reason == RTGI_SURFACE_KEY_REASON_HISTORY_INVALID ||
			surface_key_reason == RTGI_SURFACE_KEY_REASON_DEFORMED ||
			surface_key_reason == RTGI_SURFACE_KEY_REASON_PROCEDURAL) {
		return RTGI_SURFACE_CACHE_QUERY_DYNAMIC_INELIGIBLE;
	}
	return RTGI_SURFACE_CACHE_QUERY_NO_KEY;
}

bool rtgi_diffuse_cache_sample_surface(uint surface_key, uint surface_key_reason, vec3 normal, float roughness, out vec3 cached_lighting, out float cache_weight, out uint query_reason, out float query_detail, out float query_luminance, out uint query_source) {
	cached_lighting = vec3(0.0);
	cache_weight = 0.0;
	query_reason = RTGI_SURFACE_CACHE_QUERY_NONE;
	query_detail = 0.0;
	query_luminance = 0.0;
	query_source = RTGI_SURFACE_SOURCE_NONE;
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			get_rt_param(RT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_SPLIT_SIGNALS) <= 0.5 ||
			get_rt_param(RT_PARAM_DENOISER_HISTORY_WEIGHT) <= 0.001) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_INELIGIBLE;
		return false;
	}
	if (surface_key == 0u) {
		query_reason = rtgi_surface_cache_query_from_key_reason(surface_key_reason);
		return false;
	}

	ivec2 surface_size = imageSize(rtgi_diffuse_cache_surface_radiance_image);
	if (surface_size.x <= 1 || surface_size.y <= 1 ||
			any(notEqual(imageSize(rtgi_diffuse_cache_surface_meta_image), surface_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_surface_stats_image), surface_size)) ||
			any(notEqual(imageSize(rtgi_diffuse_cache_surface_history_id_image), surface_size))) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_NO_PAGE;
		return false;
	}

	vec3 normal_n = normalize(normal);
	vec4 surface_id = rt_pack_u32_rgba8(surface_key);
	if (!rtgi_diffuse_cache_id_valid(surface_id)) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_NO_KEY;
		return false;
	}

	ivec2 surface_pos = ivec2(0);
	bool found_page = false;
	bool saw_occupied_page = false;
	for (int variant = 0; variant < RTGI_DIFFUSE_CACHE_SURFACE_ASSOCIATIVITY; variant++) {
		ivec2 candidate_pos = rtgi_diffuse_cache_surface_pos_from_key_variant(surface_key, surface_size, variant);
		vec4 candidate_id = imageLoad(rtgi_diffuse_cache_surface_history_id_image, candidate_pos);
		if (!rtgi_diffuse_cache_id_valid(candidate_id)) {
			continue;
		}
		saw_occupied_page = true;
		if (rtgi_diffuse_cache_id_matches(candidate_id, surface_id)) {
			surface_pos = candidate_pos;
			found_page = true;
			break;
		}
	}
	if (!found_page && !saw_occupied_page) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_NO_PAGE;
		return false;
	}
	if (!found_page) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_ID_MISMATCH;
		return false;
	}

	vec4 radiance_sample = imageLoad(rtgi_diffuse_cache_surface_radiance_image, surface_pos);
	vec4 meta_sample = imageLoad(rtgi_diffuse_cache_surface_meta_image, surface_pos);
	vec4 stats_sample = imageLoad(rtgi_diffuse_cache_surface_stats_image, surface_pos);
	float confidence = clamp(radiance_sample.a, 0.0, 1.0);
	float support = clamp(stats_sample.x, 0.0, 1.0);
	float variance_quality = clamp(stats_sample.y, 0.0, 1.0);
	uint surface_source = rtgi_surface_source_from_bucket(stats_sample.z);
	query_source = surface_source;
	float source_quality = rtgi_surface_source_quality(surface_source);
	float stale_frames = max(stats_sample.w, 0.0);
	float page_maturity = mix(0.72, 1.0, source_quality);
	if (confidence <= 0.035) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_LOW_CONFIDENCE;
		query_detail = clamp(confidence / 0.035, 0.0, 1.0);
		return false;
	}
	if (support <= 0.010) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_LOW_SUPPORT;
		query_detail = clamp(support / 0.010, 0.0, 1.0);
		return false;
	}
	if (variance_quality <= 0.010) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_LOW_VARIANCE;
		query_detail = clamp(variance_quality / 0.010, 0.0, 1.0);
		return false;
	}

	vec3 sample_normal = normalize(meta_sample.xyz * 2.0 - 1.0);
	float normal_threshold = mix(0.82, 0.18, clamp(roughness, 0.0, 1.0));
	float normal_dot = dot(normal_n, sample_normal);
	if (normal_dot < normal_threshold) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_NORMAL_MISMATCH;
		query_detail = clamp((normal_dot + 1.0) * 0.5, 0.0, 1.0);
		return false;
	}

	vec3 radiance = sanitize_payload_vec3(radiance_sample.rgb);
	float radiance_weight = smoothstep(0.0004, 0.0055, rt_luminance(radiance));
	float stale_weight = 1.0 - smoothstep(18.0, 96.0, stale_frames);
	query_luminance = rt_luminance(radiance);
	if (stale_weight <= 0.010) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_STALE;
		query_detail = clamp(1.0 - stale_frames / 128.0, 0.0, 1.0);
		return false;
	}
	if (radiance_weight <= 0.010) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_WEAK_RADIANCE;
		query_detail = clamp(radiance_weight / 0.010, 0.0, 1.0);
		return false;
	}
	float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
	float quality = confidence * support * variance_quality * page_maturity * stale_weight * normal_weight * radiance_weight * source_quality;
	if (quality <= 0.020) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_WEAK_QUALITY;
		query_detail = clamp(quality / 0.020, 0.0, 1.0);
		return false;
	}

	cached_lighting = radiance;
	cache_weight = clamp(quality * mix(0.20, 0.28, source_quality) + confidence * 0.035 * source_quality, 0.0, mix(0.11, 0.16, source_quality));
	if (cache_weight <= 0.020) {
		query_reason = RTGI_SURFACE_CACHE_QUERY_WEAK_QUALITY;
		query_detail = clamp(cache_weight / 0.020, 0.0, 1.0);
		return false;
	}
	query_reason = RTGI_SURFACE_CACHE_QUERY_ACCEPTED;
	query_detail = clamp(cache_weight / 0.15, 0.0, 1.0);
	return true;
}

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
#define RT_SOURCE_REJECT_VISIBILITY 14u

uint rt_source_make_key(uint source_class, uint source_id) {
	return (source_class << RT_SOURCE_CLASS_SHIFT) | (source_id & 0x0FFFFFFFu);
}

uint rt_source_unpack_history_id(vec4 packed_id) {
	return rt_unpack_u32_rgba8(packed_id);
}

float rt_source_ratio(float a, float b) {
	return min(a, b) / max(max(a, b), 1e-5);
}

float rt_source_debug_log(float value, float scale) {
	return clamp(log2(max(value, 0.0) * scale + 1.0) / 16.0, 0.0, 1.0);
}

const uint RT_EMISSIVE_CANDIDATE_FLAG_COMPRESSED_GEOMETRY = 1u;
const uint RT_EMISSIVE_CANDIDATE_FLAG_PRIMITIVE_DISTRIBUTION = 2u;
const uint RT_EMISSIVE_CANDIDATE_FLAG_TEXTURED_EMISSION = 4u;

struct RTEmissivePrimitiveDistribution {
	uint primitive_id;
	float cumulative_weight;
	float area;
	float _pad;
};

struct RTEmissiveCandidate {
	float object_to_world[12];
	uint geometry_index;
	uint flags;
	float selection_weight;
	float primitive_weight_sum;
	uint primitive_offset;
	uint primitive_count;
	uint _pad[2];
};

layout(set = 0, binding = 44, std430) readonly buffer RTEmissiveCandidateBuffer {
	RTEmissiveCandidate rt_emissive_candidates[];
};

layout(set = 0, binding = 76, std430) readonly buffer RTEmissivePrimitiveDistributionBuffer {
	RTEmissivePrimitiveDistribution rt_emissive_primitive_distributions[];
};

struct RTGISTRCProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
	vec4 metadata;
};

layout(set = 0, binding = 64, rgba16f) uniform image2D rt_strc_irradiance_image;
layout(set = 0, binding = 65, rgba16f) uniform image2D rt_strc_distance_image;
layout(set = 0, binding = 66, std430) buffer RTGISTRCProbeRayResultBuffer {
	RTGISTRCProbeRayResult rt_strc_probe_ray_results[];
};
layout(set = 0, binding = 75, rgba16f) uniform image2D rt_strc_metadata_image;

// World Radiance Cache probe-update ray results. Same 48-byte 3xvec4 layout as
// RTGISTRCProbeRayResult so Task 6's accumulate reuses the texel-mapping
// convention: one ray == one (probe, direction) == one octahedral texel.
// metadata.w holds the WRC update_index (probe_linear << 6 | dir_index for the
// default oct_res 8 => 64 dirs => 6 bits, identical to STRC's packing).
struct RTGIWRCProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
	vec4 metadata;
};

layout(set = 0, binding = 107, std430) buffer RTGIWRCProbeRayResultBuffer {
	RTGIWRCProbeRayResult rt_wrc_probe_ray_results[];
};

// Screen Probe Gather (SPG) gather ray results (A2-T2). One entry per selected
// (screen-probe, octahedral direction) this frame; the T3 accumulate folds these
// into the per-probe octahedral radiance atlas. 32-byte stride (2 x vec4) matches
// RTGIScreenProbeGather::ensure_ray_result_buffer.
struct RTGISPGRayResult {
	vec4 radiance_distance; // .rgb = incident radiance, .a = hit distance (-1 = WRC-sourced / no trace)
	vec4 probe_dir; // .x = probe_linear, .y = dir_index, .zw = pad
};
layout(set = 0, binding = 108, std430) buffer RTGISPGRayResultBuffer {
	RTGISPGRayResult rt_spg_ray_results[];
};

// SPG gather read-only inputs. 109/110 = the SPG probe headers written by run_placement
// (RGBA32F plane: .xyz world-pos, .w linear-depth(<=0 invalid); RGBA16F aux: .xy oct-normal,
// .zw motion). 111/112 = the WRC radiance + distance atlases the cold-cache gather queries.
layout(set = 0, binding = 109) uniform sampler2D rt_spg_header_plane;
layout(set = 0, binding = 110) uniform sampler2D rt_spg_header_aux;
layout(set = 0, binding = 111) uniform sampler2D rt_wrc_radiance_for_spg;
layout(set = 0, binding = 112) uniform sampler2D rt_wrc_distance_for_spg;

bool rt_strc_enabled() {
	return (RT_FLAGS & RT_FLAG_STRC_ENABLED) != 0u && get_rt_param(RT_PARAM_RTGI_STRC_ENABLED) > 0.5 && get_rt_param(RT_PARAM_RTGI_STRC_STRENGTH) > 0.001;
}

bool rt_strc_internal_fallback_enabled() {
	return (RT_FLAGS & RT_FLAG_STRC_INTERNAL_FALLBACK) != 0u && get_rt_param(RT_PARAM_RTGI_STRC_RAYS_PER_FRAME) > 0.5;
}

bool rt_strc_resources_available() {
	return ((RT_FLAGS & (RT_FLAG_STRC_ENABLED | RT_FLAG_STRC_INTERNAL_FALLBACK)) != 0u) && get_rt_param(RT_PARAM_RTGI_STRC_RAYS_PER_FRAME) > 0.5;
}

mat4 rt_camera_inv_view_matrix() {
	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	return inv_view;
}

vec3 rt_camera_world_origin() {
	mat4 inv_view = rt_camera_inv_view_matrix();
	return inv_view[3].xyz;
}

vec3 rt_camera_world_axis(vec3 view_axis) {
	mat4 inv_view = rt_camera_inv_view_matrix();
	return normalize((inv_view * vec4(view_axis, 0.0)).xyz);
}

float rtgi_decode_raster_roughness(float encoded_roughness) {
	float roughness = encoded_roughness > 0.5 ? 1.0 - encoded_roughness : encoded_roughness;
	return clamp(roughness / (127.0 / 255.0), 0.0, 1.0);
}

vec3 rtgi_decode_raster_world_normal(vec4 normal_roughness, mat4 inv_view) {
	vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	return normalize(mat3(inv_view) * view_normal);
}

bool rtgi_load_current_raster_surface(ivec2 pixel, vec2 visible_uv, out float depth, out vec3 view_pos, out vec3 world_pos, out vec3 world_normal, out float roughness, out vec3 albedo_proxy) {
	ivec2 raster_size = textureSize(sampler2D(raster_depth_texture, raster_nearest_sampler), 0);
	if (pixel.x < 0 || pixel.y < 0 || pixel.x >= raster_size.x || pixel.y >= raster_size.y) {
		return false;
	}

	depth = texelFetch(sampler2D(raster_depth_texture, raster_nearest_sampler), pixel, 0).r;
	if (depth <= 0.000001) {
		return false;
	}

	vec4 view_h = scene_data_block.data.inv_projection_matrix * vec4(visible_uv * 2.0 - 1.0, depth, 1.0);
	if (any(isnan(view_h)) || any(isinf(view_h)) || abs(view_h.w) <= 1e-6) {
		return false;
	}

	view_pos = view_h.xyz / view_h.w;
	mat4 inv_view = rt_camera_inv_view_matrix();
	world_pos = (inv_view * vec4(view_pos, 1.0)).xyz;
	if (any(isnan(world_pos)) || any(isinf(world_pos))) {
		return false;
	}

	vec4 raster_normal_roughness = texelFetch(sampler2D(raster_normal_roughness_texture, raster_nearest_sampler), pixel, 0);
	world_normal = rtgi_decode_raster_world_normal(raster_normal_roughness, inv_view);
	vec3 view_dir = normalize(rt_camera_world_origin() - world_pos);
	if (dot(world_normal, view_dir) < 0.0) {
		world_normal = -world_normal;
	}
	roughness = rtgi_decode_raster_roughness(raster_normal_roughness.a);

	vec3 raster_color = sanitize_payload_vec3(texelFetch(sampler2D(raster_color_texture, raster_nearest_sampler), pixel, 0).rgb);
	float max_channel = max(max(raster_color.r, raster_color.g), raster_color.b);
	albedo_proxy = clamp(raster_color / max(max_channel, 1.0), vec3(0.04), vec3(1.0));
	return true;
}

bool rtgi_trace_current_raster_hit(vec3 ray_origin, vec3 ray_dir, vec3 origin_normal, float max_distance, out vec3 hit_pos, out vec3 hit_normal, out float hit_roughness, out vec3 hit_albedo_proxy, out float hit_confidence, out uint hit_surface_key) {
	hit_pos = vec3(0.0);
	hit_normal = vec3(0.0, 1.0, 0.0);
	hit_roughness = 1.0;
	hit_albedo_proxy = vec3(1.0);
	hit_confidence = 0.0;
	hit_surface_key = 0u;
	if (rt_strc_probe_update_mode() || uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED) {
		return false;
	}

	ivec2 raster_size = textureSize(sampler2D(raster_depth_texture, raster_nearest_sampler), 0);
	if (raster_size.x <= 1 || raster_size.y <= 1) {
		return false;
	}

	vec3 dir = normalize(ray_dir);
	float best_score = 0.0;
	const int step_count = 14;
	for (int i = 0; i < step_count; i++) {
		float step_phase = (float(i) + 0.65) / float(step_count);
		float t = mix(0.18, max(max_distance, 0.35), step_phase * step_phase);
		vec3 probe_pos = ray_origin + dir * t;

		vec2 uv;
		if (!project_uv_checked(probe_pos, curr_vp_unjittered, uv) || any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
			continue;
		}

		ivec2 pixel = clamp(ivec2(floor(uv * vec2(raster_size))), ivec2(0), raster_size - ivec2(1));
		float depth = 0.0;
		vec3 view_pos = vec3(0.0);
		vec3 candidate_pos = vec3(0.0);
		vec3 candidate_normal = vec3(0.0, 1.0, 0.0);
		float candidate_roughness = 1.0;
		vec3 candidate_albedo = vec3(1.0);
		if (!rtgi_load_current_raster_surface(pixel, uv, depth, view_pos, candidate_pos, candidate_normal, candidate_roughness, candidate_albedo)) {
			continue;
		}

		vec3 to_candidate = candidate_pos - ray_origin;
		float along = dot(to_candidate, dir);
		if (along <= 0.10 || along > max_distance) {
			continue;
		}

		float line_distance = length(to_candidate - dir * along);
		float thickness = max(0.055, along * 0.026);
		if (line_distance > thickness) {
			continue;
		}

		float facing = smoothstep(0.02, 0.28, dot(candidate_normal, -dir));
		float line_score = 1.0 - smoothstep(thickness * 0.35, thickness, line_distance);
		float same_surface = smoothstep(0.92, 0.995, dot(origin_normal, candidate_normal)) * (1.0 - smoothstep(0.18, 0.52, along));
		float distance_score = 1.0 - smoothstep(max_distance * 0.72, max_distance, along);
		float score = facing * line_score * distance_score * (1.0 - same_surface);
		if (score > best_score) {
			best_score = score;
			hit_pos = candidate_pos;
			hit_normal = candidate_normal;
			hit_roughness = candidate_roughness;
			hit_albedo_proxy = candidate_albedo;
			hit_confidence = score;
			hit_surface_key = imageLoad(rt_surface_cache_key_image, pixel).r;
		}
	}

	return best_score > 0.045;
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

uint rt_strc_direction_index(vec3 direction) {
	vec2 oct = vec3_to_oct(normalize(direction)) * 0.5 + 0.5;
	uvec2 texel = uvec2(clamp(floor(oct * 8.0), vec2(0.0), vec2(7.0)));
	return texel.x + texel.y * 8u;
}

uint rt_strc_probe_index(uint cascade, uvec3 probe_coord, uint grid_size) {
	uint grid = max(grid_size, 1u);
	return cascade * grid * grid * grid + probe_coord.x + probe_coord.y * grid + probe_coord.z * grid * grid;
}

vec3 rt_strc_probe_world_position(vec3 cascade_center, float spacing, uvec3 probe_coord, uint grid_size) {
	vec3 probe_local = (vec3(probe_coord) + vec3(0.5)) - vec3(float(grid_size) * 0.5);
	return cascade_center + probe_local * spacing;
}

uint rt_strc_cascade_weight(uint cascade, uint cascade_count) {
	return 1u << (max(cascade_count, 1u) - cascade - 1u);
}

uint rt_strc_select_round_robin_update_index(uint ray_index, uint ray_count, uint grid, uint cascade_count, uint frame_index) {
	uint active_cascades = clamp(cascade_count, 1u, 4u);
	uint texels_per_cascade = grid * grid * grid * 64u;
	if (active_cascades == 1u) {
		return texels_per_cascade > 0u ? (frame_index * max(ray_count, 1u) + ray_index) % texels_per_cascade : 0u;
	}

	uint slot_count = (1u << active_cascades) - 1u;
	uint slot = ray_index % slot_count;
	uint slot_start = 0u;
	uint cascade = 0u;
	uint selected_weight = rt_strc_cascade_weight(0u, active_cascades);
	for (uint i = 0u; i < 4u; i++) {
		if (i >= active_cascades) {
			break;
		}
		uint weight = rt_strc_cascade_weight(i, active_cascades);
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

uint rt_strc_view_update_budget(uint ray_count) {
	if (ray_count < 512u) {
		return 0u;
	}
	return min(ray_count / 3u, ray_count - 1u);
}

uint rt_strc_select_view_biased_update_index(uint ray_index, uint grid, uint cascade_count, uint frame_index) {
	uint active_cascades = clamp(cascade_count, 1u, 4u);
	uint texels_per_cascade = grid * grid * grid * 64u;
	if (texels_per_cascade == 0u) {
		return 0u;
	}

	uint slot_count = active_cascades >= 3u ? 6u : (active_cascades == 2u ? 4u : 1u);
	uint slot = ray_index % slot_count;
	uint cascade = 0u;
	uint selected_slot = 0u;
	uint selected_slot_count = 1u;
	if (active_cascades >= 3u) {
		if (slot < 4u) {
			cascade = 0u;
			selected_slot = slot;
			selected_slot_count = 4u;
		} else if (slot == 4u) {
			cascade = 1u;
		} else {
			cascade = 2u;
		}
	} else if (active_cascades == 2u) {
		if (slot < 3u) {
			cascade = 0u;
			selected_slot = slot;
			selected_slot_count = 3u;
		} else {
			cascade = 1u;
		}
	}

	uint local_ray = (ray_index / slot_count) * selected_slot_count + selected_slot;
	uint dir_index = (local_ray + frame_index * 19u + cascade * 7u) & 63u;
	uint probe_sequence = local_ray >> 6u;

	uint lateral_radius = clamp(grid / 4u, 2u, 6u);
	uint lateral_span = lateral_radius * 2u + 1u;
	uint forward_count = clamp(grid / 2u + 1u, 4u, 10u);
	uint view_probe_count = max(lateral_span * lateral_span * forward_count, 1u);
	uint view_probe = (probe_sequence + frame_index * 17u + cascade * 43u) % view_probe_count;

	int side_x = int(view_probe % lateral_span) - int(lateral_radius);
	int side_y = int((view_probe / lateral_span) % lateral_span) - int(lateral_radius);
	int front_z = int((view_probe / (lateral_span * lateral_span)) % forward_count);

	vec3 camera_right = rt_camera_world_axis(vec3(1.0, 0.0, 0.0));
	vec3 camera_up = rt_camera_world_axis(vec3(0.0, 1.0, 0.0));
	vec3 camera_forward = rt_camera_world_axis(vec3(0.0, 0.0, -1.0));
	vec3 probe_offset = camera_right * float(side_x) + camera_up * float(side_y) + camera_forward * float(front_z);
	vec3 probe_space = vec3(float(grid) * 0.5 - 0.5) + probe_offset;
	ivec3 probe_coord_i = clamp(ivec3(floor(probe_space + vec3(0.5))), ivec3(0), ivec3(int(grid) - 1));
	uvec3 probe_coord = uvec3(probe_coord_i);
	uint probe_index = rt_strc_probe_index(cascade, probe_coord, grid);
	return (probe_index << 6u) | dir_index;
}

uint rt_strc_select_update_index(uint ray_index, uint ray_count, uint grid, uint cascade_count, uint frame_index) {
	uint active_cascades = clamp(cascade_count, 1u, 4u);
	uint view_ray_count = rt_strc_view_update_budget(max(ray_count, 1u));
	if (ray_index < view_ray_count) {
		return rt_strc_select_view_biased_update_index(ray_index, grid, active_cascades, frame_index);
	}

	uint background_ray_count = max(ray_count - view_ray_count, 1u);
	uint background_ray_index = ray_index - view_ray_count;
	return rt_strc_select_round_robin_update_index(background_ray_index, background_ray_count, grid, active_cascades, frame_index);
}

float rt_strc_distance_visibility(vec4 moments, float receiver_distance, float spacing) {
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

float rt_strc_age_confidence(float age, uint cascade, uint cascade_count, uint grid) {
	float rays_per_frame = max(get_rt_param(RT_PARAM_RTGI_STRC_RAYS_PER_FRAME), 1.0);
	float cascade_weight = float(rt_strc_cascade_weight(cascade, cascade_count));
	float weight_sum = float((1u << clamp(cascade_count, 1u, 4u)) - 1u);
	float cascade_budget = max(rays_per_frame * cascade_weight / max(weight_sum, 1.0), 1.0);
	float texels_per_cascade = float(grid) * float(grid) * float(grid) * 64.0;
	float expected_cycle = max(texels_per_cascade / cascade_budget, 32.0);
	return 1.0 - smoothstep(expected_cycle * 3.0, expected_cycle * 8.0, age);
}

float rt_strc_source_quality(uint source_mask) {
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

vec3 rt_strc_sample_irradiance_with_source(vec3 world_pos, vec3 normal, out float confidence, out uint dominant_source_mask) {
	confidence = 0.0;
	dominant_source_mask = 0u;
	if (!rt_strc_resources_available() || rt_strc_probe_update_mode()) {
		return vec3(0.0);
	}

	uint grid = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_GRID_SIZE)), 12u, 32u);
	uint cascade_count = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_CASCADE_COUNT)), 1u, 4u);
	float base_spacing = max(get_rt_param(RT_PARAM_RTGI_STRC_BASE_PROBE_SPACING), 0.25);
	vec3 camera_origin = rt_camera_world_origin();
	vec3 normal_n = normalize(normal);
	vec3 irradiance_sum = vec3(0.0);
	float weight_sum = 0.0;
	vec4 source_weight_sum = vec4(0.0);
	uint normal_dir_index = rt_strc_direction_index(normal_n);

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

				vec3 probe_world = rt_strc_probe_world_position(cascade_center, spacing, probe_coord, grid);
				vec3 probe_to_point = world_pos - probe_world;
				float receiver_distance = length(probe_to_point);
				vec3 probe_to_point_dir = receiver_distance > 1e-4 ? probe_to_point / receiver_distance : normal_n;
				vec3 point_to_probe_dir = -probe_to_point_dir;
				float normal_weight = mix(0.08, 1.0, pow(max(dot(normal_n, point_to_probe_dir), 0.0), 2.0));
				uint probe_index = rt_strc_probe_index(cascade, probe_coord, grid);

				uint visibility_dir_index = rt_strc_direction_index(probe_to_point_dir);
				vec4 distance_sample = imageLoad(rt_strc_distance_image, rt_strc_atlas_coord(probe_index, visibility_dir_index, grid));
				float visibility = rt_strc_distance_visibility(distance_sample, receiver_distance, spacing);
				if (visibility <= 0.001) {
					continue;
				}

				ivec2 irradiance_coord = rt_strc_atlas_coord(probe_index, normal_dir_index, grid);
				vec4 irradiance_sample = imageLoad(rt_strc_irradiance_image, irradiance_coord);
				vec4 metadata_sample = imageLoad(rt_strc_metadata_image, irradiance_coord);
				float variance = clamp(sqrt(max(distance_sample.y, 0.0)) / max(distance_sample.x, 0.25), 0.0, 1.0);
				float variance_confidence = 1.0 - smoothstep(0.20, 0.80, variance);
				float dynamic_confidence = 1.0 - clamp(metadata_sample.y, 0.0, 1.0) * 0.85;
				float age_confidence = rt_strc_age_confidence(clamp(metadata_sample.x, 0.0, 65504.0), cascade, cascade_count, grid);
				uint source_mask = uint(clamp(metadata_sample.z, 0.0, 15.0) + 0.5);
				float source_quality = rt_strc_source_quality(source_mask);
				float sample_weight = trilinear_weight * normal_weight * visibility * variance_confidence * dynamic_confidence * age_confidence * source_quality * clamp(irradiance_sample.a, 0.0, 1.0);
				if (sample_weight <= 0.0) {
					continue;
				}

				irradiance_sum += sanitize_payload_vec3(irradiance_sample.rgb) * sample_weight;
				weight_sum += sample_weight;
				source_weight_sum.x += (source_mask & STRC_SOURCE_MASK_DIRECT) != 0u ? sample_weight : 0.0;
				source_weight_sum.y += (source_mask & STRC_SOURCE_MASK_EMISSIVE) != 0u ? sample_weight : 0.0;
				source_weight_sum.z += (source_mask & STRC_SOURCE_MASK_SKY) != 0u ? sample_weight : 0.0;
				source_weight_sum.w += (source_mask & STRC_SOURCE_MASK_INDIRECT) != 0u ? sample_weight : 0.0;
			}
			break;
		}
	}

	if (weight_sum <= 1e-5) {
		return vec3(0.0);
	}

	confidence = clamp(weight_sum, 0.0, 1.0);
	float source_threshold = max(weight_sum * 0.22, 1e-5);
	if (source_weight_sum.x >= source_threshold) {
		dominant_source_mask |= STRC_SOURCE_MASK_DIRECT;
	}
	if (source_weight_sum.y >= source_threshold) {
		dominant_source_mask |= STRC_SOURCE_MASK_EMISSIVE;
	}
	if (source_weight_sum.z >= source_threshold) {
		dominant_source_mask |= STRC_SOURCE_MASK_SKY;
	}
	if (dominant_source_mask == 0u && source_weight_sum.w > 1e-5) {
		dominant_source_mask = STRC_SOURCE_MASK_INDIRECT;
	}
	return irradiance_sum / weight_sum;
}

vec3 rt_strc_sample_irradiance(vec3 world_pos, vec3 normal, out float confidence) {
	uint ignored_source_mask = 0u;
	return rt_strc_sample_irradiance_with_source(world_pos, normal, confidence, ignored_source_mask);
}

bool rt_strc_guided_diffuse_direction(vec3 world_pos, vec3 normal, uint receiver_layer_mask, out vec3 guided_dir, out float confidence) {
	guided_dir = vec3(0.0, 1.0, 0.0);
	confidence = 0.0;
	if (!rt_strc_resources_available() ||
			rt_strc_probe_update_mode() ||
			!(rt_strc_enabled() || rt_strc_internal_fallback_enabled()) ||
			!rt_strc_visual_layer_visible(receiver_layer_mask)) {
		return false;
	}

	uint grid = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_GRID_SIZE)), 12u, 32u);
	uint cascade_count = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_CASCADE_COUNT)), 1u, 4u);
	float base_spacing = max(get_rt_param(RT_PARAM_RTGI_STRC_BASE_PROBE_SPACING), 0.25);
	vec3 camera_origin = rt_camera_world_origin();
	vec3 normal_n = normalize(normal);

	float best_score = 0.0;
	vec3 best_dir = guided_dir;
	for (uint cascade = 0u; cascade < 4u; cascade++) {
		if (cascade >= cascade_count) {
			break;
		}

		float spacing = base_spacing * exp2(float(cascade));
		vec3 cascade_center = floor(camera_origin / spacing) * spacing;
		vec3 probe_space = (world_pos - cascade_center) / spacing + vec3(float(grid) * 0.5) - vec3(0.5);
		ivec3 probe_i = ivec3(floor(probe_space + vec3(0.5)));
		if (any(lessThan(probe_i, ivec3(0))) || any(greaterThanEqual(probe_i, ivec3(int(grid))))) {
			continue;
		}

		uvec3 probe_coord = uvec3(probe_i);
		vec3 probe_world = rt_strc_probe_world_position(cascade_center, spacing, probe_coord, grid);
		vec3 probe_to_point = world_pos - probe_world;
		float receiver_distance = length(probe_to_point);
		vec3 probe_to_point_dir = receiver_distance > 1e-4 ? probe_to_point / receiver_distance : normal_n;
		uint probe_index = rt_strc_probe_index(cascade, probe_coord, grid);
		uint visibility_dir_index = rt_strc_direction_index(probe_to_point_dir);
		vec4 distance_sample = imageLoad(rt_strc_distance_image, rt_strc_atlas_coord(probe_index, visibility_dir_index, grid));
		float receiver_visibility = rt_strc_distance_visibility(distance_sample, receiver_distance, spacing);
		if (receiver_visibility <= 0.001) {
			continue;
		}

		vec3 axis = abs(normal_n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
		vec3 tangent = normalize(cross(axis, normal_n));
		vec3 bitangent = cross(normal_n, tangent);
		for (uint candidate = 0u; candidate < 6u; candidate++) {
			vec3 candidate_dir = normal_n;
			if (candidate == 1u) {
				candidate_dir = normalize(normal_n + tangent * 0.85);
			} else if (candidate == 2u) {
				candidate_dir = normalize(normal_n - tangent * 0.85);
			} else if (candidate == 3u) {
				candidate_dir = normalize(normal_n + bitangent * 0.85);
			} else if (candidate == 4u) {
				candidate_dir = normalize(normal_n - bitangent * 0.85);
			} else if (candidate == 5u) {
				candidate_dir = normalize(normal_n + vec3(0.0, 1.0, 0.0) * 0.70);
			}
			float normal_cos = dot(normal_n, candidate_dir);
			if (normal_cos <= 0.035) {
				continue;
			}

			uint dir_index = rt_strc_direction_index(candidate_dir);
			ivec2 atlas_coord = rt_strc_atlas_coord(probe_index, dir_index, grid);
			vec4 radiance_sample = imageLoad(rt_strc_irradiance_image, atlas_coord);
			vec4 metadata_sample = imageLoad(rt_strc_metadata_image, atlas_coord);
			float radiance_luma = rt_luminance(sanitize_payload_vec3(radiance_sample.rgb));
			float radiance_weight = smoothstep(0.0005, 0.0080, radiance_luma);
			if (radiance_weight <= 0.001) {
				continue;
			}

			uint source_mask = uint(clamp(metadata_sample.z, 0.0, 15.0) + 0.5);
			float source_quality = rt_strc_source_quality(source_mask);
			float dynamic_confidence = 1.0 - clamp(metadata_sample.y, 0.0, 1.0) * 0.85;
			float age_confidence = rt_strc_age_confidence(clamp(metadata_sample.x, 0.0, 65504.0), cascade, cascade_count, grid);
			float sample_confidence = clamp(radiance_sample.a, 0.0, 1.0) * source_quality * dynamic_confidence * age_confidence;
			float score = radiance_weight * sample_confidence * receiver_visibility * pow(normal_cos, 0.65);
			if (score > best_score) {
				best_score = score;
				best_dir = candidate_dir;
			}
		}

		if (best_score > 0.001) {
			break;
		}
	}

	if (best_score <= 0.001) {
		return false;
	}

	guided_dir = normalize(best_dir);
	confidence = clamp(best_score * 2.75, 0.0, 1.0);
	return confidence > 0.025;
}

#define RTGI_SECONDARY_CACHE_SOURCE_NONE 0u
#define RTGI_SECONDARY_CACHE_SOURCE_RECEIVER 1u
#define RTGI_SECONDARY_CACHE_SOURCE_STRC 2u
#define RTGI_SECONDARY_CACHE_SOURCE_SPG 3u
#define RTGI_SECONDARY_CACHE_SOURCE_REFINED_SPG 4u
#define RTGI_SECONDARY_CACHE_SOURCE_SURFACE 5u

uint rtgi_surface_source_from_secondary_cache_source(uint cache_source) {
	if (cache_source == RTGI_SECONDARY_CACHE_SOURCE_RECEIVER) {
		return RTGI_SURFACE_SOURCE_RECEIVER;
	}
	if (cache_source == RTGI_SECONDARY_CACHE_SOURCE_STRC) {
		return RTGI_SURFACE_SOURCE_STRC;
	}
	if (cache_source == RTGI_SECONDARY_CACHE_SOURCE_SPG) {
		return RTGI_SURFACE_SOURCE_BASE_SPG;
	}
	if (cache_source == RTGI_SECONDARY_CACHE_SOURCE_REFINED_SPG) {
		return RTGI_SURFACE_SOURCE_REFINED_SPG;
	}
	return RTGI_SURFACE_SOURCE_NONE;
}

#define RTGI_SECONDARY_CACHE_REJECTION_NONE 0u
#define RTGI_SECONDARY_CACHE_REJECTION_INELIGIBLE 1u
#define RTGI_SECONDARY_CACHE_REJECTION_NO_SOURCE 2u
#define RTGI_SECONDARY_CACHE_REJECTION_RECEIVER_WEAK 3u
#define RTGI_SECONDARY_CACHE_REJECTION_REFINED_SPG_WEAK 4u
#define RTGI_SECONDARY_CACHE_REJECTION_BASE_SPG_WEAK 5u
#define RTGI_SECONDARY_CACHE_REJECTION_STRC_WEAK 6u
#define RTGI_SECONDARY_CACHE_REJECTION_SCREEN_NO_HIT 7u
#define RTGI_SECONDARY_CACHE_REJECTION_SCREEN_LOW_WEIGHT 8u
#define RTGI_SECONDARY_CACHE_REJECTION_SURFACE_WEAK 9u

float rtgi_secondary_cache_source_bucket(uint cache_source) {
	return clamp(float(cache_source) / float(RTGI_SECONDARY_CACHE_SOURCE_SURFACE), 0.0, 1.0);
}

float rtgi_secondary_cache_rejection_bucket(uint rejection_reason) {
	return clamp(float(rejection_reason) / float(RTGI_SECONDARY_CACHE_REJECTION_SURFACE_WEAK), 0.0, 1.0);
}

float rtgi_surface_cache_query_bucket(uint query_reason) {
	return clamp(float(query_reason) / float(RTGI_SURFACE_CACHE_QUERY_WEAK_QUALITY), 0.0, 1.0);
}

void rtgi_secondary_cache_source_record(ivec2 pixel, uint cache_source, float cache_weight, float cached_luminance, float query_kind) {
	if (rt_strc_probe_update_mode()) {
		return;
	}

	imageStore(rt_secondary_cache_source_image, pixel, vec4(
			rtgi_secondary_cache_source_bucket(cache_source),
			clamp(cache_weight, 0.0, 1.0),
			clamp(query_kind, 0.0, 1.0),
			rt_source_debug_log(cached_luminance, 1.0)));
}

void rtgi_secondary_cache_rejection_record(ivec2 pixel, uint rejection_reason, float query_kind, float detail, uint fallback_source) {
	if (rt_strc_probe_update_mode()) {
		return;
	}

	imageStore(rt_secondary_cache_rejection_image, pixel, vec4(
			rtgi_secondary_cache_rejection_bucket(rejection_reason),
			clamp(query_kind, 0.0, 1.0),
			clamp(detail, 0.0, 1.0),
			rtgi_secondary_cache_source_bucket(fallback_source)));
}

void rtgi_surface_cache_query_record(ivec2 pixel, uint query_reason, float query_detail, float query_kind, uint query_source) {
	if (rt_strc_probe_update_mode()) {
		return;
	}

	imageStore(rt_secondary_cache_surface_image, pixel, vec4(
			rtgi_surface_cache_query_bucket(query_reason),
			rtgi_surface_source_bucket(query_source),
			clamp(query_kind, 0.0, 1.0),
			clamp(query_detail, 0.0, 1.0)));
}

void rtgi_secondary_cache_rejection_candidate(inout uint rejection_reason, inout float rejection_detail, uint candidate_reason, bool candidate_valid, float candidate_weight, float candidate_threshold) {
	float candidate_detail = candidate_valid ? clamp(candidate_weight / max(candidate_threshold, 0.0001), 0.0, 1.0) : 0.0;
	if (candidate_detail > rejection_detail) {
		rejection_detail = candidate_detail;
		rejection_reason = candidate_reason;
	}
}

bool rtgi_sample_secondary_diffuse_cache(vec3 world_pos, vec3 normal, float roughness, float metalness, vec3 albedo_proxy, uint receiver_layer_mask, uint surface_key, uint surface_key_reason, float query_kind, out vec3 cached_lighting, out float cache_weight, out uint cache_source, out uint cache_rejection, out float cache_rejection_detail) {
	cached_lighting = vec3(0.0);
	cache_weight = 0.0;
	cache_source = RTGI_SECONDARY_CACHE_SOURCE_NONE;
	cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_NONE;
	cache_rejection_detail = 0.0;
	ivec2 query_pixel = ivec2(gl_LaunchIDEXT.xy);
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			roughness <= 0.35 ||
			metalness >= 0.55) {
		cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_INELIGIBLE;
		rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_INELIGIBLE, 0.0, query_kind, RTGI_SURFACE_SOURCE_NONE);
		return false;
	}

	vec3 normal_n = normalize(normal);
	float receiver_rough_weight = smoothstep(0.38, 0.92, roughness);
	float strc_rough_weight = smoothstep(0.35, 0.90, roughness);
	float receiver_metal_weight = 1.0 - smoothstep(0.15, 0.55, clamp(metalness, 0.0, 1.0));
	float strc_metal_weight = 1.0 - smoothstep(0.25, 0.75, clamp(metalness, 0.0, 1.0));
	uint best_rejection = RTGI_SECONDARY_CACHE_REJECTION_NO_SOURCE;
	float best_rejection_detail = 0.0;

	vec3 receiver_lighting = vec3(0.0);
	float receiver_weight = 0.0;
	bool receiver_valid = rtgi_diffuse_cache_sample_receiver(world_pos, normal_n, roughness, albedo_proxy, receiver_lighting, receiver_weight);
	if (receiver_valid) {
		receiver_weight = clamp(receiver_weight * receiver_rough_weight * receiver_metal_weight, 0.0, 0.46);
		if (receiver_weight >= 0.085) {
			cached_lighting = receiver_lighting;
			cache_weight = receiver_weight;
			cache_source = RTGI_SECONDARY_CACHE_SOURCE_RECEIVER;
			rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(receiver_weight / 0.46, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_RECEIVER);
			return true;
		}
	}
	rtgi_secondary_cache_rejection_candidate(best_rejection, best_rejection_detail, RTGI_SECONDARY_CACHE_REJECTION_RECEIVER_WEAK, receiver_valid, receiver_weight, 0.085);

	vec3 refined_spg_lighting = vec3(0.0);
	float refined_spg_weight = 0.0;
	bool refined_spg_valid = rtgi_diffuse_cache_sample_refined_directional_spg(world_pos, normal_n, roughness, albedo_proxy, refined_spg_lighting, refined_spg_weight);
	if (refined_spg_valid) {
		refined_spg_weight = clamp(refined_spg_weight * receiver_rough_weight * receiver_metal_weight, 0.0, 0.18);
		if (refined_spg_weight >= 0.065 && (!receiver_valid || receiver_weight <= 0.035)) {
			cached_lighting = refined_spg_lighting;
			cache_weight = refined_spg_weight;
			cache_source = RTGI_SECONDARY_CACHE_SOURCE_REFINED_SPG;
			rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(refined_spg_weight / 0.18, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_REFINED_SPG);
			return true;
		}
	}
	rtgi_secondary_cache_rejection_candidate(best_rejection, best_rejection_detail, RTGI_SECONDARY_CACHE_REJECTION_REFINED_SPG_WEAK, refined_spg_valid, refined_spg_weight, 0.065);

	vec3 spg_lighting = vec3(0.0);
	float spg_weight = 0.0;
	bool spg_valid = rtgi_diffuse_cache_sample_directional_spg(world_pos, normal_n, roughness, albedo_proxy, spg_lighting, spg_weight);
	if (spg_valid) {
		spg_weight = clamp(spg_weight * receiver_rough_weight * receiver_metal_weight, 0.0, 0.22);
		if (spg_weight >= 0.080 && (!receiver_valid || receiver_weight <= 0.025)) {
			cached_lighting = spg_lighting;
			cache_weight = spg_weight;
			cache_source = RTGI_SECONDARY_CACHE_SOURCE_SPG;
			rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(spg_weight / 0.22, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_BASE_SPG);
			return true;
		}
	}
	rtgi_secondary_cache_rejection_candidate(best_rejection, best_rejection_detail, RTGI_SECONDARY_CACHE_REJECTION_BASE_SPG_WEAK, spg_valid, spg_weight, 0.080);

	vec3 surface_lighting = vec3(0.0);
	float surface_weight = 0.0;
	uint surface_query_reason = RTGI_SURFACE_CACHE_QUERY_NONE;
	float surface_query_detail = 0.0;
	float surface_query_luminance = 0.0;
	uint surface_query_source = RTGI_SURFACE_SOURCE_NONE;
	bool surface_valid = rtgi_diffuse_cache_sample_surface(surface_key, surface_key_reason, normal_n, roughness, surface_lighting, surface_weight, surface_query_reason, surface_query_detail, surface_query_luminance, surface_query_source);
	if (surface_valid) {
		surface_weight = clamp(surface_weight * receiver_rough_weight * receiver_metal_weight, 0.0, 0.15);
		if (surface_weight >= 0.045 && (!receiver_valid || receiver_weight <= 0.035)) {
			cached_lighting = surface_lighting;
			cache_weight = surface_weight;
			cache_source = RTGI_SECONDARY_CACHE_SOURCE_SURFACE;
			rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_ACCEPTED, clamp(surface_weight / 0.15, 0.0, 1.0), query_kind, surface_query_source);
			return true;
		}
		surface_query_reason = RTGI_SURFACE_CACHE_QUERY_WEAK_QUALITY;
		surface_query_detail = clamp(surface_weight / 0.045, 0.0, 1.0);
	}
	rtgi_surface_cache_query_record(query_pixel, surface_query_reason, surface_query_detail, query_kind, surface_query_source);
	rtgi_secondary_cache_rejection_candidate(best_rejection, best_rejection_detail, RTGI_SECONDARY_CACHE_REJECTION_SURFACE_WEAK, surface_valid, surface_weight, 0.045);

	if ((rt_strc_enabled() || rt_strc_internal_fallback_enabled()) && rt_strc_visual_layer_visible(receiver_layer_mask)) {
		float strc_confidence = 0.0;
		uint strc_source_mask = 0u;
		vec3 strc_irradiance = rt_strc_sample_irradiance_with_source(world_pos + normal_n * 0.05, normal_n, strc_confidence, strc_source_mask);
		uint strc_surface_source = rtgi_surface_source_from_strc_mask(strc_source_mask);
		if (strc_surface_source == RTGI_SURFACE_SOURCE_NONE) {
			strc_surface_source = RTGI_SURFACE_SOURCE_STRC;
		}
		float strc_strength = rt_strc_enabled() ? get_rt_param(RT_PARAM_RTGI_STRC_STRENGTH) : 0.18;
		float strc_cap = rt_strc_enabled() ? 0.38 : 0.12;
		float strc_radiance_weight = smoothstep(0.0005, 0.0060, rt_luminance(strc_irradiance));
		float strc_weight = clamp(strc_confidence * strc_rough_weight * strc_metal_weight * strc_strength * strc_radiance_weight * 0.55, 0.0, strc_cap);
		if (strc_weight > 0.012 && (!receiver_valid || receiver_weight < strc_weight * 0.75)) {
			cached_lighting = sanitize_payload_vec3(strc_irradiance);
			cache_weight = strc_weight;
			cache_source = RTGI_SECONDARY_CACHE_SOURCE_STRC;
			rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(strc_weight / max(strc_cap, 0.001), 0.0, 1.0), query_kind, strc_surface_source);
			return true;
		}
		rtgi_secondary_cache_rejection_candidate(best_rejection, best_rejection_detail, RTGI_SECONDARY_CACHE_REJECTION_STRC_WEAK, strc_confidence > 0.001, strc_weight, 0.012);
	}

	if (receiver_valid && receiver_weight > 0.025) {
		cached_lighting = receiver_lighting;
		cache_weight = receiver_weight;
		cache_source = RTGI_SECONDARY_CACHE_SOURCE_RECEIVER;
		rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(receiver_weight / 0.46, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_RECEIVER);
		return true;
	}

	if (spg_valid && spg_weight > 0.035 && (!receiver_valid || receiver_weight <= 0.025)) {
		cached_lighting = spg_lighting;
		cache_weight = spg_weight;
		cache_source = RTGI_SECONDARY_CACHE_SOURCE_SPG;
		rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(spg_weight / 0.22, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_BASE_SPG);
		return true;
	}

	if (refined_spg_valid && refined_spg_weight > 0.030 && (!receiver_valid || receiver_weight <= 0.025)) {
		cached_lighting = refined_spg_lighting;
		cache_weight = refined_spg_weight;
		cache_source = RTGI_SECONDARY_CACHE_SOURCE_REFINED_SPG;
		rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_EARLY_SOURCE, clamp(refined_spg_weight / 0.18, 0.0, 1.0), query_kind, RTGI_SURFACE_SOURCE_REFINED_SPG);
		return true;
	}

	if (surface_valid && surface_weight > 0.022 && (!receiver_valid || receiver_weight <= 0.020)) {
		cached_lighting = surface_lighting;
		cache_weight = surface_weight;
		cache_source = RTGI_SECONDARY_CACHE_SOURCE_SURFACE;
		rtgi_surface_cache_query_record(query_pixel, RTGI_SURFACE_CACHE_QUERY_ACCEPTED, clamp(surface_weight / 0.15, 0.0, 1.0), query_kind, surface_query_source);
		return true;
	}

	cache_rejection = best_rejection;
	cache_rejection_detail = best_rejection_detail;
	return false;
}

bool rtgi_screen_trace_secondary_diffuse_cache(vec3 ray_origin, vec3 ray_dir, vec3 origin_normal, float origin_roughness, float origin_metalness, uint receiver_layer_mask, out vec3 cached_lighting, out vec3 hit_albedo_proxy, out float cache_weight, out uint cache_source, out uint cache_rejection, out float cache_rejection_detail) {
	cached_lighting = vec3(0.0);
	hit_albedo_proxy = vec3(1.0);
	cache_weight = 0.0;
	cache_source = RTGI_SECONDARY_CACHE_SOURCE_NONE;
	cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_NONE;
	cache_rejection_detail = 0.0;
	if (rt_strc_probe_update_mode() ||
			uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED ||
			origin_roughness <= 0.45 ||
			origin_metalness >= 0.45) {
		cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_INELIGIBLE;
		return false;
	}

	vec3 hit_pos = vec3(0.0);
	vec3 hit_normal = vec3(0.0, 1.0, 0.0);
	float hit_roughness = 1.0;
	float trace_confidence = 0.0;
	uint hit_surface_key = 0u;
	float max_distance = mix(3.5, 14.0, smoothstep(0.45, 0.95, origin_roughness));
	if (!rtgi_trace_current_raster_hit(ray_origin, ray_dir, origin_normal, max_distance, hit_pos, hit_normal, hit_roughness, hit_albedo_proxy, trace_confidence, hit_surface_key)) {
		cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_SCREEN_NO_HIT;
		return false;
	}

	vec3 screen_cached = vec3(0.0);
	float screen_cache_weight = 0.0;
	uint screen_cache_source = RTGI_SECONDARY_CACHE_SOURCE_NONE;
	uint screen_cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_NONE;
	float screen_cache_rejection_detail = 0.0;
	uint screen_surface_key_reason = hit_surface_key != 0u ? RTGI_SURFACE_KEY_REASON_VALID : RTGI_SURFACE_KEY_REASON_RASTER;
	if (!rtgi_sample_secondary_diffuse_cache(hit_pos + hit_normal * 0.04, hit_normal, hit_roughness, 0.0, hit_albedo_proxy, receiver_layer_mask, hit_surface_key, screen_surface_key_reason, 0.75, screen_cached, screen_cache_weight, screen_cache_source, screen_cache_rejection, screen_cache_rejection_detail)) {
		cache_rejection = screen_cache_rejection;
		cache_rejection_detail = screen_cache_rejection_detail;
		return false;
	}

	float rough_weight = smoothstep(0.48, 0.92, origin_roughness);
	float radiance_weight = smoothstep(0.0005, 0.0060, rt_luminance(screen_cached));
	float source_cap = screen_cache_source == RTGI_SECONDARY_CACHE_SOURCE_STRC ? 0.18 : (screen_cache_source == RTGI_SECONDARY_CACHE_SOURCE_SURFACE ? 0.14 : ((screen_cache_source == RTGI_SECONDARY_CACHE_SOURCE_SPG || screen_cache_source == RTGI_SECONDARY_CACHE_SOURCE_REFINED_SPG) ? 0.16 : 0.30));
	cache_weight = clamp(trace_confidence * screen_cache_weight * rough_weight * radiance_weight * 0.72, 0.0, source_cap);
	if (cache_weight <= 0.012) {
		cache_rejection = RTGI_SECONDARY_CACHE_REJECTION_SCREEN_LOW_WEIGHT;
		cache_rejection_detail = clamp(cache_weight / 0.012, 0.0, 1.0);
		cache_weight = 0.0;
		cache_source = RTGI_SECONDARY_CACHE_SOURCE_NONE;
		return false;
	}

	uint screen_feedback_source = rtgi_surface_source_from_secondary_cache_source(screen_cache_source);
	if (hit_surface_key != 0u && screen_feedback_source != RTGI_SURFACE_SOURCE_NONE) {
		rtgi_surface_cache_feedback_record_source(
				ivec2(gl_LaunchIDEXT.xy),
				hit_surface_key,
				hit_normal,
				hit_roughness,
				screen_cached,
				clamp(trace_confidence * 0.50 + screen_cache_weight * 0.38, 0.0, 0.74),
				clamp(screen_cache_weight * 0.62 + trace_confidence * 0.22, 0.0, 0.78),
				screen_feedback_source);
	}

	cached_lighting = sanitize_payload_vec3(screen_cached);
	cache_source = screen_cache_source;
	return true;
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
	imageStore(rt_source_direct_reservoir_image, pixel, vec4(0.0));
	imageStore(rt_source_direct_reservoir_lighting_image, pixel, vec4(0.0));
	imageStore(rt_source_history_image, pixel, vec4(0.0));
	imageStore(rt_source_temporal_delta_image, pixel, vec4(0.0));
	imageStore(rt_source_rejection_image, pixel, vec4(0.0));
	imageStore(rt_secondary_cache_source_image, pixel, vec4(0.0));
	imageStore(rt_secondary_cache_rejection_image, pixel, vec4(0.0));
	imageStore(rt_secondary_cache_surface_image, pixel, vec4(0.0));
	rtgi_surface_cache_feedback_reset(pixel);
	imageStore(rt_primary_diffuse_direction_image, pixel, vec4(0.0));
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

bool rt_source_load_previous_direct_reservoir_at(ivec2 previous_pixel, out vec4 previous_reservoir, out vec4 previous_lighting, out uint previous_key, out uint reject_reason) {
	ivec2 extent_i = ivec2(rt_extent());
	if (any(lessThan(previous_pixel, ivec2(0))) || any(greaterThanEqual(previous_pixel, extent_i))) {
		previous_reservoir = vec4(0.0);
		previous_lighting = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_reservoir = imageLoad(rt_source_direct_reservoir_prev_image, previous_pixel);
	previous_lighting = imageLoad(rt_source_direct_reservoir_lighting_prev_image, previous_pixel);
	previous_key = imageLoad(rt_source_direct_candidate_key_prev_image, previous_pixel).r;
	if (previous_key == 0u || previous_reservoir.z <= 0.0 || previous_reservoir.y <= 0.0 || previous_lighting.a <= 0.0) {
		reject_reason = RT_SOURCE_REJECT_PDF_WEIGHT;
		return false;
	}

	reject_reason = RT_SOURCE_REJECT_NONE;
	return true;
}

bool rt_source_load_reprojected_previous_direct(ivec2 pixel, out ivec2 previous_pixel, out vec4 previous_reservoir, out vec4 previous_lighting, out uint previous_key, out uint reject_reason) {
	vec2 extent = rt_extent();
	vec2 current_texture_uv = (vec2(pixel) + vec2(0.5)) / extent;
	vec2 previous_texture_uv = current_texture_uv + imageLoad(rt_velocity_image, pixel).xy;
	if (any(lessThan(previous_texture_uv, vec2(0.0))) || any(greaterThanEqual(previous_texture_uv, vec2(1.0)))) {
		previous_pixel = ivec2(0);
		previous_reservoir = vec4(0.0);
		previous_lighting = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	previous_pixel = ivec2(floor(previous_texture_uv * extent));
	ivec2 extent_i = ivec2(extent);
	if (any(lessThan(previous_pixel, ivec2(0))) || any(greaterThanEqual(previous_pixel, extent_i))) {
		previous_reservoir = vec4(0.0);
		previous_lighting = vec4(0.0);
		previous_key = 0u;
		reject_reason = RT_SOURCE_REJECT_PREV_UV;
		return false;
	}

	return rt_source_load_previous_direct_reservoir_at(previous_pixel, previous_reservoir, previous_lighting, previous_key, reject_reason);
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
	if (previous_candidate.w < 0.75 || current_confidence < 0.75) {
		reject_reason = RT_SOURCE_REJECT_LOW_CONFIDENCE;
		return false;
	}
	if (any(isnan(previous_candidate)) || any(isinf(previous_candidate)) || isnan(current_normalized_weight) || isinf(current_normalized_weight) || previous_candidate.x <= 0.0 || previous_candidate.y <= 0.0 || previous_candidate.z <= 0.0 || current_normalized_weight <= 0.0) {
		reject_reason = RT_SOURCE_REJECT_PDF_WEIGHT;
		return false;
	}
	if (rt_source_ratio(previous_candidate.x, current_normalized_weight) < 0.25) {
		reject_reason = RT_SOURCE_REJECT_WEIGHT_RATIO;
		return false;
	}

	reject_reason = RT_SOURCE_REJECT_NONE;
	return true;
}

void rt_source_direct_reservoir_record(ivec2 pixel, uint source_key, float target_pdf, float weight_sum, float reservoir_m, float confidence, vec3 selected_contribution, float selected_target) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	bool valid_source = source_key != 0u && (source_key >> RT_SOURCE_CLASS_SHIFT) == RT_SOURCE_CLASS_DIRECT;
	bool valid_reservoir = valid_source && reservoir_m > 0.0 && weight_sum > 0.0 && target_pdf > 0.0 && selected_target > 0.0;
	if (!valid_reservoir) {
		imageStore(rt_source_direct_reservoir_image, pixel, vec4(0.0));
		imageStore(rt_source_direct_reservoir_lighting_image, pixel, vec4(0.0));
		imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(0u));
		return;
	}

	imageStore(rt_source_direct_reservoir_image, pixel, vec4(
			clamp(target_pdf, 0.0, 65504.0),
			clamp(weight_sum, 0.0, 65504.0),
			clamp(reservoir_m, 0.0, 65504.0),
			clamp(confidence, 0.0, 1.0)));
	imageStore(rt_source_direct_reservoir_lighting_image, pixel, vec4(
			clamp(sanitize_payload_vec3(selected_contribution), vec3(0.0), vec3(65504.0)),
			clamp(selected_target, 0.0, 65504.0)));
	imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(source_key, 0u, 0u, 0u));
}

void rt_source_direct_candidate_record(ivec2 pixel, uint source_key, float confidence, float normalized_weight, vec3 contribution, bool stochastic_candidate_mode, float reservoir_m, float weight_sum, float selected_target, bool temporal_accepted, bool spatial_accepted, uint temporal_reject, uint spatial_reject, uint visibility_failures) {
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
			clamp(reservoir_m / 32.0, 0.0, 1.0),
			clamp(stored_confidence, 0.0, 1.0),
			max(clamp(normalized_weight, 0.0, 1.0), rt_source_debug_log(weight_sum, 1.0)),
			max(clamp(contribution_luma, 0.0, 1.0), rt_source_debug_log(selected_target, 1.0))));
	imageStore(rt_source_direct_candidate_key_image, pixel, uvec4(source_key, 0u, 0u, 0u));
	imageStore(rt_source_candidate_image, pixel, vec4(
			0.25,
			clamp(reservoir_m / 32.0, 0.0, 1.0),
			rt_source_debug_log(weight_sum, 1.0),
			max(clamp(contribution_luma, 0.0, 1.0), rt_source_debug_log(selected_target, 1.0))));
	imageStore(rt_source_candidate_key_image, pixel, uvec4(source_key, 0u, 0u, 0u));
	imageStore(rt_source_history_image, pixel, vec4(
			0.25,
			temporal_accepted ? 1.0 : 0.0,
			spatial_accepted ? 1.0 : 0.0,
			clamp(reservoir_m / 32.0, 0.0, 1.0)));
	imageStore(rt_source_temporal_delta_image, pixel, vec4(
			temporal_reject == RT_SOURCE_REJECT_NONE ? 1.0 : 0.0,
			spatial_reject == RT_SOURCE_REJECT_NONE ? 1.0 : 0.0,
			clamp(float(visibility_failures) / 4.0, 0.0, 1.0),
			1.0));
	imageStore(rt_source_rejection_image, pixel, vec4(
			0.25,
			float(temporal_reject) / 15.0,
			float(spatial_reject) / 15.0,
			clamp(float(visibility_failures) / 4.0, 0.0, 1.0)));
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
		vec4 previous_direct_lighting = vec4(0.0);
		uint reject_reason = RT_SOURCE_REJECT_NONE;
		bool direct_reprojected = false;
		if (current_class_key == RT_SOURCE_CLASS_DIRECT) {
			ivec2 previous_pixel = ivec2(0);
			direct_reprojected = rt_source_load_reprojected_previous_direct(pixel, previous_pixel, previous_candidate, previous_direct_lighting, previous_key, reject_reason);
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
		float previous_luma = current_class_key == RT_SOURCE_CLASS_DIRECT ? previous_direct_lighting.a : previous_candidate.a;
		float temporal_delta = class_agrees ? abs(contribution_luma - previous_luma) : 0.0;
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

void rt_store_path_traced_visible_velocity(ivec2 pixel, vec2 velocity) {
	if (uint(get_rt_param(RT_PARAM_MODE)) != RT_MODE_PATH_TRACED) {
		return;
	}

	ivec2 visible_pixel = pixel - ivec2(round(rt_current_origin()));
	ivec2 visible_size_i = ivec2(round(rt_visible_size()));
	if (!all(greaterThanEqual(visible_pixel, ivec2(0))) || !all(lessThan(visible_pixel, visible_size_i))) {
		return;
	}

	ivec2 output_size = max(ivec2(round(scene_data_block.data.viewport_size)), ivec2(1));
	vec2 uv_min = vec2(visible_pixel) / vec2(visible_size_i);
	vec2 uv_max = vec2(visible_pixel + ivec2(1)) / vec2(visible_size_i);
	ivec2 output_min = clamp(ivec2(ceil(uv_min * vec2(output_size) - vec2(0.5))), ivec2(0), output_size - ivec2(1));
	ivec2 output_max = clamp(ivec2(floor(uv_max * vec2(output_size) - vec2(0.5))), output_min, output_size - ivec2(1));

	for (int y = output_min.y; y <= output_max.y; y++) {
		for (int x = output_min.x; x <= output_max.x; x++) {
			imageStore(rt_visible_velocity_image, ivec2(x, y), vec4(velocity, 0.0, 0.0));
		}
	}
}

void rt_store_primary_velocity(ivec2 pixel, vec2 curr_visible_uv, vec2 prev_visible_uv) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	vec2 curr_texture_uv = rt_visible_to_texture_uv(curr_visible_uv, rt_current_origin());
	vec2 prev_texture_uv = rt_visible_to_texture_uv(prev_visible_uv, rt_previous_origin());
	imageStore(rt_velocity_image, pixel, vec4(prev_texture_uv - curr_texture_uv, 0.0, 0.0));
	rt_store_path_traced_visible_velocity(pixel, prev_visible_uv - curr_visible_uv);
}

void rt_store_invalid_primary_velocity(ivec2 pixel) {
	if (rt_strc_probe_update_mode()) {
		return;
	}
	imageStore(rt_velocity_image, pixel, vec4(0.0));
	rt_store_path_traced_visible_velocity(pixel, vec2(0.0));
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
