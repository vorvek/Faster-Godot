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
- `rtgi_backend`
  - `Vulkan Generic`
  - `NVIDIA RTXPT`
  - `AMD HIP RT`
  - `Intel Embree`
- `rtgi_mode`
  - `Reflections RT Only`
  - `Full Path Tracing`
- `rtgi_samples_per_pixel`
- `rtgi_max_bounces`
- `rtgi_energy`
- `rtgi_disable_in_editor`
- `rtgi_denoiser_strength`
- `rtgi_denoiser_history_weight`
- `rtgi_denoiser_firefly_suppression`
- `rtgi_denoiser_detail_preservation`
- `rtgi_ray_firefly_suppression`
- `rtgi_ray_max_radiance`
- `rtgi_overscan_horizontal`
- `rtgi_overscan_vertical`
- `rtgi_denoiser`
  - `ASVFG (Experimental)`
  - `NVIDIA`
  - `AMD`
  - `Intel`
  - `None`
- `rtgi_strc_enabled`
- `rtgi_strc_strength`
- `rtgi_strc_cascade_count`
- `rtgi_strc_grid_size`
- `rtgi_strc_base_probe_spacing`
- `rtgi_strc_rays_per_frame`
- `rtgi_strc_temporal_weight`
- `rtgi_strc_static_visual_layers`
- `rtgi_strc_dynamic_visual_layers`
- RTGI debug draw modes for noisy input, guides, motion vectors, variance,
  history length, rejection mask, and final denoised output.

## RTGI Panel Option Differences

- `rtgi_enabled`
  - Turns the hardware RTGI/path tracing path on for this environment when the
    active renderer is desktop Forward+ Vulkan and the selected GPU exposes
    Vulkan ray tracing. If support is missing, the scene falls back to the
    normal non-RT rendering path. This is off by default for new environments.
- `rtgi_backend`
  - `Vulkan Generic`: uses the current built-in Vulkan ray tracing
    implementation. This is the default backend.
  - `NVIDIA RTXPT`: uses the NVIDIA Godot fork-compatible RenderingDevice/Vulkan
    ray tracing dispatch path when the RTXPT module is compiled. Optional
    NVIDIA denoising remains runtime-gated.
  - `AMD HIP RT`: uses HIP RT scene and trace-kernel dispatch when the HIP RT
    module, runtime libraries, and Vulkan/HIP external memory and semaphore
    exchange are available. It reports unavailable instead of using readbacks
    when external interop cannot be established.
  - `Intel Embree`: uses Embree/OSPRay-capable CPU rendering when the backend is
    compiled, then uploads the result into the RD-owned RTGI output. Probe
    updates may still use the Vulkan Generic path in mixed mode.
- `rtgi_mode`
  - `Reflections RT Only`: keeps the normal Forward+ raster path and raster GI
    systems responsible for diffuse GI, then uses ray tracing for specular and
    reflection paths only.
  - `Full Path Tracing`: routes full opaque lighting through the ray tracing path for
    the view. It disables incompatible baked and screen-space GI contributions
    and then composites transparent raster overlays after RT denoising. This is
    heavier and is intended for high-quality dark scenes, captures, and RT
    debugging rather than broad fallback compatibility.
- `rtgi_samples_per_pixel`
  - Controls how many RT samples are traced per pixel each frame. Higher values
    reduce raw noise but cost more GPU time. Lower values rely more heavily on
    the internal temporal denoiser and current-frame spatial filtering.
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
- `rtgi_denoiser_strength`
  - Controls the RTGI denoiser's current-frame spatial filtering,
    variance cleanup, broad blotch stabilization, and firefly suppression.
    Higher values hide more 1 SPP speckles, but can soften texture-driven
    indirect lighting and small dynamic light changes. The default is `0.90`.
    Lower values preserve more detail and response while leaving more raw RT
    noise.
- `rtgi_denoiser_history_weight`
  - Controls valid previous-frame radiance reuse in the temporal denoiser.
    Higher values improve stability on static surfaces; lower values reduce
    ghosting and response lag. `0.0` disables temporal RTGI radiance reuse, and
    `rtgi_denoiser_strength = 0.0` also forces history off.
- `rtgi_denoiser_firefly_suppression`
  - Controls additional isolated bright-pixel suppression. Raise it for dark
    1 SPP scenes with unsupported sparkles; lower it when small emissives,
    sparse highlights, or thin bright details are being clipped too strongly.
