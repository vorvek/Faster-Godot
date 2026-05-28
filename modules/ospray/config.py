import os


def _find_ospray_include_roots(sdk_path):
    include_roots = []
    for candidate in [
        os.path.join(sdk_path, "include"),
        os.path.join(sdk_path, "ospray", "include"),
        sdk_path,
    ]:
        if os.path.isfile(os.path.join(candidate, "ospray", "ospray.h")):
            include_roots.append(candidate)
    return include_roots


def is_enabled():
    return False


def can_build(env, platform):
    return platform in ["windows", "linuxbsd"] and env["arch"] == "x86_64"


def get_opts(platform):
    return [
        ("ospray_sdk_path", "Path to a local Intel OSPRay SDK checkout used by the experimental RTGI backend", ""),
    ]


def configure(env):
    ospray_path = env.get("ospray_sdk_path", "")
    if ospray_path and os.path.isdir(ospray_path):
        include_roots = _find_ospray_include_roots(ospray_path)
        if include_roots:
            env.Append(CPPPATH=include_roots)
            env.Append(
                CPPDEFINES=[
                    "RTGI_EMBREE_OSPRAY_SDK_HEADERS_PRESENT",
                    "RTGI_OSPRAY_DISPATCH_ENABLED",
                    "RTGI_EMBREE_OSPRAY_BACKEND_IMPLEMENTED",
                ]
            )

    fidelityfx_path = env.get("fidelityfx_sdk_path", "")
    if fidelityfx_path and os.path.isdir(fidelityfx_path):
        env.Append(CPPPATH=[os.path.join(fidelityfx_path, "include")])
        env.Append(CPPDEFINES=["RTGI_FIDELITYFX_SDK_HEADERS_PRESENT"])
