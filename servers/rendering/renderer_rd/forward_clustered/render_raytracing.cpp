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
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "modules/modules_enabled.gen.h"
#include "servers/rendering/renderer_rd/forward_clustered/rtgi_blue_noise_128_rgba.inc"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
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

#include <cstddef>
#include <cstring>

#if defined(LINUXBSD_ENABLED)
#include <dlfcn.h>
#endif

#if !defined(WINDOWS_ENABLED)
#include <unistd.h>
#endif

#if defined(WINDOWS_ENABLED)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace RendererSceneRenderImplementation;

#if defined(MODULE_RAYCAST_ENABLED) && defined(RTGI_BUILTIN_EMBREE_ENABLED)
#ifndef RTGI_EMBREE_CPU_DISPATCH_ENABLED
#define RTGI_EMBREE_CPU_DISPATCH_ENABLED
#endif
#ifndef RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT
#define RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT
#endif
#ifndef RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED
#define RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED
#endif
#include "thirdparty/embree/include/embree4/rtcore.h"
#endif

#if defined(MODULE_OSPRAY_ENABLED) && defined(RTGI_OSPRAY_DISPATCH_ENABLED)
#ifndef RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED
#define RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED
#endif
#endif

static constexpr real_t RT_COMPRESSED_AABB_EPSILON = 0.0001;
static constexpr uint32_t RTGI_MAX_EMISSIVE_CANDIDATES = 512;
static constexpr uint32_t RTGI_MAX_EMISSIVE_PRIMITIVE_DISTRIBUTIONS = 65536;
static constexpr uint32_t RTGI_MAX_EMISSIVE_PRIMITIVES_PER_CANDIDATE = 4096;
static constexpr uint32_t RT_EMISSIVE_CANDIDATE_FLAG_COMPRESSED_GEOMETRY = 1u;
static constexpr uint32_t RT_EMISSIVE_CANDIDATE_FLAG_PRIMITIVE_DISTRIBUTION = 2u;
static constexpr uint32_t RT_EMISSIVE_CANDIDATE_FLAG_TEXTURED_EMISSION = 4u;

template <typename PackedFloat3>
static _ALWAYS_INLINE_ void _rtgi_store_packed_float3(PackedFloat3 &r_dst, float p_x, float p_y, float p_z) {
	r_dst.x = p_x;
	r_dst.y = p_y;
	r_dst.z = p_z;
}

template <typename PackedFloat3>
static void _rtgi_pack_transformed_vertices_float3_scalar(const Vector3 *p_src, uint32_t p_count, const Transform3D &p_transform, PackedFloat3 *r_dst) {
	if (p_count == 0) {
		return;
	}

	for (uint32_t i = 0; i < p_count; i++) {
		const Vector3 transformed = p_transform.xform(p_src[i]);
		_rtgi_store_packed_float3(r_dst[i], float(transformed.x), float(transformed.y), float(transformed.z));
	}
}

#if defined(DEV_ENABLED)
static _ALWAYS_INLINE_ bool _rtgi_float3_pack_component_matches(float p_packed, float p_scalar) {
	const float scale = MAX(1.0f, MAX(Math::abs(p_packed), Math::abs(p_scalar)));
	return Math::abs(p_packed - p_scalar) <= scale * 0.00001f;
}

template <typename PackedFloat3>
static bool _rtgi_validate_transformed_vertices_float3_pack_sample(const Vector3 *p_src, uint32_t p_count, const Transform3D &p_transform, const PackedFloat3 *p_dst) {
	if (p_count == 0) {
		return true;
	}

	const uint32_t samples[] = {
		0,
		MIN(7u, p_count - 1),
		p_count / 2,
		p_count - 1,
	};

	for (uint32_t sample_index = 0; sample_index < sizeof(samples) / sizeof(samples[0]); sample_index++) {
		const uint32_t vertex_index = samples[sample_index];
		const Vector3 scalar = p_transform.xform(p_src[vertex_index]);
		const PackedFloat3 &packed = p_dst[vertex_index];
		if (!_rtgi_float3_pack_component_matches(packed.x, float(scalar.x)) ||
				!_rtgi_float3_pack_component_matches(packed.y, float(scalar.y)) ||
				!_rtgi_float3_pack_component_matches(packed.z, float(scalar.z))) {
			return false;
		}
	}
	return true;
}
#endif

template <typename PackedFloat3>
static void _rtgi_pack_transformed_vertices_float3(const Vector3 *p_src, uint32_t p_count, const Transform3D &p_transform, PackedFloat3 *r_dst) {
	static_assert(sizeof(PackedFloat3) == sizeof(float) * 3, "Packed path tracing vertices must be tightly packed float3 values.");
	if (p_count == 0) {
		return;
	}

	uint32_t i = 0;
#if defined(MATH_SIMD_AVX2_FLOAT) && defined(MATH_SIMD_FMA_FLOAT)
	const __m256 b00 = _mm256_set1_ps(p_transform.basis[0][0]);
	const __m256 b01 = _mm256_set1_ps(p_transform.basis[0][1]);
	const __m256 b02 = _mm256_set1_ps(p_transform.basis[0][2]);
	const __m256 b10 = _mm256_set1_ps(p_transform.basis[1][0]);
	const __m256 b11 = _mm256_set1_ps(p_transform.basis[1][1]);
	const __m256 b12 = _mm256_set1_ps(p_transform.basis[1][2]);
	const __m256 b20 = _mm256_set1_ps(p_transform.basis[2][0]);
	const __m256 b21 = _mm256_set1_ps(p_transform.basis[2][1]);
	const __m256 b22 = _mm256_set1_ps(p_transform.basis[2][2]);
	const __m256 ox = _mm256_set1_ps(p_transform.origin.x);
	const __m256 oy = _mm256_set1_ps(p_transform.origin.y);
	const __m256 oz = _mm256_set1_ps(p_transform.origin.z);

	for (; i + 8 <= p_count; i += 8) {
		const __m256 vx = _mm256_setr_ps(p_src[i + 0].x, p_src[i + 1].x, p_src[i + 2].x, p_src[i + 3].x, p_src[i + 4].x, p_src[i + 5].x, p_src[i + 6].x, p_src[i + 7].x);
		const __m256 vy = _mm256_setr_ps(p_src[i + 0].y, p_src[i + 1].y, p_src[i + 2].y, p_src[i + 3].y, p_src[i + 4].y, p_src[i + 5].y, p_src[i + 6].y, p_src[i + 7].y);
		const __m256 vz = _mm256_setr_ps(p_src[i + 0].z, p_src[i + 1].z, p_src[i + 2].z, p_src[i + 3].z, p_src[i + 4].z, p_src[i + 5].z, p_src[i + 6].z, p_src[i + 7].z);

		const __m256 tx = Math::simd_fmadd_ps(b02, vz, Math::simd_fmadd_ps(b01, vy, Math::simd_fmadd_ps(b00, vx, ox)));
		const __m256 ty = Math::simd_fmadd_ps(b12, vz, Math::simd_fmadd_ps(b11, vy, Math::simd_fmadd_ps(b10, vx, oy)));
		const __m256 tz = Math::simd_fmadd_ps(b22, vz, Math::simd_fmadd_ps(b21, vy, Math::simd_fmadd_ps(b20, vx, oz)));

		alignas(32) float out_x[8];
		alignas(32) float out_y[8];
		alignas(32) float out_z[8];
		_mm256_store_ps(out_x, tx);
		_mm256_store_ps(out_y, ty);
		_mm256_store_ps(out_z, tz);

		for (uint32_t j = 0; j < 8; j++) {
			_rtgi_store_packed_float3(r_dst[i + j], out_x[j], out_y[j], out_z[j]);
		}
	}
#endif

	_rtgi_pack_transformed_vertices_float3_scalar(p_src + i, p_count - i, p_transform, r_dst + i);

#if defined(DEV_ENABLED)
	if (!_rtgi_validate_transformed_vertices_float3_pack_sample(p_src, p_count, p_transform, r_dst)) {
		WARN_PRINT_ONCE("RTGI path tracing SIMD vertex packing diverged from scalar Transform3D::xform; restoring scalar-packed vertices for this upload.");
		_rtgi_pack_transformed_vertices_float3_scalar(p_src, p_count, p_transform, r_dst);
	}
#endif
}

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

static String _rtgi_rd_device_family_name(RenderingDevice *p_rd) {
	if (p_rd == nullptr) {
		return "none";
	}

	switch (p_rd->get_device_capabilities().device_family) {
		case RDD::DEVICE_VULKAN:
			return "vulkan";
		case RDD::DEVICE_DIRECTX:
			return "directx";
		case RDD::DEVICE_METAL:
			return "metal";
		case RDD::DEVICE_OPENGL:
			return "opengl";
		case RDD::DEVICE_UNKNOWN:
		default:
			return "unknown";
	}
}

static String _rtgi_rd_device_vendor_name(uint32_t p_vendor) {
	switch (p_vendor) {
		case RenderingContextDriver::Vendor::VENDOR_AMD:
			return "AMD";
		case RenderingContextDriver::Vendor::VENDOR_APPLE:
			return "Apple";
		case RenderingContextDriver::Vendor::VENDOR_ARM:
			return "Arm";
		case RenderingContextDriver::Vendor::VENDOR_IMGTEC:
			return "Imagination";
		case RenderingContextDriver::Vendor::VENDOR_INTEL:
			return "Intel";
		case RenderingContextDriver::Vendor::VENDOR_MICROSOFT:
			return "Microsoft";
		case RenderingContextDriver::Vendor::VENDOR_NVIDIA:
			return "NVIDIA";
		case RenderingContextDriver::Vendor::VENDOR_QUALCOMM:
			return "Qualcomm";
		case RenderingContextDriver::Vendor::VENDOR_UNKNOWN:
		default:
			return "unknown";
	}
}

static uint32_t _rtgi_rd_device_vendor_id(RenderingDevice *p_rd) {
	return p_rd != nullptr ? p_rd->get_device().vendor : RenderingContextDriver::Vendor::VENDOR_UNKNOWN;
}

static String _rtgi_rd_device_name(RenderingDevice *p_rd) {
	return p_rd != nullptr ? p_rd->get_device().name : "none";
}

static bool _rtgi_current_rd_device_family_is_vulkan(RenderingDevice *p_rd) {
	return p_rd != nullptr && p_rd->get_device_capabilities().device_family == RDD::DEVICE_VULKAN;
}

static bool _rtgi_vulkan_interop_self_validate(RenderingDevice *p_rd, String *r_failure_reason);

static String _rtgi_rtxpt_unavailable_reason() {
#ifdef MODULE_RTXPT_ENABLED
	return "RTXPT Vulkan module is compiled, but the RTGI backend implementation was not enabled for this build.";
#else
	return "RTXPT Vulkan module is not compiled in.";
#endif
}

static String _rtgi_hiprt_unavailable_reason() {
#ifdef MODULE_HIPRT_ENABLED
	return "HIP RT module is compiled, but the RTGI backend has no trace kernel dispatch implementation yet.";
#else
	return "HIP RT module is not compiled in.";
#endif
}

static String _rtgi_embree_unavailable_reason() {
#if defined(RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED)
	return "Embree/OSPRay RTGI backend is compiled, but the active runtime, device, or RenderingDevice exchange is unavailable.";
#else
#if defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)
	return "Embree/OSPRay module is compiled, but the RTGI backend has no CPU/SYCL renderer plus Vulkan upload/import or staged-copy path yet.";
#else
	return "Embree/OSPRay RTGI support is not compiled in and no Vulkan upload/import or staged-copy path is available.";
#endif
#endif
}

static bool _rtgi_rtxpt_backend_compiled() {
#ifdef MODULE_RTXPT_ENABLED
	return true;
#else
	return false;
#endif
}

static bool _rtgi_hiprt_backend_compiled() {
#ifdef MODULE_HIPRT_ENABLED
	return true;
#else
	return false;
#endif
}

static bool _rtgi_embree_backend_compiled() {
#if defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_rtxpt_sdk_headers_present() {
#if defined(MODULE_RTXPT_ENABLED) && defined(RTGI_RTXPT_SDK_HEADERS_PRESENT)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_hiprt_sdk_headers_present() {
#if defined(MODULE_HIPRT_ENABLED) && defined(RTGI_HIPRT_SDK_HEADERS_PRESENT)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_embree_sdk_headers_present() {
#if (defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)) && defined(RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_rtxpt_backend_implementation_ready() {
#if defined(MODULE_RTXPT_ENABLED) && defined(RTGI_RTXPT_BACKEND_IMPLEMENTED)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_hiprt_backend_implementation_ready() {
#if defined(MODULE_HIPRT_ENABLED) && defined(RTGI_HIPRT_BACKEND_IMPLEMENTED)
	return true;
#else
	return false;
#endif
}

static bool _rtgi_embree_backend_implementation_ready() {
#if (defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)) && defined(RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED)
	return true;
#else
	return false;
#endif
}

static String _rtgi_availability_failure(bool p_backend_compiled, bool p_runtime_detected, bool p_device_supported, bool p_resource_exchange_supported, bool p_implementation_ready) {
	if (!p_backend_compiled) {
		return "backend_not_compiled";
	}
	if (!p_runtime_detected) {
		return "runtime_not_detected";
	}
	if (!p_device_supported) {
		return "device_not_supported";
	}
	if (!p_resource_exchange_supported) {
		return "resource_exchange_unavailable";
	}
	if (!p_implementation_ready) {
		return "implementation_unavailable";
	}
	return "none";
}

static RTGIBackendCapabilities _rtgi_vulkan_generic_capabilities() {
	RTGIBackendCapabilities caps;
	caps.backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	caps.name = "Vulkan Generic";
	RenderingDevice *rd = RD::get_singleton();
	const bool vulkan_driver = _rtgi_current_rd_device_family_is_vulkan(rd);
	caps.backend_compiled = true;
	caps.runtime_detected = rd != nullptr && vulkan_driver;
	caps.device_supported = vulkan_driver && rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE);
	caps.resource_exchange_supported = vulkan_driver;
	caps.implementation_ready = true;
	caps.available = caps.runtime_detected && caps.device_supported && caps.resource_exchange_supported && caps.implementation_ready;
	caps.runtime_name = "Vulkan ray tracing pipeline";
	caps.integration_path = "RenderingDevice-owned Vulkan resources";
	caps.rendering_device_family = _rtgi_rd_device_family_name(rd);
	caps.rendering_device_name = _rtgi_rd_device_name(rd);
	caps.rendering_device_vendor_id = _rtgi_rd_device_vendor_id(rd);
	caps.rendering_device_vendor = _rtgi_rd_device_vendor_name(caps.rendering_device_vendor_id);
	caps.rendering_device_exchange = vulkan_driver;
	caps.vulkan_runtime = vulkan_driver;
	caps.denoiser_handoff = caps.available;
	caps.sdk_headers_present = true;
	caps.vulkan_interop_mode = caps.available ? "rd_internal" : "none";
	caps.resource_exchange_sync = caps.available ? "rendering_device_graph" : "none";
	caps.native_probe_update = caps.available;
	caps.generic_probe_update_fallback = false;
	caps.denoiser_runtime_detected = caps.available;
	caps.denoiser_available = caps.available;
	caps.denoiser_name = "ASVFG / Internal Signal Decomposition";
	caps.probe_update_path = "Vulkan Generic ray tracing pipeline";
	caps.availability_failure = _rtgi_availability_failure(caps.backend_compiled, caps.runtime_detected, caps.device_supported, caps.resource_exchange_supported, caps.implementation_ready);
	if (!caps.available) {
		if (RD::get_singleton() == nullptr) {
			caps.fallback_reason = "RenderingDevice is unavailable.";
			caps.runtime_failure_reason = caps.fallback_reason;
			caps.device_failure_reason = caps.fallback_reason;
			caps.resource_exchange_failure_reason = "RenderingDevice-owned Vulkan resource exchange is unavailable without an active RenderingDevice.";
			caps.denoiser_failure_reason = caps.fallback_reason;
		} else if (!vulkan_driver) {
			caps.fallback_reason = "The active RenderingDevice driver is not Vulkan; the RTGI backend contract is Vulkan-only in this phase.";
			caps.runtime_failure_reason = caps.fallback_reason;
			caps.device_failure_reason = caps.fallback_reason;
			caps.resource_exchange_failure_reason = "RenderingDevice-owned Vulkan resource exchange requires the active RenderingDevice driver to be Vulkan.";
			caps.denoiser_failure_reason = caps.fallback_reason;
		} else {
			caps.fallback_reason = "Vulkan ray tracing pipeline support is unavailable on this device.";
			caps.device_failure_reason = caps.fallback_reason;
			caps.denoiser_failure_reason = caps.fallback_reason;
		}
	}
	return caps;
}

static void _rtgi_append_capability_reason(String &r_reason, const String &p_reason) {
	if (p_reason.is_empty()) {
		return;
	}
	if (r_reason.is_empty()) {
		r_reason = p_reason;
	} else if (r_reason.find(p_reason) == -1) {
		r_reason += " " + p_reason;
	}
}

static String _rtgi_runtime_library_probe_cache_key(const String &p_path, const char *const *p_required_symbols, int p_required_symbol_count) {
	String key = p_path;
	for (int i = 0; i < p_required_symbol_count; i++) {
		key += "|" + String(p_required_symbols[i]);
	}
	return key;
}

static bool _rtgi_try_open_runtime_library(const String &p_path, const char *const *p_required_symbols = nullptr, int p_required_symbol_count = 0) {
	if (p_path.is_empty()) {
		return false;
	}

	static HashMap<String, bool> library_probe_cache;
	const String cache_key = _rtgi_runtime_library_probe_cache_key(p_path, p_required_symbols, p_required_symbol_count);
	if (library_probe_cache.has(cache_key)) {
		return library_probe_cache[cache_key];
	}

#if defined(LINUXBSD_ENABLED)
	void *linux_library_handle = dlopen(p_path.utf8().get_data(), RTLD_LAZY | RTLD_LOCAL);
	if (linux_library_handle != nullptr) {
		bool required_symbols_found = true;
		for (int i = 0; i < p_required_symbol_count; i++) {
			if (dlsym(linux_library_handle, p_required_symbols[i]) == nullptr) {
				required_symbols_found = false;
				break;
			}
		}
		dlclose(linux_library_handle);
		library_probe_cache[cache_key] = required_symbols_found;
		return required_symbols_found;
	}
#endif

#if defined(WINDOWS_ENABLED)
	if (!FileAccess::exists(p_path)) {
		library_probe_cache[cache_key] = false;
		return false;
	}

	const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
	const DWORD load_flags = p_path.is_absolute_path() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
	HMODULE windows_library_handle = LoadLibraryExW((LPCWSTR)(p_path.utf16().get_data()), nullptr, load_flags);
	SetErrorMode(previous_error_mode);
	if (windows_library_handle == nullptr) {
		library_probe_cache[cache_key] = false;
		return false;
	}
	bool required_symbols_found = true;
	for (int i = 0; i < p_required_symbol_count; i++) {
		if (GetProcAddress(windows_library_handle, p_required_symbols[i]) == nullptr) {
			required_symbols_found = false;
			break;
		}
	}
	FreeLibrary(windows_library_handle);
	library_probe_cache[cache_key] = required_symbols_found;
	return required_symbols_found;
#endif

	library_probe_cache[cache_key] = false;
	return false;
}

static void _rtgi_append_unique_library_search_dir(Vector<String> &r_dirs, const String &p_dir) {
	if (p_dir.is_empty()) {
		return;
	}
	for (const String &dir : r_dirs) {
		if (dir == p_dir) {
			return;
		}
	}
	r_dirs.push_back(p_dir);
}

static Vector<String> _rtgi_make_runtime_library_search_dirs(const char *const *p_root_env_vars, int p_root_env_var_count) {
	Vector<String> search_dirs;
	OS *os = OS::get_singleton();
	if (os == nullptr) {
		return search_dirs;
	}

	const String executable_dir = os->get_executable_path().get_base_dir();
	_rtgi_append_unique_library_search_dir(search_dirs, executable_dir);

	for (int i = 0; i < p_root_env_var_count; i++) {
		const String root = os->get_environment(p_root_env_vars[i]);
		if (root.is_empty()) {
			continue;
		}
		_rtgi_append_unique_library_search_dir(search_dirs, root);
		_rtgi_append_unique_library_search_dir(search_dirs, root.path_join("bin"));
		_rtgi_append_unique_library_search_dir(search_dirs, root.path_join("lib"));
		_rtgi_append_unique_library_search_dir(search_dirs, root.path_join("lib64"));
	}

#if defined(WINDOWS_ENABLED)
	const char *path_separator = ";";
#else
	const char *path_separator = ":";
#endif
	const Vector<String> path_dirs = os->get_environment("PATH").split(path_separator, false);
	for (const String &path_dir : path_dirs) {
		_rtgi_append_unique_library_search_dir(search_dirs, path_dir);
	}

	return search_dirs;
}

static bool _rtgi_find_runtime_library(const char *const *p_library_names, int p_library_name_count, const char *const *p_required_symbols, int p_required_symbol_count, const char *const *p_root_env_vars, int p_root_env_var_count, String *r_found_path) {
	for (int i = 0; i < p_library_name_count; i++) {
		const String library_name = p_library_names[i];
#if defined(LINUXBSD_ENABLED)
		if (_rtgi_try_open_runtime_library(library_name, p_required_symbols, p_required_symbol_count)) {
			if (r_found_path != nullptr) {
				*r_found_path = library_name;
			}
			return true;
		}
#endif
		if (FileAccess::exists(library_name) && _rtgi_try_open_runtime_library(library_name, p_required_symbols, p_required_symbol_count)) {
			if (r_found_path != nullptr) {
				*r_found_path = library_name;
			}
			return true;
		}
	}

	const Vector<String> search_dirs = _rtgi_make_runtime_library_search_dirs(p_root_env_vars, p_root_env_var_count);
	for (const String &dir : search_dirs) {
		for (int i = 0; i < p_library_name_count; i++) {
			const String candidate = dir.path_join(p_library_names[i]);
			if (_rtgi_try_open_runtime_library(candidate, p_required_symbols, p_required_symbol_count)) {
				if (r_found_path != nullptr) {
					*r_found_path = candidate;
				}
				return true;
			}
		}
	}

	return false;
}

struct RTGIVendorBackendRequirements {
	RSE::PathtracingBackend backend = RSE::PT_BACKEND_VULKAN_GENERIC;
	String name;
	String runtime_name;
	String integration_path;
	uint32_t required_gpu_vendor = RenderingContextDriver::Vendor::VENDOR_UNKNOWN;
	bool requires_raytracing_pipeline = false;
	bool allows_rd_internal_exchange = false;
	bool requires_external_memory_semaphore = false;
	bool allows_staged_copy = false;
};

struct RTGIVendorBackendProbe {
	bool backend_compiled = false;
	bool runtime_detected = false;
	bool device_supported = false;
	bool resource_exchange_supported = false;
	bool implementation_ready = false;
	bool sdk_headers_present = false;
	bool external_memory = false;
	bool external_semaphore = false;
	bool timeline_semaphore = false;
	bool staged_copy = false;
	String compile_failure_reason;
	String runtime_failure_reason;
	String device_failure_reason;
	String resource_exchange_failure_reason;
	String implementation_failure_reason;
};

static void _rtgi_probe_rtxpt_runtime(RTGIVendorBackendProbe &r_probe) {
	r_probe.backend_compiled = _rtgi_rtxpt_backend_compiled();
	r_probe.sdk_headers_present = _rtgi_rtxpt_sdk_headers_present();
#ifdef MODULE_RTXPT_ENABLED
	// NVIDIA's Godot reference branch records RTXPT-style work through Godot's
	// Vulkan ray tracing pipeline, not through a standalone RTXPT runtime DLL.
	r_probe.runtime_detected = true;
	r_probe.implementation_ready = _rtgi_rtxpt_backend_implementation_ready();
	if (!r_probe.implementation_ready) {
		r_probe.implementation_failure_reason = "RTXPT module is compiled, but the NVIDIA fork-compatible RenderingDevice/Vulkan dispatch adapter was not compiled into this build.";
	}
#else
	r_probe.compile_failure_reason = _rtgi_rtxpt_unavailable_reason();
#endif
}

static void _rtgi_probe_hiprt_runtime(RTGIVendorBackendProbe &r_probe) {
	r_probe.backend_compiled = _rtgi_hiprt_backend_compiled();
	r_probe.sdk_headers_present = _rtgi_hiprt_sdk_headers_present();
#ifdef MODULE_HIPRT_ENABLED
	static const char *const hip_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
		"amdhip64_7.dll",
		"amdhip64_6.dll",
		"amdhip64.dll",
#else
		"libamdhip64.so",
		"libamdhip64.so.7",
		"libamdhip64.so.6",
		"libamdhip64.so.5",
#endif
	};
	static const char *const hip_runtime_symbols[] = {
		"hipInit",
		"hipDeviceSynchronize",
		"hipMalloc",
		"hipMemcpy",
		"hipMemcpy2DToArray",
		"hipGetMipmappedArrayLevel",
		"hipModuleLaunchKernel",
		"hipFree",
		"hipImportExternalMemory",
		"hipExternalMemoryGetMappedMipmappedArray",
		"hipFreeMipmappedArray",
		"hipDestroyExternalMemory",
		"hipImportExternalSemaphore",
		"hipWaitExternalSemaphoresAsync",
		"hipSignalExternalSemaphoresAsync",
		"hipDestroyExternalSemaphore",
	};
	static const char *const hiprt_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
		"hiprt0300064.dll",
		"hiprt0200564.dll",
		"hiprt0200064.dll",
		"hiprt64.dll",
		"hiprt.dll",
#else
		"libhiprt64.so",
		"libhiprt64.so.3",
		"libhiprt64.so.2.5",
		"libhiprt64.so.2",
		"libhiprt.so",
#endif
	};
	static const char *const hiprt_runtime_symbols[] = {
		"hiprtCreateContext",
		"hiprtDestroyContext",
		"hiprtCreateGeometry",
		"hiprtDestroyGeometry",
		"hiprtGetGeometryBuildTemporaryBufferSize",
		"hiprtBuildGeometry",
		"hiprtCreateScene",
		"hiprtDestroyScene",
		"hiprtGetSceneBuildTemporaryBufferSize",
		"hiprtBuildScene",
		"hiprtBuildTraceKernels",
	};
	static const char *const hiprt_root_env_vars[] = {
		"HIPRT_PATH",
		"HIP_PATH",
		"ROCM_PATH",
	};
	String found_hip_library;
	String found_hiprt_library;
	const bool hip_runtime_detected = _rtgi_find_runtime_library(hip_runtime_libraries, sizeof(hip_runtime_libraries) / sizeof(hip_runtime_libraries[0]), hip_runtime_symbols, sizeof(hip_runtime_symbols) / sizeof(hip_runtime_symbols[0]), hiprt_root_env_vars, sizeof(hiprt_root_env_vars) / sizeof(hiprt_root_env_vars[0]), &found_hip_library);
	const bool hiprt_runtime_detected = _rtgi_find_runtime_library(hiprt_runtime_libraries, sizeof(hiprt_runtime_libraries) / sizeof(hiprt_runtime_libraries[0]), hiprt_runtime_symbols, sizeof(hiprt_runtime_symbols) / sizeof(hiprt_runtime_symbols[0]), hiprt_root_env_vars, sizeof(hiprt_root_env_vars) / sizeof(hiprt_root_env_vars[0]), &found_hiprt_library);
	r_probe.runtime_detected = hip_runtime_detected && hiprt_runtime_detected;
	if (!r_probe.runtime_detected) {
		if (!hip_runtime_detected && !hiprt_runtime_detected) {
			r_probe.runtime_failure_reason = "HIP and HIP RT runtime libraries were not found in HIPRT_PATH, HIP_PATH, ROCM_PATH, PATH, or the executable directory.";
		} else if (!hip_runtime_detected) {
			r_probe.runtime_failure_reason = "HIP runtime library was not found in HIPRT_PATH, HIP_PATH, ROCM_PATH, PATH, or the executable directory.";
		} else {
			r_probe.runtime_failure_reason = "HIP RT runtime library was not found in HIPRT_PATH, HIP_PATH, ROCM_PATH, PATH, or the executable directory.";
		}
	}
	r_probe.implementation_ready = _rtgi_hiprt_backend_implementation_ready();
	if (!r_probe.implementation_ready) {
#if defined(RTGI_HIPRT_CONTEXT_DISPATCH_ENABLED)
		r_probe.implementation_failure_reason = "HIP RT context creation, scene acceleration-structure build, Vulkan/HIP output image import, and trace-kernel dispatch are wired, but the build was not configured with a generated HIPRT_API_VERSION header.";
#else
		r_probe.implementation_failure_reason = "HIP RT SDK detection is wired, but the HIP RT scene mirror, Vulkan/HIP interop, and dispatch entrypoints were not compiled into this build.";
#endif
	}
#else
	r_probe.compile_failure_reason = _rtgi_hiprt_unavailable_reason();
#endif
}

static void _rtgi_probe_embree_runtime(RTGIVendorBackendProbe &r_probe) {
	r_probe.backend_compiled = _rtgi_embree_backend_compiled();
	r_probe.sdk_headers_present = _rtgi_embree_sdk_headers_present();
#if defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)
#if defined(RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED)
	r_probe.runtime_detected = true;
#else
	static const char *const embree_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
		"embree4.dll",
		"embree3.dll",
#else
		"libembree4.so",
		"libembree3.so",
#endif
	};
	static const char *const embree_runtime_symbols[] = {
		"rtcNewDevice",
	};
	static const char *const ospray_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
		"ospray.dll",
		"ospray_module_cpu.dll",
#else
		"libospray.so",
		"libospray_module_cpu.so",
#endif
	};
	static const char *const ospray_runtime_symbols[] = {
		"ospInit",
	};
	static const char *const embree_root_env_vars[] = {
		"EMBREE_ROOT",
		"EMBREE_DIR",
		"OSPRAY_ROOT",
		"OSPRAY_DIR",
		"ONEAPI_ROOT",
	};
	String found_embree_library;
	String found_ospray_library;
	const bool embree_runtime_detected = _rtgi_find_runtime_library(embree_runtime_libraries, sizeof(embree_runtime_libraries) / sizeof(embree_runtime_libraries[0]), embree_runtime_symbols, sizeof(embree_runtime_symbols) / sizeof(embree_runtime_symbols[0]), embree_root_env_vars, sizeof(embree_root_env_vars) / sizeof(embree_root_env_vars[0]), &found_embree_library);
	const bool ospray_runtime_detected = _rtgi_find_runtime_library(ospray_runtime_libraries, sizeof(ospray_runtime_libraries) / sizeof(ospray_runtime_libraries[0]), ospray_runtime_symbols, sizeof(ospray_runtime_symbols) / sizeof(ospray_runtime_symbols[0]), embree_root_env_vars, sizeof(embree_root_env_vars) / sizeof(embree_root_env_vars[0]), &found_ospray_library);
	r_probe.runtime_detected = embree_runtime_detected || ospray_runtime_detected;
	if (!r_probe.runtime_detected) {
		r_probe.runtime_failure_reason = "Embree or OSPRay runtime library was not found in EMBREE_ROOT, EMBREE_DIR, OSPRAY_ROOT, OSPRAY_DIR, ONEAPI_ROOT, PATH, or the executable directory.";
	}
#endif
	r_probe.implementation_ready = _rtgi_embree_backend_implementation_ready();
	if (!r_probe.implementation_ready) {
		r_probe.implementation_failure_reason = "Embree/OSPRay SDK detection is wired, but the CPU/SYCL scene conversion, renderer, and Vulkan upload/staged-copy entrypoints were not compiled into this build.";
	}
#else
	r_probe.compile_failure_reason = _rtgi_embree_unavailable_reason();
#endif
}

static void _rtgi_probe_vendor_device_exchange(const RTGIVendorBackendRequirements &p_requirements, RenderingDevice *p_rd, RTGIVendorBackendProbe &r_probe) {
	const bool vulkan_driver = _rtgi_current_rd_device_family_is_vulkan(p_rd);
	const RDD::Capabilities *rd_capabilities = p_rd != nullptr ? &p_rd->get_device_capabilities() : nullptr;
	const uint32_t device_vendor = _rtgi_rd_device_vendor_id(p_rd);

	r_probe.device_supported = vulkan_driver;
	if (p_requirements.required_gpu_vendor != RenderingContextDriver::Vendor::VENDOR_UNKNOWN) {
		r_probe.device_supported = r_probe.device_supported && device_vendor == p_requirements.required_gpu_vendor;
	}
	if (p_requirements.requires_raytracing_pipeline) {
		r_probe.device_supported = r_probe.device_supported && p_rd != nullptr && p_rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE);
	}

	if (p_rd == nullptr) {
		_rtgi_append_capability_reason(r_probe.device_failure_reason, "RenderingDevice is unavailable.");
	} else if (!vulkan_driver) {
		_rtgi_append_capability_reason(r_probe.device_failure_reason, "The active RenderingDevice driver is not Vulkan; the RTGI backend contract is Vulkan-only in this phase.");
	}
	if (p_requirements.required_gpu_vendor != RenderingContextDriver::Vendor::VENDOR_UNKNOWN && device_vendor != p_requirements.required_gpu_vendor) {
		_rtgi_append_capability_reason(r_probe.device_failure_reason, vformat("%s requires a %s GPU; active device is %s (%s).", p_requirements.name, _rtgi_rd_device_vendor_name(p_requirements.required_gpu_vendor), _rtgi_rd_device_vendor_name(device_vendor), _rtgi_rd_device_name(p_rd)));
	}
	if (p_requirements.requires_raytracing_pipeline && p_rd != nullptr && !p_rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE)) {
		_rtgi_append_capability_reason(r_probe.device_failure_reason, vformat("%s requires Vulkan ray tracing pipeline support on the active device.", p_requirements.name));
	}

	if (vulkan_driver && rd_capabilities != nullptr) {
		r_probe.external_memory = rd_capabilities->external_memory_supported;
		r_probe.external_semaphore = rd_capabilities->external_semaphore_supported;
		r_probe.timeline_semaphore = rd_capabilities->timeline_semaphore_supported;
		r_probe.staged_copy = p_requirements.allows_staged_copy;
	}

	String bridge_validation_failure;
	const bool bridge_self_validated = r_probe.external_memory && r_probe.external_semaphore && _rtgi_vulkan_interop_self_validate(p_rd, &bridge_validation_failure);
	r_probe.external_memory = r_probe.external_memory && bridge_self_validated;
	r_probe.external_semaphore = r_probe.external_semaphore && bridge_self_validated;

	const bool external_memory_semaphore_exchange = bridge_self_validated;
	const bool rd_internal_exchange = p_requirements.allows_rd_internal_exchange && vulkan_driver;
	if (p_requirements.requires_external_memory_semaphore) {
		r_probe.resource_exchange_supported = external_memory_semaphore_exchange;
	} else {
		r_probe.resource_exchange_supported = rd_internal_exchange || external_memory_semaphore_exchange || r_probe.timeline_semaphore || r_probe.staged_copy;
	}

	if (p_requirements.requires_external_memory_semaphore && !external_memory_semaphore_exchange) {
		_rtgi_append_capability_reason(r_probe.resource_exchange_failure_reason, bridge_validation_failure.is_empty() ? "The active RenderingDevice does not expose a complete Vulkan external memory plus external semaphore exchange path." : bridge_validation_failure);
	} else if (!p_requirements.requires_external_memory_semaphore && !r_probe.resource_exchange_supported) {
		_rtgi_append_capability_reason(r_probe.resource_exchange_failure_reason, bridge_validation_failure.is_empty() ? "The active RenderingDevice does not expose a supported internal, external memory/semaphore, timeline semaphore, or staged-copy exchange for this backend." : bridge_validation_failure);
	}
}

static RTGIVendorBackendProbe _rtgi_probe_vendor_backend(const RTGIVendorBackendRequirements &p_requirements) {
	RTGIVendorBackendProbe probe;
	switch (p_requirements.backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT:
			_rtgi_probe_rtxpt_runtime(probe);
			break;
		case RSE::PT_BACKEND_AMD_HIP_RT:
			_rtgi_probe_hiprt_runtime(probe);
			break;
		case RSE::PT_BACKEND_INTEL_EMBREE:
			_rtgi_probe_embree_runtime(probe);
			break;
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			break;
	}
	_rtgi_probe_vendor_device_exchange(p_requirements, RD::get_singleton(), probe);
	return probe;
}

