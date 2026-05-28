#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 8
#define MAX_RADIANCE 32768.0
#define HISTORY_LENGTH_STORAGE_SCALE 96.0

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float history_weight;
	float max_history;
	float denoise_strength;
	int step_size;
	int pass_index;
	float phi_color;
	float phi_normal;
	float phi_depth;
	float variance_boost;
	float radiance_space_history;
	float firefly_suppression;
	float detail_preservation;
	vec2 visible_origin;
	vec2 visible_size;
	float fog_inv_length;
	float fog_detail_spread;
	float fog_sky_affect;
	float fog_legacy_blending;
	float specular_guide_enabled;
	float history_clip_sigma;
}
params;

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(MAX_RADIANCE), isinf(color));
	return clamp(color, vec3(0.0), vec3(MAX_RADIANCE));
}

float luminance(vec3 color) {
	return max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0);
}

float tonemap_luma(float luma) {
	return luma / (1.0 + luma);
}

vec3 pack_radiance(vec3 color) {
	color = sanitize_color(color);
	return color / (1.0 + luminance(color));
}

vec3 unpack_radiance(vec3 color) {
	color = max(color, vec3(0.0));
	return sanitize_color(color / max(1.0 - luminance(color), 1e-4));
}

vec3 clamp_luminance(vec3 color, float max_luma) {
	float luma = luminance(color);
	return (luma > max_luma) ? color * (max_luma / max(luma, 1e-4)) : color;
}

vec3 decode_normal(vec4 normal_roughness) {
	return normalize(normal_roughness.xyz * 2.0 - 1.0);
}

bool history_id_matches(vec4 a, vec4 b) {
	return max(max(abs(a.x - b.x), abs(a.y - b.y)), max(abs(a.z - b.z), abs(a.w - b.w))) < (0.5 / 255.0);
}

vec3 safe_albedo(vec3 albedo) {
	return max(albedo, vec3(0.08));
}

float velocity_pixels(vec2 velocity) {
	return length(velocity * params.resolution);
}

float relative_luma_delta(float a, float b) {
	return abs(a - b) / max(max(a, b), 0.08);
}

float low_resolution_firefly_boost() {
	float short_side = min(params.resolution.x, params.resolution.y);
	return mix(1.75, 1.0, smoothstep(360.0, 720.0, short_side));
}

float stable_surface_match(float normal_similarity, float albedo_delta, float metalness_delta) {
	return smoothstep(0.84, 0.97, normal_similarity) *
			(1.0 - smoothstep(0.12, 0.42, albedo_delta)) *
			(1.0 - smoothstep(0.04, 0.22, metalness_delta));
}

float guide_active(vec4 guide) {
	return params.specular_guide_enabled * step(0.5, guide.w);
}

float guide_specular_risk(vec4 guide, vec4 normal_roughness, vec4 albedo_metalness) {
	float material_risk = max(1.0 - clamp(normal_roughness.a, 0.0, 1.0), clamp(albedo_metalness.a, 0.0, 1.0));
	return max(material_risk, guide_active(guide) * clamp(guide.z, 0.0, 1.0));
}

float diffuse_demodulation_weight(vec4 normal_roughness, vec4 albedo_metalness, float view_z) {
	if (view_z > 60000.0) {
		return 0.0;
	}
	float roughness = clamp(normal_roughness.a, 0.0, 1.0);
	float metalness = clamp(albedo_metalness.a, 0.0, 1.0);
	float albedo_luma = luminance(albedo_metalness.rgb);
	return (1.0 - metalness) * smoothstep(0.10, 0.40, roughness) * smoothstep(0.015, 0.12, albedo_luma);
}

vec3 radiance_modulation(vec4 normal_roughness, vec4 albedo_metalness, float view_z) {
	return mix(vec3(1.0), safe_albedo(albedo_metalness.rgb), diffuse_demodulation_weight(normal_roughness, albedo_metalness, view_z));
}

vec3 demodulate_radiance(vec3 radiance, vec4 normal_roughness, vec4 albedo_metalness, float view_z) {
	return sanitize_color(radiance / radiance_modulation(normal_roughness, albedo_metalness, view_z));
}

vec3 remodulate_radiance(vec3 radiance, vec4 normal_roughness, vec4 albedo_metalness, float view_z) {
	return sanitize_color(radiance * radiance_modulation(normal_roughness, albedo_metalness, view_z));
}

vec3 specular_virtual_brdf(vec4 normal_roughness, vec4 albedo_metalness, vec4 guide) {
	float roughness = clamp(normal_roughness.a, 0.0, 1.0);
	float metalness = clamp(albedo_metalness.a, 0.0, 1.0);
	float guide_weight = smoothstep(0.18, 0.80, guide_specular_risk(guide, normal_roughness, albedo_metalness));
	vec3 f0 = mix(vec3(0.04), safe_albedo(albedo_metalness.rgb), metalness);
	float fa = mix(0.98, 0.42, roughness);
	float fb = (1.0 - metalness) * mix(0.035, 0.006, roughness);
	vec3 brdf = max(f0 * fa + vec3(fb), vec3(0.08));
	return mix(vec3(1.0), brdf, guide_weight);
}

vec3 demodulate_specular_radiance(vec3 radiance, vec4 normal_roughness, vec4 albedo_metalness, vec4 guide) {
	return sanitize_color(radiance / specular_virtual_brdf(normal_roughness, albedo_metalness, guide));
}

vec3 remodulate_specular_radiance(vec3 radiance, vec4 normal_roughness, vec4 albedo_metalness, vec4 guide) {
	return sanitize_color(radiance * specular_virtual_brdf(normal_roughness, albedo_metalness, guide));
}

#ifdef MODE_TEMPORAL

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D noisy_image;
layout(set = 0, binding = 1) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 3) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 4) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 5) uniform sampler2D history_buffer;
layout(set = 0, binding = 6) uniform sampler2D moments_buffer;
layout(set = 0, binding = 7) uniform sampler2D prev_normal_roughness_buffer;
layout(set = 0, binding = 8) uniform sampler2D prev_viewz_hitdist_buffer;
layout(set = 0, binding = 9) uniform sampler2D prev_albedo_metalness_buffer;
layout(set = 0, binding = 10) uniform sampler2D history_validity_buffer;
layout(set = 0, binding = 11) uniform sampler2D prev_history_validity_buffer;
layout(set = 0, binding = 12) uniform sampler2D history_id_buffer;
layout(set = 0, binding = 13) uniform sampler2D prev_history_id_buffer;
layout(rgba16f, set = 0, binding = 14) uniform restrict writeonly image2D temporal_out;
layout(rgba16f, set = 0, binding = 15) uniform restrict writeonly image2D moments_out;
layout(r16f, set = 0, binding = 16) uniform restrict writeonly image2D variance_out;
layout(r8, set = 0, binding = 17) uniform restrict writeonly image2D rejection_out;
layout(r8, set = 0, binding = 18) uniform restrict writeonly image2D reactivity_out;
layout(r16f, set = 0, binding = 19) uniform restrict writeonly image2D history_length_out;
layout(set = 0, binding = 20) uniform sampler2D specular_guide_buffer;
layout(set = 0, binding = 21) uniform sampler2D prev_specular_guide_buffer;
layout(set = 0, binding = 22) uniform sampler2D specular_reprojection_buffer;

vec3 load_radiance(ivec2 pos) {
	ivec2 clamped_pos = clamp(pos, ivec2(0), ivec2(params.resolution) - ivec2(1));
	vec3 noisy = sanitize_color(imageLoad(noisy_image, clamped_pos).rgb);
	return noisy;
}

vec3 load_demodulated_radiance(ivec2 pos, vec4 normal_roughness, vec4 albedo_metalness, vec2 viewz_hitdist) {
	vec3 radiance = load_radiance(pos);
	if (params.radiance_space_history > 0.5) {
		return pack_radiance(radiance);
	}
	return pack_radiance(demodulate_radiance(radiance, normal_roughness, albedo_metalness, viewz_hitdist.x));
}

bool previous_history_tap_valid(ivec2 tap_pos, vec4 current_nr, vec2 current_viewz_hitdist, float current_expected_prev_view_z, vec4 current_albedo_metalness, vec4 current_id, vec4 current_guide, float motion_px) {
	if (any(lessThan(tap_pos, ivec2(0))) || any(greaterThanEqual(tap_pos, ivec2(params.resolution)))) {
		return false;
	}

	if (texelFetch(prev_history_validity_buffer, tap_pos, 0).r < 0.5) {
		return false;
	}

	vec4 previous_id = texelFetch(prev_history_id_buffer, tap_pos, 0);

	vec4 previous_nr = texelFetch(prev_normal_roughness_buffer, tap_pos, 0);
	vec4 previous_guide = texelFetch(prev_specular_guide_buffer, tap_pos, 0);
	float current_guide_active = guide_active(current_guide);
	float previous_guide_active = guide_active(previous_guide);
	float guide_risk = guide_specular_risk(current_guide, current_nr, current_albedo_metalness);
	float slow_motion_relax = 1.0 - smoothstep(0.5, 4.0, motion_px);
	float normal_similarity = dot(decode_normal(current_nr), decode_normal(previous_nr));
	float normal_threshold = mix(0.68, 0.86, guide_risk * current_guide_active) * mix(1.0, 0.92, slow_motion_relax);
	if (normal_similarity < normal_threshold) {
		return false;
	}

	float roughness_threshold = mix(0.4, 0.11, guide_risk * current_guide_active) * mix(1.0, 1.45, slow_motion_relax);
	if (abs(current_nr.a - previous_nr.a) > roughness_threshold) {
		return false;
	}
	if (current_guide_active > 0.5 || previous_guide_active > 0.5) {
		if (abs(current_guide_active - previous_guide_active) > 0.5 && guide_risk > 0.30) {
			return false;
		}
		if (abs(current_guide.x - previous_guide.x) > mix(0.30, 0.16, guide_risk)) {
			return false;
		}
	}

	vec2 previous_viewz_hitdist = texelFetch(prev_viewz_hitdist_buffer, tap_pos, 0).rg;
	bool current_sky_or_far = current_viewz_hitdist.x > 60000.0;
	bool previous_sky_or_far = previous_viewz_hitdist.x > 60000.0;
	if (current_sky_or_far != previous_sky_or_far) {
		return false;
	}
	if (current_sky_or_far && previous_sky_or_far) {
		vec3 current_sky_direction = normalize(current_id.rgb * 2.0 - 1.0);
		vec3 previous_sky_direction = normalize(previous_id.rgb * 2.0 - 1.0);
		return dot(current_sky_direction, previous_sky_direction) > 0.999;
	}

	if (!history_id_matches(current_id, previous_id)) {
		return false;
	}

	vec4 previous_albedo_metalness = texelFetch(prev_albedo_metalness_buffer, tap_pos, 0);
	float albedo_delta = length(current_albedo_metalness.rgb - previous_albedo_metalness.rgb);
	float metalness_delta = abs(current_albedo_metalness.a - previous_albedo_metalness.a);
	float max_albedo_delta = mix(0.45, 0.58, slow_motion_relax);
	float max_metalness_delta = mix(0.25, 0.35, slow_motion_relax);
	if (albedo_delta > max_albedo_delta || metalness_delta > max_metalness_delta) {
		return false;
	}

	float reference_prev_view_z = current_expected_prev_view_z > 0.0 ? current_expected_prev_view_z : current_viewz_hitdist.x;
	float relative_depth_error;
	float relative_hitdist_error;

	if (current_guide_active > 0.5 && previous_guide_active > 0.5) {
		// 1. Calculate and compare virtual depth of the reflection (reprojected point depth + hit distance)
		float V_curr = reference_prev_view_z + current_guide.y;
		float V_prev = previous_viewz_hitdist.x + previous_guide.y;
		float virtual_depth_scale = max(max(V_curr, V_prev), 1.0);
		relative_depth_error = abs(V_curr - V_prev) / virtual_depth_scale;

		// 2. Perform a relaxed check on the mirror surface depth itself to ensure we didn't reproject off-surface
		float surface_depth_scale = max(max(reference_prev_view_z, previous_viewz_hitdist.x), 1.0);
		float relative_surface_depth = abs(reference_prev_view_z - previous_viewz_hitdist.x) / surface_depth_scale;
		if (relative_surface_depth > 0.35) {
			return false;
		}

		// 3. Bypass surface primary hit distance check; only compare specular guide hit distance
		float guide_hitdist_scale = max(max(current_guide.y, previous_guide.y), 1.0);
		relative_hitdist_error = abs(current_guide.y - previous_guide.y) / guide_hitdist_scale;
	} else {
		// Standard diffuse depth and hit distance logic
		float depth_scale = max(max(reference_prev_view_z, previous_viewz_hitdist.x), 1.0);
		relative_depth_error = abs(reference_prev_view_z - previous_viewz_hitdist.x) / depth_scale;

		float hitdist_scale = max(max(current_viewz_hitdist.y, previous_viewz_hitdist.y), 1.0);
		relative_hitdist_error = abs(current_viewz_hitdist.y - previous_viewz_hitdist.y) / hitdist_scale;
	}

	float motion_slack = smoothstep(1.0, 32.0, motion_px);
	float stable_surface = stable_surface_match(normal_similarity, albedo_delta, metalness_delta);
	float depth_threshold = mix(mix(0.11, 0.35, motion_slack), mix(0.16, 0.55, motion_slack), stable_surface);
	float hitdist_threshold = mix(mix(0.55, 0.85, motion_slack), mix(0.95, 1.65, motion_slack), stable_surface);
	float specular_surface = guide_risk;

	if (current_guide_active > 0.5) {
		// Relax thresholds for specular signals to allow robust temporal accumulation under stochastic noise
		depth_threshold = mix(depth_threshold, mix(0.24, 0.58, motion_slack), 0.5);
		hitdist_threshold = mix(hitdist_threshold, mix(0.75, 1.85, motion_slack), 0.5);
	} else {
		// Standard diffuse tightening logic based on specular surface presence
		depth_threshold = mix(depth_threshold, min(depth_threshold, mix(0.11, 0.42, motion_slack)), specular_surface * 0.35);
		hitdist_threshold = mix(hitdist_threshold, min(hitdist_threshold, mix(0.42, 0.95, motion_slack)), specular_surface * current_guide_active * 0.65);
	}

	return relative_depth_error < depth_threshold && relative_hitdist_error < hitdist_threshold;
}

