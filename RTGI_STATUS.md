# RTGI System Status

Status date: 2026-05-28

## Summary

The Vulkan Forward+ RTGI path is functional through the built-in Vulkan Generic
backend and the shared RenderingDevice resource path. The vendor backend
selection, capability dictionaries, fallback reporting, exportable Vulkan
resource exchange, and RTGI quality harness are wired into the engine and tests.

The current vendor state is intentionally explicit:

- Vulkan Generic is working and is the reliable fallback path.
- NVIDIA RTXPT is selectable and renders on NVIDIA Vulkan hardware, but it is
  currently the NVIDIA Godot-fork-compatible RenderingDevice ray tracing path.
  It validates an RTXPT source manifest and reports RTXPT/NRD/Streamline
  capability data, but it does not yet link and dispatch the standalone RTXPT
  core sample implementation. Its capability dictionary reports
  `vendor_scene_import=false` and `vendor_sdk_dispatch=false`.
- AMD HIP RT is compiled with HIP RT scene import and trace dispatch gates. The
  backend reports `vendor_scene_import=true` and `vendor_sdk_dispatch=true`, but
  it requires an AMD GPU plus HIP/Vulkan external-memory and semaphore interop.
  On the current NVIDIA validation machine it correctly falls back to Vulkan
  Generic.
- Intel Embree/OSPRay CPU RTGI is disabled. OIDN is not part of the active RTGI
  plan.

## Backend Matrix

| Backend | Current state | Dispatch path | Denoiser path | Probe updates |
| --- | --- | --- | --- | --- |
| Vulkan Generic | Working | Godot RenderingDevice Vulkan ray tracing pipeline | ASVFG/Internal | Native Vulkan Generic |
| NVIDIA RTXPT | Selectable on NVIDIA Vulkan, renders via compatibility path | NVIDIA Godot fork-compatible RenderingDevice dispatch, not standalone RTXPT core dispatch | NRD headers detected when configured; Streamline/DLSS-RR requires runtime; ASVFG fallback remains the practical path today | Vulkan Generic STRC fallback |
| AMD HIP RT | Compiled, unavailable on non-AMD devices | HIP RT scene snapshot import and HIP trace kernel dispatch, requires AMD GPU/runtime | FidelityFX SDK headers are vendored; runtime handoff still gated by backend availability | Vulkan Generic STRC fallback |
| Intel Embree/OSPRay | Disabled | None | None | None |

## SDK And Installer State

- `modules/rtxpt` detects RTXPT source/header layouts from `rtxpt_sdk_path`.
- `modules/rtxpt` detects NRD from `nrd_sdk_path`, `NRD_SDK_PATH`, `NRD_PATH`,
  project add-on paths, or `thirdparty/nrd`.
- The editor has `Settings > Configure RTGI Vendor SDKs...` for NRD. It can
  clone/update NRD into `res://addons/rtgi_vendor_sdks/nrd`, writes `.gdignore`,
  saves the editor/project SDK path settings, and exports `NRD_SDK_PATH` for the
  current process.
- `thirdparty/fidelityfx-sdk` is vendored for AMD denoiser headers.
- `thirdparty/streamline` is vendored for Streamline headers. DLSS Ray
  Reconstruction still requires the external Streamline runtime binaries.
- Proprietary/runtime binaries are not checked in.

## Capability Reporting

`RenderingServer.pathtracing_get_backend_capabilities()` and backend status
dictionaries now expose the fields needed to distinguish real vendor SDK
execution from compatibility paths:

- `scene_import_path`
- `trace_dispatch_path`
- `vendor_scene_import`
- `vendor_sdk_dispatch`
- existing availability checks, resource exchange, denoiser, fallback, and
  probe-update fields

This prevents the current NVIDIA compatibility path from being mistaken for full
standalone RTXPT SDK dispatch.

## Local Validation

Build command used:

```powershell
scons platform=windows target=editor dev_build=yes tests=yes module_rtxpt_enabled=yes module_hiprt_enabled=yes module_embree_enabled=yes module_ospray_enabled=yes rtxpt_sdk_path=C:\Users\volsu\AppData\Local\Temp\nvidia-rtxpt-reference nrd_sdk_path=C:\Users\volsu\AppData\Local\Temp\nrd_inspect\NRD streamline_sdk_path=C:\Users\volsu\AppData\Local\Temp\nvidia-godot-pt-reference\thirdparty\streamline hiprt_sdk_path=C:\Users\volsu\AppData\Local\Temp\hiprt-reference ospray_sdk_path=C:\Users\volsu\AppData\Local\Temp\ospray-reference -j31
```

Targeted tests:

- `--test --test-case="*PathTracing*"`: passed, 11 test cases, 1038 assertions.
- `--test --test-case="*RTGI*"`: passed, 4 test cases, 76 assertions.
- `--test --test-case="*Environment*RTGI*"`: passed, 2 test cases, 33 assertions.

Visual smoke scene:

- Scene: `many_lights` (`many_light_emissive` in metrics).
- Driver/method: Vulkan Forward+.
- Device: NVIDIA GeForce RTX 4080 SUPER.
- Output directory: `D:\dev\rtgi_phase4_outputs\many_lights_20260528`.
- Vulkan Generic: passed and rendered with active backend `Vulkan Generic`.
- NVIDIA RTXPT: passed and rendered with active backend `NVIDIA RTXPT`; metrics
  report `vendor_sdk_dispatch=false`.
- AMD HIP RT: passed through expected fallback; requested backend was
  `AMD HIP RT`, active backend was `Vulkan Generic`, with fallback reason
  `AMD HIP RT requires a AMD GPU; active device is NVIDIA`.

## Remaining Work

- Link or adapt real standalone RTXPT SDK scene import and dispatch, then flip
  NVIDIA `vendor_scene_import` and `vendor_sdk_dispatch` only after that path is
  actually used by rendered frames.
- Validate AMD HIP RT on AMD hardware with HIP runtime and Vulkan external
  memory/semaphore support.
- Complete runtime denoiser handoff for NRD/Streamline DLSS-RR and FidelityFX
  alongside ASVFG, keeping runtime binaries external.
- Add CI or hardware-lab coverage for NVIDIA, AMD, missing SDKs, missing
  runtimes, non-RT GPUs, and Vulkan validation layers.