static bool _rtgi_probe_vendor_denoiser_runtime(RSE::PathtracingBackend p_backend, String *r_denoiser_name, String *r_failure_reason) {
	if (r_denoiser_name != nullptr) {
		switch (p_backend) {
			case RSE::PT_BACKEND_NVIDIA_RTXPT:
				*r_denoiser_name = "NVIDIA NRD";
				break;
			case RSE::PT_BACKEND_AMD_HIP_RT:
				*r_denoiser_name = "AMD HIP RT Denoiser";
				break;
			case RSE::PT_BACKEND_INTEL_EMBREE:
				*r_denoiser_name = "Intel Embree Denoiser";
				break;
			case RSE::PT_BACKEND_VULKAN_GENERIC:
			default:
				*r_denoiser_name = "ASVFG / Internal Signal Decomposition";
				break;
		}
	}

	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT: {
#ifdef MODULE_RTXPT_ENABLED
			static const char *const nrd_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
				"NRD.dll",
				"nrd.dll",
#else
				"libNRD.so",
				"libnrd.so",
#endif
			};
			static const char *const nrd_root_env_vars[] = {
				"NRD_SDK_PATH",
				"NRD_PATH",
				"RTXPT_SDK_PATH",
			};
			String found_library;
			const bool detected = _rtgi_find_runtime_library(nrd_runtime_libraries, sizeof(nrd_runtime_libraries) / sizeof(nrd_runtime_libraries[0]), nullptr, 0, nrd_root_env_vars, sizeof(nrd_root_env_vars) / sizeof(nrd_root_env_vars[0]), &found_library);
			if (!detected && r_failure_reason != nullptr) {
				*r_failure_reason = "NVIDIA denoiser was requested, but the NRD runtime was not found in NRD_SDK_PATH, NRD_PATH, RTXPT_SDK_PATH, PATH, or the executable directory.";
			}
			return detected;
#else
			if (r_failure_reason != nullptr) {
				*r_failure_reason = "NVIDIA denoiser support is not compiled in because the RTXPT module is disabled.";
			}
			return false;
#endif
		}
		case RSE::PT_BACKEND_AMD_HIP_RT: {
#ifdef MODULE_HIPRT_ENABLED
			return true;
#else
			if (r_failure_reason != nullptr) {
				*r_failure_reason = "AMD HIP RT denoiser support is not compiled in because the HIP RT module is disabled.";
			}
			return false;
#endif
		}
		case RSE::PT_BACKEND_INTEL_EMBREE: {
#if defined(MODULE_EMBREE_ENABLED) || defined(MODULE_OSPRAY_ENABLED) || defined(MODULE_RAYCAST_ENABLED)
			return true;
#else
			if (r_failure_reason != nullptr) {
				*r_failure_reason = "Intel Embree denoiser support is not compiled in because the Embree module is disabled.";
			}
			return false;
#endif
		}
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return true;
	}
}

static bool _rtgi_vendor_denoiser_handoff_ready(RSE::PathtracingBackend p_backend) {
	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT:
#if defined(MODULE_RTXPT_ENABLED) && defined(RTGI_NRD_DENOISER_HANDOFF_ENABLED)
			return true;
#else
			return false;
#endif
		case RSE::PT_BACKEND_AMD_HIP_RT:
			return false;
		case RSE::PT_BACKEND_INTEL_EMBREE:
			return false;
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return true;
	}
}

static const char *_rtgi_backend_denoiser_fallback_name(RSE::PathtracingBackend p_backend) {
	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT:
			return "ASVFG";
		case RSE::PT_BACKEND_AMD_HIP_RT:
		case RSE::PT_BACKEND_INTEL_EMBREE:
			return "Internal Signal Decomposition";
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return "";
	}
}

static RTGIBackendCapabilities _rtgi_vendor_backend_capabilities(const RTGIVendorBackendRequirements &p_requirements) {
	RTGIBackendCapabilities caps;
	RenderingDevice *rd = RD::get_singleton();
	const bool vulkan_driver = _rtgi_current_rd_device_family_is_vulkan(rd);
	const uint32_t device_vendor = _rtgi_rd_device_vendor_id(rd);
	const RTGIVendorBackendProbe probe = _rtgi_probe_vendor_backend(p_requirements);

	caps.backend = p_requirements.backend;
	caps.name = p_requirements.name;
	caps.available = probe.backend_compiled && probe.runtime_detected && probe.device_supported && probe.resource_exchange_supported && probe.implementation_ready;
	caps.runtime_name = p_requirements.runtime_name;
	caps.integration_path = p_requirements.integration_path;

	caps.rendering_device_family = _rtgi_rd_device_family_name(rd);
	caps.rendering_device_name = _rtgi_rd_device_name(rd);
	caps.rendering_device_vendor_id = device_vendor;
	caps.rendering_device_vendor = _rtgi_rd_device_vendor_name(device_vendor);
	caps.rendering_device_exchange = vulkan_driver;
	caps.vulkan_runtime = vulkan_driver;
	caps.backend_compiled = probe.backend_compiled;
	caps.runtime_detected = probe.runtime_detected;
	caps.device_supported = probe.device_supported;
	caps.resource_exchange_supported = probe.resource_exchange_supported;
	caps.implementation_ready = probe.implementation_ready;
	caps.sdk_headers_present = probe.sdk_headers_present;
	caps.external_memory = probe.external_memory;
	caps.external_semaphore = probe.external_semaphore;
	caps.timeline_semaphore = probe.timeline_semaphore;
	caps.staged_copy = probe.staged_copy;
	if (caps.external_memory && caps.external_semaphore) {
		caps.vulkan_interop_mode = "external_memory_semaphore";
		caps.resource_exchange_sync = p_requirements.backend == RSE::PT_BACKEND_AMD_HIP_RT ? "rd_flush_external_binary_semaphore_wait_signal" : "external_binary_semaphore";
	} else if (caps.timeline_semaphore) {
		caps.vulkan_interop_mode = "timeline_semaphore";
		caps.resource_exchange_sync = "timeline_semaphore";
	} else if (caps.staged_copy) {
		caps.vulkan_interop_mode = "staged_copy";
		caps.resource_exchange_sync = "rendering_device_staged_upload";
	} else if (caps.rendering_device_exchange && p_requirements.allows_rd_internal_exchange) {
		caps.vulkan_interop_mode = "rd_internal";
		caps.resource_exchange_sync = "rendering_device_graph";
	} else {
		caps.vulkan_interop_mode = "none";
		caps.resource_exchange_sync = "none";
	}
	caps.native_probe_update = probe.implementation_ready;
	caps.generic_probe_update_fallback = !probe.implementation_ready;
	caps.probe_update_path = probe.implementation_ready ? p_requirements.name + " native probe tracing" : "Vulkan Generic STRC probe fallback";
	if (p_requirements.backend == RSE::PT_BACKEND_AMD_HIP_RT || p_requirements.backend == RSE::PT_BACKEND_INTEL_EMBREE) {
		caps.native_probe_update = false;
		caps.generic_probe_update_fallback = probe.implementation_ready;
		caps.probe_update_path = "Vulkan Generic STRC probe fallback";
	}
	const bool denoiser_handoff_ready = _rtgi_vendor_denoiser_handoff_ready(p_requirements.backend);
	caps.denoiser_runtime_detected = _rtgi_probe_vendor_denoiser_runtime(p_requirements.backend, &caps.denoiser_name, &caps.denoiser_failure_reason);
	caps.denoiser_available = probe.implementation_ready && caps.denoiser_runtime_detected && denoiser_handoff_ready;
	caps.denoiser_handoff = caps.denoiser_available;
	if (!caps.denoiser_available && caps.denoiser_failure_reason.is_empty()) {
		caps.denoiser_failure_reason = caps.denoiser_runtime_detected ? "Vendor denoiser runtime was detected, but the matching RTGI denoiser handoff implementation was not compiled into this build." : "Vendor denoiser runtime was not detected.";
	}
	if (!caps.denoiser_available) {
		const char *fallback_name = _rtgi_backend_denoiser_fallback_name(p_requirements.backend);
		if (fallback_name[0] != '\0') {
			caps.denoiser_name = fallback_name;
			if (!caps.denoiser_failure_reason.is_empty()) {
				caps.denoiser_failure_reason += " ";
			}
			caps.denoiser_failure_reason += vformat("Falling back to %s.", fallback_name);
		}
	}
	caps.compile_failure_reason = probe.compile_failure_reason;
	caps.runtime_failure_reason = probe.runtime_failure_reason;
	caps.device_failure_reason = probe.device_failure_reason;
	caps.resource_exchange_failure_reason = probe.resource_exchange_failure_reason;
	caps.implementation_failure_reason = probe.implementation_failure_reason;
	caps.availability_failure = _rtgi_availability_failure(caps.backend_compiled, caps.runtime_detected, caps.device_supported, caps.resource_exchange_supported, caps.implementation_ready);

	String reason;
	_rtgi_append_capability_reason(reason, caps.compile_failure_reason);
	_rtgi_append_capability_reason(reason, caps.device_failure_reason);
	_rtgi_append_capability_reason(reason, caps.resource_exchange_failure_reason);
	_rtgi_append_capability_reason(reason, caps.runtime_failure_reason);
	_rtgi_append_capability_reason(reason, caps.implementation_failure_reason);
	caps.fallback_reason = reason;
	if (caps.fallback_reason.is_empty()) {
		caps.fallback_reason = caps.available ? String() : "RTGI backend is unavailable.";
	}
	return caps;
}

static RTGIBackendCapabilities _rtgi_static_backend_capabilities(RSE::PathtracingBackend p_backend) {
	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT: {
			RTGIVendorBackendRequirements requirements;
			requirements.backend = RSE::PT_BACKEND_NVIDIA_RTXPT;
			requirements.name = "NVIDIA RTXPT";
			requirements.runtime_name = "RenderingDevice Vulkan ray tracing pipeline";
			requirements.integration_path = "NVIDIA Godot fork-compatible RenderingDevice/Vulkan dispatch";
			requirements.required_gpu_vendor = RenderingContextDriver::Vendor::VENDOR_NVIDIA;
			requirements.requires_raytracing_pipeline = true;
			requirements.allows_rd_internal_exchange = true;
			return _rtgi_vendor_backend_capabilities(requirements);
		}
		case RSE::PT_BACKEND_AMD_HIP_RT: {
			RTGIVendorBackendRequirements requirements;
			requirements.backend = RSE::PT_BACKEND_AMD_HIP_RT;
			requirements.name = "AMD HIP RT";
			requirements.runtime_name = "HIP RT compute runtime";
			requirements.integration_path = "Vulkan/HIP external memory and semaphore interop";
			requirements.required_gpu_vendor = RenderingContextDriver::Vendor::VENDOR_AMD;
			requirements.requires_external_memory_semaphore = true;
			return _rtgi_vendor_backend_capabilities(requirements);
		}
		case RSE::PT_BACKEND_INTEL_EMBREE: {
			RTGIVendorBackendRequirements requirements;
			requirements.backend = RSE::PT_BACKEND_INTEL_EMBREE;
			requirements.name = "Intel Embree/OSPRay";
			requirements.runtime_name = "CPU/SYCL";
			requirements.integration_path = "Vulkan upload/import or deliberate staged copy";
			requirements.allows_staged_copy = true;
			return _rtgi_vendor_backend_capabilities(requirements);
		}
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return _rtgi_vulkan_generic_capabilities();
	}
}

static bool _rtgi_backend_is_enabled(RSE::PathtracingBackend p_backend) {
	return p_backend == RSE::PT_BACKEND_VULKAN_GENERIC;
}

static RTGIBackendCapabilities _rtgi_enabled_backend_capabilities(RSE::PathtracingBackend p_backend) {
	if (_rtgi_backend_is_enabled(p_backend)) {
		return _rtgi_static_backend_capabilities(p_backend);
	}

	const String disabled_reason = "Only the Vulkan Generic RTGI backend is enabled in this build.";
	RTGIBackendCapabilities caps;
	RenderingDevice *rd = RD::get_singleton();
	caps.backend = p_backend;
	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT:
			caps.name = "NVIDIA RTXPT";
			caps.runtime_name = "RenderingDevice Vulkan ray tracing pipeline";
			caps.integration_path = "NVIDIA Godot fork-compatible RenderingDevice/Vulkan dispatch";
			caps.denoiser_name = "ASVFG";
			break;
		case RSE::PT_BACKEND_AMD_HIP_RT:
			caps.name = "AMD HIP RT";
			caps.runtime_name = "HIP RT compute runtime";
			caps.integration_path = "Vulkan/HIP external memory and semaphore interop";
			caps.denoiser_name = "Internal Signal Decomposition";
			break;
		case RSE::PT_BACKEND_INTEL_EMBREE:
		default:
			caps.name = "Intel Embree/OSPRay";
			caps.runtime_name = "CPU/SYCL";
			caps.integration_path = "Vulkan upload/import or deliberate staged copy";
			caps.denoiser_name = "Internal Signal Decomposition";
			break;
	}
	caps.available = false;
	caps.rendering_device_family = _rtgi_rd_device_family_name(rd);
	caps.rendering_device_name = _rtgi_rd_device_name(rd);
	caps.rendering_device_vendor_id = _rtgi_rd_device_vendor_id(rd);
	caps.rendering_device_vendor = _rtgi_rd_device_vendor_name(caps.rendering_device_vendor_id);
	caps.backend_compiled = false;
	caps.runtime_detected = false;
	caps.device_supported = false;
	caps.resource_exchange_supported = false;
	caps.implementation_ready = false;
	caps.sdk_headers_present = false;
	caps.vulkan_interop_mode = "none";
	caps.resource_exchange_sync = "none";
	caps.native_probe_update = false;
	caps.generic_probe_update_fallback = true;
	caps.probe_update_path = "Vulkan Generic STRC probe fallback";
	caps.denoiser_runtime_detected = false;
	caps.denoiser_available = false;
	caps.denoiser_handoff = false;
	caps.denoiser_failure_reason = disabled_reason;
	caps.compile_failure_reason = disabled_reason;
	caps.availability_failure = _rtgi_availability_failure(caps.backend_compiled, caps.runtime_detected, caps.device_supported, caps.resource_exchange_supported, caps.implementation_ready);
	caps.fallback_reason = disabled_reason;
	return caps;
}

static uint64_t _rt_history_mix(uint64_t p_hash, uint64_t p_value) {
	return p_hash ^ (p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2));
}

static float _rt_halton_value(uint32_t p_index, uint32_t p_base) {
	float f = 1.0f;
	float r = 0.0f;
	while (p_index > 0) {
		f /= (float)p_base;
		r += f * (float)(p_index % p_base);
		p_index /= p_base;
	}
	return r * 2.0f - 1.0f;
}

static uint64_t _rt_history_mix_rid(uint64_t p_hash, RID p_rid) {
	return _rt_history_mix(p_hash, p_rid.is_valid() ? p_rid.get_id() : 0);
}

static uint64_t _rt_signature_mix_rid(uint64_t p_hash, RID p_rid) {
	return _rt_history_mix_rid(p_hash, p_rid);
}

static uint32_t _rt_light_source_id(RID p_light_instance, RSE::LightType p_type) {
	uint64_t h = 0x9e3779b97f4a7c15ULL;
	h = _rt_history_mix_rid(h, p_light_instance);
	h = _rt_history_mix(h, uint64_t(p_type));
	uint32_t id = uint32_t((h ^ (h >> 32)) & 0x0FFFFFFFULL);
	return id != 0 ? id : 1u;
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

static uint64_t _rt_radiance_signature(uint32_t p_rt_flags, RID p_environment, RID p_camera_attributes, const float p_rt_params[SceneShaderRaytracing::RT_PARAM_SHADER_FLOAT_COUNT], const Color &p_background_color, bool p_background_uses_sky, const RT_LightData *p_light_data, uint32_t p_light_count) {
	const uint32_t signature_rt_flags = p_rt_flags;
	uint64_t signature = _rt_history_mix(0x727472616469616eULL, signature_rt_flags);
	signature = _rt_history_mix_rid(signature, p_environment);
	signature = _rt_history_mix_rid(signature, p_camera_attributes);
	for (uint32_t i = 0; i < SceneShaderRaytracing::RT_PARAM_SHADER_FLOAT_COUNT; i++) {
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

struct RTGIVulkanInteropDeviceContext {
	bool valid = false;
	uint64_t instance = 0;
	uint64_t physical_device = 0;
	uint64_t logical_device = 0;
	uint64_t queue = 0;
	uint32_t queue_family_index = 0;
	String failure_reason;
};

static RTGIBackendExternalHandleType _rtgi_backend_external_handle_type_from_rdd(RDD::ExternalHandleType p_handle_type) {
	switch (p_handle_type) {
		case RDD::EXTERNAL_HANDLE_OPAQUE_FD:
			return RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD;
		case RDD::EXTERNAL_HANDLE_OPAQUE_WIN32:
			return RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32;
		case RDD::EXTERNAL_HANDLE_NONE:
		default:
			return RTGI_BACKEND_EXTERNAL_HANDLE_NONE;
	}
}

static void _rtgi_close_external_handle(uint64_t p_handle, RTGIBackendExternalHandleType p_handle_type) {
	if (p_handle == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32) {
		CloseHandle((HANDLE)(uintptr_t)p_handle);
	}
#else
	if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD) {
		close((int)p_handle);
	}
#endif
}

static void _rtgi_vulkan_interop_noop_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
}

struct RTGIVulkanInteropTextureContext {
	bool valid = false;
	uint64_t image = 0;
	uint64_t image_view = 0;
	uint64_t format = 0;
	uint64_t device_memory = 0;
	uint64_t usage_flags = 0;
	String failure_reason;
};

class RTGIVulkanInteropAdapter {
public:
	static RTGIVulkanInteropDeviceContext get_device_context(RenderingDevice *p_rd) {
		RTGIVulkanInteropDeviceContext context;
		if (p_rd == nullptr) {
			context.failure_reason = "RenderingDevice is unavailable.";
			return context;
		}
		if (!_rtgi_current_rd_device_family_is_vulkan(p_rd)) {
			context.failure_reason = "The active RenderingDevice driver is not Vulkan.";
			return context;
		}

		context.instance = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT);
		context.physical_device = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
		context.logical_device = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
		context.queue = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_COMMAND_QUEUE);
		context.queue_family_index = (uint32_t)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_QUEUE_FAMILY);
		context.valid = context.instance != 0 && context.physical_device != 0 && context.logical_device != 0 && context.queue != 0;
		if (!context.valid) {
			context.failure_reason = "RenderingDevice did not expose a complete Vulkan instance, physical device, logical device, queue, and queue family context.";
		}
		return context;
	}

	static RTGIVulkanInteropTextureContext get_texture_context(RenderingDevice *p_rd, RID p_texture) {
		RTGIVulkanInteropTextureContext context;
		if (p_rd == nullptr) {
			context.failure_reason = "RenderingDevice is unavailable.";
			return context;
		}
		if (!p_texture.is_valid()) {
			context.failure_reason = "RTGI output texture RID is invalid.";
			return context;
		}
		context.image = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture);
		context.image_view = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
		context.format = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
		context.device_memory = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture);
		context.usage_flags = p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture);
		context.valid = context.image != 0 && context.image_view != 0 && context.format != 0 && context.device_memory != 0 && context.usage_flags != 0;
		if (!context.valid) {
			context.failure_reason = "RenderingDevice did not expose a complete Vulkan image, view, format, memory, and usage context for the RTGI output texture.";
		}
		return context;
	}

	static bool populate_external_memory_exchange(RenderingDevice *p_rd, RTGIBackendResourceExchange &r_exchange, String *r_failure_reason) {
		if (p_rd == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = "RenderingDevice is unavailable.";
			}
			return false;
		}
		if (!r_exchange.output_texture.is_valid()) {
			if (r_failure_reason) {
				*r_failure_reason = "RTGI output texture RID is invalid.";
			}
			return false;
		}

		const RTGIVulkanInteropDeviceContext device_context = get_device_context(p_rd);
		if (!device_context.valid) {
			if (r_failure_reason) {
				*r_failure_reason = device_context.failure_reason;
			}
			return false;
		}

		const RTGIVulkanInteropTextureContext texture_context = get_texture_context(p_rd, r_exchange.output_texture);
		if (!texture_context.valid) {
			if (r_failure_reason) {
				*r_failure_reason = texture_context.failure_reason;
			}
			return false;
		}

		RDD::ExternalMemoryHandleInfo memory_info;
		if (!p_rd->texture_get_external_memory_handle(r_exchange.output_texture, memory_info) || memory_info.handle == 0 || !memory_info.exportable) {
			if (r_failure_reason) {
				*r_failure_reason = "RTGI output texture memory could not be exported through RenderingDevice.";
			}
			return false;
		}

		RDD::SemaphoreID wait_semaphore = p_rd->external_semaphore_create();
		if (!wait_semaphore) {
			_rtgi_close_external_handle(memory_info.handle, _rtgi_backend_external_handle_type_from_rdd(memory_info.handle_type));
			if (r_failure_reason) {
				*r_failure_reason = "RenderingDevice could not create an exportable wait semaphore for RTGI.";
			}
			return false;
		}

		RDD::SemaphoreID signal_semaphore = p_rd->external_semaphore_create();
		if (!signal_semaphore) {
			p_rd->external_semaphore_free(wait_semaphore);
			_rtgi_close_external_handle(memory_info.handle, _rtgi_backend_external_handle_type_from_rdd(memory_info.handle_type));
			if (r_failure_reason) {
				*r_failure_reason = "RenderingDevice could not create an exportable signal semaphore for RTGI.";
			}
			return false;
		}

		RDD::ExternalSemaphoreHandleInfo wait_info;
		RDD::ExternalSemaphoreHandleInfo signal_info;
		if (!p_rd->external_semaphore_get_handle(wait_semaphore, false, 0, wait_info) || wait_info.handle == 0 || !wait_info.exportable ||
				!p_rd->external_semaphore_get_handle(signal_semaphore, false, 0, signal_info) || signal_info.handle == 0 || !signal_info.exportable) {
			_rtgi_close_external_handle(wait_info.handle, _rtgi_backend_external_handle_type_from_rdd(wait_info.handle_type));
			_rtgi_close_external_handle(signal_info.handle, _rtgi_backend_external_handle_type_from_rdd(signal_info.handle_type));
			p_rd->external_semaphore_free(wait_semaphore);
			p_rd->external_semaphore_free(signal_semaphore);
			_rtgi_close_external_handle(memory_info.handle, _rtgi_backend_external_handle_type_from_rdd(memory_info.handle_type));
			if (r_failure_reason) {
				*r_failure_reason = "RenderingDevice could not export binary external semaphore handles for RTGI.";
			}
			return false;
		}

		r_exchange.mode = RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE;
		r_exchange.ownership_direction = RTGI_BACKEND_OWNERSHIP_RD_TO_BACKEND_TO_RD;
		r_exchange.external_memory_handle = memory_info.handle;
		r_exchange.external_memory_handle_type = _rtgi_backend_external_handle_type_from_rdd(memory_info.handle_type);
		r_exchange.external_memory_allocation_offset = memory_info.allocation_offset;
		r_exchange.external_memory_allocation_size = memory_info.allocation_size;
		r_exchange.external_memory_dedicated_allocation = memory_info.dedicated_allocation;
		r_exchange.external_wait_semaphore = wait_semaphore;
		r_exchange.external_wait_semaphore_handle = wait_info.handle;
		r_exchange.external_wait_semaphore_handle_type = _rtgi_backend_external_handle_type_from_rdd(wait_info.handle_type);
		r_exchange.external_signal_semaphore = signal_semaphore;
		r_exchange.external_signal_semaphore_handle = signal_info.handle;
		r_exchange.external_signal_semaphore_handle_type = _rtgi_backend_external_handle_type_from_rdd(signal_info.handle_type);
		r_exchange.external_semaphore_kind = RTGI_BACKEND_EXTERNAL_SEMAPHORE_BINARY;
		r_exchange.external_output_layout_before_backend = RDD::TEXTURE_LAYOUT_GENERAL;
		r_exchange.external_output_layout_after_backend = RDD::TEXTURE_LAYOUT_GENERAL;
		r_exchange.acquire_driver_callback = _rtgi_vulkan_interop_noop_callback;
		r_exchange.release_driver_callback = _rtgi_vulkan_interop_noop_callback;
		r_exchange.acquire_callback_resources.push_back({ r_exchange.output_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		r_exchange.release_callback_resources.push_back({ r_exchange.output_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		r_exchange.rd_owns_output_before_dispatch = true;
		r_exchange.rd_owns_output_after_dispatch = true;
		return true;
	}

	static void cleanup_external_memory_exchange(RenderingDevice *p_rd, RTGIBackendResourceExchange &r_exchange) {
		_rtgi_close_external_handle(r_exchange.external_memory_handle, r_exchange.external_memory_handle_type);
		_rtgi_close_external_handle(r_exchange.external_wait_semaphore_handle, r_exchange.external_wait_semaphore_handle_type);
		_rtgi_close_external_handle(r_exchange.external_signal_semaphore_handle, r_exchange.external_signal_semaphore_handle_type);
		if (p_rd != nullptr) {
			p_rd->external_semaphore_free(r_exchange.external_wait_semaphore);
			p_rd->external_semaphore_free(r_exchange.external_signal_semaphore);
		}
		r_exchange.external_memory_handle = 0;
		r_exchange.external_wait_semaphore_handle = 0;
		r_exchange.external_signal_semaphore_handle = 0;
		r_exchange.external_wait_semaphore = RDD::SemaphoreID();
		r_exchange.external_signal_semaphore = RDD::SemaphoreID();
	}
};

static bool _rtgi_vulkan_interop_self_validate(RenderingDevice *p_rd, String *r_failure_reason) {
	if (p_rd == nullptr) {
		if (r_failure_reason) {
			*r_failure_reason = "RenderingDevice is unavailable.";
		}
		return false;
	}
	if (!_rtgi_current_rd_device_family_is_vulkan(p_rd)) {
		if (r_failure_reason) {
			*r_failure_reason = "The active RenderingDevice driver is not Vulkan.";
		}
		return false;
	}

	const RDD::Capabilities &capabilities = p_rd->get_device_capabilities();
	if (!capabilities.external_memory_supported || !capabilities.external_semaphore_supported) {
		if (r_failure_reason) {
			*r_failure_reason = "The active Vulkan RenderingDevice does not report external memory and binary semaphore export support.";
		}
		return false;
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = 1;
	tf.height = 1;
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.samples = RD::TEXTURE_SAMPLES_1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID texture = p_rd->texture_create_exportable(tf, RD::TextureView());
	if (!texture.is_valid()) {
		if (r_failure_reason) {
			*r_failure_reason = "RenderingDevice could not allocate a self-test exportable RTGI texture.";
		}
		return false;
	}

	RTGIBackendFrameContext context;
	context.rd = p_rd;
	context.exchange.output_texture = texture;
	String exchange_failure;
	const bool ok = RTGIVulkanInteropAdapter::populate_external_memory_exchange(p_rd, context.exchange, &exchange_failure);
	if (ok) {
		if (context.exchange.external_memory_handle == 0 ||
				context.exchange.external_wait_semaphore_handle == 0 ||
				context.exchange.external_signal_semaphore_handle == 0 ||
				context.exchange.external_memory_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_NONE ||
				context.exchange.external_wait_semaphore_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_NONE ||
				context.exchange.external_signal_semaphore_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_NONE) {
			exchange_failure = "Vulkan RTGI interop self-test produced incomplete handle metadata.";
		}
	}
	RTGIVulkanInteropAdapter::cleanup_external_memory_exchange(p_rd, context.exchange);
	p_rd->free_rid(texture);

	if (!ok || !exchange_failure.is_empty()) {
		if (r_failure_reason) {
			*r_failure_reason = exchange_failure.is_empty() ? "Vulkan RTGI interop self-test failed." : exchange_failure;
		}
		return false;
	}
	return true;
}

class VulkanGenericRTGIBackend : public RTGIBackend {
	RenderForwardClustered *owner = nullptr;
	RenderRaytracing *raytracing = nullptr;

	bool _add_generic_dependencies(RD::RaytracingListID p_list, uint32_t p_extra_dependency_count = 0) const {
		ERR_FAIL_NULL_V(raytracing, false);

		const uint32_t rt_dependency_count =
				1 + p_extra_dependency_count +
				raytracing->get_material_ubo_dependencies().size() +
				raytracing->get_geometry_buffer_dependencies().size() +
				raytracing->get_deformed_buffer_dependencies().size();
		raytracing->begin_unique_buffer_dependencies(rt_dependency_count);
		RID mat_ubo_pool = raytracing->get_mat_ubo_pool_buffer();
		raytracing->add_unique_buffer_dependency(p_list, mat_ubo_pool);
		for (const RID &material_ubo : raytracing->get_material_ubo_dependencies()) {
			raytracing->add_unique_buffer_dependency(p_list, material_ubo);
		}
		for (const RID &geometry_buffer : raytracing->get_geometry_buffer_dependencies()) {
			raytracing->add_unique_buffer_dependency(p_list, geometry_buffer);
		}
		for (const RID &deformed_buffer : raytracing->get_deformed_buffer_dependencies()) {
			raytracing->add_unique_buffer_dependency(p_list, deformed_buffer);
		}
		return true;
	}

public:
	virtual RTGIBackendCapabilities query_capabilities() const override {
		return _rtgi_vulkan_generic_capabilities();
	}

	virtual bool initialize(RenderForwardClustered *p_owner, RenderRaytracing *p_raytracing, String *r_fallback_reason) override {
		owner = p_owner;
		raytracing = p_raytracing;
		RTGIBackendCapabilities caps = query_capabilities();
		if (!caps.available) {
			if (r_fallback_reason) {
				*r_fallback_reason = caps.fallback_reason;
			}
			return false;
		}
		return true;
	}

	virtual void shutdown() override {
		owner = nullptr;
		raytracing = nullptr;
	}

	virtual bool prepare_frame(RTGIBackendFrameContext &r_context, String *r_fallback_reason) override {
		if (raytracing == nullptr || raytracing->get_shader() == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic ray tracing shader is not initialized.";
			}
			return false;
		}

		Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
		if (r_context.render_data && r_context.render_data->render_buffers.is_valid() && r_context.render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = r_context.render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
		if (rb_data.is_null()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic render buffer data is unavailable.";
			}
			return false;
		}

		r_context.rd = RD::get_singleton();
		r_context.viewport_state = raytracing->build_tlas(r_context.render_data, r_context.rt_flags);
		if (r_context.viewport_state != nullptr) {
			r_context.uniform_set = raytracing->update_uniform_set(r_context.viewport_state, r_context.render_data, r_context.rt_flags);
			r_context.radiance_history_invalidated = r_context.viewport_state->radiance_history_invalidated;
			raytracing->populate_backend_scene_resources(r_context.viewport_state, r_context.scene_resources);
			raytracing->populate_backend_scene_snapshot(r_context.viewport_state, r_context.scene_snapshot);
		}
		if (r_context.uniform_set.is_valid()) {
			r_context.pipeline = raytracing->get_shader()->get_raytracing_pipeline(r_context.rt_flags);
		}
		r_context.output_size = rb_data->rt_get_size();
		r_context.exchange.mode = RTGI_BACKEND_EXCHANGE_RD_INTERNAL;
		r_context.exchange.output_texture = rb_data->rt_get_texture();
		r_context.exchange.depth_texture = rb_data->rt_has_depth_texture() ? rb_data->rt_get_depth_texture() : RID();
		r_context.exchange.diffuse_radiance_texture = rb_data->rt_get_diffuse_radiance();
		r_context.exchange.specular_radiance_texture = rb_data->rt_get_specular_radiance();
		r_context.exchange.rd_owns_output_before_dispatch = true;
		r_context.exchange.rd_owns_output_after_dispatch = true;
		if (r_context.exchange.output_texture.is_valid()) {
			r_context.exchange.release_callback_resources.push_back({ r_context.exchange.output_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		if (r_context.exchange.depth_texture.is_valid()) {
			r_context.exchange.release_callback_resources.push_back({ r_context.exchange.depth_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		if (r_context.exchange.diffuse_radiance_texture.is_valid()) {
			r_context.exchange.release_callback_resources.push_back({ r_context.exchange.diffuse_radiance_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}
		if (r_context.exchange.specular_radiance_texture.is_valid()) {
			r_context.exchange.release_callback_resources.push_back({ r_context.exchange.specular_radiance_texture, RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE });
		}

		if (r_context.viewport_state == nullptr || !r_context.uniform_set.is_valid() || !r_context.pipeline.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic TLAS, uniform set, or pipeline is invalid.";
			}
			return false;
		}
		return true;
	}

	virtual bool upload_or_import_scene(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		if (p_context.viewport_state == nullptr || !p_context.scene_resources.tlas.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic TLAS/BLAS state was not built.";
			}
			return false;
		}
		return true;
	}

	virtual bool upload_materials_lights_environment(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		if (!p_context.uniform_set.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic material/light/environment uniform set is invalid.";
			}
			return false;
		}
		return true;
	}

	virtual bool prepare_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count, RID &r_probe_pipeline, RID &r_probe_uniform_set, String *r_fallback_reason) override {
		if (raytracing == nullptr || raytracing->get_shader() == nullptr || p_context.viewport_state == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic STRC probe update is missing raytracing state.";
			}
			return false;
		}
		r_probe_uniform_set = raytracing->update_uniform_set(p_context.viewport_state, p_context.render_data, p_probe_flags);
		r_probe_pipeline = raytracing->get_shader()->get_raytracing_pipeline(p_probe_flags);
		if (!r_probe_uniform_set.is_valid() || !r_probe_pipeline.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic STRC probe pipeline or uniform set is invalid.";
			}
			return false;
		}
		return true;
	}

	virtual bool dispatch_path_trace(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		RenderingDevice *rd = RD::get_singleton();
		if (raytracing == nullptr || rd == nullptr || !p_context.pipeline.is_valid() || !p_context.uniform_set.is_valid() || !rd->uniform_set_is_valid(p_context.uniform_set)) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic path trace dispatch is missing a pipeline or uniform set.";
			}
			return false;
		}

		RD::RaytracingListID raytracing_list = rd->raytracing_list_begin();
		rd->raytracing_list_bind_raytracing_pipeline(raytracing_list, p_context.pipeline);
		rd->raytracing_list_bind_uniform_set(raytracing_list, p_context.uniform_set, 0);
		RID bindless_set = raytracing->get_bindless_uniform_set();
		if (bindless_set.is_valid()) {
			if (!rd->uniform_set_is_valid(bindless_set)) {
				if (r_fallback_reason) {
					*r_fallback_reason = "Vulkan Generic path trace dispatch has a stale bindless uniform set.";
				}
				rd->raytracing_list_end();
				return false;
			}
			rd->raytracing_list_bind_uniform_set(raytracing_list, bindless_set, 1);
		}

		if (!_add_generic_dependencies(raytracing_list)) {
			rd->raytracing_list_end();
			return false;
		}

		rd->raytracing_list_trace_rays(raytracing_list, 0, raytracing->get_shader()->get_hit_sbt(p_context.rt_flags), p_context.output_size.width, p_context.output_size.height, 1);
		rd->raytracing_list_end();
		return true;
	}

	virtual bool dispatch_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_pipeline, RID p_probe_uniform_set, RID p_probe_output_buffer, uint32_t p_ray_count, String *r_fallback_reason) override {
		RenderingDevice *rd = RD::get_singleton();
		if (raytracing == nullptr || rd == nullptr || !p_probe_pipeline.is_valid() || !p_probe_uniform_set.is_valid() || !rd->uniform_set_is_valid(p_probe_uniform_set) || !p_probe_output_buffer.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Vulkan Generic STRC probe dispatch is missing a pipeline, uniform set, or output buffer.";
			}
			return false;
		}

		RD::RaytracingListID probe_list = rd->raytracing_list_begin();
		rd->raytracing_list_bind_raytracing_pipeline(probe_list, p_probe_pipeline);
		rd->raytracing_list_bind_uniform_set(probe_list, p_probe_uniform_set, 0);
		RID probe_bindless_set = raytracing->get_bindless_uniform_set();
		if (probe_bindless_set.is_valid()) {
			if (!rd->uniform_set_is_valid(probe_bindless_set)) {
				if (r_fallback_reason) {
					*r_fallback_reason = "Vulkan Generic STRC probe dispatch has a stale bindless uniform set.";
				}
				rd->raytracing_list_end();
				return false;
			}
			rd->raytracing_list_bind_uniform_set(probe_list, probe_bindless_set, 1);
		}
		if (!_add_generic_dependencies(probe_list, 1)) {
			rd->raytracing_list_end();
			return false;
		}
		rd->raytracing_list_add_buffer_dependency(probe_list, p_probe_output_buffer, true);
		rd->raytracing_list_trace_rays(probe_list, 0, raytracing->get_shader()->get_hit_sbt(p_probe_flags), p_ray_count, 1, 1);
		rd->raytracing_list_end();
		return true;
	}

	virtual bool handoff_denoiser(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool synchronize_output(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool abort_frame(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}
};



#if defined(RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED)
struct RTGIOSPRayVec3f {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};
static_assert(sizeof(RTGIOSPRayVec3f) == sizeof(float) * 3, "RTGIOSPRayVec3f must be a tightly packed float3.");
static_assert(offsetof(RTGIOSPRayVec3f, x) == 0, "RTGIOSPRayVec3f x offset must match OSPRay vec3f.");
static_assert(offsetof(RTGIOSPRayVec3f, y) == sizeof(float), "RTGIOSPRayVec3f y offset must match OSPRay vec3f.");
static_assert(offsetof(RTGIOSPRayVec3f, z) == sizeof(float) * 2, "RTGIOSPRayVec3f z offset must match OSPRay vec3f.");

struct RTGIOSPRayVec4f {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f;
};

struct RTGIOSPRayVec3ui {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t z = 0;
};
static_assert(sizeof(RTGIOSPRayVec3ui) == sizeof(uint32_t) * 3, "RTGIOSPRayVec3ui must be a tightly packed uint3.");
static_assert(offsetof(RTGIOSPRayVec3ui, x) == 0, "RTGIOSPRayVec3ui x offset must match OSPRay vec3ui.");
static_assert(offsetof(RTGIOSPRayVec3ui, y) == sizeof(uint32_t), "RTGIOSPRayVec3ui y offset must match OSPRay vec3ui.");
static_assert(offsetof(RTGIOSPRayVec3ui, z) == sizeof(uint32_t) * 2, "RTGIOSPRayVec3ui z offset must match OSPRay vec3ui.");

enum RTGIOSPRayConstants {
	RTGI_OSP_DATA = 0x8000000 + 100,
	RTGI_OSP_GEOMETRIC_MODEL = 0x8000000 + 104,
	RTGI_OSP_INSTANCE = 0x8000000 + 108,
	RTGI_OSP_MATERIAL = 0x8000000 + 110,
	RTGI_OSP_INT = 4000,
	RTGI_OSP_VEC3UI = 4502,
	RTGI_OSP_FLOAT = 6000,
	RTGI_OSP_VEC3F = 6002,
	RTGI_OSP_VEC4F = 6003,
	RTGI_OSP_FB_COLOR = 1 << 0,
	RTGI_OSP_FB_RGBA32F = 3,
	RTGI_OSP_TASK_FINISHED = 100000,
};

struct RTGIOSPRayDispatch {
#if defined(WINDOWS_ENABLED)
	HMODULE library_handle = nullptr;
#elif defined(LINUXBSD_ENABLED)
	void *library_handle = nullptr;
#endif
	String loaded_path;
	bool loaded = false;

	int (*load_module)(const char *) = nullptr;
	void *(*new_device)(const char *) = nullptr;
	void (*set_current_device)(void *) = nullptr;
	void (*device_commit)(void *) = nullptr;
	void (*device_release)(void *) = nullptr;
	void *(*new_shared_data)(const void *, uint32_t, uint64_t, int64_t, uint64_t, int64_t, uint64_t, int64_t, void (*)(const void *, const void *), const void *) = nullptr;
	void *(*new_geometry)(const char *) = nullptr;
	void *(*new_geometric_model)(void *) = nullptr;
	void *(*new_material)(const char *) = nullptr;
	void *(*new_group)() = nullptr;
	void *(*new_instance)(void *) = nullptr;
	void *(*new_world)() = nullptr;
	void *(*new_renderer)(const char *) = nullptr;
	void *(*new_camera)(const char *) = nullptr;
	void (*set_param)(void *, const char *, uint32_t, const void *) = nullptr;
	void (*commit)(void *) = nullptr;
	void (*release)(void *) = nullptr;
	void *(*new_frame_buffer)(int, int, uint32_t, uint32_t) = nullptr;
	void (*reset_accumulation)(void *) = nullptr;
	void *(*render_frame)(void *, void *, void *, void *) = nullptr;
	void (*wait)(void *, uint32_t) = nullptr;
	const void *(*map_frame_buffer)(void *, uint32_t) = nullptr;
	void (*unmap_frame_buffer)(const void *, void *) = nullptr;

	template <typename T>
	bool _resolve_symbol(T &r_symbol, const char *p_name, String *r_failure_reason) {
#if defined(WINDOWS_ENABLED)
		void *symbol = reinterpret_cast<void *>(GetProcAddress(library_handle, p_name));
#elif defined(LINUXBSD_ENABLED)
		void *symbol = dlsym(library_handle, p_name);
#else
		void *symbol = nullptr;
#endif
		if (symbol == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = vformat("OSPRay runtime library '%s' is missing required symbol '%s'.", loaded_path, p_name);
			}
			return false;
		}
		r_symbol = reinterpret_cast<T>(symbol);
		return true;
	}

	void unload() {
#if defined(WINDOWS_ENABLED)
		if (library_handle != nullptr) {
			FreeLibrary(library_handle);
			library_handle = nullptr;
		}
#elif defined(LINUXBSD_ENABLED)
		if (library_handle != nullptr) {
			dlclose(library_handle);
			library_handle = nullptr;
		}
#endif
		*this = RTGIOSPRayDispatch();
	}

	bool load(String *r_failure_reason) {
		if (loaded) {
			return true;
		}

		static const char *const ospray_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
			"ospray.dll",
#else
			"libospray.so",
#endif
		};
		static const char *const ospray_runtime_symbols[] = {
			"ospLoadModule",
			"ospNewDevice",
			"ospSetCurrentDevice",
			"ospDeviceCommit",
			"ospDeviceRelease",
			"ospNewSharedData",
			"ospNewGeometry",
			"ospNewGeometricModel",
			"ospNewMaterial",
			"ospNewGroup",
			"ospNewInstance",
			"ospNewWorld",
			"ospNewRenderer",
			"ospNewCamera",
			"ospSetParam",
			"ospCommit",
			"ospRelease",
			"ospNewFrameBuffer",
			"ospResetAccumulation",
			"ospRenderFrame",
			"ospWait",
			"ospMapFrameBuffer",
			"ospUnmapFrameBuffer",
		};
		static const char *const ospray_root_env_vars[] = {
			"OSPRAY_ROOT",
			"OSPRAY_DIR",
			"ONEAPI_ROOT",
		};

		String found_path;
		if (!_rtgi_find_runtime_library(ospray_runtime_libraries, sizeof(ospray_runtime_libraries) / sizeof(ospray_runtime_libraries[0]), ospray_runtime_symbols, sizeof(ospray_runtime_symbols) / sizeof(ospray_runtime_symbols[0]), ospray_root_env_vars, sizeof(ospray_root_env_vars) / sizeof(ospray_root_env_vars[0]), &found_path)) {
			if (r_failure_reason) {
				*r_failure_reason = "OSPRay runtime library with required C API symbols was not found in OSPRAY_ROOT, OSPRAY_DIR, ONEAPI_ROOT, PATH, or the executable directory.";
			}
			return false;
		}

		loaded_path = found_path;
#if defined(WINDOWS_ENABLED)
		const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
		const DWORD load_flags = found_path.is_absolute_path() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
		library_handle = LoadLibraryExW((LPCWSTR)(found_path.utf16().get_data()), nullptr, load_flags);
		SetErrorMode(previous_error_mode);
#elif defined(LINUXBSD_ENABLED)
		library_handle = dlopen(found_path.utf8().get_data(), RTLD_LAZY | RTLD_LOCAL);
#endif
		if (library_handle == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = "OSPRay runtime library was found but could not be loaded for the RTGI backend.";
			}
			unload();
			return false;
		}

		if (!_resolve_symbol(load_module, "ospLoadModule", r_failure_reason) ||
				!_resolve_symbol(new_device, "ospNewDevice", r_failure_reason) ||
				!_resolve_symbol(set_current_device, "ospSetCurrentDevice", r_failure_reason) ||
				!_resolve_symbol(device_commit, "ospDeviceCommit", r_failure_reason) ||
				!_resolve_symbol(device_release, "ospDeviceRelease", r_failure_reason) ||
				!_resolve_symbol(new_shared_data, "ospNewSharedData", r_failure_reason) ||
				!_resolve_symbol(new_geometry, "ospNewGeometry", r_failure_reason) ||
				!_resolve_symbol(new_geometric_model, "ospNewGeometricModel", r_failure_reason) ||
				!_resolve_symbol(new_material, "ospNewMaterial", r_failure_reason) ||
				!_resolve_symbol(new_group, "ospNewGroup", r_failure_reason) ||
				!_resolve_symbol(new_instance, "ospNewInstance", r_failure_reason) ||
				!_resolve_symbol(new_world, "ospNewWorld", r_failure_reason) ||
				!_resolve_symbol(new_renderer, "ospNewRenderer", r_failure_reason) ||
				!_resolve_symbol(new_camera, "ospNewCamera", r_failure_reason) ||
				!_resolve_symbol(set_param, "ospSetParam", r_failure_reason) ||
				!_resolve_symbol(commit, "ospCommit", r_failure_reason) ||
				!_resolve_symbol(release, "ospRelease", r_failure_reason) ||
				!_resolve_symbol(new_frame_buffer, "ospNewFrameBuffer", r_failure_reason) ||
				!_resolve_symbol(reset_accumulation, "ospResetAccumulation", r_failure_reason) ||
				!_resolve_symbol(render_frame, "ospRenderFrame", r_failure_reason) ||
				!_resolve_symbol(wait, "ospWait", r_failure_reason) ||
				!_resolve_symbol(map_frame_buffer, "ospMapFrameBuffer", r_failure_reason) ||
				!_resolve_symbol(unmap_frame_buffer, "ospUnmapFrameBuffer", r_failure_reason)) {
			unload();
			return false;
		}

		loaded = true;
		return true;
	}
};

