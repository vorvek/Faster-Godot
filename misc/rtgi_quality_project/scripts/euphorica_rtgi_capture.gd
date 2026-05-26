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
	"rtgi_analytic_light_sampling_enabled": "RTGI analytic direct source sampling",
	"rtgi_explicit_emissive_sampling_enabled": "RTGI explicit emissive source sampling",
	"rtgi_diffuse_radiance_cache_enabled": "RTGI pre-SVGF diffuse radiance cache",
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
var _disable_rf_output_effect = false
var _diffuse_cache = true
var _profile_timings = false
var _results = []
var _split_pair_metrics = []
var _split_pair_images = {}
var _all_debug_views = ["noisy", "diffuse_noisy", "specular_noisy", "diffuse_final", "specular_final", "specular_guide", "normal_roughness", "viewz_hitdist", "motion_vectors", "signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "source_candidate", "source_history", "source_temporal_delta", "source_rejection", "cache_raw_diffuse", "cache_filtered_diffuse", "cache_hit_confidence", "cache_age", "cache_rejection", "variance", "history_length", "rejection", "final"]
var _debug_views = ["noisy", "signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "source_candidate", "source_history", "source_temporal_delta", "source_rejection", "cache_raw_diffuse", "cache_filtered_diffuse", "cache_hit_confidence", "cache_rejection", "final"]


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
		elif arg == "--euphorica-disable-rf-output":
			_disable_rf_output_effect = true
		elif arg.begins_with("--euphorica-diffuse-cache="):
			_diffuse_cache = not (arg.trim_prefix("--euphorica-diffuse-cache=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg == "--euphorica-profile-timings":
			_profile_timings = true
		elif arg.begins_with("--euphorica-debug-views="):
			_debug_views = _parse_debug_views(arg.trim_prefix("--euphorica-debug-views="))
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


func _parse_debug_views(text: String) -> Array:
	var normalized := text.strip_edges().to_lower()
	if normalized == "all":
		return _all_debug_views.duplicate()
	var views := []
	for part in normalized.split(",", false):
		var view := part.strip_edges()
		if view in _all_debug_views and not views.has(view):
			views.append(view)
	if views.is_empty():
		return _debug_views
	return views


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
		metrics.merge(capture.get("debug_metrics", {}), true)
		metrics["case"] = test_case.duplicate(true)
		metrics["rtgi_knobs"] = _collect_knobs(capture["environment"])
		metrics["rf_output_effect_disabled"] = _disable_rf_output_effect
		metrics["rf_output_effect_disabled_count"] = capture["rf_output_effect_disabled_count"]
		metrics["stage_timings_msec"] = capture["stage_timings_msec"]
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
			"rf_output_effect_disabled": _disable_rf_output_effect,
			"profile_timings": _profile_timings,
			"debug_views": _debug_views,
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
	var timings := {}
	var stage_start := Time.get_ticks_msec()
	var packed = load(_scene_path)
	_record_stage_timing(timings, "load_scene_resource", stage_start)
	if not (packed is PackedScene):
		push_error("Unable to load Euphorica scene: %s" % _scene_path)
		quit(2)
		return {}
	stage_start = Time.get_ticks_msec()
	var scene = (packed as PackedScene).instantiate()
	root.add_child(scene)
	await process_frame
	_record_stage_timing(timings, "instantiate_and_first_frame", stage_start)
	stage_start = Time.get_ticks_msec()
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
	var rf_output_effect_disabled_count := 0
	if _disable_rf_output_effect:
		rf_output_effect_disabled_count = _bypass_rf_output_effect(scene)
	_record_stage_timing(timings, "configure_scene", stage_start)

	stage_start = Time.get_ticks_msec()
	for i in range(_warmup_frames):
		await process_frame
		await RenderingServer.frame_post_draw
	_record_stage_timing(timings, "warmup_frames", stage_start)

	var frames = []
	var frame_count = max(1, _sparkle_frames)
	stage_start = Time.get_ticks_msec()
	for i in range(frame_count):
		await process_frame
		await RenderingServer.frame_post_draw
		frames.append(_capture_viewport(game_viewport))
	_record_stage_timing(timings, "sparkle_capture_frames", stage_start)
	stage_start = Time.get_ticks_msec()
	var game_image: Image = frames[frames.size() - 1]
	var final_image = _capture_viewport(root)
	game_image.save_png(_output_path("%s_game.png" % test_case["name"]))
	final_image.save_png(_output_path("%s_final.png" % test_case["name"]))
	_record_stage_timing(timings, "save_primary_images", stage_start)
	var debug_metrics := {}
	if _capture_debug and bool(test_case.get("rtgi_enabled", false)):
		stage_start = Time.get_ticks_msec()
		debug_metrics = await _capture_debug_views(test_case, game_viewport)
		_record_stage_timing(timings, "debug_capture_views", stage_start)
	return {
		"scene": scene,
		"game_viewport": game_viewport,
		"game_frames": frames,
		"game_image": game_image,
		"final_image": final_image,
		"debug_metrics": debug_metrics,
		"environment": env,
		"normal_textures_flipped": 0,
		"rf_output_effect_disabled_count": rf_output_effect_disabled_count,
		"stage_timings_msec": timings,
	}


func _record_stage_timing(timings: Dictionary, stage: String, start_msec: int) -> void:
	var elapsed := Time.get_ticks_msec() - start_msec
	timings[stage] = elapsed
	if _profile_timings:
		print("Euphorica RTGI capture timing: %s %.2fs" % [stage, float(elapsed) / 1000.0])


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
	env.rtgi_analytic_light_sampling_enabled = bool(test_case.get("analytic_light_sampling", true))
	env.rtgi_explicit_emissive_sampling_enabled = bool(test_case.get("explicit_emissive_sampling", true))
	env.rtgi_diffuse_radiance_cache_enabled = bool(test_case.get("diffuse_cache", _diffuse_cache))
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


func _bypass_rf_output_effect(scene: Node) -> int:
	var disabled := 0
	for node in _walk(scene):
		if node is TextureRect and _is_rf_output_node(node):
			(node as TextureRect).visible = true
			(node as TextureRect).mouse_filter = Control.MOUSE_FILTER_IGNORE
			_neutralize_rf_shader_noise(node as TextureRect)
			disabled += 1
	return disabled


func _is_rf_output_node(node: TextureRect) -> bool:
	if String(node.name).to_lower() == "rfoutput":
		return true
	var shader_material := node.material as ShaderMaterial
	if shader_material == null or shader_material.shader == null:
		return false
	return shader_material.shader.resource_path.to_lower().contains("rf_tv_output")


func _neutralize_rf_shader_noise(node: TextureRect) -> void:
	var shader_material := node.material as ShaderMaterial
	if shader_material == null:
		return
	var material := shader_material.duplicate(true) as ShaderMaterial
	material.set_shader_parameter("dot_crawl_strength", 0.0)
	material.set_shader_parameter("line_jitter_pixels", 0.0)
	material.set_shader_parameter("ghost_strength", 0.0)
	material.set_shader_parameter("signal_grain", 0.0)
	material.set_shader_parameter("dither_strength", 0.0)
	node.material = material


func _capture_debug_views(test_case: Dictionary, game_viewport: Viewport) -> Dictionary:
	var metrics := {}
	for view in _debug_views:
		if view.begins_with("cache_") and not _cache_debug_available(test_case):
			continue
		_apply_debug_view(game_viewport, view)
		await process_frame
		await RenderingServer.frame_post_draw
		var image := _capture_viewport(game_viewport)
		image.save_png(_output_path("%s_%s_game.png" % [test_case["name"], view]))
		var view_metrics := _image_metrics(image, "rtgi_%s" % view)
		view_metrics.merge(_image_roi_metrics(image, "rtgi_%s" % view), true)
		if view.begins_with("cache_"):
			view_metrics.merge(_image_channel_means(image, "rtgi_%s" % view), true)
		if view == "cache_hit_confidence":
			view_metrics.merge(_image_cache_hit_diagnostic_metrics(image, "rtgi_cache"), true)
		elif view == "cache_rejection":
			view_metrics.merge(_image_cache_rejection_diagnostic_metrics(image, "rtgi_cache"), true)
		elif view == "source_candidate":
			view_metrics.merge(_image_source_candidate_diagnostic_metrics(image), true)
		elif view == "source_history":
			view_metrics.merge(_image_source_history_diagnostic_metrics(image), true)
		elif view == "source_temporal_delta":
			view_metrics.merge(_image_source_temporal_delta_diagnostic_metrics(image), true)
		elif view == "source_rejection":
			view_metrics.merge(_image_source_rejection_diagnostic_metrics(image), true)
		for key in view_metrics.keys():
			metrics[key] = view_metrics[key]
	_apply_debug_view(game_viewport, "disabled")
	await process_frame
	await RenderingServer.frame_post_draw
	metrics["rtgi_instability_attribution"] = _source_attribution_summary(metrics)
	return metrics


func _cache_debug_available(test_case: Dictionary) -> bool:
	return bool(test_case.get("rtgi_enabled", false)) and bool(test_case.get("split_signals", true)) and bool(test_case.get("diffuse_cache", _diffuse_cache)) and float(test_case.get("denoise", 0.0)) > 0.001 and float(test_case.get("history", 0.0)) > 0.001


func _image_channel_means(image: Image, prefix: String) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var sum_r := 0.0
	var sum_g := 0.0
	var sum_b := 0.0
	var sum_a := 0.0
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			sum_r += c.r
			sum_g += c.g
			sum_b += c.b
			sum_a += c.a
	return {
		"%s_mean_r" % prefix: sum_r / float(count),
		"%s_mean_g" % prefix: sum_g / float(count),
		"%s_mean_b" % prefix: sum_b / float(count),
		"%s_mean_a" % prefix: sum_a / float(count),
	}


func _image_cache_hit_diagnostic_metrics(image: Image, prefix: String) -> Dictionary:
	var metrics := _image_cache_hit_region(image, prefix, Rect2i(Vector2i.ZERO, Vector2i(image.get_width(), image.get_height())))
	for roi_name in _roi_rects(image).keys():
		metrics.merge(_image_cache_hit_region(image, "%s_%s_roi" % [prefix, roi_name], _roi_rects(image)[roi_name]), true)
	return metrics


func _image_cache_hit_region(image: Image, prefix: String, rect: Rect2i) -> Dictionary:
	var x0 = clampi(rect.position.x, 0, image.get_width() - 1)
	var y0 = clampi(rect.position.y, 0, image.get_height() - 1)
	var x1 = clampi(rect.position.x + rect.size.x, x0 + 1, image.get_width())
	var y1 = clampi(rect.position.y + rect.size.y, y0 + 1, image.get_height())
	var count := maxi((x1 - x0) * (y1 - y0), 1)
	var hits := 0
	var confidence_sum := 0.0
	var age_sum := 0.0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := image.get_pixel(x, y)
			if c.r > 0.5:
				hits += 1
			confidence_sum += c.g
			age_sum += c.b
	return {
		"%s_hit_rate" % prefix: float(hits) / float(count),
		"%s_confidence_mean" % prefix: confidence_sum / float(count),
		"%s_age_mean" % prefix: age_sum / float(count),
	}


func _image_source_candidate_diagnostic_metrics(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var source_coverage := 0
	var confidence_sum := 0.0
	var weight_values: Array[float] = []
	var contribution_values: Array[float] = []
	var emissive_weight_values: Array[float] = []
	var emissive_contribution_values: Array[float] = []
	var downweighted := 0
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.r > 0.0:
				source_coverage += 1
			if _source_class_name(c.r) == "emissive":
				emissive_weight_values.append(c.b)
				emissive_contribution_values.append(c.a)
			if c.g < 0.985 and c.r > 0.0:
				downweighted += 1
			confidence_sum += c.g
			weight_values.append(c.b)
			contribution_values.append(c.a)
	weight_values.sort()
	contribution_values.sort()
	emissive_weight_values.sort()
	emissive_contribution_values.sort()
	var p95_idx := clampi(int(round(float(count - 1) * 0.95)), 0, count - 1)
	var p99_idx := clampi(int(round(float(count - 1) * 0.99)), 0, count - 1)
	var emissive_count := emissive_weight_values.size()
	var emissive_p95_idx := clampi(int(round(float(max(emissive_count - 1, 0)) * 0.95)), 0, max(emissive_count - 1, 0))
	var emissive_p99_idx := clampi(int(round(float(max(emissive_count - 1, 0)) * 0.99)), 0, max(emissive_count - 1, 0))
	var metrics := {
		"rtgi_source_candidate_coverage_rate": float(source_coverage) / float(count),
		"rtgi_source_candidate_confidence_mean": confidence_sum / float(count),
		"rtgi_source_candidate_weight_p95": weight_values[p95_idx],
		"rtgi_source_candidate_weight_p99": weight_values[p99_idx],
		"rtgi_source_candidate_weight_max": weight_values[count - 1],
		"rtgi_source_candidate_contribution_p95": contribution_values[p95_idx],
		"rtgi_source_candidate_contribution_p99": contribution_values[p99_idx],
		"rtgi_source_candidate_contribution_max": contribution_values[count - 1],
		"rtgi_source_rejection_fraction": float(downweighted) / float(count),
	}
	if emissive_count > 0:
		metrics["rtgi_source_emissive_candidate_weight_p95"] = emissive_weight_values[emissive_p95_idx]
		metrics["rtgi_source_emissive_candidate_weight_p99"] = emissive_weight_values[emissive_p99_idx]
		metrics["rtgi_source_emissive_candidate_weight_max"] = emissive_weight_values[emissive_count - 1]
		metrics["rtgi_source_emissive_candidate_contribution_p95"] = emissive_contribution_values[emissive_p95_idx]
		metrics["rtgi_source_emissive_candidate_contribution_p99"] = emissive_contribution_values[emissive_p99_idx]
	else:
		metrics["rtgi_source_emissive_candidate_weight_p95"] = 0.0
		metrics["rtgi_source_emissive_candidate_weight_p99"] = 0.0
		metrics["rtgi_source_emissive_candidate_weight_max"] = 0.0
		metrics["rtgi_source_emissive_candidate_contribution_p95"] = 0.0
		metrics["rtgi_source_emissive_candidate_contribution_p99"] = 0.0
	return metrics


func _source_class_name(encoded_class: float) -> String:
	if encoded_class < 0.375:
		return "direct"
	if encoded_class < 0.625:
		return "emissive"
	if encoded_class < 0.875:
		return "indirect"
	return "sky"


func _image_source_history_diagnostic_metrics(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var current_source := 0
	var eligible := 0
	var class_agree := 0
	var class_counts := {"direct": [0, 0], "emissive": [0, 0], "indirect": [0, 0], "sky": [0, 0]}
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.r <= 0.0:
				continue
			current_source += 1
			var source_class_name := _source_class_name(c.r)
			if c.g > 0.5:
				eligible += 1
				class_counts[source_class_name][0] += 1
				if c.b > 0.5:
					class_agree += 1
					class_counts[source_class_name][1] += 1
	var denom := maxf(float(eligible), 1.0)
	var metrics := {
		"rtgi_source_candidate_temporal_eligible_fraction": float(eligible) / float(count),
		"rtgi_source_candidate_class_agreement_rate": float(class_agree) / denom,
		"rtgi_source_candidate_current_coverage_rate": float(current_source) / float(count),
	}
	for source_label in class_counts.keys():
		var values: Array = class_counts[source_label]
		var class_denom := maxf(float(values[0]), 1.0)
		metrics["rtgi_source_%s_candidate_class_agreement_rate" % source_label] = float(values[1]) / class_denom
		if source_label == "emissive":
			metrics["rtgi_source_emissive_candidate_class_agreement_rate"] = float(values[1]) / class_denom
	return metrics


func _image_source_temporal_delta_diagnostic_metrics(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var accepted_values: Array[float] = []
	var rejected_values: Array[float] = []
	var class_delta_counts := {"direct": 0, "emissive": 0, "indirect": 0, "sky": 0}
	var class_id_counts := {"direct": [0, 0], "emissive": [0, 0], "indirect": [0, 0], "sky": [0, 0]}
	var class_accepted_deltas := {"direct": [], "emissive": [], "indirect": [], "sky": []}
	var class_rejected_deltas := {"direct": [], "emissive": [], "indirect": [], "sky": []}
	var delta_threshold := 0.10
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.r <= 0.0:
				continue
			var delta := c.b
			var source_label := _source_class_name(c.r)
			class_id_counts[source_label][0] += 1
			if c.g > 0.5:
				accepted_values.append(delta)
				class_accepted_deltas[source_label].append(delta)
				class_id_counts[source_label][1] += 1
			else:
				rejected_values.append(delta)
				class_rejected_deltas[source_label].append(delta)
			if delta > delta_threshold:
				class_delta_counts[source_label] += 1
	accepted_values.sort()
	rejected_values.sort()
	var accepted_count := accepted_values.size()
	var rejected_count := rejected_values.size()
	var eligible_count := accepted_count + rejected_count
	var eligible_denom := maxf(float(eligible_count), 1.0)
	var p95_idx := clampi(int(round(float(maxi(accepted_count - 1, 0)) * 0.95)), 0, maxi(accepted_count - 1, 0))
	var p99_idx := clampi(int(round(float(maxi(accepted_count - 1, 0)) * 0.99)), 0, maxi(accepted_count - 1, 0))
	var rejected_p99_idx := clampi(int(round(float(maxi(rejected_count - 1, 0)) * 0.99)), 0, maxi(rejected_count - 1, 0))
	var emissive_accepted: Array = class_accepted_deltas["emissive"]
	var direct_accepted: Array = class_accepted_deltas["direct"]
	var direct_rejected: Array = class_rejected_deltas["direct"]
	var indirect_accepted: Array = class_accepted_deltas["indirect"]
	emissive_accepted.sort()
	direct_accepted.sort()
	direct_rejected.sort()
	indirect_accepted.sort()
	var emissive_p99_idx := clampi(int(round(float(maxi(emissive_accepted.size() - 1, 0)) * 0.99)), 0, maxi(emissive_accepted.size() - 1, 0))
	var direct_p95_idx := clampi(int(round(float(maxi(direct_accepted.size() - 1, 0)) * 0.95)), 0, maxi(direct_accepted.size() - 1, 0))
	var direct_p99_idx := clampi(int(round(float(maxi(direct_accepted.size() - 1, 0)) * 0.99)), 0, maxi(direct_accepted.size() - 1, 0))
	var direct_rejected_p99_idx := clampi(int(round(float(maxi(direct_rejected.size() - 1, 0)) * 0.99)), 0, maxi(direct_rejected.size() - 1, 0))
	var indirect_p95_idx := clampi(int(round(float(maxi(indirect_accepted.size() - 1, 0)) * 0.95)), 0, maxi(indirect_accepted.size() - 1, 0))
	var indirect_p99_idx := clampi(int(round(float(maxi(indirect_accepted.size() - 1, 0)) * 0.99)), 0, maxi(indirect_accepted.size() - 1, 0))
	var metrics := {
		"rtgi_source_candidate_history_accept_rate": float(accepted_count) / eligible_denom,
		"rtgi_source_candidate_history_reject_rate": float(rejected_count) / eligible_denom,
		"rtgi_source_candidate_temporal_agreement_rate": float(accepted_count) / eligible_denom,
		"rtgi_source_candidate_id_reuse_rate": float(accepted_count) / eligible_denom,
		"rtgi_source_candidate_temporal_delta_p95": accepted_values[p95_idx] if accepted_count > 0 else 0.0,
		"rtgi_source_candidate_temporal_delta_p99": accepted_values[p99_idx] if accepted_count > 0 else 0.0,
		"rtgi_source_candidate_rejected_temporal_delta_p99": rejected_values[rejected_p99_idx] if rejected_count > 0 else 0.0,
		"rtgi_source_direct_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["direct"], width, height),
		"rtgi_source_direct_candidate_temporal_delta_p95": direct_accepted[direct_p95_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_candidate_temporal_delta_p99": direct_accepted[direct_p99_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_candidate_rejected_temporal_delta_p99": direct_rejected[direct_rejected_p99_idx] if not direct_rejected.is_empty() else 0.0,
		"rtgi_source_direct_reuse_temporal_delta_p95": direct_accepted[direct_p95_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_reuse_temporal_delta_p99": direct_accepted[direct_p99_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_emissive_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["emissive"], width, height),
		"rtgi_source_indirect_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["indirect"], width, height),
		"rtgi_source_sky_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["sky"], width, height),
		"rtgi_source_emissive_candidate_temporal_delta_p99": emissive_accepted[emissive_p99_idx] if not emissive_accepted.is_empty() else 0.0,
		"rtgi_source_indirect_temporal_delta_p95": indirect_accepted[indirect_p95_idx] if not indirect_accepted.is_empty() else 0.0,
		"rtgi_source_indirect_temporal_delta_p99": indirect_accepted[indirect_p99_idx] if not indirect_accepted.is_empty() else 0.0,
	}
	for source_label in class_id_counts.keys():
		var values: Array = class_id_counts[source_label]
		var source_denom := maxf(float(values[0]), 1.0)
		metrics["rtgi_source_%s_candidate_id_reuse_rate" % source_label] = float(values[1]) / source_denom
		if source_label == "emissive":
			metrics["rtgi_source_emissive_candidate_id_reuse_rate"] = float(values[1]) / source_denom
	return metrics


func _image_source_rejection_diagnostic_metrics(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var direct_current := 0
	var direct_eligible := 0
	var direct_accept := 0
	var direct_reject := 0
	var direct_reject_buckets := {
		"prev_uv": 0,
		"current_history": 0,
		"previous_history": 0,
		"history_id": 0,
		"depth": 0,
		"normal": 0,
		"hit_distance": 0,
		"source_class": 0,
		"source_id": 0,
		"pdf_weight": 0,
		"weight_ratio": 0,
		"low_confidence": 0,
	}
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.r <= 0.0:
				continue
			direct_current += 1
			var reason := clampi(int(round(c.b * 15.0)), 0, 15)
			if c.g > 0.5:
				direct_eligible += 1
			if reason == 0 and c.g > 0.5:
				direct_accept += 1
			else:
				direct_reject += 1
				match reason:
					1:
						direct_reject_buckets["prev_uv"] += 1
					2:
						direct_reject_buckets["current_history"] += 1
					3:
						direct_reject_buckets["previous_history"] += 1
					4:
						direct_reject_buckets["history_id"] += 1
					5:
						direct_reject_buckets["depth"] += 1
					6:
						direct_reject_buckets["normal"] += 1
					7:
						direct_reject_buckets["hit_distance"] += 1
					8:
						direct_reject_buckets["source_class"] += 1
					9:
						direct_reject_buckets["source_id"] += 1
					10:
						direct_reject_buckets["pdf_weight"] += 1
					12:
						direct_reject_buckets["weight_ratio"] += 1
					13:
						direct_reject_buckets["low_confidence"] += 1
	var direct_history_denom := maxf(float(direct_accept + direct_reject), 1.0)
	return {
		"rtgi_source_direct_history_coverage_rate": float(direct_current) / float(count),
		"rtgi_source_direct_history_eligible_fraction": float(direct_eligible) / float(maxi(direct_current, 1)),
		"rtgi_source_direct_history_accept_rate": float(direct_accept) / direct_history_denom,
		"rtgi_source_direct_history_reject_rate": float(direct_reject) / direct_history_denom,
		"rtgi_source_direct_history_reject_prev_uv_fraction": float(direct_reject_buckets["prev_uv"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_current_history_fraction": float(direct_reject_buckets["current_history"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_previous_history_fraction": float(direct_reject_buckets["previous_history"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_history_id_fraction": float(direct_reject_buckets["history_id"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_depth_fraction": float(direct_reject_buckets["depth"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_normal_fraction": float(direct_reject_buckets["normal"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_hit_distance_fraction": float(direct_reject_buckets["hit_distance"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_source_class_fraction": float(direct_reject_buckets["source_class"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_source_id_fraction": float(direct_reject_buckets["source_id"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_pdf_weight_fraction": float(direct_reject_buckets["pdf_weight"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_weight_ratio_fraction": float(direct_reject_buckets["weight_ratio"]) / direct_history_denom,
		"rtgi_source_direct_history_reject_low_confidence_fraction": float(direct_reject_buckets["low_confidence"]) / direct_history_denom,
		"rtgi_source_direct_reuse_attempt_rate": float(direct_accept + direct_reject) / float(count),
		"rtgi_source_direct_reuse_accept_rate": float(direct_accept) / direct_history_denom,
		"rtgi_source_direct_reuse_fallback_rate": float(direct_reject) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_depth_fraction": float(direct_reject_buckets["depth"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_normal_fraction": float(direct_reject_buckets["normal"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_hit_distance_fraction": float(direct_reject_buckets["hit_distance"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_source_id_fraction": float(direct_reject_buckets["source_id"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_pdf_ratio_fraction": float(direct_reject_buckets["pdf_weight"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_weight_ratio_fraction": float(direct_reject_buckets["weight_ratio"]) / direct_history_denom,
		"rtgi_source_direct_reuse_reject_low_confidence_fraction": float(direct_reject_buckets["low_confidence"]) / direct_history_denom,
	}


func _image_cache_rejection_diagnostic_metrics(image: Image, prefix: String) -> Dictionary:
	var metrics := _image_cache_rejection_region(image, prefix, Rect2i(Vector2i.ZERO, Vector2i(image.get_width(), image.get_height())))
	for roi_name in _roi_rects(image).keys():
		metrics.merge(_image_cache_rejection_region(image, "%s_%s_roi" % [prefix, roi_name], _roi_rects(image)[roi_name]), true)
	return metrics


func _image_cache_rejection_region(image: Image, prefix: String, rect: Rect2i) -> Dictionary:
	var x0 = clampi(rect.position.x, 0, image.get_width() - 1)
	var y0 = clampi(rect.position.y, 0, image.get_height() - 1)
	var x1 = clampi(rect.position.x + rect.size.x, x0 + 1, image.get_width())
	var y1 = clampi(rect.position.y + rect.size.y, y0 + 1, image.get_height())
	var count := maxi((x1 - x0) * (y1 - y0), 1)
	var buckets := []
	for i in range(10):
		buckets.append(0)
	for y in range(y0, y1):
		for x in range(x0, x1):
			var bucket := clampi(int(round(image.get_pixel(x, y).r * 8.0)), 0, 9)
			buckets[bucket] += 1
	return {
		"%s_rejection_low_confidence_fraction" % prefix: float(buckets[1]) / float(count),
		"%s_rejection_disocclusion_fraction" % prefix: float(buckets[2] + buckets[3]) / float(count),
		"%s_rejection_history_id_fraction" % prefix: float(buckets[4]) / float(count),
		"%s_rejection_normal_fraction" % prefix: float(buckets[5]) / float(count),
		"%s_rejection_depth_fraction" % prefix: float(buckets[6]) / float(count),
		"%s_rejection_radiance_delta_fraction" % prefix: float(buckets[7]) / float(count),
		"%s_rejection_variance_fraction" % prefix: float(buckets[8]) / float(count),
	}


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
		"source_candidate":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_CANDIDATE
		"source_history":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_HISTORY
		"source_temporal_delta":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_TEMPORAL_DELTA
		"source_rejection":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_REJECTION
		"cache_raw_diffuse":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_CACHE_RAW_DIFFUSE
		"cache_filtered_diffuse":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE
		"cache_hit_confidence":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_CACHE_HIT_CONFIDENCE
		"cache_age":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_CACHE_AGE
		"cache_rejection":
			return RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_CACHE_REJECTION
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


func _source_attribution_summary(metrics: Dictionary) -> String:
	var sources := {
		"analytic direct": _source_score(metrics, "signal_direct"),
		"visible/secondary emissive": _source_score(metrics, "signal_emissive"),
		"indirect/throughput": _source_score(metrics, "signal_indirect"),
		"sky/environment": _source_score(metrics, "signal_sky"),
		"clamp/weight risk": _source_score(metrics, "signal_confidence"),
		"final/post amplification": _source_score(metrics, "final"),
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
	var final_metric_image := _analysis_size_copy(final_image, game_image.get_width(), game_image.get_height())
	var baseline_final_metric := _analysis_size_copy(baseline_final, game_image.get_width(), game_image.get_height()) if baseline_final != null else null
	var metrics = _image_metrics(game_image, "game")
	var final_metrics = _image_metrics(final_metric_image, "final")
	for key in final_metrics.keys():
		metrics[key] = final_metrics[key]
	metrics.merge(_image_roi_metrics(game_image, "game"), true)
	metrics.merge(_image_roi_metrics(final_metric_image, "final"), true)
	metrics["temporal_sparkle_per_megapixel_max"] = _temporal_sparkle(frames)
	metrics["temporal_sparkle_per_megapixel_avg"] = _temporal_sparkle_average(frames)
	metrics["game_convergence_curve"] = _convergence_curve(frames)
	metrics["final_to_game_luma_correlation"] = _luma_correlation(game_image, final_metric_image)
	metrics["final_to_game_visible_speckle_ratio"] = _safe_ratio(metrics["final_visible_speckles_per_megapixel"], metrics["game_visible_speckles_per_megapixel"])
	metrics["final_to_game_firefly_ratio"] = _safe_ratio(metrics["final_full_frame_fireflies_per_megapixel"], metrics["game_full_frame_fireflies_per_megapixel"])
	metrics["final_to_game_p99_luma_ratio"] = _safe_ratio(metrics["final_luma_p99"], metrics["game_luma_p99"])
	metrics["final_to_game_detail_edge_ratio"] = _safe_ratio(metrics["final_detail_edge_energy"], metrics["game_detail_edge_energy"])
	if baseline_game != null:
		var diff = _diff_metrics(game_image, baseline_game, "rtgi_vs_no_rtgi")
		for key in diff.keys():
			metrics[key] = diff[key]
	if baseline_final != null:
		var final_diff = _diff_metrics(final_metric_image, baseline_final_metric, "final_rtgi_vs_no_rtgi")
		for key in final_diff.keys():
			metrics[key] = final_diff[key]
	return metrics


func _analysis_size_copy(image: Image, width: int, height: int) -> Image:
	var result := image.duplicate()
	result.convert(Image.FORMAT_RGBA8)
	if result.get_width() != width or result.get_height() != height:
		result.resize(width, height, Image.INTERPOLATE_LANCZOS)
	return result


func _image_metrics(image: Image, prefix: String) -> Dictionary:
	var width = image.get_width()
	var height = image.get_height()
	var luma_buffer := _make_luma_buffer(image)
	return _image_metrics_luma(luma_buffer, width, height, prefix, 0, 0, width, height, true)


func _make_luma_buffer(image: Image) -> PackedFloat32Array:
	var source := image
	if source.get_format() != Image.FORMAT_RGBA8:
		source = image.duplicate()
		source.convert(Image.FORMAT_RGBA8)
	var width := source.get_width()
	var height := source.get_height()
	var data := source.get_data()
	var count: int = maxi(1, width * height)
	var luma_buffer := PackedFloat32Array()
	luma_buffer.resize(count)
	for i in range(count):
		var offset := i * 4
		luma_buffer[i] = maxf((float(data[offset]) * 0.2126 + float(data[offset + 1]) * 0.7152 + float(data[offset + 2]) * 0.0722) / 255.0, 0.0)
	return luma_buffer


func _image_metrics_luma(luma_buffer: PackedFloat32Array, width: int, height: int, prefix: String, x0: int, y0: int, x1: int, y1: int, include_nonblack: bool) -> Dictionary:
	x0 = clampi(x0, 0, width - 1)
	y0 = clampi(y0, 0, height - 1)
	x1 = clampi(x1, x0 + 1, width)
	y1 = clampi(y1, y0 + 1, height)
	var count = max(1, (x1 - x0) * (y1 - y0))
	var lum_values := PackedFloat32Array()
	lum_values.resize(count)
	var sum = 0.0
	var sum_sq = 0.0
	var max_luma = 0.0
	var saturated = 0
	var nonblack = 0
	var index = 0
	for y in range(y0, y1):
		var row := y * width
		for x in range(x0, x1):
			var luma := luma_buffer[row + x]
			lum_values[index] = luma
			index += 1
			sum += luma
			sum_sq += luma * luma
			max_luma = maxf(max_luma, luma)
			if luma > 0.98:
				saturated += 1
			if luma > 0.003:
				nonblack += 1
	lum_values.sort()
	var mean = sum / count
	var variance = max(sum_sq / count - mean * mean, 0.0)
	var p95 = lum_values[clampi(roundi(float(count - 1) * 0.95), 0, count - 1)]
	var p99 = lum_values[clampi(roundi(float(count - 1) * 0.99), 0, count - 1)]
	var metrics := {
		"%s_luma_mean" % prefix: mean,
		"%s_luma_stddev" % prefix: sqrt(variance),
		"%s_luma_p95" % prefix: p95,
		"%s_luma_p99" % prefix: p99,
		"%s_luma_max" % prefix: max_luma,
		"%s_saturated_luma_fraction" % prefix: float(saturated) / float(count),
		"%s_detail_edge_energy" % prefix: _edge_energy_luma(luma_buffer, width, height, x0, y0, x1, y1),
		"%s_visible_speckles_per_megapixel" % prefix: _visible_speckles_luma(luma_buffer, width, height, x0, y0, x1, y1),
		"%s_full_frame_fireflies_per_megapixel" % prefix: _fireflies_luma(luma_buffer, width, height, x0, y0, x1, y1),
	}
	if include_nonblack:
		metrics["%s_nonblack_fraction" % prefix] = float(nonblack) / float(count)
	return metrics


func _image_roi_metrics(image: Image, prefix: String) -> Dictionary:
	var metrics = {}
	var luma_buffer := _make_luma_buffer(image)
	var width := image.get_width()
	var height := image.get_height()
	for roi_name in _roi_rects(image).keys():
		var rect: Rect2i = _roi_rects(image)[roi_name]
		var x0 = clampi(rect.position.x, 0, width - 1)
		var y0 = clampi(rect.position.y, 0, height - 1)
		var x1 = clampi(rect.position.x + rect.size.x, x0 + 1, width)
		var y1 = clampi(rect.position.y + rect.size.y, y0 + 1, height)
		var roi_metrics := _image_metrics_luma(luma_buffer, width, height, "%s_%s_roi" % [prefix, roi_name], x0, y0, x1, y1, false)
		roi_metrics["%s_%s_roi_fireflies_per_megapixel" % [prefix, roi_name]] = roi_metrics.get("%s_%s_roi_full_frame_fireflies_per_megapixel" % [prefix, roi_name], 0.0)
		roi_metrics.erase("%s_%s_roi_full_frame_fireflies_per_megapixel" % [prefix, roi_name])
		metrics.merge(roi_metrics, true)
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
	var metrics := _image_metrics_luma(_make_luma_buffer(image), image.get_width(), image.get_height(), prefix, x0, y0, x1, y1, false)
	metrics["%s_fireflies_per_megapixel" % prefix] = metrics.get("%s_full_frame_fireflies_per_megapixel" % prefix, 0.0)
	metrics.erase("%s_full_frame_fireflies_per_megapixel" % prefix)
	return metrics


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
	var width = mini(a.get_width(), b.get_width())
	var height = mini(a.get_height(), b.get_height())
	return _frame_delta_speckles_luma(_make_luma_buffer(a), _make_luma_buffer(b), width, height)


func _visible_speckles(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	return _visible_speckles_luma(_make_luma_buffer(image), width, height, 0, 0, width, height)


func _visible_speckles_luma(luma_buffer: PackedFloat32Array, width: int, height: int, x0: int, y0: int, x1: int, y1: int) -> float:
	var hits = 0
	var sx0 = maxi(1, x0)
	var sy0 = maxi(1, y0)
	var sx1 = mini(width - 1, x1)
	var sy1 = mini(height - 1, y1)
	for y in range(sy0, sy1):
		var row := y * width
		for x in range(sx0, sx1):
			var center := luma_buffer[row + x]
			var neighborhood = 0.0
			for oy in range(-1, 2):
				var nrow := (y + oy) * width
				for ox in range(-1, 2):
					if ox != 0 or oy != 0:
						neighborhood += luma_buffer[nrow + x + ox]
			neighborhood /= 8.0
			if center > maxf(neighborhood * 2.2 + 0.04, 0.10):
				hits += 1
	return _per_megapixel(hits, max(1, x1 - x0), max(1, y1 - y0))


func _fireflies(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	return _fireflies_luma(_make_luma_buffer(image), width, height, 0, 0, width, height)


func _fireflies_luma(luma_buffer: PackedFloat32Array, width: int, height: int, x0: int, y0: int, x1: int, y1: int) -> float:
	var hits = 0
	var sx0 = maxi(2, x0)
	var sy0 = maxi(2, y0)
	var sx1 = mini(width - 2, x1)
	var sy1 = mini(height - 2, y1)
	for y in range(sy0, sy1):
		var row := y * width
		for x in range(sx0, sx1):
			var center := luma_buffer[row + x]
			var max_neighbor = 0.0
			var sum_neighbor = 0.0
			var samples = 0
			for oy in range(-2, 3):
				var nrow := (y + oy) * width
				for ox in range(-2, 3):
					if ox == 0 and oy == 0:
						continue
					var luma := luma_buffer[nrow + x + ox]
					max_neighbor = maxf(max_neighbor, luma)
					sum_neighbor += luma
					samples += 1
			var avg_neighbor = sum_neighbor / maxf(1.0, float(samples))
			if center > maxf(max_neighbor * 1.45 + 0.03, avg_neighbor * 3.0 + 0.08):
				hits += 1
	return _per_megapixel(hits, max(1, x1 - x0), max(1, y1 - y0))


func _visible_speckles_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	return _visible_speckles_luma(_make_luma_buffer(image), image.get_width(), image.get_height(), x0, y0, x1, y1)


func _fireflies_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	return _fireflies_luma(_make_luma_buffer(image), image.get_width(), image.get_height(), x0, y0, x1, y1)


func _edge_energy(image: Image) -> float:
	var width = image.get_width()
	var height = image.get_height()
	return _edge_energy_luma(_make_luma_buffer(image), width, height, 0, 0, width, height)


func _edge_energy_luma(luma_buffer: PackedFloat32Array, width: int, height: int, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum = 0.0
	var count = 0
	var sx0 = maxi(1, x0)
	var sy0 = maxi(1, y0)
	var sx1 = mini(width - 1, x1)
	var sy1 = mini(height - 1, y1)
	for y in range(sy0, sy1):
		var row := y * width
		for x in range(sx0, sx1):
			var gx := luma_buffer[row + x + 1] - luma_buffer[row + x - 1]
			var gy := luma_buffer[row + width + x] - luma_buffer[row - width + x]
			sum += sqrt(gx * gx + gy * gy)
			count += 1
	return sum / max(1, count)


func _edge_energy_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	return _edge_energy_luma(_make_luma_buffer(image), image.get_width(), image.get_height(), x0, y0, x1, y1)


func _frame_delta_speckles_luma(a_luma: PackedFloat32Array, b_luma: PackedFloat32Array, width: int, height: int) -> float:
	var hits := 0
	var count: int = maxi(1, width * height)
	for i in range(count):
		if absf(a_luma[i] - b_luma[i]) > 0.08:
			hits += 1
	return _per_megapixel(hits, width, height)


func _diff_metrics(a: Image, b: Image, prefix: String) -> Dictionary:
	var width = min(a.get_width(), b.get_width())
	var height = min(a.get_height(), b.get_height())
	var count = max(1, width * height)
	var aa := _analysis_size_copy(a, width, height)
	var bb := _analysis_size_copy(b, width, height)
	var adata := aa.get_data()
	var bdata := bb.get_data()
	var luma_abs = 0.0
	var luma_sq = 0.0
	var rgb_abs = 0.0
	var rgb_sq = 0.0
	var luma_a_sum = 0.0
	var luma_b_sum = 0.0
	for i in range(count):
		var offset := i * 4
		var ar := float(adata[offset]) / 255.0
		var ag := float(adata[offset + 1]) / 255.0
		var ab := float(adata[offset + 2]) / 255.0
		var br := float(bdata[offset]) / 255.0
		var bg := float(bdata[offset + 1]) / 255.0
		var bbv := float(bdata[offset + 2]) / 255.0
		var la := maxf(ar * 0.2126 + ag * 0.7152 + ab * 0.0722, 0.0)
		var lb := maxf(br * 0.2126 + bg * 0.7152 + bbv * 0.0722, 0.0)
		var dl := la - lb
		luma_abs += absf(dl)
		luma_sq += dl * dl
		luma_a_sum += la
		luma_b_sum += lb
		var dr := ar - br
		var dg := ag - bg
		var db := ab - bbv
		rgb_abs += (absf(dr) + absf(dg) + absf(db)) / 3.0
		rgb_sq += (dr * dr + dg * dg + db * db) / 3.0
	var metrics = {
		"%s_luma_mae" % prefix: luma_abs / count,
		"%s_luma_rmse" % prefix: sqrt(luma_sq / count),
		"%s_rgb_mae" % prefix: rgb_abs / count,
		"%s_rgb_rmse" % prefix: sqrt(rgb_sq / count),
		"%s_contribution_mean_luma_delta" % prefix: (luma_a_sum - luma_b_sum) / count,
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
	var game_luma_buffer := _make_luma_buffer(game_image)
	var final_luma_buffer := _make_luma_buffer(final_image)
	var game_sum = 0.0
	var final_sum = 0.0
	var samples := PackedVector2Array()
	samples.resize(count)
	var index = 0
	for y in range(height):
		for x in range(width):
			var game_luma := game_luma_buffer[y * width + x]
			var fx = int(float(x) / max(1.0, float(width - 1)) * float(final_image.get_width() - 1))
			var fy = int(float(y) / max(1.0, float(height - 1)) * float(final_image.get_height() - 1))
			var final_luma := final_luma_buffer[fy * final_image.get_width() + fx]
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
