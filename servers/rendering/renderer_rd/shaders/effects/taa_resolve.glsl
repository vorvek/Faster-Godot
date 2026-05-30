///////////////////////////////////////////////////////////////////////////////////
// Copyright(c) 2016-2022 Panos Karabelas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////
// File changes (yyyy-mm-dd)
// 2025-11-05: Jakub Brzyski: Added dynamic variance, base variance value adjusted to reduce ghosting
// 2022-05-06: Panos Karabelas: first commit
// 2020-12-05: Joan Fons: convert to Vulkan and Godot
///////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// Based on Spartan Engine's TAA implementation (without TAA upscale).
// <https://github.com/PanosK92/SpartanEngine/blob/a8338d0609b85dc32f3732a5c27fb4463816a3b9/Data/shaders/temporal_antialiasing.hlsl>

#define GROUP_SIZE 8
#define FLT_MIN 0.00000001
#define FLT_MAX 32767.0
#define RPC_9 0.11111111111

#define DISOCCLUSION_SCALE 0.01 // Scale the weight of this pixel calculated as (change in velocity - threshold) * scale.

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D color_buffer;
layout(set = 0, binding = 1) uniform sampler2D depth_buffer;
layout(rg16f, set = 0, binding = 2) uniform restrict readonly image2D velocity_buffer;
layout(rg16f, set = 0, binding = 3) uniform restrict readonly image2D last_velocity_buffer;
layout(set = 0, binding = 4) uniform sampler2D history_buffer;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D output_buffer;
layout(set = 0, binding = 6) uniform sampler2D rt_history_validity_buffer;
layout(set = 0, binding = 7) uniform sampler2D rt_prev_history_validity_buffer;
layout(set = 0, binding = 8) uniform sampler2D rt_history_id_buffer;
layout(set = 0, binding = 9) uniform sampler2D rt_prev_history_id_buffer;
layout(set = 0, binding = 10) uniform sampler2D rt_taa_reactivity_buffer;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float disocclusion_threshold; // 0.1 / max(params.resolution.x, params.resolution.y)
	float variance_dynamic;
	float raytracing_denoise;
	float rt_history_validity_enabled;
	float rt_history_id_enabled;
	float history_weight;
	float sharpness;
	float rt_history_filter_strength;
	float rt_taa_reactivity_enabled;
}
params;

const ivec2 kOffsets3x3[9] = {
	ivec2(-1, -1),
	ivec2(0, -1),
	ivec2(1, -1),
	ivec2(-1, 0),
	ivec2(0, 0),
	ivec2(1, 0),
	ivec2(-1, 1),
	ivec2(0, 1),
	ivec2(1, 1),
};

/*------------------------------------------------------------------------------
						THREAD GROUP SHARED MEMORY (LDS)
------------------------------------------------------------------------------*/

const int kBorderSize = 1;
const int kGroupSize = GROUP_SIZE;
const int kTileDimension = kGroupSize + kBorderSize * 2;
const int kTileDimension2 = kTileDimension * kTileDimension;

vec3 reinhard(vec3 hdr) {
	return hdr / (hdr + 1.0);
}
vec3 reinhard_inverse(vec3 sdr) {
	return sdr / (1.0 - sdr);
}

const vec3 lumCoeff = vec3(0.299f, 0.587f, 0.114f);

float luminance(vec3 color) {
	return max(dot(color, lumCoeff), 0.0001f);
}

vec3 sanitize_color(vec3 color) {
	color = mix(color, vec3(0.0), isnan(color));
	color = mix(color, vec3(FLT_MAX), isinf(color));
	return clamp(color, vec3(0.0), vec3(FLT_MAX));
}

float get_depth(ivec2 thread_id) {
	return texelFetch(depth_buffer, thread_id, 0).r;
}

shared vec3 tile_color[kTileDimension][kTileDimension];
shared float tile_depth[kTileDimension][kTileDimension];

vec3 load_color(ivec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_color[group_thread_id.x][group_thread_id.y];
}