class EmbreeOSPRayRTGIBackend : public RTGIBackend {
	RenderForwardClustered *owner = nullptr;
	RenderRaytracing *raytracing = nullptr;
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
	RTCDevice embree_device = nullptr;
	RTCScene embree_scene = nullptr;
#endif
	RTGIOSPRayDispatch ospray;
	void *ospray_device = nullptr;
	void *ospray_world = nullptr;
	void *ospray_renderer = nullptr;
	void *ospray_camera = nullptr;
	void *ospray_framebuffer = nullptr;
	LocalVector<void *> ospray_scene_objects;
	LocalVector<void *> ospray_model_handles;
	LocalVector<void *> ospray_instance_handles;
	LocalVector<Vector<RTGIOSPRayVec3f>> ospray_vertex_storage;
	LocalVector<Vector<RTGIOSPRayVec3ui>> ospray_index_storage;
	Vector<uint8_t> output_rgba16f;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	bool ospray_scene_active = false;

#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
	static void _embree_error_callback(void *p_user_data, RTCError p_error, const char *p_message) {
		if (p_error == RTC_ERROR_NONE) {
			return;
		}
		WARN_PRINT(vformat("Embree RTGI backend error %d: %s", int(p_error), p_message ? p_message : ""));
	}
#endif

	void _release_scene() {
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
		if (embree_scene != nullptr) {
			rtcReleaseScene(embree_scene);
			embree_scene = nullptr;
		}
#endif
	}

	void _release_device() {
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
		_release_scene();
		if (embree_device != nullptr) {
			rtcReleaseDevice(embree_device);
			embree_device = nullptr;
		}
#endif
	}

	void _release_ospray_scene() {
		if (ospray.loaded && ospray.release != nullptr) {
			for (int64_t i = int64_t(ospray_scene_objects.size()) - 1; i >= 0; i--) {
				if (ospray_scene_objects[i] != nullptr) {
					ospray.release(ospray_scene_objects[i]);
				}
			}
			if (ospray_framebuffer != nullptr) {
				ospray.release(ospray_framebuffer);
			}
		}
		ospray_scene_objects.clear();
		ospray_model_handles.clear();
		ospray_instance_handles.clear();
		ospray_vertex_storage.clear();
		ospray_index_storage.clear();
		ospray_world = nullptr;
		ospray_renderer = nullptr;
		ospray_camera = nullptr;
		ospray_framebuffer = nullptr;
		ospray_scene_active = false;
	}

	void _release_ospray_device() {
		_release_ospray_scene();
		if (ospray_device != nullptr && ospray.loaded && ospray.device_release != nullptr) {
			ospray.device_release(ospray_device);
			ospray_device = nullptr;
		}
		ospray.unload();
	}

	void _track_ospray_object(void *p_object) {
		if (p_object != nullptr) {
			ospray_scene_objects.push_back(p_object);
		}
	}

	static void _store_rgba16f(Vector<uint8_t> &r_data, uint32_t p_pixel_index, const Color &p_color) {
		uint8_t *dst = r_data.ptrw() + uint64_t(p_pixel_index) * sizeof(uint16_t) * 4;
		encode_uint16(Math::make_half_float(MAX(0.0f, p_color.r)), dst + 0);
		encode_uint16(Math::make_half_float(MAX(0.0f, p_color.g)), dst + 2);
		encode_uint16(Math::make_half_float(MAX(0.0f, p_color.b)), dst + 4);
		encode_uint16(Math::make_half_float(MAX(0.0f, p_color.a)), dst + 6);
	}

	static Color _material_shade(const RT_MaterialData &p_material, const Vector3 &p_normal, const Vector3 &p_ray_direction) {
		const Color albedo(p_material.albedo_color[0], p_material.albedo_color[1], p_material.albedo_color[2], p_material.albedo_color[3]);
		const Color emission(p_material.emission_color[0], p_material.emission_color[1], p_material.emission_color[2], 1.0f);
		const float facing = CLAMP(Math::abs(p_normal.normalized().dot(-p_ray_direction.normalized())), 0.0f, 1.0f);
		const float diffuse = 0.25f + 0.75f * facing;
		Color shaded = albedo * diffuse;
		shaded.r += emission.r * MAX(0.0f, p_material.emission_strength);
		shaded.g += emission.g * MAX(0.0f, p_material.emission_strength);
		shaded.b += emission.b * MAX(0.0f, p_material.emission_strength);
		shaded.a = 1.0f;
		return shaded;
	}

	bool _ensure_device(String *r_fallback_reason) {
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
		if (embree_device != nullptr) {
			return true;
		}
		embree_device = rtcNewDevice(nullptr);
		if (embree_device == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree could not create an RTCDevice for the RTGI backend.";
			}
			return false;
		}
		rtcSetDeviceErrorFunction(embree_device, _embree_error_callback, nullptr);
		return true;
#else
		if (r_fallback_reason) {
			*r_fallback_reason = "Embree CPU dispatch is not compiled in this build.";
		}
		return false;
#endif
	}

	bool _ensure_ospray_device(String *r_fallback_reason) {
		if (ospray_device != nullptr) {
			return true;
		}
		if (!ospray.load(r_fallback_reason)) {
			return false;
		}

		const int load_error = ospray.load_module("cpu");
		if (load_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("OSPRay could not load the CPU module for RTGI dispatch; ospLoadModule returned %d.", load_error);
			}
			return false;
		}

		ospray_device = ospray.new_device("cpu");
		if (ospray_device == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay runtime loaded, but could not create a CPU device for the RTGI backend.";
			}
			return false;
		}
		ospray.set_current_device(ospray_device);
		ospray.device_commit(ospray_device);
		return true;
	}

	bool _create_ospray_scene_from_snapshot(const RTGIBackendSceneSnapshot &p_snapshot, String *r_fallback_reason) {
		if (!_ensure_ospray_device(r_fallback_reason)) {
			return false;
		}
		if (p_snapshot.geometries.size() != p_snapshot.cpu_geometries.size() ||
				p_snapshot.geometries.size() != p_snapshot.blas_transforms.size() ||
				p_snapshot.geometries.size() != p_snapshot.materials.size() ||
				p_snapshot.geometries.size() != p_snapshot.instance_masks.size()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay RTGI scene snapshot arrays are not parallel.";
			}
			return false;
		}

		_release_ospray_scene();
		ospray.set_current_device(ospray_device);

		for (uint32_t geometry_index = 0; geometry_index < p_snapshot.geometries.size(); geometry_index++) {
			if ((p_snapshot.instance_masks[geometry_index] & RT_INSTANCE_MASK_VISIBLE) == 0) {
				continue;
			}

			const RT_MaterialData &material_data = p_snapshot.materials[geometry_index];
			const uint32_t unsupported_material_flags = RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP | RT_MAT_FLAG_ALPHA_HASH | RT_MAT_FLAG_ALPHA_TEST;
			if ((material_data.flags & unsupported_material_flags) != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay RTGI backend cannot yet evaluate alpha-tested or custom hit shaders; falling back to Embree or Vulkan Generic.";
				}
				return false;
			}

			const RTGIBackendCPUGeometry &cpu_geometry = p_snapshot.cpu_geometries[geometry_index];
			if (!cpu_geometry.valid) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay RTGI backend requires CPU geometry in the scene snapshot; procedural, deformed, and GPU-merged surfaces still fall back to Embree or Vulkan Generic.";
				}
				return false;
			}

			ospray_vertex_storage.push_back(Vector<RTGIOSPRayVec3f>());
			Vector<RTGIOSPRayVec3f> &vertices = ospray_vertex_storage[ospray_vertex_storage.size() - 1];
			vertices.resize(cpu_geometry.vertices.size());
			const Transform3D &transform = p_snapshot.blas_transforms[geometry_index];
			_rtgi_pack_transformed_vertices_float3(cpu_geometry.vertices.ptr(), cpu_geometry.vertices.size(), transform, vertices.ptrw());

			ospray_index_storage.push_back(Vector<RTGIOSPRayVec3ui>());
			Vector<RTGIOSPRayVec3ui> &indices = ospray_index_storage[ospray_index_storage.size() - 1];
			indices.resize(cpu_geometry.primitive_count);
			memcpy(indices.ptrw(), cpu_geometry.indices.ptr(), uint64_t(cpu_geometry.primitive_count) * sizeof(RTGIOSPRayVec3ui));

			void *vertex_data = ospray.new_shared_data(vertices.ptr(), RTGI_OSP_VEC3F, vertices.size(), sizeof(RTGIOSPRayVec3f), 1, 0, 1, 0, nullptr, nullptr);
			void *index_data = ospray.new_shared_data(indices.ptr(), RTGI_OSP_VEC3UI, indices.size(), sizeof(RTGIOSPRayVec3ui), 1, 0, 1, 0, nullptr, nullptr);
			void *mesh = ospray.new_geometry("mesh");
			void *material = ospray.new_material("obj");
			if (vertex_data == nullptr || index_data == nullptr || mesh == nullptr || material == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay could not allocate mesh data, geometry, or material objects.";
				}
				return false;
			}
			_track_ospray_object(vertex_data);
			_track_ospray_object(index_data);
			_track_ospray_object(mesh);
			_track_ospray_object(material);

			ospray.commit(vertex_data);
			ospray.commit(index_data);
			ospray.set_param(mesh, "vertex.position", RTGI_OSP_DATA, &vertex_data);
			ospray.set_param(mesh, "index", RTGI_OSP_DATA, &index_data);
			ospray.commit(mesh);

			const RTGIOSPRayVec3f kd = {
				MAX(0.0f, material_data.albedo_color[0]),
				MAX(0.0f, material_data.albedo_color[1]),
				MAX(0.0f, material_data.albedo_color[2]),
			};
			const float alpha = CLAMP(material_data.albedo_color[3], 0.0f, 1.0f);
			ospray.set_param(material, "kd", RTGI_OSP_VEC3F, &kd);
			ospray.set_param(material, "d", RTGI_OSP_FLOAT, &alpha);
			ospray.commit(material);

			void *model = ospray.new_geometric_model(mesh);
			if (model == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay could not allocate a geometric model for RTGI mesh data.";
				}
				return false;
			}
			_track_ospray_object(model);
			ospray.set_param(model, "material", RTGI_OSP_MATERIAL, &material);
			ospray.commit(model);
			ospray_model_handles.push_back(model);
		}

		void *group = ospray.new_group();
		void *model_data = nullptr;
		if (group == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay could not allocate a group for the RTGI scene.";
			}
			return false;
		}
		_track_ospray_object(group);
		if (!ospray_model_handles.is_empty()) {
			model_data = ospray.new_shared_data(ospray_model_handles.ptr(), RTGI_OSP_GEOMETRIC_MODEL, ospray_model_handles.size(), sizeof(void *), 1, 0, 1, 0, nullptr, nullptr);
			if (model_data == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay could not allocate geometric model array data for the RTGI scene.";
				}
				return false;
			}
			_track_ospray_object(model_data);
			ospray.commit(model_data);
			ospray.set_param(group, "geometry", RTGI_OSP_DATA, &model_data);
		}
		ospray.commit(group);

		void *instance = ospray.new_instance(group);
		if (instance == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay could not allocate an instance for the RTGI scene.";
			}
			return false;
		}
		_track_ospray_object(instance);
		ospray.commit(instance);
		ospray_instance_handles.push_back(instance);

		void *instance_data = ospray.new_shared_data(ospray_instance_handles.ptr(), RTGI_OSP_INSTANCE, ospray_instance_handles.size(), sizeof(void *), 1, 0, 1, 0, nullptr, nullptr);
		ospray_world = ospray.new_world();
		ospray_renderer = ospray.new_renderer("pathtracer");
		if (ospray_renderer == nullptr) {
			ospray_renderer = ospray.new_renderer("ao");
		}
		if (instance_data == nullptr || ospray_world == nullptr || ospray_renderer == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay could not allocate world, renderer, or instance array data for the RTGI scene.";
			}
			return false;
		}
		_track_ospray_object(instance_data);
		_track_ospray_object(ospray_world);
		_track_ospray_object(ospray_renderer);
		ospray.commit(instance_data);
		ospray.set_param(ospray_world, "instance", RTGI_OSP_DATA, &instance_data);
		ospray.commit(ospray_world);

		const int pixel_samples = 1;
		const RTGIOSPRayVec4f background = { 0.0f, 0.0f, 0.0f, 1.0f };
		ospray.set_param(ospray_renderer, "pixelSamples", RTGI_OSP_INT, &pixel_samples);
		ospray.set_param(ospray_renderer, "backgroundColor", RTGI_OSP_VEC4F, &background);
		ospray.commit(ospray_renderer);
		ospray_scene_active = true;
		return true;
	}

	bool _create_scene_from_snapshot(const RTGIBackendSceneSnapshot &p_snapshot, String *r_fallback_reason) {
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
		if (!_ensure_device(r_fallback_reason)) {
			return false;
		}
		if (p_snapshot.geometries.size() != p_snapshot.cpu_geometries.size() ||
				p_snapshot.geometries.size() != p_snapshot.blas_transforms.size() ||
				p_snapshot.geometries.size() != p_snapshot.materials.size() ||
				p_snapshot.geometries.size() != p_snapshot.instance_masks.size()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI scene snapshot arrays are not parallel.";
			}
			return false;
		}

		_release_scene();
		embree_scene = rtcNewScene(embree_device);
		if (embree_scene == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree could not create an RTCScene for the RTGI backend.";
			}
			return false;
		}

		for (uint32_t geometry_index = 0; geometry_index < p_snapshot.geometries.size(); geometry_index++) {
			if ((p_snapshot.instance_masks[geometry_index] & RT_INSTANCE_MASK_VISIBLE) == 0) {
				continue;
			}

			const RT_MaterialData &material = p_snapshot.materials[geometry_index];
			const uint32_t unsupported_material_flags = RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP | RT_MAT_FLAG_ALPHA_HASH | RT_MAT_FLAG_ALPHA_TEST;
			if ((material.flags & unsupported_material_flags) != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = "Embree RTGI backend cannot yet evaluate alpha-tested or custom hit shaders; falling back to Vulkan Generic.";
				}
				return false;
			}

			const RTGIBackendCPUGeometry &cpu_geometry = p_snapshot.cpu_geometries[geometry_index];
			if (!cpu_geometry.valid) {
				if (r_fallback_reason) {
					*r_fallback_reason = "Embree RTGI backend requires CPU geometry in the scene snapshot; procedural, deformed, and GPU-merged surfaces still fall back to Vulkan Generic.";
				}
				return false;
			}

			RTCGeometry geometry = rtcNewGeometry(embree_device, RTC_GEOMETRY_TYPE_TRIANGLE);
			if (geometry == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "Embree could not allocate triangle geometry.";
				}
				return false;
			}

			Vector3 *vertices = static_cast<Vector3 *>(rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Vector3), cpu_geometry.vertices.size()));
			uint32_t *indices = static_cast<uint32_t *>(rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(uint32_t) * 3, cpu_geometry.primitive_count));
			if (vertices == nullptr || indices == nullptr) {
				rtcReleaseGeometry(geometry);
				if (r_fallback_reason) {
					*r_fallback_reason = "Embree could not allocate geometry vertex or index buffers.";
				}
				return false;
			}

			const Transform3D &transform = p_snapshot.blas_transforms[geometry_index];
			for (uint32_t vertex_index = 0; vertex_index < cpu_geometry.vertices.size(); vertex_index++) {
				vertices[vertex_index] = transform.xform(cpu_geometry.vertices[vertex_index]);
			}
			for (uint32_t index = 0; index < cpu_geometry.indices.size(); index++) {
				indices[index] = cpu_geometry.indices[index];
			}

			rtcCommitGeometry(geometry);
			rtcAttachGeometryByID(embree_scene, geometry, geometry_index);
			rtcReleaseGeometry(geometry);
		}

		rtcCommitScene(embree_scene);
		return true;
#else
		if (r_fallback_reason) {
			*r_fallback_reason = "Embree CPU dispatch is not compiled in this build.";
		}
		return false;
#endif
	}

	bool _trace_primary(const RTGIBackendFrameContext &p_context, String *r_fallback_reason) {
#if defined(RTGI_EMBREE_CPU_DISPATCH_ENABLED)
		if (embree_scene == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI scene was not uploaded before dispatch.";
			}
			return false;
		}
		if (p_context.render_data == nullptr || p_context.render_data->scene_data == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI dispatch is missing RenderSceneDataRD camera data.";
			}
			return false;
		}
		if (p_context.output_size.x <= 0 || p_context.output_size.y <= 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI dispatch has an invalid output size.";
			}
			return false;
		}

		output_width = p_context.output_size.x;
		output_height = p_context.output_size.y;
		output_rgba16f.resize_initialized(uint64_t(output_width) * output_height * sizeof(uint16_t) * 4);

		const Transform3D &camera_transform = p_context.render_data->scene_data->cam_transform;
		const Projection inv_projection = p_context.render_data->scene_data->cam_projection.inverse();
		const bool orthogonal = p_context.render_data->scene_data->cam_orthogonal;
		const float z_far = Math::is_finite(p_context.render_data->scene_data->z_far) && p_context.render_data->scene_data->z_far > 0.0f ? p_context.render_data->scene_data->z_far : 10000.0f;

		RTCIntersectArguments args;
		rtcInitIntersectArguments(&args);

		for (uint32_t y = 0; y < output_height; y++) {
			const float ndc_y = 1.0f - ((float(y) + 0.5f) / float(output_height)) * 2.0f;
			for (uint32_t x = 0; x < output_width; x++) {
				const float ndc_x = ((float(x) + 0.5f) / float(output_width)) * 2.0f - 1.0f;
				const Vector3 view_near = inv_projection.xform(Vector3(ndc_x, ndc_y, 0.0f));
				const Vector3 view_far = inv_projection.xform(Vector3(ndc_x, ndc_y, 1.0f));

				Vector3 ray_origin;
				Vector3 ray_direction;
				if (orthogonal) {
					ray_origin = camera_transform.xform(view_near);
					ray_direction = -camera_transform.basis.get_column(Vector3::AXIS_Z).normalized();
				} else {
					ray_origin = camera_transform.origin;
					ray_direction = camera_transform.basis.xform((view_far - view_near).normalized()).normalized();
				}

				RTCRayHit ray_hit = {};
				ray_hit.ray.org_x = ray_origin.x;
				ray_hit.ray.org_y = ray_origin.y;
				ray_hit.ray.org_z = ray_origin.z;
				ray_hit.ray.dir_x = ray_direction.x;
				ray_hit.ray.dir_y = ray_direction.y;
				ray_hit.ray.dir_z = ray_direction.z;
				ray_hit.ray.tnear = 0.001f;
				ray_hit.ray.tfar = z_far;
				ray_hit.ray.mask = 0xFFFFFFFFu;
				ray_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
				ray_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

				rtcIntersect1(embree_scene, &ray_hit, &args);

				Color color(0, 0, 0, 1);
				if (ray_hit.hit.geomID != RTC_INVALID_GEOMETRY_ID && ray_hit.hit.geomID < p_context.scene_snapshot.materials.size()) {
					const Vector3 normal(ray_hit.hit.Ng_x, ray_hit.hit.Ng_y, ray_hit.hit.Ng_z);
					color = _material_shade(p_context.scene_snapshot.materials[ray_hit.hit.geomID], normal, ray_direction);
				}
				_store_rgba16f(output_rgba16f, y * output_width + x, color);
			}
		}
		return true;
#else
		if (r_fallback_reason) {
			*r_fallback_reason = "Embree CPU dispatch is not compiled in this build.";
		}
		return false;
#endif
	}

	bool _render_ospray_frame(const RTGIBackendFrameContext &p_context, String *r_fallback_reason) {
		if (!ospray_scene_active || ospray_world == nullptr || ospray_renderer == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay RTGI scene was not uploaded before dispatch.";
			}
			return false;
		}
		if (p_context.render_data == nullptr || p_context.render_data->scene_data == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay RTGI dispatch is missing RenderSceneDataRD camera data.";
			}
			return false;
		}
		if (p_context.output_size.x <= 0 || p_context.output_size.y <= 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay RTGI dispatch has an invalid output size.";
			}
			return false;
		}

		ospray.set_current_device(ospray_device);
		if (ospray_camera == nullptr) {
			ospray_camera = ospray.new_camera(p_context.render_data->scene_data->cam_orthogonal ? "orthographic" : "perspective");
			if (ospray_camera == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay could not allocate a camera for RTGI dispatch.";
				}
				return false;
			}
			_track_ospray_object(ospray_camera);
		}

		const Transform3D &camera_transform = p_context.render_data->scene_data->cam_transform;
		const Vector3 camera_position = camera_transform.origin;
		const Vector3 camera_direction = -camera_transform.basis.get_column(Vector3::AXIS_Z).normalized();
		const Vector3 camera_up = camera_transform.basis.get_column(Vector3::AXIS_Y).normalized();
		const RTGIOSPRayVec3f osp_position = { float(camera_position.x), float(camera_position.y), float(camera_position.z) };
		const RTGIOSPRayVec3f osp_direction = { float(camera_direction.x), float(camera_direction.y), float(camera_direction.z) };
		const RTGIOSPRayVec3f osp_up = { float(camera_up.x), float(camera_up.y), float(camera_up.z) };
		const float aspect = float(p_context.output_size.x) / MAX(float(p_context.output_size.y), 1.0f);

		ospray.set_param(ospray_camera, "position", RTGI_OSP_VEC3F, &osp_position);
		ospray.set_param(ospray_camera, "direction", RTGI_OSP_VEC3F, &osp_direction);
		ospray.set_param(ospray_camera, "up", RTGI_OSP_VEC3F, &osp_up);
		ospray.set_param(ospray_camera, "aspect", RTGI_OSP_FLOAT, &aspect);
		if (p_context.render_data->scene_data->cam_orthogonal) {
			const Vector2 half_extents = p_context.render_data->scene_data->cam_projection.get_viewport_half_extents();
			const float height = float(MAX(half_extents.y * 2.0, 0.001));
			ospray.set_param(ospray_camera, "height", RTGI_OSP_FLOAT, &height);
		} else {
			const float fov_x = float(p_context.render_data->scene_data->cam_projection.get_fov());
			const float fov_y = float(Projection::get_fovy(fov_x, 1.0f / MAX(aspect, 0.001f)));
			ospray.set_param(ospray_camera, "fovy", RTGI_OSP_FLOAT, &fov_y);
		}
		ospray.commit(ospray_camera);

		if (ospray_framebuffer == nullptr || output_width != uint32_t(p_context.output_size.x) || output_height != uint32_t(p_context.output_size.y)) {
			if (ospray_framebuffer != nullptr) {
				ospray.release(ospray_framebuffer);
				ospray_framebuffer = nullptr;
			}
			ospray_framebuffer = ospray.new_frame_buffer(p_context.output_size.x, p_context.output_size.y, RTGI_OSP_FB_RGBA32F, RTGI_OSP_FB_COLOR);
			if (ospray_framebuffer == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = "OSPRay could not allocate an RGBA32F framebuffer for RTGI dispatch.";
				}
				return false;
			}
		}
		output_width = p_context.output_size.x;
		output_height = p_context.output_size.y;
		ospray.reset_accumulation(ospray_framebuffer);

		void *future = ospray.render_frame(ospray_framebuffer, ospray_renderer, ospray_camera, ospray_world);
		if (future == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay failed to start RTGI frame rendering.";
			}
			return false;
		}
		ospray.wait(future, RTGI_OSP_TASK_FINISHED);
		ospray.release(future);

		const RTGIOSPRayVec4f *pixels = static_cast<const RTGIOSPRayVec4f *>(ospray.map_frame_buffer(ospray_framebuffer, RTGI_OSP_FB_COLOR));
		if (pixels == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "OSPRay did not return a mappable RTGI framebuffer.";
			}
			return false;
		}

		output_rgba16f.resize_initialized(uint64_t(output_width) * output_height * sizeof(uint16_t) * 4);
		for (uint32_t pixel_index = 0; pixel_index < output_width * output_height; pixel_index++) {
			const RTGIOSPRayVec4f &pixel = pixels[pixel_index];
			_store_rgba16f(output_rgba16f, pixel_index, Color(pixel.x, pixel.y, pixel.z, pixel.w));
		}
		ospray.unmap_frame_buffer(pixels, ospray_framebuffer);
		return true;
	}

