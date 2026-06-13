// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0.
// Note: ray_query_alpha_test() still requires GL_EXT_ray_query (used by DLSS-RR path).

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_AREA 2 // Rectangular area light (spherical-rectangle NEE)
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff
#define RT_LIGHT_FLAG_SHADOW 1u

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 128 bytes, std430)
// ============================================================================

struct RTLightData {
	vec3 position; // World pos (omni/spot) or direction (directional).
	uint type; // RT_LIGHT_TYPE_*.
	vec3 emission; // HDR emission color (color * energy).
	float radius; // Light size (omni/spot) or angular radius (directional).
	float attenuation; // Attenuation exponent (2=inverse-square, 1=linear).
	float inv_max_range; // 1/range (-1 = infinite).
	float max_range_squared; // range^2 (0 = infinite).
	float specular_amount; // Godot specular multiplier [0..1].
	float indirect_energy; // Godot indirect energy multiplier.
	float inv_spot_attenuation; // Spot cone softness.
	float cos_spot_angle; // Cosine of spot cone half-angle.
	uint flags; // RT_LIGHT_FLAG_*.
	vec3 spot_direction; // Spot direction (normalized, world space).
	uint cull_mask; // Receiver layer mask.
	uint shadow_caster_mask;
	float shadow_opacity;
	float shadow_max_distance;
	uint source_id; // Run-local stable diagnostic ID, derived from the light instance RID.
	vec4 area_atlas_rect; // xy = atlas offset, zw = atlas scale.
	uint area_atlas_idx; // bindless index, 0 = untextured.
	float area_max_mip;
	uint area_pad0;
	uint area_pad1;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

// For RT_LIGHT_TYPE_AREA the spot fields carry the rectangle half-edge vectors.
vec3 rt_light_area_ex(RTLightData l) { return l.spot_direction; }
vec3 rt_light_area_ey(RTLightData l) { return vec3(l.radius, l.inv_spot_attenuation, l.cos_spot_angle); }
vec3 rt_light_area_normal(RTLightData l) { return normalize(cross(rt_light_area_ex(l), rt_light_area_ey(l))); }

// Directional-light accessors for fog_process (raytracing_fog_inc.glsl). Lifted here from
// the closest-hit include so every stage that iterates rt_lights (closest hit AND the
// FPT primary-direct raygen) shares one definition. The direction is returned in VIEW
// space because fog_process compares it against the view vector normalize(vertex).
vec3 fog_get_directional_color(uint index) {
	return rt_lights[index].emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_lights[index].position);
	mat3 view_rot = transpose(mat3(
			scene_data_block.data.view_matrix[0].xyz,
			scene_data_block.data.view_matrix[1].xyz,
			scene_data_block.data.view_matrix[2].xyz));
	return view_rot * world_dir;
}

#include "area_light_sample_inc.glsl"

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	if (dot(dir, dir) < 1e-8) {
		dir = vec3(0.0, 0.0, 1.0);
	}
	dir = normalize(dir);
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;
	vec3 fallback_axis = (light.type == RT_LIGHT_TYPE_SPOT && dot(light.spot_direction, light.spot_direction) > 1e-8) ? -normalize(light.spot_direction) : vec3(0.0, 0.0, 1.0);

	// Cone axis.
	vec3 sphere_axis = (dist_sq > 1e-8) ? (to_light * inv_dist) : fallback_axis;
	vec3 dir_axis = -normalize(light.position);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = (light.shadow_max_distance > 0.0) ? light.shadow_max_distance : 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

// ============================================================================
// Attenuation
// ============================================================================

// Godot-style windowed distance attenuation.
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay).
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = min(pow(max(distance, 0.0001), -decay), 1.0);

	// Windowed falloff if range is finite (inv_max_range >= 0).
	if (inv_max_range >= 0.0) {
		float nd = distance * inv_max_range;
		nd *= nd;
		nd *= nd; // nd^4
		nd = max(1.0 - nd, 0.0);
		nd *= nd; // nd^2 window
		return atten * nd;
	}

	return atten;
}

// Per-light specular multiplier.
float lights_get_specular_multiplier(float specular_amount, float roughness) {
	if (specular_amount >= 0.0) {
		return specular_amount;
	} else {
		float r3 = roughness * roughness * roughness;
		return mix(0.0, r3, -specular_amount);
	}
}

// ============================================================================
// Inline Alpha Test (shared by all ray query proceed loops)
// ============================================================================

#ifndef RT_MATERIAL_TEXTURE_SAMPLING_DEFINED
#define RT_MATERIAL_TEXTURE_SAMPLING_DEFINED
vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	return textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, 0.0);
}

vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	bool point_filter = (mat_flags & RT_MAT_FLAG_POINT_FILTER) != 0u;
	bool repeat_disabled = (mat_flags & RT_MAT_FLAG_REPEAT_DISABLED) != 0u;
	if (point_filter) {
		return repeat_disabled ?
				textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_CLAMP), uv, 0.0) :
				textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv, 0.0);
	}
	return repeat_disabled ?
			textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, 0.0) :
			textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, 0.0);
}
#endif

/// Inline alpha test for ray query candidates. Returns true if the hit is opaque.
/// Mirrors the any-hit shader logic for use with inline ray queries.
bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary, vec3 candidate_object_pos) {
	MaterialData mat = materials[geometry_idx];
	if ((mat.flags & RT_MAT_FLAG_ALPHA_TEST) == 0u) {
		return true;
	}
	if ((mat.flags & RT_MAT_FLAG_CUSTOM_SHADER) != 0u) {
		// Inline ray queries cannot run custom any-hit code. Treat custom alpha
		// candidates as opaque so debug/specular queries do not tunnel through
		// valid custom RT surfaces.
		return true;
	}

	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	uv = uv * mat.uv1_scale + mat.uv1_offset;
	float alpha = sample_material_texture(mat.albedo_texture_idx, uv, mat.flags).a;
	alpha *= rt_material_vertex_color(mat, fetch_color(geom, i0, i1, i2, bary)).a;
	alpha *= mat.albedo_color.a;
	if ((mat.flags & RT_MAT_FLAG_ALPHA_HASH) != 0u) {
		mat4 rt_aabb_xform;
		mat4 rt_inv_aabb_xform;
		get_aabb_compression_xforms(geom, rt_aabb_xform, rt_inv_aabb_xform);
		vec3 rt_hash_object_pos = (rt_aabb_xform * vec4(candidate_object_pos, 1.0)).xyz;
		return alpha >= rt_alpha_hash_threshold(rt_hash_object_pos, mat.alpha_hash_scale);
	}

	return alpha >= mat.alpha_scissor_threshold;
}

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

