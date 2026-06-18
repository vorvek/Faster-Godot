extends Node3D

# Deterministic driver for the wall_leak harness scene. The scene is static (no
# camera orbit, no moving geometry, no scripted toggle): it exists purely so the
# harness can read two steady-state ROIs after the normal settle. advance_to_frame()
# is therefore a no-op, kept only so the scene honors the same frame-counter
# contract as the other committed scenes and the harness can step it without
# special-casing.
#
# A thin opaque divider on the x = 0 plane splits one box into a LIT half (+X,
# holding a bright OmniLight3D) and a SEALED DARK half (-X, no light). The
# divider spans the full interior height and depth, and the floor, all four side
# walls, and the full-height divider are solid shadow-casters, so the only
# interior boundary between the halves is the opaque divider: no legitimate light
# path reaches the dark half. The box is open at the top (no ceiling) so a
# straight-down camera sees both chamber floors; with no ceiling there is no
# surface above the divider for the lit half's light to bounce off and back down
# into the dark half. The world radiance cache stores radiance in texels that
# face the lit side of the divider; a query for a probe just inside the dark half
# that lacks a DDGI-style backface/wrap weight can pick up that lit-side radiance
# and leak it through the sealed wall. The dark-half ROI reads that leak.
#
# get_dark_probe_point() and get_lit_probe_point() hand the harness world-space
# points so it projects the leak and context ROIs onto the actual framing rather
# than hard-coding pixels. Both points sit on the floor near the divider at
# matching |x| offsets, so wall_leak_luma / wall_lit_luma is a clean leak
# fraction.


func _ready() -> void:
	# Author-time pose is already correct; nothing to animate on frame 0.
	pass


# Kept for the frame-counter contract. The scene is static, so this is a no-op
# beyond marking that the harness, not _process, drives the frame cadence.
func advance_to_frame(_frame: int) -> void:
	pass


# World-space floor point inside the SEALED DARK half, just past the divider on
# the unlit side. The harness centers the leak ROI here. The divider stands on
# the x = 0 plane; the dark half is the -X side, so this sits a short way into it
# at floor level, set back from the front opening so the front baffle and open
# edge do not clip the ROI. A leak-free fork reads near-zero indirect luma here.
func get_dark_probe_point() -> Vector3:
	return Vector3(-1.6, 0.0, -0.4)


# World-space floor point inside the LIT half, the mirror of the dark probe
# across the divider (same |x| offset, +X side, same z). It is lit by the key
# light's indirect bounce, so its luma is the context reference the leak is read
# against: wall_leak_luma / wall_lit_luma is the through-wall leak fraction.
func get_lit_probe_point() -> Vector3:
	return Vector3(1.6, 0.0, -0.4)
