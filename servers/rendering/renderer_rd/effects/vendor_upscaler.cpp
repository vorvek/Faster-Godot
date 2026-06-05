/**************************************************************************/
/*  vendor_upscaler.cpp                                                   */
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

#include "vendor_upscaler.h"

#include "core/error/error_macros.h"
#include "core/templates/hashfuncs.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_commons.h"
#include "servers/rendering/rendering_device_driver.h"

#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
#include "core/os/os.h"
#include "drivers/vulkan/godot_vulkan.h"
#include "drivers/vulkan/xess_vulkan_loader.h"
#include "xess_vk.h"
#endif

namespace RendererRD {

#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
namespace {

struct VulkanTextureHandles {
	VkImage image = VK_NULL_HANDLE;
	VkImageView image_view = VK_NULL_HANDLE;
	VkDeviceMemory device_memory = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags usage = 0;
};

static VulkanTextureHandles _get_vulkan_texture_handles(RD *p_rd, RID p_texture) {
	VulkanTextureHandles handles;
	if (p_rd == nullptr || !p_texture.is_valid()) {
		return handles;
	}

	handles.image = (VkImage)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture);
	handles.image_view = (VkImageView)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
	handles.device_memory = (VkDeviceMemory)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture);
	handles.format = (VkFormat)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
	handles.usage = (VkImageUsageFlags)p_rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture);
	return handles;
}

static bool _vulkan_textures_share_image(const VulkanTextureHandles &p_a, const VulkanTextureHandles &p_b) {
	return p_a.image != VK_NULL_HANDLE && p_a.image == p_b.image;
}

} // namespace
#endif

#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
namespace {

typedef xess_result_t (*XessGetVersionFunc)(xess_version_t *p_version);
typedef xess_result_t (*XessDestroyContextFunc)(xess_context_handle_t p_context);
typedef xess_result_t (*XessIsOptimalDriverFunc)(xess_context_handle_t p_context);
typedef xess_result_t (*XessSetLoggingCallbackFunc)(xess_context_handle_t p_context, xess_logging_level_t p_level, xess_app_log_callback_t p_callback);
typedef xess_result_t (*XessSetVelocityScaleFunc)(xess_context_handle_t p_context, float p_x, float p_y);
typedef xess_result_t (*XessSetJitterScaleFunc)(xess_context_handle_t p_context, float p_x, float p_y);
typedef xess_result_t (*XessVKCreateContextFunc)(VkInstance p_instance, VkPhysicalDevice p_physical_device, VkDevice p_device, xess_context_handle_t *r_context);
typedef xess_result_t (*XessVKBuildPipelinesFunc)(xess_context_handle_t p_context, VkPipelineCache p_pipeline_cache, bool p_blocking, uint32_t p_init_flags);
typedef xess_result_t (*XessVKInitFunc)(xess_context_handle_t p_context, const xess_vk_init_params_t *p_params);
typedef xess_result_t (*XessVKExecuteFunc)(xess_context_handle_t p_context, VkCommandBuffer p_command_buffer, const xess_vk_execute_params_t *p_params);

static const char *_xess_result_to_string(xess_result_t p_result) {
	switch (p_result) {
		case XESS_RESULT_SUCCESS:
			return "success";
		case XESS_RESULT_WARNING_NONEXISTING_FOLDER:
			return "dump folder does not exist";
		case XESS_RESULT_WARNING_OLD_DRIVER:
			return "old driver";
		case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
			return "unsupported device";
		case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
			return "unsupported driver";
		case XESS_RESULT_ERROR_UNINITIALIZED:
			return "uninitialized context";
		case XESS_RESULT_ERROR_INVALID_ARGUMENT:
			return "invalid argument";
		case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
			return "device out of memory";
		case XESS_RESULT_ERROR_DEVICE:
			return "device error";
		case XESS_RESULT_ERROR_NOT_IMPLEMENTED:
			return "not implemented";
		case XESS_RESULT_ERROR_INVALID_CONTEXT:
			return "invalid context";
		case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS:
			return "operation in progress";
		case XESS_RESULT_ERROR_UNSUPPORTED:
			return "unsupported configuration";
		case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
			return "library could not be loaded";
		case XESS_RESULT_ERROR_WRONG_CALL_ORDER:
			return "wrong call order";
		case XESS_RESULT_ERROR_UNKNOWN:
			return "unknown error";
		default:
			return "unrecognized XeSS result";
	}
}

static bool _xess_result_succeeded(xess_result_t p_result) {
	return p_result == XESS_RESULT_SUCCESS || p_result == XESS_RESULT_WARNING_OLD_DRIVER || p_result == XESS_RESULT_WARNING_NONEXISTING_FOLDER;
}

static bool _xess_should_skip_context_creation_on_device(String &r_unavailable_reason) {
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		r_unavailable_reason = "RenderingDevice is not initialized.";
		return true;
	}

