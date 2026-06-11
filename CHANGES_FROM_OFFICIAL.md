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
| Hardware RTGI and path tracing: modes, denoising, fog model, probe depth reconstruction, alpha-scissor emissive sampling, VoxelGI/SDFGI coexistence, and particle stability | [docs/path_tracing_gi.md](docs/path_tracing_gi.md) |
| RTGI control alignment with measured behavior | `rtgi_energy` now multiplies the composited indirect GI in every RTGI mode; it used to scale only the path-traced primary in Full Scene Path-Traced GI and did nothing in Hybrid RTGI (Cornell occluded-region linear-luma ratio at energy 2.0: 1.00 before, 2.00 path-traced / 1.98 Hybrid after). `rtgi_samples_per_pixel` is honored by the path-traced primary lighting; on Cornell at 1080p (RTX 4080 SUPER), temporal sparkle per megapixel dropped from 128.9 at 1 sample to 61.0 at 2 (+0.26 ms) and 6.8 at 4 (+0.71 ms); presets pin it to 1 and the Custom preset exposes it. The None denoiser is a real bypass of the temporal stabilizer and spatial denoise polish for inspecting the accumulated signal. The inert `rtgi_wrc_strength` property was removed; a compatibility binding keeps the old setter/getter resolvable for prebuilt GDExtensions. The renderer also warns once when RTGI output is discarded or substituted. See [docs/path_tracing_gi.md](docs/path_tracing_gi.md). |
| Area light (LTC AreaLight3D) port from 4.7, plus path-traced area lights under RTGI | [docs/arealight3d.md](docs/arealight3d.md) |
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
| Clang + ThinLTO release editor and templates (GDScript-VM computed-goto unlock) | [docs/pgo.md](docs/pgo.md) |
| Opt-in clang PGO build path (IR + context-sensitive, feeds ThinLTO) | [docs/pgo.md](docs/pgo.md) |
| Independent fork identity and legal notices | [README.md](README.md), [LICENSE.txt](LICENSE.txt), [COPYRIGHT.txt](COPYRIGHT.txt) |
| Faster-Godot branding and default project icon | Root, editor, app icon, splash, platform export, default project/document, web editor, engine version banner, emitted binary basename, shell completions, Windows version resource/installer metadata, Linux desktop/AppStream/MIME/X11/Wayland metadata and installable icons, macOS bundle metadata, editor About/support links, GitHub Actions release artifacts/release-page publishing, and source archive naming now use the fork's FG gear mark and independent Faster-Godot identity instead of Godot-derived artwork/official branding. See [README.md](README.md) and [COPYRIGHT.txt](COPYRIGHT.txt). |

RTGI denoiser, temporal stability, and TAA tuning details live in
[docs/path_tracing_gi.md](docs/path_tracing_gi.md) and
[docs/taa_quality_controls.md](docs/taa_quality_controls.md).

Maintained by Jon Tamayo - https://x.com/vorvek
