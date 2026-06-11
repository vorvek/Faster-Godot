# RTGI Quality Harness

This project is a small Forward+ Vulkan RTGI scene set for denoiser tuning. It
is intended to catch the regressions that are hard to judge from a single game
screenshot:

- isolated hot pixels in dark indirect-light regions;
- loss of high-frequency brick/stone detail at high denoiser strength.
- glossy or metallic surfaces that smear, sparkle, or retain stale history.
- ShaderMaterial fallback behavior when a material defines `light()`.
- Cornell Box projection, color-bounce, and reference-image drift.
- Sponza-style high-frequency geometry and texture detail.

Run it with the locally built editor:

```powershell
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --scene res://scenes/rtgi_quality_main.tscn --rtgi-denoise-strength=1.0
```

The harness writes captures and metrics to `user://rtgi_quality` by default.
Use `--rtgi-output-dir=<path>` for an explicit output directory. The runner
disables vsync at startup and uses a convergence-aware settle instead of a
fixed warmup: it renders at least 45 frames after the scene load (120 after an
in-process mode switch), then stops as soon as the whole-frame mean luma
stabilizes, capped at the `--rtgi-warmup-frames` value. Each metrics JSON
records the frame count actually used as `settle_frames_used`.

Useful options:

- `--rtgi-scene=stress|cornell|sponza|sdfgi|voxelgi|lightmap|lightprobe|path_traced_sdfgi_exclusive|cornell_box|specular_motion|reflective_pool|fog_corridor`
- `--rtgi-denoise-strength=0.0..1.0`
- `--rtgi-history-weight=0.0..0.98` (default `0.95` for split diffuse)
- `--rtgi-firefly-suppression=0.0..1.0`
- `--rtgi-detail-preservation=0.0..1.0`
- `--rtgi-split-signals=true|false`
- `--rtgi-specular-history-weight=0.0..0.98` (default `0.90`)
- `--rtgi-specular-spatial-strength=0.0..1.0` (default `1.0`)
- `--rtgi-ray-firefly-suppression=0.0..1.0`
- `--rtgi-ray-max-radiance=0.0..4096.0`
- `--rtgi-diffuse-cache-max-entries=4096..4194304`
- `--rtgi-warmup-frames=120` (upper bound; the settle stops early once the
  image converges, and the value also acts as the floor when it is smaller)
- `--rtgi-reference-spp=16`
- `--rtgi-debug-view=beauty|final|noisy|diffuse_noisy|specular_noisy|diffuse_final|specular_final|specular_guide|normal_roughness|normal_deviation|viewz_hitdist|motion_vectors|variance|history_length|rejection|disabled`
- `--rtgi-gate-profile=strict|smoke`
- `--rtgi-capture-debug` (diagnostic-only: writes every debug-view PNG and runs
  the per-view metric sweep, which re-renders and pixel-scans dozens of views
  and adds several minutes per run; off by default, regression runs do not
  need it and none of its outputs feed the gate thresholds)
- `--rtgi-capture-comparison`
- `--rtgi-cornell-compare`
- `--rtgi-cornell-reference-image=<path>`
- `--rtgi-sponza-path=<path-to-gltf-or-glb>`
- `--rtgi-sponza-normal-y=auto|opengl|directx` (default `auto`; `directx`
  flips imported Sponza normal-map green channels in the harness only)
