#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Bakes the per-(MultiMesh, surface) merged BLAS inputs in a single dispatch:
//   * world-space positions (per instance) into the merged vertex buffer,
//   * rotated normals/tangents (if the source mesh has them),
//   * per-vertex attributes replicated N times into the merged attribute buffer,
//   * (MODE_INDEXED only) the index buffer replicated and offset by inst * V.
//
// Thread idx maps to a vertex (idx < N*V) and, for MODE_INDEXED, an index
// (idx < N*I) -- both run in the same invocation, no barrier needed because
// they target disjoint destination buffers.
//
// All source and destination buffers are bound as descriptors so the draw graph
// tracks the compute read/write dependencies before the RT pass consumes BDAs.

layout(set = 0, binding = 0) restrict writeonly buffer DstVtxBuf { uint v[]; } dst_vtx;
layout(set = 0, binding = 1) restrict readonly buffer MMBuf { float v[]; } mm_buf;
layout(set = 0, binding = 2) restrict writeonly buffer DstAttrBuf { uint v[]; } dst_attr;
layout(set = 0, binding = 3) restrict readonly buffer SrcAttrBuf { uint v[]; } src_attr;
layout(set = 0, binding = 4) restrict readonly buffer SrcVtxBuf { uint v[]; } src_vtx;

#ifdef MODE_INDEXED
layout(set = 0, binding = 5) restrict readonly buffer SrcIdxBuf { uint v[]; } src_idx;
layout(set = 0, binding = 6) restrict writeonly buffer DstIdxBuf { uint v[]; } dst_idx;
#endif

layout(push_constant, std430) uniform PC {
	uint index_count; // indices per instance
	uint src_is_16bit; // 1 if source is packed uint16
	uint vertex_count; // V (per instance)
	uint instance_count; // N
	uint pos_stride_words; // position stride in float words (3 for uncompressed)
	uint src_tbn_base_words; // word offset of TBN section in source vertex buffer
	uint src_tbn_stride_words; // 1 = normal only, 2 = normal + tangent (uint32 each)
	uint dst_tbn_base_words; // N * V * 3 (word offset of TBN section in output)
	uint mm_stride; // mm transform stride in floats
	uint mm_offset; // mm current instance offset (motion vectors double-buffer)
	uint has_tbn; // 1 if mesh has normals/tangents, 0 otherwise
	uint attr_stride_words; // attribute stride in uint words (0 = mesh has no attribs)
} push;

vec2 vec3_to_oct(vec3 v) {
	float abs_sum = abs(v.x) + abs(v.y) + abs(v.z);
	v.xy /= abs_sum;
	if (v.z < 0.0) {
		v.xy = (1.0 - abs(v.yx)) * sign(v.xy);
	}
	return v.xy;
}

