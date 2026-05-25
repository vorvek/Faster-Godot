// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0.
// Note: ray_query_alpha_test() still requires GL_EXT_ray_query (used by DLSS-RR path).

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff
#define RT_LIGHT_FLAG_SHADOW 1u

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 96 bytes, std430)
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
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

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
		out uint out_source_key) {
	out_pdf = 0.0;
	out_selected_weight = 0.0;
	out_source_key = 0u;
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
	if ((geom.layer_mask & receiver_layer_mask) == 0u || geom.primitive_count == 0u || geom.vertex_address == 0ul || (geom.flags & FLAG_COMPRESSED) != 0u) {
		return rt_direct_lighting_zero();
	}

	MaterialData light_mat = materials[candidate.geometry_index];
	vec3 emission = max(light_mat.emission_color * light_mat.emission_strength, vec3(0.0));

	uint primitive_id = min(uint(rand(rng_state) * float(geom.primitive_count)), geom.primitive_count - 1u);
	float su = sqrt(rand(rng_state));
	float bv = rand(rng_state);
	vec3 bary = vec3(1.0 - su, su * (1.0 - bv), su * bv);

	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec3 p0 = fetch_position_uncompressed(geom, i0);
	vec3 p1 = fetch_position_uncompressed(geom, i1);
	vec3 p2 = fetch_position_uncompressed(geom, i2);
	vec3 wp0 = rt_emissive_candidate_transform_point(candidate, p0);
	vec3 wp1 = rt_emissive_candidate_transform_point(candidate, p1);
	vec3 wp2 = rt_emissive_candidate_transform_point(candidate, p2);
	vec3 light_pos = bary.x * wp0 + bary.y * wp1 + bary.z * wp2;
	vec3 area_vec = cross(wp1 - wp0, wp2 - wp0);
	float tri_area = length(area_vec) * 0.5;
	if (tri_area <= 1e-8) {
		return rt_direct_lighting_zero();
	}

	if ((light_mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
		vec2 uv = fetch_uv(geom, i0, i1, i2, bary);
		uv = uv * light_mat.uv1_scale + light_mat.uv1_offset;
		emission *= sample_material_texture(light_mat.emission_texture_idx, uv, light_mat.flags).rgb;
	}
	emission *= scene_data_block.data.emissive_exposure_normalization;
	if (luminance(emission) <= 1e-6) {
		return rt_direct_lighting_zero();
	}

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
	float light_cos = max(dot(light_normal, -L), 0.0);
	if (light_cos <= 0.0) {
		return rt_direct_lighting_zero();
	}

	vec3 shadow_origin = offset_ray_origin(hit_pos, dot(geometry_normal, L) >= 0.0 ? geometry_normal : -geometry_normal);
	if (!lights_trace_shadow_ray(shadow_origin, L, max(dist - 0.002, 0.001), 0xFFFFFFFFu, rng_state)) {
		return rt_direct_lighting_zero();
	}

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);
	float pdf_area = select_pdf / max(float(geom.primitive_count) * tri_area, 1e-8);
	out_pdf = pdf_area;
	float geom_term = light_cos / max(dist_sq, 1e-6);
	float inv_pdf = 1.0 / max(pdf_area, 1e-8);

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
		bool is_indirect_bounce) {
	vec2 u = rand2(rng_state);

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
		out uint out_source_key) {
	out_source_key = 0u;
	if (light_count == 0u) {
		return rt_direct_lighting_zero();
	}

	uint deterministic_light_limit = is_indirect_bounce ? 4u : RTGI_DETERMINISTIC_DIRECT_LIGHT_LIMIT;
	uint valid_count = 0u;
	float total_weight = 0.0;

	for (uint idx = 0u; idx < light_count; idx++) {
		RTLightData test_light = rt_lights[idx];

		// Range check for positional lights.
		bool is_valid = (test_light.cull_mask & receiver_layer_mask) != 0u;
		bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
		if (is_valid && is_positional) {
			vec3 to_l = test_light.position - hit_pos;
			float d2 = dot(to_l, to_l);
			is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
		}

		if (!is_valid) {
			continue;
		}

		float light_weight = lights_selection_weight(hit_pos, N, test_light, is_indirect_bounce);
		if (light_weight <= 0.0) {
			continue;
		}

		valid_count++;
		total_weight += light_weight;
	}

	if (valid_count == 0u) {
		return rt_direct_lighting_zero();
	}

	if (valid_count <= deterministic_light_limit) {
		RTDirectLighting deterministic_sum = rt_direct_lighting_zero();
		float dominant_luma = 0.0;
		for (uint idx = 0u; idx < light_count; idx++) {
			RTLightData test_light = rt_lights[idx];

			bool is_valid = (test_light.cull_mask & receiver_layer_mask) != 0u;
			bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
			if (is_valid && is_positional) {
				vec3 to_l = test_light.position - hit_pos;
				float d2 = dot(to_l, to_l);
				is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
			}
			if (!is_valid || lights_selection_weight(hit_pos, N, test_light, is_indirect_bounce) <= 0.0) {
				continue;
			}
			RTDirectLighting light_result = lights_evaluate_single_direct_light_split(test_light, 1.0, hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce);
			deterministic_sum.diffuse += light_result.diffuse;
			deterministic_sum.specular += light_result.specular;
			float light_luma = rt_luminance(rt_direct_lighting_sum(light_result));
			if (light_luma > max(dominant_luma, 1e-6)) {
				dominant_luma = light_luma;
				out_source_key = rt_source_make_key(RT_SOURCE_CLASS_DIRECT, test_light.source_id);
			}
		}
		return deterministic_sum;
	}

	uint candidate_count = min(valid_count, is_indirect_bounce ? 2u : 3u);
	RTDirectLighting candidate_sum = rt_direct_lighting_zero();
	uint accepted_candidates = 0u;
	float dominant_luma = 0.0;
	for (uint candidate = 0u; candidate < candidate_count; candidate++) {
		uint selected_idx = 0u;
		float selected_weight = 0.0;
		float selected_cdf = (float(candidate) + rand(rng_state)) * (total_weight / float(candidate_count));
		float cdf = 0.0;
		for (uint idx = 0u; idx < light_count; idx++) {
			RTLightData test_light = rt_lights[idx];

			bool is_valid = (test_light.cull_mask & receiver_layer_mask) != 0u;
			bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
			if (is_valid && is_positional) {
				vec3 to_l = test_light.position - hit_pos;
				float d2 = dot(to_l, to_l);
				is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
			}
			if (!is_valid) {
				continue;
			}

			float light_weight = lights_selection_weight(hit_pos, N, test_light, is_indirect_bounce);
			if (light_weight <= 0.0) {
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
			continue;
		}

		float light_select_pdf = selected_weight / max(total_weight, 1e-10);
		RTDirectLighting light_result = lights_evaluate_single_direct_light_split(rt_lights[selected_idx], light_select_pdf, hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce);
		candidate_sum.diffuse += light_result.diffuse;
		candidate_sum.specular += light_result.specular;
		float light_luma = rt_luminance(rt_direct_lighting_sum(light_result));
		if (light_luma > max(dominant_luma, 1e-6)) {
			dominant_luma = light_luma;
			out_source_key = rt_source_make_key(RT_SOURCE_CLASS_DIRECT, rt_lights[selected_idx].source_id);
		}
		accepted_candidates++;
	}

	float inv_candidate_count = 1.0 / max(float(accepted_candidates), 1.0);
	candidate_sum.diffuse *= inv_candidate_count;
	candidate_sum.specular *= inv_candidate_count;
	return candidate_sum;
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
	return rt_direct_lighting_sum(lights_evaluate_direct_lighting_split(hit_pos, geometry_normal, N, V, material, rng_state, is_indirect_bounce, receiver_layer_mask, light_count, source_key));
}
