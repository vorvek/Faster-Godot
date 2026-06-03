// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl,
//   raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped for back-face hits.
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec2 uv2; // Raw UV2 (no material scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

vec3 rt_orthonormalize_tangent(vec3 world_tangent, vec3 world_normal) {
	vec3 tangent = world_tangent - world_normal * dot(world_normal, world_tangent);
	float len_sq = dot(tangent, tangent);
	if (len_sq > 1e-12) {
		return tangent * inversesqrt(len_sq);
	}
	vec3 axis = abs(world_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	return normalize(cross(axis, world_normal));
}

/// Fetch vertex attributes and transform to world space.
/// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[h.geometry_idx];
	h.uv = vec2(0.0);
	h.uv2 = vec2(0.0);
	h.color = vec4(1.0);

	mat3 model_matrix = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = transpose(mat3(gl_WorldToObjectEXT));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
		h.uv2 = hit_attribs.bary_or_uv;
		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		h.geometry_normal = normalize(normal_matrix * obj_normal);
		h.tangent = rt_orthonormalize_tangent(model_matrix * obj_tangent, h.geometry_normal);
		h.bitangent = cross(h.geometry_normal, h.tangent);

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	} else
#endif
	{
		VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
		h.uv = attrs.uv;
		h.uv2 = attrs.uv2;
		h.color = attrs.color;

		// Triangle hit: reuse `attrs` from the top-level fetch.
		h.geometry_normal = normalize(normal_matrix * attrs.normal);
		h.tangent = rt_orthonormalize_tangent(model_matrix * attrs.tangent, h.geometry_normal);
		h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

		h.is_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

	return h;
}

// ============================================================================
// HELPERS
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

/// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

/// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags)) {
		mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 view_pos = (view_mat * vec4(hit_pos, 1.0)).xyz;
		vec4 clip_pos = scene_data_block.data.projection_matrix * vec4(view_pos, 1.0);
		float ndc_depth = clip_pos.z / clip_pos.w;
		imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(ndc_depth));
	}
}

// ============================================================================
// HISTORY VALIDITY WRITE (primary ray only)
// ============================================================================

vec4 pack_history_id(uint id) {
	uvec4 bytes = uvec4(
			id & 0xFFu,
			(id >> 8u) & 0xFFu,
			(id >> 16u) & 0xFFu,
			(id >> 24u) & 0xFFu);
	return vec4(bytes) * (1.0 / 255.0);
}

uint mix_history_id(uint id, uint value) {
	uint h = id ^ (value + 0x9e3779b9u + (id << 6u) + (id >> 2u));
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h == 0u ? 1u : h;
}

void write_primary_hit_history_validity() {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	uint geom_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[geom_idx];
	float valid = ((geom.flags & FLAG_HISTORY_INVALID) != 0u) ? 0.0 : 1.0;
	uint history_id = geom.history_id;
	if ((geom.flags & FLAG_PRIMITIVE_HISTORY_ID) != 0u) {
		history_id = mix_history_id(history_id, uint(gl_PrimitiveID));
	}
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	imageStore(rt_history_validity_image, pixel, vec4(valid, 0.0, 0.0, 0.0));
	imageStore(rt_history_id_image, pixel, pack_history_id(history_id));
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
/// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

/// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

/// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
/// Uses unjittered VP matrices matching the raster motion_vectors_store convention.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	uint geom_idx = gl_InstanceCustomIndexEXT;
	int mi = motion_indices[geom_idx];

	// Resolve previous-frame model matrix: compact entry if moved, current transform otherwise.
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : mat4(gl_ObjectToWorldEXT);

	vec3 obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;
	vec3 prev_obj_pos = obj_pos;

	GeometryData geom = geometries[geom_idx];

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif

#ifdef RT_HIT_ATTRIBS_DECLARED
	if ((geom.flags & FLAG_DEFORMED) != 0u) {
		uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
		if (prev_addr != 0ul) {
			uint i0, i1, i2;
			get_triangle_indices(geom, i0, i1, i2);
			vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
			FloatBuffer prev_vb = FloatBuffer(prev_addr);
			uint stride_floats = geom.position_stride >> 2;
			vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u],
					prev_vb.v[i0 * stride_floats + 1u],
					stride_floats >= 3u ? prev_vb.v[i0 * stride_floats + 2u] : 0.0);
			vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u],
					prev_vb.v[i1 * stride_floats + 1u],
					stride_floats >= 3u ? prev_vb.v[i1 * stride_floats + 2u] : 0.0);
			vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u],
					prev_vb.v[i2 * stride_floats + 1u],
					stride_floats >= 3u ? prev_vb.v[i2 * stride_floats + 2u] : 0.0);
			prev_obj_pos = bary.x * p0 + bary.y * p1 + bary.z * p2;
		}
	}
