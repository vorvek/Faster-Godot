def can_build(env, platform):
    # glslang is only needed when the Vulkan renderer is available,
    # as OpenGL doesn't use glslang.
    return env["vulkan"]


def configure(env):
    pass
