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


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("hiprt_sdk_path", "Path to a local AMD HIP RT SDK checkout used by the experimental RTGI backend", ""),
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
            if device_headers_present:
                defines.append("RTGI_HIPRT_TRACE_KERNEL_DISPATCH_ENABLED")
                defines.append("RTGI_HIPRT_BACKEND_IMPLEMENTED")
        env.Append(CPPDEFINES=defines)

    fidelityfx_path = env.get("fidelityfx_sdk_path", "")
    if fidelityfx_path and os.path.isdir(fidelityfx_path):
        env.Append(CPPPATH=[os.path.join(fidelityfx_path, "include")])
        env.Append(CPPDEFINES=["RTGI_FIDELITYFX_SDK_HEADERS_PRESENT"])
