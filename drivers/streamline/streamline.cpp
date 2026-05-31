/**************************************************************************/
/*  streamline.cpp                                                        */
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

#include "streamline.h"

#include "drivers/streamline/streamline_context.h"

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/version.h"

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
#include "drivers/vulkan/godot_vulkan.h"

#include "sl_core_api.h"
#include "sl_helpers_vk.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
#endif

#include <string.h>

Streamline *Streamline::singleton = nullptr;

static Streamline streamline_singleton;
static StreamlineContext streamline_context;

StreamlineContext &StreamlineContext::get() {
	return streamline_context;
}

static bool _streamline_should_initialize_for_current_process(const char **r_disabled_reason = nullptr) {
	OS *os = OS::get_singleton();
	if (os != nullptr && os->get_environment("GODOT_DISABLE_STREAMLINE") == "1") {
		if (r_disabled_reason != nullptr) {
			*r_disabled_reason = "Streamline is disabled by GODOT_DISABLE_STREAMLINE.";
		}
		return false;
	}

	Engine *engine = Engine::get_singleton();
	const bool editor_process = engine != nullptr && (engine->is_editor_hint() || engine->is_project_manager_hint());
	const bool force_streamline_in_editor = os != nullptr && os->get_environment("GODOT_FORCE_STREAMLINE_IN_EDITOR") == "1";
	if (editor_process && !force_streamline_in_editor) {
		if (r_disabled_reason != nullptr) {
			*r_disabled_reason = "Streamline is disabled in editor and project manager processes.";
		}
		return false;
	}

	return true;
}

static bool _resolve_streamline_symbol(void *p_library_handle, const String &p_symbol, void *&r_symbol) {
	return OS::get_singleton()->get_dynamic_library_symbol_handle(p_library_handle, p_symbol, r_symbol, true) == OK && r_symbol != nullptr;
}

static bool _open_streamline_interposer(void *&r_library_handle, String &r_resolved_library_name) {
	OS *os = OS::get_singleton();
	ERR_FAIL_NULL_V(os, false);

	const char *const library_names[] = {
#if defined(WINDOWS_ENABLED)
		"sl.interposer.dll",
#elif defined(LINUXBSD_ENABLED)
		"libsl.interposer.so",
		"sl.interposer.so",
#else
		"sl.interposer.dll",
		"libsl.interposer.so",
#endif
	};

	for (uint32_t i = 0; i < sizeof(library_names) / sizeof(library_names[0]); i++) {
		const char *library_name = library_names[i];
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

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
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
		case sl::Result::eErrorMissingProxy:
			return "missing Streamline proxy/interposer";
		case sl::Result::eErrorInvalidIntegration:
			return "invalid Streamline integration";
		case sl::Result::eErrorNotInitialized:
			return "Streamline is not initialized";
		case sl::Result::eErrorInitNotCalled:
			return "slInit was not called";
		case sl::Result::eErrorInvalidParameter:
			return "invalid parameter";
		case sl::Result::eErrorMissingOrInvalidAPI:
			return "missing or invalid rendering API";
		case sl::Result::eErrorFeatureMissing:
			return "feature plugin is missing";
		case sl::Result::eErrorFeatureNotSupported:
			return "feature is not supported";
		case sl::Result::eErrorFeatureFailedToLoad:
			return "feature failed to load";
		case sl::Result::eWarnOutOfVRAM:
			return "out of VRAM";
		default:
			return "unknown Streamline error";
	}
}

static void _streamline_log_message_callback(sl::LogType p_type, const char *p_message) {
	if (p_message == nullptr) {
		return;
	}

	const bool demote_known_state_tracking_warning =
			strstr(p_message, "Hook sl.common:Vulkan:CmdBindPipeline is NOT supported") != nullptr ||
			strstr(p_message, "Hook sl.common:Vulkan:CmdBindDescriptorSets is NOT supported") != nullptr ||
			strstr(p_message, "Hook sl.common:Vulkan:BeginCommandBuffer is NOT supported") != nullptr ||
			(strstr(p_message, "Ignoring plugin 'sl.") != nullptr && strstr(p_message, "not requested by the host") != nullptr);

	if (demote_known_state_tracking_warning) {
		print_verbose(vformat("Streamline: %s", p_message));
		return;
	}

	switch (p_type) {
		case sl::LogType::eWarn:
		case sl::LogType::eError:
			WARN_PRINT(vformat("Streamline: %s", p_message));
			break;
		case sl::LogType::eInfo:
		default:
			print_verbose(vformat("Streamline: %s", p_message));
			break;
	}
}