void accumulate_history_tap(ivec2 tap_pos, float tap_weight, vec4 current_nr, vec2 current_viewz_hitdist, float current_expected_prev_view_z, vec4 current_albedo_metalness, vec4 current_id, vec4 current_guide, float motion_px, inout vec4 history_sum, inout vec4 moments_sum, inout float weight_sum) {
	if (tap_weight <= 0.0 || !previous_history_tap_valid(tap_pos, current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px)) {
		return;
	}

	history_sum += texelFetch(history_buffer, tap_pos, 0) * tap_weight;
	moments_sum += texelFetch(moments_buffer, tap_pos, 0) * tap_weight;
	weight_sum += tap_weight;
}

void sample_reprojected_history(vec2 prev_uv, vec4 current_nr, vec2 current_viewz_hitdist, float current_expected_prev_view_z, vec4 current_albedo_metalness, vec4 current_id, vec4 current_guide, float motion_px, out vec4 history_sample, out vec4 moments_sample, out float history_confidence) {
	vec2 history_pos = prev_uv * params.resolution - vec2(0.5);
	ivec2 base_pos = ivec2(floor(history_pos));
	vec2 fraction = fract(history_pos);

	vec4 history_sum = vec4(0.0);
	vec4 moments_sum = vec4(0.0);
	float weight_sum = 0.0;

	accumulate_history_tap(base_pos + ivec2(0, 0), (1.0 - fraction.x) * (1.0 - fraction.y), current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px, history_sum, moments_sum, weight_sum);
	accumulate_history_tap(base_pos + ivec2(1, 0), fraction.x * (1.0 - fraction.y), current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px, history_sum, moments_sum, weight_sum);
	accumulate_history_tap(base_pos + ivec2(0, 1), (1.0 - fraction.x) * fraction.y, current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px, history_sum, moments_sum, weight_sum);
	accumulate_history_tap(base_pos + ivec2(1, 1), fraction.x * fraction.y, current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px, history_sum, moments_sum, weight_sum);

	if (weight_sum < 0.75) {
		for (int y = -1; y <= 2; y++) {
			for (int x = -1; x <= 2; x++) {
				vec2 delta = vec2(x, y) - fraction;
				float tap_weight = exp(-dot(delta, delta) * 0.85) * 0.22;
				accumulate_history_tap(base_pos + ivec2(x, y), tap_weight, current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px, history_sum, moments_sum, weight_sum);
			}
		}
	}

	history_confidence = clamp(weight_sum, 0.0, 1.0);
	if (weight_sum > 0.0) {
		history_sample = history_sum / weight_sum;
		moments_sample = moments_sum / weight_sum;
	} else {
		history_sample = vec4(0.0);
		moments_sample = vec4(0.0);
	}
}

bool borrow_spatial_history(vec2 prev_uv, vec4 current_nr, vec2 current_viewz_hitdist, float current_expected_prev_view_z, vec4 current_albedo_metalness, vec4 current_id, vec4 current_guide, float motion_px, vec3 current, out vec4 history_sample, out vec4 moments_sample, out float history_confidence) {
	history_sample = vec4(0.0);
	moments_sample = vec4(0.0);
	history_confidence = 0.0;
	if (current_viewz_hitdist.x > 60000.0 || params.history_weight <= 0.001) {
		return false;
	}

	vec2 history_pos = prev_uv * params.resolution - vec2(0.5);
	ivec2 center_pos = ivec2(floor(history_pos));
	float current_luma = luminance(current);
	float current_specular_risk = guide_specular_risk(current_guide, current_nr, current_albedo_metalness);
	float best_score = 0.0;

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 tap_pos = center_pos + ivec2(x, y);
			if (!previous_history_tap_valid(tap_pos, current_nr, current_viewz_hitdist, current_expected_prev_view_z, current_albedo_metalness, current_id, current_guide, motion_px)) {
				continue;
			}

			vec4 tap_history = texelFetch(history_buffer, tap_pos, 0);
			vec4 tap_moments = texelFetch(moments_buffer, tap_pos, 0);
			float tap_history_len = tap_moments.z * HISTORY_LENGTH_STORAGE_SCALE;
			if (tap_history_len < 2.0) {
				continue;
			}

			vec4 tap_nr = texelFetch(prev_normal_roughness_buffer, tap_pos, 0);
			vec2 tap_viewz_hitdist = texelFetch(prev_viewz_hitdist_buffer, tap_pos, 0).rg;
			vec4 tap_albedo = texelFetch(prev_albedo_metalness_buffer, tap_pos, 0);
			vec4 tap_guide = texelFetch(prev_specular_guide_buffer, tap_pos, 0);
			float normal_similarity = max(dot(decode_normal(current_nr), decode_normal(tap_nr)), 0.0);
			float depth_scale = max(max(current_expected_prev_view_z > 0.0 ? current_expected_prev_view_z : current_viewz_hitdist.x, tap_viewz_hitdist.x), 1.0);
			float depth_error = abs((current_expected_prev_view_z > 0.0 ? current_expected_prev_view_z : current_viewz_hitdist.x) - tap_viewz_hitdist.x) / depth_scale;
			float hitdist_scale = max(max(current_viewz_hitdist.y, tap_viewz_hitdist.y), 1.0);
			float hitdist_error = abs(current_viewz_hitdist.y - tap_viewz_hitdist.y) / hitdist_scale;
			float albedo_delta = length(current_albedo_metalness.rgb - tap_albedo.rgb);
			float metalness_delta = abs(current_albedo_metalness.a - tap_albedo.a);
			float roughness_delta = abs(current_nr.a - tap_nr.a);
			float tap_luma = luminance(sanitize_color(tap_history.rgb));
			float radiance_agreement = 1.0 - smoothstep(0.18, 1.25, relative_luma_delta(current_luma, tap_luma));
			float guide_hitdist_scale = max(max(current_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(current_guide.y - tap_guide.y) / guide_hitdist_scale;
			float guide_match = 1.0 - smoothstep(0.08, 0.42, guide_hitdist_error * guide_active(current_guide));
			float specular_guard = 1.0 - smoothstep(0.48, 0.92, current_specular_risk * guide_active(current_guide) * (1.0 - guide_match));
			float geom_score = smoothstep(0.88, 0.995, normal_similarity) *
					(1.0 - smoothstep(0.012, 0.11, depth_error)) *
					(1.0 - smoothstep(0.04, 0.30, hitdist_error));
			float material_score = (1.0 - smoothstep(0.04, 0.32, albedo_delta)) *
					(1.0 - smoothstep(0.02, 0.18, metalness_delta)) *
					(1.0 - smoothstep(0.03, 0.28, roughness_delta));
			float age_score = smoothstep(2.0, 18.0, tap_history_len);
			float distance_score = exp(-dot(vec2(x, y), vec2(x, y)) * 0.32);
			float score = geom_score * material_score * radiance_agreement * guide_match * specular_guard * age_score * distance_score;
			if (score > best_score) {
				best_score = score;
				history_sample = tap_history;
				moments_sample = tap_moments;
			}
		}
	}

	if (best_score <= 0.08) {
		return false;
	}

	float borrowed_len = min(moments_sample.z * HISTORY_LENGTH_STORAGE_SCALE, mix(5.0, 14.0, best_score));
	moments_sample.z = borrowed_len / HISTORY_LENGTH_STORAGE_SCALE;
	history_confidence = clamp(best_score * 0.62, 0.08, 0.72);
	return true;
}

