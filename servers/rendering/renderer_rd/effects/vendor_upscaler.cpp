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

#ifdef STREAMLINE_ENABLED
#include "drivers/streamline/streamline.h"
#endif

#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
#include "drivers/streamline/streamline_context.h"
#endif

#if (defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)) || (defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)) || (defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT))
#include "core/os/os.h"
#include "drivers/vulkan/godot_vulkan.h"
#endif

#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
#include "sl.h"
#include "sl_core_api.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#endif

#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
#if !defined(_MSC_VER) && !defined(__declspec)
#define FFX_API_GCC_DECLSPEC_COMPAT
#define __declspec(p_attribute)
#endif
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/vk/ffx_api_vk.h"
#if defined(FFX_API_GCC_DECLSPEC_COMPAT)
#undef __declspec
#undef FFX_API_GCC_DECLSPEC_COMPAT
#endif
#endif

#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
#include "xess_vk.h"
#endif

namespace RendererRD {

#if (defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)) || (defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)) || (defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT))
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

static bool _open_vendor_library(const char *const *p_library_names, uint32_t p_library_name_count, void *&r_library_handle, String &r_resolved_library_name) {
	OS *os = OS::get_singleton();
	ERR_FAIL_NULL_V(os, false);

	for (uint32_t i = 0; i < p_library_name_count; i++) {
		const char *library_name = p_library_names[i];
		if (library_name == nullptr) {
			continue;
		}

		if (os->open_dynamic_library(library_name, r_library_handle) == OK && r_library_handle != nullptr) {
			r_resolved_library_name = library_name;
			return true;
		}
	}

	r_library_handle = nullptr;
	r_resolved_library_name = String();
	return false;
}

} // namespace
#endif

#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
namespace {

static const char *_streamline_result_to_string(sl::Result p_result) {
	switch (p_result) {
		case sl::Result::eOk:
			return "ok";
		case sl::Result::eErrorIO:
			return "I/O error";
		case sl::Result::eErrorDriverOutOfDate:
			return "driver out of date";
		case sl::Result::eErrorOSOutOfDate:
			return "operating system out of date";
		case sl::Result::eErrorOSDisabledHWS:
			return "hardware accelerated GPU scheduling is disabled";
		case sl::Result::eErrorDeviceNotCreated:
			return "device was not created";
		case sl::Result::eErrorNoSupportedAdapterFound:
			return "no supported adapter found";
		case sl::Result::eErrorAdapterNotSupported:
			return "adapter is not supported";
		case sl::Result::eErrorNoPlugins:
			return "no Streamline plugins found";
		case sl::Result::eErrorVulkanAPI:
			return "Vulkan API error";
		case sl::Result::eErrorDXGIAPI:
			return "DXGI API error";
		case sl::Result::eErrorD3DAPI:
			return "D3D API error";
		case sl::Result::eErrorNVAPI:
			return "NVAPI error";
		case sl::Result::eErrorNGXFailed:
			return "NGX error";
		case sl::Result::eErrorMissingProxy:
			return "missing Streamline proxy/interposer";
		case sl::Result::eErrorMissingResourceState:
			return "missing resource state";
		case sl::Result::eErrorInvalidIntegration:
			return "invalid Streamline integration";
		case sl::Result::eErrorMissingInputParameter:
			return "missing input parameter";
		case sl::Result::eErrorNotInitialized:
			return "Streamline is not initialized";
		case sl::Result::eErrorComputeFailed:
			return "compute dispatch failed";
		case sl::Result::eErrorInitNotCalled:
			return "slInit was not called";
		case sl::Result::eErrorInvalidParameter:
			return "invalid parameter";
		case sl::Result::eErrorMissingConstants:
			return "missing constants";
		case sl::Result::eErrorDuplicatedConstants:
			return "duplicated constants";
		case sl::Result::eErrorMissingOrInvalidAPI:
			return "missing or invalid rendering API";
		case sl::Result::eErrorCommonConstantsMissing:
			return "common constants are missing";
		case sl::Result::eErrorUnsupportedInterface:
			return "unsupported Streamline interface";
		case sl::Result::eErrorFeatureMissing:
			return "feature plugin is missing";
		case sl::Result::eErrorFeatureNotSupported:
			return "feature is not supported";
		case sl::Result::eErrorFeatureMissingHooks:
			return "feature is missing hooks";
		case sl::Result::eErrorFeatureFailedToLoad:
			return "feature failed to load";
		case sl::Result::eErrorFeatureWrongPriority:
			return "feature priority is wrong";
		case sl::Result::eErrorFeatureMissingDependency:
			return "feature dependency is missing";
		case sl::Result::eErrorFeatureManagerInvalidState:
			return "feature manager is in an invalid state";
		case sl::Result::eErrorInvalidState:
			return "invalid Streamline state";
		case sl::Result::eWarnOutOfVRAM:
			return "out of VRAM";
		default:
			return "unknown Streamline error";
	}
}

static sl::DLSSMode _dlss_mode_for_resolution(const Size2i &p_internal_size, const Size2i &p_target_size) {
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0 || p_target_size.x <= 0 || p_target_size.y <= 0) {
		return sl::DLSSMode::eBalanced;
	}

	const float scale_x = float(p_target_size.x) / float(p_internal_size.x);
	const float scale_y = float(p_target_size.y) / float(p_internal_size.y);
	const float scale = MAX(scale_x, scale_y);

	if (scale <= 1.05f) {
		return sl::DLSSMode::eDLAA;
	}
	if (scale <= 1.35f) {
		return sl::DLSSMode::eUltraQuality;
	}
	if (scale <= 1.55f) {
		return sl::DLSSMode::eMaxQuality;
	}
	if (scale <= 1.85f) {
		return sl::DLSSMode::eBalanced;
	}
	if (scale <= 2.45f) {
		return sl::DLSSMode::eMaxPerformance;
	}
	return sl::DLSSMode::eUltraPerformance;
}

static uint32_t _dlss_make_viewport_id_for_output(uint64_t p_output_rid, uint64_t p_output_image) {
	uint32_t hash = hash_murmur3_one_64(p_output_rid);
	hash = hash_murmur3_one_64(p_output_image, hash);
	return hash == 0 ? 1 : hash;
}

static uint32_t _dlss_viewport_id_for_frame_generation(uint64_t p_viewport_id, int p_screen) {
	uint32_t hash = hash_murmur3_one_64(p_viewport_id);
	hash = hash_murmur3_one_64(uint64_t(uint32_t(p_screen)), hash);
	return hash == 0 ? 1 : hash;
}

static void _sl_store_projection(const Projection &p_projection, sl::float4x4 &r_matrix) {
	for (int row = 0; row < 4; row++) {
		float *row_values = &r_matrix.row[row].x;
		for (int column = 0; column < 4; column++) {
			row_values[column] = float(p_projection.columns[column][row]);
		}
	}
}

static sl::float3 _sl_make_float3(const Vector3 &p_vector) {
	return sl::float3(float(p_vector.x), float(p_vector.y), float(p_vector.z));
}

static sl::Resource _sl_make_texture_resource(const VulkanTextureHandles &p_texture, const Size2i &p_size, VkImageLayout p_layout) {
	sl::Resource resource(sl::ResourceType::eTex2d, (void *)p_texture.image, (void *)p_texture.device_memory, (void *)p_texture.image_view, uint32_t(p_layout));
	resource.width = uint32_t(MAX(p_size.x, 0));
	resource.height = uint32_t(MAX(p_size.y, 0));
	resource.nativeFormat = uint32_t(p_texture.format);
	resource.mipLevels = 1;
	resource.arrayLayers = 1;
	resource.flags = 0;
	resource.usage = p_texture.usage;
	return resource;
}

static void _dlss_fill_common_constants(sl::Constants &r_constants, const Projection &p_camera_view_to_clip, const Projection &p_reprojection, const Vector2 &p_jitter, float p_z_near, float p_z_far, float p_fovy, float p_aspect, bool p_reset_accumulation, bool p_orthogonal_projection, const Transform3D &p_camera_transform) {
	_sl_store_projection(p_camera_view_to_clip, r_constants.cameraViewToClip);
	_sl_store_projection(p_camera_view_to_clip.inverse(), r_constants.clipToCameraView);
	_sl_store_projection(p_reprojection, r_constants.clipToPrevClip);
	_sl_store_projection(p_reprojection.inverse(), r_constants.prevClipToClip);
	r_constants.jitterOffset = sl::float2(p_jitter.x, p_jitter.y);
	r_constants.mvecScale = sl::float2(1.0f, 1.0f);
	r_constants.cameraPos = _sl_make_float3(p_camera_transform.origin);
	r_constants.cameraUp = _sl_make_float3(p_camera_transform.basis.get_column(Vector3::AXIS_Y).normalized());
	r_constants.cameraRight = _sl_make_float3(p_camera_transform.basis.get_column(Vector3::AXIS_X).normalized());
	r_constants.cameraFwd = _sl_make_float3((-p_camera_transform.basis.get_column(Vector3::AXIS_Z)).normalized());
	r_constants.cameraNear = p_z_near;
	r_constants.cameraFar = p_z_far;
	r_constants.cameraFOV = p_fovy;
	r_constants.cameraAspectRatio = p_aspect;
	r_constants.depthInverted = sl::Boolean::eTrue;
	r_constants.cameraMotionIncluded = sl::Boolean::eTrue;
	r_constants.motionVectors3D = sl::Boolean::eFalse;
	r_constants.reset = p_reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	r_constants.orthographicProjection = p_orthogonal_projection ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	r_constants.motionVectorsDilated = sl::Boolean::eFalse;
	r_constants.motionVectorsJittered = sl::Boolean::eFalse;
}

class DlssRuntime {
	struct ViewportSlot {
		uint64_t output_rid = 0;
		uint64_t output_image = 0;
		uint32_t viewport_id = 0;
		bool active = false;
	};

	static constexpr uint32_t MAX_DLSS_VIEWPORTS = 4;

	bool attempted_resolve = false;
	bool resolved = false;
	bool runtime_disabled = false;
	String unavailable_reason = "Streamline DLSS functions have not been resolved.";
	mutable CharString unavailable_reason_utf8;
	ViewportSlot viewport_slots[MAX_DLSS_VIEWPORTS];

public:
	PFun_slDLSSSetOptions *sl_dlss_set_options = nullptr;

	bool resolve() {
		if (runtime_disabled) {
			return false;
		}
		if (resolved) {
			return true;
		}

		attempted_resolve = true;
		resolved = false;

		Streamline *streamline = Streamline::get_singleton();
		if (streamline == nullptr) {
			unavailable_reason = "Streamline is not initialized.";
			return false;
		}
		if (!streamline->is_initialized()) {
			unavailable_reason = streamline->get_unavailable_reason();
			return false;
		}
		if (!streamline->is_feature_supported(STREAMLINE_FEATURE_DLSS)) {
			unavailable_reason = "DLSS is not supported by the current Streamline runtime or adapter.";
			return false;
		}
		if (!streamline->is_feature_loaded(STREAMLINE_FEATURE_DLSS)) {
			unavailable_reason = "The Streamline DLSS plugin is not loaded.";
			return false;
		}

		StreamlineContext &context = StreamlineContext::get();
		if (context.slGetFeatureFunction == nullptr) {
			unavailable_reason = "Streamline did not expose slGetFeatureFunction.";
			return false;
		}

		PFun_slGetFeatureFunction *sl_get_feature_function = reinterpret_cast<PFun_slGetFeatureFunction *>(context.slGetFeatureFunction);
		void *function = nullptr;
		const sl::Result result = sl_get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function);
		if (result != sl::Result::eOk || function == nullptr) {
			unavailable_reason = vformat("The Streamline DLSS plugin did not expose slDLSSSetOptions: %s.", _streamline_result_to_string(result));
			return false;
		}

