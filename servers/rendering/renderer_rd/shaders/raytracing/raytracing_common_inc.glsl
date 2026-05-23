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
	vec4 rt_params[4];
	mat4 prev_vp_unjittered;
	mat4 curr_vp_unjittered;
	mat4 inv_projection_unjittered;
	vec4 rt_overscan;
	vec4 rt_prev_overscan;
};

float get_rt_param(uint idx) {
	return rt_params[idx >> 2u][idx & 3u];
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