float current_spatial_luma_variance(ivec2 pos, vec4 center_nr, vec2 center_viewz_hitdist, vec4 center_albedo, vec4 center_id, vec4 center_guide) {
	vec3 center_n = decode_normal(center_nr);
	float center_specular_risk = guide_specular_risk(center_guide, center_nr, center_albedo);
	float luma_sum = 0.0;
	float luma_sq_sum = 0.0;
	float weight_sum = 0.0;

	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 tap_pos = pos + ivec2(x, y);
			if (any(lessThan(tap_pos, ivec2(0))) || any(greaterThanEqual(tap_pos, ivec2(params.resolution)))) {
				continue;
			}
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec2 tap_viewz_hitdist = texelFetch(viewz_hitdist_buffer, tap_pos, 0).rg;
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			vec4 tap_id = texelFetch(history_id_buffer, tap_pos, 0);
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float depth_scale = max(max(center_viewz_hitdist.x, tap_viewz_hitdist.x), 1.0);
			float relative_depth_error = abs(center_viewz_hitdist.x - tap_viewz_hitdist.x) / depth_scale;
			float albedo_delta = length(tap_albedo.rgb - center_albedo.rgb) + abs(tap_albedo.a - center_albedo.a);
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, 0.0);
			float guide_hitdist_scale = max(max(center_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(center_guide.y - tap_guide.y) / guide_hitdist_scale;
			bool sky_or_far = center_viewz_hitdist.x > 60000.0 && tap_viewz_hitdist.x > 60000.0;
			bool compatible = (normal_similarity > 0.80) && (relative_depth_error < mix(0.06, 0.22, plane_relax) || sky_or_far) && albedo_delta < 0.48;
			float guide_guard = max(smoothstep(0.34, 0.76, center_specular_risk) * guide_active(center_guide), smoothstep(0.60, 0.92, center_specular_risk));
			compatible = compatible && (guide_guard < 0.5 || history_id_matches(center_id, tap_id));
			compatible = compatible && (guide_active(center_guide) < 0.5 || guide_guard < 0.5 || guide_hitdist_error < mix(0.34, 0.15, center_specular_risk));
			if (!compatible) {
				continue;
			}

			vec3 tap = load_demodulated_radiance(tap_pos, tap_nr, tap_albedo, tap_viewz_hitdist);
			float tap_luma = luminance(tap);
			float spatial_w = (x == 0 && y == 0) ? 2.0 : ((x == 0 || y == 0) ? 1.0 : 0.72);
			float normal_w = pow(normal_similarity, mix(8.0, 3.0, plane_relax));
			float w = spatial_w * normal_w;
			luma_sum += tap_luma * w;
			luma_sq_sum += tap_luma * tap_luma * w;
			weight_sum += w;
		}
	}

	if (weight_sum <= 1e-4) {
		return 0.0;
	}
	float mean_luma = luma_sum / weight_sum;
	return clamp(max(luma_sq_sum / weight_sum - mean_luma * mean_luma, 0.0), 0.00025, 0.045);
}

