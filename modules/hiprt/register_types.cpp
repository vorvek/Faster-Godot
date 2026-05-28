/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#ifndef RTGI_HIPRT_API_VERSION
#define RTGI_HIPRT_API_VERSION 0
#endif

#ifndef RTGI_HIPRT_VERSION_STR
#define RTGI_HIPRT_VERSION_STR ""
#endif

static bool hiprt_rtgi_backend_registered = false;

bool hiprt_module_has_rtgi_backend_implementation() {
#if defined(RTGI_HIPRT_BACKEND_IMPLEMENTED)
	return true;
#else
	return false;
#endif
}

uint32_t hiprt_module_get_api_version() {
	return RTGI_HIPRT_API_VERSION;
}

const char *hiprt_module_get_version_string() {
	return RTGI_HIPRT_VERSION_STR;
}

bool hiprt_module_is_rtgi_backend_registered() {
	return hiprt_rtgi_backend_registered;
}

void initialize_hiprt_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	hiprt_rtgi_backend_registered = hiprt_module_has_rtgi_backend_implementation() && hiprt_module_get_api_version() != 0;
}

void uninitialize_hiprt_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	hiprt_rtgi_backend_registered = false;
}
