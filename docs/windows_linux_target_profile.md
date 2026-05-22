# Windows And Linux Target Profile

## Goal

Keep the fork focused on desktop export templates that match the intended
deployment profile.

## Code Changes

- `SConstruct`
  - Always builds the Faster-Godot profile.
  - Rejects platforms other than `windows` and `linuxbsd`.
  - Rejects non-`x86_64` architectures.
  - Disables non-target graphics backends in the fork profile.
- `platform/windows/SCsub`
  - Builds OpenGL Windows support files only when `opengl3` is enabled.
- `platform/linuxbsd/wayland/SCsub`
  - Builds `detect_prime_egl.cpp` only when `opengl3` is enabled.

## Pros

- Keeps the build output aligned with Windows and Linux desktop exports.
- Avoids compiling backend glue for APIs the fork profile cannot use.
- Makes accidental unsupported-platform builds fail early.

## Cons

- macOS, mobile, Web, and console targets are outside this fork profile.
- Developers who still need broader official Godot coverage should use official
  Godot.