vec3 load_color_screen(ivec2 screen_pos) {
	screen_pos = clamp(screen_pos, ivec2(0), ivec2(params.resolution) - ivec2(1));
	return sanitize_color(imageLoad(color_buffer, screen_pos).rgb);
}

void store_color(uvec2 group_thread_id, vec3 color) {
	tile_color[group_thread_id.x][group_thread_id.y] = color;
}

float load_depth(ivec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_depth[group_thread_id.x][group_thread_id.y];
}

void store_depth(uvec2 group_thread_id, float depth) {
	tile_depth[group_thread_id.x][group_thread_id.y] = depth;
}

void store_color_depth(uvec2 group_thread_id, ivec2 thread_id) {
	// out of bounds clamp
	thread_id = clamp(thread_id, ivec2(0, 0), ivec2(params.resolution) - ivec2(1, 1));

	store_color(group_thread_id, sanitize_color(imageLoad(color_buffer, thread_id).rgb));
	store_depth(group_thread_id, get_depth(thread_id));
}

void populate_group_shared_memory(uvec2 group_id, uint group_index) {
	// Populate group shared memory
	ivec2 group_top_left = ivec2(group_id) * kGroupSize - kBorderSize;
	if (group_index < (kTileDimension2 >> 2)) {
		ivec2 group_thread_id_1 = ivec2(group_index % kTileDimension, group_index / kTileDimension);
		ivec2 group_thread_id_2 = ivec2((group_index + (kTileDimension2 >> 2)) % kTileDimension, (group_index + (kTileDimension2 >> 2)) / kTileDimension);
		ivec2 group_thread_id_3 = ivec2((group_index + (kTileDimension2 >> 1)) % kTileDimension, (group_index + (kTileDimension2 >> 1)) / kTileDimension);
		ivec2 group_thread_id_4 = ivec2((group_index + kTileDimension2 * 3 / 4) % kTileDimension, (group_index + kTileDimension2 * 3 / 4) / kTileDimension);

		store_color_depth(group_thread_id_1, group_top_left + group_thread_id_1);
		store_color_depth(group_thread_id_2, group_top_left + group_thread_id_2);
		store_color_depth(group_thread_id_3, group_top_left + group_thread_id_3);
		store_color_depth(group_thread_id_4, group_top_left + group_thread_id_4);
	}

	// Wait for group threads to load store data.
	groupMemoryBarrier();
	barrier();
}

/*------------------------------------------------------------------------------
								VELOCITY
------------------------------------------------------------------------------*/

void depth_test_closest(ivec2 pos, inout float closest_depth, inout ivec2 closest_pos) {
	float depth = load_depth(pos);

	if (depth > closest_depth) {
		closest_depth = depth;
		closest_pos = pos;
	}
}

// Returns velocity with closest depth (3x3 neighborhood)
void get_closest_pixel_velocity_3x3(in uvec2 group_pos, ivec2 group_top_left, out vec2 velocity) {
	float closest_depth = 0.0;
	ivec2 local_pos = ivec2(group_pos);
	ivec2 closest_pos = local_pos;

	depth_test_closest(local_pos + kOffsets3x3[0], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[1], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[2], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[3], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[4], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[5], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[6], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[7], closest_depth, closest_pos);
	depth_test_closest(local_pos + kOffsets3x3[8], closest_depth, closest_pos);

	// Velocity out
	ivec2 velocity_pos = clamp(group_top_left + closest_pos + ivec2(kBorderSize), ivec2(0), ivec2(params.resolution) - ivec2(1));
	velocity = imageLoad(velocity_buffer, velocity_pos).xy;
}

/*------------------------------------------------------------------------------
							  HISTORY SAMPLING
------------------------------------------------------------------------------*/

