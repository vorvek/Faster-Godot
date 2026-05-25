extends SceneTree

const DEFAULT_SCENE = "res://scenes/main/main.tscn"
const DEFAULT_OUTPUT_DIR = "user://euphorica_rtgi"
const DEFAULT_WARMUP_FRAMES = 180
const DEFAULT_SPARKLE_FRAMES = 32
const RTGI_KNOB_STAGES = {
	"rtgi_enabled": "Environment setup / render path selection",
	"rtgi_mode": "Forward+ opaque replacement and RT raygen mode",
	"rtgi_samples_per_pixel": "RT raygen sampling",
	"rtgi_max_bounces": "RT raygen path depth",
	"rtgi_energy": "RTGI composite energy",
	"rtgi_denoiser": "RTGI denoise dispatch",
	"rtgi_denoiser_strength": "SVGF temporal, spatial, composite, blotch stabilization",
	"rtgi_denoiser_history_weight": "SVGF temporal accumulation",
	"rtgi_denoiser_firefly_suppression": "SVGF temporal, prefilter, composite, split composite",
	"rtgi_denoiser_detail_preservation": "SVGF spatial/composite detail guards",
	"rtgi_denoiser_split_signals": "RT diffuse/specular split denoise",
	"rtgi_denoiser_specular_history_weight": "SVGF specular temporal accumulation",
	"rtgi_denoiser_specular_spatial_strength": "SVGF specular spatial strength",
	"rtgi_ray_firefly_suppression": "RT ray/path contribution clamp",
	"rtgi_ray_max_radiance": "RT ray/path contribution clamp",
	"rtgi_overscan_horizontal": "Path Traced camera pan overscan",
	"rtgi_overscan_vertical": "Path Traced camera pan overscan",
	"rtgi_debug_mode": "RT raygen debug visualization",
}