void current_neighborhood(ivec2 pos, vec4 center_nr, vec2 center_viewz_hitdist, vec4 center_albedo, vec4 center_id, vec4 center_guide, out vec3 neighborhood_min, out vec3 neighborhood_max, out vec3 neighborhood_avg, out vec3 neighborhood_sigma, out vec3 neighbor_avg, out float neighbor_weight_sum) {
	neighborhood_min = vec3(MAX_RADIANCE);
	neighborhood_max = vec3(0.0);
	neighborhood_avg = vec3(0.0);
	vec3 neighborhood_sq_avg = vec3(0.0);
	float weight_sum = 0.0;
	neighbor_avg = vec3(0.0);
	neighbor_weight_sum = 0.0;
	vec3 center_n = decode_normal(center_nr);
	float center_specular_risk = guide_specular_risk(center_guide, center_nr, center_albedo);
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap_pos = pos + ivec2(x, y);
			if (any(lessThan(tap_pos, ivec2(0))) || any(greaterThanEqual(tap_pos, ivec2(params.resolution)))) {
				continue;
			}
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec2 tap_viewz_hitdist = texelFetch(viewz_hitdist_buffer, tap_pos, 0).rg;
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			vec4 tap_id = texelFetch(history_id_buffer, tap_pos, 0);
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			float normal_similarity = dot(center_n, decode_normal(tap_nr));
			float depth_scale = max(max(center_viewz_hitdist.x, tap_viewz_hitdist.x), 1.0);
			float relative_depth_error = abs(center_viewz_hitdist.x - tap_viewz_hitdist.x) / depth_scale;
			float guide_hitdist_scale = max(max(center_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(center_guide.y - tap_guide.y) / guide_hitdist_scale;
			float center_guide_reliable = guide_active(center_guide);
			bool sky_or_far = center_viewz_hitdist.x > 60000.0 && tap_viewz_hitdist.x > 60000.0;
			float albedo_delta = length(tap_albedo.rgb - center_albedo.rgb) + abs(tap_albedo.a - center_albedo.a);
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, 0.0);
			float guide_id_guard = max(smoothstep(0.32, 0.72, center_specular_risk) * center_guide_reliable, smoothstep(0.58, 0.90, center_specular_risk));
			bool compatible = (normal_similarity > 0.82) && (relative_depth_error < mix(0.07, 0.24, plane_relax) || sky_or_far) && albedo_delta < 0.45;
			compatible = compatible && (guide_id_guard < 0.5 || history_id_matches(center_id, tap_id));
			compatible = compatible && (center_guide_reliable < 0.5 || guide_id_guard < 0.5 || guide_hitdist_error < mix(0.38, 0.16, center_specular_risk));
			if (!compatible) {
				continue;
			}

			vec3 tap = load_demodulated_radiance(tap_pos, tap_nr, tap_albedo, tap_viewz_hitdist);
			neighborhood_min = min(neighborhood_min, tap);
			neighborhood_max = max(neighborhood_max, tap);
			neighborhood_avg += tap;
			neighborhood_sq_avg += tap * tap;
			weight_sum += 1.0;
			neighbor_avg += tap;
			neighbor_weight_sum += 1.0;
		}
	}
	if (weight_sum <= 0.0) {
		vec3 center = load_demodulated_radiance(pos, center_nr, center_albedo, center_viewz_hitdist);
		neighborhood_min = center;
		neighborhood_max = center;
		neighborhood_avg = center;
		neighborhood_sigma = vec3(0.0);
	} else {
		neighborhood_avg /= weight_sum;
		neighborhood_sq_avg /= weight_sum;
		neighborhood_sigma = sqrt(max(neighborhood_sq_avg - neighborhood_avg * neighborhood_avg, vec3(0.0)));
	}
	if (neighbor_weight_sum > 0.0) {
		neighbor_avg /= neighbor_weight_sum;
	}
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec2 uv = (vec2(pos) + 0.5) / params.resolution;
	vec2 velocity = texelFetch(velocity_buffer, pos, 0).xy;

	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 viewz_hitdist_sample = texelFetch(viewz_hitdist_buffer, pos, 0);
	vec2 viewz_hitdist = viewz_hitdist_sample.rg;
	float expected_prev_view_z = viewz_hitdist_sample.b;
	vec4 current_id = texelFetch(history_id_buffer, pos, 0);
	vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec4 specular_guide = texelFetch(specular_guide_buffer, pos, 0);
	float guide_specular_surface = guide_specular_risk(specular_guide, normal_roughness, albedo_metalness);
	vec4 specular_reprojection = texelFetch(specular_reprojection_buffer, pos, 0);
	float virtual_reprojection_weight = params.radiance_space_history > 0.5 ? guide_active(specular_guide) * specular_reprojection.a * smoothstep(0.35, 0.85, guide_specular_surface) : 0.0;
	vec2 reprojection_velocity = mix(velocity, specular_reprojection.xy, virtual_reprojection_weight);
	float motion_px = velocity_pixels(reprojection_velocity);
	vec2 prev_uv = uv + reprojection_velocity;
	bool prev_in_screen = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));

	bool current_valid = texelFetch(history_validity_buffer, pos, 0).r >= 0.5;

	vec3 current = load_demodulated_radiance(pos, normal_roughness, albedo_metalness, viewz_hitdist);
	vec4 prev_history;
	vec4 prev_moments;
	float history_confidence = 0.0;
	bool borrowed_history = false;
	if (prev_in_screen && current_valid) {
		sample_reprojected_history(prev_uv, normal_roughness, viewz_hitdist, expected_prev_view_z, albedo_metalness, current_id, specular_guide, motion_px, prev_history, prev_moments, history_confidence);
	} else {
		prev_history = vec4(0.0);
		prev_moments = vec4(0.0);
	}
	if (prev_in_screen && current_valid && history_confidence < 0.08) {
		borrowed_history = borrow_spatial_history(prev_uv, normal_roughness, viewz_hitdist, expected_prev_view_z, albedo_metalness, current_id, specular_guide, motion_px, current, prev_history, prev_moments, history_confidence);
	}
	float fast_motion = smoothstep(1.0, 16.0, motion_px);
	float extreme_motion = smoothstep(24.0, 48.0, motion_px);
	float reprojection_confidence = history_confidence;
	history_confidence *= mix(1.0, 0.02, extreme_motion);
	bool history_valid = history_confidence >= 0.08 && params.history_weight > 0.001;
	float prev_history_len = prev_moments.z * HISTORY_LENGTH_STORAGE_SCALE;
	float guide_history_stability = guide_active(specular_guide) * smoothstep(0.30, 0.80, guide_specular_surface) * smoothstep(0.70, 0.98, reprojection_confidence) * (1.0 - smoothstep(0.5, 6.0, motion_px));
	float early_specular_surface = guide_specular_surface;
	float specular_history_guard = smoothstep(0.30, 0.95, early_specular_surface);
	float motion_history_cap = mix(params.max_history, 7.0, fast_motion);
	motion_history_cap = mix(motion_history_cap, 2.0, extreme_motion);
	float specular_history_cap = mix(params.max_history, 10.0, specular_history_guard * (1.0 - guide_history_stability));
	motion_history_cap = min(motion_history_cap, specular_history_cap);
	float decay_factor = mix(0.85, 1.0, smoothstep(0.15, 0.75, history_confidence));
	float history_len = history_valid ? min(min(prev_history_len * decay_factor + history_confidence, params.max_history), motion_history_cap) : 1.0;
	history_len = borrowed_history ? min(history_len, 12.0) : history_len;
	float base_alpha = max(pow(max(1.0 - params.history_weight, 0.001), 1.15), 0.025);
	if (params.radiance_space_history > 0.5) {
		base_alpha = max(pow(max(1.0 - params.history_weight, 0.001), 2.20), 0.055);
	}
	float current_alpha = history_valid ? max(1.0 / history_len, base_alpha) : 1.0;
	current_alpha = history_valid ? mix(0.72, current_alpha, smoothstep(0.08, 0.65, history_confidence)) : current_alpha;
	float specular_min_alpha = params.radiance_space_history > 0.5 ? 0.07 : mix(0.16, 0.055, guide_history_stability);
	current_alpha = history_valid ? max(current_alpha, mix(0.0, specular_min_alpha, specular_history_guard)) : current_alpha;
	current_alpha = history_valid ? max(current_alpha, mix(base_alpha, 0.90, fast_motion)) : current_alpha;
	current_alpha = history_valid ? max(current_alpha, mix(0.90, 1.0, extreme_motion)) : current_alpha;

	vec3 neighborhood_min;
	vec3 neighborhood_max;
	vec3 neighborhood_avg;
	vec3 neighborhood_sigma;
	vec3 neighbor_avg;
	float neighbor_weight_sum;
	current_neighborhood(pos, normal_roughness, viewz_hitdist, albedo_metalness, current_id, specular_guide, neighborhood_min, neighborhood_max, neighborhood_avg, neighborhood_sigma, neighbor_avg, neighbor_weight_sum);
	if (neighbor_weight_sum <= 0.0 && history_valid) {
		vec3 history_reference = sanitize_color(prev_history.rgb);
		neighborhood_min = history_reference;
		neighborhood_max = history_reference;
		neighborhood_avg = history_reference;
		neighborhood_sigma = vec3(sqrt(max(prev_history.a, 0.0)));
	}
	float previous_variance = max(prev_history.a, 0.0);
	vec3 neighborhood_range = max(neighborhood_max - neighborhood_min, vec3(0.05));
	float history_clip_variance = min(previous_variance, mix(previous_variance + 1e-4, 0.16, params.denoise_strength));
	vec3 clip_expand = neighborhood_range * 0.45 + vec3(sqrt(history_clip_variance) * 0.85 + 0.02);

	float current_luma = luminance(current);
	float raw_history_luma = history_valid ? luminance(sanitize_color(prev_history.rgb)) : current_luma;
	float center_history_change = relative_luma_delta(current_luma, raw_history_luma);

	float relaxation = smoothstep(1.0, 8.0, history_len) * smoothstep(0.18, 0.85, history_confidence) * (1.0 - smoothstep(0.05, 0.20, center_history_change)) * (1.0 - smoothstep(1.0, 12.0, motion_px));
	float sigma_k = max(params.history_clip_sigma, 0.25) * mix(1.0, 2.75, relaxation);

	vec3 sigma_min = neighborhood_avg - neighborhood_sigma * sigma_k;
	vec3 sigma_max = neighborhood_avg + neighborhood_sigma * sigma_k;
	vec3 clip_min = max(neighborhood_min - clip_expand, sigma_min - vec3(0.025));
	vec3 clip_max = min(neighborhood_max + clip_expand, sigma_max + vec3(0.025));
	clip_max = max(clip_max, clip_min + vec3(0.01));
	vec3 history_color = history_valid ? clamp(sanitize_color(prev_history.rgb), clip_min, clip_max) : current;

	float history_luma = luminance(history_color);
	float history_support_luma = history_valid ? raw_history_luma : 0.0;
	float local_neighbor_luma = neighbor_weight_sum > 0.0 ? luminance(neighbor_avg) : 0.0;
	float neighbor_luma = neighbor_weight_sum > 0.0 ? local_neighbor_luma : history_support_luma;
	float neighbor_support = smoothstep(1.5, 4.0, neighbor_weight_sum);
	float support_luma = max(neighbor_luma, history_support_luma);
	float local_support_luma = local_neighbor_luma * max(neighbor_support, smoothstep(0.1, 1.0, neighbor_weight_sum) * 0.55);
	float variance_sigma = sqrt(previous_variance);
	float surface_firefly_risk = guide_specular_surface;
	float neighborhood_history_change = relative_luma_delta(neighbor_luma, history_luma);
	float neighborhood_agreement = (1.0 - smoothstep(0.18, 0.72, relative_luma_delta(current_luma, neighbor_luma))) * neighbor_support;
	float history_agreement = history_valid ? (1.0 - smoothstep(0.18, 0.72, center_history_change)) * smoothstep(2.0, 10.0, history_len) * history_confidence : 0.0;
	float temporal_or_local_agreement = max(neighborhood_agreement, history_agreement);
	float rough_diffuse_surface = smoothstep(0.38, 0.78, normal_roughness.a) * (1.0 - clamp(albedo_metalness.a, 0.0, 1.0));
	float visible_light = smoothstep(0.015, 0.10, max(max(support_luma, history_luma), current_luma));
	float history_or_neighbor_visible = smoothstep(0.015, 0.10, support_luma);
	float local_support_visible = smoothstep(0.015, 0.10, local_support_luma);
	float dark_support = 1.0 - smoothstep(0.018, 0.12, local_support_luma);
	float lowres_firefly_boost = low_resolution_firefly_boost();
	float stable_visible_highlight = smoothstep(0.22, 1.20, current_luma) *
			max(neighborhood_agreement, history_agreement) *
			smoothstep(0.12, 0.70, max(max(local_support_luma, history_luma), support_luma));
	float isolated_spike_limit = mix(max(support_luma * 4.0 + 0.25, 0.35), max(local_support_luma * 2.0 + 0.055, 0.08), dark_support);
	float isolated_spike = smoothstep(isolated_spike_limit, isolated_spike_limit * 2.0 + 0.5, current_luma);
	float dark_isolated_spike = dark_support * smoothstep(local_support_luma + 0.025, local_support_luma + 0.14, current_luma) * (1.0 - temporal_or_local_agreement);
	float unsupported_spike = clamp(max(isolated_spike * (1.0 - history_or_neighbor_visible), dark_isolated_spike * (1.0 - local_support_visible)) * (1.0 - temporal_or_local_agreement) * params.firefly_suppression * lowres_firefly_boost, 0.0, 1.0);
	float neighborhood_light_change = smoothstep(0.015, 0.09, neighborhood_history_change) * mix(0.25, 1.0, neighborhood_agreement) * neighbor_support;
	float coherent_light_support = max(neighborhood_agreement, history_agreement * 0.65);
	float center_light_change = smoothstep(0.05, 0.18, center_history_change) * smoothstep(0.16, 0.55, coherent_light_support) * (1.0 - unsupported_spike);
	float light_reactivity_confidence = max(history_confidence, reprojection_confidence * mix(1.0, 0.35, extreme_motion));
	float light_reactivity = history_valid ? clamp(max(neighborhood_light_change, center_light_change) * visible_light * light_reactivity_confidence, 0.0, 1.0) : 0.0;
	float stochastic_noise_guard = smoothstep(0.0015, 0.045, previous_variance) * (1.0 - coherent_light_support) * rough_diffuse_surface;
	light_reactivity *= mix(1.0, 0.20, stochastic_noise_guard);
	light_reactivity *= mix(1.0, 0.35, guide_history_stability * unsupported_spike);
	current_alpha = history_valid ? mix(current_alpha, max(current_alpha, mix(0.62, 0.98, max(neighborhood_light_change, center_light_change))), light_reactivity) : current_alpha;
	current_alpha = history_valid ? mix(current_alpha, min(current_alpha, max(base_alpha, 0.045)), guide_history_stability * unsupported_spike * (1.0 - light_reactivity)) : current_alpha;
	history_len = history_valid ? mix(history_len, min(history_len, 1.0), light_reactivity * 0.95) : history_len;

	float history_trust = history_valid ? smoothstep(2.0, 10.0, history_len) * history_confidence * params.denoise_strength * (1.0 - light_reactivity) : 0.0;
	float neighborhood_limit = max(support_luma * 3.0 + 0.18, support_luma + 0.35);
	float history_limit = max(max(history_luma, support_luma) + variance_sigma * mix(2.0, 1.0, surface_firefly_risk) + mix(0.22, 0.08, surface_firefly_risk), 0.05);
	float current_limit = mix(neighborhood_limit, min(neighborhood_limit, history_limit), history_trust);
	float dark_current_limit = max(local_support_luma + 0.12, 0.11);
	current_limit = mix(current_limit, min(current_limit, dark_current_limit), dark_support * unsupported_spike * (1.0 - light_reactivity));
	float firefly_strength = smoothstep(current_limit, current_limit * 2.5 + 0.25, current_luma) * params.denoise_strength * params.firefly_suppression * lowres_firefly_boost * (1.0 - light_reactivity * 0.85);
	float isolated_highlight_protection = smoothstep(0.35, 1.2, current_luma) * (1.0 - surface_firefly_risk) * fast_motion;
	float unsupported_firefly = unsupported_spike * params.denoise_strength * mix(0.85, 1.0, surface_firefly_risk) * (1.0 - isolated_highlight_protection * 0.45);
	firefly_strength = max(firefly_strength, unsupported_firefly * (1.0 - light_reactivity * 0.7) * mix(0.85, 1.0, dark_support));
	firefly_strength *= mix(1.0, 0.18, stable_visible_highlight);
	firefly_strength = clamp(firefly_strength, 0.0, 1.0);
	current = mix(current, clamp_luminance(current, current_limit), firefly_strength);

	vec3 temporal = sanitize_color(mix(history_color, current, current_alpha));
	current_luma = luminance(current);
	vec2 current_moments = vec2(current_luma, current_luma * current_luma);
	float seeded_variance = history_valid ? 0.0 : current_spatial_luma_variance(pos, normal_roughness, viewz_hitdist, albedo_metalness, current_id, specular_guide);
	vec2 seeded_moments = vec2(current_luma, current_luma * current_luma + seeded_variance);
	vec2 moments = history_valid ? mix(prev_moments.xy, current_moments, current_alpha) : seeded_moments;
	float variance = max(moments.y - moments.x * moments.x, 0.0);
	float rejected = history_valid ? 1.0 - history_confidence : 1.0;

	imageStore(temporal_out, pos, vec4(temporal, variance));
	imageStore(moments_out, pos, vec4(moments, history_len / HISTORY_LENGTH_STORAGE_SCALE, rejected));
	imageStore(variance_out, pos, vec4(variance, 0.0, 0.0, 0.0));
	imageStore(rejection_out, pos, vec4(rejected, 0.0, 0.0, 0.0));
	imageStore(reactivity_out, pos, vec4(light_reactivity, 0.0, 0.0, 0.0));
	imageStore(history_length_out, pos, vec4(history_len / params.max_history, 0.0, 0.0, 0.0));
}

#endif

#ifdef MODE_BLOTCH_STABILIZE

