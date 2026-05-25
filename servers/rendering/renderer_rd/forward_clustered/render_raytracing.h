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
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/bindless_block.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

#define RB_TEX_RAYTRACING SNAME("raytracing")
#define RB_TEX_RT_DIFFUSE_RADIANCE SNAME("rt_diffuse_radiance")
#define RB_TEX_RT_SPECULAR_RADIANCE SNAME("rt_specular_radiance")
#define RB_TEX_RT_SPECULAR_GUIDE SNAME("rt_specular_guide")
#define RB_TEX_RT_DEPTH SNAME("rt_depth")
#define RB_TEX_RT_DEPTH_ATTACHMENT SNAME("rt_depth_attachment")
#define RB_TEX_RT_VELOCITY SNAME("rt_velocity")
#define RB_TEX_RT_HISTORY_VALIDITY SNAME("rt_history_validity")
#define RB_TEX_RT_HISTORY_VALIDITY_PREV SNAME("rt_history_validity_prev")
#define RB_TEX_RT_HISTORY_ID SNAME("rt_history_id")
#define RB_TEX_RT_HISTORY_ID_PREV SNAME("rt_history_id_prev")
#define RB_TEX_RT_NORMAL_ROUGHNESS SNAME("rt_normal_roughness")
#define RB_TEX_RT_SOURCE_NORMAL_ROUGHNESS_PREV SNAME("rt_source_normal_roughness_prev")
#define RB_TEX_RT_ALBEDO_METALNESS SNAME("rt_albedo_metalness")
#define RB_TEX_RT_VIEWZ_HITDIST SNAME("rt_viewz_hitdist")
#define RB_TEX_RT_SOURCE_VIEWZ_HITDIST_PREV SNAME("rt_source_viewz_hitdist_prev")
#define RB_TEX_RT_SIGNAL_DIRECT_LIGHT SNAME("rt_signal_direct_light")
#define RB_TEX_RT_SIGNAL_EMISSIVE SNAME("rt_signal_emissive")
#define RB_TEX_RT_SIGNAL_INDIRECT SNAME("rt_signal_indirect")
#define RB_TEX_RT_SIGNAL_SKY SNAME("rt_signal_sky")
#define RB_TEX_RT_SIGNAL_CONFIDENCE SNAME("rt_signal_confidence")
#define RB_TEX_RT_SOURCE_CANDIDATE SNAME("rt_source_candidate")
#define RB_TEX_RT_SOURCE_CANDIDATE_PREV SNAME("rt_source_candidate_prev")
#define RB_TEX_RT_SOURCE_CANDIDATE_KEY SNAME("rt_source_candidate_key")
#define RB_TEX_RT_SOURCE_CANDIDATE_KEY_PREV SNAME("rt_source_candidate_key_prev")
#define RB_TEX_RT_SOURCE_HISTORY SNAME("rt_source_history")
#define RB_TEX_RT_SOURCE_TEMPORAL_DELTA SNAME("rt_source_temporal_delta")
#define RB_TEX_RT_SOURCE_REJECTION SNAME("rt_source_rejection")

#define RB_SCOPE_DLSS_RR SNAME("dlss_rr")
#define RB_TEX_DLSS_RR_DIFFUSE_ALBEDO SNAME("diffuse_albedo")
#define RB_TEX_DLSS_RR_SPECULAR_ALBEDO SNAME("specular_albedo")
#define RB_TEX_DLSS_RR_NORMAL_ROUGHNESS SNAME("normal_roughness")
#define RB_TEX_DLSS_RR_SPECULAR_HIT_DIST SNAME("specular_hit_dist")

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {

class RenderForwardClustered;
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

// Must match GLSL RTEmissiveCandidate (std430, 64 bytes).
struct alignas(16) RT_EmissiveCandidate {
	float object_to_world[12];
	uint32_t geometry_index;
	uint32_t flags;
	float selection_weight;
	float _pad;
};
static_assert(sizeof(RT_EmissiveCandidate) == 64, "RT_EmissiveCandidate must be 64 bytes for std430");

// Light types for raytracing (matches GLSL RT_LIGHT_TYPE_* defines).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_SPOT = 3,
};

enum RTLightFlag : uint32_t {
	RT_LIGHT_FLAG_SHADOW = 1 << 0,
};

// Must match GLSL RTLightData (std430, 96 bytes).
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
};
static_assert(sizeof(RT_LightData) == 96, "RT_LightData must be 96 bytes for std430");

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

	RID light_buffer;
	RID params_buffer;

	RID uniform_set;
	uint64_t uniform_set_signature = 0;
	RID uniform_set_shader;
	bool uniform_set_signature_valid = false;
	uint64_t light_buffer_signature = 0;
	bool light_buffer_signature_valid = false;

	uint32_t frame_counter = 0;
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

class RenderRaytracing {
	friend class RenderForwardClustered;

	RenderForwardClustered *owner = nullptr;

	SceneShaderRaytracing *shader = nullptr;
	BindlessBlock *bindless_block = nullptr;

	RID bindless_uniform_set;

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
	float emissive_candidate_total_weight = 0.0f;
	uint64_t current_emissive_candidate_signature = 0;
	LocalVector<RID> blass;
	LocalVector<Transform3D> blas_transforms;
	LocalVector<uint32_t> instance_flags;
	LocalVector<uint8_t> instance_masks;
	LocalVector<uint32_t> sbt_offsets; // 0 = default material hit group

	HashMap<RenderSceneBuffersRD *, RTViewportState *> viewport_states;

	RTViewportState *_get_or_create_viewport_state(const RenderDataRD *p_render_data);
	RTViewportState *_get_viewport_state(const RenderDataRD *p_render_data) const;
	void _free_viewport_state_internal(RTViewportState *p_state);

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

public:
	void initialize(RenderForwardClustered *p_owner);

	void cleanup_caches();

	RTViewportState *build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags);
	uint32_t gather_lights(const RenderDataRD *p_render_data, RT_LightData *r_light_data, uint32_t p_max_lights);
	RID update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags);

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