	// XeSS super resolution is cross-vendor; it runs wherever its required Vulkan extensions
	// and features were enabled at device creation. The Vulkan drivers perform that handshake
	// and record the result here, so this is a capability check rather than a GPU-vendor check.
	if (!XessVulkanLoader::is_super_resolution_ready()) {
		r_unavailable_reason = "XeSS Vulkan extensions and features were not enabled at device creation (libxess missing at startup, a required capability is unsupported on this GPU, or GODOT_DISABLE_XESS_VULKAN=1).";
		return true;
	}

	return false;
}

static void _xess_log_callback(const char *p_message, xess_logging_level_t p_level) {
	if (p_message == nullptr) {
		return;
	}

	switch (p_level) {
		case XESS_LOGGING_LEVEL_ERROR:
		case XESS_LOGGING_LEVEL_WARNING:
			WARN_PRINT(vformat("XeSS: %s", p_message));
			break;
		case XESS_LOGGING_LEVEL_INFO:
		case XESS_LOGGING_LEVEL_DEBUG:
		default:
			print_verbose(vformat("XeSS: %s", p_message));
			break;
	}
}

static xess_quality_settings_t _xess_quality_for_resolution(const Size2i &p_internal_size, const Size2i &p_target_size) {
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0 || p_target_size.x <= 0 || p_target_size.y <= 0) {
		return XESS_QUALITY_SETTING_QUALITY;
	}

	const float scale_x = float(p_target_size.x) / float(p_internal_size.x);
	const float scale_y = float(p_target_size.y) / float(p_internal_size.y);
	const float scale = MAX(scale_x, scale_y);

	if (scale <= 1.05f) {
		return XESS_QUALITY_SETTING_AA;
	}
	if (scale <= 1.33f) {
		return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
	}
	if (scale <= 1.45f) {
		return XESS_QUALITY_SETTING_ULTRA_QUALITY;
	}
	if (scale <= 1.60f) {
		return XESS_QUALITY_SETTING_QUALITY;
	}
	if (scale <= 1.85f) {
		return XESS_QUALITY_SETTING_BALANCED;
	}
	if (scale <= 2.40f) {
		return XESS_QUALITY_SETTING_PERFORMANCE;
	}
	return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}

struct XessContextKey {
	uint64_t output_image = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	uint32_t init_flags = 0;
	xess_quality_settings_t quality = XESS_QUALITY_SETTING_QUALITY;

	bool operator==(const XessContextKey &p_other) const {
		return output_image == p_other.output_image &&
				input_width == p_other.input_width &&
				input_height == p_other.input_height &&
				output_width == p_other.output_width &&
				output_height == p_other.output_height &&
				init_flags == p_other.init_flags &&
				quality == p_other.quality;
	}
};

struct XessUpscaleContext {
	XessContextKey key;
	xess_context_handle_t context = nullptr;
};

class XessRuntime {
	bool attempted_load = false;
	bool loaded = false;
	bool device_probe_attempted = false;
	bool device_supported = false;
	String unavailable_reason = "XeSS runtime has not been loaded.";
	mutable CharString unavailable_reason_utf8;
	Vector<XessUpscaleContext *> contexts;

public:
	XessGetVersionFunc xess_get_version = nullptr;
	XessDestroyContextFunc xess_destroy_context = nullptr;
	XessIsOptimalDriverFunc xess_is_optimal_driver = nullptr;
	XessSetLoggingCallbackFunc xess_set_logging_callback = nullptr;
	XessSetVelocityScaleFunc xess_set_velocity_scale = nullptr;
	XessSetJitterScaleFunc xess_set_jitter_scale = nullptr;
	XessVKCreateContextFunc xess_vk_create_context = nullptr;
	XessVKBuildPipelinesFunc xess_vk_build_pipelines = nullptr;
	XessVKInitFunc xess_vk_init = nullptr;
	XessVKExecuteFunc xess_vk_execute = nullptr;