public:
	virtual RTGIBackendCapabilities query_capabilities() const override {
		return _rtgi_static_backend_capabilities(RSE::PT_BACKEND_INTEL_EMBREE);
	}

	virtual bool initialize(RenderForwardClustered *p_owner, RenderRaytracing *p_raytracing, String *r_fallback_reason) override {
		owner = p_owner;
		raytracing = p_raytracing;
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (!capabilities.available) {
			if (r_fallback_reason) {
				*r_fallback_reason = capabilities.fallback_reason;
			}
			return false;
		}
		String ospray_reason;
		if (_ensure_ospray_device(&ospray_reason)) {
			return true;
		}
		String embree_reason;
		if (_ensure_device(&embree_reason)) {
			return true;
		}
		if (r_fallback_reason) {
			*r_fallback_reason = embree_reason;
			if (!ospray_reason.is_empty()) {
				if (r_fallback_reason->is_empty()) {
					*r_fallback_reason = "OSPRay initialization failed first: " + ospray_reason;
				} else {
					*r_fallback_reason += " OSPRay initialization failed first: " + ospray_reason;
				}
			}
		}
		return false;
	}

	virtual void shutdown() override {
		_release_device();
		_release_ospray_device();
		owner = nullptr;
		raytracing = nullptr;
		output_rgba16f.clear();
		output_width = 0;
		output_height = 0;
	}

	virtual bool prepare_frame(RTGIBackendFrameContext &r_context, String *r_fallback_reason) override {
		if (raytracing == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI backend is not initialized.";
			}
			return false;
		}

		Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
		if (r_context.render_data && r_context.render_data->render_buffers.is_valid() && r_context.render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = r_context.render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
		if (rb_data.is_null()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI render buffer data is unavailable.";
			}
			return false;
		}

		r_context.rd = RD::get_singleton();
		r_context.viewport_state = raytracing->build_tlas(r_context.render_data, r_context.rt_flags);
		if (r_context.viewport_state != nullptr) {
			r_context.radiance_history_invalidated = r_context.viewport_state->radiance_history_invalidated;
			raytracing->populate_backend_scene_resources(r_context.viewport_state, r_context.scene_resources);
			raytracing->populate_backend_scene_snapshot(r_context.viewport_state, r_context.scene_snapshot);
		}
		r_context.output_size = rb_data->rt_get_size();
		r_context.exchange.mode = RTGI_BACKEND_EXCHANGE_RD_INTERNAL;
		r_context.exchange.output_texture = rb_data->rt_get_texture();
		r_context.exchange.depth_texture = rb_data->rt_has_depth_texture() ? rb_data->rt_get_depth_texture() : RID();
		r_context.exchange.diffuse_radiance_texture = rb_data->rt_get_diffuse_radiance();
		r_context.exchange.specular_radiance_texture = rb_data->rt_get_specular_radiance();
		r_context.exchange.rd_owns_output_before_dispatch = true;
		r_context.exchange.rd_owns_output_after_dispatch = true;

		if (r_context.viewport_state == nullptr || !r_context.exchange.output_texture.is_valid()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI TLAS snapshot or RD output texture is invalid.";
			}
			return false;
		}
		return true;
	}

	virtual bool upload_or_import_scene(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		String ospray_reason;
		if (_create_ospray_scene_from_snapshot(p_context.scene_snapshot, &ospray_reason)) {
			_release_scene();
			return true;
		}
		_release_ospray_scene();
		String embree_reason;
		if (_create_scene_from_snapshot(p_context.scene_snapshot, &embree_reason)) {
			return true;
		}
		if (r_fallback_reason) {
			*r_fallback_reason = embree_reason;
			if (!ospray_reason.is_empty()) {
				if (r_fallback_reason->is_empty()) {
					*r_fallback_reason = "OSPRay path failed first: " + ospray_reason;
				} else {
					*r_fallback_reason += " OSPRay path failed first: " + ospray_reason;
				}
			}
		}
		return false;
	}

	virtual bool upload_materials_lights_environment(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool prepare_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count, RID &r_probe_pipeline, RID &r_probe_uniform_set, String *r_fallback_reason) override {
		if (r_fallback_reason) {
			*r_fallback_reason = "Embree RTGI routes STRC probe updates to Vulkan Generic.";
		}
		return false;
	}

	virtual bool dispatch_path_trace(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		if (ospray_scene_active) {
			return _render_ospray_frame(p_context, r_fallback_reason);
		}
		return _trace_primary(p_context, r_fallback_reason);
	}

	virtual bool dispatch_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_pipeline, RID p_probe_uniform_set, RID p_probe_output_buffer, uint32_t p_ray_count, String *r_fallback_reason) override {
		if (r_fallback_reason) {
			*r_fallback_reason = "Embree RTGI routes STRC probe dispatch to Vulkan Generic.";
		}
		return false;
	}

	virtual bool handoff_denoiser(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool synchronize_output(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		if (p_context.rd == nullptr || !p_context.exchange.output_texture.is_valid() || output_rgba16f.is_empty()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI has no CPU output available to upload.";
			}
			return false;
		}
		const Error err = p_context.rd->texture_update(p_context.exchange.output_texture, 0, output_rgba16f);
		if (err != OK) {
			if (r_fallback_reason) {
				*r_fallback_reason = "Embree RTGI failed to upload its CPU output into the RenderingDevice texture.";
			}
			return false;
		}
		return true;
	}

	virtual bool abort_frame(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		p_context.exchange.rd_owns_output_after_dispatch = true;
		return true;
	}
};
#endif

#if defined(WINDOWS_ENABLED)
#define RTGI_HIP_CALL __stdcall
#define RTGI_HIPRT_CALL __stdcall
#else
#define RTGI_HIP_CALL
#define RTGI_HIPRT_CALL
#endif

#ifndef RTGI_HIPRT_API_VERSION
#define RTGI_HIPRT_API_VERSION 0
#endif

#ifndef RTGI_HIPRT_SDK_ROOT
#define RTGI_HIPRT_SDK_ROOT ""
#endif

struct RTGIHIPRTContextCreationInput {
	void *ctxt = nullptr;
	int device = 0;
	int deviceType = 0;
};

enum RTGIHIPRTBuildOperation {
	RTGI_HIPRT_BUILD_OPERATION_BUILD = 1,
	RTGI_HIPRT_BUILD_OPERATION_UPDATE = 2,
};

enum RTGIHIPRTPrimitiveType {
	RTGI_HIPRT_PRIMITIVE_TYPE_TRIANGLE_MESH = 0,
	RTGI_HIPRT_PRIMITIVE_TYPE_AABB_LIST = 1,
};

enum RTGIHIPRTInstanceType {
	RTGI_HIPRT_INSTANCE_TYPE_GEOMETRY = 0,
	RTGI_HIPRT_INSTANCE_TYPE_SCENE = 1,
};

enum RTGIHIPRTFrameType {
	RTGI_HIPRT_FRAME_TYPE_SRT = 0,
	RTGI_HIPRT_FRAME_TYPE_MATRIX = 1,
};

struct RTGIHIPRTBuildOptions {
	uint32_t build_flags = 0;
	uint32_t batch_build_max_prim_count = 0;
};

struct RTGIHIPRTFloat3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct RTGIHIPRTTriangleMeshPrimitive {
	void *vertices = nullptr;
	uint32_t vertex_count = 0;
	uint32_t vertex_stride = 0;
	void *triangle_indices = nullptr;
	uint32_t triangle_count = 0;
	uint32_t triangle_stride = 0;
	void *triangle_pair_indices = nullptr;
	uint32_t triangle_pair_count = 0;
};

struct RTGIHIPRTAABBListPrimitive {
	void *aabbs = nullptr;
	uint32_t aabb_count = 0;
	uint32_t aabb_stride = 0;
};

struct RTGIHIPRTBvhNodeList {
	void *internal_nodes = nullptr;
	void *leaf_nodes = nullptr;
	uint32_t node_count = 0;
};

struct alignas(64) RTGIHIPRTGeometryBuildInput {
	uint32_t type = RTGI_HIPRT_PRIMITIVE_TYPE_TRIANGLE_MESH;
	uint32_t geom_type = UINT32_MAX;
	union {
		RTGIHIPRTTriangleMeshPrimitive triangle_mesh;
		RTGIHIPRTAABBListPrimitive aabb_list;
	} primitive = {};
	RTGIHIPRTBvhNodeList node_list;
};
static_assert(sizeof(RTGIHIPRTGeometryBuildInput) == 128, "RTGIHIPRTGeometryBuildInput must match HIP RT ABI size");

struct alignas(16) RTGIHIPRTInstance {
	uint32_t type = RTGI_HIPRT_INSTANCE_TYPE_GEOMETRY;
	uint32_t _pad = 0;
	void *geometry = nullptr;
};
static_assert(sizeof(RTGIHIPRTInstance) == 16, "RTGIHIPRTInstance must match HIP RT ABI size");

struct alignas(64) RTGIHIPRTFrameMatrix {
	float matrix[3][4] = {};
	float time = 0.0f;
};
static_assert(sizeof(RTGIHIPRTFrameMatrix) == 64, "RTGIHIPRTFrameMatrix must match HIP RT ABI size");

struct alignas(16) RTGIHIPRTSceneBuildInput {
	void *instances = nullptr;
	void *instance_transform_headers = nullptr;
	void *instance_frames = nullptr;
	void *instance_masks = nullptr;
	RTGIHIPRTBvhNodeList node_list;
	uint32_t instance_count = 0;
	uint32_t frame_count = 0;
	uint32_t frame_type = RTGI_HIPRT_FRAME_TYPE_SRT;
};
static_assert(sizeof(RTGIHIPRTSceneBuildInput) == 80, "RTGIHIPRTSceneBuildInput must match HIP RT ABI size");

struct RTGIHIPDeviceAllocation {
	void *ptr = nullptr;
	size_t size = 0;
};

struct RTGIHIPKernelVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct RTGIHIPKernelCamera {
	RTGIHIPKernelVec3 origin;
	uint32_t orthogonal = 0;
	RTGIHIPKernelVec3 forward;
	float z_far = 10000.0f;
	RTGIHIPKernelVec3 near_top_left;
	float _pad0 = 0.0f;
	RTGIHIPKernelVec3 near_top_right;
	float _pad1 = 0.0f;
	RTGIHIPKernelVec3 near_bottom_left;
	float _pad2 = 0.0f;
	RTGIHIPKernelVec3 near_bottom_right;
	float _pad3 = 0.0f;
	RTGIHIPKernelVec3 far_top_left;
	float _pad4 = 0.0f;
	RTGIHIPKernelVec3 far_top_right;
	float _pad5 = 0.0f;
	RTGIHIPKernelVec3 far_bottom_left;
	float _pad6 = 0.0f;
	RTGIHIPKernelVec3 far_bottom_right;
	float _pad7 = 0.0f;
};

struct RTGIHIPKernelMaterial {
	float albedo[4] = {};
	float emission[3] = {};
	float emission_strength = 0.0f;
};

enum RTGIHIPExternalMemoryHandleType {
	RTGI_HIP_EXTERNAL_MEMORY_HANDLE_OPAQUE_FD = 1,
	RTGI_HIP_EXTERNAL_MEMORY_HANDLE_OPAQUE_WIN32 = 2,
};

enum RTGIHIPExternalSemaphoreHandleType {
	RTGI_HIP_EXTERNAL_SEMAPHORE_HANDLE_OPAQUE_FD = 1,
	RTGI_HIP_EXTERNAL_SEMAPHORE_HANDLE_OPAQUE_WIN32 = 2,
};

enum RTGIHIPChannelFormatKind {
	RTGI_HIP_CHANNEL_FORMAT_KIND_FLOAT = 2,
};

enum RTGIHIPMemcpyKind {
	RTGI_HIP_MEMCPY_HOST_TO_DEVICE = 1,
	RTGI_HIP_MEMCPY_DEVICE_TO_DEVICE = 3,
};

struct RTGIHIPChannelFormatDesc {
	int x = 0;
	int y = 0;
	int z = 0;
	int w = 0;
	uint32_t f = RTGI_HIP_CHANNEL_FORMAT_KIND_FLOAT;
};

struct RTGIHIPExtent {
	size_t width = 0;
	size_t height = 0;
	size_t depth = 0;
};

struct RTGIHIPExternalMemoryHandleDesc {
	uint32_t type = 0;
	union {
		int fd;
		struct {
			void *handle;
			const void *name;
		} win32;
		const void *nv_sci_buf_object;
	} handle = {};
	uint64_t size = 0;
	uint32_t flags = 0;
	uint32_t reserved[16] = {};
};

struct RTGIHIPExternalMemoryMipmappedArrayDesc {
	uint64_t offset = 0;
	RTGIHIPChannelFormatDesc format_desc;
	RTGIHIPExtent extent;
	uint32_t flags = 0;
	uint32_t num_levels = 0;
};

struct RTGIHIPExternalSemaphoreHandleDesc {
	uint32_t type = 0;
	union {
		int fd;
		struct {
			void *handle;
			const void *name;
		} win32;
		const void *nv_sci_sync_obj;
	} handle = {};
	uint32_t flags = 0;
	uint32_t reserved[16] = {};
};

struct RTGIHIPExternalSemaphoreSignalParams {
	struct {
		struct {
			uint64_t value = 0;
		} fence;
		union {
			void *fence;
			uint64_t reserved;
		} nv_sci_sync = {};
		struct {
			uint64_t key = 0;
		} keyed_mutex;
		uint32_t reserved[12] = {};
	} params;
	uint32_t flags = 0;
	uint32_t reserved[16] = {};
};

struct RTGIHIPExternalSemaphoreWaitParams {
	struct {
		struct {
			uint64_t value = 0;
		} fence;
		union {
			void *fence;
			uint64_t reserved;
		} nv_sci_sync = {};
		struct {
			uint64_t key = 0;
			uint32_t timeout_ms = 0;
		} keyed_mutex;
		uint32_t reserved[10] = {};
	} params;
	uint32_t flags = 0;
	uint32_t reserved[16] = {};
};

static uint32_t _rtgi_hip_external_memory_handle_type(RTGIBackendExternalHandleType p_handle_type) {
	switch (p_handle_type) {
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD:
			return RTGI_HIP_EXTERNAL_MEMORY_HANDLE_OPAQUE_FD;
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32:
			return RTGI_HIP_EXTERNAL_MEMORY_HANDLE_OPAQUE_WIN32;
		default:
			return 0;
	}
}

static uint32_t _rtgi_hip_external_semaphore_handle_type(RTGIBackendExternalHandleType p_handle_type) {
	switch (p_handle_type) {
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD:
			return RTGI_HIP_EXTERNAL_SEMAPHORE_HANDLE_OPAQUE_FD;
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32:
			return RTGI_HIP_EXTERNAL_SEMAPHORE_HANDLE_OPAQUE_WIN32;
		default:
			return 0;
	}
}

struct RTGIHIPDispatch {
#if defined(WINDOWS_ENABLED)
	HMODULE hip_library_handle = nullptr;
	HMODULE hiprt_library_handle = nullptr;
#elif defined(LINUXBSD_ENABLED)
	void *hip_library_handle = nullptr;
	void *hiprt_library_handle = nullptr;
#endif
	String hip_library_path;
	String hiprt_library_path;
	bool loaded = false;

	int(RTGI_HIP_CALL *hipInit)(unsigned int) = nullptr;
	int(RTGI_HIP_CALL *hipSetDevice)(int) = nullptr;
	int(RTGI_HIP_CALL *hipGetDevice)(int *) = nullptr;
	int(RTGI_HIP_CALL *hipCtxGetCurrent)(void **) = nullptr;
	int(RTGI_HIP_CALL *hipDeviceSynchronize)() = nullptr;
	int(RTGI_HIP_CALL *hipMalloc)(void **, size_t) = nullptr;
	int(RTGI_HIP_CALL *hipFree)(void *) = nullptr;
	int(RTGI_HIP_CALL *hipMemcpy)(void *, const void *, size_t, int) = nullptr;
	int(RTGI_HIP_CALL *hipMemcpy2DToArray)(void *, size_t, size_t, const void *, size_t, size_t, size_t, int) = nullptr;
	int(RTGI_HIP_CALL *hipGetMipmappedArrayLevel)(void **, void *, unsigned int) = nullptr;
	int(RTGI_HIP_CALL *hipModuleLaunchKernel)(void *, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, void *, void **, void **) = nullptr;
	int(RTGI_HIP_CALL *hipImportExternalMemory)(void **, const RTGIHIPExternalMemoryHandleDesc *) = nullptr;
	int(RTGI_HIP_CALL *hipExternalMemoryGetMappedMipmappedArray)(void **, void *, const RTGIHIPExternalMemoryMipmappedArrayDesc *) = nullptr;
	int(RTGI_HIP_CALL *hipDestroyExternalMemory)(void *) = nullptr;
	int(RTGI_HIP_CALL *hipFreeMipmappedArray)(void *) = nullptr;
	int(RTGI_HIP_CALL *hipImportExternalSemaphore)(void **, const RTGIHIPExternalSemaphoreHandleDesc *) = nullptr;
	int(RTGI_HIP_CALL *hipWaitExternalSemaphoresAsync)(void *const *, const RTGIHIPExternalSemaphoreWaitParams *, unsigned int, void *) = nullptr;
	int(RTGI_HIP_CALL *hipSignalExternalSemaphoresAsync)(void *const *, const RTGIHIPExternalSemaphoreSignalParams *, unsigned int, void *) = nullptr;
	int(RTGI_HIP_CALL *hipDestroyExternalSemaphore)(void *) = nullptr;

	int(RTGI_HIPRT_CALL *hiprtCreateContext)(uint32_t, RTGIHIPRTContextCreationInput &, void *&) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtDestroyContext)(void *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtSetLogLevel)(void *, uint32_t) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtCreateGeometry)(void *, const RTGIHIPRTGeometryBuildInput &, RTGIHIPRTBuildOptions, void *&) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtDestroyGeometry)(void *, void *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtGetGeometryBuildTemporaryBufferSize)(void *, const RTGIHIPRTGeometryBuildInput &, RTGIHIPRTBuildOptions, size_t &) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtBuildGeometry)(void *, int, const RTGIHIPRTGeometryBuildInput &, RTGIHIPRTBuildOptions, void *, void *, void *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtCreateScene)(void *, const RTGIHIPRTSceneBuildInput &, RTGIHIPRTBuildOptions, void *&) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtDestroyScene)(void *, void *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtGetSceneBuildTemporaryBufferSize)(void *, const RTGIHIPRTSceneBuildInput &, RTGIHIPRTBuildOptions, size_t *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtBuildScene)(void *, int, const RTGIHIPRTSceneBuildInput *, RTGIHIPRTBuildOptions, void *, void *, void *) = nullptr;
	int(RTGI_HIPRT_CALL *hiprtBuildTraceKernels)(void *, uint32_t, const char **, const char *, const char *, uint32_t, const char **, const char **, uint32_t, const char **, uint32_t, uint32_t, void *, void **, void **, bool) = nullptr;

	template <typename T>
	bool _resolve_symbol(void *p_library_handle, T &r_symbol, const char *p_name, const String &p_library_path, String *r_failure_reason) {
#if defined(WINDOWS_ENABLED)
		void *symbol = reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(p_library_handle), p_name));
#elif defined(LINUXBSD_ENABLED)
		void *symbol = dlsym(p_library_handle, p_name);
#else
		void *symbol = nullptr;
#endif
		if (symbol == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = vformat("HIP RT backend runtime library '%s' is missing required symbol '%s'.", p_library_path, p_name);
			}
			return false;
		}
		r_symbol = reinterpret_cast<T>(symbol);
		return true;
	}

	bool _open_library(const String &p_path, bool p_hiprt, String *r_failure_reason) {
#if defined(WINDOWS_ENABLED)
		const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
		const DWORD load_flags = p_path.is_absolute_path() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
		HMODULE handle = LoadLibraryExW((LPCWSTR)(p_path.utf16().get_data()), nullptr, load_flags);
		SetErrorMode(previous_error_mode);
		if (handle == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = vformat("HIP RT backend found runtime library '%s' but could not load it.", p_path);
			}
			return false;
		}
		if (p_hiprt) {
			hiprt_library_handle = handle;
			hiprt_library_path = p_path;
		} else {
			hip_library_handle = handle;
			hip_library_path = p_path;
		}
		return true;
#elif defined(LINUXBSD_ENABLED)
		void *handle = dlopen(p_path.utf8().get_data(), RTLD_LAZY | RTLD_LOCAL);
		if (handle == nullptr) {
			if (r_failure_reason) {
				*r_failure_reason = vformat("HIP RT backend found runtime library '%s' but could not load it.", p_path);
			}
			return false;
		}
		if (p_hiprt) {
			hiprt_library_handle = handle;
			hiprt_library_path = p_path;
		} else {
			hip_library_handle = handle;
			hip_library_path = p_path;
		}
		return true;
#else
		if (r_failure_reason) {
			*r_failure_reason = "HIP RT backend runtime loading is unsupported on this platform.";
		}
		return false;
#endif
	}

	void unload() {
#if defined(WINDOWS_ENABLED)
		if (hiprt_library_handle != nullptr) {
			FreeLibrary(hiprt_library_handle);
		}
		if (hip_library_handle != nullptr) {
			FreeLibrary(hip_library_handle);
		}
#elif defined(LINUXBSD_ENABLED)
		if (hiprt_library_handle != nullptr) {
			dlclose(hiprt_library_handle);
		}
		if (hip_library_handle != nullptr) {
			dlclose(hip_library_handle);
		}
#endif
		*this = RTGIHIPDispatch();
	}

	bool load(String *r_failure_reason) {
		if (loaded) {
			return true;
		}

		static const char *const hip_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
			"amdhip64_7.dll",
			"amdhip64_6.dll",
			"amdhip64.dll",
#else
			"libamdhip64.so",
			"libamdhip64.so.7",
			"libamdhip64.so.6",
			"libamdhip64.so.5",
#endif
		};
		static const char *const hip_runtime_symbols[] = {
			"hipInit",
			"hipSetDevice",
			"hipGetDevice",
			"hipCtxGetCurrent",
			"hipDeviceSynchronize",
			"hipMalloc",
			"hipFree",
			"hipMemcpy",
			"hipMemcpy2DToArray",
			"hipGetMipmappedArrayLevel",
			"hipModuleLaunchKernel",
			"hipImportExternalMemory",
			"hipExternalMemoryGetMappedMipmappedArray",
			"hipFreeMipmappedArray",
			"hipDestroyExternalMemory",
			"hipImportExternalSemaphore",
			"hipWaitExternalSemaphoresAsync",
			"hipSignalExternalSemaphoresAsync",
			"hipDestroyExternalSemaphore",
		};
		static const char *const hiprt_runtime_libraries[] = {
#if defined(WINDOWS_ENABLED)
			"hiprt0300064.dll",
			"hiprt0200564.dll",
			"hiprt0200064.dll",
			"hiprt64.dll",
			"hiprt.dll",
#else
			"libhiprt64.so",
			"libhiprt64.so.3",
			"libhiprt64.so.2.5",
			"libhiprt64.so.2",
			"libhiprt.so",
#endif
		};
		static const char *const hiprt_runtime_symbols[] = {
			"hiprtCreateContext",
			"hiprtDestroyContext",
			"hiprtCreateGeometry",
			"hiprtDestroyGeometry",
			"hiprtGetGeometryBuildTemporaryBufferSize",
			"hiprtBuildGeometry",
			"hiprtCreateScene",
			"hiprtDestroyScene",
			"hiprtGetSceneBuildTemporaryBufferSize",
			"hiprtBuildScene",
			"hiprtBuildTraceKernels",
		};
		static const char *const hiprt_root_env_vars[] = {
			"HIPRT_PATH",
			"HIP_PATH",
			"ROCM_PATH",
		};

		String found_hip_library;
		String found_hiprt_library;
		if (!_rtgi_find_runtime_library(hip_runtime_libraries, sizeof(hip_runtime_libraries) / sizeof(hip_runtime_libraries[0]), hip_runtime_symbols, sizeof(hip_runtime_symbols) / sizeof(hip_runtime_symbols[0]), hiprt_root_env_vars, sizeof(hiprt_root_env_vars) / sizeof(hiprt_root_env_vars[0]), &found_hip_library) ||
				!_rtgi_find_runtime_library(hiprt_runtime_libraries, sizeof(hiprt_runtime_libraries) / sizeof(hiprt_runtime_libraries[0]), hiprt_runtime_symbols, sizeof(hiprt_runtime_symbols) / sizeof(hiprt_runtime_symbols[0]), hiprt_root_env_vars, sizeof(hiprt_root_env_vars) / sizeof(hiprt_root_env_vars[0]), &found_hiprt_library)) {
			if (r_failure_reason) {
				*r_failure_reason = "HIP RT backend could not find HIP and HIP RT runtime libraries with the required symbols.";
			}
			return false;
		}
		if (!_open_library(found_hip_library, false, r_failure_reason) || !_open_library(found_hiprt_library, true, r_failure_reason)) {
			unload();
			return false;
		}

		if (!_resolve_symbol(hip_library_handle, hipInit, "hipInit", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipSetDevice, "hipSetDevice", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipGetDevice, "hipGetDevice", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipCtxGetCurrent, "hipCtxGetCurrent", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipDeviceSynchronize, "hipDeviceSynchronize", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipMalloc, "hipMalloc", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipFree, "hipFree", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipMemcpy, "hipMemcpy", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipMemcpy2DToArray, "hipMemcpy2DToArray", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipGetMipmappedArrayLevel, "hipGetMipmappedArrayLevel", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipModuleLaunchKernel, "hipModuleLaunchKernel", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipImportExternalMemory, "hipImportExternalMemory", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipExternalMemoryGetMappedMipmappedArray, "hipExternalMemoryGetMappedMipmappedArray", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipFreeMipmappedArray, "hipFreeMipmappedArray", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipDestroyExternalMemory, "hipDestroyExternalMemory", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipImportExternalSemaphore, "hipImportExternalSemaphore", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipWaitExternalSemaphoresAsync, "hipWaitExternalSemaphoresAsync", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipSignalExternalSemaphoresAsync, "hipSignalExternalSemaphoresAsync", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hip_library_handle, hipDestroyExternalSemaphore, "hipDestroyExternalSemaphore", hip_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtCreateContext, "hiprtCreateContext", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtDestroyContext, "hiprtDestroyContext", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtCreateGeometry, "hiprtCreateGeometry", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtDestroyGeometry, "hiprtDestroyGeometry", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtGetGeometryBuildTemporaryBufferSize, "hiprtGetGeometryBuildTemporaryBufferSize", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtBuildGeometry, "hiprtBuildGeometry", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtCreateScene, "hiprtCreateScene", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtDestroyScene, "hiprtDestroyScene", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtGetSceneBuildTemporaryBufferSize, "hiprtGetSceneBuildTemporaryBufferSize", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtBuildScene, "hiprtBuildScene", hiprt_library_path, r_failure_reason) ||
				!_resolve_symbol(hiprt_library_handle, hiprtBuildTraceKernels, "hiprtBuildTraceKernels", hiprt_library_path, r_failure_reason)) {
			unload();
			return false;
		}
		_resolve_symbol(hiprt_library_handle, hiprtSetLogLevel, "hiprtSetLogLevel", hiprt_library_path, nullptr);

		loaded = true;
		return true;
	}
};

#if defined(MODULE_HIPRT_ENABLED)
class AmdHIPRTRTGIBackend : public RTGIBackend {
	RenderForwardClustered *owner = nullptr;
	RenderRaytracing *raytracing = nullptr;
	RTGIHIPDispatch hip;
	void *hiprt_context = nullptr;
	void *hip_context = nullptr;
	void *hiprt_scene = nullptr;
	void *hip_external_memory = nullptr;
	void *hip_output_mipmapped_array = nullptr;
	void *hip_wait_semaphore = nullptr;
	void *hip_signal_semaphore = nullptr;
	int hip_device = 0;
	void *hip_trace_function = nullptr;
	void *hip_materials_device_ptr = nullptr;
	uint32_t hip_material_count = 0;
	LocalVector<void *> hiprt_geometries;
	LocalVector<RTGIHIPDeviceAllocation> hip_allocations;

	static RTGIHIPRTFrameMatrix _make_identity_frame() {
		RTGIHIPRTFrameMatrix frame;
		frame.matrix[0][0] = 1.0f;
		frame.matrix[1][1] = 1.0f;
		frame.matrix[2][2] = 1.0f;
		frame.time = 0.0f;
		return frame;
	}

	static RTGIHIPKernelVec3 _make_kernel_vec3(const Vector3 &p_vector) {
		RTGIHIPKernelVec3 out;
		out.x = float(p_vector.x);
		out.y = float(p_vector.y);
		out.z = float(p_vector.z);
		return out;
	}

	static RTGIHIPKernelMaterial _make_kernel_material(const RT_MaterialData &p_material) {
		RTGIHIPKernelMaterial out;
		out.albedo[0] = p_material.albedo_color[0];
		out.albedo[1] = p_material.albedo_color[1];
		out.albedo[2] = p_material.albedo_color[2];
		out.albedo[3] = p_material.albedo_color[3];
		out.emission[0] = p_material.emission_color[0];
		out.emission[1] = p_material.emission_color[1];
		out.emission[2] = p_material.emission_color[2];
		out.emission_strength = p_material.emission_strength;
		return out;
	}

	static void _append_existing_hiprt_include_dir(LocalVector<String> &r_include_dirs, const String &p_dir) {
		if (p_dir.is_empty()) {
			return;
		}
		const String normalized_dir = p_dir.replace("\\", "/");
		static const char *const required_device_headers[] = {
			"hiprt/hiprt_common.h",
			"hiprt/hiprt_device.h",
			"hiprt/hiprt_math.h",
			"hiprt/hiprt_types.h",
			"hiprt/hiprt_vec.h",
		};
		for (uint32_t i = 0; i < sizeof(required_device_headers) / sizeof(required_device_headers[0]); i++) {
			if (!FileAccess::exists(normalized_dir.path_join(required_device_headers[i]))) {
				return;
			}
		}
		for (uint32_t i = 0; i < r_include_dirs.size(); i++) {
			if (r_include_dirs[i] == normalized_dir) {
				return;
			}
		}
		r_include_dirs.push_back(normalized_dir);
	}

	static LocalVector<String> _find_hiprt_include_dirs() {
		LocalVector<String> include_dirs;
		_append_existing_hiprt_include_dir(include_dirs, String(RTGI_HIPRT_SDK_ROOT));
		_append_existing_hiprt_include_dir(include_dirs, String(RTGI_HIPRT_SDK_ROOT).path_join("include"));

		OS *os = OS::get_singleton();
		if (os != nullptr) {
			static const char *const root_env_vars[] = {
				"HIPRT_PATH",
				"HIP_PATH",
				"ROCM_PATH",
			};
			for (uint32_t i = 0; i < sizeof(root_env_vars) / sizeof(root_env_vars[0]); i++) {
				const String root = os->get_environment(root_env_vars[i]);
				_append_existing_hiprt_include_dir(include_dirs, root);
				_append_existing_hiprt_include_dir(include_dirs, root.path_join("include"));
			}
		}
		return include_dirs;
	}

	static bool _make_kernel_camera(const RTGIBackendFrameContext &p_context, RTGIHIPKernelCamera &r_camera, String *r_fallback_reason) {
		if (p_context.render_data == nullptr || p_context.render_data->scene_data == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT dispatch is missing RenderSceneDataRD camera data.";
			}
			return false;
		}

		const Transform3D &camera_transform = p_context.render_data->scene_data->cam_transform;
		const Projection inv_projection = p_context.render_data->scene_data->cam_projection.inverse();
		const float z_far = Math::is_finite(p_context.render_data->scene_data->z_far) && p_context.render_data->scene_data->z_far > 0.0f ? p_context.render_data->scene_data->z_far : 10000.0f;

		auto unproject = [&](float p_ndc_x, float p_ndc_y, float p_depth) -> Vector3 {
			return camera_transform.xform(inv_projection.xform(Vector3(p_ndc_x, p_ndc_y, p_depth)));
		};

		r_camera = RTGIHIPKernelCamera();
		r_camera.origin = _make_kernel_vec3(camera_transform.origin);
		r_camera.orthogonal = p_context.render_data->scene_data->cam_orthogonal ? 1u : 0u;
		r_camera.forward = _make_kernel_vec3((-camera_transform.basis.get_column(Vector3::AXIS_Z)).normalized());
		r_camera.z_far = z_far;
		r_camera.near_top_left = _make_kernel_vec3(unproject(-1.0f, 1.0f, 0.0f));
		r_camera.near_top_right = _make_kernel_vec3(unproject(1.0f, 1.0f, 0.0f));
		r_camera.near_bottom_left = _make_kernel_vec3(unproject(-1.0f, -1.0f, 0.0f));
		r_camera.near_bottom_right = _make_kernel_vec3(unproject(1.0f, -1.0f, 0.0f));
		r_camera.far_top_left = _make_kernel_vec3(unproject(-1.0f, 1.0f, 1.0f));
		r_camera.far_top_right = _make_kernel_vec3(unproject(1.0f, 1.0f, 1.0f));
		r_camera.far_bottom_left = _make_kernel_vec3(unproject(-1.0f, -1.0f, 1.0f));
		r_camera.far_bottom_right = _make_kernel_vec3(unproject(1.0f, -1.0f, 1.0f));
		return true;
	}

	bool _hip_allocate(size_t p_size, RTGIHIPDeviceAllocation &r_allocation, String *r_fallback_reason, const char *p_label) {
		r_allocation = RTGIHIPDeviceAllocation();
		if (p_size == 0) {
			return true;
		}
		void *device_ptr = nullptr;
		const int hip_error = hip.hipMalloc(&device_ptr, p_size);
		if (hip_error != 0 || device_ptr == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to allocate %d bytes for %s; hipMalloc returned %d.", uint64_t(p_size), p_label, hip_error);
			}
			return false;
		}
		r_allocation.ptr = device_ptr;
		r_allocation.size = p_size;
		return true;
	}

	void _hip_free_allocation(RTGIHIPDeviceAllocation &r_allocation) {
		if (r_allocation.ptr != nullptr && hip.hipFree != nullptr) {
			hip.hipFree(r_allocation.ptr);
		}
		r_allocation = RTGIHIPDeviceAllocation();
	}

	bool _hip_upload_buffer(const void *p_source, size_t p_size, LocalVector<RTGIHIPDeviceAllocation> &r_allocations, void **r_device_ptr, String *r_fallback_reason, const char *p_label) {
		*r_device_ptr = nullptr;
		if (p_size == 0) {
			return true;
		}
		if (p_source == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend cannot upload %s because the source pointer is null.", p_label);
			}
			return false;
		}
		RTGIHIPDeviceAllocation allocation;
		if (!_hip_allocate(p_size, allocation, r_fallback_reason, p_label)) {
			return false;
		}
		const int hip_error = hip.hipMemcpy(allocation.ptr, p_source, p_size, RTGI_HIP_MEMCPY_HOST_TO_DEVICE);
		if (hip_error != 0) {
			_hip_free_allocation(allocation);
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to upload %d bytes for %s; hipMemcpy returned %d.", uint64_t(p_size), p_label, hip_error);
			}
			return false;
		}
		*r_device_ptr = allocation.ptr;
		r_allocations.push_back(allocation);
		return true;
	}

	void _release_scene() {
		if (hip.hipDeviceSynchronize != nullptr) {
			hip.hipDeviceSynchronize();
		}
		if (hiprt_scene != nullptr && hiprt_context != nullptr && hip.hiprtDestroyScene != nullptr) {
			hip.hiprtDestroyScene(hiprt_context, hiprt_scene);
			hiprt_scene = nullptr;
		}
		if (hiprt_context != nullptr && hip.hiprtDestroyGeometry != nullptr) {
			for (uint32_t i = 0; i < hiprt_geometries.size(); i++) {
				if (hiprt_geometries[i] != nullptr) {
					hip.hiprtDestroyGeometry(hiprt_context, hiprt_geometries[i]);
				}
			}
		}
		hiprt_geometries.clear();
		hip_materials_device_ptr = nullptr;
		hip_material_count = 0;
		for (uint32_t i = 0; i < hip_allocations.size(); i++) {
			_hip_free_allocation(hip_allocations[i]);
		}
		hip_allocations.clear();
	}

	void _release_external_imports() {
		if (hip.hipDeviceSynchronize != nullptr) {
			hip.hipDeviceSynchronize();
		}
		if (hip_output_mipmapped_array != nullptr && hip.hipFreeMipmappedArray != nullptr) {
			hip.hipFreeMipmappedArray(hip_output_mipmapped_array);
			hip_output_mipmapped_array = nullptr;
		}
		if (hip_external_memory != nullptr && hip.hipDestroyExternalMemory != nullptr) {
			hip.hipDestroyExternalMemory(hip_external_memory);
			hip_external_memory = nullptr;
		}
		if (hip_wait_semaphore != nullptr && hip.hipDestroyExternalSemaphore != nullptr) {
			hip.hipDestroyExternalSemaphore(hip_wait_semaphore);
			hip_wait_semaphore = nullptr;
		}
		if (hip_signal_semaphore != nullptr && hip.hipDestroyExternalSemaphore != nullptr) {
			hip.hipDestroyExternalSemaphore(hip_signal_semaphore);
			hip_signal_semaphore = nullptr;
		}
	}

	bool _populate_hip_memory_handle_desc(uint64_t p_handle, RTGIBackendExternalHandleType p_handle_type, uint64_t p_size, RTGIHIPExternalMemoryHandleDesc &r_desc, String *r_fallback_reason) const {
		r_desc = RTGIHIPExternalMemoryHandleDesc();
		r_desc.type = _rtgi_hip_external_memory_handle_type(p_handle_type);
		if (r_desc.type == 0 || p_handle == 0 || p_size == 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend received an invalid external memory handle from RenderingDevice.";
			}
			return false;
		}
		r_desc.size = p_size;
#if defined(WINDOWS_ENABLED)
		if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32) {
			r_desc.handle.win32.handle = reinterpret_cast<void *>(uintptr_t(p_handle));
			return true;
		}
#else
		if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD) {
			r_desc.handle.fd = int(p_handle);
			return true;
		}
#endif
		if (r_fallback_reason) {
			*r_fallback_reason = "HIP RT backend cannot import a RenderingDevice external memory handle for this platform.";
		}
		return false;
	}

	bool _populate_hip_semaphore_handle_desc(uint64_t p_handle, RTGIBackendExternalHandleType p_handle_type, RTGIHIPExternalSemaphoreHandleDesc &r_desc, String *r_fallback_reason) const {
		r_desc = RTGIHIPExternalSemaphoreHandleDesc();
		r_desc.type = _rtgi_hip_external_semaphore_handle_type(p_handle_type);
		if (r_desc.type == 0 || p_handle == 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend received an invalid external semaphore handle from RenderingDevice.";
			}
			return false;
		}
