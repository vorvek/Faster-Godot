# Vulkan-Only Windows RenderingDevice Profile

## Goal

Center this fork's Windows RenderingDevice path on Vulkan and remove the
Direct3D 12 implementation surface inherited from official Godot.

## Code Changes

- Removes the Direct3D 12 driver, shader baker platform, OpenXR extension,
  DirectX headers, D3D12MA, SDK installer, and Windows CI SDK setup.
- Removes SCons and Windows export options for Direct3D 12 dependencies,
  including Agility SDK and PIX runtime packaging.
- Exposes Vulkan as the only Windows RenderingDevice driver in project settings,
  export settings, display-driver discovery, and canonical docs.
- Routes stale `d3d12` project settings or command-line requests to `vulkan`
  with a warning so older projects still open without editing `project.godot`.

## Expected Behavior

- New projects use Vulkan on Windows.
- Existing projects that still contain `rendering_device/driver.windows="d3d12"`
  run through Vulkan in this fork.
- If Vulkan initialization fails and OpenGL support is present, the existing
  OpenGL fallback path remains available.
