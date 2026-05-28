import os


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
    env.Append(CPPDEFINES=["RTGI_RTXPT_BACKEND_IMPLEMENTED", "RTGI_RTXPT_GODOT_REFERENCE_DISPATCH_ENABLED"])
    if sdk_path and os.path.isdir(sdk_path):
        include_paths = []
        for candidate in [
            os.path.join(sdk_path, "include"),
            sdk_path,
            os.path.join(sdk_path, "Rtxpt"),
            os.path.join(sdk_path, "Rtxpt", "Shaders"),
        ]:
            if os.path.isdir(candidate):
                include_paths.append(candidate)
        if include_paths:
            env.Append(CPPPATH=include_paths)

        rtxpt_header_candidates = [
            os.path.join(sdk_path, "Rtxpt", "Sample.h"),
            os.path.join(sdk_path, "Rtxpt", "Shaders", "PathTracer", "Config.h"),
            os.path.join(sdk_path, "include", "Rtxpt", "Sample.h"),
        ]
        if any(os.path.isfile(header) for header in rtxpt_header_candidates):
            env.Append(CPPDEFINES=["RTGI_RTXPT_SDK_HEADERS_PRESENT"])

    nrd_path = env.get("nrd_sdk_path", "")
    if nrd_path and os.path.isdir(nrd_path):
        env.Append(CPPPATH=[os.path.join(nrd_path, "include")])
        env.Append(CPPDEFINES=["RTGI_NRD_SDK_HEADERS_PRESENT"])

    streamline_path = env.get("streamline_sdk_path", "")
    if streamline_path and os.path.isdir(streamline_path):
        env.Append(CPPPATH=[os.path.join(streamline_path, "include")])
        env.Append(CPPDEFINES=["RTGI_STREAMLINE_SDK_HEADERS_PRESENT"])
