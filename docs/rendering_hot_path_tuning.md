# Forward+ Rendering Hot-Path Tuning

## Goal

Tighten active Forward+ runtime paths instead of only removing unused code.

## Code Changes

- `servers/rendering/rendering_light_culler.cpp`
  - Adds direct AABB support-point distance checks for culling planes.
  - Avoids full `AABB::project_range_in_plane()` work when only the minimum
    signed distance is needed.
- `servers/rendering/renderer_scene_cull.cpp`
  - Uses squared distances for visibility range culling.
  - Computes square root only when fade alpha needs linear distance.
- `servers/rendering/renderer_rd/cluster_builder_rd.cpp`
  - Batches consecutive same-geometry cluster volumes into instanced draws.
  - Reduces repeated vertex/index array binding for common volume runs.
- `servers/rendering/rendering_server.cpp`
  - Lowers the fork default
    `rendering/limits/cluster_builder/max_clustered_elements` from `512` to
    `256`.

## Pros

- Reduces scalar math in culling loops.
- Reduces driver-facing state churn in clustered volume generation.
- Keeps optimizations in active Forward+ paths rather than relying only on build
  pruning.

## Cons

- The cluster-element default assumes scenes are tuned for a narrower desktop
  game profile.
- Instanced batching depends on adjacent render elements sharing geometry; mixed
  light/decal/probe order gains less.
- Culling changes should be revalidated if visibility fade behavior is changed.