layout(set = 0, binding = 0) uniform sampler2D input_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 3) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 4) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 5) uniform sampler2D reactivity_buffer;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D output_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 center_rgba = texelFetch(input_buffer, pos, 0);
	vec3 center_color = sanitize_color(center_rgba.rgb);
	vec4 center_nr = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 center_albedo = texelFetch(albedo_metalness_buffer, pos, 0);
	vec2 center_viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0).rg;
	vec2 center_velocity = texelFetch(velocity_buffer, pos, 0).xy;
	float center_z = center_viewz_hitdist.x;
	float center_roughness = clamp(center_nr.a, 0.0, 1.0);
	float center_metallic = clamp(center_albedo.a, 0.0, 1.0);
	float center_reactivity = texelFetch(reactivity_buffer, pos, 0).r;
	float center_motion_px = velocity_pixels(center_velocity);
	float rough_diffuse = smoothstep(0.34, 0.78, center_roughness) * (1.0 - center_metallic);
	if (center_z > 60000.0 || rough_diffuse <= 0.02 || center_reactivity > 0.85 || params.denoise_strength <= 0.0) {
		imageStore(output_image, pos, vec4(center_color, center_rgba.a));
		return;
	}

	vec3 center_n = decode_normal(center_nr);
	vec3 center_demod = demodulate_radiance(center_color, center_nr, center_albedo, center_z);
	float center_luma = luminance(center_demod);
	float center_luma_t = tonemap_luma(center_luma);

	vec3 color_sum = center_demod * 2.0;
	float luma_sum = center_luma * 2.0;
	float luma_sq_sum = center_luma * center_luma * 2.0;
	float weight_sum = 2.0;
	float same_surface_support = 0.0;
	float bright_support = 0.0;
	float surface_albedo_detail = 0.0;
	float surface_normal_detail = 0.0;

	for (int y = -4; y <= 4; y += 2) {
		for (int x = -4; x <= 4; x += 2) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			vec2 tap_viewz_hitdist = texelFetch(viewz_hitdist_buffer, tap_pos, 0).rg;
			vec2 tap_velocity = texelFetch(velocity_buffer, tap_pos, 0).xy;
			float tap_z = tap_viewz_hitdist.x;
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float albedo_delta = length(tap_albedo.rgb - center_albedo.rgb);
			float metalness_delta = abs(tap_albedo.a - center_albedo.a);
			float depth_scale = max(max(center_z, tap_z), 1.0);
			float relative_depth_error = abs(center_z - tap_z) / depth_scale;
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, metalness_delta);
			if (normal_similarity < 0.84 || albedo_delta > 0.42 || metalness_delta > 0.18 || relative_depth_error > mix(0.055, 0.22, plane_relax)) {
				continue;
			}
			if (abs(x) <= 2 && abs(y) <= 2) {
				surface_albedo_detail = max(surface_albedo_detail, albedo_delta);
				surface_normal_detail = max(surface_normal_detail, 1.0 - normal_similarity);
			}

			vec3 tap_color = sanitize_color(texelFetch(input_buffer, tap_pos, 0).rgb);
			vec3 tap_demod = demodulate_radiance(tap_color, tap_nr, tap_albedo, tap_z);
			float tap_luma = luminance(tap_demod);
			float spatial_w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.045);
			float normal_w = pow(normal_similarity, mix(8.0, 3.0, plane_relax));
			float depth_w = exp(-relative_depth_error / max(mix(0.018, 0.080, plane_relax), 1e-4));
			float albedo_w = exp(-albedo_delta * 5.0) * (1.0 - smoothstep(0.08, 0.22, metalness_delta));
			float velocity_w = exp(-velocity_pixels(tap_velocity - center_velocity) * 0.18);
			float w = spatial_w * normal_w * depth_w * albedo_w * velocity_w;
			color_sum += tap_demod * w;
			luma_sum += tap_luma * w;
			luma_sq_sum += tap_luma * tap_luma * w;
			weight_sum += w;
			same_surface_support += w;
			bright_support += (tap_luma > center_luma * 0.75 && tap_luma > 0.10) ? w : 0.0;
		}
	}

	if (same_surface_support <= 1e-4) {
		imageStore(output_image, pos, vec4(center_color, center_rgba.a));
		return;
	}

	vec3 neighborhood = color_sum / max(weight_sum, 1e-5);
	float neighborhood_luma = luma_sum / max(weight_sum, 1e-5);
	float neighborhood_variance = max(luma_sq_sum / max(weight_sum, 1e-5) - neighborhood_luma * neighborhood_luma, 0.0);
	float support = smoothstep(2.0, 7.5, same_surface_support);
	float local_delta = abs(center_luma_t - tonemap_luma(neighborhood_luma));
	float relative_delta = relative_luma_delta(center_luma, neighborhood_luma);
	float variance_gate = smoothstep(0.00012, 0.010, neighborhood_variance);
	float bright_support_ratio = bright_support / max(same_surface_support, 1e-5);
	float motion_guard = 1.0 - smoothstep(4.0, 24.0, center_motion_px);
	float unsupported_bright = smoothstep(0.08, 0.28, center_luma - neighborhood_luma) * (1.0 - smoothstep(0.10, 0.45, bright_support_ratio));
	float blotch_signal = max(smoothstep(0.018, 0.090, local_delta), smoothstep(0.08, 0.38, relative_delta) * 0.75);
	float albedo_detail_guard = smoothstep(0.055, 0.24, surface_albedo_detail) * rough_diffuse * params.detail_preservation;
	float normal_detail_guard = smoothstep(0.020, 0.16, surface_normal_detail) * rough_diffuse * params.detail_preservation;
	float surface_detail_guard = max(albedo_detail_guard, normal_detail_guard);
	float stabilizer = blotch_signal * variance_gate * support * rough_diffuse * motion_guard * params.denoise_strength * mix(1.0, 0.30, center_reactivity);
	stabilizer *= mix(1.0, 0.42, surface_detail_guard);
	stabilizer = max(stabilizer, unsupported_bright * support * rough_diffuse * motion_guard * params.denoise_strength * mix(0.40, 0.18, center_reactivity));
	stabilizer = min(stabilizer, 0.82);

	vec3 stabilized = mix(center_demod, neighborhood, stabilizer);
	vec3 output_color = params.radiance_space_history > 0.5 ? sanitize_color(stabilized) : remodulate_radiance(stabilized, center_nr, center_albedo, center_z);
	imageStore(output_image, pos, vec4(output_color, center_rgba.a));
}

#endif

#ifdef MODE_SPLIT_COMPOSITE

layout(set = 0, binding = 0) uniform sampler2D diffuse_buffer;
layout(set = 0, binding = 1) uniform sampler2D specular_buffer;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D output_image;
layout(set = 0, binding = 3) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 5) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 6) uniform sampler2D specular_guide_buffer;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec4 specular_guide = texelFetch(specular_guide_buffer, pos, 0);
	vec3 diffuse = sanitize_color(texelFetch(diffuse_buffer, pos, 0).rgb);
	vec3 specular = remodulate_specular_radiance(sanitize_color(texelFetch(specular_buffer, pos, 0).rgb), normal_roughness, albedo_metalness, specular_guide);
	vec3 output_color = sanitize_color(diffuse + specular);
	float center_luma = luminance(output_color);
	if (center_luma > 0.0) {
		vec3 neighbor_sum = vec3(0.0);
		float neighbor_luma_sum = 0.0;
		float neighbor_luma_max = 0.0;
		float bright_support_sum = 0.0;
		float neighbor_weight_sum = 0.0;
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				if (x == 0 && y == 0) {
					continue;
				}
				ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
				vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
				vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
				vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
				vec3 tap_specular = remodulate_specular_radiance(sanitize_color(texelFetch(specular_buffer, tap_pos, 0).rgb), tap_nr, tap_albedo, tap_guide);
				vec3 tap_color = sanitize_color(texelFetch(diffuse_buffer, tap_pos, 0).rgb + tap_specular);
				float tap_luma = luminance(tap_color);
				float tap_w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.22);
				neighbor_sum += tap_color * tap_w;
				neighbor_luma_sum += tap_luma * tap_w;
				neighbor_luma_max = max(neighbor_luma_max, tap_luma);
				bright_support_sum += (tap_luma > max(center_luma * 0.55, 0.08)) ? tap_w : 0.0;
				neighbor_weight_sum += tap_w;
			}
		}
		vec3 neighbor_color = neighbor_sum / max(neighbor_weight_sum, 1e-5);
		float neighbor_luma = neighbor_luma_sum / max(neighbor_weight_sum, 1e-5);
		float bright_support_ratio = bright_support_sum / max(neighbor_weight_sum, 1e-5);
		float stable_bright_support = smoothstep(0.14, 0.42, bright_support_ratio) * smoothstep(center_luma * 0.22 + 0.002, center_luma * 0.70 + 0.01, neighbor_luma_max);
		float sparse_support = 1.0 - smoothstep(0.08, 0.30, bright_support_ratio);
		float unsupported_near_black = smoothstep(max(neighbor_luma * 1.38 + 0.001, mix(neighbor_luma * 2.8 + 0.025, neighbor_luma_max * 1.035 + 0.00025, stable_bright_support)), max(neighbor_luma * 1.78 + 0.010, mix(neighbor_luma * 4.0 + 0.060, neighbor_luma_max * 1.12 + 0.0025, stable_bright_support)), center_luma);
		unsupported_near_black = max(unsupported_near_black, sparse_support * smoothstep(max(0.16, neighbor_luma * 4.0 + 0.035), max(0.22, neighbor_luma * 5.0 + 0.090), center_luma));
		unsupported_near_black *= 1.0 - smoothstep(0.16, 0.42, center_luma);
		unsupported_near_black *= mix(0.55, 1.0, smoothstep(0.02, 0.45, velocity_pixels(texelFetch(velocity_buffer, pos, 0).xy)));
		unsupported_near_black = clamp(unsupported_near_black * params.denoise_strength * params.firefly_suppression * low_resolution_firefly_boost() * mix(1.0, 0.35, stable_bright_support), 0.0, 1.0);
		output_color = sanitize_color(mix(output_color, neighbor_color, unsupported_near_black * 0.90));
	}
	imageStore(output_image, pos, vec4(output_color, 1.0));
}

#endif

#ifdef MODE_FIDELITYFX_COMPOSITE

layout(set = 0, binding = 0) uniform sampler2D direct_buffer;
layout(set = 0, binding = 1) uniform sampler2D emissive_buffer;
layout(set = 0, binding = 2) uniform sampler2D indirect_buffer;
layout(set = 0, binding = 3) uniform sampler2D sky_buffer;
layout(set = 0, binding = 4) uniform sampler2D specular_buffer;
layout(set = 0, binding = 5) uniform sampler2D diffuse_buffer;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D output_image;
layout(set = 0, binding = 7) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 8) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 9) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 10) uniform sampler2D specular_guide_buffer;

float fidelityfx_signal_activity(ivec2 pos) {
	vec3 direct = sanitize_color(texelFetch(direct_buffer, pos, 0).rgb);
	vec3 emissive = sanitize_color(texelFetch(emissive_buffer, pos, 0).rgb);
	vec3 indirect = sanitize_color(texelFetch(indirect_buffer, pos, 0).rgb);
	vec3 sky = sanitize_color(texelFetch(sky_buffer, pos, 0).rgb);
	float activity = luminance(direct) + luminance(emissive) + luminance(sky);
	activity += luminance(indirect) * 0.25;
	return tonemap_luma(activity);
}

