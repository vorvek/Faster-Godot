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
  - Loops the buffered read until the queue is empty. With a null buffer,
    `GetRawInputBuffer()` reports the byte size of the first pending message,
    not a message count, so a single read caps out at a buffer's worth of
    events (about 48 at the sizing inherited from the upstream PR). Anything
    left in the queue would then be eaten unread by the unthrottled message
    pump that follows.
  - Keeps the special Shift-key raw input handling.
  - Preserves captured mouse relative-motion parsing through raw mouse input.
  - Shares the per-event parsing between the buffered path and `WndProc`
    through `_process_raw_input_event()`.
  - Keeps `WM_INPUT` handling in `WndProc` as a fallback through
    `GetRawInputData()`. It only sees stragglers that arrive while the message
    pump itself is running; without it their motion data would be freed unread
    by `DefWindowProc`.
  - Targets the focused window (or active popup) in the buffered path instead
    of hardcoding the main window. This is the same window
    `_set_mouse_mode_impl()` picks when it captures the mouse.
  - Reuses one member buffer for both raw input read paths instead of heap
    allocating per call.
- `platform/windows/display_server_windows.h`
  - Declares `process_raw_input()` and `_process_raw_input_event()`, and holds
    the reused raw input read buffer.

## Differences from the upstream PR

- The upstream PR caps the Windows message pump at a few events per frame and
  filters `WM_INPUT` out of `PeekMessage()`. This fork drains the queue
  normally. The buffered raw input read already removes the `WM_INPUT` flood,
  and legacy `WM_MOUSEMOVE` is coalesced by the OS, so the cap buys nothing.
  Worse, the cap is what split `WM_KEYDOWN`/`WM_CHAR` pairs across frames and
  made keys fire twice (Shift+F toggling freelook on and off, for example).
- The upstream PR reads the buffer once per frame. Under its `PeekMessage()`
  filter the leftovers survive to the next frame as a growing backlog. Here
  they would have been destroyed by the unfiltered pump, hence the drain loop.
- Note for 32-bit builds: `GetRawInputBuffer()` has documented WOW64 alignment
  requirements that neither the PR nor this fork implements. This fork only
  ships x86_64 Windows binaries, so it does not hit them.

## Capture transition

When the mouse is captured the cursor is warped to the window center. That warp is
a programmatic move, not a user gesture, so the synthetic motion event emitted at
that point reports zero relative motion and resets the relative baseline (`old_x`,
`old_y`) to the center.

The upstream PR derived this event's relative motion from the previous cursor
position while `get_position()` was still zero, so it produced the negated last
cursor location: a large false delta on the first captured frame. A hidden cursor
masks the position blip, but captured-mode consumers act on the relative motion, so
editor value drags snapped to their limits and the 3D freelook camera flipped fully
up or down at the start of a drag.

## Pros

- Prevents `PeekMessage()`/`WM_INPUT` floods from dominating the frame when a
  2,000 Hz, 4,000 Hz, or 8,000 Hz mouse is moved.
- Reduces per-event heap allocation in the Windows raw input path.
- Keeps captured mouse mode on raw relative input, which is the important path
  for first-person camera controls.

## Cons

- The upstream PR is not merged yet and still asks for broader device testing.
- Raw input arriving between the buffered drain and the end of the message pump
  is handled one event at a time through the `WndProc` fallback. That is the
  pre-PR per-event path, but it only ever sees a handful of stragglers per
  frame, so the original flood cannot come back through it.
