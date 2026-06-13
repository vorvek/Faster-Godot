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
#   res://scenes/light_grid.tscn
#   res://scenes/sun_penumbra_ramp.tscn
#   res://scenes/area_light_wall.tscn
#   res://scenes/textured_area.tscn

const SCENES_DIR := "res://scenes"
const SPECULAR_ANIM_SCRIPT := "res://scripts/specular_motion_anim.gd"
const REFLECTIVE_ANIM_SCRIPT := "res://scripts/reflective_pool_anim.gd"
const FOG_METRIC_SCRIPT := "res://scripts/fog_corridor_metric.gd"
const LIGHT_GRID_ANIM_SCRIPT := "res://scripts/light_grid_anim.gd"


func _initialize() -> void:
	var ok := true
	ok = _save_scene(_build_cornell_box_scene(), "%s/cornell_box.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_specular_motion_scene(), "%s/specular_motion.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_reflective_pool_scene(), "%s/reflective_pool.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_fog_corridor_scene(), "%s/fog_corridor.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_light_grid_scene(), "%s/light_grid.tscn" % SCENES_DIR) and ok
	ok = _save_scene(_build_sun_penumbra_ramp_scene(), "%s/sun_penumbra_ramp.tscn" % SCENES_DIR) and ok
	ok = _save_scene(build_area_light_wall(), "%s/area_light_wall.tscn" % SCENES_DIR) and ok
	ok = _save_scene(build_textured_area(), "%s/textured_area.tscn" % SCENES_DIR) and ok
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
# back/floor/ceiling, a ceiling omni key light with an emissive panel, and the
# two canonical interior blocks (one tall, one short). The WorldEnvironment uses a linear tonemapper
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
	# emissive sampler. The omni key carries the direct term and the emissive panel
	# carries the indirect bounce.
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
	# Hand-tuned framing, authored verbatim so regeneration round-trips the
	# committed scenes/cornell_box.tscn exactly (head-on at mid height into the
	# open front). Do not replace this with look_at(): the generated root is
	# never inside a scene tree, so look_at() errors and leaves identity
	# rotation. The .tscn Transform3D is row-major; this constructor takes
	# basis columns.
	camera.transform = Transform3D(
			Vector3(1, 0, 0),
			Vector3(0, 1, 0),
			Vector3(0, 0, 1),
			Vector3(0.0, 1.39, 5.39))
	root.add_child(camera)
	_claim(root, camera)

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
	# Hand-tuned framing (low eye at y = 1.54, pitched down about 11 degrees),
	# authored verbatim so regeneration round-trips the committed
	# scenes/specular_motion.tscn exactly. Do not replace this with look_at():
	# the generated root is never inside a scene tree, so look_at() errors and
	# leaves identity rotation. The .tscn Transform3D is row-major; this
	# constructor takes basis columns.
	camera.transform = Transform3D(
			Vector3(0.99999994, 0, 0),
			Vector3(0, 0.98162735, -0.19080889),
			Vector3(0, 0.1908089, 0.9816273),
			Vector3(0, 1.54, 6.4))
	root.add_child(camera)
	_claim(root, camera)

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
	# Hand-tuned framing (low eye at y = 1.54, pitched down about 11 degrees so
	# each ball's lamp highlight sits on the pool just below it), authored
	# verbatim so regeneration round-trips the committed
	# scenes/reflective_pool.tscn exactly. Do not replace this with look_at():
	# the generated root is never inside a scene tree, so look_at() errors and
	# leaves identity rotation. The .tscn Transform3D is row-major; this
	# constructor takes basis columns.
	camera.transform = Transform3D(
			Vector3(0.99999994, 0, 0),
			Vector3(0, 0.98162735, -0.19080889),
			Vector3(0, 0.1908089, 0.9816273),
			Vector3(0, 1.54, 4.2))
	root.add_child(camera)
	_claim(root, camera)

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
	# must be look_at_from_position: plain look_at fails loudly with a "Node
	# not inside tree" error here because the generated root is never inside a
	# scene tree, which is also why the older scenes carry hand-tuned camera
	# transforms.
	camera.look_at_from_position(camera.position, Vector3(0.0, 0.0, -14.0), Vector3.UP)

	return root


