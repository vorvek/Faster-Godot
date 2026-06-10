extends Node3D

# Fog-parity measurement for the committed fog_corridor test scene.
#
# One harness invocation measures one fog configuration; the comparison spans
# three runs that share one --rtgi-output-dir:
#
#   1. --rtgi-mode=off     captures the raster reference (RTGI disabled, the
#                          raster depth-fog path) and records the per-rect
#                          floor luminances to fog_corridor_raster_reference.json.
#   2. --rtgi-mode=hybrid  measure the same rects and compare against the
#   3. --rtgi-mode=fpt     recorded raster reference.
#
# Eight probe rects sample the open floor strip (x in [0.2, 0.7]; no markers,
# no glass, no marker shadows) at graded depths from about 3 m out to 38 m, so
# the per-depth fog ramp is compared directly. With depth-mode fog (begin 2 m,
# end 30 m) the raster reference keeps near segments bright and mid segments
# lightly hazed; a ray path that instead applies exponential fog at density
# 1.0 grays everything past a few meters out to the fog color. The seam pair
# samples the floor at one depth (17 m) through the alpha-blend glass pane and
# beside it on the clear side, so fog applied differently to alpha-blended and
# opaque geometry shows up as a seam shift relative to the raster reference.
#
# Machine-readable verdict, printed once per run:
#
#   FOGPAR mode=off reference recorded
#   FOGPAR mode=<rtgi_mode> max_rel_err=<f> seam=<f> verdict=<PASS|FAIL>
#
# where max_rel_err = max over rects(|rtgi - raster| / max(raster, 1e-3)),
# seam = |(glass - clear)_rtgi - (glass - clear)_raster|, and PASS requires
# max_rel_err <= 0.10.

const REFERENCE_FILE_NAME := "fog_corridor_raster_reference.json"
const PASS_MAX_REL_ERR := 0.10

# Probe depths along the floor strip. The first sits just past the fog begin
# distance (and just inside the bottom of the frame); the rest grade out to
# the fog-saturated far end. Each quad spans PROBE_X0..PROBE_X1 in x and
# +/- the half depth in z around the listed value.
const PROBE_DEPTHS: Array[float] = [-2.8, -6.0, -10.0, -14.0, -18.0, -24.0, -30.0, -38.0]
const PROBE_NEAR_HALF_DEPTH := 0.4
const PROBE_HALF_DEPTH := 0.7
const PROBE_X0 := 0.2
const PROBE_X1 := 0.7

# Seam pair: two floor quads at the same depth, one seen through the glass
# pane (left of center) and one in the clear (right of center). Equal depth
# keeps the fog contribution identical, so any seam delta beyond the raster
# baseline comes from how fog treats the alpha-blended pane.
const SEAM_DEPTH := -17.0
const SEAM_HALF_DEPTH := 0.6
const SEAM_GLASS_X0 := -1.0
const SEAM_GLASS_X1 := -0.4
const SEAM_CLEAR_X0 := 0.4
const SEAM_CLEAR_X1 := 1.0


