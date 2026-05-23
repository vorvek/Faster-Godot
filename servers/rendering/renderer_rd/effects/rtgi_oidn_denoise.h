/**************************************************************************/
/*  rtgi_oidn_denoise.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/string/string_name.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

class RTGIOIDNDenoise {
public:
	enum Mode {
		MODE_GPU,
		MODE_CPU,
	};

	RTGIOIDNDenoise();
	~RTGIOIDNDenoise();

	bool process(Ref<RenderSceneBuffersRD> p_render_buffers,
			const StringName &p_source_context,
			const StringName &p_source_texture,
			RID p_normal_roughness,
			RID p_albedo_metalness,
			const Size2i &p_process_size,
			Mode p_mode,
			uint32_t p_view = 0);

	bool prepare(Mode p_mode, const Size2i &p_process_size);
	void invalidate();

private:
	typedef void *OIDNDevice;
	typedef void *OIDNBuffer;
	typedef void *OIDNFilter;

	enum OIDNDeviceType {
		OIDN_DEVICE_TYPE_DEFAULT = 0,
		OIDN_DEVICE_TYPE_CPU = 1,
		OIDN_DEVICE_TYPE_SYCL = 2,
		OIDN_DEVICE_TYPE_CUDA = 3,
		OIDN_DEVICE_TYPE_HIP = 4,
		OIDN_DEVICE_TYPE_METAL = 5,
	};

	enum OIDNFormat {
		OIDN_FORMAT_FLOAT3 = 3,
	};

	enum OIDNQuality {
		OIDN_QUALITY_BALANCED = 5,
	};

	enum OIDNError {
		OIDN_ERROR_NONE = 0,
	};

	struct OIDNApi {
		int (*oidnGetNumPhysicalDevices)() = nullptr;
		int (*oidnGetPhysicalDeviceInt)(int, const char *) = nullptr;
		const char *(*oidnGetPhysicalDeviceString)(int, const char *) = nullptr;
		OIDNDevice (*oidnNewDevice)(OIDNDeviceType) = nullptr;
		OIDNDevice (*oidnNewDeviceByID)(int) = nullptr;
		void (*oidnCommitDevice)(OIDNDevice) = nullptr;
		int (*oidnGetDeviceError)(OIDNDevice, const char **) = nullptr;
		void (*oidnReleaseDevice)(OIDNDevice) = nullptr;
		OIDNBuffer (*oidnNewBuffer)(OIDNDevice, size_t) = nullptr;
		void (*oidnWriteBuffer)(OIDNBuffer, size_t, size_t, const void *) = nullptr;
		void (*oidnReadBuffer)(OIDNBuffer, size_t, size_t, void *) = nullptr;
		void (*oidnReleaseBuffer)(OIDNBuffer) = nullptr;
		OIDNFilter (*oidnNewFilter)(OIDNDevice, const char *) = nullptr;
		void (*oidnSetFilterImage)(OIDNFilter, const char *, OIDNBuffer, OIDNFormat, size_t, size_t, size_t, size_t, size_t) = nullptr;
		void (*oidnSetFilterBool)(OIDNFilter, const char *, bool) = nullptr;
		void (*oidnSetFilterInt)(OIDNFilter, const char *, int) = nullptr;
		void (*oidnSetFilterFloat)(OIDNFilter, const char *, float) = nullptr;
		void (*oidnCommitFilter)(OIDNFilter) = nullptr;
		void (*oidnExecuteFilter)(OIDNFilter) = nullptr;
		void (*oidnReleaseFilter)(OIDNFilter) = nullptr;
	};

	struct Context {
		Mode requested_mode = MODE_GPU;
		Mode active_mode = MODE_GPU;
		bool cpu_fallback = false;
		Size2i size;
		OIDNDevice device = nullptr;
		OIDNBuffer color = nullptr;
		OIDNBuffer albedo = nullptr;
		OIDNBuffer normal = nullptr;
		OIDNFilter filter = nullptr;
		size_t image_byte_size = 0;
		String device_name;
	};

	OIDNApi api;
	Context context;
	void *library_handle = nullptr;
	bool library_load_attempted = false;
	bool library_loaded = false;

	bool _load_library();
	bool _load_symbols();
	void _unload_library();
	void _release_context();
	String _consume_device_error(OIDNDevice p_device) const;
	bool _create_device(Mode p_mode, OIDNDevice &r_device, Mode &r_active_mode, bool &r_cpu_fallback, String &r_device_name);
	bool _create_cpu_device(OIDNDevice &r_device, String &r_device_name);
	bool _ensure_context(Mode p_mode, const Size2i &p_size);
	bool _read_texture_rgbh(RID p_texture, uint32_t p_layer, const Size2i &p_size, Vector<float> &r_rgb, bool p_decode_normal, bool p_clamp_unit) const;
	bool _write_texture_rgbh(RID p_texture, uint32_t p_layer, const Size2i &p_size, const Vector<float> &p_rgb) const;
};

} // namespace RendererRD
