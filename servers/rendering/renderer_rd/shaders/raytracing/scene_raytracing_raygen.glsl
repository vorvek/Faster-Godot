#[raygen]

#version 460

#extension GL_EXT_control_flow_attributes : enable

#VERSION_DEFINES

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
// clang-format on

#pragma shader_stage(raygen)
#extension GL_EXT_ray_tracing : enable
#ifdef USE_SER
#extension GL_NV_shader_invocation_reorder : enable
#endif

#define GLSL 1
#define RT_STAGE_RAYGEN 1
#include "raytracing_common_inc.glsl"

layout(set = 0, binding = 0, rgba16f) uniform image2D image;
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 61) uniform texture2D rt_blue_noise_texture;
layout(set = 0, binding = 62) uniform sampler rt_blue_noise_sampler;

layout(location = 0) rayPayloadEXT PathPayload payload;

vec4 sample_rt_blue_noise(uvec2 pixel, uint frame_index, uint sample_idx) {
	const uint tile_mask = 127u;
	uint frame_x = uint(fract(float(frame_index) * 0.61803398875) * 128.0);
	uint frame_y = uint(fract(float(frame_index) * 0.75487766625) * 128.0);
	uvec2 tile_pixel = (pixel + uvec2(frame_x, frame_y) + uvec2(sample_idx * 37u, sample_idx * 17u)) & uvec2(tile_mask);
	return texelFetch(sampler2D(rt_blue_noise_texture, rt_blue_noise_sampler), ivec2(tile_pixel), 0);
}

uint init_blue_noise_rng(uvec2 pixel, uint frame_index, uint sample_idx) {
	vec4 blue_noise = sample_rt_blue_noise(pixel, frame_index, sample_idx);
	uvec4 packed_noise = uvec4(clamp(blue_noise, vec4(0.0), vec4(0.999)) * 255.0);
	uint blue_seed = packed_noise.x | (packed_noise.y << 8u) | (packed_noise.z << 16u) | (packed_noise.w << 24u);
	return pcg_hash(init_rng(pixel, frame_index, sample_idx) ^ rng_mix(blue_seed ^ (sample_idx * 0x9E3779B9u)));
}

vec3 rtgi_safe_albedo(vec3 albedo) {
	return max(albedo, vec3(0.08));
}

vec3 rtgi_specular_virtual_brdf(vec4 normal_roughness, vec4 albedo_metalness, vec4 guide) {
	float roughness = clamp(normal_roughness.a, 0.0, 1.0);
	float metalness = clamp(albedo_metalness.a, 0.0, 1.0);
	float guide_risk = max(1.0 - roughness, metalness);
	guide_risk = max(guide_risk, clamp(guide.z, 0.0, 1.0));
	float guide_weight = smoothstep(0.18, 0.80, guide_risk);
	vec3 f0 = mix(vec3(0.04), rtgi_safe_albedo(albedo_metalness.rgb), metalness);
	float fa = mix(0.98, 0.42, roughness);
	float fb = (1.0 - metalness) * mix(0.035, 0.006, roughness);
	vec3 brdf = max(f0 * fa + vec3(fb), vec3(0.08));
	return mix(vec3(1.0), brdf, guide_weight);
}