	~XessRuntime() {
		unload();
	}

	bool load() {
		if (attempted_load) {
			return loaded;
		}

		attempted_load = true;

		if (!XessVulkanLoader::ensure_loaded()) {
			unavailable_reason = "The XeSS runtime library could not be loaded.";
			return false;
		}

		// Resolve the runtime exports from the shared libxess handle owned by the Vulkan
		// driver, so the library is opened once for both the bootstrap handshake and dispatch.
		bool resolved = true;
		auto resolve_export = [&resolved](const char *p_name) -> void * {
			void *symbol = XessVulkanLoader::resolve_symbol(p_name);
			resolved = resolved && symbol != nullptr;
			return symbol;
		};
		xess_get_version = reinterpret_cast<XessGetVersionFunc>(resolve_export("xessGetVersion"));
		xess_destroy_context = reinterpret_cast<XessDestroyContextFunc>(resolve_export("xessDestroyContext"));
		xess_is_optimal_driver = reinterpret_cast<XessIsOptimalDriverFunc>(resolve_export("xessIsOptimalDriver"));
		xess_set_logging_callback = reinterpret_cast<XessSetLoggingCallbackFunc>(resolve_export("xessSetLoggingCallback"));
		xess_set_velocity_scale = reinterpret_cast<XessSetVelocityScaleFunc>(resolve_export("xessSetVelocityScale"));
		xess_set_jitter_scale = reinterpret_cast<XessSetJitterScaleFunc>(resolve_export("xessSetJitterScale"));
		xess_vk_create_context = reinterpret_cast<XessVKCreateContextFunc>(resolve_export("xessVKCreateContext"));
		xess_vk_build_pipelines = reinterpret_cast<XessVKBuildPipelinesFunc>(resolve_export("xessVKBuildPipelines"));
		xess_vk_init = reinterpret_cast<XessVKInitFunc>(resolve_export("xessVKInit"));
		xess_vk_execute = reinterpret_cast<XessVKExecuteFunc>(resolve_export("xessVKExecute"));

		if (!resolved) {
			unload();
			attempted_load = true;
			unavailable_reason = "The XeSS runtime is missing one or more required Vulkan SR exports.";
			return false;
		}

		xess_version_t version = {};
		const xess_result_t version_result = xess_get_version(&version);
		if (!_xess_result_succeeded(version_result)) {
			unload();
			attempted_load = true;
			unavailable_reason = vformat("XeSS could not report its version: %s.", _xess_result_to_string(version_result));
			return false;
		}

		loaded = true;
		unavailable_reason = vformat("XeSS runtime %d.%d.%d is loaded, but the Vulkan device has not been probed.", version.major, version.minor, version.patch);
		return true;
	}

	void unload() {
		for (XessUpscaleContext *context : contexts) {
			if (context != nullptr && context->context != nullptr && xess_destroy_context != nullptr) {
				xess_destroy_context(context->context);
			}
			memdelete(context);
		}
		contexts.clear();

		loaded = false;
		device_probe_attempted = false;
		device_supported = false;
		xess_get_version = nullptr;
		xess_destroy_context = nullptr;
		xess_is_optimal_driver = nullptr;
		xess_set_logging_callback = nullptr;
		xess_set_velocity_scale = nullptr;
		xess_set_jitter_scale = nullptr;
		xess_vk_create_context = nullptr;
		xess_vk_build_pipelines = nullptr;
		xess_vk_init = nullptr;
		xess_vk_execute = nullptr;
		unavailable_reason = "XeSS runtime is not loaded.";
	}