# --- Light grid -------------------------------------------------------------
# Many-light reservoir stressor: a widened 12 x 4 x 8 m mid-gray room with 24
# shadowed OmniLight3D nodes in a 6 x 4 ceiling grid. With 2 m grid spacing and
# an 8 m omni range, central floor pixels see all 24 positional lights as
# valid, which pushes the RTGI direct-light estimator off the deterministic
# per-light path (limit 12) and onto reservoir (RIS) sampling with genuinely
# fewer candidates (16) than valid lights, so the selection is actually
# stochastic; corner pixels see about 10, so both regimes are in frame at
# once. Per-light energy
# varies slightly so a camera-distance re-sort genuinely reorders the light
# list. A small emissive sphere orbits the room center (emissive-candidate
# signature stressor), the camera orbits slowly (re-sort stressor), and one
# grid light is named ToggleLight so the harness can flip it mid-measurement (a
# real light-set change whose recovery is gated). Floor blockers give the grid
# visible overlapping penumbras. Animation and the toggle hook live in
# light_grid_anim.gd on the root.
func _build_light_grid_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "LightGrid"
	root.set_script(load(LIGHT_GRID_ANIM_SCRIPT))

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.004, 0.004, 0.005)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 4
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	# Mid-gray lambert shell so the floor reads mid-exposed under the grid and
	# the overlapping penumbras stay visible (a dark shell would crush them, a
	# bright one would clip the many-light sum).
	var gray := _make_lambert_material(Color(0.48, 0.48, 0.48))
	var blocker_material := _make_lambert_material(Color(0.40, 0.40, 0.42))

	# Room interior: x in [-6, 6], floor at y = 0, ceiling at y = 4,
	# z in [-4, 4].
	var t := 0.1
	_add_box(root, root, "Floor", Vector3(0.0, -t * 0.5, 0.0), Vector3(12.0, t, 8.0), gray)
	_add_box(root, root, "Ceiling", Vector3(0.0, 4.0 + t * 0.5, 0.0), Vector3(12.0, t, 8.0), gray)
	_add_box(root, root, "BackWall", Vector3(0.0, 2.0, -4.0 - t * 0.5), Vector3(12.0, 4.0 + t * 2.0, t), gray)
	_add_box(root, root, "FrontWall", Vector3(0.0, 2.0, 4.0 + t * 0.5), Vector3(12.0, 4.0 + t * 2.0, t), gray)
	_add_box(root, root, "LeftWall", Vector3(-6.0 - t * 0.5, 2.0, 0.0), Vector3(t, 4.0 + t * 2.0, 8.0 + t * 2.0), gray)
	_add_box(root, root, "RightWall", Vector3(6.0 + t * 0.5, 2.0, 0.0), Vector3(t, 4.0 + t * 2.0, 8.0 + t * 2.0), gray)

	# 24 shadowed omnis in a 6 x 4 ceiling grid (x = -5..5 step 2,
	# z = -3..3 step 2) just under the ceiling. light_size 0.15 keeps the RT
	# light radius above the 0.01 cutoff, so every shadow sample draws a 2D
	# random (the penumbra-noise path under test). The 8 m range makes every
	# grid light valid at the room center (the reservoir regime needs more
	# valid lights than its 16 candidate slots to be truly stochastic). Energy
	# ramps with the grid index so no two lights tie when the engine re-sorts
	# them by camera distance during the orbit.
	var lights_root := Node3D.new()
	lights_root.name = "GridLights"
	root.add_child(lights_root)
	_claim(root, lights_root)
	for j in range(4):
		for i in range(6):
			var idx := j * 6 + i
			var light := OmniLight3D.new()
			# The light at (1, -1) sits near the room center; its floor patch
			# stays in frame across the slow orbit, so it is the toggle target.
			light.name = "ToggleLight" if idx == 9 else "GridLight_%02d" % idx
			light.position = Vector3(-5.0 + 2.0 * float(i), 3.7, -3.0 + 2.0 * float(j))
			light.light_energy = 0.38 + 0.018 * float(idx)
			light.light_size = 0.15
			light.omni_range = 8.0
			light.shadow_enabled = true
			lights_root.add_child(light)
			_claim(root, light)

	# Emissive orbiter: a small bright sphere circling the room center at
	# y = 1.2 (driven by light_grid_anim.gd). Its frame-0 pose must match the
	# anim script's advance_to_frame(0).
	var orbiter_material := _make_emissive_material(Color(1.0, 0.62, 0.30), 4.0)
	var orbiter := _add_sphere(root, root, "EmissiveOrbiter", Vector3(0.0, 1.2, 2.0), 0.15, orbiter_material)
	orbiter.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC

	# Floor blockers (tops at or below 1.3 m, under the 1.7 m camera orbit) so
	# the grid casts visible overlapping penumbras across the floor.
	_add_box(root, root, "Blocker_00", Vector3(-2.5, 0.5, -1.5), Vector3(1.2, 1.0, 1.0), blocker_material)
	_add_box(root, root, "Blocker_01", Vector3(2.0, 0.4, 0.5), Vector3(0.9, 0.8, 0.9), blocker_material)
	_add_box(root, root, "Blocker_02", Vector3(0.5, 0.6, -2.5), Vector3(1.0, 1.2, 1.0), blocker_material)
	_add_box(root, root, "Blocker_03", Vector3(-1.0, 0.35, 1.0), Vector3(0.7, 0.7, 0.7), blocker_material)
	_add_box(root, root, "Blocker_04", Vector3(4.2, 0.45, -0.8), Vector3(1.0, 0.9, 0.8), blocker_material)

	var camera := Camera3D.new()
	# Named plainly so both light_grid_anim.gd ("Camera3D") and the harness's
	# find_children("*", "Camera3D") resolve it.
	camera.name = "Camera3D"
	camera.current = true
	camera.fov = 65.0
	camera.near = 0.05
	camera.far = 60.0
	# Frame-0 pose of the orbit in light_grid_anim.gd: position on the orbit
	# circle (radius 3.0, height 1.7, angle 0 -> (0, 1.7, 3.0)) looking back
	# through the room center (Basis.looking_at(-pos.normalized(), UP)),
	# computed offline and authored verbatim. Do not replace this with
	# look_at(): the generated root is never inside a scene tree, so look_at()
	# errors and leaves identity rotation. This constructor takes basis
	# columns.
	camera.transform = Transform3D(
			Vector3(1, 0, 0),
			Vector3(0, 0.870023, -0.493013),
			Vector3(0, 0.493013, 0.870023),
			Vector3(0.0, 1.7, 3.0))
	root.add_child(camera)
	_claim(root, camera)

	return root