void rt_strc_probe_update_main() {
	uint ray_index = gl_LaunchIDEXT.x;
	uint ray_count = uint(max(get_rt_param(RT_PARAM_RTGI_STRC_RAYS_PER_FRAME), 0.0));
	if (ray_index >= ray_count) {
		return;
	}

	uint grid = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_GRID_SIZE)), 12u, 32u);
	uint cascade_count = clamp(uint(get_rt_param(RT_PARAM_RTGI_STRC_CASCADE_COUNT)), 1u, 4u);
	uint probe_count = cascade_count * grid * grid * grid;
	uint texel_count = probe_count * 64u;
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));
	uint update_index = texel_count > 0u ? (frame_index * max(ray_count, 1u) + ray_index) % texel_count : 0u;
	uint probe_index = update_index >> 6u;
	uint dir_index = update_index & 63u;
	uint probes_per_cascade = grid * grid * grid;
	uint cascade = min(probe_index / probes_per_cascade, cascade_count - 1u);
	uint probe = probe_index - cascade * probes_per_cascade;
	uint px = probe % grid;
	uint py = (probe / grid) % grid;
	uint pz = probe / (grid * grid);

	float spacing = max(get_rt_param(RT_PARAM_RTGI_STRC_BASE_PROBE_SPACING), 0.25) * exp2(float(cascade));
	vec3 camera_origin = rt_camera_world_origin();
	vec3 cascade_center = floor(camera_origin / spacing) * spacing;
	vec3 probe_local = (vec3(float(px), float(py), float(pz)) + vec3(0.5)) - vec3(float(grid) * 0.5);
	vec3 ray_origin = cascade_center + probe_local * spacing;

	vec2 oct_uv = (vec2(float(dir_index & 7u), float((dir_index >> 3u) & 7u)) + vec2(0.5)) / 8.0;
	vec3 ray_dir = normalize(oct_to_vec3(oct_uv * 2.0 - 1.0));

	PathState ps;
	ps.radiance = vec3(0.0);
	ps.specular_radiance = vec3(0.0);
	ps.throughput = vec3(1.0);
	ps.packed_bounces_flags = set_sample_zero(0u);
	ps.rng_state = init_blue_noise_rng(uvec2(ray_index & 255u, (ray_index >> 8u) & 255u), frame_index, dir_index);
	ps.hit_t = 65504.0;
	ps.offset_normal = vec3(0.0, 1.0, 0.0);
	ps.next_ray_dir = ray_dir;

	const uint max_bounces = RT_GET_MAX_BOUNCES();
	[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
		path_pack(payload, ps);
#ifdef USE_SER
		hitObjectNV hitObject;
		hitObjectTraceRayNV(hitObject, tlas, RT_RAY_FLAGS, RT_INSTANCE_MASK_VISIBLE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
		uint hint = 0;
		if (hitObjectIsHitNV(hitObject)) {
			hint = hitObjectGetInstanceIdNV(hitObject);
		}
		reorderThreadNV(hitObject, hint, 8);
		hitObjectExecuteShaderNV(hitObject, 0);
#else
		traceRayEXT(tlas, RT_RAY_FLAGS, RT_INSTANCE_MASK_VISIBLE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
#endif
		ps = path_unpack(payload);
		if (is_path_terminated(ps.packed_bounces_flags)) {
			break;
		}
		vec3 hit_pos = ray_origin + ray_dir * ps.hit_t;
		ray_origin = offset_ray_origin(hit_pos, ps.offset_normal);
		ray_dir = ps.next_ray_dir;
	}

	vec3 radiance = sanitize_payload_vec3(ps.radiance);
	float confidence = clamp(rt_luminance(radiance) > 0.0 ? 1.0 : 0.35, 0.0, 1.0);
	rt_strc_probe_ray_results[ray_index].radiance_distance = vec4(radiance, clamp(ps.hit_t, 0.0, 65504.0));
	rt_strc_probe_ray_results[ray_index].normal_confidence = vec4(ps.offset_normal * 0.5 + 0.5, confidence);
}

void main() {
	if (rt_strc_probe_update_mode()) {
		rt_strc_probe_update_main();
		return;
	}

	uvec2 pixel = gl_LaunchIDEXT.xy;
	ivec2 pixel_i = ivec2(pixel);
	const vec2 in_uv = rt_current_visible_uv(pixel_i);
	ivec2 visible_pixel_i = pixel_i - ivec2(round(rt_current_origin()));
	ivec2 visible_size_i = ivec2(round(rt_visible_size()));
	bool pixel_in_visible = all(greaterThanEqual(visible_pixel_i, ivec2(0))) && all(lessThan(visible_pixel_i, visible_size_i));
	uvec2 rng_pixel = pixel_in_visible ? uvec2(visible_pixel_i) : pixel + uvec2(131071u, 524287u);
	vec2 d = in_uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	vec4 target = inv_projection_unjittered * vec4(d.x, d.y, 1.0, 1.0);
	vec4 origin = inv_view * vec4(0.0, 0.0, 0.0, 1.0);
	vec4 direction = inv_view * vec4(normalize(target.xyz), 0);

	// Sample count from specialization constant, frame index from uniform
	const uint samples_per_pixel = RT_GET_SAMPLE_COUNT();
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));

	// Accumulate multiple samples per pixel
	vec3 total_radiance = vec3(0.0);
	vec3 total_specular_radiance = vec3(0.0);
	rt_signal_reset(pixel_i);

	const uint max_bounces = RT_GET_MAX_BOUNCES();

	// TODO: when we have a spp > 0 the first raycast is always identical,
	// we should move it out of the loop

	[[dont_unroll]] for (uint sample_idx = 0u; sample_idx < samples_per_pixel; sample_idx++) {
		PathState ps;
		ps.radiance = vec3(0.0);
		ps.specular_radiance = vec3(0.0);
		ps.throughput = vec3(1.0);
		ps.packed_bounces_flags = (sample_idx == 0u) ? set_sample_zero(0u) : 0u;
		ps.rng_state = init_blue_noise_rng(rng_pixel, frame_index, sample_idx);

		vec3 ray_origin = origin.xyz;
		vec3 ray_dir = direction.xyz;

		[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			path_pack(payload, ps);

#ifdef USE_SER
			hitObjectNV hitObject;
			hitObjectTraceRayNV(hitObject, tlas, RT_RAY_FLAGS, RT_INSTANCE_MASK_VISIBLE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);

			// Reorder with a coherence hint that has 8 bits
			uint hint = 0;
			if (hitObjectIsHitNV(hitObject)) {
				// TODO: This hint barely does anything. There is a lot of untapped potential here.
				hint = hitObjectGetInstanceIdNV(hitObject);
			}
			reorderThreadNV(hitObject, hint, 8);

			hitObjectExecuteShaderNV(hitObject, 0);
#else
			traceRayEXT(tlas, RT_RAY_FLAGS, RT_INSTANCE_MASK_VISIBLE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
#endif

			ps = path_unpack(payload);
			if (is_path_terminated(ps.packed_bounces_flags)) {
				break;
			}
			// Reconstruct the next ray origin from the current ray + hit distance, apply bias
			vec3 hit_pos = ray_origin + ray_dir * ps.hit_t;
			ray_origin = offset_ray_origin(hit_pos, ps.offset_normal);
			ray_dir = ps.next_ray_dir;
		}

		if (get_rt_param(RT_PARAM_VIS_MODE) == 0.0) {
			vec3 sample_radiance = sanitize_payload_vec3(ps.radiance);
			vec3 sample_specular = min(sanitize_payload_vec3(ps.specular_radiance), sample_radiance);
			if (uint(get_rt_param(RT_PARAM_MODE)) == RT_MODE_HYBRID && has_primary_raster_gi_owner(ps.packed_bounces_flags)) {
				sample_radiance = sample_specular;
			}
			total_radiance += sample_radiance;
			total_specular_radiance += sample_specular;
		} else {
			total_radiance += ps.radiance;
		}
	}

	vec3 final_radiance = total_radiance / float(samples_per_pixel);
	vec3 final_specular = total_specular_radiance / float(samples_per_pixel);
	float inv_samples = 1.0 / float(samples_per_pixel);
	imageStore(rt_signal_direct_light_image, pixel_i, imageLoad(rt_signal_direct_light_image, pixel_i) * inv_samples);
	imageStore(rt_signal_emissive_image, pixel_i, imageLoad(rt_signal_emissive_image, pixel_i) * inv_samples);
	imageStore(rt_signal_indirect_image, pixel_i, imageLoad(rt_signal_indirect_image, pixel_i) * inv_samples);
	imageStore(rt_signal_sky_image, pixel_i, imageLoad(rt_signal_sky_image, pixel_i) * inv_samples);
	imageStore(rt_signal_confidence_image, pixel_i, imageLoad(rt_signal_confidence_image, pixel_i) * vec4(inv_samples, 1.0, 1.0, 1.0));
	final_radiance *= max(0.0, get_rt_param(RT_PARAM_ENERGY));
	final_specular *= max(0.0, get_rt_param(RT_PARAM_ENERGY));
	final_radiance = sanitize_payload_vec3(final_radiance);
	final_specular = min(sanitize_payload_vec3(final_specular), final_radiance);
	vec3 final_diffuse = sanitize_payload_vec3(max(final_radiance - final_specular, vec3(0.0)));
	vec4 normal_roughness = imageLoad(rt_normal_roughness_image, pixel_i);
	vec4 albedo_metalness = imageLoad(rt_albedo_metalness_image, pixel_i);
	vec4 specular_guide = imageLoad(rt_specular_guide_image, pixel_i);
	vec3 final_specular_demodulated = sanitize_payload_vec3(final_specular / rtgi_specular_virtual_brdf(normal_roughness, albedo_metalness, specular_guide));

	imageStore(image, pixel_i, vec4(final_radiance, 1.0));
	imageStore(rt_diffuse_radiance_image, pixel_i, vec4(final_diffuse, 1.0));
	imageStore(rt_specular_radiance_image, pixel_i, vec4(final_specular_demodulated, 1.0));
}

