<p align="center">
  <img src="fast-godot.svg" alt="Faster-Godot FG gear logo" width="160">
</p>

# Faster-Godot

Faster-Godot is a performance-first fork of Godot 4.6.3-stable for desktop
games that can commit to a narrower runtime contract:

- Windows and Linux only.
- x86_64 only.
- AVX2, FMA, F16C, and POPCNT required.
- Forward+ rendering only.
- Vulkan as the active rendering backend.
- Jolt as the 3D physics backend.
- No XR/VR runtime. OpenXR, WebXR, and mobile VR are stripped from the Faster
  profile.

This is not a general-purpose replacement for official Godot. It deliberately
trades broad platform compatibility for lower binary size, less renderer and
module surface area, and tighter hot paths for desktop Forward+ games.

## Project Status

Faster-Godot is an independent fork based on Godot Engine. It is not part of,
affiliated with, endorsed by, or supported by the Godot Engine project, the
`godotengine` GitHub organization, or the Godot Foundation.

The fork is provided as-is, without warranty. It keeps Godot's MIT/Expat license
terms, but it intentionally diverges from official Godot in platform support,
module selection, renderer support, CPU requirements, and runtime behavior.
Use official Godot when broad compatibility or upstream support matters more
than this fork's narrower desktop performance profile.

Faster-Godot uses its own FG gear mark for editor, application, splash, platform
export, and default new-project icons. The Godot logo is not used for this
fork's shipped branding or generated project icon.

## Where The Speed Comes From

The current speed work is concentrated in these areas. The measured gains so far
came mostly from the narrower renderer profile and CPU-side hot-path changes:

- Forward+ only: Mobile and compatibility renderer paths are removed from the
  fork build, including mobile tonemapping variants and Forward Mobile shader
  generation.
- AVX2/FMA/F16C/POPCNT baseline: The engine is compiled for modern x86_64
  desktop CPUs instead of the official broad SSE4.2 baseline. Core math hot
  paths use this SIMD contract directly, while zstd BMI2 paths are still
  dispatched at runtime on CPUs that expose BMI2.
- Render culling hot paths: Visibility range checks avoid square roots when no
  fade value is needed, and light culling avoids full AABB projection when only
  the minimum plane distance matters.
- Occlusion raycast backend: Embree is updated to 4.4.1 and the viewport
  occlusion path stays enabled for fixed-camera rooms, corridors, and dense
  static scenes.
- Hardware RTGI and path tracing: Forward+ Vulkan can use a hardware ray
  tracing global illumination path exposed through `Environment`, intended for
  dark 3D scenes where moving local lights need real bounce lighting. RT
  denoising runs before transparent particles are composited, particle
  instances stay out of the TLAS, and RT history validity masks reject stale
  history when geometry becomes newly visible or newly ready. RTGI denoiser
  selections that do not have a shipped vendor backend fall through to the
  internal temporal denoiser instead of leaving raw noisy output.
- TAA quality controls: Forward+ TAA exposes project settings for sharpness,
  history weight, disocclusion threshold, jitter phase count, and jitter scale.
  See the focused note in [docs/taa_quality_controls.md](docs/taa_quality_controls.md).
- Editor frame-rate caps: Editor Settings expose separate max FPS values for
  normal editor use and for when a project is running, so expensive 3D editor
  viewports can be throttled while testing the game.
- Windows input pump: Raw mouse input is drained in batches and normal message
  processing is capped per frame to avoid high-polling mice flooding
  `PeekMessage()` and tanking CanvasItem-heavy scenes.
- Smaller runtime surface: Unused modules and backends are disabled by default,
  including XR/VR and networking modules outside this fork's target profile.
- Jolt-only physics: Godot Physics 2D/3D modules are disabled in the fork
  profile; Jolt is the default 3D physics server.

## Build

Faster-Godot is always built with the fork profile. The old `faster_godot`
SCons argument is ignored if passed; this tree assumes Windows or Linux,
`arch=x86_64`, `precision=single`, Forward+, Vulkan, and an
AVX2/FMA/F16C/POPCNT-capable CPU.

Windows optimized release template:

```powershell
scons platform=windows target=template_release arch=x86_64 production=yes tests=no optimize=speed lto=full debug_symbols=no
```

Windows optimized release editor:

```powershell
scons platform=windows target=editor arch=x86_64 production=yes tests=no optimize=speed lto=full debug_symbols=no
```

Linux optimized release template:

```bash
scons platform=linuxbsd target=template_release arch=x86_64 production=yes tests=no optimize=speed lto=full debug_symbols=no
```

The forked binaries use the `faster-godot` basename and receive the
`.faster_godot` suffix.

## Changes From Official Godot

The short index is in [CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md).
Each larger change links to a focused document under [docs](docs/) with the
code scope, pros, and cons.

## License

Faster-Godot keeps Godot's original MIT/Expat license terms. See
[LICENSE.txt](LICENSE.txt) and [COPYRIGHT.txt](COPYRIGHT.txt). The Faster-Godot
name and fork maintenance do not imply endorsement by the Godot Engine project
or organization.

## Attribution

Based on Godot Engine 4.6.3-stable. Godot Engine and its upstream files remain
copyright their respective contributors.

Fork maintained by Jon Tamayo - https://x.com/vorvek