#if defined(WINDOWS_ENABLED)
		if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32) {
			r_desc.handle.win32.handle = reinterpret_cast<void *>(uintptr_t(p_handle));
			return true;
		}
#else
		if (p_handle_type == RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD) {
			r_desc.handle.fd = int(p_handle);
			return true;
		}
#endif
		if (r_fallback_reason) {
			*r_fallback_reason = "HIP RT backend cannot import a RenderingDevice external semaphore handle for this platform.";
		}
		return false;
	}

	bool _import_external_exchange(RTGIBackendFrameContext &r_context, String *r_fallback_reason) {
		if (!_ensure_context(r_fallback_reason)) {
			return false;
		}
		RTGIBackendResourceExchange &exchange = r_context.exchange;
		if (exchange.mode != RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend requires external memory and semaphore exchange from RenderingDevice.";
			}
			return false;
		}
		_release_external_imports();

		RTGIHIPExternalMemoryHandleDesc memory_desc;
		if (!_populate_hip_memory_handle_desc(exchange.external_memory_handle, exchange.external_memory_handle_type, exchange.external_memory_allocation_size, memory_desc, r_fallback_reason)) {
			return false;
		}

		int hip_error = hip.hipImportExternalMemory(&hip_external_memory, &memory_desc);
		if (hip_error != 0 || hip_external_memory == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hipImportExternalMemory with error %d.", hip_error);
			}
			return false;
		}
		exchange.external_memory_handle = 0;

		RTGIHIPExternalMemoryMipmappedArrayDesc mipmap_desc;
		mipmap_desc.offset = exchange.external_memory_allocation_offset;
		mipmap_desc.format_desc.x = 16;
		mipmap_desc.format_desc.y = 16;
		mipmap_desc.format_desc.z = 16;
		mipmap_desc.format_desc.w = 16;
		mipmap_desc.format_desc.f = RTGI_HIP_CHANNEL_FORMAT_KIND_FLOAT;
		mipmap_desc.extent.width = MAX(1, r_context.output_size.width);
		mipmap_desc.extent.height = MAX(1, r_context.output_size.height);
		mipmap_desc.extent.depth = 1;
		mipmap_desc.num_levels = 1;
		hip_error = hip.hipExternalMemoryGetMappedMipmappedArray(&hip_output_mipmapped_array, hip_external_memory, &mipmap_desc);
		if (hip_error != 0 || hip_output_mipmapped_array == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend imported output memory but failed hipExternalMemoryGetMappedMipmappedArray with error %d.", hip_error);
			}
			return false;
		}

		RTGIHIPExternalSemaphoreHandleDesc wait_desc;
		if (!_populate_hip_semaphore_handle_desc(exchange.external_wait_semaphore_handle, exchange.external_wait_semaphore_handle_type, wait_desc, r_fallback_reason)) {
			return false;
		}
		hip_error = hip.hipImportExternalSemaphore(&hip_wait_semaphore, &wait_desc);
		if (hip_error != 0 || hip_wait_semaphore == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to import the RenderingDevice wait semaphore with error %d.", hip_error);
			}
			return false;
		}
		exchange.external_wait_semaphore_handle = 0;

		RTGIHIPExternalSemaphoreHandleDesc signal_desc;
		if (!_populate_hip_semaphore_handle_desc(exchange.external_signal_semaphore_handle, exchange.external_signal_semaphore_handle_type, signal_desc, r_fallback_reason)) {
			return false;
		}
		hip_error = hip.hipImportExternalSemaphore(&hip_signal_semaphore, &signal_desc);
		if (hip_error != 0 || hip_signal_semaphore == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to import the RenderingDevice signal semaphore with error %d.", hip_error);
			}
			return false;
		}
		exchange.external_signal_semaphore_handle = 0;
		return true;
	}

	bool _build_scene_from_snapshot(const RTGIBackendSceneSnapshot &p_snapshot, String *r_fallback_reason) {
		if (!_ensure_context(r_fallback_reason)) {
			return false;
		}
		if (p_snapshot.geometries.size() != p_snapshot.cpu_geometries.size() ||
				p_snapshot.geometries.size() != p_snapshot.blas_transforms.size() ||
				p_snapshot.geometries.size() != p_snapshot.instance_masks.size()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT scene snapshot arrays are not parallel.";
			}
			return false;
		}

		_release_scene();

		LocalVector<RTGIHIPRTInstance> scene_instances;
		LocalVector<RTGIHIPRTFrameMatrix> scene_frames;
		LocalVector<uint32_t> scene_masks;
		LocalVector<RTGIHIPKernelMaterial> scene_materials;
		const RTGIHIPRTBuildOptions build_options;

		for (uint32_t geometry_index = 0; geometry_index < p_snapshot.geometries.size(); geometry_index++) {
			if (p_snapshot.instance_masks[geometry_index] == 0) {
				continue;
			}

			const RTGIBackendCPUGeometry &cpu_geometry = p_snapshot.cpu_geometries[geometry_index];
			if (!cpu_geometry.valid || cpu_geometry.vertices.size() == 0 || cpu_geometry.primitive_count == 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = "HIP RT backend requires CPU triangle geometry in the scene snapshot; procedural, deformed, and GPU-merged surfaces still fall back to Vulkan Generic.";
				}
				_release_scene();
				return false;
			}

			LocalVector<RTGIHIPRTFloat3> vertices;
			vertices.resize(cpu_geometry.vertices.size());
			const Transform3D &transform = p_snapshot.blas_transforms[geometry_index];
			for (uint32_t vertex_index = 0; vertex_index < cpu_geometry.vertices.size(); vertex_index++) {
				const Vector3 transformed = transform.xform(cpu_geometry.vertices[vertex_index]);
				vertices[vertex_index].x = (float)transformed.x;
				vertices[vertex_index].y = (float)transformed.y;
				vertices[vertex_index].z = (float)transformed.z;
			}

			void *vertex_device_ptr = nullptr;
			void *index_device_ptr = nullptr;
			if (!_hip_upload_buffer(vertices.ptr(), vertices.size() * sizeof(RTGIHIPRTFloat3), hip_allocations, &vertex_device_ptr, r_fallback_reason, "triangle vertices") ||
					!_hip_upload_buffer(cpu_geometry.indices.ptr(), cpu_geometry.indices.size() * sizeof(uint32_t), hip_allocations, &index_device_ptr, r_fallback_reason, "triangle indices")) {
				_release_scene();
				return false;
			}

			RTGIHIPRTGeometryBuildInput geometry_input;
			geometry_input.type = RTGI_HIPRT_PRIMITIVE_TYPE_TRIANGLE_MESH;
			geometry_input.geom_type = UINT32_MAX;
			geometry_input.primitive.triangle_mesh.vertices = vertex_device_ptr;
			geometry_input.primitive.triangle_mesh.vertex_count = cpu_geometry.vertices.size();
			geometry_input.primitive.triangle_mesh.vertex_stride = sizeof(RTGIHIPRTFloat3);
			geometry_input.primitive.triangle_mesh.triangle_indices = index_device_ptr;
			geometry_input.primitive.triangle_mesh.triangle_count = cpu_geometry.primitive_count;
			geometry_input.primitive.triangle_mesh.triangle_stride = sizeof(uint32_t) * 3;

			void *geometry_handle = nullptr;
			int hiprt_error = hip.hiprtCreateGeometry(hiprt_context, geometry_input, build_options, geometry_handle);
			if (hiprt_error != 0 || geometry_handle == nullptr) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed hiprtCreateGeometry for snapshot geometry %d with error %d.", geometry_index, hiprt_error);
				}
				_release_scene();
				return false;
			}
			hiprt_geometries.push_back(geometry_handle);

			size_t geometry_temp_size = 0;
			hiprt_error = hip.hiprtGetGeometryBuildTemporaryBufferSize(hiprt_context, geometry_input, build_options, geometry_temp_size);
			if (hiprt_error != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed hiprtGetGeometryBuildTemporaryBufferSize for snapshot geometry %d with error %d.", geometry_index, hiprt_error);
				}
				_release_scene();
				return false;
			}

			RTGIHIPDeviceAllocation geometry_temp;
			if (!_hip_allocate(geometry_temp_size, geometry_temp, r_fallback_reason, "HIP RT geometry build scratch")) {
				_release_scene();
				return false;
			}
			hiprt_error = hip.hiprtBuildGeometry(hiprt_context, RTGI_HIPRT_BUILD_OPERATION_BUILD, geometry_input, build_options, geometry_temp.ptr, nullptr, geometry_handle);
			_hip_free_allocation(geometry_temp);
			if (hiprt_error != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed hiprtBuildGeometry for snapshot geometry %d with error %d.", geometry_index, hiprt_error);
				}
				_release_scene();
				return false;
			}

			RTGIHIPRTInstance instance;
			instance.type = RTGI_HIPRT_INSTANCE_TYPE_GEOMETRY;
			instance.geometry = geometry_handle;
			scene_instances.push_back(instance);
			scene_frames.push_back(_make_identity_frame());
			scene_masks.push_back(uint32_t(p_snapshot.instance_masks[geometry_index]));
			if (geometry_index < p_snapshot.materials.size()) {
				scene_materials.push_back(_make_kernel_material(p_snapshot.materials[geometry_index]));
			} else {
				RTGIHIPKernelMaterial fallback_material;
				fallback_material.albedo[0] = 1.0f;
				fallback_material.albedo[1] = 1.0f;
				fallback_material.albedo[2] = 1.0f;
				fallback_material.albedo[3] = 1.0f;
				scene_materials.push_back(fallback_material);
			}
		}

		if (scene_instances.size() == 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend found no CPU-backed triangle instances to build.";
			}
			_release_scene();
			return false;
		}

		void *instances_device_ptr = nullptr;
		void *frames_device_ptr = nullptr;
		void *masks_device_ptr = nullptr;
		void *materials_device_ptr = nullptr;
		if (!_hip_upload_buffer(scene_instances.ptr(), scene_instances.size() * sizeof(RTGIHIPRTInstance), hip_allocations, &instances_device_ptr, r_fallback_reason, "HIP RT scene instances") ||
				!_hip_upload_buffer(scene_frames.ptr(), scene_frames.size() * sizeof(RTGIHIPRTFrameMatrix), hip_allocations, &frames_device_ptr, r_fallback_reason, "HIP RT scene frames") ||
				!_hip_upload_buffer(scene_masks.ptr(), scene_masks.size() * sizeof(uint32_t), hip_allocations, &masks_device_ptr, r_fallback_reason, "HIP RT scene masks") ||
				!_hip_upload_buffer(scene_materials.ptr(), scene_materials.size() * sizeof(RTGIHIPKernelMaterial), hip_allocations, &materials_device_ptr, r_fallback_reason, "HIP RT scene materials")) {
			_release_scene();
			return false;
		}
		hip_materials_device_ptr = materials_device_ptr;
		hip_material_count = scene_materials.size();

		RTGIHIPRTSceneBuildInput scene_input;
		scene_input.instances = instances_device_ptr;
		scene_input.instance_frames = frames_device_ptr;
		scene_input.instance_masks = masks_device_ptr;
		scene_input.instance_count = scene_instances.size();
		scene_input.frame_count = scene_frames.size();
		scene_input.frame_type = RTGI_HIPRT_FRAME_TYPE_MATRIX;

		int hiprt_error = hip.hiprtCreateScene(hiprt_context, scene_input, build_options, hiprt_scene);
		if (hiprt_error != 0 || hiprt_scene == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hiprtCreateScene with error %d.", hiprt_error);
			}
			_release_scene();
			return false;
		}

		size_t scene_temp_size = 0;
		hiprt_error = hip.hiprtGetSceneBuildTemporaryBufferSize(hiprt_context, scene_input, build_options, &scene_temp_size);
		if (hiprt_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hiprtGetSceneBuildTemporaryBufferSize with error %d.", hiprt_error);
			}
			_release_scene();
			return false;
		}

		RTGIHIPDeviceAllocation scene_temp;
		if (!_hip_allocate(scene_temp_size, scene_temp, r_fallback_reason, "HIP RT scene build scratch")) {
			_release_scene();
			return false;
		}
		hiprt_error = hip.hiprtBuildScene(hiprt_context, RTGI_HIPRT_BUILD_OPERATION_BUILD, &scene_input, build_options, scene_temp.ptr, nullptr, hiprt_scene);
		_hip_free_allocation(scene_temp);
		if (hiprt_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hiprtBuildScene with error %d.", hiprt_error);
			}
			_release_scene();
			return false;
		}
		if (hip.hipDeviceSynchronize != nullptr) {
			const int hip_error = hip.hipDeviceSynchronize();
			if (hip_error != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed to synchronize after scene build; hipDeviceSynchronize returned %d.", hip_error);
				}
				_release_scene();
				return false;
			}
		}
		return true;
	}

	bool _ensure_context(String *r_fallback_reason) {
		if (hiprt_context != nullptr) {
			return true;
		}
		if (RTGI_HIPRT_API_VERSION == 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT SDK headers were not configured with a concrete HIPRT_API_VERSION.";
			}
			return false;
		}
		if (!hip.load(r_fallback_reason)) {
			return false;
		}
		int hip_error = hip.hipInit(0);
		if (hip_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hipInit with error %d.", hip_error);
			}
			return false;
		}
		hip_error = hip.hipGetDevice(&hip_device);
		if (hip_error != 0) {
			hip_device = 0;
			hip_error = hip.hipSetDevice(hip_device);
			if (hip_error != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend could not select HIP device 0; hipSetDevice failed with error %d.", hip_error);
				}
				return false;
			}
		}
		hip_error = hip.hipCtxGetCurrent(&hip_context);
		if (hip_error != 0 || hip_context == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend could not obtain the current HIP context; hipCtxGetCurrent failed with error %d.", hip_error);
			}
			return false;
		}

		RTGIHIPRTContextCreationInput input;
		input.ctxt = hip_context;
		input.device = hip_device;
		input.deviceType = 0; // hiprtDeviceAMD.
		void *new_context = nullptr;
		const int hiprt_error = hip.hiprtCreateContext(uint32_t(RTGI_HIPRT_API_VERSION), input, new_context);
		if (hiprt_error != 0 || new_context == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hiprtCreateContext for API version %d with error %d.", int(RTGI_HIPRT_API_VERSION), hiprt_error);
			}
			return false;
		}
		hiprt_context = new_context;
		if (hip.hiprtSetLogLevel != nullptr) {
			hip.hiprtSetLogLevel(hiprt_context, 1u << 2); // hiprtLogLevelError.
		}
		return true;
	}

	bool _ensure_trace_kernel(String *r_fallback_reason) {
		if (hip_trace_function != nullptr) {
			return true;
		}
		if (!_ensure_context(r_fallback_reason)) {
			return false;
		}

		const LocalVector<String> include_dirs = _find_hiprt_include_dirs();
		if (include_dirs.is_empty()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend cannot compile its trace kernel because hiprt/hiprt_device.h was not found under hiprt_sdk_path, HIPRT_PATH, HIP_PATH, or ROCM_PATH.";
			}
			return false;
		}

		static const char *const kernel_source = R"hiprt(
#include <hiprt/hiprt_device.h>

struct RTGIHIPKernelVec3 {
	float x;
	float y;
	float z;
};

struct RTGIHIPKernelCamera {
	RTGIHIPKernelVec3 origin;
	unsigned int orthogonal;
	RTGIHIPKernelVec3 forward;
	float z_far;
	RTGIHIPKernelVec3 near_top_left;
	float _pad0;
	RTGIHIPKernelVec3 near_top_right;
	float _pad1;
	RTGIHIPKernelVec3 near_bottom_left;
	float _pad2;
	RTGIHIPKernelVec3 near_bottom_right;
	float _pad3;
	RTGIHIPKernelVec3 far_top_left;
	float _pad4;
	RTGIHIPKernelVec3 far_top_right;
	float _pad5;
	RTGIHIPKernelVec3 far_bottom_left;
	float _pad6;
	RTGIHIPKernelVec3 far_bottom_right;
	float _pad7;
};

struct RTGIHIPKernelMaterial {
	float albedo[4];
	float emission[3];
	float emission_strength;
};

static __device__ float3 rtgi_make_float3(RTGIHIPKernelVec3 v) {
	return hiprt::make_float3(v.x, v.y, v.z);
}

static __device__ float3 rtgi_lerp3(float3 a, float3 b, float t) {
	return a + (b - a) * t;
}

static __device__ float3 rtgi_bilerp3(RTGIHIPKernelVec3 top_left, RTGIHIPKernelVec3 top_right, RTGIHIPKernelVec3 bottom_left, RTGIHIPKernelVec3 bottom_right, float u, float v) {
	const float3 top = rtgi_lerp3(rtgi_make_float3(top_left), rtgi_make_float3(top_right), u);
	const float3 bottom = rtgi_lerp3(rtgi_make_float3(bottom_left), rtgi_make_float3(bottom_right), u);
	return rtgi_lerp3(top, bottom, v);
}

static __device__ unsigned short rtgi_float_to_half(float value) {
	value = fmaxf(0.0f, fminf(value, 65504.0f));
	const unsigned int bits = __float_as_uint(value);
	const unsigned int sign = (bits >> 16) & 0x8000u;
	int exponent = int((bits >> 23) & 0xffu) - 127 + 15;
	unsigned int mantissa = bits & 0x7fffffu;
	if (exponent <= 0) {
		if (exponent < -10) {
			return (unsigned short)sign;
		}
		mantissa = (mantissa | 0x800000u) >> (1 - exponent);
		return (unsigned short)(sign | ((mantissa + 0x1000u) >> 13));
	}
	if (exponent >= 31) {
		return (unsigned short)(sign | 0x7bffu);
	}
	return (unsigned short)(sign | (unsigned int(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
}

extern "C" __global__ void GodotRTGIHIPRTTraceKernel(
	hiprtScene scene,
	const RTGIHIPKernelCamera* camera_ptr,
	const RTGIHIPKernelMaterial* materials,
	unsigned int material_count,
	unsigned int width,
	unsigned int height,
	unsigned short* output_rgba16f) {
	const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
	if (x >= width || y >= height) {
		return;
	}

	const RTGIHIPKernelCamera camera = *camera_ptr;
	const float u = (float(x) + 0.5f) / float(width);
	const float v = (float(y) + 0.5f) / float(height);
	const float3 near_point = rtgi_bilerp3(camera.near_top_left, camera.near_top_right, camera.near_bottom_left, camera.near_bottom_right, u, v);
	const float3 far_point = rtgi_bilerp3(camera.far_top_left, camera.far_top_right, camera.far_bottom_left, camera.far_bottom_right, u, v);

	hiprtRay ray;
	if (camera.orthogonal != 0u) {
		ray.origin = near_point;
		ray.direction = hiprt::normalize(rtgi_make_float3(camera.forward));
	} else {
		ray.origin = rtgi_make_float3(camera.origin);
		ray.direction = hiprt::normalize(far_point - near_point);
	}
	ray.minT = 0.001f;
	ray.maxT = camera.z_far;

	hiprtSceneTraversalClosest traversal(scene, ray);
	const hiprtHit hit = traversal.getNextHit();

	float3 radiance = hiprt::make_float3(0.0f, 0.0f, 0.0f);
	if (hit.hasHit()) {
		const unsigned int material_index = hit.instanceID < material_count ? hit.instanceID : 0u;
		const RTGIHIPKernelMaterial material = materials[material_index];
		float3 normal = hiprt::normalize(hit.normal);
		const float facing = fminf(1.0f, fmaxf(0.0f, fabsf(hiprt::dot(normal, -ray.direction))));
		const float diffuse = 0.25f + 0.75f * facing;
		radiance.x = material.albedo[0] * diffuse + material.emission[0] * fmaxf(0.0f, material.emission_strength);
		radiance.y = material.albedo[1] * diffuse + material.emission[1] * fmaxf(0.0f, material.emission_strength);
		radiance.z = material.albedo[2] * diffuse + material.emission[2] * fmaxf(0.0f, material.emission_strength);
	}

	const unsigned int pixel = (y * width + x) * 4u;
	output_rgba16f[pixel + 0u] = rtgi_float_to_half(radiance.x);
	output_rgba16f[pixel + 1u] = rtgi_float_to_half(radiance.y);
	output_rgba16f[pixel + 2u] = rtgi_float_to_half(radiance.z);
	output_rgba16f[pixel + 3u] = rtgi_float_to_half(1.0f);
}
)hiprt";

		LocalVector<CharString> option_storage;
		LocalVector<const char *> options;
		auto push_option = [&](const String &p_option) {
			option_storage.push_back(p_option.utf8());
		};

		push_option("-ffast-math");
		for (uint32_t i = 0; i < include_dirs.size(); i++) {
			push_option("-I");
			push_option(include_dirs[i]);
		}
		options.resize(option_storage.size());
		for (uint32_t i = 0; i < option_storage.size(); i++) {
			options[i] = option_storage[i].get_data();
		}

		const char *function_name = "GodotRTGIHIPRTTraceKernel";
		void *function = nullptr;
		const int hiprt_error = hip.hiprtBuildTraceKernels(
				hiprt_context,
				1,
				&function_name,
				kernel_source,
				"godot_rtgi_hiprt_trace_kernel.hip",
				0,
				nullptr,
				nullptr,
				options.size(),
				options.is_empty() ? nullptr : options.ptr(),
				1,
				1,
				nullptr,
				&function,
				nullptr,
				true);
		if (hiprt_error != 0 || function == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hiprtBuildTraceKernels for the RTGI trace kernel with error %d.", hiprt_error);
			}
			return false;
		}

		hip_trace_function = function;
		return true;
	}

	bool _dispatch_trace_kernel(RTGIBackendFrameContext &p_context, String *r_fallback_reason) {
		if (hiprt_scene == nullptr || hip_output_mipmapped_array == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT dispatch requires an uploaded HIP RT scene and imported Vulkan output image.";
			}
			return false;
		}
		if (hip_materials_device_ptr == nullptr || hip_material_count == 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT dispatch has no uploaded material table.";
			}
			return false;
		}
		if (p_context.output_size.x <= 0 || p_context.output_size.y <= 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT dispatch has an invalid output size.";
			}
			return false;
		}
		if (!_ensure_trace_kernel(r_fallback_reason)) {
			return false;
		}

		RTGIHIPKernelCamera camera;
		if (!_make_kernel_camera(p_context, camera, r_fallback_reason)) {
			return false;
		}

		RTGIHIPDeviceAllocation camera_allocation;
		RTGIHIPDeviceAllocation output_allocation;
		uint32_t width = uint32_t(p_context.output_size.x);
		uint32_t height = uint32_t(p_context.output_size.y);
		const size_t output_pitch = size_t(width) * sizeof(uint16_t) * 4;
		const size_t output_size = output_pitch * height;
		if (!_hip_allocate(sizeof(RTGIHIPKernelCamera), camera_allocation, r_fallback_reason, "HIP RT trace camera") ||
				!_hip_allocate(output_size, output_allocation, r_fallback_reason, "HIP RT trace output")) {
			_hip_free_allocation(camera_allocation);
			_hip_free_allocation(output_allocation);
			return false;
		}

		int hip_error = hip.hipMemcpy(camera_allocation.ptr, &camera, sizeof(RTGIHIPKernelCamera), RTGI_HIP_MEMCPY_HOST_TO_DEVICE);
		if (hip_error != 0) {
			_hip_free_allocation(camera_allocation);
			_hip_free_allocation(output_allocation);
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to upload trace camera; hipMemcpy returned %d.", hip_error);
			}
			return false;
		}

		if (p_context.rd != nullptr) {
			const Error handoff_err = p_context.rd->external_resource_handoff_sync(p_context.exchange.external_wait_semaphore);
			if (handoff_err != OK) {
				_hip_free_allocation(camera_allocation);
				_hip_free_allocation(output_allocation);
				if (r_fallback_reason) {
					*r_fallback_reason = "HIP RT backend could not flush the RenderingDevice graph before importing the RTGI output image for HIP dispatch.";
				}
				return false;
			}
		}

		if (hip_wait_semaphore != nullptr) {
			void *wait_semaphore = hip_wait_semaphore;
			RTGIHIPExternalSemaphoreWaitParams wait_params = {};
			const int wait_error = hip.hipWaitExternalSemaphoresAsync(&wait_semaphore, &wait_params, 1, nullptr);
			if (wait_error != 0) {
				_hip_free_allocation(camera_allocation);
				_hip_free_allocation(output_allocation);
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed to wait for the RenderingDevice external semaphore before dispatch; hipWaitExternalSemaphoresAsync returned %d.", wait_error);
				}
				return false;
			}
		}

		void *camera_device_ptr = camera_allocation.ptr;
		void *materials_device_ptr = hip_materials_device_ptr;
		void *output_device_ptr = output_allocation.ptr;
		void *kernel_args[] = {
			&hiprt_scene,
			&camera_device_ptr,
			&materials_device_ptr,
			&hip_material_count,
			&width,
			&height,
			&output_device_ptr,
		};
		const uint32_t block_x = 16;
		const uint32_t block_y = 16;
		const uint32_t grid_x = (width + block_x - 1) / block_x;
		const uint32_t grid_y = (height + block_y - 1) / block_y;
		hip_error = hip.hipModuleLaunchKernel(hip_trace_function, grid_x, grid_y, 1, block_x, block_y, 1, 0, nullptr, kernel_args, nullptr);
		if (hip_error != 0) {
			_hip_free_allocation(camera_allocation);
			_hip_free_allocation(output_allocation);
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hipModuleLaunchKernel for the RTGI trace kernel with error %d.", hip_error);
			}
			return false;
		}

		hip_error = hip.hipDeviceSynchronize();
		if (hip_error != 0) {
			_hip_free_allocation(camera_allocation);
			_hip_free_allocation(output_allocation);
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to synchronize after trace kernel launch; hipDeviceSynchronize returned %d.", hip_error);
			}
			return false;
		}

		void *output_array = nullptr;
		hip_error = hip.hipGetMipmappedArrayLevel(&output_array, hip_output_mipmapped_array, 0);
		if (hip_error != 0 || output_array == nullptr) {
			_hip_free_allocation(camera_allocation);
			_hip_free_allocation(output_allocation);
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed hipGetMipmappedArrayLevel for the imported Vulkan output image with error %d.", hip_error);
			}
			return false;
		}

		hip_error = hip.hipMemcpy2DToArray(output_array, 0, 0, output_allocation.ptr, output_pitch, output_pitch, height, RTGI_HIP_MEMCPY_DEVICE_TO_DEVICE);
		_hip_free_allocation(camera_allocation);
		_hip_free_allocation(output_allocation);
		if (hip_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to copy the trace output into the imported Vulkan image; hipMemcpy2DToArray returned %d.", hip_error);
			}
			return false;
		}

		if (hip_signal_semaphore != nullptr) {
			void *signal_semaphore = hip_signal_semaphore;
			RTGIHIPExternalSemaphoreSignalParams signal_params = {};
			hip_error = hip.hipSignalExternalSemaphoresAsync(&signal_semaphore, &signal_params, 1, nullptr);
			if (hip_error != 0) {
				if (r_fallback_reason) {
					*r_fallback_reason = vformat("HIP RT backend failed to signal the imported Vulkan semaphore after dispatch; hipSignalExternalSemaphoresAsync returned %d.", hip_error);
				}
				return false;
			}
		}

		hip_error = hip.hipDeviceSynchronize();
		if (hip_error != 0) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("HIP RT backend failed to synchronize after copying trace output; hipDeviceSynchronize returned %d.", hip_error);
			}
			return false;
		}

		if (p_context.rd != nullptr && p_context.exchange.external_signal_semaphore) {
			const Error wait_err = p_context.rd->external_semaphore_wait_on_current_frame(p_context.exchange.external_signal_semaphore);
			if (wait_err != OK) {
				if (r_fallback_reason) {
					*r_fallback_reason = "HIP RT backend could not make RenderingDevice wait on the HIP completion semaphore.";
				}
				return false;
			}
		}

		p_context.exchange.rd_owns_output_after_dispatch = true;
		return true;
	}

	void _release_context() {
		_release_scene();
		_release_external_imports();
		if (hiprt_context != nullptr && hip.hiprtDestroyContext != nullptr) {
			hip.hiprtDestroyContext(hiprt_context);
			hiprt_context = nullptr;
		}
		hip_trace_function = nullptr;
		hip.unload();
		hip_context = nullptr;
		hip_device = 0;
	}

public:
	virtual RTGIBackendCapabilities query_capabilities() const override {
		return _rtgi_static_backend_capabilities(RSE::PT_BACKEND_AMD_HIP_RT);
	}

	virtual bool initialize(RenderForwardClustered *p_owner, RenderRaytracing *p_raytracing, String *r_fallback_reason) override {
		owner = p_owner;
		raytracing = p_raytracing;
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (!capabilities.available) {
			if (r_fallback_reason) {
				*r_fallback_reason = capabilities.fallback_reason;
			}
			return false;
		}
		return _ensure_context(r_fallback_reason);
	}

	virtual void shutdown() override {
		_release_context();
		owner = nullptr;
		raytracing = nullptr;
	}

	virtual bool prepare_frame(RTGIBackendFrameContext &r_context, String *r_fallback_reason) override {
		if (!_ensure_context(r_fallback_reason)) {
			return false;
		}
		_release_external_imports();
		if (r_context.rd == nullptr || raytracing == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend is missing RenderingDevice or raytracing owner state.";
			}
			return false;
		}
		Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
		if (r_context.render_data && r_context.render_data->render_buffers.is_valid() && r_context.render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = r_context.render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
		if (rb_data.is_null()) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT render buffer data is unavailable.";
			}
			return false;
		}
		r_context.viewport_state = raytracing->build_tlas(r_context.render_data, r_context.rt_flags);
		if (r_context.viewport_state != nullptr) {
			r_context.radiance_history_invalidated = r_context.viewport_state->radiance_history_invalidated;
			raytracing->populate_backend_scene_resources(r_context.viewport_state, r_context.scene_resources);
			raytracing->populate_backend_scene_snapshot(r_context.viewport_state, r_context.scene_snapshot);
		}
		if (r_context.viewport_state == nullptr) {
			if (r_fallback_reason) {
				*r_fallback_reason = "HIP RT backend could not build the shared RTGI scene snapshot.";
			}
			return false;
		}
		r_context.output_size = rb_data->rt_get_size();
		r_context.exchange.output_texture = rb_data->rt_get_texture();
		r_context.exchange.depth_texture = rb_data->rt_has_depth_texture() ? rb_data->rt_get_depth_texture() : RID();
		r_context.exchange.diffuse_radiance_texture = rb_data->rt_get_diffuse_radiance();
		r_context.exchange.specular_radiance_texture = rb_data->rt_get_specular_radiance();
		if (!RTGIVulkanInteropAdapter::populate_external_memory_exchange(r_context.rd, r_context.exchange, r_fallback_reason)) {
			return false;
		}
		return true;
	}

	virtual bool upload_or_import_scene(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return _build_scene_from_snapshot(p_context.scene_snapshot, r_fallback_reason) &&
				_import_external_exchange(p_context, r_fallback_reason);
	}

	virtual bool upload_materials_lights_environment(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool prepare_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count, RID &r_probe_pipeline, RID &r_probe_uniform_set, String *r_fallback_reason) override {
		if (r_fallback_reason) {
			*r_fallback_reason = "HIP RT routes STRC probe updates to Vulkan Generic until native probe kernels are linked.";
		}
		return false;
	}

	virtual bool dispatch_path_trace(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return _dispatch_trace_kernel(p_context, r_fallback_reason);
	}

	virtual bool dispatch_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_pipeline, RID p_probe_uniform_set, RID p_probe_output_buffer, uint32_t p_ray_count, String *r_fallback_reason) override {
		if (r_fallback_reason) {
			*r_fallback_reason = "HIP RT native probe dispatch is not linked yet.";
		}
		return false;
	}

	virtual bool handoff_denoiser(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}

	virtual bool synchronize_output(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		if (p_context.rd != nullptr) {
			const Error handoff_err = p_context.rd->external_resource_handoff_sync();
			if (handoff_err != OK) {
				if (r_fallback_reason) {
					*r_fallback_reason = "HIP RT backend could not flush the RenderingDevice graph after HIP signaled RTGI output completion.";
				}
				return false;
			}
		}
		RTGIVulkanInteropAdapter::cleanup_external_memory_exchange(p_context.rd, p_context.exchange);
		return true;
	}

	virtual bool abort_frame(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		_release_scene();
		_release_external_imports();
		RTGIVulkanInteropAdapter::cleanup_external_memory_exchange(p_context.rd, p_context.exchange);
		p_context.exchange.rd_owns_output_after_dispatch = true;
		return true;
	}
};
#endif

class VendorRTGIBackend : public RTGIBackend {
	RSE::PathtracingBackend backend;
	RenderForwardClustered *owner = nullptr;
	RenderRaytracing *raytracing = nullptr;

public:
	VendorRTGIBackend(RSE::PathtracingBackend p_backend) {
		backend = p_backend;
	}

	virtual RTGIBackendCapabilities query_capabilities() const override {
		return _rtgi_static_backend_capabilities(backend);
	}

	virtual bool initialize(RenderForwardClustered *p_owner, RenderRaytracing *p_raytracing, String *r_fallback_reason) override {
		owner = p_owner;
		raytracing = p_raytracing;
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s backend SDK entrypoints are marked available, but no concrete adapter implementation is linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual void shutdown() override {
		owner = nullptr;
		raytracing = nullptr;
	}

	virtual bool prepare_frame(RTGIBackendFrameContext &r_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_context.rd != nullptr) {
			const RTGIVulkanInteropDeviceContext device_context = RTGIVulkanInteropAdapter::get_device_context(r_context.rd);
			if (!device_context.valid && r_fallback_reason) {
				*r_fallback_reason = device_context.failure_reason;
				return false;
			}
		}
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s frame preparation adapter entrypoints are not linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual bool upload_or_import_scene(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s scene import adapter entrypoints are not linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual bool upload_materials_lights_environment(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s material/light/environment upload adapter entrypoints are not linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual bool prepare_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count, RID &r_probe_pipeline, RID &r_probe_uniform_set, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.native_probe_update ? vformat("%s native probe update entrypoints are not linked into this build.", capabilities.name) : capabilities.probe_update_path;
		}
		return false;
	}

	virtual bool dispatch_path_trace(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s dispatch adapter entrypoints are not linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual bool dispatch_probe_update(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_pipeline, RID p_probe_uniform_set, RID p_probe_output_buffer, uint32_t p_ray_count, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.native_probe_update ? vformat("%s native probe dispatch entrypoints are not linked into this build.", capabilities.name) : capabilities.probe_update_path;
		}
		return false;
	}

	virtual bool handoff_denoiser(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.denoiser_failure_reason;
		}
		return false;
	}

	virtual bool synchronize_output(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		const RTGIBackendCapabilities capabilities = query_capabilities();
		if (r_fallback_reason) {
			*r_fallback_reason = capabilities.available ? vformat("%s output synchronization adapter entrypoints are not linked into this build.", capabilities.name) : capabilities.fallback_reason;
		}
		return false;
	}

	virtual bool abort_frame(RTGIBackendFrameContext &p_context, String *r_fallback_reason) override {
		return true;
	}
};

static String _rtgi_backend_exchange_summary(const RTGIBackendCapabilities &p_capabilities) {
	String exchange;
	if (p_capabilities.rendering_device_exchange) {
		exchange = "RenderingDevice";
	}
	if (p_capabilities.external_memory) {
		exchange += exchange.is_empty() ? "external memory" : ", external memory";
	}
	if (p_capabilities.external_semaphore) {
		exchange += exchange.is_empty() ? "external semaphores" : ", external semaphores";
	}
	if (p_capabilities.timeline_semaphore) {
		exchange += exchange.is_empty() ? "timeline semaphores" : ", timeline semaphores";
	}
	if (p_capabilities.staged_copy) {
		exchange += exchange.is_empty() ? "staged copy" : ", staged copy";
	}
	return exchange.is_empty() ? "none" : exchange;
}

static bool _rtgi_backend_has_explicit_exchange(const RTGIBackendCapabilities &p_capabilities) {
	const bool rd_internal_exchange = p_capabilities.rendering_device_exchange;
	const bool external_memory_semaphore_exchange = p_capabilities.external_memory && p_capabilities.external_semaphore;
	const bool timeline_exchange = p_capabilities.timeline_semaphore;
	const bool staged_copy_exchange = p_capabilities.staged_copy;
	return rd_internal_exchange || external_memory_semaphore_exchange || timeline_exchange || staged_copy_exchange;
}