vec3 fidelityfx_full_color(ivec2 pos) {
	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec4 specular_guide = texelFetch(specular_guide_buffer, pos, 0);
	vec3 diffuse = sanitize_color(texelFetch(diffuse_buffer, pos, 0).rgb);
	vec3 specular = remodulate_specular_radiance(sanitize_color(texelFetch(specular_buffer, pos, 0).rgb), normal_roughness, albedo_metalness, specular_guide);
	return sanitize_color(diffuse + specular);
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec4 specular_guide = texelFetch(specular_guide_buffer, pos, 0);
	vec3 center_n = decode_normal(normal_roughness);
	float center_signal_activity = fidelityfx_signal_activity(pos);
	vec3 output_color = fidelityfx_full_color(pos);
	float center_luma = luminance(output_color);

	vec3 neighbor_sum = vec3(0.0);
	float neighbor_luma_sum = 0.0;
	float neighbor_luma_sq_sum = 0.0;
	float neighbor_weight_sum = 0.0;
	float bright_support_sum = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float albedo_delta = length(tap_albedo.rgb - albedo_metalness.rgb) + abs(tap_albedo.a - albedo_metalness.a);
			float guide_hitdist_scale = max(max(specular_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(specular_guide.y - tap_guide.y) / guide_hitdist_scale;
			float specular_surface = guide_specular_risk(specular_guide, normal_roughness, albedo_metalness);
			float guide_w = exp(-guide_hitdist_error / max(mix(0.48, 0.16, specular_surface), 1e-4));
			float signal_delta = abs(fidelityfx_signal_activity(tap_pos) - center_signal_activity);
			float signal_w = exp(-signal_delta * 4.0);
			float w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.20) * pow(normal_similarity, 6.0) * exp(-albedo_delta * 3.0) * signal_w * mix(1.0, guide_w, guide_active(specular_guide) * smoothstep(0.25, 0.85, specular_surface));
			vec3 tap_color = fidelityfx_full_color(tap_pos);
			float tap_luma = luminance(tap_color);
			neighbor_sum += tap_color * w;
			neighbor_luma_sum += tap_luma * w;
			neighbor_luma_sq_sum += tap_luma * tap_luma * w;
			bright_support_sum += (tap_luma > max(center_luma * 0.55, 0.08)) ? w : 0.0;
			neighbor_weight_sum += w;
		}
	}

	if (neighbor_weight_sum > 1e-5) {
		vec3 neighbor_color = neighbor_sum / neighbor_weight_sum;
		float neighbor_luma = neighbor_luma_sum / neighbor_weight_sum;
		float neighbor_variance = max(neighbor_luma_sq_sum / neighbor_weight_sum - neighbor_luma * neighbor_luma, 0.0);
		float bright_support = bright_support_sum / neighbor_weight_sum;
		float center_motion_px = velocity_pixels(texelFetch(velocity_buffer, pos, 0).xy);
		float low_roughness = 1.0 - clamp(normal_roughness.a, 0.0, 1.0);
		float metallic = clamp(albedo_metalness.a, 0.0, 1.0);
		float specular_surface = max(max(low_roughness, metallic), guide_specular_risk(specular_guide, normal_roughness, albedo_metalness));
		float sigma = sqrt(neighbor_variance);
		float outlier_limit = neighbor_luma + sigma * mix(1.25, 0.70, specular_surface) + mix(0.06, 0.02, specular_surface);
		float unsupported = smoothstep(outlier_limit, outlier_limit + mix(0.12, 0.04, specular_surface), center_luma);
		unsupported *= 1.0 - smoothstep(0.10, 0.38, bright_support);
		unsupported *= mix(0.75, 1.0, smoothstep(0.5, 7.0, center_motion_px));
		float suppress = clamp(unsupported * params.denoise_strength * params.firefly_suppression * low_resolution_firefly_boost(), 0.0, 1.0);
		output_color = sanitize_color(mix(output_color, neighbor_color, suppress));
	}

	imageStore(output_image, pos, vec4(output_color, 1.0));
}

#endif

#ifdef MODE_VOLUMETRIC_FOG

layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 2) uniform sampler3D volumetric_fog_buffer;

vec4 sample_volumetric_fog(vec2 visible_uv, float view_z) {
	float fog_z = clamp(view_z * params.fog_inv_length, 0.0, 1.0);
	fog_z = pow(fog_z, params.fog_detail_spread);
	return texture(volumetric_fog_buffer, vec3(visible_uv, fog_z));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec2 visible_pos = vec2(pos) - params.visible_origin;
	if (any(lessThan(visible_pos, vec2(0.0))) || any(greaterThanEqual(visible_pos, params.visible_size))) {
		return;
	}

	vec2 visible_uv = (visible_pos + vec2(0.5)) / max(params.visible_size, vec2(1.0));
	float view_z = texelFetch(viewz_hitdist_buffer, pos, 0).r;
	bool sky_pixel = view_z > 60000.0;
	view_z = sky_pixel ? 1.0 / max(params.fog_inv_length, 1e-5) : view_z;

	vec4 fog = sample_volumetric_fog(visible_uv, view_z);
	if (params.fog_legacy_blending > 0.5) {
		fog.rgb *= 1.0 - fog.a;
	}

	vec4 color = imageLoad(color_image, pos);
	vec3 fogged = sanitize_color(color.rgb * fog.a + fog.rgb);
	color.rgb = sky_pixel ? mix(color.rgb, fogged, params.fog_sky_affect) : fogged;
	imageStore(color_image, pos, color);
}

#endif

#ifdef MODE_VARIANCE_PREFILTER

layout(set = 0, binding = 0) uniform sampler2D temporal_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 3) uniform sampler2D variance_buffer;
layout(set = 0, binding = 4) uniform sampler2D reactivity_buffer;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D prefilter_out;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 center_nr = texelFetch(normal_roughness_buffer, pos, 0);
	vec3 center_n = decode_normal(center_nr);
	float center_z = texelFetch(viewz_hitdist_buffer, pos, 0).x;
	float variance_sum = 0.0;
	float weight_sum = 0.0;
	vec3 neighbor_color_sum = vec3(0.0);
	float neighbor_luma_sum = 0.0;
	float neighbor_luma_sq_sum = 0.0;
	float neighbor_weight_sum = 0.0;

	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			float tap_z = texelFetch(viewz_hitdist_buffer, tap_pos, 0).x;
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float normal_w = pow(normal_similarity, 8.0);
			float plane_relax = smoothstep(0.88, 0.98, normal_similarity);
			float depth_w = exp(-abs(tap_z - center_z) / max(center_z * mix(0.03, 0.12, plane_relax), 0.02));
			float spatial_w = (x == 0 && y == 0) ? 4.0 : ((x == 0 || y == 0) ? 2.0 : 1.0);
			float w = spatial_w * normal_w * depth_w;
			variance_sum += texelFetch(variance_buffer, tap_pos, 0).r * w;
			weight_sum += w;

			if (x != 0 || y != 0) {
				vec3 tap_color = texelFetch(temporal_buffer, tap_pos, 0).rgb;
				float tap_luma = luminance(tap_color);
				float neighbor_w = normal_w * depth_w;
				neighbor_color_sum += tap_color * neighbor_w;
				neighbor_luma_sum += tap_luma * neighbor_w;
				neighbor_luma_sq_sum += tap_luma * tap_luma * neighbor_w;
				neighbor_weight_sum += neighbor_w;
			}
		}
	}

	vec4 temporal = texelFetch(temporal_buffer, pos, 0);
	float center_reactivity = texelFetch(reactivity_buffer, pos, 0).r;
	if (neighbor_weight_sum > 1e-4) {
		vec3 neighbor_color = neighbor_color_sum / neighbor_weight_sum;
		float neighbor_luma = neighbor_luma_sum / neighbor_weight_sum;
		float neighbor_variance = max(neighbor_luma_sq_sum / neighbor_weight_sum - neighbor_luma * neighbor_luma, 0.0);
		float center_luma = luminance(temporal.rgb);
		float outlier_limit = neighbor_luma + sqrt(neighbor_variance) * 0.85 + 0.025;
		float outlier = smoothstep(outlier_limit, outlier_limit + 0.08, center_luma);
		vec3 capped = clamp_luminance(temporal.rgb, max(outlier_limit, neighbor_luma + 0.02));
		float reactive_detail = center_reactivity;
		float stable_bright_support = smoothstep(center_luma * 0.32 + 0.01, center_luma * 0.82 + 0.02, neighbor_luma);
		float outlier_strength = clamp(outlier * params.denoise_strength * params.firefly_suppression * low_resolution_firefly_boost() * mix(1.0, 0.35, reactive_detail) * mix(1.0, 0.35, stable_bright_support), 0.0, 1.0);
		temporal.rgb = sanitize_color(mix(temporal.rgb, mix(capped, neighbor_color, 0.92), outlier_strength));
	}
	temporal.a = variance_sum / max(weight_sum, 1e-5);
	imageStore(prefilter_out, pos, temporal);
}

#endif

#ifdef MODE_ATROUS

layout(set = 0, binding = 0) uniform sampler2D input_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 3) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 4) uniform sampler2D velocity_buffer;
layout(set = 0, binding = 5) uniform sampler2D reactivity_buffer;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D output_buffer;
layout(set = 0, binding = 7) uniform sampler2D specular_guide_buffer;
layout(set = 0, binding = 8) uniform sampler2D history_id_buffer;