vec3 sample_catmull_rom_9(sampler2D stex, vec2 uv, vec2 resolution) {
	// Source: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	// License: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae

	// We're going to sample a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
	// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
	// location [1, 1] in the grid, where [0, 0] is the top left corner.
	vec2 sample_pos = uv * resolution;
	vec2 texPos1 = floor(sample_pos - 0.5f) + 0.5f;

	// Compute the fractional offset from our starting texel to our original sample location, which we'll
	// feed into the Catmull-Rom spline function to get our filter weights.
	vec2 f = sample_pos - texPos1;

	// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
	// These equations are pre-expanded based on our knowledge of where the texels will be located,
	// which lets us avoid having to evaluate a piece-wise function.
	vec2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	vec2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	vec2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	vec2 w3 = f * f * (-0.5f + 0.5f * f);

	// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
	// simultaneously evaluate the middle 2 samples from the 4x4 grid.
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / (w1 + w2);

	// Compute the final UV coordinates we'll use for sampling the texture
	vec2 texPos0 = texPos1 - 1.0f;
	vec2 texPos3 = texPos1 + 2.0f;
	vec2 texPos12 = texPos1 + offset12;

	texPos0 /= resolution;
	texPos3 /= resolution;
	texPos12 /= resolution;

	vec3 result = vec3(0.0f, 0.0f, 0.0f);

	result += textureLod(stex, vec2(texPos0.x, texPos0.y), 0.0).xyz * w0.x * w0.y;
	result += textureLod(stex, vec2(texPos12.x, texPos0.y), 0.0).xyz * w12.x * w0.y;
	result += textureLod(stex, vec2(texPos3.x, texPos0.y), 0.0).xyz * w3.x * w0.y;

	result += textureLod(stex, vec2(texPos0.x, texPos12.y), 0.0).xyz * w0.x * w12.y;
	result += textureLod(stex, vec2(texPos12.x, texPos12.y), 0.0).xyz * w12.x * w12.y;
	result += textureLod(stex, vec2(texPos3.x, texPos12.y), 0.0).xyz * w3.x * w12.y;

	result += textureLod(stex, vec2(texPos0.x, texPos3.y), 0.0).xyz * w0.x * w3.y;
	result += textureLod(stex, vec2(texPos12.x, texPos3.y), 0.0).xyz * w12.x * w3.y;
	result += textureLod(stex, vec2(texPos3.x, texPos3.y), 0.0).xyz * w3.x * w3.y;

	return max(result, 0.0f);
}

/*------------------------------------------------------------------------------
							  HISTORY CLIPPING
------------------------------------------------------------------------------*/

// Based on "Temporal Reprojection Anti-Aliasing" - https://github.com/playdeadgames/temporal
vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 p, vec3 q) {
	vec3 r = q - p;
	vec3 rmax = (aabb_max - p.xyz);
	vec3 rmin = (aabb_min - p.xyz);

	if (r.x > rmax.x + FLT_MIN) {
		r *= (rmax.x / r.x);
	}
	if (r.y > rmax.y + FLT_MIN) {
		r *= (rmax.y / r.y);
	}
	if (r.z > rmax.z + FLT_MIN) {
		r *= (rmax.z / r.z);
	}

	if (r.x < rmin.x - FLT_MIN) {
		r *= (rmin.x / r.x);
	}
	if (r.y < rmin.y - FLT_MIN) {
		r *= (rmin.y / r.y);
	}
	if (r.z < rmin.z - FLT_MIN) {
		r *= (rmin.z / r.z);
	}

	return p + r;
}