static bool _streamline_initialize_preferences(StreamlineContext &r_context) {
	if (r_context.initialized) {
		return true;
	}

	if (!r_context.load()) {
		return false;
	}

	PFun_slInit *sl_init = reinterpret_cast<PFun_slInit *>(r_context.slInit);
	ERR_FAIL_NULL_V(sl_init, false);

	const bool enable_dlss_g = OS::get_singleton()->get_environment("GODOT_ENABLE_STREAMLINE_DLSS_G") == "1";
	const bool force_dlss_g_in_editor = OS::get_singleton()->get_environment("GODOT_FORCE_STREAMLINE_DLSS_G_IN_EDITOR") == "1";
	const bool disable_dlss_g = OS::get_singleton()->get_environment("GODOT_DISABLE_STREAMLINE_DLSS_G") == "1";
	const bool editor_process = Engine::get_singleton() != nullptr && (Engine::get_singleton()->is_editor_hint() || Engine::get_singleton()->is_project_manager_hint());

	r_context.dlss_g_load_disabled = disable_dlss_g || !enable_dlss_g || (editor_process && !force_dlss_g_in_editor);

	sl::Feature features_to_load[3];
	uint32_t feature_count = 0;
	features_to_load[feature_count++] = sl::kFeatureDLSS;
	if (!r_context.dlss_g_load_disabled) {
		features_to_load[feature_count++] = sl::kFeatureDLSS_G;
		features_to_load[feature_count++] = sl::kFeatureReflex;
	}

	sl::Preferences preferences;
	preferences.logLevel = sl::LogLevel::eDefault;
	preferences.logMessageCallback = _streamline_log_message_callback;
	preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	preferences.featuresToLoad = features_to_load;
	preferences.numFeaturesToLoad = feature_count;
	preferences.engine = sl::EngineType::eCustom;
	preferences.engineVersion = GODOT_VERSION_FULL_CONFIG;
	preferences.projectId = "34f8eaa0-e8d4-4b51-a00d-d4b4f640d47d";
	preferences.renderAPI = sl::RenderAPI::eVulkan;

	const sl::Result init_result = sl_init(preferences, sl::kSDKVersion);
	if (init_result != sl::Result::eOk) {
		r_context.unavailable_reason = _streamline_result_to_string(init_result);
		return false;
	}

	r_context.initialized = true;
	r_context.unavailable_reason = "Streamline is initialized, but Vulkan device information has not been provided.";
	return true;
}
#endif

bool StreamlineContext::load() {
	if (attempted_load) {
		return loaded;
	}

	attempted_load = true;

	OS *os = OS::get_singleton();
	ERR_FAIL_NULL_V(os, false);

	const char *disabled_reason = nullptr;
	if (!_streamline_should_initialize_for_current_process(&disabled_reason)) {
		initialization_disabled = true;
		unavailable_reason = disabled_reason;
		return false;
	}

	String resolved_library_name;
	if (!_open_streamline_interposer(library_handle, resolved_library_name)) {
		library_handle = nullptr;
		unavailable_reason = "The Streamline interposer runtime could not be loaded.";
		return false;
	}

	bool resolved = true;
	resolved = _resolve_streamline_symbol(library_handle, "slInit", slInit) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slShutdown", slShutdown) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slIsFeatureSupported", slIsFeatureSupported) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slIsFeatureLoaded", slIsFeatureLoaded) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slSetFeatureLoaded", slSetFeatureLoaded) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slEvaluateFeature", slEvaluateFeature) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slAllocateResources", slAllocateResources) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slFreeResources", slFreeResources) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slSetTag", slSetTag) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slSetTagForFrame", slSetTagForFrame) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slSetConstants", slSetConstants) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slGetFeatureRequirements", slGetFeatureRequirements) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slGetFeatureVersion", slGetFeatureVersion) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slGetFeatureFunction", slGetFeatureFunction) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slGetNewFrameToken", slGetNewFrameToken) && resolved;
	resolved = _resolve_streamline_symbol(library_handle, "slSetVulkanInfo", slSetVulkanInfo) && resolved;

	if (!resolved) {
		unload();
		unavailable_reason = "The Streamline interposer runtime is missing one or more required exports.";
		return false;
	}

	loaded = true;
	unavailable_reason = "Streamline was loaded but has not been initialized.";
	return true;
}

void StreamlineContext::unload() {
#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	if (initialized && slShutdown != nullptr) {
		reinterpret_cast<PFun_slShutdown *>(slShutdown)();
	}
