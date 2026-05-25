extends Node3D

const DEFAULT_OUTPUT_DIR := "user://rtgi_quality"
const EXPECTED_METRICS_PATH := "res://expected/rtgi_quality_expected.json"
const CORNELL_REFERENCE_URL := "https://www.graphics.cornell.edu/online/box/simulated.jpg"
const CORNELL_PUBLIC_DATA_URL := "https://www.graphics.cornell.edu/online/box/data.html"
const SPONZA_KHRONOS_SOURCE_URL := "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza"
const SPONZA_KHRONOS_CANONICAL_SUFFIX := "Models/Sponza/glTF/Sponza.gltf"

var _denoise_strength := 1.0
var _history_weight := 0.95
var _firefly_suppression := 1.0
var _detail_preservation := 1.0
var _split_signals := true
var _specular_history_weight := 0.95
var _specular_spatial_strength := 1.0
var _ray_firefly_suppression := 0.85
var _ray_max_radiance := 32.0
var _analytic_light_sampling := true
var _explicit_emissive_sampling := true
var _warmup_frames := 120
var _output_dir := DEFAULT_OUTPUT_DIR
var _debug_view := "beauty"
var _capture_all_debug_views := true
var _capture_comparison := false
var _write_reference := false
var _camera_pan := false
var _reference_spp := 16
var _sparkle_frames := 16
var _convergence_frames := 0
var _scene_mode := "stress"
var _cornell_compare := false
var _cornell_reference_image := ""
var _sponza_path := ""
var _sponza_normal_y_mode := "auto"
var _gate_profile := "strict"
var _camera: Camera3D
var _environment: Environment
var _sponza_asset_loaded := false
var _sponza_normal_y_flipped := false
var _sponza_flipped_normal_texture_count := 0
var _normal_flip_cache := {}


func _ready() -> void:
	_parse_args()
	if _scene_mode == "convergence" and _convergence_frames <= 0:
		_convergence_frames = 48
	if _scene_mode == "cornell":
		_force_square_viewport()
	_build_scene()
	call_deferred("_run_capture")


func _force_square_viewport() -> void:
	var square_size := Vector2i(768, 768)
	get_viewport().size = square_size
	get_window().content_scale_size = square_size
	if DisplayServer.get_name().to_lower() != "headless":
		DisplayServer.window_set_size(square_size)
		get_window().size = square_size


func _parse_args() -> void:
	for arg in OS.get_cmdline_args():
		if arg.begins_with("--rtgi-denoise-strength="):
			_denoise_strength = clampf(arg.trim_prefix("--rtgi-denoise-strength=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-history-weight="):
			_history_weight = clampf(arg.trim_prefix("--rtgi-history-weight=").to_float(), 0.0, 0.98)
		elif arg.begins_with("--rtgi-firefly-suppression="):
			_firefly_suppression = clampf(arg.trim_prefix("--rtgi-firefly-suppression=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-detail-preservation="):
			_detail_preservation = clampf(arg.trim_prefix("--rtgi-detail-preservation=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-split-signals="):
			_split_signals = not (arg.trim_prefix("--rtgi-split-signals=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-specular-history-weight="):
			_specular_history_weight = clampf(arg.trim_prefix("--rtgi-specular-history-weight=").to_float(), 0.0, 0.98)
		elif arg.begins_with("--rtgi-specular-spatial-strength="):
			_specular_spatial_strength = clampf(arg.trim_prefix("--rtgi-specular-spatial-strength=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-ray-firefly-suppression="):
			_ray_firefly_suppression = clampf(arg.trim_prefix("--rtgi-ray-firefly-suppression=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-ray-max-radiance="):
			_ray_max_radiance = maxf(arg.trim_prefix("--rtgi-ray-max-radiance=").to_float(), 0.0)
		elif arg.begins_with("--rtgi-analytic-light-sampling="):
			_analytic_light_sampling = not (arg.trim_prefix("--rtgi-analytic-light-sampling=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-explicit-emissive-sampling="):
			_explicit_emissive_sampling = not (arg.trim_prefix("--rtgi-explicit-emissive-sampling=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-warmup-frames="):
			_warmup_frames = max(1, arg.trim_prefix("--rtgi-warmup-frames=").to_int())
		elif arg.begins_with("--rtgi-reference-spp="):
			_reference_spp = clampi(arg.trim_prefix("--rtgi-reference-spp=").to_int(), 1, 128)
		elif arg.begins_with("--rtgi-sparkle-frames="):
			_sparkle_frames = clampi(arg.trim_prefix("--rtgi-sparkle-frames=").to_int(), 0, 64)
		elif arg.begins_with("--rtgi-convergence-frames="):
			_convergence_frames = clampi(arg.trim_prefix("--rtgi-convergence-frames=").to_int(), 0, 128)
		elif arg.begins_with("--rtgi-scene="):
			var requested_scene := arg.trim_prefix("--rtgi-scene=").to_lower()
			if requested_scene in ["stress", "cornell", "convergence", "sponza", "sdfgi", "voxelgi", "lightmap", "lightprobe", "path_traced_sdfgi_exclusive", "many_light_emissive"]:
				_scene_mode = requested_scene
			else:
				push_warning("Unknown RTGI quality scene '%s'; using stress scene." % requested_scene)
		elif arg.begins_with("--rtgi-cornell-reference-image="):
			_cornell_reference_image = arg.trim_prefix("--rtgi-cornell-reference-image=")
		elif arg.begins_with("--rtgi-sponza-path="):
			_sponza_path = arg.trim_prefix("--rtgi-sponza-path=")
		elif arg.begins_with("--rtgi-sponza-normal-y="):
			var normal_y_mode := arg.trim_prefix("--rtgi-sponza-normal-y=").to_lower()
			if normal_y_mode in ["auto", "opengl", "gl", "+y", "directx", "dx", "dx12", "-y"]:
				_sponza_normal_y_mode = "directx" if normal_y_mode in ["directx", "dx", "dx12", "-y"] else ("opengl" if normal_y_mode in ["opengl", "gl", "+y"] else "auto")
			else:
				push_warning("Unknown Sponza normal-Y mode '%s'; using auto." % normal_y_mode)
		elif arg.begins_with("--rtgi-sponza-flip-normal-y="):
			var flip_normal_y := arg.trim_prefix("--rtgi-sponza-flip-normal-y=").to_lower()
			if flip_normal_y in ["1", "true", "on", "enabled"]:
				_sponza_normal_y_mode = "directx"
			elif flip_normal_y in ["0", "false", "off", "disabled"]:
				_sponza_normal_y_mode = "opengl"
			else:
				push_warning("Unknown Sponza normal-Y flip value '%s'; using auto." % flip_normal_y)
		elif arg.begins_with("--rtgi-gate-profile="):
			var profile := arg.trim_prefix("--rtgi-gate-profile=").to_lower()
			if profile in ["strict", "smoke"]:
				_gate_profile = profile
			else:
				push_warning("Unknown RTGI gate profile '%s'; using strict." % profile)
		elif arg.begins_with("--rtgi-output-dir="):
			_output_dir = arg.trim_prefix("--rtgi-output-dir=")
		elif arg.begins_with("--rtgi-debug-view="):
			_debug_view = arg.trim_prefix("--rtgi-debug-view=").to_lower()
		elif arg == "--rtgi-capture-debug":
			_capture_all_debug_views = true
		elif arg == "--rtgi-capture-comparison":
			_capture_comparison = true
		elif arg == "--rtgi-cornell-compare":
			_cornell_compare = true
		elif arg == "--rtgi-write-reference":
			_write_reference = true
		elif arg == "--rtgi-camera-pan":
			_camera_pan = true


func _build_scene() -> void:
	var env := Environment.new()
	env.glow_enabled = false
	env.rtgi_enabled = true
	env.rtgi_disable_in_editor = false
	env.rtgi_mode = Environment.RTGI_MODE_PATH_TRACED
	env.rtgi_samples_per_pixel = 1
	env.rtgi_max_bounces = 3
	env.rtgi_energy = 1.0
	env.rtgi_denoiser = Environment.RTGI_DENOISER_SVGF
	env.rtgi_denoiser_strength = _denoise_strength
	env.rtgi_denoiser_history_weight = _history_weight
	env.rtgi_denoiser_firefly_suppression = _firefly_suppression
	env.rtgi_denoiser_detail_preservation = _detail_preservation
	env.rtgi_denoiser_split_signals = _split_signals
	env.rtgi_denoiser_specular_history_weight = _specular_history_weight
	env.rtgi_denoiser_specular_spatial_strength = _specular_spatial_strength
	env.rtgi_ray_firefly_suppression = _ray_firefly_suppression
	env.rtgi_ray_max_radiance = _ray_max_radiance
	env.rtgi_analytic_light_sampling_enabled = _analytic_light_sampling
	env.rtgi_explicit_emissive_sampling_enabled = _explicit_emissive_sampling
	_environment = env

	var world_environment := WorldEnvironment.new()
	world_environment.name = "RTGIWorldEnvironment"
	world_environment.environment = env
	add_child(world_environment)

	if _scene_mode == "cornell":
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color.BLACK
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.070, 0.052, 0.032)
		env.ambient_light_energy = 0.42
		env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
		env.tonemap_exposure = 0.86
		env.tonemap_white = 8.0
		env.rtgi_max_bounces = 8
		_build_cornell_box_scene(env)
		return
	if _scene_mode == "sponza":
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0.018, 0.020, 0.024)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.055, 0.056, 0.060)
		env.ambient_light_energy = 0.35
		env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
		env.rtgi_max_bounces = 5
		_build_sponza_scene(env)
		return
	if _is_coexistence_scene():
		_build_coexistence_scene(env)
		return
	if _scene_mode == "many_light_emissive":
		_build_many_light_emissive_scene(env)
		return

	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.006, 0.007, 0.009)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.015, 0.015, 0.018)
	env.ambient_light_energy = 0.15
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC

	var brick_material := _make_brick_material()
	var dark_material := _make_flat_material(Color(0.015, 0.013, 0.012), 0.92, 0.0)
	var matte_material := _make_flat_material(Color(0.45, 0.40, 0.33), 0.82, 0.0)
	var metal_material := _make_flat_material(Color(0.55, 0.58, 0.62), 0.18, 1.0)
	var polished_metal_material := _make_flat_material(Color(0.78, 0.74, 0.68), 0.045, 1.0)
	var glossy_material := _make_flat_material(Color(0.82, 0.78, 0.70), 0.035, 0.0)
	var emissive_material := _make_emissive_material(Color(1.0, 0.78, 0.45), 7.5)
	var shader_light_material := _make_shader_light_material()

	_add_box("BrickFloor", Vector3(0.0, -0.05, -1.0), Vector3(7.8, 0.10, 7.0), brick_material)
	_add_box("BrickBackWall", Vector3(0.0, 1.7, -4.1), Vector3(7.8, 3.5, 0.14), brick_material)
	_add_box("DarkRightWall", Vector3(3.85, 1.55, -1.2), Vector3(0.12, 3.4, 6.0), dark_material)
	_add_box("LeftBounceWall", Vector3(-3.85, 1.55, -1.2), Vector3(0.12, 3.4, 6.0), matte_material)
	_add_box("CeilingOccluder", Vector3(0.8, 3.05, -1.3), Vector3(6.1, 0.12, 5.8), dark_material)
	_add_box("MetalPatch", Vector3(1.65, 0.55, -2.95), Vector3(0.95, 0.95, 0.08), metal_material)
	_add_box("PolishedMetalStrip", Vector3(0.95, 0.35, -2.05), Vector3(1.15, 0.08, 0.55), polished_metal_material)
	_add_sphere("GlossySphere", Vector3(1.90, 0.52, -1.92), 0.34, glossy_material)
	_add_box("ShaderLightFallbackPatch", Vector3(-0.90, 0.72, -3.88), Vector3(0.70, 0.55, 0.05), shader_light_material)
	_add_box("SmallEmitter", Vector3(-1.95, 1.15, -3.92), Vector3(0.16, 0.16, 0.05), emissive_material)

	var light := OmniLight3D.new()
	light.name = "SmallWarmLight"
	light.position = Vector3(-2.0, 1.22, -3.55)
	light.light_energy = 3.2
	light.omni_range = 4.5
	light.shadow_enabled = true
	add_child(light)

	_camera = Camera3D.new()
	_camera.name = "MetricCamera"
	_camera.current = true
	_camera.fov = 58.0
	_camera.near = 0.05
	_camera.far = 80.0
	_camera.position = Vector3(0.15, 1.45, 2.5)
	add_child(_camera)
	_camera.look_at(Vector3(-0.15, 1.15, -2.5), Vector3.UP)


func _build_many_light_emissive_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.002, 0.002, 0.003)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.004, 0.004, 0.005)
	env.ambient_light_energy = 0.05
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 0.9
	env.rtgi_max_bounces = 5

	var wall := _make_flat_material(Color(0.032, 0.034, 0.038), 0.86, 0.0)
	var dark := _make_flat_material(Color(0.010, 0.011, 0.013), 0.92, 0.0)
	var detail := _make_flat_material(Color(0.34, 0.31, 0.27), 0.68, 0.0)
	var metal := _make_flat_material(Color(0.70, 0.72, 0.74), 0.10, 1.0)
	var emitter_a := _make_emissive_material(Color(1.0, 0.50, 0.18), 9.0)
	var emitter_b := _make_emissive_material(Color(0.30, 0.68, 1.0), 6.5)
	var emitter_c := _make_emissive_material(Color(0.55, 1.0, 0.45), 5.0)

	_add_box("ManyLightFloor", Vector3(0.0, -0.05, -2.0), Vector3(8.0, 0.10, 8.0), dark)
	_add_box("ManyLightBackWall", Vector3(0.0, 1.9, -5.9), Vector3(8.0, 4.0, 0.12), wall)
	_add_box("ManyLightLeftWall", Vector3(-4.0, 1.9, -2.0), Vector3(0.12, 4.0, 8.0), wall)
	_add_box("ManyLightRightWall", Vector3(4.0, 1.9, -2.0), Vector3(0.12, 4.0, 8.0), wall)
	_add_box("ManyLightCeiling", Vector3(0.0, 3.85, -2.0), Vector3(8.0, 0.12, 8.0), wall)
	_add_box("ManyLightDarkROI", Vector3(2.65, 1.05, -5.82), Vector3(1.8, 1.45, 0.06), dark)
	_add_box("ManyLightDetailROI", Vector3(-2.35, 0.8, -5.80), Vector3(1.5, 1.1, 0.06), detail)
	_add_box("ManyLightSpecularStrip", Vector3(0.55, 0.35, -3.35), Vector3(1.5, 0.08, 0.70), metal)
	_add_sphere("ManyLightSpecularSphere", Vector3(1.55, 0.52, -3.70), 0.34, metal)

	var emitter_positions := [
		Vector3(-2.9, 1.45, -5.76), Vector3(-1.55, 2.15, -5.76), Vector3(0.15, 1.35, -5.76),
		Vector3(1.85, 2.35, -5.76), Vector3(3.05, 1.55, -5.76), Vector3(-3.88, 1.8, -2.9),
		Vector3(3.88, 2.05, -2.1), Vector3(-0.60, 3.78, -2.8)
	]
	for i in range(emitter_positions.size()):
		var mat := emitter_a if i % 3 == 0 else (emitter_b if i % 3 == 1 else emitter_c)
		_add_box("ManyLightEmitter_%02d" % i, emitter_positions[i], Vector3(0.18, 0.18, 0.05), mat)

	for i in range(36):
		var col := i % 9
		var row := int(i / 9)
		var light := OmniLight3D.new()
		light.name = "ManyAnalytic_%02d" % i
		light.position = Vector3(-3.2 + float(col) * 0.8, 0.85 + float(row) * 0.55, -4.6 + sin(float(i) * 1.7) * 0.28)
		light.light_color = Color.from_hsv(fposmod(float(i) * 0.071, 1.0), 0.36, 1.0)
		light.light_energy = 0.22 + 0.08 * float(i % 4)
		light.omni_range = 2.7
		light.shadow_enabled = true
		add_child(light)

	_camera = Camera3D.new()
	_camera.name = "ManyLightCamera"
	_camera.current = true
	_camera.fov = 57.0
	_camera.near = 0.05
	_camera.far = 70.0
	_camera.position = Vector3(0.0, 1.55, 1.95)
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 1.55, -4.4), Vector3.UP)


