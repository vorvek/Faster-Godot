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
  - Default: `0.875`.
  - Controls the base contribution from previous-frame history for pure raster
    rendering. When RTGI is active the resolve picks a per-mode history weight
    instead (see RTGI TAA Profiles below).
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

### RTGI TAA Profiles

When path-traced global illumination is active the resolve selects one of three
profiles, because the fast path-traced primary and the reference path tracer
have very different noise levels:

- Full Path Tracing (fast) and Hybrid use a low-ghost profile. The path-traced
  primary is already clean, so leaning on history mostly adds motion ghosting
  rather than useful denoising. This profile lowers the history weight, weakens
  the luminance flicker-reduction term, and rejects history on fast-moving
  fragments (a velocity-driven cutoff after Playdead's INSIDE). Moving
  characters keep far less ghosting while edge anti-aliasing still works.
- The Full Path Tracing reference oracle uses a heavy-denoise profile. The
  oracle is a very noisy, cache-free full path trace meant for A/B comparison,
  so it keeps a high history weight and the full flicker-reduction term to calm
  the noise. It overrides the viewport TAA setting and logs a one-time warning,
  so the reference looks the same regardless of how a project configures its
  viewport TAA. Its history weight is the
  `rendering/rtgi/fpt_reference_taa/history_weight` project setting (default
  `0.95`).
- Pure raster keeps the stock resolve with the project history weight.

During development the low-ghost profile can be tuned per knob with environment
variables: `FPT_TAA_HISTORY` (history weight), `FPT_TAA_DIFF_STRENGTH`
(flicker-reduction strength), and `FPT_TAA_KTRUST_LO` / `FPT_TAA_KTRUST_HI`
(the velocity history-reject range in texels).

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
  - Uses configurable TAA parameters, and selects the RTGI low-ghost or oracle
    profile (per-mode history weight, flicker-reduction strength, and velocity
    history-reject range), with environment-variable overrides for tuning.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Uses configurable history weight, disocclusion threshold, optional
    sharpening, a per-profile anti-flicker strength, and a velocity-driven
    history-reject.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
  - Picks the TAA profile from the active RTGI mode and applies the reference
    oracle's viewport-TAA override.
- `doc/classes/ProjectSettings.xml` and `doc/classes/Viewport.xml`
  - Document the public settings and official-like defaults.

## Limits

- More aggressive values can increase shimmer, flicker, aliasing, or ghosting.
- Visual tuning is content-dependent; thin geometry, particles, skinned meshes,
  alpha-tested materials, and rapid camera motion can still show artifacts.