#[miss]

#version 460

#VERSION_DEFINES

#pragma shader_stage(miss)
#extension GL_EXT_ray_tracing : enable

#define GLSL 1
#define RT_STAGE_MISS 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

void main() {
	PathState ps = path_unpack(payload);

#if !defined(USE_SER)
	// Shadow rays that miss mean the light is visible (no occluder).
	if (is_shadow_ray(ps.packed_bounces_flags)) {
		ps.radiance = vec3(1.0);
		path_pack(payload, ps);
		return;
	}
#endif

	// Miss always ends the path.
	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// Specular hit distance: the first-bounce closest_hit pre-seeds the
		// "no hit" color into radiance, so a missed reflection just keeps it.
		if (VIS_MODE == 13 && get_total_bounces(ps.packed_bounces_flags) > 0u) {
			path_pack(payload, ps);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED

	// Primary ray miss: write depth, velocity, and DLSS RR defaults (sample 0 only).
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (!rt_strc_probe_update_mode() && total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

			imageStore(rt_depth_image, pixel, vec4(0.0));

			// Sky velocity: project the ray direction at infinity so camera translation does not create parallax.
			{
				vec3 sky_direction = normalize(gl_WorldRayDirectionEXT);
				vec4 curr_clip = curr_vp_unjittered * vec4(sky_direction, 0.0);
				vec4 prev_clip = prev_vp_unjittered * vec4(sky_direction, 0.0);
				bool curr_valid = !any(isnan(curr_clip)) && !any(isinf(curr_clip)) && curr_clip.w > 1e-5;
				bool prev_valid = !any(isnan(prev_clip)) && !any(isinf(prev_clip)) && prev_clip.w > 1e-5;
				vec2 curr_uv = curr_valid ? (curr_clip.xy / curr_clip.w * 0.5 + 0.5) : rt_current_visible_uv(pixel);
				vec2 prev_uv = prev_valid ? (prev_clip.xy / prev_clip.w * 0.5 + 0.5) : curr_uv;
				float history_valid = (curr_valid && prev_valid) ? 1.0 : 0.0;
				imageStore(rt_history_validity_image, pixel, vec4(history_valid, 0.0, 0.0, 0.0));
				imageStore(rt_history_id_image, pixel, vec4(sky_direction * 0.5 + 0.5, 1.0));
				imageStore(rt_normal_roughness_image, pixel, vec4(-sky_direction * 0.5 + 0.5, 1.0));
				imageStore(rt_albedo_metalness_image, pixel, vec4(1.0, 1.0, 1.0, 0.0));
				imageStore(rt_viewz_hitdist_image, pixel, vec4(65504.0, 65504.0, 0.0, 0.0));
				imageStore(rt_specular_guide_image, pixel, vec4(1.0, 65504.0, 0.0, 0.0));
				imageStore(rt_specular_reprojection_image, pixel, vec4(0.0));
				if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_MODE_SPECULAR_REFLECTION_DIRECTION) {
					imageStore(rt_specular_reflection_direction_image, pixel, vec4(sky_direction * 0.5 + 0.5, 0.0));
				} else if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_MODE_SPECULAR_REFLECTED_HIT_DISTANCE) {
					imageStore(rt_specular_reflection_direction_image, pixel, vec4(0.0));
				} else if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_MODE_SPECULAR_REFLECTED_HIT_NORMAL) {
					imageStore(rt_specular_reflection_direction_image, pixel, vec4(0.0));
				}
				rt_signal_set_primary_confidence(pixel, 0.0, 1.0, history_valid);
				if (history_valid > 0.5) {
					rt_store_primary_velocity(pixel, curr_uv, prev_uv);
				} else {
					rt_store_invalid_primary_velocity(pixel);
				}
			}
		}
	}

	if (uint(get_rt_param(RT_PARAM_MODE)) == RT_MODE_HYBRID &&
			get_total_bounces(ps.packed_bounces_flags) == 0u) {
		ps.radiance = vec3(0.0);
		ps.specular_radiance = vec3(0.0);
		path_pack(payload, ps);
		return;
	}

	mat3 camera_basis = mat3(scene_data_block.data.inv_view_matrix);
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * camera_basis;
	vec3 sky_dir = world_to_sky * gl_WorldRayDirectionEXT;

	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 sky_uv = vec3_to_oct_with_border(sky_dir, border);

	bool background_uses_sky = get_rt_param(RT_PARAM_BACKGROUND_USES_SKY) > 0.5;
	vec3 sky_color;
	if (background_uses_sky) {
		sky_color = textureLod(sampler2D(radiance_octmap, radiance_sampler), sky_uv, 0.0).rgb;
		sky_color *= scene_data_block.data.IBL_exposure_normalization;
	} else {
		sky_color = vec3(get_rt_param(RT_PARAM_BACKGROUND_R), get_rt_param(RT_PARAM_BACKGROUND_G), get_rt_param(RT_PARAM_BACKGROUND_B));
	}

	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u) {
		vec3 fog_color = scene_data_block.data.fog_light_color;

		if (background_uses_sky && scene_data_block.data.fog_aerial_perspective > 0.0) {
			vec3 sky_fog = textureLod(sampler2D(radiance_octmap, radiance_sampler), sky_uv, 1.0).rgb;
			sky_fog *= scene_data_block.data.IBL_exposure_normalization;
			fog_color = mix(fog_color, sky_fog, scene_data_block.data.fog_aerial_perspective);
		}

		sky_color = mix(sky_color, fog_color, scene_data_block.data.fog_sky_affect);
	}

