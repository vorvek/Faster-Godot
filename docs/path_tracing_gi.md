# Hardware RTGI And Path Tracing

## Goal

Add a desktop Forward+ Vulkan global illumination path for scenes where dynamic
light is part of the game design. The main target is dark indoor content with
moving point or spot lights, such as a character-carried lantern or torch in a
dungeon, where baked GI and screen-space effects cannot represent changing
indirect light reliably.

## User-Facing Controls

The feature is exposed on `Environment`, so it appears through the same
`WorldEnvironment` workflow as SDFGI:

- `rtgi_enabled`
- `rtgi_mode`
  - `Hybrid RTGI`
  - `Path Traced`
- `rtgi_samples_per_pixel`
- `rtgi_max_bounces`
- `rtgi_energy`
- `rtgi_temporal_accumulation`
- `rtgi_temporal_accumulation_weight`
- `rtgi_denoiser`
  - `Auto`
  - `Internal`
  - `NVIDIA`
  - `AMD`
  - `Intel`
  - `Off`
- RTGI debug draw modes for lighting, rays/noise, TLAS/instance coverage, and
  denoiser input/output.

## RTGI Panel Option Differences

- `rtgi_enabled`
  - Turns the hardware RTGI/path tracing path on for this environment when the
    active renderer is desktop Forward+ Vulkan and the selected GPU exposes
    Vulkan ray tracing. If support is missing, the scene falls back to the
    normal non-RT rendering path.
- `rtgi_mode`
  - `Hybrid RTGI`: keeps the normal Forward+ raster path for primary visibility
    and direct rendering, then injects ray-traced indirect diffuse/specular
    lighting into the GI composition. This is the practical mode for normal game
    scenes that need moving-light bounce lighting.
  - `Path Traced`: routes full opaque lighting through the ray tracing path for
    the view. It disables incompatible baked and screen-space GI contributions
    and then composites transparent raster overlays after RT denoising. This is
    heavier and is intended for high-quality dark scenes, captures, and RT
    debugging rather than broad fallback compatibility.
- `rtgi_samples_per_pixel`
  - Controls how many RT samples are traced per pixel each frame. Higher values
    reduce raw noise but cost more GPU time. Lower values rely more heavily on
    temporal accumulation and denoising.
- `rtgi_max_bounces`
  - Controls the maximum indirect bounce depth. One bounce is cheaper and works
    well for most local-light GI; extra bounces can brighten enclosed scenes but
    increase cost and noise.
- `rtgi_energy`
  - Multiplies the RT lighting contribution after tracing. This is an artistic
    intensity control, not a replacement for physically scaled light energy.
- `rtgi_temporal_accumulation`
  - Enables accumulation of RT lighting across frames. It improves convergence
    at low sample counts but depends on valid motion, depth, and RT history
    masks. Newly visible or newly RT-ready geometry rejects stale history.
- `rtgi_temporal_accumulation_weight`
  - Controls how much previous-frame RT lighting contributes to temporal
    accumulation. Higher values reduce light speckles and improve stability in
    static views, but can increase light ghosting or wobbling while the camera
    tracks through the scene. The default is `0.94`, which is intentionally
    shorter than the old forced `0.97` RT denoiser history weight.
- `rtgi_denoiser`
  - `Auto`: uses the best shipped path for this build. In this fork that means
    the internal temporal RT denoiser unless a vendor backend is explicitly
    added.
  - `Internal`: forces the built-in temporal RT denoiser. It uses RT depth,
    velocity, validity, and history ID textures to reject stale history.
  - `NVIDIA`: keeps the NVIDIA/DLSS Ray Reconstruction buffer routing and debug
    outputs available. Because this fork does not ship Streamline/DLSS RR as a
    runtime dependency, final denoising falls through to the internal temporal
    RT denoiser.
  - `AMD` and `Intel`: reserved for optional vendor denoiser integrations. With
    no shipped backend present, they fall through to the internal temporal RT
    denoiser instead of exposing raw noisy output.
  - `Off`: disables the RT denoiser so the raw sampled RT result is visible.
    This is useful for debugging sample distribution, material hits, and TLAS
    coverage, but it is expected to show more noise.
- RTGI debug draw modes
  - Lighting/debug views show intermediate RT lighting, raw ray noise, TLAS and
    instance coverage, and denoiser input/output. They are diagnostic views and
    should not be treated as final color output.

## Rendering Behavior

RTGI is a view-time override. Existing SDFGI, VoxelGI, LightmapGI, lightmap
capture SH, and baked resources stay in the scene for fallback and comparison,
but they are not sampled as indirect lighting while RTGI is enabled and hardware
ray tracing support is available.

Hybrid RTGI writes ray-traced indirect diffuse/specular lighting into the
existing Forward+ GI composition path. Path Traced mode routes full lighting
through the ray tracing path and disables incompatible screen-space or baked GI
contributions for that view.

The path-traced Forward+ flow denoises the opaque RT result before transparent
rendering. Transparent particles and other alpha overlay instances are kept out
of the TLAS and rendered as the normal Forward+ transparent pass after RT
denoise, so they remain visible without contributing to RT GI, shadows, or
reflections. This avoids particle-driven TLAS spikes and black RT speckle from
billboard particle geometry.

RT temporal denoising writes explicit history validity and history ID masks from
the primary hit/miss path. The TAA resolve rejects history when the current hit
is invalid, when the reprojected previous validity mask is invalid, or when the
history IDs no longer match. Newly visible geometry, newly loaded materials, and
geometry that has just become RT-ready therefore start from fresh samples instead
of borrowing stale accumulated lighting.

