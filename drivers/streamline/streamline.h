/**************************************************************************/
/*  streamline.h                                                          */
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

#include <stdint.h>

enum StreamlineMarkerType {
	STREAMLINE_MARKER_AFTER_DEVICE_CREATION,
	STREAMLINE_MARKER_BEGIN_PRESENT,
	STREAMLINE_MARKER_END_PRESENT,
	STREAMLINE_MARKER_MODIFY_SWAPCHAIN,
	STREAMLINE_MARKER_BEFORE_DEVICE_DESTROY,
};

struct StreamlinePresentContext {
	void *queue = nullptr;
	const void *present_info = nullptr;
	const void *swapchains = nullptr;
	const uint32_t *image_indices = nullptr;
	uint32_t swapchain_count = 0;
	const void *wait_semaphores = nullptr;
	uint32_t wait_semaphore_count = 0;
};

struct StreamlineAcquireContext {
	void *device = nullptr;
	void *swapchain = nullptr;
	uint64_t timeout = 0;
	void *semaphore = nullptr;
	void *fence = nullptr;
	uint32_t *image_index = nullptr;
};

struct StreamlineSwapchainContext {
	void *swapchain = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	const void *images = nullptr;
	const void *image_views = nullptr;
	uint32_t image_count = 0;
};

struct StreamlineVulkanDeviceContext {
	void *instance = nullptr;
	void *physical_device = nullptr;
	void *device = nullptr;
	bool device_created_by_interposer = false;
	uint32_t graphics_queue_family = 0;
	uint32_t graphics_queue_index = 0;
	uint32_t compute_queue_family = 0;
	uint32_t compute_queue_index = 0;
	uint32_t optical_flow_queue_family = 0;
	uint32_t optical_flow_queue_index = 0;
};

enum StreamlineFeature {
	STREAMLINE_FEATURE_DLSS,
	STREAMLINE_FEATURE_DLSS_G,
	STREAMLINE_FEATURE_REFLEX,
	STREAMLINE_FEATURE_PCL,
};

class Streamline {
	static Streamline *singleton;

	void *vulkan_physical_device = nullptr;

public:
	Streamline();
	~Streamline();

	static Streamline *get_singleton() { return singleton; }

	void set_internal_parameter(const char *p_name, void *p_value);
	void *get_internal_parameter(const char *p_name) const;
	bool is_enabled_for_current_process() const;
	void emit_marker(StreamlineMarkerType p_marker);
	void begin_acquire_next_image(const StreamlineAcquireContext &p_context);
	void end_acquire_next_image(int32_t p_result, uint32_t p_image_index);
	void begin_present(const StreamlinePresentContext &p_context);
	void end_present();
	void notify_swapchain_resized(const StreamlineSwapchainContext &p_context);
	bool initialize_pre_device();
	bool initialize_vulkan_device(const StreamlineVulkanDeviceContext &p_context);
	void shutdown();
	void set_present_frame_index(uint32_t p_frame_index);
	void *get_frame_token(uint32_t p_frame_index);
	bool set_reflex_low_latency_enabled(bool p_enabled);
	bool is_initialized() const;
	bool is_feature_supported(StreamlineFeature p_feature) const;
	bool is_feature_loaded(StreamlineFeature p_feature) const;
	const char *get_unavailable_reason() const;
	bool is_loaded() const;
};
