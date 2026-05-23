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
- `rtgi_disable_in_editor`
- `rtgi_temporal_accumulation`
- `rtgi_temporal_accumulation_weight`
- `rtgi_denoiser_strength`
- `rtgi_overscan_horizontal`
- `rtgi_overscan_vertical`
- `rtgi_denoiser`
  - `GPU (Default)`
  - `CPU (Very Slow)`
  - `SVGF (Experimental)`
  - `None`
- RTGI debug draw modes for noisy input, guides, motion vectors, variance,
  history length, rejection mask, and final denoised output.

## RTGI Panel Option Differences

- `rtgi_enabled`
  - Turns the hardware RTGI/path tracing path on for this environment when the
    active renderer is desktop Forward+ Vulkan and the selected GPU exposes
    Vulkan ray tracing. If support is missing, the scene falls back to the
    normal non-RT rendering path. This is off by default for new environments.
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
- `rtgi_disable_in_editor`
  - Disables RTGI only for editor viewport previews. This lets authored scenes
    keep RTGI enabled for the running project and exported builds without
    making normal editor navigation pay the RTGI cost. The default is `true`.
- `rtgi_temporal_accumulation`
  - Enables accumulation of RT lighting across frames. It improves convergence
    at low sample counts but depends on valid motion, depth, and RT history
    masks. Newly visible or newly RT-ready geometry rejects stale history.
  - Moving views must render continuously so motion vectors and history buffers
    advance every frame. Static or update-once viewports are only appropriate
    for static views; they are expected to break down at `1 SPP` when the
    camera or subject moves.
- `rtgi_temporal_accumulation_weight`
  - Controls how much previous-frame RT lighting contributes to temporal
    accumulation. The value is treated as a 60 FPS reference and normalized by
    frame time so high-refresh captures do not shorten the denoiser history in
    wall-clock time. Higher values reduce light speckles and improve stability
    in static views, but can increase light ghosting or wobbling while the
    camera tracks through the scene. The default is `0.70`, which keeps a short
    enough history for gameplay lights to remain responsive.
  - This is only the temporal history weight. Set it lower for pulsing or fast
    dynamic lights that need to respond quickly. At `0.0`, RTGI denoising still
    runs when `rtgi_denoiser` is enabled, but it does not consult or blend
    previous-frame lighting.
  - It is not the viewport TAA history setting.
- `rtgi_denoiser_strength`
  - Controls the SVGF RTGI denoiser's current-frame spatial filtering,
    variance cleanup, and firefly suppression. Higher values hide more 1 SPP
    speckles, but can soften texture-driven indirect lighting and small dynamic
    light changes. The default is `0.65`. Lower values preserve more detail and
    response while leaving more raw RT noise.
  - This setting is ignored by the OIDN GPU and CPU denoisers in this version.
  - This does not change how much previous-frame lighting is reused. Use
    `rtgi_temporal_accumulation_weight` for history persistence.
- `rtgi_overscan_horizontal` and `rtgi_overscan_vertical`
  - Add an opt-in path-traced RTGI history margin around the rendered viewport.
    The values are fractions of the visible viewport size. For example, `0.05`
    allocates 5% extra pixels on both sides of that axis.
  - The visible crop is moved using camera motion so the leading edge can use up
    to twice the configured margin while the trailing edge uses less. This keeps
    accumulated RT history just outside the current viewport and can reduce
    edge-local speckles or bright strips from newly revealed pixels during
    camera motion.
  - Overscan increases the ray tracing resolution before denoising and is
    currently applied to Path Traced mode with the internal temporal denoiser.
    It does not add samples to interior pixels or replace denoiser tuning for
    full-frame motion noise. Keep it at `0.0` when the extra edge stability is
    not worth the RT cost.