#ifdef DLSS_RR_ENABLED
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (!rt_strc_probe_update_mode() && total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
			imageStore(dlss_rr_diffuse_albedo, pixel, vec4(sky_color, 1.0));
			imageStore(dlss_rr_specular_albedo, pixel, vec4(0.0));
			imageStore(dlss_rr_normal_roughness, pixel, vec4(-gl_WorldRayDirectionEXT, 0.0));
			imageStore(dlss_rr_specular_hit_dist, pixel, vec4(-1.0));
		}
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE == 20) {
			ps.radiance = vec3(1.0);
		} else if (VIS_MODE == 0) {
			uint sky_total_bounces = get_total_bounces(ps.packed_bounces_flags);
			vec3 sky_contribution = ps.throughput * sky_color;
			vec3 clamped_sky = sky_total_bounces > 0u ? rt_clamp_path_contribution(sky_contribution, 0.0, 1.0, true, true) : sky_contribution;
			rt_signal_add_sky(ivec2(gl_LaunchIDEXT.xy), clamped_sky, sky_total_bounces > 0u, rt_signal_clamp_delta(sky_contribution, clamped_sky));
			ps.radiance += clamped_sky;
			if (sky_total_bounces == 0u || get_diffuse_bounces(ps.packed_bounces_flags) == 0u) {
				ps.specular_radiance += clamped_sky;
			}
		} else {
			ps.radiance = sky_color;
		}
	}
