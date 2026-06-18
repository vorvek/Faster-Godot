extends Node3D

# Deterministic driver for the light_toggle harness scene. The scene is static
# (no camera orbit, no moving geometry): the only event is the harness flipping
# the key light off and on while it watches how fast the indirect re-settles.
# advance_to_frame() therefore has nothing to move, but it is kept so the scene
# honors the same frame-counter contract as the other committed scenes and the
# harness can step it in lock-step without special-casing.
#
# set_toggle_light() flips the dedicated key light. Turning it off removes the
# dominant direct light source, so the indirect bounce that fills the shadow
# behind the static occluder collapses and then has to rebuild when it comes
# back on: that rebuild is the reconvergence the harness measures.
#
# get_occluder_floor_contact_point() and get_occluded_floor_point() hand the
# harness world-space points so it can project the leak and contact-occlusion
# measurement rects onto the actual framing instead of hard-coding pixels.


func _ready() -> void:
	# Author-time pose is already correct; nothing to animate on frame 0.
	pass


# Kept for the frame-counter contract. The scene is static, so this is a no-op
# beyond marking that the harness, not _process, drives the frame cadence.
func advance_to_frame(_frame: int) -> void:
	pass


# Flips the dedicated key light. The harness calls this mid-measurement to
# remove (then restore) the dominant direct source and time the indirect
# reconvergence.
func set_toggle_light(p_visible: bool) -> void:
	var light := get_node_or_null("KeyLight") as OmniLight3D
	if light != null:
		light.visible = p_visible


# World-space floor point at the base of the static occluder, on the shadowed
# (back) side. The harness projects the contact-sharpness rect around this point
# so it straddles the dark contact line where the occluder meets the floor.
func get_occluder_floor_contact_point() -> Vector3:
	var occluder := get_node_or_null("Occluder") as Node3D
	if occluder == null:
		return Vector3(0.0, 0.0, -1.0)
	var p := occluder.global_position
	# Just behind the occluder base (further from the key light, which sits in
	# front of and above the occluder), at floor level.
	return Vector3(p.x, 0.0, p.z - 0.45)


# World-space floor point well inside the occluder's shadow umbra, where the
# floor receives essentially no direct light from the key light and is lit only
# by indirect bounce. The harness centers the leak ROI here.
func get_occluded_floor_point() -> Vector3:
	var occluder := get_node_or_null("Occluder") as Node3D
	if occluder == null:
		return Vector3(0.0, 0.0, -2.4)
	var p := occluder.global_position
	return Vector3(p.x, 0.0, p.z - 1.9)
