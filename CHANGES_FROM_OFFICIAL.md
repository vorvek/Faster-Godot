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
| Forward+ rendering hot-path tuning | [docs/rendering_hot_path_tuning.md](docs/rendering_hot_path_tuning.md) |
| Occlusion culling and Embree update | [docs/occlusion_culling.md](docs/occlusion_culling.md) |
| Windows high-polling mouse input | [docs/windows_high_polling_mouse_input.md](docs/windows_high_polling_mouse_input.md) |
| Binary and memory surface reduction | [docs/binary_and_memory_surface.md](docs/binary_and_memory_surface.md) |

## Current Benchmark Snapshot

Rendered Vulkan Forward+ benchmark, Windows release template, three-run average:

| Metric | Official 4.6.2 Vulkan | Faster-Godot Vulkan | Delta |
| --- | ---: | ---: | ---: |
| FPS average | 564.37 FPS | 573.88 FPS | +1.7% |
| Frame time average | 1.77 ms | 1.74 ms | -1.7% |
| VRAM monitor | 184.50 MiB | 183.23 MiB | -0.7% |

The benchmark scene used fixed-camera 3D gameplay with Vulkan Forward+ on both
official Godot and Faster-Godot. The Windows high-polling mouse fix is included
in the Faster-Godot binary used for this benchmark. Linux builds were validated
by headless boot; Linux rendered benchmark data is not included yet.

Maintained by Jon Tamayo - https://x.com/vorvek
