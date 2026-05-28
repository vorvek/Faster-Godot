import os


def _cpp_string_literal(path):
    return '\\"%s\\"' % path.replace("\\", "/").replace('"', '\\"')


def _has_embree_headers(include_root):
    return os.path.isfile(os.path.join(include_root, "embree4", "rtcore.h")) or os.path.isfile(os.path.join(include_root, "embree3", "rtcore.h"))


def _find_embree_library(sdk_path, platform):
    library_roots = [
        os.path.join(sdk_path, "lib"),
        os.path.join(sdk_path, "lib64"),
        os.path.join(sdk_path, "lib", "x64"),
        os.path.join(sdk_path, "lib", "Release"),
        sdk_path,
    ]

    if platform == "windows":
        candidates = [("embree4", "embree4.lib"), ("embree3", "embree3.lib")]
    else:
        candidates = [
            ("embree4", "libembree4.so"),
            ("embree4", "libembree4.a"),
            ("embree3", "libembree3.so"),
            ("embree3", "libembree3.a"),
        ]

    for root in library_roots:
        if not os.path.isdir(root):
            continue
        for library_name, file_name in candidates:
            if os.path.isfile(os.path.join(root, file_name)):
                return root, library_name

    return None, None


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("embree_sdk_path", "Path to a local Intel Embree SDK checkout. The RTGI CPU backend is currently disabled.", ""),
    ]


def configure(env):
    embree_path = env.get("embree_sdk_path", "")
    embree_include = os.path.join(embree_path, "include") if embree_path else ""
    if embree_path and os.path.isdir(embree_include) and _has_embree_headers(embree_include):
        env.Append(CPPPATH=[embree_include])
        defines = [
            "RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT",
            ("RTGI_EMBREE_SDK_ROOT", _cpp_string_literal(embree_path)),
        ]
        library_path, library_name = _find_embree_library(embree_path, env.get("platform", ""))
        if library_path and library_name:
            env.Append(LIBPATH=[library_path])
            env.Append(LIBS=[library_name])
        env.Append(CPPDEFINES=defines)
    elif os.path.isdir("thirdparty/embree/include") and _has_embree_headers("thirdparty/embree/include"):
        env.Append(CPPPATH=["thirdparty/embree/include"])
        env.Append(CPPDEFINES=["RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT"])

    fidelityfx_path = env.get("fidelityfx_sdk_path", "")
    if fidelityfx_path and os.path.isdir(fidelityfx_path):
        env.Append(CPPPATH=[os.path.join(fidelityfx_path, "include")])
        env.Append(CPPDEFINES=["RTGI_FIDELITYFX_SDK_HEADERS_PRESENT"])
