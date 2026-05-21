# Forward+ Rendering Hot-Path Tuning

## Goal

Tighten active Forward+ runtime paths instead of only removing unused code.

## Code Changes

- `servers/rendering/rendering_light_culler.cpp`
  - Adds direct AABB support-point distance checks for culling planes.
  - Avoids full `AABB::project_range_in_plane()` work when only the minimum
    signed distance is needed.
- `servers/rendering/renderer_rd/cluster_builder_rd.cpp`
  - Batches consecutive same-geometry cluster volumes into instanced draws.
  - Reduces repeated vertex/index array binding for common volume runs.
- `servers/rendering/rendering_server.cpp`
  - Lowers the fork default
    `rendering/limits/cluster_builder/max_clustered_elements` from `512` to
    `256`.
- `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp`
  - Reuses the per-viewport RT uniform set while the bound resource signature is
    unchanged. The params buffer still updates every frame, but descriptor
    allocation/free churn is avoided on stable frames.
  - Skips RT light-buffer uploads when the packed RT light contents are
    unchanged from the previous frame.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
  - Deduplicates RT buffer dependencies before submitting them to the
    raytracing list, reusing a renderer-owned scratch set to avoid repeated
    synchronization entries for shared material and geometry buffers without
    allocating a new hash table each dispatch.
  - Skips the duplicate pre-TLAS dirty-geometry refresh/check and RT-only
    motion-vector aging on RTGI-off Forward+ frames. Raster list filling still
    performs its normal dirty-cache refresh before drawing, while RTGI frames
    retain the pre-TLAS refresh that TLAS building needs.
  - Skips transparent-pass uniform, framebuffer, and draw-list setup when the
    alpha render list is empty. Pre/post-transparent compositor callbacks still
    run so compositor effects keep their ordering.
- `servers/rendering/renderer_rd/effects/ss_effects.cpp`
  - Allocates per-viewport SSAO importance-map textures, builds adaptive
    gather uniform sets, and resets the importance-map counter only for Ultra
    quality, the only preset that uses the adaptive importance-map gather path.
    High and lower SSAO qualities still keep the shared shaders/pipelines, but
    avoid the unused render-buffer textures and per-frame counter update.
- `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`
  - Caches glow environment settings once per post-process pass instead of
    repeatedly querying storage and copying the glow-level vector during glow
    blur and tonemap setup.

## Pros

- Reduces scalar math in culling loops.
- Reduces driver-facing state churn in clustered volume generation.
- Reduces RTGI descriptor, buffer upload, and dependency-submission churn.
- Reduces RTGI-off Forward+ CPU work in scenes that still provide RT cull lists
  or would otherwise pay the extra pre-TLAS dirty-cache check.
- Avoids no-op transparent pass submission in opaque-only frames.
- Reduces SSAO memory and driver/resource churn for non-Ultra presets.
- Reduces per-frame CPU overhead in glow-heavy scenes without changing the glow
  texture chain or tonemap inputs.
- Keeps optimizations in active Forward+ paths rather than relying only on build
  pruning.

## Cons

- The cluster-element default assumes scenes are tuned for a narrower desktop
  game profile.
- Instanced batching depends on adjacent render elements sharing geometry; mixed
  light/decal/probe order gains less.
- Culling changes should be revalidated if visibility fade behavior is changed.
- SSAO quality changes from Ultra to a lower preset clear the SSAO context to
  drop stale importance-map textures. Changes from a lower preset to Ultra reuse
  the existing context and add the missing Ultra-only textures.

## Rejected In This Pass

- `servers/rendering/renderer_scene_cull.cpp`
  - A squared-distance visibility-range variant was reviewed and benchmarked
    but not shipped. Exact boundary parity required fallback `sqrt()` checks,
    and the RTGI-off validation benchmark did not show a usable win.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
  - A faster-profile viewport-TAA-only motion-pass skip was reviewed and
    benchmarked but not shipped. The validation benchmark did not show a win,
    and static-heavy TAA scenes can regress when every opaque draw writes
    motion vectors in the main pass.
