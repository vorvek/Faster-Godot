// Shared defines and common bindings for all RT shader stages.
// Include AFTER raytracing_inc.glsl and scene_data_inc.glsl.
// The includer must set exactly one of RT_STAGE_{RAYGEN,MISS,CLOSEST_HIT,ANY_HIT,INTERSECTION}.

// Specialization constant (bits 0-20: flags, 21-28: samples, 29-31: bounces).
layout(constant_id = 0) const uint RT_FLAGS = 0u;

#define RT_FLAG_FOG_ENABLED (1u << 2)
#define RT_FLAG_WRC_PROBE_UPDATE (1u << 7)
#define RT_FLAG_SPG_GATHER (1u << 8)
#define RT_FLAG_PRIMARY_DIRECT (1u << 9)
#define RT_FLAG_FOG_DEPTH_MODE (1u << 10)

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
	// 13 vec4s == RT_PARAM_SHADER_FLOAT_COUNT (52) floats. Grown from 12 to 13 to make
	// room for the World Radiance Cache (WRC) producer-owned params (indices 45..48); the
	// prior growth 10 -> 12 added the Screen Probe Gather (SPG) params (indices 39..44).
	// See the matching RT_PARAM_SHADER_FLOAT_COUNT in scene_shader_raytracing.h + the
	// rt_ubo static_assert in render_raytracing.cpp.
	vec4 rt_params[13];
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

bool rt_wrc_probe_update_mode() {
	return (RT_FLAGS & RT_FLAG_WRC_PROBE_UPDATE) != 0u;
}

bool rt_spg_gather_mode() {
	return (RT_FLAGS & RT_FLAG_SPG_GATHER) != 0u;
}

bool rt_probe_dispatch_mode() {
	// WRC-update and SPG-gather launches are 1D probe feeds; their hits must never
	// run the screen-pixel primary writers or the reservoir machinery, exactly like
	// the hybrid_primary local flag in shade_and_bounce excludes raster-owned camera
	// primaries.
	return rt_wrc_probe_update_mode() || rt_spg_gather_mode();
}

// The full-screen FPT primary-direct dispatch (FULL_PATH_TRACING only). Indirect
// bounces are capped to 0 so the camera ray does primary hit (NEE direct + emissive)
// + sky-on-miss only; the resolved probe indirect is added at the composite.
bool rt_primary_direct_mode() {
	return (RT_FLAGS & RT_FLAG_PRIMARY_DIRECT) != 0u;
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

// World Radiance Cache probe-update ray results. 48-byte 3xvec4 layout used by
// the WRC accumulate's texel-mapping convention: one ray == one (probe, direction)
// == one octahedral texel. metadata.w holds the WRC update_index (probe_linear << 6
// | dir_index for the default oct_res 8 => 64 dirs => 6 bits).
struct RTGIWRCProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
	vec4 metadata;
};

layout(set = 0, binding = 107, std430) buffer RTGIWRCProbeRayResultBuffer {
	RTGIWRCProbeRayResult rt_wrc_probe_ray_results[];
};

// Screen Probe Gather (SPG) gather ray results. One entry per selected
// (screen-probe, octahedral direction) this frame; the SPG accumulate folds these
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

// 113/114 = raw material-guide G-buffers (the SAME RB_TEX_RT_GUIDE_ALBEDO/ORM the FPT
// composite + gi_resolve consume), bound for the FPT-fast primary-direct path so it can
// build the NEE material from real reflectance instead of the hue-proxy albedo. Combined
// NEAREST samplers (full-res, 1:1 with the launch -> point texelFetch). guide_albedo: .rgb
// = linear albedo (.a unused). guide_orm: r=ao, g=roughness, b=metallic, a=sss (the packing
// the material-guide prepass writes and rtgi_gi_resolve reads). Both fall back to a default
// when not yet allocated; only the primary-direct dispatch ever samples them.
layout(set = 0, binding = 113) uniform sampler2D rt_guide_albedo_tex;
layout(set = 0, binding = 114) uniform sampler2D rt_guide_orm_tex;
layout(set = 0, binding = 115) uniform sampler2D rt_guide_emission_tex;

// 116 = relief-FREE GEOMETRIC normal (the SAME RB_TEX_RT_GUIDE_NORMAL the material-guide prepass
// writes as encode24(geo_normal) * 0.5 + 0.5, view space, and rtgi_gi_resolve decodes at its
// binding 17). The FPT-fast primary-direct path point-samples it (full-res, 1:1 with the launch ->
// NEAREST texelFetch) to recover the macro surface orientation beneath a normal map: a grazing-lit
// crevice's relief normal can tip below the light horizon (NdotL <= 0 -> the evaluator hard-zeros it
// to a black vein), but the geometric normal still faces the light, so the call site bends the relief
// shading normal toward it for a small geometry-bounded fill (see rt_primary_direct_mode()). ALWAYS
// bound (the shared set-0 layout must stay valid for every dispatch); a neutral default is bound when
// the guides are absent and only the primary-direct dispatch samples it, with an in-shader
// degenerate-decode guard that falls back to the relief world_N, so a bad bind degrades to the
// pre-fix behavior and never crashes.
layout(set = 0, binding = 116) uniform sampler2D rt_guide_normal_tex;

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

// Pure scheduler helper reused by the WRC/SPG round-robin + view-biased selectors
// (rt_wrc_select_update_index). Functions of their args only (no STRC param-slot reads).
uint rt_strc_probe_index(uint cascade, uvec3 probe_coord, uint grid_size) {
	uint grid = max(grid_size, 1u);
	return cascade * grid * grid * grid + probe_coord.x + probe_coord.y * grid + probe_coord.z * grid * grid;
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

// World Radiance Cache probe-update / SPG gather scheduler. Delegates to pure
// scheduler helpers (rt_strc_view_update_budget / rt_strc_select_view_biased_update_index /
// rt_strc_select_round_robin_update_index), which are functions of their args only (no STRC
// param-slot reads). This is the WRC/SPG scheduler entry point.
uint rt_wrc_select_update_index(uint ray_index, uint ray_count, uint grid, uint cascade_count, uint frame_index) {
	uint active_cascades = clamp(cascade_count, 1u, 4u);
	uint view_ray_count = rt_strc_view_update_budget(max(ray_count, 1u));
	if (ray_index < view_ray_count) {
		return rt_strc_select_view_biased_update_index(ray_index, grid, active_cascades, frame_index);
	}

	uint background_ray_count = max(ray_count - view_ray_count, 1u);
	uint background_ray_index = ray_index - view_ray_count;
	return rt_strc_select_round_robin_update_index(background_ray_index, background_ray_count, grid, active_cascades, frame_index);
}

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
	vec2 curr_texture_uv = rt_visible_to_texture_uv(curr_visible_uv, rt_current_origin());
	vec2 prev_texture_uv = rt_visible_to_texture_uv(prev_visible_uv, rt_previous_origin());
	imageStore(rt_velocity_image, pixel, vec4(prev_texture_uv - curr_texture_uv, 0.0, 0.0));
	rt_store_path_traced_visible_velocity(pixel, prev_visible_uv - curr_visible_uv);
}

void rt_store_invalid_primary_velocity(ivec2 pixel) {
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
