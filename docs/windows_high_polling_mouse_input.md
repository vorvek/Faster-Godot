# Windows High-Polling Mouse Input

## Goal

Avoid frame-time collapse on Windows when high-polling mice generate thousands
of raw input messages per second, especially over CanvasItem-heavy UI or 2D
content.

This change is adapted from upstream PR
<https://github.com/godotengine/godot/pull/109639>, which is still open as of
this fork update.

## Code Changes

- `platform/windows/display_server_windows.cpp`
  - Adds `DisplayServerWindows::process_raw_input()`.
  - Drains raw input with `GetRawInputBuffer()` once per frame instead of
    allocating and parsing each `WM_INPUT` message through `GetRawInputData()`.
  - Keeps the special Shift-key raw input handling.
  - Preserves captured mouse relative-motion parsing through raw mouse input.
  - Caps normal Windows message pumping per frame, while processing keyboard and
    mouse-button messages through a separate small budget.
  - Leaves `WM_INPUT` messages as no-op in `WndProc`; raw input is handled by
    the frame-level buffer drain.
- `platform/windows/display_server_windows.h`
  - Declares `process_raw_input()`.

## Pros

- Prevents `PeekMessage()`/`WM_INPUT` floods from dominating the frame when a
  2,000 Hz, 4,000 Hz, or 8,000 Hz mouse is moved.
- Reduces per-event heap allocation in the Windows raw input path.
- Keeps captured mouse mode on raw relative input, which is the important path
  for first-person camera controls.

## Cons

- The upstream PR is not merged yet and still asks for broader device testing.
- The local automated benchmark does not simulate an 8,000 Hz mouse; this needs
  hands-on validation with the actual device and polling rate.
- Per-frame event budgets trade worst-case input queue draining for frame-time
  stability. If a tool needs every non-accumulated mouse event in visible mouse
  mode, this area should be revalidated.
