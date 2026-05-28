// Shared definitions for raytracing shaders (raygen, miss, closest_hit, any_hit).
// Must be included before scene_data_inc.glsl since it defines MAX_VIEWS.
#include "../oct_inc.glsl"

#include "../oct_inc.glsl"

#define MAX_VIEWS 2

#ifndef PI
#define PI 3.141592653589f
#endif

// ============================================================================
// RT_PARAMS INDICES - Must match RT_PARAM_* in scene_shader_raytracing.h
// ============================================================================
// rt_params is a vec4[10] uniform buffer (40 floats total)
// Access: rt_params[idx >> 2][idx & 3] or get_rt_param(idx)
#define RT_PARAM_VIS_MODE 0 // rt_params[0].x - Debug visualization mode (0 = disabled)
#define RT_PARAM_SAMPLE_COUNT 1 // rt_params[0].y - Samples per pixel
#define RT_PARAM_MAX_BOUNCES 2 // rt_params[0].z - Maximum ray bounces
#define RT_PARAM_DENOISER 3 // rt_params[0].w - Denoiser selection
#define RT_PARAM_ENERGY 4 // rt_params[1].x - RTGI energy multiplier
#define RT_PARAM_RESERVED_5 5 // rt_params[1].y - Reserved
#define RT_PARAM_MODE 6 // rt_params[1].z - 0=Reflections RT Only, 1=Full Path Tracing
#define RT_PARAM_BACKGROUND_USES_SKY 7 // rt_params[1].w - Miss shader samples sky radiance
#define RT_PARAM_BACKGROUND_R 8 // rt_params[2].x - Linear fallback background color
#define RT_PARAM_BACKGROUND_G 9 // rt_params[2].y
#define RT_PARAM_BACKGROUND_B 10 // rt_params[2].z
#define RT_PARAM_RESERVED_11 11 // rt_params[2].w - Reserved
#define RT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED 11 // rt_params[2].w - RTGI diffuse cache toggle alias
#define RT_PARAM_OVERSCAN_HORIZONTAL 12 // rt_params[3].x - Horizontal RTGI overscan fraction
#define RT_PARAM_OVERSCAN_VERTICAL 13 // rt_params[3].y - Vertical RTGI overscan fraction
#define RT_PARAM_LIGHT_COUNT 14 // rt_params[3].z - Number of active lights in light buffer
#define RT_PARAM_FRAME_INDEX 15 // rt_params[3].w - Frame counter for temporal variation
#define RT_PARAM_DENOISER_STRENGTH 16 // rt_params[4].x - RTGI denoiser strength, included for history invalidation
#define RT_PARAM_DENOISER_HISTORY_WEIGHT 17 // rt_params[4].y - RTGI denoiser history weight
#define RT_PARAM_DENOISER_FIREFLY_SUPPRESSION 18 // rt_params[4].z - RTGI denoiser firefly suppression
#define RT_PARAM_DENOISER_DETAIL_PRESERVATION 19 // rt_params[4].w - RTGI denoiser detail preservation
#define RT_PARAM_RAY_FIREFLY_SUPPRESSION 20 // rt_params[5].x - Pre-denoiser path contribution clamp strength
#define RT_PARAM_RAY_MAX_RADIANCE 21 // rt_params[5].y - Pre-denoiser linear HDR luminance limit
#define RT_PARAM_DENOISER_SPLIT_SIGNALS 22 // rt_params[5].z - Separate diffuse/specular denoising
#define RT_PARAM_DENOISER_SPECULAR_HISTORY_WEIGHT 23 // rt_params[5].w - Specular RTGI denoiser history weight
#define RT_PARAM_DENOISER_SPECULAR_SPATIAL_STRENGTH 24 // rt_params[6].x - Specular spatial filtering strength
#define RT_PARAM_RTGI_SAMPLING_CONTROLS 25 // rt_params[6].y - Bitfield for analytic/emissive sampling controls
#define RT_PARAM_EMISSIVE_CANDIDATE_COUNT 26 // rt_params[6].z - Renderer-selected emissive candidate count
#define RT_PARAM_EMISSIVE_CANDIDATE_TOTAL_WEIGHT 27 // rt_params[6].w - Sum of emissive candidate selection weights
#define RT_PARAM_RTGI_STRC_ENABLED 28 // rt_params[7].x - World-space STRC/DDGI enabled
#define RT_PARAM_RTGI_STRC_STRENGTH 29 // rt_params[7].y - STRC contribution strength
#define RT_PARAM_RTGI_STRC_CASCADE_COUNT 30 // rt_params[7].z - Active camera-centered cascades
#define RT_PARAM_RTGI_STRC_GRID_SIZE 31 // rt_params[7].w - Probes per axis
#define RT_PARAM_RTGI_STRC_BASE_PROBE_SPACING 32 // rt_params[8].x - Cascade 0 spacing in world units
#define RT_PARAM_RTGI_STRC_RAYS_PER_FRAME 33 // rt_params[8].y - Probe update ray budget
#define RT_PARAM_RTGI_STRC_TEMPORAL_WEIGHT 34 // rt_params[8].z - Probe temporal accumulation weight
#define RT_PARAM_RTGI_BACKEND 35 // rt_params[8].w - Requested vendor backend
#define RT_PARAM_RTGI_STRC_STATIC_VISUAL_LAYERS 36 // rt_params[9].x - Static STRC visual layer mask
#define RT_PARAM_RTGI_STRC_DYNAMIC_VISUAL_LAYERS 37 // rt_params[9].y - Dynamic STRC visual layer mask

