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

The scaled Full Path Tracing reconstruction now has a full-resolution material
guide prepass, but final radiance is still reconstructed as a composited color
signal. Do not apply albedo demodulation/remodulation to that composited signal:
it mixes direct, emissive, indirect, and specular energy and regressed Euphorica
temporal sparkle in quick yaw captures. The current cache layer instead moves
the diffuse signal itself into lighting space before ASVFG: it demodulates the
primary albedo before cache update/reconstruction, then remodulates the
filtered lighting when writing the diffuse layer back. Scaled Full Path Tracing
can then reconstruct diffuse and specular split outputs separately and composite
them with full-resolution material guides, keeping the material operation on a
lighting boundary instead of on final color.

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

## Bounded Cache Validation Notes

The bounded implementation keeps the cache screen-space and limited to split-signal RTGI diffuse before ASVFG, but moves persistent radiance/meta/stats history into a cache atlas capped by `Environment.rtgi_diffuse_radiance_cache_max_entries`. Full-resolution `cache_raw_diffuse`, `cache_filtered_diffuse`, hit/confidence, age, and rejection diagnostics remain available, while the persistent HDR history size is derived from the configured entry budget instead of output resolution.

Reuse remains primary-reprojection first during cache update. Full-resolution reconstruction searches the local cache neighborhood and candidates must pass history-id, previous-validity, normal, depth, hit-distance, variance, confidence, age, and radiance-delta checks. The variance-guided weighting is dynamic: stricter firefly control engages for mature, high-confidence cache history when the current diffuse sample is an unsupported local bright outlier. This avoids a scene-specific cutoff while preserving Cornell's broad bright wall energy.

The bounded cache now stores a coarse receiver-surface ID in a dedicated
persistent cache atlas alongside radiance, guide, and statistics data. This ID
is separate from the stricter RT history ID used by TAA/reprojection. It is
derived from quantized world position, normal, roughness, and albedo proxy so
diffuse GI reuse can survive small path-traced hit/history changes without
bleeding across unrelated surfaces.

Each screen-space cache cell stores four receiver slots. Cache update traces
four representative receiver positions per cell and reconstruction searches the
neighboring cells across all slots. This moves the cache closer to Lumen's
screen-probe idea: a sparse cell can carry several surfaces before temporal
reuse and full-resolution reconstruction make their decisions, instead of
collapsing edges and subpixel surfaces into one low-resolution representative.
Cache update and reconstruction both reject entries whose cached receiver
identity does not match the current surface unless the entry is mature and
passes stricter guide/radiance checks, so sparse reuse no longer relies only on
similar normal/depth/hit-distance guides when the camera moves across adjacent
surfaces.

The quality harness reports the configured cache entry budget, derived cache
dimensions, receiver slot count, persistent entry count, persistent history
bytes, and full-resolution output/diagnostic bytes alongside the existing cache
hit, age, confidence, rejection, variance, sparkle, and source attribution
metrics. This keeps cache memory and bandwidth changes visible when comparing
cache-off versus cache-on captures.

The current reconstruction resolves a weighted neighborhood of valid cache entries instead of selecting one best cache cell. Each candidate still passes the same guide and history checks, but valid neighbors are blended by quality and cache-space proximity before the current diffuse sample is mixed with cached history. The accepted neighborhood also carries a weighted luma-coherence score; inconsistent cache neighborhoods reduce reuse even when individual entries pass. This reduces cell-to-cell popping during camera motion without allowing invalid or conflicting history to bleed across guide edges.

The cache now stores lighting-space diffuse history rather than primary-albedo
modulated diffuse color. Candidate comparisons, outlier clamps, persistent
cache history, and neighborhood reconstruction operate on the demodulated
lighting value; the final cache output is remodulated with the current
albedo/metalness guide before ASVFG. Cache reuse is additionally reduced for
high screen-space velocity so the first camera-motion frame after a static
warmup does not reuse mature lighting history as aggressively.

Cache update also integrates a small guide-coherent current-frame receiver
neighborhood before temporal reuse. Combined with the four receiver slots, this
turns each bounded cache cell into a lightweight screen-space probe: coherent
neighboring diffuse samples can contribute to each slot's current lighting
estimate, while receiver ID, strict history fallback, normal, depth,
hit-distance, signal-risk, and luminance-coherence gates prevent obvious
cross-surface bleeding. The intent is to let the cache, ASVFG, and
full-resolution split reconstruction work together instead of measuring each
layer as an isolated filter.

The cache now also hands a compact confidence/risk texture to diffuse
full-resolution reconstruction. That mask combines cache hit state, accepted
cache stability, age, variance, rejection, and the source RT signal confidence,
so reconstruction preserves stable cached lighting but smooths cache misses and
disocclusions more broadly.