If the GPU or driver does not expose the required Vulkan ray tracing features,
the settings remain visible, a warning is printed, and rendering falls back to
the existing non-ray-traced path instead of destructively changing scene data.

## Code Changes

- `scene/resources/environment.*`
  - Adds RTGI properties, inspector bindings, enum values, and defaults.
  - Exposes per-environment RTGI temporal accumulation weight so scenes can tune
    the noise-versus-motion tradeoff without changing global TAA settings.
- `servers/rendering/storage/environment_storage.*`
  - Stores RTGI state in rendering-server environment data.
- `servers/rendering/rendering_server*`
  - Adds RTGI API plumbing and compatibility bindings.
- `servers/rendering/rendering_device*`
  - Adds ray shader stages, acceleration structure objects, ray tracing
    pipelines, shader binding tables, and `trace_rays` flow.
- `drivers/vulkan/rendering_device_driver_vulkan.*`
  - Adds Vulkan acceleration-structure, ray tracing pipeline, descriptor,
    command, and feature-detection support.
  - Resolves swapchain format metadata during swapchain creation so the editor
    can build startup blit pipelines before the first resize.
- `servers/rendering/renderer_rd/forward_clustered/render_raytracing.*`
  - Builds and updates ray tracing scene state for Forward+.
  - Handles geometry instances, materials, lights, temporal accumulation, and
    denoised output.
  - Skips `INSTANCE_PARTICLES` and alpha-overlay instances in TLAS construction.
  - Tracks per-viewport RT geometry/material history so newly visible or newly
    ready surfaces reject stale denoiser history.
  - Mirrors more StandardMaterial texture behavior in RT, including ORM,
    roughness, metallic, vertex color, sampler repeat/filter flags, alpha
    scissor/hash, UV2 for custom hit groups, and conservative fallback for
    unsupported ShaderMaterial sampler types.
- `servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.*`
  - Adds ray tracing shader version management.
- `servers/rendering/renderer_rd/shaders/raytracing/`
  - Adds ray generation and shared ray tracing shader includes.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Allows the RT denoiser path to provide current/previous RT validity and
    history ID textures to the TAA resolve.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Rejects reprojected history when RT validity or history ID checks fail.
- `servers/rendering/renderer_rd/effects/depth_reconstruct.*`
  - Adds depth reconstruction used by the ray tracing path.
- `servers/rendering/storage/ltc/`
  - Adds LTC lookup data used by material/light evaluation.

## Source And Licensing Notes

The implementation is Godot-native RD/Vulkan code. NVIDIA-RTX/godot was used as
the main implementation reference and source for compatible Godot renderer
patterns under Godot's MIT-compatible licensing. yuphin/Lumen was not vendored;
it remains only an algorithm reference because it is a standalone Vulkan
renderer rather than a drop-in Godot renderer module.

Vendor denoisers are not hard dependencies. `Auto` prefers the internal
temporal/spatial denoiser path first; NVIDIA, AMD, and Intel integrations are
kept as optional plugin or compile-time integration points.

The NVIDIA RTGI option maps to the DLSS Ray Reconstruction path where available
and still emits the DLSS RR G-buffer/debug textures. This fork does not ship a
Streamline/DLSS reconstruction pass, so the renderer also routes NVIDIA RTGI
through the same temporal RT denoising resolve used by the internal denoiser.
That avoids the previous behavior where selecting NVIDIA produced auxiliary
buffers but left the final RTGI image effectively undenoised.

## Validation

Validated so far:

- Windows editor build with Vulkan Forward+ and ray tracing code enabled.
- Windows editor startup through the console wrapper and live GUI launch.
- Windows headless startup.
- Linux/WSL focused builds for the changed renderer/server paths.
- Unsupported or unrelated build issues fixed in the target profile:
  - Embree/raycast no longer inherits the global AVX2/FMA flags into its
    lowest-ISA dispatch objects.
  - ETCpak now receives the MSVC AVX2/FMA feature macros required by its SIMD
    tables when building the Faster-Godot profile.
  - The NVIDIA RTGI denoiser selection now participates in the temporal RT
    denoising resolve while preserving DLSS RR auxiliary buffers for debug and
    future backend integration.
  - Internal screenshot capture of the Euphorica test scene with the lantern
    OmniLight shadows off and on. The captured sequences stayed lit, did not
    produce the previous all-black frames, and did not log shader push-constant
    or RT pipeline setup errors.

## Pros

- Gives dark dynamic-light scenes a GI option that follows moving lights.
- Keeps the user-facing workflow on `WorldEnvironment`/`Environment`.
- Keeps baked GI data in scenes for fallback instead of converting or deleting
  it.
- Narrows the first implementation to the fork's actual target: desktop
  Forward+ Vulkan on x86_64 AVX2/FMA machines.

## Cons And Limitations

- Requires Vulkan hardware ray tracing support for the RTGI path.
- Mobile, Compatibility, Metal, D3D12, XR, and non-Forward+ renderer paths are
  out of scope for this fork profile.
- Performance depends heavily on scene complexity, samples per pixel, bounce
  count, denoising settings, and GPU class.
- Vendor denoiser integrations are optional and are not shipped as required
  runtime dependencies.
- Transparent particles are raster-only in this implementation; they do not
  contribute to RT GI, shadows, or reflections.
