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
var _results = []


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
	var cases = _build_cases()
	var baseline_game: Image = null
	var baseline_final: Image = null
	for test_case in cases:
		var capture = await _run_case(test_case)
		if test_case.get("baseline", false):
			baseline_game = capture["game_image"]
			baseline_final = capture["final_image"]
		var metrics = _measure_case(capture["game_frames"], capture["game_image"], capture["final_image"], baseline_game, baseline_final)
		metrics["case"] = test_case.duplicate(true)
		metrics["rtgi_knobs"] = _collect_knobs(capture["environment"])
		metrics["normal_textures_flipped"] = capture["normal_textures_flipped"]
		_results.append(metrics)
		_write_json(_output_path("%s_metrics.json" % test_case["name"]), metrics)
		_unload_scene(capture["scene"])
		await process_frame
	_write_json(_output_path("euphorica_rtgi_summary.json"), {
		"profile": _profile,
		"scene": _scene_path,
		"results": _results,
		"knob_stages": RTGI_KNOB_STAGES,
	})
	quit(0)


func _build_cases() -> Array:
	var cases = []
	if _profile in ["compare", "matrix"]:
		cases.append(_case("no_rtgi", false, _base_mode, _base_denoise, _base_history, _base_resolution, {}, true))
	if _profile == "no_rtgi":
		return [_case("no_rtgi", false, _base_mode, _base_denoise, _base_history, _base_resolution, {}, true)]
	if _profile == "rtgi_on":
		return [_case("rtgi_on", true, _base_mode, _base_denoise, _base_history, _base_resolution, {}, false)]
	if _profile == "normal_ab":
		return [
			_case("normal_opengl_y", true, _base_mode, _base_denoise, _base_history, _base_resolution, { "normal_y": "opengl" }, false),
			_case("normal_directx_y", true, _base_mode, _base_denoise, _base_history, _base_resolution, { "normal_y": "directx" }, false),
		]
	if _profile == "matrix":
		for mode in ["simple_rt", "path_traced"]:
			for denoise in [0.90, 0.95, 0.98, 1.0]:
				for history in [0.95, 0.98]:
					cases.append(_case("%s_d%.2f_h%.2f_640" % [mode, denoise, history], true, mode, denoise, history, Vector2i(640, 360), {}, false))
		for mode in ["simple_rt", "path_traced"]:
			cases.append(_case("%s_d%.2f_h%.2f_1280" % [mode, _base_denoise, _base_history], true, mode, _base_denoise, _base_history, Vector2i(1280, 720), {}, false))
		for toggle in ["no_glow_fog", "no_lantern_emission", "no_omni_shadow"]:
			cases.append(_case("path_%s" % toggle, true, "path_traced", _base_denoise, _base_history, _base_resolution, { toggle: true }, false))
		cases.append(_case("normal_opengl_y", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "normal_y": "opengl" }, false))
		cases.append(_case("normal_directx_y", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "normal_y": "directx" }, false))
		return cases
	cases.append(_case("simple_rt_d%.2f_h%.2f" % [_base_denoise, _base_history], true, "simple_rt", _base_denoise, _base_history, _base_resolution, {}, false))
	cases.append(_case("path_traced_d%.2f_h%.2f" % [_base_denoise, _base_history], true, "path_traced", _base_denoise, _base_history, _base_resolution, {}, false))
	cases.append(_case("path_no_glow_fog", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_glow_fog": true }, false))
	cases.append(_case("path_no_lantern_emission", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_lantern_emission": true }, false))
	cases.append(_case("path_no_omni_shadow", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "no_omni_shadow": true }, false))
	if _capture_debug:
		cases.append(_case("path_normal_deviation", true, "path_traced", _base_denoise, _base_history, _base_resolution, { "debug_mode": Environment.RT_DEBUG_NORMAL_DEVIATION }, false))
	return cases


func _case(name: String, rtgi_enabled: bool, mode: String, denoise: float, history: float, resolution: Vector2i, options: Dictionary, baseline: bool) -> Dictionary:
	var result = options.duplicate(true)
	result["name"] = name.replace(".", "_")
	result["rtgi_enabled"] = rtgi_enabled
	result["mode"] = mode
	result["denoise"] = denoise
	result["history"] = history
	result["resolution"] = resolution
	result["baseline"] = baseline
	return result


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
	var flipped_count = _apply_scene_toggles(scene, test_case)

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
	return {
		"scene": scene,
		"game_viewport": game_viewport,
		"game_frames": frames,
		"game_image": game_image,
		"final_image": final_image,
		"environment": env,
		"normal_textures_flipped": flipped_count,
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
	env.rtgi_denoiser_split_signals = true
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


func _apply_scene_toggles(scene: Node, test_case: Dictionary) -> int:
	if test_case.get("no_omni_shadow", false):
		for node in _walk(scene):
			if node is OmniLight3D:
				node.shadow_enabled = false
	if test_case.get("no_lantern_emission", false):
		for node in _walk(scene):
			if node is GeometryInstance3D:
				_zero_emission_materials(node)
	if str(test_case.get("normal_y", "opengl")) == "directx":
		return _flip_normal_textures(scene)
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


func _flip_normal_textures(scene: Node) -> int:
	var flipped = 0
	for node in _walk(scene):
		if node is MeshInstance3D and node.mesh != null:
			for surface in range(node.mesh.get_surface_count()):
				var material = node.get_surface_override_material(surface)
				if material == null:
					material = node.mesh.surface_get_material(surface)
				if material is BaseMaterial3D and material.normal_texture != null:
					var duplicated = material.duplicate(true) as BaseMaterial3D
					var flipped_texture = _create_y_flipped_texture(duplicated.normal_texture)
					if flipped_texture != null:
						duplicated.normal_texture = flipped_texture
						node.set_surface_override_material(surface, duplicated)
						flipped += 1
	return flipped


func _create_y_flipped_texture(texture: Texture2D) -> Texture2D:
	var image = texture.get_image()
	if image == null or image.is_empty():
		return null
	image.convert(Image.FORMAT_RGBA8)
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var color = image.get_pixel(x, y)
			color.g = 1.0 - color.g
			image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _capture_viewport(viewport: Viewport) -> Image:
	var image = viewport.get_texture().get_image()
	image.convert(Image.FORMAT_RGBA8)
	return image


func _measure_case(frames: Array, game_image: Image, final_image: Image, baseline_game: Image, baseline_final: Image) -> Dictionary:
	var metrics = _image_metrics(game_image, "game")
	var final_metrics = _image_metrics(final_image, "final")
	for key in final_metrics.keys():
		metrics[key] = final_metrics[key]
	metrics["temporal_sparkle_per_megapixel_max"] = _temporal_sparkle(frames)
	metrics["temporal_sparkle_per_megapixel_avg"] = _temporal_sparkle_average(frames)
	metrics["final_to_game_luma_correlation"] = _luma_correlation(game_image, final_image)
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
	return {
		"%s_luma_mae" % prefix: luma_abs / count,
		"%s_luma_rmse" % prefix: sqrt(luma_sq / count),
		"%s_rgb_mae" % prefix: rgb_abs / count,
		"%s_rgb_rmse" % prefix: sqrt(rgb_sq / count),
		"%s_contribution_mean_luma_delta" % prefix: (_mean_luma(a) - _mean_luma(b)),
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


func _ensure_output_dir() -> bool:
	if _output_dir.begins_with("res://"):
		push_error("Refusing Euphorica capture output under res://; use user:// or an absolute path so the validation fixture remains read-only.")
		return false
	var absolute = _absolute_output_dir()
	DirAccess.make_dir_recursive_absolute(absolute)
	return true


func _output_path(file_name: String) -> String:
	return _absolute_output_dir().path_join(file_name)


func _absolute_output_dir() -> String:
	if _output_dir.begins_with("user://"):
		return ProjectSettings.globalize_path(_output_dir)
	return _output_dir


func _write_json(path: String, data: Variant) -> void:
	var file = FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("Unable to write %s" % path)
		return
	file.store_string(JSON.stringify(data, "\t"))
	file.store_string("\n")
