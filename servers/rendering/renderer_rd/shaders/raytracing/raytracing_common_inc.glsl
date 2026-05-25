// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant (bits 0-20: flags, 21-28: samples, 29-31: bounces).
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define RT_FLAG_DLSS_RR_ENABLED (1u << 1)
#define RT_FLAG_FOG_ENABLED (1u << 2)

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

#ifndef RT_STAGE_ANY_HIT

layout(set = 0, binding = 6, std140) uniform RaytracingParams {
	vec4 rt_params[7];
	mat4 prev_vp_unjittered;
	mat4 curr_vp_unjittered;
	mat4 inv_projection_unjittered;
	vec4 rt_overscan;
	vec4 rt_prev_overscan;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
}

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
	return max(rt_overscan.zw, vec2(1.0));
}

vec2 rt_extent() {
	return max(rt_prev_overscan.zw, vec2(1.0));
}

vec2 rt_current_origin() {
	return rt_overscan.xy;
}

vec2 rt_previous_origin() {
	return rt_prev_overscan.xy;
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
layout(set = 0, binding = 39, rgba16f) uniform image2D rt_signal_direct_light_image;
layout(set = 0, binding = 40, rgba16f) uniform image2D rt_signal_emissive_image;
layout(set = 0, binding = 41, rgba16f) uniform image2D rt_signal_indirect_image;
layout(set = 0, binding = 42, rgba16f) uniform image2D rt_signal_sky_image;
layout(set = 0, binding = 43, rgba16f) uniform image2D rt_signal_confidence_image;

void rt_signal_reset(ivec2 pixel) {
	imageStore(rt_signal_direct_light_image, pixel, vec4(0.0));
	imageStore(rt_signal_emissive_image, pixel, vec4(0.0));
	imageStore(rt_signal_indirect_image, pixel, vec4(0.0));
	imageStore(rt_signal_sky_image, pixel, vec4(0.0));
	imageStore(rt_signal_confidence_image, pixel, vec4(0.0, 0.0, 0.0, 1.0));
}

#define RT_SIGNAL_ACCUMULATE(signal_image, pixel, value) imageStore(signal_image, pixel, imageLoad(signal_image, pixel) + (value))

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

void rt_signal_add_indirect(ivec2 pixel, vec3 throughput, uint total_bounces, int brdf_type, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(throughput);
	float encoded_type = brdf_type == 2 ? 2.0 : 1.0;
	RT_SIGNAL_ACCUMULATE(rt_signal_indirect_image, pixel, vec4(value, encoded_type + float(min(total_bounces, 30u)) / 32.0));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
}

void rt_signal_add_sky(ivec2 pixel, vec3 contribution, bool secondary_miss, float clamp_delta) {
	vec3 value = sanitize_payload_vec3(contribution);
	RT_SIGNAL_ACCUMULATE(rt_signal_sky_image, pixel, vec4(value, secondary_miss ? rt_luminance(value) : 0.0));
	RT_SIGNAL_ACCUMULATE(rt_signal_confidence_image, pixel, vec4(clamp_delta, 0.0, 0.0, 0.0));
}

void rt_signal_set_primary_confidence(ivec2 pixel, float specular_risk, float material_id, float validity) {
	vec4 confidence = imageLoad(rt_signal_confidence_image, pixel);
	confidence.b = material_id;
	confidence.a = validity;
	confidence.g = max(confidence.g, clamp(specular_risk, 0.0, 1.0));
	imageStore(rt_signal_confidence_image, pixel, confidence);
}

void rt_store_primary_velocity(ivec2 pixel, vec2 curr_visible_uv, vec2 prev_visible_uv) {
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
