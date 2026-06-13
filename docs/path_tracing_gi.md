# Hardware RTGI And Path Tracing

## Goal

Add a desktop Forward+ Vulkan global illumination path for scenes where dynamic
light is part of the game design. The main target is dark indoor content with
moving point or spot lights, such as a character-carried lantern or torch in a
dungeon, where baked GI and screen-space effects cannot represent changing
indirect light reliably.

## User-Facing Controls

The feature is exposed on `Environment`, so it appears through the same
`WorldEnvironment` workflow as SDFGI. The main controls are described in the
next section, and the RTGI debug draw modes are listed under Backend
Capability Contract. Two further toggles,
`rtgi_analytic_light_sampling_enabled` and
`rtgi_explicit_emissive_sampling_enabled`, switch the two explicit
light-sampling strategies on and off; they are documented in the `Environment`
class reference.

## RTGI Controls

- `rtgi_enabled`
  - Turns the hardware RTGI/path tracing path on for this environment when the
    active renderer is desktop Forward+ Vulkan and the selected GPU exposes
    Vulkan ray tracing. If support is missing, the scene falls back to the
    normal non-RT rendering path. This is off by default for new environments.
- `rtgi_backend`
  - `Vulkan Generic`: uses the current built-in Vulkan ray tracing
    implementation. This is the only exposed backend.
- `rtgi_quality_preset`
  - Applies named quality bundles for the probe and denoise settings that the
    renderer resolves per tier. It does not change `rtgi_mode`; selecting
    `Full Scene Path-Traced GI` remains an explicit mode choice. The `Custom`
    preset keeps the `Environment` property values as authored, while the
    hidden `rendering/rtgi/*` pipeline project settings resolve to the
    `Balanced` tier; with `--verbose`, the renderer notes this substitution.
- `rtgi_mode`
  - `Reflections RT Only`: keeps the normal Forward+ raster path and raster GI
    systems responsible for diffuse GI, with ray tracing intended for specular
    and reflection paths only. This mode currently produces no composited RTGI
    output: the frame keeps the raster GI, and the engine warns once while the
    mode is active.
  - `Hybrid RTGI`: keeps the normal Forward+ raster opaque pass and traces RTGI
    against raster G-buffer guides, then denoises and blends the RTGI
    contribution into the raster frame. This is the default mode.
  - `Full Scene Path-Traced GI`: routes full opaque lighting through the ray
    tracing path for the view. It disables incompatible baked and screen-space
    GI contributions and then composites transparent raster overlays after RT
    denoising. This is heavier and is intended for explicit high-quality dark
    scenes, captures, and RT debugging rather than broad fallback compatibility.
  - `Full Scene Path-Traced GI (Reference)`: the same full path-traced view, routed
    through the deep reference path tracer with the probe composite, denoiser, and
    stabilizer bypassed. It is a slow ground-truth reference for A/B checks against
    the production Full Scene Path-Traced GI, not a shipping mode. It was previously
    a hidden flag; folding it into the mode list keeps it a normal, reversible choice
    rather than a value a saved scene could hold with no way to clear it in the editor.
- `rtgi_samples_per_pixel`
  - Sets the per-pixel sample count of the path-traced primary lighting
    estimate in `Full Scene Path-Traced GI` and of the deep-path reference.
    Higher values reduce raw primary lighting noise at extra GPU cost; the
    temporal stabilizer and the denoiser run downstream, so the visible gain
    is smaller than the raw metric. On the Cornell test scene at 1080p on an
    RTX 4080 SUPER, the temporal sparkle maximum per megapixel went from
    `128.9` at 1 sample to `61.0` at 2 samples (+0.26 ms) and `6.8` at
    4 samples (+0.71 ms). Probe rays always trace a single sample: higher
    counts made their stabilized sample sequences noisier while costing more.
    Every quality preset pins this control to `1`; the `Custom` preset
    exposes it.
- `rtgi_max_bounces`
  - Controls the maximum bounce depth of the GI probe paths and of the
    deep-path reference. The path-traced primary lighting is not affected;
    its indirect light comes from the probe gather, which this control bounds.
    One bounce is cheaper and works well for most local-light GI; extra
    bounces can brighten enclosed scenes but increase cost and noise.
- `rtgi_energy`
  - Multiplies the composited indirect GI in every mode. In `Hybrid RTGI` it
    scales only the indirect GI that RTGI adds and leaves the raster direct
    lighting untouched; in `Full Scene Path-Traced GI` it scales the whole
    ray-traced frame, the primary in the raygen and the indirect at the
    composite. It is an artistic multiplier on the GI contribution, not an
    exposure control. Earlier builds scaled only the path-traced primary in
    `Full Scene Path-Traced GI` and did nothing in `Hybrid RTGI`; on the
    Cornell scene, the occluded-region linear-luma ratio at energy `2.0` is
    now `2.00` in `Full Scene Path-Traced GI` and `1.98` in `Hybrid RTGI`,
    where it used to stay at `1.00`.
- `rtgi_resolution_scale`
  - Scales the internal RTGI render size before denoising and temporal
    stabilization, then reconstructs the result to the viewport's internal
    resolution. The default is `0.67`, similar to the way real-time GI systems
    usually trace lighting below final display resolution and rely on guided
    reconstruction. Raise it toward `1.0` for sharper GI and lower it toward
    `0.25` for cheaper tracing with more denoiser/reconstruction pressure.
- `rtgi_disable_in_editor`
  - Disables RTGI only for editor viewport previews. This lets authored scenes
    keep RTGI enabled for the running project and exported builds without
    making normal editor navigation pay the RTGI cost. The default is `true`.
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
- `rtgi_denoiser`
  - `ASVFG (Experimental)`: the default. The built-in vendor-neutral denoise
    chain: temporal accumulation and an a-trous spatial pass in the GI
    resolve, plus a temporal stabilizer on the path-traced primary when no
    temporal upscaler is active. It uses RT velocity, normal/roughness,
    depth, and history validity to preserve edges and reject invalid samples.
  - `None`: bypasses the temporal stabilizer and the spatial denoise polish so
    the accumulated GI signal can be inspected. Temporal accumulation and
    compositing still run, so this does not show the raw per-frame samples
    either. Expect visible noise; it is a debugging value, not a shipping
    configuration.
  - `Reactive`: not in the inspector dropdown, but accepted from scripts. Runs
    ASVFG and additionally derives a reactive mask from the RTGI resolve's
    per-pixel temporal confidence, then feeds it to the FSR2 reactive channel
    and the XeSS responsive-pixel mask.
  - Deprecated values (`SVGF`, `Internal Signal Decomposition`, the legacy
    serialized `None`, and the old vendor selections) are accepted on load and
    normalize to ASVFG or to the current `None` without rewriting the scene.
    They do not call an external vendor denoiser backend.