func _is_coexistence_scene() -> bool:
	return _scene_mode in ["sdfgi", "voxelgi", "lightmap", "lightprobe", "path_traced_sdfgi_exclusive"]


func _build_coexistence_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.012, 0.014, 0.018)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.018, 0.019, 0.022)
	env.ambient_light_energy = 0.08
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.rtgi_mode = Environment.RTGI_MODE_PATH_TRACED if _scene_mode == "path_traced_sdfgi_exclusive" else Environment.RTGI_MODE_HYBRID
	env.rtgi_max_bounces = 4
	env.sdfgi_enabled = _scene_mode in ["sdfgi", "path_traced_sdfgi_exclusive"]
	if env.sdfgi_enabled:
		env.sdfgi_use_occlusion = true
		env.sdfgi_energy = 1.15

	var floor_material := _make_flat_material(Color(0.36, 0.35, 0.32), 0.78, 0.0)
	var wall_material := _make_flat_material(Color(0.30, 0.32, 0.36), 0.82, 0.0)
	var owner_material := _make_flat_material(Color(0.64, 0.60, 0.52), 0.72, 0.0)
	var emitter_material := _make_emissive_material(Color(0.16, 0.72, 1.0), 5.5)
	_add_box("CoexistFloor", Vector3(0.0, -0.05, -1.0), Vector3(5.4, 0.10, 5.2), floor_material)
	_add_box("CoexistBackWall", Vector3(0.0, 1.35, -3.45), Vector3(5.4, 2.8, 0.12), wall_material)
	_add_box("CoexistLeftWall", Vector3(-2.70, 1.35, -1.0), Vector3(0.12, 2.8, 5.2), wall_material)
	_add_box("CoexistRightWall", Vector3(2.70, 1.35, -1.0), Vector3(0.12, 2.8, 5.2), wall_material)
	_add_box("CoexistCyanEmitter", Vector3(-1.85, 1.15, -3.35), Vector3(0.46, 0.70, 0.06), emitter_material)

	match _scene_mode:
		"lightmap":
			var owner := _add_lightmapped_quad("CoexistLightmapOwner", owner_material)
			_add_procedural_lightmap(owner, Color(0.26, 0.72, 0.54, 1.0))
		"lightprobe":
			var probe_owner := _add_sphere("CoexistLightProbeDynamicOwner", Vector3(0.0, 0.76, -2.45), 0.55, owner_material)
			probe_owner.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
			_add_procedural_lightprobe_data(Color(0.28, 0.62, 0.94, 1.0))
		_:
			_add_box("CoexistRasterGIOwner", Vector3(0.0, 0.75, -2.80), Vector3(1.45, 1.35, 0.16), owner_material)

	var key := OmniLight3D.new()
	key.name = "CoexistRealtimeKey"
	key.position = Vector3(1.9, 2.2, 0.6)
	key.light_energy = 6.0 if _scene_mode == "path_traced_sdfgi_exclusive" else 0.65
	key.light_bake_mode = Light3D.BAKE_DYNAMIC
	key.omni_range = 5.0
	key.shadow_enabled = true
	add_child(key)

	var baked_static := OmniLight3D.new()
	baked_static.name = "CoexistStaticBakedLight"
	baked_static.position = Vector3(-1.25, 1.45, -2.15)
	baked_static.light_color = Color(0.50, 0.78, 1.0)
	baked_static.light_energy = 1.15
	baked_static.light_bake_mode = Light3D.BAKE_STATIC
	baked_static.omni_range = 4.0
	baked_static.shadow_enabled = true
	add_child(baked_static)

	if _scene_mode == "voxelgi":
		_add_voxelgi_probe()

	_camera = Camera3D.new()
	_camera.name = "CoexistenceCamera"
	_camera.current = true
	_camera.fov = 55.0
	_camera.near = 0.05
	_camera.far = 40.0
	_camera.position = Vector3(0.0, 1.25, 1.95)
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 0.88, -2.55), Vector3.UP)


