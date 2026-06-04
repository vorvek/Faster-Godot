/**************************************************************************/
/*  test_environment.h                                                    */
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

#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

#include "tests/test_macros.h"

namespace TestEnvironment {

static void check_rtgi_backend_reaches_render_params(const Ref<Environment> &p_environment, int p_requested_backend) {
	p_environment->set_rtgi_backend((Environment::RTGIBackend)p_requested_backend);
	CHECK_EQ(p_environment->get_rtgi_backend(), Environment::RTGI_BACKEND_VULKAN_GENERIC);

	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);

	const RSE::PathtracingParams params = rendering_server->environment_get_pathtracing_params(p_environment->get_rid());
	CHECK_EQ(params.backend, RSE::PT_BACKEND_VULKAN_GENERIC);
}

TEST_CASE("[SceneTree][Environment] RTGI backend selection exposes only Vulkan Generic") {
	Ref<Environment> environment;
	environment.instantiate();

	CHECK_EQ(environment->get_rtgi_backend(), Environment::RTGI_BACKEND_VULKAN_GENERIC);
	RSE::PathtracingParams params = RenderingServer::get_singleton()->environment_get_pathtracing_params(environment->get_rid());
	CHECK_EQ(params.backend, RSE::PT_BACKEND_VULKAN_GENERIC);
	CHECK_EQ(params.mode, uint32_t(Environment::RTGI_MODE_HYBRID));

	check_rtgi_backend_reaches_render_params(environment, Environment::RTGI_BACKEND_VULKAN_GENERIC);
	check_rtgi_backend_reaches_render_params(environment, RSE::PT_BACKEND_NVIDIA_RTXPT);
	check_rtgi_backend_reaches_render_params(environment, RSE::PT_BACKEND_AMD_HIP_RT);
	check_rtgi_backend_reaches_render_params(environment, RSE::PT_BACKEND_INTEL_EMBREE);

	environment->set_rtgi_backend((Environment::RTGIBackend)-1);
	CHECK_EQ(environment->get_rtgi_backend(), Environment::RTGI_BACKEND_VULKAN_GENERIC);
	params = RenderingServer::get_singleton()->environment_get_pathtracing_params(environment->get_rid());
	CHECK_EQ(params.backend, RSE::PT_BACKEND_VULKAN_GENERIC);

	environment->set_rtgi_backend((Environment::RTGIBackend)RSE::PT_BACKEND_MAX);
	CHECK_EQ(environment->get_rtgi_backend(), Environment::RTGI_BACKEND_VULKAN_GENERIC);
	params = RenderingServer::get_singleton()->environment_get_pathtracing_params(environment->get_rid());
	CHECK_EQ(params.backend, RSE::PT_BACKEND_VULKAN_GENERIC);
}

TEST_CASE("[SceneTree][Environment] RTGI FPT reference oracle round-trips") {
	Ref<Environment> environment;
	environment.instantiate();

	// Default is the FPT-fast path (guide-surface NEE primary + probe indirect); the
	// deep-path A/B reference oracle is opt-in.
	CHECK_FALSE(environment->get_rtgi_fpt_reference());

	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);

	RSE::PathtracingParams params = rendering_server->environment_get_pathtracing_params(environment->get_rid());
	CHECK_FALSE(params.fpt_reference);

	// Enabling the oracle must round-trip through the serialized pathtracing params
	// (the renderer reads it from the param array, not the Environment object) and
	// land in repurposed slot 28 (PT_PARAM_RTGI_FPT_REFERENCE) when packed to floats.
	environment->set_rtgi_fpt_reference(true);
	CHECK(environment->get_rtgi_fpt_reference());

	params = rendering_server->environment_get_pathtracing_params(environment->get_rid());
	CHECK(params.fpt_reference);

	float packed[RSE::PT_PARAM_MAX] = {};
	RSE::pathtracing_params_to_shader_floats(params, packed, RSE::PT_PARAM_MAX);
	CHECK(packed[RSE::PT_PARAM_RTGI_FPT_REFERENCE] == 1.0f);

	// Disabling restores the FPT-fast default and the slot-28 serialization clears.
	environment->set_rtgi_fpt_reference(false);
	CHECK_FALSE(environment->get_rtgi_fpt_reference());

	params = rendering_server->environment_get_pathtracing_params(environment->get_rid());
	CHECK_FALSE(params.fpt_reference);

	RSE::pathtracing_params_to_shader_floats(params, packed, RSE::PT_PARAM_MAX);
	CHECK(packed[RSE::PT_PARAM_RTGI_FPT_REFERENCE] == 0.0f);
}

} // namespace TestEnvironment