## Backend Capability Contract

The backend API exposes `RenderingServer.pathtracing_get_backend_capabilities()`,
`RenderingServer.pathtracing_get_backend_status()`, and
`RenderingServer.pathtracing_get_backend_status_for_backend(backend)` so UI and
tooling can distinguish a serialized backend request from a runtime-available
backend. The no-argument status reports the active renderer's last request; the
request-scoped status reports the fallback decision for a specific backend
without changing the current `Environment`. `Environment.rtgi_backend` only
exposes `Vulkan Generic`; vendor backend status is diagnostic metadata for
lower-level tooling, not a scene-facing backend selector. Status dictionaries include the
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
Request-scoped vendor backend status reports the diagnostic fallback decision
without making those backends selectable from `Environment`.
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
  - `VIEWPORT_DEBUG_DRAW_RTGI_FINAL`: scaled denoised RTGI texture before
    reconstruction, crop, or composition.
  - `VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED`: full-resolution RTGI
    reconstruction used for Full Path Tracing copy-out and Hybrid RTGI
    composition when `rtgi_resolution_scale` is below `1.0`.
  - `VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_REACTIVITY`: full-resolution
    reactivity mask produced by RTGI reconstruction and consumed by the
    post-reconstruction RTGI TAA resolve.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE`: split-signal diffuse
    radiance after the diffuse cache and before ASVFG consumes it. This view is
    available only when the RTGI diffuse radiance cache is active.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_RADIANCE`: directional screen-probe
    gather atlas radiance. This is the downsampled directional cache before it
    is gathered back into full-resolution diffuse reconstruction.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_CONFIDENCE`: alpha confidence from the
    directional screen-probe gather atlas.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_STATS`: packed SPG quality metadata.
    Red is age, green is local support, blue is radiance coherence, and alpha is
    depth-plane quality.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_PLANE_QUALITY`: SPG depth-plane quality
    expanded to luminance, useful for finding mixed-surface probe bins that
    should not feed reconstruction or ray-side reuse.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_VISIBILITY`: directional SPG
    visibility/hit-distance atlas. It is shown as log luminance from the stored
    hit distance and receiver depth; the atlas also carries hit-distance quality
    and support used by reconstruction.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REJECTION`: full-resolution SPG
    reconstruction diagnostic. Red stores the dominant rejection class, green
    stores rejection/sample strength, blue stores gathered support, and alpha
    stores accepted SPG confidence. Rejection classes currently cover low
    confidence, stats, normal, depth, history, surface identity, visibility,
    hemisphere, radiance delta, and low final quality.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINEMENT`: base-probe refinement mask.
    Red is request strength, green is geometry/plane risk, blue is
    radiance/visibility risk, and alpha is the hysteresis-held request.
  - `VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINED_CONFIDENCE`: alpha confidence
    from the refined SPG atlas.
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
  - `VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SOURCE`: ray-side secondary
    diffuse cache attribution for accepted Path Tracing cache reuse. Red marks
    the selected cache source (`receiver`, `STRC`, base SPG, or refined SPG),
    green stores the accepted cache weight, blue marks the query family
    (secondary-hit cache query or current-screen trace query), and alpha stores
    log-scaled cached luminance. This view records accepted reuse only; cache
    misses and rejection reasons still live in the cache/SPG rejection
    diagnostics.
  - `VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_REJECTION`: ray-side secondary
    diffuse cache rejection attribution. Red stores the dominant miss reason,
    green marks the query family, blue stores normalized near-hit/weak-source
    detail, and alpha stores the fallback source hint. Reasons distinguish
    ineligible material/query, no source, weak receiver cache, weak refined SPG,
    weak base SPG, weak STRC, current-screen trace miss, and current-screen
    trace low final weight.

## Rendering Behavior

RTGI is a view-time override. Existing SDFGI and VoxelGI resources stay in the
scene for fallback and comparison. `Reflections RT Only` preserves raster GI
ownership for diffuse lighting and suppresses diffuse RT paths, `Hybrid RTGI`
adds denoised ray-traced GI onto the raster frame, and `Full Path Tracing`
disables incompatible baked and screen-space GI contributions for the view.

`Reflections RT Only` is intended to write ray-traced specular/reflection
lighting into the existing Forward+ composition path, but it currently
produces no composited RTGI output; the frame keeps the raster GI and the
engine warns once. `Hybrid RTGI` writes a denoised RTGI contribution before
additive blending. `Full Path Tracing` routes full lighting
through the ray tracing path and disables incompatible screen-space or baked GI
contributions for that view.

A baked `LightmapGI` does not disable RTGI. When `Hybrid RTGI` or `Full Path
Tracing` composites, RTGI owns the diffuse indirect for the view, and the opaque
pass skips the baked lightmap's contribution to ambient light so the two do not
add up to a doubled result. Direct lighting, emission, and the environment
specular path keep their raster values.

`VoxelGI` and `SDFGI` follow the same ownership rule. A culled-in `VoxelGI`
instance, or SDFGI enabled alongside Hybrid, used to silently veto the whole
RTGI composite while every RT dispatch kept running and was thrown away each
frame. When a composite-active RTGI mode is set, VoxelGI is now excluded from
the frame and SDFGI is disabled, each with a one-shot warning, so the composite
the frame already paid for is the one on screen. `Reflections RT Only` does not
composite and still coexists with both raster GI providers, unchanged. One cost
is deferred: the cull-side `voxel_gi_update` work, the re-voxelization of
probes whose dynamic objects moved, still runs while a VoxelGI node is in the
tree; the render-side voxel work is skipped.

Fog is a camera effect in the ray-traced modes, applied once where the raster
path applies it. The RT fog helper used to evaluate only the exponential model,
so a scene using depth fog (begin/end distances with a curve) rendered
exponential fog at the same density value. The two models read the density
control very differently: depth fog at density `1.0` means fully fogged at the
end distance, while exponential fog at density `1.0` saturates within a few
meters, so such a scene went from raster haze to a white blowout the moment
RTGI turned on. The helper now branches on the depth-fog specialization flag
and applies the raster formula, and a depth-fog scene at density `1.0` matches
the raster haze.