Source signal risk is treated as reduced confidence rather than a hard cache
entry failure. Valid guided diffuse samples keep a confidence floor, allowing
the radiance cache to accumulate noisy GI instead of rejecting it before the
variance and radiance-delta gates can decide whether history reuse is safe.
Reconstruction can also use mature same-surface candidates when exact cache
history IDs differ. Those relaxed candidates must pass stricter normal, depth,
hit-distance, variance, radiance, age, and confidence gates, and are
downweighted compared to exact matches. This gives the cache a small amount of
surface-cache behavior for adjacent receiver triangles without allowing broad
lighting leaks.

The cache now participates before the denoiser as well as before ASVFG:
secondary rough diffuse hits in Full Path Tracing can project the hit point into
the previous frame and query the receiver cache directly. The lookup is exact on
receiver-surface ID and still checks previous-view depth, camera distance,
normal, variance, age, and confidence. When accepted, the cached demodulated
lighting contributes as a bounded low-frequency continuation and the remaining
path throughput is reduced by the same weight. This makes the cache ray-visible
for surfaces that were recently on screen, which is closer to a Lumen
Screen Probe Gather plus Surface Cache chain than a pure post-ray denoiser.

The secondary-hit lookup now has an explicit Lumen-like handoff: screen-visible
receiver cache first, then STRC/world-radiance fallback only when the receiver
match is missing or too weak. STRC is capped, does not terminate the path, and
must contain nonblack radiance before it can reduce path throughput. This avoids
the earlier failure mode where a confident but black probe sample could consume
the bounce and darken the result.
The internal fallback path also overrides the disabled public STRC settings with
a compact `16^3`, three-cascade, higher-update-budget cache so the atlas can
warm during normal camera-motion captures. Probe alpha now tracks radiance
validity rather than mere sample lifetime, which keeps `strc_confidence` aligned
with usable fallback lighting.
Probe updates now tag direct, emissive, sky, indirect, dynamic, black-radiance,
and no-source cases in cache metadata. The STRC resolve keeps previous lighting
when an update has no usable source instead of blending black over valid cache
history, and secondary-hit sampling weights STRC entries by that source quality.
This is the provenance layer needed before a larger Screen Probe Gather can use
STRC as a low-frequency prior instead of a blind scalar-confidence fallback.
The STRC update path also now carries the selected atlas texel from raygen into
the resolve result. That decouples update scheduling from the compute resolve
and allows a split budget: a camera-forward, view-demand subset refreshes nearby
visible cascade texels first, while the remaining rays keep the previous
cascade-weighted sweep alive for broad world coverage. In short smoke captures
this is expected to behave more like infrastructure than a direct quality win;
it reduces the chance that later screen-probe or receiver-cache misses fall back
to stale visible-world radiance.
The surface-feedback bridge now carries provenance as well. Current-hit direct,
explicit emissive, conservative sky-visible, STRC-prior, and mixed feedback are
tagged before the compute consumer sees them. STRC feedback also preserves
direct/emissive/sky provenance instead of collapsing every probe result into a
single world-prior class. The surface atlas stores that source class in page
stats, and ray-side surface lookup applies source-aware quality caps.
Receiver/current direct and emissive pages can mature into stronger reuse,
while sky and STRC-prior pages remain weak seeds until a better source refreshes
the same page. This is still a hashed demand cache, not a Lumen card capture,
but it prevents low-frequency world-prior data from being treated as equal to
material-space current-hit radiance.
Visible receiver-cache promotion also now has source selection instead of a
single receiver label. The promotion pass maps each persistent cache slot back
to its representative full-resolution receiver pixel, reads that pixel's current
normal/roughness guide, and can choose receiver, refined screen-probe, or base
screen-probe lighting as the surface-page source. A stable visible current
sample can also win when the full-resolution guide is valid, the current strict
surface key still matches the demanded page key, signal risk is low, receiver
variance is controlled, and the current demodulated diffuse lighting agrees with
receiver history. Refined/base SPG pages keep lower provenance quality caps than
receiver/visible-current/current-hit sources, so they can stabilize undersampled
pages without maturing into full-trust card data.
The first upstream sampling hook now uses that provenance too. Rough
single-sample primary diffuse continuations use a screen-probe-like
low-discrepancy sequence when RTGI is scaled below native resolution. The
current base probe spacing covers eight scaled-RT pixels, while angular bins use
a separate `4x4` direction tile. The selected direction can then consult
previous STRC radiance and gently steer toward the strongest valid
normal-oriented world-radiance candidate for the receiver layer.