func _add_lightmapped_quad(node_name: String, material: Material) -> MeshInstance3D:
	var vertices := PackedVector3Array([
		Vector3(-1.15, 0.20, -2.70),
		Vector3(1.15, 0.20, -2.70),
		Vector3(1.15, 1.95, -2.70),
		Vector3(-1.15, 0.20, -2.70),
		Vector3(1.15, 1.95, -2.70),
		Vector3(-1.15, 1.95, -2.70),
	])
	var normal := Vector3(0.0, 0.0, 1.0)
	var normals := PackedVector3Array([normal, normal, normal, normal, normal, normal])
	var uvs := PackedVector2Array([
		Vector2(0.0, 1.0),
		Vector2(1.0, 1.0),
		Vector2(1.0, 0.0),
		Vector2(0.0, 1.0),
		Vector2(1.0, 0.0),
		Vector2(0.0, 0.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_TEX_UV2] = uvs
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance


func _add_procedural_lightmap(owner: MeshInstance3D, color: Color) -> void:
	var image := Image.create_empty(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(color)
	var texture_array := Texture2DArray.new()
	texture_array.create_from_images([image])
	var data := LightmapGIData.new()
	data.set_uses_spherical_harmonics(false)
	data.set_lightmap_textures([texture_array])

	var lightmap := LightmapGI.new()
	lightmap.name = "CoexistProceduralLightmapGI"
	add_child(lightmap)
	data.add_user(lightmap.get_path_to(owner), Rect2(0.0, 0.0, 1.0, 1.0), 0, -1)
	lightmap.light_data = data


func _add_procedural_lightprobe_data(color: Color) -> void:
	var data := LightmapGIData.new()
	var points := PackedVector3Array([
		Vector3(-1.6, 0.0, -3.6),
		Vector3(1.6, 0.0, -3.6),
		Vector3(0.0, 2.6, -3.2),
		Vector3(0.0, 0.8, -0.8),
	])
	var sh := PackedColorArray()
	var l0 := Color(color.r / 0.886227, color.g / 0.886227, color.b / 0.886227, color.a)
	for i in range(4):
		sh.append(l0)
		for j in range(8):
			sh.append(Color(0, 0, 0, 0))
	var tetrahedra := PackedInt32Array([0, 1, 2, 3])
	var bsp := PackedInt32Array()
	bsp.resize(6)
	bsp[0] = 0
	bsp[1] = 0
	bsp[2] = 0
	bsp[3] = 0
	bsp[4] = -1
	bsp[5] = -1
	data.probe_data = {
		"bounds": AABB(Vector3(-2.0, -0.2, -3.8), Vector3(4.0, 3.2, 3.4)),
		"interior": false,
		"points": points,
		"sh": sh,
		"tetrahedra": tetrahedra,
		"bsp": bsp,
		"baked_exposure": 1.0,
		"lightprobe_hash": 1,
	}

	var lightmap := LightmapGI.new()
	lightmap.name = "CoexistProceduralLightProbeGI"
	add_child(lightmap)
	lightmap.light_data = data

	var probe := LightmapProbe.new()
	probe.name = "CoexistLightmapProbe"
	probe.position = Vector3(0.0, 0.8, -2.2)
	add_child(probe)


func _add_voxelgi_probe() -> void:
	var voxel := VoxelGI.new()
	voxel.name = "CoexistVoxelGI"
	voxel.size = Vector3(5.6, 3.2, 5.6)
	voxel.position = Vector3(0.0, 1.25, -1.55)
	voxel.subdiv = VoxelGI.SUBDIV_64
	add_child(voxel)
	voxel.bake(self, false)
	if voxel.data == null:
		push_warning("VoxelGI coexistence bake did not produce probe data; metrics will report the missing contribution.")


func _build_cornell_box_scene(_env: Environment) -> void:
	var white := _make_lambert_material(Color(0.705, 0.675, 0.515))
	var green := _make_lambert_material(Color(0.150, 0.360, 0.125))
	var red := _make_lambert_material(Color(0.520, 0.085, 0.070))
	var light_material := _make_lambert_emissive_material(Color(1.0, 0.84, 0.58), 3.2)

	var w := 5.56
	var h := 5.488
	var d := 5.592
	var t := 0.08
	_add_box("CornellFloor", Vector3(w * 0.5, -t * 0.5, d * 0.5), Vector3(w, t, d), white)
	_add_box("CornellCeiling", Vector3(w * 0.5, h + t * 0.5, d * 0.5), Vector3(w, t, d), white)
	_add_box("CornellBackWall", Vector3(w * 0.5, h * 0.5, d + t * 0.5), Vector3(w, h, t), white)
	# With this camera basis, world +X projects to screen-left; keep the
	# material placement aligned to Cornell's rendered reference.
	_add_box("CornellScreenRightGreenWall", Vector3(-t * 0.5, h * 0.5, d * 0.5), Vector3(t, h, d), green)
	_add_box("CornellScreenLeftRedWall", Vector3(w + t * 0.5, h * 0.5, d * 0.5), Vector3(t, h, d), red)
	_add_box("CornellCeilingEmitter", Vector3(2.78, h - 0.018, 2.795), Vector3(1.30, 0.035, 1.05), light_material)

	var tall := _add_box("CornellTallBlock", Vector3(3.685, 1.65, 3.512), Vector3(1.62, 3.30, 1.62), white)
	tall.rotation_degrees.y = 17.0
	var short := _add_box("CornellShortBlock", Vector3(1.855, 0.825, 1.69), Vector3(1.62, 1.65, 1.62), white)
	short.rotation_degrees.y = -17.0

	var light := OmniLight3D.new()
	light.name = "CornellCeilingAreaApproximation"
	light.position = _cornell_point(278.0, 520.0, 279.5)
	light.light_energy = 4.6
	light.light_size = 1.35
	light.omni_range = 8.0
	light.shadow_enabled = true
	add_child(light)

	var fill := SpotLight3D.new()
	fill.name = "CornellPanelSoftFill"
	fill.position = _cornell_point(278.0, 535.0, 279.5)
	fill.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	fill.light_energy = 0.38
	fill.spot_range = 8.0
	fill.spot_angle = 80.0
	fill.shadow_enabled = true
	add_child(fill)

	var bounce_fill := OmniLight3D.new()
	bounce_fill.name = "CornellBounceApproximation"
	bounce_fill.position = _cornell_point(278.0, 190.0, -90.0)
	bounce_fill.light_color = Color(1.0, 0.78, 0.46)
	bounce_fill.light_energy = 0.82
	bounce_fill.light_size = 2.2
	bounce_fill.omni_range = 7.0
	bounce_fill.shadow_enabled = false
	add_child(bounce_fill)

	_camera = Camera3D.new()
	_camera.name = "CornellCamera"
	_camera.current = true
	_camera.keep_aspect = Camera3D.KEEP_WIDTH
	_camera.fov = rad_to_deg(2.0 * atan(0.025 / (2.0 * 0.035)))
	_camera.near = 0.05
	_camera.far = 30.0
	_camera.position = _cornell_point(278.0, 273.0, -800.0)
	add_child(_camera)
	_camera.look_at(_cornell_point(278.0, 273.0, 0.0), Vector3.UP)


func _build_sponza_scene(_env: Environment) -> void:
	var asset_path := _resolve_sponza_path()
	if not asset_path.is_empty() and FileAccess.file_exists(asset_path):
		var gltf := GLTFDocument.new()
		var state := GLTFState.new()
		var error := gltf.append_from_file(asset_path, state)
		if error == OK:
			var imported := gltf.generate_scene(state)
			if imported != null:
				imported.name = "ExternalSponza"
				imported.scale = Vector3.ONE
				add_child(imported)
				_sponza_normal_y_flipped = _should_flip_sponza_normal_y(asset_path)
				if _sponza_normal_y_flipped:
					_flip_sponza_normal_maps(imported)
					print("RTGI quality: flipped Sponza normal map green channels for DirectX-style normal-Y mode (%d textures)." % _sponza_flipped_normal_texture_count)
				_mark_geometry_static(imported)
				_sponza_asset_loaded = true
		if not _sponza_asset_loaded:
			push_warning("Could not load Sponza asset from '%s'; using procedural fallback atrium." % asset_path)
	else:
		print("RTGI quality: Sponza asset is not configured. Set --rtgi-sponza-path=<gltf/glb> or GODOT_RTGI_SPONZA_PATH; using procedural fallback atrium.")

	if not _sponza_asset_loaded:
		_build_sponza_fallback_atrium()

	var sun := DirectionalLight3D.new()
	sun.name = "SponzaSun"
	sun.rotation_degrees = Vector3(-46.0, -32.0, 0.0)
	sun.light_energy = 4.0
	sun.shadow_enabled = true
	add_child(sun)

	var camera_fill := OmniLight3D.new()
	camera_fill.name = "SponzaSoftCameraFill"
	camera_fill.position = Vector3(-10.0, 4.2, 0.0)
	camera_fill.light_color = Color(1.0, 0.91, 0.78)
	camera_fill.light_energy = 4.2
	camera_fill.light_size = 3.5
	camera_fill.omni_range = 22.0
	camera_fill.shadow_enabled = false
	add_child(camera_fill)

	_camera = Camera3D.new()
	_camera.name = "SponzaCamera"
	_camera.current = true
	_camera.fov = 58.0
	_camera.near = 0.05
	_camera.far = 120.0
	_camera.position = Vector3(-12.0, 3.4, 0.0)
	add_child(_camera)
	_camera.look_at(Vector3(2.0, 3.0, 0.0), Vector3.UP)


func _build_sponza_fallback_atrium() -> void:
	var stone := _make_brick_material()
	var dark := _make_flat_material(Color(0.12, 0.11, 0.10), 0.86, 0.0)
	var red_cloth := _make_flat_material(Color(0.42, 0.05, 0.04), 0.72, 0.0)
	var gold := _make_flat_material(Color(0.86, 0.63, 0.24), 0.18, 1.0)
	_add_box("FallbackSponzaFloor", Vector3(0.0, -0.05, 0.0), Vector3(9.0, 0.10, 16.0), stone)
	_add_box("FallbackSponzaLeftArcade", Vector3(-4.5, 1.8, 0.0), Vector3(0.24, 3.8, 15.5), stone)
	_add_box("FallbackSponzaRightArcade", Vector3(4.5, 1.8, 0.0), Vector3(0.24, 3.8, 15.5), stone)
	_add_box("FallbackSponzaBack", Vector3(0.0, 1.8, -7.8), Vector3(8.8, 3.8, 0.24), stone)
	_add_box("FallbackSponzaCeiling", Vector3(0.0, 3.7, 0.0), Vector3(9.0, 0.18, 16.0), dark)
	for side in [-1.0, 1.0]:
		for i in range(7):
			var z := -6.0 + float(i) * 2.0
			_add_box("FallbackSponzaColumn_%s_%d" % ["L" if side < 0.0 else "R", i], Vector3(side * 3.35, 1.35, z), Vector3(0.34, 2.7, 0.34), stone)
			_add_box("FallbackSponzaCurtain_%s_%d" % ["L" if side < 0.0 else "R", i], Vector3(side * 3.16, 1.65, z + 0.85), Vector3(0.07, 1.8, 0.95), red_cloth)
	_add_box("FallbackSponzaSpecularBasin", Vector3(0.0, 0.38, 0.2), Vector3(1.7, 0.20, 1.7), gold)


func _run_capture() -> void:
	_apply_debug_view(_debug_view)
	for frame in range(_warmup_frames):
		if _camera_pan:
			_animate_camera(frame)
		await _wait_render_frame()

	var output_dir_error := DirAccess.make_dir_recursive_absolute(_output_dir)
	if output_dir_error != OK:
		push_error("Could not create RTGI quality output directory: %s" % _output_dir)
		get_tree().quit(2)
		return
	var base_name := "%s_rtgi_strength_%0.2f" % [_scene_mode, _denoise_strength]
	if DisplayServer.get_name().to_lower() == "headless":
		var skipped := {
			"skipped": true,
			"reason": "Viewport texture is unavailable. Run without --headless on a Vulkan RT-capable display for RTGI metrics.",
		}
		_write_json("%s/%s_metrics.json" % [_output_dir, base_name], skipped)
		print("RTGI quality metrics skipped: %s" % JSON.stringify(skipped))
		get_tree().quit(0)
		return
	var viewport_texture := get_viewport().get_texture()
	var final_image: Image = null
	if viewport_texture != null:
		final_image = viewport_texture.get_image()
	if final_image == null:
		var skipped := {
			"skipped": true,
			"reason": "Viewport texture is unavailable. Run without --headless on a Vulkan RT-capable display for RTGI metrics.",
		}
		_write_json("%s/%s_metrics.json" % [_output_dir, base_name], skipped)
		print("RTGI quality metrics skipped: %s" % JSON.stringify(skipped))
		get_tree().quit(0)
		return
	final_image.convert(Image.FORMAT_RGBA8)
	var png_path := "%s/%s_%s.png" % [_output_dir, base_name, _debug_view]
	var metrics_path := "%s/%s_metrics.json" % [_output_dir, base_name]
	var png_error := final_image.save_png(png_path)
	if png_error != OK:
		push_error("Could not write RTGI quality PNG: %s" % png_path)
		get_tree().quit(2)
		return

	var metrics := _measure_image(final_image)
	if _scene_mode == "cornell":
		metrics.merge(_measure_cornell_image(final_image), true)
		if _cornell_compare:
			metrics.merge(_compare_cornell_reference(final_image, base_name), true)
	elif _scene_mode == "sponza":
		metrics.merge(_measure_sponza_image(final_image), true)
	elif _is_coexistence_scene():
		metrics.merge(await _measure_coexistence_image(final_image, base_name), true)
	metrics["denoise_strength"] = _denoise_strength
	metrics["history_weight"] = _history_weight
	metrics["firefly_suppression"] = _firefly_suppression
	metrics["detail_preservation"] = _detail_preservation
	metrics["split_signals"] = _split_signals
	metrics["specular_history_weight"] = _specular_history_weight
	metrics["specular_spatial_strength"] = _specular_spatial_strength
	metrics["ray_firefly_suppression"] = _ray_firefly_suppression
	metrics["ray_max_radiance"] = _ray_max_radiance
	metrics["warmup_frames"] = _warmup_frames
	metrics["sparkle_frames"] = _sparkle_frames
	metrics["convergence_frames"] = _convergence_frames
	metrics["gate_profile"] = _gate_profile
	metrics["debug_view"] = _debug_view
	metrics["camera_pan"] = _camera_pan
	metrics["scene"] = _scene_mode
	metrics["sponza_asset_loaded"] = _sponza_asset_loaded
	metrics["analytic_light_sampling_enabled"] = _analytic_light_sampling
	metrics["explicit_emissive_sampling_enabled"] = _explicit_emissive_sampling
	if _sparkle_frames > 1 and DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_temporal_sparkle(base_name), true)
	if _convergence_frames > 1 and DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_convergence_curve(base_name), true)
	if DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_signal_debug_views(), true)
		metrics["rtgi_instability_attribution"] = _source_attribution_summary(metrics)

	var expected := _expected_metrics_for_scene(_load_expected_metrics())
	var failures := _compare_metrics(metrics, expected)
	metrics["expected_thresholds_applied"] = not expected.is_empty()
	metrics["passed"] = failures.is_empty()
	metrics["failures"] = failures
	_write_json(metrics_path, metrics)

	if _write_reference:
		_write_json(EXPECTED_METRICS_PATH, _reference_from_metrics(metrics))

	if _capture_all_debug_views:
		await _capture_debug_views(base_name)
	if _capture_comparison:
		await _capture_comparison_grid(base_name)

	print("RTGI quality metrics: %s" % JSON.stringify(metrics))
	print("RTGI quality output: %s" % ProjectSettings.globalize_path(_output_dir))
	get_tree().quit(0 if failures.is_empty() else 1)