/// Returns true if light is visible.
/// Uses SkipClosestHitShader so only any_hit (alpha test) and miss are invoked.
/// TerminateOnFirstHit causes early exit on first confirmed opaque hit.
bool lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, uint shadow_caster_mask, inout uint rng_state) {
	if (max_dist <= 0.001) {
		return true;
	}

#ifdef USE_SER
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.specular_radiance = vec3(0.0);
	shadow_ps.throughput = vec3(0.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = shadow_caster_mask;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	shadow_ps.pdf_bsdf = 0.0;
	path_pack(payload, shadow_ps);

	hitObjectNV hitObject;
	hitObjectTraceRayNV(hitObject, tlas,
			RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_INSTANCE_MASK_SHADOW, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	bool visible = !(hitObjectIsHitNV(hitObject));
	payload = saved_payload;
	return visible;
#else
	/// The miss shader writes radiance = vec3(1.0) for shadow rays (visible).
	/// If an opaque hit occurs, miss is never called and radiance stays vec3(0.0).
	// Save full payload, set up shadow ray, then restore after trace.
	// saved_payload is opaque caller state: restored verbatim after the trace, never unpacked here; the only unpack below reads the freshly packed shadow payload.
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.specular_radiance = vec3(0.0);
	shadow_ps.throughput = vec3(0.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = shadow_caster_mask;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	shadow_ps.pdf_bsdf = 0.0;
	path_pack(payload, shadow_ps);

	traceRayEXT(tlas,
			RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_INSTANCE_MASK_SHADOW, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	// Unpack to check visibility (miss shader packs radiance = 1.0).
	shadow_ps = path_unpack(payload);
	bool visible = shadow_ps.radiance.x > 0.5;

	payload = saved_payload;

	return visible;
#endif
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

float lights_selection_weight(vec3 hit_pos, vec3 N, RTLightData light, bool is_indirect_bounce) {
	float energy = max(luminance(max(light.emission, vec3(0.0))), 0.0);
	energy *= is_indirect_bounce ? max(light.indirect_energy, 0.0) : 1.0;
	if (energy <= 0.0) {
		return 0.0;
	}

	if (light.type == RT_LIGHT_TYPE_OMNI || light.type == RT_LIGHT_TYPE_SPOT) {
		vec3 to_light = light.position - hit_pos;
		float dist_sq = dot(to_light, to_light);
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return 0.0;
		}

		float dist = sqrt(max(dist_sq, 1e-8));
		vec3 L = to_light / dist;
		float spot_atten = 1.0;
		if (light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, light.spot_direction);
			if (scos <= light.cos_spot_angle) {
				return 0.0;
			}
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
			spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}

		LightSample ls;
		ls.distance_sq = dist_sq;
		float atten = lights_get_attenuation(ls, light.inv_max_range, light.attenuation) * spot_atten;
		float n_dot_l = max(dot(N, L), 0.02);
		return max(energy * atten * n_dot_l, 1e-6);
	}

	if (light.type == RT_LIGHT_TYPE_AREA) {
		vec3 center = light.position;
		vec3 to_light = center - hit_pos;
		float dist_sq = dot(to_light, to_light);
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return 0.0;
		}
		vec3 nrm = rt_light_area_normal(light);
		// One-sided: shading point must be on the front side.
		if (dot(to_light, nrm) < 0.0) {
			return 0.0; // receiver behind the emitter (light emits toward -nrm)
		}
		float dist = sqrt(max(dist_sq, 1e-8));
		vec3 L = to_light / dist;
		float n_dot_l = max(dot(N, L), 0.02);
		// Approximate area solid angle ~ (ex x ey area) * cos / dist^2.
		float area = 4.0 * length(cross(rt_light_area_ex(light), rt_light_area_ey(light)));
		float approx_solid = area * max(-dot(L, nrm), 0.0) / max(dist_sq, 1e-4);
		return max(energy * approx_solid * n_dot_l, 1e-6);
	}

	vec3 L = -normalize(light.position);
	float n_dot_l = max(dot(N, L), 0.02);
	return max(energy * n_dot_l, 1e-6);
}

const uint RTGI_DETERMINISTIC_DIRECT_LIGHT_LIMIT = 12u;

struct RTDirectLighting {
	vec3 diffuse;
	vec3 specular;
};

RTDirectLighting rt_direct_lighting_zero() {
	RTDirectLighting result;
	result.diffuse = vec3(0.0);
	result.specular = vec3(0.0);
	return result;
}

vec3 rt_direct_lighting_sum(RTDirectLighting lighting) {
	return lighting.diffuse + lighting.specular;
}

struct RTDirectLightReservoir {
	uint selected_key;
	RTDirectLighting selected_lighting;
	float selected_pdf;
	float selected_target;
	float weight_sum;
	float M;
	float confidence;
	bool valid;
	bool temporal_accepted;
	bool spatial_accepted;
	uint temporal_reject;
	uint spatial_reject;
	uint visibility_failures;
};

RTDirectLightReservoir rt_direct_light_reservoir_empty() {
	RTDirectLightReservoir reservoir;
	reservoir.selected_key = 0u;
	reservoir.selected_lighting = rt_direct_lighting_zero();
	reservoir.selected_pdf = 0.0;
	reservoir.selected_target = 0.0;
	reservoir.weight_sum = 0.0;
	reservoir.M = 0.0;
	reservoir.confidence = 0.0;
	reservoir.valid = false;
	reservoir.temporal_accepted = false;
	reservoir.spatial_accepted = false;
	reservoir.temporal_reject = RT_SOURCE_REJECT_PREV_UV;
	reservoir.spatial_reject = RT_SOURCE_REJECT_PREV_UV;
	reservoir.visibility_failures = 0u;
	return reservoir;
}

float rt_direct_reservoir_target(RTDirectLighting lighting) {
	vec3 contribution = sanitize_payload_vec3(rt_direct_lighting_sum(lighting));
	float target = rt_luminance(contribution);
	return (isnan(target) || isinf(target)) ? 0.0 : max(target, 0.0);
}

void rt_direct_reservoir_update(
		inout RTDirectLightReservoir reservoir,
		uint source_key,
		RTDirectLighting lighting,
		float source_pdf,
		float selected_target,
		float candidate_weight,
		float sample_count,
		float confidence,
		inout uint rng_state) {
	reservoir.M += max(sample_count, 0.0);
	if (source_key == 0u || selected_target <= 0.0 || candidate_weight <= 0.0 || source_pdf <= 0.0 ||
			isnan(candidate_weight) || isinf(candidate_weight) || isnan(selected_target) || isinf(selected_target)) {
		return;
	}

	float next_weight_sum = reservoir.weight_sum + candidate_weight;
	if (next_weight_sum <= 0.0 || isnan(next_weight_sum) || isinf(next_weight_sum)) {
		return;
	}

	if (!reservoir.valid || rand(rng_state) < candidate_weight / next_weight_sum) {
		reservoir.selected_key = source_key;
		reservoir.selected_lighting = lighting;
		reservoir.selected_pdf = source_pdf;
		reservoir.selected_target = selected_target;
		reservoir.valid = true;
	}

	reservoir.weight_sum = min(next_weight_sum, 65504.0);
	reservoir.confidence = max(reservoir.confidence, confidence);
}

RTDirectLighting rt_direct_reservoir_resolve(RTDirectLightReservoir reservoir) {
	if (!reservoir.valid || reservoir.M <= 0.0 || reservoir.weight_sum <= 0.0 || reservoir.selected_target <= 0.0) {
		return rt_direct_lighting_zero();
	}

	float reservoir_weight = reservoir.weight_sum / max(reservoir.M * reservoir.selected_target, 1e-6);
	reservoir_weight = min(max(reservoir_weight, 0.0), 65504.0);
	RTDirectLighting resolved = reservoir.selected_lighting;
	resolved.diffuse *= reservoir_weight;
	resolved.specular *= reservoir_weight;
	return resolved;
}

bool lights_direct_source_weight(
		uint light_index,
		uint light_count,
		uint receiver_layer_mask,
		bool is_indirect_bounce,
		vec3 hit_pos,
		vec3 N,
		out float light_weight) {
	light_weight = 0.0;
	if (light_index >= light_count) {
		return false;
	}

	RTLightData test_light = rt_lights[light_index];
	// Directionals are evaluated deterministically OUTSIDE the reservoir machinery (one
	// shadow ray each, every frame, in both regimes). They never enter the regime predicate,
	// so they cannot drive valid_count / total_weight, never enter the stratified CDF, and are
	// never recorded into a reservoir. This kills the occluded-sun candidate monopoly (a bright
	// occluded sun has no visibility term in its selection weight, so it would otherwise win
	// every RIS CDF draw and starve the positional lights that actually reach the pixel).
	// Skipping here keeps the count pass, the CDF candidate walk, and lights_find_direct_source_by_id
	// on one shared positional-only valid-set definition (the stratified draw depends on it).
	if (test_light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		return false;
	}
	bool is_valid = (test_light.cull_mask & receiver_layer_mask) != 0u;
	bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
	if (is_valid && is_positional) {
		vec3 to_l = test_light.position - hit_pos;
		float d2 = dot(to_l, to_l);
		is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
	}
	if (!is_valid) {
		return false;
	}

	light_weight = lights_selection_weight(hit_pos, N, test_light, is_indirect_bounce);
	return light_weight > 0.0;
}

bool lights_find_direct_source_by_id(
		uint source_id,
		uint light_count,
		uint receiver_layer_mask,
		bool is_indirect_bounce,
		vec3 hit_pos,
		vec3 N,
		out uint light_index,
		out float selected_weight,
		out float total_weight) {
	light_index = 0u;
	selected_weight = 0.0;
	total_weight = 0.0;
	if (source_id == 0u) {
		return false;
	}

	for (uint idx = 0u; idx < light_count; idx++) {
		float light_weight = 0.0;
		// lights_direct_source_weight returns false for directionals, so a directional source_id
		// can never be re-matched here. A prev reservoir that recorded a directional before this
		// change (or on the upgrade frame) therefore fails to pair and dies in one frame, which
		// is the intended one-frame history discontinuity at the upgrade. Directionals are not
		// reservoir-eligible.
		if (!lights_direct_source_weight(idx, light_count, receiver_layer_mask, is_indirect_bounce, hit_pos, N, light_weight)) {
			continue;
		}

		total_weight += light_weight;
		if (rt_lights[idx].source_id == source_id) {
			light_index = idx;
			selected_weight = light_weight;
		}
	}

	return selected_weight > 0.0 && total_weight > 0.0;
}

vec3 rt_emissive_candidate_transform_point(RTEmissiveCandidate candidate, vec3 p) {
	return vec3(
			dot(vec4(p, 1.0), vec4(candidate.object_to_world[0], candidate.object_to_world[1], candidate.object_to_world[2], candidate.object_to_world[3])),
			dot(vec4(p, 1.0), vec4(candidate.object_to_world[4], candidate.object_to_world[5], candidate.object_to_world[6], candidate.object_to_world[7])),
			dot(vec4(p, 1.0), vec4(candidate.object_to_world[8], candidate.object_to_world[9], candidate.object_to_world[10], candidate.object_to_world[11])));
}

vec3 rt_emissive_candidate_transform_vector(RTEmissiveCandidate candidate, vec3 v) {
	return vec3(
			dot(v, vec3(candidate.object_to_world[0], candidate.object_to_world[1], candidate.object_to_world[2])),
			dot(v, vec3(candidate.object_to_world[4], candidate.object_to_world[5], candidate.object_to_world[6])),
			dot(v, vec3(candidate.object_to_world[8], candidate.object_to_world[9], candidate.object_to_world[10])));
}

vec3 rt_emissive_sample_triangle_bary(inout uint rng_state) {
	float su = sqrt(rand(rng_state));
	float bv = rand(rng_state);
	return vec3(1.0 - su, su * (1.0 - bv), su * bv);
}

vec3 rt_emissive_candidate_evaluate_emission(MaterialData light_mat, GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	vec3 emission = max(light_mat.emission_color * light_mat.emission_strength, vec3(0.0));
	bool has_emission_tex = (light_mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0u;
	bool alpha_test = (light_mat.flags & RT_MAT_FLAG_ALPHA_TEST) != 0u;
	if (has_emission_tex || alpha_test) {
		vec2 uv = fetch_uv(geom, i0, i1, i2, bary);
		uv = uv * light_mat.uv1_scale + light_mat.uv1_offset;
		if (alpha_test) {
			// Alpha-scissor emitters: a cut-out texel emits nothing, exactly like
			// the BSDF/any-hit view of the surface (the any-hit ignores it, so a
			// BSDF path never collects its Le). Mirrors ray_query_alpha_test's
			// opaque alpha; hash/custom-clip never get here (candidate builder
			// rejects them). Mip 0, same as the any-hit cutout (idx 0 = the
			// default white texture, alpha 1).
			float alpha = light_mat.albedo_color.a;
			alpha *= rt_material_vertex_color(light_mat, fetch_color(geom, i0, i1, i2, bary)).a;
			if (light_mat.albedo_texture_idx != 0u) {
				alpha *= sample_material_texture(light_mat.albedo_texture_idx, uv, light_mat.flags).a;
			}
			if (alpha < light_mat.alpha_scissor_threshold) {
				return vec3(0.0); // cut-out texel: emits nothing
			}
		}
		if (has_emission_tex) {
			emission *= sample_material_texture(light_mat.emission_texture_idx, uv, light_mat.flags).rgb;
		}
	}
	return emission * scene_data_block.data.emissive_exposure_normalization;
}

bool rt_emissive_candidate_select_primitive(
		RTEmissiveCandidate candidate,
		GeometryData geom,
		inout uint rng_state,
		out uint primitive_id,
		out float primitive_pdf,
		out float primitive_debug) {
	primitive_id = 0u;
	primitive_pdf = 0.0;
	primitive_debug = 0.0;

	if ((candidate.flags & RT_EMISSIVE_CANDIDATE_FLAG_PRIMITIVE_DISTRIBUTION) != 0u && candidate.primitive_count > 0u && candidate.primitive_weight_sum > 1e-8) {
		float target = rand(rng_state) * candidate.primitive_weight_sum;
		uint lo = 0u;
		uint hi = candidate.primitive_count;
		for (uint step = 0u; step < 16u; step++) {
			if (lo >= hi) {
				break;
			}
			uint mid = (lo + hi) >> 1u;
			float cdf = rt_emissive_primitive_distributions[candidate.primitive_offset + mid].cumulative_weight;
			if (cdf < target) {
				lo = mid + 1u;
			} else {
				hi = mid;
			}
		}
		uint local_index = min(lo, candidate.primitive_count - 1u);
		RTEmissivePrimitiveDistribution selected_distribution = rt_emissive_primitive_distributions[candidate.primitive_offset + local_index];
		float previous_cdf = local_index == 0u ? 0.0 : rt_emissive_primitive_distributions[candidate.primitive_offset + local_index - 1u].cumulative_weight;
		float primitive_weight = max(selected_distribution.cumulative_weight - previous_cdf, 0.0);
		if (primitive_weight <= 1e-8 || selected_distribution.primitive_id >= geom.primitive_count) {
			return false;
		}
		primitive_id = selected_distribution.primitive_id;
		primitive_pdf = primitive_weight / max(candidate.primitive_weight_sum, 1e-8);
		primitive_debug = candidate.primitive_count > 1u ? float(local_index) / float(candidate.primitive_count - 1u) : 0.0;
		return primitive_pdf > 1e-8;
	}

	primitive_id = min(uint(rand(rng_state) * float(geom.primitive_count)), geom.primitive_count - 1u);
	primitive_pdf = 1.0 / max(float(geom.primitive_count), 1.0);
	primitive_debug = geom.primitive_count > 1u ? float(primitive_id) / float(geom.primitive_count - 1u) : 0.0;
	return true;
}

bool rt_emissive_candidate_sample_bary_and_emission(
		MaterialData light_mat,
		GeometryData geom,
		uint i0,
		uint i1,
		uint i2,
		inout uint rng_state,
		out vec3 bary,
		out vec3 emission,
		out float texel_pdf_factor,
		out float texel_debug) {
	// Alpha-test emitters vary across the triangle through their cutout even with
	// constant emission, so they get the texel reservoir too.
	uint candidate_count = (light_mat.flags & (RT_MAT_FLAG_HAS_EMISSION_TEX | RT_MAT_FLAG_ALPHA_TEST)) != 0u ? 4u : 1u;
	float reservoir_weight_sum = 0.0;
	float selected_weight = 0.0;
	bary = vec3(1.0, 0.0, 0.0);
	emission = vec3(0.0);
	texel_pdf_factor = 1.0;
	texel_debug = 0.0;

	for (uint i = 0u; i < 4u; i++) {
		if (i >= candidate_count) {
			break;
		}
		vec3 test_bary = rt_emissive_sample_triangle_bary(rng_state);
		vec3 test_emission = rt_emissive_candidate_evaluate_emission(light_mat, geom, i0, i1, i2, test_bary);
		float test_weight = max(luminance(test_emission), 0.0);
		reservoir_weight_sum += test_weight;
		if (test_weight > 0.0 && rand(rng_state) * reservoir_weight_sum <= test_weight) {
			bary = test_bary;
			emission = test_emission;
			selected_weight = test_weight;
		}
	}

	if (reservoir_weight_sum <= 1e-6 || selected_weight <= 1e-6) {
		return false;
	}

	if (candidate_count > 1u) {
		float average_weight = reservoir_weight_sum / float(candidate_count);
		texel_pdf_factor = selected_weight / max(average_weight, 1e-6);
		texel_debug = clamp(texel_pdf_factor / float(candidate_count), 0.0, 1.0);
	}
	return true;
}

// ============================================================================
// Emissive MIS (power heuristic) — shared BSDF/NEE solid-angle pdf helpers
// ============================================================================

// Full BSDF-sampling solid-angle pdf for direction `dir`, as the lobe mixture
// used by shade_and_bounce's importance sampler:
//   P(diffuse) = 1 - p_spec  -> cosine-hemisphere pdf
//   P(specular) = p_spec     -> GGX VNDF reflection pdf (brdf_inc.glsl)
// `p_spec` must equal the lobe-selection probability actually used at that
// vertex (brdfProbability, or 1/0 for the single-lobe degenerate cases).
float rt_bsdf_sampling_pdf(vec3 dir, vec3 N, vec3 V, MaterialProperties mat, float p_spec) {
	float ndl = max(dot(N, dir), 0.0);
	if (ndl <= 0.0) {
		return 0.0;
	}
	float pdf_diffuse = ndl * ONE_OVER_PI; // cosine-hemisphere

	vec3 H = normalize(dir + V);
	float NdotH = max(dot(N, H), 0.0);
	float NdotV = max(dot(N, V), 0.00001);
	float LdotH = max(dot(dir, H), 0.0);
	float alpha = mat.roughness * mat.roughness;
	float alphaSquared = alpha * alpha;
	float pdf_specular = sampleGGXVNDFReflectionPdf(alpha, alphaSquared, NdotH, NdotV, LdotH);

	return clamp(1.0 - p_spec, 0.0, 1.0) * pdf_diffuse + clamp(p_spec, 0.0, 1.0) * pdf_specular;
}

// Lobe-selection probability P(specular) used by shade_and_bounce (line ~1019),
// including the single-lobe degenerate cases. Mirrors that logic so NEE-side MIS
// (which runs before the lobe is chosen) and BSDF-side MIS agree.
float rt_lobe_specular_probability(vec3 specularF0, vec3 diffuseReflectance) {
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);
	if (diffuseLum < 0.0001) {
		return 1.0; // specular-only
	}
	if (specularLum < 0.0001) {
		return 0.0; // diffuse-only
	}
	return clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99);
}

// Reverse lookup: the emissive-NEE SOLID-ANGLE pdf that explicit-emissive NEE
// would have produced for a BSDF-sampled hit on `geometry_idx`/`primitive_id`.
// Returns 0.0 when the hit geometry is not an emissive candidate (then the BSDF
// MIS weight collapses to 1.0). Texel pdf factor is approximated as 1.0 (exact
// for untextured opaque emitters incl. the furnace; textured and alpha-cutout
// emitters are approximated, matching the small bias already present in the
// NEE-side texel reservoir).
float rt_emissive_nee_solid_angle_pdf_at_hit(
		uint geometry_idx,
		uint primitive_id,
		vec3 hit_geom_normal,
		vec3 ray_dir,
		float hit_distance) {
	uint candidate_count = min(uint(get_rt_param(RT_PARAM_EMISSIVE_CANDIDATE_COUNT)), 512u);
	float total_weight = get_rt_param(RT_PARAM_EMISSIVE_CANDIDATE_TOTAL_WEIGHT);
	if (candidate_count == 0u || total_weight <= 0.0) {
		return 0.0;
	}

	// Find the candidate that covers this geometry (selection-distribution entry).
	int found = -1;
	for (uint i = 0u; i < 512u; i++) {
		if (i >= candidate_count) {
			break;
		}
		if (rt_emissive_candidates[i].geometry_index == geometry_idx) {
			found = int(i);
			break;
		}
	}
	if (found < 0) {
		return 0.0;
	}

	RTEmissiveCandidate candidate = rt_emissive_candidates[found];
	float candidate_weight = max(candidate.selection_weight, 0.0);
	float select_pdf = candidate_weight / total_weight;
	if (select_pdf <= 1e-8) {
		return 0.0;
	}

	GeometryData geom = geometries[geometry_idx];
	if (geom.primitive_count == 0u || geom.vertex_address == 0ul || primitive_id >= geom.primitive_count) {
		return 0.0;
	}

	// Per-primitive pdf: mirror rt_emissive_candidate_select_primitive for the
	// specific hit primitive when a primitive distribution is present, else the
	// uniform 1/primitive_count fallback.
	float primitive_pdf;
	if ((candidate.flags & RT_EMISSIVE_CANDIDATE_FLAG_PRIMITIVE_DISTRIBUTION) != 0u && candidate.primitive_count > 0u && candidate.primitive_weight_sum > 1e-8) {
		float prim_weight = 0.0;
		// Linear scan of the candidate's CDF slice to recover the hit primitive's
		// individual weight (cumulative[k] - cumulative[k-1]).
		float prev_cdf = 0.0;
		bool prim_found = false;
		for (uint k = 0u; k < candidate.primitive_count; k++) {
			RTEmissivePrimitiveDistribution dist = rt_emissive_primitive_distributions[candidate.primitive_offset + k];
			float w = max(dist.cumulative_weight - prev_cdf, 0.0);
			if (dist.primitive_id == primitive_id) {
				prim_weight = w;
				prim_found = true;
				break;
			}
			prev_cdf = dist.cumulative_weight;
		}
		if (!prim_found || prim_weight <= 1e-8) {
			return 0.0; // primitive not selectable under the distribution
		}
		primitive_pdf = prim_weight / max(candidate.primitive_weight_sum, 1e-8);
	} else {
		primitive_pdf = 1.0 / max(float(geom.primitive_count), 1.0);
	}
	if (primitive_pdf <= 1e-8) {
		return 0.0;
	}

	float texel_pdf_factor = 1.0; // see function comment (untextured-exact approximation)

	// Triangle area in the candidate's world space (same math as NEE, lights:692-699).
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec3 p0 = fetch_position(geom, i0);
	vec3 p1 = fetch_position(geom, i1);
	vec3 p2 = fetch_position(geom, i2);
	vec3 wp0 = rt_emissive_candidate_transform_point(candidate, p0);
	vec3 wp1 = rt_emissive_candidate_transform_point(candidate, p1);
	vec3 wp2 = rt_emissive_candidate_transform_point(candidate, p2);
	float tri_area = length(cross(wp1 - wp0, wp2 - wp0)) * 0.5;
	if (tri_area <= 1e-8) {
		return 0.0;
	}

	// abs() matches the two-sided NEE flip: emitter cosine is taken on the
	// face that faces the receiver, so direction sign is irrelevant here.
	float light_cos = max(abs(dot(normalize(hit_geom_normal), ray_dir)), 1e-4);
	float dist_sq = hit_distance * hit_distance;

	float area_pdf = select_pdf * primitive_pdf * texel_pdf_factor / max(tri_area, 1e-8);
	return area_pdf * dist_sq / light_cos; // area -> solid-angle measure
}

RTDirectLighting lights_evaluate_explicit_emissive_candidate_split(
		vec3 hit_pos,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		uint receiver_layer_mask,
		out float out_pdf,
		out float out_selected_weight,
		out uint out_source_key,
		out float out_distribution_debug,
		out float out_pdf_solid_angle,
		out vec3 out_L) {
	out_pdf = 0.0;
	out_selected_weight = 0.0;
	out_source_key = 0u;
	out_distribution_debug = 0.0;
	out_pdf_solid_angle = 0.0;
	out_L = vec3(0.0);
	if ((uint(get_rt_param(RT_PARAM_RTGI_SAMPLING_CONTROLS)) & RTGI_SAMPLING_EXPLICIT_EMISSIVE_BIT) == 0u) {
		return rt_direct_lighting_zero();
	}

	uint candidate_count = min(uint(get_rt_param(RT_PARAM_EMISSIVE_CANDIDATE_COUNT)), 512u);
	float total_weight = get_rt_param(RT_PARAM_EMISSIVE_CANDIDATE_TOTAL_WEIGHT);
	if (candidate_count == 0u || total_weight <= 1e-6) {
		return rt_direct_lighting_zero();
	}

	float roulette = rand(rng_state) * total_weight;
	uint selected = candidate_count - 1u;
	float cumulative = 0.0;
	for (uint i = 0u; i < 512u; i++) {
		if (i >= candidate_count) {
			break;
		}
		float weight = max(rt_emissive_candidates[i].selection_weight, 0.0);
		cumulative += weight;
		if (roulette <= cumulative) {
			selected = i;
			break;
		}
	}

	RTEmissiveCandidate candidate = rt_emissive_candidates[selected];
	out_selected_weight = max(candidate.selection_weight, 0.0);
	float select_pdf = out_selected_weight / total_weight;
	if (select_pdf <= 1e-8) {
		return rt_direct_lighting_zero();
	}

	GeometryData geom = geometries[candidate.geometry_index];
	if ((geom.layer_mask & receiver_layer_mask) == 0u || geom.primitive_count == 0u || geom.vertex_address == 0ul) {
		return rt_direct_lighting_zero();
	}

	MaterialData light_mat = materials[candidate.geometry_index];
	uint primitive_id;
	float primitive_pdf;
	float primitive_debug;
	if (!rt_emissive_candidate_select_primitive(candidate, geom, rng_state, primitive_id, primitive_pdf, primitive_debug)) {
		return rt_direct_lighting_zero();
	}

	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec3 p0 = fetch_position(geom, i0);
	vec3 p1 = fetch_position(geom, i1);
	vec3 p2 = fetch_position(geom, i2);
	vec3 wp0 = rt_emissive_candidate_transform_point(candidate, p0);
	vec3 wp1 = rt_emissive_candidate_transform_point(candidate, p1);
	vec3 wp2 = rt_emissive_candidate_transform_point(candidate, p2);
	vec3 area_vec = cross(wp1 - wp0, wp2 - wp0);
	float tri_area = length(area_vec) * 0.5;
	if (tri_area <= 1e-8) {
		return rt_direct_lighting_zero();
	}

	vec3 bary;
	vec3 emission;
	float texel_pdf_factor;
	float texel_debug;
	if (!rt_emissive_candidate_sample_bary_and_emission(light_mat, geom, i0, i1, i2, rng_state, bary, emission, texel_pdf_factor, texel_debug)) {
		return rt_direct_lighting_zero();
	}
	vec3 light_pos = bary.x * wp0 + bary.y * wp1 + bary.z * wp2;
	out_distribution_debug = max(primitive_debug, texel_debug);

	vec3 to_light = light_pos - hit_pos;
	float dist_sq = dot(to_light, to_light);
	if (dist_sq <= 1e-8) {
		return rt_direct_lighting_zero();
	}
	float dist = sqrt(dist_sq);
	vec3 L = to_light / dist;
	float NdotL = dot(N, L);
	if (NdotL <= 0.0) {
		return rt_direct_lighting_zero();
	}

	vec3 light_normal = normalize(area_vec);
	// Two-sided emitters (material cull_mode == disabled) emit from both faces, so
	// flip the winding normal to whichever side faces the receiver. One-sided
	// emitters keep the back-face rejection (no light leak from their dark side).
	bool emitter_two_sided = (geom.flags & RT_GEOM_FLAG_TWO_SIDED) != 0u;
	if (emitter_two_sided && dot(light_normal, -L) < 0.0) {
		light_normal = -light_normal;
	}
	float light_cos = dot(light_normal, -L);
	if (light_cos <= 0.0) {
		return rt_direct_lighting_zero();
	}

	vec3 shadow_origin = offset_ray_origin(hit_pos, dot(geometry_normal, L) >= 0.0 ? geometry_normal : -geometry_normal);
	if (!lights_trace_shadow_ray(shadow_origin, L, max(dist - 0.002, 0.001), 0xFFFFFFFFu, rng_state)) {
		return rt_direct_lighting_zero();
	}

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);
	float pdf_area = select_pdf * primitive_pdf * max(texel_pdf_factor, 1e-4) / max(tri_area, 1e-8);
	out_pdf = pdf_area;
	float geom_term = light_cos / max(dist_sq, 1e-6);
	float inv_pdf = 1.0 / max(pdf_area, 1e-8);
	// Solid-angle measure pdf and sampled direction for emissive MIS (power
	// heuristic against rt_bsdf_sampling_pdf(L) at this vertex).
	out_pdf_solid_angle = pdf_area / max(geom_term, 1e-12);
	out_L = L;

	RTDirectLighting result;
	result.diffuse = brdf_diffuse * emission * geom_term * inv_pdf;
	result.specular = brdf_specular * emission * geom_term * inv_pdf;
	out_source_key = rt_source_make_key(RT_SOURCE_CLASS_EMISSIVE, geom.history_id);
	return result;
}