# --- Sun penumbra ramp ------------------------------------------------------
# Soft contact-shadow characterization scene. A neutral 10 x 10 m ground plane,
# a thin vertical wall-like occluder standing on it, and a single
# DirectionalLight3D set to a deliberately large angular diameter
# (light_angular_distance = 4 degrees, eight times the real sun) with
# shadow_enabled and shadow_blur = 1.0 so the raster path takes its soft
# percentage-closer path. The sun comes in from one side at a moderate
# elevation, so the wall throws a shadow band across the plane whose edge runs
# front-to-back; a steep camera looks down at the band so screen-x maps to world
# x and the lit-to-shadowed transition crosses a horizontal strip through the
# middle of the frame. The width of the penumbra band along that edge is the
# signal the measurement reads: a larger source widens it, and the soft raster
# reference and a one-sample ray-traced pass disagree on both its width and its
# noise. The camera transform is authored directly (look_at() errors on a root
# that is never inside a scene tree).
func _build_sun_penumbra_ramp_scene() -> Node3D:
	var root := Node3D.new()
	root.name = "SunPenumbraRamp"

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.004, 0.004, 0.005)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 4
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	# Neutral mid-gray ground so the lit-to-shadowed luma ramp has a clean
	# dynamic range and the soft edge is not crushed or clipped.
	var gray := _make_lambert_material(Color(0.5, 0.5, 0.5))

	# 40 x 40 m floor, top surface at y = 0. Oversized so its edges stay well
	# outside the frame and the measurement strip only ever samples floor, never
	# the black background.
	var t := 0.1
	_add_box(root, root, "Floor", Vector3(0.0, -t * 0.5, 0.0), Vector3(40.0, t, 40.0), gray)

	# Thin vertical occluder standing on the floor: 0.2 m thin in x, 3 m tall,
	# 4 m long in z, centered at the origin. The sun crosses it from the +x side,
	# so it blocks light from reaching the floor on the -x side and the shadow
	# band runs front-to-back (in z). Long in z so the band is wide enough to
	# fill the measurement strip without the wall's own ends clipping it. With a
	# 35-degree sun the band reaches to about x = -4.3 (where the grazing ray
	# clears the 3 m top), and the penumbra is widest there, far from the
	# occluder; the camera is aimed at that soft tip.
	_add_box(root, root, "Occluder", Vector3(0.0, 1.5, 0.0), Vector3(0.2, 3.0, 4.0), gray)

	# A single large-disc sun coming in from the +x side at a 35-degree
	# elevation, so the wall shadows the -x half of the floor and the soft edge
	# of that shadow runs in z. light_angular_distance = 4 degrees is the
	# deliberately wide source; shadow_blur = 1.0 engages the soft raster path.
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 2.0
	sun.shadow_enabled = true
	sun.light_angular_distance = 4.0
	sun.shadow_blur = 1.0
	# Travel direction d = (-cos 35, -sin 35, 0) = (-0.819152, -0.573576, 0):
	# light goes toward -x and down. The light looks down its local -z, so the z
	# column is -d. Basis computed offline (z = -d, x = normalize(up x z),
	# y = z x x) and authored verbatim; position is irrelevant for a directional
	# light but is kept above the +x side for clarity.
	sun.transform = Transform3D(
			Vector3(0, 0, -1),
			Vector3(-0.573576, 0.819152, 0),
			Vector3(0.819152, 0.573576, 0),
			Vector3(5.0, 6.0, 0.0))
	root.add_child(sun)
	_claim(root, sun)

	var camera := Camera3D.new()
	camera.name = "Camera3D"
	camera.current = true
	camera.fov = 60.0
	camera.near = 0.05
	camera.far = 60.0
	# High oblique looking down at the soft tip of the shadow band from
	# (-4.3, 7, 5) toward (-4.3, 0, 0), pitched about 54 degrees below the
	# horizon so screen-x tracks world x (the axis the shadow edge sweeps) and the
	# horizontal mid strip crosses world z near 0. The aim point x = -4.3 puts the
	# widest part of the penumbra (the grazing tip, farthest from the occluder) in
	# the middle of the frame, with lit floor to its left and deep shadow to its
	# right. Forward = normalize(target - position); the camera looks along its
	# -z, so the z column is -forward and x stays world x. Computed offline and
	# authored verbatim; do not replace this with look_at() (the generated root is
	# never inside a scene tree, so look_at() errors and leaves identity
	# rotation).
	camera.transform = Transform3D(
			Vector3(1, 0, 0),
			Vector3(0, 0.581238, -0.813734),
			Vector3(0, 0.813734, 0.581238),
			Vector3(-4.3, 7.0, 5.0))
	root.add_child(camera)
	_claim(root, camera)

	return root