vec3 rt_firefly_clamp_3x3(ivec2 group_pos, vec3 center) {
	vec3 n1 = load_color(group_pos + kOffsets3x3[0]);
	vec3 n2 = load_color(group_pos + kOffsets3x3[1]);
	vec3 n3 = load_color(group_pos + kOffsets3x3[2]);
	vec3 n4 = load_color(group_pos + kOffsets3x3[3]);
	vec3 n6 = load_color(group_pos + kOffsets3x3[5]);
	vec3 n7 = load_color(group_pos + kOffsets3x3[6]);
	vec3 n8 = load_color(group_pos + kOffsets3x3[7]);
	vec3 n9 = load_color(group_pos + kOffsets3x3[8]);

	float l1 = luminance(n1);
	float l2 = luminance(n2);
	float l3 = luminance(n3);
	float l4 = luminance(n4);
	float l6 = luminance(n6);
	float l7 = luminance(n7);
	float l8 = luminance(n8);
	float l9 = luminance(n9);
	float mean = (l1 + l2 + l3 + l4 + l6 + l7 + l8 + l9) * 0.125;
	float mean2 = (l1 * l1 + l2 * l2 + l3 * l3 + l4 * l4 + l6 * l6 + l7 * l7 + l8 * l8 + l9 * l9) * 0.125;
	float deviation = sqrt(max(mean2 - mean * mean, 0.0));
	float limit = max(mean + deviation * 2.0, mean * 2.0 + 0.02);
	float center_luma = luminance(center);

	if (center_luma > limit) {
		return center * (limit / center_luma);
	}

	return center;
}

vec3 rt_firefly_clamp_screen_3x3(ivec2 screen_pos, vec3 center) {
	vec3 n1 = load_color_screen(screen_pos + kOffsets3x3[0]);
	vec3 n2 = load_color_screen(screen_pos + kOffsets3x3[1]);
	vec3 n3 = load_color_screen(screen_pos + kOffsets3x3[2]);
	vec3 n4 = load_color_screen(screen_pos + kOffsets3x3[3]);
	vec3 n6 = load_color_screen(screen_pos + kOffsets3x3[5]);
	vec3 n7 = load_color_screen(screen_pos + kOffsets3x3[6]);
	vec3 n8 = load_color_screen(screen_pos + kOffsets3x3[7]);
	vec3 n9 = load_color_screen(screen_pos + kOffsets3x3[8]);

	float l1 = luminance(n1);
	float l2 = luminance(n2);
	float l3 = luminance(n3);
	float l4 = luminance(n4);
	float l6 = luminance(n6);
	float l7 = luminance(n7);
	float l8 = luminance(n8);
	float l9 = luminance(n9);
	float mean = (l1 + l2 + l3 + l4 + l6 + l7 + l8 + l9) * 0.125;
	float mean2 = (l1 * l1 + l2 * l2 + l3 * l3 + l4 * l4 + l6 * l6 + l7 * l7 + l8 * l8 + l9 * l9) * 0.125;
	float deviation = sqrt(max(mean2 - mean * mean, 0.0));
	float limit = max(mean + deviation * 2.0, mean * 2.0 + 0.02);
	float center_luma = luminance(center);

	if (center_luma > limit) {
		return center * (limit / center_luma);
	}

	return center;
}