static RTGIBackendCapabilities _rtgi_backend_effective_capabilities(const RTGIBackendCapabilities &p_capabilities) {
	RTGIBackendCapabilities effective = p_capabilities;
	if (effective.available && !_rtgi_backend_has_explicit_exchange(effective)) {
		effective.available = false;
		effective.resource_exchange_supported = false;
		effective.availability_failure = "resource_exchange_unavailable";
		effective.fallback_reason = "RTGI backend did not declare an explicit RenderingDevice/Vulkan resource exchange path.";
	}
	return effective;
}

static bool _rtgi_backend_external_handle_type_is_valid(RTGIBackendExternalHandleType p_handle_type) {
	switch (p_handle_type) {
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD:
		case RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32:
			return true;
		case RTGI_BACKEND_EXTERNAL_HANDLE_NONE:
		default:
			return false;
	}
}

static RTGIBackendExternalHandleType _rtgi_backend_platform_external_handle_type() {
#if defined(WINDOWS_ENABLED)
	return RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_WIN32;
#else
	return RTGI_BACKEND_EXTERNAL_HANDLE_OPAQUE_FD;
#endif
}

static bool _rtgi_backend_external_handle_types_match_platform(const RTGIBackendResourceExchange &p_exchange) {
	const RTGIBackendExternalHandleType platform_handle_type = _rtgi_backend_platform_external_handle_type();
	return p_exchange.external_memory_handle_type == platform_handle_type &&
			p_exchange.external_wait_semaphore_handle_type == platform_handle_type &&
			p_exchange.external_signal_semaphore_handle_type == platform_handle_type;
}

static bool _rtgi_backend_external_layout_is_valid(RDD::TextureLayout p_layout) {
	return p_layout > RDD::TEXTURE_LAYOUT_UNDEFINED && p_layout < RDD::TEXTURE_LAYOUT_MAX;
}

static bool _rtgi_backend_staged_copy_regions_are_valid(const Vector<RDD::BufferTextureCopyRegion> &p_regions) {
	if (p_regions.is_empty()) {
		return false;
	}
	for (const RDD::BufferTextureCopyRegion &region : p_regions) {
		if (region.row_pitch == 0 ||
				region.texture_subresource.aspect < RDD::TEXTURE_ASPECT_COLOR ||
				region.texture_subresource.aspect >= RDD::TEXTURE_ASPECT_MAX ||
				region.texture_offset.x < 0 ||
				region.texture_offset.y < 0 ||
				region.texture_offset.z < 0 ||
				region.texture_region_size.x <= 0 ||
				region.texture_region_size.y <= 0 ||
				region.texture_region_size.z <= 0) {
			return false;
		}
	}
	return true;
}

static void _rtgi_backend_append_fallback_reason(String &r_fallback_reason, const String &p_reason) {
	if (p_reason.is_empty()) {
		return;
	}
	if (r_fallback_reason.is_empty()) {
		r_fallback_reason = p_reason;
	} else if (r_fallback_reason.find(p_reason) == -1) {
		r_fallback_reason += " " + p_reason;
	}
}

static bool _rtgi_backend_validate_frame_exchange(const RTGIBackendCapabilities &p_capabilities, const RTGIBackendResourceExchange &p_exchange, String *r_fallback_reason) {
	if (!p_exchange.rd_owns_output_before_dispatch) {
		if (r_fallback_reason) {
			*r_fallback_reason = "RTGI backend frame exchange starts without RenderingDevice output ownership.";
		}
		return false;
	}

	switch (p_exchange.mode) {
		case RTGI_BACKEND_EXCHANGE_RD_INTERNAL: {
			if (!p_capabilities.rendering_device_exchange || !p_exchange.output_texture.is_valid() || p_exchange.ownership_direction != RTGI_BACKEND_OWNERSHIP_RD_INTERNAL) {
				if (r_fallback_reason) {
					*r_fallback_reason = "RTGI backend frame exchange did not provide a valid RenderingDevice-owned output texture.";
				}
				return false;
			}
			return true;
		}
		case RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE: {
			if (!p_capabilities.external_memory || !p_capabilities.external_semaphore ||
					!p_exchange.output_texture.is_valid() ||
					p_exchange.external_memory_handle == 0 ||
					!_rtgi_backend_external_handle_type_is_valid(p_exchange.external_memory_handle_type) ||
					p_exchange.external_memory_allocation_size == 0 ||
					p_exchange.external_wait_semaphore_handle == 0 ||
					!_rtgi_backend_external_handle_type_is_valid(p_exchange.external_wait_semaphore_handle_type) ||
					p_exchange.external_signal_semaphore_handle == 0 ||
					!_rtgi_backend_external_handle_type_is_valid(p_exchange.external_signal_semaphore_handle_type) ||
					!_rtgi_backend_external_handle_types_match_platform(p_exchange) ||
					p_exchange.external_semaphore_kind != RTGI_BACKEND_EXTERNAL_SEMAPHORE_BINARY ||
					p_exchange.ownership_direction != RTGI_BACKEND_OWNERSHIP_RD_TO_BACKEND_TO_RD ||
					!_rtgi_backend_external_layout_is_valid(p_exchange.external_output_layout_before_backend) ||
					!_rtgi_backend_external_layout_is_valid(p_exchange.external_output_layout_after_backend)) {
				if (r_fallback_reason) {
					*r_fallback_reason = "RTGI backend frame exchange did not provide a valid output texture plus platform-compatible typed external memory, binary semaphore handles, ownership, and image layout metadata.";
				}
				return false;
			}
			return true;
		}
		case RTGI_BACKEND_EXCHANGE_TIMELINE_SEMAPHORE: {
			if (!p_capabilities.timeline_semaphore || !p_exchange.output_texture.is_valid() || !p_exchange.wait_semaphore.is_valid() || !p_exchange.signal_semaphore.is_valid() ||
					p_exchange.external_semaphore_kind != RTGI_BACKEND_EXTERNAL_SEMAPHORE_TIMELINE ||
					p_exchange.ownership_direction != RTGI_BACKEND_OWNERSHIP_RD_TO_BACKEND_TO_RD ||
					!_rtgi_backend_external_layout_is_valid(p_exchange.external_output_layout_before_backend) ||
					!_rtgi_backend_external_layout_is_valid(p_exchange.external_output_layout_after_backend) ||
					p_exchange.signal_timeline_value <= p_exchange.wait_timeline_value) {
				if (r_fallback_reason) {
					*r_fallback_reason = "RTGI backend frame exchange did not provide a valid output texture, timeline semaphores, ownership, image layout metadata, and values.";
				}
				return false;
			}
			return true;
		}
		case RTGI_BACKEND_EXCHANGE_STAGED_COPY: {
			if (!p_capabilities.staged_copy || !p_exchange.output_texture.is_valid() || !p_exchange.staged_copy_source_buffer.is_valid() || !p_exchange.staged_copy_target_texture.is_valid() ||
					p_exchange.ownership_direction != RTGI_BACKEND_OWNERSHIP_BACKEND_TO_RD_COPY ||
					!_rtgi_backend_external_layout_is_valid(p_exchange.staged_copy_target_layout) ||
					!_rtgi_backend_staged_copy_regions_are_valid(p_exchange.staged_copy_regions)) {
				if (r_fallback_reason) {
					*r_fallback_reason = "RTGI backend frame exchange did not provide a valid output texture plus staged copy source, target, copy ownership direction, target layout, and buffer-to-texture copy regions.";
				}
				return false;
			}
			if (p_exchange.staged_copy_target_texture != p_exchange.output_texture) {
				if (r_fallback_reason) {
					*r_fallback_reason = "RTGI backend staged-copy target texture is not the declared output texture.";
				}
				return false;
			}
			return true;
		}
	}

	if (r_fallback_reason) {
		*r_fallback_reason = "RTGI backend frame exchange mode is unknown.";
	}
	return false;
}

static bool _rtgi_backend_callback_usage_can_write_texture(RenderingDevice::CallbackResourceUsage p_usage) {
	switch (p_usage) {
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_COPY_TO:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_RESOLVE_TO:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_GENERAL:
			return true;
		default:
			return false;
	}
}

static bool _rtgi_backend_callback_usage_can_read_buffer(RenderingDevice::CallbackResourceUsage p_usage) {
	switch (p_usage) {
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_COPY_FROM:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_UNIFORM_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_INDIRECT_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_TEXTURE_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_VERTEX_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_INDEX_BUFFER_READ:
		case RenderingDevice::CALLBACK_RESOURCE_USAGE_GENERAL:
			return true;
		default:
			return false;
	}
}

enum RTGIBackendCallbackPhase {
	RTGI_BACKEND_CALLBACK_ACQUIRE,
	RTGI_BACKEND_CALLBACK_RELEASE,
};

static const Vector<RenderingDevice::CallbackResource> &_rtgi_backend_callback_resources_for_phase(const RTGIBackendResourceExchange &p_exchange, RTGIBackendCallbackPhase p_phase) {
	return p_phase == RTGI_BACKEND_CALLBACK_ACQUIRE ? p_exchange.acquire_callback_resources : p_exchange.release_callback_resources;
}

static RDD::DriverCallback _rtgi_backend_driver_callback_for_phase(const RTGIBackendResourceExchange &p_exchange, RTGIBackendCallbackPhase p_phase) {
	return p_phase == RTGI_BACKEND_CALLBACK_ACQUIRE ? p_exchange.acquire_driver_callback : p_exchange.release_driver_callback;
}

static void *_rtgi_backend_driver_callback_userdata_for_phase(const RTGIBackendResourceExchange &p_exchange, RTGIBackendCallbackPhase p_phase) {
	return p_phase == RTGI_BACKEND_CALLBACK_ACQUIRE ? p_exchange.acquire_driver_callback_userdata : p_exchange.release_driver_callback_userdata;
}

static const char *_rtgi_backend_callback_phase_name(RTGIBackendCallbackPhase p_phase) {
	return p_phase == RTGI_BACKEND_CALLBACK_ACQUIRE ? "acquire" : "release";
}

static bool _rtgi_backend_callback_tracks_output_texture(const RTGIBackendResourceExchange &p_exchange, RTGIBackendCallbackPhase p_phase) {
	if (!p_exchange.output_texture.is_valid()) {
		return false;
	}
	for (const RenderingDevice::CallbackResource &resource : _rtgi_backend_callback_resources_for_phase(p_exchange, p_phase)) {
		if (resource.rid == p_exchange.output_texture &&
				resource.type == RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE &&
				_rtgi_backend_callback_usage_can_write_texture(resource.usage)) {
			return true;
		}
	}
	return false;
}

static bool _rtgi_backend_callback_tracks_staged_copy(const RTGIBackendResourceExchange &p_exchange, RTGIBackendCallbackPhase p_phase) {
	bool source_tracked = false;
	bool target_tracked = false;
	for (const RenderingDevice::CallbackResource &resource : _rtgi_backend_callback_resources_for_phase(p_exchange, p_phase)) {
		if (resource.rid == p_exchange.staged_copy_source_buffer &&
				resource.type == RenderingDevice::CALLBACK_RESOURCE_TYPE_BUFFER &&
				_rtgi_backend_callback_usage_can_read_buffer(resource.usage)) {
			source_tracked = true;
		}
		if (resource.rid == p_exchange.staged_copy_target_texture &&
				resource.type == RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE &&
				_rtgi_backend_callback_usage_can_write_texture(resource.usage)) {
			target_tracked = true;
		}
	}
	return source_tracked && target_tracked;
}

static bool _rtgi_backend_validate_driver_callback_phase(const RTGIBackendFrameContext &p_context, RTGIBackendCallbackPhase p_phase, String *r_fallback_reason) {
	const RTGIBackendResourceExchange &exchange = p_context.exchange;
	const RDD::DriverCallback driver_callback = _rtgi_backend_driver_callback_for_phase(exchange, p_phase);
	const Vector<RenderingDevice::CallbackResource> &callback_resources = _rtgi_backend_callback_resources_for_phase(exchange, p_phase);
	if (driver_callback == nullptr) {
		const bool phase_required =
				(exchange.mode == RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE || exchange.mode == RTGI_BACKEND_EXCHANGE_TIMELINE_SEMAPHORE) ||
				(exchange.mode == RTGI_BACKEND_EXCHANGE_STAGED_COPY && p_phase == RTGI_BACKEND_CALLBACK_RELEASE);
		if (phase_required) {
			if (r_fallback_reason) {
				*r_fallback_reason = vformat("RTGI backend selected an external or staged Vulkan exchange without a RenderingDevice %s driver callback.", _rtgi_backend_callback_phase_name(p_phase));
			}
			return false;
		}
		return true;
	}

	if (p_context.rd == nullptr) {
		if (r_fallback_reason) {
			*r_fallback_reason = "RTGI backend cannot record a driver callback without a RenderingDevice.";
		}
		return false;
	}
	if (callback_resources.is_empty()) {
		if (r_fallback_reason) {
			*r_fallback_reason = vformat("RTGI backend %s driver callback did not declare any RenderingDevice resources.", _rtgi_backend_callback_phase_name(p_phase));
		}
		return false;
	}
	if (exchange.mode == RTGI_BACKEND_EXCHANGE_STAGED_COPY && p_phase == RTGI_BACKEND_CALLBACK_RELEASE && !_rtgi_backend_callback_tracks_staged_copy(exchange, p_phase)) {
		if (r_fallback_reason) {
			*r_fallback_reason = "RTGI backend staged-copy release callback did not declare the source buffer as readable and target texture as writable RenderingDevice resources.";
		}
		return false;
	}
	if ((exchange.mode == RTGI_BACKEND_EXCHANGE_EXTERNAL_MEMORY_SEMAPHORE || exchange.mode == RTGI_BACKEND_EXCHANGE_TIMELINE_SEMAPHORE) &&
			!_rtgi_backend_callback_tracks_output_texture(exchange, p_phase)) {
		if (r_fallback_reason) {
			*r_fallback_reason = vformat("RTGI backend %s driver callback did not declare the output texture as a writable RenderingDevice resource.", _rtgi_backend_callback_phase_name(p_phase));
		}
		return false;
	}
	return true;
}

static bool _rtgi_backend_validate_driver_callback_exchange(const RTGIBackendFrameContext &p_context, String *r_fallback_reason) {
	return _rtgi_backend_validate_driver_callback_phase(p_context, RTGI_BACKEND_CALLBACK_ACQUIRE, r_fallback_reason) &&
			_rtgi_backend_validate_driver_callback_phase(p_context, RTGI_BACKEND_CALLBACK_RELEASE, r_fallback_reason);
}

#ifdef TESTS_ENABLED
bool RenderRaytracing::test_vulkan_external_resource_exchange(RenderingDevice *p_rd, Dictionary *r_result, String *r_failure_reason) {
	if (p_rd == nullptr) {
		if (r_failure_reason) {
			*r_failure_reason = "RenderingDevice is unavailable.";
		}
		return false;
	}
	if (!_rtgi_current_rd_device_family_is_vulkan(p_rd)) {
		if (r_failure_reason) {
			*r_failure_reason = "The active RenderingDevice driver is not Vulkan.";
		}
		return false;
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

	RID output_texture = p_rd->texture_create_exportable(tf, RD::TextureView());
	if (!output_texture.is_valid()) {
		if (r_failure_reason) {
			*r_failure_reason = "Could not create an exportable smoke-test RTGI texture.";
		}
		return false;
	}

	RTGIBackendFrameContext context;
	context.rd = p_rd;
	context.exchange.output_texture = output_texture;

	String failure_reason;
	const bool exchange_ok = RTGIVulkanInteropAdapter::populate_external_memory_exchange(p_rd, context.exchange, &failure_reason);
	RTGIBackendCapabilities capabilities;
	capabilities.external_memory = true;
	capabilities.external_semaphore = true;
	const bool metadata_ok = exchange_ok &&
			_rtgi_backend_validate_frame_exchange(capabilities, context.exchange, &failure_reason) &&
			_rtgi_backend_validate_driver_callback_exchange(context, &failure_reason);

	if (r_result != nullptr) {
		(*r_result)["exchange_ok"] = exchange_ok;
		(*r_result)["metadata_ok"] = metadata_ok;
		(*r_result)["output_texture_valid"] = output_texture.is_valid();
		(*r_result)["external_memory_handle"] = context.exchange.external_memory_handle;
		(*r_result)["external_memory_handle_type"] = int(context.exchange.external_memory_handle_type);
		(*r_result)["external_memory_allocation_size"] = context.exchange.external_memory_allocation_size;
		(*r_result)["external_wait_semaphore_handle"] = context.exchange.external_wait_semaphore_handle;
		(*r_result)["external_wait_semaphore_handle_type"] = int(context.exchange.external_wait_semaphore_handle_type);
		(*r_result)["external_signal_semaphore_handle"] = context.exchange.external_signal_semaphore_handle;
		(*r_result)["external_signal_semaphore_handle_type"] = int(context.exchange.external_signal_semaphore_handle_type);
		(*r_result)["acquire_callback_declared"] = context.exchange.acquire_driver_callback != nullptr && !context.exchange.acquire_callback_resources.is_empty();
		(*r_result)["release_callback_declared"] = context.exchange.release_driver_callback != nullptr && !context.exchange.release_callback_resources.is_empty();
		(*r_result)["rd_owns_output_after_dispatch"] = context.exchange.rd_owns_output_after_dispatch;
	}

	RTGIVulkanInteropAdapter::cleanup_external_memory_exchange(p_rd, context.exchange);
	p_rd->free_rid(output_texture);

	if (!metadata_ok) {
		if (r_failure_reason) {
			*r_failure_reason = failure_reason.is_empty() ? "Vulkan external RTGI smoke exchange validation failed." : failure_reason;
		}
		return false;
	}
	return true;
}
#endif

static bool _rtgi_backend_record_driver_callback(RTGIBackendFrameContext &p_context, RTGIBackendCallbackPhase p_phase, String *r_fallback_reason) {
	if (!_rtgi_backend_validate_driver_callback_phase(p_context, p_phase, r_fallback_reason)) {
		return false;
	}

	const RTGIBackendResourceExchange &exchange = p_context.exchange;
	const RDD::DriverCallback driver_callback = _rtgi_backend_driver_callback_for_phase(exchange, p_phase);
	if (driver_callback == nullptr) {
		return true;
	}

	const Vector<RenderingDevice::CallbackResource> &callback_resources = _rtgi_backend_callback_resources_for_phase(exchange, p_phase);
	const Error err = p_context.rd->driver_callback_add(driver_callback, _rtgi_backend_driver_callback_userdata_for_phase(exchange, p_phase), VectorView<RenderingDevice::CallbackResource>(callback_resources.ptr(), callback_resources.size()));
	if (err != OK) {
		if (r_fallback_reason) {
			*r_fallback_reason = vformat("RTGI backend failed to record its RenderingDevice %s driver callback.", _rtgi_backend_callback_phase_name(p_phase));
		}
		return false;
	}
	if (p_phase == RTGI_BACKEND_CALLBACK_ACQUIRE) {
		p_context.acquire_callback_recorded = true;
	} else {
		p_context.release_callback_recorded = true;
	}
	return true;
}

static bool _rtgi_backend_release_after_abort_if_needed(RTGIBackendFrameContext &p_context, String *r_fallback_reason) {
	if (!p_context.acquire_callback_recorded || p_context.release_callback_recorded) {
		return true;
	}

	String release_reason;
	if (!_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_RELEASE, &release_reason)) {
		p_context.exchange.rd_owns_output_after_dispatch = false;
		if (r_fallback_reason) {
			_rtgi_backend_append_fallback_reason(*r_fallback_reason, "RTGI backend recorded an acquire callback but could not record the matching release callback after abort: " + release_reason);
		}
		return false;
	}
	return true;
}

static String _rtgi_backend_capability_summary(const RTGIBackendCapabilities &p_capabilities) {
	const String active_runtime = p_capabilities.available ? (p_capabilities.runtime_name.is_empty() ? (p_capabilities.vulkan_runtime ? "Vulkan" : "unspecified") : p_capabilities.runtime_name) : "none";
	String summary = vformat(
			"RTGI backend '%s': %s, runtime=%s, integration=%s, exchange=%s, denoiser_handoff=%s",
			p_capabilities.name,
			p_capabilities.available ? "available" : "unavailable",
			active_runtime,
			p_capabilities.integration_path.is_empty() ? "none" : p_capabilities.integration_path,
			_rtgi_backend_exchange_summary(p_capabilities),
			p_capabilities.denoiser_handoff ? "yes" : "no");
	if (!p_capabilities.available && !p_capabilities.fallback_reason.is_empty()) {
		summary += ". Fallback reason: " + p_capabilities.fallback_reason;
	}
	return summary;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
	bindless_block = memnew(BindlessBlock);
	rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC] = memnew(VulkanGenericRTGIBackend);
	String fallback_reason;
	rtgi_backend_initialized[RSE::PT_BACKEND_VULKAN_GENERIC] = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC]->initialize(owner, this, &fallback_reason);
	for (uint32_t i = 0; i < RSE::PT_BACKEND_MAX; i++) {
		if (rtgi_backends[i] != nullptr) {
			print_line(_rtgi_backend_capability_summary(get_backend_capabilities((RSE::PathtracingBackend)i)));
		}
	}

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
	for (uint32_t i = 0; i < RSE::PT_BACKEND_MAX; i++) {
		if (rtgi_backends[i] != nullptr) {
			rtgi_backends[i]->shutdown();
			rtgi_backend_initialized[i] = false;
			memdelete(rtgi_backends[i]);
			rtgi_backends[i] = nullptr;
		}
	}

	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();

	cleanup_caches();

	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}
	if (blue_noise_texture.is_valid()) {
		RD::get_singleton()->free_rid(blue_noise_texture);
		blue_noise_texture = RID();
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

bool RenderRaytracing::_initialize_backend(RSE::PathtracingBackend p_backend, String *r_fallback_reason) {
	if (p_backend < 0 || p_backend >= RSE::PT_BACKEND_MAX || rtgi_backends[p_backend] == nullptr) {
		if (r_fallback_reason) {
			*r_fallback_reason = p_backend >= 0 && p_backend < RSE::PT_BACKEND_MAX ? "Only the Vulkan Generic RTGI backend is enabled in this build." : "Unknown RTGI backend.";
		}
		return false;
	}
	if (rtgi_backend_initialized[p_backend]) {
		return true;
	}
	rtgi_backend_initialized[p_backend] = rtgi_backends[p_backend]->initialize(owner, this, r_fallback_reason);
	if (rtgi_backend_initialized[p_backend]) {
		const RTGIBackendCapabilities capabilities = get_backend_capabilities(p_backend);
		if (!capabilities.available) {
			rtgi_backends[p_backend]->shutdown();
			rtgi_backend_initialized[p_backend] = false;
			if (r_fallback_reason) {
				*r_fallback_reason = capabilities.fallback_reason;
			}
		}
	}
	return rtgi_backend_initialized[p_backend];
}

void RenderRaytracing::_activate_backend(RSE::PathtracingBackend p_backend) {
	if (p_backend < 0 || p_backend >= RSE::PT_BACKEND_MAX || active_backend == p_backend) {
		return;
	}
	active_backend = p_backend;
}

RTGIBackendCapabilities RenderRaytracing::get_backend_capabilities(RSE::PathtracingBackend p_backend) const {
	if (p_backend < 0 || p_backend >= RSE::PT_BACKEND_MAX || rtgi_backends[p_backend] == nullptr) {
		if (p_backend >= 0 && p_backend < RSE::PT_BACKEND_MAX) {
			return _rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities(p_backend));
		}
		return _rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities(RSE::PT_BACKEND_VULKAN_GENERIC));
	}
	return _rtgi_backend_effective_capabilities(rtgi_backends[p_backend]->query_capabilities());
}

RSE::PathtracingBackend RenderRaytracing::resolve_backend(RSE::PathtracingBackend p_requested) {
	p_requested = RSE::PT_BACKEND_VULKAN_GENERIC;

	RTGIBackendCapabilities requested_caps = get_backend_capabilities(p_requested);
	if (requested_caps.available) {
		String fallback_reason;
		if (_initialize_backend(p_requested, &fallback_reason)) {
			_activate_backend(p_requested);
			rtgi_backend_unavailable_warned[p_requested] = false;
			last_requested_backend = p_requested;
			active_backend_fallback_reason = String();
			return active_backend;
		}
		requested_caps.available = false;
		requested_caps.fallback_reason = fallback_reason;
	}

	if (p_requested != RSE::PT_BACKEND_VULKAN_GENERIC) {
		active_backend_fallback_reason = requested_caps.fallback_reason;
		if (!rtgi_backend_unavailable_warned[p_requested]) {
			WARN_PRINT(vformat("RTGI backend '%s' is unavailable: %s Falling back to Vulkan Generic.", backend_get_name(p_requested), active_backend_fallback_reason));
			rtgi_backend_unavailable_warned[p_requested] = true;
		}
	} else {
		active_backend_fallback_reason = String();
	}

	String generic_reason;
	if (_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
		_activate_backend(RSE::PT_BACKEND_VULKAN_GENERIC);
	} else {
		active_backend = RSE::PT_BACKEND_VULKAN_GENERIC;
		if (generic_reason.is_empty()) {
			generic_reason = "Vulkan Generic backend could not be initialized.";
		}
		if (active_backend_fallback_reason.is_empty()) {
			active_backend_fallback_reason = generic_reason;
		} else {
			active_backend_fallback_reason += " Vulkan Generic fallback is also unavailable: " + generic_reason;
		}
		WARN_PRINT_ONCE(vformat("RTGI Vulkan Generic fallback is unavailable: %s Path tracing will be disabled for this frame.", generic_reason));
	}
	last_requested_backend = p_requested;
	return active_backend;
}

RTGIBackendStatus RenderRaytracing::get_backend_status() const {
	RTGIBackendStatus status;
	status.requested_backend = last_requested_backend;
	status.active_backend = active_backend;
	status.requested_capabilities = get_backend_capabilities(last_requested_backend);
	status.active_capabilities = get_backend_capabilities(active_backend);
	status.requested_backend_available = status.requested_capabilities.available;
	status.requested_backend_initialized = last_requested_backend >= 0 && last_requested_backend < RSE::PT_BACKEND_MAX && rtgi_backend_initialized[last_requested_backend];
	status.active_backend_initialized = active_backend >= 0 && active_backend < RSE::PT_BACKEND_MAX && rtgi_backend_initialized[active_backend];
	status.active_backend_available = status.active_capabilities.available;
	status.using_fallback = status.requested_backend != status.active_backend;
	status.fallback_reason = active_backend_fallback_reason;
	return status;
}

Dictionary RenderRaytracing::get_backend_status_dictionary() const {
	const RTGIBackendStatus status = get_backend_status();
	Dictionary result;
	result["requested_backend"] = (int)status.requested_backend;
	result["requested_backend_name"] = backend_get_name(status.requested_backend);
	result["active_backend"] = (int)status.active_backend;
	result["active_backend_name"] = backend_get_name(status.active_backend);
	result["requested_backend_available"] = status.requested_backend_available;
	result["active_backend_available"] = status.active_backend_available;
	result["requested_backend_initialized"] = status.requested_backend_initialized;
	result["active_backend_initialized"] = status.active_backend_initialized;
	result["using_fallback"] = status.using_fallback;
	result["fallback_backend"] = status.using_fallback ? int(status.active_backend) : -1;
	result["fallback_backend_name"] = status.using_fallback ? backend_get_name(status.active_backend) : String();
	result["fallback_reason"] = status.fallback_reason;
	result["requested_capabilities"] = get_backend_capabilities_dictionary(status.requested_backend);
	result["active_capabilities"] = get_backend_capabilities_dictionary(status.active_backend);
	return result;
}

Dictionary RenderRaytracing::get_backend_status_dictionary(RSE::PathtracingBackend p_requested) const {
	if (p_requested < 0 || p_requested >= RSE::PT_BACKEND_MAX) {
		p_requested = RSE::PT_BACKEND_VULKAN_GENERIC;
	}

	const RTGIBackendCapabilities requested_capabilities = get_backend_capabilities(p_requested);
	const RTGIBackendCapabilities generic_capabilities = get_backend_capabilities(RSE::PT_BACKEND_VULKAN_GENERIC);
	const bool use_fallback = p_requested != RSE::PT_BACKEND_VULKAN_GENERIC && !requested_capabilities.available;
	const RSE::PathtracingBackend active_backend_for_request = use_fallback ? RSE::PT_BACKEND_VULKAN_GENERIC : p_requested;
	const RTGIBackendCapabilities active_capabilities = use_fallback ? generic_capabilities : requested_capabilities;

	String fallback_reason;
	if (use_fallback) {
		fallback_reason = requested_capabilities.fallback_reason;
		if (!generic_capabilities.available && !generic_capabilities.fallback_reason.is_empty()) {
			_rtgi_backend_append_fallback_reason(fallback_reason, "Vulkan Generic fallback is also unavailable: " + generic_capabilities.fallback_reason);
		}
	}

	Dictionary result;
	result["requested_backend"] = (int)p_requested;
	result["requested_backend_name"] = backend_get_name(p_requested);
	result["active_backend"] = (int)active_backend_for_request;
	result["active_backend_name"] = backend_get_name(active_backend_for_request);
	result["requested_backend_available"] = requested_capabilities.available;
	result["active_backend_available"] = active_capabilities.available;
	result["requested_backend_initialized"] = rtgi_backend_initialized[p_requested];
	result["active_backend_initialized"] = rtgi_backend_initialized[active_backend_for_request];
	result["using_fallback"] = use_fallback;
	result["fallback_backend"] = use_fallback ? int(active_backend_for_request) : -1;
	result["fallback_backend_name"] = use_fallback ? backend_get_name(active_backend_for_request) : String();
	result["fallback_reason"] = fallback_reason;
	result["requested_capabilities"] = get_backend_capabilities_dictionary(p_requested);
	result["active_capabilities"] = get_backend_capabilities_dictionary(active_backend_for_request);
	return result;
}

RSE::PathtracingBackend RenderRaytracing::backend_from_env_param(float p_backend) {
	const int backend = int(p_backend);
	switch (backend) {
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return RSE::PT_BACKEND_VULKAN_GENERIC;
	}
}

const char *RenderRaytracing::backend_get_name(RSE::PathtracingBackend p_backend) {
	switch (p_backend) {
		case RSE::PT_BACKEND_NVIDIA_RTXPT:
			return "NVIDIA RTXPT";
		case RSE::PT_BACKEND_AMD_HIP_RT:
			return "AMD HIP RT";
		case RSE::PT_BACKEND_INTEL_EMBREE:
			return "Intel Embree/OSPRay";
		case RSE::PT_BACKEND_VULKAN_GENERIC:
		default:
			return "Vulkan Generic";
	}
}

static Dictionary _rtgi_backend_capabilities_to_dictionary(const RTGIBackendCapabilities &p_capabilities, bool p_initialized) {
	Dictionary exchange;
	exchange["rendering_device"] = p_capabilities.rendering_device_exchange;
	exchange["external_memory"] = p_capabilities.external_memory;
	exchange["external_semaphore"] = p_capabilities.external_semaphore;
	exchange["timeline_semaphore"] = p_capabilities.timeline_semaphore;
	exchange["staged_copy"] = p_capabilities.staged_copy;

	Dictionary availability_checks;
	availability_checks["backend_compiled"] = p_capabilities.backend_compiled;
	availability_checks["sdk_headers_present"] = p_capabilities.sdk_headers_present;
	availability_checks["runtime_detected"] = p_capabilities.runtime_detected;
	availability_checks["device_supported"] = p_capabilities.device_supported;
	availability_checks["resource_exchange_supported"] = p_capabilities.resource_exchange_supported;
	availability_checks["implementation_ready"] = p_capabilities.implementation_ready;
	availability_checks["failure"] = p_capabilities.availability_failure.is_empty() ? _rtgi_availability_failure(p_capabilities.backend_compiled, p_capabilities.runtime_detected, p_capabilities.device_supported, p_capabilities.resource_exchange_supported, p_capabilities.implementation_ready) : p_capabilities.availability_failure;
	availability_checks["compile_failure_reason"] = p_capabilities.compile_failure_reason;
	availability_checks["runtime_failure_reason"] = p_capabilities.runtime_failure_reason;
	availability_checks["device_failure_reason"] = p_capabilities.device_failure_reason;
	availability_checks["resource_exchange_failure_reason"] = p_capabilities.resource_exchange_failure_reason;
	availability_checks["implementation_failure_reason"] = p_capabilities.implementation_failure_reason;

	Dictionary result;
	result["backend"] = (int)p_capabilities.backend;
	result["name"] = p_capabilities.name;
	result["available"] = p_capabilities.available;
	result["initialized"] = p_initialized;
	result["runtime_name"] = p_capabilities.runtime_name;
	result["integration_path"] = p_capabilities.integration_path;
	result["rendering_device_family"] = p_capabilities.rendering_device_family;
	result["rendering_device_name"] = p_capabilities.rendering_device_name;
	result["rendering_device_vendor"] = p_capabilities.rendering_device_vendor;
	result["rendering_device_vendor_id"] = (int64_t)p_capabilities.rendering_device_vendor_id;
	result["vulkan_runtime"] = p_capabilities.vulkan_runtime;
	result["vulkan_interop_mode"] = p_capabilities.vulkan_interop_mode;
	result["resource_exchange_sync"] = p_capabilities.resource_exchange_sync;
	result["exchange"] = exchange;
	result["exchange_summary"] = _rtgi_backend_exchange_summary(p_capabilities);
	result["availability_checks"] = availability_checks;
	result["denoiser_handoff"] = p_capabilities.denoiser_handoff;
	result["denoiser_name"] = p_capabilities.denoiser_name;
	result["denoiser_runtime_detected"] = p_capabilities.denoiser_runtime_detected;
	result["denoiser_available"] = p_capabilities.denoiser_available;
	result["denoiser_failure_reason"] = p_capabilities.denoiser_failure_reason;
	result["native_probe_update"] = p_capabilities.native_probe_update;
	result["generic_probe_update_fallback"] = p_capabilities.generic_probe_update_fallback;
	result["probe_update_path"] = p_capabilities.probe_update_path;
	result["fallback_reason"] = p_capabilities.fallback_reason;
	return result;
}

Dictionary RenderRaytracing::get_backend_capabilities_dictionary(RSE::PathtracingBackend p_backend) const {
	const bool initialized = p_backend >= 0 && p_backend < RSE::PT_BACKEND_MAX && rtgi_backend_initialized[p_backend];
	return _rtgi_backend_capabilities_to_dictionary(get_backend_capabilities(p_backend), initialized);
}

Array RenderRaytracing::get_backend_capabilities_dictionaries() const {
	Array result;
	for (int i = 0; i < RSE::PT_BACKEND_MAX; i++) {
		result.push_back(get_backend_capabilities_dictionary((RSE::PathtracingBackend)i));
	}
	return result;
}

Array RenderRaytracing::get_static_backend_capabilities_dictionaries() {
	Array result;
	for (int i = 0; i < RSE::PT_BACKEND_MAX; i++) {
		result.push_back(_rtgi_backend_capabilities_to_dictionary(_rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities((RSE::PathtracingBackend)i)), false));
	}
	return result;
}

Dictionary RenderRaytracing::get_static_backend_status_dictionary() {
	const RTGIBackendCapabilities generic_capabilities = _rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities(RSE::PT_BACKEND_VULKAN_GENERIC));
	const Dictionary generic_capabilities_dict = _rtgi_backend_capabilities_to_dictionary(generic_capabilities, false);

	Dictionary result;
	result["requested_backend"] = (int)RSE::PT_BACKEND_VULKAN_GENERIC;
	result["requested_backend_name"] = backend_get_name(RSE::PT_BACKEND_VULKAN_GENERIC);
	result["active_backend"] = (int)RSE::PT_BACKEND_VULKAN_GENERIC;
	result["active_backend_name"] = backend_get_name(RSE::PT_BACKEND_VULKAN_GENERIC);
	result["requested_backend_available"] = generic_capabilities.available;
	result["active_backend_available"] = generic_capabilities.available;
	result["requested_backend_initialized"] = false;
	result["active_backend_initialized"] = false;
	result["using_fallback"] = false;
	result["fallback_backend"] = -1;
	result["fallback_backend_name"] = String();
	result["fallback_reason"] = "RTGI backend has not been initialized yet.";
	result["requested_capabilities"] = generic_capabilities_dict;
	result["active_capabilities"] = generic_capabilities_dict;
	return result;
}