func _animate_camera(frame: int) -> void:
	var t: float = float(frame) / maxf(float(_warmup_frames - 1), 1.0)
	if _scene_mode == "cornell":
		_camera.position = _cornell_point(lerpf(268.0, 288.0, t), 273.0, -800.0)
		_camera.look_at(_cornell_point(278.0, 273.0, 0.0), Vector3.UP)
		return
	if _scene_mode == "sponza":
		_camera.position = Vector3(lerpf(-12.6, -11.4, t), 3.4, lerpf(-0.25, 0.25, t))
		_camera.look_at(Vector3(2.0, 3.0, 0.0), Vector3.UP)
		return
	_camera.position.x = lerpf(-0.18, 0.32, t)
	_camera.look_at(Vector3(-0.05, 1.15, -2.5), Vector3.UP)


func _capture_debug_views(base_name: String) -> void:
	var views := ["beauty", "noisy", "diffuse_noisy", "specular_noisy", "diffuse_final", "specular_final", "specular_guide", "normal_roughness", "normal_deviation", "viewz_hitdist", "motion_vectors", "signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "variance", "history_length", "rejection", "final"]
	for view in views:
		_apply_debug_view(view)
		await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		image.convert(Image.FORMAT_RGBA8)
		image.save_png("%s/%s_%s.png" % [_output_dir, base_name, view])
	_apply_debug_view(_debug_view)


func _measure_signal_debug_views() -> Dictionary:
	var result := {}
	var previous_view := _debug_view
	var signal_views := ["signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "noisy", "final"]
	for view in signal_views:
		_apply_debug_view(view)
		await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		if image == null:
			continue
		image.convert(Image.FORMAT_RGBA8)
		var view_metrics := _measure_image(image)
		for key in view_metrics.keys():
			result["rtgi_%s_%s" % [view, key]] = view_metrics[key]
	_apply_debug_view(previous_view)
	return result


func _source_attribution_summary(metrics: Dictionary) -> String:
	var sources := {
		"analytic direct": _source_score(metrics, "signal_direct"),
		"visible/secondary emissive": _source_score(metrics, "signal_emissive"),
		"indirect/throughput": _source_score(metrics, "signal_indirect"),
		"sky/environment": _source_score(metrics, "signal_sky"),
		"clamp/weight risk": _source_score(metrics, "signal_confidence"),
		"final/post amplification": float(metrics.get("rtgi_final_visible_speckles_per_megapixel", 0.0)) + float(metrics.get("rtgi_final_luma_p99", 0.0)) * 120.0,
	}
	var best_name := "unknown"
	var best_score := -1.0
	for source in sources.keys():
		var score: float = sources[source]
		if score > best_score:
			best_score = score
			best_name = source
	return "%s (score %.3f)" % [best_name, best_score]


func _source_score(metrics: Dictionary, view: String) -> float:
	return float(metrics.get("rtgi_%s_visible_speckles_per_megapixel" % view, 0.0)) + float(metrics.get("rtgi_%s_full_frame_fireflies_per_megapixel" % view, 0.0)) + float(metrics.get("rtgi_%s_luma_p99" % view, 0.0)) * 120.0


func _capture_comparison_grid(base_name: String) -> void:
	if _environment == null or DisplayServer.get_name().to_lower() == "headless":
		return

	var configs := [
		{
			"name": "random_emissive_discovery_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": false,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "analytic_direct_only_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": true,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "analytic_explicit_emissive_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": true,
			"explicit_emissive_sampling": true,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_split_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_SVGF,
			"max_bounces": 3,
			"split_signals": true,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_single_beauty_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_SVGF,
			"max_bounces": 3,
			"split_signals": false,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "hybrid_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_HYBRID,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_SVGF,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "no_rtgi",
			"enabled": false,
			"mode": Environment.RTGI_MODE_HYBRID,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": false,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_reference_%dspp" % _reference_spp,
			"enabled": true,
			"mode": Environment.RTGI_MODE_PATH_TRACED,
			"spp": _reference_spp,
			"denoiser": Environment.RTGI_DENOISER_NONE,
				"max_bounces": 8,
				"split_signals": false,
				"analytic_light_sampling": true,
				"explicit_emissive_sampling": false,
				"ray_firefly_suppression": 0.0,
			"ray_max_radiance": 0.0,
		},
	]

	var captures: Array[Image] = []
	var manifest := {
		"base_name": base_name,
		"reference_spp": _reference_spp,
		"captures": [],
	}
	var comparison_warmup := mini(_warmup_frames, 45)
	for config in configs:
		_environment.rtgi_enabled = false
		await _wait_render_frame()
		_environment.rtgi_enabled = config["enabled"]
		_environment.rtgi_mode = config["mode"]
		_environment.rtgi_samples_per_pixel = config["spp"]
		_environment.rtgi_denoiser = config["denoiser"]
		_environment.rtgi_max_bounces = config["max_bounces"]
		_environment.rtgi_denoiser_split_signals = config["split_signals"]
		_environment.rtgi_analytic_light_sampling_enabled = config["analytic_light_sampling"]
		_environment.rtgi_explicit_emissive_sampling_enabled = config["explicit_emissive_sampling"]
		_environment.rtgi_ray_firefly_suppression = config["ray_firefly_suppression"]
		_environment.rtgi_ray_max_radiance = config["ray_max_radiance"]
		_apply_debug_view("beauty")
		for frame in range(comparison_warmup):
			await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		image.convert(Image.FORMAT_RGBA8)
		var path := "%s/%s_compare_%s.png" % [_output_dir, base_name, config["name"]]
		image.save_png(path)
		captures.append(image)
		manifest["captures"].append({
			"name": config["name"],
			"path": ProjectSettings.globalize_path(path),
			"rtgi_enabled": config["enabled"],
			"rtgi_mode": config["mode"],
			"spp": config["spp"],
			"denoiser": config["denoiser"],
			"max_bounces": config["max_bounces"],
			"split_signals": config["split_signals"],
			"analytic_light_sampling": config["analytic_light_sampling"],
			"explicit_emissive_sampling": config["explicit_emissive_sampling"],
			"ray_firefly_suppression": config["ray_firefly_suppression"],
			"ray_max_radiance": config["ray_max_radiance"],
		})

	if captures.is_empty():
		return
	var tile_w := captures[0].get_width()
	var tile_h := captures[0].get_height()
	var cols := 2
	var rows := int(ceil(float(captures.size()) / float(cols)))
	var grid := Image.create_empty(tile_w * cols, tile_h * rows, false, Image.FORMAT_RGBA8)
	grid.fill(Color(0, 0, 0, 1))
	for i in range(captures.size()):
		var x := (i % cols) * tile_w
		var y := int(i / cols) * tile_h
		grid.blit_rect(captures[i], Rect2i(Vector2i.ZERO, Vector2i(tile_w, tile_h)), Vector2i(x, y))
	var grid_path := "%s/%s_comparison_grid.png" % [_output_dir, base_name]
	grid.save_png(grid_path)
	manifest["grid_path"] = ProjectSettings.globalize_path(grid_path)
	_write_json("%s/%s_comparison_manifest.json" % [_output_dir, base_name], manifest)


func _wait_render_frame() -> void:
	if DisplayServer.get_name() == "headless":
		await get_tree().process_frame
	else:
		await RenderingServer.frame_post_draw


func _apply_debug_view(view: String) -> void:
	if _environment != null:
		_environment.rtgi_debug_mode = Environment.RT_DEBUG_NORMAL_DEVIATION if view == "normal_deviation" else Environment.RT_DEBUG_DISABLED
	RenderingServer.viewport_set_debug_draw(get_viewport().get_viewport_rid(), _debug_draw_value(view))


func _debug_draw_value(view: String) -> int:
	match view:
		"beauty", "disabled":
			return 0
		"noisy":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_NOISY
		"diffuse_noisy":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_NOISY
		"specular_noisy":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_NOISY
		"diffuse_final":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_FINAL
		"specular_final":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_FINAL
		"specular_guide":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_GUIDE
		"normal_roughness":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_NORMAL_ROUGHNESS
		"viewz_hitdist":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_VIEWZ_HITDIST
		"motion_vectors":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_MOTION_VECTORS
		"signal_direct":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_DIRECT_LIGHT
		"signal_emissive":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_EMISSIVE
		"signal_indirect":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_INDIRECT
		"signal_sky":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_SKY
		"signal_confidence":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_CONFIDENCE
		"variance":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_VARIANCE
		"history_length":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_HISTORY_LENGTH
		"rejection":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_REJECTION
		"final":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_FINAL
		_:
			return 0


func _measure_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var full_frame_fireflies: int = _count_isolated_hot_pixels(image, 0, 0, width, height)
	var visible_speckles: int = _count_visible_speckles(image, 0, 0, width, height)
	var visible_speckle_samples := _sampled_grid_pixel_count(width, height, 2, 2)
	var dark_x0 := int(width * 0.62)
	var dark_y0 := int(height * 0.18)
	var dark_x1 := int(width * 0.94)
	var dark_y1 := int(height * 0.84)
	var detail_x0 := int(width * 0.07)
	var detail_y0 := int(height * 0.30)
	var detail_x1 := int(width * 0.42)
	var detail_y1 := int(height * 0.76)
	var specular_x0 := int(width * 0.48)
	var specular_y0 := int(height * 0.46)
	var specular_x1 := int(width * 0.78)
	var specular_y1 := int(height * 0.75)

	var fireflies: int = _count_isolated_hot_pixels(image, dark_x0, dark_y0, dark_x1, dark_y1)
	var dark_pixels: int = maxi((dark_x1 - dark_x0) * (dark_y1 - dark_y0), 1)
	var detail: Dictionary = _measure_detail_region(image, detail_x0, detail_y0, detail_x1, detail_y1)
	var specular: Dictionary = _measure_detail_region(image, specular_x0, specular_y0, specular_x1, specular_y1)
	var luma_stats: Dictionary = _measure_luma_stats(image)
	var dark_mean: float = _measure_mean_luma(image, dark_x0, dark_y0, dark_x1, dark_y1)
	var specular_fireflies: int = _count_isolated_hot_pixels(image, specular_x0, specular_y0, specular_x1, specular_y1)

	return {
		"full_frame_fireflies": full_frame_fireflies,
		"full_frame_fireflies_per_megapixel": _per_megapixel(full_frame_fireflies, width * height),
		"visible_speckles": visible_speckles,
		"visible_speckles_per_megapixel": _per_megapixel(visible_speckles, visible_speckle_samples),
		"dark_fireflies": fireflies,
		"dark_fireflies_per_megapixel": _per_megapixel(fireflies, dark_pixels),
		"dark_roi_mean_luma": dark_mean,
		"detail_edge_energy": detail["edge_energy"],
		"detail_luma_stddev": detail["luma_stddev"],
		"specular_fireflies": specular_fireflies,
		"specular_edge_energy": specular["edge_energy"],
		"specular_luma_stddev": specular["luma_stddev"],
		"luma_p95": luma_stats["p95"],
		"luma_p99": luma_stats["p99"],
		"luma_max": luma_stats["max"],
		"saturated_luma_fraction": luma_stats["saturated_fraction"],
	}


func _measure_coexistence_image(rt_image: Image, base_name: String) -> Dictionary:
	var roi := _coexistence_owner_roi(rt_image)
	var rt_luma := _measure_mean_luma(rt_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var rt_color := _measure_mean_color(rt_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var metrics := {
		"coexistence_rt_owner_luma": rt_luma,
		"coexistence_rt_owner_blue_green_margin": maxf(rt_color.b, rt_color.g) - rt_color.r,
		"coexistence_mode": _environment.rtgi_mode if _environment != null else -1,
	}
	if _environment == null:
		return metrics

	var previous_enabled := _environment.rtgi_enabled
	var previous_mode := _environment.rtgi_mode
	_environment.rtgi_enabled = false
	_apply_debug_view("beauty")
	for frame in range(mini(_warmup_frames, 24)):
		await _wait_render_frame()
	var raster_image := get_viewport().get_texture().get_image()
	raster_image.convert(Image.FORMAT_RGBA8)
	var raster_path := "%s/%s_raster_fallback.png" % [_output_dir, base_name]
	raster_image.save_png(raster_path)
	var raster_luma := _measure_mean_luma(raster_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var raster_color := _measure_mean_color(raster_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)

	_environment.rtgi_enabled = previous_enabled
	_environment.rtgi_mode = previous_mode
	for frame in range(mini(_warmup_frames, 24)):
		await _wait_render_frame()

	metrics["coexistence_raster_owner_luma"] = raster_luma
	metrics["coexistence_raster_owner_blue_green_margin"] = maxf(raster_color.b, raster_color.g) - raster_color.r
	metrics["coexistence_owner_luma_delta"] = rt_luma - raster_luma
	metrics["coexistence_owner_luma_ratio"] = rt_luma / maxf(raster_luma, 0.001)
	metrics["coexistence_raster_fallback_path"] = ProjectSettings.globalize_path(raster_path)
	return metrics


func _coexistence_owner_roi(image: Image) -> Rect2i:
	var width := image.get_width()
	var height := image.get_height()
	return Rect2i(
			Vector2i(int(width * 0.36), int(height * 0.29)),
			Vector2i(int(width * 0.28), int(height * 0.34)))


func _measure_cornell_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var center_detail := _measure_detail_region(image, int(width * 0.30), int(height * 0.46), int(width * 0.72), int(height * 0.86))
	var ceiling_hot_pixels := _count_isolated_hot_pixels(image, int(width * 0.34), int(height * 0.02), int(width * 0.66), int(height * 0.28))
	var red_wall := _measure_mean_luma(image, int(width * 0.03), int(height * 0.16), int(width * 0.22), int(height * 0.78))
	var green_wall := _measure_mean_luma(image, int(width * 0.78), int(height * 0.16), int(width * 0.97), int(height * 0.78))
	var back_wall := _measure_mean_luma(image, int(width * 0.28), int(height * 0.16), int(width * 0.72), int(height * 0.56))
	var floor_luma := _measure_mean_luma(image, int(width * 0.15), int(height * 0.70), int(width * 0.86), int(height * 0.96))
	var ceiling_luma := _measure_mean_luma(image, int(width * 0.16), int(height * 0.02), int(width * 0.86), int(height * 0.25))
	var wall_min: float = minf(minf(red_wall, green_wall), minf(back_wall, minf(floor_luma, ceiling_luma)))
	var black_002 := _measure_black_fraction(image, 0, 0, width, height, 0.02)
	var black_005 := _measure_black_fraction(image, 0, 0, width, height, 0.05)
	var red_wall_black := _measure_black_fraction(image, int(width * 0.04), int(height * 0.18), int(width * 0.20), int(height * 0.68), 0.05)
	var green_wall_black := _measure_black_fraction(image, int(width * 0.80), int(height * 0.18), int(width * 0.96), int(height * 0.68), 0.05)
	var back_wall_black := _measure_black_fraction(image, int(width * 0.28), int(height * 0.16), int(width * 0.72), int(height * 0.56), 0.05)
	var floor_black := _measure_black_fraction(image, int(width * 0.15), int(height * 0.70), int(width * 0.86), int(height * 0.96), 0.05)
	var flat_noise := _measure_flat_patch_noise(image, int(width * 0.56), int(height * 0.26), int(width * 0.72), int(height * 0.48))
	var red_chroma := _measure_mean_color(image, int(width * 0.03), int(height * 0.16), int(width * 0.22), int(height * 0.78))
	var green_chroma := _measure_mean_color(image, int(width * 0.78), int(height * 0.16), int(width * 0.97), int(height * 0.78))
	return {
		"cornell_center_edge_energy": center_detail["edge_energy"],
		"cornell_center_luma_stddev": center_detail["luma_stddev"],
		"cornell_ceiling_hot_pixels": ceiling_hot_pixels,
		"cornell_black_fraction_luma_002": black_002,
		"cornell_black_fraction_luma_005": black_005,
		"cornell_red_wall_black_fraction": red_wall_black,
		"cornell_green_wall_black_fraction": green_wall_black,
		"cornell_back_wall_black_fraction": back_wall_black,
		"cornell_floor_black_fraction": floor_black,
		"cornell_flat_patch_noise": flat_noise,
		"cornell_red_wall_chroma_margin": red_chroma.r - maxf(red_chroma.g, red_chroma.b),
		"cornell_green_wall_chroma_margin": green_chroma.g - maxf(green_chroma.r, green_chroma.b),
		"cornell_red_wall_mean_luma": red_wall,
		"cornell_green_wall_mean_luma": green_wall,
		"cornell_back_wall_mean_luma": back_wall,
		"cornell_floor_mean_luma": floor_luma,
		"cornell_ceiling_mean_luma": ceiling_luma,
		"cornell_wall_min_luma": wall_min,
		"cornell_left_right_luma_delta": red_wall - green_wall,
		"cornell_public_data_url": CORNELL_PUBLIC_DATA_URL,
		"cornell_reference_url": CORNELL_REFERENCE_URL,
	}


func _measure_black_fraction(image: Image, x0: int, y0: int, x1: int, y1: int, threshold: float) -> float:
	var black := 0
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height())):
		for x in range(maxi(x0, 0), mini(x1, image.get_width())):
			if _luma(image.get_pixel(x, y)) < threshold:
				black += 1
			count += 1
	return float(black) / maxf(float(count), 1.0)


func _measure_flat_patch_noise(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height() - 1)):
		for x in range(maxi(x0, 0), mini(x1, image.get_width() - 1)):
			var l := _luma(image.get_pixel(x, y))
			sum += absf(l - _luma(image.get_pixel(x + 1, y)))
			sum += absf(l - _luma(image.get_pixel(x, y + 1)))
			count += 2
	return sum / maxf(float(count), 1.0)


func _measure_mean_color(image: Image, x0: int, y0: int, x1: int, y1: int) -> Color:
	var sum := Color(0, 0, 0, 0)
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height())):
		for x in range(maxi(x0, 0), mini(x1, image.get_width())):
			sum += image.get_pixel(x, y)
			count += 1
	return sum / maxf(float(count), 1.0)


