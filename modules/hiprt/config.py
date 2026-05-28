import os
import re


def _cpp_string_literal(path):
    return '\\"%s\\"' % path.replace("\\", "/").replace('"', '\\"')


def _parse_generated_hiprt_header_version(header_path):
    if not os.path.isfile(header_path):
        return None
    with open(header_path, "r", encoding="utf-8", errors="ignore") as header:
        match = re.search(r"#\s*define\s+HIPRT_API_VERSION\s+([0-9]+)", header.read())
    return match.group(1) if match else None


def _parse_hiprt_source_version(sdk_path):
    version_path = os.path.join(sdk_path, "version.txt")
    if not os.path.isfile(version_path):
        return None
    with open(version_path, "r", encoding="utf-8", errors="ignore") as version_file:
        lines = [line.strip() for line in version_file.readlines()]
    if len(lines) < 2 or not lines[0].isdigit() or not lines[1].isdigit():
        return None
    return str(int(lines[0]) * 1000 + int(lines[1]))


def _find_hiprt_api_version(sdk_path):
    for header_path in [
        os.path.join(sdk_path, "include", "hiprt", "hiprt.h"),
        os.path.join(sdk_path, "hiprt", "hiprt.h"),
    ]:
        version = _parse_generated_hiprt_header_version(header_path)
        if version:
            return version
    return _parse_hiprt_source_version(sdk_path)


def _format_hiprt_version_string(api_version):
    return str(api_version).zfill(5)


def _resolve_sdk_path(configured_path, vendored_path):
    if configured_path and os.path.isdir(configured_path):
        return configured_path
    if os.path.isdir(vendored_path):
        return vendored_path
    return configured_path


def _find_fidelityfx_denoiser_include_roots(sdk_path):
    include_roots = []
    for candidate in [
        os.path.join(sdk_path, "Kits", "FidelityFX", "denoisers", "include"),
        os.path.join(sdk_path, "denoisers", "include"),
        os.path.join(sdk_path, "include"),
        sdk_path,
    ]:
        if os.path.isfile(os.path.join(candidate, "ffx_denoiser.h")):
            include_roots.append(candidate)
    return include_roots


def _find_fidelityfx_api_include_roots(sdk_path):
    include_roots = []
    for candidate in [
        os.path.join(sdk_path, "Kits", "FidelityFX", "api", "include"),
        os.path.join(sdk_path, "api", "include"),
        os.path.join(sdk_path, "include"),
        sdk_path,
    ]:
        if os.path.isfile(os.path.join(candidate, "ffx_api.h")):
            include_roots.append(candidate)
    return include_roots


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("hiprt_sdk_path", "Path to a local AMD HIP RT SDK checkout used by the experimental RTGI backend", ""),
        ("fidelityfx_sdk_path", "Path to a local AMD FidelityFX SDK checkout used by the experimental RTGI denoiser", ""),
    ]


def configure(env):
    hiprt_path = env.get("hiprt_sdk_path", "")
    if hiprt_path and os.path.isdir(hiprt_path):
        include_paths = []
        for include_path in [
            os.path.join(hiprt_path, "include"),
            hiprt_path,
        ]:
            if os.path.isdir(os.path.join(include_path, "hiprt")):
                include_paths.append(include_path)
        if include_paths:
            env.Append(CPPPATH=include_paths)
        defines = ["RTGI_HIPRT_SDK_HEADERS_PRESENT", "RTGI_HIPRT_CONTEXT_DISPATCH_ENABLED"]
        defines.append(("RTGI_HIPRT_SDK_ROOT", _cpp_string_literal(hiprt_path)))
        required_device_headers = [
            "hiprt_common.h",
            "hiprt_device.h",
            "hiprt_math.h",
            "hiprt_types.h",
            "hiprt_vec.h",
        ]
        device_headers_present = any(
            all(os.path.isfile(os.path.join(include_path, "hiprt", header)) for header in required_device_headers)
            for include_path in include_paths
        )
        api_version = _find_hiprt_api_version(hiprt_path)
        if api_version:
            defines.append("RTGI_HIPRT_API_VERSION=%s" % api_version)
            defines.append(("RTGI_HIPRT_VERSION_STR", _cpp_string_literal(_format_hiprt_version_string(api_version))))
            if device_headers_present:
                defines.append("RTGI_HIPRT_TRACE_KERNEL_DISPATCH_ENABLED")
                defines.append("RTGI_HIPRT_BACKEND_IMPLEMENTED")
        env.Append(CPPDEFINES=defines)

    fidelityfx_path = _resolve_sdk_path(env.get("fidelityfx_sdk_path", ""), os.path.join("thirdparty", "fidelityfx-sdk"))
    if fidelityfx_path and os.path.isdir(fidelityfx_path):
        include_roots = _find_fidelityfx_denoiser_include_roots(fidelityfx_path)
        include_roots += [path for path in _find_fidelityfx_api_include_roots(fidelityfx_path) if path not in include_roots]
        if include_roots:
            env.Append(CPPPATH=include_roots)
            env.Append(
                CPPDEFINES=[
                    ("RTGI_FIDELITYFX_SDK_ROOT", _cpp_string_literal(fidelityfx_path)),
                    "RTGI_FIDELITYFX_SDK_HEADERS_PRESENT",
                    "RTGI_FIDELITYFX_SDK_DENOISER_HEADERS_PRESENT",
                ]
            )
