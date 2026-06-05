# Custom Viewport Resolution Scaling Modes

This document provides a detailed overview of the custom, GPU-accelerated post-process resolution upscalers added to the `faster-godot` engine. These modes can be configured via Project Settings (`rendering/scaling_3d/mode`) or dynamically inside a Viewport node using the `scaling_3d_mode` property.

---

## 1. Added Scaling Modes

We have integrated three new high-performance, GPU-powered spatial upscaling modes:

### Sharp Bilinear
* **Identifier**: `SCALING_3D_MODE_SHARP_BILINEAR` / `VIEWPORT_SCALING_3D_MODE_SHARP_BILINEAR`
* **Purpose**: Designed specifically for 2D pixel-art based games.
* **Mechanism**: When upscaling pixel art, typical Bilinear interpolation creates a muddy, blurry result, while Nearest Neighbor interpolation causes extreme, distracting "pixel shimmering" (pixels changing sizes unevenly as they move). Sharp Bilinear uses an analytical derivative step to calculate a sharp transition boundary between texels, performing custom bilinear interpolation only exactly on the pixel edges. This produces a perfectly crisp, shimmer-free scaling result at arbitrary resolutions.

### Bicubic (Catmull-Rom) + CAS
* **Identifier**: `SCALING_3D_MODE_BICUBIC` / `VIEWPORT_SCALING_3D_MODE_BICUBIC`
* **Purpose**: Premium 2D/3D smooth upscaler with custom adaptive sharpening.
* **Mechanism**: Uses a highly optimized **9-tap bicubic filter** based on Catmull-Rom spline interpolation. Rather than doing the standard 16 texture samples of standard bicubic, it leverages bilinear filtering hardware to reconstruct 16-tap quality in only 9 taps. This smooth result is then processed in a single pass with **AMD's Contrast Adaptive Sharpening (CAS)**, which sharpens high-frequency edges while leaving flat areas clean of noise and halo artifacts.

### SGSR (Qualcomm Snapdragon Game Super Resolution)
* **Identifier**: `SCALING_3D_MODE_SGSR` / `VIEWPORT_SCALING_3D_MODE_SGSR`
* **Purpose**: High-fidelity, ultra-performance edge-aware spatial upscaler.
* **Mechanism**: Re-implements the industry-standard Qualcomm SGSR spatial upscaler natively on the GPU. It evaluates 12 surrounding texels to detect pixel directionality, blends diagonally to reconstruct clean, non-aliased geometric edges, and applies high-quality edge-aware sharpening in a single high-performance pass.

### Vendor Temporal Upscalers
* **Identifier**: `SCALING_3D_MODE_XESS`.
* **Purpose**: Public per-Viewport entry point for Intel XeSS temporal super resolution.
* **Current behavior**: XeSS dispatches through Intel's native Vulkan SDK path on any GPU that supports it. At startup the engine runs the XeSS Vulkan handshake and enables the instance and device extensions and features the runtime requires, so the cross-vendor DP4a path works on non-Intel GPUs alongside the XMX path on Arc. If `libxess` is absent or the GPU lacks a required capability, XeSS reports unavailable and the viewport falls back to FSR 2.2 with a one-time warning. Set `GODOT_DISABLE_XESS_VULKAN=1` to skip the handshake for debugging.

---

## 2. Scale Range Constraints (Standard 2.0x limit)

* **Standard Bounds**: The rendering scale constraint is kept at the official Godot maximum of `2.0x` (with slider range `0.1` to `2.0`).
* **Rationale**: Limiting the maximum rendering scale to `2.0x` prevents accidental and extremely heavy GPU workloads (such as supersampling above 4K/8K resolutions) which drastically reduce performance. Downscaling from extremely large custom scales back to viewport size also lacks proper mipmap support, causing massive performance overhead without visual benefits.

---

## 3. Architecture & Post-Render Execution

All custom modes are fully integrated into the **Vulkan RD (Rendering Device)** backend:
1. **Low-Resolution Render**: All 3D rendering (including heavy passes like **Ray Tracing** and post-processing) is executed completely at the lower internal resolution (`rb->get_internal_size()`).
2. **Post-Process Upscale Pass**: The upscale is executed during the final copy-to-framebuffer pass (`copy_to_fb.glsl` / `copy_effects.cpp`). By performing the upscaling here, 3D render-time bottlenecks are avoided, resulting in a massive framerate boost for heavy render features compared to rendering natively at 2x.