- `rtgi_denoiser`
  - `GPU (Default)`: uses Intel Open Image Denoise (OIDN) with the fastest
    detected non-CPU OIDN physical device. OIDN then selects its available GPU
    module, such as CUDA, HIP, or SYCL. If GPU device creation fails, the
    renderer falls back to OIDN CPU and logs that fallback once.
  - `CPU (Very Slow)`: uses OIDN CPU directly. It is intended as a correctness
    fallback and comparison path, not the real-time default.
  - `SVGF (Experimental)`: uses the previous built-in RTGI denoiser. It is a
    vendor-neutral SVGF/RELAX-style RD effect with its own history, moment,
    variance, and atrous passes. It uses RT velocity, normal/roughness,
    albedo/metalness, linear view-Z, hit distance, validity masks, and history
    IDs to reject stale history and preserve edges.
  - `None`: disables the RT denoiser so the raw sampled RT result is visible.
    This is useful for debugging sample distribution, material hits, and TLAS
    coverage, but it is expected to show more noise.
  - Old serialized values are normalized on load: `Auto`, `NVIDIA`, `AMD`, and
    `Intel` map to `GPU (Default)`, `Internal` maps to `SVGF (Experimental)`,
    and `Off` maps to `None`.
- RTGI debug draw modes
  - `VIEWPORT_DEBUG_DRAW_RTGI_NOISY`: raw path-traced RTGI input before
    denoising.
  - `VIEWPORT_DEBUG_DRAW_RTGI_NORMAL_ROUGHNESS`: RT normal/roughness guide.
  - `VIEWPORT_DEBUG_DRAW_RTGI_VIEWZ_HITDIST`: linear view-Z and hit-distance
    guide buffer.
  - `VIEWPORT_DEBUG_DRAW_RTGI_MOTION_VECTORS`: RT-space motion vectors.
  - `VIEWPORT_DEBUG_DRAW_RTGI_VARIANCE`: temporal luminance variance.
  - `VIEWPORT_DEBUG_DRAW_RTGI_HISTORY_LENGTH`: normalized history length.
  - `VIEWPORT_DEBUG_DRAW_RTGI_REJECTION`: disocclusion/history rejection mask.
  - `VIEWPORT_DEBUG_DRAW_RTGI_FINAL`: final denoised RTGI texture before crop or
    composition.

## Rendering Behavior

RTGI is a view-time override. Existing SDFGI and VoxelGI resources stay in the
scene for fallback and comparison, but they are not sampled as indirect lighting
while RTGI is enabled and hardware ray tracing support is available. Hybrid RTGI
keeps raster lightmaps and lightmap capture SH in the primary Forward+ pass so
scenes that rely on baked/static lighting do not collapse to a black base image;
Path Traced mode still disables those baked contributions.

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

RTGI writes noisy radiance, depth, velocity, normal/roughness,
albedo/metalness, view-Z, hit-distance, validity, and history ID guides at RT
texture size. In the default OIDN path, the renderer first runs the existing RT
temporal resolve when temporal accumulation is enabled, then uses OIDN's `RT`
filter as the final spatial denoiser with radiance, albedo, and decoded normal
guides. The first OIDN integration uses blocking CPU staging from `RGBA16F` RT
textures into OIDN `Float3` buffers for correctness; zero-copy GPU interop is
reserved for a follow-up pass.

The `SVGF (Experimental)` option uses the dedicated `RTGIDenoise` RD effect for
both Hybrid RTGI and Path Traced mode. It runs temporal reprojection,
light-change reactivity, luminance moments, variance prefiltering, and
edge-aware atrous filtering before the path-traced output is cropped back to the
visible viewport. Newly visible geometry, newly loaded materials, and geometry
that has just become RT-ready therefore start from fresh samples instead of
borrowing stale accumulated lighting.

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
  - Handles geometry instances, materials, lights, guide-buffer output, temporal
    accumulation state, and denoised output.
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
  - Writes RTGI guide buffers for the internal denoiser: noisy radiance, RT
    depth, RT-space motion vectors, normal/roughness, albedo/metalness, view-Z,
    hit-distance, validity, and history ID.
- `servers/rendering/renderer_rd/effects/rtgi_denoise.*`
  - Adds the SVGF/RELAX-style RTGI denoiser as a separate RD effect.
    It maintains its own history, moments, variance, rejection, noisy-input, and
    previous-guide textures.
- `servers/rendering/renderer_rd/effects/rtgi_oidn_denoise.*`
  - Adds the OIDN RTGI denoiser wrapper. It dynamically loads the bundled
    Windows/Linux OIDN runtime, selects GPU or CPU mode, converts RTGI `RGBA16F`
    radiance/albedo/normal guides to OIDN `Float3`, runs the `RT` filter, and
    uploads the result back into the RTGI texture.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Remains the normal viewport TAA path and fallback temporal resolve. Path
    traced RTGI no longer depends on viewport TAA for the shipped internal
    denoiser.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Rejects reprojected history when RT validity or history ID checks fail in
    fallback RT temporal resolves without carrying the failed RTGI prefilter and
    relaxed-history experiments.
