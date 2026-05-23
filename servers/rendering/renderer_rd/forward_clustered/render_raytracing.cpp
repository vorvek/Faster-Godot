/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"
#include "servers/rendering/storage/variant_converters.h"

using namespace RendererSceneRenderImplementation;

static constexpr real_t RT_COMPRESSED_AABB_EPSILON = 0.0001;

static AABB _rt_make_safe_compressed_aabb(const AABB &p_aabb) {
	AABB safe_aabb = p_aabb;
	safe_aabb.size.x = MAX(safe_aabb.size.x, RT_COMPRESSED_AABB_EPSILON);
	safe_aabb.size.y = MAX(safe_aabb.size.y, RT_COMPRESSED_AABB_EPSILON);
	safe_aabb.size.z = MAX(safe_aabb.size.z, RT_COMPRESSED_AABB_EPSILON);
	return safe_aabb;
}

static bool _rt_acceleration_structure_is_alive(RID p_rid) {
	return p_rid.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(p_rid);
}

static void _rt_free_acceleration_structure_if_alive(RID &r_rid) {
	if (!r_rid.is_valid()) {
		return;
	}
	if (RD::get_singleton()->acceleration_structure_is_valid(r_rid)) {
		RD::get_singleton()->free_rid(r_rid);
	}
	r_rid = RID();
}

static void _rt_free_uniform_set_if_alive(RID &r_rid) {
	if (!r_rid.is_valid()) {
		return;
	}
	if (RD::get_singleton()->uniform_set_is_valid(r_rid)) {
		RD::get_singleton()->free_rid(r_rid);
	}
	r_rid = RID();
}

static uint64_t _rt_history_mix(uint64_t p_hash, uint64_t p_value) {
	return p_hash ^ (p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2));
}

static uint64_t _rt_history_mix_rid(uint64_t p_hash, RID p_rid) {
	return _rt_history_mix(p_hash, p_rid.is_valid() ? p_rid.get_id() : 0);
}

static uint64_t _rt_signature_mix_rid(uint64_t p_hash, RID p_rid) {
	return _rt_history_mix_rid(p_hash, p_rid);
}

static uint64_t _rt_history_mix_float(uint64_t p_hash, float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	return _rt_history_mix(p_hash, bits);
}

static uint64_t _rt_history_mix_color(uint64_t p_hash, const Color &p_color) {
	p_hash = _rt_history_mix_float(p_hash, p_color.r);
	p_hash = _rt_history_mix_float(p_hash, p_color.g);
	p_hash = _rt_history_mix_float(p_hash, p_color.b);
	return _rt_history_mix_float(p_hash, p_color.a);
}

static uint64_t _rt_radiance_signature(uint32_t p_rt_flags, RID p_environment, RID p_camera_attributes, const float p_rt_params[16], const Color &p_background_color, bool p_background_uses_sky, const RT_LightData *p_light_data, uint32_t p_light_count) {
	uint64_t signature = _rt_history_mix(0x727472616469616eULL, p_rt_flags);
	signature = _rt_history_mix_rid(signature, p_environment);
	signature = _rt_history_mix_rid(signature, p_camera_attributes);
	for (uint32_t i = 0; i < 16; i++) {
		if (i == SceneShaderRaytracing::RT_PARAM_FRAME_INDEX) {
			continue;
		}
		signature = _rt_history_mix_float(signature, p_rt_params[i]);
	}
	signature = _rt_history_mix(signature, p_background_uses_sky ? 1u : 0u);
	signature = _rt_history_mix_color(signature, p_background_color);
	signature = _rt_history_mix(signature, p_light_count);
	for (uint32_t i = 0; i < p_light_count; i++) {
		const RT_LightData &ld = p_light_data[i];
		signature = _rt_history_mix(signature, ld.type);
		// Keep smooth animated light changes temporal. Resetting on every
		// carried-lantern energy/position tick prevents the denoiser from ever
		// converging on characters lit by that lantern.
		signature = _rt_history_mix(signature, ld.flags);
		signature = _rt_history_mix(signature, ld.cull_mask);
		signature = _rt_history_mix(signature, ld.shadow_caster_mask);
	}
	return signature;
}

static uint64_t _rt_light_buffer_signature(const RT_LightData *p_light_data, uint32_t p_light_count) {
	uint64_t signature = _rt_history_mix(0x72746c6967687473ULL, p_light_count);
	for (uint32_t i = 0; i < p_light_count; i++) {
		const RT_LightData &ld = p_light_data[i];
		const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&ld);
		for (uint32_t offset = 0; offset < sizeof(RT_LightData); offset += sizeof(uint32_t)) {
			uint32_t word = 0;
			const uint32_t copy_bytes = MIN((uint32_t)sizeof(uint32_t), (uint32_t)sizeof(RT_LightData) - offset);
			memcpy(&word, bytes + offset, copy_bytes);
			signature = _rt_history_mix(signature, word);
		}
	}
	return signature;
}

static Vector3 _rt_light_vec3(const float p_value[3]) {
	return Vector3(p_value[0], p_value[1], p_value[2]);
}

static float _rt_light_luminance(const RT_LightData &p_light) {
	return MAX(0.0f, p_light.emission[0] * 0.2126f + p_light.emission[1] * 0.7152f + p_light.emission[2] * 0.0722f);
}

static bool _rt_light_relative_delta_exceeds(float p_previous, float p_current, float p_relative_threshold, float p_absolute_threshold) {
	const float delta = Math::abs(p_current - p_previous);
	const float scale = MAX(MAX(Math::abs(p_previous), Math::abs(p_current)), 0.001f);
	return delta > p_absolute_threshold && delta / scale > p_relative_threshold;
}

static bool _rt_light_change_requires_history_reset(RTViewportState *p_state, const RT_LightData *p_light_data, uint32_t p_light_count) {
	if (!p_state->previous_light_data_valid) {
		return false;
	}
	if (p_state->previous_light_count != p_light_count) {
		return true;
	}

	for (uint32_t i = 0; i < p_light_count; i++) {
		const RT_LightData &previous = p_state->previous_light_data[i];
		const RT_LightData &current = p_light_data[i];

		if (previous.type != current.type || previous.flags != current.flags || previous.cull_mask != current.cull_mask || previous.shadow_caster_mask != current.shadow_caster_mask) {
			return true;
		}
		if (Math::abs(previous.shadow_opacity - current.shadow_opacity) > 0.25f) {
			return true;
		}
		if (_rt_light_relative_delta_exceeds(previous.indirect_energy, current.indirect_energy, 0.5f, 0.15f)) {
			return true;
		}
		if (_rt_light_relative_delta_exceeds(_rt_light_luminance(previous), _rt_light_luminance(current), 0.6f, 0.2f)) {
			return true;
		}

		if (current.type == RT_LIGHT_TYPE_DIRECTIONAL) {
			Vector3 previous_dir = _rt_light_vec3(previous.position).normalized();
			Vector3 current_dir = _rt_light_vec3(current.position).normalized();
			if (!previous_dir.is_finite() || !current_dir.is_finite() || previous_dir.dot(current_dir) < 0.94f) {
				return true;
			}
			continue;
		}

		const Vector3 previous_pos = _rt_light_vec3(previous.position);
		const Vector3 current_pos = _rt_light_vec3(current.position);
		if (!previous_pos.is_finite() || !current_pos.is_finite()) {
			return true;
		}

		const float current_range = current.inv_max_range > 0.0f ? 1.0f / current.inv_max_range : 32.0f;
		const float previous_range = previous.inv_max_range > 0.0f ? 1.0f / previous.inv_max_range : current_range;
		const float movement_threshold = MAX(1.0f, MIN(current_range, previous_range) * 0.2f);
		if (previous_pos.distance_squared_to(current_pos) > movement_threshold * movement_threshold) {
			return true;
		}

		if (current.type == RT_LIGHT_TYPE_SPOT) {
			Vector3 previous_dir = _rt_light_vec3(previous.spot_direction).normalized();
			Vector3 current_dir = _rt_light_vec3(current.spot_direction).normalized();
			if (!previous_dir.is_finite() || !current_dir.is_finite() || previous_dir.dot(current_dir) < 0.94f) {
				return true;
			}
		}
	}

	return false;
}

static uint32_t _rt_cache_index_from_key(uint64_t p_key) {
	uint32_t index = uint32_t(p_key) ^ uint32_t(p_key >> 32);
	return index != 0 ? index : 1u;
}

static bool _rt_history_key_is_valid(RTViewportState *p_state, uint64_t p_key) {
	bool valid = p_state->previous_history_keys.has(p_key);
	p_state->current_history_keys.insert(p_key);
	return valid;
}

static RT_GeometryData _rt_geometry_with_history_validity(RTViewportState *p_state, const RT_GeometryData &p_geometry, uint64_t p_history_key, uint32_t p_layer_mask, uint8_t p_instance_mask, bool p_force_invalid = false) {
	RT_GeometryData geometry = p_geometry;
	geometry.layer_mask = p_layer_mask;
	uint64_t history_key = _rt_history_mix(p_history_key, p_layer_mask);
	history_key = _rt_history_mix(history_key, (p_instance_mask & RT_INSTANCE_MASK_VISIBLE) != 0 ? 1u : 0u);
	uint32_t history_id = uint32_t(history_key ^ (history_key >> 32));
	geometry.history_id = history_id != 0 ? history_id : 1u;
	bool history_valid = _rt_history_key_is_valid(p_state, history_key);
	if (p_force_invalid || !history_valid) {
		geometry.flags |= RT_GEOM_FLAG_HISTORY_INVALID;
	}
	return geometry;
}

static uint8_t _rt_instance_mask(bool p_visible, bool p_casts_shadows) {
	uint8_t mask = 0;
	if (p_visible) {
		mask |= RT_INSTANCE_MASK_VISIBLE;
	}
	if (p_casts_shadows) {
		mask |= RT_INSTANCE_MASK_SHADOW;
	}
	return mask;
}

static bool _rt_instance_uses_alpha_overlay(const RenderGeometryInstanceBase *p_instance, const RenderDataRD *p_render_data) {
	constexpr float RT_FADE_ALPHA_PASS_THRESHOLD = 0.999f;

	float fade_alpha = 1.0f;
	if (p_instance->fade_near || p_instance->fade_far) {
		Vector3 cam_origin = p_render_data->scene_data->cam_transform.origin;
		Vector3 instance_center = p_instance->transformed_aabb.get_center();
		float fade_dist = cam_origin.distance_to(instance_center);

		if (p_instance->fade_far && fade_dist > p_instance->fade_far_begin) {
			fade_alpha = Math::smoothstep(0.0f, 1.0f, 1.0f - (fade_dist - p_instance->fade_far_begin) / (p_instance->fade_far_end - p_instance->fade_far_begin));
		} else if (p_instance->fade_near && fade_dist < p_instance->fade_near_end) {
			fade_alpha = Math::smoothstep(0.0f, 1.0f, (fade_dist - p_instance->fade_near_begin) / (p_instance->fade_near_end - p_instance->fade_near_begin));
		}
	}

	fade_alpha *= p_instance->force_alpha * p_instance->parent_fade_alpha;
	return fade_alpha < RT_FADE_ALPHA_PASS_THRESHOLD;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
	bindless_block = memnew(BindlessBlock);

	// Initialize merged MultiMesh BLAS compute shader.
	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
	}
}

RenderRaytracing::~RenderRaytracing() {
	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();

	cleanup_caches();

	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}

	if (bindless_block) {
		memdelete(bindless_block);
		bindless_block = nullptr;
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}

	mm_merge_shader.shader.version_free(mm_merge_shader.version);
}

// ---------------------------------------------------------------------------
// Per-viewport state lifecycle
// ---------------------------------------------------------------------------

RTViewportState *RenderRaytracing::_get_or_create_viewport_state(const RenderDataRD *p_render_data) {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(key);
	if (it != viewport_states.end()) {
		return it->value;
	}
	RTViewportState *state = memnew(RTViewportState);
	viewport_states.insert(key, state);
	return state;
}

RTViewportState *RenderRaytracing::_get_viewport_state(const RenderDataRD *p_render_data) const {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::ConstIterator it = viewport_states.find(key);
	return (it != viewport_states.end()) ? it->value : nullptr;
}

void RenderRaytracing::_free_viewport_state_internal(RTViewportState *p_state) {
	if (!p_state) {
		return;
	}
	_rt_free_uniform_set_if_alive(p_state->uniform_set);
	if (p_state->tlas.is_valid()) {
		_rt_free_acceleration_structure_if_alive(p_state->tlas);
	}
	if (p_state->geometry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->geometry_buffer);
	}
	if (p_state->material_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->material_buffer);
	}
	if (p_state->motion_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_index_buffer);
	}
	if (p_state->motion_transform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_transform_buffer);
	}
	if (p_state->light_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->light_buffer);
	}
	if (p_state->params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->params_buffer);
	}
	memdelete(p_state);
}

void RenderRaytracing::free_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(p_render_buffers);
	if (it == viewport_states.end()) {
		return;
	}
	_free_viewport_state_internal(it->value);
	viewport_states.remove(it);
}

// ---------------------------------------------------------------------------
// Material UBO sub-allocation pool
//
// Single device-address storage buffer of MAT_UBO_POOL_TOTAL_BYTES, divided
// into MAT_UBO_POOL_CAPACITY fixed-size slots. Allocate/release just bump a
// next-slot counter and a free-list. Per-material UBO writes are
// buffer_update at slot offset, no allocation. mat.uniform_address is
// pool_bda + slot * slot_size, so the closest-hit shader's
// CustomMaterialUniforms(addr) cast lands on the correct slot.
// ---------------------------------------------------------------------------

namespace {
// Pool tuning. Kept in this TU because nothing outside it needs to know.
// SLOT_SIZE bounds the per-material UBO before we fall back to a dedicated
// buffer; CAPACITY is the maximum number of pooled materials in flight.
constexpr uint32_t MAT_UBO_POOL_SLOT_SIZE = 512;
constexpr uint32_t MAT_UBO_POOL_CAPACITY = 16384;
constexpr uint64_t MAT_UBO_POOL_TOTAL_BYTES = uint64_t(MAT_UBO_POOL_SLOT_SIZE) * MAT_UBO_POOL_CAPACITY;
} // namespace

void RenderRaytracing::mat_ubo_pool_ensure_initialized() {
	if (mat_ubo_pool_buffer.is_valid()) {
		return;
	}
	Vector<uint8_t> init;
	mat_ubo_pool_buffer = RD::get_singleton()->storage_buffer_create(
			MAT_UBO_POOL_TOTAL_BYTES, init, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(mat_ubo_pool_buffer, "RT Material UBO Pool");
		mat_ubo_pool_bda = RD::get_singleton()->buffer_get_device_address(mat_ubo_pool_buffer);
	}
}

uint32_t RenderRaytracing::mat_ubo_pool_allocate() {
	mat_ubo_pool_ensure_initialized();
	if (!mat_ubo_pool_buffer.is_valid()) {
		return UINT32_MAX;
	}
	// Pop from the free-list stack. mat_ubo_pool_free_count is the logical top;
	// the underlying vector is never shrunk so this is a pure counter op.
	if (mat_ubo_pool_free_count > 0) {
		--mat_ubo_pool_free_count;
		return mat_ubo_pool_free_slots[mat_ubo_pool_free_count];
	}
	if (mat_ubo_pool_next_slot >= MAT_UBO_POOL_CAPACITY) {
		ERR_PRINT_ONCE("RT Material UBO Pool exhausted; falling back to dedicated buffer.");
		return UINT32_MAX;
	}
	return mat_ubo_pool_next_slot++;
}

void RenderRaytracing::mat_ubo_pool_release(uint32_t p_slot) {
	if (p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	// Push onto the free-list stack. Grow the underlying buffer only when the
	// counter would overflow the existing capacity; never shrink.
	if (mat_ubo_pool_free_count < mat_ubo_pool_free_slots.size()) {
		mat_ubo_pool_free_slots[mat_ubo_pool_free_count] = p_slot;
	} else {
		mat_ubo_pool_free_slots.push_back(p_slot);
	}
	++mat_ubo_pool_free_count;
}

void RenderRaytracing::mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size) {
	if (!mat_ubo_pool_buffer.is_valid() || p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	uint32_t size = MIN(p_size, MAT_UBO_POOL_SLOT_SIZE);
	RD::get_singleton()->buffer_update(mat_ubo_pool_buffer, uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE, size, p_data);
}

uint64_t RenderRaytracing::mat_ubo_pool_get_address(uint32_t p_slot) const {
	return mat_ubo_pool_bda + uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void RenderRaytracing::cleanup_caches() {
	for (KeyValue<uint64_t, RTCacheEntry> &kv : surface_cache) {
		RTCacheEntry &entry = kv.value;
		if (entry.ptr) {
			if (entry.ptr->blas.is_valid()) {
				_rt_free_acceleration_structure_if_alive(entry.ptr->blas);
			}
			memdelete(entry.ptr);
			entry.ptr = nullptr;
		}
	}
	surface_cache.clear();

	for (KeyValue<uint64_t, RTDeformedCacheEntry> &kv : deformed_surface_cache) {
		RTDeformedCacheEntry &e = kv.value;
		if (e.ptr) {
			_rt_free_acceleration_structure_if_alive(e.ptr->blas);
			memdelete(e.ptr);
			e.ptr = nullptr;
		}
	}
	deformed_surface_cache.clear();

	for (KeyValue<uint64_t, RTMergedMMEntry> &kv : merged_mm_cache) {
		RTMergedMMEntry &e = kv.value;
		RD *rd = RD::get_singleton();
		// Uniform set must be freed before its bound buffers.
		// RD auto-frees a uniform set when any of its bound resources is freed,
		// so freeing the buffer first would leave a stale RID here.
		_rt_free_uniform_set_if_alive(e.merge_uniform_set);
		_rt_free_acceleration_structure_if_alive(e.blas);
		if (e.merged_vtx_buffer.is_valid()) {
			rd->free_rid(e.merged_vtx_buffer);
			e.merged_vtx_buffer = RID();
		}
		if (e.merged_attr_buffer.is_valid()) {
			rd->free_rid(e.merged_attr_buffer);
			e.merged_attr_buffer = RID();
		}
		if (e.replicated_idx_buffer.is_valid()) {
			rd->free_rid(e.replicated_idx_buffer);
			e.replicated_idx_buffer = RID();
		}
	}
	merged_mm_cache.clear();

	// Free all cached material data
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (material_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry *entry = &material_chunks[i][j];
				auto free_material_data = [&](RTMaterialData *&p_ptr) {
					if (!p_ptr) {
						return;
					}
					if (p_ptr->uniform_buffer.is_valid()) {
						RD::get_singleton()->free_rid(p_ptr->uniform_buffer);
					}
					if (p_ptr->uniform_pool_slot != UINT32_MAX) {
						mat_ubo_pool_release(p_ptr->uniform_pool_slot);
					}
					memdelete(p_ptr);
					p_ptr = nullptr;
				};
				if (entry->ptr) {
					free_material_data(entry->ptr);
				}
				if (entry->procedural_ptr) {
					free_material_data(entry->procedural_ptr);
				}
			}
			memdelete_arr(material_chunks[i]);
		}
	}
	material_chunks.clear();

	// Reset counters
	material_free_slots.clear();
	next_material_slot = 0;
	vram_used = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ---------------------------------------------------------------------------
// RID helpers
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::get_rid_index(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() & 0xFFFFFFFFULL);
}