vec3 oct_to_vec3(vec2 e) {
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

vec3 orthonormalize_tangent(vec3 tangent, vec3 normal) {
	tangent -= normal * dot(normal, tangent);
	float len_sq = dot(tangent, tangent);
	if (len_sq > 1e-12) {
		return tangent * inversesqrt(len_sq);
	}
	vec3 axis = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	return normalize(cross(axis, normal));
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	uint total_v = push.vertex_count * push.instance_count;

	// --- Per-vertex section: bake position, replicate attributes, rotate TBN ---
	if (idx < total_v) {
		uint inst = idx / push.vertex_count;
		uint vtx = idx % push.vertex_count;

		uint pos_word = vtx * push.pos_stride_words;
		vec3 p = vec3(
				uintBitsToFloat(src_vtx.v[pos_word + 0u]),
				uintBitsToFloat(src_vtx.v[pos_word + 1u]),
				uintBitsToFloat(src_vtx.v[pos_word + 2u]));

		// MM buffer stores a row-major 3x4 transform per instance.
		// Read the three rows, then transpose so GLSL's column-major mat3
		// matches the source's rotation/scale and we can write `rot * v`.
		uint b = (push.mm_offset + inst) * push.mm_stride;
		vec4 row0 = vec4(mm_buf.v[b + 0u], mm_buf.v[b + 1u], mm_buf.v[b + 2u], mm_buf.v[b + 3u]);
		vec4 row1 = vec4(mm_buf.v[b + 4u], mm_buf.v[b + 5u], mm_buf.v[b + 6u], mm_buf.v[b + 7u]);
		vec4 row2 = vec4(mm_buf.v[b + 8u], mm_buf.v[b + 9u], mm_buf.v[b + 10u], mm_buf.v[b + 11u]);
		mat3 rot = transpose(mat3(row0.xyz, row1.xyz, row2.xyz));
		mat3 normal_xform = rot;
		if (abs(determinant(rot)) > 1e-8) {
			normal_xform = transpose(inverse(rot));
		}
		vec3 t = vec3(row0.w, row1.w, row2.w);

		vec3 wp = rot * p + t;
		uint pos_base = idx * 3u;
		dst_vtx.v[pos_base + 0u] = floatBitsToUint(wp.x);
		dst_vtx.v[pos_base + 1u] = floatBitsToUint(wp.y);
		dst_vtx.v[pos_base + 2u] = floatBitsToUint(wp.z);

		for (uint w = 0u; w < push.attr_stride_words; w++) {
			dst_attr.v[idx * push.attr_stride_words + w] = src_attr.v[vtx * push.attr_stride_words + w];
		}

		if (push.has_tbn != 0u) {
			uint tbn_src = push.src_tbn_base_words + vtx * push.src_tbn_stride_words;
			uint n_packed = src_vtx.v[tbn_src];

			vec2 n_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16u)) / 65535.0 * 2.0 - 1.0;
			vec3 rn = normalize(normal_xform * oct_to_vec3(n_oct));
			vec2 rn_oct = vec3_to_oct(rn);
			uint rn_packed = uint(rn_oct.x * 32767.5 + 32767.5) | (uint(rn_oct.y * 32767.5 + 32767.5) << 16u);

			uint tbn_dst = push.dst_tbn_base_words + idx * push.src_tbn_stride_words;
			dst_vtx.v[tbn_dst] = rn_packed;

			if (push.src_tbn_stride_words >= 2u) {
				// Tangent encodes the bitangent sign in sign(t_raw.y); preserve it.
				uint t_packed = src_vtx.v[tbn_src + 1u];
				vec2 t_raw = vec2(float(t_packed & 0xFFFFu), float(t_packed >> 16u)) / 65535.0 * 2.0 - 1.0;
				float bitan_sign = sign(t_raw.y);
				vec2 t_oct = vec2(t_raw.x, abs(t_raw.y) * 2.0 - 1.0);
				vec3 rt = orthonormalize_tangent(rot * oct_to_vec3(t_oct), rn);
				vec2 rt_oct = vec3_to_oct(rt);
				float t_raw_y = (abs(rt_oct.y) * 0.5 + 0.5) * bitan_sign;
				uint rt_packed = uint(rt_oct.x * 32767.5 + 32767.5) | (uint(t_raw_y * 32767.5 + 32767.5) << 16u);
				dst_vtx.v[tbn_dst + 1u] = rt_packed;
			}
		}
	}

#ifdef MODE_INDEXED
	// --- Per-index section: replicate and offset by inst * V ---
	uint total_i = push.index_count * push.instance_count;
	if (idx < total_i) {
		uint inst_i = idx / push.index_count;
		uint local_pos = idx % push.index_count;

		uint source_index;
		if (push.src_is_16bit != 0u) {
			uint byte_off = local_pos * 2u;
			uint word = src_idx.v[byte_off >> 2u];
			source_index = ((byte_off & 2u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
		} else {
			source_index = src_idx.v[local_pos];
		}

		dst_idx.v[idx] = source_index + inst_i * push.vertex_count;
	}
#endif
}
