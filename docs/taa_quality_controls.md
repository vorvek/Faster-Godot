# TAA Quality Controls

## Goal

Expose the Forward+ TAA tuning that official Godot keeps mostly hard-coded.
The fork targets desktop Forward+ games, so projects should be able to trade
history stability, ghosting, shimmer, and sharpness without patching renderer
source.

## User-Facing Controls

The controls are Project Settings under `rendering/anti_aliasing/quality`:

- `taa_sharpness`
  - Default: `0.20`.
  - Adds clamp-aware sharpening during the TAA resolve.
  - Higher values counter TAA blur but can increase shimmer or edge halos.
- `taa_history_weight`
  - Default: `0.90`.
  - Controls the base contribution from previous-frame history.
  - Official behavior was equivalent to `0.9375`, which is steadier but more
    prone to ghosting.
- `taa_disocclusion_threshold`
  - Default: `2.0` texels.
  - Controls how much motion-vector change is tolerated before history is
    rejected.
  - Official behavior used a hard-coded `2.5` texel threshold.
- `taa_jitter_phase_count`
  - Default: `16`.
  - Controls how many Halton jitter positions plain TAA cycles through.
- `taa_jitter_scale`
  - Default: `1.0`.
  - Scales plain TAA camera jitter. `0.0` disables camera jitter while keeping
    the TAA resolve active.

These settings are intentionally project-global. The fork does not add new
`Viewport` or `RenderingServer` runtime API for per-viewport TAA tuning.

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

- `servers/rendering/rendering_server.cpp`
  - Registers the new Project Settings with editor-visible ranges and balanced
    defaults.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Reads cached Project Settings and sends the values through TAA push
    constants.
  - Preserves the existing raytracing/TAA history-validity plumbing.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Uses configurable history weight and disocclusion threshold.
  - Adds clamp-aware resolve sharpening.
- `servers/rendering/renderer_viewport.*`,
  `servers/rendering/renderer_scene_cull.*`, and
  `servers/rendering/rendering_method.h`
  - Carry the plain-TAA jitter scale through the internal render-camera path.
- `doc/classes/ProjectSettings.xml`
  - Documents the new settings.

## Pros

- Gives projects a direct way to reduce TAA ghosting without disabling TAA.
- Adds a bounded sharpening pass that counters blur while limiting halos.
- Keeps temporal upscaler behavior unchanged.
- Keeps the public runtime API small and avoids adding per-viewport tuning
  surface before there is a concrete need.

## Cons And Limitations

- More aggressive values can increase shimmer, flicker, or aliasing.
- The settings are read from Project Settings, not exposed as per-viewport
  runtime properties.
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
