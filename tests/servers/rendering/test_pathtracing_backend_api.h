/**************************************************************************/
/*  test_pathtracing_backend_api.h                                        */
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
#include "modules/modules_enabled.gen.h"
#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#if defined(MODULE_RTXPT_ENABLED)
#include "modules/rtxpt/register_types.h"
#endif
#if defined(MODULE_HIPRT_ENABLED)
#include "modules/hiprt/register_types.h"
#endif
#if defined(MODULE_EMBREE_ENABLED)
#include "modules/embree/register_types.h"
#endif
#if defined(MODULE_OSPRAY_ENABLED)
#include "modules/ospray/register_types.h"
#endif
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include "tests/test_macros.h"

#if defined(WINDOWS_ENABLED)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace TestPathtracingBackendAPI {

static RDD::ExternalHandleType expected_platform_external_handle_type() {
#if defined(WINDOWS_ENABLED)
	return RDD::EXTERNAL_HANDLE_OPAQUE_WIN32;
#else
	return RDD::EXTERNAL_HANDLE_OPAQUE_FD;
#endif
}

static void close_external_handle(uint64_t p_handle, RDD::ExternalHandleType p_handle_type) {
	if (p_handle == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	if (p_handle_type == RDD::EXTERNAL_HANDLE_OPAQUE_WIN32) {
		CloseHandle((HANDLE)(uintptr_t)p_handle);
	}
#else
	if (p_handle_type == RDD::EXTERNAL_HANDLE_OPAQUE_FD) {
		close((int)p_handle);
	}
#endif
}

struct TestRenderingDeviceScope {
	RenderingDevice *rd = nullptr;
	RenderingContextDriver *context = nullptr;
	bool owns_rd = false;
	bool owns_context = false;

	TestRenderingDeviceScope() = default;
	TestRenderingDeviceScope(const TestRenderingDeviceScope &) = delete;
	TestRenderingDeviceScope &operator=(const TestRenderingDeviceScope &) = delete;

	TestRenderingDeviceScope(TestRenderingDeviceScope &&p_other) {
		rd = p_other.rd;
		context = p_other.context;
		owns_rd = p_other.owns_rd;
		owns_context = p_other.owns_context;
		p_other.rd = nullptr;
		p_other.context = nullptr;
		p_other.owns_rd = false;
		p_other.owns_context = false;
	}

	TestRenderingDeviceScope &operator=(TestRenderingDeviceScope &&p_other) {
		if (this == &p_other) {
			return *this;
		}
		if (owns_rd && rd != nullptr) {
			memdelete(rd);
		}
		if (owns_context && context != nullptr) {
			memdelete(context);
		}
		rd = p_other.rd;
		context = p_other.context;
		owns_rd = p_other.owns_rd;
		owns_context = p_other.owns_context;
		p_other.rd = nullptr;
		p_other.context = nullptr;
		p_other.owns_rd = false;
		p_other.owns_context = false;
		return *this;
	}

	~TestRenderingDeviceScope() {
		if (owns_rd && rd != nullptr) {
			memdelete(rd);
			rd = nullptr;
		}
		if (owns_context && context != nullptr) {
			memdelete(context);
			context = nullptr;
		}
	}
};

static TestRenderingDeviceScope make_test_rendering_device_scope() {
	TestRenderingDeviceScope scope;
	scope.rd = RD::get_singleton();
	if (scope.rd != nullptr) {
		return scope;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		scope.rd = rs->create_local_rendering_device();
		if (scope.rd != nullptr) {
			scope.owns_rd = true;
			return scope;
		}
	}

#if defined(VULKAN_ENABLED)
	scope.context = memnew(RenderingContextDriverVulkan);
	scope.rd = memnew(RenderingDevice);
	if (scope.context->initialize() == OK && scope.rd->initialize(scope.context) == OK) {
		scope.owns_context = true;
		scope.owns_rd = true;
		return scope;
	}

	if (scope.rd != nullptr) {
		memdelete(scope.rd);
		scope.rd = nullptr;
	}
	if (scope.context != nullptr) {
		memdelete(scope.context);
		scope.context = nullptr;
	}
#endif

	return scope;
}

static void check_backend_capability_dictionary_shape(const Dictionary &p_capability) {
	CHECK(p_capability.has("backend"));
	CHECK(p_capability.has("name"));
	CHECK(p_capability.has("available"));
	CHECK(p_capability.has("initialized"));
	CHECK(p_capability.has("runtime_name"));
	CHECK(p_capability.has("integration_path"));
	CHECK(p_capability.has("scene_import_path"));
	CHECK(p_capability.has("trace_dispatch_path"));
	CHECK(p_capability.has("vendor_scene_import"));
	CHECK(p_capability.has("vendor_sdk_dispatch"));
	CHECK(p_capability.has("rendering_device_family"));
	CHECK(p_capability.has("rendering_device_name"));
	CHECK(p_capability.has("rendering_device_vendor"));
	CHECK(p_capability.has("rendering_device_vendor_id"));
	CHECK(p_capability.has("vulkan_runtime"));
	CHECK(p_capability.has("vulkan_interop_mode"));
	CHECK(p_capability.has("resource_exchange_sync"));
	CHECK(p_capability.has("exchange"));
	CHECK(p_capability.has("exchange_summary"));
	CHECK(p_capability.has("availability_checks"));
	CHECK(p_capability.has("denoiser_handoff"));
	CHECK(p_capability.has("denoiser_name"));
	CHECK(p_capability.has("denoiser_runtime_detected"));
	CHECK(p_capability.has("denoiser_available"));
	CHECK(p_capability.has("denoiser_failure_reason"));
	CHECK(p_capability.has("denoiser_integration_path"));
	CHECK(p_capability.has("nvidia_streamline_headers_present"));
	CHECK(p_capability.has("nvidia_streamline_runtime_detected"));
	CHECK(p_capability.has("nvidia_dlss_rr_handoff_ready"));
	CHECK(p_capability.has("nvidia_dlss_rr_device_supported"));
	CHECK(p_capability.has("nvidia_dlss_rr_available"));
	CHECK(p_capability.has("nvidia_streamline_failure_reason"));
	CHECK(p_capability.has("nvidia_nrd_headers_present"));
	CHECK(p_capability.has("nvidia_nrd_version"));
	CHECK(p_capability.has("amd_fidelityfx_sdk_headers_present"));
	CHECK(p_capability.has("amd_fidelityfx_denoiser_version"));
	CHECK(p_capability.has("native_probe_update"));
	CHECK(p_capability.has("generic_probe_update_fallback"));
	CHECK(p_capability.has("probe_update_path"));
	CHECK(p_capability.has("fallback_reason"));

	const Variant exchange_variant = p_capability.get("exchange", Variant());
	CHECK_EQ(exchange_variant.get_type(), Variant::DICTIONARY);
	if (exchange_variant.get_type() != Variant::DICTIONARY) {
		return;
	}

	const Dictionary exchange = exchange_variant;
	CHECK(exchange.has("rendering_device"));
	CHECK(exchange.has("external_memory"));
	CHECK(exchange.has("external_semaphore"));
	CHECK(exchange.has("timeline_semaphore"));
	CHECK(exchange.has("staged_copy"));

	const Variant availability_checks_variant = p_capability.get("availability_checks", Variant());
	CHECK_EQ(availability_checks_variant.get_type(), Variant::DICTIONARY);
	if (availability_checks_variant.get_type() != Variant::DICTIONARY) {
		return;
	}

	const Dictionary availability_checks = availability_checks_variant;
	CHECK(availability_checks.has("backend_compiled"));
	CHECK(availability_checks.has("sdk_headers_present"));
	CHECK(availability_checks.has("runtime_detected"));
	CHECK(availability_checks.has("device_supported"));
	CHECK(availability_checks.has("resource_exchange_supported"));
	CHECK(availability_checks.has("implementation_ready"));
	CHECK(availability_checks.has("failure"));
	CHECK(availability_checks.has("compile_failure_reason"));
	CHECK(availability_checks.has("runtime_failure_reason"));
	CHECK(availability_checks.has("device_failure_reason"));
	CHECK(availability_checks.has("resource_exchange_failure_reason"));
	CHECK(availability_checks.has("implementation_failure_reason"));
}

static void check_backend_capability_dictionary_semantics(const Dictionary &p_capability) {
	const String runtime_name = String(p_capability.get("runtime_name", ""));
	const String integration_path = String(p_capability.get("integration_path", ""));
	const String scene_import_path = String(p_capability.get("scene_import_path", ""));
	const String trace_dispatch_path = String(p_capability.get("trace_dispatch_path", ""));
	const String rendering_device_family = String(p_capability.get("rendering_device_family", ""));
	const String rendering_device_name = String(p_capability.get("rendering_device_name", ""));
	const String rendering_device_vendor = String(p_capability.get("rendering_device_vendor", ""));
	const String denoiser_name = String(p_capability.get("denoiser_name", ""));
	const String denoiser_integration_path = String(p_capability.get("denoiser_integration_path", ""));
	const String probe_update_path = String(p_capability.get("probe_update_path", ""));
	const String fallback_reason = String(p_capability.get("fallback_reason", ""));
	const String nvidia_streamline_failure_reason = String(p_capability.get("nvidia_streamline_failure_reason", ""));
	const String vulkan_interop_mode = String(p_capability.get("vulkan_interop_mode", ""));
	const String resource_exchange_sync = String(p_capability.get("resource_exchange_sync", ""));
	CHECK_FALSE(runtime_name.is_empty());
	CHECK_FALSE(integration_path.is_empty());
	CHECK_FALSE(scene_import_path.is_empty());
	CHECK_FALSE(trace_dispatch_path.is_empty());
	CHECK_FALSE(rendering_device_family.is_empty());
	CHECK_FALSE(rendering_device_name.is_empty());
	CHECK_FALSE(rendering_device_vendor.is_empty());
	CHECK_FALSE(denoiser_name.is_empty());
	CHECK_FALSE(denoiser_integration_path.is_empty());
	CHECK_FALSE(probe_update_path.is_empty());
	CHECK_FALSE(vulkan_interop_mode.is_empty());
	CHECK_FALSE(resource_exchange_sync.is_empty());
	CHECK_NE(runtime_name, String("Unavailable"));
	CHECK_NE(integration_path, String("Unavailable"));
	CHECK_FALSE(runtime_name.to_lower().contains("d3d"));
	CHECK_FALSE(runtime_name.to_lower().contains("direct3d"));
	CHECK_FALSE(runtime_name.to_lower().contains("dx12"));
	CHECK_FALSE(integration_path.to_lower().contains("d3d"));
	CHECK_FALSE(integration_path.to_lower().contains("direct3d"));
	CHECK_FALSE(integration_path.to_lower().contains("dx12"));
	CHECK_FALSE(scene_import_path.to_lower().contains("d3d"));
	CHECK_FALSE(scene_import_path.to_lower().contains("direct3d"));
	CHECK_FALSE(scene_import_path.to_lower().contains("dx12"));
	CHECK_FALSE(trace_dispatch_path.to_lower().contains("d3d"));
	CHECK_FALSE(trace_dispatch_path.to_lower().contains("direct3d"));
	CHECK_FALSE(trace_dispatch_path.to_lower().contains("dx12"));
	CHECK_FALSE(fallback_reason.to_lower().contains("d3d"));
	CHECK_FALSE(fallback_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(fallback_reason.to_lower().contains("dx12"));
	CHECK_FALSE(denoiser_name.to_lower().contains("d3d"));
	CHECK_FALSE(denoiser_name.to_lower().contains("direct3d"));
	CHECK_FALSE(denoiser_name.to_lower().contains("dx12"));
	CHECK_FALSE(denoiser_integration_path.to_lower().contains("d3d"));
	CHECK_FALSE(denoiser_integration_path.to_lower().contains("direct3d"));
	CHECK_FALSE(denoiser_integration_path.to_lower().contains("dx12"));
	CHECK_FALSE(probe_update_path.to_lower().contains("d3d"));
	CHECK_FALSE(probe_update_path.to_lower().contains("direct3d"));
	CHECK_FALSE(probe_update_path.to_lower().contains("dx12"));
	CHECK_FALSE(nvidia_streamline_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(nvidia_streamline_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(nvidia_streamline_failure_reason.to_lower().contains("dx12"));

	const Dictionary exchange = p_capability["exchange"];
	const Dictionary availability_checks = p_capability["availability_checks"];
	const int backend = int(p_capability["backend"]);
	const String availability_failure = String(availability_checks.get("failure", ""));
	CHECK_FALSE(availability_failure.is_empty());
	CHECK_FALSE(availability_failure.to_lower().contains("d3d"));
	CHECK_FALSE(availability_failure.to_lower().contains("direct3d"));
	CHECK_FALSE(availability_failure.to_lower().contains("dx12"));
	const String compile_failure_reason = String(availability_checks.get("compile_failure_reason", ""));
	const String runtime_failure_reason = String(availability_checks.get("runtime_failure_reason", ""));
	const String device_failure_reason = String(availability_checks.get("device_failure_reason", ""));
	const String resource_exchange_failure_reason = String(availability_checks.get("resource_exchange_failure_reason", ""));
	const String implementation_failure_reason = String(availability_checks.get("implementation_failure_reason", ""));
	CHECK_FALSE(compile_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(compile_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(compile_failure_reason.to_lower().contains("dx12"));
	CHECK_FALSE(runtime_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(runtime_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(runtime_failure_reason.to_lower().contains("dx12"));
	CHECK_FALSE(device_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(device_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(device_failure_reason.to_lower().contains("dx12"));
	CHECK_FALSE(resource_exchange_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(resource_exchange_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(resource_exchange_failure_reason.to_lower().contains("dx12"));
	CHECK_FALSE(implementation_failure_reason.to_lower().contains("d3d"));
	CHECK_FALSE(implementation_failure_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(implementation_failure_reason.to_lower().contains("dx12"));
	CHECK_FALSE(fallback_reason.to_lower().contains("not wired"));
	CHECK_FALSE(runtime_failure_reason.to_lower().contains("not wired"));
	CHECK_FALSE(implementation_failure_reason.to_lower().contains("not wired"));
	CHECK_FALSE(vulkan_interop_mode.to_lower().contains("not wired"));
	CHECK_FALSE(resource_exchange_sync.to_lower().contains("not wired"));
	CHECK_FALSE(scene_import_path.to_lower().contains("not wired"));
	CHECK_FALSE(trace_dispatch_path.to_lower().contains("not wired"));
	if (backend == int(RSE::PT_BACKEND_NVIDIA_RTXPT)) {
		CHECK_FALSE(bool(p_capability["vendor_scene_import"]));
		CHECK_FALSE(bool(p_capability["vendor_sdk_dispatch"]));
		CHECK(String(p_capability["trace_dispatch_path"]).contains("Godot fork"));
		CHECK(String(p_capability["trace_dispatch_path"]).contains("not linked"));
#if defined(RTGI_STREAMLINE_SDK_HEADERS_PRESENT)
		CHECK(bool(p_capability["nvidia_streamline_headers_present"]));
#else
		CHECK_FALSE(bool(p_capability["nvidia_streamline_headers_present"]));
#endif
#if defined(RTGI_NRD_SDK_HEADERS_PRESENT)
		CHECK(bool(p_capability["nvidia_nrd_headers_present"]));
		CHECK_FALSE(String(p_capability["nvidia_nrd_version"]).is_empty());
#else
		CHECK_FALSE(bool(p_capability["nvidia_nrd_headers_present"]));
#endif
		const String nvidia_denoiser_integration_path = p_capability["denoiser_integration_path"];
		const bool nvidia_denoiser_path_names_vendor = nvidia_denoiser_integration_path.contains("NRD") || nvidia_denoiser_integration_path.contains("DLSS");
		CHECK(nvidia_denoiser_path_names_vendor);
		CHECK_EQ(bool(p_capability["nvidia_dlss_rr_available"]), bool(p_capability["available"]) && bool(p_capability["nvidia_streamline_headers_present"]) && bool(p_capability["nvidia_streamline_runtime_detected"]) && bool(p_capability["nvidia_dlss_rr_handoff_ready"]) && bool(p_capability["nvidia_dlss_rr_device_supported"]));
		if (!bool(p_capability["nvidia_streamline_runtime_detected"]) || (bool(p_capability["nvidia_streamline_runtime_detected"]) && !bool(p_capability["nvidia_dlss_rr_handoff_ready"]))) {
			CHECK_FALSE(nvidia_streamline_failure_reason.is_empty());
		}
		if (bool(p_capability["nvidia_dlss_rr_available"])) {
			CHECK(String(p_capability["denoiser_name"]).contains("DLSS Ray Reconstruction"));
			CHECK(String(p_capability["denoiser_failure_reason"]).is_empty());
		}
#if !defined(RTGI_NRD_DENOISER_HANDOFF_ENABLED)
		CHECK_EQ(bool(p_capability["denoiser_handoff"]), bool(p_capability["nvidia_dlss_rr_available"]));
		CHECK_EQ(bool(p_capability["denoiser_available"]), bool(p_capability["nvidia_dlss_rr_available"]));
#endif
	} else if (backend == int(RSE::PT_BACKEND_AMD_HIP_RT)) {
		CHECK(bool(p_capability["vendor_scene_import"]));
		CHECK(bool(p_capability["vendor_sdk_dispatch"]));
		CHECK(String(p_capability["scene_import_path"]).contains("HIP RT"));
		CHECK(String(p_capability["trace_dispatch_path"]).contains("HIP RT"));
		CHECK_FALSE(bool(p_capability["nvidia_streamline_headers_present"]));
		CHECK_FALSE(bool(p_capability["nvidia_streamline_runtime_detected"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_handoff_ready"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_device_supported"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_available"]));
#if defined(RTGI_FIDELITYFX_SDK_DENOISER_HEADERS_PRESENT)
		CHECK(bool(p_capability["amd_fidelityfx_sdk_headers_present"]));
		CHECK_FALSE(String(p_capability["amd_fidelityfx_denoiser_version"]).is_empty());
#else
		CHECK_FALSE(bool(p_capability["amd_fidelityfx_sdk_headers_present"]));
#endif
		CHECK(String(p_capability["denoiser_integration_path"]).contains("FidelityFX"));
#if !defined(RTGI_FIDELITYFX_DENOISER_HANDOFF_ENABLED)
		CHECK_FALSE(bool(p_capability["denoiser_handoff"]));
		CHECK_FALSE(bool(p_capability["denoiser_available"]));
#endif
	} else if (backend == int(RSE::PT_BACKEND_INTEL_EMBREE)) {
		CHECK_FALSE(bool(p_capability["vendor_scene_import"]));
		CHECK_FALSE(bool(p_capability["vendor_sdk_dispatch"]));
		CHECK_FALSE(bool(p_capability["nvidia_streamline_headers_present"]));
		CHECK_FALSE(bool(p_capability["nvidia_streamline_runtime_detected"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_handoff_ready"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_device_supported"]));
		CHECK_FALSE(bool(p_capability["nvidia_dlss_rr_available"]));
		CHECK(String(p_capability["denoiser_name"]).contains("disabled"));
		CHECK_FALSE(String(p_capability["denoiser_name"]).contains("Open Image Denoise"));
		CHECK_FALSE(String(p_capability["denoiser_name"]).contains("OIDN"));
		CHECK_FALSE(bool(p_capability["denoiser_handoff"]));
		CHECK_FALSE(bool(p_capability["denoiser_available"]));
	}
	const bool explicit_exchange = bool(exchange["rendering_device"]) || (bool(exchange["external_memory"]) && bool(exchange["external_semaphore"])) || bool(exchange["timeline_semaphore"]) || bool(exchange["staged_copy"]);
	if (bool(p_capability["available"])) {
		CHECK(explicit_exchange);
		CHECK_EQ(rendering_device_family, String("vulkan"));
		CHECK_NE(vulkan_interop_mode, String("none"));
		CHECK_NE(resource_exchange_sync, String("none"));
		CHECK(bool(p_capability["vulkan_runtime"]));
		CHECK(bool(availability_checks["backend_compiled"]));
		CHECK(bool(availability_checks["runtime_detected"]));
		CHECK(bool(availability_checks["device_supported"]));
		CHECK(bool(availability_checks["resource_exchange_supported"]));
		CHECK(bool(availability_checks["implementation_ready"]));
		CHECK_EQ(availability_failure, String("none"));
		const bool probe_path_declared = bool(p_capability["native_probe_update"]) || bool(p_capability["generic_probe_update_fallback"]);
		CHECK(probe_path_declared);
	} else {
		CHECK_FALSE(String(p_capability["fallback_reason"]).is_empty());
		CHECK_FALSE(bool(p_capability["denoiser_handoff"]));
		CHECK_EQ(bool(p_capability["denoiser_handoff"]), bool(p_capability["denoiser_available"]));
		CHECK_NE(availability_failure, String("none"));
		if (availability_failure == "backend_not_compiled") {
			CHECK_FALSE(compile_failure_reason.is_empty());
		} else if (availability_failure == "runtime_not_detected") {
			CHECK_FALSE(runtime_failure_reason.is_empty());
		} else if (availability_failure == "device_not_supported") {
			CHECK_FALSE(device_failure_reason.is_empty());
		} else if (availability_failure == "resource_exchange_unavailable") {
			CHECK_FALSE(resource_exchange_failure_reason.is_empty());
		} else if (availability_failure == "implementation_unavailable") {
			CHECK_FALSE(implementation_failure_reason.is_empty());
		}
	}
}

TEST_CASE("[SceneTree][RenderingServer][PathTracing] Backend status and capability dictionaries expose the stable contract") {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);

	const Dictionary status = rendering_server->pathtracing_get_backend_status();
	CHECK(status.has("requested_backend"));
	CHECK(status.has("requested_backend_name"));
	CHECK(status.has("active_backend"));
	CHECK(status.has("active_backend_name"));
	CHECK(status.has("requested_backend_available"));
	CHECK(status.has("active_backend_available"));
	CHECK(status.has("requested_backend_initialized"));
	CHECK(status.has("active_backend_initialized"));
	CHECK(status.has("using_fallback"));
	CHECK(status.has("fallback_backend"));
	CHECK(status.has("fallback_backend_name"));
	CHECK(status.has("fallback_reason"));
	CHECK(status.has("requested_capabilities"));
	CHECK(status.has("active_capabilities"));
	const String status_fallback_reason = String(status.get("fallback_reason", ""));
	CHECK_FALSE(status_fallback_reason.to_lower().contains("d3d"));
	CHECK_FALSE(status_fallback_reason.to_lower().contains("direct3d"));
	CHECK_FALSE(status_fallback_reason.to_lower().contains("dx12"));

	const Variant requested_capabilities_variant = status.get("requested_capabilities", Variant());
	REQUIRE_EQ(requested_capabilities_variant.get_type(), Variant::DICTIONARY);
	const Dictionary requested_capabilities = requested_capabilities_variant;
	check_backend_capability_dictionary_shape(requested_capabilities);
	check_backend_capability_dictionary_semantics(requested_capabilities);

	const Variant active_capabilities_variant = status.get("active_capabilities", Variant());
	REQUIRE_EQ(active_capabilities_variant.get_type(), Variant::DICTIONARY);
	const Dictionary active_capabilities = active_capabilities_variant;
	check_backend_capability_dictionary_shape(active_capabilities);
	check_backend_capability_dictionary_semantics(active_capabilities);

	CHECK_EQ(bool(status["requested_backend_available"]), bool(requested_capabilities["available"]));
	CHECK_EQ(bool(status["active_backend_available"]), bool(active_capabilities["available"]));
	CHECK_EQ(bool(status["requested_backend_initialized"]), bool(requested_capabilities["initialized"]));
	CHECK_EQ(bool(status["active_backend_initialized"]), bool(active_capabilities["initialized"]));
	if (bool(status["using_fallback"])) {
		CHECK_EQ(int(status["fallback_backend"]), int(status["active_backend"]));
		CHECK_EQ(String(status["fallback_backend_name"]), String(status["active_backend_name"]));
	} else {
		CHECK_EQ(int(status["fallback_backend"]), -1);
		CHECK(String(status["fallback_backend_name"]).is_empty());
	}

	const Array capabilities = rendering_server->pathtracing_get_backend_capabilities();
	REQUIRE_EQ(capabilities.size(), RenderingServer::PT_BACKEND_MAX);
	for (int i = 0; i < capabilities.size(); i++) {
		REQUIRE_EQ(capabilities[i].get_type(), Variant::DICTIONARY);
		const Dictionary capability = capabilities[i];
		check_backend_capability_dictionary_shape(capability);
		check_backend_capability_dictionary_semantics(capability);
		CHECK_EQ(int(capability["backend"]), i);
	}

	const Dictionary generic_request_status = rendering_server->pathtracing_get_backend_status_for_backend(RenderingServer::PT_BACKEND_VULKAN_GENERIC);
	CHECK_EQ(int(generic_request_status["requested_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK_EQ(int(generic_request_status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK_FALSE(bool(generic_request_status["using_fallback"]));
	CHECK_EQ(int(generic_request_status["fallback_backend"]), -1);
	CHECK(String(generic_request_status["fallback_backend_name"]).is_empty());

	const Dictionary invalid_request_status = rendering_server->pathtracing_get_backend_status_for_backend((RenderingServer::PathtracingBackend)RenderingServer::PT_BACKEND_MAX);
	CHECK_EQ(int(invalid_request_status["requested_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK_EQ(int(invalid_request_status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK_FALSE(bool(invalid_request_status["using_fallback"]));
	CHECK_EQ(int(invalid_request_status["fallback_backend"]), -1);

	const RSE::PathtracingBackend vendor_backends[] = {
		RSE::PT_BACKEND_NVIDIA_RTXPT,
		RSE::PT_BACKEND_AMD_HIP_RT,
	};
	for (RSE::PathtracingBackend backend : vendor_backends) {
		const Dictionary scoped_status = rendering_server->pathtracing_get_backend_status_for_backend((RenderingServer::PathtracingBackend)backend);
		const Dictionary requested_capabilities = scoped_status["requested_capabilities"];
		const Dictionary active_capabilities = scoped_status["active_capabilities"];
		const bool requested_available = bool(requested_capabilities["available"]);

		CHECK_EQ(int(scoped_status["requested_backend"]), int(backend));
		CHECK_EQ(bool(scoped_status["requested_backend_available"]), requested_available);
		CHECK_EQ(int(requested_capabilities["backend"]), int(backend));

		if (requested_available) {
			CHECK_EQ(int(scoped_status["active_backend"]), int(backend));
			CHECK_FALSE(bool(scoped_status["using_fallback"]));
			CHECK_EQ(int(scoped_status["fallback_backend"]), -1);
			CHECK(String(scoped_status["fallback_backend_name"]).is_empty());
			CHECK_EQ(int(active_capabilities["backend"]), int(backend));
		} else {
			CHECK_EQ(int(scoped_status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK(bool(scoped_status["using_fallback"]));
			CHECK_EQ(int(scoped_status["fallback_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK_EQ(String(scoped_status["fallback_backend_name"]), String("Vulkan Generic"));
			CHECK_EQ(int(active_capabilities["backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK_FALSE(String(scoped_status["fallback_reason"]).is_empty());
			CHECK_FALSE(String(scoped_status["fallback_reason"]).to_lower().contains("d3d"));
			CHECK_FALSE(String(scoped_status["fallback_reason"]).to_lower().contains("direct3d"));
			CHECK_FALSE(String(scoped_status["fallback_reason"]).to_lower().contains("dx12"));
		}
	}
}

TEST_CASE("[RenderingServer][PathTracing] Vendor RTGI optional modules expose compiled backend descriptors") {
#if defined(MODULE_RTXPT_ENABLED)
	const bool expected_rtxpt_implemented =
#if defined(RTGI_RTXPT_BACKEND_IMPLEMENTED)
			true;
#else
			false;
#endif
	const bool expected_rtxpt_reference_dispatch =
#if defined(RTGI_RTXPT_GODOT_REFERENCE_DISPATCH_ENABLED)
			true;
#else
			false;
#endif
	CHECK_EQ(rtxpt_module_has_rtgi_backend_implementation(), expected_rtxpt_implemented);
	CHECK_EQ(rtxpt_module_has_godot_reference_dispatch(), expected_rtxpt_reference_dispatch);
	if (expected_rtxpt_implemented && expected_rtxpt_reference_dispatch) {
		CHECK(rtxpt_module_is_rtgi_backend_registered());
	}
#endif

#if defined(MODULE_HIPRT_ENABLED)
	const bool expected_hiprt_implemented =
#if defined(RTGI_HIPRT_BACKEND_IMPLEMENTED)
			true;
#else
			false;
#endif
	CHECK_EQ(hiprt_module_has_rtgi_backend_implementation(), expected_hiprt_implemented);
#if defined(RTGI_HIPRT_API_VERSION) && RTGI_HIPRT_API_VERSION != 0
	CHECK_EQ(hiprt_module_get_api_version(), uint32_t(RTGI_HIPRT_API_VERSION));
	CHECK_EQ(String(hiprt_module_get_version_string()), String(RTGI_HIPRT_VERSION_STR));
	if (expected_hiprt_implemented) {
		CHECK(hiprt_module_is_rtgi_backend_registered());
	}
#else
	CHECK_EQ(hiprt_module_get_api_version(), uint32_t(0));
#endif
#endif

#if defined(MODULE_EMBREE_ENABLED)
	CHECK_FALSE(embree_module_has_rtgi_backend_implementation());
	CHECK_FALSE(embree_module_has_cpu_dispatch());
	CHECK_FALSE(embree_module_is_rtgi_backend_registered());
#endif

#if defined(MODULE_OSPRAY_ENABLED)
	CHECK_FALSE(ospray_module_has_rtgi_backend_implementation());
	CHECK_FALSE(ospray_module_has_dispatch());
	CHECK_FALSE(ospray_module_is_rtgi_backend_registered());
#endif
}

TEST_CASE("[RenderingServer][PathTracing] Backend environment parameter mapping preserves active backends and disables the Intel CPU value") {
	CHECK_EQ(int(Environment::RTGI_BACKEND_VULKAN_GENERIC), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK_EQ(int(Environment::RTGI_BACKEND_NVIDIA_RTXPT), int(RSE::PT_BACKEND_NVIDIA_RTXPT));
	CHECK_EQ(int(Environment::RTGI_BACKEND_AMD_HIP_RT), int(RSE::PT_BACKEND_AMD_HIP_RT));
	CHECK_EQ(int(Environment::RTGI_BACKEND_INTEL_EMBREE), int(RSE::PT_BACKEND_INTEL_EMBREE));

	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(float(RSE::PT_BACKEND_VULKAN_GENERIC)), RSE::PT_BACKEND_VULKAN_GENERIC);
	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(float(RSE::PT_BACKEND_NVIDIA_RTXPT)), RSE::PT_BACKEND_NVIDIA_RTXPT);
	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(float(RSE::PT_BACKEND_AMD_HIP_RT)), RSE::PT_BACKEND_AMD_HIP_RT);
	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(float(RSE::PT_BACKEND_INTEL_EMBREE)), RSE::PT_BACKEND_VULKAN_GENERIC);

	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(-1.0f), RSE::PT_BACKEND_VULKAN_GENERIC);
	CHECK_EQ(RendererSceneRenderImplementation::RenderRaytracing::backend_from_env_param(float(RSE::PT_BACKEND_MAX)), RSE::PT_BACKEND_VULKAN_GENERIC);
}

TEST_CASE("[RenderingServer][PathTracing] Vendor backend selection preserves the request and falls back only when unavailable") {
	const RSE::PathtracingBackend vendor_backends[] = {
		RSE::PT_BACKEND_NVIDIA_RTXPT,
		RSE::PT_BACKEND_AMD_HIP_RT,
	};

	for (RSE::PathtracingBackend backend : vendor_backends) {
		const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(backend);
		const Dictionary requested_capabilities = status["requested_capabilities"];
		const Dictionary active_capabilities = status["active_capabilities"];
		const bool requested_available = bool(requested_capabilities["available"]);

		CHECK_EQ(int(status["requested_backend"]), int(backend));
		CHECK_EQ(bool(status["requested_backend_available"]), requested_available);
		CHECK_FALSE(bool(status["requested_backend_initialized"]));
		CHECK_FALSE(String(status["fallback_reason"]).to_lower().contains("d3d"));
		CHECK_FALSE(String(status["fallback_reason"]).to_lower().contains("direct3d"));
		CHECK_FALSE(String(status["fallback_reason"]).to_lower().contains("dx12"));
		CHECK_EQ(int(requested_capabilities["backend"]), int(backend));
		const Dictionary availability_checks = requested_capabilities["availability_checks"];

		if (requested_available) {
			CHECK_EQ(String(availability_checks["failure"]), String("none"));
			CHECK_EQ(int(status["active_backend"]), int(backend));
			CHECK_FALSE(bool(status["using_fallback"]));
			CHECK_EQ(int(status["fallback_backend"]), -1);
			CHECK(String(status["fallback_backend_name"]).is_empty());
			CHECK_EQ(int(active_capabilities["backend"]), int(backend));
		} else {
			CHECK_NE(String(availability_checks["failure"]), String("none"));
			CHECK_EQ(int(status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK(bool(status["using_fallback"]));
			CHECK_EQ(int(status["fallback_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK_EQ(String(status["fallback_backend_name"]), String("Vulkan Generic"));
			CHECK_EQ(int(active_capabilities["backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
			CHECK_FALSE(String(status["fallback_reason"]).is_empty());
			CHECK_FALSE(String(requested_capabilities["fallback_reason"]).is_empty());
		}
	}
}

TEST_CASE("[RenderingServer][PathTracing] Vendor backend compiled checks are tied to backend-specific optional modules") {
	auto check_backend_not_compiled = [](RSE::PathtracingBackend p_backend) {
		const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(p_backend);
		const Dictionary requested_capabilities = status["requested_capabilities"];
		const Dictionary availability_checks = requested_capabilities["availability_checks"];
		CHECK_FALSE(bool(availability_checks["backend_compiled"]));
		CHECK_FALSE(bool(requested_capabilities["available"]));
		CHECK_EQ(String(availability_checks["failure"]), String("backend_not_compiled"));
		CHECK_FALSE(String(requested_capabilities["fallback_reason"]).is_empty());
	};

#ifndef MODULE_RTXPT_ENABLED
	check_backend_not_compiled(RSE::PT_BACKEND_NVIDIA_RTXPT);
#endif

#ifndef MODULE_HIPRT_ENABLED
	check_backend_not_compiled(RSE::PT_BACKEND_AMD_HIP_RT);
#endif

	check_backend_not_compiled(RSE::PT_BACKEND_INTEL_EMBREE);
}

TEST_CASE("[RenderingServer][PathTracing] Compiled vendor adapters remain unavailable until SDK entrypoints are linked") {
	auto check_compiled_adapter_gate = [](RSE::PathtracingBackend p_backend) {
		const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(p_backend);
		const Dictionary requested_capabilities = status["requested_capabilities"];
		const Dictionary availability_checks = requested_capabilities["availability_checks"];
		CHECK(bool(availability_checks["backend_compiled"]));
		CHECK_FALSE(bool(availability_checks["implementation_ready"]));
		CHECK_FALSE(bool(requested_capabilities["available"]));
		CHECK_FALSE(String(availability_checks["implementation_failure_reason"]).is_empty());
		CHECK_FALSE(String(requested_capabilities["fallback_reason"]).is_empty());
		CHECK_FALSE(String(requested_capabilities["probe_update_path"]).is_empty());
		CHECK_FALSE(bool(requested_capabilities["denoiser_handoff"]));
		CHECK_EQ(bool(requested_capabilities["denoiser_available"]), bool(requested_capabilities["denoiser_handoff"]));
		CHECK_EQ(int(status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
		CHECK(bool(status["using_fallback"]));
		CHECK_EQ(int(status["fallback_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
		CHECK_EQ(String(status["fallback_backend_name"]), String("Vulkan Generic"));
	};

#if defined(MODULE_RTXPT_ENABLED) && (!defined(RTGI_RTXPT_BACKEND_IMPLEMENTED) || !defined(RTGI_RTXPT_GODOT_REFERENCE_DISPATCH_ENABLED))
	check_compiled_adapter_gate(RSE::PT_BACKEND_NVIDIA_RTXPT);
#endif

#if defined(MODULE_HIPRT_ENABLED) && !defined(RTGI_HIPRT_BACKEND_IMPLEMENTED)
	check_compiled_adapter_gate(RSE::PT_BACKEND_AMD_HIP_RT);
#endif

}

TEST_CASE("[RenderingServer][PathTracing] RTXPT backend reports linked NVIDIA fork-compatible dispatch when compiled") {
#if defined(RTGI_RTXPT_BACKEND_IMPLEMENTED) && defined(RTGI_RTXPT_GODOT_REFERENCE_DISPATCH_ENABLED)
	const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(RSE::PT_BACKEND_NVIDIA_RTXPT);
	const Dictionary requested_capabilities = status["requested_capabilities"];
	const Dictionary availability_checks = requested_capabilities["availability_checks"];
	CHECK(bool(availability_checks["backend_compiled"]));
	CHECK(bool(availability_checks["sdk_headers_present"]));
	CHECK(bool(availability_checks["runtime_detected"]));
	CHECK(bool(availability_checks["implementation_ready"]));
	CHECK(String(availability_checks["runtime_failure_reason"]).is_empty());
	CHECK(String(requested_capabilities["runtime_name"]).contains("RenderingDevice"));
	CHECK(String(requested_capabilities["integration_path"]).contains("NVIDIA Godot fork"));
	CHECK_FALSE(bool(requested_capabilities["vendor_scene_import"]));
	CHECK_FALSE(bool(requested_capabilities["vendor_sdk_dispatch"]));
	CHECK(String(requested_capabilities["scene_import_path"]).contains("manifest"));
	CHECK(String(requested_capabilities["trace_dispatch_path"]).contains("not linked"));
	CHECK(String(requested_capabilities["denoiser_integration_path"]).contains("NRD"));
#if defined(RTGI_NRD_SDK_HEADERS_PRESENT)
	CHECK(bool(requested_capabilities["nvidia_nrd_headers_present"]));
	CHECK_FALSE(String(requested_capabilities["nvidia_nrd_version"]).is_empty());
#endif
#if defined(RTGI_STREAMLINE_SDK_HEADERS_PRESENT)
	CHECK(bool(requested_capabilities["nvidia_streamline_headers_present"]));
#endif
	CHECK(bool(requested_capabilities["generic_probe_update_fallback"]));
	CHECK_FALSE(bool(requested_capabilities["native_probe_update"]));
	CHECK(String(requested_capabilities["probe_update_path"]).contains("Vulkan Generic"));
#endif
}

TEST_CASE("[RenderingServer][PathTracing] HIP RT backend reports linked trace dispatch when compiled") {
#if defined(RTGI_HIPRT_BACKEND_IMPLEMENTED)
	const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(RSE::PT_BACKEND_AMD_HIP_RT);
	const Dictionary requested_capabilities = status["requested_capabilities"];
	const Dictionary availability_checks = requested_capabilities["availability_checks"];
	CHECK(bool(availability_checks["backend_compiled"]));
	CHECK(bool(availability_checks["sdk_headers_present"]));
	CHECK(bool(availability_checks["implementation_ready"]));
	CHECK(String(requested_capabilities["runtime_name"]).contains("HIP RT"));
	CHECK(String(requested_capabilities["integration_path"]).contains("Vulkan/HIP"));
	CHECK(bool(requested_capabilities["vendor_scene_import"]));
	CHECK(bool(requested_capabilities["vendor_sdk_dispatch"]));
	CHECK(String(requested_capabilities["scene_import_path"]).contains("HIP RT"));
	CHECK(String(requested_capabilities["trace_dispatch_path"]).contains("HIP RT"));
	CHECK(String(requested_capabilities["denoiser_name"]).contains("FidelityFX"));
	CHECK(String(requested_capabilities["denoiser_integration_path"]).contains("FidelityFX"));
#if defined(RTGI_FIDELITYFX_SDK_DENOISER_HEADERS_PRESENT)
	CHECK(bool(requested_capabilities["amd_fidelityfx_sdk_headers_present"]));
	CHECK_FALSE(String(requested_capabilities["amd_fidelityfx_denoiser_version"]).is_empty());
#endif
#if defined(RTGI_HIPRT_API_VERSION) && RTGI_HIPRT_API_VERSION != 0
	CHECK_FALSE(String(RTGI_HIPRT_VERSION_STR).is_empty());
	CHECK_EQ(String(RTGI_HIPRT_VERSION_STR).length(), 5);
#endif
	const String vulkan_interop_mode = requested_capabilities["vulkan_interop_mode"];
	const String resource_exchange_sync = requested_capabilities["resource_exchange_sync"];
	const bool expected_hiprt_interop_mode = vulkan_interop_mode == "none" || vulkan_interop_mode.contains("external_memory");
	CHECK(expected_hiprt_interop_mode);
	CHECK_FALSE(resource_exchange_sync.is_empty());
	if (vulkan_interop_mode.contains("external_memory")) {
		CHECK(resource_exchange_sync.contains("semaphore"));
	}
	CHECK(bool(requested_capabilities["generic_probe_update_fallback"]));
#endif
}

TEST_CASE("[RenderingServer][PathTracing] Intel Embree/OSPRay CPU backend stays disabled") {
	const Dictionary status = RendererSceneRenderImplementation::RenderRaytracing::get_static_backend_status_dictionary(RSE::PT_BACKEND_INTEL_EMBREE);
	const Dictionary requested_capabilities = status["requested_capabilities"];
	const Dictionary availability_checks = requested_capabilities["availability_checks"];
	CHECK_FALSE(bool(availability_checks["backend_compiled"]));
	CHECK_FALSE(bool(availability_checks["sdk_headers_present"]));
	CHECK_FALSE(bool(availability_checks["implementation_ready"]));
	CHECK_EQ(String(availability_checks["failure"]), String("backend_not_compiled"));
	CHECK(String(requested_capabilities["name"]).contains("disabled"));
	CHECK(String(requested_capabilities["integration_path"]).contains("disabled"));
	CHECK(String(requested_capabilities["fallback_reason"]).contains("disabled"));
	CHECK_EQ(int(status["active_backend"]), int(RSE::PT_BACKEND_VULKAN_GENERIC));
	CHECK(bool(status["using_fallback"]));
}

TEST_CASE("[SceneTree][RenderingDevice][PathTracing] Vulkan exportable texture and semaphore handles are gated by exportable allocation") {
	TestRenderingDeviceScope rd_scope = make_test_rendering_device_scope();
	RenderingDevice *rd = rd_scope.rd;
	if (rd == nullptr) {
		MESSAGE("Skipping Vulkan RTGI interop test because no global or local RenderingDevice could be created in this test configuration.");
		return;
	}
	if (rd->get_device_capabilities().device_family != RDD::DEVICE_VULKAN) {
		MESSAGE("Skipping Vulkan RTGI interop test because the active RenderingDevice is not Vulkan.");
		return;
	}

	const RDD::Capabilities &capabilities = rd->get_device_capabilities();
	if (!capabilities.external_memory_supported || !capabilities.external_semaphore_supported) {
		MESSAGE("Skipping Vulkan RTGI interop export test because external memory/semaphore export is not supported by this device.");
		return;
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = 2;
	tf.height = 2;
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.samples = RD::TEXTURE_SAMPLES_1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID normal_texture = rd->texture_create(tf, RD::TextureView());
	REQUIRE(normal_texture.is_valid());
	RDD::ExternalMemoryHandleInfo normal_memory_info;
	ERR_PRINT_OFF;
	const bool normal_exported = rd->texture_get_external_memory_handle(normal_texture, normal_memory_info);
	ERR_PRINT_ON;
	CHECK_FALSE(normal_exported);
	rd->free_rid(normal_texture);

	RID exportable_texture = rd->texture_create_exportable(tf, RD::TextureView());
	REQUIRE(exportable_texture.is_valid());
	const RD::TextureFormat exportable_format = rd->texture_get_format(exportable_texture);
	CHECK(exportable_format.is_external_memory_exportable);

	RDD::ExternalMemoryHandleInfo memory_info;
	CHECK(rd->texture_get_external_memory_handle(exportable_texture, memory_info));
	CHECK(memory_info.exportable);
	CHECK_NE(memory_info.handle, uint64_t(0));
	CHECK_EQ(memory_info.handle_type, expected_platform_external_handle_type());
	CHECK_GT(memory_info.allocation_size, uint64_t(0));
	CHECK(memory_info.dedicated_allocation);
	close_external_handle(memory_info.handle, memory_info.handle_type);
	rd->free_rid(exportable_texture);

	RDD::SemaphoreID semaphore = rd->external_semaphore_create();
	REQUIRE(semaphore);
	RDD::ExternalSemaphoreHandleInfo semaphore_info;
	CHECK(rd->external_semaphore_get_handle(semaphore, false, 0, semaphore_info));
	CHECK(semaphore_info.exportable);
	CHECK_FALSE(semaphore_info.timeline);
	CHECK_NE(semaphore_info.handle, uint64_t(0));
	CHECK_EQ(semaphore_info.handle_type, expected_platform_external_handle_type());
	close_external_handle(semaphore_info.handle, semaphore_info.handle_type);
	rd->external_semaphore_free(semaphore);
}

TEST_CASE("[SceneTree][RenderingDevice][PathTracing] Vulkan RTGI smoke exchange declares handles callbacks and ownership") {
	TestRenderingDeviceScope rd_scope = make_test_rendering_device_scope();
	RenderingDevice *rd = rd_scope.rd;
	if (rd == nullptr) {
		MESSAGE("Skipping Vulkan RTGI smoke exchange test because no global or local RenderingDevice could be created in this test configuration.");
		return;
	}
	if (rd->get_device_capabilities().device_family != RDD::DEVICE_VULKAN) {
		MESSAGE("Skipping Vulkan RTGI smoke exchange test because the active RenderingDevice is not Vulkan.");
		return;
	}
	if (!rd->get_device_capabilities().external_memory_supported || !rd->get_device_capabilities().external_semaphore_supported) {
		MESSAGE("Skipping Vulkan RTGI smoke exchange test because external memory/semaphore export is not supported by this device.");
		return;
	}

	Dictionary result;
	String failure_reason;
	CHECK(RendererSceneRenderImplementation::RenderRaytracing::test_vulkan_external_resource_exchange(rd, &result, &failure_reason));
	CHECK(String(failure_reason).is_empty());
	CHECK(bool(result["exchange_ok"]));
	CHECK(bool(result["metadata_ok"]));
	CHECK(bool(result["output_texture_valid"]));
	CHECK(bool(result["native_device_context_valid"]));
	CHECK(bool(result["streamline_native_contexts_ok"]));
	CHECK_NE(uint64_t(result["external_memory_handle"]), uint64_t(0));
	CHECK_NE(uint64_t(result["external_wait_semaphore_handle"]), uint64_t(0));
	CHECK_NE(uint64_t(result["external_signal_semaphore_handle"]), uint64_t(0));
	CHECK_EQ(int(result["external_memory_handle_type"]), int(expected_platform_external_handle_type()));
	CHECK_EQ(int(result["external_wait_semaphore_handle_type"]), int(expected_platform_external_handle_type()));
	CHECK_EQ(int(result["external_signal_semaphore_handle_type"]), int(expected_platform_external_handle_type()));
	CHECK_GT(uint64_t(result["external_memory_allocation_size"]), uint64_t(0));
	CHECK(bool(result["external_memory_dedicated_allocation"]));
	CHECK_NE(uint64_t(result["native_device_topmost_object"]), uint64_t(0));
	CHECK_NE(uint64_t(result["native_device_logical_device"]), uint64_t(0));
	CHECK_NE(uint64_t(result["native_device_command_queue"]), uint64_t(0));
	CHECK(bool(result["output_native_texture_valid"]));
	CHECK(bool(result["depth_native_texture_valid"]));
	CHECK(bool(result["velocity_native_texture_valid"]));
	CHECK(bool(result["diffuse_radiance_native_texture_valid"]));
	CHECK(bool(result["specular_radiance_native_texture_valid"]));
	CHECK(bool(result["dlss_rr_diffuse_albedo_native_texture_valid"]));
	CHECK(bool(result["dlss_rr_specular_albedo_native_texture_valid"]));
	CHECK(bool(result["dlss_rr_normal_roughness_native_texture_valid"]));
	CHECK(bool(result["dlss_rr_specular_hit_dist_native_texture_valid"]));
	CHECK(bool(result["acquire_callback_declared"]));
	CHECK(bool(result["release_callback_declared"]));
	CHECK(bool(result["rd_owns_output_after_dispatch"]));
}

} // namespace TestPathtracingBackendAPI
