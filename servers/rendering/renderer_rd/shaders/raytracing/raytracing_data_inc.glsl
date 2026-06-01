// Shared data structures for raytracing shaders.
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, raytracing_inc.glsl (for OFFSET_NONE/FLAG_*)

// ============================================================================
// BUFFER REFERENCES
// ============================================================================
layout(buffer_reference, std430) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430) readonly buffer Uint32Buffer {
	uint v[];
};

// ============================================================================
// GEOMETRY DATA (matches C++ RT_GeometryData, 128 bytes)
// ============================================================================
struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed; // fp16 x, fp16 y
	uint index_format;
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;

	// Byte offset of the vertex color attribute inside `attribute_stride`,
	// or OFFSET_NONE if the mesh has no vertex colors.
	uint color_byte_offset;

	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;

	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;

	uint layer_mask;
	uint history_id;
	uint uv2_byte_offset;
	uint _pad[2];
};

const uint RT_GEOM_FLAG_RASTER_GI_LIGHTMAP = 64u;
const uint RT_GEOM_FLAG_RASTER_GI_LIGHTMAP_CAPTURE = 128u;
const uint RT_GEOM_FLAG_RASTER_GI_VOXELGI = 256u;
const uint RT_GEOM_FLAG_RASTER_GI_SDFGI = 512u;
const uint RT_GEOM_FLAG_EXPLICIT_EMISSIVE_CANDIDATE = 1024u;
// Emitter whose material has cull_mode == DISABLED; NEE flips the winding normal
// to whichever face faces the receiver instead of rejecting back-winding triangles.
const uint RT_GEOM_FLAG_TWO_SIDED = 2048u;
const uint RT_GEOM_FLAG_RASTER_GI_OWNER =
		RT_GEOM_FLAG_RASTER_GI_LIGHTMAP |
		RT_GEOM_FLAG_RASTER_GI_LIGHTMAP_CAPTURE |
		RT_GEOM_FLAG_RASTER_GI_VOXELGI |
		RT_GEOM_FLAG_RASTER_GI_SDFGI;

void get_aabb_compression_xforms(GeometryData geom, out mat4 aabb_xform, out mat4 inv_aabb_xform) {
	if ((geom.flags & FLAG_COMPRESSED) == 0u) {
		aabb_xform = mat4(1.0);
		inv_aabb_xform = mat4(1.0);
		return;
	}
	vec3 aabb_sz = max(vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z), vec3(0.0001));
	vec3 aabb_po = vec3(geom.aabb_pos_x, geom.aabb_pos_y, geom.aabb_pos_z);
	aabb_xform = mat4(
			vec4(aabb_sz.x, 0.0, 0.0, 0.0),
			vec4(0.0, aabb_sz.y, 0.0, 0.0),
			vec4(0.0, 0.0, aabb_sz.z, 0.0),
			vec4(aabb_po, 1.0));
	vec3 inv_sz = 1.0 / aabb_sz;
	inv_aabb_xform = mat4(
			vec4(inv_sz.x, 0.0, 0.0, 0.0),
			vec4(0.0, inv_sz.y, 0.0, 0.0),
			vec4(0.0, 0.0, inv_sz.z, 0.0),
			vec4(-aabb_po * inv_sz, 1.0));
}

// ============================================================================
// PER-INSTANCE MOTION DATA (matches C++ RT_InstanceMotionData, 48 bytes)
// ============================================================================
struct InstanceMotionData {
	float prev_xform[12]; // Previous object-to-world (mat3x4, transposed 3x4)
};

// ============================================================================
// MATERIAL DATA (matches C++ layout, 112 bytes)
// ============================================================================
struct MaterialData {
	uint albedo_texture_idx;
	uint normal_texture_idx;
	uint orm_texture_idx;
	uint emission_texture_idx;

	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;

	float metallic;
	float roughness;
	float ao_strength;
	uint flags; // Bit 0: has_normal_map, Bit 1: has_emission

	vec2 uv1_scale; // UV1 scale (default 1,1)
	vec2 uv1_offset; // UV1 offset (default 0,0)

	float normal_map_depth; // Normal map strength (default 1.0)
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.
	float alpha_scissor_threshold;
	float alpha_hash_scale;
	uint metallic_texture_idx;
	uint _pad0;
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none)
};

#define RT_MAT_FLAG_HAS_NORMAL_MAP 1u
#define RT_MAT_FLAG_HAS_EMISSION_TEX 2u
#define RT_MAT_FLAG_POINT_FILTER 4u
#define RT_MAT_FLAG_CUSTOM_SHADER 8u
#define RT_MAT_FLAG_ALPHA_HASH 16u
#define RT_MAT_FLAG_CUSTOM_ALPHA_CLIP 32u
#define RT_MAT_FLAG_ALPHA_TEST 64u
#define RT_MAT_FLAG_VERTEX_COLOR_ALBEDO 128u
#define RT_MAT_FLAG_VERTEX_COLOR_SRGB 256u
#define RT_MAT_FLAG_ROUGHNESS_TEXTURE 512u
#define RT_MAT_FLAG_ROUGHNESS_CHANNEL_SHIFT 10u
#define RT_MAT_FLAG_REPEAT_DISABLED 8192u
#define RT_MAT_FLAG_ORM_TEXTURE 16384u
#define RT_MAT_FLAG_METALLIC_TEXTURE 32768u
#define RT_MAT_FLAG_METALLIC_CHANNEL_SHIFT 16u

float rt_material_texture_channel(vec4 texel, uint channel) {
	if (channel == 1u) {
		return texel.g;
	} else if (channel == 2u) {
		return texel.b;
	} else if (channel == 3u) {
		return texel.a;
	} else if (channel == 4u) {
		return dot(texel.rgb, vec3(0.333333));
	}
	return texel.r;
}

vec4 rt_material_vertex_color(MaterialData mat, vec4 color) {
	if ((mat.flags & RT_MAT_FLAG_VERTEX_COLOR_ALBEDO) == 0u) {
		return vec4(1.0);
	}

	vec4 vertex_color = color;
	if ((mat.flags & RT_MAT_FLAG_VERTEX_COLOR_SRGB) != 0u) {
		vec3 low = vertex_color.rgb * (1.0 / 12.92);
		vec3 high = pow((vertex_color.rgb + vec3(0.055)) * (1.0 / 1.055), vec3(2.4));
		vertex_color.rgb = mix(high, low, lessThan(vertex_color.rgb, vec3(0.04045)));
	}
	return vertex_color;
}
