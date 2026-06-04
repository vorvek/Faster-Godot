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