#endif
	loaded = false;
	initialized = false;
	initialization_disabled = false;
	vulkan_info_set = false;
	dlss_supported = false;
	dlss_g_supported = false;
	reflex_supported = false;
	pcl_supported = false;
	dlss_loaded = false;
	dlss_g_loaded = false;
	reflex_loaded = false;
	pcl_loaded = false;
	dlss_g_load_disabled = false;
	reflex_low_latency_enabled = false;
	has_present_frame_index = false;
	present_frame_index = 0;
	slInit = nullptr;
	slShutdown = nullptr;
	slIsFeatureSupported = nullptr;
	slIsFeatureLoaded = nullptr;
	slSetFeatureLoaded = nullptr;
	slEvaluateFeature = nullptr;
	slAllocateResources = nullptr;
	slFreeResources = nullptr;
	slSetTag = nullptr;
	slSetTagForFrame = nullptr;
	slSetConstants = nullptr;
	slGetFeatureRequirements = nullptr;
	slGetFeatureVersion = nullptr;
	slGetFeatureFunction = nullptr;
	slGetNewFrameToken = nullptr;
	slSetVulkanInfo = nullptr;
	slPCLSetMarker = nullptr;
	slReflexSetOptions = nullptr;
	slReflexSleep = nullptr;

	if (library_handle != nullptr && OS::get_singleton() != nullptr) {
		OS::get_singleton()->close_dynamic_library(library_handle);
	}
	library_handle = nullptr;
	unavailable_reason = "Streamline is not loaded.";
}

Streamline::Streamline() {
	singleton = this;
}

Streamline::~Streamline() {
	StreamlineContext::get().unload();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void Streamline::set_internal_parameter(const char *p_name, void *p_value) {
	StreamlineContext::get().load();
	if (p_name != nullptr && strcmp(p_name, "vulkan_physical_device") == 0) {
		vulkan_physical_device = p_value;
	}
}

void *Streamline::get_internal_parameter(const char *p_name) const {
	if (p_name != nullptr && strcmp(p_name, "vulkan_physical_device") == 0) {
		return vulkan_physical_device;
	}
	return nullptr;
}

bool Streamline::is_enabled_for_current_process() const {
	return _streamline_should_initialize_for_current_process();
}

void Streamline::emit_marker(StreamlineMarkerType p_marker) {
	(void)p_marker;
}

void Streamline::begin_acquire_next_image(const StreamlineAcquireContext &p_context) {
	StreamlineContext::get().load();
	(void)p_context;
}

void Streamline::end_acquire_next_image(int32_t p_result, uint32_t p_image_index) {
	(void)p_result;
	(void)p_image_index;
}

void Streamline::begin_present(const StreamlinePresentContext &p_context) {
	StreamlineContext::get().load();
	(void)p_context;
#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || !context.vulkan_info_set || !context.has_present_frame_index || context.slPCLSetMarker == nullptr) {
		return;
	}

	sl::FrameToken *frame_token = static_cast<sl::FrameToken *>(get_frame_token(context.present_frame_index));
	if (frame_token == nullptr) {
		return;
	}

	PFun_slPCLSetMarker *sl_pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker *>(context.slPCLSetMarker);
	sl_pcl_set_marker(sl::PCLMarker::ePresentStart, *frame_token);
#endif
}

void Streamline::end_present() {
#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || !context.vulkan_info_set || !context.has_present_frame_index || context.slPCLSetMarker == nullptr) {
		return;
	}

	sl::FrameToken *frame_token = static_cast<sl::FrameToken *>(get_frame_token(context.present_frame_index));
	if (frame_token != nullptr) {
		PFun_slPCLSetMarker *sl_pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker *>(context.slPCLSetMarker);
		sl_pcl_set_marker(sl::PCLMarker::ePresentEnd, *frame_token);
	}
	context.has_present_frame_index = false;
#endif
}

void Streamline::notify_swapchain_resized(const StreamlineSwapchainContext &p_context) {
	StreamlineContext::get().load();
	(void)p_context;
}

bool Streamline::initialize_pre_device() {
	StreamlineContext &context = StreamlineContext::get();
	if (context.initialized) {
		return true;
	}
	if (!context.load()) {
		return false;
	}

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	return _streamline_initialize_preferences(context);
#else
	context.unavailable_reason = "Streamline SDK headers were not found at build time.";
	return false;
#endif
}

