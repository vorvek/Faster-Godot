# Binary And Memory Surface Reduction

## Goal

Reduce binary size and runtime surface area by removing backends and modules not
used by the target profile.

## Code Changes

- `SConstruct`
  - Disables these modules by default in the fork profile:
    `camera`, `enet`, `jsonrpc`, `mobile_vr`, `multiplayer`,
    `objectdb_profiler`, `openxr`, `upnp`, `webrtc`, `websocket`, and `webxr`.
  - Disables XR and non-target rendering backends.
  - Keeps one physics backend per dimension: Rapier for 2D and Jolt for 3D.
  - Restricts the registered export platforms, platform APIs, and platform class
    documentation to Windows and Linux, so the editor no longer compiles or
    offers the Web, Android, iOS, and macOS export plugins.
- `servers/rendering/rendering_server.cpp`
  - Skips several mobile and OpenGL compatibility project-setting defaults in
    the Forward+ only profile.
- Renderer build scripts
  - Skip Forward Mobile source and shader generation.

## Pros

- Smaller export template binary.
- Less unused code linked into the target runtime.
- Fewer runtime settings and renderer variants to initialize or reason about.
- Current measured Windows release executable is about 73 MiB, compared with an
  official executable around 164 MiB in the same local environment.

## Cons

- Disabled modules are not available unless the fork profile is turned off or
  the module list is changed.
- Multiplayer, WebRTC/WebSocket, OpenXR, WebXR, mobile VR, and Godot Physics
  implementation modules are not part of the default Faster-Godot runtime.
- Broad official Godot compatibility is intentionally reduced.