#endif

	vec3 prev_world_pos = (prev_model * vec4(prev_obj_pos, 1.0)).xyz;

	vec2 curr_uv;
	vec2 prev_uv;
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	if (!project_uv_checked(hit_pos, curr_vp_unjittered, curr_uv) ||
			!project_uv_checked(prev_world_pos, prev_vp_unjittered, prev_uv)) {
		rt_store_invalid_primary_velocity(pixel);
		imageStore(rt_history_validity_image, pixel, vec4(0.0));
		return;
	}

	rt_store_primary_velocity(pixel, curr_uv, prev_uv);
}

// ============================================================================
// RTGI GUIDE WRITES (primary ray only)
// ============================================================================

// rtgi_trace_specular_reflected_hit removed (deferred to raygen)

void write_primary_hit_guides(HitData h, MaterialResult m) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 view_pos = (view_mat * vec4(h.hit_pos, 1.0)).xyz;
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	uint geom_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[geom_idx];
	int mi = motion_indices[geom_idx];
	float expected_prev_view_z = 0.0;
	bool can_reconstruct_prev_pos = (geom.flags & (FLAG_DEFORMED | FLAG_PROCEDURAL)) == 0u;
	if (can_reconstruct_prev_pos) {
		mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : mat4(gl_ObjectToWorldEXT);
		vec3 obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(h.hit_pos, 1.0)).xyz;
		vec3 prev_world_pos = (prev_model * vec4(obj_pos, 1.0)).xyz;
		mat4 prev_view_mat = transpose(mat4(scene_data_block.prev_data.view_matrix[0],
				scene_data_block.prev_data.view_matrix[1],
				scene_data_block.prev_data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 prev_view_pos = (prev_view_mat * vec4(prev_world_pos, 1.0)).xyz;
		expected_prev_view_z = abs(prev_view_pos.z);
	}

	vec3 normal = normalize(m.normal);
	float guide_roughness = clamp(m.roughness, 0.0, 1.0);
	imageStore(rt_albedo_metalness_image, pixel, vec4(max(m.albedo, vec3(0.0)), clamp(m.metalness, 0.0, 1.0)));
	imageStore(rt_normal_roughness_image, pixel, vec4(normal * 0.5 + 0.5, guide_roughness));
	imageStore(rt_viewz_hitdist_image, pixel, vec4(abs(view_pos.z), max(gl_HitTEXT, 0.0), expected_prev_view_z, 0.0));
	float specular_risk = max(1.0 - guide_roughness, clamp(m.metalness, 0.0, 1.0));
	vec3 view_dir = normalize(-gl_WorldRayDirectionEXT);
	int vis_mode = int(get_rt_param(RT_PARAM_VIS_MODE));
	float guide_hit_distance = max(gl_HitTEXT, 0.0);
	imageStore(rt_specular_guide_image, pixel, vec4(guide_roughness, guide_hit_distance, specular_risk, 1.0));
	imageStore(rt_specular_reprojection_image, pixel, vec4(0.0));
	if (vis_mode == RT_VIS_MODE_SPECULAR_REFLECTION_DIRECTION) {
		vec3 diagnostic_reflection_dir = normalize(reflect(-view_dir, normal));
		imageStore(rt_specular_reflection_direction_image, pixel, vec4(diagnostic_reflection_dir * 0.5 + 0.5, specular_risk));
	} else if (vis_mode == RT_VIS_MODE_SPECULAR_REFLECTED_HIT_DISTANCE) {
		imageStore(rt_specular_reflection_direction_image, pixel, vec4(0.0, 0.0, 0.0, 0.0));
	} else if (vis_mode == RT_VIS_MODE_SPECULAR_REFLECTED_HIT_NORMAL) {
		imageStore(rt_specular_reflection_direction_image, pixel, vec4(vec3(0.0), specular_risk));
	}
}

uint rtgi_primary_surface_rng_seed(HitData h, MaterialResult m, uint frame_index) {
	GeometryData geom = geometries[h.geometry_idx];
	uint seed = geom.history_id;
	if ((geom.flags & FLAG_PRIMITIVE_HISTORY_ID) != 0u) {
		seed = mix_history_id(seed, uint(gl_PrimitiveID));
	}

	ivec3 quantized_position = ivec3(floor(h.hit_pos * 32.0 + vec3(0.5)));
	vec3 normal = normalize(m.normal);
	uvec3 quantized_normal = uvec3(clamp(floor(normal * 127.0 + vec3(128.0)), vec3(0.0), vec3(255.0)));
	uvec2 quantized_uv = uvec2(clamp(floor(fract(abs(h.uv)) * 4096.0), vec2(0.0), vec2(4095.0)));
	uint material_key = (uint(clamp(floor(clamp(m.roughness, 0.0, 1.0) * 63.0 + 0.5), 0.0, 63.0)) << 16u) |
			(uint(clamp(floor(clamp(m.metalness, 0.0, 1.0) * 63.0 + 0.5), 0.0, 63.0)) << 22u);
	uint normal_key = quantized_normal.x | (quantized_normal.y << 8u) | (quantized_normal.z << 16u);
	uint uv_key = (quantized_uv.x & 0xFFFu) | ((quantized_uv.y & 0xFFFu) << 12u);

	seed = mix_history_id(seed, uint(quantized_position.x));
	seed = mix_history_id(seed, uint(quantized_position.y));
	seed = mix_history_id(seed, uint(quantized_position.z));
	seed = mix_history_id(seed, normal_key);
	seed = mix_history_id(seed, uv_key);
	seed = mix_history_id(seed, material_key);
	seed = mix_history_id(seed, frame_index);
	return seed;
}

