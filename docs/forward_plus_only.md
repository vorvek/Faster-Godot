# Forward+ Only Renderer Profile

## Goal

Remove runtime and build-time renderer paths that are not needed by a desktop
Vulkan Forward+ game.

## Code Changes

- `SConstruct`
  - Enables Vulkan.
  - Disables OpenGL/GLES compatibility, Metal, XR, and deprecated APIs in
    the Faster-Godot profile.
  - Defines `FASTER_GODOT_FORWARD_PLUS_ONLY`.
- `main/main.cpp`
  - Limits renderer hints to `forward_plus`.
  - Redirects a `mobile` or `gl_compatibility` rendering method to `forward_plus`
    with a warning, so a project authored in official Godot with one of those
    methods still opens instead of aborting at startup.
- `servers/rendering/renderer_rd/renderer_compositor_rd.cpp`
  - Always instantiates `RenderForwardClustered`.
  - Removes the fallback to Forward Mobile.
- `servers/rendering/renderer_rd/SCsub`
  - Skips building `forward_mobile`.
- `servers/rendering/renderer_rd/shaders/SCsub`
  - Skips Forward Mobile shader generation.
- `servers/rendering/renderer_rd/effects/tone_mapper.*`
  - Skips mobile tonemap shader state, pipelines, and subpass code.
- `servers/rendering/renderer_rd/shaders/effects/SCsub`
  - Skips `tonemap_mobile.glsl` generation.
- `servers/rendering/rendering_server.cpp`
  - Removes mobile and OpenGL compatibility project-setting defaults from the
    fork build where they no longer apply.

## Pros

- Reduces compiled code and shader variants.
- Removes renderer selection and fallback branches that cannot be used by the
  target profile.
- Keeps renderer behavior more predictable for profiling.
- Reduces binary size and startup/project-setting surface area.

## Cons

- Projects that need Mobile or Compatibility rendering run on Forward+ instead;
  the fork has no Mobile or Compatibility renderer to fall back to.
- Low-end GPUs with fewer than 48 textures per shader stage are no longer
  supported by fallback.
- Samples that depend on Compatibility-specific rendering behavior run on
  Forward+ here, so their results can differ from official Godot.