That direction is now carried into the diffuse cache as a directional
screen-probe gather V1. The cache builds a `4x4` angular radiance tile for each
scaled-RT probe from demodulated diffuse lighting, primary rough-diffuse
direction, guide normal, view-depth, receiver identity, and signal confidence.
Each bin now also carries SPG-local stats: temporal age, support, radiance
coherence, and depth-plane quality. Bin construction chooses a surface anchor
inside the probe cell and downweights samples that disagree in receiver
identity, normal, view depth, or roughness, which is the first lightweight stand
in for the plane/visibility weighting used by larger screen-probe systems. A
paired visibility atlas now stores representative hit distance, hit-distance
coherence, receiver view-depth, and support for each direction bin.
Full-resolution reconstruction samples this atlas before STRC fallback, using
surface compatibility, hemisphere weighting, SPG stats quality, and a soft
visibility/hit-distance downweight to preserve more directional GI than the
scalar receiver cache can provide. A deterministic refined layer now sits on top:
base probes whose stats, visibility, geometry, or radiance variation indicate
undersampling get a stable `2x2` refined subprobe layout with a `2x2` angular
tile. The refined layer reuses the base atlas texel budget, has hysteresis in
the refinement mask, and blends conservatively before base SPG so it acts as a
local detail assist rather than a global replacement. It remains an internal
screen-space layer, not a complete Lumen Screen Probe Gather: first-pass
surface-page feedback now exists, but there is no explicit surface-card
parameterization, compacted page queue, or full world-radiance-cache miss chain
yet. The
ray-side secondary diffuse path now samples the refined SPG hierarchy for risky
receiver misses before falling back to base SPG or STRC, so refinement is no
longer limited to final reconstruction.
The expected benefit is reduced visible half-res directionality loss in Path
Tracing once the confidence/reconstruction/TAA layers work together; isolated
probe bins can still look softer or stale if they are evaluated without the
rest of that chain.
The same diagnostics now feed refinement instead of only explaining it after the
fact. The diffuse cache refinement mask reads secondary-cache source/rejection
feedback, so accepted screen traces, screen misses, low-weight screen hits, weak
SPG hits, and weak surface-cache hits can request refined probes for the next
frame. Current-screen hits with a strict surface key can also write bounded
receiver/STRC/SPG-sourced surface feedback, which closes the first feedback loop
between screen traces and surface pages.
Ray-side accepted-source attribution is now available through the
`secondary_cache_source` debug view. It tells whether a Path Tracing secondary
diffuse continuation used the receiver cache, STRC, base SPG, refined SPG, or
the surface cache, and records the accepted cache weight and cached luminance.
Ray-side miss attribution is available through `secondary_cache_rejection`,
which records the dominant failed layer or current-screen trace failure without
feeding that diagnostic back into the cache. Surface-cache lookup has a
separate `secondary_cache_surface` diagnostic for no key, no page,
collision/id mismatch, stale, low confidence/support/variance, dynamic
ineligibility, normal mismatch, weak radiance, weak quality, early-source, and
accepted query states. It also reports the accepted or early-source class in
RGB-captured metrics, so receiver/base-SPG/refined-SPG/visible-current/STRC/
surface-page query fractions can be tracked separately. This keeps the
raytracing pass write-only for the diagnostic outputs while exposing why the
path fell through to stochastic continuation.

The atlas is also now visible to the next frame's secondary diffuse cache query.
Path Tracing still prefers exact receiver-cache hits; only receiver misses or
near-zero receiver coverage fall through to the directional screen-probe atlas
before STRC. That makes the screen-probe layer part of the GI path itself
instead of only a post-trace smoothing pass. Its contribution is identity-,
normal-, depth-, hemisphere-, roughness-, plane-, history-quality-, and
confidence-gated, and it is capped below the scalar receiver cache so a partial
atlas does not replace reliable surface-local reuse.
There is also now a conservative screen-trace-first shortcut for rough diffuse
continuations. Once the BRDF direction is known, the shader ray-marches that
direction through the current raster depth and normal guides, then samples the
receiver/STRC cache at the screen hit and applies only a capped fraction of
that cached continuation. The rest of the throughput keeps tracing. This is the
first piece of Lumen's screen-trace/fallback stack: it can cover visible-surface
mismatches before the path falls through to the world cache or triangle scene,
but it is not expected to match a full Screen Probe Gather until adaptive probes,
visibility weighting, and a stronger surface/world lighting cache exist.