// Clip history to the neighbourhood of the current sample
vec3 clip_history_3x3(uvec2 group_pos, uvec2 screen_pos, vec3 color_history, vec2 velocity_closest, out vec3 neighborhood_avg, out vec3 neighborhood_min, out vec3 neighborhood_max) {
	ivec2 local_pos = ivec2(group_pos);

	// Sample a 3x3 neighbourhood
	vec3 s1 = load_color(local_pos + kOffsets3x3[0]);
	vec3 s2 = load_color(local_pos + kOffsets3x3[1]);
	vec3 s3 = load_color(local_pos + kOffsets3x3[2]);
	vec3 s4 = load_color(local_pos + kOffsets3x3[3]);
	vec3 s5 = load_color(local_pos + kOffsets3x3[4]);
	vec3 s6 = load_color(local_pos + kOffsets3x3[5]);
	vec3 s7 = load_color(local_pos + kOffsets3x3[6]);
	vec3 s8 = load_color(local_pos + kOffsets3x3[7]);
	vec3 s9 = load_color(local_pos + kOffsets3x3[8]);

	if (params.raytracing_denoise > 0.5) {
		ivec2 screen = ivec2(screen_pos);
		s1 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[0], s1);
		s2 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[1], s2);
		s3 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[2], s3);
		s4 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[3], s4);
		s5 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[4], s5);
		s6 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[5], s6);
		s7 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[6], s7);
		s8 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[7], s8);
		s9 = rt_firefly_clamp_screen_3x3(screen + kOffsets3x3[8], s9);
	}

	// Compute min and max (with an adaptive box size, which greatly reduces ghosting)
	vec3 color_avg = (s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9) * RPC_9;
	vec3 color_avg2 = ((s1 * s1) + (s2 * s2) + (s3 * s3) + (s4 * s4) + (s5 * s5) + (s6 * s6) + (s7 * s7) + (s8 * s8) + (s9 * s9)) * RPC_9;
	neighborhood_avg = color_avg;
	neighborhood_min = min(s1, min(s2, min(s3, min(s4, min(s5, min(s6, min(s7, min(s8, s9))))))));
	neighborhood_max = max(s1, max(s2, max(s3, max(s4, max(s5, max(s6, max(s7, max(s8, s9))))))));
	// Use variance clipping as described in https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
	float box_size = mix(0.0f, params.variance_dynamic, smoothstep(0.02f, 0.0f, length(velocity_closest)));
	vec3 dev = sqrt(abs(color_avg2 - (color_avg * color_avg))) * box_size;
	vec3 color_min = color_avg - dev;
	vec3 color_max = color_avg + dev;

	// Variance clipping
	vec3 color = clip_aabb(color_min, color_max, clamp(color_avg, color_min, color_max), color_history);

	// Clamp to prevent NaNs
	color = clamp(color, FLT_MIN, FLT_MAX);

	return color;
}

/*------------------------------------------------------------------------------
									TAA
------------------------------------------------------------------------------*/

// This is "velocity disocclusion" as described by https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/.
// We use texel space, so our scale and threshold differ.
float get_factor_disocclusion(vec2 uv_reprojected, vec2 velocity) {
	ivec2 previous_pos = clamp(ivec2(uv_reprojected * params.resolution), ivec2(0), ivec2(params.resolution) - ivec2(1));
	vec2 velocity_previous = imageLoad(last_velocity_buffer, previous_pos).xy;
	vec2 velocity_texels = velocity * params.resolution;
	vec2 prev_velocity_texels = velocity_previous * params.resolution;
	float disocclusion = length(prev_velocity_texels - velocity_texels) - params.disocclusion_threshold;
	return clamp(disocclusion * DISOCCLUSION_SCALE, 0.0, 1.0);
}

bool rt_current_history_is_invalid(uvec2 pos_screen) {
	if (params.rt_history_validity_enabled < 0.5) {
		return false;
	}

	float current_valid = texelFetch(rt_history_validity_buffer, ivec2(pos_screen), 0).r;
	return current_valid < 0.5;
}

bool rt_history_id_matches(vec4 a, vec4 b) {
	return max(max(abs(a.x - b.x), abs(a.y - b.y)), max(abs(a.z - b.z), abs(a.w - b.w))) < (0.5 / 255.0);
}

bool rt_previous_history_tap_matches(ivec2 previous_pos, vec4 current_id) {
	if (any(lessThan(previous_pos, ivec2(0))) || any(greaterThanEqual(previous_pos, ivec2(params.resolution)))) {
		return false;
	}

	if (params.rt_history_validity_enabled > 0.5 && texelFetch(rt_prev_history_validity_buffer, previous_pos, 0).r < 0.5) {
		return false;
	}

	if (params.rt_history_id_enabled > 0.5) {
		vec4 prev_id = texelFetch(rt_prev_history_id_buffer, previous_pos, 0);
		if (!rt_history_id_matches(current_id, prev_id)) {
			return false;
		}
	}

	return true;
}

void rt_accumulate_history_tap(sampler2D tex_history, ivec2 previous_pos, vec4 current_id, float tap_weight, inout vec3 color_sum, inout float weight_sum) {
	if (tap_weight <= 0.0 || !rt_previous_history_tap_matches(previous_pos, current_id)) {
		return;
	}

	color_sum += sanitize_color(texelFetch(tex_history, previous_pos, 0).rgb) * tap_weight;
	weight_sum += tap_weight;
}