#else
	uint sky_total_bounces = get_total_bounces(ps.packed_bounces_flags);
	vec3 sky_contribution = ps.throughput * sky_color;
	vec3 clamped_sky = sky_total_bounces > 0u ? rt_clamp_path_contribution(sky_contribution, 0.0, 1.0, true, true) : sky_contribution;
	rt_signal_add_sky(ivec2(gl_LaunchIDEXT.xy), clamped_sky, sky_total_bounces > 0u, rt_signal_clamp_delta(sky_contribution, clamped_sky));
	ps.radiance += clamped_sky;
	if (sky_total_bounces == 0u || get_diffuse_bounces(ps.packed_bounces_flags) == 0u) {
		ps.specular_radiance += clamped_sky;
	}
#endif // RT_DEBUG_ENABLED

	path_pack(payload, ps);
}

#[closest_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(closest_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef USE_SER
#extension GL_NV_shader_invocation_reorder : enable
#endif

#define GLSL 1
#define RT_STAGE_CLOSEST_HIT 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_lights_inc.glsl"

// clang-format off
layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// clang-format off
#include "raytracing_material_eval_inc.glsl"
#include "raytracing_closest_hit_common_inc.glsl"
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected by ShaderCompiler for HG1+)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

// ============================================================================
// MAIN
// ============================================================================
void main() {
	HitData h = compute_hit_data();
	write_primary_hit_depth(h.hit_pos);
	write_primary_hit_history_validity();
	write_primary_hit_velocity(h.hit_pos);

#ifdef RT_CUSTOM_HIT_GROUP
	uint rt_geometry_idx = h.geometry_idx;
	vec3 rt_hit_pos = h.hit_pos;
	vec2 rt_uv = h.uv;
	vec2 rt_uv2 = h.uv2;
	vec4 rt_color = h.color;
	vec3 rt_normal = h.geometry_normal;
	vec3 rt_tangent = h.tangent;
	vec3 rt_bitangent = h.bitangent;
	bool rt_front_face = h.is_front_face;

#include "raytracing_custom_fragment_inc.glsl"

	// Build MaterialResult from fragment outputs.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = alpha;
	m.roughness = roughness;
	m.metalness = metallic;
	m.specular = specular;
	m.emissive = emission * scene_data_block.data.emissive_exposure_normalization;
	m.normal = normalize(mat3(inv_view_matrix) * normal); // view space -> world space

	// Apply normal map if it was written.
	if (normal_map != vec3(0.5, 0.5, 1.0)) {
		vec3 ts_normal;
		ts_normal.xy = normal_map.xy * 2.0 - 1.0;
		ts_normal.z = sqrt(max(0.0, 1.0 - dot(ts_normal.xy, ts_normal.xy)));
		m.normal = apply_normal_map(h, ts_normal, normal_map_depth);
	}

	vec3 V = -gl_WorldRayDirectionEXT;
	m.normal = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	write_primary_hit_guides(h, m);

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE != 0) {
			float NdotV = max(dot(m.normal, V), 0.0001);
			vec3 orm = vec3(1.0, m.roughness, m.metalness);
			debug_visualize(VIS_MODE, h.geometry_normal, m.normal, normal_map,
					h.tangent, h.bitangent, h.uv, m.albedo, orm, m.metalness, m.roughness, m.specular, m.emissive, V, NdotV);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#else
	// HG0: StandardMaterial3D evaluation.
	MaterialData mat = materials[h.geometry_idx];
	vec2 uv = h.uv * mat.uv1_scale + mat.uv1_offset;

	// Normal mapping.
	vec3 tangent_space_normal = vec3(0.0, 0.0, 1.0);
	vec3 final_normal = h.geometry_normal;
	if ((mat.flags & 1u) != 0u) {
		vec3 normal_sample = sample_material_texture(mat.normal_texture_idx, uv, mat.flags).rgb;
		tangent_space_normal.xy = normal_sample.xy * 2.0 - 1.0;
		tangent_space_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_space_normal.xy, tangent_space_normal.xy)));
		final_normal = apply_normal_map(h, tangent_space_normal, mat.normal_map_depth);
	}

	// Texture sampling.
	vec4 albedo_tex = sample_material_texture(mat.albedo_texture_idx, uv, mat.flags);
	albedo_tex *= rt_material_vertex_color(mat, h.color);
	vec3 albedo = albedo_tex.rgb * mat.albedo_color.rgb;
	float roughness = mat.roughness;
	float metalness = mat.metallic;
	vec3 orm = vec3(1.0, roughness, metalness);
	if ((mat.flags & RT_MAT_FLAG_ORM_TEXTURE) != 0u) {
		orm = sample_material_texture(mat.orm_texture_idx, uv, mat.flags).rgb;
		roughness = saturate(orm.g);
		metalness = saturate(orm.b);
	} else {
		if ((mat.flags & RT_MAT_FLAG_ROUGHNESS_TEXTURE) != 0u) {
			vec4 roughness_sample = sample_material_texture(mat.orm_texture_idx, uv, mat.flags);
			uint roughness_channel = (mat.flags >> RT_MAT_FLAG_ROUGHNESS_CHANNEL_SHIFT) & 7u;
			roughness = saturate(rt_material_texture_channel(roughness_sample, roughness_channel) * mat.roughness);
		}
		if ((mat.flags & RT_MAT_FLAG_METALLIC_TEXTURE) != 0u) {
			vec4 metallic_sample = sample_material_texture(mat.metallic_texture_idx, uv, mat.flags);
			uint metallic_channel = (mat.flags >> RT_MAT_FLAG_METALLIC_CHANNEL_SHIFT) & 7u;
			metalness = saturate(rt_material_texture_channel(metallic_sample, metallic_channel) * mat.metallic);
		}
		orm = vec3(1.0, roughness, metalness);
	}

	vec3 emissive = mat.emission_color * mat.emission_strength;
	if ((mat.flags & 2u) != 0u) {
		emissive *= sample_material_texture(mat.emission_texture_idx, uv, mat.flags).rgb;
	}
	emissive *= scene_data_block.data.emissive_exposure_normalization;

	// Build MaterialResult.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = albedo_tex.a * mat.albedo_color.a;
	m.roughness = roughness;
	m.metalness = metalness;
	m.specular = mat.specular;
	m.emissive = emissive;
	m.normal = final_normal;

	vec3 V = -gl_WorldRayDirectionEXT;
	m.normal = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	write_primary_hit_guides(h, m);

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE != 0) {
			float NdotV = max(dot(m.normal, V), 0.0001);
			debug_visualize(VIS_MODE, h.geometry_normal, m.normal, tangent_space_normal,
					h.tangent, h.bitangent, uv, albedo, orm, metalness, roughness, mat.specular, emissive, V, NdotV);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#endif
}