float rtgi_radical_inverse_vdc(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 rtgi_primary_diffuse_temporal_sample(HitData h, MaterialResult m, uint frame_index) {
	uint stable_seed = rtgi_primary_surface_rng_seed(h, m, 0u);
	uint sample_index = (frame_index + (stable_seed & 15u)) & 15u;
	vec2 sequence_sample = vec2((float(sample_index) + 0.5) * (1.0 / 16.0), rtgi_radical_inverse_vdc(sample_index));
	vec2 rotation = vec2(float((stable_seed >> 8u) & 255u), float((stable_seed >> 16u) & 255u)) * (1.0 / 256.0);
	return fract(sequence_sample + rotation);
}

vec2 rtgi_primary_diffuse_screen_probe_sample(HitData h, MaterialResult m, uint frame_index) {
	const uint cell_size = 4u;
	const uint cell_mask = cell_size - 1u;
	uint stable_seed = rtgi_primary_surface_rng_seed(h, m, 0u);
	ivec2 visible_pixel = ivec2(gl_LaunchIDEXT.xy) - ivec2(round(rt_current_origin()));
	uvec2 visible_pixel_u = uvec2(max(visible_pixel, ivec2(0)));
	uvec2 probe_cell = visible_pixel_u / uvec2(cell_size);
	uvec2 local_pixel = visible_pixel_u & uvec2(cell_mask);
	uint local_slot = (local_pixel.x + local_pixel.y * cell_size) & 15u;
	uint probe_seed = pcg_hash((probe_cell.x * 73856093u) ^ (probe_cell.y * 19349663u) ^ (stable_seed & 0xffff0000u));
	uint sample_index = (local_slot + frame_index + (probe_seed & 15u)) & 15u;
	vec2 sequence_sample = vec2((float(sample_index) + 0.5) * (1.0 / 16.0), rtgi_radical_inverse_vdc(sample_index));
	vec2 rotation = vec2(float((probe_seed >> 8u) & 255u), float((probe_seed >> 16u) & 255u)) * (1.0 / 256.0);
	return fract(sequence_sample + rotation);
}

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

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

#define FOG_HAS_RADIANCE

vec3 fog_sample_radiance(vec3 vertex, float mip_level) {
	vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
	float roughness_lod = mip_level * MAX_ROUGHNESS_LOD;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 cube_uv = vec3_to_oct_with_border(cube_view, border);
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), cube_uv, roughness_lod).rgb;
}

#include "raytracing_fog_inc.glsl"

/// Apply environment fog for the ray segment that was just traversed.
/// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(float segment_dist, inout vec3 radiance, inout vec3 throughput) {
	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u) {
		return;
	}

	// Build a view-space vertex along the ray direction at the hit distance.
	// fog_process needs view-space position for distance and height calculations.
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * segment_dist;
	vec3 vertex = (view_mat * vec4(world_hit, 1.0)).xyz;

	vec4 fog = fog_process(scene_data_block.data, vertex);
	radiance += throughput * fog.rgb * fog.a;
	throughput *= (1.0 - fog.a);
}