Where fog is applied matters as much as which model. The probe traces used to
bake camera-relative fog into the world radiance caches, so every consumer of a
cached probe received fog computed for someone else's view segment, and each
bounce segment compounded it. The probe dispatches now trace fog-free, the
path-traced primary applies camera-segment fog once at shade time, and the
composite attenuates the indirect contribution by the camera transmittance, so
fog touches the image exactly once. On the fog-parity corridor scene, the
maximum relative error against the raster reference dropped from `0.68` to
`0.049` for Full Path Tracing and from `0.36` to `0.096` for Hybrid RTGI. The
deep-path reference oracle keeps per-segment fog participation along the full
path, since it exists to integrate the transport rather than approximate it.

3D MSAA runs together with RTGI. The raster passes anti-alias primary visibility
at the sample count the viewport sets, while the ray-traced GI and its denoiser
run at a single sample, the same as they do without MSAA. The material-guide
prepass that feeds the GI resolve renders into its own single-sample depth
attachment when MSAA is on, so it stays consistent with its single-sample guide
buffers. MSAA only sharpens raster primary-visibility edges here; it does not
change the ray-traced GI itself.

The path-traced Forward+ flow denoises the opaque RT result before transparent
rendering. Transparent particles and other alpha overlay instances are kept out
of the TLAS and rendered as the normal Forward+ transparent pass after RT
denoise, so they remain visible without contributing to RT GI, shadows, or
reflections. This avoids particle-driven TLAS spikes and black RT speckle from
billboard particle geometry.

Alpha-scissor emissive materials participate in explicit emissive next-event
estimation. They were excluded from the emissive candidate list and contributed
only when a path happened to hit them, so a cut-out emitter such as a foliage
card or a grate-shaped light lit the scene more dimly and more noisily than the
same emitter without the scissor. The candidate texel sampler now zeroes
cut-out texels with the same alpha chain the any-hit shader uses, so the
explicit and BSDF sampling strategies integrate the same emission domain.

Full Path Tracing shades the directly visible surface with a next-event-estimation
direct term. The shadow and area-light samples redraw each frame, but the direct
lighting is no longer an independent one-sample estimate: a screen-space reservoir
(ReSTIR) reuses the surface's accepted light samples from previous frames and from
neighboring pixels, so a still surface keeps a converged direct term instead of
re-rolling its soft shadow from scratch. After the primary dispatch the renderer
copies the reservoir and history textures the reuse reads into their previous-frame
slots, and the temporal combine scales the carried reservoir weight by the same cap
ratio it applies to the accumulated sample count. Without that scaling the carried
weight grows while the sample count is held at its cap, which over-brightens the
result frame over frame; with it the reuse reduces noise while the average energy
stays put. The reuse is keyed on stable light ids and a once-per-frame history
signature, so a slowly orbiting camera that re-sorts the light list, or a smoothly
moving emissive mesh, no longer counts as a light-set change and clears the history.
A real light-set change (a light switched on or off, or one moving past the per-light
delta thresholds) still resets the affected history, as it should. Reuse runs in Full
Path Tracing only; Hybrid traces no screen rays, and the probe rays that feed the
world and screen-probe caches are kept out of the screen reservoir entirely.

Directional lights (the sun) are evaluated deterministically, with their own shadow
ray, outside the reservoir. A directional's importance weight has no visibility term,
so a bright but fully occluded sun used to win nearly every reservoir candidate draw
and starve the local lights that actually reach the surface, leaving the shadowed
interior noisy. Pulling the sun out of the candidate set fixes that: the reservoir
samples only the positional lights, and the sun contributes its own deterministic
term every frame. The screen primary's soft-shadow cone samples are drawn from the
blue-noise texture with a per-frame golden-ratio rotation rather than white noise,
which gives a better screen-space sample distribution and lowers single-sample shadow
sparkle (Cornell temporal sparkle per megapixel fell from 128.9 to 101.7 at 1080p on
an RTX 4080 SUPER, with the average energy unchanged).

The sun's angular size follows the rasterizer convention. A directional light's angular
distance is its full angular diameter, and the rasterizer builds its soft-shadow penumbra
from that whole value. The path tracer used half of it, so its sun shadows came out about
half as soft; the cone half-angle now uses the full diameter, so the path-traced penumbra
matches the rasterizer's width. A large sun spreads its penumbra across many pixels, where one
shadow sample per frame leaves the band noisy, so the screen primary takes several stratified
cone samples for it. The count scales with the sun's solid angle and is bounded by a per-preset
budget, `rendering/rtgi/direct_light/{performance,balanced,production}/shadow_samples` (1, 2,
and 4), and a small sun stays at one sample. On a directional penumbra-ramp scene the balanced
two-sample setting cut the soft-edge noise about 40 percent with the penumbra width unchanged.
The extra samples are taken only on the path-traced screen primary; the probe and deep paths
keep a single sample.

A light's indirect energy multiplier scales its contribution to the path-traced global
illumination. The probe gather that fills the world and screen-probe caches treats the light
it hits as indirect, the same way the multiplier already works for VoxelGI and SDFGI, so
setting a light's indirect energy below or above one dims or lifts its bounced contribution
without changing the directly visible shading. The camera's direct view of a surface is never
scaled by it.

The number of reservoir candidates the screen primary samples each frame is the
direct-light shadow-ray budget. It is a hidden per-preset project setting,
`rendering/rtgi/direct_light/{performance,balanced,production}/ris_candidates`, that
follows the active RTGI quality preset (Custom maps to balanced, like the other
RTGI tier settings). The shipped defaults are 4 for performance, 8 for balanced, and
16 for production; on the 24-light reservoir stress scene balanced is about 15 percent
and performance about 22 percent faster per GPU frame than the full 16 count, while
production keeps the full sample count. The deterministic regime (a surface that sees at
most twelve positional lights evaluates them all directly) and indirect bounces are not
budgeted by this setting. A `PathtracingDebugMode` view, Direct Light Regime, colors
each pixel by which regime it is in (a green ramp for the deterministic per-light
evaluation, a red-to-yellow ramp for reservoir sampling) with brightness tracking the
reuse depth, so the over-twelve-light seam and dead reuse are both visible.