		sl_dlss_set_options = reinterpret_cast<PFun_slDLSSSetOptions *>(function);
		resolved = true;
		unavailable_reason = "Streamline DLSS SR is available.";
		return true;
	}

	bool acquire_viewport_id(uint64_t p_output_rid, uint64_t p_output_image, uint32_t &r_viewport_id) {
		if (runtime_disabled) {
			return false;
		}

		for (uint32_t i = 0; i < MAX_DLSS_VIEWPORTS; i++) {
			ViewportSlot &slot = viewport_slots[i];
			if (slot.active && slot.output_rid == p_output_rid) {
				slot.output_image = p_output_image;
				slot.viewport_id = _dlss_make_viewport_id_for_output(p_output_rid, p_output_image);
				r_viewport_id = slot.viewport_id;
				return true;
			}
		}

		for (uint32_t i = 0; i < MAX_DLSS_VIEWPORTS; i++) {
			ViewportSlot &slot = viewport_slots[i];
			if (!slot.active) {
				slot.output_rid = p_output_rid;
				slot.output_image = p_output_image;
				slot.viewport_id = _dlss_make_viewport_id_for_output(p_output_rid, p_output_image);
				slot.active = true;
				r_viewport_id = slot.viewport_id;
				return true;
			}
		}

		disable_runtime("DLSS SR was disabled because Streamline allows at most 4 active DLSS viewports in this integration. Falling back to FSR 2.");
		return false;
	}

	void disable_runtime(const String &p_reason) {
		runtime_disabled = true;
		resolved = false;
		unavailable_reason = p_reason;
	}

	const String &get_unavailable_reason() const {
		return unavailable_reason;
	}

	const char *get_unavailable_reason_cstr() const {
		unavailable_reason_utf8 = unavailable_reason.utf8();
		return unavailable_reason_utf8.get_data();
	}

};

static DlssRuntime &dlss_runtime() {
	static DlssRuntime runtime;
	return runtime;
}

class DlssFrameGenerationRuntime {
	bool attempted_resolve = false;
	bool resolved = false;
	String unavailable_reason = "Streamline DLSS-G functions have not been resolved.";
	mutable CharString unavailable_reason_utf8;

public:
	PFun_slDLSSGGetState *sl_dlss_g_get_state = nullptr;
	PFun_slDLSSGSetOptions *sl_dlss_g_set_options = nullptr;

	bool resolve() {
		if (resolved) {
			return true;
		}

		attempted_resolve = true;
		resolved = false;

		Streamline *streamline = Streamline::get_singleton();
		if (streamline == nullptr) {
			unavailable_reason = "Streamline is not initialized.";
			return false;
		}
		if (!streamline->is_initialized()) {
			unavailable_reason = streamline->get_unavailable_reason();
			return false;
		}
		if (StreamlineContext::get().dlss_g_load_disabled) {
			unavailable_reason = "DLSS Frame Generation is disabled by default because it installs present/swapchain hooks. Set GODOT_ENABLE_STREAMLINE_DLSS_G=1 before startup to enable it for game runs.";
			return false;
		}
		if (!streamline->is_feature_supported(STREAMLINE_FEATURE_DLSS_G)) {
			unavailable_reason = "DLSS Frame Generation is not supported by the current Streamline runtime or adapter.";
			return false;
		}
		if (!streamline->is_feature_loaded(STREAMLINE_FEATURE_DLSS_G)) {
			unavailable_reason = "The Streamline DLSS-G plugin is not loaded.";
			return false;
		}
		if (!streamline->is_feature_supported(STREAMLINE_FEATURE_REFLEX) || !streamline->is_feature_loaded(STREAMLINE_FEATURE_REFLEX)) {
			unavailable_reason = "Streamline Reflex is required for DLSS Frame Generation but is not available.";
			return false;
		}

		StreamlineContext &context = StreamlineContext::get();
		if (context.slGetFeatureFunction == nullptr) {
			unavailable_reason = "Streamline did not expose slGetFeatureFunction.";
			return false;
		}
		if (context.slPCLSetMarker == nullptr || context.slReflexSetOptions == nullptr) {
			unavailable_reason = "Streamline did not expose the Reflex/PCL functions required by DLSS Frame Generation.";
			return false;
		}

		PFun_slGetFeatureFunction *sl_get_feature_function = reinterpret_cast<PFun_slGetFeatureFunction *>(context.slGetFeatureFunction);
		void *function = nullptr;
		sl::Result result = sl_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function);
		if (result != sl::Result::eOk || function == nullptr) {
			unavailable_reason = vformat("The Streamline DLSS-G plugin did not expose slDLSSGSetOptions: %s.", _streamline_result_to_string(result));
			return false;
		}
		sl_dlss_g_set_options = reinterpret_cast<PFun_slDLSSGSetOptions *>(function);

		function = nullptr;
		result = sl_get_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", function);
		if (result != sl::Result::eOk || function == nullptr) {
			unavailable_reason = vformat("The Streamline DLSS-G plugin did not expose slDLSSGGetState: %s.", _streamline_result_to_string(result));
			return false;
		}
		sl_dlss_g_get_state = reinterpret_cast<PFun_slDLSSGGetState *>(function);

		if (!streamline->set_reflex_low_latency_enabled(true)) {
			unavailable_reason = "Streamline Reflex low latency mode could not be enabled for DLSS Frame Generation.";
			return false;
		}

		resolved = true;
		unavailable_reason = "DLSS Frame Generation is available.";
		return true;
	}

	const String &get_unavailable_reason() const {
		return unavailable_reason;
	}

	const char *get_unavailable_reason_cstr() const {
		unavailable_reason_utf8 = unavailable_reason.utf8();
		return unavailable_reason_utf8.get_data();
	}

	void set_options_off(uint32_t p_viewport_id) {
		if (!resolve()) {
			return;
		}

		sl::DLSSGOptions options;
		options.mode = sl::DLSSGMode::eOff;
		sl_dlss_g_set_options(sl::ViewportHandle(p_viewport_id), options);
	}
};

static DlssFrameGenerationRuntime &dlss_frame_generation_runtime() {
	static DlssFrameGenerationRuntime runtime;
	return runtime;
}

struct DlssCallbackData {
	uint32_t viewport_id = 0;
	VulkanTextureHandles color;
	VulkanTextureHandles depth;
	VulkanTextureHandles velocity;
	VulkanTextureHandles reactive;
	VulkanTextureHandles exposure;
	VulkanTextureHandles output;
	Size2i internal_size;
	Size2i target_size;
	Vector2 jitter;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float aspect = 1.0f;
	bool reset_accumulation = false;
	bool orthogonal_projection = false;
	bool use_auto_exposure = true;
	bool has_reactive = false;
	Projection camera_view_to_clip;
	Projection reprojection;
	Transform3D camera_transform;
	sl::DLSSMode mode = sl::DLSSMode::eBalanced;
};