RTDirectLighting lights_evaluate_single_direct_light_split(
		RTLightData light,
		float light_select_pdf,
		vec3 hit_pos,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		bool p_use_blue_noise_u,
		vec2 p_blue_noise_u) {
	// The 2D sample for the cone/sphere (and area-light) shadow draw. On the FPT screen
	// primary's sample 0 it is a blue-noise point (better screen-space distribution -> less
	// 1-spp shadow sparkle); everywhere else it stays white-noise PCG, so those paths are
	// bit-identical. A per-light golden offset decorrelates the lights within a pixel while
	// keeping the base point blue-noise distributed across the screen. The offset index is
	// source_id REDUCED mod 256: source_id is a 28-bit hash (up to ~2.7e8), and multiplying a
	// golden constant by a value that large overflows float32 precision (epsilon >> 1), so
	// fract() would return quantized garbage and destroy the distribution. Mod 256 keeps the
	// product small enough for clean fract() while staying stable per light.
	// (A radius<=0.01 point light draws no u; the offset is harmless there.)
	vec2 u;
	if (p_use_blue_noise_u) {
		float light_offset = float(light.source_id & 0xFFu);
		// Same R2 constants as rt_blue_noise_2d's frame scroll, independent role: per-light offset within the pixel, not a per-frame shift.
		u = fract(p_blue_noise_u + vec2(0.7548776662, 0.5698402909) * light_offset);
	} else {
		u = rand2(rng_state);
	}

	// === AREA LIGHT PATH (spherical-rectangle NEE) ===
	if (light.type == RT_LIGHT_TYPE_AREA) {
		vec3 center = light.position;
		vec3 ex = rt_light_area_ex(light);
		vec3 ey = rt_light_area_ey(light);
		vec3 nrm = normalize(cross(ex, ey));
		vec3 to_center = center - hit_pos;
		// One-sided: the light emits toward its local -Z (= -nrm); reject the back side.
		if (dot(to_center, nrm) < 0.0) {
			return rt_direct_lighting_zero();
		}
		if (light.max_range_squared != 0.0 && dot(to_center, to_center) > light.max_range_squared) {
			return rt_direct_lighting_zero();
		}
		// Corner + full edges for the spherical-rectangle sampler.
		vec3 corner = center - ex - ey;
		SphQuad sq = sph_quad_init(corner, ex * 2.0, ey * 2.0, hit_pos);
		if (sq.S <= 1e-5) {
			return rt_direct_lighting_zero(); // degenerate (edge-on / tiny solid angle)
		}
		vec3 sample_pos = sph_quad_sample(sq, u.x, u.y);
		vec3 to_sample = sample_pos - hit_pos;
		float sample_dist = length(to_sample);
		vec3 L = to_sample / max(sample_dist, 1e-6);
		float NdotL = dot(N, L);
		if (NdotL <= 0.0) {
			return rt_direct_lighting_zero();
		}
		vec3 emission = light.emission;
		if (light.area_atlas_idx != 0u) {
			// Local rect UV in [0,1] from the sampled point.
			vec3 rel = sample_pos - corner;
			float exl2 = dot(ex * 2.0, ex * 2.0);
			float eyl2 = dot(ey * 2.0, ey * 2.0);
			vec2 luv = vec2(dot(rel, ex * 2.0) / max(exl2, 1e-8), dot(rel, ey * 2.0) / max(eyl2, 1e-8));
			vec2 auv = light.area_atlas_rect.xy + clamp(luv, 0.0, 1.0) * light.area_atlas_rect.zw;
			vec4 tex = textureLod(sampler2D(bindless_textures[nonuniformEXT(light.area_atlas_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), auv, light.area_max_mip);
			emission *= tex.rgb * tex.a; // premultiplied alpha, matching the raster
		}
		if ((light.flags & RT_LIGHT_FLAG_SHADOW) != 0u) {
			vec3 shadow_origin = offset_ray_origin(hit_pos, dot(geometry_normal, L) >= 0.0 ? geometry_normal : -geometry_normal);
			if (!lights_trace_shadow_ray(shadow_origin, L, max(sample_dist - 0.002, 0.001), light.shadow_caster_mask, rng_state)) {
				float shadow_visibility = 1.0 - clamp(light.shadow_opacity, 0.0, 1.0);
				if (shadow_visibility <= 0.001) {
					return rt_direct_lighting_zero();
				}
				emission *= shadow_visibility;
			}
		}
		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);
		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;
		// Solid-angle pdf = 1/S, so contribution = brdf * Le * S * (1/light_select_pdf).
		float solid_angle = sq.S;
		float inv_pdf = solid_angle / max(light_select_pdf, 1e-10);
		RTDirectLighting result;
		result.diffuse = brdf_diffuse * emission * indirect_mul * inv_pdf;
		result.specular = brdf_specular * spec_mul * emission * indirect_mul * inv_pdf;
		return result;
	}

	// === POSITIONAL LIGHT PATH (omni + spot) ===
	if (light.type == RT_LIGHT_TYPE_OMNI || light.type == RT_LIGHT_TYPE_SPOT) {
		vec3 to_light = light.position - hit_pos;
		float dist_sq = dot(to_light, to_light);

		// Early out: outside max range.
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return rt_direct_lighting_zero();
		}

		float dist = sqrt(dist_sq);
		vec3 L;
		float shadow_dist;

		if (light.radius <= 0.01) {
			// True point light: exact direction.
			L = to_light / max(dist, 0.0001);
			shadow_dist = dist;
		} else {
			// Sphere light: cone sampling for soft shadows.
			LightSample ls = lights_prepare_sample(hit_pos, light);
			float light_pdf;
			L = lights_sample_cone(ls, u, light_pdf);
			float t_center = dot(to_light, L);
			vec3 perp = to_light - t_center * L;
			float perp_sq = dot(perp, perp);
			float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
			shadow_dist = max(0.0, t_center - dt);
		}

		// Spot cone early-out.
		float spot_atten = 1.0;
		if (light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, light.spot_direction);
			if (scos <= light.cos_spot_angle) {
				return rt_direct_lighting_zero();
			}
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
			spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}

		float NdotL = dot(N, L);
		if (NdotL <= 0.0) {
			return rt_direct_lighting_zero();
		}

		if ((light.flags & RT_LIGHT_FLAG_SHADOW) != 0u) {
			vec3 shadow_origin = offset_ray_origin(hit_pos, dot(geometry_normal, L) >= 0.0 ? geometry_normal : -geometry_normal);
			if (!lights_trace_shadow_ray(shadow_origin, L, shadow_dist, light.shadow_caster_mask, rng_state)) {
				float shadow_visibility = 1.0 - clamp(light.shadow_opacity, 0.0, 1.0);
				if (shadow_visibility <= 0.001) {
					return rt_direct_lighting_zero();
				}
				spot_atten *= shadow_visibility;
			}
		}

		// Evaluate BRDF (diffuse + specular separately for specular_amount control).
		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		// Distance attenuation.
		LightSample ls_atten;
		ls_atten.distance_sq = dist_sq;
		float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		// NdotL is already included in brdf_value (evalLambertian/evalMicrofacet bake it in).
		RTDirectLighting result;
		float inv_pdf = 1.0 / max(light_select_pdf, 1e-10);
		result.diffuse = brdf_diffuse * light.emission * atten * indirect_mul * inv_pdf;
		result.specular = brdf_specular * spec_mul * light.emission * atten * indirect_mul * inv_pdf;
		return result;
	}
	// === CONE LIGHT PATH (directional) ===
	else {
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		vec3 L = lights_sample_cone(ls, u, light_pdf);

		float NdotL = dot(N, L);
		if (NdotL <= 0.0) {
			return rt_direct_lighting_zero();
		}

		if ((light.flags & RT_LIGHT_FLAG_SHADOW) != 0u) {
			vec3 shadow_origin = offset_ray_origin(hit_pos, dot(geometry_normal, L) >= 0.0 ? geometry_normal : -geometry_normal);
			if (!lights_trace_shadow_ray(shadow_origin, L, ls.max_distance, light.shadow_caster_mask, rng_state)) {
				float shadow_visibility = 1.0 - clamp(light.shadow_opacity, 0.0, 1.0);
				if (shadow_visibility <= 0.001) {
					return rt_direct_lighting_zero();
				}
				light.emission *= shadow_visibility;
			}
		}

		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		RTDirectLighting result;
		float inv_pdf = 1.0 / max(light_select_pdf, 1e-10);
		result.diffuse = brdf_diffuse * light.emission * indirect_mul * inv_pdf;
		result.specular = brdf_specular * spec_mul * light.emission * indirect_mul * inv_pdf;
		return result;
	}
}

