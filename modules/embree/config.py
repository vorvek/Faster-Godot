import os


def _has_embree_headers(include_root):
    return os.path.isfile(os.path.join(include_root, "embree4", "rtcore.h")) or os.path.isfile(os.path.join(include_root, "embree3", "rtcore.h"))


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("embree_sdk_path", "Path to a local Intel Embree SDK checkout used by the experimental RTGI backend", ""),
    ]


def configure(env):
    embree_path = env.get("embree_sdk_path", "")
    embree_include = os.path.join(embree_path, "include") if embree_path else ""
    if embree_path and os.path.isdir(embree_include) and _has_embree_headers(embree_include):
        env.Append(CPPPATH=[embree_include])
        env.Append(CPPDEFINES=["RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT"])
    elif os.path.isdir("thirdparty/embree/include") and _has_embree_headers("thirdparty/embree/include"):
        env.Append(CPPPATH=["thirdparty/embree/include"])
        env.Append(CPPDEFINES=["RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT"])

    fidelityfx_path = env.get("fidelityfx_sdk_path", "")
    if fidelityfx_path and os.path.isdir(fidelityfx_path):
        env.Append(CPPPATH=[os.path.join(fidelityfx_path, "include")])
        env.Append(CPPDEFINES=["RTGI_FIDELITYFX_SDK_HEADERS_PRESENT"])