#[any_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(any_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_ANY_HIT 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(location = 0) rayPayloadInEXT PathPayload payload;

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

#ifndef RT_MATERIAL_TEXTURE_SAMPLING_DEFINED
#define RT_MATERIAL_TEXTURE_SAMPLING_DEFINED
vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}

vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	bool point_filter = (mat_flags & RT_MAT_FLAG_POINT_FILTER) != 0u;
	bool repeat_disabled = (mat_flags & RT_MAT_FLAG_REPEAT_DISABLED) != 0u;
	if (point_filter) {
		return repeat_disabled ?
				texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_CLAMP), uv) :
				texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
	}
	return repeat_disabled ?
			texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv) :
			texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}
#endif

// clang-format off
layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected for per-HG any-hit)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

vec3 rt_orthonormalize_tangent(vec3 world_tangent, vec3 world_normal) {
	vec3 tangent = world_tangent - world_normal * dot(world_normal, world_tangent);
	float len_sq = dot(tangent, tangent);
	if (len_sq > 1e-12) {
		return tangent * inversesqrt(len_sq);
	}
	vec3 axis = abs(world_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	return normalize(cross(axis, world_normal));
}

void main() {
	uint geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[geometry_idx];

	bool shadow_ray = is_shadow_ray(payload.packed_bounces_flags);
	if (shadow_ray && (geom.layer_mask & payload.rng_state) == 0u) {
		ignoreIntersectionEXT;
		return;
	}

	MaterialData mat = materials[geometry_idx];
	if ((mat.flags & RT_MAT_FLAG_ALPHA_TEST) == 0u) {
		return;
	}

#ifdef RT_CUSTOM_HIT_GROUP
	uint rt_geometry_idx = geometry_idx;
	vec2 rt_uv;
	vec2 rt_uv2;
	vec3 rt_normal;
	vec3 rt_tangent;
	vec3 rt_bitangent;
	bool rt_front_face;
	vec3 rt_hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	vec4 rt_color = vec4(1.0);

	mat3 model_matrix = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = transpose(mat3(gl_WorldToObjectEXT));

	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		rt_uv = hit_attribs.bary_or_uv;
		rt_uv2 = hit_attribs.bary_or_uv;

		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		rt_normal = normalize(normal_matrix * obj_normal);
		rt_tangent = rt_orthonormalize_tangent(model_matrix * obj_tangent, rt_normal);
		rt_bitangent = cross(rt_normal, rt_tangent);

		rt_front_face = (dot(rt_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!rt_front_face) {
			rt_normal = -rt_normal;
		}
	} else {
		uint i0, i1, i2;
		get_triangle_indices(geom, i0, i1, i2);
		vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

		rt_uv = fetch_uv(geom, i0, i1, i2, bary);
		rt_uv2 = fetch_uv2(geom, i0, i1, i2, bary);
		TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);

		rt_normal = normalize(normal_matrix * ah_tbn.normal);
		rt_tangent = rt_orthonormalize_tangent(model_matrix * ah_tbn.tangent, rt_normal);
		rt_bitangent = cross(rt_normal, rt_tangent) * ah_tbn.bitangent_sign;

		rt_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
		if (!rt_front_face) {
			rt_normal = -rt_normal;
		}

		rt_color = fetch_color(geom, i0, i1, i2, bary);
	}

#include "raytracing_custom_fragment_inc.glsl"

	if (alpha_scissor_threshold > 0.0 && alpha < alpha_scissor_threshold) {
		ignoreIntersectionEXT;
	}
#ifdef ALPHA_HASH_USED
	vec3 rt_hash_object_pos = (inverse(read_model_matrix) * inv_view_matrix * vec4(vertex, 1.0)).xyz;
	if (alpha < rt_alpha_hash_threshold(rt_hash_object_pos, alpha_hash_scale)) {
		ignoreIntersectionEXT;
	}
#endif
#else
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		return;
	}

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

	// HG0: Standard material alpha test.
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);
	uv = uv * mat.uv1_scale + mat.uv1_offset;
	float alpha = sample_material_texture(mat.albedo_texture_idx, uv, mat.flags).a;
	alpha *= rt_material_vertex_color(mat, fetch_color(geom, i0, i1, i2, bary)).a;
	alpha *= mat.albedo_color.a;

	if ((mat.flags & RT_MAT_FLAG_ALPHA_HASH) != 0u) {
		mat4 rt_aabb_xform;
		mat4 rt_inv_aabb_xform;
		get_aabb_compression_xforms(geom, rt_aabb_xform, rt_inv_aabb_xform);
		vec3 rt_hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		vec3 rt_hash_object_pos = (rt_aabb_xform * mat4(gl_WorldToObjectEXT) * vec4(rt_hit_pos, 1.0)).xyz;
		if (alpha < rt_alpha_hash_threshold(rt_hash_object_pos, mat.alpha_hash_scale)) {
			ignoreIntersectionEXT;
		}
		return;
	}

	if (alpha < mat.alpha_scissor_threshold) {
		ignoreIntersectionEXT;
	}
