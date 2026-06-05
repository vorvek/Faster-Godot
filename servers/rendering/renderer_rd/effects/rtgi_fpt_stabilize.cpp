/**************************************************************************/
/*  rtgi_fpt_stabilize.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_fpt_stabilize.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RTGIFPTStabilize::RTGIFPTStabilize() {
	// Single compute shader with no mode variants (mirrors the RTGIGIResolve ctor). This is the first
	// compile of rtgi_fpt_stabilize.glsl, so a GLSL error there trips here.
	shader.initialize({ "" });
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RTGIFPTStabilize::~RTGIFPTStabilize() {
	free_resources();
	shader.version_free(shader_version);
}

void RTGIFPTStabilize::free_resources() {
	for (uint32_t i = 0; i < 2; i++) {
		if (stable[i].is_valid()) {
			RD::get_singleton()->free_rid(stable[i]);
			stable[i] = RID();
		}
	}
	read_index = 0;
	cached_rt_size = Size2i();
	resources_valid = false;
	needs_reset = false;
}

void RTGIFPTStabilize::_allocate(const Size2i &p_rt_size) {
	free_resources();

	const int w = MAX(p_rt_size.x, 1);
	const int h = MAX(p_rt_size.y, 1);
	cached_rt_size = Size2i(w, h);

	// RGBA16F at the RT render size, STORAGE (the shader writes the front buffer) + SAMPLING (the
	// shader reads the history buffer and the composite blits the front buffer) + COPY both ways
	// (parity with the GI resolve buffers).
	RD::TextureFormat fmt;
	fmt.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	fmt.width = w;
	fmt.height = h;
	fmt.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	for (uint32_t i = 0; i < 2; i++) {
		stable[i] = RD::get_singleton()->texture_create(fmt, RD::TextureView());
		RD::get_singleton()->set_resource_name(stable[i], i == 0 ? "RTGI FPT Stable A" : "RTGI FPT Stable B");
		// Clear to "no accumulation": rgb 0, n 0.
		RD::get_singleton()->texture_clear(stable[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	read_index = 0;
	resources_valid = true;
	needs_reset = true; // the freshly cleared history must not be reprojected on the next run.

	const uint64_t bytes = uint64_t(w) * uint64_t(h) * 8ULL * 2ULL; // RGBA16F (8 B) x 2 ping-pong.
	print_verbose(vformat("RTGI FPT Stabilize: allocated %dx%d ping-pong using %.2f MiB VRAM.",
			w, h, double(bytes) / (1024.0 * 1024.0)));
}

bool RTGIFPTStabilize::ensure_resources(const Size2i &p_rt_size) {
	const int w = MAX(p_rt_size.x, 1);
	const int h = MAX(p_rt_size.y, 1);
	const Size2i wanted = Size2i(w, h);

	if (resources_valid && cached_rt_size == wanted && stable[0].is_valid()) {
		return false;
	}
	_allocate(wanted);
	return true;
}

void RTGIFPTStabilize::run_stabilize(RID p_cur_color, RID p_velocity, RID p_history_validity,
		RID p_viewz_hitdist, RID p_normal_roughness,
		const Size2i &p_rt_size, uint32_t p_frame_index, bool p_force_reset,
		const StabilizeParams &p_params) {
	// THE FRAME SWAP: flip read_index ONCE at the top (mirrors RTGIGIResolve::run_resolve). After the
	// flip [read_index] is THIS frame's output and [1 - read_index] is the previous frame's accumulated
	// result (the reproject history). get_stable() returns [read_index].
	read_index = 1u - read_index;

	if (!resources_valid || !stable[read_index].is_valid()) {
		return;
	}
	ERR_FAIL_COND(p_cur_color.is_null());
	ERR_FAIL_COND(p_velocity.is_null());
	ERR_FAIL_COND(p_history_validity.is_null());
	ERR_FAIL_COND(p_viewz_hitdist.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());

	const Size2i size = cached_rt_size;
	const uint32_t prev_index = 1u - read_index;

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	RID shader_rd = shader.version_get_shader(shader_version, 0);
	// Every guide read is an integer texelFetch (sampler filtering is irrelevant); clamp-to-edge linear
	// matches the GI resolve's sampler choice.
	RID linear_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// One-frame reset after a (re)allocation (cleared history) or on the caller's request (camera cut /
	// RT history invalidation), plus the natural frame-0 seed.
	const bool reset = needs_reset || p_force_reset;
	needs_reset = false;

	// No same-resource sampler+image hazard: stable[read_index] is the ONLY image (binding 2);
	// stable[prev_index] is a different buffer bound only as a sampler (binding 1); the rt guides
	// (0, 3-6) are distinct RB textures bound only as samplers.
	LocalVector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(linear_sampler);
		u.append_id(p_cur_color);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(linear_sampler);
		u.append_id(stable[prev_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 2;
		u.append_id(stable[read_index]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 3;
		u.append_id(linear_sampler);
		u.append_id(p_velocity);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 4;
		u.append_id(linear_sampler);
		u.append_id(p_history_validity);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 5;
		u.append_id(linear_sampler);
		u.append_id(p_viewz_hitdist);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 6;
		u.append_id(linear_sampler);
		u.append_id(p_normal_roughness);
		uniforms.push_back(u);
	}

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.screen_w = (uint32_t)size.x;
	push_constant.screen_h = (uint32_t)size.y;
	push_constant.frame_index = p_frame_index;
	push_constant.reset = reset ? 1u : 0u;
	push_constant.n_cap = MAX(p_params.n_cap, 1.0f);
	push_constant.firefly_k = MAX(p_params.firefly_k, 0.0f);
	push_constant.normal_threshold = p_params.normal_threshold;
	push_constant.depth_rel_tol = MAX(p_params.depth_rel_tol, 0.0f);

	RD::get_singleton()->draw_command_begin_label("RTGI FPT Stabilize");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rd, 0, uniforms), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
}