uint32_t RenderRaytracing::get_rid_version(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() >> 32);
}

RTCacheEntry *RenderRaytracing::get_surface_cache_entry(uint64_t p_key) {
	RTCacheEntry &entry = surface_cache[p_key];
	entry.cache_key = p_key;
	return &entry;
}

RTMaterialCacheEntry *RenderRaytracing::get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= material_chunks.size()) {
		material_chunks.push_back(nullptr);
	}

	if (!material_chunks[chunk_idx]) {
		material_chunks.set(chunk_idx, memnew_arr(RTMaterialCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			material_chunks[chunk_idx][i] = RTMaterialCacheEntry();
		}
	}

	return &material_chunks[chunk_idx][entry_idx];
}

uint32_t RenderRaytracing::allocate_material_slot() {
	if (!material_free_slots.is_empty()) {
		uint32_t slot = material_free_slots[material_free_slots.size() - 1];
		material_free_slots.resize(material_free_slots.size() - 1);
		return slot;
	}
	return next_material_slot++;
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::prepare_frame() {
	// Don't free BLAS or materials - they're cached.
	// Scratch arrays are shared (single-threaded render thread); refilled per viewport.
	blass.clear();
	blas_transforms.clear();
	instance_flags.clear();
	instance_masks.clear();
	sbt_offsets.clear();
	geometry_data.clear();
	material_data.clear();
	material_ubo_dependencies.clear();
	geometry_buffer_dependencies.clear();
	deformed_buffer_dependencies.clear();
	motion_indices.clear();
	motion_transforms.clear();

	// Procedural BLAS/AABB lifetime is on the geometry instance.
	{
		static const uint32_t SURFACE_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracer/multimesh_blas_cache_ttl_frames");
		uint32_t current_frame = RSG::rasterizer->get_frame_number();
		LocalVector<uint64_t> to_remove;
		for (KeyValue<uint64_t, RTCacheEntry> &kv : surface_cache) {
			RTCacheEntry &e = kv.value;
			if (e.last_used_frame != 0 && current_frame - e.last_used_frame > SURFACE_CACHE_TTL) {
				if (e.ptr) {
					_rt_free_acceleration_structure_if_alive(e.ptr->blas);
					memdelete(e.ptr);
					e.ptr = nullptr;
				}
				to_remove.push_back(kv.key);
			}
		}
		for (uint64_t k : to_remove) {
			surface_cache.erase(k);
		}
	}

	{
		static const uint32_t DEFORMED_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracer/deformed_mesh_cache_ttl_frames");
		uint32_t current_frame = RSG::rasterizer->get_frame_number();
		LocalVector<uint64_t> to_remove;
		for (KeyValue<uint64_t, RTDeformedCacheEntry> &kv : deformed_surface_cache) {
			RTDeformedCacheEntry &e = kv.value;
			if (e.last_used_frame != 0 && current_frame - e.last_used_frame > DEFORMED_CACHE_TTL) {
				if (e.ptr) {
					_rt_free_acceleration_structure_if_alive(e.ptr->blas);
					memdelete(e.ptr);
					e.ptr = nullptr;
				}
				to_remove.push_back(kv.key);
			}
		}
		for (uint64_t k : to_remove) {
			deformed_surface_cache.erase(k);
		}
	}

	// Evict stale merged MultiMesh BLASes.
	{
		static const uint32_t MM_BLAS_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracer/multimesh_blas_cache_ttl_frames");
		uint32_t current_frame = RSG::rasterizer->get_frame_number();
		RD *rd = RD::get_singleton();
		LocalVector<uint64_t> to_remove;
		for (KeyValue<uint64_t, RTMergedMMEntry> &kv : merged_mm_cache) {
			RTMergedMMEntry &e = kv.value;
			if (e.last_used_frame != 0 && current_frame - e.last_used_frame > MM_BLAS_CACHE_TTL) {
				_rt_free_uniform_set_if_alive(e.merge_uniform_set);
				_rt_free_acceleration_structure_if_alive(e.blas);
				if (e.merged_vtx_buffer.is_valid()) {
					rd->free_rid(e.merged_vtx_buffer);
					e.merged_vtx_buffer = RID();
				}
				if (e.merged_attr_buffer.is_valid()) {
					rd->free_rid(e.merged_attr_buffer);
					e.merged_attr_buffer = RID();
				}
				if (e.replicated_idx_buffer.is_valid()) {
					rd->free_rid(e.replicated_idx_buffer);
					e.replicated_idx_buffer = RID();
				}
				to_remove.push_back(kv.key);
			}
		}
		for (uint64_t k : to_remove) {
			merged_mm_cache.erase(k);
		}
	}

	// Finish async HG compiles so live_ready_mask matches this frame (sync path fills at build_tlas end).
	SceneShaderRaytracing::get_singleton()->drain_completed_compiles();

	// Grow-only geometry/material/motion; TLAS reused; uploads in finalize_buffers().

	// Reset per-frame metrics
	cache_hits = 0;
	cache_misses = 0;

	if (!bindless_block->is_initialized()) {
		bindless_block->initialize(RD::get_singleton());
	}
	bindless_block->begin_frame();
}

// ---------------------------------------------------------------------------
// Surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_surface(
		const void *p_surf,
		void *p_mesh_surface,
		uint32_t p_surface_invalidation_counter,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	if (surf->primitive != RSE::PRIMITIVE_TRIANGLES) {
		return nullptr;
	}

	// For MultiMesh, base is the MultiMesh RID; resolve the underlying Mesh so that
	// different MultiMesh nodes using the same Mesh share one BLAS.
	RID mesh_rid = surf->owner->data->base;
	if (surf->owner->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID underlying = mesh_storage->multimesh_get_mesh(mesh_rid);
		if (underlying.is_valid()) {
			mesh_rid = underlying;
		}
	}

	uint64_t cache_key = _rt_history_mix_rid(0x7375726661636500ULL, mesh_rid);
	cache_key = _rt_history_mix(cache_key, surf->surface_index);
	uint32_t mesh_version = get_rid_version(mesh_rid);

	// Cache lookup
	RTCacheEntry *entry = get_surface_cache_entry(cache_key);
	if (entry->ptr && entry->ptr->blas.is_valid() && !_rt_acceleration_structure_is_alive(entry->ptr->blas)) {
		entry->ptr->blas = RID();
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mesh_version ||
			entry->cached_counter != p_surface_invalidation_counter;

	if (!needs_refresh && entry->ptr->blas.is_valid()) {
		cache_hits++;
		entry->last_used_frame = current_frame;
		_register_surface_buffer_dependencies(entry->ptr);
		return entry->ptr;
	}

	// Cache miss - need to create new BLAS
	cache_misses++;

	// Allocate or reuse entry
	if (!entry->ptr) {
		entry->ptr = memnew(RTSurfaceData);
	} else if (entry->ptr->blas.is_valid()) {
		_rt_free_acceleration_structure_if_alive(entry->ptr->blas);
	}
	entry->cache_key = cache_key;

	RTSurfaceData *surf_data = entry->ptr;

	_populate_surface_blas(p_mesh_surface, RID(), false, false, false, _rt_cache_index_from_key(cache_key), surf_data, r_dirty_blas_list);

	surf->cached_final_transform_valid = false;

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	entry->cached_counter = p_surface_invalidation_counter;
	entry->cached_rid_version = mesh_version;
	entry->last_used_frame = current_frame;
	_register_surface_buffer_dependencies(surf_data);

	return surf_data;
}

// ---------------------------------------------------------------------------
// Deformed surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_deformed_surface(
		const void *p_surf,
		void *p_mesh_surface,
		const RTDeformedGeometrySource &p_source,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	if (surf->primitive != RSE::PRIMITIVE_TRIANGLES) {
		return nullptr;
	}

	if (!p_source.current_vb.is_valid()) {
		return nullptr;
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	uint64_t buffer_id = p_source.current_vb.get_id();

	RTDeformedCacheEntry &entry = deformed_surface_cache[p_source.cache_key];
	if (entry.ptr && entry.ptr->blas.is_valid() && !_rt_acceleration_structure_is_alive(entry.ptr->blas)) {
		entry.ptr->blas = RID();
		entry.blas_built_once = false;
	}

	bool needs_refresh = !entry.ptr ||
			entry.cached_key_version != p_source.cache_version ||
			entry.cached_surface_counter != p_source.surface_counter ||
			entry.cached_buffer_id != buffer_id ||
			entry.cached_change_stamp != p_source.change_stamp;

	auto stamp_deformed_geometry = [&](RTSurfaceData *p_surf_data) {
		if (!p_surf_data || !p_surf_data->blas.is_valid()) {
			return;
		}
		p_surf_data->geometry.flags |= RT_GEOM_FLAG_DEFORMED;
		uint64_t prev_addr = 0;
		if (p_source.prev_vb.is_valid() && p_source.prev_vb != p_source.current_vb) {
			prev_addr = RD::get_singleton()->buffer_get_device_address(p_source.prev_vb);
			deformed_buffer_dependencies.push_back(p_source.prev_vb);
		}
		p_surf_data->geometry.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_addr & 0xFFFFFFFFULL);
		p_surf_data->geometry.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_addr >> 32);
	};

	if (!needs_refresh && entry.ptr->blas.is_valid()) {
		cache_hits++;
		entry.last_used_frame = current_frame;
		stamp_deformed_geometry(entry.ptr);
		_register_surface_buffer_dependencies(entry.ptr);
		return entry.ptr;
	}

	cache_misses++;

	// Refit when only the current deformed vertex buffer contents changed.
	bool can_refit = entry.ptr && entry.ptr->blas.is_valid() && entry.blas_built_once &&
			entry.cached_key_version == p_source.cache_version &&
			entry.cached_surface_counter == p_source.surface_counter &&
			entry.cached_buffer_id == buffer_id;

	if (can_refit) {
		RTSurfaceData *surf_data = entry.ptr;
		r_dirty_blas_update_list.push_back(surf_data->blas);
		surf->cached_final_transform_valid = false;
		stamp_deformed_geometry(surf_data);
		entry.cached_change_stamp = p_source.change_stamp;
		entry.last_used_frame = current_frame;
		_register_surface_buffer_dependencies(surf_data);
		return surf_data;
	}

	if (!entry.ptr) {
		entry.ptr = memnew(RTSurfaceData);
	} else if (entry.ptr->blas.is_valid()) {
		_rt_free_acceleration_structure_if_alive(entry.ptr->blas);
		entry.blas_built_once = false;
	}

	RTSurfaceData *surf_data = entry.ptr;

	_populate_surface_blas(p_mesh_surface, p_source.current_vb, true, true, true, static_cast<uint32_t>(p_source.cache_key), surf_data, r_dirty_blas_list);

	surf->cached_final_transform_valid = false;

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	stamp_deformed_geometry(surf_data);

	entry.blas_built_once = true;
	entry.cached_change_stamp = p_source.change_stamp;
	entry.cached_key_version = p_source.cache_version;
	entry.cached_surface_counter = p_source.surface_counter;
	entry.cached_buffer_id = buffer_id;
	entry.last_used_frame = current_frame;
	_register_surface_buffer_dependencies(surf_data);

	return surf_data;
}

// ---------------------------------------------------------------------------
// Surface BLAS helper (shared between static and deformed paths)
// ---------------------------------------------------------------------------

// Fills RTSurfaceData geometry metadata from the surface format.
// Returns the RIDs needed for BLAS creation so _populate_surface_blas can use them.
static void _fill_surface_geometry_data(
		void *p_mesh_surface,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RID *r_vertex_buffer = nullptr,
		RID *r_attribute_buffer = nullptr,
		RID *r_index_buffer = nullptr) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	bool surface_compressed = surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	bool compressed = surface_compressed && !p_force_uncompressed;
	bool compressed_attributes = surface_compressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	r_surf_data->is_compressed = compressed;

	if (compressed) {
		AABB surface_aabb = _rt_make_safe_compressed_aabb(mesh_storage->mesh_surface_get_aabb(p_mesh_surface));
		r_surf_data->aabb_transform.basis = Basis::from_scale(surface_aabb.size);
		r_surf_data->aabb_transform.origin = surface_aabb.position;
	} else {
		r_surf_data->aabb_transform = Transform3D();
	}

	RT_GeometryData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));

	RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);

	if (r_vertex_buffer) {
		*r_vertex_buffer = vertex_buffer;
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = attribute_buffer;
	}
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);

	geom.vertex_count = vertex_count;

	// Position stride
	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;

	// Normal/tangent layout
	uint32_t normal_stride;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;

	if (compressed) {
		normal_stride = sizeof(uint16_t) * 2;
		if (has_normal) {
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		}
	} else {
		if (has_normal && has_tangent) {
			normal_stride = sizeof(uint16_t) * 4;
			tangent_stride = sizeof(uint16_t) * 4;
			geom.normal_byte_offset = current_offset;
			geom.tangent_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else {
			normal_stride = 0;
		}
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = compressed ? RT_GEOM_FLAG_COMPRESSED : 0;
	if (compressed_attributes) {
		geom.flags |= RT_GEOM_FLAG_COMPRESSED_ATTRIBUTES;
	}

	if (compressed) {
		AABB surface_aabb = _rt_make_safe_compressed_aabb(mesh_storage->mesh_surface_get_aabb(p_mesh_surface));
		geom.aabb_size_x = surface_aabb.size.x;
		geom.aabb_size_y = surface_aabb.size.y;
		geom.aabb_size_z = surface_aabb.size.z;
		geom.aabb_pos_x = surface_aabb.position.x;
		geom.aabb_pos_y = surface_aabb.position.y;
		geom.aabb_pos_z = surface_aabb.position.z;
	} else {
		geom.aabb_size_x = 1.0f;
		geom.aabb_size_y = 1.0f;
		geom.aabb_size_z = 1.0f;
		geom.aabb_pos_x = 0.0f;
		geom.aabb_pos_y = 0.0f;
		geom.aabb_pos_z = 0.0f;
	}

	// Attribute buffer layout
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.uv2_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;

	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += compressed_attributes ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		geom.uv2_byte_offset = attrib_offset;
		attrib_offset += compressed_attributes ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
			attrib_offset += fmtsize[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;

	// UV scale (fp16 packed, matches GLSL unpackHalf2x16)
	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);

	// Index format (no device address — caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

void RenderRaytracing::_populate_surface_blas(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		bool p_force_uncompressed,
		bool p_prefer_fast_build,
		bool p_allow_update,
		uint32_t p_cache_key,
		RTSurfaceData *r_surf_data,
		LocalVector<RID> &r_dirty_blas_list) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_force_uncompressed, r_surf_data,
			&vertex_buffer, &attribute_buffer, &index_buffer);

	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}
	r_surf_data->vertex_buffer_dependency = vertex_buffer;
	r_surf_data->attribute_buffer_dependency = attribute_buffer;
	r_surf_data->index_buffer_dependency = (index_buffer.is_valid() && r_surf_data->geometry.index_format != RT_INDEX_FORMAT_NONE) ? index_buffer : RID();

	RD *rd = RD::get_singleton();
	RT_GeometryData &geom = r_surf_data->geometry;

	if (vertex_buffer.is_valid()) {
		geom.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
	}
	if (attribute_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_buffer_address = rd->buffer_get_device_address(index_buffer);
	}

	uint32_t vertex_count = geom.vertex_count;
	uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	uint32_t position_stride = geom.position_stride;

	bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool compressed = r_surf_data->is_compressed;

	// Create BLAS using the new geometry-based API.
	{
		RD::DataFormat pos_format;
		if (is_2d) {
			// The RT path only builds triangle BLASes for triangle surfaces; this
			// format is kept for imported 2D triangle meshes.
			pos_format = RD::DATA_FORMAT_R32G32_SFLOAT;
		} else if (compressed) {
			pos_format = RD::DATA_FORMAT_R16G16B16A16_UNORM;
		} else {
			pos_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		}

		RD::AccelerationStructureGeometry as_geom;
		// Type defaults to TYPE_TRIANGLES; set explicitly for clarity.
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
		as_geom.geometry.triangles.vertex_stride = position_stride;
		as_geom.geometry.triangles.vertex_count = vertex_count;
		as_geom.geometry.triangles.vertex_format = pos_format;

		if (index_buffer.is_valid() && index_count > 0) {
			as_geom.geometry.triangles.index_buffer = index_buffer;
			as_geom.geometry.triangles.index_count = index_count;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags = p_prefer_fast_build
				? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT
				: RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (p_allow_update) {
			as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		}

		r_surf_data->blas = RD::get_singleton()->blas_create({ &as_geom, 1 }, as_flags);
		if (!r_surf_data->blas.is_valid()) {
			return;
		}
		RD::get_singleton()->set_resource_name(r_surf_data->blas,
				String(p_vertex_buffer_override.is_valid() ? "RT BLAS deformed [" : "RT BLAS [") + itos(p_cache_key) + "]");
		r_dirty_blas_list.push_back(r_surf_data->blas);
	}
}

void RenderRaytracing::_register_surface_buffer_dependencies(const RTSurfaceData *p_surf_data) {
	if (!p_surf_data) {
		return;
	}
	if (p_surf_data->vertex_buffer_dependency.is_valid()) {
		geometry_buffer_dependencies.push_back(p_surf_data->vertex_buffer_dependency);
	}
	if (p_surf_data->attribute_buffer_dependency.is_valid()) {
		geometry_buffer_dependencies.push_back(p_surf_data->attribute_buffer_dependency);
	}
	if (p_surf_data->index_buffer_dependency.is_valid()) {
		geometry_buffer_dependencies.push_back(p_surf_data->index_buffer_dependency);
	}
}

// ---------------------------------------------------------------------------
// Uniform packing (file-local helpers)
// ---------------------------------------------------------------------------

static float _def_real(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].real : 0.0f;
}

static int32_t _def_sint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].sint : 0;
}

static uint32_t _def_uint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].uint : 0u;
}

static uint32_t _def_bool(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? (uint32_t)u.default_value[idx].boolean : 0u;
}

static uint32_t _rt_uniform_std140_size(const ShaderLanguage::ShaderNode::Uniform &u) {
	uint32_t size = ShaderLanguage::get_datatype_size(u.type);
	if (u.array_size > 0) {
		size *= u.array_size;
		uint32_t array_alignment = 16U * (uint32_t)u.array_size;
		if ((size % array_alignment) != 0U) {
			size += array_alignment - (size % array_alignment);
		}
	}
	return size;
}

static Variant _rt_uniform_pack_value(const ShaderLanguage::ShaderNode::Uniform &u, const Variant &val) {
	if (val.get_type() != Variant::NIL) {
		return val;
	}
	if (!u.default_value.is_empty()) {
		return ShaderLanguage::constant_value_to_variant(u.default_value, u.type, u.array_size, u.hint);
	}
	if (u.type == ShaderLanguage::TYPE_MAT2) {
		return Transform2D();
	}
	if (u.type == ShaderLanguage::TYPE_MAT3) {
		return Basis();
	}
	if (u.type == ShaderLanguage::TYPE_MAT4) {
		return Projection();
	}
	if ((u.type == ShaderLanguage::TYPE_VEC3 || u.type == ShaderLanguage::TYPE_VEC4) &&
			(u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SOURCE_COLOR ||
					u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_COLOR_CONVERSION_DISABLED)) {
		return Color(0, 0, 0, 1);
	}
	return ShaderLanguage::get_default_datatype_value(u.type, u.array_size, u.hint);
}

