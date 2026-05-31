/**************************************************************************/
/*  streamline_context.h                                                  */
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

struct StreamlineContext {
	void *library_handle = nullptr;
	void *slInit = nullptr;
	void *slShutdown = nullptr;
	void *slIsFeatureSupported = nullptr;
	void *slIsFeatureLoaded = nullptr;
	void *slSetFeatureLoaded = nullptr;
	void *slEvaluateFeature = nullptr;
	void *slAllocateResources = nullptr;
	void *slFreeResources = nullptr;
	void *slSetTag = nullptr;
	void *slSetTagForFrame = nullptr;
	void *slSetConstants = nullptr;
	void *slGetFeatureRequirements = nullptr;
	void *slGetFeatureVersion = nullptr;
	void *slGetFeatureFunction = nullptr;
	void *slGetNewFrameToken = nullptr;
	void *slSetVulkanInfo = nullptr;
	void *slPCLSetMarker = nullptr;
	void *slReflexSetOptions = nullptr;
	void *slReflexSleep = nullptr;

	bool attempted_load = false;
	bool loaded = false;
	bool initialized = false;
	bool initialization_disabled = false;
	bool vulkan_info_set = false;
	bool dlss_supported = false;
	bool dlss_g_supported = false;
	bool reflex_supported = false;
	bool pcl_supported = false;
	bool dlss_loaded = false;
	bool dlss_g_loaded = false;
	bool reflex_loaded = false;
	bool pcl_loaded = false;
	bool dlss_g_load_disabled = false;
	bool reflex_low_latency_enabled = false;
	bool has_present_frame_index = false;
	uint32_t present_frame_index = 0;
	const char *unavailable_reason = "Streamline has not been initialized.";

	bool load();
	void unload();

	static StreamlineContext &get();
};