	bool probe_device() {
		if (device_probe_attempted) {
			return device_supported;
		}

		device_probe_attempted = true;
		device_supported = false;

		if (!load()) {
			return false;
		}

		if (_xess_should_skip_context_creation_on_device(unavailable_reason)) {
			return false;
		}

		RD *rd = RD::get_singleton();
		if (rd == nullptr) {
			unavailable_reason = "RenderingDevice is not initialized.";
			return false;
		}

		VkInstance instance = (VkInstance)rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT);
		VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
		VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
		if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
			unavailable_reason = "Vulkan native handles are not available from RenderingDevice.";
			return false;
		}

		xess_context_handle_t probe_context = nullptr;
		xess_result_t result = xess_vk_create_context(instance, physical_device, device, &probe_context);
		if (!_xess_result_succeeded(result) || probe_context == nullptr) {
			unavailable_reason = vformat("XeSS Vulkan context creation failed: %s.", _xess_result_to_string(result));
			return false;
		}

		result = xess_is_optimal_driver(probe_context);
		if (result == XESS_RESULT_WARNING_OLD_DRIVER) {
			unavailable_reason = "XeSS is available, but the installed graphics driver is older than recommended.";
		} else if (!_xess_result_succeeded(result)) {
			unavailable_reason = vformat("XeSS driver check failed: %s.", _xess_result_to_string(result));
			xess_destroy_context(probe_context);
			return false;
		} else {
			unavailable_reason = "XeSS Vulkan SR is available.";
		}

		xess_destroy_context(probe_context);
		device_supported = true;
		return true;
	}

	const String &get_unavailable_reason() const {
		return unavailable_reason;
	}

	const char *get_unavailable_reason_cstr() const {
		unavailable_reason_utf8 = unavailable_reason.utf8();
		return unavailable_reason_utf8.get_data();
	}

	XessUpscaleContext *get_or_create_context(const XessContextKey &p_key) {
		for (XessUpscaleContext *context : contexts) {
			if (context != nullptr && context->key == p_key) {
				return context;
			}
		}

		if (!probe_device()) {
			return nullptr;
		}

		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, nullptr);

		VkInstance instance = (VkInstance)rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT);
		VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
		VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);

		xess_context_handle_t xess_context = nullptr;
		xess_result_t result = xess_vk_create_context(instance, physical_device, device, &xess_context);
		if (!_xess_result_succeeded(result) || xess_context == nullptr) {
			unavailable_reason = vformat("XeSS Vulkan context creation failed: %s.", _xess_result_to_string(result));
			return nullptr;
		}

		xess_set_logging_callback(xess_context, XESS_LOGGING_LEVEL_WARNING, _xess_log_callback);
		xess_set_velocity_scale(xess_context, float(p_key.input_width), float(p_key.input_height));
		xess_set_jitter_scale(xess_context, 1.0f, 1.0f);

		result = xess_vk_build_pipelines(xess_context, VK_NULL_HANDLE, true, p_key.init_flags);
		if (!_xess_result_succeeded(result)) {
			unavailable_reason = vformat("XeSS Vulkan pipeline build failed: %s.", _xess_result_to_string(result));
			xess_destroy_context(xess_context);
			return nullptr;
		}

		xess_vk_init_params_t init_params = {};
		init_params.outputResolution.x = p_key.output_width;
		init_params.outputResolution.y = p_key.output_height;
		init_params.qualitySetting = p_key.quality;
		init_params.initFlags = p_key.init_flags;
		init_params.creationNodeMask = 0;
		init_params.visibleNodeMask = 0;
		init_params.tempBufferHeap = VK_NULL_HANDLE;
		init_params.bufferHeapOffset = 0;
		init_params.tempTextureHeap = VK_NULL_HANDLE;
		init_params.textureHeapOffset = 0;
		init_params.pipelineCache = VK_NULL_HANDLE;

		result = xess_vk_init(xess_context, &init_params);
		if (!_xess_result_succeeded(result)) {
			unavailable_reason = vformat("XeSS Vulkan initialization failed: %s.", _xess_result_to_string(result));
			xess_destroy_context(xess_context);
			return nullptr;
		}

		XessUpscaleContext *context = memnew(XessUpscaleContext);
		context->key = p_key;
		context->context = xess_context;
		contexts.push_back(context);
		return context;
	}
};

static XessRuntime &xess_runtime() {
	static XessRuntime runtime;
	return runtime;
}

static xess_vk_image_view_info _xess_make_image_view_info(const VulkanTextureHandles &p_texture, const Size2i &p_size, VkImageAspectFlags p_aspect) {
	xess_vk_image_view_info info = {};
	if (p_texture.image == VK_NULL_HANDLE || p_texture.image_view == VK_NULL_HANDLE) {
		return info;
	}

	info.image = p_texture.image;
	info.imageView = p_texture.image_view;
	info.format = p_texture.format;
	info.subresourceRange.aspectMask = p_aspect;
	info.subresourceRange.baseMipLevel = 0;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.baseArrayLayer = 0;
	info.subresourceRange.layerCount = 1;
	info.width = uint32_t(p_size.x);
	info.height = uint32_t(p_size.y);
	return info;
}