#endif
}

#[intersection]

#version 460

#VERSION_DEFINES

#pragma shader_stage(intersection)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_INTERSECTION 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

// Write all attributes and report the intersection. Transparently delta-compresses
// PREV_POSITION into spare .w bytes of packed_normal/tangent + prev_pos_delta_yz.
#define report_intersection(t_hit, kind) \
	{ \
		vec3 _obj_hit = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * (t_hit); \
		vec3 _delta = any(isnan(m_PREV_POSITION)) ? vec3(0.0) : (m_PREV_POSITION - _obj_hit); \
		uint _n4 = packSnorm4x8(vec4(m_HIT_NORMAL, 0.0)); \
		uint _t4 = packSnorm4x8(vec4(m_HIT_TANGENT, 0.0)); \
		uint _dx = packHalf2x16(vec2(_delta.x, 0.0)); \
		hit_attribs.bary_or_uv = m_HIT_UV; \
		hit_attribs.packed_normal = (_n4 & 0x00FFFFFFu) | ((_dx & 0xFFu) << 24u); \
		hit_attribs.packed_tangent = (_t4 & 0x00FFFFFFu) | (((_dx >> 8u) & 0xFFu) << 24u); \
		hit_attribs.prev_pos_delta_yz = packHalf2x16(vec2(_delta.y, _delta.z)); \
		reportIntersectionEXT(t_hit, kind); \
	}

#ifdef RT_CUSTOM_HIT_GROUP

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

/* RT_CUSTOM_TEXTURE_DEFINES */

