# Faster-Godot 4.6.4a

Faster-Godot 4.6.4a is a hotfix on 4.6.4. The headline change is that the editor now ships on the same clang and ThinLTO toolchain as the export templates, so testing a project from the editor runs the fast codegen. It also folds in a real-time GI resolve performance pass, a Forward+ area-light optimization, and correctness fixes.

## The fast editor

In Godot, Run Project (F5) and Run Current Scene (F6) run inside the editor binary, using the editor's own renderer, GDScript VM, and physics, not an export template. Until now the release editor was built with MSVC and GCC while only the templates used clang and ThinLTO, so the dev test-loop missed the fork's largest build-level speedup. The 4.6.4a editor is built with clang, lld, and ThinLTO on Windows and Linux, measured at 12 to 22 percent faster than the MSVC build on the reliable CPU-bound scenes, led by the GDScript virtual machine at about 18 to 22 percent because clang unlocks its computed-goto dispatch. Building from source still defaults to MSVC and GCC for fast iteration and native debugging; the faster toolchain is opt-in with use_llvm.

## New in 4.6.4a (since 4.6.4)
- The release editor ships on clang and ThinLTO, on Windows and Linux.
- Real-time GI resolve performance: the per-pixel screen-GI resolve now walks each surrounding probe's octahedral tile once instead of twice, compiles each resolve mode as its own pipeline, reads each spatial-filter tap more cheaply with view-space normals and a two-term linear depth, and caches its working tile in shared memory. Every change leaves the rendered frame bit-for-bit identical. Together they take roughly 30 percent off the dominant Integrate pass and about 45 percent off the spatial pass on an RTX 4080 SUPER.
- RTGI now renders correctly with 3D MSAA enabled. The material-guide prepass that feeds the GI resolve failed to build its framebuffer under MSAA, so the guide buffer stayed white and the composite multiplied the frame by white, producing an all-white screen. The prepass now renders into its own single-sample depth-stencil attachment when MSAA is on. All three RTGI modes render with MSAA, and the non-MSAA path is unchanged.
- The Forward+ area-light path is gated behind a specialization constant, so scenes with no area lights skip that branch in the opaque shader. Measured at about 0.55 ms off the opaque pass on the area-light test scene.
- NaN serialization and convex-segment intersection stay correct under the fork's fast floating-point build.

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