func _measure_temporal_sparkle(base_name: String) -> Dictionary:
	_apply_debug_view("beauty")
	await _wait_render_frame()
	var previous := get_viewport().get_texture().get_image()
	previous.convert(Image.FORMAT_RGBA8)
	var first := previous.duplicate()
	var max_sparkle_pixels := 0
	var total_sparkle_pixels := 0
	var sampled_pixels := 0
	var last := previous
	for frame in range(1, _sparkle_frames):
		await _wait_render_frame()
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		var frame_sparkles := _count_temporal_sparkles(previous, current)
		max_sparkle_pixels = maxi(max_sparkle_pixels, frame_sparkles)
		total_sparkle_pixels += frame_sparkles
		sampled_pixels = maxi(sampled_pixels, (current.get_width() / 2) * (current.get_height() / 2))
		previous = current
		last = current
	var first_path := "%s/%s_sparkle_first.png" % [_output_dir, base_name]
	var last_path := "%s/%s_sparkle_last.png" % [_output_dir, base_name]
	first.save_png(first_path)
	last.save_png(last_path)
	return {
		"temporal_sparkle_pixels_max": max_sparkle_pixels,
		"temporal_sparkle_pixels_avg": float(total_sparkle_pixels) / maxf(float(_sparkle_frames - 1), 1.0),
		"temporal_sparkle_per_megapixel_max": _per_megapixel(max_sparkle_pixels, sampled_pixels),
		"temporal_sparkle_per_megapixel_avg": _per_megapixel(total_sparkle_pixels, sampled_pixels * maxi(_sparkle_frames - 1, 1)),
	}


