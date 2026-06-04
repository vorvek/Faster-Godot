# AreaLight3D parity / validation scene

Minimal Forward+ scene used to validate the AreaLight3D port (upstream PR
#108219) and to compare the fork against stock Godot 4.7.

## Scenes
- `parity.tscn` — the canonical comparison scene: a floor + box (StandardMaterial3D,
  roughness 0.4) lit by one `AreaLight3D` (size 3×1.5, energy 5, facing down). Camera
  and transforms are fixed so a screenshot is reproducible.

## What was verified in the fork (Vulkan / Forward+, RTX 4080 SUPER)
Self-captured screenshots during implementation confirmed:
- **Diffuse** — soft LTC rectangular falloff; correct directionality (box top lit, camera-facing face dark).
- **Specular + textured** — a red→blue gradient `area_texture` tints the emitted light magenta
  (the form-factor-weighted average a diffuse surface integrates — correct LTC behavior).
- **Soft shadows** — an occluder casts a penumbra (PCSS-like) soft shadow.
- **No regression** — `OmniLight3D` + `SpotLight3D` + `AreaLight3D` render correctly together
  (round / cone / rectangle pools, each shadowing), so the shared `LightData` / cluster / binding
  changes don't disturb existing lights.
- **Doctest** — `tests/scene/test_area_light_3d.h` passes (light type + property round-trip).

## Remaining parity checks (require a stock-4.7 build / the RTGI harness)
1. **Stock-4.7 side-by-side.** Open `parity.tscn` in stock Godot 4.7 and in this fork with
   identical transforms; screenshot and compare: untextured diffuse, specular highlight,
   textured, shadowed. Expect no perceptible difference in the lit region.
   (Note: stock 4.7 and the fork may tonemap/expose slightly differently in unrelated ways;
   compare the *area-light contribution shape/brightness*, not absolute pixels.)
2. **RTGI regression.** Confirm the RTGI §6 furnace (GI-only) measure is green-unchanged with
   an AreaLight3D present — area lights are a raster light isolated from `radiance_probes`, so
   they should not perturb it, but the furnace harness is the authority. See the
   `rtgi-gi-only-validation` / `rtgi-a3-status` notes.

## Capture helper (local, git-ignored)
`_capture.gd` + `_capture.tscn` (git-ignored) drive an automated screenshot: set
`run/main_scene` to `res://_capture.tscn`, run `<editor-binary> --path misc/arealight_parity`
(normal window — a minimized window pauses rendering), and the script saves `user://shot.png`
then quits. NOTE: the mono editor binary needs the GodotSharp/.NET assemblies next to it
(`bin/GodotSharp/`) or it hangs at ".NET: Initializing module..." when running a project.

For M2 the helper builds a code-only Cornell-style box (**red floor** so indirect bounce onto
the white walls/ceiling is an unmistakable signal) and selects the GI/fog mode from the
`GI_MODE` env var: `direct` (no GI baseline) | `sdfgi` | `voxelgi` | `fog`.

---

# M2 — real-time GI + volumetric fog (units U8/U9)

M2 makes an `AreaLight3D` contribute indirect bounce to **VoxelGI** + **SDFGI** and in-scatter in
**volumetric fog** (`gi.{cpp,h}`, `voxel_gi.glsl`, `sdfgi_direct_light.glsl`, `fog.cpp`,
`volumetric_fog_process.glsl`). Plan: `docs/superpowers/plans/2026-06-04-arealight3d-m2-gi-fog.md`.

## What was verified in the fork (Vulkan / Forward+, RTX 4080 SUPER)
Self-captured screenshots (the Cornell box above) confirmed area-light energy reaches each GI/fog
path. Mean RGB sampled in three regions (the `direct` column is the no-GI baseline — any value above
it is the area light's *indirect* contribution; the ceiling sits above a down-facing light so it
receives **zero** direct light):

| region (only light = the AreaLight3D) | `direct` (no GI) | `sdfgi` | `voxelgi` | `fog` |
|---|---|---|---|---|
| ceiling (pure-bounce surface) | 0,0,0 | **82,72,72** | **14,14,14** | 15,15,15 |
| red floor | 0,0,0 | **84,9,9** | **18,0,0** | 0,0,0 |
| back wall | 0,0,0 | **105,13,13** | **28,0,0** | 16,16,16 |

- **SDFGI** — lit ceiling (82, pure bounce) + strong **red colour-bleed** off the red floor (floor/wall
  R ≫ G,B) = correct indirect transport.
- **VoxelGI** — above-baseline red bounce (floor 18,0,0; wall 28,0,0) after a runtime `VoxelGI.bake()`;
  the appended binding-13 atlas path validated with no RD errors.
- **Volumetric fog** — gray in-scatter haze (ceiling/wall 15–16) from the area light; floor ≈ 0 (short
  view ray, `gi_inject` off) — the volumetric contribution, not surface lighting.
- **No errors** — all four runs exit 0 with **no shader-compile and no RD validation/VUID errors**
  (the M1-class "wrong binding silently nulls the uniform set" failure is absent), and the
  `*AreaLight3D*` doctest still passes (no node-API regression).

## Remaining checks (require stock-4.7 / the MAIN checkout)
1. **Stock-4.7 side-by-side** for the GI/fog modes (same as the M1 note): build the same
   VoxelGI / SDFGI / volfog + AreaLight3D scene in stock 4.7 and compare the bounce/in-scatter shape
   and brightness. (RTGI scenes are excluded from parity by design.)
2. **RTGI regression gate — must run in the MAIN checkout.** The §6 furnace + FPT suites
   (`run_signoff_gates`, `t5_fpt_accept`) cannot run from this worktree: the `rtgi_quality_project`
   harness references `RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_*` enums that the worktree's
   merge-base engine doesn't define (parse error before the harness runs) — confirming the
   `rtgi-gate-validation` note that the gates are a MAIN-checkout step. M2's `gi.cpp` edits are
   **additive and isolated from `radiance_probes`** (new `LIGHT_AREA` branches; `VoxelGILight` /
   `SDFGIShader::Light` grown only by tail-appended fields, preserving existing offsets; bindings
   appended, never renumbered), so the gates are expected green-unchanged — **please run them in
   `D:\dev\faster-godot-4.6.3` after merge to confirm.**