static void _dlss_driver_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DlssCallbackData *data = static_cast<DlssCallbackData *>(p_userdata);
	ERR_FAIL_NULL(data);

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	if (command_buffer == VK_NULL_HANDLE) {
		WARN_PRINT("DLSS dispatch skipped because the native command buffer handle is unavailable.");
		memdelete(data);
		return;
	}

	if (!dlss_runtime().resolve()) {
		WARN_PRINT_ONCE(vformat("DLSS dispatch skipped: %s", dlss_runtime().get_unavailable_reason()));
		memdelete(data);
		return;
	}

	StreamlineContext &context = StreamlineContext::get();
	PFun_slGetNewFrameToken *sl_get_new_frame_token = reinterpret_cast<PFun_slGetNewFrameToken *>(context.slGetNewFrameToken);
	PFun_slSetConstants *sl_set_constants = reinterpret_cast<PFun_slSetConstants *>(context.slSetConstants);
	PFun_slSetTagForFrame *sl_set_tag_for_frame = reinterpret_cast<PFun_slSetTagForFrame *>(context.slSetTagForFrame);
	PFun_slEvaluateFeature *sl_evaluate_feature = reinterpret_cast<PFun_slEvaluateFeature *>(context.slEvaluateFeature);
	if (sl_get_new_frame_token == nullptr || sl_set_constants == nullptr || sl_set_tag_for_frame == nullptr || sl_evaluate_feature == nullptr) {
		dlss_runtime().disable_runtime("DLSS SR was disabled because Streamline did not expose all required core functions. Falling back to FSR 2.");
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
		memdelete(data);
		return;
	}

	sl::FrameToken *frame_token = nullptr;
	sl::Result result = sl_get_new_frame_token(frame_token, nullptr);
	if (result != sl::Result::eOk || frame_token == nullptr) {
		dlss_runtime().disable_runtime(vformat("DLSS SR was disabled because Streamline could not create a frame token: %s. Falling back to FSR 2.", _streamline_result_to_string(result)));
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
		memdelete(data);
		return;
	}

	sl::ViewportHandle viewport(data->viewport_id);

	sl::DLSSOptions options;
	options.mode = data->mode;
	options.outputWidth = uint32_t(data->target_size.x);
	options.outputHeight = uint32_t(data->target_size.y);
	options.preExposure = 1.0f;
	options.exposureScale = 1.0f;
	options.colorBuffersHDR = sl::Boolean::eTrue;
	options.useAutoExposure = data->use_auto_exposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.alphaUpscalingEnabled = sl::Boolean::eFalse;
	options.dlaaPreset = sl::DLSSPreset::ePresetK;
	options.qualityPreset = sl::DLSSPreset::ePresetK;
	options.balancedPreset = sl::DLSSPreset::ePresetK;
	options.performancePreset = sl::DLSSPreset::ePresetM;
	options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
	options.ultraQualityPreset = sl::DLSSPreset::ePresetK;

	result = dlss_runtime().sl_dlss_set_options(viewport, options);
	if (result != sl::Result::eOk) {
		dlss_runtime().disable_runtime(vformat("DLSS SR was disabled because the options update failed: %s. Falling back to FSR 2.", _streamline_result_to_string(result)));
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
		memdelete(data);
		return;
	}

	sl::Constants constants;
	_sl_store_projection(data->camera_view_to_clip, constants.cameraViewToClip);
	_sl_store_projection(data->camera_view_to_clip.inverse(), constants.clipToCameraView);
	_sl_store_projection(data->reprojection, constants.clipToPrevClip);
	_sl_store_projection(data->reprojection.inverse(), constants.prevClipToClip);
	constants.jitterOffset = sl::float2(data->jitter.x, data->jitter.y);
	constants.mvecScale = sl::float2(1.0f, 1.0f);
	constants.cameraPos = _sl_make_float3(data->camera_transform.origin);
	constants.cameraUp = _sl_make_float3(data->camera_transform.basis.get_column(Vector3::AXIS_Y).normalized());
	constants.cameraRight = _sl_make_float3(data->camera_transform.basis.get_column(Vector3::AXIS_X).normalized());
	constants.cameraFwd = _sl_make_float3((-data->camera_transform.basis.get_column(Vector3::AXIS_Z)).normalized());
	constants.cameraNear = data->z_near;
	constants.cameraFar = data->z_far;
	constants.cameraFOV = data->fovy;
	constants.cameraAspectRatio = data->aspect;
	constants.depthInverted = sl::Boolean::eTrue;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D = sl::Boolean::eFalse;
	constants.reset = data->reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.orthographicProjection = data->orthogonal_projection ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.motionVectorsDilated = sl::Boolean::eFalse;
	constants.motionVectorsJittered = sl::Boolean::eFalse;

	result = sl_set_constants(constants, *frame_token, viewport);
	if (result != sl::Result::eOk) {
		dlss_runtime().disable_runtime(vformat("DLSS SR was disabled because the constants update failed: %s. Falling back to FSR 2.", _streamline_result_to_string(result)));
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
		memdelete(data);
		return;
	}

	sl::Extent input_extent;
	input_extent.width = uint32_t(data->internal_size.x);
	input_extent.height = uint32_t(data->internal_size.y);
	sl::Extent output_extent;
	output_extent.width = uint32_t(data->target_size.x);
	output_extent.height = uint32_t(data->target_size.y);
	sl::Extent exposure_extent;
	exposure_extent.width = 1;
	exposure_extent.height = 1;

	sl::Resource color = _sl_make_texture_resource(data->color, data->internal_size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource depth = _sl_make_texture_resource(data->depth, data->internal_size, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	sl::Resource velocity = _sl_make_texture_resource(data->velocity, data->internal_size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource output = _sl_make_texture_resource(data->output, data->target_size, VK_IMAGE_LAYOUT_GENERAL);
	sl::Resource reactive = _sl_make_texture_resource(data->reactive, data->internal_size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource exposure = _sl_make_texture_resource(data->exposure, Size2i(1, 1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	sl::ResourceTag tags[6];
	uint32_t tag_count = 0;
	tags[tag_count++] = sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &input_extent);
	tags[tag_count++] = sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &output_extent);
	tags[tag_count++] = sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &input_extent);
	tags[tag_count++] = sl::ResourceTag(&velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &input_extent);
	if (!data->use_auto_exposure) {
		tags[tag_count++] = sl::ResourceTag(&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eOnlyValidNow, &exposure_extent);
	}
	if (data->has_reactive) {
		tags[tag_count++] = sl::ResourceTag(&reactive, sl::kBufferTypeReactiveMaskHint, sl::ResourceLifecycle::eOnlyValidNow, &input_extent);
	}

	sl::CommandBuffer *sl_command_buffer = reinterpret_cast<sl::CommandBuffer *>(command_buffer);
	result = sl_set_tag_for_frame(*frame_token, viewport, tags, tag_count, sl_command_buffer);
	if (result != sl::Result::eOk) {
		dlss_runtime().disable_runtime(vformat("DLSS SR was disabled because resource tagging failed: %s. Falling back to FSR 2.", _streamline_result_to_string(result)));
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
		memdelete(data);
		return;
	}

	const sl::BaseStructure *inputs[] = { &viewport };
	result = sl_evaluate_feature(sl::kFeatureDLSS, *frame_token, inputs, 1, sl_command_buffer);
	if (result != sl::Result::eOk) {
		dlss_runtime().disable_runtime(vformat("DLSS SR was disabled because the Streamline dispatch failed: %s. Falling back to FSR 2.", _streamline_result_to_string(result)));
		WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
	}

	memdelete(data);
}

struct DlssFrameGenerationCallbackData {
	uint32_t viewport_id = 0;
	uint32_t frame_index = 0;
	VkFormat backbuffer_format = VK_FORMAT_UNDEFINED;
	VulkanTextureHandles depth;
	VulkanTextureHandles velocity;
	VulkanTextureHandles hudless_color;
	Size2i render_size;
	Size2i display_size;
	Rect2i generation_rect;
	Vector2 jitter;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float aspect = 1.0f;
	bool reset_accumulation = false;
	bool orthogonal_projection = false;
	bool has_hudless_color = false;
	Projection camera_view_to_clip;
	Projection reprojection;
	Transform3D camera_transform;
};

static void _dlss_frame_generation_driver_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DlssFrameGenerationCallbackData *data = static_cast<DlssFrameGenerationCallbackData *>(p_userdata);
	ERR_FAIL_NULL(data);

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	if (command_buffer == VK_NULL_HANDLE) {
		WARN_PRINT("DLSS Frame Generation prepare skipped because the native command buffer handle is unavailable.");
		memdelete(data);
		return;
	}

	if (!dlss_frame_generation_runtime().resolve()) {
		WARN_PRINT(vformat("DLSS Frame Generation prepare skipped: %s", dlss_frame_generation_runtime().get_unavailable_reason()));
		memdelete(data);
		return;
	}

	Streamline *streamline = Streamline::get_singleton();
	StreamlineContext &context = StreamlineContext::get();
	PFun_slSetConstants *sl_set_constants = reinterpret_cast<PFun_slSetConstants *>(context.slSetConstants);
	PFun_slSetTagForFrame *sl_set_tag_for_frame = reinterpret_cast<PFun_slSetTagForFrame *>(context.slSetTagForFrame);
	if (streamline == nullptr || sl_set_constants == nullptr || sl_set_tag_for_frame == nullptr) {
		WARN_PRINT("DLSS Frame Generation prepare skipped because Streamline did not expose all required core functions.");
		memdelete(data);
		return;
	}

	sl::FrameToken *frame_token = static_cast<sl::FrameToken *>(streamline->get_frame_token(data->frame_index));
	if (frame_token == nullptr) {
		WARN_PRINT("DLSS Frame Generation prepare skipped because Streamline could not create a frame token.");
		memdelete(data);
		return;
	}

	sl::ViewportHandle viewport(data->viewport_id);

	sl::DLSSGOptions options;
	options.mode = sl::DLSSGMode::eOn;
	options.numFramesToGenerate = 1;
	options.numBackBuffers = 0;
	options.mvecDepthWidth = uint32_t(data->render_size.x);
	options.mvecDepthHeight = uint32_t(data->render_size.y);
	options.colorWidth = uint32_t(data->display_size.x);
	options.colorHeight = uint32_t(data->display_size.y);
	options.colorBufferFormat = uint32_t(data->backbuffer_format);
	options.mvecBufferFormat = uint32_t(data->velocity.format);
	options.depthBufferFormat = uint32_t(data->depth.format);
	options.hudLessBufferFormat = data->has_hudless_color ? uint32_t(data->hudless_color.format) : 0;
	options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
	options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

	sl::Result result = dlss_frame_generation_runtime().sl_dlss_g_set_options(viewport, options);
	if (result != sl::Result::eOk) {
		WARN_PRINT(vformat("DLSS Frame Generation options update failed: %s.", _streamline_result_to_string(result)));
		memdelete(data);
		return;
	}

	sl::DLSSGState state;
	result = dlss_frame_generation_runtime().sl_dlss_g_get_state(viewport, state, &options);
	if (result == sl::Result::eOk && state.status != sl::DLSSGStatus::eOk) {
		WARN_PRINT_ONCE("DLSS Frame Generation reported a runtime status warning. Check Streamline logs for the vendor-specific status details.");
	}

	sl::Constants constants;
	_dlss_fill_common_constants(constants, data->camera_view_to_clip, data->reprojection, data->jitter, data->z_near, data->z_far, data->fovy, data->aspect, data->reset_accumulation, data->orthogonal_projection, data->camera_transform);

	result = sl_set_constants(constants, *frame_token, viewport);
	if (result != sl::Result::eOk) {
		WARN_PRINT(vformat("DLSS Frame Generation constants update failed: %s.", _streamline_result_to_string(result)));
		memdelete(data);
		return;
	}

	sl::Extent input_extent;
	input_extent.width = uint32_t(data->render_size.x);
	input_extent.height = uint32_t(data->render_size.y);
	sl::Extent display_extent;
	display_extent.left = uint32_t(MAX(data->generation_rect.position.x, 0));
	display_extent.top = uint32_t(MAX(data->generation_rect.position.y, 0));
	display_extent.width = uint32_t(MAX(data->generation_rect.size.x, 0));
	display_extent.height = uint32_t(MAX(data->generation_rect.size.y, 0));

	sl::Resource depth = _sl_make_texture_resource(data->depth, data->render_size, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	sl::Resource velocity = _sl_make_texture_resource(data->velocity, data->render_size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sl::Resource hudless_color = _sl_make_texture_resource(data->hudless_color, data->display_size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	sl::ResourceTag tags[4];
	uint32_t tag_count = 0;
	tags[tag_count++] = sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &display_extent);
	tags[tag_count++] = sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &input_extent);
	tags[tag_count++] = sl::ResourceTag(&velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &input_extent);
	if (data->has_hudless_color) {
		tags[tag_count++] = sl::ResourceTag(&hudless_color, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &display_extent);
	}

	sl::CommandBuffer *sl_command_buffer = reinterpret_cast<sl::CommandBuffer *>(command_buffer);
	result = sl_set_tag_for_frame(*frame_token, viewport, tags, tag_count, sl_command_buffer);
	if (result != sl::Result::eOk) {
		WARN_PRINT(vformat("DLSS Frame Generation resource tagging failed: %s.", _streamline_result_to_string(result)));
	}

	memdelete(data);
}

} // namespace
#endif

#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
namespace {

static const char *_fsr31_result_to_string(ffxReturnCode_t p_result) {
	switch (p_result) {
		case FFX_API_RETURN_OK:
			return "ok";
		case FFX_API_RETURN_ERROR:
			return "error";
		case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
			return "unknown descriptor type";
		case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
			return "runtime error";
		case FFX_API_RETURN_NO_PROVIDER:
			return "no provider";
		case FFX_API_RETURN_ERROR_MEMORY:
			return "memory allocation failed";
		case FFX_API_RETURN_ERROR_PARAMETER:
			return "invalid parameter";
		default:
			return "unrecognized FidelityFX API result";
	}
}

static bool _fsr31_resolve_symbol(void *p_library_handle, const String &p_symbol, void *&r_symbol) {
	r_symbol = nullptr;
	return OS::get_singleton()->get_dynamic_library_symbol_handle(p_library_handle, p_symbol, r_symbol, true) == OK && r_symbol != nullptr;
}

static void _fsr31_message_callback(uint32_t p_type, const wchar_t *p_message) {
	if (p_message == nullptr) {
		return;
	}

	const String message = String::utf16((const char16_t *)p_message);
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR || p_type == FFX_API_MESSAGE_TYPE_WARNING) {
		WARN_PRINT(vformat("FidelityFX FSR 3.1: %s", message));
	} else {
		print_verbose(vformat("FidelityFX FSR 3.1: %s", message));
	}
}

static uint32_t _fsr31_context_flags(bool p_has_exposure) {
	uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
	if (!p_has_exposure) {
		flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	}
	return flags;
}

static bool _fsr31_fill_backend_desc(ffxCreateBackendVKDesc &r_backend_desc, String &r_unavailable_reason) {
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		r_unavailable_reason = "RenderingDevice is not initialized.";
		return false;
	}

	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	if (physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
		r_unavailable_reason = "Vulkan native device handles are not available from RenderingDevice.";
		return false;
	}

	r_backend_desc = {};
	r_backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	r_backend_desc.header.pNext = nullptr;
	r_backend_desc.vkDevice = device;
	r_backend_desc.vkPhysicalDevice = physical_device;
	r_backend_desc.vkDeviceProcAddr = (PFN_vkGetDeviceProcAddr)rd->get_driver_resource(RD::DRIVER_RESOURCE_DEVICE_PROC_ADDR);
	if (r_backend_desc.vkDeviceProcAddr == nullptr) {
		r_backend_desc.vkDeviceProcAddr = vkGetDeviceProcAddr;
	}
	return true;
}

