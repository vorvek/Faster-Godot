# Jolt-Only Physics Profile

## Goal

Use Jolt as the only real 3D physics backend and remove Godot Physics from the
fork profile.

## Code Changes

- `SConstruct`
  - Disables `godot_physics_2d` and `godot_physics_3d`.
  - Keeps `jolt_physics` enabled by default.
- `modules/jolt_physics/register_types.cpp`
  - Sets `Jolt Physics` as the default 3D physics server in the fork profile.
- `servers/register_server_types.cpp`
  - Sets `Dummy` as the default 2D physics server when the fork profile is
    active.

## Pros

- Reduces physics code and server choice surface area.
- Avoids carrying two 3D physics implementations in the target build.
- Keeps 3D behavior on the physics backend intended for production use.

## Cons

- Projects depending on Godot Physics behavior need official Godot.
- 2D physics is not a supported runtime feature in this profile.
- Physics results are allowed to differ from official Godot.
