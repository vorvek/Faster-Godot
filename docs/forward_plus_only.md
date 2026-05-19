# Forward+ Only Renderer Profile

## Goal

Remove runtime and build-time renderer paths that are not needed by a desktop
Vulkan Forward+ game.

## Code Changes

- `SConstruct`
  - Enables Vulkan.
  - Disables OpenGL/GLES compatibility, D3D12, Metal, XR, and deprecated APIs in
    the Faster-Godot profile.
  - Defines `FASTER_GODOT_FORWARD_PLUS_ONLY`.
- `main/main.cpp`
  - Limits renderer hints to `forward_plus`.
  - Rejects `mobile` and `gl_compatibility` as valid renderer selections in the
    fork profile.
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

- Projects that need Mobile or Compatibility rendering cannot use this fork
  profile.
- Low-end GPUs with fewer than 48 textures per shader stage are no longer
  supported by fallback.
- Some official Godot tutorials or samples that depend on Compatibility mode may
  not run without building with `faster_godot=no`.