const uint RT_DIRECT_RESERVOIR_SPATIAL_SAMPLES = 4u;

ivec2 rt_direct_reservoir_spatial_offset(uint sample_idx) {
	if (sample_idx == 0u) {
		return ivec2(1, 0);
	}
	if (sample_idx == 1u) {
		return ivec2(-1, 0);
	}
	if (sample_idx == 2u) {
		return ivec2(0, 1);
	}
	return ivec2(0, -1);
}

void lights_merge_previous_direct_reservoir(
		inout RTDirectLightReservoir reservoir,
		ivec2 pixel,
		ivec2 previous_pixel,
		vec4 previous_reservoir,
		vec4 previous_lighting,
		uint previous_key,
		bool spatial_sample,
		vec3 hit_pos,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		uint receiver_layer_mask,
		uint light_count,
		inout uint rng_state,
		bool p_use_blue_noise_u,
		vec2 p_blue_noise_u) {
	uint reject_reason = RT_SOURCE_REJECT_SOURCE_ID;
	uint previous_source_id = previous_key & 0x0FFFFFFFu;
	uint selected_idx = 0u;
	float selected_weight = 0.0;
	float found_total_weight = 0.0;
	float previous_m_uncapped = max(previous_reservoir.z, 1.0);
	float previous_m = clamp(previous_reservoir.z, 1.0, 32.0);

	if (lights_find_direct_source_by_id(previous_source_id, light_count, receiver_layer_mask, false, hit_pos, N, selected_idx, selected_weight, found_total_weight)) {
		float current_pdf = selected_weight / max(found_total_weight, 1e-10);
		uint current_key = rt_source_make_key(RT_SOURCE_CLASS_DIRECT, rt_lights[selected_idx].source_id);
		if (rt_source_direct_history_accept(pixel, previous_pixel, previous_reservoir, current_key, previous_key, current_pdf, previous_reservoir.w, reject_reason)) {
			RTDirectLighting current_lighting = lights_evaluate_single_direct_light_split(rt_lights[selected_idx], current_pdf, hit_pos, geometry_normal, N, V, material, rng_state, false, p_use_blue_noise_u, p_blue_noise_u);
			float current_target = rt_direct_reservoir_target(current_lighting);
			float previous_target = max(previous_lighting.a, 1e-6);
			// M-capped ReSTIR: when previous_m is clamped to the cap, the carried weight_sum
			// must be scaled by the same ratio, or weight_sum/M (the resolve's contribution
			// weight) inflates every frame and the image blows out. previous_m/previous_m_uncapped
			// is 1.0 until history exceeds the cap, then damps the carried weight to the cap.
			float previous_weight_sum = max(previous_reservoir.y, 0.0) * (previous_m / previous_m_uncapped);
			float candidate_weight = current_target * previous_weight_sum / previous_target;
			if (current_target > 0.0 && candidate_weight > 0.0) {
				rt_direct_reservoir_update(reservoir, current_key, current_lighting, current_pdf, current_target, candidate_weight, previous_m, previous_reservoir.w, rng_state);
				reject_reason = RT_SOURCE_REJECT_NONE;
				if (spatial_sample) {
					reservoir.spatial_accepted = true;
				} else {
					reservoir.temporal_accepted = true;
				}
			} else {
				reservoir.M += previous_m;
				reservoir.visibility_failures++;
				reject_reason = RT_SOURCE_REJECT_VISIBILITY;
			}
		}
	}

	if (spatial_sample) {
		if (reservoir.spatial_reject != RT_SOURCE_REJECT_NONE) {
			reservoir.spatial_reject = reject_reason;
		}
	} else {
		reservoir.temporal_reject = reject_reason;
	}
}