func _measure_convergence_curve(base_name: String) -> Dictionary:
	_apply_debug_view("beauty")
	var previous_enabled := false
	if _environment != null:
		previous_enabled = _environment.rtgi_enabled
		_environment.rtgi_enabled = false
		await _wait_render_frame()
		_environment.rtgi_enabled = previous_enabled
	var previous: Image = null
	var first: Image = null
	var last: Image = null
	var curve := []
	for frame in range(_convergence_frames):
		await _wait_render_frame()
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		if first == null:
			first = current.duplicate()
		last = current.duplicate()
		var frame_metrics := _measure_image(current)
		var sampled_pixels := int(current.get_width() / 2) * int(current.get_height() / 2)
		var delta_sparkles := 0
		if previous != null:
			delta_sparkles = _count_temporal_sparkles(previous, current)
		curve.append({
			"frame": frame,
			"luma_p95": frame_metrics["luma_p95"],
			"luma_p99": frame_metrics["luma_p99"],
			"luma_max": frame_metrics["luma_max"],
			"full_frame_fireflies_per_megapixel": frame_metrics["full_frame_fireflies_per_megapixel"],
			"visible_speckles_per_megapixel": frame_metrics["visible_speckles_per_megapixel"],
			"frame_delta_sparkle_per_megapixel": _per_megapixel(delta_sparkles, sampled_pixels),
		})
		previous = current
	var first_path := "%s/%s_convergence_first.png" % [_output_dir, base_name]
	var last_path := "%s/%s_convergence_last.png" % [_output_dir, base_name]
	if first != null:
		first.save_png(first_path)
	if last != null:
		last.save_png(last_path)
	var curve_path := "%s/%s_convergence_curve.json" % [_output_dir, base_name]
	var payload := {
		"frames": _convergence_frames,
		"curve": curve,
		"first_path": ProjectSettings.globalize_path(first_path),
		"last_path": ProjectSettings.globalize_path(last_path),
	}
	_write_json(curve_path, payload)
	return {
		"convergence_curve_path": ProjectSettings.globalize_path(curve_path),
		"convergence_first_path": ProjectSettings.globalize_path(first_path),
		"convergence_last_path": ProjectSettings.globalize_path(last_path),
		"convergence_last_luma_p99": curve.back()["luma_p99"] if not curve.is_empty() else 0.0,
		"convergence_last_fireflies_per_megapixel": curve.back()["full_frame_fireflies_per_megapixel"] if not curve.is_empty() else 0.0,
		"convergence_last_frame_delta_sparkle_per_megapixel": curve.back()["frame_delta_sparkle_per_megapixel"] if not curve.is_empty() else 0.0,
	}


func _measure_sponza_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var corridor_detail := _measure_detail_region(image, int(width * 0.16), int(height * 0.18), int(width * 0.84), int(height * 0.84))
	var deep_shadow_fireflies := _count_isolated_hot_pixels(image, int(width * 0.08), int(height * 0.12), int(width * 0.92), int(height * 0.90))
	return {
		"sponza_asset_path": _resolve_sponza_path(),
		"sponza_external_asset_missing": not _sponza_asset_loaded,
		"sponza_khronos_source_url": SPONZA_KHRONOS_SOURCE_URL,
		"sponza_source": "khronos_gltf_sample_assets" if _is_khronos_sponza_path(_resolve_sponza_path()) else "external",
		"sponza_canonical_asset": _is_khronos_sponza_path(_resolve_sponza_path()),
		"sponza_normal_y_mode": _sponza_normal_y_mode,
		"sponza_normal_y_flipped": _sponza_normal_y_flipped,
		"sponza_flipped_normal_texture_count": _sponza_flipped_normal_texture_count,
		"sponza_corridor_edge_energy": corridor_detail["edge_energy"],
		"sponza_corridor_luma_stddev": corridor_detail["luma_stddev"],
		"sponza_deep_shadow_fireflies": deep_shadow_fireflies,
	}


func _compare_cornell_reference(rendered_image: Image, base_name: String) -> Dictionary:
	var reference_path := _resolve_cornell_reference_path()
	var metrics := {
		"cornell_reference_available": false,
		"cornell_reference_path": reference_path,
		"cornell_reference_url": CORNELL_REFERENCE_URL,
		"cornell_public_data_url": CORNELL_PUBLIC_DATA_URL,
	}
	if reference_path.is_empty() or not FileAccess.file_exists(reference_path):
		push_warning("Cornell reference image was not found. Place Cornell's simulated image at '%s' or pass --rtgi-cornell-reference-image=<path>." % reference_path)
		return metrics

	var reference := Image.new()
	var load_error := reference.load(reference_path)
	if load_error != OK:
		push_warning("Could not load Cornell reference image '%s'." % reference_path)
		return metrics

	var rendered := rendered_image.duplicate()
	rendered.convert(Image.FORMAT_RGBA8)
	reference.convert(Image.FORMAT_RGBA8)
	if reference.get_width() != rendered.get_width() or reference.get_height() != rendered.get_height():
		reference.resize(rendered.get_width(), rendered.get_height(), Image.INTERPOLATE_LANCZOS)

	metrics.merge(_measure_image_difference(rendered, reference, "cornell_reference"), true)
	metrics["cornell_reference_available"] = true
	_write_cornell_reference_images(rendered, reference, base_name)
	return metrics


func _measure_image_difference(rendered: Image, reference: Image, prefix: String) -> Dictionary:
	var width := mini(rendered.get_width(), reference.get_width())
	var height := mini(rendered.get_height(), reference.get_height())
	var rgb_abs_sum := 0.0
	var rgb_sq_sum := 0.0
	var luma_abs_sum := 0.0
	var luma_sq_sum := 0.0
	var max_luma_error := 0.0
	var count := maxi(width * height, 1)
	for y in range(height):
		for x in range(width):
			var a := rendered.get_pixel(x, y)
			var b := reference.get_pixel(x, y)
			var dr := a.r - b.r
			var dg := a.g - b.g
			var db := a.b - b.b
			var rgb_abs := (absf(dr) + absf(dg) + absf(db)) / 3.0
			var rgb_sq := (dr * dr + dg * dg + db * db) / 3.0
			var dl := _luma(a) - _luma(b)
			rgb_abs_sum += rgb_abs
			rgb_sq_sum += rgb_sq
			luma_abs_sum += absf(dl)
			luma_sq_sum += dl * dl
			max_luma_error = maxf(max_luma_error, absf(dl))
	return {
		"%s_rgb_mae" % prefix: rgb_abs_sum / float(count),
		"%s_rgb_rmse" % prefix: sqrt(rgb_sq_sum / float(count)),
		"%s_luma_mae" % prefix: luma_abs_sum / float(count),
		"%s_luma_rmse" % prefix: sqrt(luma_sq_sum / float(count)),
		"%s_luma_max_error" % prefix: max_luma_error,
	}


func _write_cornell_reference_images(rendered: Image, reference: Image, base_name: String) -> void:
	var width := rendered.get_width()
	var height := rendered.get_height()
	var diff := Image.create_empty(width, height, false, Image.FORMAT_RGBA8)
	for y in range(height):
		for x in range(width):
			var a := rendered.get_pixel(x, y)
			var b := reference.get_pixel(x, y)
			var error := clampf(absf(_luma(a) - _luma(b)) * 4.0, 0.0, 1.0)
			diff.set_pixel(x, y, Color(error, error * 0.35, 1.0 - error, 1.0))
	var reference_path := "%s/%s_cornell_reference_resized.png" % [_output_dir, base_name]
	var diff_path := "%s/%s_cornell_reference_diff.png" % [_output_dir, base_name]
	var grid_path := "%s/%s_cornell_reference_grid.png" % [_output_dir, base_name]
	reference.save_png(reference_path)
	diff.save_png(diff_path)
	var grid := Image.create_empty(width * 3, height, false, Image.FORMAT_RGBA8)
	grid.fill(Color.BLACK)
	grid.blit_rect(rendered, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i.ZERO)
	grid.blit_rect(reference, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i(width, 0))
	grid.blit_rect(diff, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i(width * 2, 0))
	grid.save_png(grid_path)