- `--rtgi-camera-pan`
- `--rtgi-write-reference`
- `--rtgi-mode=hybrid|fpt|fpt-reference|reflections|off` (overrides the scene's authored RTGI mode; `fpt-reference` selects the deep-path FPT oracle for informational A/B runs; `off` disables RTGI for the run so per-scene comparisons can record a raster reference; left untouched when absent)
- `--rtgi-modes=<comma list>` (multi-config run: one process and one scene load
  measure every listed mode in sequence, e.g. `--rtgi-modes=off,fpt,hybrid`.
  Each mode settles, then writes its own `_<mode>`-suffixed metrics JSON and
  capture, and prints its FOGPAR/metric lines exactly like a single-mode run.
  The process exits non-zero if any config failed. List `fpt` before `hybrid`:
  full path tracing leans on the world radiance cache, so measuring it after
  another RTGI mode has warmed that cache reads a different steady state than
  the cold start the recorded baselines use)
- `--rtgi-denoiser=asvfg|reactive|none` (overrides the scene's RTGI denoiser; left untouched when absent)
- `--rtgi-resolution-scale=0.25..1.0` (scales the RTGI trace inside the 3D render; left untouched when absent)
- `--rtgi-upscaler=none|taa|fsr2|xess` (configures the root viewport scaling; `none`/`taa` use bilinear, `taa` also enables built-in temporal AA; left untouched when absent)
- `--rtgi-scale-3d=0.5..1.0` (the 3D render scale that the upscaler upscales from, separate from `--rtgi-resolution-scale`; left untouched when absent)

The mode, denoiser, and resolution-scale overrides apply to the scene's
environment after it is built; the upscaler and 3D-scale overrides apply to the
root viewport after any square-viewport forcing, so they are not clobbered. Every
run records its effective applied values plus per-run performance metrics
(`perf_gpu_frame_msec_avg`, `perf_gpu_frame_msec_min`, `perf_cpu_frame_msec_avg`,
`perf_video_mem_used_bytes`, `perf_draw_calls`) in the metrics JSON. GPU/CPU frame
times are sampled over the tail of the warmup loop (steady state); `*_min` is the
most-representative uncontended frame.

A typical sweep run combining the new flags:

```
... --rtgi-scene=specular_motion --rtgi-mode=fpt --rtgi-upscaler=fsr2 --rtgi-scale-3d=0.67 --rtgi-denoiser=reactive --rtgi-resolution-scale=0.67 --rtgi-fast
```

`--rtgi-capture-comparison` writes beauty-frame Full Path Tracing split-signal,
Full Path Tracing single-beauty fallback, Reflections RT Only, no-RTGI, and high-SPP reference
captures plus a comparison grid for visual
inspection. `final` is still captured by `--rtgi-capture-debug` as the denoised
RTGI debug buffer.

## Coexistence Modes

The `sdfgi`, `voxelgi`, `lightmap`, and `lightprobe` scenes validate Reflections RT Only
coexisting with raster GI owners. Each mode captures the RTGI frame and a
temporary no-RTGI raster fallback frame, then checks that the raster GI
contribution remains visible without a large Reflections RT Only diffuse boost.

`lightmap` builds a tiny procedural `Texture2DArray` and `LightmapGIData` at
runtime, so it does not require baked assets. `lightprobe` creates procedural
LightmapGI probe capture data for a dynamic object. `voxelgi` bakes a small
procedural VoxelGI during scene setup. `path_traced_sdfgi_exclusive` keeps SDFGI
enabled in the environment, but expects the Full Path Tracing frame to stay exclusive
while the temporary raster fallback still shows SDFGI lighting.

## Euphorica RTGI Capture

`scripts/euphorica_rtgi_capture.gd` runs against `D:\dev\euphorica` as the
active project, so it does not modify or depend on saved changes in the
Euphorica tree. Use a windowed Vulkan editor build because the harness captures
the live `GameViewport` and the final RF/post-processed root viewport.

```powershell
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\euphorica --rendering-driver vulkan --rendering-method forward_plus --script D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project\scripts\euphorica_rtgi_capture.gd --euphorica-output-dir=D:\dev\rtgi_phase4_outputs\euphorica_rtgi --euphorica-profile=compare --euphorica-warmup-frames=180 --euphorica-sparkle-frames=32 --euphorica-capture-debug
```

Profiles:

- `compare`: no-RTGI baseline plus Reflections RT Only, Full Path Tracing, glow/fog, lantern
  emissive, shadowed OmniLight, and normal-deviation debug captures.
- `split_ab`: no-RTGI baseline plus split-signal on/off pairs for Reflections RT Only and
  Full Path Tracing at `640x360` and `1280x720`, plus content toggles.
- `matrix`: full Reflections RT Only/Full Path Tracing sweep for denoise `0.90`, `0.95`,
  `0.98`, `1.0`, history `0.95`, `0.98`, `640x360`/`1280x720`, content
  toggles, and split-signal on/off comparisons. For Euphorica-centered RTGI
  work, use Cornell plus Euphorica validation; Sponza is optional and not part
  of the default phase gate.

Useful Euphorica split options:

- By default the harness keeps Euphorica's scene-authored `GameViewport` size
  (`1920x1080` in `res://scenes/main/main.tscn`). Use
  `--euphorica-resolution=<width>x<height>` only when intentionally running a
  lower-resolution stress test; `--euphorica-resolution=native` restores the
  scene-authored size.
- `--euphorica-split-signals=on|off|both` selects the denoiser topology. The
  comparison profiles default to `both`.
- `--euphorica-diffuse-cache-max-entries=4096..4194304` sets the bounded
  diffuse cache entry budget for each RTGI case.
- `--euphorica-strc=on|off|default` overrides Euphorica's scene-authored STRC
  setting for radiance-cache experiments. `default` preserves the project file.
- `--euphorica-strc-strength=0..1`,
  `--euphorica-strc-rays-per-frame=0..32768`,
  `--euphorica-strc-grid-size=12..32`,
  `--euphorica-strc-base-probe-spacing=0.25..8`, and
  `--euphorica-strc-temporal-weight=0..0.995` tune forced STRC runs.
- Euphorica captures default STRC visual layers to static layer `1` and dynamic
  layer `0`, so the world layer seeds STRC while character/dynamic layers stay
  out of the probe cache. Override with
  `--euphorica-strc-static-layers=<mask>` and
  `--euphorica-strc-dynamic-layers=<mask>` when testing layer classification.
- `--euphorica-rtgi-resolution-scale=0.25..1.0` sets one RTGI trace scale for
  each case.
- `--euphorica-rtgi-resolution-scales=0.5,1.0` sweeps multiple RTGI trace
  scales. Use this with `--euphorica-profile=reconstruction` to compare the
  half-resolution reconstruction path against full-resolution RTGI while still
  recording the already-scaled Euphorica `GameViewport` size separately.
- `--euphorica-camera-motion=yaw --euphorica-camera-motion-degrees=8` rotates
  the current `GameViewport` camera left-to-right during the capture frames.
  This is intended for Path Tracing motion-stability checks; it keeps the
  scene-authored `GameViewport` size unless `--euphorica-resolution` is also
  provided.
- `--euphorica-analysis-scale=0.5` keeps rendering and saved captures at the
  requested/native resolution, but computes expensive metrics on a downsampled
  copy. This is useful for native 1080p motion A/B runs where relative metrics
  matter more than full-resolution metric precision.
- `--euphorica-case-filter=<substring>` runs only matching cases, plus the
  no-RTGI baseline when needed, so long matrices can be resumed in chunks.
- `--euphorica-fast` is for shader-iteration smoke checks. It uses 12 warmup
  frames, 8 sparkle frames, disables the automatic no-RTGI baseline, uses
  smoke metrics, and keeps analysis downsampled. Pair it with a case filter for
  focused Path Tracing checks that finish quickly.
- `--euphorica-skip-baseline` disables the automatic no-RTGI baseline without
  changing warmup or capture frame counts. Use `--euphorica-include-baseline`
  to force the baseline back on.
- `--euphorica-metrics=full|smoke|none` selects metric cost. `full` is for
  checkpoint A/B runs; `smoke` records basic luma and last-frame temporal
  sparkle; `none` records only metadata and captures.
- `--euphorica-list-cases` writes a summary with the planned case names and
  exits without rendering.
- `--euphorica-capture-debug` writes RTGI denoiser debug captures for each RTGI
  case, including source attribution, cache diagnostics, cache raw diffuse,
  cache filtered diffuse, reconstructed output, reconstructed reactivity, and
  final buffers when those views are available.

The `reconstruction` profile writes no-RTGI, Hybrid RTGI, and Full Path Tracing
cases across the requested `rtgi_resolution_scale` values. Each metrics JSON
includes a `resolution_context` block with the requested `GameViewport` size,
whether that size came from the native scene or an override, captured game/final
image sizes, root window size, the viewport 3D scaling mode/scale, the
estimated 3D render size, the RTGI scale, and the estimated RTGI trace texture
size. This matters for Euphorica because the game render is already inside a
scaled SubViewport before RTGI applies its own scale. For example, the native
`1920x1080` GameViewport with `scaling_3d_scale = 0.33333334` and
`rtgi_resolution_scale = 0.5` traces roughly `320x180`, not `960x540`.

Each case writes `_game.png`, `_final.png`, per-case metrics JSON, and
`euphorica_rtgi_summary.json` with effective RTGI knob values, the active RTGI
denoiser path, and the renderer stage each knob feeds. The summary is rewritten
after every completed case and contains split-on/off pair metrics when both
sides of a pair have completed.

## Cornell Box

`--rtgi-scene=cornell` constructs the Cornell Box from Cornell's public geometry
layout in script. Cornell mode forces a square viewport, disables inherited
ambient lighting, uses a linear tone mapper, and uses the measured camera
projection (`25 mm` square image plane, `35 mm` focal length).

`--rtgi-cornell-compare` compares the capture against a local copy of Cornell's
synthetic reference image. The reference image is not committed to this MIT
tree. Pass it explicitly:

```powershell
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --scene res://scenes/rtgi_quality_main.tscn --rtgi-scene=cornell --rtgi-cornell-compare --rtgi-cornell-reference-image=D:\dev\rtgi_external_assets\cornell\simulated.jpg --rtgi-capture-comparison
```

Sources:

- Cornell Box public data: <https://www.graphics.cornell.edu/online/box/data.html>
- Cornell synthetic reference image: <https://www.graphics.cornell.edu/online/box/simulated.jpg>

## Committed Weak-Spot Scenes

Four extra scenes target specific RTGI weak spots. Unlike the runtime-built
modes above, these are committed static `.tscn` files under `scenes/`, so the
harness loads them instead of rebuilding geometry each run. An earlier scene set
was lost because it was only built at runtime in script and never committed;
keeping real `.tscn` files avoids that.

- `cornell_box`: a classic Cornell box with a red left wall, a green right wall,
  neutral white floor/ceiling/back, a ceiling `AreaLight3D`, and the two
  canonical interior blocks. The `WorldEnvironment` uses a linear tonemapper with
  ambient light off, so the red-onto-white and green-onto-white color bleed is
  the ground-truth RTGI signal. It measures the wall and floor chroma bleed
  margins, floor luma, and ceiling firefly count.
- `specular_motion`: a large matte floor with four low-roughness metallic spheres
  that orbit the center while four colored `OmniLight3D` nodes orbit on opposing
  paths, sweeping specular highlights across the spheres every frame. This is the
  temporal-stability stressor; it measures frame-to-frame temporal sparkle under
  motion.
- `reflective_pool`: a near-mirror reflective plane with three emissive spheres at
  roughly 1x, 4x, and 16x energy floating above it and gently bobbing. It tests
  sharp reflections of the emissives in the plane and emissive GI bleeding onto
  the plane, using a linear tonemapper. It measures reflection edge energy,
  reflected-firefly count, and the bled surface luma.
- `fog_corridor`: a 40 m gray corridor with marker blocks every 4 m, one shadowed
  OmniLight at the fixed camera, an alpha-blend glass pane at 8 m, and depth-mode
  fog (begin 2 m, end 30 m). It checks that the ray-traced paths apply the same
  fog the raster pipeline does. The comparison spans three configs sharing one
  `--rtgi-output-dir`, run either as one process
  (`--rtgi-modes=off,fpt,hybrid`) or as three single-mode runs with
  `--rtgi-mode=off` first to record the raster reference. Each RTGI config
  prints a `FOGPAR mode=<n> max_rel_err=<f> seam=<f> verdict=<PASS|FAIL>` line
  comparing per-depth floor luminance (and the opaque/alpha seam at the glass
  pane) against the reference; PASS requires `max_rel_err <= 0.10`.

The `specular_motion` and `reflective_pool` animations are driven by an integer
frame counter (never delta-time or wall-clock), so a captured run reproduces
frame for frame. The harness steps the animation in lock-step with the
warmup/capture loop.

Generate or regenerate the `.tscn` files from the committed generator with the
editor binary:

```powershell
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --headless --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --script res://tools/generate_test_scenes.gd
```

The canonical regression pattern runs the modes of one scene in a single
process with `--rtgi-modes` (about 40 seconds per scene instead of several
minutes per mode). The per-task tier is two processes:

```powershell
$run = "D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --scene res://scenes/rtgi_quality_main.tscn"
Invoke-Expression "$run --rtgi-scene=fog_corridor --rtgi-modes=off,fpt,hybrid --rtgi-output-dir=<dir>\fog_corridor"
Invoke-Expression "$run --rtgi-scene=cornell_box --rtgi-modes=fpt,hybrid --rtgi-output-dir=<dir>\cornell_box"
```

For the fog-parity scene the `off` config records the raster reference and the
later configs compare against it in the same process. Keep `fpt` ahead of
`hybrid` in the list (see `--rtgi-modes` above). The full sign-off matrix adds
the two single-mode animated scenes:

```powershell
Invoke-Expression "$run --rtgi-scene=specular_motion"
Invoke-Expression "$run --rtgi-scene=reflective_pool"
```

Drop `--headless` for actual RTGI metrics; the headless display has no
RT-capable viewport texture. Single-mode runs still work unchanged, including
the original three-run fog-parity flow sharing one output dir
(`--rtgi-mode=off`, then `--rtgi-mode=hybrid`, then `--rtgi-mode=fpt`).

The thresholds for these scenes in
`expected/rtgi_quality_expected.json` are initial, intentionally lenient
baselines. Tighten them after a reviewed per-GPU baseline capture.
`fog_corridor` carries no JSON thresholds; its gate is the FOGPAR verdict
line, since the comparison spans multiple runs.

## Sponza

`--rtgi-scene=sponza` loads an external Sponza glTF/GLB if available, then runs
the same RTGI captures and metrics. The asset is intentionally not shipped with
the engine. Pass a local path or set `GODOT_RTGI_SPONZA_PATH`:

```powershell
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --scene res://scenes/rtgi_quality_main.tscn --rtgi-scene=sponza --rtgi-sponza-path=D:\dev\rtgi_external_assets\sponza\Sponza.gltf --rtgi-capture-debug --rtgi-capture-comparison
```

The canonical Phase 3 source is Khronos glTF Sample Assets Sponza:
<https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza>.
Use `Models/Sponza/glTF/Sponza.gltf` from a local checkout or download, for
example `D:\dev\rtgi_external_assets\sponza\Models\Sponza\glTF\Sponza.gltf`.
glTF normal maps are expected to use +Y tangent-space. If testing a
DirectX-style -Y source texture set or checking normal handedness, pass
`--rtgi-sponza-normal-y=directx`; this creates temporary flipped normal
textures inside the QA run and does not modify the source asset or engine
importer behavior.
The model license is external to the engine license, so do not vendor it into
this repository. When the asset is missing, the harness uses a small procedural
atrium smoke test and skips Sponza thresholds.

`--rtgi-write-reference` updates `res://expected/rtgi_quality_expected.json`
from the current run. Only use it after reviewing the captures.
