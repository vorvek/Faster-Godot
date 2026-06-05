/**************************************************************************/
/*  xess_vulkan_loader.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

#include "xess_vulkan_loader.h"

#ifdef XESS_VK_HEADERS_PRESENT

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include "xess_vk.h"

namespace {
void *g_library = nullptr;
bool g_attempted = false;
bool g_loaded = false;
bool g_instance_ready = false;
bool g_device_ready = false;

typedef xess_result_t (*PFN_GetReqInstExt)(uint32_t *, const char *const **, uint32_t *);
typedef xess_result_t (*PFN_GetReqDevExt)(VkInstance, VkPhysicalDevice, uint32_t *, const char *const **);
typedef xess_result_t (*PFN_GetReqDevFeat)(VkInstance, VkPhysicalDevice, void **);

PFN_GetReqInstExt g_get_required_instance_extensions = nullptr;
PFN_GetReqDevExt g_get_required_device_extensions = nullptr;
PFN_GetReqDevFeat g_get_required_device_features = nullptr;

bool resolve(const String &p_symbol, void *&r_symbol) {
	return OS::get_singleton()->get_dynamic_library_symbol_handle(g_library, p_symbol, r_symbol, true) == OK && r_symbol != nullptr;
}
} // namespace

bool XessVulkanLoader::ensure_loaded() {
	if (g_attempted) {
		return g_loaded;
	}
	g_attempted = true;

	OS *os = OS::get_singleton();
	if (os == nullptr) {
		return false;
	}
	if (os->get_environment("GODOT_DISABLE_XESS_VULKAN") == "1") {
		print_verbose("XeSS: disabled via GODOT_DISABLE_XESS_VULKAN.");
		return false;
	}

	const char *const names[] = {
#if defined(WINDOWS_ENABLED)
		"libxess.dll",
#else
		"libxess.so",
#endif
	};
	for (const char *name : names) {
		if (os->open_dynamic_library(name, g_library) == OK && g_library != nullptr) {
			break;
		}
		g_library = nullptr;
	}
	if (g_library == nullptr) {
		print_verbose("XeSS: libxess runtime not found; XeSS super resolution unavailable.");
		return false;
	}

	void *sym = nullptr;
	bool ok = true;
	ok = resolve("xessVKGetRequiredInstanceExtensions", sym) && ok;
	g_get_required_instance_extensions = reinterpret_cast<PFN_GetReqInstExt>(sym);
	ok = resolve("xessVKGetRequiredDeviceExtensions", sym) && ok;
	g_get_required_device_extensions = reinterpret_cast<PFN_GetReqDevExt>(sym);
	ok = resolve("xessVKGetRequiredDeviceFeatures", sym) && ok;
	g_get_required_device_features = reinterpret_cast<PFN_GetReqDevFeat>(sym);

	if (!ok) {
		os->close_dynamic_library(g_library);
		g_library = nullptr;
		WARN_PRINT("XeSS: libxess is missing one or more Vulkan handshake exports; XeSS super resolution disabled.");
		return false;
	}

	g_loaded = true;
	return true;
}

bool XessVulkanLoader::get_required_instance_extensions(Vector<CharString> &r_extensions, uint32_t &r_min_api_version) {
	if (!ensure_loaded()) {
		return false;
	}
	uint32_t count = 0;
	const char *const *names = nullptr;
	uint32_t min_api = 0;
	if (g_get_required_instance_extensions(&count, &names, &min_api) != XESS_RESULT_SUCCESS) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		r_extensions.push_back(CharString(names[i]));
	}
	r_min_api_version = min_api;
	return true;
}

bool XessVulkanLoader::get_required_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, Vector<CharString> &r_extensions) {
	if (!ensure_loaded()) {
		return false;
	}
	uint32_t count = 0;
	const char *const *names = nullptr;
	if (g_get_required_device_extensions(p_instance, p_physical_device, &count, &names) != XESS_RESULT_SUCCESS) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		r_extensions.push_back(CharString(names[i]));
	}
	return true;
}

bool XessVulkanLoader::patch_required_device_features(VkInstance p_instance, VkPhysicalDevice p_physical_device, void **r_chain_head) {
	if (!ensure_loaded()) {
		return false;
	}
	return g_get_required_device_features(p_instance, p_physical_device, r_chain_head) == XESS_RESULT_SUCCESS;
}

void XessVulkanLoader::set_instance_ready(bool p_ready) { g_instance_ready = p_ready; }
void XessVulkanLoader::set_device_ready(bool p_ready) { g_device_ready = p_ready; }
bool XessVulkanLoader::is_instance_ready() { return g_instance_ready; }
bool XessVulkanLoader::is_device_ready() { return g_device_ready; }
bool XessVulkanLoader::is_super_resolution_ready() { return g_loaded && g_instance_ready && g_device_ready; }

void *XessVulkanLoader::resolve_symbol(const char *p_name) {
	if (!ensure_loaded()) {
		return nullptr;
	}
	void *sym = nullptr;
	return resolve(String(p_name), sym) ? sym : nullptr;
}

#endif // XESS_VK_HEADERS_PRESENT