# --- Area-light wall --------------------------------------------------------
# Solid-color area-light isolation scene. A 12 x 12 m neutral Lambert ground
# (albedo ~0.5, Lambert BRDF), black sky and ambient, and a single AreaLight3D
# (2 x 2 m, white, energy 18, shadow enabled) mounted 3 m above the plane
# facing straight down. A thin vertical occluder (0.2 x 1.5 x 0.2 m) sits
# between the light and the +Z side so a soft contact shadow falls on the
# floor, visible from the oblique camera. light_range = 5 m so the lit pool's
# falloff edge is visible within the 12 m plane (the attenuation-edge metric
# reads that transition). Camera: eye at (0, 9, 7), looking toward (0, 0, 0)
# at about 52 degrees below horizontal so the pool, shadow, and range edge are
# all framed. Transform computed offline, authored verbatim (no look_at).
func build_area_light_wall() -> Node3D:
	var root := Node3D.new()
	root.name = "AreaLightWall"

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 4
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	# Neutral mid-gray Lambert ground so the lit pool reads mid-exposed and the
	# falloff gradient is not crushed or clipped.
	var gray := _make_lambert_material(Color(0.5, 0.5, 0.5))
	var t := 0.1
	_add_box(root, root, "Floor", Vector3(0.0, -t * 0.5, 0.0), Vector3(12.0, t, 12.0), gray)

	# Thin occluder standing on the floor between the light and the +Z side,
	# casting a soft contact shadow toward +Z. 0.2 m wide in X, 1.5 m tall,
	# 0.2 m deep in Z, placed at (0, 0.75, 1.5).
	var occluder_mat := _make_lambert_material(Color(0.3, 0.3, 0.3))
	_add_box(root, root, "Occluder", Vector3(0.0, 0.75, 1.5), Vector3(0.2, 1.5, 0.2), occluder_mat)

	# Single solid-color AreaLight3D. Faces down (rotation_degrees.x = -90 so
	# local -Z = world -Y). Energy 18 gives mid-range floor luma (~0.45) at the
	# center of the pool with a 2 x 2 m panel 3 m up. area_range 5 keeps the
	# falloff edge inside the 12 m plane.
	var area := AreaLight3D.new()
	area.name = "AreaLight"
	area.position = Vector3(0.0, 3.0, -1.0)
	area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	area.light_color = Color(1.0, 1.0, 1.0)
	area.light_energy = 18.0
	area.shadow_enabled = true
	area.area_size = Vector2(2.0, 2.0)
	area.area_range = 5.0
	root.add_child(area)
	_claim(root, area)

	# Camera: eye at (0, 9, 7), target (0, 0, 0). Forward = normalize((0,0,0) -
	# (0,9,7)) = normalize(0,-9,-7) = (0, -0.789352, -0.613941). Camera -Z =
	# forward, so z column = -forward = (0, 0.789352, 0.613941). x column stays
	# world X = (1,0,0). y = cross(z,x)... wait, y = cross(x,z) for a
	# right-handed basis: y = cross((1,0,0),(0,0.789352,0.613941)) =
	# (0*0.613941-0*0.789352, 0*0-1*0.613941, 1*0.789352-0*0) =
	# (0, -0.613941, 0.789352). This constructor takes columns (x, y, z, origin).
	var camera := Camera3D.new()
	camera.name = "AreaLightWallCamera"
	camera.current = true
	camera.fov = 60.0
	camera.near = 0.05
	camera.far = 80.0
	camera.transform = Transform3D(
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 0.613941, -0.789352),
			Vector3(0.0, 0.789352, 0.613941),
			Vector3(0.0, 9.0, 7.0))
	root.add_child(camera)
	_claim(root, camera)

	return root


