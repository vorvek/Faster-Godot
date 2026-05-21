# Changes From Official Godot

Faster-Godot starts from Godot 4.6.3-stable and narrows the engine around a
desktop Forward+ profile. The detailed notes are split by change area:

| Area | Detail |
| --- | --- |
| Forward+ only renderer profile | [docs/forward_plus_only.md](docs/forward_plus_only.md) |
| Vulkan-only Windows RenderingDevice profile | [docs/vulkan_only_windows_rendering.md](docs/vulkan_only_windows_rendering.md) |
| x86_64 AVX2/FMA/F16C/POPCNT baseline and codec SIMD hooks | [docs/x86_64_avx2_fma.md](docs/x86_64_avx2_fma.md) |
| Windows and Linux target profile | [docs/windows_linux_target_profile.md](docs/windows_linux_target_profile.md) |
| Jolt-only physics profile | [docs/jolt_only_physics.md](docs/jolt_only_physics.md) |
| Hardware RTGI, path tracing, denoiser history, and particle stability | [docs/path_tracing_gi.md](docs/path_tracing_gi.md) |
| TAA quality controls | [docs/taa_quality_controls.md](docs/taa_quality_controls.md) |
| Editor frame-rate limits while testing | [docs/editor_frame_rate_limits.md](docs/editor_frame_rate_limits.md) |
| Forward+/RTGI rendering hot-path tuning | [docs/rendering_hot_path_tuning.md](docs/rendering_hot_path_tuning.md) |
| Audio hot-path tuning | [docs/audio_hot_path_tuning.md](docs/audio_hot_path_tuning.md) |
| Animation and skinning hot-path tuning | [docs/animation_player_hot_path_tuning.md](docs/animation_player_hot_path_tuning.md) |
| SceneTree hot-path tuning | [docs/scene_tree_hot_path_tuning.md](docs/scene_tree_hot_path_tuning.md) |
| Occlusion culling and Embree update | [docs/occlusion_culling.md](docs/occlusion_culling.md) |
| Windows high-polling mouse input | [docs/windows_high_polling_mouse_input.md](docs/windows_high_polling_mouse_input.md) |
| Binary and memory surface reduction | [docs/binary_and_memory_surface.md](docs/binary_and_memory_surface.md) |
| Independent fork identity and legal notices | [README.md](README.md), [LICENSE.txt](LICENSE.txt), [COPYRIGHT.txt](COPYRIGHT.txt) |
| Faster-Godot branding and default project icon | Root, editor, app icon, splash, platform export, default project/document, web editor, engine version banner, emitted binary basename, shell completions, Windows version resource/installer metadata, Linux desktop/AppStream/MIME/X11/Wayland metadata and installable icons, macOS bundle metadata, editor About/support links, GitHub Actions release artifacts/release-page publishing, and source archive naming now use the fork's FG gear mark and independent Faster-Godot identity instead of Godot-derived artwork/official branding. The logo and splash rasters use white lettering with contrast outlines/backgrounds so they remain readable on the default gray/dark boot surfaces; see [README.md](README.md) and [COPYRIGHT.txt](COPYRIGHT.txt). |

## RTGI Vendor Denoiser Reference Status

The RTGI implementation was compared against NVIDIA's Godot path tracing branch,
Streamline/DLSS Ray Reconstruction routing, and NVIDIA NRD. This fork keeps the
internal temporal RT denoiser as the shipped path. Streamline/DLSS and NRD SDK
imports are deferred vendor-dependency projects because they add separate source,
license, build, and packaging obligations.

The local renderer emits RT depth, RT velocity, and history validity/identity
masks for RTGI denoising. When the NVIDIA/DLSS RR buffer-output variant is
selected, it also emits DLSS RR diffuse/specular albedo, normal/roughness, and
specular hit distance. Direct NRD integration would still need explicit
NRD-style viewZ ownership, packed diffuse/specular radiance-plus-hit-distance
inputs, material-demodulated signal contracts, permanent/transient pool
management, and NRD dispatch integration.

## Current Benchmark Snapshot

Windows Vulkan Forward+ validation stress scene, 1920x1080 viewport, RTGI
disabled at runtime, VSync disabled, uncapped frame rate, five-run average
against official 4.6.3:

Scene shape at the sampled alive-state workload:

| Context | Value |
| --- | ---: |
| Test CPU | AMD Ryzen 9 9950X3D |
| Test memory | 48 GB DDR5-6000 |
| Test GPU | NVIDIA GeForce RTX 4080 SUPER |
| Animated character-body workload | 26 |
| Additional skinned actors spawned for stress | 24 |
| Runtime character logic | Scripted updates, target queries/raycasts, movement, animation, and hit/removal transitions |
| Skeleton3D nodes in scene | 26 |
| MeshInstance3D nodes in scene | 672 |
| Light nodes | 1 omni |
| Rendered objects in sampled frames | ~1.3K |
| Rendered primitives in sampled frames | ~2.87M |
| Draw calls in benchmark table | ~1.1K |
| Particle systems validated after sample window | 24 |

| Metric | Official 4.6.3 Vulkan | Faster-Godot Vulkan | Delta |
| --- | ---: | ---: | ---: |
| FPS average | 448.46 FPS | 548.98 FPS | +22.413% |
| Process time average | 3.46 ms | 3.14 ms | -9.255% |
| VRAM monitor | 313.896 MiB | 318.469 MiB | +4.573 MiB |

This snapshot uses a heavier animated 3D stress path than the previous
single-actor smoke comparison, so it is the preferred current desktop
performance baseline. Draw-call counts are recorded only as scene context, not
as a batching improvement claim. Linux rendered benchmark data is not included
yet.

Maintained by Jon Tamayo - https://x.com/vorvek