Dictionary RenderRaytracing::get_static_backend_status_dictionary(RSE::PathtracingBackend p_requested) {
	if (p_requested < 0 || p_requested >= RSE::PT_BACKEND_MAX) {
		p_requested = RSE::PT_BACKEND_VULKAN_GENERIC;
	}

	const RTGIBackendCapabilities requested_capabilities = _rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities(p_requested));
	const RTGIBackendCapabilities generic_capabilities = _rtgi_backend_effective_capabilities(_rtgi_enabled_backend_capabilities(RSE::PT_BACKEND_VULKAN_GENERIC));
	const bool use_fallback = p_requested != RSE::PT_BACKEND_VULKAN_GENERIC && !requested_capabilities.available;
	const RSE::PathtracingBackend active_backend = use_fallback ? RSE::PT_BACKEND_VULKAN_GENERIC : p_requested;
	const RTGIBackendCapabilities active_capabilities = use_fallback ? generic_capabilities : requested_capabilities;

	String fallback_reason;
	if (use_fallback) {
		fallback_reason = requested_capabilities.fallback_reason;
		if (!generic_capabilities.available && !generic_capabilities.fallback_reason.is_empty()) {
			_rtgi_backend_append_fallback_reason(fallback_reason, "Vulkan Generic fallback is also unavailable: " + generic_capabilities.fallback_reason);
		}
	}

	Dictionary result;
	result["requested_backend"] = (int)p_requested;
	result["requested_backend_name"] = backend_get_name(p_requested);
	result["active_backend"] = (int)active_backend;
	result["active_backend_name"] = backend_get_name(active_backend);
	result["requested_backend_available"] = requested_capabilities.available;
	result["active_backend_available"] = active_capabilities.available;
	result["requested_backend_initialized"] = false;
	result["active_backend_initialized"] = false;
	result["using_fallback"] = use_fallback;
	result["fallback_backend"] = use_fallback ? int(active_backend) : -1;
	result["fallback_backend_name"] = use_fallback ? backend_get_name(active_backend) : String();
	result["fallback_reason"] = fallback_reason;
	result["requested_capabilities"] = _rtgi_backend_capabilities_to_dictionary(requested_capabilities, false);
	result["active_capabilities"] = _rtgi_backend_capabilities_to_dictionary(active_capabilities, false);
	return result;
}

bool RenderRaytracing::prepare_backend_frame(const RenderDataRD *p_render_data, uint32_t p_rt_flags, RTGIBackendFrameContext &r_context) {
	RTGIBackend *backend = rtgi_backends[active_backend];
	ERR_FAIL_NULL_V(backend, false);

	r_context = RTGIBackendFrameContext();
	r_context.rd = RD::get_singleton();
	r_context.raytracing = this;
	r_context.render_data = p_render_data;
	r_context.rt_flags = p_rt_flags;

	String fallback_reason;
	auto prepare_and_validate = [](RTGIBackend *p_backend, RTGIBackendFrameContext &r_frame_context, String *r_reason) -> bool {
		if (p_backend == nullptr || !p_backend->prepare_frame(r_frame_context, r_reason)) {
			return false;
		}
		const RTGIBackendCapabilities capabilities = _rtgi_backend_effective_capabilities(p_backend->query_capabilities());
		return _rtgi_backend_validate_frame_exchange(capabilities, r_frame_context.exchange, r_reason) &&
				_rtgi_backend_validate_driver_callback_exchange(r_frame_context, r_reason);
	};
	if (!prepare_and_validate(backend, r_context, &fallback_reason)) {
		if (active_backend != RSE::PT_BACKEND_VULKAN_GENERIC) {
			const RSE::PathtracingBackend failed_backend = active_backend;
			active_backend_fallback_reason = vformat("RTGI backend '%s' failed frame preparation: %s", backend_get_name(failed_backend), fallback_reason);
			WARN_PRINT(vformat("%s Falling back to Vulkan Generic.", active_backend_fallback_reason));
			String abort_reason;
			const bool abort_ok = backend->abort_frame(r_context, &abort_reason);
			const bool release_ok = _rtgi_backend_release_after_abort_if_needed(r_context, &abort_reason);
			if (!abort_ok || !release_ok || !r_context.exchange.rd_owns_output_after_dispatch) {
				active_backend_fallback_reason += vformat(" Could not safely return RD ownership: %s", abort_reason);
				ERR_PRINT_ONCE(active_backend_fallback_reason);
				return false;
			}
			String generic_reason;
			if (_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
				_activate_backend(RSE::PT_BACKEND_VULKAN_GENERIC);
				RTGIBackend *generic_backend = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC];
				r_context = RTGIBackendFrameContext();
				r_context.rd = RD::get_singleton();
				r_context.raytracing = this;
				r_context.render_data = p_render_data;
				r_context.rt_flags = p_rt_flags;
				if (prepare_and_validate(generic_backend, r_context, &fallback_reason)) {
					return true;
				}
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback also failed frame preparation: " + fallback_reason);
			} else {
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback could not be initialized: " + generic_reason);
			}
		}
		_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, vformat("RTGI backend '%s' failed frame preparation: %s", backend_get_name(active_backend), fallback_reason));
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return false;
	}

	return true;
}

RTGIBackendDispatchResult RenderRaytracing::dispatch_path_trace_backend(RTGIBackendFrameContext &r_context) {
	RTGIBackend *backend = rtgi_backends[active_backend];
	ERR_FAIL_NULL_V(backend, RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE);

	String fallback_reason;
	auto run_backend = [&](RTGIBackend *p_backend, RTGIBackendFrameContext &p_context, String *r_fallback_reason) -> bool {
		return _rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_ACQUIRE, r_fallback_reason) &&
				p_backend->upload_or_import_scene(p_context, r_fallback_reason) &&
				p_backend->upload_materials_lights_environment(p_context, r_fallback_reason) &&
				p_backend->dispatch_path_trace(p_context, r_fallback_reason) &&
				p_backend->handoff_denoiser(p_context, r_fallback_reason) &&
				_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_RELEASE, r_fallback_reason) &&
				p_backend->synchronize_output(p_context, r_fallback_reason);
	};
	auto failure_result = [&r_context]() {
		return r_context.exchange.rd_owns_output_after_dispatch ? RTGI_BACKEND_DISPATCH_SAFE_FAILURE : RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	};
	auto prepare_and_validate = [](RTGIBackend *p_backend, RTGIBackendFrameContext &r_frame_context, String *r_reason) -> bool {
		if (p_backend == nullptr || !p_backend->prepare_frame(r_frame_context, r_reason)) {
			return false;
		}
		const RTGIBackendCapabilities capabilities = _rtgi_backend_effective_capabilities(p_backend->query_capabilities());
		return _rtgi_backend_validate_frame_exchange(capabilities, r_frame_context.exchange, r_reason) &&
				_rtgi_backend_validate_driver_callback_exchange(r_frame_context, r_reason);
	};
	auto validate_rd_output_owner = [](RTGIBackendFrameContext &r_frame_context, String *r_reason) -> bool {
		if (r_frame_context.exchange.rd_owns_output_after_dispatch) {
			return true;
		}
		if (r_reason) {
			*r_reason = "RTGI backend completed dispatch without returning output ownership to RenderingDevice.";
		}
		return false;
	};

	if (!run_backend(backend, r_context, &fallback_reason)) {
		if (active_backend != RSE::PT_BACKEND_VULKAN_GENERIC) {
			const RSE::PathtracingBackend failed_backend = active_backend;
			active_backend_fallback_reason = vformat("RTGI backend '%s' failed during dispatch: %s", backend_get_name(failed_backend), fallback_reason);
			WARN_PRINT(vformat("%s Falling back to Vulkan Generic.", active_backend_fallback_reason));
			String abort_reason;
			const bool abort_ok = backend->abort_frame(r_context, &abort_reason);
			const bool release_ok = _rtgi_backend_release_after_abort_if_needed(r_context, &abort_reason);
			if (!abort_ok || !release_ok || !r_context.exchange.rd_owns_output_after_dispatch) {
				active_backend_fallback_reason += vformat(" Could not safely return RD ownership: %s", abort_reason);
				ERR_PRINT_ONCE(active_backend_fallback_reason);
				return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
			}
			const RenderDataRD *render_data = r_context.render_data;
			const uint32_t rt_flags = r_context.rt_flags;
			String generic_reason;
			if (_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
				_activate_backend(RSE::PT_BACKEND_VULKAN_GENERIC);
				RTGIBackend *generic_backend = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC];
				r_context = RTGIBackendFrameContext();
				r_context.rd = RD::get_singleton();
				r_context.raytracing = this;
				r_context.render_data = render_data;
				r_context.rt_flags = rt_flags;
				fallback_reason = String();
				if (prepare_and_validate(generic_backend, r_context, &fallback_reason) && run_backend(generic_backend, r_context, &fallback_reason)) {
					if (!validate_rd_output_owner(r_context, &fallback_reason)) {
						_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed after dispatch: " + fallback_reason);
						ERR_PRINT_ONCE(active_backend_fallback_reason);
						return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
					}
					return RTGI_BACKEND_DISPATCH_OK;
				}
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed during dispatch: " + fallback_reason);
			} else {
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback could not be initialized: " + generic_reason);
			}
		}
		_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, vformat("RTGI backend '%s' failed during dispatch: %s", backend_get_name(active_backend), fallback_reason));
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return failure_result();
	}
	if (!validate_rd_output_owner(r_context, &fallback_reason)) {
		active_backend_fallback_reason = vformat("RTGI backend '%s' failed after dispatch: %s", backend_get_name(active_backend), fallback_reason);
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}

	return RTGI_BACKEND_DISPATCH_OK;
}

RTGIBackendDispatchResult RenderRaytracing::dispatch_primary_direct_backend(RTGIBackendFrameContext &p_context, uint32_t p_primary_direct_flags) {
	// A4: the true radiance_probes-FPT primary-direct full-screen path-trace. It must
	// COEXIST with the WRC/SPG probe dispatches already recorded this frame, so it REUSES
	// the backend frame context's already-built viewport_state/TLAS -- re-running
	// prepare_backend_frame would rebuild the in-flight TLAS the probe dispatches reference
	// (build_tlas is not frame-cached). It only swaps in a primary-direct uniform set +
	// pipeline built from p_primary_direct_flags (which carries RT_FLAG_PRIMARY_DIRECT, so
	// update_uniform_set binds the probe ray-result buffers 107/108 to the default RW
	// buffer -- no phantom probe-buffer dependency -- and the raygen caps indirect bounces
	// to 0). The dispatch itself is the existing full-screen dispatch_path_trace_backend,
	// run against the same context after a save/restore of its dispatch fields. This
	// mirrors the proven dispatch_probe_update_backend pattern (own set, shared context).
	if (p_context.viewport_state == nullptr) {
		return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}
	SceneShaderRaytracing *rt_shader = get_shader();
	if (rt_shader == nullptr) {
		return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}

	const RID pd_uniform_set = update_uniform_set(p_context.viewport_state, p_context.render_data, p_primary_direct_flags);
	const RID pd_pipeline = rt_shader->get_raytracing_pipeline(p_primary_direct_flags);
	if (!pd_uniform_set.is_valid() || !pd_pipeline.is_valid()) {
		return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}

	const RID saved_uniform_set = p_context.uniform_set;
	const RID saved_pipeline = p_context.pipeline;
	const uint32_t saved_rt_flags = p_context.rt_flags;
	p_context.uniform_set = pd_uniform_set;
	p_context.pipeline = pd_pipeline;
	p_context.rt_flags = p_primary_direct_flags;

	const RTGIBackendDispatchResult result = dispatch_path_trace_backend(p_context);

	p_context.uniform_set = saved_uniform_set;
	p_context.pipeline = saved_pipeline;
	p_context.rt_flags = saved_rt_flags;
	return result;
}

RTGIBackendDispatchResult RenderRaytracing::dispatch_probe_update_backend(RTGIBackendFrameContext &p_context, uint32_t p_probe_flags, RID p_probe_output_buffer, uint32_t p_ray_count) {
	RTGIBackend *backend = rtgi_backends[active_backend];
	ERR_FAIL_NULL_V(backend, RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE);

	String fallback_reason;
	RID probe_pipeline;
	RID probe_uniform_set;
	auto failure_result = [&p_context]() {
		return p_context.exchange.rd_owns_output_after_dispatch ? RTGI_BACKEND_DISPATCH_SAFE_FAILURE : RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	};
	auto prepare_and_validate = [](RTGIBackend *p_backend, RTGIBackendFrameContext &r_frame_context, String *r_reason) -> bool {
		if (p_backend == nullptr || !p_backend->prepare_frame(r_frame_context, r_reason)) {
			return false;
		}
		const RTGIBackendCapabilities capabilities = _rtgi_backend_effective_capabilities(p_backend->query_capabilities());
		return _rtgi_backend_validate_frame_exchange(capabilities, r_frame_context.exchange, r_reason) &&
				_rtgi_backend_validate_driver_callback_exchange(r_frame_context, r_reason);
	};
	auto validate_rd_output_owner = [](RTGIBackendFrameContext &r_frame_context, String *r_reason) -> bool {
		if (r_frame_context.exchange.rd_owns_output_after_dispatch) {
			return true;
		}
		if (r_reason) {
			*r_reason = "RTGI backend completed STRC probe dispatch without returning output ownership to RenderingDevice.";
		}
		return false;
	};

	const RTGIBackendCapabilities backend_capabilities = _rtgi_backend_effective_capabilities(backend->query_capabilities());
	if (active_backend != RSE::PT_BACKEND_VULKAN_GENERIC && backend_capabilities.generic_probe_update_fallback) {
		const RSE::PathtracingBackend mixed_backend = active_backend;
		const RenderDataRD *render_data = p_context.render_data;
		const uint32_t rt_flags = p_context.rt_flags;

		String generic_reason;
		if (!_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
			active_backend_fallback_reason = vformat("RTGI backend '%s' routes STRC probe updates to Vulkan Generic, but Vulkan Generic could not be initialized: %s", backend_get_name(mixed_backend), generic_reason);
			ERR_PRINT_ONCE(active_backend_fallback_reason);
			return failure_result();
		}

		RTGIBackend *generic_backend = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC];
		RTGIBackendFrameContext generic_context;
		generic_context.rd = RD::get_singleton();
		generic_context.raytracing = this;
		generic_context.render_data = render_data;
		generic_context.rt_flags = rt_flags;

		RID generic_probe_pipeline;
		RID generic_probe_uniform_set;
		fallback_reason = String();
		if (generic_backend != nullptr &&
				prepare_and_validate(generic_backend, generic_context, &fallback_reason) &&
				_rtgi_backend_record_driver_callback(generic_context, RTGI_BACKEND_CALLBACK_ACQUIRE, &fallback_reason) &&
				generic_backend->prepare_probe_update(generic_context, p_probe_flags, p_probe_output_buffer, p_ray_count, generic_probe_pipeline, generic_probe_uniform_set, &fallback_reason) &&
				generic_backend->dispatch_probe_update(generic_context, p_probe_flags, generic_probe_pipeline, generic_probe_uniform_set, p_probe_output_buffer, p_ray_count, &fallback_reason) &&
				_rtgi_backend_record_driver_callback(generic_context, RTGI_BACKEND_CALLBACK_RELEASE, &fallback_reason) &&
				validate_rd_output_owner(generic_context, &fallback_reason)) {
			p_context = generic_context;
			return RTGI_BACKEND_DISPATCH_OK;
		}

		active_backend_fallback_reason = vformat("RTGI backend '%s' routes STRC probe updates to Vulkan Generic, but the mixed probe dispatch failed: %s", backend_get_name(mixed_backend), fallback_reason);
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return generic_context.exchange.rd_owns_output_after_dispatch ? RTGI_BACKEND_DISPATCH_SAFE_FAILURE : RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}

	if (!backend->prepare_probe_update(p_context, p_probe_flags, p_probe_output_buffer, p_ray_count, probe_pipeline, probe_uniform_set, &fallback_reason)) {
		if (active_backend != RSE::PT_BACKEND_VULKAN_GENERIC) {
			const RSE::PathtracingBackend failed_backend = active_backend;
			active_backend_fallback_reason = vformat("RTGI backend '%s' failed STRC probe preparation: %s", backend_get_name(failed_backend), fallback_reason);
			WARN_PRINT(vformat("%s Falling back to Vulkan Generic.", active_backend_fallback_reason));
			String abort_reason;
			const bool abort_ok = backend->abort_frame(p_context, &abort_reason);
			const bool release_ok = _rtgi_backend_release_after_abort_if_needed(p_context, &abort_reason);
			if (!abort_ok || !release_ok || !p_context.exchange.rd_owns_output_after_dispatch) {
				active_backend_fallback_reason += vformat(" Could not safely return RD ownership: %s", abort_reason);
				ERR_PRINT_ONCE(active_backend_fallback_reason);
				return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
			}
			const RenderDataRD *render_data = p_context.render_data;
			const uint32_t rt_flags = p_context.rt_flags;
			String generic_reason;
			if (_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
				_activate_backend(RSE::PT_BACKEND_VULKAN_GENERIC);
				RTGIBackend *generic_backend = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC];
				p_context = RTGIBackendFrameContext();
				p_context.rd = RD::get_singleton();
				p_context.raytracing = this;
				p_context.render_data = render_data;
				p_context.rt_flags = rt_flags;
				fallback_reason = String();
				if (generic_backend != nullptr &&
						prepare_and_validate(generic_backend, p_context, &fallback_reason) &&
						_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_ACQUIRE, &fallback_reason) &&
						generic_backend->prepare_probe_update(p_context, p_probe_flags, p_probe_output_buffer, p_ray_count, probe_pipeline, probe_uniform_set, &fallback_reason) &&
						generic_backend->dispatch_probe_update(p_context, p_probe_flags, probe_pipeline, probe_uniform_set, p_probe_output_buffer, p_ray_count, &fallback_reason) &&
						_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_RELEASE, &fallback_reason)) {
					if (!validate_rd_output_owner(p_context, &fallback_reason)) {
						_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed after STRC probe dispatch: " + fallback_reason);
						ERR_PRINT_ONCE(active_backend_fallback_reason);
						return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
					}
					return RTGI_BACKEND_DISPATCH_OK;
				}
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed STRC probe preparation or dispatch: " + fallback_reason);
			} else {
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback could not be initialized: " + generic_reason);
			}
		}
		_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, vformat("RTGI backend '%s' failed STRC probe preparation: %s", backend_get_name(active_backend), fallback_reason));
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return failure_result();
	}
	if (!_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_ACQUIRE, &fallback_reason) || !backend->dispatch_probe_update(p_context, p_probe_flags, probe_pipeline, probe_uniform_set, p_probe_output_buffer, p_ray_count, &fallback_reason) || !_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_RELEASE, &fallback_reason)) {
		if (active_backend != RSE::PT_BACKEND_VULKAN_GENERIC) {
			const RSE::PathtracingBackend failed_backend = active_backend;
			active_backend_fallback_reason = vformat("RTGI backend '%s' failed during STRC probe dispatch: %s", backend_get_name(failed_backend), fallback_reason);
			WARN_PRINT(vformat("%s Falling back to Vulkan Generic.", active_backend_fallback_reason));
			String abort_reason;
			const bool abort_ok = backend->abort_frame(p_context, &abort_reason);
			const bool release_ok = _rtgi_backend_release_after_abort_if_needed(p_context, &abort_reason);
			if (!abort_ok || !release_ok || !p_context.exchange.rd_owns_output_after_dispatch) {
				active_backend_fallback_reason += vformat(" Could not safely return RD ownership: %s", abort_reason);
				ERR_PRINT_ONCE(active_backend_fallback_reason);
				return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
			}
			const RenderDataRD *render_data = p_context.render_data;
			const uint32_t rt_flags = p_context.rt_flags;
			String generic_reason;
			if (_initialize_backend(RSE::PT_BACKEND_VULKAN_GENERIC, &generic_reason)) {
				_activate_backend(RSE::PT_BACKEND_VULKAN_GENERIC);
				RTGIBackend *generic_backend = rtgi_backends[RSE::PT_BACKEND_VULKAN_GENERIC];
				RID generic_probe_pipeline;
				RID generic_probe_uniform_set;
				p_context = RTGIBackendFrameContext();
				p_context.rd = RD::get_singleton();
				p_context.raytracing = this;
				p_context.render_data = render_data;
				p_context.rt_flags = rt_flags;
				fallback_reason = String();
				if (generic_backend != nullptr &&
						prepare_and_validate(generic_backend, p_context, &fallback_reason) &&
						_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_ACQUIRE, &fallback_reason) &&
						generic_backend->prepare_probe_update(p_context, p_probe_flags, p_probe_output_buffer, p_ray_count, generic_probe_pipeline, generic_probe_uniform_set, &fallback_reason) &&
						generic_backend->dispatch_probe_update(p_context, p_probe_flags, generic_probe_pipeline, generic_probe_uniform_set, p_probe_output_buffer, p_ray_count, &fallback_reason) &&
						_rtgi_backend_record_driver_callback(p_context, RTGI_BACKEND_CALLBACK_RELEASE, &fallback_reason)) {
					if (!validate_rd_output_owner(p_context, &fallback_reason)) {
						_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed after STRC probe dispatch: " + fallback_reason);
						ERR_PRINT_ONCE(active_backend_fallback_reason);
						return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
					}
					return RTGI_BACKEND_DISPATCH_OK;
				}
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback failed during STRC probe dispatch: " + fallback_reason);
			} else {
				_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, "Vulkan Generic fallback could not be initialized: " + generic_reason);
			}
		}
		_rtgi_backend_append_fallback_reason(active_backend_fallback_reason, vformat("RTGI backend '%s' failed during STRC probe dispatch: %s", backend_get_name(active_backend), fallback_reason));
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return failure_result();
	}
	if (!validate_rd_output_owner(p_context, &fallback_reason)) {
		active_backend_fallback_reason = vformat("RTGI backend '%s' failed after STRC probe dispatch: %s", backend_get_name(active_backend), fallback_reason);
		ERR_PRINT_ONCE(active_backend_fallback_reason);
		return RTGI_BACKEND_DISPATCH_UNSAFE_FAILURE;
	}

	return RTGI_BACKEND_DISPATCH_OK;
}

RID RenderRaytracing::_ensure_blue_noise_texture() {
	if (blue_noise_texture.is_valid()) {
		return blue_noise_texture;
	}

	Image blue_noise_image(rtgi_blue_noise_128_rgba_png);
	ERR_FAIL_COND_V_MSG(blue_noise_image.is_empty(), RID(), "Failed to decode embedded RTGI blue-noise texture.");
	if (blue_noise_image.get_format() != Image::FORMAT_RGBA8) {
		blue_noise_image.convert(Image::FORMAT_RGBA8);
	}

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.width = blue_noise_image.get_width();
	tf.height = blue_noise_image.get_height();
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;

	blue_noise_texture = RD::get_singleton()->texture_create(tf, RD::TextureView(), Vector<Vector<uint8_t>>{ blue_noise_image.get_data() });
	RD::get_singleton()->set_resource_name(blue_noise_texture, "RTGI Blue Noise 128 RGBA");
	return blue_noise_texture;
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
	if (p_state->emissive_candidate_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->emissive_candidate_buffer);
	}
	if (p_state->emissive_primitive_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->emissive_primitive_buffer);
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
	cpu_geometry_data.clear();
	material_data.clear();
	material_ubo_dependencies.clear();
	geometry_buffer_dependencies.clear();
	deformed_buffer_dependencies.clear();
	motion_indices.clear();
	motion_transforms.clear();
	emissive_candidates.clear();
	emissive_primitive_distributions.clear();
	emissive_candidate_total_weight = 0.0f;
	current_emissive_candidate_signature = _rt_history_mix(0x7274656d69737376ULL, 0);

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

	// Index format (no device address â€” caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

