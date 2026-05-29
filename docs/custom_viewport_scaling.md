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

---

## 2. Scale Range Extensions (Up to 100x)

* **Extended Bounds**: The rendering scale constraint has been increased from `2.0` to `100.0`.
* **UI Slider**: The inspector slider range is set to `0.1` to `10.0`, with the property hint `"or_greater"` enabling developers to type arbitrary values up to `100.0`.
* **Advantage**: Allows extreme scaling factors (e.g., massive internal supersampling/rendering or low-resolution retro-styled scaling effects).

---

## 3. Architecture & Post-Render Execution

All custom modes are fully integrated into the **Vulkan RD (Rendering Device)** backend:
1. **Low-Resolution Render**: All 3D rendering (including heavy passes like **Ray Tracing** and post-processing) is executed completely at the lower internal resolution (`rb->get_internal_size()`).
2. **Post-Process Upscale Pass**: The upscale is executed during the final copy-to-framebuffer pass (`copy_to_fb.glsl` / `copy_effects.cpp`). By performing the upscaling here, 3D render-time bottlenecks are avoided, resulting in a massive framerate boost for heavy render features compared to rendering natively at 2x.

---

## 4. Modified Files

A quick summary of files changed to support this feature:
* [rendering_server_enums.h](file:///d:/dev/faster-godot-4.6.3/servers/rendering/rendering_server_enums.h): Added custom scaling modes to `ViewportScaling3DMode` and categorized them as spatial.
* [viewport.h](file:///d:/dev/faster-godot-4.6.3/scene/main/viewport.h): Registered new scaling modes in `Viewport::Scaling3DMode` to align indexes perfectly.
* [viewport.cpp](file:///d:/dev/faster-godot-4.6.3/scene/main/viewport.cpp): Extended limits up to 100.0, registered property hints, and bound property enum constants.
* [rendering_server.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/rendering_server.cpp): Registered default Project Settings hints and default values for indexes 5-9.
* [renderer_viewport.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_viewport.cpp): Adjusted bounds, ensured FSR/temporal checks do not interfere with our custom modes, and kept our modes in the safe rendering path.
* [copy_effects.h](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/effects/copy_effects.h): Added custom modes to `CopyToFBMode` enum and expanded `copy_to_fb_rect` method signature.
* [copy_effects.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/effects/copy_effects.cpp): Appended corresponding preprocessor defines to compiler modes, handled texture size query, and mapped enums to shader pipelines.
* [renderer_scene_render_rd.cpp](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/renderer_scene_render_rd.cpp): Enabled scaling copy-pass for our modes and passed active scale mode.
* [copy_to_fb.glsl](file:///d:/dev/faster-godot-4.6.3/servers/rendering/renderer_rd/shaders/effects/copy_to_fb.glsl): Implemented GLSL shaders for Sharp Bilinear, Bicubic + CAS, and SGSR.
