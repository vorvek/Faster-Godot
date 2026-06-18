extends Node3D

# Deterministic, frame-counter driven driver for the motion_ghost harness scene.
# A single bright OmniLight3D (with a small co-located emissive marker so the hot
# spot also reads in the image) orbits a circle on a large neutral diffuse floor
# at a constant rate per frame. The camera is STATIC and looks straight down, so
# the only thing that moves on screen is the bright pool the light paints on the
# floor: this isolates the diffuse screen-probe-gather (SPG) indirect path. The
# motion is driven by an integer frame counter (never delta-time), so a captured
# run reproduces frame for frame.
#
# A CIRCULAR orbit (rather than a straight sweep) is used so the light stays on
# the floor and keeps moving in one consistent direction no matter how large the
# frame counter gets. The harness frame counter accumulates monotonically across
# the settle and across every mode in a multi-config run, so a straight sweep
# would drive the light off the floor edge by the second mode; an orbit is
# bounded and periodic, so every mode sees the light on the floor and moving.
#
# The SPG reprojects last frame's indirect forward and (unlike the world radiance
# cache) has no rel_change accumulation-collapse, so a continuously moving bright
# feature can smear stale indirect into the floor it just vacated.
# get_vacated_point() returns the orbit floor point the light occupied GHOST_LAG
# frames ago; over GHOST_LAG frames the light moves far enough around the orbit
# that its (small) direct pool has fully cleared that point, so any luma left
# there is indirect/temporal. get_current_point() returns the floor point under
# the light now. The harness samples the indirect luma at the vacated point (the
# trail) against the current point (the live pool) so the residual reads as a
# ratio.

# Orbit on the floor (world X-Z plane). Radius keeps the pool well inside the
# 16 m floor; the light rides at a low height so its pool stays compact. The
# per-frame angular step is brisk enough that over GHOST_LAG frames the light
# travels several pool radii along the orbit, fully clearing the vacated point.
const ORBIT_RADIUS := 3.4
const ORBIT_HEIGHT := 1.4
const ORBIT_DEGREES_PER_FRAME := 1.6
# Frames between the live pool and the vacated sample point. At
# ORBIT_DEGREES_PER_FRAME this is GHOST_LAG * 1.6 = 80 degrees of arc, a chord of
# 2 * ORBIT_RADIUS * sin(40 deg) = ~4.4 m. The light's direct pool on the floor
# has a radius of about sqrt(omni_range^2 - height^2) = sqrt(3^2 - 1.4^2) ~= 2.65
# m, so a 4.4 m separation puts the vacated point well outside the live direct
# pool: any luma left there is indirect/temporal, which is the SPG trail signal.
const GHOST_LAG := 50

var _frame := 0
var _self_drive := true


func _ready() -> void:
	_apply_frame(0)


func _process(_delta: float) -> void:
	# Only self-advance when nobody is stepping the scene explicitly. The harness
	# calls advance_to_frame() and clears self-drive to keep captures deterministic.
	if not _self_drive:
		return
	_frame += 1
	_apply_frame(_frame)


# Called by the harness so the light motion tracks the deterministic capture loop.
func advance_to_frame(frame: int) -> void:
	_self_drive = false
	_frame = frame
	_apply_frame(frame)


func _orbit_point_at(frame: int) -> Vector3:
	var a := deg_to_rad(ORBIT_DEGREES_PER_FRAME * float(frame))
	return Vector3(sin(a) * ORBIT_RADIUS, 0.0, cos(a) * ORBIT_RADIUS)


func _apply_frame(frame: int) -> void:
	var p := _orbit_point_at(frame)
	var mover := get_node_or_null("Mover") as Node3D
	if mover != null:
		mover.position = Vector3(p.x, ORBIT_HEIGHT, p.z)


# World floor point directly under the light's CURRENT orbit position: the live
# lit pool. The harness reads this as the context reference for the ghost ratio.
func get_current_point() -> Vector3:
	return _orbit_point_at(_frame)


# World floor point the light's pool covered GHOST_LAG frames ago but has since
# orbited well past. On a trail-free fork this reads near the static-background
# indirect; an SPG trail leaves elevated residual indirect here. The harness
# centers the ghost ROI on this point.
func get_vacated_point() -> Vector3:
	return _orbit_point_at(_frame - GHOST_LAG)
