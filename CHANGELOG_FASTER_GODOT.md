# Faster-Godot Changelog

Release notes for what Faster-Godot changes on top of its Godot 4.6.3-stable
base. Upstream Godot's own changelog is in [CHANGELOG.md](CHANGELOG.md); the
per-area ledger of how the fork differs from official Godot is in
[CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md).

## 4.6.4a

### Build

- The release editor now ships on the clang and ThinLTO toolchain, the same one
  the export templates already use, on Windows and Linux. Testing a project from
  the editor runs the editor's own renderer, GDScript VM, and physics, so this is
  where the 12 to 22 percent clang gains land for day-to-day development. The
  default source build stays on MSVC and GCC for fast iteration and native
  debugging.

### Performance

#### RTGI Resolve

These changes tune the per-pixel screen-GI resolve, the pass that turns the
screen-probe gather and the world-radiance cache into the final indirect
lighting. Each one leaves the rendered frame bit-for-bit identical. The numbers
below come from an RTX 4080 SUPER, measured with pre-warmed before/after
comparisons.

- The Integrate pass, which dominates the resolve cost, now walks each
  surrounding probe's octahedral tile once instead of twice. The diffuse cosine
  integral and the rough-spec cone prefilter were fetching and decoding the same
  texels separately; folding them into one walk cuts the pass by about 30 percent.
- Each resolve mode now compiles as its own pipeline, so one mode's register and
  shared-memory use no longer lowers the occupancy of the others.
- The edge-aware spatial filter reads each tap's surface once with cheaper math.
  It uses view-space normals (the edge test is rotation-invariant, so the
  world-space rotation is gone), a two-term linear depth taken straight from the
  inverse projection, and roughness read from the normal buffer it already
  samples, which drops one texture fetch per tap. That is about 17 percent off
  the spatial pass.
- The spatial filter caches its working tile in shared memory for the default
  filter step, so the 5x5 neighborhood reads from on-chip memory instead of
  refetching from global memory per tap. That is about another 35 percent, for
  roughly 45 percent off the spatial pass in total.
- The spatial filter now lives in its own shader, so the other resolve pipelines
  no longer reserve its shared-memory tile. That gave back occupancy the shared
  cache had quietly cost the Integrate and Temporal passes.
- A GPU profiler timestamp that the resolve issued inside an active compute list,
  which the RenderingDevice does not allow, now sits outside it. Before, it logged
  an error every frame and dropped the spatial pass from the profile.

#### Forward+ rendering

- The Forward+ area-light path is now gated behind a specialization constant, so
  scenes with no area lights skip that branch in the opaque shader. Measured at
  about 0.55 ms off the opaque pass, roughly 36 percent, on the area-light test
  scene.

### Fixes

- RTGI now renders correctly with 3D MSAA enabled. The material-guide prepass that
  feeds the GI resolve attached the viewport's resolved depth buffer as a render
  target. With MSAA on, that buffer is a sampling and storage resolve target with
  no attachment usage, so the prepass framebuffer failed to build every frame. The
  prepass was then skipped, its guide buffer stayed at the white value it clears to,
  and the composite multiplied the frame by that white guide, which produced the
  all-white screen in the report. The prepass now renders into its own single-sample
  depth-stencil attachment when MSAA is on, matching its single-sample guide buffers.
  All three RTGI modes (Hybrid, Reflections, and Full Path Tracing) render with MSAA
  enabled, and the non-MSAA path is unchanged.
- NaN serialization and convex-segment intersection now stay correct under the
  fork's fast floating-point build. A raw NaN comparison that fast FP folds away
  was replaced with an explicit NaN predicate, fixing JSON round-tripping of NaN
  and a convex segment-intersection edge case.
