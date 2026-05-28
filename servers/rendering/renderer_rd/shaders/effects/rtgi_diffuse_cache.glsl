#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 8
#define MAX_RADIANCE 32768.0

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	vec2 cache_resolution;
	float max_history;
	uint mode;
	uvec2 pad0;
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

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(MAX_RADIANCE), isinf(color));
	return clamp(color, vec3(0.0), vec3(MAX_RADIANCE));
}

float luminance(vec3 color) {
	return max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0);
}

vec3 clamp_luminance(vec3 color, float max_luma) {
	float luma = luminance(color);
	return luma > max_luma ? color * (max_luma / max(luma, 1e-4)) : color;
}

vec3 decode_normal(vec4 normal_roughness) {
	return normalize(normal_roughness.xyz * 2.0 - 1.0);
}

bool history_id_matches(vec4 a, vec4 b) {
	return max(max(abs(a.x - b.x), abs(a.y - b.y)), max(abs(a.z - b.z), abs(a.w - b.w))) < (0.5 / 255.0);
}

float relative_delta(float a, float b, float floor_value) {
	return abs(a - b) / max(max(a, b), floor_value);
}

float signal_clamp_risk(vec4 confidence_signal) {
	return clamp(confidence_signal.r * 0.35 + confidence_signal.g * 0.55, 0.0, 1.0);
}

ivec2 output_size_i() {
	return ivec2(params.resolution);
}

ivec2 cache_size_i() {
	return ivec2(params.cache_resolution);
}

ivec2 output_pos_to_cache_pos(ivec2 pos) {
	ivec2 cache_size = cache_size_i();
	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	return clamp(ivec2(floor(uv * params.cache_resolution)), ivec2(0), cache_size - ivec2(1));
}