var _output_dir = DEFAULT_OUTPUT_DIR
var _scene_path = DEFAULT_SCENE
var _profile = "compare"
var _warmup_frames = DEFAULT_WARMUP_FRAMES
var _sparkle_frames = DEFAULT_SPARKLE_FRAMES
var _capture_debug = false
var _base_denoise = 0.98
var _base_history = 0.98
var _base_mode = "path_traced"
var _base_resolution = Vector2i(640, 360)
var _window_size = Vector2i(1920, 1080)
var _split_signals_mode = "both"
var _case_filter = ""
var _list_cases = false
var _resume = false
var _results = []
var _split_pair_metrics = []
var _split_pair_images = {}
var _debug_views = ["noisy", "diffuse_noisy", "specular_noisy", "diffuse_final", "specular_final", "specular_guide", "normal_roughness", "viewz_hitdist", "motion_vectors", "signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "variance", "history_length", "rejection", "final"]


func _initialize() -> void:
	_parse_args()
	call_deferred("_run")


func _parse_args() -> void:
	for arg in OS.get_cmdline_args():
		if arg.begins_with("--euphorica-output-dir="):
			_output_dir = arg.trim_prefix("--euphorica-output-dir=")
		elif arg.begins_with("--euphorica-scene="):
			_scene_path = arg.trim_prefix("--euphorica-scene=")
		elif arg.begins_with("--euphorica-profile="):
			_profile = arg.trim_prefix("--euphorica-profile=").to_lower()
		elif arg.begins_with("--euphorica-warmup-frames="):
			_warmup_frames = max(1, arg.trim_prefix("--euphorica-warmup-frames=").to_int())
		elif arg.begins_with("--euphorica-sparkle-frames="):
			_sparkle_frames = clampi(arg.trim_prefix("--euphorica-sparkle-frames=").to_int(), 0, 96)
		elif arg.begins_with("--euphorica-denoise="):
			_base_denoise = clampf(arg.trim_prefix("--euphorica-denoise=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--euphorica-history="):
			_base_history = clampf(arg.trim_prefix("--euphorica-history=").to_float(), 0.0, 0.98)
		elif arg.begins_with("--euphorica-mode="):
			_base_mode = arg.trim_prefix("--euphorica-mode=").to_lower()
		elif arg.begins_with("--euphorica-resolution="):
			_base_resolution = _parse_resolution(arg.trim_prefix("--euphorica-resolution="), _base_resolution)
		elif arg.begins_with("--euphorica-window-size="):
			_window_size = _parse_resolution(arg.trim_prefix("--euphorica-window-size="), _window_size)
		elif arg.begins_with("--euphorica-split-signals="):
			var split_mode = arg.trim_prefix("--euphorica-split-signals=").to_lower()
			if split_mode in ["on", "off", "both"]:
				_split_signals_mode = split_mode
		elif arg.begins_with("--euphorica-case-filter="):
			_case_filter = arg.trim_prefix("--euphorica-case-filter=").to_lower()
		elif arg == "--euphorica-list-cases":
			_list_cases = true
		elif arg == "--euphorica-resume":
			_resume = true
		elif arg == "--euphorica-capture-debug":
			_capture_debug = true
		elif arg.begins_with("--euphorica-rtgi="):
			var enabled = not (arg.trim_prefix("--euphorica-rtgi=").to_lower() in ["0", "false", "off", "disabled"])
			_profile = "rtgi_on" if enabled else "no_rtgi"


func _parse_resolution(text: String, fallback: Vector2i) -> Vector2i:
	var normalized = text.to_lower().replace(",", "x")
	var parts = normalized.split("x")
	if parts.size() != 2:
		return fallback
	return Vector2i(max(1, parts[0].to_int()), max(1, parts[1].to_int()))


func _run() -> void:
	DisplayServer.window_set_size(_window_size)
	root.size = _window_size
	if not _ensure_output_dir():
		quit(2)
		return
	var cases = _filter_cases(_build_cases())
	if _list_cases:
		_write_summary(cases)
		quit(0)
		return
	var completed_cases = _load_resume_results(cases) if _resume else {}
	var baseline_game: Image = null
	var baseline_final: Image = null
	for test_case in cases:
		if completed_cases.has(test_case["name"]):
			print("Euphorica RTGI capture: skipping completed %s" % test_case["name"])
			var resumed_game = _load_png_or_null(_output_path("%s_game.png" % test_case["name"]))
			var resumed_final = _load_png_or_null(_output_path("%s_final.png" % test_case["name"]))
			if test_case.get("baseline", false):
				baseline_game = resumed_game
				baseline_final = resumed_final
			if resumed_game != null and resumed_final != null:
				_record_split_pair(resumed_game, resumed_final, test_case)
			continue
		var case_start_msec = Time.get_ticks_msec()
		print("Euphorica RTGI capture: starting %s" % test_case["name"])
		var capture = await _run_case(test_case)
		print("Euphorica RTGI capture: measuring %s" % test_case["name"])
		if test_case.get("baseline", false):
			baseline_game = capture["game_image"]
			baseline_final = capture["final_image"]
		var metrics = _measure_case(capture["game_frames"], capture["game_image"], capture["final_image"], baseline_game, baseline_final)
		metrics["case"] = test_case.duplicate(true)
		metrics["rtgi_knobs"] = _collect_knobs(capture["environment"])
		metrics["normal_textures_flipped"] = capture["normal_textures_flipped"]
		_results.append(metrics)
		_write_json(_output_path("%s_metrics.json" % test_case["name"]), metrics)
		_record_split_pair(capture["game_image"], capture["final_image"], test_case)
		_write_summary(cases)
		print("Euphorica RTGI capture: finished %s in %.2fs" % [test_case["name"], float(Time.get_ticks_msec() - case_start_msec) / 1000.0])
		_unload_scene(capture["scene"])
		await process_frame
	_write_summary(cases)
	quit(0)


func _build_cases() -> Array:
	var cases = []
	if _profile in ["compare", "matrix", "split_ab"]:
		cases.append(_case("no_rtgi", false, _base_mode, _base_denoise, _base_history, _base_resolution, {}, true))
	if _profile == "no_rtgi":
		return [_case("no_rtgi", false, _base_mode, _base_denoise, _base_history, _base_resolution, {}, true)]
	if _profile == "rtgi_on":
		var rtgi_cases = []
		for split_enabled in _split_values():
			rtgi_cases.append(_split_case("rtgi_on", true, _base_mode, _base_denoise, _base_history, _base_resolution, {}, false, split_enabled))
		return rtgi_cases
	if _profile == "split_ab":
		for resolution in [Vector2i(640, 360), Vector2i(1280, 720)]:
			for mode in ["simple_rt", "path_traced"]:
				for split_enabled in _split_values():
					cases.append(_split_case("%s_d%.2f_h%.2f_%d" % [mode, _base_denoise, _base_history, resolution.x], true, mode, _base_denoise, _base_history, resolution, {}, false, split_enabled))
		for toggle in ["no_glow_fog", "no_lantern_emission", "no_omni_shadow"]:
			for split_enabled in _split_values():
				cases.append(_split_case("path_%s" % toggle, true, "path_traced", _base_denoise, _base_history, _base_resolution, { toggle: true }, false, split_enabled))
		if _capture_debug:
			for split_enabled in _split_values():
				cases.append(_split_case("path_normal_deviation", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "debug_mode": Environment.RT_DEBUG_NORMAL_DEVIATION }, false, split_enabled))
		return cases
	if _profile == "matrix":
		for mode in ["simple_rt", "path_traced"]:
			for denoise in [0.90, 0.95, 0.98, 1.0]:
				for history in [0.95, 0.98]:
					for split_enabled in _split_values():
						cases.append(_split_case("%s_d%.2f_h%.2f_640" % [mode, denoise, history], true, mode, denoise, history, Vector2i(640, 360), {}, false, split_enabled))
		for mode in ["simple_rt", "path_traced"]:
			for split_enabled in _split_values():
				cases.append(_split_case("%s_d%.2f_h%.2f_1280" % [mode, _base_denoise, _base_history], true, mode, _base_denoise, _base_history, Vector2i(1280, 720), {}, false, split_enabled))
		for toggle in ["no_glow_fog", "no_lantern_emission", "no_omni_shadow"]:
			for split_enabled in _split_values():
				cases.append(_split_case("path_%s" % toggle, true, "path_traced", _base_denoise, _base_history, _base_resolution, { toggle: true }, false, split_enabled))
		return cases
	for split_enabled in _split_values():
		cases.append(_split_case("simple_rt_d%.2f_h%.2f" % [_base_denoise, _base_history], true, "simple_rt", _base_denoise, _base_history, _base_resolution, {}, false, split_enabled))
		cases.append(_split_case("path_traced_d%.2f_h%.2f" % [_base_denoise, _base_history], true, "path_traced", _base_denoise, _base_history, _base_resolution, {}, false, split_enabled))
		cases.append(_split_case("path_no_glow_fog", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_glow_fog": true }, false, split_enabled))
		cases.append(_split_case("path_no_lantern_emission", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_lantern_emission": true }, false, split_enabled))
		cases.append(_split_case("path_no_omni_shadow", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_omni_shadow": true }, false, split_enabled))
	if _capture_debug:
		for split_enabled in _split_values():
			cases.append(_split_case("path_normal_deviation", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "debug_mode": Environment.RT_DEBUG_NORMAL_DEVIATION }, false, split_enabled))
	return cases


func _split_values() -> Array:
	if _split_signals_mode == "on":
		return [true]
	if _split_signals_mode == "off":
		return [false]
	return [true, false]


func _split_case(name: String, rtgi_enabled: bool, mode: String, denoise: float, history: float, resolution: Vector2i, options: Dictionary, baseline: bool, split_signals: bool) -> Dictionary:
	var result = _case("%s_split_%s" % [name, "on" if split_signals else "off"], rtgi_enabled, mode, denoise, history, resolution, options, baseline)
	result["split_signals"] = split_signals
	result["split_pair_key"] = _split_pair_key(result)
	return result


func _case(name: String, rtgi_enabled: bool, mode: String, denoise: float, history: float, resolution: Vector2i, options: Dictionary, baseline: bool) -> Dictionary:
	var result = options.duplicate(true)
	result["name"] = name.replace(".", "_")
	result["rtgi_enabled"] = rtgi_enabled
	result["mode"] = mode
	result["denoise"] = denoise
	result["history"] = history
	result["resolution"] = resolution
	result["baseline"] = baseline
	result["split_signals"] = true
	result["split_pair_key"] = ""
	return result


func _filter_cases(cases: Array) -> Array:
	if _case_filter.is_empty():
		return cases
	var filtered = []
	var has_filtered_rtgi_case = false
	for test_case in cases:
		if str(test_case["name"]).to_lower().contains(_case_filter):
			filtered.append(test_case)
			has_filtered_rtgi_case = has_filtered_rtgi_case or bool(test_case["rtgi_enabled"])
	if has_filtered_rtgi_case:
		for test_case in cases:
			if bool(test_case.get("baseline", false)) and not filtered.has(test_case):
				filtered.push_front(test_case)
	return filtered


func _split_pair_key(test_case: Dictionary) -> String:
	var options = []
	for key in ["no_glow_fog", "no_lantern_emission", "no_omni_shadow", "debug_mode"]:
		if test_case.has(key):
			options.append("%s=%s" % [key, str(test_case[key])])
	options.sort()
	return "%s|%dx%d|d%.3f|h%.3f|%s" % [
		test_case["mode"],
		test_case["resolution"].x,
		test_case["resolution"].y,
		float(test_case["denoise"]),
		float(test_case["history"]),
		",".join(options),
	]


func _write_summary(cases: Array) -> void:
	_write_json(_output_path("euphorica_rtgi_summary.json"), {
		"profile": _profile,
		"scene": _scene_path,
		"case_filter": _case_filter,
		"split_signals": _split_signals_mode,
		"resume": _resume,
		"planned_cases": cases.map(func(test_case): return test_case["name"]),
		"completed_cases": _results.size(),
		"results": _results,
		"split_pair_metrics": _split_pair_metrics,
		"knob_stages": RTGI_KNOB_STAGES,
	})


func _record_split_pair(game_image: Image, final_image: Image, test_case: Dictionary) -> void:
	if not bool(test_case.get("rtgi_enabled", false)):
		return
	var pair_key = str(test_case.get("split_pair_key", ""))
	if pair_key.is_empty():
		return
	if not _split_pair_images.has(pair_key):
		_split_pair_images[pair_key] = {}
	var side = "on" if bool(test_case.get("split_signals", true)) else "off"
	_split_pair_images[pair_key][side] = {
		"case_name": test_case["name"],
		"game_image": game_image,
		"final_image": final_image,
	}
	if _split_pair_images[pair_key].has("on") and _split_pair_images[pair_key].has("off"):
		var on_case = _split_pair_images[pair_key]["on"]
		var off_case = _split_pair_images[pair_key]["off"]
		var metrics = {
			"pair_key": pair_key,
			"split_on_case": on_case["case_name"],
			"split_off_case": off_case["case_name"],
		}
		metrics.merge(_diff_metrics(on_case["game_image"], off_case["game_image"], "split_on_vs_off_game"), true)
		metrics.merge(_diff_metrics(on_case["final_image"], off_case["final_image"], "split_on_vs_off_final"), true)
		_split_pair_metrics.append(metrics)
		_split_pair_images.erase(pair_key)


func _run_case(test_case: Dictionary) -> Dictionary:
	var packed = load(_scene_path)
	if not (packed is PackedScene):
		push_error("Unable to load Euphorica scene: %s" % _scene_path)
		quit(2)
		return {}
	var scene = (packed as PackedScene).instantiate()
	root.add_child(scene)
	await process_frame
	var game_viewport = _find_node(scene, "GameViewport") as SubViewport
	if game_viewport == null:
		push_error("Unable to find GameViewport in Euphorica scene.")
		quit(2)
		return {}
	game_viewport.size = test_case["resolution"]
	game_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS

	var world_environment = _find_first_world_environment(game_viewport)
	var env: Environment = null
	if world_environment != null and world_environment.environment != null:
		env = world_environment.environment.duplicate(true) as Environment
		world_environment.environment = env
	else:
		env = Environment.new()
		if world_environment == null:
			world_environment = WorldEnvironment.new()
			game_viewport.add_child(world_environment)
		world_environment.environment = env
	_apply_environment(test_case, env)
	_apply_camera_environment(scene, test_case)
	_apply_scene_toggles(scene, test_case)

	for i in range(_warmup_frames):
		await process_frame
		await RenderingServer.frame_post_draw

	var frames = []
	var frame_count = max(1, _sparkle_frames)
	for i in range(frame_count):
		await process_frame
		await RenderingServer.frame_post_draw
		frames.append(_capture_viewport(game_viewport))
	var game_image: Image = frames[frames.size() - 1]
	var final_image = _capture_viewport(root)
	game_image.save_png(_output_path("%s_game.png" % test_case["name"]))
	final_image.save_png(_output_path("%s_final.png" % test_case["name"]))
	if _capture_debug and bool(test_case.get("rtgi_enabled", false)):
		await _capture_debug_views(test_case, game_viewport)
	return {
		"scene": scene,
		"game_viewport": game_viewport,
		"game_frames": frames,
		"game_image": game_image,
		"final_image": final_image,
		"environment": env,
		"normal_textures_flipped": 0,
	}


func _apply_environment(test_case: Dictionary, env: Environment) -> void:
	env.rtgi_enabled = test_case["rtgi_enabled"]
	env.rtgi_disable_in_editor = false
	env.rtgi_mode = Environment.RTGI_MODE_HYBRID if test_case["mode"] == "simple_rt" else Environment.RTGI_MODE_PATH_TRACED
	env.rtgi_samples_per_pixel = int(test_case.get("spp", 1))
	env.rtgi_max_bounces = int(test_case.get("max_bounces", 3))
	env.rtgi_denoiser = Environment.RTGI_DENOISER_SVGF
	env.rtgi_denoiser_strength = test_case["denoise"]
	env.rtgi_denoiser_history_weight = test_case["history"]
	env.rtgi_denoiser_firefly_suppression = float(test_case.get("firefly_suppression", 1.0))
	env.rtgi_denoiser_detail_preservation = float(test_case.get("detail_preservation", 1.0))
	env.rtgi_denoiser_split_signals = bool(test_case.get("split_signals", true))
	env.rtgi_denoiser_specular_history_weight = test_case["history"]
	env.rtgi_denoiser_specular_spatial_strength = float(test_case.get("specular_spatial_strength", 1.0))
	env.rtgi_ray_firefly_suppression = float(test_case.get("ray_firefly_suppression", 0.85))
	env.rtgi_ray_max_radiance = float(test_case.get("ray_max_radiance", 32.0))
	env.rtgi_debug_mode = int(test_case.get("debug_mode", Environment.RT_DEBUG_DISABLED))
	if test_case.get("no_glow_fog", false):
		env.glow_enabled = false
		env.fog_enabled = false
		env.volumetric_fog_enabled = false


func _apply_camera_environment(scene: Node, test_case: Dictionary) -> void:
	for node in _walk(scene):
		if node is Camera3D and node.environment != null:
			node.environment = node.environment.duplicate(true)
			_apply_environment(test_case, node.environment)


func _apply_scene_toggles(scene: Node, test_case: Dictionary) -> void:
	if test_case.get("no_omni_shadow", false):
		for node in _walk(scene):
			if node is OmniLight3D:
				node.shadow_enabled = false
	if test_case.get("no_lantern_emission", false):
		for node in _walk(scene):
			if node is GeometryInstance3D:
				_zero_emission_materials(node)


func _capture_debug_views(test_case: Dictionary, game_viewport: Viewport) -> void:
	for view in _debug_views:
		_apply_debug_view(game_viewport, view)
		await process_frame
		await RenderingServer.frame_post_draw
		_capture_viewport(game_viewport).save_png(_output_path("%s_%s_game.png" % [test_case["name"], view]))
	_apply_debug_view(game_viewport, "disabled")
	await process_frame
	await RenderingServer.frame_post_draw


func _apply_debug_view(viewport: Viewport, view: String) -> void:
	RenderingServer.viewport_set_debug_draw(viewport.get_viewport_rid(), _debug_draw_value(view))


func _debug_draw_value(view: String) -> int:
	match view:
		"beauty", "disabled":
			return 0
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
		"noisy":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_NOISY
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


func _zero_emission_materials(node: GeometryInstance3D) -> void:
	if node.material_override != null:
		node.material_override = _zero_emission_material(node.material_override)
	if node is MeshInstance3D and node.mesh != null:
		for surface in range(node.mesh.get_surface_count()):
			var material = node.get_surface_override_material(surface)
			if material == null:
				material = node.mesh.surface_get_material(surface)
			if material != null:
				node.set_surface_override_material(surface, _zero_emission_material(material))


func _zero_emission_material(material: Material) -> Material:
	var duplicate = material.duplicate(true)
	if duplicate is BaseMaterial3D:
		duplicate.emission_enabled = false
	elif duplicate is ShaderMaterial:
		if duplicate.get_shader_parameter("emission_energy") != null:
			duplicate.set_shader_parameter("emission_energy", 0.0)
	return duplicate


func _capture_viewport(viewport: Viewport) -> Image:
	var image = viewport.get_texture().get_image()
	image.convert(Image.FORMAT_RGBA8)
	return image


func _measure_case(frames: Array, game_image: Image, final_image: Image, baseline_game: Image, baseline_final: Image) -> Dictionary:
	var metrics = _image_metrics(game_image, "game")
	var final_metrics = _image_metrics(final_image, "final")
	for key in final_metrics.keys():
		metrics[key] = final_metrics[key]
	metrics.merge(_image_roi_metrics(game_image, "game"), true)
	metrics.merge(_image_roi_metrics(final_image, "final"), true)
	metrics["temporal_sparkle_per_megapixel_max"] = _temporal_sparkle(frames)
	metrics["temporal_sparkle_per_megapixel_avg"] = _temporal_sparkle_average(frames)
	metrics["game_convergence_curve"] = _convergence_curve(frames)
	metrics["final_to_game_luma_correlation"] = _luma_correlation(game_image, final_image)
	metrics["final_to_game_visible_speckle_ratio"] = _safe_ratio(metrics["final_visible_speckles_per_megapixel"], metrics["game_visible_speckles_per_megapixel"])
	metrics["final_to_game_firefly_ratio"] = _safe_ratio(metrics["final_full_frame_fireflies_per_megapixel"], metrics["game_full_frame_fireflies_per_megapixel"])
	metrics["final_to_game_p99_luma_ratio"] = _safe_ratio(metrics["final_luma_p99"], metrics["game_luma_p99"])
	metrics["final_to_game_detail_edge_ratio"] = _safe_ratio(metrics["final_detail_edge_energy"], metrics["game_detail_edge_energy"])
	if baseline_game != null:
		var diff = _diff_metrics(game_image, baseline_game, "rtgi_vs_no_rtgi")
		for key in diff.keys():
			metrics[key] = diff[key]
	if baseline_final != null:
		var final_diff = _diff_metrics(final_image, baseline_final, "final_rtgi_vs_no_rtgi")
		for key in final_diff.keys():
			metrics[key] = final_diff[key]
	return metrics


func _image_metrics(image: Image, prefix: String) -> Dictionary:
	var width = image.get_width()
	var height = image.get_height()
	var count = max(1, width * height)
	var lum_values = []
	lum_values.resize(count)
	var sum = 0.0
	var sum_sq = 0.0
	var max_luma = 0.0
	var saturated = 0
	var nonblack = 0
	var index = 0
	for y in range(height):
		for x in range(width):
			var luma = _luma(image.get_pixel(x, y))
			lum_values[index] = luma
			index += 1
			sum += luma
			sum_sq += luma * luma
			max_luma = max(max_luma, luma)
			if luma > 0.98:
				saturated += 1
			if luma > 0.003:
				nonblack += 1
	lum_values.sort()
	var mean = sum / count
	var variance = max(sum_sq / count - mean * mean, 0.0)
	var p95 = lum_values[clampi(roundi(float(count - 1) * 0.95), 0, count - 1)]
	var p99 = lum_values[clampi(roundi(float(count - 1) * 0.99), 0, count - 1)]
	return {
		"%s_luma_mean" % prefix: mean,
		"%s_luma_stddev" % prefix: sqrt(variance),
		"%s_luma_p95" % prefix: p95,
		"%s_luma_p99" % prefix: p99,
		"%s_luma_max" % prefix: max_luma,
		"%s_saturated_luma_fraction" % prefix: float(saturated) / float(count),
		"%s_nonblack_fraction" % prefix: float(nonblack) / float(count),
		"%s_detail_edge_energy" % prefix: _edge_energy(image),
		"%s_visible_speckles_per_megapixel" % prefix: _visible_speckles(image),
		"%s_full_frame_fireflies_per_megapixel" % prefix: _fireflies(image),
	}


func _image_roi_metrics(image: Image, prefix: String) -> Dictionary:
	var metrics = {}
	for roi_name in _roi_rects(image).keys():
		var rect: Rect2i = _roi_rects(image)[roi_name]
		metrics.merge(_image_metrics_region(image, "%s_%s_roi" % [prefix, roi_name], rect), true)
	return metrics


func _roi_rects(image: Image) -> Dictionary:
	var width = image.get_width()
	var height = image.get_height()
	return {
		"lantern_emissive": Rect2i(Vector2i(int(width * 0.40), int(height * 0.12)), Vector2i(max(4, int(width * 0.28)), max(4, int(height * 0.34)))),
		"dark_wall_floor": Rect2i(Vector2i(int(width * 0.58), int(height * 0.42)), Vector2i(max(4, int(width * 0.34)), max(4, int(height * 0.44)))),
		"normal_detail": Rect2i(Vector2i(int(width * 0.08), int(height * 0.25)), Vector2i(max(4, int(width * 0.34)), max(4, int(height * 0.52)))),
		"final_post": Rect2i(Vector2i(int(width * 0.18), int(height * 0.12)), Vector2i(max(4, int(width * 0.64)), max(4, int(height * 0.72)))),
	}


func _image_metrics_region(image: Image, prefix: String, rect: Rect2i) -> Dictionary:
	var x0 = clampi(rect.position.x, 0, image.get_width() - 1)
	var y0 = clampi(rect.position.y, 0, image.get_height() - 1)
	var x1 = clampi(rect.position.x + rect.size.x, x0 + 1, image.get_width())
	var y1 = clampi(rect.position.y + rect.size.y, y0 + 1, image.get_height())
	var count = max(1, (x1 - x0) * (y1 - y0))
	var lum_values = []
	lum_values.resize(count)
	var sum = 0.0
	var sum_sq = 0.0
	var max_luma = 0.0
	var saturated = 0
	var index = 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var luma = _luma(image.get_pixel(x, y))
			lum_values[index] = luma
			index += 1
			sum += luma
			sum_sq += luma * luma
			max_luma = max(max_luma, luma)
			if luma > 0.98:
				saturated += 1
	lum_values.sort()
	var mean = sum / count
	var variance = max(sum_sq / count - mean * mean, 0.0)
	var p95 = lum_values[clampi(roundi(float(count - 1) * 0.95), 0, count - 1)]
	var p99 = lum_values[clampi(roundi(float(count - 1) * 0.99), 0, count - 1)]
	return {
		"%s_luma_mean" % prefix: mean,
		"%s_luma_stddev" % prefix: sqrt(variance),
		"%s_luma_p95" % prefix: p95,
		"%s_luma_p99" % prefix: p99,
		"%s_luma_max" % prefix: max_luma,
		"%s_saturated_luma_fraction" % prefix: float(saturated) / float(count),
		"%s_detail_edge_energy" % prefix: _edge_energy_region(image, x0, y0, x1, y1),
		"%s_visible_speckles_per_megapixel" % prefix: _visible_speckles_region(image, x0, y0, x1, y1),
		"%s_fireflies_per_megapixel" % prefix: _fireflies_region(image, x0, y0, x1, y1),
	}


func _convergence_curve(frames: Array) -> Array:
	var curve = []
	var previous: Image = null
	for i in range(frames.size()):
		var image: Image = frames[i]
		var full = _image_metrics_region(image, "frame", Rect2i(Vector2i.ZERO, Vector2i(image.get_width(), image.get_height())))
		var delta = 0.0 if previous == null else _frame_delta_speckles(previous, image)
		curve.append({
			"frame": i,
			"luma_mean": full["frame_luma_mean"],
			"luma_p99": full["frame_luma_p99"],
			"luma_max": full["frame_luma_max"],
			"fireflies_per_megapixel": full["frame_fireflies_per_megapixel"],
			"visible_speckles_per_megapixel": full["frame_visible_speckles_per_megapixel"],
			"frame_delta_sparkle_per_megapixel": delta,
		})
		previous = image
	return curve


func _temporal_sparkle(frames: Array) -> float:
	if frames.size() < 2:
		return 0.0
	var max_value = 0.0
	for i in range(1, frames.size()):
		max_value = max(max_value, _frame_delta_speckles(frames[i - 1], frames[i]))
	return max_value


func _temporal_sparkle_average(frames: Array) -> float:
	if frames.size() < 2:
		return 0.0
	var sum = 0.0
	for i in range(1, frames.size()):
		sum += _frame_delta_speckles(frames[i - 1], frames[i])
	return sum / float(frames.size() - 1)


func _frame_delta_speckles(a: Image, b: Image) -> float:
	var width = min(a.get_width(), b.get_width())
	var height = min(a.get_height(), b.get_height())
	var hits = 0
	for y in range(height):
		for x in range(width):
			var delta = abs(_luma(a.get_pixel(x, y)) - _luma(b.get_pixel(x, y)))
			if delta > 0.08:
				hits += 1
	return _per_megapixel(hits, width, height)


func _visible_speckles(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	var hits = 0
	for y in range(1, height - 1):
		for x in range(1, width - 1):
			var center = _luma(image.get_pixel(x, y))
			var neighborhood = 0.0
			for oy in range(-1, 2):
				for ox in range(-1, 2):
					if ox != 0 or oy != 0:
						neighborhood += _luma(image.get_pixel(x + ox, y + oy))
			neighborhood /= 8.0
			if center > max(neighborhood * 2.2 + 0.04, 0.10):
				hits += 1
	return _per_megapixel(hits, width, height)


func _fireflies(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	var hits = 0
	for y in range(2, height - 2):
		for x in range(2, width - 2):
			var center = _luma(image.get_pixel(x, y))
			var max_neighbor = 0.0
			var sum_neighbor = 0.0
			var samples = 0
			for oy in range(-2, 3):
				for ox in range(-2, 3):
					if ox == 0 and oy == 0:
						continue
					var luma = _luma(image.get_pixel(x + ox, y + oy))
					max_neighbor = max(max_neighbor, luma)
					sum_neighbor += luma
					samples += 1
			var avg_neighbor = sum_neighbor / max(1, samples)
			if center > max(max_neighbor * 1.45 + 0.03, avg_neighbor * 3.0 + 0.08):
				hits += 1
	return _per_megapixel(hits, width, height)


func _visible_speckles_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var hits = 0
	var sx0 = max(1, x0)
	var sy0 = max(1, y0)
	var sx1 = min(image.get_width() - 1, x1)
	var sy1 = min(image.get_height() - 1, y1)
	for y in range(sy0, sy1):
		for x in range(sx0, sx1):
			var center = _luma(image.get_pixel(x, y))
			var neighborhood = 0.0
			for oy in range(-1, 2):
				for ox in range(-1, 2):
					if ox != 0 or oy != 0:
						neighborhood += _luma(image.get_pixel(x + ox, y + oy))
			neighborhood /= 8.0
			if center > max(neighborhood * 2.2 + 0.04, 0.10):
				hits += 1
	return _per_megapixel(hits, max(1, x1 - x0), max(1, y1 - y0))


func _fireflies_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var hits = 0
	var sx0 = max(2, x0)
	var sy0 = max(2, y0)
	var sx1 = min(image.get_width() - 2, x1)
	var sy1 = min(image.get_height() - 2, y1)
	for y in range(sy0, sy1):
		for x in range(sx0, sx1):
			var center = _luma(image.get_pixel(x, y))
			var max_neighbor = 0.0
			var sum_neighbor = 0.0
			var samples = 0
			for oy in range(-2, 3):
				for ox in range(-2, 3):
					if ox == 0 and oy == 0:
						continue
					var luma = _luma(image.get_pixel(x + ox, y + oy))
					max_neighbor = max(max_neighbor, luma)
					sum_neighbor += luma
					samples += 1
			var avg_neighbor = sum_neighbor / max(1, samples)
			if center > max(max_neighbor * 1.45 + 0.03, avg_neighbor * 3.0 + 0.08):
				hits += 1
	return _per_megapixel(hits, max(1, x1 - x0), max(1, y1 - y0))


func _edge_energy(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	var sum = 0.0
	var count = 0
	for y in range(1, height - 1):
		for x in range(1, width - 1):
			var gx = _luma(image.get_pixel(x + 1, y)) - _luma(image.get_pixel(x - 1, y))
			var gy = _luma(image.get_pixel(x, y + 1)) - _luma(image.get_pixel(x, y - 1))
			sum += sqrt(gx * gx + gy * gy)
			count += 1
	return sum / max(1, count)


func _edge_energy_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum = 0.0
	var count = 0
	var sx0 = max(1, x0)
	var sy0 = max(1, y0)
	var sx1 = min(image.get_width() - 1, x1)
	var sy1 = min(image.get_height() - 1, y1)
	for y in range(sy0, sy1):
		for x in range(sx0, sx1):
			var gx = _luma(image.get_pixel(x + 1, y)) - _luma(image.get_pixel(x - 1, y))
			var gy = _luma(image.get_pixel(x, y + 1)) - _luma(image.get_pixel(x, y - 1))
			sum += sqrt(gx * gx + gy * gy)
			count += 1
	return sum / max(1, count)


func _diff_metrics(a: Image, b: Image, prefix: String) -> Dictionary:
	var width = min(a.get_width(), b.get_width())
	var height = min(a.get_height(), b.get_height())
	var count = max(1, width * height)
	var luma_abs = 0.0
	var luma_sq = 0.0
	var rgb_abs = 0.0
	var rgb_sq = 0.0
	for y in range(height):
		for x in range(width):
			var ca = a.get_pixel(x, y)
			var cb = b.get_pixel(x, y)
			var dl = _luma(ca) - _luma(cb)
			luma_abs += abs(dl)
			luma_sq += dl * dl
			var dr = ca.r - cb.r
			var dg = ca.g - cb.g
			var db = ca.b - cb.b
			rgb_abs += (abs(dr) + abs(dg) + abs(db)) / 3.0
			rgb_sq += (dr * dr + dg * dg + db * db) / 3.0
	var metrics = {
		"%s_luma_mae" % prefix: luma_abs / count,
		"%s_luma_rmse" % prefix: sqrt(luma_sq / count),
		"%s_rgb_mae" % prefix: rgb_abs / count,
		"%s_rgb_rmse" % prefix: sqrt(rgb_sq / count),
		"%s_contribution_mean_luma_delta" % prefix: (_mean_luma(a) - _mean_luma(b)),
	}
	metrics.merge(_downsampled_diff_metrics(a, b, prefix, 2), true)
	metrics.merge(_downsampled_diff_metrics(a, b, prefix, 4), true)
	return metrics


func _downsampled_diff_metrics(a: Image, b: Image, prefix: String, factor: int) -> Dictionary:
	var da = a.duplicate()
	var db = b.duplicate()
	var width = max(1, int(min(a.get_width(), b.get_width()) / factor))
	var height = max(1, int(min(a.get_height(), b.get_height()) / factor))
	da.resize(width, height, Image.INTERPOLATE_LANCZOS)
	db.resize(width, height, Image.INTERPOLATE_LANCZOS)
	var count = max(1, width * height)
	var luma_abs = 0.0
	var luma_sq = 0.0
	for y in range(height):
		for x in range(width):
			var dl = _luma(da.get_pixel(x, y)) - _luma(db.get_pixel(x, y))
			luma_abs += abs(dl)
			luma_sq += dl * dl
	return {
		"%s_downsample_%dx_luma_mae" % [prefix, factor]: luma_abs / count,
		"%s_downsample_%dx_luma_rmse" % [prefix, factor]: sqrt(luma_sq / count),
	}


func _luma_correlation(game_image: Image, final_image: Image) -> float:
	var width = game_image.get_width()
	var height = game_image.get_height()
	var count = max(1, width * height)
	var game_sum = 0.0
	var final_sum = 0.0
	var samples = []
	samples.resize(count)
	var index = 0
	for y in range(height):
		for x in range(width):
			var game_luma = _luma(game_image.get_pixel(x, y))
			var fx = int(float(x) / max(1.0, float(width - 1)) * float(final_image.get_width() - 1))
			var fy = int(float(y) / max(1.0, float(height - 1)) * float(final_image.get_height() - 1))
			var final_luma = _luma(final_image.get_pixel(fx, fy))
			samples[index] = Vector2(game_luma, final_luma)
			index += 1
			game_sum += game_luma
			final_sum += final_luma
	var game_mean = game_sum / count
	var final_mean = final_sum / count
	var numerator = 0.0
	var game_var = 0.0
	var final_var = 0.0
	for sample in samples:
		var gd = sample.x - game_mean
		var fd = sample.y - final_mean
		numerator += gd * fd
		game_var += gd * gd
		final_var += fd * fd
	return numerator / max(sqrt(game_var * final_var), 1e-6)


func _mean_luma(image: Image) -> float:
	var sum = 0.0
	var count = max(1, image.get_width() * image.get_height())
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			sum += _luma(image.get_pixel(x, y))
	return sum / count


func _luma(color: Color) -> float:
	return max(color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722, 0.0)


func _per_megapixel(count: int, width: int, height: int) -> float:
	return float(count) / max(float(width * height) / 1000000.0, 1e-6)


func _safe_ratio(a: float, b: float) -> float:
	return a / max(b, 1e-6)


func _collect_knobs(env: Environment) -> Dictionary:
	var knobs = {}
	for property_name in RTGI_KNOB_STAGES.keys():
		knobs[property_name] = {
			"value": env.get(property_name),
			"stage": RTGI_KNOB_STAGES[property_name],
		}
	return knobs


func _find_first_world_environment(node: Node) -> WorldEnvironment:
	for child in _walk(node):
		if child is WorldEnvironment:
			return child
	return null


func _find_node(node: Node, target_name: String) -> Node:
	if node.name == target_name:
		return node
	for child in node.get_children():
		var found = _find_node(child, target_name)
		if found != null:
			return found
	return null


func _walk(node: Node) -> Array:
	var nodes = [node]
	for child in node.get_children():
		nodes.append_array(_walk(child))
	return nodes


func _unload_scene(scene: Node) -> void:
	if scene != null and is_instance_valid(scene):
		scene.queue_free()


func _load_resume_results(cases: Array) -> Dictionary:
	var completed = {}
	var planned = {}
	for test_case in cases:
		planned[test_case["name"]] = test_case
	var summary_path = _output_path("euphorica_rtgi_summary.json")
	if not FileAccess.file_exists(summary_path):
		return completed
	var file = FileAccess.open(summary_path, FileAccess.READ)
	if file == null:
		return completed
	var parsed = JSON.parse_string(file.get_as_text())
	if typeof(parsed) != TYPE_DICTIONARY:
		return completed
	var prior_results = parsed.get("results", [])
	if typeof(prior_results) != TYPE_ARRAY:
		return completed
	for metrics in prior_results:
		if typeof(metrics) != TYPE_DICTIONARY or not metrics.has("case"):
			continue
		var test_case = metrics["case"]
		if typeof(test_case) != TYPE_DICTIONARY or not test_case.has("name"):
			continue
		var case_name = str(test_case["name"])
		if planned.has(case_name) and _case_outputs_complete(planned[case_name]):
			completed[case_name] = true
			_results.append(metrics)
	return completed


func _case_outputs_complete(test_case: Dictionary) -> bool:
	var case_name = str(test_case["name"])
	if not (FileAccess.file_exists(_output_path("%s_metrics.json" % case_name)) and FileAccess.file_exists(_output_path("%s_game.png" % case_name)) and FileAccess.file_exists(_output_path("%s_final.png" % case_name))):
		return false
	if _capture_debug and bool(test_case.get("rtgi_enabled", false)):
		for view in _debug_views:
			if not FileAccess.file_exists(_output_path("%s_%s_game.png" % [case_name, view])):
				return false
	return true


func _load_png_or_null(path: String) -> Image:
	if not FileAccess.file_exists(path):
		return null
	var image = Image.new()
	if image.load(path) != OK:
		return null
	image.convert(Image.FORMAT_RGBA8)
	return image


func _ensure_output_dir() -> bool:
	if _output_dir.begins_with("res://"):
		push_error("Refusing Euphorica capture output under res://; use user:// or an absolute path so the validation fixture remains read-only.")
		return false
	var absolute = _absolute_output_dir()
	var euphorica_root = ProjectSettings.globalize_path("res://")
	if _is_same_or_child_path(absolute, euphorica_root):
		push_error("Refusing Euphorica capture output inside the active Euphorica project: %s" % absolute)
		return false
	DirAccess.make_dir_recursive_absolute(absolute)
	return true


func _output_path(file_name: String) -> String:
	return _absolute_output_dir().path_join(file_name)


func _absolute_output_dir() -> String:
	if _output_dir.begins_with("user://"):
		return ProjectSettings.globalize_path(_output_dir)
	return _output_dir


func _is_same_or_child_path(path: String, root_path: String) -> bool:
	var normalized_path = path.replace("\\", "/").rstrip("/")
	var normalized_root = root_path.replace("\\", "/").rstrip("/")
	if OS.has_feature("windows"):
		normalized_path = normalized_path.to_lower()
		normalized_root = normalized_root.to_lower()
	return normalized_path == normalized_root or normalized_path.begins_with(normalized_root + "/")


func _write_json(path: String, data: Variant) -> void:
	var file = FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("Unable to write %s" % path)
		return
	file.store_string(JSON.stringify(data, "\t"))
	file.store_string("\n")