### 3D Render Buffer Allocation & Validation Bugfixes
We have implemented critical engine fixes to ensure these custom spatial scaling modes operate correctly under all viewport conditions:
* **Prevention of False-Positive Downsampling Fallbacks**: Official Godot safety checks fall back to bilinear upscaling when scale is $\ge 1.0$ under the assumption that all non-bilinear scaling modes are advanced temporal/spatial upscalers (like FSR or DLSS) which do not support supersampling/downsampling. We introduced `scaling_3d_is_advanced_upscaler` to restrict this fallback logic exclusively to actual advanced upscalers, allowing our custom spatial modes (`NEAREST`, `SHARP_BILINEAR`, `BICUBIC`, `SGSR`) to correctly handle downsampling scales up to `2.0x`.
* **Explicit Buffer Allocation Mappings**: Fixed a fallback bug where custom scaling modes were omitted from the viewport configuration `switch` block in `_configure_3d_render_buffers()`. This omission caused them to hit the `default` branch, silently disabling 3D resolution scaling by reverting the mode to `OFF` and scale to `1.0`. We mapped all custom modes (`NEAREST`, `SHARP_BILINEAR`, `BICUBIC`, `SGSR`) explicitly to their correct scaled resolution targets (up to a clamped safe limit of `16384` pixels).

---

## 4. Modified Files

A quick summary of files changed to support this feature:
* [rendering_server_enums.h](file:///d:/dev/faster-godot-4.6.3/servers/rendering/rendering_server_enums.h): Added custom scaling modes to `ViewportScaling3DMode` and categorized them as spatial.
* [viewport.h](file:///d:/dev/faster-godot-4.6.3/scene/main/viewport.h): Registered new scaling modes in `Viewport::Scaling3DMode` to align indexes perfectly.
* [viewport.cpp](file:///d:/dev/faster-godot-4.6.3/scene/main/viewport.cpp): Registered property hints, kept/restored the standard 2.0x scale limit, and bound property enum constants.
* [rendering_server.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/rendering_server.cpp): Registered default Project Settings hints while keeping the standard 2.0x scale limit.
* [renderer_viewport.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_viewport.cpp): Kept/restored standard scale limits, ensured FSR/temporal checks do not interfere with our custom modes, and kept our modes in the safe rendering path.
* [vendor_upscaler.h](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/effects/vendor_upscaler.h): Provides the vendor super-resolution (XeSS) capability and fallback scaffold used by Viewport configuration.
* [copy_effects.h](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/effects/copy_effects.h): Added custom modes to `CopyToFBMode` enum and expanded `copy_to_fb_rect` method signature.
* [copy_effects.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/effects/copy_effects.cpp): Appended corresponding preprocessor defines to compiler modes, handled texture size query, and mapped enums to shader pipelines.
* [renderer_scene_render_rd.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/renderer_scene_render_rd.cpp): Enabled scaling copy-pass for our modes and passed active scale mode.
* [copy_to_fb.glsl](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/shaders/effects/copy_to_fb.glsl): Implemented GLSL shaders for Sharp Bilinear, Bicubic + CAS, and SGSR.

---

## 5. Vendor Upscaler Enum Ordering

Vendor upscaler modes are appended after the existing scaling modes so previously serialized mode values remain stable:

* **Viewport Property Inspector**: XeSS is visible in the Viewport `scaling_3d_mode` dropdown.
* **Project Settings Dropdown**: XeSS is visible in `rendering/scaling_3d/mode`.
* **Fallback Safety**: If the XeSS SDK backend is unavailable or unsafe on the current GPU/driver, the renderer prints a one-time warning and falls back to FSR 2.2 (`SCALING_3D_MODE_FSR2`) inside `servers/rendering/renderer_viewport.cpp`.
* **Frame Generation**: `frame_generation_mode` has two options, `Disabled` and `Interpolated (3D Motion Vectors)`. The interpolated path reprojects between two real frames using the engine's own motion vectors and works on any viewport. There is no vendor frame generation. The Intel XeSS-FG SDK only ships a D3D12 proxy-swapchain entry point, which the Vulkan renderer here cannot drive, so a vendor mode would do nothing except fall back to the interpolated path.
