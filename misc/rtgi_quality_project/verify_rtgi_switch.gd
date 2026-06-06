# In-process RTGI mode/scene switch stress test. Reproduces the conditions that
# made the editor crash when switching between RTGI scenes: cycling the GI mode
# (which flips the material-guide prepass on and off) while forcing render-buffer
# reconfigures (viewport size toggles tear the RT textures down). A transition
# frame that consumes a torn-down or not-yet-created material guide used to crash
# the resolve/composite; this test renders through many such transitions and must
# finish without a crash and without flooding the named-texture error.
#
# Run: faster-godot...console.exe --path <this project> --script res://verify_rtgi_switch.gd
extends SceneTree

var _root: Window
var _env: Environment
var _frame := 0
var _iter := 0
var _modes: Array[int] = []
const ITERATIONS := 48
const FRAMES_PER_ITER := 3


func _initialize() -> void:
	_root = get_root()
	_root.size = Vector2i(640, 360)
	_modes = [Environment.RTGI_MODE_HYBRID, Environment.RTGI_MODE_REFLECTIONS_RT_ONLY, Environment.RTGI_MODE_FULL_PATH_TRACING]

	var scene := Node3D.new()
	_root.add_child(scene)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.0, 4.0)
	scene.add_child(cam)
	cam.look_at(Vector3.ZERO, Vector3.UP)
	cam.current = true

	var floor_mesh := MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(10.0, 0.1, 10.0)
	floor_mesh.mesh = floor_box
	floor_mesh.position = Vector3(0.0, -0.6, 0.0)
	scene.add_child(floor_mesh)

	var mesh := MeshInstance3D.new()
	mesh.mesh = SphereMesh.new()
	var mat := StandardMaterial3D.new()
	mat.metallic = 1.0
	mat.roughness = 0.12
	mesh.material_override = mat
	scene.add_child(mesh)

	var light := OmniLight3D.new()
	light.position = Vector3(2.0, 3.0, 2.0)
	light.light_energy = 4.0
	scene.add_child(light)

	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.05, 0.06, 0.08)
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_env.rtgi_enabled = true
	_env.rtgi_disable_in_editor = false
	_env.rtgi_mode = Environment.RTGI_MODE_HYBRID
	var we := WorldEnvironment.new()
	we.environment = _env
	scene.add_child(we)
	print("RTGI-SWITCH-TEST start")


func _process(_delta: float) -> bool:
	_frame += 1
	if _frame % FRAMES_PER_ITER != 0:
		return false
	_iter += 1
	if _iter >= ITERATIONS:
		print("RTGI-SWITCH-TEST COMPLETE: %d iterations, no crash" % ITERATIONS)
		return true
	_env.rtgi_mode = _modes[_iter % _modes.size()]
	# Toggle the viewport size to force a render-buffer reconfigure (clear_context),
	# which is what tears the RT textures down on a real scene/editor switch.
	if _iter % 3 == 0:
		_root.size = Vector2i(800, 450) if (_iter % 6 == 0) else Vector2i(640, 360)
	return false