struct XessCallbackData {
	XessUpscaleContext *context = nullptr;
	VulkanTextureHandles color;
	VulkanTextureHandles depth;
	VulkanTextureHandles velocity;
	VulkanTextureHandles reactive;
	VulkanTextureHandles output;
	Size2i internal_size;
	Size2i target_size;
	Vector2 jitter;
	bool reset_history = false;
};

static void _xess_driver_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	XessCallbackData *data = static_cast<XessCallbackData *>(p_userdata);
	ERR_FAIL_NULL(data);

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	if (command_buffer == VK_NULL_HANDLE) {
		WARN_PRINT("XeSS Vulkan dispatch skipped because the native command buffer handle is unavailable.");
		memdelete(data);
		return;
	}

	xess_vk_execute_params_t execute_params = {};
	execute_params.colorTexture = _xess_make_image_view_info(data->color, data->internal_size, VK_IMAGE_ASPECT_COLOR_BIT);
	execute_params.velocityTexture = _xess_make_image_view_info(data->velocity, data->internal_size, VK_IMAGE_ASPECT_COLOR_BIT);
	execute_params.depthTexture = _xess_make_image_view_info(data->depth, data->internal_size, VK_IMAGE_ASPECT_DEPTH_BIT);
	execute_params.responsivePixelMaskTexture = _xess_make_image_view_info(data->reactive, data->internal_size, VK_IMAGE_ASPECT_COLOR_BIT);
	execute_params.outputTexture = _xess_make_image_view_info(data->output, data->target_size, VK_IMAGE_ASPECT_COLOR_BIT);
	execute_params.jitterOffsetX = data->jitter.x;
	execute_params.jitterOffsetY = -data->jitter.y;
	execute_params.exposureScale = 1.0f;
	execute_params.resetHistory = data->reset_history ? 1 : 0;
	execute_params.inputWidth = uint32_t(data->internal_size.x);
	execute_params.inputHeight = uint32_t(data->internal_size.y);
	execute_params.inputColorBase = { 0, 0 };
	execute_params.inputMotionVectorBase = { 0, 0 };
	execute_params.inputDepthBase = { 0, 0 };
	execute_params.inputResponsiveMaskBase = { 0, 0 };
	execute_params.outputColorBase = { 0, 0 };

	const xess_result_t result = xess_runtime().xess_vk_execute(data->context->context, command_buffer, &execute_params);
	if (!_xess_result_succeeded(result)) {
		WARN_PRINT(vformat("XeSS Vulkan dispatch failed: %s.", _xess_result_to_string(result)));
	}

	memdelete(data);
}

} // namespace
#endif

bool VendorUpscaler::is_super_resolution_mode(RSE::ViewportScaling3DMode p_mode) {
	return p_mode == RSE::VIEWPORT_SCALING_3D_MODE_XESS;
}

bool VendorUpscaler::is_super_resolution_available(RSE::ViewportScaling3DMode p_mode) {
	if (!is_super_resolution_mode(p_mode)) {
		return false;
	}

	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_XESS) {
#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
		return xess_runtime().probe_device();
#else
		return false;
#endif
	}

	return false;
}

const char *VendorUpscaler::get_super_resolution_name(RSE::ViewportScaling3DMode p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_SCALING_3D_MODE_XESS:
			return "XeSS";
		default:
			return "Vendor";
	}
}

const char *VendorUpscaler::get_super_resolution_unavailable_reason(RSE::ViewportScaling3DMode p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_SCALING_3D_MODE_XESS:
#if !defined(VENDOR_UPSCALER_XESS_REQUESTED)
			return "XeSS was not enabled at build time.";
#elif !defined(XESS_VK_HEADERS_PRESENT)
			return "XeSS Vulkan headers were not found at build time.";
#else
			return xess_runtime().get_unavailable_reason_cstr();
#endif
		default:
			return "This is not a vendor temporal upscaler.";
	}
}

RSE::ViewportScaling3DMode VendorUpscaler::get_super_resolution_fallback(RSE::ViewportScaling3DMode p_mode) {
	if (is_super_resolution_mode(p_mode)) {
		return RSE::VIEWPORT_SCALING_3D_MODE_FSR2;
	}
	return p_mode;
}

