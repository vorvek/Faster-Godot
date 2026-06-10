extends SceneTree

# Committed generator for the RTGI weak-spot test scenes. It builds each
# scene programmatically with the same node and material construction the
# runtime harness uses (BoxMesh/SphereMesh primitives, StandardMaterial3D,
# OmniLight3D, a WorldEnvironment with RTGI enabled), then packs and saves a
# static .tscn under res://scenes/. The earlier harness lost its scenes because
# they were only built at runtime in GDScript and never committed; saving real
# .tscn files here keeps them durable.
#
# Run it headlessly with the editor binary, for example:
#
#   faster-godot.windows.editor.dev.x86_64.faster_godot.mono.console.exe \
#     --headless --path D:\dev\faster-godot-4.6.3\misc\rtgi_quality_project \
#     --script res://tools/generate_test_scenes.gd
#
# The script quits on its own when finished. Saved scenes:
#   res://scenes/cornell_box.tscn
#   res://scenes/specular_motion.tscn
#   res://scenes/reflective_pool.tscn
#   res://scenes/fog_corridor.tscn

const SCENES_DIR := "res://scenes"
const SPECULAR_ANIM_SCRIPT := "res://scripts/specular_motion_anim.gd"
const REFLECTIVE_ANIM_SCRIPT := "res://scripts/reflective_pool_anim.gd"
const FOG_METRIC_SCRIPT := "res://scripts/fog_corridor_metric.gd"