When no temporal upscaler is active, a dedicated pass accumulates that primary color
before the composite. It reprojects the previous accumulated value with the RT velocity
guide, accepts it only on a same-surface depth and normal test read from the current
guides, clamps fireflies against the local neighborhood, and blends with a
sample-counted weight. On a still camera this brought the frame-to-frame luminance
change of the path-traced primary down by about four times in our test scene. Before
blending, it also rectifies the reprojected history into the current 3x3 neighborhood
color box. This is what removes moving-light trails: when a light leaves a surface, the
surface's stale bright history is clipped back toward the now-dark neighborhood and decays
the same frame, instead of latching once the surface darkens. Grazing sub-native sampling
misses are already pulled toward the lit neighborhood by the firefly clamp, so no separate
dark-drop rule is needed. The pass is limited to the no-upscaler path. FSR2 runs its own jittered temporal accumulation. That accumulation settles
a deterministic raster primary, but a pre-averaged path-traced primary works against it, so
under those upscalers the composite keeps the raw path-traced color and lets them accumulate it.
A reduced-strength pre-average was measured under FSR2 and left the frame-to-frame delta
unchanged, so the raw color is kept. In practice the path-traced primary stays clean with no
upscaler and native TAA, but boils under FSR2 specifically, whose feed-forward
accumulation cannot lock the stochastic base, so Hybrid RTGI is the better mode under FSR2.

Native TAA runs after that pass, and left alone it would put the boil back: its
neighborhood clamp is built for a deterministic raster primary and cannot lock a
path-traced one, so every frame it pulls the stabilized primary back toward the
current noisy sample. On the Full Path Tracing path TAA now reads the stabilizer's
per-pixel convergence confidence and leans on history for pixels that have converged,
holding the calm value instead of re-shaking it. It relaxes the clamp only where a
history-agreement test confirms the reprojected history still matches the current
frame, and that test measures the history against the current neighborhood mean and
minimum, not just the box-clamped value. That distinction matters: a wide neighborhood
box, such as a bright edge moving over a dark background or a region that is still
converging, would otherwise read as agreement and hold a stale value, so checking the
raw mean and minimum keeps the ordinary clamp there and leaves no trail. On a still,
converged frame the result is the calm the clamp would otherwise undo. The behavior is
limited to Full Path Tracing without a vendor upscaler; Hybrid RTGI, the reference
oracle, and every other viewport keep the stock TAA path unchanged. Its thresholds are
tunable through the FPT_TAA_CHANGE_GAIN, FPT_TAA_DT_GAIN, FPT_TAA_RELAX_FLOOR,
FPT_TAA_AGREE_LO, FPT_TAA_AGREE_HI, and FPT_TAA_NORELAX environment variables.

The screen-probe stack reconstructs world positions from device depth, and its
consumers used to invert the raw GL-convention projection while reading the
depth-corrected reverse-Z values the engine actually writes. Every
reconstructed position collapsed to roughly twice the near plane: gather rays
originated at the camera instead of the receiving surface, world-cache queries
all read one near-camera cell, and the depth-rejection gates compared positions
that were never apart, so they never fired. The probe placement, gather,
resolve, and debug consumers now invert the depth-corrected projection that the
SceneData convention provides, for the current and previous frame with their
own jitters. On the Cornell scene this took fireflies from 982 and 417 per
megapixel down to zero in both detection regions and cut the path-traced
sparkle maximum from 692 to 129, and it restored real contact occlusion. The
Screen Probe Gather pass had been degenerately cheap because every ray started
at the camera; its cost rose from 0.08 ms to a real 2.0 ms at 1080p on a
4080-class GPU, with the other RTGI passes net cheaper.

A freshly disoccluded screen probe has no accumulated history, so starting it
from a single newly traced ray, which is high variance, left transient blotches
in the resolved GI for a handful of frames until the probe filled in. Each such
probe is now seeded from the World Radiance Cache, the coarse persistent clipmap
the gather already maintains, so it starts at a smooth, plausible value and
sharpens into its own traced detail over the next frames rather than rising up
out of noise. This is the same fallback Lumen uses when a screen probe has no
reprojected history. The seed strength is a per-preset Screen Probe Gather
setting, wrc_seed_samples, with an RTGI_SPG_WRC_SEED_SAMPLES environment override.

The seed narrows the blotch but cannot remove it on a hard camera cut, because a
cut turns everything cold at once. The screen probes reset, the World Radiance
Cache recenters and re-traces at the new view, and the screen-space history is
rejected wholesale. With no trustworthy prior anywhere, the composite hides the
indirect contribution outright and fades it in from zero as the disoccluded
pixels converge. A contribution of zero cannot show a wrong value, so the biased
cold-start estimate stays invisible while it settles, at the cost of the
indirect light easing in over a fraction of a second rather than appearing at
once.

The reveal is gated on convergence, not on time. A wall-clock window counts
frames in which the pixel still shows an unverified prior: after a cut the
seeded probes hold coarse cache data until their own rays land, and at the
production trace rates each probe texel is re-traced only about every ten
frames, so a timed reveal opens while the prior is still on screen and the
residue shows up as soft, cell-sized light spots in dark areas. Instead, the
integrate pass writes a source quality with each pixel: the cosine-weighted mean
of the gathered probe texels' own sample counts, or a constant low value when
the pixel fell back to the cache directly. The temporal pass advances the
pixel's accumulated sample count by that quality rather than by one per frame,
and the composite reveals on the accumulated count. The gate opens as traced
rays replace the prior and behaves the same at any frame rate. A converged pixel
pays nothing, since full quality advances the count exactly as a flat
once-per-frame advance would. The hide is enabled by the per-preset GI-resolve
setting cold_start_fade_time, with an RTGI_GI_FADE_TIME environment override;
zero disables it. It is a read-time display gate only, so it never feeds back
into the stored history.

One reprojection detail matters on cuts. Static geometry writes no motion
vectors, so the resolve reprojects it through the previous frame's camera, and a
surface that lands behind that camera has no history at all and is treated as a
true disocclusion. Falling back to the pixel's own screen position instead would
compare the surface with itself, accept whatever the previous frame stored at
that coordinate, and let the old view's lighting bleed through the gate.

RTGI writes noisy radiance, depth, velocity, normal/roughness,
albedo/metalness, view-Z, hit-distance, validity, and history ID guides at the
scaled RT texture size. Hybrid RTGI maps those scaled GI pixels back onto the
full-resolution raster depth, normal/roughness, and color guide textures before
tracing. Scaled Full Path Tracing now runs an opaque full-resolution material
guide prepass before ray tracing. That prepass stores depth, albedo, normal,
ORM, emission, and view-Z guide outputs; reconstruction currently uses the
full-resolution depth plus material normal and true ORM roughness so it no
longer depends on the packed raster normal/roughness alpha used by the regular
Forward+ guide buffer. The RTGI denoiser consumes the scaled guides directly on
the GPU and reuses previous-frame radiance history. Fast movement and
disocclusion favor current-frame samples through guide-based rejection.