# Called by rtgi_quality_main.gd after the standard warmup/capture with the
# beauty frame, the run's output directory, and the live Environment.
func measure_fog_parity(image: Image, output_dir: String, environment: Environment) -> Dictionary:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		push_error("fog_corridor: no active camera; cannot project probe rects.")
		print("FOGPAR mode=unknown max_rel_err=1.0 seam=1.0 verdict=FAIL")
		return {
			"fog_corridor_verdict": "FAIL",
			"fog_corridor_max_rel_err": 1.0,
			"fog_corridor_no_camera": true,
		}

	var rect_lumas: Array[float] = []
	for i in range(PROBE_DEPTHS.size()):
		var z: float = PROBE_DEPTHS[i]
		var half_depth := PROBE_NEAR_HALF_DEPTH if i == 0 else PROBE_HALF_DEPTH
		var rect := _floor_rect(camera, image, PROBE_X0, PROBE_X1, z - half_depth, z + half_depth)
		rect_lumas.append(_mean_luma(image, rect))
	var glass_rect := _floor_rect(camera, image, SEAM_GLASS_X0, SEAM_GLASS_X1, SEAM_DEPTH - SEAM_HALF_DEPTH, SEAM_DEPTH + SEAM_HALF_DEPTH)
	var clear_rect := _floor_rect(camera, image, SEAM_CLEAR_X0, SEAM_CLEAR_X1, SEAM_DEPTH - SEAM_HALF_DEPTH, SEAM_DEPTH + SEAM_HALF_DEPTH)
	var glass_luma := _mean_luma(image, glass_rect)
	var clear_luma := _mean_luma(image, clear_rect)

	var metrics := {
		"fog_corridor_rect_lumas": rect_lumas.duplicate(),
		"fog_corridor_seam_glass_luma": glass_luma,
		"fog_corridor_seam_clear_luma": clear_luma,
	}

	var reference_path := "%s/%s" % [output_dir, REFERENCE_FILE_NAME]
	if environment == null:
		# A null environment is a broken run, not the raster reference: there is no
		# way to tell whether RTGI was on, so never record a reference from it.
		push_error("fog_corridor: no Environment was resolved; cannot tell the raster reference run from an RTGI run.")
		print("FOGPAR mode=unknown max_rel_err=1.0 seam=1.0 verdict=FAIL")
		metrics["fog_corridor_verdict"] = "FAIL"
		metrics["fog_corridor_max_rel_err"] = 1.0
		metrics["fog_corridor_null_environment"] = true
		return metrics
	if not environment.rtgi_enabled:
		_write_json(reference_path, {
			"rect_lumas": rect_lumas,
			"seam_glass_luma": glass_luma,
			"seam_clear_luma": clear_luma,
			"image_width": image.get_width(),
			"image_height": image.get_height(),
		})
		print("FOGPAR mode=off reference recorded")
		metrics["fog_corridor_reference_recorded"] = true
		return metrics

	var mode_id := int(environment.rtgi_mode)
	var reference := _read_json(reference_path)
	if reference.is_empty() or not reference.has("rect_lumas"):
		push_error("fog_corridor: raster reference '%s' is missing. Run the same scene and output dir with --rtgi-mode=off first." % reference_path)
		print("FOGPAR mode=%d max_rel_err=nan seam=nan verdict=FAIL" % mode_id)
		metrics["fog_corridor_verdict"] = "FAIL"
		metrics["fog_corridor_reference_missing"] = true
		return metrics

	var raster_lumas: Array = reference["rect_lumas"]
	var ref_width := int(reference.get("image_width", -1))
	var ref_height := int(reference.get("image_height", -1))
	if raster_lumas.size() != rect_lumas.size() or ref_width != image.get_width() or ref_height != image.get_height():
		# A reference recorded with a different probe layout or capture resolution
		# is stale; comparing a truncated subset would silently weaken the gate.
		push_error("fog_corridor: stale raster reference '%s' (probes %d vs %d, image %dx%d vs %dx%d). Re-run --rtgi-mode=off into this output dir." % [
				reference_path, raster_lumas.size(), rect_lumas.size(),
				ref_width, ref_height, image.get_width(), image.get_height()])
		print("FOGPAR mode=%d max_rel_err=1.0 seam=1.0 verdict=FAIL" % mode_id)
		metrics["fog_corridor_verdict"] = "FAIL"
		metrics["fog_corridor_max_rel_err"] = 1.0
		metrics["fog_corridor_reference_stale"] = true
		return metrics
	var rel_errs: Array[float] = []
	var max_rel_err := 0.0
	for i in range(rect_lumas.size()):
		var raster := float(raster_lumas[i])
		var rel_err: float = absf(rect_lumas[i] - raster) / maxf(raster, 1e-3)
		rel_errs.append(rel_err)
		max_rel_err = maxf(max_rel_err, rel_err)
	var seam_delta_run := glass_luma - clear_luma
	var seam_delta_raster := float(reference.get("seam_glass_luma", 0.0)) - float(reference.get("seam_clear_luma", 0.0))
	var seam := absf(seam_delta_run - seam_delta_raster)
	var verdict := "PASS" if max_rel_err <= PASS_MAX_REL_ERR else "FAIL"
	print("FOGPAR mode=%d max_rel_err=%.6f seam=%.6f verdict=%s" % [mode_id, max_rel_err, seam, verdict])

	metrics["fog_corridor_max_rel_err"] = max_rel_err
	metrics["fog_corridor_rect_rel_errs"] = rel_errs.duplicate()
	metrics["fog_corridor_seam_dev"] = seam
	metrics["fog_corridor_verdict"] = verdict
	_write_json("%s/fog_corridor_fogpar_mode_%d.json" % [output_dir, mode_id], {
		"mode": mode_id,
		"max_rel_err": max_rel_err,
		"rect_lumas": rect_lumas,
		"raster_rect_lumas": raster_lumas,
		"rect_rel_errs": rel_errs,
		"seam_glass_luma": glass_luma,
		"seam_clear_luma": clear_luma,
		"seam_dev": seam,
		"verdict": verdict,
	})
	return metrics


