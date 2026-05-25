#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 8
#define MAX_RADIANCE 32768.0

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float max_history;
	float pad0;
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

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= int(params.resolution.x) || pos.y >= int(params.resolution.y)) {
		return;
	}

	vec3 current = sanitize_color(imageLoad(source_image, pos).rgb);
	float current_luma = luminance(current);
	vec4 current_normal_roughness = texelFetch(normal_roughness_buffer, pos, 0);
	vec4 current_viewz_hitdist = texelFetch(viewz_hitdist_buffer, pos, 0);
	vec4 confidence_signal = texelFetch(signal_confidence_buffer, pos, 0);
	float current_valid = texelFetch(history_validity_buffer, pos, 0).r >= 0.5 ? 1.0 : 0.0;
	float guide_valid = current_viewz_hitdist.x < 60000.0 ? 1.0 : 0.0;
	float clamp_risk = clamp(confidence_signal.r * 0.35 + confidence_signal.g * 0.55, 0.0, 1.0);
	float current_confidence = current_valid * guide_valid * clamp(1.0 - clamp_risk, 0.0, 1.0);

	vec3 filtered = current;
	float out_age = 1.0;
	float out_confidence = current_confidence * 0.5;
	float rejection = 0.0;
	float hit = 0.0;

	vec2 uv = (vec2(pos) + vec2(0.5)) / params.resolution;
	vec2 prev_uv = uv + texelFetch(velocity_buffer, pos, 0).xy;
	ivec2 prev_pos = ivec2(floor(prev_uv * params.resolution));

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
	if (reusable && !history_id_matches(texelFetch(history_id_buffer, pos, 0), texelFetch(prev_history_id_buffer, prev_pos, 0))) {
		reusable = false;
		rejection = 4.0;
	}

	vec4 previous_radiance_sample = reusable ? texelFetch(previous_radiance, prev_pos, 0) : vec4(0.0);
	vec4 previous_meta_sample = reusable ? texelFetch(previous_meta, prev_pos, 0) : vec4(0.5, 0.5, 1.0, 65504.0);
	vec4 previous_stats_sample = reusable ? texelFetch(previous_stats, prev_pos, 0) : vec4(65504.0, 0.0, 0.0, 0.0);
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
		if (normal_dot < 0.88) {
			reusable = false;
			rejection = 5.0;
		}
	}
	if (reusable) {
		float depth_delta = relative_delta(current_viewz_hitdist.x, previous_meta_sample.w, 0.25);
		float hit_delta = relative_delta(current_viewz_hitdist.y, previous_stats_sample.x, 0.25);
		if (depth_delta > 0.08 || hit_delta > 0.20) {
			reusable = false;
			rejection = 6.0;
		}
	}
	if (reusable) {
		float radiance_delta = relative_delta(current_luma, previous_luma, 0.08);
		if (radiance_delta > 3.0) {
			reusable = false;
			rejection = 7.0;
		}
	}
	if (reusable) {
		float variance_ratio = previous_sigma / max(previous_mean, 0.08);
		if (previous_age < 1.0 || previous_confidence <= 0.04 || variance_ratio > 1.85) {
			reusable = false;
			rejection = 8.0;
		}
	}

	if (reusable) {
		hit = 1.0;
		float radiance_delta = relative_delta(current_luma, previous_luma, 0.08);
		float normal_weight = smoothstep(0.88, 0.98, dot(decode_normal(current_normal_roughness), decode_normal(previous_meta_sample)));
		float delta_weight = 1.0 - smoothstep(0.75, 2.25, radiance_delta);
		float age_weight = smoothstep(1.0, 12.0, previous_age);
		float confidence = clamp(min(current_confidence, previous_confidence) * normal_weight * delta_weight, 0.0, 1.0);
		float history_weight = min(0.93, mix(0.50, 0.88, age_weight) * confidence);
		float max_previous_luma = max(current_luma * 3.15 + 0.08, previous_mean + previous_sigma * 2.50 + 0.05);
		vec3 clamped_previous = clamp_luminance(previous, max_previous_luma);
		filtered = sanitize_color(mix(current, clamped_previous, history_weight));
		out_age = min(previous_age + 1.0, params.max_history);
		out_confidence = clamp(mix(current_confidence, previous_confidence, history_weight) + 0.05, 0.0, 1.0);
	}

	float filtered_luma = luminance(filtered);
	float moment_weight = reusable ? min(0.94, 0.48 + out_age * 0.035) * out_confidence : 0.0;
	float next_mean = mix(filtered_luma, previous_mean, moment_weight);
	float next_second = mix(filtered_luma * filtered_luma, previous_second, moment_weight);

	imageStore(output_image, pos, vec4(filtered, 1.0));
	imageStore(next_radiance_image, pos, vec4(filtered, out_confidence));
	imageStore(next_meta_image, pos, vec4(current_normal_roughness.xyz, current_viewz_hitdist.x));
	imageStore(next_stats_image, pos, vec4(current_viewz_hitdist.y, next_mean, next_second, out_age));
	imageStore(diagnostic_image, pos, vec4(hit, out_confidence, clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0), rejection / 8.0));
	imageStore(age_image, pos, vec4(clamp(out_age / max(params.max_history, 1.0), 0.0, 1.0)));
	imageStore(rejection_image, pos, vec4(rejection / 8.0));
}
