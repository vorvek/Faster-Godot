import os


def _cpp_string_literal(path):
    return '\\"%s\\"' % path.replace("\\", "/").replace('"', '\\"')


def _append_existing_dirs(r_dirs, candidates):
    for candidate in candidates:
        if os.path.isdir(candidate) and candidate not in r_dirs:
            r_dirs.append(candidate)


def _has_rtxpt_dispatch_sources(sdk_path):
    required_source_tree_files = [
        os.path.join(sdk_path, "Rtxpt", "Sample.h"),
        os.path.join(sdk_path, "Rtxpt", "Sample.cpp"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracerSample.hlsl"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracerBridge.hlsli"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracer", "Config.h"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracer", "PathTracerShared.h"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracer", "PathTracer.hlsli"),
        os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracer", "StablePlanes.hlsli"),
    ]
    if all(os.path.isfile(path) for path in required_source_tree_files):
        return True

    required_include_layout_files = [
        os.path.join(sdk_path, "include", "Rtxpt", "Sample.h"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracerSample.hlsl"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracerBridge.hlsli"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracer", "Config.h"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracer", "PathTracerShared.h"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracer", "PathTracer.hlsli"),
        os.path.join(sdk_path, "include", "Rtxpt", "Shaders", "PathTracer", "StablePlanes.hlsli"),
    ]
    return all(os.path.isfile(path) for path in required_include_layout_files)


def _find_nrd_include_roots(sdk_path):
    include_roots = []
    for candidate in [
        os.path.join(sdk_path, "include"),
        os.path.join(sdk_path, "Include"),
        sdk_path,
    ]:
        if os.path.isfile(os.path.join(candidate, "NRD.h")) or os.path.isfile(os.path.join(candidate, "NRDDescs.h")):
            include_roots.append(candidate)
    return include_roots


def _has_streamline_dlss_rr_headers(include_root):
    required_headers = [
        "sl.h",
        "sl_core_api.h",
        "sl_core_types.h",
        "sl_dlss_d.h",
        "sl_helpers_vk.h",
    ]
    return all(os.path.isfile(os.path.join(include_root, header)) for header in required_headers)


def _find_streamline_include_roots(sdk_path):
    include_roots = []
    for candidate in [
        os.path.join(sdk_path, "include"),
        os.path.join(sdk_path, "Include"),
        sdk_path,
    ]:
        if _has_streamline_dlss_rr_headers(candidate):
            include_roots.append(candidate)
    return include_roots


def _resolve_sdk_path(configured_path, *candidate_paths):
    if configured_path and os.path.isdir(configured_path):
        return configured_path
    for candidate_path in candidate_paths:
        if candidate_path and os.path.isdir(candidate_path):
            return candidate_path
    return configured_path


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("rtxpt_sdk_path", "Path to a local NVIDIA RTXPT SDK checkout used by the experimental RTGI backend", ""),
        ("nrd_sdk_path", "Path to a local NVIDIA NRD SDK checkout used by the experimental RTGI denoiser", ""),
        ("streamline_sdk_path", "Path to a local NVIDIA Streamline SDK checkout used by optional DLSS Ray Reconstruction handoff", ""),
    ]


def configure(env):
    sdk_path = env.get("rtxpt_sdk_path", "")
    if sdk_path and os.path.isdir(sdk_path):
        include_paths = []
        _append_existing_dirs(include_paths, [
            os.path.join(sdk_path, "include"),
            os.path.join(sdk_path, "Include"),
            sdk_path,
            os.path.join(sdk_path, "Rtxpt"),
            os.path.join(sdk_path, "Rtxpt", "Shaders"),
        ])
        if include_paths:
            env.Append(CPPPATH=include_paths)

        defines = [("RTGI_RTXPT_SDK_ROOT", _cpp_string_literal(sdk_path))]
        if _has_rtxpt_dispatch_sources(sdk_path):
            defines += [
                "RTGI_RTXPT_SDK_HEADERS_PRESENT",
                "RTGI_RTXPT_GODOT_REFERENCE_DISPATCH_ENABLED",
                "RTGI_RTXPT_BACKEND_IMPLEMENTED",
            ]
        env.Append(CPPDEFINES=defines)

    nrd_path = _resolve_sdk_path(
        env.get("nrd_sdk_path", ""),
        os.environ.get("NRD_SDK_PATH", ""),
        os.environ.get("NRD_PATH", ""),
        os.path.join("addons", "rtgi_vendor_sdks", "nrd"),
        os.path.join("misc", "rtgi_quality_project", "addons", "rtgi_vendor_sdks", "nrd"),
        os.path.join("thirdparty", "nrd"),
    )
    if nrd_path and os.path.isdir(nrd_path):
        env.Append(CPPDEFINES=[("RTGI_NRD_SDK_ROOT", _cpp_string_literal(nrd_path))])
        include_roots = _find_nrd_include_roots(nrd_path)
        if include_roots:
            env.Append(CPPPATH=include_roots)
            env.Append(
                CPPDEFINES=[
                    "RTGI_NRD_SDK_HEADERS_PRESENT",
                ]
            )

    streamline_path = _resolve_sdk_path(env.get("streamline_sdk_path", ""), os.path.join("thirdparty", "streamline"))
    if streamline_path and os.path.isdir(streamline_path):
        env.Append(CPPDEFINES=[("RTGI_STREAMLINE_SDK_ROOT", _cpp_string_literal(streamline_path))])
        include_roots = _find_streamline_include_roots(streamline_path)
        if include_roots:
            env.Append(CPPPATH=include_roots)
            env.Append(
                CPPDEFINES=[
                    "RTGI_STREAMLINE_SDK_HEADERS_PRESENT",
                    "RTGI_STREAMLINE_DLSS_RR_HANDOFF_ENABLED",
                ]
            )