ivec2 cache_pos_to_output_center(ivec2 cache_pos) {
	ivec2 output_size = output_size_i();
	vec2 uv = (vec2(cache_pos) + vec2(0.5)) / params.cache_resolution;
	return clamp(ivec2(floor(uv * params.resolution)), ivec2(0), output_size - ivec2(1));
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
	vec4 current_history_id = texelFetch(history_id_buffer, pos, 0);
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
		if (!history_id_matches(current_history_id, texelFetch(history_id_buffer, candidate_pos, 0))) {
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

		float candidate_luma = luminance(sanitize_color(imageLoad(source_image, candidate_pos).rgb));
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

void update_cache_entry(ivec2 cache_pos) {
	ivec2 pos = cache_pos_to_output_center(cache_pos);
	vec3 current = sanitize_color(imageLoad(source_image, pos).rgb);
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
	float current_confidence = current_valid * guide_valid * clamp(1.0 - clamp_risk, 0.0, 1.0);

	vec3 filtered = current;
	float out_age = 1.0;
	float out_confidence = current_confidence * 0.5;
	float rejection = 0.0;

	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	vec2 prev_uv = uv + texelFetch(velocity_buffer, pos, 0).xy;
	ivec2 prev_pos = ivec2(floor(prev_uv * params.resolution));
	ivec2 prev_cache_pos = clamp(ivec2(floor(prev_uv * params.cache_resolution)), ivec2(0), cache_size_i() - ivec2(1));
	vec4 current_history_id = texelFetch(history_id_buffer, pos, 0);

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
	if (reusable && !history_id_matches(current_history_id, texelFetch(prev_history_id_buffer, prev_pos, 0))) {
		reusable = false;
		rejection = 4.0;
	}

	vec4 previous_radiance_sample = reusable ? texelFetch(previous_radiance, prev_cache_pos, 0) : vec4(0.0);
	vec4 previous_meta_sample = reusable ? texelFetch(previous_meta, prev_cache_pos, 0) : vec4(0.5, 0.5, 1.0, 65504.0);
	vec4 previous_stats_sample = reusable ? texelFetch(previous_stats, prev_cache_pos, 0) : vec4(65504.0, 0.0, 0.0, 0.0);
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

	imageStore(next_radiance_image, cache_pos, vec4(filtered, out_confidence));
	imageStore(next_meta_image, cache_pos, vec4(current_normal_roughness.xyz, current_viewz_hitdist.x));
	imageStore(next_stats_image, cache_pos, vec4(current_viewz_hitdist.y, next_mean, next_second, out_age));
}

bool load_reconstruction_candidate(ivec2 candidate_pos, vec3 current, float current_luma, vec4 current_normal_roughness, vec4 current_viewz_hitdist, out vec4 candidate_radiance, out vec4 candidate_meta, out vec4 candidate_stats, out float candidate_quality) {
	candidate_quality = 0.0;
	ivec2 cache_size = cache_size_i();
	if (candidate_pos.x < 0 || candidate_pos.y < 0 || candidate_pos.x >= cache_size.x || candidate_pos.y >= cache_size.y) {
		return false;
	}

	candidate_radiance = texelFetch(previous_radiance, candidate_pos, 0);
	candidate_meta = texelFetch(previous_meta, candidate_pos, 0);
	candidate_stats = texelFetch(previous_stats, candidate_pos, 0);
	float candidate_age = candidate_stats.w;
	float candidate_confidence = candidate_radiance.a;
	if (candidate_age < 1.0 || candidate_confidence <= 0.04) {
		return false;
	}

	float roughness = current_normal_roughness.a;
	vec3 current_normal = decode_normal(current_normal_roughness);
	vec3 candidate_normal = decode_normal(candidate_meta);
	float normal_threshold = mix(0.72, 0.02, clamp(roughness, 0.0, 1.0));
	float normal_dot = dot(current_normal, candidate_normal);
	if (normal_dot < normal_threshold) {
		return false;
	}

	float depth_delta = relative_delta(current_viewz_hitdist.x, candidate_meta.w, 0.25);
	float hit_delta = relative_delta(current_viewz_hitdist.y, candidate_stats.x, 0.25);
	float depth_threshold = mix(0.10, 0.35, clamp(roughness, 0.0, 1.0));
	float hit_threshold = mix(0.28, 1.50, clamp(roughness, 0.0, 1.0));
	if (depth_delta > depth_threshold || hit_delta > hit_threshold) {
		return false;
	}

	vec3 candidate = sanitize_color(candidate_radiance.rgb);
	float candidate_luma = luminance(candidate);
	float radiance_delta = relative_delta(current_luma, candidate_luma, 0.08);
	float variance_ratio = variance_ratio_from_stats(candidate_stats);
	if (radiance_delta > 2.50 || variance_ratio > 1.85) {
		return false;
	}

	float normal_weight = smoothstep(normal_threshold, 0.98, normal_dot);
	float depth_weight = 1.0 - smoothstep(depth_threshold * 0.35, depth_threshold, depth_delta);
	float hit_weight = 1.0 - smoothstep(hit_threshold * 0.35, hit_threshold, hit_delta);
	float delta_weight = 1.0 - smoothstep(0.75, 2.50, radiance_delta);
	float variance_weight = 1.0 - smoothstep(0.35, 1.85, variance_ratio);
	float age_weight = smoothstep(1.0, 18.0, candidate_age);
	candidate_quality = candidate_confidence * normal_weight * depth_weight * hit_weight * delta_weight * variance_weight * mix(0.50, 1.0, age_weight);
	return candidate_quality > 0.04;
}

void reconstruct_output_pixel(ivec2 pos) {
	vec3 current = sanitize_color(imageLoad(source_image, pos).rgb);
	vec4 current_normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 current_viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
	vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
	float clamp_activity = 0.0;
	current = clamp_current_diffuse_outlier(pos, current, current_normal_roughness, current_viewz_hitdist, confidence_signal, clamp_activity);
	float current_luma = luminance(current);
	float current_valid = texelFetch(history_validity_buffer, pos, 0).r >= 0.5 ? 1.0 : 0.0;
	float guide_valid = current_viewz_hitdist.x < 60000.0 ? 1.0 : 0.0;
	float clamp_risk = signal_clamp_risk(confidence_signal);
	float current_confidence = current_valid * guide_valid * clamp(1.0 - clamp_risk, 0.0, 1.0);

	ivec2 base_cache_pos = output_pos_to_cache_pos(pos);
	vec4 cache_radiance_sample = vec4(0.0);
	vec4 cache_meta_sample = vec4(0.5, 0.5, 1.0, 65504.0);
	vec4 cache_stats_sample = vec4(65504.0, 0.0, 0.0, 0.0);
	float best_quality = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec4 candidate_radiance;
			vec4 candidate_meta;
			vec4 candidate_stats;
			float candidate_quality = 0.0;
			ivec2 candidate_pos = base_cache_pos + ivec2(x, y);
			if (load_reconstruction_candidate(candidate_pos, current, current_luma, current_normal_roughness, current_viewz_hitdist, candidate_radiance, candidate_meta, candidate_stats, candidate_quality)) {
				candidate_quality *= (x == 0 && y == 0) ? 1.0 : 0.82;
				if (candidate_quality > best_quality) {
					best_quality = candidate_quality;
					cache_radiance_sample = candidate_radiance;
					cache_meta_sample = candidate_meta;
					cache_stats_sample = candidate_stats;
				}
			}
		}
	}

	vec3 filtered = current;
	float out_age = 1.0;
	float out_confidence = current_confidence * 0.5;
	float hit = 0.0;
	float rejection = 0.0;
	if (current_confidence <= 0.05) {
		rejection = 1.0;
	} else if (best_quality <= 0.0) {
		rejection = 8.0;
	} else {
		hit = 1.0;
		vec3 cached = sanitize_color(cache_radiance_sample.rgb);
		float cached_luma = luminance(cached);
		float cached_age = cache_stats_sample.w;
		float cached_confidence = cache_radiance_sample.a;
		float cached_mean = max(cache_stats_sample.y, 0.0);
		float cached_second = max(cache_stats_sample.z, 0.0);
		float cached_variance = max(cached_second - cached_mean * cached_mean, 0.0);
		float cached_sigma = sqrt(cached_variance);
		float radiance_delta = relative_delta(current_luma, cached_luma, 0.08);
		float normal_threshold = mix(0.72, 0.02, clamp(current_normal_roughness.a, 0.0, 1.0));
		float normal_weight = smoothstep(normal_threshold, 0.98, dot(decode_normal(current_normal_roughness), decode_normal(cache_meta_sample)));
		float delta_weight = 1.0 - smoothstep(0.75, 2.50, radiance_delta);
		float variance_ratio = cached_sigma / max(cached_mean, 0.08);
		float variance_weight = 1.0 - smoothstep(0.35, 1.85, variance_ratio);
		float age_weight = smoothstep(1.0, 18.0, cached_age);
		float history_weight = min(0.82, mix(0.32, 0.78, age_weight) * min(current_confidence, cached_confidence) * normal_weight * delta_weight * mix(0.55, 1.0, variance_weight));
		float max_cached_luma = max(current_luma * 2.35 + 0.05, cached_mean + cached_sigma * 2.0 + 0.04);
		filtered = sanitize_color(mix(current, clamp_luminance(cached, max_cached_luma), history_weight));
		out_age = min(cached_age, params.max_history);
		out_confidence = clamp(mix(current_confidence, cached_confidence, history_weight) + 0.03, 0.0, 1.0);
	}

	imageStore(output_image, pos, vec4(filtered, 1.0));
	imageStore(diagnostic_image, pos, vec4(hit, out_confidence, clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0), rejection / 8.0));
	imageStore(age_image, pos, vec4(clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0)));
	imageStore(rejection_image, pos, vec4(rejection / 8.0));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (params.mode == 0u) {
		if (pos.x >= int(params.cache_resolution.x) || pos.y >= int(params.cache_resolution.y)) {
			return;
		}
		update_cache_entry(pos);
	} else {
		if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
			return;
		}
		reconstruct_output_pixel(pos);
	}
}
