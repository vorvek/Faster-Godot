# RTGI Diffuse Radiance Cache Next Iteration

This is the next RTGI upstream reconstruction step after explicit analytic and emissive source sampling. It is intentionally not implemented in the current emissive reconstruction phase. ASVFG remains the final cleanup stage for the default denoiser path; the cache is meant to reduce raw diffuse variance before denoise.

## Goals

- Reduce raw diffuse p99, visible speckles, fireflies, and frame-delta sparkle before ASVFG.
- Preserve RTGI-off behavior and raster GI coexistence.
- Keep path-traced SDFGI exclusive behavior isolated from hybrid raster GI paths.
- Provide clear per-source attribution proving whether cache reuse reduces upstream diffuse instability.

## Data Layout

Use a screen-space tile cache first. A sparse world/probe grid can follow only if screen-space reuse proves insufficient.

Each cache entry stores:

- Diffuse radiance, preferably RGB16F.
- View-space or world-space normal.
- Depth and RT hit distance.
- Material/history id.
- Raster GI ownership/classification bits.
- Age/history length.
- First and second moments or compact variance.
- Confidence.
- Last update frame index.

Use a fixed-size per-viewport buffer sized from render resolution and tile size, for example 8x8 or 16x16 pixels. Keep the layout independent from ASVFG history buffers so cache invalidation and metrics can be measured separately.

## Update Cadence

Update a bounded fraction of cache entries each frame, with a deterministic rotating pattern plus priority refresh. Force refresh on:

- Camera cuts or large camera motion.
- RTGI mode or relevant quality knob changes.
- Material, emissive candidate, or light signature changes.
- Low confidence.
- High variance.
- Large radiance delta versus the current sample.
- Disocclusion or invalid reprojection.

Entries that are not refreshed can contribute only when reprojection validity passes and confidence remains above the configured threshold.

## Reprojection And Validity

Reject cache reuse on:

- Disocclusion.
- Normal mismatch above the existing RTGI history tolerance.
- Depth or hit-distance mismatch.
- Material/history id change.
- Dynamic object/history id change.
- Raster GI ownership mismatch.
- Large radiance delta after exposure-normalized comparison.
- Low confidence or high variance.

Downweight rather than fully reject for small normal/depth disagreement, short history, or mild radiance drift.

## Dynamic Object Handling

Prefer conservative rejection for dynamic objects in the first implementation. Dynamic instances should either:

- Carry a history id that invalidates on transform/material changes, or
- Bypass diffuse cache reuse until stable-object classification is available.

Do not cache raster GI owner primary contribution in hybrid mode.

## Integration Point

Integrate before ASVFG:

1. Generate direct, explicit emissive, sky, and raw indirect signals.
2. Resolve or blend the diffuse radiance cache estimate for eligible diffuse hits.
3. Feed the cache-refined diffuse estimate into the existing RTGI signal split.
4. Let ASVFG continue as the final temporal/spatial cleanup pass.

The cache must be disabled by a separate internal guard during development so direct comparisons can capture raw 1-spp, explicit emissive only, cache only, and cache plus ASVFG modes.

## Metrics

The quality harness should report full-frame and ROI metrics for cache-off versus cache-on:

- Raw diffuse luma mean, p95, p99, max.
- Visible speckles per megapixel.
- Fireflies per megapixel.
- Temporal sparkle max and average.
- Cache hit rate.
- Cache rejection reasons by category.
- Cache age distribution.
- Confidence distribution.
- Euphorica lantern/dark-wall/final-post ROIs.
- Many-light/emissive dark-wall and specular/detail ROIs.
- High-spp reference deltas in the controlled validation scene.

Acceptance requires measurable raw diffuse variance reduction before ASVFG, not just cleaner final denoised output.

## Raster GI Coexistence Risks

- Hybrid raster GI surfaces must not reuse cached path-traced diffuse primary contribution as if it were raster GI.
- VoxelGI, SDFGI, lightmap, and lightprobe paths need explicit coexistence coverage.
- Path-traced SDFGI exclusive mode must keep its current isolation and invalidation behavior.
- Cache signatures should include raster GI ownership bits and the emissive candidate/light signatures used for the current frame.

## v2 Validation Notes

The v2 implementation keeps the cache screen-space, full resolution, per viewport, and limited to split-signal RTGI diffuse before ASVFG. It adds a post-cache pre-ASVFG debug draw named `cache_filtered_diffuse`, diagnostic metric extraction for cache hit/confidence/age/rejection buckets, and a per-render-buffer signature that clears the cache when relevant RTGI mode, quality, sampling, render-size, view-count, or RT radiance-history state changes.

Reuse remains primary-reprojection first. Stable-neighborhood recovery is limited to a five-tap cross after geometric or reprojection failure, and candidates must pass history-id, previous-validity, normal, depth, hit-distance, variance, confidence, age, and radiance-delta checks. The variance-guided weighting is dynamic: the stricter firefly-control path only engages for mature, high-confidence history when the current diffuse sample is a local bright outlier over stable darker history. This avoids a scene-specific cutoff while preserving Cornell's broad bright wall energy.

The current many-light fixture remains source-attributed to confidence/clamp-risk with visible emissive and indirect source involvement. The cache hit rate is very low in that scene, and cache-on/cache-off temporal max remains `61.73/MP`; forcing broader diffuse clamping would risk visible emissive surfaces. This phase therefore documents the bottleneck rather than adding a broad emissive/direct clamp.

The procedural raster-GI history-key caveat remains: mesh and multimesh history ids carry raster ownership, but procedural or atypical paths should continue to be treated conservatively until a cheap local ownership signature is available.
