/**************************************************************************/
/*  render_raytracing.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

#define RB_TEX_RAYTRACING SNAME("raytracing")
#define RB_TEX_RT_RECONSTRUCTED SNAME("rt_reconstructed")
#define RB_TEX_RT_RECONSTRUCTED_DIFFUSE SNAME("rt_reconstructed_diffuse")
#define RB_TEX_RT_RECONSTRUCTED_SPECULAR SNAME("rt_reconstructed_specular")
#define RB_TEX_RT_RECONSTRUCTED_TEMP SNAME("rt_reconstructed_temp")
#define RB_TEX_RT_RECONSTRUCTED_REACTIVITY SNAME("rt_reconstructed_reactivity")
#define RB_TEX_RT_RECONSTRUCTED_SIGNAL_CONFIDENCE SNAME("rt_reconstructed_signal_confidence")
#define RB_TEX_RT_RECONSTRUCTED_GUIDE_MISMATCH SNAME("rt_reconstructed_guide_mismatch")
#define RB_TEX_RT_RECONSTRUCTED_FILL_SOURCE SNAME("rt_reconstructed_fill_source")
#define RB_TEX_RT_RECONSTRUCTED_MOMENTS SNAME("rt_reconstructed_moments")
#define RB_TEX_RT_RECONSTRUCTED_MOMENTS_PREV SNAME("rt_reconstructed_moments_prev")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_META SNAME("rt_reconstructed_history_meta")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_META_PREV SNAME("rt_reconstructed_history_meta_prev")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_VALIDITY SNAME("rt_reconstructed_history_validity")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_VALIDITY_PREV SNAME("rt_reconstructed_history_validity_prev")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_ID SNAME("rt_reconstructed_history_id")
#define RB_TEX_RT_RECONSTRUCTED_HISTORY_ID_PREV SNAME("rt_reconstructed_history_id_prev")
#define RB_TEX_RT_GUIDE_ALBEDO SNAME("rt_guide_albedo")
#define RB_TEX_RT_GUIDE_NORMAL SNAME("rt_guide_normal")
#define RB_TEX_RT_GUIDE_ORM SNAME("rt_guide_orm")
#define RB_TEX_RT_GUIDE_EMISSION SNAME("rt_guide_emission")
#define RB_TEX_RT_GUIDE_VIEWZ SNAME("rt_guide_viewz")
#define RB_TEX_RT_DIFFUSE_RADIANCE SNAME("rt_diffuse_radiance")
#define RB_TEX_RT_SPECULAR_RADIANCE SNAME("rt_specular_radiance")
#define RB_TEX_RT_SPECULAR_GUIDE SNAME("rt_specular_guide")
#define RB_TEX_RT_SPECULAR_REPROJECTION SNAME("rt_specular_reprojection")
#define RB_TEX_RT_SPECULAR_REFLECTION_DIRECTION SNAME("rt_specular_reflection_direction")
#define RB_TEX_RT_DEPTH SNAME("rt_depth")
#define RB_TEX_RT_DEPTH_ATTACHMENT SNAME("rt_depth_attachment")
#define RB_TEX_RT_VELOCITY SNAME("rt_velocity")
#define RB_TEX_RT_HISTORY_VALIDITY SNAME("rt_history_validity")
#define RB_TEX_RT_HISTORY_VALIDITY_PREV SNAME("rt_history_validity_prev")
#define RB_TEX_RT_HISTORY_ID SNAME("rt_history_id")
#define RB_TEX_RT_HISTORY_ID_PREV SNAME("rt_history_id_prev")
#define RB_TEX_RT_TAA_HISTORY_VALIDITY_PREV SNAME("rt_taa_history_validity_prev")
#define RB_TEX_RT_TAA_HISTORY_ID_PREV SNAME("rt_taa_history_id_prev")
#define RB_TEX_RT_HYBRID_TAA_HISTORY_VALIDITY_PREV SNAME("rt_hybrid_taa_history_validity_prev")
#define RB_TEX_RT_HYBRID_TAA_HISTORY_ID_PREV SNAME("rt_hybrid_taa_history_id_prev")
#define RB_TEX_RT_TAA_REACTIVITY SNAME("rt_taa_reactivity")
#define RB_TEX_RT_NORMAL_ROUGHNESS SNAME("rt_normal_roughness")
#define RB_TEX_RT_SOURCE_NORMAL_ROUGHNESS_PREV SNAME("rt_source_normal_roughness_prev")
#define RB_TEX_RT_ALBEDO_METALNESS SNAME("rt_albedo_metalness")
#define RB_TEX_RT_VIEWZ_HITDIST SNAME("rt_viewz_hitdist")
#define RB_TEX_RT_SOURCE_VIEWZ_HITDIST_PREV SNAME("rt_source_viewz_hitdist_prev")
#define RB_TEX_RT_SOURCE_CANDIDATE SNAME("rt_source_candidate")
#define RB_TEX_RT_SOURCE_CANDIDATE_PREV SNAME("rt_source_candidate_prev")
#define RB_TEX_RT_SOURCE_CANDIDATE_KEY SNAME("rt_source_candidate_key")
#define RB_TEX_RT_SOURCE_CANDIDATE_KEY_PREV SNAME("rt_source_candidate_key_prev")
#define RB_TEX_RT_SOURCE_DIRECT_CANDIDATE SNAME("rt_source_direct_candidate")
#define RB_TEX_RT_SOURCE_DIRECT_CANDIDATE_PREV SNAME("rt_source_direct_candidate_prev")
#define RB_TEX_RT_SOURCE_DIRECT_CANDIDATE_KEY SNAME("rt_source_direct_candidate_key")
#define RB_TEX_RT_SOURCE_DIRECT_CANDIDATE_KEY_PREV SNAME("rt_source_direct_candidate_key_prev")
#define RB_TEX_RT_SOURCE_DIRECT_RESERVOIR SNAME("rt_source_direct_reservoir")
#define RB_TEX_RT_SOURCE_DIRECT_RESERVOIR_PREV SNAME("rt_source_direct_reservoir_prev")
#define RB_TEX_RT_SOURCE_DIRECT_RESERVOIR_LIGHTING SNAME("rt_source_direct_reservoir_lighting")
#define RB_TEX_RT_SOURCE_DIRECT_RESERVOIR_LIGHTING_PREV SNAME("rt_source_direct_reservoir_lighting_prev")
#define RB_TEX_RT_SOURCE_HISTORY SNAME("rt_source_history")
#define RB_TEX_RT_SOURCE_TEMPORAL_DELTA SNAME("rt_source_temporal_delta")
#define RB_TEX_RT_SOURCE_REJECTION SNAME("rt_source_rejection")
// GI-aware reactive mask ("poor-man's Ray Reconstruction"): the RTGI resolve composite
// writes 1 - confidence here, so the temporal upscaler (FSR2 reactive / XeSS responsive
// pixel mask) trusts the current frame where GI just disoccluded or is low-confidence.
// Internal-size R8_UNORM, allocated + written ONLY when the Reactive denoiser is selected.
#define RB_TEX_RTGI_REACTIVE SNAME("rtgi_reactive")

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;
class RenderRaytracing;
class SceneShaderRaytracing;

// Must match GLSL GeometryData (std430, 128 bytes).
struct alignas(16) RT_GeometryData {
	uint64_t vertex_buffer_address;
	uint64_t attribute_buffer_address;
	uint64_t index_buffer_address;
	uint32_t vertex_count;
	uint32_t position_stride;
	uint32_t normal_byte_offset;
	uint32_t normal_stride;
	uint32_t tangent_byte_offset;
	uint32_t tangent_stride;
	uint32_t attribute_stride;
	uint32_t uv_byte_offset;
	uint32_t uv_scale_packed;
	uint32_t index_format;
	uint32_t primitive_count;
	uint32_t flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;
	uint32_t color_byte_offset;
	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;
	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint32_t prev_vertex_buffer_address_lo;
	uint32_t prev_vertex_buffer_address_hi;
	uint32_t layer_mask;
	uint32_t history_id;
	uint32_t uv2_byte_offset;
	uint32_t _pad[2];
};
static_assert(sizeof(RT_GeometryData) == 128, "RT_GeometryData must be 128 bytes for std430");

/// Per-instance motion data for velocity computation (matches GLSL InstanceMotionData, 48 bytes).
struct RT_InstanceMotionData {
	float prev_object_to_world[12]; // Previous object-to-world (mat3x4, transposed 3x4).
};
static_assert(sizeof(RT_InstanceMotionData) == 48, "RT_InstanceMotionData must be 48 bytes");

// Must match GLSL MaterialData (std430, 112 bytes).
struct alignas(16) RT_MaterialData {
	uint32_t albedo_texture_idx;
	uint32_t normal_texture_idx;
	uint32_t orm_texture_idx;
	uint32_t emission_texture_idx;
	float albedo_color[4];
	float emission_color[3];
	float emission_strength;
	float metallic;
	float roughness;
	float ao_strength;
	uint32_t flags;
	float uv1_scale[2];
	float uv1_offset[2];
	float normal_map_depth; // Strength [0..N], default 1.0 (not Z-depth).
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.
	float alpha_scissor_threshold;
	float alpha_hash_scale;
	uint32_t metallic_texture_idx;
	uint32_t _pad0;
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none).
};
static_assert(sizeof(RT_MaterialData) == 112, "RT_MaterialData must be 112 bytes for std430");

// Must match GLSL RTEmissivePrimitiveDistribution (std430, 16 bytes).
struct alignas(16) RT_EmissivePrimitiveDistribution {
	uint32_t primitive_id;
	float cumulative_weight;
	float area;
	float _pad;
};
static_assert(sizeof(RT_EmissivePrimitiveDistribution) == 16, "RT_EmissivePrimitiveDistribution must be 16 bytes for std430");

// Must match GLSL RTEmissiveCandidate (std430, 80 bytes).
struct alignas(16) RT_EmissiveCandidate {
	float object_to_world[12];
	uint32_t geometry_index;
	uint32_t flags;
	float selection_weight;
	float primitive_weight_sum;
	uint32_t primitive_offset;
	uint32_t primitive_count;
	uint32_t _pad[2];
};
static_assert(sizeof(RT_EmissiveCandidate) == 80, "RT_EmissiveCandidate must be 80 bytes for std430");

// Light types for raytracing (matches GLSL RT_LIGHT_TYPE_* defines).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_AREA = 2,
	RT_LIGHT_TYPE_SPOT = 3,
};

enum RTLightFlag : uint32_t {
	RT_LIGHT_FLAG_SHADOW = 1 << 0,
	RT_LIGHT_FLAG_AREA_TWO_SIDED = 1 << 1, // reserved; one-sided is the default match for the raster.
};

// Must match GLSL RTLightData (std430, 128 bytes).
struct alignas(16) RT_LightData {
	float position[3]; // World position (omni/spot) or direction (directional, normalized).
	uint32_t type;
	float emission[3];
	float radius;
	float attenuation;
	float inv_max_range; // 1.0/range, or -1.0 for infinite.
	float max_range_squared; // range*range, or 0.0 for infinite.
	float specular_amount;
	float indirect_energy;
	float inv_spot_attenuation;
	float cos_spot_angle;
	uint32_t flags;
	float spot_direction[3];
	uint32_t cull_mask;
	uint32_t shadow_caster_mask;
	float shadow_opacity;
	float shadow_max_distance;
	uint32_t source_id;
	// Area-light texture (RT_LIGHT_TYPE_AREA only; area_atlas_idx == 0 means untextured).
	// For RT_LIGHT_TYPE_AREA the unused spot fields carry the rectangle half-edge vectors:
	// spot_direction = ex, and (radius, inv_spot_attenuation, cos_spot_angle) = ey.
	float area_atlas_rect[4]; // xy = atlas offset, zw = atlas scale (normalized).
	uint32_t area_atlas_idx; // bindless index of the area-light atlas, 0 = none.
	float area_max_mip; // max mip level for the atlas fetch.
	uint32_t area_pad0;
	uint32_t area_pad1;
};
static_assert(sizeof(RT_LightData) == 128, "RT_LightData must be 128 bytes for std430");

enum {
	RT_LIGHTS_MAX = 64,
	RT_LIGHTS_FRUSTUM_BUDGET = 48,
	RT_LIGHTS_INDIRECT_BUDGET = RT_LIGHTS_MAX - RT_LIGHTS_FRUSTUM_BUDGET,
};

enum {
	RT_INSTANCE_MASK_VISIBLE = 1u,
	RT_INSTANCE_MASK_SHADOW = 2u,
};

enum {
	RT_OFFSET_NONE = 0xFFFFFFFFu,
	RT_CACHE_CHUNK_SIZE = 256,
	RT_CACHE_CHUNK_SHIFT = 8,
	RT_CACHE_CHUNK_MASK = 255,
};

// Material flags for RT (matches GLSL mat_flags bit layout).
enum {
	RT_MAT_FLAG_HAS_NORMAL_MAP = 1u,
	RT_MAT_FLAG_HAS_EMISSION_TEX = 2u,
	RT_MAT_FLAG_POINT_FILTER = 4u,
	RT_MAT_FLAG_CUSTOM_SHADER = 8u,
	RT_MAT_FLAG_ALPHA_HASH = 16u,
	RT_MAT_FLAG_CUSTOM_ALPHA_CLIP = 32u,
	RT_MAT_FLAG_ALPHA_TEST = 64u,
	RT_MAT_FLAG_VERTEX_COLOR_ALBEDO = 128u,
	RT_MAT_FLAG_VERTEX_COLOR_SRGB = 256u,
	RT_MAT_FLAG_ROUGHNESS_TEXTURE = 512u,
	RT_MAT_FLAG_ROUGHNESS_CHANNEL_SHIFT = 10u,
	RT_MAT_FLAG_REPEAT_DISABLED = 8192u,
	RT_MAT_FLAG_ORM_TEXTURE = 16384u,
	RT_MAT_FLAG_METALLIC_TEXTURE = 32768u,
	RT_MAT_FLAG_METALLIC_CHANNEL_SHIFT = 16u,
};

// Index format for RT geometry (matches GLSL fetch_indices).
enum {
	RT_INDEX_FORMAT_UINT16 = 0,
	RT_INDEX_FORMAT_UINT32 = 1,
	RT_INDEX_FORMAT_NONE = 2,
};

enum {
	RT_GEOM_FLAG_COMPRESSED = 1u,
	RT_GEOM_FLAG_PROCEDURAL = 2u,
	// Set when the BLAS uses a per-frame-deformed vertex buffer.
	RT_GEOM_FLAG_DEFORMED = 4u,
	// Set on TLAS entries that were not part of the previous RT history set.
	RT_GEOM_FLAG_HISTORY_INVALID = 8u,
	// Attribute buffer is still in the compressed surface layout.
	RT_GEOM_FLAG_COMPRESSED_ATTRIBUTES = 16u,
	// Fold gl_PrimitiveID into the guide history ID for merged BLASes.
	RT_GEOM_FLAG_PRIMITIVE_HISTORY_ID = 32u,
	RT_GEOM_FLAG_EXPLICIT_EMISSIVE_CANDIDATE = 1024u,
	// Emitter whose material has cull_mode == DISABLED. NEE flips the geometric
	// normal toward the receiver instead of rejecting back-winding triangles.
	RT_GEOM_FLAG_TWO_SIDED = 2048u,
	// Primary hit belongs to a raster GI owner in Simple RT. The shader uses
	// these to suppress diffuse RTGI while keeping RT specular/reflections.
	RT_GEOM_FLAG_RASTER_GI_LIGHTMAP = 64u,
	RT_GEOM_FLAG_RASTER_GI_LIGHTMAP_CAPTURE = 128u,
	RT_GEOM_FLAG_RASTER_GI_VOXELGI = 256u,
	RT_GEOM_FLAG_RASTER_GI_SDFGI = 512u,
};

/// Per-instance state for procedural RT geometry. Heap-allocated, only exists for procedural instances.
struct RTProceduralState {
	AABB base_aabb;
	AABB culling_aabb;
	PackedFloat32Array aabb_data; // N * 6 floats (min/max per AABB). Empty = single AABB.
	bool expose_bounds = false;
	bool enabled = false;
	bool dirty = true;
	RID blas;
	RID gpu_buffer;
	uint32_t gpu_buffer_capacity = 0; // Bytes, grow-only.
	uint64_t gpu_buffer_address = 0; // BDA (0 = not exposed).
	uint32_t aabb_count = 0;
};

struct RTSurfaceData {
	RID blas;
	RT_GeometryData geometry = {};
	Transform3D aabb_transform;
	RID vertex_buffer_dependency;
	RID attribute_buffer_dependency;
	RID index_buffer_dependency;
	bool is_compressed = false;
	uint64_t blas_size = 0;
};

/// Inputs for a surface backed by a per-frame-deformed vertex buffer.
struct RTDeformedGeometrySource {
	RID current_vb; ///< Vertex buffer (object-space positions) the BLAS is built / refit against.
	RID prev_vb; ///< Previous-frame positions for motion vectors. Optional.
	uint64_t change_stamp = 0; ///< Changes when `current_vb` contents change.
	uint64_t cache_key = 0; ///< Caller-defined cache identity.
	uint32_t cache_version = 0; ///< Changes when the resource behind `cache_key` is recycled.
	uint32_t surface_counter = 0; ///< Changes when the underlying mesh surface changes.
};

struct RTMaterialData {
	alignas(16) RT_MaterialData data = {};
	uint32_t global_buffer_index = UINT32_MAX;
	uint32_t rt_sbt_offset = 0;
	bool is_custom_shader = false;
	bool uses_global_texture_uniforms = false;
	RID uniform_buffer; // Buffer pointer for mats > 512 bytes.
	uint32_t uniform_pool_slot = UINT32_MAX; // Index into the material UBO pool, or UINT32_MAX (unused)
	RID albedo_texture_rd;
	RID normal_texture_rd;
	RID orm_texture_rd;
	RID emission_texture_rd;
};

struct RTCacheEntry {
	RTSurfaceData *ptr = nullptr;
	uint64_t cache_key = 0;
	uint32_t last_used_frame = 0;
	uint32_t cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint8_t failed_attempts = 0;
	uint64_t size_bytes = 0;
};

/// Cache entry for a per-(MultiMesh, surface) merged BLAS.
/// All vertex data (positions, normals, tangents, UVs, colors) is fully baked per-instance
/// so the hit shader uses the standard code path — no special per-instance lookups.
struct RTMergedMMEntry {
	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if mesh has normals).
	// The BLAS reads only the position section; the hit shader reads TBN via normal_byte_offset.
	RID merged_vtx_buffer;
	uint32_t vtx_capacity_bytes = 0;

	// Merged attribute buffer: [UV + color × N*V] replicated per instance.
	RID merged_attr_buffer;
	uint32_t attr_capacity_bytes = 0;

	// Replicated index buffer: uint32 N*I entries (invalid if non-indexed).
	RID replicated_idx_buffer;
	uint32_t idx_capacity = 0;

	RID merge_uniform_set;
	RID last_mm_buffer;
	RID last_src_vtx_buffer;
	RID last_src_attr_buffer;
	RID last_src_index_buffer;

	RID blas;
	uint32_t last_mm_count = 0;
	uint32_t last_surface_counter = 0;
	uint32_t last_mesh_version = 0;
	uint32_t last_used_frame = 0;
	bool blas_built_once = false;
	bool indexed = false; // selects MODE_INDEXED vs MODE_NON_INDEXED variant
};

struct RTMaterialCacheEntry {
	RTMaterialData *ptr = nullptr;
	RTMaterialData *procedural_ptr = nullptr;
	uint32_t last_used_frame = 0;
	uint32_t procedural_last_used_frame = 0;
	uint16_t cached_counter = 0;
	uint16_t procedural_cached_counter = 0;
	uint32_t cached_rid_version = 0;
	uint32_t procedural_cached_rid_version = 0;
	uint32_t procedural_cached_sbt_offset = 0;
};

/// Per-viewport raytracing state.
///
/// Each viewport has its own visibility set (frustum/LOD/visibility ranges), so
/// the TLAS instance composition and per-instance SSBO contents differ across
/// viewports. Sharing them caused the wrong `gl_InstanceCustomIndexEXT` to
/// resolve to the wrong `geometries[]` / `materials[]` entries, dereferencing
/// stale BDAs and faulting the GPU.
///
/// Lifetime is tied to a `RenderSceneBuffersRD`: created lazily on first
/// `build_tlas` for that viewport, freed via `RenderRaytracing::free_viewport_state`
/// from `RenderBufferDataForwardClustered::free_data()`.
struct RTViewportState {
	RID tlas;
	uint32_t tlas_max_instances = 0;
	uint32_t tlas_instance_count = 0;

	RID geometry_buffer;
	uint32_t geometry_buffer_capacity = 0;
	RID material_buffer;
	uint32_t material_buffer_capacity = 0;
	RID motion_index_buffer;
	uint32_t motion_index_buffer_capacity = 0;
	RID motion_transform_buffer;
	uint32_t motion_transform_buffer_capacity = 0;
	RID emissive_candidate_buffer;
	uint32_t emissive_candidate_buffer_capacity = 0;
	RID emissive_primitive_buffer;
	uint32_t emissive_primitive_buffer_capacity = 0;

	RID light_buffer;
	RID params_buffer;

	// One cached RT uniform set per dispatch flavor. update_uniform_set is called
	// 3-4x per frame with different RT flags (prepare base, then |WRC_PROBE_UPDATE,
	// |SPG_GATHER, |PRIMARY_DIRECT); the signature folds p_rt_flags and the per-flags
	// pipeline shader, so the flavors alternate and a single slot could never hit
	// across them (it freed and recreated the set 3-4x every frame). Keying the cache
	// on rt_flags lets each flavor keep its own set.
	struct UniformSetCacheEntry {
		uint32_t rt_flags = 0;
		RID uniform_set;
		uint64_t signature = 0;
		RID shader;
		bool valid = false;
	};
	// Six slots: today's four flavors plus slack.
	UniformSetCacheEntry uniform_set_cache[6];
	uint64_t light_buffer_signature = 0;
	bool light_buffer_signature_valid = false;

	uint32_t frame_counter = 0;

	// WRC clipmap params channeled to update_uniform_set so the WRC probe-update
	// raygen reads the WRC's own grid/cascade/spacing/rays (which the WRC atlas was
	// sized from) out of the STRC RT-param slots, instead of the Environment's STRC
	// settings. A zero wrc_grid is the "not a WRC probe-update dispatch" sentinel:
	// the override in update_uniform_set only fires when wrc_grid > 0 AND the
	// RT_FLAG_WRC_PROBE_UPDATE flag is set, so STRC/main dispatches are untouched.
	uint32_t wrc_grid = 0;
	uint32_t wrc_cascade_count = 0;
	float wrc_base_spacing = 0.0f;
	uint32_t wrc_rays_per_frame = 0;

	// Screen Probe Gather (SPG) per-frame scalars channeled to update_uniform_set so the
	// SPG gather raygen reads its grid/oct/dir budget + WRC-query oct_res out of the
	// RT_PARAM_RTGI_SPG_* slots (filled by the RT_FLAG_SPG_GATHER override). All zero is
	// the "not an SPG gather dispatch" sentinel; the override only fires when the
	// RT_FLAG_SPG_GATHER flag is set, so STRC/WRC/main dispatches are untouched. The SPG
	// gather reuses the WRC's STRC-slot override (wrc_grid/...) to address the WRC atlas.
	uint32_t spg_grid_w = 0;
	uint32_t spg_grid_h = 0;
	uint32_t spg_oct_res = 0;
	uint32_t spg_dirs_per_frame = 0;
	uint32_t spg_wrc_oct_res = 0;
	float spg_fallback_conf = 0.0f;

	uint64_t radiance_history_signature = 0;
	bool radiance_history_signature_valid = false;
	bool radiance_history_invalidated = false;
	uint64_t emissive_candidate_signature = 0;
	RT_LightData previous_light_data[RT_LIGHTS_MAX] = {};
	uint32_t previous_light_count = 0;
	bool previous_light_data_valid = false;
	HashSet<uint64_t> previous_history_keys;
	HashSet<uint64_t> current_history_keys;
};

struct RTGIBackendCapabilities {
	RSE::PathtracingBackend backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	String name;
	bool available = false;
	String runtime_name;
	String integration_path;
	String rendering_device_family;
	String rendering_device_name;
	String rendering_device_vendor;
	uint32_t rendering_device_vendor_id = 0;
	// Backend integrations must exchange images, buffers, and synchronization
	// through RenderingDevice-owned RIDs/capabilities. The active implementation
	// is Vulkan-only; this keeps the contract driver-neutral enough for a future
	// RD backend to grow equivalent support without adding a D3D12 runtime path.
	bool rendering_device_exchange = false;
	bool vulkan_runtime = false;
	bool external_memory = false;
	bool external_semaphore = false;
	bool timeline_semaphore = false;
	bool staged_copy = false;
	bool denoiser_handoff = false;
	bool backend_compiled = false;
	bool runtime_detected = false;
	bool device_supported = false;
	bool resource_exchange_supported = false;
	bool implementation_ready = false;
	bool sdk_headers_present = false;
	String vulkan_interop_mode;
	String resource_exchange_sync;
	bool native_probe_update = false;
	bool generic_probe_update_fallback = false;
	bool denoiser_runtime_detected = false;
	bool denoiser_available = false;
	String denoiser_name;
	String denoiser_failure_reason;
	String probe_update_path;
	String availability_failure;
	String compile_failure_reason;
	String runtime_failure_reason;
	String device_failure_reason;
	String resource_exchange_failure_reason;
	String implementation_failure_reason;
	String fallback_reason;
};

struct RTGIBackendStatus {
	RSE::PathtracingBackend requested_backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	RSE::PathtracingBackend active_backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	RTGIBackendCapabilities requested_capabilities;
	RTGIBackendCapabilities active_capabilities;
	bool requested_backend_available = false;
	bool active_backend_available = false;
	bool requested_backend_initialized = false;
	bool active_backend_initialized = false;
	bool using_fallback = false;
	String fallback_reason;
};

enum RTGIBackendExchangeMode {
	RTGI_BACKEND_EXCHANGE_RD_INTERNAL,
	RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE,
	RTGI_BACKEND_EXCHANGE_TIMELINE_SEMAPHORE,
	RTGI_BACKEND_EXCHANGE_STAGED_COPY,
};

enum RTGIBackendExternalHandleType {
	RTGI_BACKEND_EXTERNAL_HANDLE_NONE,
	RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD,
	RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32,
};

enum RTGIBackendExternalSemaphoreKind {
	RTGI_BACKEND_EXTERNAL_SEMAPHORE_BINARY,
	RTGI_BACKEND_EXTERNAL_SEMAPHORE_TIMELINE,
};

enum RTGIBackendOwnershipDirection {
	RTGI_BACKEND_OWNERSHIP_RD_INTERNAL,
	RTGI_BACKEND_OWNERSHIP_RD_TO_BACKEND_TO_RD,
	RTGI_BACKEND_OWNERSHIP_BACKEND_TO_RD_COPY,
};

struct RTGIBackendResourceExchange {
	RTGIBackendExchangeMode mode = RTGI_BACKEND_EXCHANGE_RD_INTERNAL;
	RTGIBackendOwnershipDirection ownership_direction = RTGI_BACKEND_OWNERSHIP_RD_INTERNAL;
	RID output_texture;
	RID depth_texture;
	RID diffuse_radiance_texture;
	RID specular_radiance_texture;
	Vector<RenderingDevice::CallbackResource> acquire_callback_resources;
	RDD::DriverCallback acquire_driver_callback = nullptr;
	void *acquire_driver_callback_userdata = nullptr;
	Vector<RenderingDevice::CallbackResource> release_callback_resources;
	RDD::DriverCallback release_driver_callback = nullptr;
	void *release_driver_callback_userdata = nullptr;
	uint64_t external_memory_handle = 0;
	RTGIBackendExternalHandleType external_memory_handle_type = RTGI_BACKEND_EXTERNAL_HANDLE_NONE;
	uint64_t external_memory_allocation_offset = 0;
	uint64_t external_memory_allocation_size = 0;
	bool external_memory_dedicated_allocation = false;
	uint64_t external_wait_semaphore_handle = 0;
	RTGIBackendExternalHandleType external_wait_semaphore_handle_type = RTGI_BACKEND_EXTERNAL_HANDLE_NONE;
	uint64_t external_signal_semaphore_handle = 0;
	RTGIBackendExternalHandleType external_signal_semaphore_handle_type = RTGI_BACKEND_EXTERNAL_HANDLE_NONE;
	RTGIBackendExternalSemaphoreKind external_semaphore_kind = RTGI_BACKEND_EXTERNAL_SEMAPHORE_BINARY;
	RDD::SemaphoreID external_wait_semaphore;
	RDD::SemaphoreID external_signal_semaphore;
	RDD::TextureLayout external_output_layout_before_backend = RDD::TEXTURE_LAYOUT_GENERAL;
	RDD::TextureLayout external_output_layout_after_backend = RDD::TEXTURE_LAYOUT_GENERAL;
	RID wait_semaphore;
	RID signal_semaphore;
	uint64_t wait_timeline_value = 0;
	uint64_t signal_timeline_value = 0;
	RID staged_copy_source_buffer;
	RID staged_copy_target_texture;
	// Staged copy mode is callback-managed in this phase. The backend release
	// callback performs the actual copy, but must declare the exact RD/RDD copy
	// metadata it intends to execute so validation can reject underspecified
	// buffer-to-texture handoffs.
	Vector<RDD::BufferTextureCopyRegion> staged_copy_regions;
	RDD::TextureLayout staged_copy_target_layout = RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL;
	bool rd_owns_output_before_dispatch = true;
	bool rd_owns_output_after_dispatch = true;
};

struct RTGIBackendSceneResources {
	RID tlas;
	LocalVector<RID> blases;
	uint32_t tlas_instance_count = 0;

	RID geometry_buffer;
	uint32_t geometry_count = 0;
	RID material_buffer;
	uint32_t material_count = 0;
	RID motion_index_buffer;
	uint32_t motion_index_count = 0;
	RID motion_transform_buffer;
	uint32_t motion_transform_count = 0;
	RID emissive_candidate_buffer;
	uint32_t emissive_candidate_count = 0;
	float emissive_candidate_total_weight = 0.0f;
	uint64_t emissive_candidate_signature = 0;

	RID light_buffer;
	uint32_t light_count = 0;
	RID params_buffer;
};

struct RTGIBackendCPUGeometry {
	LocalVector<Vector3> vertices;
	LocalVector<uint32_t> indices;
	uint32_t primitive_count = 0;
	bool valid = false;
	bool indexed = false;
};

struct RTGIBackendSceneSnapshot {
	RID tlas;
	LocalVector<RID> blases;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks;
	LocalVector<uint32_t> sbt_offsets;
	LocalVector<RT_GeometryData> geometries;
	LocalVector<RTGIBackendCPUGeometry> cpu_geometries;
	LocalVector<RT_MaterialData> materials;
	LocalVector<RID> material_uniform_buffers;
	LocalVector<int32_t> motion_indices;
	LocalVector<RT_InstanceMotionData> motion_transforms;
	LocalVector<RT_EmissiveCandidate> emissive_candidates;
	LocalVector<RT_EmissivePrimitiveDistribution> emissive_primitive_distributions;
	LocalVector<RT_LightData> lights;
	uint32_t tlas_instance_count = 0;
	float emissive_candidate_total_weight = 0.0f;
	uint64_t emissive_candidate_signature = 0;
	uint64_t radiance_history_signature = 0;
	bool radiance_history_signature_valid = false;
	bool radiance_history_invalidated = false;
};

struct RTGIBackendFrameContext {
	RenderingDevice *rd = nullptr;
	RenderRaytracing *raytracing = nullptr;
	const RenderDataRD *render_data = nullptr;
	RTViewportState *viewport_state = nullptr;
	RID pipeline;
	RID uniform_set;
	uint32_t rt_flags = 0;
	Size2i output_size;
	RTGIBackendSceneResources scene_resources;
	RTGIBackendSceneSnapshot scene_snapshot;
	RTGIBackendResourceExchange exchange;
	bool radiance_history_invalidated = false;
	bool acquire_callback_recorded = false;
	bool release_callback_recorded = false;
};

enum RTGIBackendDispatchResult {
	RTGI_BACKEND_DISPATCH_OK,
	RTGI_BACKEND_DISPATCH_SAFE_FAILURE,
	RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE,
};

class RTGIBackend {
public:
	virtual ~RTGIBackend() {}

	virtual RTGIBackendCapabilities query_capabilities() const = 0;
	virtual bool initialize(RenderForwardClustered *p_owner, RenderRaytracing *p_raytracing, String *r_fallback_reason) = 0;
	virtual void shutdown() = 0;
	virtual bool prepare_frame(RTGIBackendFrameContext &r_context, String *r_fallback_reason) = 0;
	virtual bool upload_or_import_scene(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
	virtual bool upload_materials_lights_environment(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
	virtual bool prepare_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count, RID &r_probe_pipeline, RID &r_probe_uniform_set, String *r_fallback_reason) = 0;
	virtual bool dispatch_path_trace(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
	virtual bool dispatch_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_pipeline, RID p_probe_uniform_set, RID p_probe_output_buffer, uint32_t p_ray_count, String *r_fallback_reason) = 0;
	virtual bool handoff_denoiser(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
	virtual bool synchronize_output(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
	virtual bool abort_frame(RTGIBackendFrameContext &p_context, String *r_fallback_reason) = 0;
};

class RenderRaytracing {
	friend class RenderForwardClustered;

	RenderForwardClustered *owner = nullptr;

	SceneShaderRaytracing *shader = nullptr;
	BindlessBlock *bindless_block = nullptr;

	RID bindless_uniform_set;
	RID blue_noise_texture;

	HashMap<uint64_t, RTCacheEntry> surface_cache;
	Vector<RTMaterialCacheEntry *> material_chunks;

	// Merged MultiMesh BLAS cache and compute shader.
	struct MergeShader {
		enum Mode {
			MODE_NON_INDEXED = 0,
			MODE_INDEXED = 1,
			MODE_MAX,
		};
		MultimeshMergeShaderRD shader;
		RID version;
		RID version_shader[MODE_MAX];
		RID pipeline[MODE_MAX];
	} mm_merge_shader;
	HashMap<uint64_t, RTMergedMMEntry> merged_mm_cache;

	// BLAS cache for surfaces driven by per-frame-deformed vertex buffers.
	struct RTDeformedCacheEntry {
		RTSurfaceData *ptr = nullptr;
		uint32_t last_used_frame = 0;
		uint64_t cached_change_stamp = 0;
		uint32_t cached_key_version = 0;
		uint32_t cached_surface_counter = 0;
		uint64_t cached_buffer_id = 0; // RID id of the deformed vertex buffer at the time of build.
		bool blas_built_once = false; // True once the BLAS has been fully built; subsequent ticks can refit.
	};
	HashMap<uint64_t, RTDeformedCacheEntry> deformed_surface_cache;
	LocalVector<uint32_t> material_free_slots;
	uint32_t next_material_slot = 0;
	uint64_t vram_used = 0;
	uint32_t cache_hits = 0;
	uint32_t cache_misses = 0;

	// Per-frame scratch arrays. Refilled per viewport's build_tlas; immediately
	// consumed by build_acceleration_structures + finalize_buffers, so they
	// don't need to be per-viewport.
	LocalVector<RT_GeometryData> geometry_data;
	LocalVector<RT_MaterialData> material_data;
	LocalVector<RID> material_ubo_dependencies;
	LocalVector<RID> geometry_buffer_dependencies;
	LocalVector<RID> deformed_buffer_dependencies;
	HashSet<RID> buffer_dependency_dedupe_scratch;
	LocalVector<int32_t> motion_indices; ///< Per-instance: index into motion_transforms[], or -1.
	LocalVector<RT_InstanceMotionData> motion_transforms; ///< Compact: only moving instances.
	LocalVector<RT_EmissiveCandidate> emissive_candidates;
	LocalVector<RT_EmissivePrimitiveDistribution> emissive_primitive_distributions;
	float emissive_candidate_total_weight = 0.0f;
	uint64_t current_emissive_candidate_signature = 0;
	LocalVector<RID> blass;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks;
	LocalVector<uint32_t> sbt_offsets; // 0 = default material hit group
	LocalVector<RTGIBackendCPUGeometry> cpu_geometry_data;

	HashMap<RenderSceneBuffersRD *, RTViewportState *> viewport_states;
	RTGIBackend *rtgi_backends[RSE::PT_BACKEND_MAX] = {};
	bool rtgi_backend_initialized[RSE::PT_BACKEND_MAX] = {};
	bool rtgi_backend_unavailable_warned[RSE::PT_BACKEND_MAX] = {};
	RSE::PathtracingBackend active_backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	RSE::PathtracingBackend last_requested_backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	String active_backend_fallback_reason;

	RTViewportState *_get_or_create_viewport_state(const RenderDataRD *p_render_data);
	RTViewportState *_get_viewport_state(const RenderDataRD *p_render_data) const;
	void _free_viewport_state_internal(RTViewportState *p_state);
	bool _initialize_backend(RSE::PathtracingBackend p_backend, String *r_fallback_reason);
	void _activate_backend(RSE::PathtracingBackend p_backend);

	// Material UBO sub-allocation pool. One large device-address buffer divided
	// into fixed-size slots for performance reasons, and easier to debug.
	RID mat_ubo_pool_buffer;
	uint64_t mat_ubo_pool_bda = 0;
	LocalVector<uint32_t> mat_ubo_pool_free_slots;
	uint32_t mat_ubo_pool_free_count = 0;
	uint32_t mat_ubo_pool_next_slot = 0;

	void mat_ubo_pool_ensure_initialized();
	uint32_t mat_ubo_pool_allocate(); // Returns slot index or UINT32_MAX if pool is exhausted.
	void mat_ubo_pool_release(uint32_t p_slot);
	void mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size);
	uint64_t mat_ubo_pool_get_address(uint32_t p_slot) const;

	// Cache helpers.
	static uint32_t get_rid_index(RID p_rid);
	static uint32_t get_rid_version(RID p_rid);
	RTCacheEntry *get_surface_cache_entry(uint64_t p_key);
	RTMaterialCacheEntry *get_material_cache_entry(uint32_t p_index);
	uint32_t allocate_material_slot();

	// Internal methods.
	RTSurfaceData *process_surface(
			const void *p_surf,
			void *p_mesh_surface,
			uint32_t p_surface_invalidation_counter,
			const Transform3D &p_transform,
			LocalVector<RID> &r_dirty_blas_list);
	RTSurfaceData *process_deformed_surface(
			const void *p_surf,
			void *p_mesh_surface,
			const struct RTDeformedGeometrySource &p_source,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list);
	void _populate_surface_blas(
			void *p_mesh_surface,
			RID p_vertex_buffer_override,
			bool p_force_uncompressed,
			bool p_prefer_fast_build,
			bool p_allow_update,
			uint32_t p_cache_key,
			RTSurfaceData *r_surf_data,
			LocalVector<RID> &r_dirty_blas_list);
	void _register_surface_buffer_dependencies(const RTSurfaceData *p_surf_data);
	RTMaterialData *process_material(RID p_material_rid, uint16_t p_material_invalidation_counter, uint32_t p_shader_slot_override = UINT32_MAX);
	bool _build_merged_mm_blas(
			RID p_mm_rid,
			RID p_mm_gpu_buffer,
			void *p_mesh_surface,
			uint32_t p_mm_count,
			uint32_t p_surface_index,
			uint32_t p_surface_counter,
			RD::ComputeListID p_compute_list,
			LocalVector<RID> &r_dirty_blas_list,
			LocalVector<RID> &r_dirty_blas_update_list,
			RTSurfaceData *r_surf_data);
	void update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list);
	void build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list);
	void finalize_buffers(RTViewportState *p_state);
	void prepare_frame();
	RID _ensure_blue_noise_texture();

public:
	void initialize(RenderForwardClustered *p_owner);

	void cleanup_caches();

	RTGIBackendCapabilities get_backend_capabilities(RSE::PathtracingBackend p_backend) const;
	Dictionary get_backend_capabilities_dictionary(RSE::PathtracingBackend p_backend) const;
	Array get_backend_capabilities_dictionaries() const;
	static Array get_static_backend_capabilities_dictionaries();
	static Dictionary get_static_backend_status_dictionary();
	static Dictionary get_static_backend_status_dictionary(RSE::PathtracingBackend p_requested);
#ifdef TESTS_ENABLED
	static bool test_vulkan_external_resource_exchange(RenderingDevice *p_rd, Dictionary *r_result, String *r_failure_reason);
#endif
	RSE::PathtracingBackend resolve_backend(RSE::PathtracingBackend p_requested);
	RSE::PathtracingBackend get_active_backend() const { return active_backend; }
	String get_active_backend_fallback_reason() const { return active_backend_fallback_reason; }
	RTGIBackendStatus get_backend_status() const;
	Dictionary get_backend_status_dictionary() const;
	Dictionary get_backend_status_dictionary(RSE::PathtracingBackend p_requested) const;
	static RSE::PathtracingBackend backend_from_env_param(float p_backend);
	static const char *backend_get_name(RSE::PathtracingBackend p_backend);

	RTViewportState *build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags);
	void populate_backend_scene_resources(RTViewportState *p_state, RTGIBackendSceneResources &r_resources) const;
	void populate_backend_scene_snapshot(RTViewportState *p_state, RTGIBackendSceneSnapshot &r_snapshot) const;
	uint32_t gather_lights(const RenderDataRD *p_render_data, const RTViewportState *p_state, RT_LightData *r_light_data, uint32_t p_max_lights);
	RID update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags);
	bool prepare_backend_frame(const RenderDataRD *p_render_data, uint32_t p_rt_flags, RTGIBackendFrameContext &r_context);
	RTGIBackendDispatchResult dispatch_path_trace_backend(RTGIBackendFrameContext &r_context);
	RTGIBackendDispatchResult dispatch_probe_update_backend(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count);
	// Full-screen FPT primary-direct dispatch that coexists with the probe dispatches.
	RTGIBackendDispatchResult dispatch_primary_direct_backend(RTGIBackendFrameContext &p_context, uint32_t p_primary_direct_flags);

	void copy_output_texture(const RenderDataRD *p_render_data);
	void free_viewport_state(RenderSceneBuffersRD *p_render_buffers);

	SceneShaderRaytracing *get_shader() const { return shader; }

	RID get_bindless_uniform_set() const { return bindless_uniform_set; }
	RID get_mat_ubo_pool_buffer() const { return mat_ubo_pool_buffer; }
	const LocalVector<RID> &get_material_ubo_dependencies() const { return material_ubo_dependencies; }
	const LocalVector<RID> &get_geometry_buffer_dependencies() const { return geometry_buffer_dependencies; }
	const LocalVector<RID> &get_deformed_buffer_dependencies() const { return deformed_buffer_dependencies; }
	void begin_unique_buffer_dependencies(uint32_t p_expected_dependencies);
	void add_unique_buffer_dependency(RD::RaytracingListID p_raytracing_list, RID p_buffer);

	~RenderRaytracing();
};

} // namespace RendererSceneRenderImplementation
