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
  - Loops the buffered read until the queue reports empty. With a null buffer,
    `GetRawInputBuffer()` reports the byte size of the first pending message,
    not a message count, so a single read caps out at a buffer's worth of
    events (about 48 at the sizing inherited from the upstream PR). The drain
    must not be bounded by a small pass count: when a long frame (a scene
    load) overlaps mouse motion, the thread's message queue reaches the
    10,000-message Windows cap, after which Windows drops every new hardware
    message, clicks and keys included, until the backlog is consumed. A
    bounded drain at a low frame rate (an unfocused editor idles near 10 FPS)
    then never catches up with a high-polling mouse, and the window looks
    permanently frozen and stops responding to input. Unlike the per-message pump,
    "until empty" cannot spin here: one buffered read consumes tens of events
    in microseconds while new packets arrive 125 us apart at 8,000 Hz.
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

## Message pump policy

The pump throttles only what the mouse hardware can flood, and drains the rest
every frame:

- `WM_INPUT` is excluded from `PeekMessage()` entirely. Pulling raw input
  through the pump one message at a time is the original collapse; the
  buffered read consumes it in batch instead.
- `WM_MOUSEMOVE` and `WM_NCMOUSEMOVE` are dispatched at most once per frame.
  Windows synthesizes these on demand from the hardware input stream, so while
  the mouse is moving the queue never reads empty and an unbounded pump spins
  until the motion stops. Measured on an 8,000 Hz mouse over a 2D scene at
  240 Hz V-Sync: the unbounded pump fell below 1 FPS (frames over 1,000 ms);
  the throttled pump loses 1 to 2 FPS. No motion is lost, because these
  messages carry the latest coalesced cursor position and the engine derives
  relative motion from successive positions.
- Keys, characters, mouse buttons, wheel, and everything else discrete drain
  fully. Capping them is what split `WM_KEYDOWN`/`WM_CHAR` pairs across frames
  in the upstream PR and made keys fire twice (Shift+F toggling freelook on
  and off, for example).

## Differences from the upstream PR

- The upstream PR caps all message pumping at one event per frame (plus one
  keyboard or mouse button event through a second budget). This fork caps only
  the synthesized mouse motion messages and drains discrete input fully, which
  keeps key pairs intact.
- The upstream PR reads the raw input buffer once per frame, which holds about
  48 events. This fork loops the read until the queue is empty, so any backlog
  clears within a frame, including the full 10,000-message queue that builds
  up when a long frame overlaps mouse motion.
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
- In visible, hidden, and confined mouse modes, scripts receive at most one
  mouse motion event per rendered frame even with input accumulation disabled.
  The total delta is exact, but sub-frame cursor samples from the legacy path
  are gone. Captured mode keeps every raw event, so accumulation still has its
  full meaning there.
- Captured mode does per-event work for every poll (an `InputEventMouseMotion`
  plus a cursor recenter), so its cost grows linearly with the polling rate.
  Bounded and spread evenly, but an 8,000 Hz mouse does about 16 times the
  per-event work of a 500 Hz one in that mode.