static RTGIBackendCPUGeometry _rt_make_cpu_geometry_from_surface(void *p_mesh_surface, const RT_GeometryData &p_geometry) {
	RTGIBackendCPUGeometry cpu_geometry;
	if (p_mesh_surface == nullptr || p_geometry.vertex_count == 0 || p_geometry.primitive_count == 0) {
		return cpu_geometry;
	}
	if ((p_geometry.flags & (RT_GEOM_FLAG_PROCEDURAL | RT_GEOM_FLAG_DEFORMED | RT_GEOM_FLAG_PRIMITIVE_HISTORY_ID)) != 0) {
		return cpu_geometry;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const Vector<uint8_t> &vertex_data = mesh_storage->mesh_surface_get_rt_vertex_data(p_mesh_surface);
	const Vector<uint8_t> &index_data = mesh_storage->mesh_surface_get_rt_index_data(p_mesh_surface);
	const uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	const bool is_2d = (surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES) != 0;
	const bool compressed = (p_geometry.flags & RT_GEOM_FLAG_COMPRESSED) != 0;
	const uint32_t vertex_stride = p_geometry.position_stride;

	if (vertex_stride == 0 || uint64_t(vertex_stride) * p_geometry.vertex_count > uint64_t(vertex_data.size())) {
		return cpu_geometry;
	}

	cpu_geometry.vertices.resize(p_geometry.vertex_count);
	for (uint32_t i = 0; i < p_geometry.vertex_count; i++) {
		const uint8_t *vertex_ptr = vertex_data.ptr() + uint64_t(i) * vertex_stride;
		if (is_2d) {
			float x;
			float y;
			memcpy(&x, vertex_ptr, sizeof(float));
			memcpy(&y, vertex_ptr + sizeof(float), sizeof(float));
			cpu_geometry.vertices[i] = Vector3(x, y, 0.0f);
		} else if (compressed) {
			const float x = float(decode_uint16(vertex_ptr + 0)) / 65535.0f;
			const float y = float(decode_uint16(vertex_ptr + 2)) / 65535.0f;
			const float z = float(decode_uint16(vertex_ptr + 4)) / 65535.0f;
			cpu_geometry.vertices[i] = Vector3(x, y, z);
		} else {
			float x;
			float y;
			float z;
			memcpy(&x, vertex_ptr, sizeof(float));
			memcpy(&y, vertex_ptr + sizeof(float), sizeof(float));
			memcpy(&z, vertex_ptr + sizeof(float) * 2, sizeof(float));
			cpu_geometry.vertices[i] = Vector3(x, y, z);
		}
	}

	const uint32_t index_count = p_geometry.primitive_count * 3;
	cpu_geometry.indices.resize(index_count);
	if (p_geometry.index_format == RT_INDEX_FORMAT_NONE) {
		if (index_count > p_geometry.vertex_count) {
			cpu_geometry = RTGIBackendCPUGeometry();
			return cpu_geometry;
		}
		for (uint32_t i = 0; i < index_count; i++) {
			cpu_geometry.indices[i] = i;
		}
		cpu_geometry.indexed = false;
	} else if (p_geometry.index_format == RT_INDEX_FORMAT_UINT16) {
		if (uint64_t(index_count) * sizeof(uint16_t) > uint64_t(index_data.size())) {
			cpu_geometry = RTGIBackendCPUGeometry();
			return cpu_geometry;
		}
		for (uint32_t i = 0; i < index_count; i++) {
			cpu_geometry.indices[i] = decode_uint16(index_data.ptr() + uint64_t(i) * sizeof(uint16_t));
		}
		cpu_geometry.indexed = true;
	} else if (p_geometry.index_format == RT_INDEX_FORMAT_UINT32) {
		if (uint64_t(index_count) * sizeof(uint32_t) > uint64_t(index_data.size())) {
			cpu_geometry = RTGIBackendCPUGeometry();
			return cpu_geometry;
		}
		for (uint32_t i = 0; i < index_count; i++) {
			cpu_geometry.indices[i] = decode_uint32(index_data.ptr() + uint64_t(i) * sizeof(uint32_t));
		}
		cpu_geometry.indexed = true;
	} else {
		cpu_geometry = RTGIBackendCPUGeometry();
		return cpu_geometry;
	}

	for (uint32_t index : cpu_geometry.indices) {
		if (index >= p_geometry.vertex_count) {
			cpu_geometry = RTGIBackendCPUGeometry();
			return cpu_geometry;
		}
	}

	cpu_geometry.primitive_count = p_geometry.primitive_count;
	cpu_geometry.valid = true;
	return cpu_geometry;
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
	p_state->tlas_instance_count = valid_instance_count;
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
	update_or_grow(p_state->emissive_candidate_buffer, p_state->emissive_candidate_buffer_capacity,
			emissive_candidates.ptr(), emissive_candidates.size() * sizeof(RT_EmissiveCandidate));
	update_or_grow(p_state->emissive_primitive_buffer, p_state->emissive_primitive_buffer_capacity,
			emissive_primitive_distributions.ptr(), emissive_primitive_distributions.size() * sizeof(RT_EmissivePrimitiveDistribution));
	p_state->emissive_candidate_signature = current_emissive_candidate_signature;
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

	// Layout of uncompressed vertex buffer: [float3 positions Ã— V] + [packed TBN Ã— V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 Ã— uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 Ã— uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer.
	uint32_t src_tbn_byte_offset = vertex_count * 12; // after all float3 positions

	// Merged vertex buffer: [float3 pos Ã— N*V] + [packed TBN Ã— N*V] (if TBN present).
	const uint64_t merged_vtx_bytes_u64 = total_vertices_u64 * 12u + (has_tbn ? total_vertices_u64 * tbn_stride : 0u);
	if (merged_vtx_bytes_u64 > UINT32_MAX) {
		return false;
	}
	uint32_t merged_vtx_bytes = (uint32_t)merged_vtx_bytes_u64;

	// Attribute buffer: attribute_stride bytes Ã— V, replicated N times.
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
		uint32_t raster_gi_flags;
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

	auto emissive_proxy_luminance = [](const RT_MaterialData &p_mat) -> float {
		const float emission_luma = MAX(0.0f, p_mat.emission_color[0] * 0.2126f + p_mat.emission_color[1] * 0.7152f + p_mat.emission_color[2] * 0.0722f) * MAX(p_mat.emission_strength, 0.0f);
		const float texture_floor = (p_mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0 ? 0.20f : 0.0f;
		return MAX(emission_luma, texture_floor);
	};

	auto estimate_emissive_proxy_weight = [&](const Transform3D &p_transform, const RT_GeometryData &p_geom, const RT_MaterialData &p_mat) -> float {
		const float emissive_luma = emissive_proxy_luminance(p_mat);
		if (emissive_luma <= 0.0f || p_geom.primitive_count == 0) {
			return 0.0f;
		}
		const float sx = MAX(p_geom.aabb_size_x, 0.001f);
		const float sy = MAX(p_geom.aabb_size_y, 0.001f);
		const float sz = MAX(p_geom.aabb_size_z, 0.001f);
		const Vector3 wx = p_transform.basis.xform(Vector3(sx, 0.0f, 0.0f));
		const Vector3 wy = p_transform.basis.xform(Vector3(0.0f, sy, 0.0f));
		const Vector3 wz = p_transform.basis.xform(Vector3(0.0f, 0.0f, sz));
		const float proxy_area = MAX(MAX(wx.cross(wy).length(), wy.cross(wz).length()), wz.cross(wx).length());
		return emissive_luma * MAX(proxy_area, 0.001f);
	};

	auto append_emissive_primitive_distribution = [&](void *p_mesh_surface, const Transform3D &p_transform, const RT_GeometryData &p_geom, const RT_MaterialData &p_mat, RT_EmissiveCandidate &r_candidate) -> float {
		const float emissive_luma = emissive_proxy_luminance(p_mat);
		if (p_mesh_surface == nullptr || emissive_luma <= 0.0f || emissive_primitive_distributions.size() >= RTGI_MAX_EMISSIVE_PRIMITIVE_DISTRIBUTIONS) {
			return 0.0f;
		}

		const RTGIBackendCPUGeometry cpu_geometry = _rt_make_cpu_geometry_from_surface(p_mesh_surface, p_geom);
		if (!cpu_geometry.valid || cpu_geometry.primitive_count == 0) {
			return 0.0f;
		}

		const uint32_t available_entries = RTGI_MAX_EMISSIVE_PRIMITIVE_DISTRIBUTIONS - (uint32_t)emissive_primitive_distributions.size();
		const uint32_t distribution_count = MIN(MIN(cpu_geometry.primitive_count, RTGI_MAX_EMISSIVE_PRIMITIVES_PER_CANDIDATE), available_entries);
		if (distribution_count == 0) {
			return 0.0f;
		}

		const uint32_t primitive_offset = (uint32_t)emissive_primitive_distributions.size();
		float cumulative_weight = 0.0f;
		for (uint32_t sample_idx = 0; sample_idx < distribution_count; sample_idx++) {
			const uint32_t bucket_begin = (uint32_t)((uint64_t)sample_idx * cpu_geometry.primitive_count / distribution_count);
			const uint32_t bucket_end = (uint32_t)((uint64_t)(sample_idx + 1u) * cpu_geometry.primitive_count / distribution_count);
			const uint32_t represented_primitives = MAX(bucket_end - bucket_begin, 1u);
			const uint32_t primitive_id = MIN(bucket_begin + represented_primitives / 2u, cpu_geometry.primitive_count - 1u);
			const uint32_t index_offset = primitive_id * 3u;
			if (index_offset + 2u >= cpu_geometry.indices.size()) {
				continue;
			}

			const Vector3 wp0 = p_transform.xform(cpu_geometry.vertices[cpu_geometry.indices[index_offset]]);
			const Vector3 wp1 = p_transform.xform(cpu_geometry.vertices[cpu_geometry.indices[index_offset + 1u]]);
			const Vector3 wp2 = p_transform.xform(cpu_geometry.vertices[cpu_geometry.indices[index_offset + 2u]]);
			const float tri_area = (wp1 - wp0).cross(wp2 - wp0).length() * 0.5f;
			if (tri_area <= 0.00000001f) {
				continue;
			}

			cumulative_weight += tri_area * emissive_luma * (float)represented_primitives;
			RT_EmissivePrimitiveDistribution primitive_distribution = {};
			primitive_distribution.primitive_id = primitive_id;
			primitive_distribution.cumulative_weight = cumulative_weight;
			primitive_distribution.area = tri_area;
			emissive_primitive_distributions.push_back(primitive_distribution);
		}

		const uint32_t primitive_count = (uint32_t)emissive_primitive_distributions.size() - primitive_offset;
		if (cumulative_weight <= 0.0f || primitive_count == 0) {
			emissive_primitive_distributions.resize(primitive_offset);
			return 0.0f;
		}

		r_candidate.primitive_offset = primitive_offset;
		r_candidate.primitive_count = primitive_count;
		r_candidate.primitive_weight_sum = cumulative_weight;
		r_candidate.flags |= RT_EMISSIVE_CANDIDATE_FLAG_PRIMITIVE_DISTRIBUTION;
		return cumulative_weight;
	};

	auto add_emissive_candidate = [&](const Transform3D &p_transform, uint32_t p_geometry_index, const RT_GeometryData &p_geom, const RT_MaterialData &p_mat, uint32_t p_sbt_offset, uint8_t p_instance_mask, void *p_mesh_surface, bool p_two_sided = false) {
		if ((p_instance_mask & RT_INSTANCE_MASK_VISIBLE) == 0 || p_sbt_offset != 0 || emissive_candidates.size() >= RTGI_MAX_EMISSIVE_CANDIDATES) {
			return;
		}
		const uint32_t rejected_geom_flags = RT_GEOM_FLAG_PROCEDURAL | RT_GEOM_FLAG_DEFORMED | RT_GEOM_FLAG_RASTER_GI_LIGHTMAP | RT_GEOM_FLAG_RASTER_GI_LIGHTMAP_CAPTURE | RT_GEOM_FLAG_RASTER_GI_VOXELGI | RT_GEOM_FLAG_RASTER_GI_SDFGI;
		if ((p_geom.flags & rejected_geom_flags) != 0 || p_geom.vertex_buffer_address == 0 || p_geom.primitive_count == 0) {
			return;
		}
		const uint32_t rejected_mat_flags = RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_ALPHA_HASH | RT_MAT_FLAG_CUSTOM_ALPHA_CLIP | RT_MAT_FLAG_ALPHA_TEST;
		if ((p_mat.flags & rejected_mat_flags) != 0) {
			return;
		}
		RT_EmissiveCandidate candidate = {};
		RendererRD::MaterialStorage::store_transform_transposed_3x4(p_transform, candidate.object_to_world);
		candidate.geometry_index = p_geometry_index;
		candidate.flags = 0;
		if ((p_geom.flags & RT_GEOM_FLAG_COMPRESSED) != 0) {
			candidate.flags |= RT_EMISSIVE_CANDIDATE_FLAG_COMPRESSED_GEOMETRY;
		}
		if ((p_mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0) {
			candidate.flags |= RT_EMISSIVE_CANDIDATE_FLAG_TEXTURED_EMISSION;
		}

		float weight = append_emissive_primitive_distribution(p_mesh_surface, p_transform, p_geom, p_mat, candidate);
		if (weight <= 0.0f) {
			weight = estimate_emissive_proxy_weight(p_transform, p_geom, p_mat);
		}
		if (weight <= 0.0f) {
			return;
		}
		candidate.selection_weight = weight;
		emissive_candidates.push_back(candidate);
		geometry_data[p_geometry_index].flags |= RT_GEOM_FLAG_EXPLICIT_EMISSIVE_CANDIDATE;
		if (p_two_sided) {
			geometry_data[p_geometry_index].flags |= RT_GEOM_FLAG_TWO_SIDED;
		}
		emissive_candidate_total_weight += weight;
		current_emissive_candidate_signature = _rt_history_mix(current_emissive_candidate_signature, p_geometry_index);
		current_emissive_candidate_signature = _rt_history_mix(current_emissive_candidate_signature, p_mat.flags);
		current_emissive_candidate_signature = _rt_history_mix(current_emissive_candidate_signature, p_mat.emission_texture_idx);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_mat.emission_color[0]);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_mat.emission_color[1]);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_mat.emission_color[2]);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_mat.emission_strength);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, weight);
		current_emissive_candidate_signature = _rt_history_mix(current_emissive_candidate_signature, candidate.flags);
		current_emissive_candidate_signature = _rt_history_mix(current_emissive_candidate_signature, candidate.primitive_count);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, candidate.primitive_weight_sum);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_transform.origin.x);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_transform.origin.y);
		current_emissive_candidate_signature = _rt_history_mix_float(current_emissive_candidate_signature, p_transform.origin.z);
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
		uint32_t raster_gi_flags = 0;
		if (inst->lightmap_instance.is_valid()) {
			raster_gi_flags = RT_GEOM_FLAG_RASTER_GI_LIGHTMAP;
		} else if (inst->lightmap_sh) {
			raster_gi_flags = RT_GEOM_FLAG_RASTER_GI_LIGHTMAP_CAPTURE;
		} else if (inst->voxel_gi_instances[0].is_valid()) {
			raster_gi_flags = RT_GEOM_FLAG_RASTER_GI_VOXELGI;
		} else if (inst->can_sdfgi && p_render_data->environment.is_valid() &&
				RendererEnvironmentStorage::get_singleton()->environment_get_sdfgi_enabled(p_render_data->environment)) {
			raster_gi_flags = RT_GEOM_FLAG_RASTER_GI_SDFGI;
		}

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
				geom.flags = RT_GEOM_FLAG_PROCEDURAL | raster_gi_flags;
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
					cpu_geometry_data.push_back(RTGIBackendCPUGeometry());

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
				pending.raster_gi_flags = raster_gi_flags;
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
			RT_GeometryData rt_geometry = surf_data->geometry;
			rt_geometry.flags |= raster_gi_flags;
			const RTGIBackendCPUGeometry surface_cpu_geometry = (active_backend == RSE::PT_BACKEND_INTEL_EMBREE) ? _rt_make_cpu_geometry_from_surface(mesh_surface, rt_geometry) : RTGIBackendCPUGeometry();

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
			history_key = _rt_history_mix(history_key, rt_geometry.flags);

			uint32_t pushed_entries = 0;
			auto push_mesh_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
				if (p_instance_mask == 0) {
					return;
				}
				const uint32_t geometry_index = geometry_data.size();
				const RT_MaterialData surface_material = get_surface_material_data_with_sbt(surf, mat_data, rt_sbt_offset);
				blass.push_back(surf_data->blas);
				blas_transforms.push_back(final_transform);
				const bool mesh_history_invalid = transform_teleported || deformed_history_invalid || custom_temporal_unsupported;
				geometry_data.push_back(_rt_geometry_with_history_validity(state, rt_geometry, history_key, inst->layer_mask, p_instance_mask, mesh_history_invalid));
				cpu_geometry_data.push_back(surface_cpu_geometry);

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
				material_data.push_back(surface_material);
				instance_flags.push_back(p_inst_flags);
				instance_masks.push_back(p_instance_mask);
				const bool surface_two_sided = surf->shader && surf->shader->rt_cull_mode() == RSE::CULL_MODE_DISABLED;
				add_emissive_candidate(final_transform, geometry_index, rt_geometry, surface_material, rt_sbt_offset, p_instance_mask, mesh_surface, surface_two_sided);
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
	// Phase 2: GPU compute â€” merged MultiMesh BLAS dispatches.
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
			merged_geometry.flags |= RT_GEOM_FLAG_PRIMITIVE_HISTORY_ID | pending.raster_gi_flags;
			uint64_t history_key = _rt_history_mix(pending.history_key, merged_geometry.flags);
			uint32_t pushed_entries = 0;
			auto push_merged_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
				if (p_instance_mask == 0) {
					return;
				}
				const uint32_t geometry_index = geometry_data.size();
				const RT_MaterialData surface_material = get_surface_material_data_with_sbt(pending.mm_surf, pending.mat_data, pending.rt_sbt_offset);
				blass.push_back(merged_sd.blas);
				blas_transforms.push_back(pending.instance_transform);
				geometry_data.push_back(_rt_geometry_with_history_validity(state, merged_geometry, history_key, pending.layer_mask, p_instance_mask, pending.history_invalid));
				cpu_geometry_data.push_back(RTGIBackendCPUGeometry());
				sbt_offsets.push_back(pending.rt_sbt_offset);
				material_data.push_back(surface_material);
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
				const bool mm_merged_two_sided = pending.mm_surf->shader && pending.mm_surf->shader->rt_cull_mode() == RSE::CULL_MODE_DISABLED;
				add_emissive_candidate(pending.instance_transform, geometry_index, merged_geometry, surface_material, pending.rt_sbt_offset, p_instance_mask, nullptr, mm_merged_two_sided);
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
			RT_GeometryData rt_geometry = surf_data->geometry;
			rt_geometry.flags |= pending.raster_gi_flags;
			const RTGIBackendCPUGeometry surface_cpu_geometry = (active_backend == RSE::PT_BACKEND_INTEL_EMBREE) ? _rt_make_cpu_geometry_from_surface(pending.mesh_surface, rt_geometry) : RTGIBackendCPUGeometry();

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
				history_key = _rt_history_mix(history_key, rt_geometry.flags);

				auto push_mm_instance_entry = [&](uint8_t p_instance_mask, uint32_t p_inst_flags) {
					if (p_instance_mask == 0) {
						return;
					}
					const uint32_t geometry_index = geometry_data.size();
					const RT_MaterialData surface_material = get_surface_material_data_with_sbt(pending.mm_surf, pending.mat_data, pending.rt_sbt_offset);
					blass.push_back(surf_data->blas);
					blas_transforms.push_back(final_transform);
					geometry_data.push_back(_rt_geometry_with_history_validity(state, rt_geometry, history_key, pending.layer_mask, p_instance_mask, pending.history_invalid));
					cpu_geometry_data.push_back(surface_cpu_geometry);
					sbt_offsets.push_back(pending.rt_sbt_offset);
					material_data.push_back(surface_material);

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
					const bool mm_expanded_two_sided = pending.mm_surf->shader && pending.mm_surf->shader->rt_cull_mode() == RSE::CULL_MODE_DISABLED;
					add_emissive_candidate(final_transform, geometry_index, rt_geometry, surface_material, pending.rt_sbt_offset, p_instance_mask, pending.mesh_surface, mm_expanded_two_sided);
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

void RenderRaytracing::populate_backend_scene_resources(RTViewportState *p_state, RTGIBackendSceneResources &r_resources) const {
	r_resources = RTGIBackendSceneResources();
	if (p_state == nullptr) {
		return;
	}

	r_resources.tlas = p_state->tlas;
	r_resources.tlas_instance_count = p_state->tlas_instance_count;
	r_resources.blases.reserve(blass.size());
	for (const RID &blas : blass) {
		r_resources.blases.push_back(blas);
	}

	r_resources.geometry_buffer = p_state->geometry_buffer;
	r_resources.geometry_count = geometry_data.size();
	r_resources.material_buffer = p_state->material_buffer;
	r_resources.material_count = material_data.size();
	r_resources.motion_index_buffer = p_state->motion_index_buffer;
	r_resources.motion_index_count = motion_indices.size();
	r_resources.motion_transform_buffer = p_state->motion_transform_buffer;
	r_resources.motion_transform_count = motion_transforms.size();
	r_resources.emissive_candidate_buffer = p_state->emissive_candidate_buffer;
	r_resources.emissive_candidate_count = emissive_candidates.size();
	r_resources.emissive_candidate_total_weight = emissive_candidate_total_weight;
	r_resources.emissive_candidate_signature = p_state->emissive_candidate_signature;
	r_resources.light_buffer = p_state->light_buffer;
	r_resources.light_count = p_state->previous_light_count;
	r_resources.params_buffer = p_state->params_buffer;
}

void RenderRaytracing::populate_backend_scene_snapshot(RTViewportState *p_state, RTGIBackendSceneSnapshot &r_snapshot) const {
	r_snapshot = RTGIBackendSceneSnapshot();
	if (p_state == nullptr) {
		return;
	}

	r_snapshot.tlas = p_state->tlas;
	r_snapshot.tlas_instance_count = p_state->tlas_instance_count;
	r_snapshot.blases = blass;
	r_snapshot.blas_transforms = blas_transforms;
	r_snapshot.instance_flags = instance_flags;
	r_snapshot.instance_masks = instance_masks;
	r_snapshot.sbt_offsets = sbt_offsets;
	r_snapshot.geometries = geometry_data;
	r_snapshot.cpu_geometries = cpu_geometry_data;
	r_snapshot.materials = material_data;
	r_snapshot.material_uniform_buffers = material_ubo_dependencies;
	r_snapshot.motion_indices = motion_indices;
	r_snapshot.motion_transforms = motion_transforms;
	r_snapshot.emissive_candidates = emissive_candidates;
	r_snapshot.emissive_primitive_distributions = emissive_primitive_distributions;
	r_snapshot.emissive_candidate_total_weight = emissive_candidate_total_weight;
	r_snapshot.emissive_candidate_signature = p_state->emissive_candidate_signature;
	r_snapshot.radiance_history_signature = p_state->radiance_history_signature;
	r_snapshot.radiance_history_signature_valid = p_state->radiance_history_signature_valid;
	r_snapshot.radiance_history_invalidated = p_state->radiance_history_invalidated;
	r_snapshot.lights.resize(p_state->previous_light_count);
	for (uint32_t i = 0; i < p_state->previous_light_count; i++) {
		r_snapshot.lights[i] = p_state->previous_light_data[i];
	}
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
			} else if (p_type == RSE::LIGHT_AREA) {
				e *= 1.0f / (Math::PI * 2.0f); // matches raster light_storage.cpp area-light branch
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
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f; // Matches rasterizer convention, normalizes 0.5 default to 1.0.
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
		ld.source_id = _rt_light_source_id(light_instance, RSE::LIGHT_DIRECTIONAL);
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

		if (type == RSE::LIGHT_AREA) {
			ld.type = RT_LIGHT_TYPE_AREA;
			Vector2 area_size = ls->light_area_get_size(base);
			// Half-edge vectors from the light's local X/Y axes (matches raster
			// area_width/area_height at light_storage.cpp:1075-1076, which use the
			// normalized basis axes scaled by the full size; we store halves).
			Vector3 ex = xform.basis.get_column(0).normalized() * (area_size.x * 0.5f);
			Vector3 ey = xform.basis.get_column(1).normalized() * (area_size.y * 0.5f);
			ld.spot_direction[0] = ex.x; ld.spot_direction[1] = ex.y; ld.spot_direction[2] = ex.z;
			ld.radius = ey.x; ld.inv_spot_attenuation = ey.y; ld.cos_spot_angle = ey.z;

			Color linear_col = ls->light_get_color(base).srgb_to_linear();
			float energy = compute_light_energy(base, type) * fade;
			if (ls->light_area_get_normalize_energy(base)) {
				float surface_area = MAX(area_size.x * area_size.y, 1e-6f);
				energy /= surface_area;
			}
			ld.emission[0] = linear_col.r * energy;
			ld.emission[1] = linear_col.g * energy;
			ld.emission[2] = linear_col.b * energy;

			ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
			float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
			if (range > 0.0f) { ld.inv_max_range = 1.0f / range; ld.max_range_squared = range * range; }
			else { ld.inv_max_range = -1.0f; ld.max_range_squared = 0.0f; }
			ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;
			ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
			ld.shadow_opacity = compute_light_shadow_opacity(base, camera_distance);
			ld.shadow_max_distance = 0.0f;
			ld.flags = (ls->light_has_shadow(base) && ld.shadow_opacity > 0.001f) ? uint32_t(RT_LIGHT_FLAG_SHADOW) : 0u;
			ld.cull_mask = ls->light_get_cull_mask(base);
			ld.shadow_caster_mask = ls->light_get_shadow_caster_mask(base);
			ld.source_id = _rt_light_source_id(light_instance, type);
			RID area_tex = ls->light_area_get_texture(base);
			if (area_tex.is_valid()) {
				RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
				Rect2 rect = ts->area_light_atlas_get_texture_rect(area_tex);
				ld.area_atlas_rect[0] = rect.position.x;
				ld.area_atlas_rect[1] = rect.position.y;
				ld.area_atlas_rect[2] = rect.size.width;
				ld.area_atlas_rect[3] = rect.size.height;
				ld.area_atlas_idx = bindless_block->add_texture(ts->area_light_atlas_get_texture());
				Size2i tex_px = (rect.size * Size2(ts->area_light_atlas_get_size())).ceil();
				float max_dim = MAX(tex_px.x, tex_px.y);
				ld.area_max_mip = MAX(0.0f, MIN(Math::floor(Math::log2(MAX(max_dim, 1.0f))), (float)ts->area_light_atlas_get_mipmaps()) - 1.0f);
			} else {
				ld.area_atlas_idx = 0u;
				ld.area_atlas_rect[0] = 0.0f; ld.area_atlas_rect[1] = 0.0f;
				ld.area_atlas_rect[2] = 0.0f; ld.area_atlas_rect[3] = 0.0f;
				ld.area_max_mip = 0.0f;
			}
			ld.area_pad0 = 0u; ld.area_pad1 = 0u;
			rt_light_count++;
			continue;
		}

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
		ld.source_id = _rt_light_source_id(light_instance, type);
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
	bool rt_external_memory_exportable = false;
	if (rb_data->rt_has_texture()) {
		rt_external_memory_exportable = RD::get_singleton()->texture_get_format(rb_data->rt_get_texture()).is_external_memory_exportable;
	}
	rb_data->rt_ensure_textures(rt_external_memory_exportable);

	// SET 0 indices must match raytracing_common_inc.glsl / scene_raytracing_raygen.glsl / samplers includes.
	Vector<RD::Uniform> uniforms;
	uint64_t uniform_signature = _rt_history_mix(0x7274756e69666f72ULL, p_rt_flags);
	RID default_storage_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer();
	// Writable-binding fallback (distinct RID) — see binding 107 below for why a
	// separate read-write default is required.
	RID default_rw_storage_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rw_rd_storage_buffer();
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

	// Binding 44: bounded emissive surface/proxy candidates for RTGI direct sampling.
	{
		RD::Uniform u;
		u.binding = 44;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->emissive_candidate_buffer.is_valid()) {
			add_uniform_id(u, p_state->emissive_candidate_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 76: per-candidate emissive primitive CDF entries.
	{
		RD::Uniform u;
		u.binding = 76;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->emissive_primitive_buffer.is_valid()) {
			add_uniform_id(u, p_state->emissive_primitive_buffer);
		} else {
			add_uniform_id(u, default_storage_buffer);
		}
		uniforms.push_back(u);
	}

		// Binding 6: Raytracing params + unjittered VP matrices.
	{
		struct {
			float params[SceneShaderRaytracing::RT_PARAM_SHADER_FLOAT_COUNT];
			float prev_vp_unjittered[16];
			float curr_vp_unjittered[16];
			float inv_projection_unjittered[16];
			float rt_view_rect[4];
			float rt_prev_view_rect[4];
			float rt_jitter[4];
		} rt_ubo = {};
		// 52 params + 3 mat4 (48) + 3 vec4 (12) = 112 floats. params[] grew from 48 to 52
		// (RT_PARAM_SHADER_FLOAT_COUNT) for the WRC producer-owned params (indices 45..48);
		// the prior 40 -> 48 growth added the SPG params. Keep this in lock-step with the
		// GLSL `vec4 rt_params[13]` declaration in raytracing_common_inc.glsl. The brace-init
		// above zero-fills all params[] (incl. the new WRC slots) just like every other slot.
		static_assert(sizeof(rt_ubo) == 112 * sizeof(float));

		if (p_render_data && p_render_data->environment.is_valid()) {
			const RSE::PathtracingParams *env_params = RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_params_ptr(p_render_data->environment);
			if (env_params) {
				RSE::pathtracing_params_to_shader_floats(*env_params, rt_ubo.params, MIN((uint32_t)RSE::PT_PARAM_MAX, SceneShaderRaytracing::RT_PARAM_SHADER_FLOAT_COUNT));
			}
		}
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_BACKEND] = float(active_backend);
		// WRC probe-update needs the WRC's own clipmap values (which the WRC atlas was
		// sized from) for probe-addressing. A3-T9 moved these off the borrowed STRC slots
		// onto the WRC producer-owned RT_PARAM_RTGI_WRC_* slots, so the STRC slots are no
		// longer touched by any radiance_probes path (the STRC effect can be deleted later
		// without touching this producer). The dispatch site channels these values through
		// RTViewportState; write them here gated on the WRC flag (wrc_grid > 0 is a
		// belt-and-suspenders sentinel). Values are unchanged from the prior STRC-slot
		// borrow, so the WRC producer stays byte-identical.
		if ((p_rt_flags & SceneShaderRaytracing::RT_FLAG_WRC_PROBE_UPDATE) != 0 && p_state->wrc_grid > 0u) {
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_GRID] = float(p_state->wrc_grid);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_CASCADE_COUNT] = float(p_state->wrc_cascade_count);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_BASE_SPACING] = p_state->wrc_base_spacing;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_RAYS] = float(p_state->wrc_rays_per_frame);
		}
		// SPG gather (A2-T2) writes (a) the WRC producer-owned grid/cascade/spacing slots so
		// the WRC radiance query inside the gather addresses the SAME atlas the WRC was sized
		// from (mirrors the WRC override above, sourced from the same wrc_* fields the dispatch
		// site set; A3-T9 migrated these off the borrowed STRC slots, values unchanged), AND
		// (b) the dedicated RT_PARAM_RTGI_SPG_* slots for the gather's own grid/oct/dir budget
		// + WRC-query oct_res. Gated on the SPG flag + spg_grid_w > 0 sentinel so STRC/WRC/main
		// dispatches stay byte-identical.
		if ((p_rt_flags & SceneShaderRaytracing::RT_FLAG_SPG_GATHER) != 0 && p_state->spg_grid_w > 0u) {
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_GRID] = float(p_state->wrc_grid);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_CASCADE_COUNT] = float(p_state->wrc_cascade_count);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_WRC_BASE_SPACING] = p_state->wrc_base_spacing;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_GRID_W] = float(p_state->spg_grid_w);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_GRID_H] = float(p_state->spg_grid_h);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_OCT_RES] = float(p_state->spg_oct_res);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_DIRS_PER_FRAME] = float(p_state->spg_dirs_per_frame);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_FALLBACK_CONF] = p_state->spg_fallback_conf;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_RTGI_SPG_WRC_OCT_RES] = float(p_state->spg_wrc_oct_res);
		}

		// rt_params layout (see RaytracingParamIndex enum):
		// [0] = VIS_MODE, [1] = SAMPLE_COUNT, [2] = MAX_BOUNCES,
		// [3] = DENOISER, [5] and [11] = reserved,
		// [14] = LIGHT_COUNT, [15] = FRAME_INDEX,
		// [16-19] = RTGI denoiser controls, [20] = RAY_FIREFLY_SUPPRESSION,
		// [21] = RAY_MAX_RADIANCE, [22-24] = split-signal denoiser controls.
		const uint32_t rt_frame_index = p_state->frame_counter++;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRAME_INDEX] = float(rt_frame_index);

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
		rt_ubo.rt_view_rect[0] = (float)rt_visible_origin.x;
		rt_ubo.rt_view_rect[1] = (float)rt_visible_origin.y;
		rt_ubo.rt_view_rect[2] = (float)rt_visible_size.x;
		rt_ubo.rt_view_rect[3] = (float)rt_visible_size.y;
		rt_ubo.rt_prev_view_rect[0] = (float)rt_prev_visible_origin.x;
		rt_ubo.rt_prev_view_rect[1] = (float)rt_prev_visible_origin.y;
		rt_ubo.rt_prev_view_rect[2] = (float)rt_size.x;
		rt_ubo.rt_prev_view_rect[3] = (float)rt_size.y;

		Size2i internal_size = p_render_data && p_render_data->render_buffers.is_valid() ? p_render_data->render_buffers->get_internal_size() : rt_visible_size;
		internal_size.x = MAX(internal_size.x, 1);
		internal_size.y = MAX(internal_size.y, 1);
		const Vector2 source_from_internal_scale(
				(float)MAX(rt_visible_size.x, 1) / (float)internal_size.x,
				(float)MAX(rt_visible_size.y, 1) / (float)internal_size.y);
		const Vector2 taa_jitter_pixels = p_render_data ? p_render_data->scene_data->taa_jitter * Vector2(internal_size) * 0.5f : Vector2();
		const bool has_camera_taa_jitter = Math::abs(taa_jitter_pixels.x) > 0.00001f || Math::abs(taa_jitter_pixels.y) > 0.00001f;
		const bool low_resolution_rtgi = rt_visible_size != internal_size;
		// Projection jitter shifts the rendered sample in the opposite unjittered screen direction.
		Vector2 source_sample_jitter = taa_jitter_pixels * source_from_internal_scale * -1.0f;
		Vector2 explicit_raygen_jitter;
		if (!has_camera_taa_jitter && low_resolution_rtgi) {
			source_sample_jitter = Vector2(_rt_halton_value(rt_frame_index & 15u, 2u), _rt_halton_value(rt_frame_index & 15u, 3u)) * 0.5f * source_from_internal_scale;
			explicit_raygen_jitter = source_sample_jitter;
		}
		rb_data->rt_set_source_sample_jitter(source_sample_jitter);
		rt_ubo.rt_jitter[0] = source_sample_jitter.x;
		rt_ubo.rt_jitter[1] = source_sample_jitter.y;
		rt_ubo.rt_jitter[2] = explicit_raygen_jitter.x;
		rt_ubo.rt_jitter[3] = explicit_raygen_jitter.y;

		// --- Light gathering ---
		uint32_t rt_light_count = 0;
		RT_LightData rt_light_data[RT_LIGHTS_MAX] = {};

		rt_light_count = gather_lights(p_render_data, rt_light_data, RT_LIGHTS_MAX);

		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_LIGHT_COUNT] = float(rt_light_count);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_EMISSIVE_CANDIDATE_COUNT] = float(MIN((uint32_t)emissive_candidates.size(), RTGI_MAX_EMISSIVE_CANDIDATES));
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_EMISSIVE_CANDIDATE_TOTAL_WEIGHT] = emissive_candidate_total_weight;

		uint64_t radiance_signature = _rt_radiance_signature(p_rt_flags, p_render_data ? p_render_data->environment : RID(), p_render_data ? p_render_data->camera_attributes : RID(), rt_ubo.params, background_color, background_uses_sky, rt_light_data, rt_light_count);
		radiance_signature = _rt_history_mix(radiance_signature, p_state->emissive_candidate_signature);
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

	// Binding 36: RTGI diffuse radiance split output.
	{
		RD::Uniform u;
		u.binding = 36;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_diffuse_radiance());
		uniforms.push_back(u);
	}

	// Binding 37: RTGI specular/source radiance split output.
	{
		RD::Uniform u;
		u.binding = 37;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_specular_radiance());
		uniforms.push_back(u);
	}

	// Binding 38: RTGI specular guide output.
	{
		RD::Uniform u;
		u.binding = 38;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_specular_guide());
		uniforms.push_back(u);
	}

	// Binding 63: RTGI specular virtual reprojection output.
	{
		RD::Uniform u;
		u.binding = 63;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_specular_reprojection());
		uniforms.push_back(u);
	}

	// A4: the full-screen FPT primary-direct dispatch (RT_FLAG_PRIMARY_DIRECT) binds the
	// WRC/SPG ray-result buffers (107/108) to the *default* RW buffer instead of the real
	// probe buffers. The primary-direct raygen never touches 107/108, but binding the real
	// buffers RW would record this full-screen dispatch as a phantom WRITER of the probe
	// buffers in the draw graph -- the A4 coexistence root cause (it zeroed the gather /
	// GPU-hung). Pointing them at the default buffer removes that phantom dependency while
	// keeping the descriptor layout the pipeline expects.
	const bool primary_direct_dispatch = (p_rt_flags & SceneShaderRaytracing::RT_FLAG_PRIMARY_DIRECT) != 0;

	// Binding 107: RTGI World Radiance Cache probe-update ray-result buffer.
	// Mirrors the STRC binding-66 wiring; written by the WRC probe-update raygen
	// when RT_FLAG_WRC_PROBE_UPDATE is set. Falls back to the dedicated *writable*
	// default buffer when the WRC effect has no results buffer (e.g. the legacy
	// pipeline, which never allocates one). The fallback MUST be the read-write
	// default, not the shared read-only one: this binding is writable, so reusing
	// the read-only default would record that RID with both READ and READ_WRITE
	// usage in the raytracing list and trip the render-graph single-usage assert
	// whenever a read-only binding also falls back to the default the same frame.
	{
		RD::Uniform u;
		u.binding = 107;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (!primary_direct_dispatch && owner->rtgi_wrc && owner->rtgi_wrc->get_ray_result_buffer().is_valid()) {
			add_uniform_id(u, owner->rtgi_wrc->get_ray_result_buffer());
		} else {
			add_uniform_id(u, default_rw_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Binding 108: RTGI Screen Probe Gather (SPG) gather ray-result buffer (A2-T2).
	// Written by the SPG gather raygen when RT_FLAG_SPG_GATHER is set; read by the T3
	// accumulate. Mirrors the WRC binding-107 wiring, including the *writable* default-
	// buffer fallback (NOT the read-only default) for the same single-usage reason
	// documented on binding 107, and the A4 primary-direct decoupling documented above.
	{
		RD::Uniform u;
		u.binding = 108;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (!primary_direct_dispatch && owner->rtgi_spg && owner->rtgi_spg->get_ray_result_buffer().is_valid()) {
			add_uniform_id(u, owner->rtgi_spg->get_ray_result_buffer());
		} else {
			add_uniform_id(u, default_rw_storage_buffer);
		}
		uniforms.push_back(u);
	}

	// Bindings 109-112: SPG gather combined-sampler inputs (GLSL `sampler2D`, so each is
	// UNIFORM_TYPE_SAMPLER_WITH_TEXTURE = sampler + texture, mirroring the WRC GI consumer
	// which the gather's rtgi_wrc_sample_radiance() query is shared with). 109/110 = the
	// SPG probe headers (plane: world-pos + linear-depth; aux: oct-normal + motion) from
	// run_placement, point-sampled (per-probe texelFetch) so a NEAREST sampler. 111/112 =
	// the WRC radiance + distance atlases the cold-cache gather queries with bilinear
	// textureLod taps, so a LINEAR clamp sampler (identical to render_gi_debug). Each
	// texture falls back to a default when its effect is null / not yet allocated; the
	// gather only runs with the SPG flag + valid resources, so the fallbacks are bound
	// only on the unrelated dispatches that never sample them.
	{
		RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID default_black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

		RID spg_header_plane = (owner->rtgi_spg && owner->rtgi_spg->get_header_plane().is_valid()) ? owner->rtgi_spg->get_header_plane() : default_black;
		RID spg_header_aux = (owner->rtgi_spg && owner->rtgi_spg->get_header_aux().is_valid()) ? owner->rtgi_spg->get_header_aux() : default_black;
		RID wrc_radiance = (owner->rtgi_wrc && owner->rtgi_wrc->get_radiance_atlas().is_valid()) ? owner->rtgi_wrc->get_radiance_atlas() : default_black;
		RID wrc_distance = (owner->rtgi_wrc && owner->rtgi_wrc->get_distance_atlas().is_valid()) ? owner->rtgi_wrc->get_distance_atlas() : default_black;

		auto add_combined = [&](uint32_t p_binding, RID p_sampler, RID p_texture) {
			RD::Uniform u;
			u.binding = p_binding;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.append_id(p_sampler);
			u.append_id(p_texture);
			signature_add(p_sampler);
			signature_add(p_texture);
			uniforms.push_back(u);
		};
		add_combined(109, nearest_sampler, spg_header_plane);
		add_combined(110, nearest_sampler, spg_header_aux);
		add_combined(111, linear_sampler, wrc_radiance);
		add_combined(112, linear_sampler, wrc_distance);

		// 113/114/115: raw material-guide albedo + ORM + emission (the same RB_TEX_RT_GUIDE_*
		// the FPT composite/prepass produce). The FPT-fast primary-direct path point-samples these
		// (full-res, 1:1 with the launch) to build the NEE material from real reflectance instead of
		// the hue-proxy albedo, and to add first-hit emission (115) for directly-visible emitters.
		// NEAREST samplers (texelFetch). Fall back to default-black when the guides are not yet
		// allocated; only the primary-direct dispatch ever samples them.
		// Probe existence with has_texture (rt_has_material_guides), NOT the erroring get_texture: in
		// REFLECTIONS_RT_ONLY and before the material-guide prepass runs the guides are not allocated,
		// and calling get_texture on a missing key would flood the log every frame. Only the
		// primary-direct dispatch samples them; otherwise default-black is correct.
		const bool rt_guides_ready = rb_data->rt_has_material_guides();
		RID guide_albedo = rt_guides_ready ? rb_data->rt_get_guide_albedo() : default_black;
		RID guide_orm = rt_guides_ready ? rb_data->rt_get_guide_orm() : default_black;
		RID guide_emission = rt_guides_ready ? rb_data->rt_get_guide_emission() : default_black;
		add_combined(113, nearest_sampler, guide_albedo);
		add_combined(114, nearest_sampler, guide_orm);
		add_combined(115, nearest_sampler, guide_emission);

		// 116: relief-FREE geometric guide normal (RB_TEX_RT_GUIDE_NORMAL, the same texture
		// rtgi_gi_resolve binds at its binding 17). The FPT-fast primary-direct path point-samples it
		// (NEAREST, full-res, 1:1 with the launch) to bend the relief shading normal toward the macro
		// surface in grazing-lit normal-mapped crevices that would otherwise hard-zero to a black vein.
		// ALWAYS bind (the shared set-0 layout must stay valid for every dispatch); fall back to the
		// neutral default-white when the guides are not yet allocated -- enc*2-1 = (1,1,1) is still
		// finite, so the in-shader degenerate guard falls back to the relief world_N. rt_has_material_guides()
		// (above) already verifies RB_TEX_RT_GUIDE_NORMAL exists, so the getter never floods the log.
		RID default_white = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
		RID guide_normal = rt_guides_ready ? rb_data->rt_get_guide_normal() : default_white;
		add_combined(116, nearest_sampler, guide_normal);
	}

	// Binding 60: RTGI specular reflection-direction diagnostic output.
	{
		RD::Uniform u;
		u.binding = 60;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_specular_reflection_direction());
		uniforms.push_back(u);
	}

	// Bindings 61-62: Tiled 128x128 blue-noise seed texture and sampler.
	{
		RD::Uniform u;
		u.binding = 61;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		add_uniform_id(u, _ensure_blue_noise_texture());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 62;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		add_uniform_id(u, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
								  RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED));
		uniforms.push_back(u);
	}

	// Bindings 71-74: raster G-buffer inputs used by Hybrid RTGI raygen.
	{
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		RID raster_depth = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		RID raster_normal_roughness = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
		RID raster_color = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		if (p_render_data && p_render_data->render_buffers.is_valid()) {
			RID depth = p_render_data->render_buffers->get_depth_texture();
			if (depth.is_valid()) {
				raster_depth = depth;
			}
			RID color = p_render_data->render_buffers->get_internal_texture();
			if (color.is_valid()) {
				raster_color = color;
			}
		}
		if (rb_data->has_normal_roughness()) {
			raster_normal_roughness = rb_data->get_normal_roughness();
		}

		RD::Uniform u;
		u.binding = 71;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		add_uniform_id(u, raster_depth);
		uniforms.push_back(u);
	}
	{
		RID raster_normal_roughness = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
		if (rb_data->has_normal_roughness()) {
			raster_normal_roughness = rb_data->get_normal_roughness();
		}

		RD::Uniform u;
		u.binding = 72;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		add_uniform_id(u, raster_normal_roughness);
		uniforms.push_back(u);
	}
	{
		RID raster_color = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		if (p_render_data && p_render_data->render_buffers.is_valid()) {
			RID color = p_render_data->render_buffers->get_internal_texture();
			if (color.is_valid()) {
				raster_color = color;
			}
		}

		RD::Uniform u;
		u.binding = 73;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		add_uniform_id(u, raster_color);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 74;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		add_uniform_id(u, RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
								  RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 45;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_candidate());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 46;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_candidate_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 47;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_candidate_key());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 48;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_candidate_key_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 49;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_history());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 50;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_temporal_delta());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 51;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_prev_history_validity());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 52;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_prev_history_id());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 53;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_normal_roughness_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 54;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_viewz_hitdist_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 55;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_rejection());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 56;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_candidate());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 57;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_candidate_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 58;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_candidate_key());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 59;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_candidate_key_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 67;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_reservoir());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 68;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_reservoir_prev());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 69;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_reservoir_lighting());
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.binding = 70;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		add_uniform_id(u, rb_data->rt_get_source_direct_reservoir_lighting_prev());
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

	// Copy raytracing output to main color buffer. Scaled RTGI frames use the
	// dedicated reconstruction texture once the RTGI pass has populated it.
	const bool use_reconstructed = rb_data->rt_is_reconstructed_valid();
	const Size2i dst_size = rb->get_internal_size();
	const Rect2i src_rect = use_reconstructed ? Rect2i(Vector2i(), dst_size) : Rect2i(rb_data->rt_get_visible_origin(), rb_data->rt_get_visible_size());
	const Size2i rt_size = use_reconstructed ? dst_size : rb_data->rt_get_size();
	const bool direct_copy = src_rect.size == dst_size;
	if (p_render_data->render_info) {
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][use_reconstructed ? RSE::VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTED_COPY_COUNT : RSE::VIEWPORT_RENDER_INFO_RTGI_RAW_FALLBACK_COPY_COUNT]++;
	}
	if (!use_reconstructed && rb_data->rt_get_visible_size() != dst_size && p_render_data->environment.is_valid()) {
		const RSE::PathtracingParams *rt_env_params = RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_params_ptr(p_render_data->environment);
		const bool scaled_full_path_tracing = rt_env_params && rt_env_params->mode == SceneShaderRaytracing::RT_MODE_FULL_PATH_TRACING;
		if (scaled_full_path_tracing) {
			WARN_PRINT_ONCE("Scaled Full Path Tracing RTGI copied raw ray tracing output because reconstructed output was unavailable. This is a quality-path fallback and should be investigated.");
		}
	}
	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RID src = use_reconstructed ? rb_data->rt_get_reconstructed(v) : rb_data->rt_get_texture(v);
		RID dst = rb->get_internal_texture(v);
		if (direct_copy) {
			owner->copy_effects->copy_to_rect_region(src, dst, src_rect, Vector2i(), false, false, false, false, true);
		} else {
			RID dst_fb = FramebufferCacheRD::get_singleton()->get_cache(dst);
			const Rect2 source_rect(
					Vector2((float)src_rect.position.x / (float)rt_size.x, (float)src_rect.position.y / (float)rt_size.y),
					Vector2((float)src_rect.size.x / (float)rt_size.x, (float)src_rect.size.y / (float)rt_size.y));
			owner->copy_effects->copy_to_fb_rect(src, dst_fb, Rect2i(0, 0, dst_size.x, dst_size.y), false, false, false, false, RID(), false, false, false, false, source_rect, 1.0f, true);
		}
	}
}