#define RTGI_SAMPLING_ANALYTIC_LIGHTS_BIT 1u
#define RTGI_SAMPLING_EXPLICIT_EMISSIVE_BIT 2u

#define RT_MODE_REFLECTIONS_RT_ONLY 0u
#define RT_MODE_FULL_PATH_TRACING 1u
#define RT_MODE_HYBRID RT_MODE_REFLECTIONS_RT_ONLY // Compatibility name.
#define RT_MODE_PATH_TRACED RT_MODE_FULL_PATH_TRACING // Compatibility name.

// ============================================================================
// PATHTRACING PAYLOAD (32 bytes, fp16/unorm-packed)
// ============================================================================
// Radiance (r) and throughput (T) interleaved as fp16 into 3 uints:
//   [0]=rg, [1]=bR, [2]=GB  (packHalf2x16 pairs).
// The next-bounce origin is not stored: raygen reconstructs it from the
// current ray and hit_t as `offset_ray_origin(ray_origin + ray_dir * hit_t,
// offset_normal)`. The offset normal and next direction are stored as
// 16-bit unorm octahedral pairs (effective angular error <~0.001 deg,
// below the 8-bit normal quantization baked into offset_ray_origin).
// Use PathState + path_pack/path_unpack for fp32 working copies.

const float RT_FP16_MAX = 65504.0;