float kernel_weight(int offset) {
	int a = abs(offset);
	if (a == 0) {
		return 0.375;
	}
	if (a == 1) {
		return 0.25;
	}
	if (a == 2) {
		return 0.0625;
	}
	return 0.0;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 center = texelFetch(input_buffer, pos, 0);
	vec4 center_nr = texelFetch(normal_roughness_buffer, pos, 0);
	vec3 center_n = decode_normal(center_nr);
	vec4 center_albedo = texelFetch(albedo_metalness_buffer, pos, 0);
	float center_z = texelFetch(viewz_hitdist_buffer, pos, 0).x;
	vec4 center_guide = texelFetch(specular_guide_buffer, pos, 0);
	vec4 center_id = texelFetch(history_id_buffer, pos, 0);
	vec2 center_velocity = texelFetch(velocity_buffer, pos, 0).xy;
	float center_reactivity = texelFetch(reactivity_buffer, pos, 0).r;
	float center_motion_px = velocity_pixels(center_velocity);
	float center_luma = luminance(center.rgb);
	float center_luma_t = tonemap_luma(center_luma);
	float variance = max(center.a * params.variance_boost, 1e-4);
	float specular_surface = guide_specular_risk(center_guide, center_nr, center_albedo);
	if (params.radiance_space_history > 0.5) {
		specular_surface = max(specular_surface, 0.70);
	}
	float specular_spatial_guard = smoothstep(0.25, 0.95, specular_surface);
	float roughness_filter = mix(1.45, 1.0, clamp(center_nr.a, 0.0, 1.0));
	float variance_filter = smoothstep(0.0008, 0.055, variance);
	float reactive_detail = max(center_reactivity, smoothstep(0.18, 1.0, center_motion_px) * 0.45);
	float spatial_strength = clamp(pow(params.denoise_strength, 0.85) * mix(0.50, 0.98, variance_filter) * mix(1.0, 0.76, reactive_detail), 0.0, 1.0);
	spatial_strength *= mix(1.0, 0.72, specular_spatial_guard);
	float moving_step_weight = exp(-float(max(params.step_size - 1, 0)) * (smoothstep(1.0, 16.0, center_motion_px) * 0.16 + center_reactivity * 0.24));
	float center_hit_distance = max(center_guide.y, 0.0);
	float hit_distance_filter_scale = mix(1.0, smoothstep(0.35, 7.0, center_hit_distance), guide_active(center_guide) * specular_spatial_guard);
	int filter_step_size = max(1, int(round(float(params.step_size) * hit_distance_filter_scale)));
	spatial_strength *= mix(1.0, mix(0.28, 1.0, hit_distance_filter_scale), specular_spatial_guard);
	float contact_normal_tightening = mix(1.45, 1.0, hit_distance_filter_scale);
	float contact_depth_tightening = mix(0.42, 1.0, hit_distance_filter_scale);
	float guide_roughness = guide_active(center_guide) > 0.5 ? clamp(center_guide.x, 0.0, 1.0) : clamp(center_nr.a, 0.0, 1.0);
	vec2 projected_normal = center_n.xy;
	float projected_normal_len = length(projected_normal);
	vec2 major_axis = projected_normal_len > 1e-3 ? normalize(vec2(-projected_normal.y, projected_normal.x)) : vec2(1.0, 0.0);
	vec2 minor_axis = vec2(-major_axis.y, major_axis.x);
	float glossy_lobe = 1.0 - smoothstep(0.18, 0.72, guide_roughness);
	float grazing_lobe = smoothstep(0.18, 0.88, projected_normal_len);
	float anisotropy_strength = guide_active(center_guide) * specular_spatial_guard * glossy_lobe * grazing_lobe * hit_distance_filter_scale * (1.0 - smoothstep(0.20, 0.85, reactive_detail));
	float major_stretch = mix(1.0, 2.25, anisotropy_strength);
	float minor_stretch = mix(1.0, 0.58, anisotropy_strength);

	vec3 color_sum = vec3(0.0);
	float variance_sum = 0.0;
	float weight_sum = 0.0;
	float local_albedo_detail = 0.0;
	float local_normal_detail = 0.0;

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			vec2 kernel_offset = vec2(x, y);
			vec2 anisotropic_offset = kernel_offset;
			if (anisotropy_strength > 0.001 && (x != 0 || y != 0)) {
				anisotropic_offset = major_axis * dot(kernel_offset, major_axis) * major_stretch + minor_axis * dot(kernel_offset, minor_axis) * minor_stretch;
			}
			ivec2 tap_pos = clamp(pos + ivec2(round(anisotropic_offset * float(filter_step_size))), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap = texelFetch(input_buffer, tap_pos, 0);
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			float tap_z = texelFetch(viewz_hitdist_buffer, tap_pos, 0).x;
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			vec4 tap_id = texelFetch(history_id_buffer, tap_pos, 0);
			vec2 tap_velocity = texelFetch(velocity_buffer, tap_pos, 0).xy;

			float base_w = kernel_weight(x) * kernel_weight(y);
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float normal_w = pow(normal_similarity, params.phi_normal * roughness_filter * contact_normal_tightening);
			float albedo_delta = length(tap_albedo.rgb - center_albedo.rgb);
			float metalness_delta = abs(tap_albedo.a - center_albedo.a);
			if ((x != 0 || y != 0) && abs(x) <= 1 && abs(y) <= 1) {
				local_albedo_detail = max(local_albedo_detail, albedo_delta + metalness_delta * 0.5);
				local_normal_detail = max(local_normal_detail, 1.0 - normal_similarity);
			}
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, metalness_delta);
			float depth_w = exp(-abs(tap_z - center_z) / max(center_z * params.phi_depth * contact_depth_tightening * mix(1.0, 3.2, plane_relax), 0.02));
			float albedo_w = exp(-albedo_delta * 7.0);
			float tap_luma = luminance(tap.rgb);
			float tap_luma_t = tonemap_luma(tap_luma);
			float luma_sigma = sqrt(variance);
			float luma_width = (tonemap_luma(luma_sigma * params.phi_color) * 0.50 + mix(0.12, 0.24, specular_spatial_guard)) * mix(1.0, 0.78, reactive_detail);
			float luma_w = exp(-abs(tap_luma_t - center_luma_t) / max(luma_width * mix(1.0, 3.5, plane_relax * (1.0 - reactive_detail)), 1e-4));
			float bright_center_dark_tap = smoothstep(0.03, 0.24, center_luma_t - tap_luma_t) * smoothstep(0.08, 0.65, center_luma);
			luma_w = max(luma_w, bright_center_dark_tap * params.denoise_strength * mix(0.18, 0.42, specular_surface));
			float bright_tap_limit = center_luma + luma_sigma * 1.5 + 0.2;
			float bright_tap_w = (x == 0 && y == 0) ? 1.0 : min(1.0, bright_tap_limit / max(tap_luma, 1e-4));
			float guide_hitdist_scale = max(max(center_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(center_guide.y - tap_guide.y) / guide_hitdist_scale;
			float guide_hit_w = exp(-guide_hitdist_error / max(mix(0.46, 0.16, specular_surface) * mix(0.50, 1.0, hit_distance_filter_scale), 1e-4));
			float identity_w = history_id_matches(center_id, tap_id) ? 1.0 : 0.05;
			float material_identity_guard = smoothstep(0.58, 0.90, specular_surface);
			float guide_identity_guard = max(specular_spatial_guard * guide_active(center_guide), material_identity_guard);
			float guarded_identity_w = mix(identity_w, guide_hit_w * identity_w, guide_active(center_guide));
			float metal_w = 1.0 - metalness_delta;
			float velocity_w = exp(-velocity_pixels(tap_velocity - center_velocity) * 0.35);
			float step_w = (x == 0 && y == 0) ? 1.0 : moving_step_weight;
			float guide_w = mix(1.0, guarded_identity_w, guide_identity_guard);
			float w = base_w * normal_w * depth_w * albedo_w * luma_w * bright_tap_w * velocity_w * step_w * clamp(metal_w, 0.0, 1.0) * guide_w;

			vec3 tap_color_filtered = tap.rgb;
			if (x != 0 || y != 0) {
				float tap_limit = center_luma + luma_sigma * mix(2.2, 1.1, params.firefly_suppression) + 0.15;
				if (tap_luma > tap_limit) {
					tap_color_filtered = tap.rgb * (tap_limit / max(tap_luma, 1e-4));
				}
			}
			color_sum += tap_color_filtered * w;
			variance_sum += tap.a * w * w;
			weight_sum += w;
		}
	}

	vec3 filtered_avg = color_sum / max(weight_sum, 1e-5);
	float filtered_avg_variance = variance_sum / max(weight_sum * weight_sum, 1e-5);
	float albedo_detail_guard = smoothstep(0.045, 0.20, local_albedo_detail) * diffuse_demodulation_weight(center_nr, center_albedo, center_z) * params.detail_preservation;
	float normal_detail_guard = smoothstep(0.018, 0.14, local_normal_detail) * diffuse_demodulation_weight(center_nr, center_albedo, center_z) * params.detail_preservation;
	float surface_detail_guard = max(albedo_detail_guard, normal_detail_guard);
	spatial_strength *= mix(1.0, 0.48, surface_detail_guard);
	vec3 filtered = mix(center.rgb, filtered_avg, spatial_strength);
	float filtered_variance = mix(center.a, filtered_avg_variance, spatial_strength);
	imageStore(output_buffer, pos, vec4(sanitize_color(filtered), filtered_variance));
}

#endif

#ifdef MODE_COMPOSITE