func _count_isolated_hot_pixels(image: Image, x0: int, y0: int, x1: int, y1: int) -> int:
	var count := 0
	for y in range(max(y0, 1), min(y1, image.get_height() - 1)):
		for x in range(max(x0, 1), min(x1, image.get_width() - 1)):
			var center := _luma(image.get_pixel(x, y))
			var neighbor_sum := 0.0
			var neighbor_count := 0
			for ny in range(-1, 2):
				for nx in range(-1, 2):
					if nx == 0 and ny == 0:
						continue
					neighbor_sum += _luma(image.get_pixel(x + nx, y + ny))
					neighbor_count += 1
			var neighbor_mean := neighbor_sum / float(neighbor_count)
			if center > max(0.16, neighbor_mean * 4.0 + 0.035):
				count += 1
	return count


func _count_visible_speckles(image: Image, x0: int, y0: int, x1: int, y1: int) -> int:
	var count := 0
	for y in range(max(y0, 2), min(y1, image.get_height() - 2), 2):
		for x in range(max(x0, 2), min(x1, image.get_width() - 2), 2):
			var center := _luma(image.get_pixel(x, y))
			var neighbor_sum := 0.0
			var neighbor_max := 0.0
			var neighbor_count := 0
			for ny in range(-2, 3):
				for nx in range(-2, 3):
					if nx == 0 and ny == 0:
						continue
					var tap_luma := _luma(image.get_pixel(x + nx, y + ny))
					neighbor_sum += tap_luma
					neighbor_max = maxf(neighbor_max, tap_luma)
					neighbor_count += 1
			var neighbor_mean := neighbor_sum / maxf(float(neighbor_count), 1.0)
			var local_threshold := maxf(0.045, maxf(neighbor_mean * 2.15 + 0.018, neighbor_max * 1.28 + 0.006))
			if center > local_threshold:
				count += 1
	return count


func _count_temporal_sparkles(previous: Image, current: Image) -> int:
	var width := mini(previous.get_width(), current.get_width())
	var height := mini(previous.get_height(), current.get_height())
	var count := 0
	for y in range(1, height - 1, 2):
		for x in range(1, width - 1, 2):
			var prev_luma := _luma(previous.get_pixel(x, y))
			var curr_luma := _luma(current.get_pixel(x, y))
			var delta := absf(curr_luma - prev_luma)
			var support := maxf(prev_luma, curr_luma)
			if support <= 0.060 or delta <= maxf(0.055, support * 0.48):
				continue
			var prev_neighbor := _local_neighbor_mean_luma(previous, x, y)
			var curr_neighbor := _local_neighbor_mean_luma(current, x, y)
			var local_support := maxf(prev_neighbor, curr_neighbor)
			var local_edge_range := maxf(_local_neighbor_luma_range(previous, x, y), _local_neighbor_luma_range(current, x, y))
			if local_edge_range > 0.10 and delta < local_edge_range * 1.00:
				continue
			var prev_isolated := prev_luma > maxf(prev_neighbor * 1.65 + 0.020, prev_neighbor + 0.055)
			var curr_isolated := curr_luma > maxf(curr_neighbor * 1.65 + 0.020, curr_neighbor + 0.055)
			var dark_flash := local_support < 0.12 and delta > maxf(0.050, support * 0.42)
			if prev_isolated or curr_isolated or dark_flash:
				count += 1
	return count


func _sampled_grid_pixel_count(width: int, height: int, stride: int, margin: int) -> int:
	var sample_w := int(ceil(float(maxi(width - margin * 2, 1)) / float(maxi(stride, 1))))
	var sample_h := int(ceil(float(maxi(height - margin * 2, 1)) / float(maxi(stride, 1))))
	return maxi(sample_w * sample_h, 1)


func _local_neighbor_mean_luma(image: Image, x: int, y: int) -> float:
	var sum := 0.0
	var count := 0
	for ny in range(-1, 2):
		for nx in range(-1, 2):
			if nx == 0 and ny == 0:
				continue
			sum += _luma(image.get_pixel(x + nx, y + ny))
			count += 1
	return sum / maxf(float(count), 1.0)


func _local_neighbor_luma_range(image: Image, x: int, y: int) -> float:
	var min_luma := 1.0
	var max_luma := 0.0
	for ny in range(-1, 2):
		for nx in range(-1, 2):
			var luma := _luma(image.get_pixel(x + nx, y + ny))
			min_luma = minf(min_luma, luma)
			max_luma = maxf(max_luma, luma)
	return max_luma - min_luma


func _per_megapixel(count: int, pixels: int) -> float:
	return float(count) / maxf(float(pixels) / 1000000.0, 1e-5)


func _measure_detail_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> Dictionary:
	var edge_sum := 0.0
	var edge_count := 0
	var luma_sum := 0.0
	var luma_sq_sum := 0.0
	var count := 0
	for y in range(y0, min(y1, image.get_height() - 1)):
		for x in range(x0, min(x1, image.get_width() - 1)):
			var luma := _luma(image.get_pixel(x, y))
			var luma_x := _luma(image.get_pixel(x + 1, y))
			var luma_y := _luma(image.get_pixel(x, y + 1))
			edge_sum += absf(luma - luma_x) + absf(luma - luma_y)
			edge_count += 2
			luma_sum += luma
			luma_sq_sum += luma * luma
			count += 1
	var mean: float = luma_sum / maxf(float(count), 1.0)
	return {
		"edge_energy": edge_sum / max(float(edge_count), 1.0),
		"luma_stddev": sqrt(max(luma_sq_sum / max(float(count), 1.0) - mean * mean, 0.0)),
	}


