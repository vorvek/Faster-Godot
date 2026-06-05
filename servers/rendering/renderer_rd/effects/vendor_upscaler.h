/**************************************************************************/
/*  vendor_upscaler.h                                                     */
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

#include "core/math/projection.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererRD {

class VendorUpscaler {
public:
	struct SuperResolutionParameters {
		RSE::ViewportScaling3DMode mode = RSE::VIEWPORT_SCALING_3D_MODE_OFF;
		Size2i internal_size;
		Size2i target_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID exposure;
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		float aspect = 1.0f;
		float sharpness = 0.0f;
		Vector2 jitter;
		float delta_time = 0.0f;
		bool reset_accumulation = false;
		bool orthogonal_projection = false;
		Projection camera_view_to_clip;
		Projection reprojection;
		Transform3D camera_transform;
	};

	static bool is_super_resolution_mode(RSE::ViewportScaling3DMode p_mode);
	static bool is_super_resolution_available(RSE::ViewportScaling3DMode p_mode);
	static const char *get_super_resolution_name(RSE::ViewportScaling3DMode p_mode);
	static const char *get_super_resolution_unavailable_reason(RSE::ViewportScaling3DMode p_mode);
	static RSE::ViewportScaling3DMode get_super_resolution_fallback(RSE::ViewportScaling3DMode p_mode);
	static bool upscale(const SuperResolutionParameters &p_params);

	// Destroys any live vendor-upscaler GPU contexts while the RenderingDevice is still
	// valid. Must be called during rendering-backend shutdown, before the device is torn
	// down. No-op when no vendor upscaler is compiled in or none were used.
	static void cleanup();
};

} // namespace RendererRD
