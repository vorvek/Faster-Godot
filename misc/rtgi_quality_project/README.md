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
D:\dev\faster-godot-4.6.3\bin\faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project --scene res://scenes/rtgi_quality_main.tscn --rtgi-denoise-strength=1.0 --rtgi-capture-debug
```

The harness writes captures and metrics to `user://rtgi_quality` by default.
Use `--rtgi-output-dir=<path>` for an explicit output directory.

Useful options:

- `--rtgi-scene=stress|cornell|sponza`
- `--rtgi-denoise-strength=0.0..1.0`
- `--rtgi-history-weight=0.0..0.98` (default `0.95` for split diffuse)
- `--rtgi-firefly-suppression=0.0..1.0`
- `--rtgi-detail-preservation=0.0..1.0`
- `--rtgi-split-signals=true|false`
- `--rtgi-specular-history-weight=0.0..0.98` (default `0.95`)
- `--rtgi-specular-spatial-strength=0.0..1.0` (default `1.0`)
- `--rtgi-ray-firefly-suppression=0.0..1.0`
- `--rtgi-ray-max-radiance=0.0..4096.0`
- `--rtgi-warmup-frames=120`
- `--rtgi-reference-spp=16`
- `--rtgi-debug-view=beauty|final|noisy|diffuse_noisy|specular_noisy|diffuse_final|specular_final|specular_guide|normal_roughness|viewz_hitdist|motion_vectors|variance|history_length|rejection|disabled`
- `--rtgi-gate-profile=strict|smoke`
- `--rtgi-capture-debug`
- `--rtgi-capture-comparison`
- `--rtgi-cornell-compare`
- `--rtgi-cornell-reference-image=<path>`
- `--rtgi-sponza-path=<path-to-gltf-or-glb>`
- `--rtgi-sponza-normal-y=auto|opengl|directx` (default `auto`; `directx`
  flips imported Sponza normal-map green channels in the harness only)
- `--rtgi-camera-pan`
- `--rtgi-write-reference`

`--rtgi-capture-comparison` writes beauty-frame Path Traced split-signal,
Path Traced single-beauty fallback, Hybrid RT, no-RTGI, and high-SPP reference
captures plus a comparison grid for visual
inspection. `final` is still captured by `--rtgi-capture-debug` as the denoised
RTGI debug buffer.

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
glTF normal maps are expected to use +Y tangent-space. If testing a DirectX
source texture set or checking normal handedness, pass
`--rtgi-sponza-normal-y=directx`; this creates temporary flipped normal
textures inside the QA run and does not modify the source asset or engine
importer behavior.
The model license is external to the engine license, so do not vendor it into
this repository. When the asset is missing, the harness uses a small procedural
atrium smoke test and skips Sponza thresholds.

`--rtgi-write-reference` updates `res://expected/rtgi_quality_expected.json`
from the current run. Only use it after reviewing the captures.
