# Area Light (LTC AreaLight3D)

## Goal

Bring Godot 4.7's `AreaLight3D` node — a rectangular area light evaluated with
**Linearly Transformed Cosines** (LTC) — to the fork's 4.6.3 base, so an
imported 4.7 project renders its area lights with the same result instead of
losing them.

This is a port of upstream PR
[godotengine/godot#108219](https://github.com/godotengine/godot/pull/108219)
(Emil Dobetsberger / "CookieBadger"), which shipped in Godot 4.7-beta 1. Stock
4.6.3-stable has no area-light node.

The north-star is **pixel-parity with 4.7 for `AreaLight3D` in Forward+**: the
exact class name, property names, defaults, and energy/LTC normalization match
upstream, so a 4.7 `.tscn`/`.scn`/glTF/FBX area light deserializes and renders
identically. The one explicit exception is when the fork's RTGI
(`radiance_probes`) is the active GI — there, indirect lighting is RTGI's
domain and area-light GI injection is bypassed (see *Cons*).

The port targets the **Forward+ / Vulkan (RenderingDevice) path only**. The
fork is Vulkan-only/Forward+, so the GLES3 Compatibility and Forward Mobile
paths upstream also touches are out of scope; the Mobile/Compatibility caveats
in the shipped class reference do not apply to this build.

### Fill-stubs-in-place

The fork's earlier RTGI work already imported the upstream PR's *host-side /
API / culling / shadow / material-shader-hook* scaffolding and deliberately
**stubbed every actual rendering path** — RTGI needed the `LIGHT_AREA` concept,
not the raster LTC implementation. So this port does not add the feature from
scratch: it **fills the existing stubs in place**, implementing the missing
rendering core and reconciling it with the fork's divergent layout rather than
adopting upstream's final form. Concretely, the reconciliation kept the fork's
cluster `ElementType` ordering (`OMNI, SPOT, DECAL, REFLECTION_PROBE, AREA`),
**appended** the new Forward+ uniform binding slots instead of renumbering the
existing ones the way upstream did, and re-derived the area-light fields'
offsets against the fork's tightly-packed `LightData` struct so the GLSL and
C++ layouts stay byte-identical.

## The node

`AreaLight3D` ([scene/3d/light_3d.h:242](scene/3d/light_3d.h)) inherits
`Light3D` and is registered in
[scene/register_scene_types.cpp](scene/register_scene_types.cpp); its class
reference ships in
[doc/classes/AreaLight3D.xml](doc/classes/AreaLight3D.xml). It emits light over
a rectangular 2D area in the node's local -Z direction, attenuated over
distance, and can cast soft PCSS shadows drawn from the center of the light.

User-facing members (names/defaults match 4.7 for serialization parity):

- `area_size : Vector2` (default `Vector2(1, 1)`) — the rectangle's extents
  (width and height) in meters.
- `area_texture : Texture2D` (optional) — a texture used as the light source,
  drawn into the area-light atlas with filtered mipmaps. If unset, the light
  emits uniformly across its surface.
- `area_normalize_energy : bool` (default `true`, getter
  `is_area_normalizing_energy`) — divides energy by the light's surface area so
  resizing the light does not change its total output.
- `area_attenuation : float` (default `1.0`) and `area_range : float` (default
  `5.0`) — the distance-attenuation curve and the range in meters (reused
  `Light3D` params; `2.0` attenuation gives physically-accurate inverse-square
  falloff).
- Inherited `light_size` (default `0.5`) drives the PCSS penumbra width, and
  `shadow_normal_bias` defaults to `1.0`.

On the server side the RenderingServer API is completed with
`area_light_create`, `light_area_set_size` / `set_texture` /
`set_normalize_energy` plus their getters, and `bake_render_area_light_atlas`,
backed by the `RenderingServer.LIGHT_AREA` light type.

## Code scope

The port is a faithful but layout-reconciled change across direct lighting,
real-time GI, volumetric fog, baked lightmaps, and the editor. Items marked
*(scaffolding)* were already present from the RTGI import and were only
activated/filled; everything else is new in this port.

**Scene node, RenderingServer API, and per-light data**
- [scene/3d/light_3d.{h,cpp}](scene/3d/light_3d.h) — the `AreaLight3D` node;
  [scene/register_scene_types.cpp](scene/register_scene_types.cpp),
  [doc/classes/AreaLight3D.xml](doc/classes/AreaLight3D.xml),
  [tests/scene/test_area_light_3d.h](tests/scene/test_area_light_3d.h).
- [servers/rendering/rendering_server.cpp](servers/rendering/rendering_server.cpp),
  `rendering_server.h`, `rendering_server_default.h`,
  [servers/rendering/storage/light_storage.h](servers/rendering/storage/light_storage.h),
  and the dummy storage — RS API surface completion (creation + getters +
  bake) over the partially-present `light_area_set_*` setters *(scaffolding)*.
- [servers/rendering/renderer_rd/storage_rd/light_storage.{h,cpp}](servers/rendering/renderer_rd/storage_rd/light_storage.h)
  — host-side area fields, the real `area_light_buffer`, and the per-frame GPU
  fill/upload that replaces the stub `continue;`.
- [servers/rendering/renderer_rd/shaders/light_data_inc.glsl](servers/rendering/renderer_rd/shaders/light_data_inc.glsl)
  — the GLSL `LightData` area fields, kept byte-identical to the C++ struct.

**LTC lookup tables and area-light texture atlas**
- [servers/rendering/storage/ltc/ltc_lut1.dds](servers/rendering/storage/ltc/ltc_lut1.dds),
  [ltc_lut2.dds](servers/rendering/storage/ltc/ltc_lut2.dds) — the precomputed
  LTC tables *(vendored with the RTGI import)*;
  [servers/rendering/storage/make_ltc_lut.py](servers/rendering/storage/make_ltc_lut.py)
  (new build-time generator → `ltc_lut.gen.h`) wired through
  [servers/rendering/storage/SCsub](servers/rendering/storage/SCsub).
- [servers/rendering/renderer_rd/storage_rd/texture_storage.{h,cpp}](servers/rendering/renderer_rd/storage_rd/texture_storage.h)
  — loads the two LUTs into RD textures and adds an `AreaLightAtlas` modeled on
  the existing `DecalAtlas`.

**Forward+ clustered direct lighting** (the primary parity surface)
- [servers/rendering/renderer_rd/cluster_builder_rd.{h,cpp}](servers/rendering/renderer_rd/cluster_builder_rd.h)
  — box-volume insertion for area lights (fork `ElementType` order).
- [servers/rendering/renderer_rd/shaders/area_lights_inc.glsl](servers/rendering/renderer_rd/shaders/area_lights_inc.glsl)
  (new; LTC math copied verbatim from upstream),
  [scene_forward_lights_inc.glsl](servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl)
  (`light_process_area()`), and the
  [forward_clustered](servers/rendering/renderer_rd/shaders/forward_clustered)
  shaders (appended bindings + the area cluster loop).
- [servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp](servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp)
  — binds the area buffer + LTC LUTs + atlas, and drives the area-light shadow
  pass (soft dual-paraboloid / PCSS). Diffuse, specular, textured, and shadowed
  lighting are integrated through the material hooks (`is_area`,
  `area_diffuse`, `area_specular`) the fork already defined *(scaffolding)*.

**Real-time GI and volumetric fog** (non-RTGI projects)
- VoxelGI + SDFGI injection via `ltc_evaluate_diff`:
  [servers/rendering/renderer_rd/environment/gi.cpp](servers/rendering/renderer_rd/environment/gi.cpp),
  [shaders/environment/voxel_gi.glsl](servers/rendering/renderer_rd/shaders/environment/voxel_gi.glsl),
  [sdfgi_direct_light.glsl](servers/rendering/renderer_rd/shaders/environment/sdfgi_direct_light.glsl).
- Volumetric fog:
  [servers/rendering/renderer_rd/environment/fog.cpp](servers/rendering/renderer_rd/environment/fog.cpp),
  [shaders/environment/volumetric_fog_process.glsl](servers/rendering/renderer_rd/shaders/environment/volumetric_fog_process.glsl).

**Baked lightmaps**
- [modules/lightmapper_rd/](modules/lightmapper_rd) plus
  [lm_area_lights_inc.glsl](modules/lightmapper_rd/lm_area_lights_inc.glsl),
  [scene/3d/lightmap_gi.cpp](scene/3d/lightmap_gi.cpp), and
  `scene/3d/lightmapper.h` — bake-time LTC area contribution; `LightmapGI`
  builds the area-texture atlas via `bake_render_area_light_atlas`.

**Editor**
- [editor/icons/AreaLight3D.svg](editor/icons/AreaLight3D.svg),
  [GizmoAreaLight.svg](editor/icons/GizmoAreaLight.svg);
  [editor/scene/3d/gizmos/light_3d_gizmo_plugin.cpp](editor/scene/3d/gizmos/light_3d_gizmo_plugin.cpp)
  (rectangle gizmo with width/height size handles) and the class color in
  [editor/themes/editor_color_map.cpp](editor/themes/editor_color_map.cpp).
- [editor/scene/3d/node_3d_editor_plugin.cpp](editor/scene/3d/node_3d_editor_plugin.cpp)
  plus the Viewport/RenderingServer API expose area-light debug-draw modes in
  the 3D editor's View menu.

## Pros

- Imported Godot 4.7 scenes — and glTF/FBX assets carrying area lights — keep
  their soft rectangular lights (neon tubes, screens, softboxes) without
  re-authoring; the node deserializes and lights identically.
- Physically-based LTC area lighting: diffuse, specular, and textured emission
  with PCSS soft shadows whose penumbra scales with the light's size.
- Full non-RTGI GI coverage — VoxelGI, SDFGI, volumetric fog, and baked
  lightmaps all receive the area-light contribution, matching 4.7.
- Reuses the existing RTGI-import scaffolding (enum, culling, shadow setup,
  material hooks, vendored LTC LUTs, FBX `area` mapping), so the new surface
  area is small and the risk to the working RTGI path is low.
- Coexists with the fork's `radiance_probes` RTGI without regressing it: the
  RTGI GI-only furnace measure stays green-unchanged when area lights are
  present.

## Cons

- Forward+ / Vulkan (RenderingDevice) only. The Mobile and Compatibility
  renderers — and the penumbra/shadow caveats the upstream class reference
  lists for them — are not part of this fork.
- Rectangular shape only. Disk, anisotropic, and other non-rectangular area
  lights remain upstream future-work and are not ported.
- Area lights are not yet fed into the fork's RTGI (`radiance_probes`). When
  RTGI is the active GI, area-light indirect lighting is RTGI's domain and the
  LTC GI / fog / baked injection is bypassed for that view — a deferred
  follow-up, not a parity target.
- Activating the previously dead cull / culler / shadow paths (which no node
  could exercise before) can surface latent bugs; because area lights share the
  `LightData` and cluster paths, omni/spot/directional lighting must stay
  byte-unchanged and is part of the regression check.
- Changing `area_texture` at runtime triggers an atlas redraw with filtered
  mipmaps; keeping each texture dimension a multiple of 128 (or a power of two)
  avoids the extra scaling pass.

## Source and licensing

The LTC math (`area_lights_inc.glsl`) and the LUT generator are copied from
upstream PR #108219 under Godot's MIT/Expat terms; the vendored `.dds` LTC
tables originate with that work. Everything else is fork code adapted to the
Forward+/Vulkan layout described above.
