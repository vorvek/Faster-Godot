def can_build(env, platform):
    # glslang is only needed when Vulkan or Metal-based renderers are available,
    # as OpenGL doesn't use glslang.
    return env["vulkan"] or env["metal"]


def configure(env):
    pass