layout(set = 0, binding = 0) uniform sampler2D filtered_buffer;
layout(set = 0, binding = 1) uniform sampler2D temporal_buffer;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 3) uniform sampler2D albedo_metalness_buffer;
layout(set = 0, binding = 4) uniform sampler2D viewz_hitdist_buffer;
layout(set = 0, binding = 5) uniform sampler2D reactivity_buffer;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D output_image;
layout(set = 0, binding = 7) uniform sampler2D specular_guide_buffer;
layout(set = 0, binding = 8) uniform sampler2D history_id_buffer;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(params.resolution)))) {
		return;
	}

	vec4 filtered = texelFetch(filtered_buffer, pos, 0);
	vec4 temporal = texelFetch(temporal_buffer, pos, 0);
	vec4 albedo_metalness = texelFetch(albedo_metalness_buffer, pos, 0);
	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	float center_z = texelFetch(viewz_hitdist_buffer, pos, 0).x;
	float center_reactivity = texelFetch(reactivity_buffer, pos, 0).r;
	vec4 center_guide = texelFetch(specular_guide_buffer, pos, 0);
	vec4 center_id = texelFetch(history_id_buffer, pos, 0);
	float temporal_luma = luminance(temporal.rgb);
	float filtered_luma = luminance(filtered.rgb);
	float variance_sigma = sqrt(max(temporal.a, 0.0));
	float bright_bleed = smoothstep(temporal_luma + variance_sigma * 1.5 + 0.15, temporal_luma + variance_sigma * 4.0 + 0.75, filtered_luma);
	float low_roughness = 1.0 - clamp(normal_roughness.a, 0.0, 1.0);
	float metallic = clamp(albedo_metalness.a, 0.0, 1.0);
	float guide_specular_surface = guide_specular_risk(center_guide, normal_roughness, albedo_metalness);
	float rough_diffuse_plane = smoothstep(0.28, 0.72, normal_roughness.a) * (1.0 - metallic);
	float temporal_guard = bright_bleed * mix(0.65, 0.9, max(low_roughness, metallic)) * params.denoise_strength * mix(1.0, 0.35, center_reactivity);
	vec3 denoised = sanitize_color(mix(filtered.rgb, temporal.rgb, temporal_guard));

	vec3 center_n = decode_normal(normal_roughness);
	vec3 neighbor_color_sum = vec3(0.0);
	float neighbor_luma_sum = 0.0;
	float neighbor_luma_sq_sum = 0.0;
	float neighbor_weight_sum = 0.0;
	vec3 lower_color_sum = vec3(0.0);
	float lower_weight_sum = 0.0;
	float center_final_luma = luminance(denoised);
	float bright_support_sum = 0.0;
	float local_albedo_detail = 0.0;
	float local_normal_detail = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			float tap_z = texelFetch(viewz_hitdist_buffer, tap_pos, 0).x;
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			vec4 tap_id = texelFetch(history_id_buffer, tap_pos, 0);
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float normal_w = pow(normal_similarity, 4.0);
			float depth_scale = max(max(center_z, tap_z), 1.0);
			float relative_depth_error = abs(center_z - tap_z) / depth_scale;
			bool sky_or_far = center_z > 60000.0 && tap_z > 60000.0;
			float albedo_delta = length(tap_albedo.rgb - albedo_metalness.rgb) + abs(tap_albedo.a - albedo_metalness.a);
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, 0.0);
			if (normal_w < 0.015 || (!sky_or_far && relative_depth_error > mix(0.09, 0.25, plane_relax))) {
				continue;
			}
			if (albedo_delta > 0.7) {
				continue;
			}
			if (abs(x) <= 1 && abs(y) <= 1) {
				local_albedo_detail = max(local_albedo_detail, albedo_delta);
				local_normal_detail = max(local_normal_detail, 1.0 - normal_similarity);
			}

			vec3 tap_color = texelFetch(filtered_buffer, tap_pos, 0).rgb;
			float tap_luma = luminance(tap_color);
			float spatial_w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.12);
			float guide_hitdist_scale = max(max(center_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(center_guide.y - tap_guide.y) / guide_hitdist_scale;
			float guide_hit_w = exp(-guide_hitdist_error / max(mix(0.42, 0.14, guide_specular_surface), 1e-4));
			float identity_w = history_id_matches(center_id, tap_id) ? 1.0 : 0.04;
			float material_identity_guard = smoothstep(0.58, 0.90, guide_specular_surface);
			float guide_support = max(smoothstep(0.32, 0.72, guide_specular_surface) * guide_active(center_guide), material_identity_guard);
			float guarded_identity_w = mix(identity_w, guide_hit_w * identity_w, guide_active(center_guide));
			float w = spatial_w * normal_w * exp(-albedo_delta * 2.0) * mix(1.0, guarded_identity_w, guide_support);
			neighbor_color_sum += tap_color * w;
			neighbor_luma_sum += tap_luma * w;
			neighbor_luma_sq_sum += tap_luma * tap_luma * w;
			neighbor_weight_sum += w;
			bright_support_sum += (tap_luma > center_final_luma * 0.7 && tap_luma > 0.09) ? w : 0.0;
			float lower_gate = 1.0 - smoothstep(center_final_luma * 0.72 + 0.006, center_final_luma * 0.94 + 0.02, tap_luma);
			float lower_w = w * lower_gate;
			lower_color_sum += tap_color * lower_w;
			lower_weight_sum += lower_w;
		}
	}

	if (neighbor_weight_sum > 1e-4) {
		float specular_surface = max(max(low_roughness, metallic), guide_specular_surface);
		vec3 neighbor_color = neighbor_color_sum / neighbor_weight_sum;
		float neighbor_luma = neighbor_luma_sum / neighbor_weight_sum;
		vec3 lower_neighbor_color = lower_weight_sum > neighbor_weight_sum * 0.18 ? lower_color_sum / lower_weight_sum : neighbor_color;
		float neighbor_variance = max(neighbor_luma_sq_sum / neighbor_weight_sum - neighbor_luma * neighbor_luma, 0.0);
		float outlier_limit = neighbor_luma + sqrt(neighbor_variance) * mix(1.00, 0.55, specular_surface) + mix(0.045, 0.014, specular_surface);
		float outlier = smoothstep(outlier_limit, outlier_limit + mix(0.12, 0.038, specular_surface), luminance(denoised));
		vec3 capped = clamp_luminance(denoised, max(outlier_limit, neighbor_luma + 0.02));
		float dark_neighborhood = 1.0 - smoothstep(0.08, 0.35, neighbor_luma);
		vec3 outlier_reference = mix(neighbor_color, lower_neighbor_color, dark_neighborhood);
		float reactive_detail = center_reactivity;
		float albedo_detail_guard = smoothstep(0.045, 0.22, local_albedo_detail) * rough_diffuse_plane * params.detail_preservation;
		float normal_detail_guard = smoothstep(0.018, 0.16, local_normal_detail) * rough_diffuse_plane * params.detail_preservation;
		float surface_detail_guard = max(albedo_detail_guard, normal_detail_guard);
		float supported_texture_guard = surface_detail_guard * (1.0 - dark_neighborhood) * smoothstep(0.12, 0.50, bright_support_sum / neighbor_weight_sum);
		float final_outlier_strength = clamp(outlier * mix(0.85, 1.0, specular_surface) * params.denoise_strength * params.firefly_suppression * low_resolution_firefly_boost() * mix(1.0, 0.42, reactive_detail), 0.0, 1.0);
		final_outlier_strength *= mix(1.0, 0.72, supported_texture_guard);
		denoised = sanitize_color(mix(denoised, mix(capped, outlier_reference, mix(0.55, 0.92, specular_surface)), final_outlier_strength));
		center_final_luma = luminance(denoised);

		float bright_support = bright_support_sum / neighbor_weight_sum;
		float isolated_bright = (1.0 - smoothstep(0.08, 0.35, bright_support)) *
				smoothstep(neighbor_luma + 0.008, neighbor_luma + mix(0.105, 0.045, specular_surface), center_final_luma);
		float isolated_dim = dark_neighborhood * (1.0 - smoothstep(0.08, 0.35, bright_support)) *
				smoothstep(neighbor_luma + 0.002, neighbor_luma + 0.026, center_final_luma);
		float dark_excess = smoothstep(neighbor_luma + 0.0010, neighbor_luma + 0.018, center_final_luma);
		float dark_smooth = dark_neighborhood * dark_excess * (1.0 - smoothstep(0.20, 0.74, bright_support)) * mix(0.86, 1.0, specular_surface);
		float salt_noise = dark_neighborhood * smoothstep(neighbor_luma + 0.002, neighbor_luma + 0.018, center_final_luma) * (1.0 - smoothstep(0.30, 0.82, bright_support));
		float suppress = max(max(max(isolated_bright * mix(0.82, 1.0, max(specular_surface, dark_neighborhood)), isolated_dim), dark_smooth), salt_noise) * params.denoise_strength * params.firefly_suppression * mix(1.0, 0.48, reactive_detail);
		float dark_temporal_flash = dark_neighborhood *
				rough_diffuse_plane *
				smoothstep(0.032, 0.068, max(center_final_luma - temporal_luma, 0.0)) *
				(1.0 - smoothstep(0.14, 0.46, bright_support)) *
				(1.0 - surface_detail_guard * 0.92) *
				params.denoise_strength * params.firefly_suppression * mix(1.0, 0.45, reactive_detail);
		suppress = max(suppress, dark_temporal_flash);
		if (params.radiance_space_history > 0.5) {
			float unsupported_specular_twinkle = smoothstep(neighbor_luma + 0.003, neighbor_luma + 0.035, center_final_luma) *
					(1.0 - smoothstep(0.10, 0.75, bright_support)) *
					mix(0.55, 1.0, dark_neighborhood);
			suppress = max(suppress, unsupported_specular_twinkle * params.denoise_strength * params.firefly_suppression * mix(1.0, 0.42, reactive_detail));
		}
		suppress = min(suppress * low_resolution_firefly_boost() * mix(1.0, params.radiance_space_history > 0.5 ? 6.50 : 3.80, dark_neighborhood), 1.0);
		suppress *= mix(1.0, 0.80, supported_texture_guard);
		denoised = sanitize_color(mix(denoised, lower_neighbor_color, suppress));
		center_final_luma = luminance(denoised);

		float local_grain_delta = abs(center_final_luma - neighbor_luma);
		float local_grain = smoothstep(0.012, 0.09, local_grain_delta) *
				smoothstep(0.00008, 0.006, neighbor_variance) *
				(1.0 - smoothstep(0.35, 0.95, bright_support));
		float plane_grain_smooth = rough_diffuse_plane * local_grain * params.denoise_strength * mix(1.0, 0.35, reactive_detail);
		plane_grain_smooth *= mix(1.0, 0.16, surface_detail_guard);
		denoised = sanitize_color(mix(denoised, neighbor_color, min(plane_grain_smooth * mix(2.40, 0.58, surface_detail_guard), 0.88)));
		center_final_luma = luminance(denoised);

		float dark_flat_noise = dark_neighborhood *
				rough_diffuse_plane *
				smoothstep(0.006, 0.050, abs(center_final_luma - neighbor_luma)) *
				(1.0 - smoothstep(0.018, 0.090, local_albedo_detail)) *
				(1.0 - smoothstep(0.018, 0.14, local_normal_detail)) *
				(1.0 - smoothstep(0.18, 0.48, bright_support)) *
				params.denoise_strength * params.firefly_suppression * mix(1.0, 0.45, reactive_detail);
		denoised = sanitize_color(mix(denoised, neighbor_color, min(dark_flat_noise * 1.35, 0.84)));
		center_final_luma = luminance(denoised);

		float dark_hole = rough_diffuse_plane *
				smoothstep(0.05, 0.24, neighbor_luma) *
				smoothstep(center_final_luma + 0.018, center_final_luma + 0.16, neighbor_luma) *
				smoothstep(1.8, 6.0, neighbor_weight_sum) *
				(1.0 - smoothstep(0.03, 0.12, local_albedo_detail)) *
				(1.0 - smoothstep(0.018, 0.14, local_normal_detail)) *
				params.denoise_strength * params.firefly_suppression * mix(1.0, 0.35, reactive_detail);
		denoised = sanitize_color(mix(denoised, neighbor_color, min(dark_hole * 0.80, 0.70)));
	}

	vec3 orphan_color_sum = vec3(0.0);
	float orphan_luma_sum = 0.0;
	float orphan_luma_max = 0.0;
	float orphan_weight_sum = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap_pos = clamp(pos + ivec2(x, y), ivec2(0), ivec2(params.resolution) - ivec2(1));
			vec4 tap_nr = texelFetch(normal_roughness_buffer, tap_pos, 0);
			vec4 tap_albedo = texelFetch(albedo_metalness_buffer, tap_pos, 0);
			float tap_z = texelFetch(viewz_hitdist_buffer, tap_pos, 0).x;
			vec4 tap_guide = texelFetch(specular_guide_buffer, tap_pos, 0);
			vec4 tap_id = texelFetch(history_id_buffer, tap_pos, 0);
			float normal_similarity = max(dot(center_n, decode_normal(tap_nr)), 0.0);
			float depth_scale = max(max(center_z, tap_z), 1.0);
			float relative_depth_error = abs(center_z - tap_z) / depth_scale;
			float guide_hitdist_scale = max(max(center_guide.y, tap_guide.y), 1.0);
			float guide_hitdist_error = abs(center_guide.y - tap_guide.y) / guide_hitdist_scale;
			float albedo_delta = length(tap_albedo.rgb - albedo_metalness.rgb) + abs(tap_albedo.a - albedo_metalness.a);
			float plane_relax = stable_surface_match(normal_similarity, albedo_delta, 0.0);
			if (normal_similarity < 0.82 || relative_depth_error > mix(0.08, 0.20, plane_relax) || albedo_delta > 0.55) {
				continue;
			}
			float orphan_material_identity_guard = smoothstep(0.58, 0.90, guide_specular_surface);
			if (orphan_material_identity_guard > 0.5 && !history_id_matches(center_id, tap_id)) {
				continue;
			}
			if (guide_active(center_guide) > 0.5 && guide_specular_surface > 0.45 && guide_hitdist_error > mix(0.36, 0.14, guide_specular_surface)) {
				continue;
			}

			vec3 tap_color = texelFetch(filtered_buffer, tap_pos, 0).rgb;
			float tap_luma = luminance(tap_color);
			float w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.35) * pow(normal_similarity, 4.0) * exp(-albedo_delta * 2.0);
			orphan_color_sum += tap_color * w;
			orphan_luma_sum += tap_luma * w;
			orphan_luma_max = max(orphan_luma_max, tap_luma);
			orphan_weight_sum += w;
		}
	}
	if (orphan_weight_sum > 1e-4) {
		vec3 orphan_avg = orphan_color_sum / orphan_weight_sum;
		float orphan_luma = orphan_luma_sum / orphan_weight_sum;
		center_final_luma = luminance(denoised);
		float no_bright_support = 1.0 - smoothstep(center_final_luma * 0.08, center_final_luma * 0.22 + 0.02, orphan_luma_max);
		float dark_orphan = 1.0 - smoothstep(0.02, 0.16, max(orphan_luma, orphan_luma_max));
		float orphan_limit = mix(max(orphan_luma * 7.0 + 0.28, orphan_luma_max * 3.0 + 0.22), max(orphan_luma * 2.6 + 0.045, orphan_luma_max * 1.7 + 0.035), dark_orphan);
		float orphan_hot = smoothstep(orphan_limit, orphan_limit + max(orphan_limit * 0.18, 0.06), center_final_luma) * no_bright_support * params.denoise_strength * params.firefly_suppression;
		orphan_hot = max(orphan_hot, dark_orphan * smoothstep(orphan_luma + 0.010, orphan_luma + 0.050, center_final_luma) * params.denoise_strength * params.firefly_suppression);
		denoised = sanitize_color(mix(denoised, orphan_avg, clamp(orphan_hot, 0.0, 1.0)));
	}

	denoised = unpack_radiance(denoised);
	if (params.radiance_space_history <= 0.5) {
		denoised = remodulate_radiance(denoised, normal_roughness, albedo_metalness, center_z);
	}
	imageStore(output_image, pos, vec4(denoised, 1.0));
}

#endif
