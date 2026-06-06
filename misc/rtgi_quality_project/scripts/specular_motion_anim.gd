extends Node3D

# Deterministic, frame-counter driven animation for the specular_motion test
# scene. Low-roughness metallic spheres orbit the scene center while colored
# OmniLight3D nodes orbit on opposing paths, so specular highlights sweep across
# the surfaces every frame. This is the temporal-stability stressor for the RTGI
# denoiser. The motion is driven by an integer frame counter, never delta-time
# or wall-clock, so a captured run is reproducible frame for frame.
#
# The harness drives this animation through advance_to_frame() so it stays in
# lock-step with the warmup/capture loop. When the scene is opened on its own
# (for example in the editor) _process() advances the same counter so the scene
# is still alive and animated.

const SPHERE_ORBIT_RADIUS := 2.4
const SPHERE_ORBIT_HEIGHT := 0.85
const SPHERE_BOB_AMPLITUDE := 0.10
const LIGHT_ORBIT_RADIUS := 3.0
const LIGHT_ORBIT_HEIGHT := 2.35
const SPHERE_RADIANS_PER_FRAME := 0.018
const LIGHT_RADIANS_PER_FRAME := 0.024

var _frame := 0
var _self_drive := true
var _spheres: Array[Node3D] = []
var _lights: Array[Node3D] = []


func _ready() -> void:
	_collect_nodes()
	_apply_frame(0)


func _collect_nodes() -> void:
	_spheres.clear()
	_lights.clear()
	var spheres_root := get_node_or_null("Spheres")
	if spheres_root != null:
		for child in spheres_root.get_children():
			if child is Node3D:
				_spheres.append(child)
	var lights_root := get_node_or_null("MovingLights")
	if lights_root != null:
		for child in lights_root.get_children():
			if child is Node3D:
				_lights.append(child)


func _process(_delta: float) -> void:
	# Only self-advance when nobody is stepping the scene explicitly. The harness
	# calls advance_to_frame() and clears self-drive to keep captures deterministic.
	if not _self_drive:
		return
	_frame += 1
	_apply_frame(_frame)


# Called by the harness so the animation tracks the deterministic capture loop.
func advance_to_frame(frame: int) -> void:
	_self_drive = false
	_frame = frame
	if _spheres.is_empty() and _lights.is_empty():
		_collect_nodes()
	_apply_frame(frame)


func _apply_frame(frame: int) -> void:
	var sphere_phase := float(frame) * SPHERE_RADIANS_PER_FRAME
	for i in range(_spheres.size()):
		var node := _spheres[i]
		if not is_instance_valid(node):
			continue
		var angle := sphere_phase + float(i) * (TAU / float(maxi(_spheres.size(), 1)))
		var bob := sin(float(frame) * 0.05 + float(i)) * SPHERE_BOB_AMPLITUDE
		node.position = Vector3(
				cos(angle) * SPHERE_ORBIT_RADIUS,
				SPHERE_ORBIT_HEIGHT + bob,
				sin(angle) * SPHERE_ORBIT_RADIUS)
		node.rotation_degrees.y = fposmod(float(frame) * 1.7 + float(i) * 40.0, 360.0)

	var light_phase := float(frame) * LIGHT_RADIANS_PER_FRAME
	for i in range(_lights.size()):
		var node := _lights[i]
		if not is_instance_valid(node):
			continue
		# Opposing orbit direction from the spheres so highlights cross surfaces.
		var angle := -light_phase + float(i) * (TAU / float(maxi(_lights.size(), 1)))
		var height_wobble := sin(float(frame) * 0.037 + float(i) * 1.3) * 0.55
		node.position = Vector3(
				cos(angle) * LIGHT_ORBIT_RADIUS,
				LIGHT_ORBIT_HEIGHT + height_wobble,
				sin(angle) * LIGHT_ORBIT_RADIUS)
