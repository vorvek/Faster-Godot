/**************************************************************************/
/*  xess_vulkan_loader.h                                                  */
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

#pragma once

#ifdef XESS_VK_HEADERS_PRESENT

#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "drivers/vulkan/godot_vulkan.h"

// Owns a single libxess dynamic-library handle and the XeSS Vulkan handshake exports.
// Every method is a no-op-safe static; failure to load leaves XeSS unavailable, never fatal.
// The handshake (instance extensions, device extensions, device features) must run during
// Vulkan bootstrap, before vkCreateInstance/vkCreateDevice, which is why this lives in the
// driver layer rather than in the renderer's vendor_upscaler.
class XessVulkanLoader {
public:
	// Loads libxess once. Returns false if the library is absent, fails to resolve, or
	// GODOT_DISABLE_XESS_VULKAN=1 is set. Safe to call repeatedly.
	static bool ensure_loaded();

	// Instance handshake. Appends XeSS-required instance extension names to r_extensions
	// and reports the minimum VkApplicationInfo.apiVersion XeSS needs. Returns false on failure.
	static bool get_required_instance_extensions(Vector<CharString> &r_extensions, uint32_t &r_min_api_version);

	// Device handshake. Appends XeSS-required device extension names. Returns false on failure.
	static bool get_required_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, Vector<CharString> &r_extensions);

	// Patches an application-owned VkPhysicalDeviceFeatures2 chain (passed via r_chain_head)
	// with the device features XeSS requires. Returns false on failure.
	static bool patch_required_device_features(VkInstance p_instance, VkPhysicalDevice p_physical_device, void **r_chain_head);

	// Bootstrap bookkeeping: the drivers record whether each stage was fully satisfied.
	static void set_instance_ready(bool p_ready);
	static void set_device_ready(bool p_ready);
	static bool is_instance_ready();
	static bool is_device_ready();

	// True only if both stages succeeded; the capability gate consults this.
	static bool is_super_resolution_ready();

	// Resolve a runtime export (e.g. "xessVKCreateContext") from the shared handle.
	// Lets vendor_upscaler reuse the same libxess handle instead of opening it twice.
	static void *resolve_symbol(const char *p_name);
};

#endif // XESS_VK_HEADERS_PRESENT