- `servers/rendering/renderer_rd/effects/depth_reconstruct.*`
  - Adds depth reconstruction used by the ray tracing path.
- `servers/rendering/storage/ltc/`
  - Adds LTC lookup data used by material/light evaluation.

## Source And Licensing Notes

The RTGI renderer is Godot-native RD/Vulkan code. NVIDIA-RTX/godot was used as
an implementation reference and source for compatible Godot renderer patterns
under Godot's MIT-compatible licensing. yuphin/Lumen was not vendored; it
remains only an algorithm reference because it is a standalone Vulkan renderer
rather than a drop-in Godot renderer module.

OIDN 2.4.1 is bundled under `thirdparty/oidn` for Windows and Linux RTGI
denoising. The runtime is loaded dynamically so startup and non-RTGI renders do
not link against OIDN.

## NRD Reference Audit

NVIDIA NRD v4.17.4 was reviewed as a denoiser design reference, not vendored.
NRD describes REBLUR, RELAX, and SIGMA as source/dispatched denoisers that depend
on per-pixel guides such as motion vectors, normal/roughness, viewZ, noisy
diffuse/specular radiance with hit distance, material demodulation, and explicit
application-side resource allocation for permanent/transient texture pools. Its
headers also carry NVIDIA proprietary license text, so importing the SDK is a
separate vendor-dependency decision rather than part of this general performance
pass.

Current RTGI buffer coverage compared with NRD:

- Present for every internal RTGI denoised frame: noisy radiance, RT depth
  output, RT velocity, normal/roughness, albedo/metalness, view-Z, hit-distance,
  history validity, history identity masks, temporal moments, variance, history
  length, and rejection mask.
- Present in the legacy DLSS RR buffer-output shader variant: diffuse/specular
  albedo buffers, normal/roughness, and specular hit-distance debug/output
  buffers.
- Present through the main renderer rather than an NRD-specific input: raster
  depth, raster velocity, and Forward+ normal/roughness buffers used by TAA and
  compositor paths.
- Missing for direct NRD REBLUR/RELAX import: NRD-packed diffuse/specular
  radiance-plus-hit-distance signal separation, normalized diffuse hit distance,
  NRD permanent/transient pool management, NRD dispatch/pipeline integration,
  and NRD-specific common/denoiser settings.
- Missing for SIGMA import: dedicated penumbra/translucency shadow inputs and
  a shadow denoiser dispatch path.

The default shipped RTGI denoiser is now OIDN GPU with CPU fallback. Future NRD
or vendor-specific work should start from the existing guide textures, split
diffuse/specular signals to match the target backend's resource model, then add
an optional backend that consumes those guides without changing the four-option
RTGI denoiser UI.

Legacy NVIDIA/DLSS RR enum values are retained only as compatibility aliases and
normalize to OIDN GPU for RTGI scene data. This fork does not ship a
Streamline/DLSS reconstruction pass.

## Validation

Validated so far:

- Windows editor build with Vulkan Forward+ and ray tracing code enabled.
- Windows editor startup through the console wrapper and live GUI launch.
- Windows headless startup.
- Linux/WSL focused builds for the changed renderer/server paths.
- OIDN 2.4.1 runtime packaged for Windows x64 and Linux x86_64 under
  `thirdparty/oidn`.
- Unsupported or unrelated build issues fixed in the target profile:
  - Embree/raycast no longer inherits the global AVX2/FMA flags into its
    lowest-ISA dispatch objects.
  - ETCpak now receives the MSVC AVX2/FMA feature macros required by its SIMD
    tables when building the Faster-Godot profile.
  - The RTGI denoiser selection now routes `GPU`/`CPU` through OIDN, keeps the
    previous denoiser available as `SVGF (Experimental)`, and preserves legacy
    serialized values as compatibility aliases.
  - Internal runtime capture of a representative dark 3D validation scene with
    local-light shadows off and on. The captured sequences stayed lit, did not
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
- OIDN GPU/CPU denoising currently uses blocking staging between RD textures and
  OIDN buffers. This prioritizes correctness over frame time until a zero-copy
  interop path is validated.
- Transparent particles are raster-only in this implementation; they do not
  contribute to RT GI, shadows, or reflections.