float rt_luminance(vec3 rgb) {
	return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

vec3 sanitize_payload_vec3(vec3 v) {
	v = mix(v, vec3(0.0), isnan(v));
	v = mix(v, vec3(0.0), bvec3(isinf(v.x) && v.x < 0.0, isinf(v.y) && v.y < 0.0, isinf(v.z) && v.z < 0.0));
	v = mix(v, vec3(RT_FP16_MAX), bvec3(isinf(v.x) && v.x > 0.0, isinf(v.y) && v.y > 0.0, isinf(v.z) && v.z > 0.0));
	return clamp(v, vec3(0.0), vec3(RT_FP16_MAX));
}

float rt_hash_2d(vec2 p) {
	return fract(1.0e4 * sin(17.0 * p.x + 0.1 * p.y) *
			(0.1 + abs(sin(13.0 * p.y + p.x))));
}

float rt_hash_3d(vec3 p) {
	return rt_hash_2d(vec2(rt_hash_2d(p.xy), p.z));
}

float rt_alpha_hash_threshold(vec3 object_pos, float hash_scale) {
	float scale = max(hash_scale, 0.0001);
	return clamp(rt_hash_3d(floor(object_pos * scale * 64.0)), 0.00001, 1.0);
}

struct PathPayload {
	uint packed_rt[3]; // 12 bytes - radiance+throughput interleaved as fp16
	uint packed_specular[2]; // 8 bytes - specular radiance as fp16 rgb
	uint packed_bounces_flags; //  4 bytes - [flags:8][primary specular fraction:8][diffuse:8][total:8]
	uint rng_state; //  4 bytes - RNG state for PCG
	float hit_t; //  4 bytes - ray distance, used by raygen to rebuild origin
	uint oct_offset_nrm; //  4 bytes - packUnorm2x16(vec3_to_oct(offset normal))
	uint oct_next_dir; //  4 bytes - packUnorm2x16(vec3_to_oct(next direction))
};

/// Unpacked fp32 working copy of the payload.
struct PathState {
	vec3 radiance;
	vec3 specular_radiance;
	vec3 throughput;
	uint packed_bounces_flags;
	uint rng_state;
	float hit_t;
	vec3 offset_normal;
	vec3 next_ray_dir;
};

PathState path_unpack(PathPayload p) {
	PathState s;
	vec2 rg = unpackHalf2x16(p.packed_rt[0]);
	vec2 bR = unpackHalf2x16(p.packed_rt[1]);
	vec2 GB = unpackHalf2x16(p.packed_rt[2]);
	vec2 specular_rg = unpackHalf2x16(p.packed_specular[0]);
	vec2 specular_b = unpackHalf2x16(p.packed_specular[1]);
	s.radiance = sanitize_payload_vec3(vec3(rg.x, rg.y, bR.x));
	s.specular_radiance = sanitize_payload_vec3(vec3(specular_rg.x, specular_rg.y, specular_b.x));
	s.throughput = sanitize_payload_vec3(vec3(bR.y, GB.x, GB.y));
	s.packed_bounces_flags = p.packed_bounces_flags;
	s.rng_state = p.rng_state;
	s.hit_t = p.hit_t;
	s.offset_normal = oct_to_vec3(unpackUnorm2x16(p.oct_offset_nrm) * 2.0 - 1.0);
	s.next_ray_dir = oct_to_vec3(unpackUnorm2x16(p.oct_next_dir) * 2.0 - 1.0);
	return s;
}

void path_pack(inout PathPayload p, PathState s) {
	vec3 r = sanitize_payload_vec3(s.radiance);
	vec3 sr = sanitize_payload_vec3(s.specular_radiance);
	vec3 t = sanitize_payload_vec3(s.throughput);
	p.packed_rt[0] = packHalf2x16(vec2(r.r, r.g));
	p.packed_rt[1] = packHalf2x16(vec2(r.b, t.r));
	p.packed_rt[2] = packHalf2x16(vec2(t.g, t.b));
	p.packed_specular[0] = packHalf2x16(vec2(sr.r, sr.g));
	p.packed_specular[1] = packHalf2x16(vec2(sr.b, 0.0));
	p.packed_bounces_flags = s.packed_bounces_flags;
	p.rng_state = s.rng_state;
	p.hit_t = s.hit_t;
	p.oct_offset_nrm = packUnorm2x16(vec3_to_oct(s.offset_normal));
	p.oct_next_dir = packUnorm2x16(vec3_to_oct(s.next_ray_dir));
}

// Bounce count helpers (bits 0-7: total, bits 8-15: diffuse)
uint get_total_bounces(uint packed) {
	return packed & 0xFFu;
}
uint get_diffuse_bounces(uint packed) {
	return (packed >> 8u) & 0xFFu;
}
uint pack_bounces(uint total, uint diffuse) {
	return total | (diffuse << 8u);
}
uint inc_total_bounce(uint packed) {
	return packed + 1u;
}
uint inc_diffuse_bounce(uint packed) {
	return packed + 0x101u;
} // +1 to both total and diffuse

const uint PRIMARY_SPECULAR_FRACTION_MASK = 0x00FF0000u;
uint set_primary_specular_fraction(uint packed, float fraction) {
	uint packed_fraction = uint(clamp(fraction, 0.0, 1.0) * 255.0 + 0.5);
	return (packed & ~PRIMARY_SPECULAR_FRACTION_MASK) | (packed_fraction << 16u);
}
float get_primary_specular_fraction(uint packed) {
	return float((packed & PRIMARY_SPECULAR_FRACTION_MASK) >> 16u) * (1.0 / 255.0);
}

// Sample 0 flag (bit 24) - only write DLSS RR outputs on first sample
const uint SAMPLE_ZERO_FLAG = (1u << 24);
uint set_sample_zero(uint packed) {
	return packed | SAMPLE_ZERO_FLAG;
}
bool is_sample_zero(uint packed) {
	return (packed & SAMPLE_ZERO_FLAG) != 0u;
}

// Shadow ray flag (bit 25) - indicates this ray is a shadow/occlusion test
const uint SHADOW_RAY_FLAG = (1u << 25);
uint set_shadow_ray(uint packed) {
	return packed | SHADOW_RAY_FLAG;
}
bool is_shadow_ray(uint packed) {
	return (packed & SHADOW_RAY_FLAG) != 0u;
}

const uint PATH_TERMINATED_FLAG = (1u << 26);
uint set_path_terminated(uint packed) {
	return packed | PATH_TERMINATED_FLAG;
}
uint clear_path_terminated(uint packed) {
	return packed & ~PATH_TERMINATED_FLAG;
}
bool is_path_terminated(uint packed) {
	return (packed & PATH_TERMINATED_FLAG) != 0u;
}

const uint PRIMARY_RASTER_GI_OWNER_FLAG = (1u << 27);
uint set_primary_raster_gi_owner(uint packed) {
	return packed | PRIMARY_RASTER_GI_OWNER_FLAG;
}
bool has_primary_raster_gi_owner(uint packed) {
	return (packed & PRIMARY_RASTER_GI_OWNER_FLAG) != 0u;
}

const uint STRC_DYNAMIC_HIT_FLAG = (1u << 28);
uint set_strc_dynamic_hit(uint packed) {
	return packed | STRC_DYNAMIC_HIT_FLAG;
}
bool has_strc_dynamic_hit(uint packed) {
	return (packed & STRC_DYNAMIC_HIT_FLAG) != 0u;
}

// Bounce limits
#define MAX_DIFFUSE_BOUNCES 2u
#define MAX_DENOISER_SPECULAR_HIT_THRESHOLD 0.25

// ============================================================================
// CONSTANTS
// ============================================================================
const uint OFFSET_NONE = 0xFFFFFFFFu;
const uint RT_INSTANCE_MASK_VISIBLE = 1u;
const uint RT_INSTANCE_MASK_SHADOW = 2u;
const uint FLAG_COMPRESSED = 1u;
const uint FLAG_PROCEDURAL = 2u;
// Set when the BLAS uses a per-frame-deformed vertex buffer.
const uint FLAG_DEFORMED = 4u;
// Set when this TLAS entry was not present in the previous RT history set.
const uint FLAG_HISTORY_INVALID = 8u;
// Set when the attribute buffer is still in the compressed surface layout.
const uint FLAG_COMPRESSED_ATTRIBUTES = 16u;
// Fold gl_PrimitiveID into the guide history ID for merged BLASes.
const uint FLAG_PRIMITIVE_HISTORY_ID = 32u;

// ============================================================================
// RANDOM NUMBER GENERATION - PCG (Permuted Congruential Generator)
// ============================================================================

/// PCG random number generator - improved XSH-RR variant with better avalanche
uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint rng_mix(uint value) {
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return value;
}

/// Initialize RNG state with independent pixel/frame/sample mixing.
uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	uint seed = rng_mix(pixel.x);
	seed ^= rng_mix(pixel.y + 0x9e3779b9u);
	seed ^= rng_mix(frame + 0x85ebca6bu);
	seed ^= rng_mix(sample_idx + 0xc2b2ae35u);
	return pcg_hash(seed);
}

/// Random float in [0, 1)
float rand(inout uint state) {
	state = pcg_hash(state);
	return float(state) / 4294967296.0;
}

/// Random vec2 in [0, 1)
vec2 rand2(inout uint state) {
	return vec2(rand(state), rand(state));
}

// ============================================================================
// RAY ORIGIN OFFSET (prevents self-intersection)
// Wachter-Binder method: offsets in integer float representation so the
// displacement scales naturally with position magnitude.
// ============================================================================
float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(
			_offset_component(p.x, n.x, of.x),
			_offset_component(p.y, n.y, of.y),
			_offset_component(p.z, n.z, of.z));
}
