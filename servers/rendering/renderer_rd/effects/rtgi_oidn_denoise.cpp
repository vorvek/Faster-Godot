/**************************************************************************/
/*  rtgi_oidn_denoise.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "rtgi_oidn_denoise.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"

#include <string.h>

namespace RendererRD {

static constexpr float OIDN_MAX_RADIANCE = 32768.0f;

static float _oidn_finite_or(float p_value, float p_fallback) {
	return Math::is_finite(p_value) ? p_value : p_fallback;
}

static float _oidn_sanitize_nonnegative(float p_value, float p_max_value) {
	return CLAMP(_oidn_finite_or(p_value, 0.0f), 0.0f, p_max_value);
}

RTGIOIDNDenoise::RTGIOIDNDenoise() {
}

RTGIOIDNDenoise::~RTGIOIDNDenoise() {
	_release_context();
	_unload_library();
}

bool RTGIOIDNDenoise::_load_library() {
	if (library_loaded) {
		return true;
	}
	if (library_load_attempted) {
		return false;
	}

	library_load_attempted = true;

	Vector<String> candidates;
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();

#if defined(WINDOWS_ENABLED)
	const String library_name = "OpenImageDenoise.dll";
	candidates.push_back(exe_dir.path_join(library_name));
	candidates.push_back(exe_dir.path_join("oidn").path_join(library_name));
	candidates.push_back(exe_dir.get_base_dir().path_join("thirdparty").path_join("oidn").path_join("bin").path_join("windows").path_join(library_name));
#elif defined(LINUXBSD_ENABLED)
	const char *library_names[] = {
		"libOpenImageDenoise.so",
		"libOpenImageDenoise.so.2",
		"libOpenImageDenoise.so.2.4.1",
	};
	for (const char *library_name : library_names) {
		candidates.push_back(exe_dir.path_join(library_name));
		candidates.push_back(exe_dir.path_join("oidn").path_join(library_name));
		candidates.push_back(exe_dir.get_base_dir().path_join("thirdparty").path_join("oidn").path_join("lib").path_join("linux").path_join(library_name));
	}
#else
	WARN_PRINT_ONCE("RTGI OIDN denoising is only packaged for Windows and Linux in this build.");
	return false;
#endif

	for (const String &candidate : candidates) {
		if (!FileAccess::exists(candidate)) {
			continue;
		}

		if (OS::get_singleton()->open_dynamic_library(candidate, library_handle) == OK) {
			if (_load_symbols()) {
				library_loaded = true;
				print_verbose(vformat("RTGI OIDN loaded runtime: %s", candidate));
				return true;
			}
			_unload_library();
			break;
		}
	}

	WARN_PRINT_ONCE("RTGI OIDN runtime was not found. Place the bundled OIDN libraries next to the executable, in an 'oidn' subdirectory, or under thirdparty/oidn for source-tree runs.");
	return false;
}

bool RTGIOIDNDenoise::_load_symbols() {
#define LOAD_OIDN_SYMBOL(m_name)                                                                                \
	if (OS::get_singleton()->get_dynamic_library_symbol_handle(library_handle, #m_name, (void *&)api.m_name) != OK) { \
		WARN_PRINT_ONCE(vformat("RTGI OIDN runtime is missing required symbol: %s", #m_name));                \
		return false;                                                                                          \
	}

	LOAD_OIDN_SYMBOL(oidnGetNumPhysicalDevices);
	LOAD_OIDN_SYMBOL(oidnGetPhysicalDeviceInt);
	LOAD_OIDN_SYMBOL(oidnGetPhysicalDeviceString);
	LOAD_OIDN_SYMBOL(oidnNewDevice);
	LOAD_OIDN_SYMBOL(oidnNewDeviceByID);
	LOAD_OIDN_SYMBOL(oidnCommitDevice);
	LOAD_OIDN_SYMBOL(oidnGetDeviceError);
	LOAD_OIDN_SYMBOL(oidnReleaseDevice);
	LOAD_OIDN_SYMBOL(oidnNewBuffer);
	LOAD_OIDN_SYMBOL(oidnWriteBuffer);
	LOAD_OIDN_SYMBOL(oidnReadBuffer);
	LOAD_OIDN_SYMBOL(oidnReleaseBuffer);
	LOAD_OIDN_SYMBOL(oidnNewFilter);
	LOAD_OIDN_SYMBOL(oidnSetFilterImage);
	LOAD_OIDN_SYMBOL(oidnSetFilterBool);
	LOAD_OIDN_SYMBOL(oidnSetFilterInt);
	LOAD_OIDN_SYMBOL(oidnSetFilterFloat);
	LOAD_OIDN_SYMBOL(oidnCommitFilter);
	LOAD_OIDN_SYMBOL(oidnExecuteFilter);
	LOAD_OIDN_SYMBOL(oidnReleaseFilter);

#undef LOAD_OIDN_SYMBOL

	return true;
}

void RTGIOIDNDenoise::_unload_library() {
	if (library_handle != nullptr) {
		OS::get_singleton()->close_dynamic_library(library_handle);
		library_handle = nullptr;
	}
	library_loaded = false;
	memset(&api, 0, sizeof(api));
}

void RTGIOIDNDenoise::_release_context() {
	if (context.filter != nullptr && api.oidnReleaseFilter != nullptr) {
		api.oidnReleaseFilter(context.filter);
	}
	if (context.color != nullptr && api.oidnReleaseBuffer != nullptr) {
		api.oidnReleaseBuffer(context.color);
	}
	if (context.albedo != nullptr && api.oidnReleaseBuffer != nullptr) {
		api.oidnReleaseBuffer(context.albedo);
	}
	if (context.normal != nullptr && api.oidnReleaseBuffer != nullptr) {
		api.oidnReleaseBuffer(context.normal);
	}
	if (context.device != nullptr && api.oidnReleaseDevice != nullptr) {
		api.oidnReleaseDevice(context.device);
	}
	context = Context();
}

void RTGIOIDNDenoise::invalidate() {
	_release_context();
}

bool RTGIOIDNDenoise::prepare(Mode p_mode, const Size2i &p_process_size) {
	ERR_FAIL_COND_V(p_process_size.x <= 0 || p_process_size.y <= 0, false);
	return _ensure_context(p_mode, p_process_size);
}

String RTGIOIDNDenoise::_consume_device_error(OIDNDevice p_device) const {
	if (api.oidnGetDeviceError == nullptr) {
		return String();
	}

	const char *message = nullptr;
	const int code = api.oidnGetDeviceError(p_device, &message);
	if (code == OIDN_ERROR_NONE) {
		return String();
	}

	if (message != nullptr && message[0] != '\0') {
		return String::utf8(message);
	}
	return vformat("OIDN error code %d", code);
}

bool RTGIOIDNDenoise::_create_cpu_device(OIDNDevice &r_device, String &r_device_name) {
	r_device = api.oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
	if (r_device == nullptr) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN CPU device creation failed: %s", _consume_device_error(nullptr)));
		return false;
	}

	api.oidnCommitDevice(r_device);
	const String error = _consume_device_error(r_device);
	if (!error.is_empty()) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN CPU device commit failed: %s", error));
		api.oidnReleaseDevice(r_device);
		r_device = nullptr;
		return false;
	}

	r_device_name = "CPU";
	return true;
}

bool RTGIOIDNDenoise::_create_device(Mode p_mode, OIDNDevice &r_device, Mode &r_active_mode, bool &r_cpu_fallback, String &r_device_name) {
	r_device = nullptr;
	r_active_mode = p_mode;
	r_cpu_fallback = false;
	r_device_name = String();

	if (p_mode == MODE_GPU) {
		String gpu_error;
		const int physical_device_count = api.oidnGetNumPhysicalDevices();
		for (int i = 0; i < physical_device_count; i++) {
			// OIDN orders physical devices approximately fastest-to-slowest, so
			// the first non-CPU device is the preferred GPU device for this mode.
			const int device_type = api.oidnGetPhysicalDeviceInt(i, "type");
			if (device_type == OIDN_DEVICE_TYPE_CPU) {
				continue;
			}

			OIDNDevice device = api.oidnNewDeviceByID(i);
			if (device == nullptr) {
				gpu_error = _consume_device_error(nullptr);
				continue;
			}

			api.oidnCommitDevice(device);
			const String error = _consume_device_error(device);
			if (error.is_empty()) {
				const char *name = api.oidnGetPhysicalDeviceString(i, "name");
				r_device = device;
				r_device_name = name != nullptr ? String::utf8(name) : vformat("physical device %d", i);
				return true;
			}

			gpu_error = error;
			api.oidnReleaseDevice(device);
		}

		WARN_PRINT_ONCE(vformat("RTGI OIDN GPU device unavailable; falling back to OIDN CPU.%s", gpu_error.is_empty() ? String() : " Last GPU error: " + gpu_error));
		r_cpu_fallback = true;
		r_active_mode = MODE_CPU;
		return _create_cpu_device(r_device, r_device_name);
	}

	r_active_mode = MODE_CPU;
	return _create_cpu_device(r_device, r_device_name);
}

bool RTGIOIDNDenoise::_ensure_context(Mode p_mode, const Size2i &p_size) {
	if (context.device != nullptr && context.requested_mode == p_mode && context.size == p_size) {
		return true;
	}

	_release_context();

	if (!_load_library()) {
		return false;
	}

	context.requested_mode = p_mode;
	context.size = p_size;
	context.image_byte_size = (size_t)p_size.x * (size_t)p_size.y * 3 * sizeof(float);

	if (!_create_device(p_mode, context.device, context.active_mode, context.cpu_fallback, context.device_name)) {
		return false;
	}

	context.color = api.oidnNewBuffer(context.device, context.image_byte_size);
	context.albedo = api.oidnNewBuffer(context.device, context.image_byte_size);
	context.normal = api.oidnNewBuffer(context.device, context.image_byte_size);
	if (context.color == nullptr || context.albedo == nullptr || context.normal == nullptr) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN buffer allocation failed: %s", _consume_device_error(context.device)));
		_release_context();
		return false;
	}

	context.filter = api.oidnNewFilter(context.device, "RT");
	if (context.filter == nullptr) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN RT filter creation failed: %s", _consume_device_error(context.device)));
		_release_context();
		return false;
	}

	api.oidnSetFilterImage(context.filter, "color", context.color, OIDN_FORMAT_FLOAT3, p_size.x, p_size.y, 0, 0, 0);
	api.oidnSetFilterImage(context.filter, "albedo", context.albedo, OIDN_FORMAT_FLOAT3, p_size.x, p_size.y, 0, 0, 0);
	api.oidnSetFilterImage(context.filter, "normal", context.normal, OIDN_FORMAT_FLOAT3, p_size.x, p_size.y, 0, 0, 0);
	api.oidnSetFilterImage(context.filter, "output", context.color, OIDN_FORMAT_FLOAT3, p_size.x, p_size.y, 0, 0, 0);
	api.oidnSetFilterBool(context.filter, "hdr", true);
	api.oidnSetFilterBool(context.filter, "cleanAux", true);
	api.oidnSetFilterInt(context.filter, "quality", OIDN_QUALITY_BALANCED);
	api.oidnSetFilterFloat(context.filter, "inputScale", 1.0f);
	api.oidnCommitFilter(context.filter);

	const String error = _consume_device_error(context.device);
	if (!error.is_empty()) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN RT filter commit failed: %s", error));
		_release_context();
		return false;
	}

	print_verbose(vformat("RTGI OIDN using %s device: %s", context.active_mode == MODE_GPU ? "GPU" : "CPU", context.device_name));
	return true;
}

bool RTGIOIDNDenoise::_read_texture_rgbh(RID p_texture, uint32_t p_layer, const Size2i &p_size, Vector<float> &r_rgb, bool p_decode_normal, bool p_clamp_unit) const {
	Vector<uint8_t> data = RD::get_singleton()->texture_get_data(p_texture, p_layer);
	const size_t pixel_count = (size_t)p_size.x * (size_t)p_size.y;
	const size_t required_size = pixel_count * 4 * sizeof(uint16_t);
	if ((size_t)data.size() < required_size) {
		return false;
	}

	r_rgb.resize((int)(pixel_count * 3));
	const uint16_t *src = reinterpret_cast<const uint16_t *>(data.ptr());
	float *dst = r_rgb.ptrw();

	for (size_t i = 0; i < pixel_count; i++) {
		float r = _oidn_finite_or(Math::half_to_float(src[i * 4 + 0]), p_decode_normal ? 0.5f : 0.0f);
		float g = _oidn_finite_or(Math::half_to_float(src[i * 4 + 1]), p_decode_normal ? 0.5f : 0.0f);
		float b = _oidn_finite_or(Math::half_to_float(src[i * 4 + 2]), p_decode_normal ? 1.0f : 0.0f);

		if (p_decode_normal) {
			Vector3 normal(r * 2.0f - 1.0f, g * 2.0f - 1.0f, b * 2.0f - 1.0f);
			if (normal.is_finite() && normal.length_squared() > 0.000001f) {
				normal.normalize();
			} else {
				normal = Vector3(0.0f, 0.0f, 1.0f);
			}
			dst[i * 3 + 0] = normal.x;
			dst[i * 3 + 1] = normal.y;
			dst[i * 3 + 2] = normal.z;
		} else {
			const float max_value = p_clamp_unit ? 1.0f : OIDN_MAX_RADIANCE;
			dst[i * 3 + 0] = _oidn_sanitize_nonnegative(r, max_value);
			dst[i * 3 + 1] = _oidn_sanitize_nonnegative(g, max_value);
			dst[i * 3 + 2] = _oidn_sanitize_nonnegative(b, max_value);
		}
	}

	return true;
}

bool RTGIOIDNDenoise::_write_texture_rgbh(RID p_texture, uint32_t p_layer, const Size2i &p_size, const Vector<float> &p_rgb) const {
	Vector<uint8_t> data = RD::get_singleton()->texture_get_data(p_texture, p_layer);
	const size_t pixel_count = (size_t)p_size.x * (size_t)p_size.y;
	const size_t required_size = pixel_count * 4 * sizeof(uint16_t);
	if ((size_t)data.size() < required_size || (size_t)p_rgb.size() < pixel_count * 3) {
		return false;
	}

	uint16_t *dst = reinterpret_cast<uint16_t *>(data.ptrw());
	const float *src = p_rgb.ptr();
	for (size_t i = 0; i < pixel_count; i++) {
		dst[i * 4 + 0] = Math::make_half_float(_oidn_sanitize_nonnegative(src[i * 3 + 0], OIDN_MAX_RADIANCE));
		dst[i * 4 + 1] = Math::make_half_float(_oidn_sanitize_nonnegative(src[i * 3 + 1], OIDN_MAX_RADIANCE));
		dst[i * 4 + 2] = Math::make_half_float(_oidn_sanitize_nonnegative(src[i * 3 + 2], OIDN_MAX_RADIANCE));
	}

	return RD::get_singleton()->texture_update(p_texture, p_layer, data) == OK;
}

bool RTGIOIDNDenoise::process(Ref<RenderSceneBuffersRD> p_render_buffers,
		const StringName &p_source_context,
		const StringName &p_source_texture,
		RID p_normal_roughness,
		RID p_albedo_metalness,
		const Size2i &p_process_size,
		Mode p_mode,
		uint32_t p_view) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_process_size.x <= 0 || p_process_size.y <= 0, false);
	ERR_FAIL_COND_V(!p_render_buffers->has_texture(p_source_context, p_source_texture), false);
	ERR_FAIL_COND_V(!p_normal_roughness.is_valid() || !p_albedo_metalness.is_valid(), false);
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, p_render_buffers->get_view_count(), false);

	if (!_ensure_context(p_mode, p_process_size)) {
		return false;
	}

	RID source = p_render_buffers->get_texture(p_source_context, p_source_texture);

	Vector<float> color;
	Vector<float> albedo;
	Vector<float> normal;
	if (!_read_texture_rgbh(source, p_view, p_process_size, color, false, false) ||
			!_read_texture_rgbh(p_albedo_metalness, p_view, p_process_size, albedo, false, true) ||
			!_read_texture_rgbh(p_normal_roughness, p_view, p_process_size, normal, true, false)) {
		WARN_PRINT_ONCE("RTGI OIDN staging read failed. Leaving RTGI output undenoised for this frame.");
		return false;
	}

	api.oidnWriteBuffer(context.color, 0, context.image_byte_size, color.ptr());
	api.oidnWriteBuffer(context.albedo, 0, context.image_byte_size, albedo.ptr());
	api.oidnWriteBuffer(context.normal, 0, context.image_byte_size, normal.ptr());
	api.oidnExecuteFilter(context.filter);

	String error = _consume_device_error(context.device);
	if (!error.is_empty()) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN denoising failed: %s", error));
		_release_context();
		return false;
	}

	api.oidnReadBuffer(context.color, 0, context.image_byte_size, color.ptrw());
	error = _consume_device_error(context.device);
	if (!error.is_empty()) {
		WARN_PRINT_ONCE(vformat("RTGI OIDN output read failed: %s", error));
		_release_context();
		return false;
	}

	if (!_write_texture_rgbh(source, p_view, p_process_size, color)) {
		WARN_PRINT_ONCE("RTGI OIDN staging write failed. Leaving RTGI output undenoised for this frame.");
		return false;
	}

	return true;
}

} // namespace RendererRD