When split-signal ASVFG is active, the pre-ASVFG diffuse radiance cache runs
before diffuse denoising. It demodulates the primary surface albedo out of the
raw diffuse signal, stores a bounded screen-space lighting history, and
remodulates that filtered lighting for the current pixel before ASVFG sees the
diffuse layer. Reuse is also motion-gated so the cache contributes less during
high-velocity reprojection. Each cache cell stores four receiver-surface slots
instead of one representative texel. The cache identity is a coarse
camera-stable receiver hash built from quantized world position, normal,
roughness, and albedo proxy; the strict ray-hit history ID is still used for
RT TAA/reprojection rejection. Candidate cache entries must still pass
persistent receiver identity, previous-validity, normal, depth, hit-distance,
radiance-delta, variance, age, and confidence checks. The cache also measures
luminance coherence across the accepted neighborhood and reduces reuse when
otherwise valid cache entries disagree. Each updated cache cell now integrates a
small guide-coherent current-frame receiver neighborhood before temporal reuse,
so the cache behaves more like a small screen-space receiver-probe gather than
a single chosen low-resolution texel. This is still not Lumen's full Screen
Probe Gather/surface-card/world-radiance-cache chain, but it addresses the main
divergence in the current scaled Path Tracing path: sparse GI reuse now has
multiple surface identities per cell before ASVFG and full-resolution
reconstruction run. The weighted reconstruction lets stable surfaces reuse
nearby diffuse lighting, while disoccluded, internally inconsistent,
high-motion, or high-risk surfaces fall back toward the current sample before
ASVFG does the final temporal/spatial cleanup.
The cache also writes a reconstruction confidence mask derived from its hit
state, accepted-cache stability, confidence, variance, age, rejection, and the
original RT signal-risk channels. Full Path Tracing uses that cache-authored
mask for diffuse split-signal reconstruction only, so stable cached diffuse
lighting can stay sharper while cache misses, disocclusions, or rejected
regions get wider full-resolution smoothing. Specular reconstruction keeps the
original RT signal confidence.
High source-signal risk no longer makes an otherwise valid receiver sample
unusable for the diffuse cache. Instead, valid guided samples enter with a
reduced confidence floor so noisy GI can still be accumulated by the cache,
while variance, radiance-delta, age, and rejection gates decide how much of that
history is safe to reuse.
Diffuse cache reconstruction also has a conservative same-surface fallback for
Path Tracing receiver coverage. Exact receiver-surface matches are preferred,
but mature high-confidence cache entries can contribute across adjacent
receiver-ID differences when normal, depth, hit-distance, variance, age, and
radiance-delta checks all agree. The fallback is deliberately downweighted so it
can bridge stable quantization/history fragmentation without becoming a broad
cross-surface blur.

The receiver diffuse cache is also visible to Full Path Tracing secondary
diffuse hits before the next stochastic bounce is sampled. A rough secondary
surface projects its hit point into the previous frame, searches the matching
screen-space receiver-cache cell and neighbors, and only accepts exact
receiver-surface IDs that pass normal, previous-view depth, camera-distance,
variance, age, and confidence tests. The accepted receiver cache is the
high-detail, screen-visible layer in the handoff.

The directional screen-probe atlas participates in that handoff too. When the
receiver cache has no viable local hit, a rough secondary diffuse surface can
sample the previous-frame `4x4`/16-direction screen-probe atlas at the
reprojected hit point. Probe bins are accepted only when their receiver
identity, guide normal, previous depth, direction hemisphere, radiance
confidence, SPG history quality, current material roughness, and reconstructed
previous-frame receiver plane all agree. The ray-side query reconstructs an
approximate previous-frame probe-plane position from the SPG probe UV and
stored view depth, then rejects samples whose normal plane is too far from the
current hit. The contribution is capped well below a strong receiver-cache hit,
but it gives the path tracer a directional screen-visible fallback before it
asks the lower-frequency world cache. This mirrors Lumen's broad ordering:
screen-visible radiance first, then a more persistent scene/cache fallback.

If the receiver lookup misses or returns only a weak result, the same
secondary-hit cache decision can query STRC as a lower-frequency world-radiance
fallback. This is closer to the Lumen-style hierarchy: screen-visible reuse is
preferred for local detail, while a camera-centered world cache can provide
more stable offscreen or disoccluded lighting. STRC fallback is deliberately
capped, never terminates the path by itself, and only reduces remaining path
throughput when the sampled world cache has nonblack radiance. A confidence-only
or black STRC sample keeps tracing instead of consuming the bounce.
When this fallback is enabled internally for Path Tracing, it uses a compact
`16^3` three-cascade cache with a higher probe-ray update budget than the
artist-facing default. This gives the world cache a practical warmup cycle for
camera motion tests while keeping it lower-frequency than the screen-visible
receiver cache. STRC probe confidence now represents usable radiance confidence:
near-black radiance does not keep a probe texel alive as a lighting fallback.
Probe updates also classify their radiance provenance. Direct light, emissive,
sky, indirect fallback, dynamic-hit state, and black/no-source updates are
carried into the STRC cache metadata. The cache resolve preserves previous
lighting when a new update has no valid source or only black radiance, and STRC
sampling weights entries by that source quality before they can contribute to a
secondary diffuse hit.
STRC updates are no longer a purely blind atlas sweep. Each probe ray now writes
the exact atlas texel it traced into the result buffer, which lets the raygen
stage spend part of the update budget on camera-visible, camera-forward probe
directions while the resolve stage still writes the correct cache texel. The
remaining budget keeps the old cascade-weighted background sweep so offscreen
coverage continues to age forward. This is closer to a Lumen-style world
radiance-cache update budget: visible cache cells can refresh sooner during
camera motion, but the world cache remains a coarse prior behind the
screen-visible receiver cache.

