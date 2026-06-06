extends Node3D

# Deterministic, frame-counter driven animation for the reflective_pool test
# scene. Emissive spheres at clearly different emission energies float above a
# near-mirror plane and gently bob up and down. The point is to test sharp
# reflections of the emissives in the plane plus the emissive GI that bleeds
# onto the plane, while the slow vertical motion keeps the reflections moving so
# temporal handling of sharp specular is exercised.
#
# As with specular_motion, the bob is driven by an integer frame counter (never
# delta-time or wall-clock) so a captured run reproduces frame for frame. The
# harness calls advance_to_frame(); _process() self-drives when the scene runs
# alone.

const BOB_AMPLITUDE := 0.18
const BOB_RADIANS_PER_FRAME := 0.040

var _frame := 0
var _self_drive := true
var _emitters: Array[Node3D] = []
var _base_heights: Array[float] = []


func _ready() -> void:
	_collect_nodes()
	_apply_frame(0)


func _collect_nodes() -> void:
	_emitters.clear()
	_base_heights.clear()
	var emitters_root := get_node_or_null("Emitters")
	if emitters_root != null:
		for child in emitters_root.get_children():
			if child is Node3D:
				_emitters.append(child)
				_base_heights.append((child as Node3D).position.y)


func _process(_delta: float) -> void:
	if not _self_drive:
		return
	_frame += 1
	_apply_frame(_frame)


# Called by the harness so the bob tracks the deterministic capture loop.
func advance_to_frame(frame: int) -> void:
	_self_drive = false
	_frame = frame
	if _emitters.is_empty():
		_collect_nodes()
	_apply_frame(frame)


func _apply_frame(frame: int) -> void:
	for i in range(_emitters.size()):
		var node := _emitters[i]
		if not is_instance_valid(node):
			continue
		# Each emitter bobs on its own phase so the reflected highlights do not
		# move in lockstep, which keeps the specular history under realistic load.
		var phase := float(frame) * BOB_RADIANS_PER_FRAME + float(i) * 1.1
		var base_height: float = _base_heights[i] if i < _base_heights.size() else node.position.y
		node.position.y = base_height + sin(phase) * BOB_AMPLITUDE