struct RtHistorySample {
	vec3 color;
	float confidence;
};

RtHistorySample sample_rt_history(sampler2D tex_history, uvec2 pos_screen, vec2 uv_reprojected, bool reprojected_in_screen) {
	RtHistorySample result;
	result.color = vec3(0.0);
	result.confidence = 0.0;

	if (!reprojected_in_screen || rt_current_history_is_invalid(pos_screen)) {
		return result;
	}

	vec4 current_id = params.rt_history_id_enabled > 0.5 ? texelFetch(rt_history_id_buffer, ivec2(pos_screen), 0) : vec4(0.0);
	ivec2 max_pos = ivec2(params.resolution) - ivec2(1);
	ivec2 nearest_pos = clamp(ivec2(uv_reprojected * params.resolution), ivec2(0), max_pos);
	vec3 nearest_color = sanitize_color(texelFetch(tex_history, nearest_pos, 0).rgb);
	bool nearest_matches = rt_previous_history_tap_matches(nearest_pos, current_id);

	if (params.rt_history_filter_strength <= 0.001) {
		result.color = nearest_color;
		result.confidence = nearest_matches ? 1.0 : 0.0;
		return result;
	}

	vec2 history_pos = uv_reprojected * params.resolution - vec2(0.5);
	ivec2 base_pos = ivec2(floor(history_pos));
	vec2 fraction = fract(history_pos);

	vec3 filtered_color_sum = vec3(0.0);
	float filtered_weight_sum = 0.0;
	rt_accumulate_history_tap(tex_history, base_pos + ivec2(0, 0), current_id, (1.0 - fraction.x) * (1.0 - fraction.y), filtered_color_sum, filtered_weight_sum);
	rt_accumulate_history_tap(tex_history, base_pos + ivec2(1, 0), current_id, fraction.x * (1.0 - fraction.y), filtered_color_sum, filtered_weight_sum);
	rt_accumulate_history_tap(tex_history, base_pos + ivec2(0, 1), current_id, (1.0 - fraction.x) * fraction.y, filtered_color_sum, filtered_weight_sum);
	rt_accumulate_history_tap(tex_history, base_pos + ivec2(1, 1), current_id, fraction.x * fraction.y, filtered_color_sum, filtered_weight_sum);

	if (filtered_weight_sum > 0.0) {
		vec3 filtered_color = filtered_color_sum / filtered_weight_sum;
		if (nearest_matches) {
			result.color = mix(nearest_color, filtered_color, params.rt_history_filter_strength);
			result.confidence = mix(1.0, filtered_weight_sum, params.rt_history_filter_strength);
		} else {
			result.color = filtered_color;
			result.confidence = filtered_weight_sum * params.rt_history_filter_strength;
		}
	} else {
		result.color = nearest_color;
		result.confidence = nearest_matches ? 1.0 - params.rt_history_filter_strength : 0.0;
	}

	return result;
}

