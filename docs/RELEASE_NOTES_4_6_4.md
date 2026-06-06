# Faster-Godot 4.6.4

Faster-Godot 4.6.4 is a desktop performance and real-time ray tracing fork of Godot 4.6.3-stable, narrowed to a Windows and Linux Forward+ Vulkan profile. This release folds in the full real-time global illumination pipeline, the ported area light, the Intel XeSS upscaler, the clang and ThinLTO release toolchain, and a robustness pass over frame generation and the path-traced primary.

## Biggest differences from official Godot 4.6.3

### Real-time ray-traced global illumination
Hardware RTGI runs against the Vulkan ray tracing pipeline and is selectable per environment with three modes:
- Reflections RT Only: keeps the raster path and adds ray-traced reflections.
- Hybrid RTGI: raster-lit opaque plus traced indirect diffuse and reflections, denoised and composited. This is the production default.
- Full Path Tracing: a path-traced primary plus the same traced indirect, for the highest-quality look.
The pipeline uses a screen probe gather backed by a world radiance cache, an internal temporal and spatial denoiser, and quality presets (Performance, Balanced, Production).

### Frame generation
An interpolated frame generation path doubles apparent frame rate by warping between two rendered frames with motion vectors and disocclusion masking. Useful when heavy ray tracing pulls the base frame rate down.

### Temporal upscaling and anti-aliasing
TAA with extra quality controls, AMD FSR2, and Intel XeSS, plus custom viewport 3D scaling modes. The viewport scale and the upscaler are the recommended way to trade resolution for performance, and RTGI composites cleanly underneath them.

### Build-level performance
- The release templates are built with clang and ThinLTO, measured at 12 to 22 percent faster than the MSVC build on the trustworthy benchmark scenes, up to 32 percent on 2D, led by the GDScript virtual machine at about 18 to 22 percent because clang unlocks its computed-goto dispatch. The editor stays on MSVC and GCC.
- An opt-in clang PGO path builds on the clang toolchain.
- The runtime side adds a Vulkan descriptor set cache, Forward+ and RTGI hot-path tuning, animation and skinning SIMD, SceneTree and audio work, and AVX2/FMA culling.

### x86_64 AVX2 baseline (a hard requirement)
The desktop binaries assume an AVX2, FMA3, F16C, BMI, AES, and SSE4.2 capable x86_64 CPU and check for it at startup. On an older CPU the binary shows a short message and exits instead of crashing. This is the cost of the SIMD baseline that the culling, codec, and math paths are built on.

### Other engine changes
- AreaLight3D, the LTC area light from Godot 4.7, ported to 4.6.3 with direct lighting, VoxelGI and SDFGI and volumetric fog support, lightmap baking, and editor gizmos.
- A Forward+ only, Vulkan only desktop profile with a smaller binary and memory surface.
- Draco mesh import, Embree occlusion-culling updates, Rapier 2D physics, and high-polling Windows mouse input.

## New in 4.6.4 (since 4.6.3b)
- The complete radiance-probes RTGI pipeline (screen probe gather, world radiance cache, GI resolve) replacing the earlier stack, with Hybrid and Full Path Tracing modes.
- Frame generation robustness: a per-frame warp-magnitude clamp and out-of-bounds fallback so fast motion no longer smears, a velocity-aware disocclusion term, and pacing guards against clock jumps and pathological stalls. The 2D UI is composited after interpolation, so it is never warped.
- A Full Path Tracing primary stabilizer fix: a moving light used to leave a permanent bright trail on surfaces it had left, because stale history was kept whenever the surface darkened. The stabilizer now rectifies reprojected history into the current neighborhood color box, so the lighting decays the same frame the light moves on, while the per-frame path-traced noise still resolves.
- Intel XeSS integration with the missing Vulkan handshake fixed, and the DLSS, Streamline, and FSR 3.1 vendor paths removed in favor of XeSS and FSR2.
- A denoiser-option cleanup: deprecated and placeholder values normalize to the shipping denoiser on load, and a developer-only path-tracing reference toggle is hidden from the inspector.
- An editor RTGI fix: enabling RTGI in the editor no longer floods the log with texture errors, and switching between scenes that use different RTGI modes no longer crashes. The material guides are now requested through an existence check instead of a mandatory accessor, so a mode that does not produce them, or a mid-switch buffer teardown, is handled gracefully.
- Measured upscaler behavior: Hybrid RTGI is stable under TAA, FSR2, and XeSS, and below native resolution. Full Path Tracing pairs cleanly with no upscaler, TAA, and XeSS; it boils under FSR2 specifically, because a feed-forward temporal upscaler cannot lock a per-frame stochastic path-traced primary, so Hybrid is the recommended mode under FSR2.

## Requirements
- Windows or Linux, x86_64.
- An AVX2, FMA3, F16C, BMI, AES, SSE4.2 capable CPU (enforced at startup).
- A Vulkan 1.2 capable GPU. Ray tracing modes need a GPU with ray tracing support.

## Downloads
Each archive below is the editor or the export templates. The .NET archives are the C# (Mono) editor and templates; the others are the standard build.
- Windows and Linux editors, standard and .NET.
- Windows and Linux export templates, standard and .NET (.tpz).
- SHA256SUMS for verification and a source archive.

Maintained by Jon Tamayo. https://x.com/vorvek
