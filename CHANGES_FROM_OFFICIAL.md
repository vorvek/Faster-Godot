# Changes From Official Godot

Faster-Godot starts from Godot 4.6.3-stable and narrows the engine around a
desktop Forward+ profile. The detailed notes are split by change area:

| Area | Detail |
| --- | --- |
| Forward+ only renderer profile | [docs/forward_plus_only.md](docs/forward_plus_only.md) |
| Vulkan-only Windows RenderingDevice profile | [docs/vulkan_only_windows_rendering.md](docs/vulkan_only_windows_rendering.md) |
| Vulkan descriptor set cache | The Vulkan RenderingDevice driver now caches descriptor sets by layout and bound resource signature, reuses matching `VkDescriptorSet` handles, skips redundant `vkAllocateDescriptorSets()`/`vkUpdateDescriptorSets()` calls, evicts stale entries after several frames, and purges entries when referenced resources, layouts, or linear pools are destroyed/reset. |
| x86_64 AVX2/FMA3/AES/BMI desktop baseline and codec SIMD hooks | [docs/x86_64_avx2_fma.md](docs/x86_64_avx2_fma.md) |
| Windows and Linux target profile | [docs/windows_linux_target_profile.md](docs/windows_linux_target_profile.md) |
| Rapier 2D physics backend | [docs/rapier_2d_physics.md](docs/rapier_2d_physics.md) |
| Rapier/Jolt physics profile | [docs/rapier_jolt_physics.md](docs/rapier_jolt_physics.md) |
| Hardware RTGI, path tracing, RTGI denoising, and particle stability | [docs/path_tracing_gi.md](docs/path_tracing_gi.md) |
| Area light (LTC AreaLight3D) port from 4.7 | [docs/arealight3d.md](docs/arealight3d.md) |
| TAA quality controls | [docs/taa_quality_controls.md](docs/taa_quality_controls.md) |
| Editor frame-rate limits while testing | [docs/editor_frame_rate_limits.md](docs/editor_frame_rate_limits.md) |
| Forward+/RTGI rendering hot-path tuning | [docs/rendering_hot_path_tuning.md](docs/rendering_hot_path_tuning.md) |
| Audio hot-path tuning | [docs/audio_hot_path_tuning.md](docs/audio_hot_path_tuning.md) |
| AnimationMixer SIMD track blending and skinning hot-path tuning | [docs/animation_player_hot_path_tuning.md](docs/animation_player_hot_path_tuning.md) |
| SceneTree hot-path tuning | [docs/scene_tree_hot_path_tuning.md](docs/scene_tree_hot_path_tuning.md) |
| GLTF/GLB Draco mesh import | The GLTF importer now supports existing Blender-style `KHR_draco_mesh_compression` assets by bundling a decoder-only Google Draco 1.5.7 source subset in `thirdparty/draco`, expanding compressed mesh primitives into transient in-memory accessors before normal mesh parsing, and reporting `KHR_draco_mesh_compression` from `GLTFDocument.get_supported_gltf_extensions()`. This is importer-only: Faster-Godot does not export Draco-compressed GLTF/GLB files and does not add a `.blend` importer option to make Blender emit Draco during temporary export. |
| DynamicBVH convex/frustum culling SIMD tuning | `DynamicBVH::Volume::intersects_convex()` now batches AABB-vs-plane rejection checks with AVX2/FMA for 8-plane and 4-plane groups, preserving the scalar tail and point-separation checks for exact behavior. |
| Occlusion culling and Embree update | [docs/occlusion_culling.md](docs/occlusion_culling.md) |
| Windows high-polling mouse input | [docs/windows_high_polling_mouse_input.md](docs/windows_high_polling_mouse_input.md) |
| Binary and memory surface reduction | [docs/binary_and_memory_surface.md](docs/binary_and_memory_surface.md) |
| Custom Viewport Resolution Scaling Modes | [docs/custom_viewport_scaling.md](docs/custom_viewport_scaling.md) |
| Linux optimized release build hygiene | Linux LTO release builds suppress known GCC cross-translation-unit range-analysis false positives and avoid duplicate or ambiguous symbols in ICU data, KTX/Vulkan format aliases, ETC texture compression helpers, and selected hot-path temporaries without changing shipped features. |
| Desktop build-level compiler/linker flag pass | [docs/pgo.md](docs/pgo.md) |
| Independent fork identity and legal notices | [README.md](README.md), [LICENSE.txt](LICENSE.txt), [COPYRIGHT.txt](COPYRIGHT.txt) |
| Faster-Godot branding and default project icon | Root, editor, app icon, splash, platform export, default project/document, web editor, engine version banner, emitted binary basename, shell completions, Windows version resource/installer metadata, Linux desktop/AppStream/MIME/X11/Wayland metadata and installable icons, macOS bundle metadata, editor About/support links, GitHub Actions release artifacts/release-page publishing, and source archive naming now use the fork's FG gear mark and independent Faster-Godot identity instead of Godot-derived artwork/official branding. See [README.md](README.md) and [COPYRIGHT.txt](COPYRIGHT.txt). |

RTGI denoiser, temporal stability, and TAA tuning details live in
[docs/path_tracing_gi.md](docs/path_tracing_gi.md) and
[docs/taa_quality_controls.md](docs/taa_quality_controls.md).

Maintained by Jon Tamayo - https://x.com/vorvek