vec3 temporal_antialiasing(ivec2 pos_group_top_left, uvec2 pos_group, uvec2 pos_screen, vec2 uv, sampler2D tex_history) {
	// Get the velocity of the current pixel
	vec2 velocity = imageLoad(velocity_buffer, ivec2(pos_screen)).xy;

	// Get reprojected uv
	vec2 uv_reprojected = uv + velocity;
	bool reprojected_in_screen = all(greaterThanEqual(uv_reprojected, vec2(0.0))) && all(lessThanEqual(uv_reprojected, vec2(1.0)));

	// Get input color
	vec3 color_input = load_color(ivec2(pos_group));
	if (params.raytracing_denoise > 0.5) {
		color_input = rt_firefly_clamp_3x3(ivec2(pos_group), color_input);
	}
	color_input = sanitize_color(color_input);

	// Get history color (catmull-rom reduces a lot of the blurring that you get under motion)
	vec3 color_history = color_input;
	vec2 velocity_closest = vec2(0.0); // This is best done by using the velocity with the closest depth.
	vec3 neighborhood_avg = color_input;
	vec3 neighborhood_min = color_input;
	vec3 neighborhood_max = color_input;
	float rt_history_confidence = 1.0;
	if (reprojected_in_screen) {
		if (params.rt_history_validity_enabled > 0.5 || params.rt_history_id_enabled > 0.5) {
			RtHistorySample rt_history = sample_rt_history(tex_history, pos_screen, uv_reprojected, reprojected_in_screen);
			color_history = sanitize_color(rt_history.color);
			rt_history_confidence = rt_history.confidence;
		} else {
			color_history = sanitize_color(sample_catmull_rom_9(tex_history, uv_reprojected, params.resolution).rgb);
		}

		// Clip history to the neighbourhood of the current sample (fixes a lot of the ghosting).
		get_closest_pixel_velocity_3x3(pos_group, pos_group_top_left, velocity_closest);
		color_history = sanitize_color(clip_history_3x3(pos_group, pos_screen, color_history, velocity_closest, neighborhood_avg, neighborhood_min, neighborhood_max));
	}

	// Compute blend factor
	float blend_factor = 1.0 - params.history_weight;
	bool force_current = false;
	{
		// If re-projected UV is out of screen, converge to current color immediately.
		float factor_screen = reprojected_in_screen ? 0.0 : 1.0;

		// Increase blend factor when there is disocclusion (fixes a lot of the remaining ghosting).
		float factor_disocclusion = reprojected_in_screen ? get_factor_disocclusion(uv_reprojected, velocity) : 0.0;

		float factor_history_invalid = clamp(1.0 - rt_history_confidence, 0.0, 1.0);
		force_current = factor_screen > 0.5 || rt_history_confidence <= 0.001;

		// Add to the blend factor
		blend_factor = clamp(blend_factor + factor_screen + factor_disocclusion + factor_history_invalid, 0.0, 1.0);
	}

	// Resolve
	vec3 color_resolved = vec3(0.0);
	{
		// Tonemap
		color_history = reinhard(color_history);
		color_input = reinhard(color_input);

		// Reduce flickering
		float lum_color = luminance(color_input);
		float lum_history = luminance(color_history);
		float diff = abs(lum_color - lum_history) / max(lum_color, max(lum_history, 1.001));
		diff = 1.0 - diff;
		diff = diff * diff;
		blend_factor = force_current ? 1.0 : mix(0.0, blend_factor, diff);
		if (params.rt_taa_reactivity_enabled > 0.5) {
			blend_factor = max(blend_factor, texelFetch(rt_taa_reactivity_buffer, ivec2(pos_screen), 0).r);
		}

		// Lerp/blend
		color_resolved = mix(color_history, color_input, blend_factor);

		// Inverse tonemap
		color_resolved = sanitize_color(reinhard_inverse(color_resolved));
	}

	if (params.sharpness > 0.0) {
		color_resolved = clamp(color_resolved + (color_resolved - neighborhood_avg) * params.sharpness, neighborhood_min, neighborhood_max);
	}

	return color_resolved;
}

void main() {
	populate_group_shared_memory(gl_WorkGroupID.xy, gl_LocalInvocationIndex);

	// Out of bounds check
	if (any(greaterThanEqual(vec2(gl_GlobalInvocationID.xy), params.resolution))) {
		return;
	}

	const uvec2 pos_group = gl_LocalInvocationID.xy;
	const ivec2 pos_group_top_left = ivec2(gl_WorkGroupID.xy) * kGroupSize - ivec2(kBorderSize);
	const uvec2 pos_screen = gl_GlobalInvocationID.xy;
	const vec2 uv = (gl_GlobalInvocationID.xy + 0.5f) / params.resolution;

	vec3 result = temporal_antialiasing(pos_group_top_left, pos_group, pos_screen, uv, history_buffer);
	imageStore(output_buffer, ivec2(gl_GlobalInvocationID.xy), vec4(result, 1.0));
}
