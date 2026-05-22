# Rapier And Jolt Physics Profile

## Goal

Use Rapier as the default real 2D physics backend and Jolt as the default real
3D physics backend. The old Godot Physics implementation modules are removed
from the fork.

## Code Changes

- `modules/rapier_2d`
  - Embeds [appsinacup/godot-rapier-physics](https://github.com/appsinacup/godot-rapier-physics)
    2D as a built-in module.
  - Builds and links a static Rapier library from vendored Rust source in
    `thirdparty/rapier_2d`.
  - Uses offline Cargo builds with crates.io dependencies in
    `thirdparty/rapier_2d/vendor` and pinned Rapier/Salva git dependencies in
    `thirdparty/rapier_2d/vendor_git`.
  - Skips the built-in loader when a project has already loaded a Rapier 2D
    GDExtension, or when `faster_godot/physics_2d/load_builtin_rapier` is set
    to `false`.
- `modules/jolt_physics/register_types.cpp`
  - Keeps `Jolt Physics` as the default 3D physics server in the fork profile.
- `SConstruct`
  - Keeps `rapier_2d` and `jolt_physics` enabled by default.
- Removed modules
  - `godot_physics_2d`
  - `godot_physics_3d`

## Pros

- Restores real 2D physics without bringing back Godot Physics.
- Keeps 3D behavior on the Jolt backend intended for production use.
- Reduces the implementation surface to one supported backend per dimension.

## Cons

- Projects depending on Godot Physics behavior need official Godot.
- Physics results are allowed to differ from official Godot.
- Rapier adds a Rust toolchain requirement for builds that include 2D physics.
- Binary releases that include Rapier must include the notice and source
  materials listed in `thirdparty/rapier_2d/RELEASE_NOTICES.md`.
