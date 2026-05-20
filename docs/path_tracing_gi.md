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
- `rtgi_denoiser`
  - `Auto`
  - `Internal`
  - `NVIDIA`
  - `AMD`
  - `Intel`
  - `Off`
- RTGI debug draw modes for lighting, rays/noise, TLAS/instance coverage, and
  denoiser input/output.

## Rendering Behavior

RTGI is a view-time override. Existing SDFGI, VoxelGI, LightmapGI, lightmap
capture SH, and baked resources stay in the scene for fallback and comparison,
but they are not sampled as indirect lighting while RTGI is enabled and hardware
ray tracing support is available.

Hybrid RTGI writes ray-traced indirect diffuse/specular lighting into the
existing Forward+ GI composition path. Path Traced mode routes full lighting
through the ray tracing path and disables incompatible screen-space or baked GI
contributions for that view.

If the GPU or driver does not expose the required Vulkan ray tracing features,
the settings remain visible, a warning is printed, and rendering falls back to
the existing non-ray-traced path instead of destructively changing scene data.

## Code Changes

- `scene/resources/environment.*`
  - Adds RTGI properties, inspector bindings, enum values, and defaults.
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
- `servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.*`
  - Adds ray tracing shader version management.
- `servers/rendering/renderer_rd/shaders/raytracing/`
  - Adds ray generation and shared ray tracing shader includes.
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