// File-scope built-ins accessible from user helper functions in globals.
float global_time = 0.0;
float global_prev_time = 0.0;
mat4 read_model_matrix = mat4(0.0);
mat4 m_INV_MODEL_MATRIX = mat4(0.0);
mat4 read_view_matrix = mat4(1.0);
mat4 inv_view_matrix = mat4(1.0);
mat4 projection_matrix = mat4(1.0);
mat4 inv_projection_matrix = mat4(1.0);
vec2 read_viewport_size = vec2(1.0);
float m_Z_NEAR = 0.0;
float m_Z_FAR = 0.0;
float rt_point_size = 1.0;
int rt_instance_id = 0;
int rt_vertex_id = 0;
int ViewIndex = 0;
vec3 eye_offset = vec3(0.0);
vec4 instance_custom = vec4(0.0);
uvec4 bone_attrib = uvec4(0u);
vec4 weight_attrib = vec4(0.0);
vec4 custom0_attrib = vec4(0.0);
vec4 custom1_attrib = vec4(0.0);
vec4 custom2_attrib = vec4(0.0);
vec4 custom3_attrib = vec4(0.0);
vec3 light_vertex = vec3(0.0);
vec2 rt_point_coord = vec2(0.0);
float rt_depth = 0.0;
vec4 fog = vec4(0.0);
vec4 custom_radiance = vec4(0.0);
vec4 custom_irradiance = vec4(0.0);
float specular_amount = 0.0;
vec3 light_color = vec3(0.0);
bool is_directional = false;
vec3 light = vec3(0.0);
float attenuation = 1.0;
vec3 diffuse_light = vec3(0.0);
vec3 specular_light = vec3(0.0);

uint64_t _rt_material_address;
#define material CustomMaterialUniforms(_rt_material_address)

/* RT_CUSTOM_INTERSECTION_GLOBALS */

#endif

void main() {
#ifdef RT_CUSTOM_HIT_GROUP
	// Writable outputs.
	vec2 m_HIT_UV = vec2(0.0);
	vec3 m_HIT_NORMAL = vec3(0.0, 1.0, 0.0);
	vec3 m_HIT_TANGENT = vec3(1.0, 0.0, 0.0);
	vec3 m_PREV_POSITION = vec3(uintBitsToFloat(0x7FC00000u)); // NaN sentinel = not set.

	// Per-invocation built-ins (require RT intrinsics, only available in main).
	vec3 m_ORIGIN = gl_ObjectRayOriginEXT;
	vec3 m_DIRECTION = gl_ObjectRayDirectionEXT;
	vec3 m_WORLD_ORIGIN = gl_WorldRayOriginEXT;
	vec3 m_WORLD_DIRECTION = gl_WorldRayDirectionEXT;
	float m_T_MIN = gl_RayTminEXT;
	float m_T_MAX = gl_RayTmaxEXT;
	global_time = scene_data_block.data.time;
	global_prev_time = scene_data_block.prev_data.time;
	read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
	inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
	projection_matrix = scene_data_block.data.projection_matrix;
	inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
	read_viewport_size = scene_data_block.data.viewport_size;
	m_Z_NEAR = scene_data_block.data.z_near;
	m_Z_FAR = scene_data_block.data.z_far;
	rt_instance_id = int(gl_InstanceID);
	rt_vertex_id = int(gl_PrimitiveID);
	ViewIndex = 0;
	eye_offset = vec3(0.0);

	// Resolve custom material uniforms via BDA (assigns file-scope address).
	uint rt_geometry_idx = gl_InstanceCustomIndexEXT;
	MaterialData rt_mat = materials[rt_geometry_idx];
	_rt_material_address = rt_mat.uniform_address;

	// Per-primitive AABB bounds (available when expose_aabb_bounds is enabled).
	GeometryData rt_geom = geometries[rt_geometry_idx];
	vec3 m_AABB_MIN = vec3(0.0);
	vec3 m_AABB_MAX = vec3(0.0);
	if (rt_geom.vertex_address != 0ul) {
		FloatBuffer aabb_buf = FloatBuffer(rt_geom.vertex_address);
		int base = int(gl_PrimitiveID) * 6;
		m_AABB_MIN = vec3(aabb_buf.v[base + 0], aabb_buf.v[base + 1], aabb_buf.v[base + 2]);
		m_AABB_MAX = vec3(aabb_buf.v[base + 3], aabb_buf.v[base + 4], aabb_buf.v[base + 5]);
	}

	mat4 rt_aabb_xform;
	mat4 rt_inv_aabb_xform;
	get_aabb_compression_xforms(rt_geom, rt_aabb_xform, rt_inv_aabb_xform);
	read_model_matrix = mat4(gl_ObjectToWorldEXT) * rt_inv_aabb_xform;
	m_INV_MODEL_MATRIX = rt_aabb_xform * mat4(gl_WorldToObjectEXT);

	/* RT_CUSTOM_INTERSECTION_CODE */

#else
	// Base-variant fallback: never executed at runtime (the intersection
	// stage is always rebuilt per-HG with RT_CUSTOM_HIT_GROUP defined).
	// Only touch unconditional HitAttribs fields so the base variant parses.
	hit_attribs.bary_or_uv = vec2(0.0);
	reportIntersectionEXT(gl_RayTminEXT, 0u);
#endif
}