static bool _fsr31_should_skip_context_creation_on_device(String &r_unavailable_reason) {
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		r_unavailable_reason = "RenderingDevice is not initialized.";
		return true;
	}

	OS *os = OS::get_singleton();
	const bool force_fsr31_on_nvidia = os != nullptr && os->get_environment("GODOT_FORCE_FSR31_VULKAN_ON_NVIDIA") == "1";
	if (rd->get_device().vendor == RenderingContextDriver::Vendor::VENDOR_NVIDIA && !force_fsr31_on_nvidia) {
		r_unavailable_reason = "FidelityFX FSR 3.1 Vulkan SR is disabled on NVIDIA by default because the signed Vulkan runtime crashes during context creation in this integration. Use DLSS on NVIDIA, or set GODOT_FORCE_FSR31_VULKAN_ON_NVIDIA=1 for debugging.";
		return true;
	}

	return false;
}

static uint32_t _fsr31_usage_from_vk(VkImageUsageFlags p_vk_usage, VkFormat p_format, uint32_t p_additional_usage) {
	uint32_t usage = FFX_API_RESOURCE_USAGE_READ_ONLY | p_additional_usage;
	if (ffxApiIsDepthFormat(p_format) || (p_vk_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
		usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
	}
	if (ffxApiIsStencilFormat(p_format)) {
		usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
	}
	if ((p_vk_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
		usage |= FFX_API_RESOURCE_USAGE_UAV;
	}
	if ((p_vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
		usage |= FFX_API_RESOURCE_USAGE_RENDERTARGET;
	}
	return usage;
}

static FfxApiResource _fsr31_make_empty_resource(uint32_t p_state) {
	return ffxApiGetResourceVK(nullptr, FfxApiResourceDescription(), p_state);
}

static FfxApiResource _fsr31_make_texture_resource(const VulkanTextureHandles &p_texture, const Size2i &p_size, uint32_t p_state, uint32_t p_additional_usage = 0) {
	if (p_texture.image == VK_NULL_HANDLE) {
		return _fsr31_make_empty_resource(p_state);
	}

	FfxApiResourceDescription description = {};
	description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format = ffxApiGetSurfaceFormatVK(p_texture.format);
	description.width = uint32_t(MAX(p_size.x, 0));
	description.height = uint32_t(MAX(p_size.y, 0));
	description.depth = 1;
	description.mipCount = 1;
	description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	description.usage = _fsr31_usage_from_vk(p_texture.usage, p_texture.format, p_additional_usage);

	return ffxApiGetResourceVK((void *)p_texture.image, description, p_state);
}

struct Fsr31ContextKey {
	uint64_t output_image = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	uint32_t flags = 0;

	bool operator==(const Fsr31ContextKey &p_other) const {
		return output_image == p_other.output_image &&
				input_width == p_other.input_width &&
				input_height == p_other.input_height &&
				output_width == p_other.output_width &&
				output_height == p_other.output_height &&
				flags == p_other.flags;
	}
};

struct Fsr31UpscaleContext {
	Fsr31ContextKey key;
	ffxContext context = nullptr;
};

struct Fsr31FrameGenerationContextKey {
	uint64_t swapchain = 0;
	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	uint32_t backbuffer_format = 0;
	uint32_t flags = 0;

	bool operator==(const Fsr31FrameGenerationContextKey &p_other) const {
		return swapchain == p_other.swapchain &&
				render_width == p_other.render_width &&
				render_height == p_other.render_height &&
				display_width == p_other.display_width &&
				display_height == p_other.display_height &&
				backbuffer_format == p_other.backbuffer_format &&
				flags == p_other.flags;
	}
};

struct Fsr31FrameGenerationContext {
	Fsr31FrameGenerationContextKey key;
	ffxContext context = nullptr;
	uint64_t frame_id = 0;
};

class Fsr31Runtime {
	void *library_handle = nullptr;
	bool attempted_load = false;
	bool loaded = false;
	bool device_probe_attempted = false;
	bool device_supported = false;
	bool frame_generation_probe_attempted = false;
	bool frame_generation_supported = false;
	String unavailable_reason = "FidelityFX FSR 3.1 Vulkan runtime has not been loaded.";
	String frame_generation_unavailable_reason = "FidelityFX FSR 3 frame generation Vulkan runtime has not been probed.";
	mutable CharString unavailable_reason_utf8;
	mutable CharString frame_generation_unavailable_reason_utf8;
	Vector<Fsr31UpscaleContext *> contexts;
	Vector<Fsr31FrameGenerationContext *> frame_generation_contexts;

public:
	PfnFfxCreateContext ffx_create_context = nullptr;
	PfnFfxDestroyContext ffx_destroy_context = nullptr;
	PfnFfxConfigure ffx_configure = nullptr;
	PfnFfxQuery ffx_query = nullptr;
	PfnFfxDispatch ffx_dispatch = nullptr;

	~Fsr31Runtime() {
		unload();
	}

	bool load() {
		if (attempted_load) {
			return loaded;
		}

		attempted_load = true;

		OS *os = OS::get_singleton();
		ERR_FAIL_NULL_V(os, false);

		const char *const library_names[] = {
#if defined(WINDOWS_ENABLED)
			"amd_fidelityfx_vk.dll",
#elif defined(LINUXBSD_ENABLED)
			"libamd_fidelityfx_vk.so",
			"amd_fidelityfx_vk.so",
#else
			"amd_fidelityfx_vk.dll",
			"libamd_fidelityfx_vk.so",
#endif
		};
		String resolved_library_name;
		if (!_open_vendor_library(library_names, std_size(library_names), library_handle, resolved_library_name)) {
			library_handle = nullptr;
			unavailable_reason = "The FidelityFX Vulkan runtime library could not be loaded.";
			return false;
		}

		bool resolved = true;
		void *symbol = nullptr;
		resolved = _fsr31_resolve_symbol(library_handle, "ffxCreateContext", symbol) && resolved;
		ffx_create_context = reinterpret_cast<PfnFfxCreateContext>(symbol);
		resolved = _fsr31_resolve_symbol(library_handle, "ffxDestroyContext", symbol) && resolved;
		ffx_destroy_context = reinterpret_cast<PfnFfxDestroyContext>(symbol);
		resolved = _fsr31_resolve_symbol(library_handle, "ffxConfigure", symbol) && resolved;
		ffx_configure = reinterpret_cast<PfnFfxConfigure>(symbol);
		resolved = _fsr31_resolve_symbol(library_handle, "ffxQuery", symbol) && resolved;
		ffx_query = reinterpret_cast<PfnFfxQuery>(symbol);
		resolved = _fsr31_resolve_symbol(library_handle, "ffxDispatch", symbol) && resolved;
		ffx_dispatch = reinterpret_cast<PfnFfxDispatch>(symbol);

		if (!resolved) {
			unload();
			attempted_load = true;
			unavailable_reason = vformat("%s is missing one or more required FidelityFX API exports.", resolved_library_name);
			return false;
		}

		loaded = true;
		unavailable_reason = "FidelityFX FSR 3.1 Vulkan runtime is loaded, but the Vulkan device has not been probed.";
		return true;
	}

	void unload() {
		for (Fsr31UpscaleContext *context : contexts) {
			if (context != nullptr && context->context != nullptr && ffx_destroy_context != nullptr) {
				ffx_destroy_context(&context->context, nullptr);
			}
			memdelete(context);
		}
		contexts.clear();
		for (Fsr31FrameGenerationContext *context : frame_generation_contexts) {
			if (context != nullptr && context->context != nullptr && ffx_destroy_context != nullptr) {
				ffx_destroy_context(&context->context, nullptr);
			}
			memdelete(context);
		}
		frame_generation_contexts.clear();

		if (library_handle != nullptr && OS::get_singleton() != nullptr) {
			OS::get_singleton()->close_dynamic_library(library_handle);
		}
		library_handle = nullptr;
		loaded = false;
		device_probe_attempted = false;
		device_supported = false;
		frame_generation_probe_attempted = false;
		frame_generation_supported = false;
		ffx_create_context = nullptr;
		ffx_destroy_context = nullptr;
		ffx_configure = nullptr;
		ffx_query = nullptr;
		ffx_dispatch = nullptr;
		unavailable_reason = "FidelityFX FSR 3.1 Vulkan runtime is not loaded.";
		frame_generation_unavailable_reason = "FidelityFX FSR 3 frame generation Vulkan runtime is not loaded.";
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

		if (_fsr31_should_skip_context_creation_on_device(unavailable_reason)) {
			return false;
		}

		ffxCreateBackendVKDesc backend_desc = {};
		if (!_fsr31_fill_backend_desc(backend_desc, unavailable_reason)) {
			return false;
		}

		ffxCreateContextDescUpscale create_desc = {};
		create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		create_desc.header.pNext = &backend_desc.header;
		create_desc.flags = _fsr31_context_flags(false);
		create_desc.maxRenderSize = { 128, 128 };
		create_desc.maxUpscaleSize = { 256, 256 };
		create_desc.fpMessage = _fsr31_message_callback;

		ffxContext probe_context = nullptr;
		const ffxReturnCode_t result = ffx_create_context(&probe_context, &create_desc.header, nullptr);
		if (result != FFX_API_RETURN_OK || probe_context == nullptr) {
			unavailable_reason = vformat("FidelityFX FSR 3.1 Vulkan context probe failed: %s.", _fsr31_result_to_string(result));
			return false;
		}

		ffx_destroy_context(&probe_context, nullptr);
		device_supported = true;
		unavailable_reason = "FidelityFX FSR 3.1 Vulkan SR is available.";
		return true;
	}

	const String &get_unavailable_reason() const {
		return unavailable_reason;
	}

	const char *get_unavailable_reason_cstr() const {
		unavailable_reason_utf8 = unavailable_reason.utf8();
		return unavailable_reason_utf8.get_data();
	}

	const String &get_frame_generation_unavailable_reason() const {
		return frame_generation_unavailable_reason;
	}

	const char *get_frame_generation_unavailable_reason_cstr() const {
		frame_generation_unavailable_reason_utf8 = frame_generation_unavailable_reason.utf8();
		return frame_generation_unavailable_reason_utf8.get_data();
	}

	bool probe_frame_generation() {
		if (frame_generation_probe_attempted) {
			return frame_generation_supported;
		}

		frame_generation_probe_attempted = true;
		frame_generation_supported = false;

		if (!load()) {
			frame_generation_unavailable_reason = unavailable_reason;
			return false;
		}

		if (_fsr31_should_skip_context_creation_on_device(frame_generation_unavailable_reason)) {
			return false;
		}

		ffxCreateBackendVKDesc backend_desc = {};
		if (!_fsr31_fill_backend_desc(backend_desc, frame_generation_unavailable_reason)) {
			return false;
		}

		ffxCreateContextDescFrameGeneration create_desc = {};
		create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		create_desc.header.pNext = &backend_desc.header;
		create_desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
		create_desc.displaySize = { 256, 256 };
		create_desc.maxRenderSize = { 128, 128 };
		create_desc.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;

		ffxContext probe_context = nullptr;
		const ffxReturnCode_t result = ffx_create_context(&probe_context, &create_desc.header, nullptr);
		if (result != FFX_API_RETURN_OK || probe_context == nullptr) {
			frame_generation_unavailable_reason = vformat("FidelityFX FSR 3 frame generation context probe failed: %s.", _fsr31_result_to_string(result));
			return false;
		}

		ffx_destroy_context(&probe_context, nullptr);
		frame_generation_supported = true;
		frame_generation_unavailable_reason = "FidelityFX FSR 3 frame generation is available.";
		return true;
	}

	Fsr31UpscaleContext *get_or_create_context(const Fsr31ContextKey &p_key) {
		for (Fsr31UpscaleContext *context : contexts) {
			if (context != nullptr && context->key == p_key) {
				return context;
			}
		}

		if (!probe_device()) {
			return nullptr;
		}

		ffxCreateBackendVKDesc backend_desc = {};
		if (!_fsr31_fill_backend_desc(backend_desc, unavailable_reason)) {
			return nullptr;
		}

		ffxCreateContextDescUpscale create_desc = {};
		create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		create_desc.header.pNext = &backend_desc.header;
		create_desc.flags = p_key.flags;
		create_desc.maxRenderSize = { p_key.input_width, p_key.input_height };
		create_desc.maxUpscaleSize = { p_key.output_width, p_key.output_height };
		create_desc.fpMessage = _fsr31_message_callback;

		ffxContext ffx_context = nullptr;
		ffxReturnCode_t result = ffx_create_context(&ffx_context, &create_desc.header, nullptr);
		if (result != FFX_API_RETURN_OK || ffx_context == nullptr) {
			unavailable_reason = vformat("FidelityFX FSR 3.1 Vulkan context creation failed: %s.", _fsr31_result_to_string(result));
			return nullptr;
		}

		Fsr31UpscaleContext *context = memnew(Fsr31UpscaleContext);
		context->key = p_key;
		context->context = ffx_context;
		contexts.push_back(context);

		ffxQueryGetProviderVersion provider_version = {};
		provider_version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		result = ffx_query(&context->context, &provider_version.header);
		if (result == FFX_API_RETURN_OK && provider_version.versionName != nullptr) {
			unavailable_reason = vformat("FidelityFX FSR 3.1 Vulkan SR is available through provider %s.", provider_version.versionName);
		} else {
			unavailable_reason = "FidelityFX FSR 3.1 Vulkan SR is available.";
		}

		return context;
	}

	Fsr31FrameGenerationContext *get_or_create_frame_generation_context(const Fsr31FrameGenerationContextKey &p_key) {
		for (Fsr31FrameGenerationContext *context : frame_generation_contexts) {
			if (context != nullptr && context->key == p_key) {
				return context;
			}
		}

		if (!probe_frame_generation()) {
			return nullptr;
		}

		ffxCreateBackendVKDesc backend_desc = {};
		if (!_fsr31_fill_backend_desc(backend_desc, frame_generation_unavailable_reason)) {
			return nullptr;
		}

		ffxCreateContextDescFrameGeneration create_desc = {};
		create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		create_desc.header.pNext = &backend_desc.header;
		create_desc.flags = p_key.flags;
		create_desc.displaySize = { p_key.display_width, p_key.display_height };
		create_desc.maxRenderSize = { p_key.render_width, p_key.render_height };
		create_desc.backBufferFormat = p_key.backbuffer_format;

		ffxContext ffx_context = nullptr;
		const ffxReturnCode_t result = ffx_create_context(&ffx_context, &create_desc.header, nullptr);
		if (result != FFX_API_RETURN_OK || ffx_context == nullptr) {
			frame_generation_unavailable_reason = vformat("FidelityFX FSR 3 frame generation context creation failed: %s.", _fsr31_result_to_string(result));
			return nullptr;
		}

		Fsr31FrameGenerationContext *context = memnew(Fsr31FrameGenerationContext);
		context->key = p_key;
		context->context = ffx_context;
		frame_generation_contexts.push_back(context);
		frame_generation_unavailable_reason = "FidelityFX FSR 3 frame generation is available.";
		return context;
	}
};

static Fsr31Runtime &fsr31_runtime() {
	static Fsr31Runtime runtime;
	return runtime;
}

struct Fsr31CallbackData {
	Fsr31UpscaleContext *context = nullptr;
	VulkanTextureHandles color;
	VulkanTextureHandles depth;
	VulkanTextureHandles velocity;
	VulkanTextureHandles reactive;
	VulkanTextureHandles exposure;
	VulkanTextureHandles output;
	Size2i internal_size;
	Size2i target_size;
	Vector2 jitter;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float sharpness = 0.0f;
	float delta_time = 0.0f;
	bool reset_accumulation = false;
	bool has_reactive = false;
	bool has_exposure = false;
};

static void _fsr31_driver_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	Fsr31CallbackData *data = static_cast<Fsr31CallbackData *>(p_userdata);
	ERR_FAIL_NULL(data);

	if (data->context == nullptr || data->context->context == nullptr || fsr31_runtime().ffx_dispatch == nullptr) {
		WARN_PRINT("FidelityFX FSR 3.1 dispatch skipped because the runtime context is unavailable.");
		memdelete(data);
		return;
	}

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	if (command_buffer == VK_NULL_HANDLE) {
		WARN_PRINT("FidelityFX FSR 3.1 dispatch skipped because the native command buffer handle is unavailable.");
		memdelete(data);
		return;
	}

	ffxDispatchDescUpscale dispatch_desc = {};
	dispatch_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch_desc.header.pNext = nullptr;
	dispatch_desc.commandList = command_buffer;
	dispatch_desc.color = _fsr31_make_texture_resource(data->color, data->internal_size, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.depth = _fsr31_make_texture_resource(data->depth, data->internal_size, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.motionVectors = _fsr31_make_texture_resource(data->velocity, data->internal_size, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.exposure = data->has_exposure ? _fsr31_make_texture_resource(data->exposure, Size2i(1, 1), FFX_API_RESOURCE_STATE_COMPUTE_READ) : _fsr31_make_empty_resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.reactive = data->has_reactive ? _fsr31_make_texture_resource(data->reactive, data->internal_size, FFX_API_RESOURCE_STATE_COMPUTE_READ) : _fsr31_make_empty_resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.transparencyAndComposition = _fsr31_make_empty_resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.output = _fsr31_make_texture_resource(data->output, data->target_size, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
	dispatch_desc.jitterOffset = { data->jitter.x, data->jitter.y };
	dispatch_desc.motionVectorScale = { float(data->internal_size.x), float(data->internal_size.y) };
	dispatch_desc.renderSize = { uint32_t(data->internal_size.x), uint32_t(data->internal_size.y) };
	dispatch_desc.upscaleSize = { uint32_t(data->target_size.x), uint32_t(data->target_size.y) };
	dispatch_desc.enableSharpening = data->sharpness > 1e-6f;
	dispatch_desc.sharpness = data->sharpness;
	dispatch_desc.frameTimeDelta = data->delta_time * 1000.0f;
	dispatch_desc.preExposure = 1.0f;
	dispatch_desc.reset = data->reset_accumulation;
	dispatch_desc.cameraNear = data->z_near;
	dispatch_desc.cameraFar = data->z_far;
	dispatch_desc.cameraFovAngleVertical = data->fovy;
	dispatch_desc.viewSpaceToMetersFactor = 1.0f;
	dispatch_desc.flags = 0;

	const ffxReturnCode_t result = fsr31_runtime().ffx_dispatch(&data->context->context, &dispatch_desc.header);
	if (result != FFX_API_RETURN_OK) {
		WARN_PRINT(vformat("FidelityFX FSR 3.1 dispatch failed: %s.", _fsr31_result_to_string(result)));
	}

	memdelete(data);
}

struct Fsr31FrameGenerationCallbackData {
	Fsr31FrameGenerationContext *context = nullptr;
	uint64_t swapchain = 0;
	VulkanTextureHandles depth;
	VulkanTextureHandles velocity;
	VulkanTextureHandles hudless_color;
	Size2i render_size;
	Size2i display_size;
	Rect2i generation_rect;
	Vector2 jitter;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float delta_time = 0.0f;
	bool reset_accumulation = false;
	bool has_hudless_color = false;
	Transform3D camera_transform;
	uint64_t frame_id = 0;
};

static void _fsr31_frame_generation_driver_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	Fsr31FrameGenerationCallbackData *data = static_cast<Fsr31FrameGenerationCallbackData *>(p_userdata);
	ERR_FAIL_NULL(data);

	if (data->context == nullptr || data->context->context == nullptr || fsr31_runtime().ffx_configure == nullptr || fsr31_runtime().ffx_dispatch == nullptr) {
		WARN_PRINT("FidelityFX FSR 3 frame generation skipped because the runtime context is unavailable.");
		memdelete(data);
		return;
	}

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	if (command_buffer == VK_NULL_HANDLE) {
		WARN_PRINT("FidelityFX FSR 3 frame generation skipped because the native command buffer handle is unavailable.");
		memdelete(data);
		return;
	}

	ffxConfigureDescFrameGeneration configure_desc = {};
	configure_desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	configure_desc.header.pNext = nullptr;
	configure_desc.swapChain = (void *)data->swapchain;
	configure_desc.presentCallback = nullptr;
	configure_desc.presentCallbackUserContext = nullptr;
	configure_desc.frameGenerationCallback = [](ffxDispatchDescFrameGeneration *p_params, void *p_user_context) -> ffxReturnCode_t {
		return fsr31_runtime().ffx_dispatch(reinterpret_cast<ffxContext *>(p_user_context), &p_params->header);
	};
	configure_desc.frameGenerationCallbackUserContext = &data->context->context;
	configure_desc.frameGenerationEnabled = true;
	configure_desc.allowAsyncWorkloads = false;
	configure_desc.HUDLessColor = data->has_hudless_color ? _fsr31_make_texture_resource(data->hudless_color, data->display_size, FFX_API_RESOURCE_STATE_COMPUTE_READ) : _fsr31_make_empty_resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
	configure_desc.flags = 0;
	configure_desc.onlyPresentGenerated = false;
	configure_desc.generationRect.left = data->generation_rect.position.x;
	configure_desc.generationRect.top = data->generation_rect.position.y;
	configure_desc.generationRect.width = data->generation_rect.size.x;
	configure_desc.generationRect.height = data->generation_rect.size.y;
	configure_desc.frameID = data->frame_id;

	ffxReturnCode_t result = fsr31_runtime().ffx_configure(&data->context->context, &configure_desc.header);
	if (result != FFX_API_RETURN_OK) {
		WARN_PRINT(vformat("FidelityFX FSR 3 frame generation configuration failed: %s.", _fsr31_result_to_string(result)));
		memdelete(data);
		return;
	}

	ffxDispatchDescFrameGenerationPrepare prepare_desc = {};
	prepare_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare_desc.header.pNext = nullptr;
	prepare_desc.frameID = data->frame_id;
	prepare_desc.flags = 0;
	prepare_desc.commandList = command_buffer;
	prepare_desc.renderSize = { uint32_t(data->render_size.x), uint32_t(data->render_size.y) };
	prepare_desc.jitterOffset = { -data->jitter.x, -data->jitter.y };
	prepare_desc.motionVectorScale = { float(data->render_size.x), float(data->render_size.y) };
	prepare_desc.frameTimeDelta = data->delta_time * 1000.0f;
	prepare_desc.unused_reset = data->reset_accumulation;
	prepare_desc.cameraNear = data->z_near;
	prepare_desc.cameraFar = data->z_far;
	prepare_desc.cameraFovAngleVertical = data->fovy;
	prepare_desc.viewSpaceToMetersFactor = 1.0f;
	prepare_desc.depth = _fsr31_make_texture_resource(data->depth, data->render_size, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare_desc.motionVectors = _fsr31_make_texture_resource(data->velocity, data->render_size, FFX_API_RESOURCE_STATE_COMPUTE_READ);

	ffxDispatchDescFrameGenerationPrepareCameraInfo camera_info = {};
	camera_info.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	camera_info.header.pNext = nullptr;
	Vector3 camera_up = data->camera_transform.basis.get_column(Vector3::AXIS_Y).normalized();
	Vector3 camera_right = data->camera_transform.basis.get_column(Vector3::AXIS_X).normalized();
	Vector3 camera_forward = (-data->camera_transform.basis.get_column(Vector3::AXIS_Z)).normalized();
	camera_info.cameraPosition[0] = data->camera_transform.origin.x;
	camera_info.cameraPosition[1] = data->camera_transform.origin.y;
	camera_info.cameraPosition[2] = data->camera_transform.origin.z;
	camera_info.cameraUp[0] = camera_up.x;
	camera_info.cameraUp[1] = camera_up.y;
	camera_info.cameraUp[2] = camera_up.z;
	camera_info.cameraRight[0] = camera_right.x;
	camera_info.cameraRight[1] = camera_right.y;
	camera_info.cameraRight[2] = camera_right.z;
	camera_info.cameraForward[0] = camera_forward.x;
	camera_info.cameraForward[1] = camera_forward.y;
	camera_info.cameraForward[2] = camera_forward.z;
	prepare_desc.header.pNext = &camera_info.header;

	result = fsr31_runtime().ffx_dispatch(&data->context->context, &prepare_desc.header);
	if (result != FFX_API_RETURN_OK) {
		WARN_PRINT(vformat("FidelityFX FSR 3 frame generation prepare dispatch failed: %s.", _fsr31_result_to_string(result)));
	}

	memdelete(data);
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

	OS *os = OS::get_singleton();
	const bool force_xess_on_non_intel = os != nullptr && os->get_environment("GODOT_FORCE_XESS_VULKAN_ON_NON_INTEL") == "1";
	if (rd->get_device().vendor != RenderingContextDriver::Vendor::VENDOR_INTEL && !force_xess_on_non_intel) {
		r_unavailable_reason = "XeSS Vulkan SR is enabled by default only on Intel GPUs because the native XeSS runtime crashed during context creation on the tested non-Intel Vulkan driver. Set GODOT_FORCE_XESS_VULKAN_ON_NON_INTEL=1 for debugging.";
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

static bool _xess_resolve_symbol(void *p_library_handle, const String &p_symbol, void *&r_symbol) {
	return OS::get_singleton()->get_dynamic_library_symbol_handle(p_library_handle, p_symbol, r_symbol, true) == OK && r_symbol != nullptr;
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
	void *library_handle = nullptr;
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

		OS *os = OS::get_singleton();
		ERR_FAIL_NULL_V(os, false);

		const char *const library_names[] = {
#if defined(WINDOWS_ENABLED)
			"libxess.dll",
#elif defined(LINUXBSD_ENABLED)
			"libxess.so",
#else
			"libxess.dll",
			"libxess.so",
#endif
		};
		String resolved_library_name;
		if (!_open_vendor_library(library_names, std_size(library_names), library_handle, resolved_library_name)) {
			library_handle = nullptr;
			unavailable_reason = "The XeSS runtime library could not be loaded.";
			return false;
		}

		bool resolved = true;
		void *symbol = nullptr;
		resolved = _xess_resolve_symbol(library_handle, "xessGetVersion", symbol) && resolved;
		xess_get_version = reinterpret_cast<XessGetVersionFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessDestroyContext", symbol) && resolved;
		xess_destroy_context = reinterpret_cast<XessDestroyContextFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessIsOptimalDriver", symbol) && resolved;
		xess_is_optimal_driver = reinterpret_cast<XessIsOptimalDriverFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessSetLoggingCallback", symbol) && resolved;
		xess_set_logging_callback = reinterpret_cast<XessSetLoggingCallbackFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessSetVelocityScale", symbol) && resolved;
		xess_set_velocity_scale = reinterpret_cast<XessSetVelocityScaleFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessSetJitterScale", symbol) && resolved;
		xess_set_jitter_scale = reinterpret_cast<XessSetJitterScaleFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessVKCreateContext", symbol) && resolved;
		xess_vk_create_context = reinterpret_cast<XessVKCreateContextFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessVKBuildPipelines", symbol) && resolved;
		xess_vk_build_pipelines = reinterpret_cast<XessVKBuildPipelinesFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessVKInit", symbol) && resolved;
		xess_vk_init = reinterpret_cast<XessVKInitFunc>(symbol);
		resolved = _xess_resolve_symbol(library_handle, "xessVKExecute", symbol) && resolved;
		xess_vk_execute = reinterpret_cast<XessVKExecuteFunc>(symbol);

		if (!resolved) {
			unload();
			attempted_load = true;
			unavailable_reason = vformat("%s is missing one or more required Vulkan SR exports.", resolved_library_name);
			return false;
		}

		xess_version_t version = {};
		const xess_result_t version_result = xess_get_version(&version);
		if (!_xess_result_succeeded(version_result)) {
			unload();
			attempted_load = true;
			unavailable_reason = vformat("%s could not report its version: %s.", resolved_library_name, _xess_result_to_string(version_result));
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

		if (library_handle != nullptr && OS::get_singleton() != nullptr) {
			OS::get_singleton()->close_dynamic_library(library_handle);
		}
		library_handle = nullptr;
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
	return p_mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS ||
			p_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR31 ||
			p_mode == RSE::VIEWPORT_SCALING_3D_MODE_XESS;
}

bool VendorUpscaler::is_super_resolution_available(RSE::ViewportScaling3DMode p_mode) {
	if (!is_super_resolution_mode(p_mode)) {
		return false;
	}

	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS) {
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
		return dlss_runtime().resolve();
#else
		return false;
#endif
	}

	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_XESS) {
#if defined(VENDOR_UPSCALER_XESS_REQUESTED) && defined(XESS_VK_HEADERS_PRESENT)
		return xess_runtime().probe_device();
#else
		return false;
#endif
	}

	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR31) {
#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
		return fsr31_runtime().probe_device();
#else
		return false;
#endif
	}

	return false;
}

const char *VendorUpscaler::get_super_resolution_name(RSE::ViewportScaling3DMode p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_SCALING_3D_MODE_DLSS:
			return "DLSS";
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR31:
			return "FSR 3.1";
		case RSE::VIEWPORT_SCALING_3D_MODE_XESS:
			return "XeSS";
		default:
			return "Vendor";
	}
}

const char *VendorUpscaler::get_super_resolution_unavailable_reason(RSE::ViewportScaling3DMode p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_SCALING_3D_MODE_DLSS:
#if !defined(VENDOR_UPSCALER_DLSS_REQUESTED)
			return "DLSS was not enabled at build time.";
#elif !defined(STREAMLINE_ENABLED)
			return "Streamline support was not compiled in.";
#elif !defined(STREAMLINE_SDK_HEADERS_PRESENT)
			return "Streamline SDK headers were not found at build time.";
#else
			if (!Streamline::get_singleton()) {
				return "Streamline is not initialized.";
			}
			if (!Streamline::get_singleton()->is_initialized()) {
				return Streamline::get_singleton()->get_unavailable_reason();
			}
			if (!Streamline::get_singleton()->is_feature_supported(STREAMLINE_FEATURE_DLSS)) {
				return "DLSS is not supported by the current Streamline runtime or adapter.";
			}
			if (!Streamline::get_singleton()->is_feature_loaded(STREAMLINE_FEATURE_DLSS)) {
				return "The Streamline DLSS plugin is not loaded.";
			}
			return dlss_runtime().get_unavailable_reason_cstr();
#endif
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR31:
#if !defined(VENDOR_UPSCALER_FSR31_REQUESTED)
			return "FSR 3.1 was not enabled at build time.";
#elif !defined(FIDELITYFX_FSR31_API_HEADERS_PRESENT)
			return "FidelityFX SDK 1.1.x API headers were not found at build time.";
#elif !defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
			return "FidelityFX SDK 1.1.x Vulkan API headers were not found at build time.";
#else
			return fsr31_runtime().get_unavailable_reason_cstr();
#endif
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

	if (p_params.mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS) {
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, false);
		ERR_FAIL_COND_V_MSG(!p_params.color.is_valid(), false, "DLSS requires a color input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.depth.is_valid(), false, "DLSS requires a depth input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.velocity.is_valid(), false, "DLSS requires a motion-vector input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.output.is_valid(), false, "DLSS requires an output texture.");
		ERR_FAIL_COND_V_MSG(p_params.internal_size.x <= 0 || p_params.internal_size.y <= 0, false, "DLSS requires a valid internal render size.");
		ERR_FAIL_COND_V_MSG(p_params.target_size.x <= 0 || p_params.target_size.y <= 0, false, "DLSS requires a valid output render size.");

		const VulkanTextureHandles color_handles = _get_vulkan_texture_handles(rd, p_params.color);
		const VulkanTextureHandles depth_handles = _get_vulkan_texture_handles(rd, p_params.depth);
		const VulkanTextureHandles velocity_handles = _get_vulkan_texture_handles(rd, p_params.velocity);
		const VulkanTextureHandles output_handles = _get_vulkan_texture_handles(rd, p_params.output);
		const VulkanTextureHandles reactive_handles = _get_vulkan_texture_handles(rd, p_params.reactive);
		const VulkanTextureHandles exposure_handles = _get_vulkan_texture_handles(rd, p_params.exposure);

		ERR_FAIL_COND_V_MSG(color_handles.image == VK_NULL_HANDLE || color_handles.image_view == VK_NULL_HANDLE, false, "DLSS could not retrieve the Vulkan color image handles.");
		ERR_FAIL_COND_V_MSG(depth_handles.image == VK_NULL_HANDLE || depth_handles.image_view == VK_NULL_HANDLE, false, "DLSS could not retrieve the Vulkan depth image handles.");
		ERR_FAIL_COND_V_MSG(velocity_handles.image == VK_NULL_HANDLE || velocity_handles.image_view == VK_NULL_HANDLE, false, "DLSS could not retrieve the Vulkan motion-vector image handles.");
		ERR_FAIL_COND_V_MSG(output_handles.image == VK_NULL_HANDLE || output_handles.image_view == VK_NULL_HANDLE, false, "DLSS could not retrieve the Vulkan output image handles.");
		ERR_FAIL_COND_V_MSG(p_params.reactive.is_valid() && (reactive_handles.image == VK_NULL_HANDLE || reactive_handles.image_view == VK_NULL_HANDLE), false, "DLSS could not retrieve the Vulkan reactive-mask image handles.");
		ERR_FAIL_COND_V_MSG(p_params.exposure.is_valid() && (exposure_handles.image == VK_NULL_HANDLE || exposure_handles.image_view == VK_NULL_HANDLE), false, "DLSS could not retrieve the Vulkan exposure image handles.");

		uint32_t dlss_viewport_id = 0;
		if (!dlss_runtime().acquire_viewport_id(p_params.output.get_id(), (uint64_t)output_handles.image, dlss_viewport_id)) {
			WARN_PRINT_ONCE(dlss_runtime().get_unavailable_reason());
			return false;
		}

		DlssCallbackData *data = memnew(DlssCallbackData);
		data->viewport_id = dlss_viewport_id;
		data->color = color_handles;
		data->depth = depth_handles;
		data->velocity = velocity_handles;
		data->reactive = reactive_handles;
		data->exposure = exposure_handles;
		data->output = output_handles;
		data->internal_size = p_params.internal_size;
		data->target_size = p_params.target_size;
		data->jitter = p_params.jitter;
		data->z_near = p_params.z_near;
		data->z_far = p_params.z_far;
		data->fovy = p_params.fovy;
		data->aspect = p_params.aspect;
		data->reset_accumulation = p_params.reset_accumulation;
		data->orthogonal_projection = p_params.orthogonal_projection;
		data->use_auto_exposure = !p_params.exposure.is_valid();
		data->has_reactive = p_params.reactive.is_valid();
		data->camera_view_to_clip = p_params.camera_view_to_clip;
		data->reprojection = p_params.reprojection;
		data->camera_transform = p_params.camera_transform;
		data->mode = _dlss_mode_for_resolution(p_params.internal_size, p_params.target_size);

		RD::CallbackResource resources[6];
		uint32_t resource_count = 0;
		resources[resource_count].rid = p_params.color;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.depth;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.velocity;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.output;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
		resource_count++;
		if (p_params.reactive.is_valid() && !_vulkan_textures_share_image(reactive_handles, color_handles)) {
			resources[resource_count].rid = p_params.reactive;
			resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			resource_count++;
		}
		if (p_params.exposure.is_valid()) {
			resources[resource_count].rid = p_params.exposure;
			resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			resource_count++;
		}

		const Error err = rd->driver_callback_add((RDD::DriverCallback)_dlss_driver_callback, data, VectorView<RD::CallbackResource>(resources, resource_count));
		if (err != OK) {
			memdelete(data);
			ERR_FAIL_V_MSG(false, "DLSS failed to record its RenderingDevice driver callback.");
		}

		return true;
#else
		return false;
#endif
	}

	if (p_params.mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR31) {
#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, false);
		ERR_FAIL_COND_V_MSG(!p_params.color.is_valid(), false, "FSR 3.1 requires a color input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.depth.is_valid(), false, "FSR 3.1 requires a depth input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.velocity.is_valid(), false, "FSR 3.1 requires a motion-vector input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.output.is_valid(), false, "FSR 3.1 requires an output texture.");
		ERR_FAIL_COND_V_MSG(p_params.internal_size.x <= 0 || p_params.internal_size.y <= 0, false, "FSR 3.1 requires a valid internal render size.");
		ERR_FAIL_COND_V_MSG(p_params.target_size.x <= 0 || p_params.target_size.y <= 0, false, "FSR 3.1 requires a valid output render size.");

		const VulkanTextureHandles color_handles = _get_vulkan_texture_handles(rd, p_params.color);
		const VulkanTextureHandles depth_handles = _get_vulkan_texture_handles(rd, p_params.depth);
		const VulkanTextureHandles velocity_handles = _get_vulkan_texture_handles(rd, p_params.velocity);
		const VulkanTextureHandles output_handles = _get_vulkan_texture_handles(rd, p_params.output);
		const VulkanTextureHandles reactive_handles = _get_vulkan_texture_handles(rd, p_params.reactive);
		const VulkanTextureHandles exposure_handles = _get_vulkan_texture_handles(rd, p_params.exposure);

		ERR_FAIL_COND_V_MSG(color_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan color image handle.");
		ERR_FAIL_COND_V_MSG(depth_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan depth image handle.");
		ERR_FAIL_COND_V_MSG(velocity_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan motion-vector image handle.");
		ERR_FAIL_COND_V_MSG(output_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan output image handle.");
		ERR_FAIL_COND_V_MSG(color_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3.1 could not retrieve the Vulkan color image format.");
		ERR_FAIL_COND_V_MSG(depth_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3.1 could not retrieve the Vulkan depth image format.");
		ERR_FAIL_COND_V_MSG(velocity_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3.1 could not retrieve the Vulkan motion-vector image format.");
		ERR_FAIL_COND_V_MSG(output_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3.1 could not retrieve the Vulkan output image format.");
		ERR_FAIL_COND_V_MSG(p_params.reactive.is_valid() && reactive_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan reactive-mask image handle.");
		ERR_FAIL_COND_V_MSG(p_params.exposure.is_valid() && exposure_handles.image == VK_NULL_HANDLE, false, "FSR 3.1 could not retrieve the Vulkan exposure image handle.");

		Fsr31ContextKey key;
		key.output_image = (uint64_t)output_handles.image;
		key.input_width = uint32_t(p_params.internal_size.x);
		key.input_height = uint32_t(p_params.internal_size.y);
		key.output_width = uint32_t(p_params.target_size.x);
		key.output_height = uint32_t(p_params.target_size.y);
		key.flags = _fsr31_context_flags(p_params.exposure.is_valid());

		Fsr31UpscaleContext *context = fsr31_runtime().get_or_create_context(key);
		ERR_FAIL_NULL_V_MSG(context, false, fsr31_runtime().get_unavailable_reason());

		Fsr31CallbackData *data = memnew(Fsr31CallbackData);
		data->context = context;
		data->color = color_handles;
		data->depth = depth_handles;
		data->velocity = velocity_handles;
		data->reactive = reactive_handles;
		data->exposure = exposure_handles;
		data->output = output_handles;
		data->internal_size = p_params.internal_size;
		data->target_size = p_params.target_size;
		data->jitter = p_params.jitter;
		data->z_near = p_params.z_near;
		data->z_far = p_params.z_far;
		data->fovy = p_params.fovy;
		data->sharpness = p_params.sharpness;
		data->delta_time = p_params.delta_time;
		data->reset_accumulation = p_params.reset_accumulation;
		data->has_reactive = p_params.reactive.is_valid();
		data->has_exposure = p_params.exposure.is_valid();

		RD::CallbackResource resources[6];
		uint32_t resource_count = 0;
		resources[resource_count].rid = p_params.color;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.depth;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.velocity;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resource_count++;
		resources[resource_count].rid = p_params.output;
		resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
		resource_count++;
		if (p_params.reactive.is_valid() && !_vulkan_textures_share_image(reactive_handles, color_handles)) {
			resources[resource_count].rid = p_params.reactive;
			resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			resource_count++;
		}
		if (p_params.exposure.is_valid()) {
			resources[resource_count].rid = p_params.exposure;
			resources[resource_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			resource_count++;
		}

		const Error err = rd->driver_callback_add((RDD::DriverCallback)_fsr31_driver_callback, data, VectorView<RD::CallbackResource>(resources, resource_count));
		if (err != OK) {
			memdelete(data);
			ERR_FAIL_V_MSG(false, "FSR 3.1 failed to record its RenderingDevice driver callback.");
		}

		return true;
#else
		return false;
#endif
	}

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

bool VendorUpscaler::is_frame_generation_mode(RSE::ViewportFrameGenerationMode p_mode) {
	return RSE::frame_generation_mode_is_vendor(p_mode);
}

bool VendorUpscaler::is_frame_generation_available(RSE::ViewportFrameGenerationMode p_mode, bool p_presented_to_swapchain) {
	if (!is_frame_generation_mode(p_mode) || !p_presented_to_swapchain) {
		return false;
	}

	switch (p_mode) {
		case RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO:
			return is_frame_generation_available(RSE::VIEWPORT_FRAME_GENERATION_DLSS, p_presented_to_swapchain) ||
					is_frame_generation_available(RSE::VIEWPORT_FRAME_GENERATION_FSR3, p_presented_to_swapchain) ||
					is_frame_generation_available(RSE::VIEWPORT_FRAME_GENERATION_XESS, p_presented_to_swapchain);
		case RSE::VIEWPORT_FRAME_GENERATION_FSR3:
#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
			return fsr31_runtime().probe_frame_generation();
#else
			return false;
#endif
		case RSE::VIEWPORT_FRAME_GENERATION_DLSS:
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
			return dlss_frame_generation_runtime().resolve();
#else
			return false;
#endif
		case RSE::VIEWPORT_FRAME_GENERATION_XESS:
		default:
			return false;
	}
}

const char *VendorUpscaler::get_frame_generation_name(RSE::ViewportFrameGenerationMode p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO:
			return "vendor frame generation";
		case RSE::VIEWPORT_FRAME_GENERATION_DLSS:
			return "DLSS Frame Generation";
		case RSE::VIEWPORT_FRAME_GENERATION_FSR3:
			return "FSR 3 Frame Generation";
		case RSE::VIEWPORT_FRAME_GENERATION_XESS:
			return "XeSS Frame Generation";
		default:
			return "vendor frame generation";
	}
}

const char *VendorUpscaler::get_frame_generation_unavailable_reason(RSE::ViewportFrameGenerationMode p_mode, bool p_presented_to_swapchain) {
	if (!is_frame_generation_mode(p_mode)) {
		return "This is not a vendor frame generation mode.";
	}
	if (!p_presented_to_swapchain) {
		return "Vendor frame generation requires a viewport that contributes to the presented swapchain.";
	}

	switch (p_mode) {
		case RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO:
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
			if (dlss_frame_generation_runtime().resolve()) {
				return "DLSS Frame Generation is available.";
			}
#endif
#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
			if (fsr31_runtime().probe_frame_generation()) {
				return "FSR 3 frame generation is available.";
			}
			return fsr31_runtime().get_frame_generation_unavailable_reason_cstr();
#else
			return "No vendor frame generation backend is initialized for the current runtime.";
#endif
		case RSE::VIEWPORT_FRAME_GENERATION_DLSS:
#if !defined(VENDOR_UPSCALER_DLSS_REQUESTED)
			return "DLSS Frame Generation was not enabled at build time.";
#elif !defined(STREAMLINE_ENABLED)
			return "Streamline support was not compiled in.";
#elif !defined(STREAMLINE_SDK_HEADERS_PRESENT)
			return "Streamline SDK headers were not found at build time.";
#else
			if (!Streamline::get_singleton()) {
				return "Streamline is not initialized.";
			}
			if (!Streamline::get_singleton()->is_initialized()) {
				return Streamline::get_singleton()->get_unavailable_reason();
			}
			if (StreamlineContext::get().dlss_g_load_disabled) {
				return "DLSS Frame Generation is disabled by default because it installs present/swapchain hooks. Set GODOT_ENABLE_STREAMLINE_DLSS_G=1 before startup to enable it for game runs.";
			}
			if (!Streamline::get_singleton()->is_feature_supported(STREAMLINE_FEATURE_DLSS_G)) {
				return "DLSS Frame Generation is not supported by the current Streamline runtime or adapter.";
			}
			if (!Streamline::get_singleton()->is_feature_loaded(STREAMLINE_FEATURE_DLSS_G)) {
				return "The Streamline DLSS-G plugin is not loaded.";
			}
			if (dlss_frame_generation_runtime().resolve()) {
				return "DLSS Frame Generation is available.";
			}
			return dlss_frame_generation_runtime().get_unavailable_reason_cstr();
#endif
		case RSE::VIEWPORT_FRAME_GENERATION_FSR3:
#if !defined(VENDOR_UPSCALER_FSR31_REQUESTED)
			return "FSR 3 frame generation was not enabled at build time.";
#elif !defined(FIDELITYFX_FSR31_API_HEADERS_PRESENT)
			return "FidelityFX SDK 1.1.x API headers were not found at build time.";
#elif !defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
			return "FidelityFX SDK 1.1.x Vulkan API headers were not found at build time.";
#else
			if (fsr31_runtime().probe_frame_generation()) {
				return "FSR 3 frame generation is available.";
			}
			return fsr31_runtime().get_frame_generation_unavailable_reason_cstr();
#endif
		case RSE::VIEWPORT_FRAME_GENERATION_XESS:
#if !defined(VENDOR_UPSCALER_XESS_REQUESTED)
			return "XeSS Frame Generation was not enabled at build time.";
#elif !defined(XESS_FG_D3D12_HEADERS_PRESENT)
			return "XeSS-FG headers were not found at build time.";
#else
			return "XeSS-FG currently exposes a D3D12 swapchain API, which is incompatible with this Vulkan-only renderer path.";
#endif
		default:
			return "Vendor frame generation is not available.";
	}
}

bool VendorUpscaler::should_use_interpolated_frame_generation_fallback(RSE::ViewportFrameGenerationMode p_mode) {
	return is_frame_generation_mode(p_mode) && !is_frame_generation_available(p_mode, true);
}

bool VendorUpscaler::prepare_frame_generation(const FrameGenerationParameters &p_params) {
	ERR_FAIL_COND_V_MSG(!is_frame_generation_mode(p_params.mode), false, "The requested frame generation mode is not a vendor backend.");

	if (p_params.mode == RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO) {
		FrameGenerationParameters dlss_params = p_params;
		dlss_params.mode = RSE::VIEWPORT_FRAME_GENERATION_DLSS;
		if (is_frame_generation_available(RSE::VIEWPORT_FRAME_GENERATION_DLSS, true) && prepare_frame_generation(dlss_params)) {
			return true;
		}
		FrameGenerationParameters fsr3_params = p_params;
		fsr3_params.mode = RSE::VIEWPORT_FRAME_GENERATION_FSR3;
		if (is_frame_generation_available(RSE::VIEWPORT_FRAME_GENERATION_FSR3, true) && prepare_frame_generation(fsr3_params)) {
			return true;
		}
		return false;
	}

	if (p_params.mode == RSE::VIEWPORT_FRAME_GENERATION_DLSS) {
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, false);
		ERR_FAIL_COND_V_MSG(p_params.screen < 0, false, "DLSS Frame Generation requires a presented screen.");
		ERR_FAIL_COND_V_MSG(!p_params.depth.is_valid(), false, "DLSS Frame Generation requires a depth input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.velocity.is_valid(), false, "DLSS Frame Generation requires a motion-vector input texture.");
		ERR_FAIL_COND_V_MSG(p_params.render_size.x <= 0 || p_params.render_size.y <= 0, false, "DLSS Frame Generation requires a valid render size.");
		ERR_FAIL_COND_V_MSG(p_params.display_size.x <= 0 || p_params.display_size.y <= 0, false, "DLSS Frame Generation requires a valid display size.");
		ERR_FAIL_COND_V_MSG(!dlss_frame_generation_runtime().resolve(), false, dlss_frame_generation_runtime().get_unavailable_reason());

		const DisplayServerEnums::WindowID window_id = DisplayServerEnums::WindowID(p_params.screen);
		const uint64_t swapchain_format = rd->screen_get_driver_resource(RD::DRIVER_RESOURCE_SWAP_CHAIN_DATA_FORMAT, window_id);
		ERR_FAIL_COND_V_MSG(swapchain_format == 0, false, "DLSS Frame Generation could not retrieve the Vulkan swapchain format.");

		const VulkanTextureHandles depth_handles = _get_vulkan_texture_handles(rd, p_params.depth);
		const VulkanTextureHandles velocity_handles = _get_vulkan_texture_handles(rd, p_params.velocity);
		const VulkanTextureHandles hudless_color_handles = _get_vulkan_texture_handles(rd, p_params.hudless_color);
		ERR_FAIL_COND_V_MSG(depth_handles.image == VK_NULL_HANDLE, false, "DLSS Frame Generation could not retrieve the Vulkan depth image handle.");
		ERR_FAIL_COND_V_MSG(velocity_handles.image == VK_NULL_HANDLE, false, "DLSS Frame Generation could not retrieve the Vulkan motion-vector image handle.");
		ERR_FAIL_COND_V_MSG(depth_handles.format == VK_FORMAT_UNDEFINED, false, "DLSS Frame Generation could not retrieve the Vulkan depth image format.");
		ERR_FAIL_COND_V_MSG(velocity_handles.format == VK_FORMAT_UNDEFINED, false, "DLSS Frame Generation could not retrieve the Vulkan motion-vector image format.");

		Rect2i generation_rect = p_params.generation_rect;
		if (generation_rect.size.x <= 0 || generation_rect.size.y <= 0) {
			generation_rect = Rect2i(Point2i(), p_params.display_size);
		}

		static uint32_t dlss_g_frame_index = 0;
		dlss_g_frame_index++;
		if (dlss_g_frame_index == 0) {
			dlss_g_frame_index++;
		}
		Streamline::get_singleton()->set_present_frame_index(dlss_g_frame_index);

		DlssFrameGenerationCallbackData *data = memnew(DlssFrameGenerationCallbackData);
		data->viewport_id = _dlss_viewport_id_for_frame_generation(p_params.viewport_id, p_params.screen);
		data->frame_index = dlss_g_frame_index;
		data->backbuffer_format = VkFormat(swapchain_format);
		data->depth = depth_handles;
		data->velocity = velocity_handles;
		data->hudless_color = hudless_color_handles;
		data->render_size = p_params.render_size;
		data->display_size = p_params.display_size;
		data->generation_rect = generation_rect;
		data->jitter = p_params.jitter;
		data->z_near = p_params.z_near;
		data->z_far = p_params.z_far;
		data->fovy = p_params.fovy;
		data->aspect = p_params.aspect;
		data->reset_accumulation = p_params.reset_accumulation;
		data->orthogonal_projection = p_params.orthogonal_projection;
		data->has_hudless_color = hudless_color_handles.image != VK_NULL_HANDLE;
		data->camera_view_to_clip = p_params.camera_view_to_clip;
		data->reprojection = p_params.reprojection;
		data->camera_transform = p_params.camera_transform;

		RD::CallbackResource resources[3];
		resources[0].rid = p_params.depth;
		resources[0].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[1].rid = p_params.velocity;
		resources[1].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[2].rid = p_params.hudless_color;
		resources[2].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		const uint32_t resource_count = data->has_hudless_color ? 3 : 2;

		const Error err = rd->driver_callback_add((RDD::DriverCallback)_dlss_frame_generation_driver_callback, data, VectorView<RD::CallbackResource>(resources, resource_count));
		if (err != OK) {
			memdelete(data);
			ERR_FAIL_V_MSG(false, "DLSS Frame Generation failed to record its RenderingDevice driver callback.");
		}

		return true;
#else
		return false;
#endif
	}

	if (p_params.mode == RSE::VIEWPORT_FRAME_GENERATION_FSR3) {
#if defined(VENDOR_UPSCALER_FSR31_REQUESTED) && defined(FIDELITYFX_FSR31_API_VK_HEADERS_PRESENT)
		RD *rd = RD::get_singleton();
		ERR_FAIL_NULL_V(rd, false);
		ERR_FAIL_COND_V_MSG(p_params.screen < 0, false, "FSR 3 frame generation requires a presented screen.");
		ERR_FAIL_COND_V_MSG(!p_params.depth.is_valid(), false, "FSR 3 frame generation requires a depth input texture.");
		ERR_FAIL_COND_V_MSG(!p_params.velocity.is_valid(), false, "FSR 3 frame generation requires a motion-vector input texture.");
		ERR_FAIL_COND_V_MSG(p_params.render_size.x <= 0 || p_params.render_size.y <= 0, false, "FSR 3 frame generation requires a valid render size.");
		ERR_FAIL_COND_V_MSG(p_params.display_size.x <= 0 || p_params.display_size.y <= 0, false, "FSR 3 frame generation requires a valid display size.");

		const DisplayServerEnums::WindowID window_id = DisplayServerEnums::WindowID(p_params.screen);
		const uint64_t swapchain_handle = rd->screen_get_driver_resource(RD::DRIVER_RESOURCE_SWAP_CHAIN, window_id);
		const uint64_t swapchain_format = rd->screen_get_driver_resource(RD::DRIVER_RESOURCE_SWAP_CHAIN_DATA_FORMAT, window_id);
		ERR_FAIL_COND_V_MSG(swapchain_handle == 0, false, "FSR 3 frame generation could not retrieve the Vulkan swapchain handle.");
		ERR_FAIL_COND_V_MSG(swapchain_format == 0, false, "FSR 3 frame generation could not retrieve the Vulkan swapchain format.");

		const VulkanTextureHandles depth_handles = _get_vulkan_texture_handles(rd, p_params.depth);
		const VulkanTextureHandles velocity_handles = _get_vulkan_texture_handles(rd, p_params.velocity);
		const VulkanTextureHandles hudless_color_handles = _get_vulkan_texture_handles(rd, p_params.hudless_color);
		ERR_FAIL_COND_V_MSG(depth_handles.image == VK_NULL_HANDLE, false, "FSR 3 frame generation could not retrieve the Vulkan depth image handle.");
		ERR_FAIL_COND_V_MSG(velocity_handles.image == VK_NULL_HANDLE, false, "FSR 3 frame generation could not retrieve the Vulkan motion-vector image handle.");
		ERR_FAIL_COND_V_MSG(depth_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3 frame generation could not retrieve the Vulkan depth image format.");
		ERR_FAIL_COND_V_MSG(velocity_handles.format == VK_FORMAT_UNDEFINED, false, "FSR 3 frame generation could not retrieve the Vulkan motion-vector image format.");

		Fsr31FrameGenerationContextKey key;
		key.swapchain = swapchain_handle;
		key.render_width = uint32_t(p_params.render_size.x);
		key.render_height = uint32_t(p_params.render_size.y);
		key.display_width = uint32_t(p_params.display_size.x);
		key.display_height = uint32_t(p_params.display_size.y);
		key.backbuffer_format = ffxApiGetSurfaceFormatVK(VkFormat(swapchain_format));
		key.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;

		Fsr31FrameGenerationContext *context = fsr31_runtime().get_or_create_frame_generation_context(key);
		ERR_FAIL_NULL_V_MSG(context, false, fsr31_runtime().get_frame_generation_unavailable_reason());

		Rect2i generation_rect = p_params.generation_rect;
		if (generation_rect.size.x <= 0 || generation_rect.size.y <= 0) {
			generation_rect = Rect2i(Point2i(), p_params.display_size);
		}

		Fsr31FrameGenerationCallbackData *data = memnew(Fsr31FrameGenerationCallbackData);
		data->context = context;
		data->swapchain = swapchain_handle;
		data->depth = depth_handles;
		data->velocity = velocity_handles;
		data->hudless_color = hudless_color_handles;
		data->render_size = p_params.render_size;
		data->display_size = p_params.display_size;
		data->generation_rect = generation_rect;
		data->jitter = p_params.jitter;
		data->z_near = p_params.z_near;
		data->z_far = p_params.z_far;
		data->fovy = p_params.fovy;
		data->delta_time = p_params.delta_time;
		data->reset_accumulation = p_params.reset_accumulation;
		data->has_hudless_color = hudless_color_handles.image != VK_NULL_HANDLE;
		data->camera_transform = p_params.camera_transform;
		data->frame_id = context->frame_id++;

		RD::CallbackResource resources[3];
		resources[0].rid = p_params.depth;
		resources[0].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[1].rid = p_params.velocity;
		resources[1].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources[2].rid = p_params.hudless_color;
		resources[2].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		const uint32_t resource_count = data->has_hudless_color ? 3 : 2;

		const Error err = rd->driver_callback_add((RDD::DriverCallback)_fsr31_frame_generation_driver_callback, data, VectorView<RD::CallbackResource>(resources, resource_count));
		if (err != OK) {
			memdelete(data);
			ERR_FAIL_V_MSG(false, "FSR 3 frame generation failed to record its RenderingDevice driver callback.");
		}

		return true;
#else
		return false;
#endif
	}

	return false;
}

void VendorUpscaler::disable_frame_generation(RSE::ViewportFrameGenerationMode p_mode, uint64_t p_viewport_id, int p_screen) {
	if (!is_frame_generation_mode(p_mode)) {
		return;
	}

	switch (p_mode) {
		case RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO:
		case RSE::VIEWPORT_FRAME_GENERATION_DLSS:
#if defined(VENDOR_UPSCALER_DLSS_REQUESTED) && defined(STREAMLINE_ENABLED) && defined(STREAMLINE_SDK_HEADERS_PRESENT)
			if (p_screen >= 0) {
				dlss_frame_generation_runtime().set_options_off(_dlss_viewport_id_for_frame_generation(p_viewport_id, p_screen));
			}
#endif
			break;
		default:
			break;
	}
}

} // namespace RendererRD