static void pack_uniform(const ShaderLanguage::ShaderNode::Uniform &u, const Variant &val, uint8_t *dst) {
	using SL = ShaderLanguage;
	Variant pack_val = _rt_uniform_pack_value(u, val);

	switch (u.type) {
		case SL::TYPE_FLOAT: {
			float v = pack_val.get_type() == Variant::FLOAT ? (float)(double)pack_val : _def_real(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_INT: {
			int32_t v = pack_val.get_type() == Variant::INT ? (int32_t)(int64_t)pack_val : _def_sint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_UINT: {
			uint32_t v = pack_val.get_type() == Variant::INT ? (uint32_t)(int64_t)pack_val : _def_uint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_BOOL: {
			uint32_t v = pack_val.get_type() == Variant::BOOL ? (uint32_t)(bool)pack_val : _def_bool(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_VEC2: {
			float fv[2];
			if (pack_val.get_type() == Variant::VECTOR2) {
				Vector2 v = pack_val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
			} else {
				fv[0] = _def_real(u, 0);
				fv[1] = _def_real(u, 1);
			}
			memcpy(dst, fv, 8);
		} break;
		case SL::TYPE_VEC3: {
			float fv[3] = {};
			if (pack_val.get_type() == Variant::VECTOR3) {
				Vector3 v = pack_val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
			} else if (pack_val.get_type() == Variant::COLOR) {
				Color c = pack_val;
				if (u.hint == SL::ShaderNode::Uniform::HINT_SOURCE_COLOR) {
					c = c.srgb_to_linear();
				}
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
			} else {
				fv[0] = _def_real(u, 0);
				fv[1] = _def_real(u, 1);
				fv[2] = _def_real(u, 2);
			}
			memcpy(dst, fv, 12);
		} break;
		case SL::TYPE_VEC4: {
			float fv[4] = {};
			if (pack_val.get_type() == Variant::COLOR) {
				Color c = pack_val;
				if (u.hint == SL::ShaderNode::Uniform::HINT_SOURCE_COLOR) {
					c = c.srgb_to_linear();
				}
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
				fv[3] = c.a;
			} else if (pack_val.get_type() == Variant::VECTOR4) {
				Vector4 v = pack_val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
				fv[3] = (float)v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					fv[i] = _def_real(u, i);
				}
			}
			memcpy(dst, fv, 16);
		} break;
		case SL::TYPE_IVEC2: {
			int32_t iv[2];
			if (pack_val.get_type() == Variant::VECTOR2I) {
				Vector2i v = pack_val;
				iv[0] = v.x;
				iv[1] = v.y;
			} else {
				iv[0] = _def_sint(u, 0);
				iv[1] = _def_sint(u, 1);
			}
			memcpy(dst, iv, 8);
		} break;
		case SL::TYPE_IVEC3: {
			int32_t iv[3] = {};
			if (pack_val.get_type() == Variant::VECTOR3I) {
				Vector3i v = pack_val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 12);
		} break;
		case SL::TYPE_IVEC4: {
			int32_t iv[4] = {};
			if (pack_val.get_type() == Variant::VECTOR4I) {
				Vector4i v = pack_val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
				iv[3] = v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 16);
		} break;
		case SL::TYPE_UVEC2: {
			uint32_t uv[2];
			if (pack_val.get_type() == Variant::VECTOR2I) {
				Vector2i v = pack_val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
			} else {
				uv[0] = _def_uint(u, 0);
				uv[1] = _def_uint(u, 1);
			}
			memcpy(dst, uv, 8);
		} break;
		case SL::TYPE_UVEC3: {
			uint32_t uv[3] = {};
			if (pack_val.get_type() == Variant::VECTOR3I) {
				Vector3i v = pack_val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 12);
		} break;
		case SL::TYPE_UVEC4: {
			uint32_t uv[4] = {};
			if (pack_val.get_type() == Variant::VECTOR4I) {
				Vector4i v = pack_val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
				uv[3] = (uint32_t)v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 16);
		} break;
		case SL::TYPE_BVEC2: {
			uint32_t bv[2] = { _def_bool(u, 0), _def_bool(u, 1) };
			memcpy(dst, bv, 8);
		} break;
		case SL::TYPE_BVEC3: {
			uint32_t bv[3] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2) };
			memcpy(dst, bv, 12);
		} break;
		case SL::TYPE_BVEC4: {
			uint32_t bv[4] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2), _def_bool(u, 3) };
			memcpy(dst, bv, 16);
		} break;
		case SL::TYPE_MAT2: {
			// std140: mat2 = 2 column vec2s, each padded to vec4 (2x16 = 32 bytes).
			float m[8] = {};
			if (pack_val.get_type() == Variant::TRANSFORM2D) {
				Transform2D t = pack_val;
				m[0] = (float)t[0].x;
				m[1] = (float)t[0].y;
				m[4] = (float)t[1].x;
				m[5] = (float)t[1].y;
			} else {
				for (int i = 0; i < 4; i++) {
					m[(i / 2) * 4 + (i % 2)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 32);
		} break;
		case SL::TYPE_MAT3: {
			// std140: mat3 = 3 column vec3s, each padded to vec4 (3x16 = 48 bytes).
			float m[12] = {};
			if (pack_val.get_type() == Variant::BASIS) {
				Basis b = pack_val;
				for (int col = 0; col < 3; col++) {
					Vector3 c = b.get_column(col);
					m[col * 4 + 0] = (float)c.x;
					m[col * 4 + 1] = (float)c.y;
					m[col * 4 + 2] = (float)c.z;
				}
			} else {
				for (int i = 0; i < 9; i++) {
					m[(i / 3) * 4 + (i % 3)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 48);
		} break;
		case SL::TYPE_MAT4: {
			// std140: mat4 = 4 column vec4s (4x16 = 64 bytes).
			float m[16] = {};
			if (pack_val.get_type() == Variant::PROJECTION) {
				Projection p = pack_val;
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else if (pack_val.get_type() == Variant::TRANSFORM3D) {
				Transform3D t = pack_val;
				Projection p(t);
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else {
				for (int i = 0; i < 16; i++) {
					m[i] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 64);
		} break;
		default:
			break;
	}
}

static void pack_uniform_array(const ShaderLanguage::ShaderNode::Uniform &u, const Variant &val, uint8_t *dst) {
	if (u.array_size <= 0) {
		pack_uniform(u, val, dst);
		return;
	}

	Variant pack_val = _rt_uniform_pack_value(u, val);
	bool linear_color = u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SOURCE_COLOR;

	switch (u.type) {
		case ShaderLanguage::TYPE_BOOL: {
			uint32_t *gui = (uint32_t *)dst;
			PackedInt32Array ba = pack_val;
			for (int i = 0; i < ba.size(); i++) {
				ba.set(i, ba[i] ? 1 : 0);
			}
			write_array_std140<int32_t>(ba, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_BVEC2: {
			uint32_t *gui = (uint32_t *)dst;
			PackedInt32Array ba = convert_array_std140<Vector2i, int32_t>(pack_val);
			for (int i = 0; i < ba.size(); i++) {
				ba.set(i, ba[i] ? 1 : 0);
			}
			write_array_std140<Vector2i>(ba, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_BVEC3: {
			uint32_t *gui = (uint32_t *)dst;
			PackedInt32Array ba = convert_array_std140<Vector3i, int32_t>(pack_val);
			for (int i = 0; i < ba.size(); i++) {
				ba.set(i, ba[i] ? 1 : 0);
			}
			write_array_std140<Vector3i>(ba, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_BVEC4: {
			uint32_t *gui = (uint32_t *)dst;
			PackedInt32Array ba = convert_array_std140<Vector4i, int32_t>(pack_val);
			for (int i = 0; i < ba.size(); i++) {
				ba.set(i, ba[i] ? 1 : 0);
			}
			write_array_std140<Vector4i>(ba, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_INT: {
			int32_t *gui = (int32_t *)dst;
			const PackedInt32Array &iv = pack_val;
			write_array_std140<int32_t>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_IVEC2: {
			int32_t *gui = (int32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector2i, int32_t>(pack_val);
			write_array_std140<Vector2i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_IVEC3: {
			int32_t *gui = (int32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector3i, int32_t>(pack_val);
			write_array_std140<Vector3i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_IVEC4: {
			int32_t *gui = (int32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector4i, int32_t>(pack_val);
			write_array_std140<Vector4i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_UINT: {
			uint32_t *gui = (uint32_t *)dst;
			const PackedInt32Array &iv = pack_val;
			write_array_std140<uint32_t>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_UVEC2: {
			uint32_t *gui = (uint32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector2i, int32_t>(pack_val);
			write_array_std140<Vector2i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_UVEC3: {
			uint32_t *gui = (uint32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector3i, int32_t>(pack_val);
			write_array_std140<Vector3i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_UVEC4: {
			uint32_t *gui = (uint32_t *)dst;
			const PackedInt32Array &iv = convert_array_std140<Vector4i, int32_t>(pack_val);
			write_array_std140<Vector4i>(iv, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_FLOAT: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = pack_val;
			write_array_std140<float>(a, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_VEC2: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = convert_array_std140<Vector2, float>(pack_val);
			write_array_std140<Vector2>(a, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_VEC3: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = convert_array_std140<Vector3, float>(pack_val, linear_color);
			write_array_std140<Vector3>(a, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_VEC4: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = convert_array_std140<Vector4, float>(pack_val, linear_color);
			write_array_std140<Vector4>(a, gui, u.array_size, 4);
		} break;
		case ShaderLanguage::TYPE_MAT2: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = pack_val;
			const int s = a.size();
			for (int i = 0, j = 0; i < u.array_size * 4; i += 4, j += 8) {
				if (i + 3 < s) {
					gui[j] = a[i];
					gui[j + 1] = a[i + 1];
					gui[j + 4] = a[i + 2];
					gui[j + 5] = a[i + 3];
				} else {
					gui[j] = 1;
					gui[j + 1] = 0;
					gui[j + 4] = 0;
					gui[j + 5] = 1;
				}
				gui[j + 2] = 0;
				gui[j + 3] = 0;
				gui[j + 6] = 0;
				gui[j + 7] = 0;
			}
		} break;
		case ShaderLanguage::TYPE_MAT3: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = convert_array_std140<Basis, float>(pack_val);
			const Basis default_basis;
			const int s = a.size();
			for (int i = 0, j = 0; i < u.array_size * 9; i += 9, j += 12) {
				if (i + 8 < s) {
					gui[j] = a[i];
					gui[j + 1] = a[i + 1];
					gui[j + 2] = a[i + 2];
					gui[j + 3] = 0;
					gui[j + 4] = a[i + 3];
					gui[j + 5] = a[i + 4];
					gui[j + 6] = a[i + 5];
					gui[j + 7] = 0;
					gui[j + 8] = a[i + 6];
					gui[j + 9] = a[i + 7];
					gui[j + 10] = a[i + 8];
					gui[j + 11] = 0;
				} else {
					convert_item_std140(default_basis, gui + j);
				}
			}
		} break;
		case ShaderLanguage::TYPE_MAT4: {
			float *gui = reinterpret_cast<float *>(dst);
			const PackedFloat32Array &a = convert_array_std140<Projection, float>(pack_val);
			write_array_std140<Projection>(a, gui, u.array_size, 16);
		} break;
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Procedural geometry processing
// ---------------------------------------------------------------------------

void RenderRaytracing::update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list) {
	// Pack AABB data into a byte buffer.
	Vector<uint8_t> aabb_bytes;
	uint32_t aabb_count = 1;

	const bool has_aabb_data = p_state->aabb_data.size() >= 6 && (p_state->aabb_data.size() % 6) == 0;
	if (has_aabb_data) {
		aabb_count = p_state->aabb_data.size() / 6;
		aabb_bytes.resize(p_state->aabb_data.size() * sizeof(float));
		memcpy(aabb_bytes.ptrw(), p_state->aabb_data.ptr(), aabb_bytes.size());
	} else if (!p_state->culling_aabb.has_surface()) {
		if (p_state->blas.is_valid()) {
			_rt_free_acceleration_structure_if_alive(p_state->blas);
			p_state->blas = RID();
		}
		if (p_state->gpu_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->gpu_buffer);
			p_state->gpu_buffer = RID();
		}
		p_state->gpu_buffer_capacity = 0;
		p_state->gpu_buffer_address = 0;
		p_state->aabb_count = 0;
		p_state->dirty = false;
		return;
	} else {
		const AABB &a = p_state->culling_aabb;
		float single[6] = {
			(float)a.position.x, (float)a.position.y, (float)a.position.z,
			(float)(a.position.x + a.size.x), (float)(a.position.y + a.size.y), (float)(a.position.z + a.size.z)
		};
		aabb_bytes.resize(sizeof(single));
		memcpy(aabb_bytes.ptrw(), single, sizeof(single));
	}

	uint32_t required_bytes = aabb_bytes.size();
	bool needs_new_blas = false;

	// Grow-only: only recreate the buffer when capacity is exceeded or count changed.
	if (required_bytes > p_state->gpu_buffer_capacity || aabb_count != p_state->aabb_count) {
		if (p_state->blas.is_valid()) {
			_rt_free_acceleration_structure_if_alive(p_state->blas);
		}
		if (p_state->gpu_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->gpu_buffer);
		}
		p_state->gpu_buffer = RD::get_singleton()->storage_buffer_create(required_bytes, aabb_bytes,
				0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		p_state->gpu_buffer_capacity = required_bytes;
		p_state->aabb_count = aabb_count;
		needs_new_blas = true;
	} else {
		// Buffer is large enough -- just update contents.
		RD::get_singleton()->buffer_update(p_state->gpu_buffer, 0, required_bytes, aabb_bytes.ptr());
		needs_new_blas = !p_state->blas.is_valid();
	}

	if (needs_new_blas) {
		ERR_FAIL_COND(!p_state->gpu_buffer.is_valid());

		RD::AccelerationStructureGeometry geom;
		geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		geom.geometry.aabbs.buffer = p_state->gpu_buffer;
		geom.geometry.aabbs.count = aabb_count;
		geom.geometry.aabbs.stride = 24; // VkAabbPositionsKHR: two float3 (min, max).
		p_state->blas = RD::get_singleton()->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}

	// BDA for shader access.
	if (p_state->expose_bounds && p_state->gpu_buffer.is_valid()) {
		p_state->gpu_buffer_address = RD::get_singleton()->buffer_get_device_address(p_state->gpu_buffer);
	} else {
		p_state->gpu_buffer_address = 0;
	}

	if (p_state->blas.is_valid()) {
		r_dirty_blas_list.push_back(p_state->blas);
	}
}

// ---------------------------------------------------------------------------
// Material processing
// ---------------------------------------------------------------------------

RTMaterialData *RenderRaytracing::process_material(RID p_material_rid, uint16_t p_material_invalidation_counter, uint32_t p_shader_slot_override) {
	// Static default material for invalid/null materials
	static RTMaterialData s_default_mat;
	static bool s_default_mat_initialized = false;
	if (!s_default_mat_initialized) {
		s_default_mat.data.albedo_color[0] = 1.0f;
		s_default_mat.data.albedo_color[1] = 1.0f;
		s_default_mat.data.albedo_color[2] = 1.0f;
		s_default_mat.data.albedo_color[3] = 1.0f;
		s_default_mat.data.emission_color[0] = 0.0f;
		s_default_mat.data.emission_color[1] = 0.0f;
		s_default_mat.data.emission_color[2] = 0.0f;
		s_default_mat.data.emission_strength = 0.0f;
		s_default_mat.data.roughness = 1.0f;
		s_default_mat.data.specular = 0.5f;
		s_default_mat.data.ao_strength = 1.0f;
		s_default_mat.data.uv1_scale[0] = 1.0f;
		s_default_mat.data.uv1_scale[1] = 1.0f;
		s_default_mat.data.uv1_offset[0] = 0.0f;
		s_default_mat.data.uv1_offset[1] = 0.0f;
		s_default_mat_initialized = true;
	}

	if (!p_material_rid.is_valid()) {
		return &s_default_mat;
	}

	// Cache lookup
	uint32_t mat_idx = get_rid_index(p_material_rid);
	uint32_t mat_version = get_rid_version(p_material_rid);
	RTMaterialCacheEntry *entry = get_material_cache_entry(mat_idx);
	const bool procedural_variant = p_shader_slot_override != UINT32_MAX;
	RTMaterialData *&cached_ptr = procedural_variant ? entry->procedural_ptr : entry->ptr;
	uint32_t &cached_last_used_frame = procedural_variant ? entry->procedural_last_used_frame : entry->last_used_frame;
	uint16_t &cached_counter = procedural_variant ? entry->procedural_cached_counter : entry->cached_counter;
	uint32_t &cached_rid_version = procedural_variant ? entry->procedural_cached_rid_version : entry->cached_rid_version;

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !cached_ptr ||
			cached_rid_version != mat_version ||
			cached_counter != p_material_invalidation_counter ||
			(procedural_variant && entry->procedural_cached_sbt_offset != p_shader_slot_override);
	if (!needs_refresh && cached_ptr->uses_global_texture_uniforms) {
		needs_refresh = true;
	}

	if (!needs_refresh) {
		cached_last_used_frame = current_frame;
		if (!procedural_variant && cached_ptr->is_custom_shader) {
			uint32_t shader_id = RendererRD::MaterialStorage::get_singleton()->material_get_shader_id(p_material_rid);
			uint32_t old_sbt = cached_ptr->rt_sbt_offset;
			uint32_t new_sbt = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid);
			cached_ptr->rt_sbt_offset = new_sbt;
			// HG slot change invalidates cached UBO layout / BDA.
			if (old_sbt != new_sbt) {
				needs_refresh = true;
			}
		}
		if (!needs_refresh) {
			if (cached_ptr->uniform_buffer.is_valid()) {
				material_ubo_dependencies.push_back(cached_ptr->uniform_buffer);
			}
			return cached_ptr;
		}
	}

	// Cache miss - need to rebuild material
	if (!cached_ptr) {
		cached_ptr = memnew(RTMaterialData);
	} else {
		if (cached_ptr->uniform_buffer.is_valid()) {
			RD::get_singleton()->free_rid(cached_ptr->uniform_buffer);
			cached_ptr->uniform_buffer = RID();
		}
		if (cached_ptr->uniform_pool_slot != UINT32_MAX) {
			mat_ubo_pool_release(cached_ptr->uniform_pool_slot);
			cached_ptr->uniform_pool_slot = UINT32_MAX;
		}
	}

	RTMaterialData *mat_data = cached_ptr;
	RT_MaterialData &mat = mat_data->data;

	// Initialize defaults
	mat.albedo_color[0] = 1.0f;
	mat.albedo_color[1] = 1.0f;
	mat.albedo_color[2] = 1.0f;
	mat.albedo_color[3] = 1.0f;
	mat.emission_color[0] = 0.0f;
	mat.emission_color[1] = 0.0f;
	mat.emission_color[2] = 0.0f;
	mat.emission_strength = 0.0f;
	mat.metallic = 0.0f;
	mat.roughness = 1.0f;
	mat.specular = 0.5f;
	mat.ao_strength = 1.0f;
	mat.flags = 0;
	mat.albedo_texture_idx = 0;
	mat.normal_texture_idx = 0;
	mat.orm_texture_idx = 0;
	mat.emission_texture_idx = 0;
	mat.metallic_texture_idx = 0;
	mat.uv1_scale[0] = 1.0f;
	mat.uv1_scale[1] = 1.0f;
	mat.uv1_offset[0] = 0.0f;
	mat.uv1_offset[1] = 0.0f;
	mat.normal_map_depth = 1.0f;
	mat.alpha_scissor_threshold = 0.5f;
	mat.alpha_hash_scale = 0.0f;
	mat._pad0 = 0;
	mat.uniform_address = 0;
	mat_data->rt_sbt_offset = 0;
	mat_data->is_custom_shader = false;
	mat_data->uses_global_texture_uniforms = false;

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const String rt_shader_code = material_storage->material_get_shader_code_rt(p_material_rid);
	const bool generated_base_material = rt_shader_code.begins_with("// NOTE: Shader automatically converted from");
	const bool shader_uses_alpha_hash = rt_shader_code.find("ALPHA_HASH_SCALE") >= 0;
	const bool generated_vertex_color_albedo = generated_base_material && rt_shader_code.find("albedo_tex *= COLOR") >= 0;
	const bool generated_vertex_color_srgb = generated_base_material && rt_shader_code.find("COLOR.rgb = mix") >= 0;
	const bool generated_repeat_disabled = generated_base_material && rt_shader_code.find("repeat_disable") >= 0;

	// Helper lambda to get texture from material parameter
	// p_srgb should be true for color textures (albedo, emission) that need sRGB->linear conversion
	auto get_material_texture = [&](const StringName &p_param, bool p_srgb = false) -> RID {
		Variant tex_var = material_storage->material_get_param(p_material_rid, p_param);
		if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
			RID tex_rid = tex_var;
			if (tex_rid.is_valid()) {
				return texture_storage->texture_get_rd_texture(tex_rid, p_srgb);
			}
		}
		return RID();
	};
	auto apply_emission_color_param = [&](const StringName &p_param) -> bool {
		Variant emission_var = material_storage->material_get_param(p_material_rid, p_param);
		if (emission_var.get_type() == Variant::COLOR) {
			Color c = ((Color)emission_var).srgb_to_linear();
			mat.emission_color[0] = c.r;
			mat.emission_color[1] = c.g;
			mat.emission_color[2] = c.b;
			return true;
		}
		if (emission_var.get_type() == Variant::VECTOR3) {
			Vector3 v = emission_var;
			mat.emission_color[0] = v.x;
			mat.emission_color[1] = v.y;
			mat.emission_color[2] = v.z;
			return true;
		}
		return false;
	};
	auto apply_emission_energy_param = [&](const StringName &p_param) -> bool {
		Variant emission_energy_var = material_storage->material_get_param(p_material_rid, p_param);
		if (emission_energy_var.get_type() == Variant::FLOAT) {
			mat.emission_strength = emission_energy_var;
			return true;
		}
		return false;
	};
	auto texture_channel_from_mask = [](const Variant &p_value, uint32_t p_default) -> uint32_t {
		if (p_value.get_type() != Variant::VECTOR4) {
			return p_default;
		}
		Vector4 mask = p_value;
		if (Math::is_equal_approx(mask.x, 0.333333f) &&
				Math::is_equal_approx(mask.y, 0.333333f) &&
				Math::is_equal_approx(mask.z, 0.333333f)) {
			return 4;
		}
		uint32_t channel = 0;
		real_t best = mask.x;
		if (mask.y > best) {
			best = mask.y;
			channel = 1;
		}
		if (mask.z > best) {
			best = mask.z;
			channel = 2;
		}
		if (mask.w > best) {
			channel = 3;
		}
		return channel;
	};
	auto get_texture_channel_param = [&](const StringName &p_param, uint32_t p_default) -> uint32_t {
		return texture_channel_from_mask(material_storage->material_get_param(p_material_rid, p_param), p_default);
	};

	// Textures
	// Albedo is a color texture - needs sRGB->linear conversion
	RID albedo_rd = get_material_texture("texture_albedo", true);
	if (!albedo_rd.is_valid()) {
		albedo_rd = get_material_texture("main_texture", true);
	}
	if (albedo_rd.is_valid()) {
		mat.albedo_texture_idx = bindless_block->add_texture(albedo_rd);
	}
	if (generated_vertex_color_albedo) {
		mat.flags |= RT_MAT_FLAG_VERTEX_COLOR_ALBEDO;
	}
	if (generated_vertex_color_srgb) {
		mat.flags |= RT_MAT_FLAG_VERTEX_COLOR_SRGB;
	}

	RID normal_rd = get_material_texture("texture_normal");
	if (normal_rd.is_valid()) {
		mat.normal_texture_idx = bindless_block->add_texture(normal_rd);
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;

		Variant normal_scale_var = material_storage->material_get_param(p_material_rid, "normal_scale");
		if (normal_scale_var.get_type() == Variant::FLOAT) {
			mat.normal_map_depth = normal_scale_var;
		}
	}

	RID orm_rd = get_material_texture("texture_orm");
	if (orm_rd.is_valid()) {
		mat.orm_texture_idx = bindless_block->add_texture(orm_rd);
		mat.flags |= RT_MAT_FLAG_ORM_TEXTURE;
	} else {
		RID roughness_rd = get_material_texture("texture_roughness");
		if (roughness_rd.is_valid()) {
			mat.orm_texture_idx = bindless_block->add_texture(roughness_rd);
			mat.flags |= RT_MAT_FLAG_ROUGHNESS_TEXTURE;
			uint32_t roughness_channel = get_texture_channel_param("roughness_texture_channel", 0);
			if (rt_shader_code.find("roughness_texture_channel = vec4(0.0, 1.0, 0.0, 0.0)") >= 0) {
				roughness_channel = 1;
			} else if (rt_shader_code.find("roughness_texture_channel = vec4(0.0, 0.0, 1.0, 0.0)") >= 0) {
				roughness_channel = 2;
			} else if (rt_shader_code.find("roughness_texture_channel = vec4(0.0, 0.0, 0.0, 1.0)") >= 0) {
				roughness_channel = 3;
			} else if (rt_shader_code.find("roughness_texture_channel = vec4(0.333333, 0.333333, 0.333333, 0.0)") >= 0) {
				roughness_channel = 4;
			}
			mat.flags |= roughness_channel << RT_MAT_FLAG_ROUGHNESS_CHANNEL_SHIFT;
		}
		RID metallic_rd = get_material_texture("texture_metallic");
		if (metallic_rd.is_valid()) {
			mat.metallic_texture_idx = bindless_block->add_texture(metallic_rd);
			mat.flags |= RT_MAT_FLAG_METALLIC_TEXTURE;
			uint32_t metallic_channel = get_texture_channel_param("metallic_texture_channel", 0);
			mat.flags |= metallic_channel << RT_MAT_FLAG_METALLIC_CHANNEL_SHIFT;
		}
	}

	// Emission is a color texture - needs sRGB->linear conversion
	RID emission_rd = get_material_texture("texture_emission", true);
	if (!emission_rd.is_valid()) {
		emission_rd = get_material_texture("emission_texture", true);
	}
	if (!emission_rd.is_valid()) {
		emission_rd = get_material_texture("emissive_texture", true);
	}
	if (!emission_rd.is_valid()) {
		emission_rd = get_material_texture("texture_emissive", true);
	}
	if (emission_rd.is_valid()) {
		mat.emission_texture_idx = bindless_block->add_texture(emission_rd);
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
		// Set sensible defaults for emission when texture is present
		mat.emission_color[0] = 1.0f;
		mat.emission_color[1] = 1.0f;
		mat.emission_color[2] = 1.0f;
		mat.emission_strength = 1.0f;
	}

	// Material properties
	// Colors declared with source_color in Godot shaders are stored in sRGB;
	// material_get_param returns the raw sRGB value, so we convert to linear here.
	Variant albedo_var = material_storage->material_get_param(p_material_rid, "albedo");
	if (!procedural_variant && albedo_var.get_type() == Variant::COLOR && generated_base_material) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
		mat_data->rt_sbt_offset = 0;
		mat_data->is_custom_shader = false;
	} else {
		mat_data->is_custom_shader = true;
		if (procedural_variant) {
			mat_data->rt_sbt_offset = p_shader_slot_override;
		} else {
			uint32_t shader_id = material_storage->material_get_shader_id(p_material_rid);
			mat_data->rt_sbt_offset = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid);
		}
		if (mat_data->rt_sbt_offset == 0) {
			mat_data->is_custom_shader = false;
		} else {
			mat.flags |= RT_MAT_FLAG_CUSTOM_SHADER;
		}

		const SceneShaderRaytracing::CustomShaderEntry *cse =
				SceneShaderRaytracing::get_singleton()->get_custom_shader_entry(mat_data->rt_sbt_offset);
		if (cse && cse->uses_alpha_clip) {
			mat.flags |= RT_MAT_FLAG_CUSTOM_ALPHA_CLIP;
		}
		if (cse && cse->uniform_total_size > 0) {
			Vector<uint8_t> ubo_data;
			ubo_data.resize(cse->uniform_total_size);
			memset(ubo_data.ptrw(), 0, cse->uniform_total_size);

			for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : cse->uniforms) {
				const ShaderLanguage::ShaderNode::Uniform &u = kv.value;
				if (ShaderLanguage::is_sampler_type(u.type)) {
					continue;
				}
				if (u.order < 0 || u.order >= (int)cse->uniform_offsets.size()) {
					continue;
				}

				uint32_t offset = cse->uniform_offsets[u.order];
				uint32_t size = _rt_uniform_std140_size(u);
				if (offset + size > cse->uniform_total_size) {
					continue;
				}

				uint8_t *dst = ubo_data.ptrw() + offset;

				if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
					int32_t idx = material_storage->global_shader_uniform_get_buffer_index(kv.key);
					uint32_t uidx = (idx >= 0) ? (uint32_t)idx : 0;
					memcpy(dst, &uidx, sizeof(uint32_t));
				} else {
					Variant val = material_storage->material_get_param(p_material_rid, kv.key);
					pack_uniform_array(u, val, dst);
				}
			}

			RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
			auto custom_texture_to_bindless_index = [&](const Variant &p_texture_value, bool p_use_color) -> uint32_t {
				if (p_texture_value.get_type() == Variant::OBJECT || p_texture_value.get_type() == Variant::RID) {
					RID tex_rid = p_texture_value;
					if (tex_rid.is_valid()) {
						RID rd_tex = ts->texture_get_rd_texture(tex_rid, p_use_color);
						if (rd_tex.is_valid()) {
							return bindless_block->add_texture(rd_tex);
						}
					}
				}
				return 0;
			};
			auto custom_default_texture_for_hint = [&](ShaderLanguage::ShaderNode::Uniform::Hint p_hint) -> RID {
				using Hint = ShaderLanguage::ShaderNode::Uniform::Hint;
				switch (p_hint) {
					case Hint::HINT_DEFAULT_BLACK:
						return ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
					case Hint::HINT_DEFAULT_TRANSPARENT:
						return ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_TRANSPARENT);
					case Hint::HINT_NORMAL:
						return ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
					case Hint::HINT_ANISOTROPY:
						return ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_ANISO);
					default:
						return ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
				}
			};
			for (int ti = 0; ti < cse->texture_uniforms.size(); ti++) {
				const SceneShaderRaytracing::TextureUniformInfo &tui = cse->texture_uniforms[ti];
				const int texture_count = tui.array_size > 0 ? tui.array_size : 1;
				Variant texture_param;
				Array texture_array;
				if (!tui.is_global) {
					texture_param = material_storage->material_get_param(p_material_rid, tui.name);
					if (texture_param.get_type() == Variant::ARRAY) {
						texture_array = texture_param;
					}
				}

				for (int texture_index = 0; texture_index < texture_count; texture_index++) {
					uint32_t bindless_idx = 0;

					if (tui.is_global) {
						mat_data->uses_global_texture_uniforms = true;
						RID tex_rid = material_storage->global_shader_uniform_get_texture(tui.name);
						if (tex_rid.is_valid()) {
							RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
							if (rd_tex.is_valid()) {
								bindless_idx = bindless_block->add_texture(rd_tex);
							}
						}
					} else {
						Variant texture_value = texture_param;
						if (tui.array_size > 0) {
							texture_value = texture_index < texture_array.size() ? texture_array[texture_index] : Variant();
						}
						bindless_idx = custom_texture_to_bindless_index(texture_value, tui.use_color);

						if (bindless_idx == 0) {
							RID tex_rid = material_storage->material_get_shader_default_texture_parameter(p_material_rid, tui.name, texture_index);
							if (tex_rid.is_valid()) {
								RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
								if (rd_tex.is_valid()) {
									bindless_idx = bindless_block->add_texture(rd_tex);
								}
							}
						}
					}

					if (bindless_idx == 0 && tui.hint != ShaderLanguage::ShaderNode::Uniform::HINT_NONE) {
						RID default_tex = custom_default_texture_for_hint(tui.hint);
						if (default_tex.is_valid()) {
							bindless_idx = bindless_block->add_texture(default_tex);
						}
					}

					uint32_t buffer_offset = tui.buffer_offset + uint32_t(texture_index) * uint32_t(tui.array_size > 0 ? 16 : 4);
					if (buffer_offset + 4 <= cse->uniform_total_size) {
						memcpy(ubo_data.ptrw() + buffer_offset, &bindless_idx, 4);
					}
				}
			}

			// Try the suballoc pool first. Common materials (UBO <= slot size)
			// just buffer_update an existing slot - O(1), no driver allocation,
			// no per-frame storage_buffer_create cost.
			bool used_pool = false;
			if (cse->uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE) {
				if (mat_data->uniform_pool_slot == UINT32_MAX) {
					mat_data->uniform_pool_slot = mat_ubo_pool_allocate();
				}
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					// Transitioning from a dedicated buffer back into the pool.
					if (mat_data->uniform_buffer.is_valid()) {
						RD::get_singleton()->free_rid(mat_data->uniform_buffer);
						mat_data->uniform_buffer = RID();
					}
					mat_ubo_pool_update(mat_data->uniform_pool_slot, ubo_data.ptr(), cse->uniform_total_size);
					mat.uniform_address = mat_ubo_pool_get_address(mat_data->uniform_pool_slot);
					used_pool = true;
				}
			}

			if (!used_pool) {
				// Oversized or pool exhausted: dedicated per-material buffer.
				// This is the slow path: a per-material storage_buffer_create on
				// every rebuild. Warn once so it's visible in the log; the fix is
				// either to shrink the material's uniform footprint below
				// MAT_UBO_POOL_SLOT_SIZE or to grow the pool slot/capacity.
				const char *reason = (cse->uniform_total_size > MAT_UBO_POOL_SLOT_SIZE)
						? "uniform size exceeds slot"
						: "pool exhausted";
				WARN_PRINT_ONCE(vformat(
						"RT Material UBO falling back to dedicated buffer (%s): "
						"sbt_offset=%u, uniform_total_size=%u, slot_size=%u.",
						String(reason), mat_data->rt_sbt_offset,
						cse->uniform_total_size, MAT_UBO_POOL_SLOT_SIZE));

				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(mat_data->uniform_pool_slot);
					mat_data->uniform_pool_slot = UINT32_MAX;
				}
				if (mat_data->uniform_buffer.is_valid()) {
					RD::get_singleton()->free_rid(mat_data->uniform_buffer);
				}
				mat_data->uniform_buffer = RD::get_singleton()->storage_buffer_create(cse->uniform_total_size, ubo_data, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
				RD::get_singleton()->set_resource_name(mat_data->uniform_buffer, String("RT Material UBO [sbt=") + itos(mat_data->rt_sbt_offset) + "]");
				mat.uniform_address = RD::get_singleton()->buffer_get_device_address(mat_data->uniform_buffer);
			}
		}
	}

	Variant metallic_var = material_storage->material_get_param(p_material_rid, "metallic");
	if (metallic_var.get_type() == Variant::FLOAT) {
		mat.metallic = metallic_var;
	}

	Variant roughness_var = material_storage->material_get_param(p_material_rid, "roughness");
	if (roughness_var.get_type() == Variant::FLOAT) {
		mat.roughness = roughness_var;
	}

	Variant specular_var = material_storage->material_get_param(p_material_rid, "specular");
	if (specular_var.get_type() == Variant::FLOAT) {
		mat.specular = specular_var;
	}
	if (rt_shader_code.find("SPECULAR_DISABLED") >= 0 || rt_shader_code.find("specular_disabled") >= 0) {
		mat.specular = 0.0f;
	}

	Variant alpha_scissor_var = material_storage->material_get_param(p_material_rid, "alpha_scissor_threshold");
	if (alpha_scissor_var.get_type() == Variant::FLOAT) {
		mat.alpha_scissor_threshold = CLAMP(float(alpha_scissor_var), 0.0f, 1.0f);
	}

	Variant alpha_hash_var = material_storage->material_get_param(p_material_rid, "alpha_hash_scale");
	if (alpha_hash_var.get_type() == Variant::FLOAT) {
		mat.alpha_hash_scale = MAX(float(alpha_hash_var), 0.0f);
		if (shader_uses_alpha_hash && mat.alpha_hash_scale > 0.0f) {
			mat.flags |= RT_MAT_FLAG_ALPHA_HASH;
		}
	}

	bool has_emission_color = apply_emission_color_param("emission");
	if (!has_emission_color) {
		has_emission_color = apply_emission_color_param("emission_color");
	}
	if (!has_emission_color) {
		has_emission_color = apply_emission_color_param("emissive");
	}
	if (!has_emission_color) {
		has_emission_color = apply_emission_color_param("emissive_color");
	}

	bool has_emission_energy = apply_emission_energy_param("emission_energy");
	if (!has_emission_energy) {
		has_emission_energy = apply_emission_energy_param("emission_strength");
	}
	if (!has_emission_energy) {
		has_emission_energy = apply_emission_energy_param("emissive_energy");
	}
	if (!has_emission_energy) {
		has_emission_energy = apply_emission_energy_param("emissive_strength");
	}
	if (has_emission_color && !has_emission_energy && mat.emission_strength == 0.0f) {
		mat.emission_strength = 1.0f;
	}

	// UV1 scale and offset (vec3 in Godot, we only use xy).
	Variant uv1_scale_var = material_storage->material_get_param(p_material_rid, "uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 s = uv1_scale_var;
		mat.uv1_scale[0] = s.x;
		mat.uv1_scale[1] = s.y;
	}

	Variant uv1_offset_var = material_storage->material_get_param(p_material_rid, "uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
	}

	// Point filtering: check if material requests nearest filtering (e.g. pixel art).
	// BaseMaterial3D exposes this as "texture_filter" int param (0=nearest, 1=linear, etc.).
	Variant filter_var = material_storage->material_get_param(p_material_rid, "texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		// 0 = TEXTURE_FILTER_NEAREST, 2 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		// 4 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}
	if (generated_repeat_disabled) {
		mat.flags |= RT_MAT_FLAG_REPEAT_DISABLED;
	}

	// Update cache entry
	cached_counter = p_material_invalidation_counter;
	cached_rid_version = mat_version;
	cached_last_used_frame = current_frame;
	if (procedural_variant) {
		entry->procedural_cached_sbt_offset = p_shader_slot_override;
	}

	if (mat_data->uniform_buffer.is_valid()) {
		material_ubo_dependencies.push_back(mat_data->uniform_buffer);
	}

	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

void RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	for (const RID &blas_rid : p_dirty_blas_list) {
		if (_rt_acceleration_structure_is_alive(blas_rid)) {
			RD::get_singleton()->blas_build(blas_rid);
		}
	}

	for (const RID &blas_rid : p_dirty_blas_update_list) {
		if (_rt_acceleration_structure_is_alive(blas_rid)) {
			RD::get_singleton()->blas_update(blas_rid);
		}
	}

	uint32_t valid_instance_count = 0;
	for (const RID &blas_rid : blass) {
		if (_rt_acceleration_structure_is_alive(blas_rid)) {
			valid_instance_count++;
		}
	}

	uint32_t needed = MAX(valid_instance_count, (uint32_t)1);
	if (!p_state->tlas.is_valid() || needed > p_state->tlas_max_instances) {
		if (p_state->tlas.is_valid()) {
			_rt_free_acceleration_structure_if_alive(p_state->tlas);
		}
		p_state->tlas_max_instances = needed * 2;
		p_state->tlas = RD::get_singleton()->tlas_create(p_state->tlas_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->tlas, "RT TLAS");
	}

	LocalVector<RD::AccelerationStructureInstance> instances;
	instances.resize(valid_instance_count);
	uint32_t dst_idx = 0;
	for (uint32_t i = 0; i < blass.size(); i++) {
		if (!_rt_acceleration_structure_is_alive(blass[i])) {
			continue;
		}
		RD::AccelerationStructureInstance &inst = instances[dst_idx++];
		inst.id = i;
		inst.transform = blas_transforms[i];
		inst.blas = blass[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(instance_flags[i]);
		inst.mask = (i < instance_masks.size()) ? instance_masks[i] : uint8_t(RT_INSTANCE_MASK_VISIBLE | RT_INSTANCE_MASK_SHADOW);
		uint32_t sbt_off = (i < sbt_offsets.size()) ? sbt_offsets[i] : 0;
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(sbt_off));
	}

	RD::get_singleton()->tlas_build(p_state->tlas, instances);
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame().
	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size) {
		if (p_size == 0) {
			return;
		}
		if (p_size > p_capacity) {
			if (p_buffer.is_valid()) {
				RD::get_singleton()->free_rid(p_buffer);
			}
			p_capacity = p_size;
			Vector<uint8_t> init;
			init.resize(p_size);
			memcpy(init.ptrw(), p_data, p_size);
			p_buffer = RD::get_singleton()->storage_buffer_create(p_size, init);
		} else {
			RD::get_singleton()->buffer_update(p_buffer, 0, p_size, p_data);
		}
	};

	update_or_grow(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.ptr(), geometry_data.size() * sizeof(RT_GeometryData));
	update_or_grow(p_state->material_buffer, p_state->material_buffer_capacity,
			material_data.ptr(), material_data.size() * sizeof(RT_MaterialData));
	update_or_grow(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.ptr(), motion_indices.size() * sizeof(int32_t));
	update_or_grow(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.ptr(), motion_transforms.size() * sizeof(RT_InstanceMotionData));
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS builder
// ---------------------------------------------------------------------------

bool RenderRaytracing::_build_merged_mm_blas(
		RID p_mm_rid,
		RID p_mm_gpu_buffer,
		void *p_mesh_surface,
		uint32_t p_mm_count,
		uint32_t p_surface_index,
		uint32_t p_surface_counter,
		RD::ComputeListID p_compute_list,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list,
		RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	if (mesh_storage->mesh_surface_get_primitive(p_mesh_surface) != RSE::PRIMITIVE_TRIANGLES) {
		return false;
	}
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);

	// Skip layouts whose source position stream is not float3.
	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	if (surface_format & (RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES | RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
		return false;
	}

	if (prim_count == 0 || vertex_count == 0) {
		return false;
	}
	static const uint32_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint32_t)GLOBAL_GET("rendering/pathtracer/multimesh_merged_blas_max_triangles");
	const uint64_t total_vertices_u64 = (uint64_t)p_mm_count * vertex_count;
	const uint64_t total_indices_u64 = (uint64_t)p_mm_count * index_count;
	const uint64_t total_primitives_u64 = (uint64_t)p_mm_count * prim_count;
	if (total_primitives_u64 > MM_MERGED_BLAS_MAX_TRIANGLES || total_vertices_u64 > UINT32_MAX || (indexed && total_indices_u64 > UINT32_MAX)) {
		return false; // Too large; fall back to expanded TLAS.
	}
	const uint32_t total_vertices = (uint32_t)total_vertices_u64;
	const uint32_t total_indices = (uint32_t)total_indices_u64;
	const uint32_t total_primitives = (uint32_t)total_primitives_u64;

	RID mesh_rid = mesh_storage->multimesh_get_mesh(p_mm_rid);
	uint32_t mesh_version = get_rid_version(mesh_rid);
	uint64_t cache_key = _rt_history_mix_rid(0x6d6d657267656400ULL, p_mm_rid);
	cache_key = _rt_history_mix_rid(cache_key, mesh_rid);
	cache_key = _rt_history_mix(cache_key, mesh_version);
	cache_key = _rt_history_mix(cache_key, p_surface_index);
	RTMergedMMEntry &entry = merged_mm_cache[cache_key];

	entry.last_used_frame = RSG::rasterizer->get_frame_number();

	bool structure_changed = (entry.last_mm_count != p_mm_count ||
			entry.last_surface_counter != p_surface_counter ||
			entry.last_mesh_version != mesh_version);

	// Switching variants (e.g. mesh switched indexed-ness) requires a new
	// descriptor set since the shader layout differs (binding 4 only exists
	// for MODE_INDEXED).
	if (entry.merge_uniform_set.is_valid() && entry.indexed != indexed) {
		_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
	}
	entry.indexed = indexed;

	if (structure_changed) {
		RD *rd = RD::get_singleton();
		if (entry.blas.is_valid()) {
			_rt_free_acceleration_structure_if_alive(entry.blas);
		}
		_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
		entry.blas_built_once = false;
		entry.last_mm_count = p_mm_count;
		entry.last_surface_counter = p_surface_counter;
		entry.last_mesh_version = mesh_version;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	bool has_tbn = has_normal;

	// Layout of uncompressed vertex buffer: [float3 positions × V] + [packed TBN × V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 × uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 × uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer.
	uint32_t src_tbn_byte_offset = vertex_count * 12; // after all float3 positions

	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if TBN present).
	const uint64_t merged_vtx_bytes_u64 = total_vertices_u64 * 12u + (has_tbn ? total_vertices_u64 * tbn_stride : 0u);
	if (merged_vtx_bytes_u64 > UINT32_MAX) {
		return false;
	}
	uint32_t merged_vtx_bytes = (uint32_t)merged_vtx_bytes_u64;

	// Attribute buffer: attribute_stride bytes × V, replicated N times.
	RTSurfaceData meta_sd;
	_fill_surface_geometry_data(p_mesh_surface, false, &meta_sd);
	uint32_t attrib_stride = meta_sd.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	const uint32_t MIN_ATTR_BYTES = 16;
	const uint64_t merged_attr_bytes_u64 = has_attr ? (total_vertices_u64 * attrib_stride) : MIN_ATTR_BYTES;
	if (merged_attr_bytes_u64 > UINT32_MAX) {
		return false;
	}
	uint32_t merged_attr_bytes = (uint32_t)merged_attr_bytes_u64;

	RD *rd = RD::get_singleton();
	BitField<RD::BufferCreationBits> gpu_buf_flags =
			RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	// --- Grow / allocate merged vertex buffer ---
	if (!entry.merged_vtx_buffer.is_valid() || entry.vtx_capacity_bytes < merged_vtx_bytes) {
		_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
		if (entry.merged_vtx_buffer.is_valid()) {
			rd->free_rid(entry.merged_vtx_buffer);
			entry.merged_vtx_buffer = RID();
		}
		entry.vtx_capacity_bytes = merged_vtx_bytes;
		entry.merged_vtx_buffer = rd->vertex_buffer_create(merged_vtx_bytes, {}, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_vtx_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_vtx_buffer, "RT MM merged vtx [" + uitos(cache_key) + "]");
		entry.blas_built_once = false;
	}

	// --- Grow / allocate merged attribute buffer ---
	if (!entry.merged_attr_buffer.is_valid() || entry.attr_capacity_bytes < merged_attr_bytes) {
		_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
		if (entry.merged_attr_buffer.is_valid()) {
			rd->free_rid(entry.merged_attr_buffer);
			entry.merged_attr_buffer = RID();
		}
		entry.attr_capacity_bytes = merged_attr_bytes;
		entry.merged_attr_buffer = rd->storage_buffer_create(merged_attr_bytes, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_attr_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_attr_buffer, "RT MM merged attr [" + uitos(cache_key) + "]");
	}

	// --- Grow / allocate replicated index buffer ---
	if (indexed) {
		uint32_t needed_idx = total_indices;
		if (!entry.replicated_idx_buffer.is_valid() || entry.idx_capacity < needed_idx) {
			_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
			if (entry.replicated_idx_buffer.is_valid()) {
				rd->free_rid(entry.replicated_idx_buffer);
				entry.replicated_idx_buffer = RID();
			}
			entry.idx_capacity = needed_idx;
			entry.replicated_idx_buffer = rd->index_buffer_create(
					needed_idx, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(!entry.replicated_idx_buffer.is_valid(), false);
			rd->set_resource_name(entry.replicated_idx_buffer, "RT MM replicated idx [" + uitos(cache_key) + "]");
			entry.blas_built_once = false;
		}
	}

	RID src_attr_buf;
	if (has_attr) {
		src_attr_buf = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
		ERR_FAIL_COND_V(!src_attr_buf.is_valid(), false);
	} else {
		src_attr_buf = entry.merged_attr_buffer;
	}

	if (entry.merge_uniform_set.is_valid() &&
			(entry.last_mm_buffer != p_mm_gpu_buffer ||
					entry.last_src_attr_buffer != src_attr_buf ||
					entry.last_src_vtx_buffer != mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface) ||
					entry.last_src_index_buffer != index_buffer)) {
		_rt_free_uniform_set_if_alive(entry.merge_uniform_set);
	}

	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(!vtx_buf.is_valid(), false);

	// --- (Re)build the merge descriptor set ---
	if (!entry.merge_uniform_set.is_valid()) {
		Vector<RD::Uniform> uniforms;
		auto push_buf = [&](uint32_t binding, RID buf) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = binding;
			u.append_id(buf);
			uniforms.push_back(u);
		};
		push_buf(0, entry.merged_vtx_buffer);
		push_buf(1, p_mm_gpu_buffer);
		push_buf(2, entry.merged_attr_buffer);
		push_buf(3, src_attr_buf);
		push_buf(4, vtx_buf);
		if (indexed) {
			ERR_FAIL_COND_V(!index_buffer.is_valid(), false);
			push_buf(5, index_buffer);
			push_buf(6, entry.replicated_idx_buffer);
		}
		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;
		entry.merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[mode], 0);
		ERR_FAIL_COND_V(!entry.merge_uniform_set.is_valid(), false);
		entry.last_mm_buffer = p_mm_gpu_buffer;
		entry.last_src_vtx_buffer = vtx_buf;
		entry.last_src_attr_buffer = src_attr_buf;
		entry.last_src_index_buffer = index_buffer;
	}

	// --- Single merged dispatch: bake vertices + TBN + attributes + (optional) indices ---
	{
		uint32_t mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
		uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
		uint32_t tbn_stride_words = tbn_stride / 4;
		// In the merged vertex buffer the TBN block starts after all N*V float3 positions.
		uint32_t dst_tbn_base_words = total_vertices * 3;

		struct MergePC {
			uint32_t index_count, src_is_16bit;
			uint32_t vertex_count, instance_count;
			uint32_t pos_stride_words;
			uint32_t src_tbn_base_words;
			uint32_t src_tbn_stride_words;
			uint32_t dst_tbn_base_words;
			uint32_t mm_stride, mm_offset;
			uint32_t has_tbn;
			uint32_t attr_stride_words;
		} pc;

		if (indexed) {
			pc.index_count = index_count;
			pc.src_is_16bit = (vertex_count <= 65536) ? 1u : 0u;
		} else {
			pc.index_count = 0;
			pc.src_is_16bit = 0;
		}

		pc.vertex_count = vertex_count;
		pc.instance_count = p_mm_count;
		pc.pos_stride_words = 3; // always float3 (uncompressed check at top)
		pc.src_tbn_base_words = src_tbn_byte_offset / 4;
		pc.src_tbn_stride_words = tbn_stride_words;
		pc.dst_tbn_base_words = dst_tbn_base_words;
		pc.mm_stride = mm_stride;
		pc.mm_offset = mm_cur_offset;
		pc.has_tbn = has_tbn ? 1u : 0u;
		pc.attr_stride_words = has_attr ? (attrib_stride / 4) : 0u;

		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;
		rd->compute_list_bind_compute_pipeline(p_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(p_compute_list, entry.merge_uniform_set, 0);
		rd->compute_list_set_push_constant(p_compute_list, &pc, sizeof(MergePC));

		// Single thread count: every thread processes one vertex (idx < N*V)
		// and -- for MODE_INDEXED -- one output index (idx < N*I) using disjoint
		// destination buffers, so no in-shader barrier is required.
		uint32_t thread_count = total_vertices;
		if (indexed) {
			thread_count = MAX(thread_count, total_indices);
		}
		rd->compute_list_dispatch_threads(p_compute_list, thread_count, 1, 1);
	}

	// --- Build or refit the merged BLAS (uses merged_vtx_buffer for positions) ---
	if (!entry.blas.is_valid()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = entry.merged_vtx_buffer;
		as_geom.geometry.triangles.vertex_stride = 12; // float3, positions section only
		as_geom.geometry.triangles.vertex_count = total_vertices;
		as_geom.geometry.triangles.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		if (indexed) {
			as_geom.geometry.triangles.index_buffer = entry.replicated_idx_buffer;
			as_geom.geometry.triangles.index_count = total_indices;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags =
				RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT |
				RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		entry.blas = rd->blas_create({ &as_geom, 1 }, as_flags);
		ERR_FAIL_COND_V(!entry.blas.is_valid(), false);
		rd->set_resource_name(entry.blas, "RT MM merged BLAS [" + uitos(cache_key) + "]");
	}

	if (!entry.blas_built_once) {
		r_dirty_blas_list.push_back(entry.blas);
		entry.blas_built_once = true;
	} else {
		r_dirty_blas_update_list.push_back(entry.blas);
	}

	// --- Populate r_surf_data from the metadata already computed above, then override merged buffer addresses.
	*r_surf_data = meta_sd;
	r_surf_data->blas = entry.blas;
	r_surf_data->is_compressed = false;
	r_surf_data->aabb_transform = Transform3D();

	RT_GeometryData &geom = r_surf_data->geometry;

	// Point vertex address at merged buffer (positions + TBN all baked world-space).
	geom.vertex_buffer_address = rd->buffer_get_device_address(entry.merged_vtx_buffer);
	r_surf_data->vertex_buffer_dependency = entry.merged_vtx_buffer;
	geom.vertex_count = total_vertices;
	geom.position_stride = 12; // float3, uncompressed
	geom.flags &= ~RT_GEOM_FLAG_COMPRESSED;

	// TBN section starts after all positions in the merged vertex buffer.
	if (has_tbn) {
		uint32_t tbn_base = total_vertices * 12;
		geom.normal_byte_offset = tbn_base;
		geom.normal_stride = tbn_stride;
		if (has_tangent) {
			geom.tangent_byte_offset = tbn_base; // tangent is at +4 from normal within the same stride pair
			geom.tangent_stride = tbn_stride;
		} else {
			geom.tangent_byte_offset = RT_OFFSET_NONE;
			geom.tangent_stride = 0;
		}
	}

	// Point attribute address at fully replicated attribute buffer.
	if (has_attr && entry.merged_attr_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(entry.merged_attr_buffer);
		r_surf_data->attribute_buffer_dependency = entry.merged_attr_buffer;
	} else {
		r_surf_data->attribute_buffer_dependency = RID();
	}

	// Point index address at the replicated (uint32) index buffer.
	if (indexed && entry.replicated_idx_buffer.is_valid()) {
		geom.index_buffer_address = rd->buffer_get_device_address(entry.replicated_idx_buffer);
		r_surf_data->index_buffer_dependency = entry.replicated_idx_buffer;
		geom.index_format = RT_INDEX_FORMAT_UINT32; // always uint32 in replicated buffer
		geom.primitive_count = total_primitives;
	} else {
		r_surf_data->index_buffer_dependency = RID();
		geom.index_buffer_address = 0;
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = total_primitives;
	}
	_register_surface_buffer_dependencies(r_surf_data);

	return true;
}

// ---------------------------------------------------------------------------
// TLAS creation (main entry point per frame)
// ---------------------------------------------------------------------------

_FORCE_INLINE_ static uint32_t _rt_indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

RTViewportState *RenderRaytracing::build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	if (!p_render_data || !p_render_data->rt_instances) {
		return nullptr;
	}

	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return nullptr;
	}

	prepare_frame();
	state->current_history_keys.clear();

	// Builds bundle if needed; live_ready_mask drives TLAS inclusion below.
	SceneShaderRaytracing *rt_shader_singleton = SceneShaderRaytracing::get_singleton();
	rt_shader_singleton->ensure_pipeline_bundle(p_rt_flags);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;
	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

#ifdef TOOLS_ENABLED
	uint32_t tlas_instance_count = 0;
	uint32_t tlas_primitive_count = 0;
	uint32_t rt_blas_builds = 0;
	uint32_t rt_blas_refits = 0;
	uint32_t rt_triangles_built = 0;
	uint32_t rt_triangles_refit = 0;
	const bool collect_render_info = (p_render_data->render_info != nullptr);
#endif

	// -----------------------------------------------------------------------
	// Phase 1: CPU / buffer-update work
	// -----------------------------------------------------------------------
	struct PendingMMSurface {
		RID mm_rid;
		RID mm_gpu_buffer;
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf;
		void *mesh_surface;
		uint32_t mm_count;
		uint32_t surface_index;
		uint32_t surface_counter;
		uint32_t mm_current_offset;
		uint32_t mm_previous_offset;
		Transform3D instance_transform;
		Transform3D prev_instance_transform;
		bool transform_moved;
		bool mm_uses_motion_vectors;
		bool history_invalid;
		uint32_t layer_mask;
		RTMaterialData *mat_data;
		uint32_t visible_inst_flags;
		uint32_t shadow_inst_flags;
		uint8_t visible_instance_mask;
		uint8_t shadow_instance_mask;
		uint32_t rt_sbt_offset;
		uint64_t history_key;
	};
	LocalVector<PendingMMSurface> pending_mm_surfaces;

	auto get_surface_cull_flags = [](const SceneShaderForwardClustered::ShaderData *p_shader, bool p_shadow_double_sided) -> uint32_t {
		if (p_shadow_double_sided) {
			return RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
		}

		uint32_t flags = 0;
		if (p_shader) {
			switch (p_shader->rt_cull_mode()) {
				case RSE::CULL_MODE_DISABLED:
					flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
					flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
					break;
				case RSE::CULL_MODE_FRONT:
					break;
				case RSE::CULL_MODE_BACK:
				default:
					flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
					break;
			}
		} else {
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
		}
		return flags;
	};

	auto get_ready_rt_sbt_offset = [&](RTMaterialData *p_mat_data) -> uint32_t {
		if (p_mat_data->rt_sbt_offset == 0) {
			return 0;
		}
		return rt_shader_singleton->is_hg_ready_in_bundle(p_mat_data->rt_sbt_offset, p_rt_flags) ? p_mat_data->rt_sbt_offset : 0;
	};

	auto custom_alpha_hit_group_unready = [&](RTMaterialData *p_mat_data, uint32_t p_rt_sbt_offset) -> bool {
		if (p_mat_data->rt_sbt_offset == 0 || p_rt_sbt_offset != 0) {
			return false;
		}
		const SceneShaderRaytracing::CustomShaderEntry *cse =
				rt_shader_singleton->get_custom_shader_entry(p_mat_data->rt_sbt_offset);
		return cse && cse->uses_alpha_clip;
	};

	auto custom_hit_group_temporal_unsupported = [&](uint32_t p_rt_sbt_offset) -> bool {
		if (p_rt_sbt_offset == 0) {
			return false;
		}
		const SceneShaderRaytracing::CustomShaderEntry *cse =
				rt_shader_singleton->get_custom_shader_entry(p_rt_sbt_offset);
		return cse && cse->uses_time;
	};

	auto custom_hit_group_global_uniform_version = [&](uint32_t p_rt_sbt_offset) -> uint64_t {
		if (p_rt_sbt_offset == 0) {
			return 0;
		}
		const SceneShaderRaytracing::CustomShaderEntry *cse =
				rt_shader_singleton->get_custom_shader_entry(p_rt_sbt_offset);
		return (cse && cse->uses_global_uniforms) ? material_storage->global_shader_uniforms_get_version() : 0;
	};

	auto surface_uses_alpha_with_sbt = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf, RTMaterialData *p_mat_data, uint32_t p_rt_sbt_offset) -> bool {
		if (custom_alpha_hit_group_unready(p_mat_data, p_rt_sbt_offset)) {
			return true;
		}
		if ((p_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) != 0) {
			return true;
		}
		if (p_mat_data->rt_sbt_offset > 0 && p_rt_sbt_offset == 0) {
			return false;
		}
		if (p_rt_sbt_offset > 0) {
			const SceneShaderRaytracing::CustomShaderEntry *cse =
					rt_shader_singleton->get_custom_shader_entry(p_rt_sbt_offset);
			return cse && cse->uses_alpha_clip;
		}
		if (!p_surf->shader) {
			return false;
		}
		if (p_surf->shader->rt) {
			return p_surf->shader->rt->uses_alpha_clip ||
					p_surf->shader->rt->uses_blend_alpha ||
					p_surf->shader->rt->uses_alpha ||
					p_surf->shader->rt->uses_alpha_antialiasing;
		}
		return p_surf->shader->uses_alpha_clip ||
				p_surf->shader->uses_blend_alpha ||
				p_surf->shader->uses_alpha ||
				p_surf->shader->uses_alpha_antialiasing;
	};

	auto get_visible_inst_flags_with_sbt = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf, RTMaterialData *p_mat_data, uint32_t p_rt_sbt_offset) -> uint32_t {
		uint32_t flags = get_surface_cull_flags(p_surf->shader, false);
		if (!surface_uses_alpha_with_sbt(p_surf, p_mat_data, p_rt_sbt_offset)) {
			flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
		}
		return flags;
	};

	auto get_shadow_inst_flags = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf) -> uint32_t {
		const bool double_sided_shadow = (p_surf->flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_USES_DOUBLE_SIDED_SHADOWS) != 0;
		return get_surface_cull_flags(p_surf->shader, double_sided_shadow);
	};

	auto get_surface_material_data_with_sbt = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf, RTMaterialData *p_mat_data, uint32_t p_rt_sbt_offset) -> RT_MaterialData {
		RT_MaterialData data = p_mat_data->data;
		if (p_mat_data->rt_sbt_offset > 0 && p_rt_sbt_offset == 0) {
			data.flags &= ~(RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP);
			if (custom_alpha_hit_group_unready(p_mat_data, p_rt_sbt_offset)) {
				data.flags |= RT_MAT_FLAG_ALPHA_TEST;
			} else {
				data.flags &= ~(RT_MAT_FLAG_ALPHA_TEST | RT_MAT_FLAG_ALPHA_HASH);
				data.alpha_hash_scale = 0.0f;
			}
			return data;
		}
		if (surface_uses_alpha_with_sbt(p_surf, p_mat_data, p_rt_sbt_offset)) {
			data.flags |= RT_MAT_FLAG_ALPHA_TEST;
		} else {
			data.flags &= ~(RT_MAT_FLAG_ALPHA_TEST | RT_MAT_FLAG_ALPHA_HASH | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP);
			data.alpha_hash_scale = 0.0f;
		}
		return data;
	};

	auto resolve_surface_material_rid = [&](const RenderForwardClustered::GeometryInstanceSurfaceDataCache *p_surf, RID p_mesh_rid) -> RID {
		if (p_surf->material_rid.is_valid()) {
			return p_surf->material_rid;
		}
		if (p_surf->owner->data->material_override.is_valid()) {
			return p_surf->owner->data->material_override;
		}
		if (p_surf->surface_index < p_surf->owner->data->surface_materials.size() &&
				p_surf->owner->data->surface_materials[p_surf->surface_index].is_valid()) {
			return p_surf->owner->data->surface_materials[p_surf->surface_index];
		}
		if (p_mesh_rid.is_valid() && mesh_storage->owns_mesh(p_mesh_rid)) {
			return mesh_storage->mesh_surface_get_material(p_mesh_rid, p_surf->surface_index);
		}
		return RID();
	};

	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
				static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		// GPUParticles only expose their live transform stream through the raster
		// instance buffer, so keep them as a transparent raster overlay instead
		// of tracing a stale draw-pass mesh in the TLAS.
		if (inst->data->base_type == RSE::INSTANCE_PARTICLES || (inst->base_flags & RenderForwardClustered::INSTANCE_DATA_FLAG_PARTICLES)) {
			continue;
		}
		const Transform3D &instance_transform = inst->transform;
		const bool transform_teleported = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::TELEPORTED;
		const bool instance_shadow_only = inst->data->cast_shadows_only;
		const bool instance_alpha_overlay = _rt_instance_uses_alpha_overlay(inst, p_render_data);
		const bool instance_can_cast_rt_shadows = inst->data->shadow_casting_setting_enabled;

		// Determine previous-frame transform for motion vectors.
		const Transform3D &prev_instance_transform =
				transform_teleported
				? inst->transform
				: inst->prev_transform;

		// Handle procedural RT instances (intersection shaders).
		if (inst->rt_procedural && inst->rt_procedural->enabled) {
			SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
			RTProceduralState *ps = inst->rt_procedural;

			// Intersection code comes from ShaderMaterial on material_override.
			if (!inst->data || !inst->data->material_override.is_valid()) {
				continue;
			}
			RID proc_material_rid = inst->data->material_override;
			uint32_t shader_id = material_storage->material_get_shader_id(proc_material_rid);
			if (shader_id == 0) {
				continue;
			}

			uint16_t proc_mat_counter = material_storage->material_get_rt_invalidation_counter(proc_material_rid);
			uint32_t hg_index = rt_shader->register_procedural_shader(shader_id, proc_material_rid);
			if (hg_index == 0) {
				continue;
			}
			if (!rt_shader->is_hg_ready_in_bundle(hg_index, p_rt_flags)) {
				continue;
			}
			const SceneShaderRaytracing::CustomShaderEntry *proc_entry = rt_shader->get_custom_shader_entry(hg_index);
			const bool procedural_temporal_unsupported = !proc_entry || proc_entry->uses_time;
			const uint64_t procedural_global_uniform_version = proc_entry && proc_entry->uses_global_uniforms ? material_storage->global_shader_uniforms_get_version() : 0;

			const bool procedural_history_invalid = transform_teleported || ps->dirty || procedural_temporal_unsupported;
			if (ps->dirty) {
#ifdef TOOLS_ENABLED
				uint32_t pre_proc_build_size = dirty_blas_list.size();
#endif
				update_procedural_blas(ps, dirty_blas_list);
				ps->dirty = false;
#ifdef TOOLS_ENABLED
				if (collect_render_info) {
					rt_blas_builds += dirty_blas_list.size() - pre_proc_build_size;
				}
#endif
			}

			if (ps->blas.is_valid()) {
				const uint8_t visible_instance_mask = _rt_instance_mask(!instance_shadow_only && !instance_alpha_overlay, false);
				const uint8_t shadow_instance_mask = _rt_instance_mask(false, instance_can_cast_rt_shadows && !instance_alpha_overlay);
				if ((visible_instance_mask | shadow_instance_mask) == 0) {
					continue;
				}

				RT_GeometryData geom = {};
				geom.flags = RT_GEOM_FLAG_PROCEDURAL;
				geom.normal_byte_offset = RT_OFFSET_NONE;
				geom.tangent_byte_offset = RT_OFFSET_NONE;
				geom.uv_byte_offset = RT_OFFSET_NONE;
				geom.uv2_byte_offset = RT_OFFSET_NONE;
				geom.color_byte_offset = RT_OFFSET_NONE;
				geom.index_format = RT_INDEX_FORMAT_NONE;
				geom.vertex_buffer_address = ps->gpu_buffer_address;
				if (ps->gpu_buffer.is_valid()) {
					geometry_buffer_dependencies.push_back(ps->gpu_buffer);
				}
				geom.aabb_size_x = (float)ps->culling_aabb.size.x;
				geom.aabb_size_y = (float)ps->culling_aabb.size.y;
				geom.aabb_size_z = (float)ps->culling_aabb.size.z;
				uint64_t history_key = _rt_history_mix(0x70726f6365647572ULL, inst->rt_history_instance_id);
				history_key = _rt_history_mix_rid(history_key, proc_material_rid);
				history_key = _rt_history_mix(history_key, proc_mat_counter);
				history_key = _rt_history_mix(history_key, shader_id);
				history_key = _rt_history_mix(history_key, hg_index);
				history_key = _rt_history_mix(history_key, procedural_global_uniform_version);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.position.x);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.position.y);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.position.z);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.size.x);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.size.y);
				history_key = _rt_history_mix_float(history_key, (float)ps->culling_aabb.size.z);
				history_key = _rt_history_mix(history_key, ps->expose_bounds ? 1u : 0u);
				history_key = _rt_history_mix(history_key, ps->aabb_data.size());
				for (int32_t aabb_data_idx = 0; aabb_data_idx < ps->aabb_data.size(); aabb_data_idx++) {
					history_key = _rt_history_mix_float(history_key, ps->aabb_data[aabb_data_idx]);
				}

				// Material for procedural geometry (already validated above).
				RTMaterialData *proc_mat_data = process_material(proc_material_rid, proc_mat_counter, hg_index);
				RT_MaterialData proc_material_data = proc_mat_data->data;
				const bool proc_uses_alpha = (proc_material_data.flags & (RT_MAT_FLAG_CUSTOM_ALPHA_CLIP | RT_MAT_FLAG_ALPHA_HASH)) != 0;
				if (proc_uses_alpha) {
					proc_material_data.flags |= RT_MAT_FLAG_ALPHA_TEST;
				} else {
					proc_material_data.flags &= ~(RT_MAT_FLAG_ALPHA_TEST | RT_MAT_FLAG_ALPHA_HASH | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP);
					proc_material_data.alpha_hash_scale = 0.0f;
				}

				auto push_procedural_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
					if (p_instance_mask == 0) {
						return;
					}
					blass.push_back(ps->blas);
					blas_transforms.push_back(instance_transform);
					sbt_offsets.push_back(hg_index);
					geometry_data.push_back(_rt_geometry_with_history_validity(state, geom, history_key, inst->layer_mask, p_instance_mask, procedural_history_invalid));

					if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
						motion_indices.push_back((int32_t)motion_transforms.size());
						RT_InstanceMotionData motion = {};
						RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_instance_transform, motion.prev_object_to_world);
						motion_transforms.push_back(motion);
					} else {
						motion_indices.push_back(-1);
					}

					material_data.push_back(proc_material_data);
					instance_flags.push_back(p_inst_flags);
					instance_masks.push_back(p_instance_mask);
				};

				const uint32_t procedural_cull_flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
				push_procedural_entry(visible_instance_mask, procedural_cull_flags | (proc_uses_alpha ? 0 : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT));
				// Shadow entries must not be FORCE_OPAQUE: any-hit filters the light's shadow caster mask.
				push_procedural_entry(shadow_instance_mask, procedural_cull_flags);
			}
			continue;
		}

		// MultiMesh: resolve materials and warm data cache now.
		// Compute dispatches and TLAS assembly are deferred to Phase 2.
		if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mm_rid = inst->data->base;

			if (mesh_storage->multimesh_get_transform_format(mm_rid) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}

			uint32_t mm_count = mesh_storage->multimesh_get_instances_to_draw(mm_rid);
			if (mm_count == 0) {
				continue;
			}

			RID mm_gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(mm_rid);

			bool transform_moved = (inst->transform_status ==
					RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED);
			uint32_t mm_current_offset = 0;
			uint32_t mm_previous_offset = 0;
			mesh_storage->_multimesh_get_motion_vectors_offsets(mm_rid, mm_current_offset, mm_previous_offset);
			bool mm_uses_motion_vectors = mesh_storage->_multimesh_uses_motion_vectors_offsets(mm_rid);
			uint64_t mm_transform_change_stamp = mesh_storage->multimesh_get_rt_transform_last_change(mm_rid);
			uint64_t mm_appearance_change_stamp = mesh_storage->multimesh_get_rt_appearance_last_change(mm_rid);
			bool mm_transform_changed_without_motion_vectors = !mm_uses_motion_vectors && mm_transform_change_stamp == current_frame;
			bool mm_appearance_changed = mm_appearance_change_stamp == current_frame;
			bool mm_history_invalid = transform_teleported || (transform_moved && !mm_uses_motion_vectors) || mm_transform_changed_without_motion_vectors || mm_appearance_changed;
			RID mm_mesh_rid = mesh_storage->multimesh_get_mesh(mm_rid);

			const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf = inst->surface_caches;
			while (mm_surf) {
				if (mm_surf->primitive != RSE::PRIMITIVE_TRIANGLES) {
					mm_surf = mm_surf->next;
					continue;
				}
				const bool surface_alpha_overlay = (mm_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) != 0;
				const bool visible_to_primary = !instance_shadow_only && !instance_alpha_overlay && !surface_alpha_overlay;
				const bool visible_to_shadows = instance_can_cast_rt_shadows && !instance_alpha_overlay && (mm_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW);
				const uint8_t visible_instance_mask = _rt_instance_mask(visible_to_primary, false);
				const uint8_t shadow_instance_mask = _rt_instance_mask(false, visible_to_shadows);
				if ((visible_instance_mask | shadow_instance_mask) == 0) {
					mm_surf = mm_surf->next;
					continue;
				}

				void *mesh_surface = mm_surf->surface;
				uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

				RID material_rid = resolve_surface_material_rid(mm_surf, mm_mesh_rid);

				uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
				RTMaterialData *mat_data = process_material(material_rid, material_counter);
				uint32_t rt_sbt_offset = get_ready_rt_sbt_offset(mat_data);
				const uint8_t effective_shadow_instance_mask = custom_alpha_hit_group_unready(mat_data, rt_sbt_offset) ? 0 : shadow_instance_mask;
				const bool custom_temporal_unsupported = custom_hit_group_temporal_unsupported(rt_sbt_offset);
				const uint64_t custom_global_uniform_version = custom_hit_group_global_uniform_version(rt_sbt_offset);

				PendingMMSurface pending;
				pending.mm_rid = mm_rid;
				pending.mm_gpu_buffer = mm_gpu_buffer;
				pending.mm_surf = mm_surf;
				pending.mesh_surface = mesh_surface;
				pending.mm_count = mm_count;
				pending.surface_index = mm_surf->surface_index;
				pending.surface_counter = surface_counter;
				pending.mm_current_offset = mm_current_offset;
				pending.mm_previous_offset = mm_previous_offset;
				pending.instance_transform = instance_transform;
				pending.prev_instance_transform = prev_instance_transform;
				pending.transform_moved = transform_moved;
				pending.mm_uses_motion_vectors = mm_uses_motion_vectors;
				pending.history_invalid = mm_history_invalid || custom_temporal_unsupported;
				pending.layer_mask = inst->layer_mask;
				pending.mat_data = mat_data;
				pending.visible_inst_flags = get_visible_inst_flags_with_sbt(mm_surf, mat_data, rt_sbt_offset);
				pending.shadow_inst_flags = get_shadow_inst_flags(mm_surf);
				pending.visible_instance_mask = visible_instance_mask;
				pending.shadow_instance_mask = effective_shadow_instance_mask;
				pending.rt_sbt_offset = rt_sbt_offset;
				uint64_t history_key = _rt_history_mix(0x6d756c74696d6573ULL, inst->rt_history_instance_id);
				history_key = _rt_history_mix_rid(history_key, mm_rid);
				history_key = _rt_history_mix(history_key, mm_count);
				history_key = _rt_history_mix(history_key, mm_surf->surface_index);
				history_key = _rt_history_mix(history_key, surface_counter);
				if (!mm_uses_motion_vectors) {
					history_key = _rt_history_mix(history_key, mm_transform_change_stamp);
				}
				history_key = _rt_history_mix(history_key, mm_appearance_change_stamp);
				history_key = _rt_history_mix_rid(history_key, material_rid);
				history_key = _rt_history_mix(history_key, material_counter);
				history_key = _rt_history_mix(history_key, rt_sbt_offset);
				history_key = _rt_history_mix(history_key, custom_global_uniform_version);
				pending.history_key = history_key;
				pending_mm_surfaces.push_back(pending);

				mm_surf = mm_surf->next;
			}
			continue;
		}

		// Walk the surface cache linked list.
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;
		bool instance_static = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::NONE;
		while (surf) {
			if (surf->primitive != RSE::PRIMITIVE_TRIANGLES) {
				surf = surf->next;
				continue;
			}
			const bool surface_alpha_overlay = (surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) != 0;
			const bool visible_to_primary = !instance_shadow_only && !instance_alpha_overlay && !surface_alpha_overlay;
			const bool visible_to_shadows = instance_can_cast_rt_shadows && !instance_alpha_overlay && (surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_SHADOW);
			const uint8_t visible_instance_mask = _rt_instance_mask(visible_to_primary, false);
			const uint8_t shadow_instance_mask = _rt_instance_mask(false, visible_to_shadows);
			if ((visible_instance_mask | shadow_instance_mask) == 0) {
				surf = surf->next;
				continue;
			}

			void *mesh_surface = surf->surface;
			uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

#ifdef TOOLS_ENABLED
			uint32_t pre_build_size = dirty_blas_list.size();
			uint32_t pre_refit_size = dirty_blas_update_list.size();
#endif

			// MeshInstance skinning/blend shapes provide a deformed vertex buffer.
			RTSurfaceData *surf_data = nullptr;
			bool deformed_history_invalid = false;
			if (inst->mesh_instance.is_valid()) {
				RID curr_vb = mesh_storage->mesh_instance_get_vertex_buffer(inst->mesh_instance, surf->surface_index);
				if (curr_vb.is_valid()) {
					RTDeformedGeometrySource src;
					src.current_vb = curr_vb;
					src.prev_vb = mesh_storage->mesh_instance_get_prev_vertex_buffer(inst->mesh_instance, surf->surface_index);
					src.change_stamp = mesh_storage->mesh_instance_get_last_change(inst->mesh_instance, surf->surface_index);
					const bool deformed_updated_this_frame = src.change_stamp == current_frame;
					deformed_history_invalid = !src.prev_vb.is_valid() || (deformed_updated_this_frame && src.prev_vb == curr_vb);
					uint64_t mi_id = inst->mesh_instance.get_id();
					uint32_t mi_index = static_cast<uint32_t>(mi_id & 0xFFFFFFFFULL);
					src.cache_version = static_cast<uint32_t>(mi_id >> 32);
					src.cache_key = (static_cast<uint64_t>(mi_index) << 16) | (surf->surface_index & 0xFFFFu);
					src.surface_counter = surface_counter;
					surf_data = process_deformed_surface(surf, mesh_surface, src, dirty_blas_list, dirty_blas_update_list);
				}
			}
			if (!surf_data) {
				surf_data = process_surface(surf, mesh_surface, surface_counter, instance_transform, dirty_blas_list);
			}
			if (!surf_data || !surf_data->blas.is_valid()) {
				surf = surf->next;
				continue;
			}

			// Resolve material before TLAS so unready custom HGs can use the default material path for this frame.
			RID material_rid = resolve_surface_material_rid(surf, surf->owner->data->base);

			uint16_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
			RTMaterialData *mat_data = process_material(material_rid, material_counter);
			uint32_t rt_sbt_offset = get_ready_rt_sbt_offset(mat_data);
			const uint8_t effective_shadow_instance_mask = custom_alpha_hit_group_unready(mat_data, rt_sbt_offset) ? 0 : shadow_instance_mask;
			const bool custom_temporal_unsupported = custom_hit_group_temporal_unsupported(rt_sbt_offset);
			const uint64_t custom_global_uniform_version = custom_hit_group_global_uniform_version(rt_sbt_offset);

			// Compute or reuse cached final transform (instance * aabb_transform for compressed meshes).
			Transform3D final_transform;
			if (instance_static && surf->cached_final_transform_valid) {
				final_transform = surf->cached_final_transform;
			} else {
				final_transform = instance_transform;
				if (surf_data->is_compressed) {
					final_transform = instance_transform * surf_data->aabb_transform;
				}
				surf->cached_final_transform = final_transform;
				surf->cached_final_transform_valid = true;
			}

			uint64_t history_key = _rt_history_mix(0x6d65736873757266ULL, inst->rt_history_instance_id);
			history_key = _rt_history_mix_rid(history_key, surf->owner->data->base);
			history_key = _rt_history_mix(history_key, surf->surface_index);
			history_key = _rt_history_mix(history_key, surface_counter);
			history_key = _rt_history_mix_rid(history_key, material_rid);
			history_key = _rt_history_mix(history_key, material_counter);
			history_key = _rt_history_mix(history_key, rt_sbt_offset);
			history_key = _rt_history_mix(history_key, custom_global_uniform_version);
			history_key = _rt_history_mix(history_key, surf_data->geometry.flags);

			uint32_t pushed_entries = 0;
			auto push_mesh_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
				if (p_instance_mask == 0) {
					return;
				}
				blass.push_back(surf_data->blas);
				blas_transforms.push_back(final_transform);
				const bool mesh_history_invalid = transform_teleported || deformed_history_invalid || custom_temporal_unsupported;
				geometry_data.push_back(_rt_geometry_with_history_validity(state, surf_data->geometry, history_key, inst->layer_mask, p_instance_mask, mesh_history_invalid));

				if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					Transform3D prev_final = prev_instance_transform;
					if (surf_data->is_compressed) {
						prev_final = prev_instance_transform * surf_data->aabb_transform;
					}
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				sbt_offsets.push_back(rt_sbt_offset);
				material_data.push_back(get_surface_material_data_with_sbt(surf, mat_data, rt_sbt_offset));
				instance_flags.push_back(p_inst_flags);
				instance_masks.push_back(p_instance_mask);
				pushed_entries++;
			};

			push_mesh_entry(visible_instance_mask, get_visible_inst_flags_with_sbt(surf, mat_data, rt_sbt_offset));
			// Shadow entries must stay non-opaque so any-hit can apply per-light shadow caster masks.
			push_mesh_entry(effective_shadow_instance_mask, get_shadow_inst_flags(surf));

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pushed_entries;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(surf->primitive, vertices);
				tlas_primitive_count += prim_count * pushed_entries;
				uint32_t build_delta = dirty_blas_list.size() - pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif

			surf = surf->next;
		}
	}

	// -----------------------------------------------------------------------
	// Phase 2: GPU compute — merged MultiMesh BLAS dispatches.
	// -----------------------------------------------------------------------
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (const PendingMMSurface &pending : pending_mm_surfaces) {
#ifdef TOOLS_ENABLED
		uint32_t mm_pre_build_size = dirty_blas_list.size();
		uint32_t mm_pre_refit_size = dirty_blas_update_list.size();
#endif
		RTSurfaceData merged_sd;
		bool use_merged = !pending.mm_uses_motion_vectors && pending.mm_gpu_buffer.is_valid() &&
				_build_merged_mm_blas(pending.mm_rid, pending.mm_gpu_buffer, pending.mesh_surface,
						pending.mm_count, pending.surface_index, pending.surface_counter,
						compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd);

		if (use_merged) {
			RT_GeometryData merged_geometry = merged_sd.geometry;
			merged_geometry.flags |= RT_GEOM_FLAG_PRIMITIVE_HISTORY_ID;
			uint64_t history_key = _rt_history_mix(pending.history_key, merged_geometry.flags);
			uint32_t pushed_entries = 0;
			auto push_merged_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
				if (p_instance_mask == 0) {
					return;
				}
				blass.push_back(merged_sd.blas);
				blas_transforms.push_back(pending.instance_transform);
				geometry_data.push_back(_rt_geometry_with_history_validity(state, merged_geometry, history_key, pending.layer_mask, p_instance_mask, pending.history_invalid));
				sbt_offsets.push_back(pending.rt_sbt_offset);
				material_data.push_back(get_surface_material_data_with_sbt(pending.mm_surf, pending.mat_data, pending.rt_sbt_offset));
				if (pending.transform_moved) {
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(pending.prev_instance_transform, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}
				instance_flags.push_back(p_inst_flags);
				instance_masks.push_back(p_instance_mask);
				pushed_entries++;
			};

			push_merged_entry(pending.visible_instance_mask, pending.visible_inst_flags);
			push_merged_entry(pending.shadow_instance_mask, pending.shadow_inst_flags);
#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pushed_entries;
				uint32_t prim_count = merged_sd.geometry.primitive_count;
				tlas_primitive_count += prim_count * pushed_entries;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		} else {
			// Fallback: expanded TLAS - one entry per instance, shared BLAS.
			// Only use already-local CPU transforms here; forcing a GPU readback
			// during TLAS construction can stall exactly when large particle-like
			// MultiMeshes enter view.
			const float *mm_data = mesh_storage->multimesh_get_cached_local_data_ptr(pending.mm_rid);
			if (!mm_data) {
				WARN_PRINT_ONCE("RT: Skipping expanded MultiMesh fallback because transforms are GPU-only; use BLAS-compatible merged MultiMeshes or keep transforms CPU-visible to avoid a synchronous GPU readback.");
				continue;
			}

			const uint32_t mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
			const uint32_t mm_cur_offset = pending.mm_current_offset;
			const uint32_t mm_prev_offset = pending.mm_previous_offset;

			RTSurfaceData *surf_data = process_surface(pending.mm_surf, pending.mesh_surface,
					pending.surface_counter, pending.instance_transform, dirty_blas_list);
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}

			for (uint32_t mi = 0; mi < pending.mm_count; mi++) {
				const float *d = mm_data + (mm_cur_offset + mi) * mm_stride;
				Transform3D mm_xform;
				mm_xform.basis.rows[0][0] = d[0];
				mm_xform.basis.rows[0][1] = d[1];
				mm_xform.basis.rows[0][2] = d[2];
				mm_xform.origin.x = d[3];
				mm_xform.basis.rows[1][0] = d[4];
				mm_xform.basis.rows[1][1] = d[5];
				mm_xform.basis.rows[1][2] = d[6];
				mm_xform.origin.y = d[7];
				mm_xform.basis.rows[2][0] = d[8];
				mm_xform.basis.rows[2][1] = d[9];
				mm_xform.basis.rows[2][2] = d[10];
				mm_xform.origin.z = d[11];

				const float *pd = mm_data + (mm_prev_offset + mi) * mm_stride;
				Transform3D prev_mm_xform;
				prev_mm_xform.basis.rows[0][0] = pd[0];
				prev_mm_xform.basis.rows[0][1] = pd[1];
				prev_mm_xform.basis.rows[0][2] = pd[2];
				prev_mm_xform.origin.x = pd[3];
				prev_mm_xform.basis.rows[1][0] = pd[4];
				prev_mm_xform.basis.rows[1][1] = pd[5];
				prev_mm_xform.basis.rows[1][2] = pd[6];
				prev_mm_xform.origin.y = pd[7];
				prev_mm_xform.basis.rows[2][0] = pd[8];
				prev_mm_xform.basis.rows[2][1] = pd[9];
				prev_mm_xform.basis.rows[2][2] = pd[10];
				prev_mm_xform.origin.z = pd[11];

				Transform3D final_transform = pending.instance_transform * mm_xform;
				if (surf_data->is_compressed) {
					final_transform = final_transform * surf_data->aabb_transform;
				}

				uint64_t history_key = _rt_history_mix(pending.history_key, mi);
				history_key = _rt_history_mix(history_key, surf_data->geometry.flags);

				auto push_mm_instance_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
					if (p_instance_mask == 0) {
						return;
					}
					blass.push_back(surf_data->blas);
					blas_transforms.push_back(final_transform);
					geometry_data.push_back(_rt_geometry_with_history_validity(state, surf_data->geometry, history_key, pending.layer_mask, p_instance_mask, pending.history_invalid));
					sbt_offsets.push_back(pending.rt_sbt_offset);
					material_data.push_back(get_surface_material_data_with_sbt(pending.mm_surf, pending.mat_data, pending.rt_sbt_offset));

					if (pending.transform_moved || pending.mm_uses_motion_vectors) {
						Transform3D prev_final = pending.prev_instance_transform * prev_mm_xform;
						if (surf_data->is_compressed) {
							prev_final = prev_final * surf_data->aabb_transform;
						}
						motion_indices.push_back((int32_t)motion_transforms.size());
						RT_InstanceMotionData motion = {};
						RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
						motion_transforms.push_back(motion);
					} else {
						motion_indices.push_back(-1);
					}

					instance_flags.push_back(p_inst_flags);
					instance_masks.push_back(p_instance_mask);
				};

				push_mm_instance_entry(pending.visible_instance_mask, pending.visible_inst_flags);
				push_mm_instance_entry(pending.shadow_instance_mask, pending.shadow_inst_flags);
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				uint32_t entries_per_instance = (pending.visible_instance_mask != 0 ? 1u : 0u) + (pending.shadow_instance_mask != 0 ? 1u : 0u);
				tlas_instance_count += pending.mm_count * entries_per_instance;
				uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(pending.mesh_surface);
				uint32_t prim_count = _rt_indices_to_primitives(pending.mm_surf->primitive, vertices);
				tlas_primitive_count += prim_count * pending.mm_count * entries_per_instance;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		}
	}

	// -----------------------------------------------------------------------
	// Phase 3: BLAS / TLAS build.
	// -----------------------------------------------------------------------
#ifdef TOOLS_ENABLED
	if (collect_render_info) {
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += tlas_primitive_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += rt_blas_builds;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += rt_blas_refits;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT] += rt_triangles_built;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT] += rt_triangles_refit;
	}
#endif

	SceneShaderRaytracing::get_singleton()->finalize_custom_shaders();

	// End compute list before BLAS builds
	RD::get_singleton()->compute_list_end();

	build_acceleration_structures(state, dirty_blas_list, dirty_blas_update_list);
	finalize_buffers(state);
	state->previous_history_keys = state->current_history_keys;

	return state;
}

// ---------------------------------------------------------------------------
// Light gathering
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::gather_lights(const RenderDataRD *p_render_data, RT_LightData *r_light_data, uint32_t p_max_lights) {
	uint32_t rt_light_count = 0;

	if (!p_render_data || !p_render_data->lights) {
		return rt_light_count;
	}

	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	const Transform3D &cam_xform = p_render_data->scene_data->cam_transform;
	const Vector3 cam_pos = cam_xform.origin;

	// Compute light energy matching rasterizer conventions (light_storage.cpp).
	// Applies PI multiplier (or physical-unit intensity), exposure, and negative sign.
	auto compute_light_energy = [&](RID p_base, RSE::LightType p_type) -> float {
		float sign = ls->light_is_negative(p_base) ? -1.0f : 1.0f;
		float e = sign * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_SPOT) {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		if (p_render_data->camera_attributes.is_valid()) {
			e *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		return e;
	};
	auto compute_light_distance_fade = [&](RID p_base, float p_distance) -> float {
		if (!ls->light_is_distance_fade_enabled(p_base)) {
			return 1.0f;
		}
		const float fade_begin = ls->light_get_distance_fade_begin(p_base);
		const float fade_length = ls->light_get_distance_fade_length(p_base);
		if (fade_length <= 0.0f) {
			return p_distance > fade_begin ? 0.0f : 1.0f;
		}
		if (p_distance > fade_begin + fade_length) {
			return 0.0f;
		}
		if (p_distance > fade_begin) {
			return Math::smoothstep(0.0f, 1.0f, 1.0f - (p_distance - fade_begin) / fade_length);
		}
		return 1.0f;
	};
	auto compute_light_shadow_opacity = [&](RID p_base, float p_distance) -> float {
		float opacity = ls->light_get_param(p_base, RSE::LIGHT_PARAM_SHADOW_OPACITY);
		if (!ls->light_is_distance_fade_enabled(p_base)) {
			return CLAMP(opacity, 0.0f, 1.0f);
		}
		const float fade_shadow = ls->light_get_distance_fade_shadow(p_base);
		const float fade_length = ls->light_get_distance_fade_length(p_base);
		if (fade_length <= 0.0f) {
			return p_distance > fade_shadow ? 0.0f : CLAMP(opacity, 0.0f, 1.0f);
		}
		if (p_distance > fade_shadow + fade_length) {
			return 0.0f;
		}
		if (p_distance > fade_shadow) {
			opacity *= Math::smoothstep(0.0f, 1.0f, 1.0f - (p_distance - fade_shadow) / fade_length);
		}
		return CLAMP(opacity, 0.0f, 1.0f);
	};

	// Scoring helper: approximate power/solid-angle contribution.
	struct LightScore {
		RID light_instance;
		float score;
		uint32_t cull_mask;
	};

	LocalVector<LightScore> positional_lights;
	HashSet<RID> positional_lights_seen;
	uint32_t active_receiver_mask = 0;
	if (p_render_data->rt_instances) {
		const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
		for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
			const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
					static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
			if (!inst || !inst->data) {
				continue;
			}
			if (inst->data->base_type == RSE::INSTANCE_PARTICLES || (inst->base_flags & RenderForwardClustered::INSTANCE_DATA_FLAG_PARTICLES)) {
				continue;
			}
			if (inst->data->cast_shadows_only || _rt_instance_uses_alpha_overlay(inst, p_render_data)) {
				continue;
			}
			active_receiver_mask |= inst->layer_mask;
		}
	}
	if (active_receiver_mask == 0) {
		active_receiver_mask = 0xFFFFFFFFu;
	}

	// Helper: score a positional light and add to candidates.
	auto score_positional_light = [&](RID light_instance) {
		if (positional_lights_seen.has(light_instance)) {
			return;
		}
		RID base = ls->light_instance_get_base_light(light_instance);
		uint32_t cull_mask = ls->light_get_cull_mask(base) & active_receiver_mask;
		if (cull_mask == 0) {
			return;
		}
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 light_pos = xform.origin;
		float dist_sq = cam_pos.distance_squared_to(light_pos);
		float fade = compute_light_distance_fade(base, Math::sqrt(dist_sq));
		if (fade <= 0.0f) {
			return;
		}
		Color color = ls->light_get_color(base);
		float energy = compute_light_energy(base, ls->light_get_type(base)) * fade;
		float lum = color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;
		float score = (Math::abs(energy) * lum) / MAX(dist_sq, 0.01f);

		LightScore ls_entry = {};
		ls_entry.light_instance = light_instance;
		ls_entry.score = score;
		ls_entry.cull_mask = cull_mask;
		positional_lights.push_back(ls_entry);
		positional_lights_seen.insert(light_instance);
	};

	// Directional lights from the frustum-culled list (they're global, always included).
	// Positional lights are also collected here as a conservative fallback for
	// dynamic carried lights that may not enter the wider RT light list.
	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint32_t li = 0; li < (uint32_t)lights.size(); li++) {
		RID light_instance = lights[li];
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		if (type != RSE::LIGHT_DIRECTIONAL) {
			score_positional_light(light_instance);
			continue;
		}
		if ((ls->light_get_cull_mask(base) & active_receiver_mask) == 0) {
			continue;
		}
		if (rt_light_count >= p_max_lights) {
			break;
		}
		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 dir = -xform.basis.get_column(2).normalized();
		ld.position[0] = dir.x;
		ld.position[1] = dir.y;
		ld.position[2] = dir.z;
		ld.type = RT_LIGHT_TYPE_DIRECTIONAL;
		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, RSE::LIGHT_DIRECTIONAL);
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f); // Half-angle in radians.
		ld.attenuation = 0.0f; // No distance attenuation.
		ld.inv_max_range = -1.0f; // Infinite range.
		ld.max_range_squared = 0.0f;
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR);
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		ld.inv_spot_attenuation = 0.0f;
		ld.cos_spot_angle = 0.0f;
		ld.shadow_opacity = compute_light_shadow_opacity(base, 0.0f);
		ld.shadow_max_distance = ls->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
		ld.flags = (ls->light_has_shadow(base) && ld.shadow_opacity > 0.001f) ? uint32_t(RT_LIGHT_FLAG_SHADOW) : 0u;
		ld.spot_direction[0] = 0.0f;
		ld.spot_direction[1] = 0.0f;
		ld.spot_direction[2] = 0.0f;
		ld.cull_mask = ls->light_get_cull_mask(base);
		ld.shadow_caster_mask = ls->light_get_shadow_caster_mask(base);
		rt_light_count++;
	}

	// Positional lights from the AABB-culled RT list (superset of frustum).
	if (p_render_data->rt_lights) {
		const PagedArray<RID> &rt_lights = *p_render_data->rt_lights;
		for (uint32_t li = 0; li < (uint32_t)rt_lights.size(); li++) {
			score_positional_light(rt_lights[li]);
		}
	}

	// Sort all positional lights by score descending.
	struct LightScoreComparator {
		bool operator()(const LightScore &a, const LightScore &b) const {
			return a.score > b.score;
		}
	};
	positional_lights.sort_custom<LightScoreComparator>();

	LocalVector<LightScore> selected_positional_lights;
	HashSet<RID> selected_positional_set;
	uint32_t positional_budget = p_max_lights - rt_light_count;
	uint32_t covered_receiver_mask = 0;
	auto select_positional_light = [&](const LightScore &p_light) {
		if (selected_positional_lights.size() >= positional_budget || selected_positional_set.has(p_light.light_instance)) {
			return;
		}
		selected_positional_lights.push_back(p_light);
		selected_positional_set.insert(p_light.light_instance);
		covered_receiver_mask |= p_light.cull_mask;
	};

	for (uint32_t i = 0; i < positional_lights.size() && selected_positional_lights.size() < positional_budget; i++) {
		if ((positional_lights[i].cull_mask & ~covered_receiver_mask) != 0) {
			select_positional_light(positional_lights[i]);
		}
	}
	for (uint32_t i = 0; i < positional_lights.size() && selected_positional_lights.size() < positional_budget; i++) {
		select_positional_light(positional_lights[i]);
	}

	// Fill remaining slots with selected positional lights.
	for (uint32_t i = 0; i < selected_positional_lights.size() && rt_light_count < p_max_lights; i++) {
		RID light_instance = selected_positional_lights[i].light_instance;
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		const float camera_distance = cam_pos.distance_to(xform.origin);
		const float fade = compute_light_distance_fade(base, camera_distance);
		if (fade <= 0.0f) {
			continue;
		}
		ld.position[0] = xform.origin.x;
		ld.position[1] = xform.origin.y;
		ld.position[2] = xform.origin.z;
		ld.type = (type == RSE::LIGHT_SPOT) ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_OMNI;

		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, type) * fade;
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
		ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		if (range > 0.0f) {
			ld.inv_max_range = 1.0f / range;
			ld.max_range_squared = range * range;
		} else {
			ld.inv_max_range = -1.0f;
			ld.max_range_squared = 0.0f;
		}
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f; // Matches rasterizer convention (light_storage.cpp), normalizes 0.5 default to 1.0.
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		ld.shadow_opacity = compute_light_shadow_opacity(base, camera_distance);
		ld.shadow_max_distance = 0.0f;
		ld.flags = (ls->light_has_shadow(base) && ld.shadow_opacity > 0.001f) ? uint32_t(RT_LIGHT_FLAG_SHADOW) : 0u;

		if (type == RSE::LIGHT_SPOT) {
			ld.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			float spot_angle_deg = ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE);
			ld.cos_spot_angle = Math::cos(Math::deg_to_rad(spot_angle_deg));
			Vector3 spot_dir = -xform.basis.get_column(2).normalized();
			ld.spot_direction[0] = spot_dir.x;
			ld.spot_direction[1] = spot_dir.y;
			ld.spot_direction[2] = spot_dir.z;
		} else {
			ld.inv_spot_attenuation = 0.0f;
			ld.cos_spot_angle = 0.0f;
			ld.spot_direction[0] = 0.0f;
			ld.spot_direction[1] = 0.0f;
			ld.spot_direction[2] = 0.0f;
		}
		ld.cull_mask = ls->light_get_cull_mask(base);
		ld.shadow_caster_mask = ls->light_get_shadow_caster_mask(base);
		rt_light_count++;
	}

	return rt_light_count;
}