- `rtgi_denoiser_detail_preservation`
  - Controls albedo-detail protection in the broad rough-surface cleanup
    stages. Raise it when brick, stone, patterned floors, or other textured
    surfaces get flattened at high denoiser strength; lower it if textured
    surfaces keep too much residual grain.
- `rtgi_ray_firefly_suppression`
  - Applies a biased linear-HDR contribution clamp before the RTGI denoiser.
    It affects direct-light samples, secondary emissive/sky hits, and secondary
    throughput before those values enter temporal history. Primary sky/background
    rays are not clamped by this control. Raise it when dark 1 SPP scenes show
    isolated fireflies that survive the denoiser; lower it when legitimate tiny
    lights or glossy highlights are clipped.
- `rtgi_ray_max_radiance`
  - Sets the approximate luminance limit used by the pre-denoiser ray clamp.
    `0.0` disables this source-side clamp. The default is conservative and is
    intended to stop extreme path samples rather than flatten authored lighting.
- `rtgi_overscan_horizontal` and `rtgi_overscan_vertical`
  - Add an opt-in RTGI render margin around the visible viewport. The values are
    fractions of the visible viewport size. Higher values can reduce edge
    speckles from newly revealed pixels while moving, but increase RT cost and
    do not add samples to the interior of the image.
- `rtgi_denoiser`
  - `ASVFG (Experimental)`: uses the built-in GPU ASVFG denoiser. It is a
    vendor-neutral RD effect with guided stabilization, moment, variance, and
    atrous passes. It uses RT velocity, normal/roughness, albedo/metalness,
    linear view-Z, hit distance, validity masks, and history IDs to preserve
    edges and reject invalid samples.
  - `Internal Signal Decomposition`: uses the internal multi-signal denoise and
    recomposition path. Direct light, emissive, indirect, sky, diffuse, and
    specular radiance keep separate temporal/spatial resources before the final
    internal composite pass.
  - `NVIDIA`: requests the NVIDIA denoiser path. NRD and DLSS Ray
    Reconstruction remain gated by Streamline/API/platform/runtime requirements.
    Until available, this warns once and falls back to ASVFG without rewriting
    the scene.
  - Legacy `FidelityFX`, `AMD`, and `Intel` constants are accepted for older
    scenes and scripts, warn once, and normalize to `Internal Signal
    Decomposition`. They do not call an external FidelityFX, AMD, or Intel
    denoiser backend.
  - `None`: disables the RT denoiser so the raw sampled RT result is visible.
    This is useful for debugging sample distribution, material hits, and TLAS
    coverage, but it is expected to show more noise.
- `rtgi_strc_*`
  - Controls the spatiotemporal RTGI radiance cache used for rough secondary
    diffuse paths. The static and dynamic visual layer masks select which
    render layers participate in STRC probe updates and cache sampling; objects
    outside both masks are ignored by STRC. Layers present in both masks are
    treated as static, while dynamic-only layers are traced into the cache with
    lower temporal confidence so they refresh more aggressively.

## Backend Capability Contract

The backend API exposes `RenderingServer.pathtracing_get_backend_capabilities()`,
`RenderingServer.pathtracing_get_backend_status()`, and
`RenderingServer.pathtracing_get_backend_status_for_backend(backend)` so UI and
tooling can distinguish a serialized backend request from a runtime-available
backend. The no-argument status reports the active renderer's last request; the
request-scoped status reports the fallback decision for a specific backend
without changing the current `Environment`. Status dictionaries include the
requested backend, active backend, `using_fallback`, `fallback_backend`,
`fallback_backend_name`, `fallback_reason`, and the requested/active capability
dictionaries. `fallback_backend` is `-1` when no fallback is active; otherwise
it names the backend that will render instead of the request.

Each capability dictionary includes:

- `backend`, `name`, `runtime_name`, `integration_path`,
  `rendering_device_family`, `rendering_device_name`,
  `rendering_device_vendor`, and `rendering_device_vendor_id`
- `available` and `initialized`
- `vulkan_runtime`
- `exchange`, with explicit `rendering_device`, `external_memory`,
  `external_semaphore`, `timeline_semaphore`, and `staged_copy` booleans