func _initialize() -> void:
	var ok := true
	ok = _save_scene(_build_cornell_box_scene(), "%s/cornell_box.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_specular_motion_scene(), "%s/specular_motion.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_reflective_pool_scene(), "%s/reflective_pool.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_fog_corridor_scene(), "%s/fog_corridor.tscn" % SCENES_DIR) and ok
	if ok:
		print("RTGI test-scene generation complete.")
	else:
		push_error("RTGI test-scene generation reported one or more failures.")
	quit(0 if ok else 1)


func _save_scene(root: Node3D, path: String) -> bool:
	var packed := PackedScene.new()
	var pack_error := packed.pack(root)
	if pack_error != OK:
		push_error("Could not pack scene '%s' (error %d)." % [path, pack_error])
		return false
	var save_error := ResourceSaver.save(packed, path)
	if save_error != OK:
		push_error("Could not save scene '%s' (error %d)." % [path, save_error])
		return false
	print("Saved %s" % path)
	return true


# Sets node.owner = root for every descendant so PackedScene.pack() captures the
# whole subtree. add_child() must already have run before owner is assigned.
func _claim(root: Node, node: Node) -> void:
	node.owner = root


func _make_world_environment(env: Environment) -> WorldEnvironment:
	var world_environment := WorldEnvironment.new()
	world_environment.name = "RTGIWorldEnvironment"
	world_environment.environment = env
	return world_environment


func _make_rtgi_environment() -> Environment:
	var env := Environment.new()
	env.glow_enabled = false
	env.rtgi_enabled = true
	env.rtgi_disable_in_editor = false
	env.rtgi_mode = Environment.RTGI_MODE_HYBRID
	env.rtgi_samples_per_pixel = 1
	env.rtgi_max_bounces = 3
	env.rtgi_energy = 1.0
	env.rtgi_denoiser = Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL
	return env


# Shared primitive and material builders. These mirror the runtime harness so
# the committed scenes match the geometry the harness would build at runtime.
func _make_flat_material(color: Color, roughness: float, metallic: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	return material


func _make_lambert_material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 1.0
	material.metallic = 0.0
	material.diffuse_mode = StandardMaterial3D.DIFFUSE_LAMBERT
	material.specular_mode = StandardMaterial3D.SPECULAR_DISABLED
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	return material


func _make_lambert_emissive_material(color: Color, energy: float) -> StandardMaterial3D:
	var material := _make_lambert_material(color)
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = energy
	return material


func _make_emissive_material(color: Color, energy: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = energy
	material.roughness = 0.35
	return material


func _add_box(root: Node3D, parent: Node, node_name: String, position: Vector3, size: Vector3, material: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.subdivide_width = 3
	mesh.subdivide_height = 3
	mesh.subdivide_depth = 3
	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	parent.add_child(instance)
	_claim(root, instance)
	return instance


func _add_sphere(root: Node3D, parent: Node, node_name: String, position: Vector3, radius: float, material: Material) -> MeshInstance3D:
	var mesh := SphereMesh.new()
	mesh.radius = radius
	mesh.height = radius * 2.0
	mesh.radial_segments = 48
	mesh.rings = 24
	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	parent.add_child(instance)
	_claim(root, instance)
	return instance


# --- Cornell box ----------------------------------------------------------
# Classic Cornell box: red left wall, green right wall, neutral white/gray
# back/floor/ceiling, a ceiling area light, and the two canonical interior
# blocks (one tall, one short). The WorldEnvironment uses a linear tonemapper
# and ambient light OFF so the red-onto-white and green-onto-white color bleed
# is the ground-truth RTGI signal. RTGI is enabled in Hybrid mode; the harness
# overrides the mode per run.
func _build_cornell_box_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "CornellBox"

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 8
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	var white := _make_lambert_material(Color(0.705, 0.675, 0.515))
	var green := _make_lambert_material(Color(0.150, 0.360, 0.125))
	var red := _make_lambert_material(Color(0.520, 0.085, 0.070))
	var light_material := _make_lambert_emissive_material(Color(1.0, 0.84, 0.58), 3.2)

	# Small, brightly lit interior that matches the proven runtime "cornell" mode
	# (interior side about 2.78 m). A 5 m box was far too large for the ceiling
	# panel, which left the floor near-black. The box is centered on X and Z with
	# the floor at y = 0, opening toward +Z (the camera side).
	var box_size := 2.78
	var t := 0.08
	var half := box_size * 0.5
	var floor_y := 0.0
	var ceiling_y := box_size
	var mid_y := box_size * 0.5
	_add_box(root, root, "Floor", Vector3(0.0, floor_y - t * 0.5, 0.0), Vector3(box_size, t, box_size), white)
	_add_box(root, root, "Ceiling", Vector3(0.0, ceiling_y + t * 0.5, 0.0), Vector3(box_size, t, box_size), white)
	_add_box(root, root, "BackWall", Vector3(0.0, mid_y, -half - t * 0.5), Vector3(box_size, box_size + t * 2.0, t), white)
	# The camera sits at +Z and looks toward -Z, so world -X projects to
	# screen-left (red) and world +X to screen-right (green). The cornell_box
	# measurement ROIs expect red on the left and green on the right.
	_add_box(root, root, "LeftRedWall", Vector3(-half - t * 0.5, mid_y, 0.0), Vector3(t, box_size + t * 2.0, box_size), red)
	_add_box(root, root, "RightGreenWall", Vector3(half + t * 0.5, mid_y, 0.0), Vector3(t, box_size + t * 2.0, box_size), green)

	# Ceiling light approximated the way the proven runtime cornell mode does it:
	# a bright OmniLight3D tucked just under the ceiling for the analytic key plus
	# an emissive panel so the source reads in the image and feeds the RTGI
	# emissive sampler. AreaLight3D faces down here, but a small area panel over a
	# 2.78 m box does not push enough energy to the floor on its own, so the omni
	# key carries the direct term and the panel carries the indirect bounce.
	var light := OmniLight3D.new()
	light.name = "CeilingKeyLight"
	light.position = Vector3(0.0, ceiling_y - 0.12, 0.0)
	light.light_color = Color(1.0, 0.84, 0.58)
	light.light_energy = 4.6
	light.light_size = 0.65
	light.omni_range = 6.0
	light.shadow_enabled = true
	root.add_child(light)
	_claim(root, light)

	var area := AreaLight3D.new()
	area.name = "CeilingAreaLight"
	area.position = Vector3(0.0, ceiling_y - 0.02, 0.0)
	area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	area.area_size = Vector2(0.65, 0.52)
	area.light_color = Color(1.0, 0.84, 0.58)
	area.light_energy = 4.0
	area.area_range = 8.0
	area.shadow_enabled = true
	root.add_child(area)
	_claim(root, area)

	_add_box(root, root, "CeilingEmitter", Vector3(0.0, ceiling_y - 0.03, 0.0), Vector3(0.65, 0.035, 0.52), light_material)

	# Two canonical interior blocks scaled to the smaller box.
	var tall := _add_box(root, root, "TallBlock", Vector3(0.46, floor_y + 0.83, -0.36), Vector3(0.72, 1.65, 0.72), white)
	tall.rotation_degrees.y = 17.0
	var short := _add_box(root, root, "ShortBlock", Vector3(-0.46, floor_y + 0.41, 0.31), Vector3(0.72, 0.82, 0.72), white)
	short.rotation_degrees.y = -17.0

	# Measured projection from the proven runtime cornell mode: a 25 mm square
	# image plane with a 35 mm focal length, head-on into the open front looking
	# at the center of the box.
	var camera := Camera3D.new()
	camera.name = "CornellCamera"
	camera.current = true
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = rad_to_deg(2.0 * atan(0.025 / (2.0 * 0.035)))
	camera.near = 0.05
	camera.far = 40.0
	camera.position = Vector3(0.0, mid_y, half + 4.0)
	root.add_child(camera)
	_claim(root, camera)
	camera.look_at(Vector3(0.0, mid_y, 0.0), Vector3.UP)

	return root


# --- Specular motion ------------------------------------------------------
# A large neutral matte floor, a few low-roughness metallic spheres that orbit
# the scene center, and a few differently-colored OmniLight3D nodes that orbit
# on opposing paths. Specular highlights sweep across the spheres every frame,
# which is the temporal-stability stressor. All motion is frame-counter driven
# by specular_motion_anim.gd for reproducibility. RTGI is enabled.
func _build_specular_motion_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "SpecularMotion"
	root.set_script(load(SPECULAR_ANIM_SCRIPT))

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.004, 0.005, 0.007)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.010, 0.010, 0.013)
	env.ambient_light_energy = 0.06
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 0.85
	env.rtgi_max_bounces = 4
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	var floor_material := _make_flat_material(Color(0.42, 0.40, 0.37), 0.86, 0.0)
	_add_box(root, root, "Floor", Vector3(0.0, -0.05, 0.0), Vector3(14.0, 0.10, 14.0), floor_material)

	# Low-roughness metallic spheres. Animated by the attached script.
	var spheres_root := Node3D.new()
	spheres_root.name = "Spheres"
	root.add_child(spheres_root)
	_claim(root, spheres_root)
	var sphere_roughness := [0.05, 0.08, 0.14, 0.20]
	for i in range(sphere_roughness.size()):
		var roughness: float = sphere_roughness[i]
		var metal := _make_flat_material(Color(0.82, 0.82, 0.84), roughness, 1.0)
		var sphere := _add_sphere(root, spheres_root, "MetalSphere_%02d" % i, Vector3(0.0, 0.85, 0.0), 0.55, metal)
		sphere.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC

	# Colored moving lights on opposing orbits. Animated by the attached script.
	var lights_root := Node3D.new()
	lights_root.name = "MovingLights"
	root.add_child(lights_root)
	_claim(root, lights_root)
	var light_colors := [
		Color(1.0, 0.18, 0.12),
		Color(0.16, 1.0, 0.22),
		Color(0.18, 0.34, 1.0),
		Color(1.0, 0.82, 0.55),
	]
	for i in range(light_colors.size()):
		var light := OmniLight3D.new()
		light.name = "MovingLight_%02d" % i
		light.position = Vector3(0.0, 2.35, 0.0)
		light.light_color = light_colors[i]
		light.light_energy = 4.0
		light.light_size = 0.10
		light.omni_range = 9.0
		light.shadow_enabled = true
		lights_root.add_child(light)
		_claim(root, light)

	var camera := Camera3D.new()
	camera.name = "SpecularMotionCamera"
	camera.current = true
	camera.fov = 56.0
	camera.near = 0.05
	camera.far = 60.0
	camera.position = Vector3(0.0, 3.1, 6.4)
	root.add_child(camera)
	_claim(root, camera)
	camera.look_at(Vector3(0.0, 0.7, 0.0), Vector3.UP)

	return root


# --- Reflective pool ------------------------------------------------------
# A flat, near-mirror reflective plane and several emissive spheres at clearly
# different emission energies floating above it. The emitters gently bob
# (frame-counter driven by reflective_pool_anim.gd). This tests sharp
# reflections of the emissives in the plane plus the emissive GI that bleeds
# onto the plane. RTGI is enabled with a linear tonemapper so the reflected and
# bled emissive energy is read directly.
func _build_reflective_pool_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "ReflectivePool"
	root.set_script(load(REFLECTIVE_ANIM_SCRIPT))

	var env := _make_rtgi_environment()
	# A procedural sky so the mirror pool has a real environment to reflect. RT
	# reflection rays that miss scene geometry sample the sky, so the chrome plane
	# mirrors the sky gradient (reading clearly as a reflective surface) while the
	# emissive spheres add bright colored reflections on top. A flat BG_COLOR is not
	# sampled by reflection-miss rays, which would leave the mirror reading as black.
	env.background_mode = Environment.BG_SKY
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.10, 0.16, 0.34)
	sky_material.sky_horizon_color = Color(0.34, 0.38, 0.46)
	sky_material.ground_horizon_color = Color(0.30, 0.32, 0.36)
	sky_material.ground_bottom_color = Color(0.10, 0.10, 0.13)
	sky_material.sun_angle_max = 30.0
	var sky := Sky.new()
	sky.sky_material = sky_material
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 5
	# Reflections RT Only so the low-roughness plane shows true ray-traced mirror
	# reflections of the floating emissives. Hybrid resolves indirect specular from
	# the probe gather (too low frequency to mirror small bright emitters), and
	# FPT-fast shades a NEE primary-direct without a sharp reflection bounce; only
	# the dedicated RT reflection path mirrors the emitters crisply on the surface.
	env.rtgi_mode = Environment.RTGI_MODE_REFLECTIONS_RT_ONLY
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	# Near-mirror reflective plane (still pool). Top surface sits at y = 0. For a
	# metallic surface the albedo IS the reflectance (F0), so a bright chrome albedo
	# is needed to actually mirror the emissives; the dark-pool look then comes from
	# the plane reflecting the dark sky, while the bright spheres reflect at near
	# full strength. Low roughness keeps those reflections sharp.
	var pool_material := _make_flat_material(Color(0.90, 0.92, 0.96), 0.04, 1.0)
	_add_box(root, root, "Pool", Vector3(0.0, -0.05, 0.0), Vector3(24.0, 0.10, 24.0), pool_material)

	# A neutral matte backdrop so reflections and bleed have context.
	var backdrop := _make_flat_material(Color(0.18, 0.18, 0.20), 0.85, 0.0)
	_add_box(root, root, "Backdrop", Vector3(0.0, 4.0, -8.0), Vector3(24.0, 8.0, 0.20), backdrop)

	# Emissive spheres at clearly different energies (about 3x, 6x, 12x), floating
	# above the pool and spread across it so each one's mirror reflection lands on
	# the visible plane below it and the colored emissive GI pools on the surface.
	# The brightest is near-white so it reads as a hot highlight in the reflection.
	var emitters_root := Node3D.new()
	emitters_root.name = "Emitters"
	root.add_child(emitters_root)
	_claim(root, emitters_root)
	var emitter_specs := [
		{"name": "EmitterLow", "pos": Vector3(-2.4, 0.95, -1.4), "color": Color(1.0, 0.32, 0.16), "energy": 6.0},
		{"name": "EmitterMid", "pos": Vector3(0.0, 1.15, -2.2), "color": Color(0.22, 1.0, 0.70), "energy": 10.0},
		{"name": "EmitterHigh", "pos": Vector3(2.4, 1.3, -1.4), "color": Color(0.95, 0.97, 1.0), "energy": 18.0},
	]
	for spec in emitter_specs:
		var material := _make_emissive_material(spec["color"], spec["energy"])
		var sphere := _add_sphere(root, emitters_root, spec["name"], spec["pos"], 0.7, material)
		sphere.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
		# A colored light co-located with each emitter. RT reflections mirror direct
		# lights as bright specular highlights, so each lamp paints a colored streak
		# on the chrome pool below its sphere, making the surface read as clearly
		# reflective and giving the reflection measurement real high-frequency detail.
		var lamp := OmniLight3D.new()
		lamp.name = str(spec["name"]) + "Lamp"
		lamp.position = spec["pos"]
		lamp.light_color = spec["color"]
		lamp.light_energy = 4.0
		lamp.omni_range = 16.0
		lamp.shadow_enabled = false
		emitters_root.add_child(lamp)
		_claim(root, lamp)

	# A dim neutral fill so the matte backdrop reads. The metallic pool ignores
	# diffuse light, so this mainly lights the backdrop and the spheres' rims.
	var fill := OmniLight3D.new()
	fill.name = "SoftFill"
	fill.position = Vector3(0.0, 6.0, 5.0)
	fill.light_color = Color(0.86, 0.90, 1.0)
	fill.light_energy = 0.6
	fill.omni_range = 24.0
	fill.shadow_enabled = false
	root.add_child(fill)
	_claim(root, fill)

	# Elevated camera looking down across the pool so the reflective surface fills
	# the lower part of the frame and the mirror reflections of the floating
	# emissives are clearly visible below each sphere, where the reflective_pool
	# measurement ROIs sample.
	var camera := Camera3D.new()
	camera.name = "ReflectivePoolCamera"
	camera.current = true
	camera.fov = 52.0
	camera.near = 0.05
	camera.far = 90.0
	# Framing set by hand in the editor (low eye, pitched down about 11 degrees) so
	# each ball's lamp highlight sits on the pool just below it.
	camera.position = Vector3(0.0, 1.54, 4.2)
	root.add_child(camera)
	_claim(root, camera)
	camera.look_at(Vector3(0.0, 0.72, 0.0), Vector3.UP)

	return root


# --- Fog corridor -----------------------------------------------------------
# Depth-fog parity scene: a 40 m gray corridor receding from a fixed camera,
# marker blocks every 4 m from z = -2 to z = -38, one shadowed OmniLight at
# the camera, and an alpha-blend glass pane over the left half of the corridor
# at z = -8. The Environment authors depth-mode fog (begin 2 m, end 30 m,
# density 1.0), so the raster reference shows a gentle near-to-far haze ramp.
# A ray path that ignores fog_mode and applies exponential fog at density 1.0
# grays the corridor out within a few meters instead, which is exactly what
# fog_corridor_metric.gd (attached to the root) measures: per-depth floor
# luminance against the raster (--rtgi-mode=off) reference, plus the
# opaque/alpha seam either side of the glass pane. The shell albedo is kept
# dark so the RTGI indirect bounce stays a small share of the floor signal;
# the fog ramp is the comparison, not the GI lift. RTGI is authored to Full
# Path Tracing; the harness overrides the mode per run.
func _build_fog_corridor_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "FogCorridor"
	root.set_script(load(FOG_METRIC_SCRIPT))

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.4, 0.5, 0.7)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	env.fog_enabled = true
	env.fog_mode = Environment.FOG_MODE_DEPTH
	env.fog_density = 1.0
	env.fog_depth_begin = 2.0
	env.fog_depth_end = 30.0
	env.fog_light_color = Color(0.8, 0.75, 0.7)
	env.rtgi_mode = Environment.RTGI_MODE_FULL_PATH_TRACING
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	var shell := _make_flat_material(Color(0.30, 0.30, 0.30), 0.95, 0.0)
	var marker_material := _make_flat_material(Color(0.45, 0.45, 0.45), 0.85, 0.0)

	# Corridor shell: x in [-2.5, 2.5], floor at y = 0, ceiling at y = 4,
	# running from z = +1 down to z = -43 with a far end cap.
	_add_box(root, root, "Floor", Vector3(0.0, -0.1, -21.0), Vector3(5.0, 0.2, 44.0), shell)
	_add_box(root, root, "Ceiling", Vector3(0.0, 4.1, -21.0), Vector3(5.0, 0.2, 44.0), shell)
	_add_box(root, root, "LeftWall", Vector3(-2.6, 2.0, -21.0), Vector3(0.2, 4.4, 44.0), shell)
	_add_box(root, root, "RightWall", Vector3(2.6, 2.0, -21.0), Vector3(0.2, 4.4, 44.0), shell)
	_add_box(root, root, "EndCap", Vector3(0.0, 2.0, -43.1), Vector3(5.4, 4.4, 0.2), shell)

	# Marker blocks every 4 m, alternating sides so every front face stays
	# visible from the fixed camera. They give the captures readable depth
	# steps; the measurement itself probes the open floor strip at
	# x in [0.2, 0.7], which no marker body or marker shadow reaches (the
	# light sits on the corridor axis, so marker shadows fall outward).
	for i in range(10):
		var z := -2.0 - 4.0 * float(i)
		var side := -1.4 if i % 2 == 0 else 1.4
		_add_box(root, root, "Marker_%02d" % i, Vector3(side, 0.6, z), Vector3(1.0, 1.2, 0.5), marker_material)

	# Alpha-blend glass pane over the left half of the corridor at z = -8. The
	# metric compares the floor seen through this pane against the clear right
	# side at the same depth (the opaque/alpha fog seam check). Shadow casting
	# is off so the pane does not darken the raster reference.
	var glass := StandardMaterial3D.new()
	glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	glass.albedo_color = Color(0.85, 0.92, 1.0, 0.4)
	glass.roughness = 0.08
	glass.metallic = 0.0
	var pane := _add_box(root, root, "GlassPane", Vector3(-1.225, 1.8, -8.0), Vector3(2.35, 3.6, 0.05), glass)
	pane.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

	# Single shadowed key light at the camera, per the fog-parity setup: the
	# corridor is lit head-on, so the per-depth luminance ramp comes from the
	# light falloff plus the fog, with no other emitters in play.
	var light := OmniLight3D.new()
	light.name = "CameraOmniLight"
	light.position = Vector3(0.0, 1.7, 0.4)
	light.light_energy = 4.0
	light.omni_range = 46.0
	light.shadow_enabled = true
	root.add_child(light)
	_claim(root, light)

	var camera := Camera3D.new()
	camera.name = "FogCorridorCamera"
	camera.current = true
	camera.fov = 60.0
	camera.near = 0.05
	camera.far = 80.0
	camera.position = Vector3(0.0, 1.6, 0.0)
	root.add_child(camera)
	_claim(root, camera)
	# Pitch down slightly so the nearest floor probe (about 3 m out) stays
	# inside the bottom of the frame while the far end remains visible. This
	# must be look_at_from_position: plain look_at silently does nothing here
	# because the generated root is never inside a scene tree, which is also
	# why the older scenes carry hand-tuned camera transforms.
	camera.look_at_from_position(camera.position, Vector3(0.0, 0.0, -14.0), Vector3.UP)

	return root
