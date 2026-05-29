# TAA Quality Controls

## Goal

Expose the Forward+ TAA tuning that official Godot keeps mostly hard-coded.
The fork targets desktop Forward+ games, so projects should be able to trade
history stability, ghosting, shimmer, and sharpness without patching renderer
source.

## User-Facing Controls

The controls are available as per-`Viewport` properties and can be configured dynamically or via the inspector. By default, the root viewport inherits their initial values from the global Project Settings under `rendering/anti_aliasing/quality`:

- `taa_sharpness`
  - Default: `0.10`.
  - Adds clamp-aware sharpening during the TAA resolve.
  - Higher values counter TAA blur but can increase shimmer or edge halos.
- `taa_history_weight`
  - Default: `0.93`.
  - Controls the base contribution from previous-frame history.
  - Official behavior was equivalent to `0.9375`, which is steadier but more
    prone to ghosting.
- `taa_disocclusion_threshold`
  - Default: `2.25` texels.
  - Controls how much motion-vector change is tolerated before history is
    rejected.
  - Official behavior used a hard-coded `2.5` texel threshold.
- `taa_jitter_phase_count`
  - Default: `16`.
  - Controls how many Halton jitter positions plain TAA cycles through.
- `taa_jitter_scale`
  - Default: `0.85`.
  - Scales plain TAA camera jitter. `0.0` disables camera jitter while keeping
    the TAA resolve active.

The root `Viewport` automatically falls back to these Project Settings, but any `Viewport` can customize them at runtime via scripting (`viewport.taa_sharpness`, etc.) or directly in the editor inspector, allowing fine-grained control for different render targets (e.g., split-screen, UI viewports, or high-performance viewports).

## Rendering Behavior

Plain TAA still uses the existing Forward+ jitter, motion-vector reprojection,
neighborhood clipping, velocity disocclusion, luminance-based flicker
reduction, and history buffers. The new settings only replace hard-coded
constants or add a final bounded sharpening step.

Sharpening reuses the current 3x3 resolve neighborhood. The shader computes the
neighborhood average, min, and max, then applies:

```text
resolved + (resolved - neighborhood_average) * taa_sharpness
```

The sharpened result is clamped back to the 3x3 neighborhood bounds to avoid
obvious overshoot halos.

Temporal upscalers keep their existing jitter behavior. FSR2 and MetalFX
Temporal still compute their own jitter phase counts and ignore
`taa_jitter_phase_count` and `taa_jitter_scale`.

## Code Changes

- `scene/main/viewport.h`, `scene/main/viewport.cpp`
  - Exposes properties, getters, setters, and ClassDB bindings for the 5 TAA tuning options, so they appear in the editor inspector and can be updated dynamically via scripts.
- `scene/main/scene_tree.cpp`
  - Initializes the root viewport's TAA parameters using Project Settings fallbacks at startup.
- `servers/rendering/rendering_server.*`
  - Declares the viewport TAA setter methods on `RenderingServer`, implements them in `RenderingServerDefault`, and registers the global Project Settings.
- `servers/rendering/renderer_viewport.*`
  - Declares and implements `RendererViewport` setters to assign viewport TAA fields.
  - Passes these values to `RenderSceneBuffersConfiguration` during viewport 3D rendering configuration.
- `servers/rendering/storage/render_scene_buffers.*`
  - Adds and binds properties and methods to `RenderSceneBuffersConfiguration` to carry these settings to GDExtension and internal render buffers.
- `servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.*`
  - Caches the TAA parameters in `RenderSceneBuffersRD` when configuring buffers.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Updates the `resolve()` signature to accept custom TAA parameters as inputs and uses them to populate the shader push constants.
  - Extracts the 3 resolution-invariant parameters directly from `p_render_buffers` and forwards them to `resolve()`.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Uses configurable history weight and disocclusion threshold.
  - Adds clamp-aware resolve sharpening.
- `doc/classes/ProjectSettings.xml`
  - Documents the new global settings.

## Pros

- Gives projects a direct way to reduce TAA ghosting without disabling TAA.
- Adds a bounded sharpening pass that counters blur while limiting halos.
- Keeps temporal upscaler behavior unchanged.
- Keeps the public runtime API small and avoids adding per-viewport tuning
  surface before there is a concrete need.

## Cons And Limitations

- More aggressive values can increase shimmer, flicker, or aliasing.
- Visual tuning is content-dependent; thin geometry, particles, skinned meshes,
  alpha-tested materials, and rapid camera motion can still show artifacts.
- The defaults are intentionally biased toward this fork's desktop Forward+
  profile rather than matching official Godot bit-for-bit.

## Validation

Validated with:

```powershell
scons platform=windows target=editor dev_build=yes tests=no
```

The build regenerates the TAA shader header and links the Windows editor and
console editor binaries successfully.
