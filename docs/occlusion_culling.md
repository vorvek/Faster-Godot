# Occlusion Culling And Embree

## Goal

Keep Godot's Embree-backed viewport occlusion culling enabled and stable while
the rest of the fork narrows around Forward+ desktop rendering. Fixed cameras,
rooms, tunnels, and dense static scenes are expected to benefit from this path
as content density grows.

## Code Changes

- `modules/raycast/godot_update_embree.py`
  - Updates the vendored source tag from Embree 4.4.0 to 4.4.1.
- `modules/raycast/SCsub`
  - Keeps Embree's base objects on the stable upstream/Godot dispatch profile
    instead of inheriting Faster-Godot's global AVX2/FMA flags.
  - This avoids incorrect Embree template instantiation and a Windows rendered
    viewport fast-fail observed while testing AVX2/AVX512/BVH8 variants.
- `modules/raycast/raycast_occlusion_cull.*`
  - Already batches camera visibility rays as `RTCRayHit16` tiles and calls
    `rtcIntersect16`.
  - Keeps scheduling on Godot's `WorkerThreadPool`; no OpenMP runtime is added.
- `thirdparty/embree`
  - Refreshes vendored Embree files to 4.4.1.

## Pros

- Lets fixed-camera scenes, rooms, tunnels, and hallways get more value from
  occlusion culling as static mesh density grows.
- Preserves Godot's existing batched `rtcIntersect16` viewport path.
- Avoids disabling occlusion culling while the fork removes other rendering
  paths.
- Keeps the validated release path stable on Windows and Linux.

## Cons

- The AVX2/AVX512 Embree experiment is not shipped yet. The attempted wider
  Embree object set built, but rendered viewport occlusion fast-failed on the
  Windows benchmark path and was backed out.
- Static raycasters and lightmap raycasters still use Embree's single-ray API.
- Projects still need correct occlusion bake/setup work to benefit from the
  feature.
