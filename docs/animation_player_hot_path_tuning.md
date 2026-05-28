# Animation And Skinning Hot-Path Tuning

## Intent

Remove avoidable string conversion, hashing, checked-access overhead, and
per-bone render-server queue traffic from active animation playback and
per-frame skeleton skin uploads without changing animation behavior.

## Changes

- `scene/animation/animation_player.{h,cpp}`
  - Stores `PlaybackData::animation_name` as `StringName`, matching the
    `animation_set` key type used by `AnimationMixer`.
  - Keeps per-frame animation-change snapshots and queued transition names as
    `StringName` instead of converting them through `String`.
- `scene/animation/animation_player.cpp`
  - Replaces paired blend-time, next-animation, and animation-data lookups
    with single `getptr()` probes on transition and seek paths.
- `scene/3d/skeleton_3d.cpp` and `scene/resources/3d/skin.h`
  - Uses an unchecked bind-pose accessor inside the skeleton upload loop after
    the loop has already bounded iteration by the skin bind count.
- `scene/3d/skeleton_3d.{h,cpp}` and rendering mesh-storage backends
  - Packs each 3D skin's per-bone transform data into a reusable float buffer
    and submits it through one bulk RenderingServer command instead of one
    command per bone.
  - Double-buffers the scene-thread upload vector per skin reference so the
    queued render-thread command owns stable data until it is flushed.
- `scene/animation/animation_mixer.cpp`
  - Batches transform position and scale blend accumulation into eight-lane
    AVX2/FMA updates while preserving the existing quaternion rest-axis slerp
    behavior.

## Pros

- Avoids repeated `String` construction when active animations are processed
  and looked up every frame.
- Reduces allocator pressure in scenes with many active `AnimationPlayer`
  nodes.
- Reduces hash probes when animations transition, seek, and resolve queued
  auto-advance entries.
- Avoids repeated checked bind-pose access while uploading skinned bone
  transforms each frame.
- Reduces scene-thread command queue pressure for animated crowds by replacing
  dozens of per-bone render-server calls per skinned mesh with one bulk upload.
- Reduces scalar arithmetic in `AnimationMixer` transform blend accumulation
  for animated skeleton and `Node3D` position/scale tracks.

## Validation

- Windows editor dev build.
- Targeted Windows editor dev object rebuild for `animation_mixer.cpp`.
- RTGI runtime smoke suite.
- Static adversarial review of playback, stopping, queued transition,
  animation change behavior, skeleton skin upload bounds, and queued bulk
  upload data lifetime.