Scaled RTGI reconstruction now also produces full-resolution history-validity
and history-ID gates for the post-reconstruction TAA pass. The gates are derived
from the low-resolution RT history guides and invalidated at ambiguous support
edges, so the final reconstructed history can be rejected by RT identity rather
than only by velocity/depth and the reactivity mask.

The remaining structural gap is the quality and directionality of the gather
chain itself. The current STRC guide is still a coarse camera-centered probe
atlas, and the new surface cache is a hashed world-page bridge rather than a
true card/page capture system with compacted update queues, fixed page budgets,
ray-cone filtering, or material captures. Until those pieces exist, the world
fallback and the small direction guide can reduce view-dependent screen-cache
misses but may not visibly improve every metric, and they can still look softer
or laggier than a true Surface Cache plus World Radiance Cache chain.

The current research-backed divergence list is:

- Keep probe spacing separate from directional resolution. The current defaults
  are eight scaled-RT pixels per base probe and a `4x4` angular tile per probe,
  but larger Screen Probe Gather designs treat both as independent quality
  levers.
- Extend SPG debug visibility from atlas coverage to rejection reasons. The
  current dedicated views expose atlas radiance, confidence/support, packed
  age/support/coherence/plane-quality stats, plane quality, and the directional
  visibility/hit-distance atlas. The full-resolution SPG rejection view now
  records the dominant reconstruction rejection class, sample strength, gathered
  support, and accepted SPG confidence. The refinement mask and refined
  confidence views show where the deterministic refined layer activates.
  Ray-side reuse now consumes that refined hierarchy, accepted-source
  attribution is visible through `secondary_cache_source`, and matching
  ray-side miss/rejection-reason attribution is visible through
  `secondary_cache_rejection`.
- Feed current-screen hits into the SPG/source decision before stale cache
  reuse. The screen-trace-first shortcut exists, but it does not yet update or
  validate the SPG atlas itself. Code exploration found that doing this in the
  same frame would require pass reordering or a dedicated ping-pong feedback
  resource, because ray-side reuse samples previous receiver/SPG state before
  `RTGIDiffuseCache::process()` writes the next state.
- Add adaptive probes or refinement where interpolation fails. Fixed probe
  cells can still expose half-resolution structure when geometry density or
  disocclusion changes faster than the uniform atlas can represent.
- Build a stronger surface-cache bridge. The current bridge uses coarse
  world-space page keys, per-pixel demand feedback, STRC-seeded direct/emissive
  priors, deterministic demand-feedback budget gating, selected/skipped/starved
  diagnostics, and a small associative hashed atlas. It still needs separate
  stale/background page budgets, compacted page queues or exact SSBO counters,
  and a real card/page capture model before it can behave like Lumen's Surface
  Cache.

The next larger Lumen-like layer should continue hardening the persistent
surface-space radiance cache, not broaden screen-space blur. Lumen's Surface
Cache stores material and lighting for nearby scene surfaces and lets ray hits
query stable offscreen lighting; this repo now has the first hashed
surface-page bridge, but not explicit card/page coverage. The next iteration
should compact feedback, split demand/stale/background update budgets, add exact
page counters where needed, and improve radiance provenance for visible and
demanded pages. Interim layers can look worse in isolation because they trade
random noise for structured stale-cache bias; the full chain needs screen
traces, screen probes, a surface cache, and a world fallback to validate each
other.

Rejected reconstruction experiments from the Euphorica path-traced yaw harness:

- Previous-cache 3x3 neighborhood stealing improved neither stability nor
  low-frequency error; exact cache identity remains better.
- STRC cone-direction gather worsened the forced-STRC quick capture when it was
  an unconditional contribution. STRC is now a capped secondary-hit fallback,
  and black-radiance samples no longer consume path throughput. The remaining
  work is improving the stored world-radiance signal, not making the fallback
  more aggressive.
- Final material-guide weighting during full-resolution reconstruction
  increased RMSE and edge energy. Material awareness should be applied at a
  split diffuse/specular lighting boundary, not as a late color-weight tweak.
- Screen-cache representative probe selection and cache-miss relaxed
  current-frame interpolation both regressed the Path Tracing `rs0_50` yaw case.
- Coarse receiver-probe RNG for the first diffuse BRDF continuation made the
  current combined stack slightly worse and did not show a clear visual upside.
  The replacement is a bounded screen-probe-like 4x4 cell sequence, which keeps
  the 16-direction low-discrepancy set but shares it spatially across scaled-RT
  pixels instead of reseeding a broad receiver-probe RNG.

The procedural raster-GI history-key caveat remains: mesh and multimesh history ids carry raster ownership, but procedural or atypical paths should continue to be treated conservatively until a cheap local ownership signature is available.