func _measure_mean_luma(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			sum += _luma(image.get_pixel(x, y))
			count += 1
	return sum / max(float(count), 1.0)


func _measure_luma_stats(image: Image) -> Dictionary:
	var values: Array[float] = []
	var max_luma := 0.0
	var step := 2
	for y in range(0, image.get_height(), step):
		for x in range(0, image.get_width(), step):
			if _is_intentional_emitter_metric_pixel(x, y, image.get_width(), image.get_height()):
				continue
			var luma := _luma(image.get_pixel(x, y))
			values.append(luma)
			max_luma = maxf(max_luma, luma)
	values.sort()
	if values.is_empty():
		return { "p95": 0.0, "p99": 0.0, "max": 0.0, "saturated_fraction": 0.0 }
	var p95_idx := clampi(int(round(float(values.size() - 1) * 0.95)), 0, values.size() - 1)
	var p99_idx := clampi(int(round(float(values.size() - 1) * 0.99)), 0, values.size() - 1)
	var saturated := 0
	for value in values:
		if value >= 0.985:
			saturated += 1
	return {
		"p95": values[p95_idx],
		"p99": values[p99_idx],
		"max": max_luma,
		"saturated_fraction": float(saturated) / maxf(float(values.size()), 1.0),
	}


func _is_intentional_emitter_metric_pixel(x: int, y: int, width: int, height: int) -> bool:
	var nx := float(x) / maxf(float(width), 1.0)
	var ny := float(y) / maxf(float(height), 1.0)
	if _scene_mode == "stress" or _scene_mode == "convergence":
		return nx >= 0.34 and nx <= 0.49 and ny >= 0.37 and ny <= 0.60
	if _scene_mode == "cornell":
		return nx >= 0.37 and nx <= 0.63 and ny >= 0.06 and ny <= 0.22
	return false


func _luma(color: Color) -> float:
	return max(color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722, 0.0)


func _load_expected_metrics() -> Dictionary:
	if not FileAccess.file_exists(EXPECTED_METRICS_PATH):
		return {}
	var file := FileAccess.open(EXPECTED_METRICS_PATH, FileAccess.READ)
	if file == null:
		return {}
	var parsed = JSON.parse_string(file.get_as_text())
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _expected_metrics_for_scene(expected: Dictionary) -> Dictionary:
	if _scene_mode == "sponza" and not _sponza_asset_loaded:
		return {}
	if expected.has("scenes"):
		var scenes = expected["scenes"]
		if typeof(scenes) == TYPE_DICTIONARY and scenes.has(_scene_mode):
			var scene_expected = scenes[_scene_mode]
			return scene_expected if typeof(scene_expected) == TYPE_DICTIONARY else {}
		if _scene_mode == "many_light_emissive":
			return {}
	return expected


func _compare_metrics(metrics: Dictionary, expected: Dictionary) -> Array[String]:
	var failures: Array[String] = []
	if _gate_profile == "strict" and _scene_mode == "cornell" and _cornell_compare and not bool(metrics.get("cornell_reference_available", false)):
		failures.append("cornell_reference_available is false; strict Cornell comparison requires the external reference image")
	var thresholds: Dictionary = expected.get("thresholds", {})
	_check_max_threshold(metrics, thresholds, "dark_fireflies_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "full_frame_fireflies_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "visible_speckles_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "temporal_sparkle_per_megapixel_max", failures)
	_check_max_threshold(metrics, thresholds, "dark_roi_mean_luma", failures)
	_check_min_threshold(metrics, thresholds, "detail_edge_energy", failures)
	_check_min_threshold(metrics, thresholds, "detail_luma_stddev", failures)
	_check_max_threshold(metrics, thresholds, "specular_fireflies", failures)
	_check_min_threshold(metrics, thresholds, "specular_edge_energy", failures)
	_check_max_threshold(metrics, thresholds, "luma_p99", failures)
	_check_max_threshold(metrics, thresholds, "luma_max", failures)
	_check_max_threshold(metrics, thresholds, "saturated_luma_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_ceiling_hot_pixels", failures)
	_check_min_threshold(metrics, thresholds, "cornell_center_edge_energy", failures)
	_check_max_threshold(metrics, thresholds, "cornell_reference_luma_mae", failures)
	_check_max_threshold(metrics, thresholds, "cornell_black_fraction_luma_002", failures)
	_check_max_threshold(metrics, thresholds, "cornell_black_fraction_luma_005", failures)
	_check_max_threshold(metrics, thresholds, "cornell_red_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_green_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_back_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_floor_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_flat_patch_noise", failures)
	_check_min_threshold(metrics, thresholds, "cornell_red_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_green_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_wall_min_luma", failures)
	_check_min_threshold(metrics, thresholds, "sponza_corridor_edge_energy", failures)
	_check_min_threshold(metrics, thresholds, "sponza_corridor_luma_stddev", failures)
	_check_max_threshold(metrics, thresholds, "sponza_deep_shadow_fireflies", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_rt_owner_luma", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_raster_owner_luma", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_rt_owner_blue_green_margin", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_raster_owner_blue_green_margin", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_rt_owner_luma", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_owner_luma_ratio", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_owner_luma_delta", failures)
	return failures


func _check_max_threshold(metrics: Dictionary, thresholds: Dictionary, key: String, failures: Array[String]) -> void:
	if not thresholds.has("max_%s" % key):
		return
	if not metrics.has(key):
		failures.append("%s is missing; required max threshold %.6f was not measured" % [key, float(thresholds["max_%s" % key])])
		return
	var limit: float = thresholds["max_%s" % key]
	var actual: float = metrics[key]
	if actual > limit:
		failures.append("%s %.6f exceeds %.6f" % [key, actual, limit])


func _check_min_threshold(metrics: Dictionary, thresholds: Dictionary, key: String, failures: Array[String]) -> void:
	if not thresholds.has("min_%s" % key):
		return
	if not metrics.has(key):
		failures.append("%s is missing; required min threshold %.6f was not measured" % [key, float(thresholds["min_%s" % key])])
		return
	var limit: float = thresholds["min_%s" % key]
	var actual: float = metrics[key]
	if actual < limit:
		failures.append("%s %.6f below %.6f" % [key, actual, limit])


func _reference_from_metrics(metrics: Dictionary) -> Dictionary:
	return {
		"description": "Baseline thresholds for the RTGI quality harness. Regenerate intentionally after visual review.",
		"thresholds": {
			"max_dark_fireflies_per_megapixel": metrics["dark_fireflies_per_megapixel"] * 1.15 + 4.0,
			"max_dark_roi_mean_luma": metrics["dark_roi_mean_luma"] * 1.10 + 0.005,
			"min_detail_edge_energy": metrics["detail_edge_energy"] * 0.85,
			"min_detail_luma_stddev": metrics["detail_luma_stddev"] * 0.85,
			"max_specular_fireflies": metrics["specular_fireflies"] * 1.25 + 4.0,
			"min_specular_edge_energy": metrics["specular_edge_energy"] * 0.75,
			"max_luma_p99": metrics["luma_p99"] * 1.20 + 0.02,
			"max_luma_max": metrics["luma_max"] * 1.35 + 0.05,
		},
	}


func _write_json(path: String, payload: Dictionary) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("Could not write RTGI quality JSON: %s" % path)
		get_tree().quit(2)
		return
	file.store_string(JSON.stringify(payload, "\t"))


func _resolve_cornell_reference_path() -> String:
	if not _cornell_reference_image.is_empty():
		return _cornell_reference_image
	var env_path := OS.get_environment("GODOT_RTGI_CORNELL_REFERENCE")
	if not env_path.is_empty():
		return env_path
	return "D:/dev/rtgi_external_assets/cornell/simulated.jpg"


func _resolve_sponza_path() -> String:
	if not _sponza_path.is_empty():
		return _sponza_path
	var env_path := OS.get_environment("GODOT_RTGI_SPONZA_PATH")
	if not env_path.is_empty():
		return env_path
	var default_gltf := "D:/dev/rtgi_external_assets/sponza/Models/Sponza/glTF/Sponza.gltf"
	if FileAccess.file_exists(default_gltf):
		return default_gltf
	var flat_default_gltf := "D:/dev/rtgi_external_assets/sponza/Sponza.gltf"
	if FileAccess.file_exists(flat_default_gltf):
		return flat_default_gltf
	var default_glb := "D:/dev/rtgi_external_assets/sponza/Sponza.glb"
	if FileAccess.file_exists(default_glb):
		return default_glb
	return default_gltf


func _is_khronos_sponza_path(path: String) -> bool:
	return path.replace("\\", "/").ends_with(SPONZA_KHRONOS_CANONICAL_SUFFIX)


func _should_flip_sponza_normal_y(asset_path: String) -> bool:
	match _sponza_normal_y_mode:
		"directx":
			return true
		"opengl":
			return false
		_:
			# glTF normal maps are +Y tangent-space. The Khronos Sponza package notes
			# that its normals were manually inspected, so auto mode keeps them as-is.
			return false


func _flip_sponza_normal_maps(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		var mesh := mesh_instance.mesh
		if mesh != null:
			for surface_index in range(mesh.get_surface_count()):
				var material := mesh_instance.get_surface_override_material(surface_index)
				if material == null:
					material = mesh.surface_get_material(surface_index)
				var flipped_material := _material_with_flipped_normal_y(material)
				if flipped_material != null and flipped_material != material:
					mesh_instance.set_surface_override_material(surface_index, flipped_material)
	for child in node.get_children():
		_flip_sponza_normal_maps(child)


func _material_with_flipped_normal_y(material: Material) -> Material:
	if material == null or not (material is StandardMaterial3D):
		return material
	var standard_material := material as StandardMaterial3D
	var normal_texture := standard_material.get_texture(BaseMaterial3D.TEXTURE_NORMAL)
	if normal_texture == null:
		return material
	var flipped_texture := _flipped_normal_y_texture(normal_texture)
	if flipped_texture == normal_texture:
		return material
	var flipped_material := standard_material.duplicate(true) as StandardMaterial3D
	flipped_material.set_texture(BaseMaterial3D.TEXTURE_NORMAL, flipped_texture)
	flipped_material.set_feature(BaseMaterial3D.FEATURE_NORMAL_MAPPING, true)
	return flipped_material


func _flipped_normal_y_texture(texture: Texture2D) -> Texture2D:
	var key := texture.get_instance_id()
	if _normal_flip_cache.has(key):
		return _normal_flip_cache[key]
	var image := texture.get_image()
	if image == null or image.is_empty():
		return texture
	if image.is_compressed():
		var decompress_error := image.decompress()
		if decompress_error != OK:
			push_warning("Could not decompress Sponza normal texture for green-channel flip.")
			return texture
	image.convert(Image.FORMAT_RGBA8)
	var width := image.get_width()
	var height := image.get_height()
	var data := image.get_data()
	var index := 1
	while index < data.size():
		data[index] = 255 - data[index]
		index += 4
	image.set_data(width, height, false, Image.FORMAT_RGBA8, data)
	var flipped_texture := ImageTexture.create_from_image(image)
	_normal_flip_cache[key] = flipped_texture
	_sponza_flipped_normal_texture_count += 1
	return flipped_texture


func _mark_geometry_static(node: Node) -> void:
	if node is GeometryInstance3D:
		node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	for child in node.get_children():
		_mark_geometry_static(child)


func _cornell_point(x: float, y: float, z: float) -> Vector3:
	return Vector3(x, y, z) * 0.01


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


func _add_cornell_block(block_name: String, faces: Array, material: Material, block_center: Vector3) -> void:
	for i in range(faces.size()):
		var face: Array = faces[i]
		var center := Vector3.ZERO
		for point in face:
			center += point
		center /= 4.0
		_add_cornell_quad("%sFace%d" % [block_name, i], face, material, (center - block_center).normalized())


func _add_cornell_solid(node_name: String, points: Array, outward_normal: Vector3, thickness: float, material: Material) -> void:
	var front: Array = points.duplicate()
	var back: Array = []
	for point in front:
		back.append(point + outward_normal.normalized() * thickness)
	_add_cornell_quad("%sInner" % node_name, front, material, -outward_normal)
	_add_cornell_quad("%sOuter" % node_name, [back[3], back[2], back[1], back[0]], material, outward_normal)
	for i in range(4):
		var j := (i + 1) % 4
		_add_cornell_quad("%sEdge%d" % [node_name, i], [front[i], front[j], back[j], back[i]], material, (front[j] - front[i]).cross(outward_normal).normalized())


func _add_cornell_quad(node_name: String, points: Array, material: Material, desired_normal: Vector3) -> MeshInstance3D:
	var quad: Array = points.duplicate()
	if quad.size() != 4:
		push_error("Cornell quad '%s' does not have four vertices." % node_name)
		return null
	_orient_quad(quad, desired_normal)

	var normal := _quad_normal(quad)
	var vertices := PackedVector3Array([quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]])
	var normals := PackedVector3Array([normal, normal, normal, normal, normal, normal])
	var uvs := PackedVector2Array([
		Vector2(0.0, 0.0),
		Vector2(1.0, 0.0),
		Vector2(1.0, 1.0),
		Vector2(0.0, 0.0),
		Vector2(1.0, 1.0),
		Vector2(0.0, 1.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance


func _orient_quad(quad: Array, desired_normal: Vector3) -> void:
	var normal := _quad_normal(quad)
	if normal.dot(desired_normal.normalized()) < 0.0:
		var b = quad[1]
		quad[1] = quad[3]
		quad[3] = b


func _quad_normal(quad: Array) -> Vector3:
	return (quad[1] - quad[0]).cross(quad[2] - quad[0]).normalized()


func _make_brick_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_texture = _make_brick_texture()
	material.albedo_color = Color(1.0, 1.0, 1.0)
	material.roughness = 0.88
	material.metallic = 0.0
	material.uv1_scale = Vector3(5.0, 5.0, 1.0)
	return material


func _make_flat_material(color: Color, roughness: float, metallic: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	return material


func _make_emissive_material(color: Color, energy: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = energy
	material.roughness = 0.35
	return material


func _make_shader_light_material() -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = "\n".join(PackedStringArray([
		"shader_type spatial;",
		"render_mode specular_schlick_ggx;",
		"void fragment() {",
		"	ALBEDO = vec3(0.18, 0.42, 0.72);",
		"	ROUGHNESS = 0.42;",
		"	METALLIC = 0.0;",
		"	EMISSION = vec3(0.0);",
		"}",
		"void light() {",
		"	DIFFUSE_LIGHT += ATTENUATION * LIGHT_COLOR * max(dot(NORMAL, LIGHT), 0.0) * vec3(0.2, 0.5, 1.0);",
		"}",
	]))
	var material := ShaderMaterial.new()
	material.shader = shader
	return material


func _make_brick_texture() -> ImageTexture:
	var image := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in range(256):
		for x in range(256):
			var row := int(y / 32)
			var offset := 32 if row % 2 == 1 else 0
			var local_x := (x + offset) % 64
			var local_y := y % 32
			var mortar := local_x < 3 or local_y < 3
			var seed := (x * 17 + y * 31 + (x * y) % 29) % 23
			var grain := float(seed) / 22.0
			var base := Color(0.47 + grain * 0.10, 0.29 + grain * 0.055, 0.16 + grain * 0.035)
			var color := Color(0.10, 0.085, 0.070) if mortar else base
			image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _add_box(node_name: String, position: Vector3, size: Vector3, material: Material) -> MeshInstance3D:
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
	add_child(instance)
	return instance


func _add_sphere(node_name: String, position: Vector3, radius: float, material: Material) -> MeshInstance3D:
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
	add_child(instance)
	return instance