STRC also feeds the first upstream sampling hook toward a Lumen-style
history-guided final gather. For single-sample Full Path Tracing, rough
non-metal primary diffuse continuations use a screen-probe-like direction
sequence when RTGI is scaled below native resolution. The current base probe
spacing is eight scaled-RT pixels and the current angular atlas uses `4x4`
direction bins; these are separate internal constants so probe density can be
changed independently from directional resolution. Nearby pixels trace a shared
probe-cell distribution instead of unrelated random BRDF directions. The same
continuation can then query the previous STRC atlas and nudge the selected
direction toward the strongest valid normal-oriented world-radiance candidate.
This happens before ASVFG and reconstruction see the sample, so the denoiser
receives a less random rough diffuse path instead of only filtering a random ray
after the fact. The STRC blend is small, layer-masked by the STRC visual-layer
settings, and only uses a compact candidate set around the receiver normal to
avoid turning every primary hit into a full probe scan.

The diffuse cache now records that primary rough-diffuse direction into an
internal directional screen-probe gather atlas. Each scaled-RT probe stores a
`4x4` angular tile with demodulated diffuse lighting, guide normal, view-depth,
receiver identity, confidence, and short temporal reuse. A compact SPG stats
atlas travels with it: age, local support, radiance coherence, and depth-plane
quality. A second SPG visibility atlas stores representative per-direction hit
distance, hit-distance coherence, receiver view-depth, and support. Probe-bin
construction first elects a surface anchor and downweights mixed
normal/depth/roughness support, so a single bin is less likely to average
different surfaces just because they share a screen probe cell and direction
bin. During diffuse reconstruction the full-resolution pixel gathers nearby
probe cells, weights bins by surface compatibility, hemisphere direction, SPG
stats quality, and the visibility atlas. The visibility term is intentionally a
soft confidence-weighted downweight rather than a hard reject, because
single-frame hit distance can be noisy until adaptive/refined probes exist.
The full-resolution SPG rejection debug view records the dominant failure mode
for the current reconstruction pixel, which is the map used to decide where a
future adaptive/refined probe layer should spend extra samples. A first
deterministic refined layer is now present: risky base probes get a stable `2x2`
subprobe layout with a lower `2x2` angular tile, reusing the same atlas texel
budget as the base `4x4` angular tile. Refined probes are gated by the
refinement mask, carry the same radiance/meta/stats/visibility/history payloads,
and blend conservatively before base SPG so a partial refined layer cannot
dominate stable receiver-cache or base-SPG history. The ray-side secondary
diffuse query now sees the same refined atlas: receiver-cache hits still win,
but risky receiver misses can query the refined SPG hierarchy before falling
back to the base SPG or STRC. This makes refinement part of the Path Tracing GI
reuse path instead of only a final reconstruction detail layer.
A dedicated `rt_secondary_cache_source` debug texture now records which
ray-side cache layer actually reduced Path Tracing throughput. It is a
provenance view for accepted secondary diffuse reuse, separate from the
full-resolution reconstruction views: receiver-cache hits, STRC fallback,
base-SPG reuse, refined-SPG reuse, surface-cache reuse, and current-screen trace reuse can be
distinguished without inferring them from final color. The current view does
not encode failed lookup reasons; those are written to a separate
`rt_secondary_cache_rejection` debug texture so accepted source attribution and
miss attribution stay semantically separate while both raytracing outputs remain
write-only for RD graph compatibility.
Surface-cache lookup also has its own `rt_secondary_cache_surface` diagnostic.
That view reports whether a strict surface-page query was accepted, skipped
because another cache layer won first, or rejected for no key, no page,
collision/id mismatch, stale data, low confidence/support/variance, normal
mismatch, dynamic ineligibility, weak radiance, or weak quality. The same view
also encodes the accepted or early-source class in RGB-captured diagnostics, so
the harness can report whether the query path used receiver, base SPG, refined
SPG, visible-current pages, STRC, or a surface page without relying on final
color.
This is still not Lumen's full Screen Probe
Gather/surface-card/world-radiance-cache chain: the atlas is screen-space,
previous-frame only for ray-side reuse, and intentionally confidence-capped. It
does add the missing directional screen-probe layer so half-resolution Path
Tracing is no longer reconstructed only from scalar receiver-cache samples.
The first screen-trace fallback layer is now present for rough diffuse
continuations. After the BRDF direction is sampled, Path Tracing raygen can
march that direction through the current raster depth/normal guides. If the ray
intersects a current-screen surface, the shader queries the receiver/STRC cache
at that hit point, adds a capped cached diffuse continuation, and leaves the
remaining throughput to keep tracing normally. This follows Lumen's ordering in
spirit: current screen data is tried before relying entirely on the lower-detail
world/cache representation, but it remains a conservative assist rather than a
hard screen-space replacement.

The first surface-cache bridge is now present for Full Path Tracing. Primary
and secondary hits generate a coarse camera-stable world-space surface-page key
from static geometry history, quantized world position, normal, and material
class. Ray tracing samples the previous surface atlas between refined/base SPG
and STRC, while secondary misses write per-RT-pixel demand feedback containing
the demanded surface key, guide normal/roughness, demodulated direct/emissive
lighting, conservative sky-visible seeds, and STRC-seeded low-frequency priors
with direct/emissive/sky provenance. `RTGIDiffuseCache` consumes
that feedback into a small associative hashed surface atlas using atomic
claims and deterministic budget gating. The `surface_feedback` debug view
reports which feedback pixels were selected, starved by budget, skipped by an
atomic claim, rejected for collision/no-radiance/low-confidence/low-quality, or
classified as invalid, dynamic/ineligible, or stale-refresh updates. Feedback
also carries a private source class for current-hit direct lighting, explicit
emissive lighting, sky-visible seeding, STRC-prior seeding, or mixed current-hit
radiance. The
consumer writes that provenance into surface-page stats and the ray-side
surface lookup uses it as a confidence cap, so STRC pages can seed low-frequency
reuse without being trusted like receiver-promoted or direct/emissive current
surface pages. Visible receiver-cache promotion derives its normal/roughness
guide from the representative full-resolution receiver pixel for the cache slot,
then can promote receiver, refined screen-probe, base screen-probe, or a
conservatively gated current visible diffuse sample into the surface page with
matching provenance. The current visible source is accepted only when the guide
is valid, the current strict surface key still matches the demanded page key,
signal risk is low, receiver variance is controlled, and the current
demodulated lighting agrees with receiver history. This is
still not a full Lumen Surface Cache: there are no explicit cards, no compacted
page queue, no exact SSBO page counters, and no material page captures. The
public STRC controls remain available, but Full Path Tracing can also run an
internal fallback path when split-signal denoising and the diffuse receiver
cache are active, so the receiver-cache chain does not drop to a silent zero
just because previous-frame screen projection failed.
Current-screen secondary hits now carry the strict surface key from the hit
pixel into surface-cache lookup and can write bounded receiver/STRC/SPG-sourced
surface feedback for future frames when that key is valid. The SPG refinement
mask also reads secondary-cache source and rejection diagnostics, so screen
trace accepts, screen misses, weak screen hits, weak SPG hits, and weak surface
hits can request refined probes instead of relying only on local variance.

