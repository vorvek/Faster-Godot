/**************************************************************************/
/*  rtgi_diffuse_cache.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_diffuse_cache.h"

#include "servers/rendering/renderer_rd/effects/rtgi_spatiotemporal_radiance_cache.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

static constexpr int RTGI_DIFFUSE_CACHE_SLOT_COUNT = 4;
static constexpr int RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING = 4;
static constexpr int RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION = 4;
static constexpr int RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS = 2;
static constexpr int RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION = 2;

RTGIDiffuseCache::RTGIDiffuseCache() {
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIDiffuseCache::~RTGIDiffuseCache() {
	shader.version_free(shader_version);
}

Size2i RTGIDiffuseCache::_cache_size(const Size2i &p_output_size, uint32_t p_max_cache_entries) const {
	ERR_FAIL_COND_V(p_output_size.x <= 0 || p_output_size.y <= 0, Size2i());

	const uint32_t max_entries = MIN(4194304u, MAX(4096u, p_max_cache_entries));
	const uint64_t output_pixels = (uint64_t)p_output_size.x * (uint64_t)p_output_size.y;
	if (output_pixels <= max_entries) {
		return p_output_size;
	}

	const double scale = Math::sqrt((double)max_entries / (double)output_pixels);
	Size2i cache_size(
			MAX(1, (int32_t)Math::floor((double)p_output_size.x * scale)),
			MAX(1, (int32_t)Math::floor((double)p_output_size.y * scale)));
	while ((uint64_t)cache_size.x * (uint64_t)cache_size.y > max_entries) {
		if (cache_size.x >= cache_size.y && cache_size.x > 1) {
			cache_size.x--;
		} else if (cache_size.y > 1) {
			cache_size.y--;
		} else {
			break;
		}
	}
	return cache_size;
}

bool RTGIDiffuseCache::ensure_resources(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_process_size, uint32_t p_max_cache_entries) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_process_size.x <= 0 || p_process_size.y <= 0, false);

	const Size2i cache_size = _cache_size(p_process_size, p_max_cache_entries);
	return _ensure_buffers(p_render_buffers, p_process_size, cache_size);
}

bool RTGIDiffuseCache::_ensure_buffers(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_output_size, const Size2i &p_cache_size) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_output_size.x <= 0 || p_output_size.y <= 0, false);
	ERR_FAIL_COND_V(p_cache_size.x <= 0 || p_cache_size.y <= 0, false);

	const Size2i persistent_cache_size(p_cache_size.x * RTGI_DIFFUSE_CACHE_SLOT_COUNT, p_cache_size.y);
	const Size2i spg_probe_size((p_output_size.x + RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING - 1) / RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING, (p_output_size.y + RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING - 1) / RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING);
	const Size2i spg_atlas_size(spg_probe_size.x * RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION, spg_probe_size.y * RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION);
	const int refined_cell_size = RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS * RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION;
	const Size2i spg_refined_atlas_size(spg_probe_size.x * refined_cell_size, spg_probe_size.y * refined_cell_size);
	const bool has_radiance = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE);
	const bool has_history_id = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID);
	const bool has_output = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT);
	const bool has_signal_confidence = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SIGNAL_CONFIDENCE);
	const bool has_spg_rejection = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REJECTION);
	const bool has_spg_refinement_mask = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK);
	const bool has_spg_radiance = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE);
	const bool has_spg_stats = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS);
	const bool has_spg_visibility = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY);
	const bool has_spg_refined_radiance = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE);
	const bool has_surface_radiance = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE);
	const bool has_slot_surface_key = p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY);

	bool needs_clear = false;
	if (has_radiance &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, 0) != persistent_cache_size) {
		needs_clear = true;
	}
	if (has_history_id &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID, 0) != persistent_cache_size) {
		needs_clear = true;
	}
	if (has_output &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, 0) != p_output_size) {
		needs_clear = true;
	}
	if (has_signal_confidence &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SIGNAL_CONFIDENCE, 0) != p_output_size) {
		needs_clear = true;
	}
	if (has_spg_rejection &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REJECTION, 0) != p_output_size) {
		needs_clear = true;
	}
	if (has_spg_refinement_mask &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK, 0) != spg_probe_size) {
		needs_clear = true;
	}
	if (has_spg_radiance &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE, 0) != spg_atlas_size) {
		needs_clear = true;
	}
	if (has_spg_stats &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS, 0) != spg_atlas_size) {
		needs_clear = true;
	}
	if (has_spg_visibility &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY, 0) != spg_atlas_size) {
		needs_clear = true;
	}
	if (has_spg_refined_radiance &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE, 0) != spg_refined_atlas_size) {
		needs_clear = true;
	}
	if (has_surface_radiance &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE, 0) != persistent_cache_size) {
		needs_clear = true;
	}
	if (has_slot_surface_key &&
			p_render_buffers->get_texture_slice_size(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY, 0) != persistent_cache_size) {
		needs_clear = true;
	}

	const bool has_complete_context = has_radiance &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT) &&
			has_history_id &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID_NEXT) &&
			has_output &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION) &&
			has_signal_confidence &&
			has_spg_rejection &&
			has_spg_radiance &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META_NEXT) &&
			has_spg_stats &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS_NEXT) &&
			has_spg_visibility &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID_NEXT) &&
			has_spg_refinement_mask &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK_NEXT) &&
			has_spg_refined_radiance &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID_NEXT) &&
			has_surface_radiance &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID_NEXT) &&
			has_slot_surface_key &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY_NEXT) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_CLAIM);

	if ((has_radiance || has_output) && !has_complete_context) {
		needs_clear = true;
	}

	if (needs_clear) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI_DIFFUSE_CACHE);
	}

	if (p_render_buffers->has_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE)) {
		return false;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID history_id = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID_NEXT, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID spg_radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	RID spg_meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	RID spg_stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	RID spg_visibility = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	RID spg_history_id = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID_NEXT, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID diagnostic = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID age = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID rejection = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION, RD::DATA_FORMAT_R8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID signal_confidence = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SIGNAL_CONFIDENCE, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID spg_rejection = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REJECTION, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, p_output_size);
	RID spg_refinement_mask = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_probe_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK_NEXT, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_probe_size);
	RID spg_refined_radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	RID spg_refined_meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	RID spg_refined_stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	RID spg_refined_visibility = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	RID spg_refined_history_id = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID_NEXT, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, spg_refined_atlas_size);
	RID surface_radiance = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID surface_meta = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID surface_stats = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS_NEXT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID surface_history_id = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID_NEXT, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID slot_surface_key = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY, RD::DATA_FORMAT_R32_UINT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY_NEXT, RD::DATA_FORMAT_R32_UINT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);
	RID surface_claim = p_render_buffers->create_texture(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_CLAIM, RD::DATA_FORMAT_R32_UINT, usage_bits, RD::TEXTURE_SAMPLES_1, persistent_cache_size);

	RD::get_singleton()->texture_clear(radiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(meta, Color(0.5, 0.5, 1.0, 65504.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(stats, Color(65504.0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(history_id, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_radiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_meta, Color(0.5, 0.5, 1.0, 65504.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_stats, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_visibility, Color(65504.0, 0, 65504.0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_history_id, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(diagnostic, Color(0, 0, 0, 1), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(age, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(rejection, Color(1, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(signal_confidence, Color(0, 0, 0, 1), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_rejection, Color(0.1, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refinement_mask, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refined_radiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refined_meta, Color(0.5, 0.5, 1.0, 65504.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refined_stats, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refined_visibility, Color(65504.0, 0, 65504.0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(spg_refined_history_id, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(surface_radiance, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(surface_meta, Color(0.5, 0.5, 1.0, 65504.0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(surface_stats, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(surface_history_id, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(slot_surface_key, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	RD::get_singleton()->texture_clear(surface_claim, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());

	return true;
}

void RTGIDiffuseCache::process(Ref<RenderSceneBuffersRD> p_render_buffers,
		RID p_diffuse_radiance,
		RID p_albedo_metalness,
		RID p_velocity,
		RID p_normal_roughness,
		RID p_viewz_hitdist,
		RID p_history_validity,
		RID p_prev_history_validity,
		RID p_history_id,
		RID p_prev_history_id,
		RID p_receiver_surface_id,
		RID p_prev_receiver_surface_id,
		RID p_signal_confidence,
		RID p_primary_diffuse_direction,
		const Vector3 &p_camera_origin,
		const Projection &p_inv_view_projection,
		bool p_strc_enabled,
		uint32_t p_strc_cascade_count,
		uint32_t p_strc_grid_size,
		float p_strc_base_probe_spacing,
		const Size2i &p_process_size,
		uint32_t p_max_cache_entries,
		uint32_t p_view) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	ERR_FAIL_COND(p_process_size.x <= 0 || p_process_size.y <= 0);
	ERR_FAIL_COND(!p_diffuse_radiance.is_valid() || !p_albedo_metalness.is_valid() || !p_velocity.is_valid() || !p_normal_roughness.is_valid() || !p_viewz_hitdist.is_valid());
	ERR_FAIL_COND(!p_history_validity.is_valid() || !p_prev_history_validity.is_valid() || !p_history_id.is_valid() || !p_prev_history_id.is_valid() || !p_receiver_surface_id.is_valid() || !p_prev_receiver_surface_id.is_valid() || !p_signal_confidence.is_valid() || !p_primary_diffuse_direction.is_valid());
	ERR_FAIL_UNSIGNED_INDEX(p_view, p_render_buffers->get_view_count());

	const Size2i cache_size = _cache_size(p_process_size, p_max_cache_entries);
	const Size2i persistent_cache_size(cache_size.x * RTGI_DIFFUSE_CACHE_SLOT_COUNT, cache_size.y);
	const Size2i spg_probe_size((p_process_size.x + RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING - 1) / RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING, (p_process_size.y + RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING - 1) / RTGI_DIFFUSE_CACHE_SPG_PROBE_SPACING);
	const Size2i spg_atlas_size(spg_probe_size.x * RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION, spg_probe_size.y * RTGI_DIFFUSE_CACHE_SPG_DIRECTION_RESOLUTION);
	const int refined_cell_size = RTGI_DIFFUSE_CACHE_SPG_REFINED_SUBDIVS * RTGI_DIFFUSE_CACHE_SPG_REFINED_DIRECTION_RESOLUTION;
	const Size2i spg_refined_atlas_size(spg_probe_size.x * refined_cell_size, spg_probe_size.y * refined_cell_size);
	const bool reset_history = _ensure_buffers(p_render_buffers, p_process_size, cache_size);

	RID radiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE, p_view, 0);
	RID radiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RADIANCE_NEXT, p_view, 0);
	RID meta = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META, p_view, 0);
	RID meta_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_META_NEXT, p_view, 0);
	RID stats = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS, p_view, 0);
	RID stats_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_STATS_NEXT, p_view, 0);
	RID history_id = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID, p_view, 0);
	RID history_id_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_HISTORY_ID_NEXT, p_view, 0);
	RID spg_radiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE, p_view, 0);
	RID spg_radiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_RADIANCE_NEXT, p_view, 0);
	RID spg_meta = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META, p_view, 0);
	RID spg_meta_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_META_NEXT, p_view, 0);
	RID spg_stats = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS, p_view, 0);
	RID spg_stats_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_STATS_NEXT, p_view, 0);
	RID spg_visibility = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY, p_view, 0);
	RID spg_visibility_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_VISIBILITY_NEXT, p_view, 0);
	RID spg_history_id = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID, p_view, 0);
	RID spg_history_id_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_HISTORY_ID_NEXT, p_view, 0);
	RID output = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_OUTPUT, p_view, 0);
	RID raw = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_RAW, p_view, 0);
	RID diagnostic = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_DIAGNOSTIC, p_view, 0);
	RID age = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_AGE, p_view, 0);
	RID rejection = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_REJECTION, p_view, 0);
	RID reconstruction_signal_confidence = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SIGNAL_CONFIDENCE, p_view, 0);
	RID spg_rejection = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REJECTION, p_view, 0);
	RID spg_refinement_mask = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK, p_view, 0);
	RID spg_refinement_mask_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINEMENT_MASK_NEXT, p_view, 0);
	RID spg_refined_radiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE, p_view, 0);
	RID spg_refined_radiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_RADIANCE_NEXT, p_view, 0);
	RID spg_refined_meta = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META, p_view, 0);
	RID spg_refined_meta_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_META_NEXT, p_view, 0);
	RID spg_refined_stats = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS, p_view, 0);
	RID spg_refined_stats_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_STATS_NEXT, p_view, 0);
	RID spg_refined_visibility = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY, p_view, 0);
	RID spg_refined_visibility_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_VISIBILITY_NEXT, p_view, 0);
	RID spg_refined_history_id = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID, p_view, 0);
	RID spg_refined_history_id_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SPG_REFINED_HISTORY_ID_NEXT, p_view, 0);
	RID surface_radiance = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE, p_view, 0);
	RID surface_radiance_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_RADIANCE_NEXT, p_view, 0);
	RID surface_meta = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META, p_view, 0);
	RID surface_meta_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_META_NEXT, p_view, 0);
	RID surface_stats = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS, p_view, 0);
	RID surface_stats_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_STATS_NEXT, p_view, 0);
	RID surface_history_id = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID, p_view, 0);
	RID surface_history_id_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_HISTORY_ID_NEXT, p_view, 0);
	RID slot_surface_key = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY, p_view, 0);
	RID slot_surface_key_next = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SLOT_SURFACE_KEY_NEXT, p_view, 0);
	RID surface_claim = p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_DIFFUSE_CACHE, RB_TEX_RTGI_DIFFUSE_CACHE_SURFACE_CLAIM, p_view, 0);

	RD::get_singleton()->texture_copy(p_diffuse_radiance, raw, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, p_view, 0);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	ERR_FAIL_NULL(texture_storage);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rd = shader.version_get_shader(shader_version, 0);

	RID velocity_slice = p_velocity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_velocity"), p_view, 0) : RID();
	RID normal_roughness_slice = p_normal_roughness.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_normal_roughness"), p_view, 0) : RID();
	RID albedo_metalness_slice = p_albedo_metalness.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_albedo_metalness"), p_view, 0) : RID();
	RID viewz_hitdist_slice = p_viewz_hitdist.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_viewz_hitdist"), p_view, 0) : RID();
	RID history_validity_slice = p_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity"), p_view, 0) : RID();
	RID prev_history_validity_slice = p_prev_history_validity.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_validity_prev"), p_view, 0) : RID();
	RID history_id_slice = p_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id"), p_view, 0) : RID();
	RID prev_history_id_slice = p_prev_history_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_history_id_prev"), p_view, 0) : RID();
	RID receiver_surface_id_slice = p_receiver_surface_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_receiver_surface_id"), p_view, 0) : RID();
	RID prev_receiver_surface_id_slice = p_prev_receiver_surface_id.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_receiver_surface_id_prev"), p_view, 0) : RID();
	RID signal_confidence_slice = p_signal_confidence.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_signal_confidence"), p_view, 0) : RID();
	RID primary_diffuse_direction_slice = p_primary_diffuse_direction.is_valid() ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_primary_diffuse_direction"), p_view, 0) : RID();
	RID surface_cache_key_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_key")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_key"), p_view, 0) : RID();
	RID surface_feedback_key_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_key")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_key"), p_view, 0) : RID();
	RID surface_feedback_radiance_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_radiance")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_radiance"), p_view, 0) : RID();
	RID surface_feedback_meta_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_meta")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_meta"), p_view, 0) : RID();
	RID surface_feedback_stats_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_stats")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_stats"), p_view, 0) : RID();
	RID surface_feedback_diagnostic_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_diagnostic")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_surface_cache_feedback_diagnostic"), p_view, 0) : RID();
	RID secondary_cache_source_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_secondary_cache_source")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_secondary_cache_source"), p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RID secondary_cache_rejection_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_secondary_cache_rejection")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_secondary_cache_rejection"), p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	ERR_FAIL_COND(!surface_cache_key_slice.is_valid());
	ERR_FAIL_COND(!surface_feedback_key_slice.is_valid() || !surface_feedback_radiance_slice.is_valid() || !surface_feedback_meta_slice.is_valid() || !surface_feedback_stats_slice.is_valid() || !surface_feedback_diagnostic_slice.is_valid());
	RID depth_slice = p_render_buffers->has_texture(SNAME("forward_clustered"), SNAME("rt_depth")) ? p_render_buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("rt_depth"), p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);

	const bool strc_available = p_strc_enabled &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE) &&
			p_render_buffers->has_texture(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA);
	RID strc_irradiance = strc_available ? p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_IRRADIANCE, p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RID strc_distance = strc_available ? p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_DISTANCE, p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RID strc_metadata = strc_available ? p_render_buffers->get_texture_slice(RB_SCOPE_RTGI_STRC, RB_TEX_RTGI_STRC_METADATA, p_view, 0) : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, raw));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, velocity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, normal_roughness_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, viewz_hitdist_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, history_validity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, prev_history_validity_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, history_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, prev_history_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ nearest_sampler, signal_confidence_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, output));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, radiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, meta_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, stats_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, diagnostic));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, age));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, rejection));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ nearest_sampler, history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 20, history_id_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 21, Vector<RID>({ nearest_sampler, albedo_metalness_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 22, reconstruction_signal_confidence));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 23, Vector<RID>({ nearest_sampler, receiver_surface_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 24, Vector<RID>({ nearest_sampler, prev_receiver_surface_id_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 25, Vector<RID>({ nearest_sampler, depth_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 26, Vector<RID>({ nearest_sampler, strc_irradiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 27, Vector<RID>({ nearest_sampler, strc_distance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 28, Vector<RID>({ nearest_sampler, strc_metadata })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 29, Vector<RID>({ nearest_sampler, primary_diffuse_direction_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 30, Vector<RID>({ linear_sampler, spg_radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 31, Vector<RID>({ nearest_sampler, spg_meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 32, Vector<RID>({ nearest_sampler, spg_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 33, spg_radiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 34, spg_meta_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 35, spg_history_id_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 36, Vector<RID>({ nearest_sampler, spg_stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 37, spg_stats_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 38, Vector<RID>({ nearest_sampler, spg_visibility })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 39, spg_visibility_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 40, spg_rejection));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 41, Vector<RID>({ nearest_sampler, spg_refinement_mask })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 42, spg_refinement_mask_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 43, Vector<RID>({ linear_sampler, spg_refined_radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 44, Vector<RID>({ nearest_sampler, spg_refined_meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 45, Vector<RID>({ nearest_sampler, spg_refined_stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 46, Vector<RID>({ nearest_sampler, spg_refined_visibility })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 47, Vector<RID>({ nearest_sampler, spg_refined_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 48, spg_refined_radiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 49, spg_refined_meta_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 50, spg_refined_stats_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 51, spg_refined_visibility_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 52, spg_refined_history_id_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 53, Vector<RID>({ linear_sampler, surface_radiance })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 54, Vector<RID>({ nearest_sampler, surface_meta })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 55, Vector<RID>({ nearest_sampler, surface_stats })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 56, Vector<RID>({ nearest_sampler, surface_history_id })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 57, surface_radiance_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 58, surface_meta_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 59, surface_stats_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 60, surface_history_id_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 61, surface_cache_key_slice));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 62, slot_surface_key));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 63, slot_surface_key_next));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 64, surface_claim));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 65, surface_feedback_key_slice));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 66, Vector<RID>({ nearest_sampler, surface_feedback_radiance_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 67, Vector<RID>({ nearest_sampler, surface_feedback_meta_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 68, Vector<RID>({ nearest_sampler, surface_feedback_stats_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 69, surface_feedback_diagnostic_slice));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 70, Vector<RID>({ nearest_sampler, secondary_cache_source_slice })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 71, Vector<RID>({ nearest_sampler, secondary_cache_rejection_slice })));

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.resolution_width = (float)p_process_size.x;
	push_constant.resolution_height = (float)p_process_size.y;
	push_constant.cache_width = (float)cache_size.x;
	push_constant.cache_height = (float)cache_size.y;
	push_constant.max_history = reset_history ? 1.0f : 48.0f;
	push_constant.spg_probe_width = spg_probe_size.x;
	push_constant.spg_probe_height = spg_probe_size.y;
	MaterialStorage::store_camera(p_inv_view_projection, push_constant.inv_view_projection);
	push_constant.camera_origin[0] = p_camera_origin.x;
	push_constant.camera_origin[1] = p_camera_origin.y;
	push_constant.camera_origin[2] = p_camera_origin.z;
	push_constant.strc_base_probe_spacing = CLAMP(p_strc_base_probe_spacing, 0.25f, 8.0f);
	push_constant.strc_grid_size = CLAMP(p_strc_grid_size, 12u, 32u);
	push_constant.strc_cascade_count = CLAMP(p_strc_cascade_count, 1u, 4u);
	push_constant.strc_enabled = strc_available ? 1u : 0u;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 0u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, cache_size.x, cache_size.y, 1);
	push_constant.mode = 2u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, spg_atlas_size.x, spg_atlas_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(radiance_next, radiance, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(meta_next, meta, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(stats_next, stats, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(history_id_next, history_id, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(slot_surface_key_next, slot_surface_key, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_radiance_next, spg_radiance, Vector3(), Vector3(), Vector3(spg_atlas_size.x, spg_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_meta_next, spg_meta, Vector3(), Vector3(), Vector3(spg_atlas_size.x, spg_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_stats_next, spg_stats, Vector3(), Vector3(), Vector3(spg_atlas_size.x, spg_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_visibility_next, spg_visibility, Vector3(), Vector3(), Vector3(spg_atlas_size.x, spg_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_history_id_next, spg_history_id, Vector3(), Vector3(), Vector3(spg_atlas_size.x, spg_atlas_size.y, 1), 0, 0, 0, 0);

	compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 3u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, spg_probe_size.x, spg_probe_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(spg_refinement_mask_next, spg_refinement_mask, Vector3(), Vector3(), Vector3(spg_probe_size.x, spg_probe_size.y, 1), 0, 0, 0, 0);

	compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 4u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(spg_refined_radiance_next, spg_refined_radiance, Vector3(), Vector3(), Vector3(spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_refined_meta_next, spg_refined_meta, Vector3(), Vector3(), Vector3(spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_refined_stats_next, spg_refined_stats, Vector3(), Vector3(), Vector3(spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_refined_visibility_next, spg_refined_visibility, Vector3(), Vector3(), Vector3(spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(spg_refined_history_id_next, spg_refined_history_id, Vector3(), Vector3(), Vector3(spg_refined_atlas_size.x, spg_refined_atlas_size.y, 1), 0, 0, 0, 0);

	compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 5u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, persistent_cache_size.x, persistent_cache_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	push_constant.mode = 6u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, persistent_cache_size.x, persistent_cache_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	push_constant.mode = 7u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_process_size.x, p_process_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(surface_radiance_next, surface_radiance, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(surface_meta_next, surface_meta, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(surface_stats_next, surface_stats, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);
	RD::get_singleton()->texture_copy(surface_history_id_next, surface_history_id, Vector3(), Vector3(), Vector3(persistent_cache_size.x, persistent_cache_size.y, 1), 0, 0, 0, 0);

	compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	push_constant.mode = 1u;
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_process_size.x, p_process_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->texture_copy(output, p_diffuse_radiance, Vector3(), Vector3(), Vector3(p_process_size.x, p_process_size.y, 1), 0, 0, 0, p_view);
}