bool Streamline::initialize_vulkan_device(const StreamlineVulkanDeviceContext &p_context) {
	StreamlineContext &context = StreamlineContext::get();
	if (context.initialized && context.vulkan_info_set) {
		return true;
	}
	if (!context.load()) {
		return false;
	}

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	ERR_FAIL_NULL_V(p_context.instance, false);
	ERR_FAIL_NULL_V(p_context.physical_device, false);
	ERR_FAIL_NULL_V(p_context.device, false);

	PFun_slSetVulkanInfo *sl_set_vulkan_info = reinterpret_cast<PFun_slSetVulkanInfo *>(context.slSetVulkanInfo);
	PFun_slIsFeatureSupported *sl_is_feature_supported = reinterpret_cast<PFun_slIsFeatureSupported *>(context.slIsFeatureSupported);
	PFun_slIsFeatureLoaded *sl_is_feature_loaded = reinterpret_cast<PFun_slIsFeatureLoaded *>(context.slIsFeatureLoaded);
	PFun_slGetFeatureFunction *sl_get_feature_function = reinterpret_cast<PFun_slGetFeatureFunction *>(context.slGetFeatureFunction);

	if (!_streamline_initialize_preferences(context)) {
		return false;
	}

	sl::VulkanInfo vulkan_info;
	vulkan_info.instance = (VkInstance)p_context.instance;
	vulkan_info.physicalDevice = (VkPhysicalDevice)p_context.physical_device;
	vulkan_info.device = (VkDevice)p_context.device;
	vulkan_info.graphicsQueueFamily = p_context.graphics_queue_family;
	vulkan_info.graphicsQueueIndex = p_context.graphics_queue_index;
	vulkan_info.computeQueueFamily = p_context.compute_queue_family;
	vulkan_info.computeQueueIndex = p_context.compute_queue_index;
	vulkan_info.opticalFlowQueueFamily = p_context.optical_flow_queue_family;
	vulkan_info.opticalFlowQueueIndex = p_context.optical_flow_queue_index;

	if (!p_context.device_created_by_interposer) {
		const sl::Result vulkan_info_result = sl_set_vulkan_info(vulkan_info);
		if (vulkan_info_result != sl::Result::eOk) {
			context.unavailable_reason = _streamline_result_to_string(vulkan_info_result);
			return false;
		}
	}

	context.vulkan_info_set = true;
	context.dlss_supported = false;
	context.dlss_g_supported = false;
	context.reflex_supported = false;
	context.pcl_supported = false;
	context.dlss_loaded = false;
	context.dlss_g_loaded = false;
	context.reflex_loaded = false;
	context.pcl_loaded = false;
	context.slPCLSetMarker = nullptr;
	context.slReflexSetOptions = nullptr;
	context.slReflexSleep = nullptr;

	sl::AdapterInfo adapter_info;
	adapter_info.vkPhysicalDevice = p_context.physical_device;
	context.dlss_supported = sl_is_feature_supported(sl::kFeatureDLSS, adapter_info) == sl::Result::eOk;
	if (!context.dlss_g_load_disabled) {
		context.dlss_g_supported = sl_is_feature_supported(sl::kFeatureDLSS_G, adapter_info) == sl::Result::eOk;
		context.reflex_supported = sl_is_feature_supported(sl::kFeatureReflex, adapter_info) == sl::Result::eOk;
		context.pcl_supported = sl_is_feature_supported(sl::kFeaturePCL, adapter_info) == sl::Result::eOk;
	}

	bool feature_loaded = false;
	if (sl_is_feature_loaded(sl::kFeatureDLSS, feature_loaded) == sl::Result::eOk) {
		context.dlss_loaded = feature_loaded;
	}
	if (!context.dlss_g_load_disabled) {
		feature_loaded = false;
		if (sl_is_feature_loaded(sl::kFeatureDLSS_G, feature_loaded) == sl::Result::eOk) {
			context.dlss_g_loaded = feature_loaded;
		}
	}
	if (!context.dlss_g_load_disabled) {
		feature_loaded = false;
		if (sl_is_feature_loaded(sl::kFeatureReflex, feature_loaded) == sl::Result::eOk) {
			context.reflex_loaded = feature_loaded;
		}
		feature_loaded = false;
		if (sl_is_feature_loaded(sl::kFeaturePCL, feature_loaded) == sl::Result::eOk) {
			context.pcl_loaded = feature_loaded;
		}
	}

	if (!context.dlss_g_load_disabled && sl_get_feature_function != nullptr) {
		void *function = nullptr;
		if (sl_get_feature_function(sl::kFeaturePCL, "slPCLSetMarker", function) == sl::Result::eOk) {
			context.slPCLSetMarker = function;
		}
		function = nullptr;
		if (sl_get_feature_function(sl::kFeatureReflex, "slReflexSetOptions", function) == sl::Result::eOk) {
			context.slReflexSetOptions = function;
		}
		function = nullptr;
		if (sl_get_feature_function(sl::kFeatureReflex, "slReflexSleep", function) == sl::Result::eOk) {
			context.slReflexSleep = function;
		}
	}

	context.unavailable_reason = "Streamline is initialized.";
	return true;
#else
	context.unavailable_reason = "Streamline SDK headers were not found at build time.";
	return false;
#endif
}