# Projects a floor-plane quad (y = 0, world space) to a clamped screen-space
# Rect2i on the captured image. The camera is fixed and authored into the
# scene, so the resulting rects are fixed per resolution. Grazing far-floor
# quads can project under a pixel tall, so rects are padded to at least 3 px
# per axis around their center; the padded rows stay on the same fog-saturated
# surface, so the mean is unaffected in practice.
func _floor_rect(camera: Camera3D, image: Image, x0: float, x1: float, z0: float, z1: float) -> Rect2i:
	var viewport_size := camera.get_viewport().get_visible_rect().size
	var scale := Vector2(
			float(image.get_width()) / maxf(viewport_size.x, 1.0),
			float(image.get_height()) / maxf(viewport_size.y, 1.0))
	var corners := [Vector3(x0, 0.0, z0), Vector3(x1, 0.0, z0), Vector3(x0, 0.0, z1), Vector3(x1, 0.0, z1)]
	var min_p := Vector2(INF, INF)
	var max_p := Vector2(-INF, -INF)
	for corner in corners:
		var screen: Vector2 = camera.unproject_position(corner) * scale
		min_p = min_p.min(screen)
		max_p = max_p.max(screen)
	var center := (min_p + max_p) * 0.5
	var half := (max_p - min_p) * 0.5
	half.x = maxf(half.x, 1.5)
	half.y = maxf(half.y, 1.5)
	var x_start := clampi(int(floor(center.x - half.x)), 0, image.get_width() - 1)
	var x_end := clampi(int(ceil(center.x + half.x)), x_start + 1, image.get_width())
	var y_start := clampi(int(floor(center.y - half.y)), 0, image.get_height() - 1)
	var y_end := clampi(int(ceil(center.y + half.y)), y_start + 1, image.get_height())
	return Rect2i(Vector2i(x_start, y_start), Vector2i(x_end - x_start, y_end - y_start))


func _mean_luma(image: Image, rect: Rect2i) -> float:
	var sum := 0.0
	var count := 0
	for y in range(rect.position.y, rect.end.y):
		for x in range(rect.position.x, rect.end.x):
			var color := image.get_pixel(x, y)
			sum += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			count += 1
	return sum / maxf(float(count), 1.0)


func _write_json(path: String, payload: Dictionary) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("fog_corridor: could not write '%s'." % path)
		return
	file.store_string(JSON.stringify(payload, "\t"))


func _read_json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var parsed = JSON.parse_string(file.get_as_text())
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}