/// Converts specular parameter [0..1] to dielectric F0.
float specular_to_f0(float specular) {
	return 0.16 * specular * specular;
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
void debug_visualize(
		int vis_mode,
		vec3 geometry_normal,
		vec3 final_normal,
		vec3 tangent_space_normal,
		vec3 world_tangent,
		vec3 world_bitangent,
		vec2 uv,
		vec3 albedo,
		vec3 orm,
		float metalness,
		float roughness,
		float specular,
		vec3 emissive,
		vec3 V,
		float NdotV) {
	PathState ps = path_unpack(payload);

	if (vis_mode == 1) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ps.hit_t = gl_HitTEXT;
			ps.offset_normal = geometry_normal;
			ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, geometry_normal);
			path_pack(payload, ps);
			return;
		} else {
			ps.radiance = geometry_normal * 0.5 + 0.5;
		}
	} else if (vis_mode == 2) {
		ps.radiance = geometry_normal * 0.5 + 0.5;
	} else if (vis_mode == 3) {
		ps.radiance = final_normal * 0.5 + 0.5;
	} else if (vis_mode == 4) {
		ps.radiance = tangent_space_normal * 0.5 + 0.5;
	} else if (vis_mode == 5) {
		ps.radiance = world_tangent * 0.5 + 0.5;
	} else if (vis_mode == 6) {
		ps.radiance = world_bitangent * 0.5 + 0.5;
	} else if (vis_mode == 7) {
		ps.radiance = vec3(fract(uv), 0.0);
	} else if (vis_mode == 8) {
		ps.radiance = albedo;
	} else if (vis_mode == 9) {
		ps.radiance = orm;
	} else if (vis_mode == 10) {
		ps.radiance = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
	} else if (vis_mode == 11) {
		ps.radiance = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
	} else if (vis_mode == 12) {
		ps.radiance = (final_normal * 0.5 + 0.5) * (1.0 - roughness * 0.5);
	} else if (vis_mode == 13) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			if (roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
				ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
				ps.radiance = vec3(0.1, 0.1, 0.4);
				ps.hit_t = gl_HitTEXT;
				ps.offset_normal = final_normal;
				ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, final_normal);
				path_pack(payload, ps);
				return;
			} else {
				ps.radiance = vec3(0.1, 0.1, 0.4);
			}
		} else {
			float spec_hit_t = gl_HitTEXT;
			float v = clamp(log(spec_hit_t + 1.0) / log(1000.0), 0.0, 1.0);
			vec3 color;
			if (v < 0.33) {
				color = mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), v * 3.0);
			} else if (v < 0.66) {
				color = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), (v - 0.33) * 3.0);
			} else {
				color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), (v - 0.66) * 3.0);
			}
			ps.radiance = color;
		}
	} else if (vis_mode == 14) {
		ps.radiance = vec3(metalness);
	} else if (vis_mode == 15) {
		ps.radiance = vec3(roughness);
	} else if (vis_mode == 16) {
		mat3 world_to_view = mat3(scene_data_block.data.inv_view_matrix);
		ps.radiance = normalize(world_to_view * final_normal) * 0.5 + 0.5;
	} else if (vis_mode == 17) {
		vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
		ps.radiance = mix(diffuse_albedo, specular_albedo, metalness);
	} else if (vis_mode == 18) {
		ps.radiance = baseColorToSpecularF0(albedo, metalness, specular_to_f0(specular));
	} else if (vis_mode == 19) {
		bool is_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
		ps.radiance = is_front_face ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (vis_mode == 20) {
		float depth_range = scene_data_block.data.z_far - scene_data_block.data.z_near;
		float d = clamp(gl_HitTEXT / depth_range, 0.0, 1.0);
		ps.radiance = vec3(d);
	} else if (vis_mode == 21) {
		ps.radiance = emissive;
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		// We sample both BRDF lobes once with the un-clamped shading normal
		// and once with the clamped shading normal, reusing the same random
		// pair so the two passes are directly comparable.
		//
		// Color legend (ordered cleanest -> most biased):
		//   green   = no rejection in either pass (no fallback needed)
		//   blue    = clamp alone resolves rejection (energy preserving)
		//   yellow  = clamp + mirror (one lobe still mirrored after clamp)
		//   red     = clamp + mirror (both lobes mirrored - most biased pixel)
		//
		// Production output is never black anymore -- yellow/red just indicate
		// where the cheaper Layer-1 clamp could not catch the bump and the
		// biased Layer-2 mirror had to step in. Raise
		// RT_SHADING_NORMAL_CLAMP_THRESHOLD if yellow/red dominate.
		MaterialProperties dbg_mat;
		dbg_mat.baseColor = albedo;
		dbg_mat.metalness = metalness;
		dbg_mat.roughness = roughness;
		dbg_mat.dielectricF0 = specular_to_f0(specular);
		dbg_mat.emissive = vec3(0.0);
		dbg_mat.transmissivness = 0.0;
		dbg_mat.opacity = 1.0;

		vec3 dbg_dir;
		vec3 dbg_weight;
		// Draw both random pairs up front so each (specular/diffuse) test in
		// the un-clamped and clamped pass uses the exact same numbers.
		vec2 u_spec_rng = rand2(ps.rng_state);
		vec2 u_diff_rng = rand2(ps.rng_state);

		bool u_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, final_normal, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool u_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, final_normal, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		// Uses the same threshold as shade_and_bounce so the visualization
		// stays in sync with production sampling.
		vec3 N_clamped = clampShadingNormal(final_normal, geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		bool c_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, N_clamped, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool c_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, N_clamped, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		bool all_uncl_ok = u_spec_ok && u_diff_ok;
		bool all_cl_ok = c_spec_ok && c_diff_ok;
		bool any_cl_ok = c_spec_ok || c_diff_ok;

		if (all_uncl_ok) {
			ps.radiance = vec3(0.0, 1.0, 0.0);
		} else if (all_cl_ok) {
			ps.radiance = vec3(0.0, 0.4, 1.0);
		} else if (any_cl_ok) {
			ps.radiance = vec3(1.0, 1.0, 0.0);
		} else {
			ps.radiance = vec3(1.0, 0.0, 0.0);
		}
	} else if (vis_mode == 23) {
		float deviation = clamp(1.0 - max(dot(normalize(geometry_normal), normalize(final_normal)), 0.0), 0.0, 1.0);
		float tangent_y = clamp(tangent_space_normal.y * 0.5 + 0.5, 0.0, 1.0);
		ps.radiance = vec3(smoothstep(0.00, 0.45, deviation), tangent_y, 1.0 - smoothstep(0.10, 0.85, deviation));
	} else if (vis_mode == RT_VIS_MODE_SPECULAR_REFLECTION_DIRECTION) {
		vec3 reflection_dir = normalize(reflect(-V, normalize(final_normal)));
		ps.radiance = reflection_dir * 0.5 + 0.5;
	}

	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	path_pack(payload, ps);
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

/// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
/// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	PathState ps = path_unpack(payload);
	GeometryData hit_geom = geometries[h.geometry_idx];

	vec3 V = -gl_WorldRayDirectionEXT;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);
	float material_roughness = clamp(m.roughness, 0.0, 1.0);
	float path_min_roughness = (RT_GET_SAMPLE_COUNT() >= 16u) ? 0.0 : min(0.75, 0.02 + 0.05 * float(total_bounces));
	float sampling_roughness = max(material_roughness, path_min_roughness);
	uint rtgi_sampling_controls = uint(get_rt_param(RT_PARAM_RTGI_SAMPLING_CONTROLS));
	uint rt_mode = uint(get_rt_param(RT_PARAM_MODE));
	if (total_bounces == 0u && rt_mode == RT_MODE_PATH_TRACED && RT_GET_SAMPLE_COUNT() == 1u) {
		ps.rng_state = rtgi_primary_surface_rng_seed(h, m, uint(get_rt_param(RT_PARAM_FRAME_INDEX)));
	}
	bool reflections_only = rt_mode == RT_MODE_REFLECTIONS_RT_ONLY;
	bool raster_owned_primary = (reflections_only || rt_mode == RT_MODE_HYBRID) && total_bounces == 0u;
	// WRC probe-update and SPG-gather rays are full path traces that cache WORLD radiance for the GI
	// probes. They inherit RT_PARAM_MODE (== RT_MODE_HYBRID in a Hybrid frame), but their first hit is
	// NOT a raster-owned camera primary -- raster shades the camera pixel, not what a probe ray sees.
	// Treating the probe's first hit as raster-owned drops its first-hit emission (and direct) term, so
	// emissive-mesh light never enters the cache and emissive-only GI renders black (analytic light
	// survives only because NEE re-captures it at secondary bounces; a mesh's own Le has no such
	// re-capture). Exclude the probe-feed dispatches so they shade fully at the first hit.
	bool hybrid_primary = raster_owned_primary && !rt_wrc_probe_update_mode() && !rt_spg_gather_mode();
	if (hybrid_primary && (hit_geom.flags & RT_GEOM_FLAG_RASTER_GI_OWNER) != 0u) {
		ps.packed_bounces_flags = set_primary_raster_gi_owner(ps.packed_bounces_flags);
	}

	// Environment fog for this ray segment (before surface contribution).
	if (!hybrid_primary) {
		apply_segment_fog(gl_HitTEXT, ps.radiance, ps.throughput);
	}

	// Emissive contribution.
	if (!hybrid_primary) {
		bool secondary_emissive = total_bounces > 0u;
		bool explicit_emissive_candidate = (geometries[h.geometry_idx].flags & RT_GEOM_FLAG_EXPLICIT_EMISSIVE_CANDIDATE) != 0u;
		// Power-heuristic (beta=2) MIS weight for this BSDF-sampled emissive hit.
		// Defaults to 1.0 for the primary hit / when explicit-emissive NEE is off
		// (no competing strategy). When NEE could have sampled this emitter, split
		// the energy against NEE so a glossy receiver under emissive-only light no
		// longer double-counts (its own NEE + its specular-bounce BSDF-hit). The
		// the diffuse path still integrates to albedo*Le: the power-heuristic weights
		// partition each emitter direction (w_nee + w_bsdf = 1) between NEE and the
		// BSDF-hit, so the two estimators jointly single-count it.
		float w_bsdf = 1.0;
		if (total_bounces > 0u && explicit_emissive_candidate && (rtgi_sampling_controls & RTGI_SAMPLING_EXPLICIT_EMISSIVE_BIT) != 0u) {
			float pdf_b = ps.pdf_bsdf; // carried from the previous (sampling) vertex
			float pdf_n = rt_emissive_nee_solid_angle_pdf_at_hit(h.geometry_idx, uint(gl_PrimitiveID), h.geometry_normal, gl_WorldRayDirectionEXT, gl_HitTEXT);
			if (pdf_n > 0.0) {
				w_bsdf = (pdf_b * pdf_b) / max(pdf_b * pdf_b + pdf_n * pdf_n, 1e-12);
			}
			// pdf_n == 0 (NEE could not have sampled this hit) -> w_bsdf stays 1.0.
		}
		vec3 raw_emissive_contribution = ps.throughput * m.emissive * w_bsdf;
		vec3 emissive_contribution = rt_clamp_path_contribution(raw_emissive_contribution, material_roughness, m.metalness, secondary_emissive, secondary_emissive);
		ps.radiance += emissive_contribution;
		if (total_bounces == 0u || get_diffuse_bounces(ps.packed_bounces_flags) == 0u) {
			ps.specular_radiance += emissive_contribution;
		}
	}

	// Russian Roulette path termination to stochastically terminate low-energy paths
	if (total_bounces >= 3u) {
		float q = max(0.05, min(0.95, rt_luminance(ps.throughput)));
		if (rand(ps.rng_state) > q) {
			ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
			path_pack(payload, ps);
			return;
		}
		ps.throughput /= q; // Rescale weight to remain unbiased
	}

	// Bounce limit check.
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
		path_pack(payload, ps);
		return;
	}

	// BRDF material setup.
	MaterialProperties brdf_mat;
	brdf_mat.baseColor = m.albedo;
	brdf_mat.metalness = m.metalness;
	brdf_mat.roughness = material_roughness;
	brdf_mat.dielectricF0 = specular_to_f0(m.specular);
	brdf_mat.emissive = m.emissive;
	brdf_mat.transmissivness = 0.0;
	brdf_mat.opacity = 1.0;

	vec3 specularF0 = baseColorToSpecularF0(brdf_mat.baseColor, brdf_mat.metalness, brdf_mat.dielectricF0);
	vec3 diffuseReflectance = baseColorToDiffuseReflectance(brdf_mat.baseColor, brdf_mat.metalness);

	// =================================================================
	// NEE: Next Event Estimation (direct light sampling)
	// =================================================================
	path_pack(payload, ps);

	uint rt_light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	if (rt_light_count > 0u && !hybrid_primary && (rtgi_sampling_controls & RTGI_SAMPLING_ANALYTIC_LIGHTS_BIT) != 0u) {
		bool is_indirect = (diffuse_bounces > 0u);
		uint receiver_layer_mask = geometries[h.geometry_idx].layer_mask;
		uint direct_source_key = 0u;
		uint direct_slot_source_key = 0u;
		float direct_slot_pdf = 0.0;
		RTDirectLighting direct_slot_light = rt_direct_lighting_zero();
		bool direct_slot_stochastic = false;
		float direct_slot_reservoir_m = 0.0;
		float direct_slot_reservoir_weight_sum = 0.0;
		float direct_slot_target = 0.0;
		bool direct_slot_temporal_accepted = false;
		bool direct_slot_spatial_accepted = false;
		uint direct_slot_temporal_reject = RT_SOURCE_REJECT_PREV_UV;
		uint direct_slot_spatial_reject = RT_SOURCE_REJECT_PREV_UV;
		uint direct_slot_visibility_failures = 0u;
		RTDirectLighting direct_light = lights_evaluate_direct_lighting_split(
				h.hit_pos, h.geometry_normal, N, V, brdf_mat, ps.rng_state, is_indirect, receiver_layer_mask, rt_light_count,
				direct_source_key, direct_slot_source_key, direct_slot_pdf, direct_slot_light, direct_slot_stochastic,
				direct_slot_reservoir_m, direct_slot_reservoir_weight_sum, direct_slot_target,
				direct_slot_temporal_accepted, direct_slot_spatial_accepted, direct_slot_temporal_reject,
				direct_slot_spatial_reject, direct_slot_visibility_failures);
		vec3 raw_direct_diffuse = ps.throughput * direct_light.diffuse;
		vec3 raw_direct_specular = ps.throughput * direct_light.specular;
		vec3 direct_diffuse = rt_clamp_path_contribution(raw_direct_diffuse, material_roughness, m.metalness, is_indirect, false);
		vec3 direct_specular = rt_clamp_path_contribution(raw_direct_specular, material_roughness, m.metalness, is_indirect, false);
		vec3 direct_total = reflections_only ? direct_specular : direct_diffuse + direct_specular;
		vec3 raw_direct_slot_diffuse = ps.throughput * direct_slot_light.diffuse;
		vec3 raw_direct_slot_specular = ps.throughput * direct_slot_light.specular;
		vec3 direct_slot_diffuse = reflections_only ? vec3(0.0) : rt_clamp_path_contribution(raw_direct_slot_diffuse, material_roughness, m.metalness, is_indirect, false);
		vec3 direct_slot_specular = rt_clamp_path_contribution(raw_direct_slot_specular, material_roughness, m.metalness, is_indirect, false);
		vec3 direct_slot_total = direct_slot_diffuse + direct_slot_specular;
		if (!is_indirect) {
			float direct_record_confidence = direct_slot_stochastic ? 1.0 : 0.5;
			rt_source_candidate_record(ivec2(gl_LaunchIDEXT.xy), 0.25, direct_record_confidence, direct_slot_pdf, direct_slot_total, 0.0, direct_slot_source_key);
			rt_source_direct_candidate_record(ivec2(gl_LaunchIDEXT.xy), direct_slot_source_key, direct_record_confidence, direct_slot_pdf, direct_slot_total, direct_slot_stochastic,
					direct_slot_reservoir_m, direct_slot_reservoir_weight_sum, direct_slot_target,
					direct_slot_temporal_accepted, direct_slot_spatial_accepted, direct_slot_temporal_reject,
					direct_slot_spatial_reject, direct_slot_visibility_failures);
		}
		if (total_bounces == 0u) {
			float direct_total_luma = rt_luminance(direct_total);
			float direct_specular_fraction = direct_total_luma > 1e-5 ? rt_luminance(direct_specular) / direct_total_luma : 0.0;
			ps.packed_bounces_flags = set_primary_specular_fraction(ps.packed_bounces_flags, direct_specular_fraction);
			ps.specular_radiance += direct_specular;
		} else if (get_diffuse_bounces(ps.packed_bounces_flags) == 0u) {
			ps.specular_radiance += direct_total;
		}
		ps.radiance += direct_total;
	}
	if (!hybrid_primary && (rtgi_sampling_controls & RTGI_SAMPLING_EXPLICIT_EMISSIVE_BIT) != 0u) {
		bool is_indirect = (diffuse_bounces > 0u);
		uint receiver_layer_mask = geometries[h.geometry_idx].layer_mask;
		float emissive_pdf = 0.0;
		float emissive_weight = 0.0;
		uint emissive_source_key = 0u;
		float emissive_distribution_debug = 0.0;
		float emissive_pdf_solid_angle = 0.0;
		vec3 emissive_L = vec3(0.0);
		RTDirectLighting emissive_light = lights_evaluate_explicit_emissive_candidate_split(
				h.hit_pos, h.geometry_normal, N, V, brdf_mat, ps.rng_state, receiver_layer_mask, emissive_pdf, emissive_weight, emissive_source_key, emissive_distribution_debug, emissive_pdf_solid_angle, emissive_L);
		// Emissive MIS (power heuristic, beta=2): weight this NEE sample against the
		// competing BSDF-sampling strategy that could have hit the same emitter.
		// NEE runs before the lobe is chosen (line ~1019), so derive P(specular)
		// inline from specularF0/diffuseReflectance using the same formula.
		if (emissive_pdf_solid_angle > 0.0) {
			float p_spec_nee = rt_lobe_specular_probability(specularF0, diffuseReflectance);
			// Evaluate the BSDF pdf at the actual sampling roughness (the variance-
			// reduction clamp the BSDF sampler uses), matching the BSDF-hit MIS side so
			// the weights stay consistent for smooth secondary bounces (1-spp pipeline).
			MaterialProperties nee_pdf_mat = brdf_mat;
			nee_pdf_mat.roughness = sampling_roughness;
			float pdf_b_L = rt_bsdf_sampling_pdf(emissive_L, N, V, nee_pdf_mat, p_spec_nee);
			float pdf_n_sa = emissive_pdf_solid_angle;
			float w_nee = (pdf_n_sa * pdf_n_sa) / max(pdf_b_L * pdf_b_L + pdf_n_sa * pdf_n_sa, 1e-12);
			emissive_light.diffuse *= w_nee;
			emissive_light.specular *= w_nee;
		}
		vec3 raw_emissive_diffuse = ps.throughput * emissive_light.diffuse;
		vec3 raw_emissive_specular = ps.throughput * emissive_light.specular;
		vec3 emissive_diffuse = rt_clamp_path_contribution(raw_emissive_diffuse, material_roughness, m.metalness, is_indirect, true);
		vec3 emissive_specular = rt_clamp_path_contribution(raw_emissive_specular, material_roughness, m.metalness, is_indirect, true);
		vec3 explicit_emissive_total = reflections_only ? emissive_specular : emissive_diffuse + emissive_specular;
		rt_source_candidate_record(ivec2(gl_LaunchIDEXT.xy), 0.50, emissive_distribution_debug, clamp(emissive_pdf * 64.0, 0.0, 1.0), explicit_emissive_total, 0.0, emissive_source_key);
		if (total_bounces == 0u) {
			ps.specular_radiance += emissive_specular;
		} else if (get_diffuse_bounces(ps.packed_bounces_flags) == 0u) {
			ps.specular_radiance += explicit_emissive_total;
		}
		ps.radiance += explicit_emissive_total;
	}

	// =================================================================
	// BRDF importance sampling for next bounce
	// =================================================================
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);
	bool rtgi_primary_rough_diffuse_sequence = rt_mode == RT_MODE_PATH_TRACED && RT_GET_SAMPLE_COUNT() == 1u && total_bounces == 0u && material_roughness > 0.52 && m.metalness < 0.20;

	int brdfType;
	// P(specular lobe) actually used for selection — fed to rt_bsdf_sampling_pdf
	// so the emissive BSDF-hit MIS weight (Part 3) matches this vertex's sampler.
	float next_p_spec = 0.0;
	if (reflections_only) {
		if (specularLum < 0.0001) {
			ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
			path_pack(payload, ps);
			return;
		}
		brdfType = SPECULAR_TYPE;
		next_p_spec = 1.0;
	} else if (diffuseLum < 0.0001) {
		brdfType = SPECULAR_TYPE;
		next_p_spec = 1.0;
	} else if (specularLum < 0.0001) {
		brdfType = DIFFUSE_TYPE;
		next_p_spec = 0.0;
	} else if (rtgi_primary_rough_diffuse_sequence) {
		// Treat rough dielectric single-sample GI as diffuse final gather work.
		// Direct specular is already evaluated above; stochastic specular
		// continuation here mostly creates high-throughput temporal outliers.
		brdfType = DIFFUSE_TYPE;
		next_p_spec = 0.0;
	} else {
		float brdfProbability = clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99);
		next_p_spec = brdfProbability;
		if (rand(ps.rng_state) < brdfProbability) {
			brdfType = SPECULAR_TYPE;
			ps.throughput /= brdfProbability;
		} else {
			brdfType = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdfProbability);
		}
	}

	vec2 u = rand2(ps.rng_state);
	if (rtgi_primary_rough_diffuse_sequence && brdfType == DIFFUSE_TYPE) {
		uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));
		u = get_rt_param(RT_PARAM_RTGI_RESOLUTION_SCALE) < 0.999 ?
				rtgi_primary_diffuse_screen_probe_sample(h, m, frame_index) :
				rtgi_primary_diffuse_temporal_sample(h, m, frame_index);
	}
	vec3 next_dir;
	vec3 brdf_weight;
	MaterialProperties sampling_brdf_mat = brdf_mat;
	sampling_brdf_mat.roughness = sampling_roughness;
	if (!evalIndirectCombinedBRDF(u, N, h.geometry_normal, V, sampling_brdf_mat, brdfType, next_dir, brdf_weight, vec4(0.0))) {
		// Two failure modes:
		//   1) Sample weight is zero (no contribution). Terminate.
		//   2) Sampled direction is below the geometry plane. Recover by
		//      mirroring across the geometry plane (see brdf_inc.glsl). The
		//      mirror is gated by RT_BELOW_HEMISPHERE_RECOVERY_ENABLED; when
		//      disabled, recovery returns false and we terminate the path.
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
			path_pack(payload, ps);
			return;
		}
		next_dir = recovered_dir;
	}

	ps.throughput *= brdf_weight;
	ps.throughput = rt_clamp_throughput(ps.throughput, sampling_roughness, m.metalness, total_bounces + 1u);

	if (brdfType == DIFFUSE_TYPE) {
		ps.packed_bounces_flags = inc_diffuse_bounce(ps.packed_bounces_flags);
	} else {
		if (total_bounces == 0u) {
			float primary_roughness = max(material_roughness, 0.02);
			float primary_metalness = clamp(m.metalness, 0.0, 1.0);
			float primary_specular_risk = max(1.0 - primary_roughness, primary_metalness);
			bool primary_specular_signal = primary_specular_risk > 0.35 && primary_roughness < mix(0.32, 0.14, primary_metalness);
			float current_specular_fraction = get_primary_specular_fraction(ps.packed_bounces_flags);
			ps.packed_bounces_flags = set_primary_specular_fraction(ps.packed_bounces_flags, max(current_specular_fraction, primary_specular_signal ? 0.85 : 0.0));
		}
		ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
	}

	// Carry the BSDF-sampling solid-angle pdf of next_dir for emissive MIS at the
	// next hit (Part 3). Evaluated at the actual sampling roughness (sampling_brdf_mat,
	// the variance-reduction clamp the sampler used), matching the NEE-side pdf so the
	// BSDF/NEE MIS weights for any emitter direction sum to 1.
	ps.pdf_bsdf = rt_bsdf_sampling_pdf(next_dir, N, V, sampling_brdf_mat, next_p_spec);

	// Hand the next ray back to raygen. PATH_TERMINATED_FLAG stays clear so
	// the raygen loop continues with the reconstructed origin and next_ray_dir.
	ps.hit_t = gl_HitTEXT;
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	path_pack(payload, ps);
}