When `rtgi_resolution_scale` is below `1.0`, RTGI no longer relies on the
generic color copy or additive blend to hide the lower-resolution signal. A
dedicated two-stage RTGI reconstruction path first upsamples into
`rt_reconstructed_temp`, then refines the result into the full-resolution
`rt_reconstructed` texture before Full Path Tracing copy-out or Hybrid RTGI
additive composition. Hybrid and Reflections modes guide both stages with
full-resolution raster depth and normal/roughness textures. Scaled Full Path
Tracing uses its material guide prepass for full-resolution depth, normal, and
roughness, so the final reconstruction is anchored to full-resolution primary
visibility and material roughness instead of scaled RT primary-hit guides.
Composited final radiance is still never albedo-demodulated, because it mixes
direct, emissive, indirect, and specular energy. When split-signal ASVFG is
active, scaled Full Path Tracing reconstructs diffuse and specular lighting
separately, then composites the full-resolution split outputs with the
full-resolution material guides. That keeps material-space transfer on a
diffuse/specular lighting boundary instead of trying to reinterpret the final
color after composition.
The reconstruction path is
confidence-aware: high-history, low-variance pixels use tighter guide
filtering, while rejected, reactive, low-history, or high-variance pixels use
wider smoothing. The full-resolution refinement pass only smooths
low-confidence/unstable regions, and samples the RT signal confidence buffer so
valid low-risk direct, emissive, and specular energy keeps more of its local
intensity instead of being blurred into surrounding GI.
For cached diffuse GI, that confidence buffer is replaced with the cache's
post-reconstruction confidence handoff, which lets the low-resolution
radiance/probe layer and the full-resolution reconstruction layer make the same
reliability decision instead of disagreeing about which pixels are stable.
The first stage uses a bilinear low-resolution seed instead of a nearest-texel
anchor, which reduces persistent 2x2 block signatures before the guide filter
and temporal pass refine the result. Reconstruction diagnostics such as
instability and signal confidence are sampled bilinearly as well, so filter
radius, energy preservation, and the post-reconstruction reactivity mask change
smoothly across full-resolution pixels instead of stepping at low-resolution
texel boundaries. If a full-resolution guide is unavailable, reconstruction can
still fall back to bilinear source-space depth/normal/roughness guides. That
source guide interpolation is edge-aware: strong depth, normal, or roughness
discontinuities fall back toward the nearest stable source guide and raise the
post-reconstruction reactivity mask so temporal accumulation does not smear
synthetic guides across geometry edges. For scaled Full Path Tracing, raygen also
keeps primary camera-ray jitter in final-pixel units instead of scaled-RT-pixel
units, and makes that primary-visibility jitter screen-stable for scaled Full
Path Tracing. This keeps stochastic primary samples closer to the source
positions that reconstruction assumes, reducing low-resolution crawl while
preserving the full-resolution behavior when `rtgi_resolution_scale` is `1.0`.
For single-sample Full Path Tracing, the first primary surface also reseeds
lighting and BRDF sampling from stable geometry, world-position, UV, normal, and
material keys plus the frame index. This keeps sampling temporally varying for
denoising while avoiding purely screen-pixel-seeded noise crawling across the
same wall during camera motion. Single-sample Full Path Tracing also treats
rough non-metal BRDF continuation as a diffuse-GI gather path instead of
roulette-selecting the specular lobe on those surfaces. Direct specular lighting
and glossy/metallic continuation stay unchanged, but broad rough GI avoids rare
high-throughput specular continuation samples that are hard for temporal history
to hide at `0.5` scale. At native RTGI resolution that rough diffuse
continuation uses a 16-step low-discrepancy temporal direction sequence with a
stable per-surface rotation. At scaled RTGI resolution, the sequence becomes
screen-probe-like: each `4x4` scaled-RT pixel cell covers the 16 directions in
the current frame and rotates that set over time. This makes the visible
diffuse gather behave more like a persistent screen radiance cache than
independent per-frame random BRDF picks. When valid STRC world radiance is
available for the receiver layer, that sequence can be slightly guided toward
the previous frame's strongest normal-oriented world radiance candidate, which
is the current lightweight stand-in for Lumen's previous-frame lighting-guided
ray selection. Raygen also expands each scaled RT velocity
sample over the matching full-resolution pixel footprint. The
post-reconstruction TAA pass therefore sees coherent full-resolution motion
vectors at `rtgi_resolution_scale` values below `1.0` instead of receiving valid
vectors only in the scaled RT texture's top-left footprint. Reconstruction also
writes a full-resolution `rt_reconstructed_reactivity` mask,
which is used by a dedicated post-reconstruction RTGI TAA resolve. That resolve runs in
separate history contexts for Full Path Tracing, Hybrid RTGI, and
Reflections-only RT so full-resolution GI history is stabilized before copy-out
or additive composition without sharing state with the normal viewport TAA.
The reconstruction step also builds full-resolution history-validity and
history-ID textures from the low-resolution RT guides. These are conservative:
the nearest low-resolution identity is used only when the local source support
agrees, and ambiguous low-resolution edge footprints are invalidated. The
post-reconstruction TAA resolve consumes those full-resolution gates so
reconstructed GI history cannot freely accumulate across surface-ID changes just
because velocity/depth reprojection still lands on screen.
Hybrid/Reflections RTGI also requests the raster motion-vector pass for this
internal stabilization even when the user-facing viewport TAA option is off, so
the GI reconstruction does not rely on final-frame TAA to hide low-resolution
lighting.

After RTGI denoising, the renderer also builds an internal `rt_taa_reactivity`
mask from the denoiser's diagnostics: light-change reactivity, rejection,
low history length, high variance, invalid history, and high motion. This mask
is not exposed as a project setting. It is consumed by the internal TAA resolve
to force more current-frame contribution only where RTGI history is unreliable,
so high TAA history weights do less damage during camera movement.