# --- Textured area -----------------------------------------------------------
# Textured area-light mip-accuracy scene. Same neutral Lambert ground (12 x 12
# m) and black env. Two AreaLight3D nodes share the same procedural 256x256
# checkerboard ImageTexture (black/white 16-px tiles). The NEAR light (2 x 2 m,
# 2 m above the floor) subtends a large solid angle and should show tile
# structure in the projected illumination; the FAR light (2 x 2 m, 10 m up and
# 5 m back at a grazing angle) subtends a small footprint. Camera frames both
# lit regions. The area_textured_structure_stddev metric captures whether the
# projected pattern retains luma variance (raster path: high stddev) or is
# flattened to a uniform average (always-coarsest-mip path: near-zero stddev).
func build_textured_area() -> Node3D:
	var root := Node3D.new()
	root.name = "TexturedArea"

	var env := _make_rtgi_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.rtgi_max_bounces = 4
	root.add_child(_make_world_environment(env))
	_claim(root, root.get_node("RTGIWorldEnvironment"))

	var gray := _make_lambert_material(Color(0.5, 0.5, 0.5))
	var t := 0.1
	_add_box(root, root, "Floor", Vector3(0.0, -t * 0.5, 0.0), Vector3(12.0, t, 12.0), gray)

	# Procedural 256x256 checkerboard texture: 16-pixel black/white tiles.
	# Generated at build time; ImageTexture.create_from_image wraps it.
	var img := Image.create(256, 256, false, Image.FORMAT_RGB8)
	var tile := 16
	for py in range(256):
		for px in range(256):
			var even_col := (px / tile) % 2 == 0
			var even_row := (py / tile) % 2 == 0
			var bright := even_col != even_row
			var val := 1.0 if bright else 0.0
			img.set_pixel(px, py, Color(val, val, val))
	var checker_tex := ImageTexture.create_from_image(img)

	# NEAR light: 2 x 2 m, 2 m above the floor center, facing straight down.
	# Subtends a large solid angle; the projected tile structure should survive
	# as visible luma variation on the floor.
	var near_area := AreaLight3D.new()
	near_area.name = "NearAreaLight"
	near_area.position = Vector3(0.0, 2.0, 0.0)
	near_area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	near_area.light_color = Color(1.0, 1.0, 1.0)
	near_area.light_energy = 12.0
	near_area.shadow_enabled = false
	near_area.area_size = Vector2(2.0, 2.0)
	near_area.area_range = 8.0
	near_area.area_texture = checker_tex
	root.add_child(near_area)
	_claim(root, near_area)

	# FAR light: same panel, 10 m up and 5 m toward -Z, tilted to project onto
	# the floor at a grazing angle. Subtends a small footprint (the far/grazing
	# region where the adaptive mip would choose a coarser level even correctly).
	var far_area := AreaLight3D.new()
	far_area.name = "FarAreaLight"
	far_area.position = Vector3(0.0, 10.0, -5.0)
	# Rotate -70 degrees around X so the panel faces mostly downward with a
	# forward tilt, projecting onto the floor in front of it.
	far_area.rotation_degrees = Vector3(-70.0, 0.0, 0.0)
	far_area.light_color = Color(1.0, 1.0, 1.0)
	far_area.light_energy = 20.0
	far_area.shadow_enabled = false
	far_area.area_size = Vector2(2.0, 2.0)
	far_area.area_range = 14.0
	far_area.area_texture = checker_tex
	root.add_child(far_area)
	_claim(root, far_area)

	# Camera: eye at (0, 7, 8), target (0, 0, 0). Forward = normalize((0,0,0) -
	# (0,7,8)) = normalize(0,-7,-8), len = sqrt(49+64) = sqrt(113) ~= 10.630.
	# forward = (0, -0.659, -0.753). z_col = -forward = (0, 0.659, 0.753).
	# x_col = world X = (1,0,0). y_col = cross(x_col, z_col) =
	# (0*0.753-0*0.659, 0*0-1*0.753, 1*0.659-0*0) = (0, -0.753, 0.659).
	var camera := Camera3D.new()
	camera.name = "TexturedAreaCamera"
	camera.current = true
	camera.fov = 65.0
	camera.near = 0.05
	camera.far = 80.0
	camera.transform = Transform3D(
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 0.659380, -0.751828),
			Vector3(0.0, 0.751828, 0.659380),
			Vector3(0.0, 7.0, 8.0))
	root.add_child(camera)
	_claim(root, camera)

	return root
