# TAA Quality Controls

## Goal

Expose the Forward+ TAA tuning that official Godot keeps mostly hard-coded,
while keeping Faster-Godot's default behavior close to the official resolve.
Projects can still trade history stability, ghosting, shimmer, and sharpness
per viewport without patching renderer source.

## User-Facing Controls

The controls are available as per-`Viewport` properties and can be configured
dynamically or via the inspector. The root viewport inherits its initial values
from Project Settings under `rendering/anti_aliasing/quality`:

- `taa_sharpness`
  - Default: `0.0`.
  - Adds optional clamp-aware sharpening during the TAA resolve. The default
    keeps official-like no-extra-sharpening behavior.
- `taa_history_weight`
  - Default: `0.9375`.
  - Controls the base contribution from previous-frame history.
- `taa_disocclusion_threshold`
  - Default: `2.5` texels.
  - Controls how much motion-vector change is tolerated before history is
    rejected.
- `taa_jitter_phase_count`
  - Default: `16`.
  - Controls how many Halton jitter positions plain TAA cycles through.
- `taa_jitter_scale`
  - Default: `1.0`.
  - Scales plain TAA camera jitter. `0.0` disables camera jitter while keeping
    the TAA resolve active.

Any `Viewport` can override these values at runtime via scripting
(`viewport.taa_sharpness`, etc.) or directly in the editor inspector.

## Rendering Behavior

Plain TAA uses the existing Forward+ jitter, motion-vector reprojection,
neighborhood clipping, velocity disocclusion, luminance-based flicker
reduction, and history buffers. The exposed settings replace the hard-coded
constants or add an optional bounded sharpening step.

Sharpening reuses the current 3x3 resolve neighborhood. The shader computes the
neighborhood average, min, and max, then applies:

```text
resolved + (resolved - neighborhood_average) * taa_sharpness
```

The sharpened result is clamped back to the 3x3 neighborhood bounds to avoid
obvious overshoot halos.

RTGI has an additional internal stabilization path described in
[path_tracing_gi.md](path_tracing_gi.md). That path can feed RTGI-specific
reactivity into TAA without adding new public TAA or RTGI controls.

Temporal upscalers keep their existing jitter behavior. FSR2 computes its own
jitter phase count and ignores `taa_jitter_phase_count` and `taa_jitter_scale`.

## Code Changes

- `scene/main/viewport.*`
  - Exposes properties, getters, setters, and ClassDB bindings for the 5 TAA
    tuning options.
- `scene/main/scene_tree.cpp`
  - Initializes the root viewport's TAA parameters using Project Settings
    fallbacks at startup.
- `servers/rendering/rendering_server.*`
  - Declares the viewport TAA setter methods and registers the global Project
    Settings.
- `servers/rendering/renderer_viewport.*`
  - Stores viewport TAA fields and passes them to
    `RenderSceneBuffersConfiguration`.
- `servers/rendering/storage/render_scene_buffers.*`
  - Carries the TAA settings through render-buffer configuration.
- `servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.*`
  - Caches the TAA parameters in `RenderSceneBuffersRD`.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Uses configurable TAA parameters and supports an internal RTGI reactivity
    mask for RTGI-only resolves.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Uses configurable history weight, disocclusion threshold, optional
    sharpening, and optional RTGI reactivity.
- `doc/classes/ProjectSettings.xml` and `doc/classes/Viewport.xml`
  - Document the public settings and official-like defaults.

## Limits

- More aggressive values can increase shimmer, flicker, aliasing, or ghosting.
- Visual tuning is content-dependent; thin geometry, particles, skinned meshes,
  alpha-tested materials, and rapid camera motion can still show artifacts.
