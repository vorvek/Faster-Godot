# Editor Frame-Rate Limits

## Goal

Reduce the editor's CPU and GPU cost while testing a running project, especially
when an expensive 3D viewport remains open in the editor.

This is useful for hardware ray tracing scenes where the game window needs the
available GPU budget, while the editor viewport only needs to stay minimally
responsive.

## Code Changes

- `editor/settings/editor_settings.cpp`
  - Adds `interface/editor/max_fps`.
  - Adds `interface/editor/max_fps_while_playing`.
- `editor/editor_node.cpp`
  - Applies the editor max FPS setting when the editor enters the tree.
  - Reapplies the active editor FPS cap when Editor Settings change.
  - Switches to `interface/editor/max_fps_while_playing` while a project is
    running from the editor, when that value is greater than `0`.
  - Restores `interface/editor/max_fps` when the project stops.
- `core/os/os.cpp` and `platform/windows/os_windows.cpp`
  - Allow the existing `Engine::max_fps` frame-delay path to apply to editor
    builds too.
- `doc/classes/EditorSettings.xml` and `doc/classes/ProjectSettings.xml`
  - Document the editor settings and clarify that project `max_fps` does not
    control the editor itself.

## Behavior

- `interface/editor/max_fps = 0` leaves the editor uncapped.
- `interface/editor/max_fps_while_playing = 0` keeps using
  `interface/editor/max_fps` while a project is running.
- Setting `interface/editor/max_fps_while_playing` to a low value, such as `1`,
  heavily throttles editor redraws while testing.

## Pros

- Keeps the editor from rendering at high-refresh-rate V-Sync speeds when the
  user wants GPU headroom for the running project.
- Provides a direct workaround for expensive ray-traced editor viewports during
  playtesting.
- Reuses Godot's existing max-FPS frame pacing instead of adding a new timing
  path.

## Cons

- Very low caps make the editor less responsive while the project is running.
- The setting throttles the editor process globally, not only individual 3D
  viewports.
- V-Sync and driver-level frame pacing can still impose their own limits.