Full Path Tracing keeps the existing post-denoise RT TAA placement. Transparent
raster overlays are still rendered after RT TAA. Hybrid RTGI now runs a
dedicated internal TAA resolve on the denoised `RB_TEX_RAYTRACING` texture
before that texture is additively blended into the raster frame. These RTGI TAA
resolves are driven by the RTGI denoiser path rather than the public viewport
TAA toggle, and use a separate `rtgi_hybrid_taa` history context so Hybrid RTGI
cannot share history with the full path-tracing resolve or the normal viewport
TAA context. The RTGI TAA resolves also keep their previous-frame
validity/history-ID buffers separate
from the RTGI denoiser's previous-frame buffers, so denoiser history updates do
not make the later TAA validation compare against same-frame IDs.

The `ASVFG (Experimental)` option uses the dedicated `RTGIDenoise` RD effect for
both RTGI modes. It runs temporal reprojection, guided
stabilization, light-change reactivity, luminance moments, variance
prefiltering, and edge-aware atrous filtering before the path-traced output is
written back to the viewport. Newly visible geometry, newly loaded
materials, and geometry that has just become RT-ready therefore start from fresh
samples instead of borrowing stale accumulated lighting.

The screen-space temporal reprojection reads the engine motion-vector buffer in its
native UV, previous-minus-current convention and scales it to pixels, so the indirect
lighting follows the surface as the camera or an object moves. Builds before this read
the buffer as if it were already in pixels, so the per-frame offset rounded to zero and
the low-frequency indirect smeared across the screen under motion.

The multi-signal Internal Signal Decomposition denoiser was removed: it
rendered identically to ASVFG, so its serialized value and the old vendor
selections normalize to ASVFG on load. Switching between ASVFG, the None
bypass, and the script-only Reactive value clears the inactive denoiser
resources so stale histories are not reused across modes.

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

## Diagnostics

The engine reports once per session when RTGI output is discarded or
substituted, instead of staying silent:

- `Reflections RT Only` warns that it produces no composited RTGI output; the
  frame keeps the raster GI.
- `Full Scene Path-Traced GI` under FSR 2 warns that the path-traced primary
  is not temporally stabilized there and recommends `Hybrid RTGI`.
- With `--verbose`, the `Custom` quality preset notes that the hidden
  `rendering/rtgi/*` pipeline settings resolve to the `Balanced` tier.
- With `--verbose`, `rtgi_disable_in_editor` notes that the editor viewport
  previews the raster fallback, so the preview differs from the running
  project.

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
- `servers/rendering/renderer_rd/effects/rtgi_gi_resolve.*` and
  `servers/rendering/renderer_rd/effects/rtgi_fpt_stabilize.*`
  - House the shipping denoise machinery: the resolve runs the temporal
    accumulation and the a-trous spatial pass on the probe-composited GI, and
    the stabilizer temporally accumulates the path-traced primary when no
    temporal upscaler is active. Selecting the None denoiser bypasses the
    stabilizer and the spatial pass while accumulation and compositing keep
    running.
  - The composite can also write a GI-confidence reactive mask for FSR 2 and
    XeSS when the Reactive denoiser is selected from script.
- `servers/rendering/renderer_rd/effects/taa.*`
  - Remains the normal viewport TAA path and fallback temporal resolve.
    RTGI-only resolves can consume the internal reactivity mask before the
    final viewport TAA path runs.
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
  - Rejects reprojected history when RT validity or history ID checks fail in
    fallback RT temporal resolves.
  - Uses the internal RTGI reactivity mask to raise current-frame contribution
    in unreliable RTGI history regions.
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

The default shipped RTGI denoiser is ASVFG for interactive motion stability.
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
region luminance, and high-frequency texture detail. A committed many-light scene
(24 shadowed omnis with a moving emissive and a scripted light toggle) measures the
direct-light reservoir reuse: an orbit-phase whole-image delta that rises if history
starts resetting per frame again, a frozen-frame floor that climbs if reuse breaks,
and a toggle-recovery count.

The harness and Euphorica capture script also consume the `source_candidate`,
`source_history`, `source_temporal_delta`, `source_rejection`,
`secondary_cache_source`, and `secondary_cache_rejection` debug views when
requested. The reported
`rtgi_source_candidate_*` metrics expose
source-class coverage,
confidence, candidate weight percentiles, contribution percentiles, temporal
eligible fraction, class agreement, exact source-key reuse, direct dominant-key
accepted/rejected history rates, and contribution-delta percentiles so
many-light instability can be separated from diffuse-cache and denoiser
behavior. Direct lighting attribution stores one dominant analytic source key
alongside the aggregate direct contribution; with reservoir reuse live, the
per-frame reuse is keyed on the full reservoir state and stable light ids, while
this dominant-key record stays a coarse diagnostic for dominant-source stability.
The `rtgi_secondary_cache_*` metrics report accepted ray-side cache source
coverage, selected source fractions, accepted weight, and whether accepted reuse
came from a direct secondary-hit query or the current-screen trace shortcut.
The `rtgi_secondary_cache_rejection_*` metrics report ray-side miss coverage,
dominant rejection buckets, query-family split, and weak-source detail so
surface-cache misses can be separated from screen-trace misses before adding
more reuse.

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

## Geometric normals for grazing direct lighting (Full Path Tracing fast mode)

Full Path Tracing fast mode evaluates the primary hit's direct lighting with the
surface normal from the raster G-buffer, which already carries the tangent-space
normal map. On a normal-mapped surface lit at a grazing angle, the mapped normal
can dip below the light's horizon while the underlying geometry still faces the
light. A plain cosine test then zeroes that texel's direct lighting, which reads
as hard black veins along the normal-map relief, such as the mortar lines on a
brick wall. The deep-path reference does not show this, because it gathers the
open hemisphere over several bounces.

The fast path now also reads the relief-free geometric normal, which the
material-guide prepass already produces. Where the geometry faces the light but
the mapped normal does not, it bends the shading normal back toward the geometric
normal by just enough to give the texel a small amount of direct light, scaled by
the geometric incidence so it never exceeds what the flat surface would receive.
Texels whose geometry truly faces away stay dark. This keeps the normal-map
relief while removing the black veins, and it matches the deep-path reference.
The change touches only the fast path's primary direct lighting; the deep-path
reference, Hybrid, and Reflections modes are unchanged.

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
- Full Path Tracing under a temporal upscaler (FSR2) still shows
  residual temporal instability. The upscaler does not settle onto the per-frame
  stochastic path-traced primary the way it settles onto a deterministic raster
  primary, so the no-upscaler primary accumulator is gated off under those upscalers.
  The no-upscaler path is stable, and a fix for the upscaler path is tracked for a
  later change.