void RenderRaytracing::begin_unique_buffer_dependencies(uint32_t p_expected_dependencies) {
	buffer_dependency_dedupe_scratch.clear();
	if (buffer_dependency_dedupe_scratch.get_capacity() < p_expected_dependencies) {
		buffer_dependency_dedupe_scratch.reserve(p_expected_dependencies);
	}
}

void RenderRaytracing::add_unique_buffer_dependency(RD::RaytracingListID p_raytracing_list, RID p_buffer) {
	if (!p_buffer.is_valid() || buffer_dependency_dedupe_scratch.has(p_buffer)) {
		return;
	}
	buffer_dependency_dedupe_scratch.insert(p_buffer);
	RD::get_singleton()->raytracing_list_add_buffer_dependency(p_raytracing_list, p_buffer, /*p_writable=*/false);
}

// ---------------------------------------------------------------------------
// Uniform set update
// ---------------------------------------------------------------------------

RID RenderRaytracing::update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	ERR_FAIL_NULL_V(p_state, RID());

	// BindlessBlock handles its own uniform set cleanup via clear()

	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		if (p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	if (rb_data.is_null()) {
		return RID();
	}
	rb_data->rt_ensure_textures();

	// SET 0 indices must match raytracing_common_inc.glsl / scene_raytracing_raygen.glsl / samplers includes.
	Vector<RD::Uniform> uniforms;
	uint64_t uniform_signature = _rt_history_mix(0x7274756e69666f72ULL, p_rt_flags);
	RID default_storage_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer();
	auto signature_add = [&](RID p_rid) {
		uniform_signature = _rt_signature_mix_rid(uniform_signature, p_rid);
	};
	auto add_uniform_id = [&](RD::Uniform &r_uniform, RID p_rid) {
		r_uniform.append_id(p_rid);
		signature_add(p_rid);
	};

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_texture());
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		ERR_FAIL_COND_V(p_state->tlas == RID(), RID());
		add_uniform_id(u, p_state->tlas);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		add_uniform_id(u, owner->scene_state.uniform_buffers[0]);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->geometry_buffer.is_valid()) {
			add_uniform_id(u, p_state->geometry_buffer);
		} else {
			// Use a default buffer if no geometry
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 4: Per-instance motion index buffer (int32 per TLAS instance, -1 = no motion).
	{
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_index_buffer.is_valid()) {
			add_uniform_id(u, p_state->motion_index_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Motion transforms past sampler block growth reservation (bindings 28-31).
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_transform_buffer.is_valid()) {
			add_uniform_id(u, p_state->motion_transform_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 5: Material buffer.
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->material_buffer.is_valid()) {
			add_uniform_id(u, p_state->material_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 6: Raytracing params + unjittered VP matrices.
	{
		struct {
			float params[16];
			float prev_vp_unjittered[16];
			float curr_vp_unjittered[16];
			float inv_projection_unjittered[16];
			float rt_overscan[4];
			float rt_prev_overscan[4];
		} rt_ubo = {};
		static_assert(sizeof(rt_ubo) == 72 * sizeof(float));

		if (p_render_data && p_render_data->environment.is_valid()) {
			const float *env_params = RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_params_ptr(p_render_data->environment);
			if (env_params) {
				memcpy(rt_ubo.params, env_params, sizeof(float) * 16);
			}
		}

		// rt_params layout (see RaytracingParamIndex enum):
		// [0] = VIS_MODE, [1] = SAMPLE_COUNT, [2] = MAX_BOUNCES,
		// [3] = DENOISER, [11] = TEMPORAL_ACCUMULATION_WEIGHT,
		// [12] = OVERSCAN_HORIZONTAL, [13] = OVERSCAN_VERTICAL,
		// [14] = LIGHT_COUNT, [15] = FRAME_INDEX
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRAME_INDEX] = float(p_state->frame_counter++);

		bool background_uses_sky = false;
		Color background_color = RSG::texture_storage->get_default_clear_color();
		if (p_render_data && owner->is_environment(p_render_data->environment)) {
			RSE::EnvironmentBG bg_mode = owner->environment_get_background(p_render_data->environment);
			float bg_energy_multiplier = owner->environment_get_bg_energy_multiplier(p_render_data->environment);
			bg_energy_multiplier *= owner->environment_get_bg_intensity(p_render_data->environment);
			if (p_render_data->camera_attributes.is_valid()) {
				bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
			}

			switch (bg_mode) {
				case RSE::ENV_BG_CLEAR_COLOR:
					background_color = RSG::texture_storage->get_default_clear_color();
					background_color.r *= bg_energy_multiplier;
					background_color.g *= bg_energy_multiplier;
					background_color.b *= bg_energy_multiplier;
					break;
				case RSE::ENV_BG_COLOR:
					background_color = owner->environment_get_bg_color(p_render_data->environment);
					background_color.r *= bg_energy_multiplier;
					background_color.g *= bg_energy_multiplier;
					background_color.b *= bg_energy_multiplier;
					break;
				case RSE::ENV_BG_SKY: {
					RID sky_rid = owner->environment_get_sky(p_render_data->environment);
					background_uses_sky = sky_rid.is_valid() && owner->sky.sky_get_radiance_texture_rd(sky_rid).is_valid();
					if (!background_uses_sky) {
						background_color = Color(0, 0, 0, 1);
					}
				} break;
				default:
					break;
			}
		}

		background_color = background_color.srgb_to_linear();
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_USES_SKY] = background_uses_sky ? 1.0f : 0.0f;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_R] = background_color.r;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_G] = background_color.g;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_BACKGROUND_B] = background_color.b;

		// Unjittered VP for motion vectors (matches raster convention).
		{
			Projection correction;
			correction.set_depth_correction(true);

			Projection prev_vp = (correction * p_render_data->scene_data->prev_cam_projection) * Projection(p_render_data->scene_data->prev_cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(prev_vp, rt_ubo.prev_vp_unjittered);

			Projection curr_vp = (correction * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(curr_vp, rt_ubo.curr_vp_unjittered);

			Projection curr_projection = correction * p_render_data->scene_data->cam_projection;
			RendererRD::MaterialStorage::store_camera(curr_projection.inverse(), rt_ubo.inv_projection_unjittered);
		}

		const Vector2i rt_visible_origin = rb_data->rt_get_visible_origin();
		const Vector2i rt_prev_visible_origin = rb_data->rt_get_prev_visible_origin();
		const Size2i rt_visible_size = rb_data->rt_get_visible_size();
		const Size2i rt_size = rb_data->rt_get_size();
		rt_ubo.rt_overscan[0] = (float)rt_visible_origin.x;
		rt_ubo.rt_overscan[1] = (float)rt_visible_origin.y;
		rt_ubo.rt_overscan[2] = (float)rt_visible_size.x;
		rt_ubo.rt_overscan[3] = (float)rt_visible_size.y;
		rt_ubo.rt_prev_overscan[0] = (float)rt_prev_visible_origin.x;
		rt_ubo.rt_prev_overscan[1] = (float)rt_prev_visible_origin.y;
		rt_ubo.rt_prev_overscan[2] = (float)rt_size.x;
		rt_ubo.rt_prev_overscan[3] = (float)rt_size.y;

		// --- Light gathering ---
		uint32_t rt_light_count = 0;
		RT_LightData rt_light_data[RT_LIGHTS_MAX] = {};

		rt_light_count = gather_lights(p_render_data, rt_light_data, RT_LIGHTS_MAX);

		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_LIGHT_COUNT] = float(rt_light_count);

		uint64_t radiance_signature = _rt_radiance_signature(p_rt_flags, p_render_data ? p_render_data->environment : RID(), p_render_data ? p_render_data->camera_attributes : RID(), rt_ubo.params, background_color, background_uses_sky, rt_light_data, rt_light_count);
		if (!p_state->radiance_history_signature_valid) {
			p_state->radiance_history_signature = radiance_signature;
			p_state->radiance_history_signature_valid = true;
		} else if (p_state->radiance_history_signature != radiance_signature) {
			p_state->radiance_history_signature = radiance_signature;
			p_state->radiance_history_invalidated = true;
		}

		if (_rt_light_change_requires_history_reset(p_state, rt_light_data, rt_light_count)) {
			p_state->radiance_history_invalidated = true;
		}
		if (rt_light_count > 0) {
			memcpy(p_state->previous_light_data, rt_light_data, rt_light_count * sizeof(RT_LightData));
		}
		p_state->previous_light_count = rt_light_count;
		p_state->previous_light_data_valid = true;

		// Upload light buffer.
		{
			uint32_t buf_size = RT_LIGHTS_MAX * sizeof(RT_LightData);
			if (!p_state->light_buffer.is_valid()) {
				p_state->light_buffer = RD::get_singleton()->storage_buffer_create(buf_size);
				RD::get_singleton()->set_resource_name(p_state->light_buffer, "RT Light Buffer");
				p_state->light_buffer_signature_valid = false;
			}
			uint64_t light_signature = _rt_light_buffer_signature(rt_light_data, rt_light_count);
			if (!p_state->light_buffer_signature_valid || p_state->light_buffer_signature != light_signature) {
				RD::get_singleton()->buffer_update(p_state->light_buffer, 0, buf_size, rt_light_data);
				p_state->light_buffer_signature = light_signature;
				p_state->light_buffer_signature_valid = true;
			}
		}

		if (!p_state->params_buffer.is_valid()) {
			p_state->params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(rt_ubo));
			RD::get_singleton()->set_resource_name(p_state->params_buffer, "RT Params Buffer");
		}
		RD::get_singleton()->buffer_update(p_state->params_buffer, 0, sizeof(rt_ubo), &rt_ubo);

		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		add_uniform_id(u, p_state->params_buffer);
		uniforms.push_back(u);
	}

	// Binding 7: Sky radiance octahedral map (for pathtracing sky sampling).
	{
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		RID radiance_texture;

		// Try to get radiance texture from sky
		if (p_render_data && p_render_data->environment.is_valid()) {
			RID sky_rid = owner->environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				radiance_texture = owner->sky.sky_get_radiance_texture_rd(sky_rid);
			}
		}

		// Fall back to default black texture if no sky
		if (!radiance_texture.is_valid()) {
			radiance_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}

		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		add_uniform_id(u, radiance_texture);
		uniforms.push_back(u);
	}

	// Binding 8: Sampler for radiance texture (linear filtering with mipmaps and clamp).
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		add_uniform_id(u, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
								  RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		uniforms.push_back(u);
	}

	// Bindings 9-12: DLSS Ray Reconstruction output buffers (only in DLSS RR shader variant).
	bool dlss_rr_enabled = rb_data->dlss_rr_has_buffers();
	if (dlss_rr_enabled) {
		// Binding 9: DLSS RR Diffuse Albedo
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			add_uniform_id(u, rb_data->dlss_rr_get_diffuse_albedo());
			uniforms.push_back(u);
		}

		// Binding 10: DLSS RR Specular Albedo
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			add_uniform_id(u, rb_data->dlss_rr_get_specular_albedo());
			uniforms.push_back(u);
		}

		// Binding 11: DLSS RR Normal + Roughness
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			add_uniform_id(u, rb_data->dlss_rr_get_normal_roughness());
			uniforms.push_back(u);
		}

		// Binding 12: DLSS RR Specular Hit Distance
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			add_uniform_id(u, rb_data->dlss_rr_get_specular_hit_dist());
			uniforms.push_back(u);
		}
	}

	// Binding 13: Light buffer (SSBO).
	{
		RD::Uniform u;
		u.binding = 13;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->light_buffer.is_valid()) {
			add_uniform_id(u, p_state->light_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 14: Global shader uniforms SSBO.
	{
		RD::Uniform u;
		u.binding = 14;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID buf = RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer();
		if (buf.is_valid()) {
			add_uniform_id(u, buf);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 15: RT depth output (R32F storage image for writing depth from closest_hit/miss).
	{
		RD::Uniform u;
		u.binding = 15;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_depth_texture());
		uniforms.push_back(u);
	}

	// Bindings 16-27: Material samplers (12 filter/repeat combinations for custom shaders).
	RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default().append_uniforms(uniforms, 16);
	const RendererRD::MaterialStorage::Samplers &default_samplers = RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default();
	for (uint32_t filter = 0; filter < RSE::CANVAS_ITEM_TEXTURE_FILTER_MAX; filter++) {
		for (uint32_t repeat = 0; repeat < RSE::CANVAS_ITEM_TEXTURE_REPEAT_MAX; repeat++) {
			signature_add(default_samplers.rids[filter][repeat]);
		}
	}

	// Binding 28: RT-space velocity output (RG16F). Past the 16-27 sampler range.
	{
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_velocity_texture());
		uniforms.push_back(u);
	}

	// Binding 29: RT history-validity output (R8). Written by primary hit/miss shaders.
	{
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_history_validity());
		uniforms.push_back(u);
	}

	// Binding 30: RT history identity output (RGBA8 packed uint). Written by primary hit/miss shaders.
	{
		RD::Uniform u;
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_history_id());
		uniforms.push_back(u);
	}

	// Binding 31: Visible viewport velocity output for path-traced mode.
	{
		Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
		rb->ensure_velocity();

		RD::Uniform u;
		u.binding = 31;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb->get_velocity_buffer(false));
		uniforms.push_back(u);
	}

	// Binding 33: RTGI normal + roughness guide buffer.
	{
		RD::Uniform u;
		u.binding = 33;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_normal_roughness());
		uniforms.push_back(u);
	}

	// Binding 34: RTGI albedo + metalness guide buffer.
	{
		RD::Uniform u;
		u.binding = 34;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_albedo_metalness());
		uniforms.push_back(u);
	}

	// Binding 35: RTGI linear view-Z + hit-distance guide buffer.
	{
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_viewz_hitdist());
		uniforms.push_back(u);
	}

	// Use the pipeline-side shader so UniformSetFormat matches at bind time.
	RID shader_rd = shader ? shader->get_pipeline_shader_rd(p_rt_flags) : RID();
	signature_add(shader_rd);

	if (shader_rd.is_valid() && p_state->uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->uniform_set) && p_state->uniform_set_signature_valid && p_state->uniform_set_signature == uniform_signature && p_state->uniform_set_shader == shader_rd) {
		if (bindless_block && bindless_block->is_initialized()) {
			bindless_block->finalize(shader_rd, 1);
			bindless_uniform_set = bindless_block->get_uniform_set();
		}
		return p_state->uniform_set;
	}

	_rt_free_uniform_set_if_alive(p_state->uniform_set);
	p_state->uniform_set = RID();
	p_state->uniform_set_signature_valid = false;

	if (shader_rd.is_valid()) {
		p_state->uniform_set = RD::get_singleton()->uniform_set_create(
				uniforms,
				shader_rd,
				RenderForwardClustered::SCENE_UNIFORM_SET);
		RD::get_singleton()->set_resource_name(p_state->uniform_set, "RT Uniform Set");
		p_state->uniform_set_signature = uniform_signature;
		p_state->uniform_set_shader = shader_rd;
		p_state->uniform_set_signature_valid = p_state->uniform_set.is_valid();

		// === SET 1: Bindless textures ===
		if (bindless_block && bindless_block->is_initialized()) {
			bindless_block->finalize(shader_rd, 1);
			bindless_uniform_set = bindless_block->get_uniform_set();
		}
	}

	return p_state->uniform_set;
}

// ---------------------------------------------------------------------------
// Output copy
// ---------------------------------------------------------------------------

void RenderRaytracing::copy_output_texture(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	if (rb_data.is_null() || !rb_data->rt_has_texture()) {
		return;
	}

	// Copy raytracing output to main color buffer
	const Rect2i src_rect(rb_data->rt_get_visible_origin(), rb_data->rt_get_visible_size());
	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RID src = rb_data->rt_get_texture();
		RID dst = rb->get_internal_texture(v);
		owner->copy_effects->copy_to_rect_region(src, dst, src_rect, Vector2i(), false, false, false, false, true);
	}
}