// Evaluate direct lighting using NEE. Small light sets are summed explicitly to
// avoid one-light roulette impulses that are very visible in 1-SPP RTGI.
RTDirectLighting lights_evaluate_direct_lighting_split(
		vec3 hit_pos,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint receiver_layer_mask,
		uint light_count,
		out uint out_source_key,
		out uint out_direct_source_key,
		out float out_direct_source_pdf,
		out RTDirectLighting out_direct_source_lighting,
		out bool out_direct_source_stochastic,
		out float out_direct_source_reservoir_m,
		out float out_direct_source_reservoir_weight_sum,
		out float out_direct_source_target,
		out bool out_direct_source_temporal_accepted,
		out bool out_direct_source_spatial_accepted,
		out uint out_direct_source_temporal_reject,
		out uint out_direct_source_spatial_reject,
		out uint out_direct_source_visibility_failures,
		bool p_use_blue_noise_u,
		vec2 p_blue_noise_u) {
	out_source_key = 0u;
	out_direct_source_key = 0u;
	out_direct_source_pdf = 0.0;
	out_direct_source_lighting = rt_direct_lighting_zero();
	out_direct_source_stochastic = false;
	out_direct_source_reservoir_m = 0.0;
	out_direct_source_reservoir_weight_sum = 0.0;
	out_direct_source_target = 0.0;
	out_direct_source_temporal_accepted = false;
	out_direct_source_spatial_accepted = false;
	out_direct_source_temporal_reject = RT_SOURCE_REJECT_PREV_UV;
	out_direct_source_spatial_reject = RT_SOURCE_REJECT_PREV_UV;
	out_direct_source_visibility_failures = 0u;
	if (light_count == 0u) {
		return rt_direct_lighting_zero();
	}

	uint deterministic_light_limit = is_indirect_bounce ? 4u : RTGI_DETERMINISTIC_DIRECT_LIGHT_LIMIT;
	uint valid_count = 0u;
	float total_weight = 0.0;

	// Directional lights are evaluated deterministically here, outside the regime machinery
	// below: one shadow ray each, every frame, in BOTH the deterministic and the RIS regime.
	// They no longer enter lights_direct_source_weight (Edit A), so valid_count / total_weight /
	// the stratified CDF / the reservoir are all positional-only. directional_sum is added to
	// every return path below. The evaluator call uses pdf = 1.0 and the same is_indirect_bounce,
	// matching exactly the form the deterministic-sum loop used to evaluate a directional with,
	// so a deterministic-regime directional is bit-identical to before this change.
	RTDirectLighting directional_sum = rt_direct_lighting_zero();
	for (uint idx = 0u; idx < light_count; idx++) {
		RTLightData dir_light = rt_lights[idx];
		if (dir_light.type != RT_LIGHT_TYPE_DIRECTIONAL) {
			continue;
		}
		// Same validity gate the regime predicate applied to a directional before Edit A:
		// receiver layer-mask cull + a positive selection weight (energy > 0). No range test
		// applies to a directional (the positional max-range gate never ran for it).
		if ((dir_light.cull_mask & receiver_layer_mask) == 0u) {
			continue;
		}
		if (lights_selection_weight(hit_pos, N, dir_light, is_indirect_bounce) <= 0.0) {
			continue;
		}
		RTDirectLighting dir_result = lights_evaluate_single_direct_light_split(dir_light, 1.0, hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce, p_use_blue_noise_u, p_blue_noise_u);
		directional_sum.diffuse += dir_result.diffuse;
		directional_sum.specular += dir_result.specular;
	}

	for (uint idx = 0u; idx < light_count; idx++) {
		float light_weight = 0.0;
		if (!lights_direct_source_weight(idx, light_count, receiver_layer_mask, is_indirect_bounce, hit_pos, N, light_weight)) {
			continue;
		}

		valid_count++;
		total_weight += light_weight;
	}

	if (valid_count == 0u) {
		// A directional can still be valid when no positional light is, so its deterministic
		// contribution must be returned even with an empty positional set.
		return directional_sum;
	}

	if (valid_count <= deterministic_light_limit) {
		RTDirectLighting deterministic_sum = rt_direct_lighting_zero();
		float dominant_luma = 0.0;
		for (uint idx = 0u; idx < light_count; idx++) {
			float light_weight = 0.0;
			if (!lights_direct_source_weight(idx, light_count, receiver_layer_mask, is_indirect_bounce, hit_pos, N, light_weight)) {
				continue;
			}
			RTLightData test_light = rt_lights[idx];
			RTDirectLighting light_result = lights_evaluate_single_direct_light_split(test_light, 1.0, hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce, p_use_blue_noise_u, p_blue_noise_u);
			deterministic_sum.diffuse += light_result.diffuse;
			deterministic_sum.specular += light_result.specular;
			float light_luma = rt_luminance(rt_direct_lighting_sum(light_result));
			if (light_luma > max(dominant_luma, 1e-6)) {
				dominant_luma = light_luma;
				out_source_key = rt_source_make_key(RT_SOURCE_CLASS_DIRECT, test_light.source_id);
				out_direct_source_key = out_source_key;
				out_direct_source_pdf = 1.0;
				out_direct_source_lighting = light_result;
				out_direct_source_stochastic = false;
				out_direct_source_reservoir_m = float(valid_count);
				out_direct_source_reservoir_weight_sum = light_luma;
				out_direct_source_target = light_luma;
			}
		}
		// Directionals were evaluated separately (Edit A excludes them from the loop above);
		// fold their deterministic contribution back in. For a positional-only deterministic
		// scene directional_sum is zero, so the result is bit-identical to before.
		deterministic_sum.diffuse += directional_sum.diffuse;
		deterministic_sum.specular += directional_sum.specular;
		return deterministic_sum;
	}

	uint candidate_count = min(valid_count, is_indirect_bounce ? 2u : RT_LIGHT_RESERVOIR_SIZE);
	RTDirectLightReservoir reservoir = rt_direct_light_reservoir_empty();
	for (uint candidate = 0u; candidate < RT_LIGHT_RESERVOIR_SIZE; candidate++) {
		if (candidate >= candidate_count) {
			break;
		}

		uint selected_idx = 0u;
		float selected_weight = 0.0;

		float selected_cdf = (float(candidate) + rand(rng_state)) * (total_weight / float(candidate_count));
		float cdf = 0.0;
		for (uint idx = 0u; idx < light_count; idx++) {
			float light_weight = 0.0;
			if (!lights_direct_source_weight(idx, light_count, receiver_layer_mask, is_indirect_bounce, hit_pos, N, light_weight)) {
				continue;
			}
			cdf += light_weight;
			if (selected_cdf <= cdf) {
				selected_idx = idx;
				selected_weight = light_weight;
				break;
			}
		}

		if (selected_weight <= 0.0) {
			reservoir.M += 1.0;
			continue;
		}

		float light_select_pdf = selected_weight / max(total_weight, 1e-10);
		RTDirectLighting light_result = lights_evaluate_single_direct_light_split(rt_lights[selected_idx], light_select_pdf, hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce, p_use_blue_noise_u, p_blue_noise_u);
		uint source_key = rt_source_make_key(RT_SOURCE_CLASS_DIRECT, rt_lights[selected_idx].source_id);
		float light_target = rt_direct_reservoir_target(light_result);
		rt_direct_reservoir_update(reservoir, source_key, light_result, light_select_pdf, light_target, light_target, 1.0, 1.0, rng_state);
	}

	// Probe-feed dispatches (WRC update / SPG gather) keep ONLY the local single-frame
	// reservoir they just built above plus the resolve below; the cross-frame reuse below
	// (the temporal merge AND the 4-tap spatial reuse loop, both nested in this block)
	// reads and merges screen-pixel reservoir state (reprojected from the camera buffers at
	// (ray_index, 0)), which has nothing to do with the world point a probe ray sampled.
	// Gate both so probe radiance stays self-contained; the resolve still runs.
	if (!is_indirect_bounce && !rt_probe_dispatch_mode()) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
		ivec2 previous_pixel = ivec2(0);
		vec4 previous_reservoir = vec4(0.0);
		vec4 previous_lighting = vec4(0.0);
		uint previous_key = 0u;
		uint reject_reason = RT_SOURCE_REJECT_NONE;
		bool has_reprojected_previous = rt_source_load_reprojected_previous_direct(pixel, previous_pixel, previous_reservoir, previous_lighting, previous_key, reject_reason);
		if (has_reprojected_previous) {
			lights_merge_previous_direct_reservoir(reservoir, pixel, previous_pixel, previous_reservoir, previous_lighting, previous_key, false, hit_pos, geometry_normal, N, V, material, receiver_layer_mask, light_count, rng_state, p_use_blue_noise_u, p_blue_noise_u);
		} else {
			reservoir.temporal_reject = reject_reason;
		}

		if (has_reprojected_previous) {
			for (uint spatial_idx = 0u; spatial_idx < RT_DIRECT_RESERVOIR_SPATIAL_SAMPLES; spatial_idx++) {
				ivec2 spatial_pixel = previous_pixel + rt_direct_reservoir_spatial_offset(spatial_idx);
				vec4 spatial_reservoir = vec4(0.0);
				vec4 spatial_lighting = vec4(0.0);
				uint spatial_key = 0u;
				uint spatial_reject = RT_SOURCE_REJECT_NONE;
				if (!rt_source_load_previous_direct_reservoir_at(spatial_pixel, spatial_reservoir, spatial_lighting, spatial_key, spatial_reject)) {
					if (reservoir.spatial_reject != RT_SOURCE_REJECT_NONE) {
						reservoir.spatial_reject = spatial_reject;
					}
					continue;
				}

				lights_merge_previous_direct_reservoir(reservoir, pixel, spatial_pixel, spatial_reservoir, spatial_lighting, spatial_key, true, hit_pos, geometry_normal, N, V, material, receiver_layer_mask, light_count, rng_state, p_use_blue_noise_u, p_blue_noise_u);
			}
		}
	}

	// resolved_lighting is the positional-only reservoir result. It alone drives the out_*
	// direct-source bookkeeping and the reservoir record below: the directional is NOT a
	// reservoir source, so the recorded radiance/target (re-read next frame by the temporal
	// merge as previous_lighting.a) must stay positional-only or it would bias the history
	// reweight with a sun contribution keyed to a positional source. The directional is added
	// only to returned_lighting, on BOTH return paths.
	RTDirectLighting resolved_lighting = rt_direct_reservoir_resolve(reservoir);
	RTDirectLighting returned_lighting = resolved_lighting;
	returned_lighting.diffuse += directional_sum.diffuse;
	returned_lighting.specular += directional_sum.specular;
	if (!reservoir.valid) {
		if (!is_indirect_bounce && !rt_probe_dispatch_mode()) {
			rt_source_direct_reservoir_record(ivec2(gl_LaunchIDEXT.xy), 0u, 0.0, 0.0, reservoir.M, 0.0, vec3(0.0), 0.0);
		}
		return returned_lighting;
	}

	out_source_key = reservoir.selected_key;
	out_direct_source_key = reservoir.selected_key;
	out_direct_source_pdf = reservoir.selected_pdf;
	out_direct_source_lighting = resolved_lighting;
	out_direct_source_stochastic = true;
	out_direct_source_reservoir_m = reservoir.M;
	out_direct_source_reservoir_weight_sum = reservoir.weight_sum;
	out_direct_source_target = reservoir.selected_target;
	out_direct_source_temporal_accepted = reservoir.temporal_accepted;
	out_direct_source_spatial_accepted = reservoir.spatial_accepted;
	out_direct_source_temporal_reject = reservoir.temporal_reject;
	out_direct_source_spatial_reject = reservoir.spatial_reject;
	out_direct_source_visibility_failures = reservoir.visibility_failures;

	if (!is_indirect_bounce && !rt_probe_dispatch_mode()) {
		rt_source_direct_reservoir_record(ivec2(gl_LaunchIDEXT.xy), reservoir.selected_key, reservoir.selected_pdf, reservoir.weight_sum, reservoir.M, reservoir.confidence, rt_direct_lighting_sum(resolved_lighting), reservoir.selected_target);
	}

	return returned_lighting;
}

vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 geometry_normal,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint receiver_layer_mask,
		uint light_count) {
	uint source_key;
	uint direct_source_key;
	float direct_source_pdf;
	RTDirectLighting direct_source_lighting;
	bool direct_source_stochastic;
	float direct_source_reservoir_m;
	float direct_source_reservoir_weight_sum;
	float direct_source_target;
	bool direct_source_temporal_accepted;
	bool direct_source_spatial_accepted;
	uint direct_source_temporal_reject;
	uint direct_source_spatial_reject;
	uint direct_source_visibility_failures;
	return rt_direct_lighting_sum(lights_evaluate_direct_lighting_split(
			hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce, receiver_layer_mask, light_count,
			source_key, direct_source_key, direct_source_pdf, direct_source_lighting, direct_source_stochastic,
			direct_source_reservoir_m, direct_source_reservoir_weight_sum, direct_source_target,
			direct_source_temporal_accepted, direct_source_spatial_accepted, direct_source_temporal_reject,
			direct_source_spatial_reject, direct_source_visibility_failures,
			/*p_use_blue_noise_u=*/false, /*p_blue_noise_u=*/vec2(0.0)));
}
