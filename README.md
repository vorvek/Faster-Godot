# Faster-Godot

Faster-Godot is a performance-first fork of Godot 4.6.2-stable for desktop
games that can commit to a narrower runtime contract:

- Windows and Linux only.
- x86_64 only.
- AVX2 and FMA required.
- Forward+ rendering only.
- Vulkan as the active rendering backend.
- Jolt as the 3D physics backend.

This is not a general-purpose replacement for official Godot. It deliberately
trades broad platform compatibility for lower binary size, less renderer and
module surface area, and tighter hot paths for desktop Forward+ games.

## Where The Speed Comes From

The current speed work is concentrated in these areas. The benchmarked gains so
far came mostly from the renderer-profile and hot-path changes:

- Forward+ only: Mobile and compatibility renderer paths are removed from the
  fork build, including mobile tonemapping variants and Forward Mobile shader
  generation.
- AVX2/FMA baseline: The engine is compiled for modern x86_64 desktop CPUs
  instead of the official broad SSE4.2 baseline.
- Render culling hot paths: Visibility range checks avoid square roots when no
  fade value is needed, light culling avoids full AABB projection when only the
  minimum plane distance matters, and clustered volume draws batch consecutive
  same-geometry elements.
- Occlusion raycast backend: Embree is updated to 4.4.1 and the viewport
  occlusion path stays enabled for fixed-camera rooms, corridors, and dense
  static scenes.
- Hardware RTGI and path tracing: Forward+ Vulkan can use a hardware ray
  tracing global illumination path exposed through `Environment`, intended for
  dark 3D scenes where moving local lights need real bounce lighting.
- Windows input pump: Raw mouse input is drained in batches and normal message
  processing is capped per frame to avoid high-polling mice flooding
  `PeekMessage()` and tanking CanvasItem-heavy scenes.
- Smaller runtime surface: Unused modules and backends are disabled by default,
  including mobile/XR/networking modules outside this fork's target profile.
- Jolt-only physics: Godot Physics 2D/3D modules are disabled in the fork
  profile; Jolt is the default 3D physics server.

## Build

Faster-Godot is enabled by default through the `faster_godot=yes` SCons option.
Use `faster_godot=no` to build closer to official Godot behavior from this tree.

Windows release template:

```powershell
scons platform=windows target=template_release arch=x86_64 use_mingw=yes tests=no optimize=speed lto=none debug_symbols=no -j16
```

Linux release template:

```bash
scons platform=linuxbsd target=template_release arch=x86_64 tests=no optimize=speed lto=none debug_symbols=no -j16
```

The forked binaries receive the `.faster_godot` suffix.

## Changes From Official Godot

The short index is in [CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md).
Each larger change links to a focused document under [docs](docs/) with the
code scope, pros, and cons.

## License

Faster-Godot keeps Godot's original MIT license. See [LICENSE.txt](LICENSE.txt).

## Attribution

Based on Godot Engine 4.6.2-stable.

Fork maintained by Jon Tamayo - https://x.com/vorvek