void Streamline::shutdown() {
	StreamlineContext &context = StreamlineContext::get();
#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	if (context.initialized && context.slShutdown != nullptr) {
		reinterpret_cast<PFun_slShutdown *>(context.slShutdown)();
	}
#endif
	context.initialized = false;
	context.initialization_disabled = false;
	context.vulkan_info_set = false;
	context.dlss_supported = false;
	context.dlss_g_supported = false;
	context.reflex_supported = false;
	context.pcl_supported = false;
	context.dlss_loaded = false;
	context.dlss_g_loaded = false;
	context.reflex_loaded = false;
	context.pcl_loaded = false;
	context.dlss_g_load_disabled = false;
	context.slPCLSetMarker = nullptr;
	context.slReflexSetOptions = nullptr;
	context.slReflexSleep = nullptr;
	context.reflex_low_latency_enabled = false;
	context.has_present_frame_index = false;
	context.unavailable_reason = "Streamline has been shut down.";
}

void Streamline::set_present_frame_index(uint32_t p_frame_index) {
	StreamlineContext &context = StreamlineContext::get();
	context.present_frame_index = p_frame_index;
	context.has_present_frame_index = true;
}

void *Streamline::get_frame_token(uint32_t p_frame_index) {
	StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || context.slGetNewFrameToken == nullptr) {
		return nullptr;
	}

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	sl::FrameToken *frame_token = nullptr;
	PFun_slGetNewFrameToken *sl_get_new_frame_token = reinterpret_cast<PFun_slGetNewFrameToken *>(context.slGetNewFrameToken);
	const sl::Result result = sl_get_new_frame_token(frame_token, &p_frame_index);
	if (result != sl::Result::eOk) {
		return nullptr;
	}
	return frame_token;
#else
	return nullptr;
#endif
}

bool Streamline::set_reflex_low_latency_enabled(bool p_enabled) {
	StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || !context.vulkan_info_set || context.slReflexSetOptions == nullptr) {
		return false;
	}
	if (context.reflex_low_latency_enabled == p_enabled) {
		return true;
	}

#ifdef STREAMLINE_SDK_HEADERS_PRESENT
	sl::ReflexOptions options;
	options.mode = p_enabled ? sl::eLowLatency : sl::eOff;
	PFun_slReflexSetOptions *sl_reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions *>(context.slReflexSetOptions);
	const sl::Result result = sl_reflex_set_options(options);
	if (result != sl::Result::eOk) {
		return false;
	}
	context.reflex_low_latency_enabled = p_enabled;
	return true;
#else
	return false;
#endif
}

bool Streamline::is_initialized() const {
	return StreamlineContext::get().initialized && StreamlineContext::get().vulkan_info_set;
}

bool Streamline::is_feature_supported(StreamlineFeature p_feature) const {
	const StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || !context.vulkan_info_set) {
		return false;
	}

	switch (p_feature) {
		case STREAMLINE_FEATURE_DLSS:
			return context.dlss_supported;
		case STREAMLINE_FEATURE_DLSS_G:
			return context.dlss_g_supported;
		case STREAMLINE_FEATURE_REFLEX:
			return context.reflex_supported;
		case STREAMLINE_FEATURE_PCL:
			return context.pcl_supported;
		default:
			return false;
	}
}

bool Streamline::is_feature_loaded(StreamlineFeature p_feature) const {
	const StreamlineContext &context = StreamlineContext::get();
	if (!context.initialized || !context.vulkan_info_set) {
		return false;
	}

	switch (p_feature) {
		case STREAMLINE_FEATURE_DLSS:
			return context.dlss_loaded;
		case STREAMLINE_FEATURE_DLSS_G:
			return context.dlss_g_loaded;
		case STREAMLINE_FEATURE_REFLEX:
			return context.reflex_loaded;
		case STREAMLINE_FEATURE_PCL:
			return context.pcl_loaded;
		default:
			return false;
	}
}

const char *Streamline::get_unavailable_reason() const {
	return StreamlineContext::get().unavailable_reason;
}

bool Streamline::is_loaded() const {
	return StreamlineContext::get().load();
}