- `availability_checks`
  - `backend_compiled`
  - `runtime_detected`
  - `device_supported`
  - `resource_exchange_supported`
  - `implementation_ready`
  - `failure`, one of `none`, `backend_not_compiled`,
    `runtime_not_detected`, `device_not_supported`,
    `resource_exchange_unavailable`, or `implementation_unavailable`
  - `compile_failure_reason`, `runtime_failure_reason`,
    `device_failure_reason`, `resource_exchange_failure_reason`, and
    `implementation_failure_reason`, which provide the human-readable reason
    for the corresponding failed check
- `denoiser_handoff`
- `fallback_reason`

`available` must only become true when the backend is compiled in, its runtime
is detected through the active `RenderingDevice`, the active device family,
vendor, and features are supported, one complete resource exchange route is
available, and the backend has a real scene import/dispatch implementation:
`rendering_device`, `external_memory` plus
`external_semaphore`, `timeline_semaphore`, or `staged_copy`. Vendor backends
stay unavailable until those checks and the implementation-ready gate are real.
Selecting an unavailable vendor backend leaves the `Environment` request intact,
emits one clear warning per backend, and falls back to `Vulkan Generic` for
rendering.
The Vulkan `RenderingDevice` reports external-memory, external-semaphore, and
timeline-semaphore support as driver capabilities; vendor backends may expose
those bits in their status even while their runtime probe or
implementation-ready gate keeps them unavailable. Vendor runtime probes are
conservative dynamic-library checks against the executable directory, `PATH`,
and the expected SDK root variables (`RTXPT_SDK_PATH`/`RTXPT_PATH`,
`HIPRT_PATH`/`HIP_PATH`/`ROCM_PATH`, and
`EMBREE_ROOT`/`EMBREE_DIR`/`OSPRAY_ROOT`/`OSPRAY_DIR`/`ONEAPI_ROOT`).
Where the public ABI is stable enough for this phase, probes also require one
expected entry point (`hipInit`, `rtcNewDevice`, or `ospInit`) before reporting
the runtime as detected.
Backend-specific compile checks are intentionally tied to their own optional
modules: `module_rtxpt_enabled`, `module_hiprt_enabled`, and
`module_embree_enabled` or `module_ospray_enabled`. Unrelated modules do not
make a vendor RTGI backend compiled or available.
Unavailable backends also report `denoiser_handoff = false`; denoiser handoff
only describes a path the active backend can actually reach.
The per-frame backend context also carries an explicit scene resource snapshot:
the Vulkan-owned TLAS, BLAS RID list, geometry/material/motion/emissive buffers,
light buffer, environment/parameter buffer, and resource counts. Vendor
backends must import or mirror from that snapshot instead of reaching around the
contract for hidden renderer state.
Backends that use external Vulkan memory/semaphore or timeline-semaphore
exchange must provide explicit `RenderingDevice` acquire and release driver
callbacks. The acquire callback is recorded before backend scene import,
material/light/environment upload, and dispatch; the release callback is
recorded after backend synchronization. Both phases must declare the output
texture as a writable callback resource so the frame graph tracks the ownership
handoff. External-memory exchange must also declare typed platform handles
(`opaque FD` or `opaque Win32`), binary semaphore handles, output image layout
expectations before and after backend work, and an RD-to-backend-to-RD ownership
direction. Timeline-semaphore exchange uses `RenderingDevice` semaphore RIDs,
timeline values, the same image layout metadata, and the same ownership
direction. Staged-copy exchange is deliberately callback-managed in this phase:
the backend release callback performs the copy into the Vulkan-owned output
texture, must declare a backend-to-RD copy ownership direction, must use that
same texture as the staged copy target, and must declare the source buffer as
readable and target texture as writable. It must also declare the
`RDD::BufferTextureCopyRegion` list and target texture layout for the copy; the
shared backend validator rejects empty regions, invalid aspects, negative
texture offsets, zero row pitch, and non-positive copy extents. RD-internal
backends can dispatch directly through `RenderingDevice`.
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
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE`: split-signal diffuse
    radiance after the diffuse cache and before ASVFG consumes it. This view is
    available only when the RTGI diffuse radiance cache is active.
  - `VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_CANDIDATE`: source-selection diagnostic
    for RTGI. Red marks the selected source class, green marks
    source confidence, blue marks normalized candidate weight, and alpha stores
    normalized contribution luminance for harness metrics. It is intended for
    source-side variance attribution before the diffuse cache and ASVFG.
  - `VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_HISTORY`: source-history diagnostic for
    RTGI. Red marks the current selected source class, green marks
    whether previous source history exists for the pixel, and blue marks source
    class agreement with the previous frame.
  - `VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_TEMPORAL_DELTA`: source temporal-delta
    diagnostic for RTGI. Red marks pixels eligible for temporal
    comparison by source class, green marks exact source-key reuse, and blue
    stores normalized contribution-luminance delta.
  - `VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_REJECTION`: direct source-history
    validation diagnostic for RTGI. Red marks direct analytic candidates, green
    marks dominant-source history eligibility, and blue stores the primary
    rejection reason bucket.

## Rendering Behavior

RTGI is a view-time override. Existing SDFGI and VoxelGI resources stay in the
scene for fallback and comparison. `Reflections RT Only` preserves raster GI
ownership for diffuse lighting and suppresses diffuse RT paths, while
`Full Path Tracing` disables incompatible baked and screen-space GI
contributions for the view.

`Reflections RT Only` writes ray-traced specular/reflection lighting into the
existing Forward+ composition path. `Full Path Tracing` routes full lighting
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
texture size. The RTGI denoiser consumes those guides directly on the GPU and uses
previous-frame radiance history controlled by `rtgi_denoiser_history_weight`.
Fast movement and disocclusion favor current-frame samples through guide-based
rejection.

The `ASVFG (Experimental)` option uses the dedicated `RTGIDenoise` RD effect for
both RTGI modes. It runs temporal reprojection, guided
stabilization, light-change reactivity, luminance moments, variance
prefiltering, and edge-aware atrous filtering before the path-traced output is
written at the visible internal render size. Newly visible geometry, newly loaded
materials, and geometry that has just become RT-ready therefore start from fresh
samples instead of borrowing stale accumulated lighting.

The Internal Signal Decomposition path runs a separate RTGI denoise graph for
the split lighting signals. Low-frequency diffuse GI, dominant direct light,
emissive, sky, and specular radiance each keep their own temporal/spatial
resources, then an internal composite pass remodulates the denoised signals into
the final RTGI output. Switching between ASVFG, Internal Signal Decomposition,
legacy vendor selections, and raw output clears the inactive denoiser resources
so stale histories are not reused across modes.

The ray tracing path now applies optional source-side contribution clamping in
linear HDR space before denoising. The high-strength denoiser path
then treats isolated bright pixels in dark, unsupported neighborhoods as
firefly candidates before they enter temporal moments/history. Broad diffuse
cleanup is reduced on textured rough surfaces, while glossy and metallic
surfaces use shorter temporal history and less spatial filtering so brick,
stone, polished metal, and similar material detail are less likely to be
flattened by max-strength filtering.

If the GPU or driver does not expose the required Vulkan ray tracing features,
the settings remain visible, a warning is printed, and rendering falls back to
the existing non-ray-traced path instead of destructively changing scene data.

## Code Changes

- `scene/resources/environment.*`
  - Adds RTGI properties, inspector bindings, enum values, and defaults.
  - Low-SPP stability is handled by the RTGI denoiser with a dedicated RTGI
    history-weight control.
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
  - Handles geometry instances, materials, lights, guide-buffer output, and
    denoised output.
  - Passes source-side ray firefly controls to the ray shaders and invalidates
    RT history when those radiance-shaping parameters change.
  - Skips `INSTANCE_PARTICLES` and alpha-overlay instances in TLAS construction.
  - Tracks per-viewport RT geometry/material validity so newly visible or newly
    ready surfaces start from fresh denoiser inputs.
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
  - Applies clean-room, biased linear-HDR contribution limits to extreme direct
    samples, secondary emissive/sky hits, and secondary path throughput before
    ASVFG history.
- `servers/rendering/renderer_rd/effects/rtgi_denoise.*`
  - Adds the ASVFG/RELAX-style RTGI denoiser and Internal Signal Decomposition
    denoiser as separate RD effect paths.
    It maintains moments, variance, rejection, noisy-input, and guide textures,
    then runs a current-frame guided stabilizer to reduce broad diffuse
    blotches. Max denoiser strength uses stronger isolated-outlier suppression
    instead of forcing an extra large-radius atrous pass.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Remains the normal viewport TAA path and fallback temporal resolve. Path
    traced RTGI uses its dedicated RTGI denoiser before transparent rendering.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Rejects reprojected history when RT validity or history ID checks fail in
    fallback RT temporal resolves.
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

RTGI denoising is Godot-native RD/Vulkan code implemented in-tree.

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
- Present through the main renderer rather than an NRD-specific input: raster
  depth, raster velocity, and Forward+ normal/roughness buffers used by TAA and
  compositor paths.
- Missing for direct NRD REBLUR/RELAX import: NRD-packed diffuse/specular
  radiance-plus-hit-distance signal separation, normalized diffuse hit distance,
  NRD permanent/transient pool management, NRD dispatch/pipeline integration,
  and NRD-specific common/denoiser settings.
- Missing for SIGMA import: dedicated penumbra/translucency shadow inputs and
  a shadow denoiser dispatch path.

The default shipped RTGI denoiser is now ASVFG for interactive motion stability,
with Internal Signal Decomposition available as the multi-signal alternative.
Future NRD or vendor-specific work should start from the existing guide
textures, split diffuse/specular signals to match the target backend's resource
model, then add an optional backend that consumes those guides without changing
the vendor-neutral RTGI denoiser UI.

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
  - Internal runtime capture of a representative dark 3D validation scene with
    local-light shadows off and on. The captured sequences stayed lit, did not
    produce the previous all-black frames, and did not log shader push-constant
    or RT pipeline setup errors.

The standalone `misc/rtgi_quality_project` harness procedurally builds a small
dark RTGI room with textured brick/stone surfaces, a small warm light, a dark
firefly-detection region, and a detail-retention region. It can capture final
and RTGI debug views, then writes JSON metrics for isolated hot pixels, dark
region luminance, and high-frequency texture detail. Use it for Phase 2 baseline
captures before enabling experimental direct-light reuse.

The harness and Euphorica capture script also consume the `source_candidate`,
`source_history`, `source_temporal_delta`, and `source_rejection` debug views
when requested. The reported `rtgi_source_candidate_*` metrics expose
source-class coverage,
confidence, candidate weight percentiles, contribution percentiles, temporal
eligible fraction, class agreement, exact source-key reuse, direct dominant-key
accepted/rejected history rates, and contribution-delta percentiles so
many-light instability can be separated from diffuse-cache and denoiser
behavior. Direct lighting attribution still stores one dominant analytic source
key alongside the aggregate direct contribution, so temporal deltas are
diagnostics for dominant-source stability rather than proof of source-specific
radiance reuse safety.

The temporal source-key metrics are diagnostics-only in this phase. Analytic
lights use a 28-bit run-local hash of the light instance RID and light type.
Explicit emissive candidates use geometry history IDs for source keys. These
keys are stable enough for within-run diagnostics, but they are not persistent
collision-safe radiance reuse keys. The `source_rejection` view records
reprojected previous-surface validation reasons for direct analytic source
history; it currently validates against the dominant previous source candidate,
not a dedicated direct-only source history surface.
Indirect contributions are included in the current source-class diagnostic, but
not in exact source-id reuse metrics because this phase does not yet carry a
stable secondary-path source identity. Non-direct source-history deltas remain
same-pixel diagnostics in this phase; only the direct rejection view uses
reprojected previous-surface validation. Radiance-affecting source reuse must
first add a direct-only history surface and document the replacement slot
PDF/weight accounting.

## Pros

- Gives dark dynamic-light scenes a GI option that follows moving lights.
- Keeps the user-facing workflow on `WorldEnvironment`/`Environment`.
- Keeps baked GI data in scenes for fallback instead of converting or deleting
  it.
- Narrows the first implementation to the fork's actual target: desktop
  Forward+ Vulkan on x86_64 AVX2/FMA machines.

## Cons And Limitations

- Requires Vulkan hardware ray tracing support for the RTGI path.
- Mobile, Compatibility, Metal, XR, non-Vulkan, and non-Forward+ renderer paths
  are out of scope for this fork profile.
- Performance depends heavily on scene complexity, samples per pixel, bounce
  count, denoising settings, and GPU class.
- Transparent particles are raster-only in this implementation; they do not
  contribute to RT GI, shadows, or reflections.
