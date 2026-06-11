/**************************************************************************/
/*  environment.compat.inc                                                */
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

#ifndef DISABLE_DEPRECATED

void Environment::_set_rtgi_wrc_strength_bind_compat_bc5243e(float p_strength) {
	// Removed property; kept resolvable for prebuilt GDExtensions that bind the
	// old API (the bundled rapier_2d resolves it eagerly at load). Does nothing.
	// Delete this file and the environment.h declarations once the bundled
	// rapier_2d is rebuilt against the current extension_api.
}

float Environment::_get_rtgi_wrc_strength_bind_compat_bc5243e() const {
	// Removed property; 1.0 was the old default (and the value the WRC now
	// effectively samples at).
	return 1.0f;
}

void Environment::_bind_compatibility_methods() {
	ClassDB::bind_compatibility_method(D_METHOD("set_rtgi_wrc_strength", "strength"), &Environment::_set_rtgi_wrc_strength_bind_compat_bc5243e);
	ClassDB::bind_compatibility_method(D_METHOD("get_rtgi_wrc_strength"), &Environment::_get_rtgi_wrc_strength_bind_compat_bc5243e);
}

#endif // DISABLE_DEPRECATED
