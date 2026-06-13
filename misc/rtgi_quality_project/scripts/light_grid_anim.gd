extends Node3D

# Deterministic animation for the light_grid harness scene. advance_to_frame()
# orbits the camera around the room center (the light re-sort stressor: the
# engine orders positional lights by camera distance, so a moving camera keeps
# reordering the 24-light grid) and orbits the emissive sphere (the
# emissive-candidate signature stressor). set_toggle_light() flips one grid
# light, which is a REAL light-set change: the temporal history reset it causes
# is correct behavior, and the harness gates how fast the image recovers.
#
# The motion is driven by an integer frame counter, never delta-time or
# wall-clock, so a captured run reproduces frame for frame. The frame-0 camera
# pose must match the Transform3D constant authored in
# tools/generate_test_scenes.gd (_build_light_grid_scene), so settle and
# animation agree on the first frame.

const ORBIT_RADIUS := 3.0
const ORBIT_HEIGHT := 1.7
const ORBIT_DEGREES_PER_FRAME := 0.35
const EMISSIVE_RADIUS := 2.0
const EMISSIVE_HEIGHT := 1.2
const EMISSIVE_DEGREES_PER_FRAME := 1.2

var _frame := 0
var _self_drive := true


func _ready() -> void:
	_apply_frame(0)


func _process(_delta: float) -> void:
	# Only self-advance when nobody is stepping the scene explicitly. The
	# harness calls advance_to_frame() and clears self-drive to keep captures
	# deterministic.
	if not _self_drive:
		return
	_frame += 1
	_apply_frame(_frame)


# Called by the harness so the animation tracks the deterministic capture loop.
func advance_to_frame(frame: int) -> void:
	_self_drive = false
	_frame = frame
	_apply_frame(frame)


# Flips the dedicated grid light. The harness calls this mid-measurement to
# produce a real light-set change and then measures the recovery.
func set_toggle_light(p_visible: bool) -> void:
	var light := get_node_or_null("GridLights/ToggleLight") as OmniLight3D
	if light != null:
		light.visible = p_visible


# World-space floor point directly under the toggle light, so the harness can
# project a measurement rect around the patch the toggled light dominates.
func get_toggle_light_floor_point() -> Vector3:
	var light := get_node_or_null("GridLights/ToggleLight") as Node3D
	if light == null:
		return Vector3.ZERO
	var p := light.global_position
	return Vector3(p.x, 0.0, p.z)


func _apply_frame(frame: int) -> void:
	var camera := get_node_or_null("Camera3D") as Camera3D
	if camera != null:
		var a := deg_to_rad(ORBIT_DEGREES_PER_FRAME * float(frame))
		var pos := Vector3(sin(a) * ORBIT_RADIUS, ORBIT_HEIGHT, cos(a) * ORBIT_RADIUS)
		camera.transform = Transform3D(Basis.looking_at(-pos.normalized(), Vector3.UP), pos)
	var orbiter := get_node_or_null("EmissiveOrbiter") as Node3D
	if orbiter != null:
		var b := deg_to_rad(EMISSIVE_DEGREES_PER_FRAME * float(frame))
		orbiter.position = Vector3(sin(b) * EMISSIVE_RADIUS, EMISSIVE_HEIGHT, cos(b) * EMISSIVE_RADIUS)