bool VendorUpscaler::upscale(const SuperResolutionParameters &p_params) {
	ERR_FAIL_COND_V_MSG(!is_super_resolution_mode(p_params.mode), false, "The requested scaling mode is not a vendor temporal upscaler.");
	ERR_FAIL_COND_V_MSG(!is_super_resolution_available(p_params.mode), false, get_super_resolution_unavailable_reason(p_params.mode));

	if (p_params.mode == RSE::VIEWPORT_SCALING_3D_MODE_XESS) {
#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, false);
		ERR_FAIL_COND_V_MSG(!p_params.color.is_valid(), false, "XeSS requires a color input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.depth.is_valid(), false, "XeSS requires a depth input texture when using internal-resolution motion vectors.");
		ERR_FAIL_COND_V_MSG(!p_params.velocity.is_valid(), false, "XeSS requires a motion-vector input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.output.is_valid(), false, "XeSS requires an output texture.");
		ERR_FAIL_COND_V_MSG(p_params.internal_size.x <= 0 || p_params.internal_size.y <= 0, false, "XeSS requires a valid internal render size.");
		ERR_FAIL_COND_V_MSG(p_params.target_size.x <= 0 || p_params.target_size.y <= 0, false, "XeSS requires a valid output render size.");

		const VulkanTextureHandles output_handles = _get_vulkan_texture_handles(rd, p_params.output);
		ERR_FAIL_COND_V_MSG(output_handles.image == VK_NULL_HANDLE, false, "XeSS could not retrieve the Vulkan output image handle.");

		XessContextKey key;
		key.output_image = (uint64_t)output_handles.image;
		key.input_width = uint32_t(p_params.internal_size.x);
		key.input_height = uint32_t(p_params.internal_size.y);
		key.output_width = uint32_t(p_params.target_size.x);
		key.output_height = uint32_t(p_params.target_size.y);
		key.init_flags = XESS_INIT_FLAG_INVERTED_DEPTH;
		if (p_params.reactive.is_valid()) {
			key.init_flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
		}
		key.quality = _xess_quality_for_resolution(p_params.internal_size, p_params.target_size);

		XessUpscaleContext *context = xess_runtime().get_or_create_context(key);
		ERR_FAIL_NULL_V_MSG(context, false, xess_runtime().get_unavailable_reason());

		const VulkanTextureHandles color_handles = _get_vulkan_texture_handles(rd, p_params.color);
		const VulkanTextureHandles depth_handles = _get_vulkan_texture_handles(rd, p_params.depth);
		const VulkanTextureHandles velocity_handles = _get_vulkan_texture_handles(rd, p_params.velocity);
		const VulkanTextureHandles reactive_handles = _get_vulkan_texture_handles(rd, p_params.reactive);

		XessCallbackData *data = memnew(XessCallbackData);
		data->context = context;
		data->color = color_handles;
		data->depth = depth_handles;
		data->velocity = velocity_handles;
		data->reactive = reactive_handles;
		data->output = output_handles;
		data->internal_size = p_params.internal_size;
		data->target_size = p_params.target_size;
		data->jitter = p_params.jitter;
		data->reset_history = p_params.reset_accumulation;

		RD::CallbackResource resources[5];
		resources[0].rid = p_params.color;
		resources[0].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[1].rid = p_params.depth;
		resources[1].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[2].rid = p_params.velocity;
		resources[2].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[3].rid = p_params.output;
		resources[3].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
		uint32_t resource_count = 4;
		if (p_params.reactive.is_valid() && !_vulkan_textures_share_image(reactive_handles, color_handles)) {
			resources[resource_count].rid = p_params.reactive;
			resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			resource_count++;
		}

		const Error err = rd->driver_callback_add((RDD::DriverCallback)_xess_driver_callback, data, VectorView<RD::CallbackResource>(resources, resource_count));
		if (err != OK) {
			memdelete(data);
			ERR_FAIL_V_MSG(false, "XeSS failed to record its RenderingDevice driver callback.");
		}

		return true;
#else
		return false;
#endif
	}

	// Backend dispatch is intentionally centralized here so the renderer only
	// needs to provide temporal-upscaler inputs once per viewport/view.
	return false;
}

} // namespace RendererRD
