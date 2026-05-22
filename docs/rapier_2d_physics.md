# Rapier 2D Physics

## Summary

Faster-Godot removes the upstream Godot Physics 2D module and embeds Rapier as
the default real 2D physics server. The integration is based on
[appsinacup/godot-rapier-physics](https://github.com/appsinacup/godot-rapier-physics)
and is built as a static library from the vendored Rust source under
`thirdparty/rapier_2d`.

Jolt remains the default 3D physics backend. The combined physics profile is
tracked in [rapier_jolt_physics.md](rapier_jolt_physics.md).

## Build Requirements

- Cargo must be available on `PATH` when `module_rapier_2d_enabled=yes`.
- The required Rust toolchain is pinned in
  `thirdparty/rapier_2d/rust-toolchain.toml` and is currently
  `nightly-2025-12-12`.
- The module only builds with `precision=single`; Faster-Godot release editor
  builds use that precision profile.
- The module guard also disables Rapier when `disable_physics_2d=yes`. Normal
  Faster-Godot release editor builds keep 2D physics enabled.

The default build is offline and reproducible from the repository contents:

- `thirdparty/rapier_2d/vendor` contains crates.io dependencies from
  `Cargo.lock`.
- `thirdparty/rapier_2d/vendor_git` contains the pinned Rapier and Salva git
  dependencies.
- `modules/rapier_2d/SCsub` invokes Cargo with `--locked --offline` and writes
  generated build state under `bin/rapier_2d_*`.

Do not remove apparently generic crate directories such as
`thirdparty/rapier_2d/vendor/log` or nested Windows bindings such as
`thirdparty/rapier_2d/vendor/windows-sys/src/Windows/Win32`; they are required
vendored source, not build output.

## Runtime Behavior

At startup, `modules/rapier_2d` statically loads the embedded Rapier
GDExtension entry point unless one of these conditions is true:

- A project has already loaded the extension at
  `res://addons/godot-rapier2d/godot-rapier2d.gdextension`.
- A project has loaded an extension whose path contains `godot-rapier2d` or
  `rapier2d`.
- `faster_godot/physics_2d/load_builtin_rapier` is set to `false`.

Disabling the built-in loader does not restore Godot Physics 2D. It is intended
for projects that provide their own compatible Rapier extension.

## Licenses And Notices

The embedded integration is based on appsinacup's MIT/Expat-licensed
`godot-rapier-physics` project. Rapier and Salva are Apache-2.0 licensed, and
the vendored godot-rust crates keep their upstream MPL-2.0 terms.

The full crate license inventory is recorded in
`thirdparty/rapier_2d/RUST_CRATE_LICENSES.md`. Binary releases that ship the
built-in Rapier module must include the notice and source materials listed in
`thirdparty/rapier_2d/RELEASE_NOTICES.md`.
