extends Node3D

const DEFAULT_OUTPUT_DIR := "user://rtgi_quality"
const EXPECTED_METRICS_PATH := "res://expected/rtgi_quality_expected.json"
const CORNELL_REFERENCE_URL := "https://www.graphics.cornell.edu/online/box/simulated.jpg"
const CORNELL_PUBLIC_DATA_URL := "https://www.graphics.cornell.edu/online/box/data.html"
const SPONZA_KHRONOS_SOURCE_URL := "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza"
const SPONZA_KHRONOS_CANONICAL_SUFFIX := "Models/Sponza/glTF/Sponza.gltf"

# Convergence-aware settle windows (frames). The GI history rebuilds in <= 30
# frames on steady scenes and the temporal resolve caps at 16 accumulated
# samples, so a 45-frame floor after a scene load carries comfortable margin.
# The caps bound scenes that never settle (built-in animated content keeps the
# luma delta high): the load cap is the old fixed warmup count, so the worst
# case costs what the harness always paid.
const SETTLE_LOAD_FLOOR_FRAMES := 45
# After an in-process mode switch, settle at least as long as the old fixed
# 120-frame warmup the per-scene reference values were captured with. The FPT
# temporal tail keeps refining past the point where whole-frame luma deltas
# can detect it (fog max_rel_err 0.0596 at 60 frames vs 0.0495 at 120), so a
# shorter floor reads measurably stale against the recorded baselines. With
# vsync off the extra frames cost well under a second per mode.
const SETTLE_MODE_SWITCH_FLOOR_FRAMES := 120
const SETTLE_MODE_SWITCH_CAP_FRAMES := 240
const SETTLE_CHECK_INTERVAL_FRAMES := 15
# Relative whole-frame mean-luma delta per check interval under which the GI
# is considered settled. Whole-frame luma cannot resolve the late FPT
# accumulation tail (that is what the 120-frame mode-switch floor is for);
# this check exists to extend the settle past the floor while a scene is
# still visibly converging, so it is kept tight.
const SETTLE_MEAN_LUMA_REL_DELTA := 0.001
# Steady-state perf readings kept from the settle tail (the measured render
# time has a few-frame latency, so only the last frames are representative).
const PERF_TAIL_SAMPLES := 8
# light_grid toggle/orbit measurement: frames per phase (the orbit steady-state
# window and the post-toggle recovery window). The recovery criterion is the
# first post-toggle pair whose whole-image mean delta drops back within
# LIGHT_GRID_RECOVERY_FACTOR times the frozen static-noise floor (the
# light_grid_static_tail_delta measured at the end of this same Phase-2 series).
# Anchoring on the static tail rather than the Phase-1 orbit mean is what makes
# this a real recovery measure: the orbit mean is inflated by the camera motion,
# so 1.5x of it sat far above the converged floor and a transient cleared it
# almost immediately. Against the static floor a wrongly GLOBAL reset that keeps
# the whole image churning for many frames pushes the count up instead. The
# recovery window length doubles as the gateable worst-case
# light_grid_toggle_recovery_frames value when the image never recovers.
const LIGHT_GRID_PHASE_FRAMES := 32
const LIGHT_GRID_RECOVERY_FACTOR := 3.0
# Absolute floor under the static-tail-derived recovery threshold, so a scene
# that converges to a near-zero floor cannot make the threshold so tight that
# ordinary temporal noise never clears it.
const LIGHT_GRID_RECOVERY_FLOOR := 0.0005
# Pairs averaged from the end of the frozen post-toggle series for the
# static-scene noise-floor metric. By then the toggle transient is long gone,
# so any remaining frame-to-frame delta is pure temporal noise: a raster
# (--rtgi-mode=off) run measures exactly 0.0 here, and an RTGI pipeline whose
# history survives frames must approach it.
const LIGHT_GRID_TAIL_PAIRS := 8

var _denoise_strength := 1.0
var _history_weight := 0.95
var _firefly_suppression := 1.0
var _detail_preservation := 1.0
var _split_signals := true
var _specular_history_weight := 0.90
var _specular_spatial_strength := 1.0
var _ray_firefly_suppression := 0.85
var _ray_max_radiance := 32.0
var _analytic_light_sampling := true
var _explicit_emissive_sampling := true
var _diffuse_cache := true
var _diffuse_cache_max_entries := 262144
var _strc_enabled := true
var _warmup_frames := 120
# When > 0, every settle renders EXACTLY this many frames instead of the
# convergence-aware early stop. The early stop is content-dependent (a luma
# delta crossing a threshold), so two otherwise identical runs can measure
# different accumulation frames; bit-for-bit regression comparisons need the
# capture frame pinned.
var _settle_frames_override := 0
var _output_dir := DEFAULT_OUTPUT_DIR
var _debug_view := "beauty"
# Per-debug-view captures and metric sweeps are diagnostic-only and cost
# minutes per run (each of the dozens of views re-renders the frame and scans
# it pixel by pixel in script). Regression runs skip them; pass
# --rtgi-capture-debug to opt in.
var _capture_all_debug_views := false
var _capture_comparison := false
var _write_reference := false
var _camera_pan := false
var _specular_object_motion := false
var _specular_motion_nodes: Array[Node3D] = []
var _packed_scene_root: Node3D = null
var _reference_spp := 16
var _sparkle_frames := 16
var _convergence_frames := 0
var _fast_iteration := false
var _scene_mode := "stress"
var _cornell_compare := false
var _cornell_reference_image := ""
var _sponza_path := ""
var _sponza_normal_y_mode := "auto"
var _gate_profile := "strict"
# Per-run override flags. Each stays empty (or NAN) when the matching CLI flag is
# absent, which means "leave the scene-authored value untouched".
var _rtgi_mode_override := ""
# Multi-config run list from --rtgi-modes=<comma list>. When non-empty, one
# process measures every listed mode in sequence over a single scene load.
var _rtgi_modes: Array[String] = []
# Scene-animation frame counter. It advances once per rendered frame across
# settle, capture, and sparkle loops (and across configs in a multi-config
# run) so the frame-counter-driven scene animations stay in lock-step.
var _scene_frame := 0
# Frames the most recent settle actually rendered; recorded in the metrics.
var _settle_frames_used := 0
# Set when a config hit a skip/fatal path that already queued a tree quit, so
# the multi-config loop stops instead of measuring further configs.
var _abort_run := false
var _rtgi_denoiser_override := ""
var _rtgi_resolution_scale_override := NAN
var _rtgi_energy_override := NAN
# 0 means "absent"; requested values are clamped to 1..16.
var _rtgi_samples_per_pixel_override := 0
# Per-preset direct-light RIS candidate budget override (the shadow-ray budget knob).
# 0 means "absent"; a requested value is clamped to 2..16 and written to all three
# rendering/rtgi/direct_light/<tier>/ris_candidates Project Settings BEFORE scene build,
# so whatever preset the scene authors picks it up (the renderer reads it live via
# GLOBAL_GET, no restart). The effective read-back value is recorded in the metrics JSON.
var _rtgi_ris_candidates_override := 0
var _upscaler_override := ""
var _scale_3d_override := NAN
var _camera: Camera3D
var _environment: Environment
# Steady-state perf samples gathered over the tail of the warmup loop. The measured
# render time has a few-frame latency, so only the last frames are sampled.
var _perf_gpu_samples: Array[float] = []
var _perf_cpu_samples: Array[float] = []
var _sponza_asset_loaded := false
var _sponza_normal_y_flipped := false
var _sponza_flipped_normal_texture_count := 0
var _normal_flip_cache := {}


func _ready() -> void:
	_parse_args()
	if _rtgi_ris_candidates_override > 0:
		# Write the requested RIS candidate budget to all three per-preset tier settings
		# BEFORE the scene (and its Environment) is built, so whichever preset the scene
		# authors resolves to picks it up. The renderer reads these live via GLOBAL_GET each
		# frame, so no restart is needed. Setting all three covers performance/balanced/
		# production uniformly for the ladder sweep.
		for _tier in ["performance", "balanced", "production"]:
			ProjectSettings.set_setting("rendering/rtgi/direct_light/%s/ris_candidates" % _tier, _rtgi_ris_candidates_override)
	if DisplayServer.get_name().to_lower() != "headless":
		# Measurement runs must not be paced by the display: with vsync on, the
		# settle loop and every capture frame serialize on vblank.
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if not _rtgi_modes.is_empty():
		# Multi-config run: the first config rides the regular single-mode
		# override machinery; later configs are applied in-process after a
		# mode-switch settle.
		if not _rtgi_mode_override.is_empty():
			push_warning("--rtgi-mode is ignored when --rtgi-modes is given.")
		_rtgi_mode_override = _rtgi_modes[0]
	if _scene_mode == "convergence" and _convergence_frames <= 0:
		_convergence_frames = 48
	if _fast_iteration:
		# Fast iteration: short warmup, beauty PNG + metrics JSON + the
		# scene-specific measurement only. Skip the per-view debug captures and
		# the comparison grid so a single scene finishes in well under a minute.
		_warmup_frames = mini(_warmup_frames, 24)
		_capture_all_debug_views = false
		_capture_comparison = false
	if _scene_mode == "cornell" or _scene_mode == "cornell_box":
		# The committed cornell_box scene reuses the proven runtime cornell
		# projection (25 mm square image plane, 35 mm focal length), which frames
		# the open-front box correctly only on a square viewport.
		_force_square_viewport()
	_build_scene()
	# Apply the per-run environment overrides on top of whatever the scene authored.
	_apply_environment_overrides()
	# Configure the viewport upscaler LAST so it is not clobbered by the
	# square-viewport forcing above (which runs before _build_scene), and so it
	# wins over any scaling the instanced scene set up.
	_apply_viewport_overrides()
	# Enable per-viewport GPU/CPU render-time measurement so _sample_frame_perf can
	# read it. It is off by default (it has a small cost) and returns 0 until enabled.
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	call_deferred("_run_capture")


# Overrides _environment.rtgi_mode / rtgi_denoiser / rtgi_resolution_scale from the
# matching CLI flags. Each branch is a no-op when its flag was absent, so the scene's
# authored values survive untouched.
func _apply_environment_overrides() -> void:
	if _environment == null:
		return
	if not _rtgi_mode_override.is_empty():
		_apply_rtgi_mode(_rtgi_mode_override)
	match _rtgi_denoiser_override:
		"asvfg":
			_environment.rtgi_denoiser = Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL
		"reactive":
			_environment.rtgi_denoiser = Environment.RTGI_DENOISER_REACTIVE
		"none":
			_environment.rtgi_denoiser = Environment.RTGI_DENOISER_NONE
	if not is_nan(_rtgi_resolution_scale_override):
		_environment.rtgi_resolution_scale = _rtgi_resolution_scale_override
	if not is_nan(_rtgi_energy_override):
		_environment.rtgi_energy = _rtgi_energy_override
	if _rtgi_samples_per_pixel_override > 0:
		# Every quality preset pins rtgi_samples_per_pixel to 1, so switch the
		# environment to the Custom preset FIRST or the requested value is
		# discarded. The metrics JSON records the read-back value, which makes
		# any remaining preset pin visible.
		_environment.rtgi_quality_preset = Environment.RTGI_QUALITY_PRESET_CUSTOM
		_environment.rtgi_samples_per_pixel = _rtgi_samples_per_pixel_override


# Applies one RTGI mode config to the live environment. Non-off modes force
# rtgi_enabled back on so an in-process multi-config sequence like
# off,fpt,hybrid can re-enable RTGI after the raster reference config disabled
# it (the committed scenes all author rtgi_enabled = true, so this is a no-op
# for single-mode runs).
func _apply_rtgi_mode(mode: String) -> void:
	if _environment == null:
		return
	match mode:
		"hybrid":
			_environment.rtgi_enabled = true
			_environment.rtgi_mode = Environment.RTGI_MODE_HYBRID
		"fpt":
			_environment.rtgi_enabled = true
			_environment.rtgi_mode = Environment.RTGI_MODE_FULL_PATH_TRACING
		"fpt-reference":
			# Deep-path A/B oracle: the full per-pixel camera-ray path tracer with
			# the probe composite bypassed. Used for informational A/B comparisons
			# against the fast FPT path (for example recording the fog-model
			# divergence); the per-scene gates are calibrated for the fast path.
			_environment.rtgi_enabled = true
			_environment.rtgi_mode = Environment.RTGI_MODE_FULL_PATH_TRACING_REFERENCE
		"reflections":
			_environment.rtgi_enabled = true
			_environment.rtgi_mode = Environment.RTGI_MODE_REFLECTIONS_RT_ONLY
		"off":
			# Raster reference config: keep the scene as authored but disable RTGI
			# entirely, so per-scene comparisons (for example fog parity) can
			# record what the raster pipeline alone produces.
			_environment.rtgi_enabled = false


# Configures the root window viewport's 3D scaling pipeline from --rtgi-upscaler and
# --rtgi-scale-3d. When --rtgi-upscaler is absent the engine-default scaling mode is
# left alone; --rtgi-scale-3d is independent and only touches the render scale.
func _apply_viewport_overrides() -> void:
	var viewport := get_viewport()
	if viewport == null:
		return
	match _upscaler_override:
		"none":
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.use_taa = false
		"taa":
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.use_taa = true
		"fsr2":
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			viewport.use_taa = false
	if not is_nan(_scale_3d_override):
		viewport.scaling_3d_scale = _scale_3d_override


func _rtgi_denoiser_path_name(denoiser: int) -> String:
	match denoiser:
		Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL:
			return "ASVFG"
		Environment.RTGI_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION:
			return "Internal Signal Decomposition"
		Environment.RTGI_DENOISER_NONE:
			return "None"
		Environment.RTGI_DENOISER_REACTIVE:
			return "Reactive (RR-style)"
		_:
			return "Unknown"


func _force_square_viewport() -> void:
	var square_size := Vector2i(768, 768)
	get_viewport().size = square_size
	get_window().content_scale_size = square_size
	if DisplayServer.get_name().to_lower() != "headless":
		DisplayServer.window_set_size(square_size)
		get_window().size = square_size


func _parse_args() -> void:
	for arg in OS.get_cmdline_args():
		if arg.begins_with("--rtgi-denoise-strength="):
			_denoise_strength = clampf(arg.trim_prefix("--rtgi-denoise-strength=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-history-weight="):
			_history_weight = clampf(arg.trim_prefix("--rtgi-history-weight=").to_float(), 0.0, 0.98)
		elif arg.begins_with("--rtgi-firefly-suppression="):
			_firefly_suppression = clampf(arg.trim_prefix("--rtgi-firefly-suppression=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-detail-preservation="):
			_detail_preservation = clampf(arg.trim_prefix("--rtgi-detail-preservation=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-split-signals="):
			_split_signals = not (arg.trim_prefix("--rtgi-split-signals=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-specular-history-weight="):
			_specular_history_weight = clampf(arg.trim_prefix("--rtgi-specular-history-weight=").to_float(), 0.0, 0.98)
		elif arg.begins_with("--rtgi-specular-spatial-strength="):
			_specular_spatial_strength = clampf(arg.trim_prefix("--rtgi-specular-spatial-strength=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-ray-firefly-suppression="):
			_ray_firefly_suppression = clampf(arg.trim_prefix("--rtgi-ray-firefly-suppression=").to_float(), 0.0, 1.0)
		elif arg.begins_with("--rtgi-ray-max-radiance="):
			_ray_max_radiance = maxf(arg.trim_prefix("--rtgi-ray-max-radiance=").to_float(), 0.0)
		elif arg.begins_with("--rtgi-analytic-light-sampling="):
			_analytic_light_sampling = not (arg.trim_prefix("--rtgi-analytic-light-sampling=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-explicit-emissive-sampling="):
			_explicit_emissive_sampling = not (arg.trim_prefix("--rtgi-explicit-emissive-sampling=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-diffuse-cache="):
			_diffuse_cache = not (arg.trim_prefix("--rtgi-diffuse-cache=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-diffuse-cache-max-entries="):
			_diffuse_cache_max_entries = clampi(arg.trim_prefix("--rtgi-diffuse-cache-max-entries=").to_int(), 4096, 4194304)
		elif arg.begins_with("--rtgi-strc-enabled="):
			_strc_enabled = not (arg.trim_prefix("--rtgi-strc-enabled=").to_lower() in ["0", "false", "off", "disabled"])
		elif arg.begins_with("--rtgi-warmup-frames="):
			_warmup_frames = max(1, arg.trim_prefix("--rtgi-warmup-frames=").to_int())
		elif arg.begins_with("--rtgi-settle-frames="):
			_settle_frames_override = max(0, arg.trim_prefix("--rtgi-settle-frames=").to_int())
		elif arg.begins_with("--rtgi-reference-spp="):
			_reference_spp = clampi(arg.trim_prefix("--rtgi-reference-spp=").to_int(), 1, 128)
		elif arg.begins_with("--rtgi-sparkle-frames="):
			_sparkle_frames = clampi(arg.trim_prefix("--rtgi-sparkle-frames=").to_int(), 0, 64)
		elif arg.begins_with("--rtgi-convergence-frames="):
			_convergence_frames = clampi(arg.trim_prefix("--rtgi-convergence-frames=").to_int(), 0, 128)
		elif arg.begins_with("--rtgi-scene="):
			var requested_scene := arg.trim_prefix("--rtgi-scene=").to_lower()
			if requested_scene in ["stress", "cornell", "convergence", "sponza", "sdfgi", "voxelgi", "lightmap", "lightprobe", "path_traced_sdfgi_exclusive", "many_light_emissive", "specular_stability", "offscreen_bounce", "cornell_box", "specular_motion", "reflective_pool", "fog_corridor", "light_grid", "sun_penumbra_ramp"]:
				_scene_mode = requested_scene
			else:
				push_warning("Unknown RTGI quality scene '%s'; using stress scene." % requested_scene)
		elif arg.begins_with("--rtgi-cornell-reference-image="):
			_cornell_reference_image = arg.trim_prefix("--rtgi-cornell-reference-image=")
		elif arg.begins_with("--rtgi-sponza-path="):
			_sponza_path = arg.trim_prefix("--rtgi-sponza-path=")
		elif arg.begins_with("--rtgi-sponza-normal-y="):
			var normal_y_mode := arg.trim_prefix("--rtgi-sponza-normal-y=").to_lower()
			if normal_y_mode in ["auto", "opengl", "gl", "+y", "directx", "dx", "dx12", "-y"]:
				_sponza_normal_y_mode = "directx" if normal_y_mode in ["directx", "dx", "dx12", "-y"] else ("opengl" if normal_y_mode in ["opengl", "gl", "+y"] else "auto")
			else:
				push_warning("Unknown Sponza normal-Y mode '%s'; using auto." % normal_y_mode)
		elif arg.begins_with("--rtgi-sponza-flip-normal-y="):
			var flip_normal_y := arg.trim_prefix("--rtgi-sponza-flip-normal-y=").to_lower()
			if flip_normal_y in ["1", "true", "on", "enabled"]:
				_sponza_normal_y_mode = "directx"
			elif flip_normal_y in ["0", "false", "off", "disabled"]:
				_sponza_normal_y_mode = "opengl"
			else:
				push_warning("Unknown Sponza normal-Y flip value '%s'; using auto." % flip_normal_y)
		elif arg.begins_with("--rtgi-gate-profile="):
			var profile := arg.trim_prefix("--rtgi-gate-profile=").to_lower()
			if profile in ["strict", "smoke"]:
				_gate_profile = profile
			else:
				push_warning("Unknown RTGI gate profile '%s'; using strict." % profile)
		elif arg.begins_with("--rtgi-output-dir="):
			_output_dir = arg.trim_prefix("--rtgi-output-dir=")
		elif arg.begins_with("--rtgi-debug-view="):
			_debug_view = arg.trim_prefix("--rtgi-debug-view=").to_lower()
		elif arg == "--rtgi-capture-debug":
			_capture_all_debug_views = true
		elif arg == "--rtgi-capture-comparison":
			_capture_comparison = true
		elif arg == "--rtgi-cornell-compare":
			_cornell_compare = true
		elif arg == "--rtgi-write-reference":
			_write_reference = true
		elif arg == "--rtgi-camera-pan":
			_camera_pan = true
		elif arg == "--rtgi-specular-object-motion":
			_specular_object_motion = true
		elif arg.begins_with("--rtgi-mode="):
			var mode := arg.trim_prefix("--rtgi-mode=").to_lower()
			if mode in ["hybrid", "fpt", "fpt-reference", "reflections", "off"]:
				_rtgi_mode_override = mode
			else:
				push_warning("Unknown RTGI mode '%s'; leaving the scene-authored mode untouched." % mode)
		elif arg.begins_with("--rtgi-modes="):
			for mode_token in arg.trim_prefix("--rtgi-modes=").to_lower().split(",", false):
				var token := mode_token.strip_edges()
				if token in ["hybrid", "fpt", "fpt-reference", "reflections", "off"]:
					_rtgi_modes.append(token)
				else:
					push_warning("Unknown RTGI mode '%s' in --rtgi-modes; skipping it." % token)
		elif arg.begins_with("--rtgi-denoiser="):
			var denoiser := arg.trim_prefix("--rtgi-denoiser=").to_lower()
			if denoiser in ["asvfg", "reactive", "none"]:
				_rtgi_denoiser_override = denoiser
			else:
				push_warning("Unknown RTGI denoiser '%s'; leaving the scene-authored denoiser untouched." % denoiser)
		elif arg.begins_with("--rtgi-resolution-scale="):
			_rtgi_resolution_scale_override = clampf(arg.trim_prefix("--rtgi-resolution-scale=").to_float(), 0.25, 1.0)
		elif arg.begins_with("--rtgi-energy="):
			# to_float() parses garbage to 0.0, which would silently arm the energy
			# check with a guaranteed-FAIL expectation; only accept a real float.
			var energy_text := arg.trim_prefix("--rtgi-energy=").strip_edges()
			if energy_text.is_valid_float():
				_rtgi_energy_override = clampf(energy_text.to_float(), 0.0, 16.0)
			else:
				push_warning("Invalid --rtgi-energy value '%s'; ignoring the flag." % energy_text)
		elif arg.begins_with("--rtgi-samples-per-pixel="):
			_rtgi_samples_per_pixel_override = clampi(arg.trim_prefix("--rtgi-samples-per-pixel=").to_int(), 1, 16)
		elif arg.begins_with("--rtgi-ris-candidates="):
			_rtgi_ris_candidates_override = clampi(arg.trim_prefix("--rtgi-ris-candidates=").to_int(), 2, 16)
		elif arg.begins_with("--rtgi-upscaler="):
			var upscaler := arg.trim_prefix("--rtgi-upscaler=").to_lower()
			if upscaler in ["none", "taa", "fsr2"]:
				_upscaler_override = upscaler
			else:
				push_warning("Unknown RTGI upscaler '%s'; leaving the viewport scaling untouched." % upscaler)
		elif arg.begins_with("--rtgi-scale-3d="):
			_scale_3d_override = clampf(arg.trim_prefix("--rtgi-scale-3d=").to_float(), 0.5, 1.0)
		elif arg == "--rtgi-fast":
			_fast_iteration = true


func _build_scene() -> void:
	if _is_packed_test_scene():
		_build_packed_test_scene()
		return

	var env := Environment.new()
	env.glow_enabled = false
	env.rtgi_enabled = true
	env.rtgi_disable_in_editor = false
	env.rtgi_mode = Environment.RTGI_MODE_HYBRID
	env.rtgi_samples_per_pixel = 1
	env.rtgi_max_bounces = 3
	env.rtgi_energy = 1.0
	env.rtgi_denoiser = Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL
	env.rtgi_ray_firefly_suppression = _ray_firefly_suppression
	env.rtgi_ray_max_radiance = _ray_max_radiance
	env.rtgi_analytic_light_sampling_enabled = _analytic_light_sampling
	env.rtgi_explicit_emissive_sampling_enabled = _explicit_emissive_sampling
	_environment = env

	var world_environment := WorldEnvironment.new()
	world_environment.name = "RTGIWorldEnvironment"
	world_environment.environment = env
	add_child(world_environment)

	if _scene_mode == "cornell":
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color.BLACK
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.070, 0.052, 0.032)
		env.ambient_light_energy = 0.42
		env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
		env.tonemap_exposure = 0.86
		env.tonemap_white = 8.0
		env.rtgi_max_bounces = 8
		_build_cornell_box_scene(env)
		return
	if _scene_mode == "sponza":
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0.018, 0.020, 0.024)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.055, 0.056, 0.060)
		env.ambient_light_energy = 0.35
		env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
		env.rtgi_max_bounces = 5
		_build_sponza_scene(env)
		return
	if _is_coexistence_scene():
		_build_coexistence_scene(env)
		return
	if _scene_mode == "many_light_emissive":
		_build_many_light_emissive_scene(env)
		return
	if _scene_mode == "specular_stability":
		_build_specular_stability_scene(env)
		return
	if _scene_mode == "offscreen_bounce":
		_build_offscreen_bounce_scene(env)
		return

	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.006, 0.007, 0.009)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.015, 0.015, 0.018)
	env.ambient_light_energy = 0.15
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC

	var brick_material := _make_brick_material()
	var dark_material := _make_flat_material(Color(0.015, 0.013, 0.012), 0.92, 0.0)
	var matte_material := _make_flat_material(Color(0.45, 0.40, 0.33), 0.82, 0.0)
	var metal_material := _make_flat_material(Color(0.55, 0.58, 0.62), 0.18, 1.0)
	var polished_metal_material := _make_flat_material(Color(0.78, 0.74, 0.68), 0.045, 1.0)
	var glossy_material := _make_flat_material(Color(0.82, 0.78, 0.70), 0.035, 0.0)
	var emissive_material := _make_emissive_material(Color(1.0, 0.78, 0.45), 7.5)
	var shader_light_material := _make_shader_light_material()

	_add_box("BrickFloor", Vector3(0.0, -0.05, -1.0), Vector3(7.8, 0.10, 7.0), brick_material)
	_add_box("BrickBackWall", Vector3(0.0, 1.7, -4.1), Vector3(7.8, 3.5, 0.14), brick_material)
	_add_box("DarkRightWall", Vector3(3.85, 1.55, -1.2), Vector3(0.12, 3.4, 6.0), dark_material)
	_add_box("LeftBounceWall", Vector3(-3.85, 1.55, -1.2), Vector3(0.12, 3.4, 6.0), matte_material)
	_add_box("CeilingOccluder", Vector3(0.8, 3.05, -1.3), Vector3(6.1, 0.12, 5.8), dark_material)
	_add_box("MetalPatch", Vector3(1.65, 0.55, -2.95), Vector3(0.95, 0.95, 0.08), metal_material)
	_add_box("PolishedMetalStrip", Vector3(0.95, 0.35, -2.05), Vector3(1.15, 0.08, 0.55), polished_metal_material)
	_add_sphere("GlossySphere", Vector3(1.90, 0.52, -1.92), 0.34, glossy_material)
	_add_box("ShaderLightFallbackPatch", Vector3(-0.90, 0.72, -3.88), Vector3(0.70, 0.55, 0.05), shader_light_material)
	_add_box("SmallEmitter", Vector3(-1.95, 1.15, -3.92), Vector3(0.16, 0.16, 0.05), emissive_material)

	var light := OmniLight3D.new()
	light.name = "SmallWarmLight"
	light.position = Vector3(-2.0, 1.22, -3.55)
	light.light_energy = 3.2
	light.omni_range = 4.5
	light.shadow_enabled = true
	add_child(light)

	_camera = Camera3D.new()
	_camera.name = "MetricCamera"
	_camera.current = true
	_camera.fov = 58.0
	_camera.near = 0.05
	_camera.far = 80.0
	_camera.position = Vector3(0.15, 1.45, 2.5)
	add_child(_camera)
	_camera.look_at(Vector3(-0.15, 1.15, -2.5), Vector3.UP)


func _is_packed_test_scene() -> bool:
	return _scene_mode in ["cornell_box", "specular_motion", "reflective_pool", "fog_corridor", "light_grid", "sun_penumbra_ramp"]


# Loads one of the committed static test scenes (cornell_box, specular_motion,
# reflective_pool) and runs it through the same warmup/capture/measure pipeline
# as the runtime-built modes. These scenes carry their own WorldEnvironment with
# RTGI enabled, so the harness pulls _environment and _camera from the instanced
# tree and applies the per-run RTGI knob overrides on top.
func _build_packed_test_scene() -> void:
	var scene_path := "res://scenes/%s.tscn" % _scene_mode
	if not ResourceLoader.exists(scene_path):
		push_error("Packed test scene '%s' is missing. Run tools/generate_test_scenes.gd to regenerate it." % scene_path)
		get_tree().quit(2)
		return
	var packed: PackedScene = load(scene_path)
	if packed == null:
		push_error("Could not load packed test scene '%s'." % scene_path)
		get_tree().quit(2)
		return
	var instance := packed.instantiate()
	if not (instance is Node3D):
		push_error("Packed test scene '%s' did not instantiate a Node3D root." % scene_path)
		get_tree().quit(2)
		return
	_packed_scene_root = instance as Node3D
	add_child(_packed_scene_root)

	var found_envs := _packed_scene_root.find_children("*", "WorldEnvironment", true, false)
	var world_environment: WorldEnvironment = found_envs[0] if not found_envs.is_empty() else null
	if world_environment != null and world_environment.environment != null:
		_environment = world_environment.environment
		# Apply per-run RTGI knob overrides on top of the scene-authored values so
		# CLI sweeps behave the same way they do for the runtime-built modes.
		_environment.rtgi_ray_firefly_suppression = _ray_firefly_suppression
		_environment.rtgi_ray_max_radiance = _ray_max_radiance
		_environment.rtgi_analytic_light_sampling_enabled = _analytic_light_sampling
		_environment.rtgi_explicit_emissive_sampling_enabled = _explicit_emissive_sampling
	else:
		push_warning("Packed test scene '%s' has no WorldEnvironment; RTGI metrics may be unreliable." % scene_path)

	var found_cams := _packed_scene_root.find_children("*", "Camera3D", true, false)
	_camera = found_cams[0] if not found_cams.is_empty() else null
	if _camera != null:
		_camera.current = true


func _build_many_light_emissive_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.002, 0.002, 0.003)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.004, 0.004, 0.005)
	env.ambient_light_energy = 0.05
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 0.9
	env.rtgi_max_bounces = 5

	var wall := _make_flat_material(Color(0.032, 0.034, 0.038), 0.86, 0.0)
	var dark := _make_flat_material(Color(0.010, 0.011, 0.013), 0.92, 0.0)
	var detail := _make_flat_material(Color(0.34, 0.31, 0.27), 0.68, 0.0)
	var metal := _make_flat_material(Color(0.70, 0.72, 0.74), 0.10, 1.0)
	var emitter_a := _make_emissive_material(Color(1.0, 0.50, 0.18), 9.0)
	var emitter_b := _make_emissive_material(Color(0.30, 0.68, 1.0), 6.5)
	var emitter_c := _make_emissive_material(Color(0.55, 1.0, 0.45), 5.0)

	_add_box("ManyLightFloor", Vector3(0.0, -0.05, -2.0), Vector3(8.0, 0.10, 8.0), dark)
	_add_box("ManyLightBackWall", Vector3(0.0, 1.9, -5.9), Vector3(8.0, 4.0, 0.12), wall)
	_add_box("ManyLightLeftWall", Vector3(-4.0, 1.9, -2.0), Vector3(0.12, 4.0, 8.0), wall)
	_add_box("ManyLightRightWall", Vector3(4.0, 1.9, -2.0), Vector3(0.12, 4.0, 8.0), wall)
	_add_box("ManyLightCeiling", Vector3(0.0, 3.85, -2.0), Vector3(8.0, 0.12, 8.0), wall)
	_add_box("ManyLightDarkROI", Vector3(2.65, 1.05, -5.82), Vector3(1.8, 1.45, 0.06), dark)
	_add_box("ManyLightDetailROI", Vector3(-2.35, 0.8, -5.80), Vector3(1.5, 1.1, 0.06), detail)
	_add_box("ManyLightSpecularStrip", Vector3(0.55, 0.35, -3.35), Vector3(1.5, 0.08, 0.70), metal)
	_add_sphere("ManyLightSpecularSphere", Vector3(1.55, 0.52, -3.70), 0.34, metal)

	var emitter_positions := [
		Vector3(-2.9, 1.45, -5.76), Vector3(-1.55, 2.15, -5.76), Vector3(0.15, 1.35, -5.76),
		Vector3(1.85, 2.35, -5.76), Vector3(3.05, 1.55, -5.76), Vector3(-3.88, 1.8, -2.9),
		Vector3(3.88, 2.05, -2.1), Vector3(-0.60, 3.78, -2.8)
	]
	for i in range(emitter_positions.size()):
		var mat := emitter_a if i % 3 == 0 else (emitter_b if i % 3 == 1 else emitter_c)
		_add_box("ManyLightEmitter_%02d" % i, emitter_positions[i], Vector3(0.18, 0.18, 0.05), mat)

	for i in range(36):
		var col := i % 9
		var row := int(i / 9)
		var light := OmniLight3D.new()
		light.name = "ManyAnalytic_%02d" % i
		light.position = Vector3(-3.2 + float(col) * 0.8, 0.85 + float(row) * 0.55, -4.6 + sin(float(i) * 1.7) * 0.28)
		light.light_color = Color.from_hsv(fposmod(float(i) * 0.071, 1.0), 0.36, 1.0)
		light.light_energy = 0.22 + 0.08 * float(i % 4)
		light.omni_range = 2.7
		light.shadow_enabled = true
		add_child(light)

	_camera = Camera3D.new()
	_camera.name = "ManyLightCamera"
	_camera.current = true
	_camera.fov = 57.0
	_camera.near = 0.05
	_camera.far = 70.0
	_camera.position = Vector3(0.0, 1.55, 1.95)
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 1.55, -4.4), Vector3.UP)


func _build_offscreen_bounce_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.003, 0.004, 0.006)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.01, 0.01, 0.012)
	env.ambient_light_energy = 0.08
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.rtgi_max_bounces = 5

	var floor_material := _make_flat_material(Color(0.42, 0.40, 0.36), 0.84, 0.0)
	var wall_material := _make_flat_material(Color(0.55, 0.57, 0.60), 0.88, 0.0)
	var red_emissive := _make_emissive_material(Color(1.0, 0.18, 0.05), 9.0)
	var blue_emissive := _make_emissive_material(Color(0.05, 0.25, 1.0), 7.0)

	_add_box("BounceFloor", Vector3(0.0, -0.05, -1.0), Vector3(8.0, 0.10, 8.0), floor_material)
	_add_box("FrontWall", Vector3(0.0, 1.55, -4.2), Vector3(8.0, 3.2, 0.12), wall_material)
	_add_box("LeftWall", Vector3(-3.9, 1.55, -0.9), Vector3(0.12, 3.2, 6.6), wall_material)
	_add_box("RightWall", Vector3(3.9, 1.55, -0.9), Vector3(0.12, 3.2, 6.6), wall_material)
	_add_box("BehindCameraRedEmitter", Vector3(-1.35, 1.25, 2.9), Vector3(1.1, 1.0, 0.08), red_emissive)
	_add_box("BehindCameraBlueEmitter", Vector3(1.35, 1.25, 2.9), Vector3(1.1, 1.0, 0.08), blue_emissive)

	var red_light := OmniLight3D.new()
	red_light.name = "OffscreenRedLight"
	red_light.position = Vector3(-1.35, 1.30, 2.55)
	red_light.light_color = Color(1.0, 0.20, 0.08)
	red_light.light_energy = 4.5
	red_light.omni_range = 5.5
	add_child(red_light)

	var blue_light := OmniLight3D.new()
	blue_light.name = "OffscreenBlueLight"
	blue_light.position = Vector3(1.35, 1.30, 2.55)
	blue_light.light_color = Color(0.08, 0.24, 1.0)
	blue_light.light_energy = 3.8
	blue_light.omni_range = 5.5
	add_child(blue_light)

	_camera = Camera3D.new()
	_camera.name = "OffscreenBounceCamera"
	_camera.current = true
	_camera.fov = 60.0
	_camera.near = 0.05
	_camera.far = 80.0
	_camera.position = Vector3(0.0, 1.25, 1.75)
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 1.15, -3.4), Vector3.UP)


func _build_specular_stability_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0015, 0.0018, 0.0024)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.004, 0.004, 0.005)
	env.ambient_light_energy = 0.03
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 0.72
	env.tonemap_white = 8.0
	env.rtgi_max_bounces = 5

	var wall := _make_flat_material(Color(0.018, 0.020, 0.024), 0.82, 0.0)
	var dark := _make_flat_material(Color(0.006, 0.006, 0.007), 0.92, 0.0)
	var glossy_floor := _make_flat_material(Color(0.34, 0.36, 0.38), 0.055, 0.0)
	var mirror := _make_flat_material(Color(0.86, 0.88, 0.90), 0.012, 1.0)
	var brushed := _make_flat_material(Color(0.70, 0.68, 0.62), 0.11, 1.0)
	var matte_detail := _make_flat_material(Color(0.26, 0.23, 0.20), 0.64, 0.0)
	var normal_gloss := _make_normal_mapped_glossy_material()
	var red_emitter := _make_emissive_material(Color(1.0, 0.16, 0.08), 13.0)
	var cyan_emitter := _make_emissive_material(Color(0.12, 0.70, 1.0), 9.5)
	var warm_emitter := _make_emissive_material(Color(1.0, 0.74, 0.32), 10.5)

	_add_box("SpecularFloor", Vector3(0.0, -0.05, -2.2), Vector3(9.5, 0.10, 8.8), glossy_floor)
	_add_box("SpecularBackWall", Vector3(0.0, 2.05, -6.35), Vector3(9.5, 4.2, 0.12), wall)
	_add_box("SpecularLeftWall", Vector3(-4.75, 2.05, -2.2), Vector3(0.12, 4.2, 8.8), wall)
	_add_box("SpecularRightWall", Vector3(4.75, 2.05, -2.2), Vector3(0.12, 4.2, 8.8), wall)
	_add_box("SpecularCeiling", Vector3(0.0, 4.10, -2.2), Vector3(9.5, 0.12, 8.8), dark)

	_add_box("SpecularRedPanel", Vector3(-2.7, 1.75, -6.27), Vector3(0.42, 0.42, 0.05), red_emitter)
	_add_box("SpecularCyanPanel", Vector3(2.85, 2.35, -6.27), Vector3(0.58, 0.34, 0.05), cyan_emitter)
	_add_box("SpecularWarmPanel", Vector3(0.1, 3.25, -4.8), Vector3(1.1, 0.07, 0.46), warm_emitter)

	var mirror_sphere := _add_sphere("SpecularMirrorSphere", Vector3(-1.45, 0.62, -3.25), 0.46, mirror)
	mirror_sphere.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	_specular_motion_nodes.append(mirror_sphere)
	var glossy_sphere := _add_sphere("SpecularGlossySphere", Vector3(1.35, 0.50, -3.85), 0.38, brushed)
	glossy_sphere.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	_specular_motion_nodes.append(glossy_sphere)
	var normal_panel := _add_box("SpecularNormalMappedGlossyPanel", Vector3(-3.10, 0.84, -4.15), Vector3(1.15, 1.25, 0.08), normal_gloss)
	normal_panel.rotation_degrees.y = -18.0

	var ramp_roughness := [0.02, 0.08, 0.20, 0.42, 0.72]
	for i in range(ramp_roughness.size()):
		var roughness: float = ramp_roughness[i]
		var ramp_material := _make_flat_material(Color(0.44, 0.45, 0.46), roughness, 0.0)
		var tile := _add_box("SpecularRoughnessRamp_%02d" % i, Vector3(-2.35 + float(i) * 1.15, 0.015, -1.05), Vector3(1.0, 0.05, 1.30), ramp_material)
		tile.gi_mode = GeometryInstance3D.GI_MODE_STATIC

	for i in range(8):
		var x := -3.5 + float(i) * 1.0
		var height := 0.55 + 0.22 * float(i % 3)
		_add_box("SpecularOccluder_%02d" % i, Vector3(x, height * 0.5, -4.75 + sin(float(i)) * 0.42), Vector3(0.16, height, 0.55), matte_detail)

	for i in range(28):
		var col := i % 7
		var row := int(i / 7)
		var light := OmniLight3D.new()
		light.name = "SpecularAnalytic_%02d" % i
		light.position = Vector3(-3.45 + float(col) * 1.15, 0.95 + float(row) * 0.48, -4.90 + cos(float(i) * 1.37) * 0.35)
		light.light_color = Color.from_hsv(fposmod(float(i) * 0.113, 1.0), 0.52, 1.0)
		light.light_energy = 0.34 + 0.14 * float(i % 4)
		light.light_size = 0.035
		light.omni_range = 3.1
		light.shadow_enabled = true
		add_child(light)

	_camera = Camera3D.new()
	_camera.name = "SpecularStabilityCamera"
	_camera.current = true
	_camera.fov = 54.0
	_camera.near = 0.05
	_camera.far = 80.0
	_camera.position = Vector3(0.0, 1.38, 1.55)
	add_child(_camera)
	_camera.look_at(Vector3(-0.08, 1.05, -4.05), Vector3.UP)


func _is_coexistence_scene() -> bool:
	return _scene_mode in ["sdfgi", "voxelgi", "lightmap", "lightprobe", "path_traced_sdfgi_exclusive"]


func _build_coexistence_scene(env: Environment) -> void:
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.012, 0.014, 0.018)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.018, 0.019, 0.022)
	env.ambient_light_energy = 0.08
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.rtgi_mode = Environment.RTGI_MODE_FULL_PATH_TRACING if _scene_mode == "path_traced_sdfgi_exclusive" else Environment.RTGI_MODE_HYBRID
	env.rtgi_max_bounces = 4
	env.sdfgi_enabled = _scene_mode in ["sdfgi", "path_traced_sdfgi_exclusive"]
	if env.sdfgi_enabled:
		env.sdfgi_use_occlusion = true
		env.sdfgi_energy = 1.15

	var floor_material := _make_flat_material(Color(0.36, 0.35, 0.32), 0.78, 0.0)
	var wall_material := _make_flat_material(Color(0.30, 0.32, 0.36), 0.82, 0.0)
	var owner_material := _make_flat_material(Color(0.64, 0.60, 0.52), 0.72, 0.0)
	var emitter_material := _make_emissive_material(Color(0.16, 0.72, 1.0), 5.5)
	_add_box("CoexistFloor", Vector3(0.0, -0.05, -1.0), Vector3(5.4, 0.10, 5.2), floor_material)
	_add_box("CoexistBackWall", Vector3(0.0, 1.35, -3.45), Vector3(5.4, 2.8, 0.12), wall_material)
	_add_box("CoexistLeftWall", Vector3(-2.70, 1.35, -1.0), Vector3(0.12, 2.8, 5.2), wall_material)
	_add_box("CoexistRightWall", Vector3(2.70, 1.35, -1.0), Vector3(0.12, 2.8, 5.2), wall_material)
	_add_box("CoexistCyanEmitter", Vector3(-1.85, 1.15, -3.35), Vector3(0.46, 0.70, 0.06), emitter_material)

	match _scene_mode:
		"lightmap":
			var owner := _add_lightmapped_quad("CoexistLightmapOwner", owner_material)
			_add_procedural_lightmap(owner, Color(0.26, 0.72, 0.54, 1.0))
		"lightprobe":
			var probe_owner := _add_sphere("CoexistLightProbeDynamicOwner", Vector3(0.0, 0.76, -2.45), 0.55, owner_material)
			probe_owner.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
			_add_procedural_lightprobe_data(Color(0.28, 0.62, 0.94, 1.0))
		_:
			_add_box("CoexistRasterGIOwner", Vector3(0.0, 0.75, -2.80), Vector3(1.45, 1.35, 0.16), owner_material)

	var key := OmniLight3D.new()
	key.name = "CoexistRealtimeKey"
	key.position = Vector3(1.9, 2.2, 0.6)
	key.light_energy = 6.0 if _scene_mode == "path_traced_sdfgi_exclusive" else 0.65
	key.light_bake_mode = Light3D.BAKE_DYNAMIC
	key.omni_range = 5.0
	key.shadow_enabled = true
	add_child(key)

	var baked_static := OmniLight3D.new()
	baked_static.name = "CoexistStaticBakedLight"
	baked_static.position = Vector3(-1.25, 1.45, -2.15)
	baked_static.light_color = Color(0.50, 0.78, 1.0)
	baked_static.light_energy = 1.15
	baked_static.light_bake_mode = Light3D.BAKE_STATIC
	baked_static.omni_range = 4.0
	baked_static.shadow_enabled = true
	add_child(baked_static)

	if _scene_mode == "voxelgi":
		_add_voxelgi_probe()

	_camera = Camera3D.new()
	_camera.name = "CoexistenceCamera"
	_camera.current = true
	_camera.fov = 55.0
	_camera.near = 0.05
	_camera.far = 40.0
	_camera.position = Vector3(0.0, 1.25, 1.95)
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 0.88, -2.55), Vector3.UP)


func _add_lightmapped_quad(node_name: String, material: Material) -> MeshInstance3D:
	var vertices := PackedVector3Array([
		Vector3(-1.15, 0.20, -2.70),
		Vector3(1.15, 0.20, -2.70),
		Vector3(1.15, 1.95, -2.70),
		Vector3(-1.15, 0.20, -2.70),
		Vector3(1.15, 1.95, -2.70),
		Vector3(-1.15, 1.95, -2.70),
	])
	var normal := Vector3(0.0, 0.0, 1.0)
	var normals := PackedVector3Array([normal, normal, normal, normal, normal, normal])
	var uvs := PackedVector2Array([
		Vector2(0.0, 1.0),
		Vector2(1.0, 1.0),
		Vector2(1.0, 0.0),
		Vector2(0.0, 1.0),
		Vector2(1.0, 0.0),
		Vector2(0.0, 0.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_TEX_UV2] = uvs
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance


func _add_procedural_lightmap(owner: MeshInstance3D, color: Color) -> void:
	var image := Image.create_empty(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(color)
	var texture_array := Texture2DArray.new()
	texture_array.create_from_images([image])
	var data := LightmapGIData.new()
	data.set_uses_spherical_harmonics(false)
	data.set_lightmap_textures([texture_array])

	var lightmap := LightmapGI.new()
	lightmap.name = "CoexistProceduralLightmapGI"
	add_child(lightmap)
	data.add_user(lightmap.get_path_to(owner), Rect2(0.0, 0.0, 1.0, 1.0), 0, -1)
	lightmap.light_data = data


func _add_procedural_lightprobe_data(color: Color) -> void:
	var data := LightmapGIData.new()
	var points := PackedVector3Array([
		Vector3(-1.6, 0.0, -3.6),
		Vector3(1.6, 0.0, -3.6),
		Vector3(0.0, 2.6, -3.2),
		Vector3(0.0, 0.8, -0.8),
	])
	var sh := PackedColorArray()
	var l0 := Color(color.r / 0.886227, color.g / 0.886227, color.b / 0.886227, color.a)
	for i in range(4):
		sh.append(l0)
		for j in range(8):
			sh.append(Color(0, 0, 0, 0))
	var tetrahedra := PackedInt32Array([0, 1, 2, 3])
	var bsp := PackedInt32Array()
	bsp.resize(6)
	bsp[0] = 0
	bsp[1] = 0
	bsp[2] = 0
	bsp[3] = 0
	bsp[4] = -1
	bsp[5] = -1
	data.probe_data = {
		"bounds": AABB(Vector3(-2.0, -0.2, -3.8), Vector3(4.0, 3.2, 3.4)),
		"interior": false,
		"points": points,
		"sh": sh,
		"tetrahedra": tetrahedra,
		"bsp": bsp,
		"baked_exposure": 1.0,
		"lightprobe_hash": 1,
	}

	var lightmap := LightmapGI.new()
	lightmap.name = "CoexistProceduralLightProbeGI"
	add_child(lightmap)
	lightmap.light_data = data

	var probe := LightmapProbe.new()
	probe.name = "CoexistLightmapProbe"
	probe.position = Vector3(0.0, 0.8, -2.2)
	add_child(probe)


func _add_voxelgi_probe() -> void:
	var voxel := VoxelGI.new()
	voxel.name = "CoexistVoxelGI"
	voxel.size = Vector3(5.6, 3.2, 5.6)
	voxel.position = Vector3(0.0, 1.25, -1.55)
	voxel.subdiv = VoxelGI.SUBDIV_64
	add_child(voxel)
	voxel.bake(self, false)
	if voxel.data == null:
		push_warning("VoxelGI coexistence bake did not produce probe data; metrics will report the missing contribution.")


func _build_cornell_box_scene(_env: Environment) -> void:
	var white := _make_lambert_material(Color(0.705, 0.675, 0.515))
	var green := _make_lambert_material(Color(0.150, 0.360, 0.125))
	var red := _make_lambert_material(Color(0.520, 0.085, 0.070))
	var light_material := _make_lambert_emissive_material(Color(1.0, 0.84, 0.58), 3.2)

	var w := 5.56
	var h := 5.488
	var d := 5.592
	var t := 0.08
	_add_box("CornellFloor", Vector3(w * 0.5, -t * 0.5, d * 0.5), Vector3(w, t, d), white)
	_add_box("CornellCeiling", Vector3(w * 0.5, h + t * 0.5, d * 0.5), Vector3(w, t, d), white)
	_add_box("CornellBackWall", Vector3(w * 0.5, h * 0.5, d + t * 0.5), Vector3(w, h, t), white)
	# With this camera basis, world +X projects to screen-left; keep the
	# material placement aligned to Cornell's rendered reference.
	_add_box("CornellScreenRightGreenWall", Vector3(-t * 0.5, h * 0.5, d * 0.5), Vector3(t, h, d), green)
	_add_box("CornellScreenLeftRedWall", Vector3(w + t * 0.5, h * 0.5, d * 0.5), Vector3(t, h, d), red)
	_add_box("CornellCeilingEmitter", Vector3(2.78, h - 0.018, 2.795), Vector3(1.30, 0.035, 1.05), light_material)

	var tall := _add_box("CornellTallBlock", Vector3(3.685, 1.65, 3.512), Vector3(1.62, 3.30, 1.62), white)
	tall.rotation_degrees.y = 17.0
	var short := _add_box("CornellShortBlock", Vector3(1.855, 0.825, 1.69), Vector3(1.62, 1.65, 1.62), white)
	short.rotation_degrees.y = -17.0

	var light := OmniLight3D.new()
	light.name = "CornellCeilingAreaApproximation"
	light.position = _cornell_point(278.0, 520.0, 279.5)
	light.light_energy = 4.6
	light.light_size = 1.35
	light.omni_range = 8.0
	light.shadow_enabled = true
	add_child(light)

	var fill := SpotLight3D.new()
	fill.name = "CornellPanelSoftFill"
	fill.position = _cornell_point(278.0, 535.0, 279.5)
	fill.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	fill.light_energy = 0.38
	fill.spot_range = 8.0
	fill.spot_angle = 80.0
	fill.shadow_enabled = true
	add_child(fill)

	var bounce_fill := OmniLight3D.new()
	bounce_fill.name = "CornellBounceApproximation"
	bounce_fill.position = _cornell_point(278.0, 190.0, -90.0)
	bounce_fill.light_color = Color(1.0, 0.78, 0.46)
	bounce_fill.light_energy = 0.82
	bounce_fill.light_size = 2.2
	bounce_fill.omni_range = 7.0
	bounce_fill.shadow_enabled = false
	add_child(bounce_fill)

	_camera = Camera3D.new()
	_camera.name = "CornellCamera"
	_camera.current = true
	_camera.keep_aspect = Camera3D.KEEP_WIDTH
	_camera.fov = rad_to_deg(2.0 * atan(0.025 / (2.0 * 0.035)))
	_camera.near = 0.05
	_camera.far = 30.0
	_camera.position = _cornell_point(278.0, 273.0, -800.0)
	add_child(_camera)
	_camera.look_at(_cornell_point(278.0, 273.0, 0.0), Vector3.UP)


func _build_sponza_scene(_env: Environment) -> void:
	var asset_path := _resolve_sponza_path()
	if not asset_path.is_empty() and FileAccess.file_exists(asset_path):
		var gltf := GLTFDocument.new()
		var state := GLTFState.new()
		var error := gltf.append_from_file(asset_path, state)
		if error == OK:
			var imported := gltf.generate_scene(state)
			if imported != null:
				imported.name = "ExternalSponza"
				imported.scale = Vector3.ONE
				add_child(imported)
				_sponza_normal_y_flipped = _should_flip_sponza_normal_y(asset_path)
				if _sponza_normal_y_flipped:
					_flip_sponza_normal_maps(imported)
					print("RTGI quality: flipped Sponza normal map green channels for DirectX-style normal-Y mode (%d textures)." % _sponza_flipped_normal_texture_count)
				_mark_geometry_static(imported)
				_sponza_asset_loaded = true
		if not _sponza_asset_loaded:
			push_warning("Could not load Sponza asset from '%s'; using procedural fallback atrium." % asset_path)
	else:
		print("RTGI quality: Sponza asset is not configured. Set --rtgi-sponza-path=<gltf/glb> or GODOT_RTGI_SPONZA_PATH; using procedural fallback atrium.")

	if not _sponza_asset_loaded:
		_build_sponza_fallback_atrium()

	var sun := DirectionalLight3D.new()
	sun.name = "SponzaSun"
	sun.rotation_degrees = Vector3(-46.0, -32.0, 0.0)
	sun.light_energy = 4.0
	sun.shadow_enabled = true
	add_child(sun)

	var camera_fill := OmniLight3D.new()
	camera_fill.name = "SponzaSoftCameraFill"
	camera_fill.position = Vector3(-10.0, 4.2, 0.0)
	camera_fill.light_color = Color(1.0, 0.91, 0.78)
	camera_fill.light_energy = 4.2
	camera_fill.light_size = 3.5
	camera_fill.omni_range = 22.0
	camera_fill.shadow_enabled = false
	add_child(camera_fill)

	_camera = Camera3D.new()
	_camera.name = "SponzaCamera"
	_camera.current = true
	_camera.fov = 58.0
	_camera.near = 0.05
	_camera.far = 120.0
	_camera.position = Vector3(-12.0, 3.4, 0.0)
	add_child(_camera)
	_camera.look_at(Vector3(2.0, 3.0, 0.0), Vector3.UP)


func _build_sponza_fallback_atrium() -> void:
	var stone := _make_brick_material()
	var dark := _make_flat_material(Color(0.12, 0.11, 0.10), 0.86, 0.0)
	var red_cloth := _make_flat_material(Color(0.42, 0.05, 0.04), 0.72, 0.0)
	var gold := _make_flat_material(Color(0.86, 0.63, 0.24), 0.18, 1.0)
	_add_box("FallbackSponzaFloor", Vector3(0.0, -0.05, 0.0), Vector3(9.0, 0.10, 16.0), stone)
	_add_box("FallbackSponzaLeftArcade", Vector3(-4.5, 1.8, 0.0), Vector3(0.24, 3.8, 15.5), stone)
	_add_box("FallbackSponzaRightArcade", Vector3(4.5, 1.8, 0.0), Vector3(0.24, 3.8, 15.5), stone)
	_add_box("FallbackSponzaBack", Vector3(0.0, 1.8, -7.8), Vector3(8.8, 3.8, 0.24), stone)
	_add_box("FallbackSponzaCeiling", Vector3(0.0, 3.7, 0.0), Vector3(9.0, 0.18, 16.0), dark)
	for side in [-1.0, 1.0]:
		for i in range(7):
			var z := -6.0 + float(i) * 2.0
			_add_box("FallbackSponzaColumn_%s_%d" % ["L" if side < 0.0 else "R", i], Vector3(side * 3.35, 1.35, z), Vector3(0.34, 2.7, 0.34), stone)
			_add_box("FallbackSponzaCurtain_%s_%d" % ["L" if side < 0.0 else "R", i], Vector3(side * 3.16, 1.65, z + 0.85), Vector3(0.07, 1.8, 0.95), red_cloth)
	_add_box("FallbackSponzaSpecularBasin", Vector3(0.0, 0.38, 0.2), Vector3(1.7, 0.20, 1.7), gold)


func _run_capture() -> void:
	_apply_debug_view(_debug_view)
	# Convergence-aware settle replaces the old fixed warmup loop: render at
	# least the floor, stop once the mean luma stabilizes, and never exceed the
	# old fixed count (--rtgi-warmup-frames stays the cap, and acts as the floor
	# when it is smaller, e.g. under --rtgi-fast).
	if _settle_frames_override > 0:
		_settle_frames_used = await _settle_until_converged(_settle_frames_override, _settle_frames_override)
	else:
		_settle_frames_used = await _settle_until_converged(SETTLE_LOAD_FLOOR_FRAMES, _warmup_frames)

	var output_dir_error := DirAccess.make_dir_recursive_absolute(_output_dir)
	if output_dir_error != OK:
		push_error("Could not create RTGI quality output directory: %s" % _output_dir)
		get_tree().quit(2)
		return

	if _rtgi_modes.is_empty():
		var failures: Array[String] = await _capture_and_measure_config("")
		if _abort_run:
			return
		print("RTGI quality output: %s" % ProjectSettings.globalize_path(_output_dir))
		get_tree().quit(0 if failures.is_empty() else 1)
		return

	# Multi-config run: one process, one scene load; each listed mode settles,
	# measures, and writes its own per-mode outputs. The exit code fails if ANY
	# config failed.
	var any_failed := false
	for i in range(_rtgi_modes.size()):
		var mode: String = _rtgi_modes[i]
		if i > 0:
			_apply_rtgi_mode(mode)
			# Settle again with the mode-switch floor/cap before measuring.
			if _settle_frames_override > 0:
				_settle_frames_used = await _settle_until_converged(_settle_frames_override, _settle_frames_override)
			else:
				_settle_frames_used = await _settle_until_converged(SETTLE_MODE_SWITCH_FLOOR_FRAMES, SETTLE_MODE_SWITCH_CAP_FRAMES)
		var config_failures: Array[String] = await _capture_and_measure_config("_%s" % mode)
		if _abort_run:
			return
		print("RTGI config %s: %s" % [mode, "PASS" if config_failures.is_empty() else "FAIL (%s)" % "; ".join(config_failures)])
		if not config_failures.is_empty():
			any_failed = true
	print("RTGI quality output: %s" % ProjectSettings.globalize_path(_output_dir))
	get_tree().quit(1 if any_failed else 0)


# Renders at least floor_frames, then every SETTLE_CHECK_INTERVAL_FRAMES
# compares the viewport mean luma against the previous check; once the
# relative delta drops under SETTLE_MEAN_LUMA_REL_DELTA the GI is treated as
# converged. cap_frames bounds scenes that never settle (built-in motion keeps
# the delta high), so the worst case equals the old fixed warmup. Returns the
# number of frames rendered.
func _settle_until_converged(floor_frames: int, cap_frames: int) -> int:
	var cap := maxi(cap_frames, 1)
	var floor_count := clampi(floor_frames, 1, cap)
	# Take the first luma reading one interval before the floor so the run can
	# stop exactly at the floor when the scene is already steady.
	var first_check := maxi(floor_count - SETTLE_CHECK_INTERVAL_FRAMES, 1)
	_perf_gpu_samples.clear()
	_perf_cpu_samples.clear()
	var previous_luma := -1.0
	var frames := 0
	while frames < cap:
		if _camera_pan:
			_animate_camera(_scene_frame)
		_animate_specular_objects(_scene_frame)
		await _wait_render_frame()
		_scene_frame += 1
		frames += 1
		_sample_frame_perf()
		if frames >= first_check and (frames - first_check) % SETTLE_CHECK_INTERVAL_FRAMES == 0:
			var luma := _viewport_mean_luma_fast()
			if luma >= 0.0 and previous_luma >= 0.0 and frames >= floor_count \
					and absf(luma - previous_luma) <= maxf(previous_luma, 1e-4) * SETTLE_MEAN_LUMA_REL_DELTA:
				break
			previous_luma = luma
	# Keep only the steady-state tail of the perf samples; the measured render
	# time has a few-frame latency, so the last readings are the ones we want.
	while _perf_gpu_samples.size() > PERF_TAIL_SAMPLES:
		_perf_gpu_samples.pop_front()
	while _perf_cpu_samples.size() > PERF_TAIL_SAMPLES:
		_perf_cpu_samples.pop_front()
	return frames


# Cheap whole-frame mean luma for the settle convergence check: one GPU
# readback, a bilinear downsample to 64x36, then a ~2k-pixel average. Returns
# -1.0 when no viewport image is available (headless), disabling the check.
func _viewport_mean_luma_fast() -> float:
	var viewport_texture := get_viewport().get_texture()
	if viewport_texture == null:
		return -1.0
	var image := viewport_texture.get_image()
	if image == null:
		return -1.0
	image.convert(Image.FORMAT_RGBA8)
	image.resize(64, 36, Image.INTERPOLATE_BILINEAR)
	var sum := 0.0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			sum += _luma(image.get_pixel(x, y))
	return sum / float(maxi(image.get_width() * image.get_height(), 1))


# Captures and measures the current configuration: beauty PNG, metrics JSON,
# scene-specific measurements, optional diagnostic sweeps. base_suffix keeps
# multi-config outputs distinct per mode (empty for single-mode runs, so the
# single-mode file names are unchanged). Returns the gate-failure list; on the
# skip/fatal paths it quits the tree and sets _abort_run.
func _capture_and_measure_config(base_suffix: String) -> Array[String]:
	var base_name := "%s_rtgi_strength_%0.2f%s" % [_scene_mode, _denoise_strength, base_suffix]
	if DisplayServer.get_name().to_lower() == "headless":
		var skipped := {
			"skipped": true,
			"reason": "Viewport texture is unavailable. Run without --headless on a Vulkan RT-capable display for RTGI metrics.",
		}
		_write_json("%s/%s_metrics.json" % [_output_dir, base_name], skipped)
		print("RTGI quality metrics skipped: %s" % JSON.stringify(skipped))
		_abort_run = true
		get_tree().quit(0)
		return []
	var viewport_texture := get_viewport().get_texture()
	var final_image: Image = null
	if viewport_texture != null:
		final_image = viewport_texture.get_image()
	if final_image == null:
		var skipped := {
			"skipped": true,
			"reason": "Viewport texture is unavailable. Run without --headless on a Vulkan RT-capable display for RTGI metrics.",
		}
		_write_json("%s/%s_metrics.json" % [_output_dir, base_name], skipped)
		print("RTGI quality metrics skipped: %s" % JSON.stringify(skipped))
		_abort_run = true
		get_tree().quit(0)
		return []
	final_image.convert(Image.FORMAT_RGBA8)
	var png_path := "%s/%s_%s.png" % [_output_dir, base_name, _debug_view]
	var metrics_path := "%s/%s_metrics.json" % [_output_dir, base_name]
	var png_error := final_image.save_png(png_path)
	if png_error != OK:
		push_error("Could not write RTGI quality PNG: %s" % png_path)
		_abort_run = true
		get_tree().quit(2)
		return []

	var metrics := _measure_image(final_image)
	if _scene_mode == "cornell":
		metrics.merge(_measure_cornell_image(final_image), true)
		if _cornell_compare:
			metrics.merge(_compare_cornell_reference(final_image, base_name), true)
	elif _scene_mode == "sponza":
		metrics.merge(_measure_sponza_image(final_image), true)
	elif _scene_mode == "cornell_box":
		metrics.merge(_measure_cornell_box_image(final_image), true)
	elif _scene_mode == "reflective_pool":
		metrics.merge(_measure_reflective_pool_image(final_image), true)
	elif _scene_mode == "fog_corridor":
		metrics.merge(_measure_fog_corridor_image(final_image), true)
	elif _scene_mode == "light_grid":
		metrics.merge(await _measure_light_grid(final_image, base_name), true)
	elif _scene_mode == "sun_penumbra_ramp":
		metrics.merge(_measure_sun_penumbra_ramp(final_image, base_name), true)
	elif _is_coexistence_scene():
		metrics.merge(await _measure_coexistence_image(final_image, base_name), true)
	metrics["denoise_strength"] = _denoise_strength
	metrics["history_weight"] = _history_weight
	metrics["firefly_suppression"] = _firefly_suppression
	metrics["detail_preservation"] = _detail_preservation
	metrics["split_signals"] = _split_signals
	metrics["specular_history_weight"] = _specular_history_weight
	metrics["specular_spatial_strength"] = _specular_spatial_strength
	var active_denoiser := int(_environment.rtgi_denoiser) if _environment != null else Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL
	metrics["rtgi_denoiser"] = active_denoiser
	metrics["rtgi_denoiser_path"] = _rtgi_denoiser_path_name(active_denoiser)
	metrics["ray_firefly_suppression"] = _ray_firefly_suppression
	metrics["ray_max_radiance"] = _ray_max_radiance
	metrics["warmup_frames"] = _warmup_frames
	metrics["settle_frames_override"] = _settle_frames_override
	metrics["settle_frames_used"] = _settle_frames_used
	metrics["sparkle_frames"] = _sparkle_frames
	metrics["convergence_frames"] = _convergence_frames
	metrics["gate_profile"] = _gate_profile
	metrics["debug_view"] = _debug_view
	metrics["camera_pan"] = _camera_pan
	metrics["specular_object_motion"] = _specular_object_motion
	metrics["scene"] = _scene_mode
	metrics["sponza_asset_loaded"] = _sponza_asset_loaded
	metrics["analytic_light_sampling_enabled"] = _analytic_light_sampling
	metrics["explicit_emissive_sampling_enabled"] = _explicit_emissive_sampling
	metrics["diffuse_radiance_cache_enabled"] = _diffuse_cache
	var render_diagnostics := _collect_rtgi_render_diagnostics(get_viewport())
	metrics["render_diagnostics"] = render_diagnostics
	metrics.merge(render_diagnostics, true)
	# Per-run performance readings and the effective applied override values, so the
	# metrics file is self-describing for sweep comparisons.
	metrics.merge(_collect_perf_metrics(), true)
	metrics.merge(_collect_applied_override_metrics(), true)
	metrics.merge(_rtgi_diffuse_cache_budget_metrics(Vector2i(final_image.get_width(), final_image.get_height())), true)
	if _sparkle_frames > 1 and DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_temporal_sparkle(base_name), true)
	if not _fast_iteration and _convergence_frames > 1 and DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_convergence_curve(base_name), true)
	# The per-view signal sweep re-renders and pixel-scans dozens of debug views
	# (minutes of script time); it is diagnostic-only, none of its outputs feed
	# the gate thresholds, so it rides the same opt-in as the debug captures.
	if not _fast_iteration and _capture_all_debug_views and DisplayServer.get_name().to_lower() != "headless":
		metrics.merge(await _measure_signal_debug_views(), true)
		metrics["rtgi_instability_attribution"] = _source_attribution_summary(metrics)

	# The energy-scaling gate spans multiple runs sharing one output dir
	# (FOGPAR-style), so it has no JSON thresholds; it only arms when
	# --rtgi-energy was given on the cornell_box scene.
	if _scene_mode == "cornell_box" and not is_nan(_rtgi_energy_override):
		_measure_energy_scaling(metrics, _config_mode_label(base_suffix),
				Vector2i(final_image.get_width(), final_image.get_height()))
	# Informational samples-per-pixel line: pairs the requested and read-back
	# spp with the temporal sparkle of this config, so an spp sweep shows at a
	# glance whether extra samples changed the per-frame noise at all.
	if _rtgi_samples_per_pixel_override > 0:
		var spp_effective := int(_environment.rtgi_samples_per_pixel) if _environment != null else -1
		print("SPP scene=%s mode=%s spp_requested=%d spp_effective=%d sparkle_max=%.6f sparkle_avg=%.6f" % [
				_scene_mode, _config_mode_label(base_suffix), _rtgi_samples_per_pixel_override, spp_effective,
				float(metrics.get("temporal_sparkle_per_megapixel_max", 0.0)),
				float(metrics.get("temporal_sparkle_per_megapixel_avg", 0.0))])

	var expected := _expected_metrics_for_scene(_load_expected_metrics())
	# A non-unit --rtgi-energy run is gated by the ENERGY verdict alone: the JSON
	# thresholds are calibrated at energy 1.0, so a scaled run trips the sparkle and
	# firefly gates spuriously. Skipping the comparison leaves expected empty, which
	# records expected_thresholds_applied = false in this config's metrics below.
	if not is_nan(_rtgi_energy_override) and not is_equal_approx(_rtgi_energy_override, 1.0):
		expected = {}
	var failures := _compare_metrics(metrics, expected)
	# The fog-corridor gate is the FOGPAR verdict (it spans multiple runs, so it has
	# no JSON thresholds); route it into the same failure list so a FAIL verdict
	# reaches the exit code exactly like the threshold gates. The --rtgi-mode=off
	# reference run records the baseline and sets no verdict key, so it stays green.
	if str(metrics.get("fog_corridor_verdict", "")) == "FAIL":
		failures.append("fog_corridor_fogpar_verdict FAIL (max_rel_err=%.6f)" % float(metrics.get("fog_corridor_max_rel_err", 1.0)))
	# The ENERGY verdict routes into the exit code the same way as FOGPAR.
	if str(metrics.get("rtgi_energy_verdict", "")) == "FAIL":
		if metrics.get("rtgi_energy_reference_missing", false):
			failures.append("rtgi_energy_scaling_verdict FAIL (reference missing, run --rtgi-energy=1.0 into this output dir first)")
		elif metrics.get("rtgi_energy_reference_stale", false):
			failures.append("rtgi_energy_scaling_verdict FAIL (stale reference, re-record with --rtgi-energy=1.0)")
		else:
			failures.append("rtgi_energy_scaling_verdict FAIL (occluded_ratio=%.6f expected=%.6f)" % [
					float(metrics.get("rtgi_energy_occluded_ratio", 0.0)), _rtgi_energy_override])
	metrics["expected_thresholds_applied"] = not expected.is_empty()
	metrics["passed"] = failures.is_empty()
	metrics["failures"] = failures
	_write_json(metrics_path, metrics)

	if _write_reference:
		_write_json(EXPECTED_METRICS_PATH, _reference_from_metrics(metrics))

	if _capture_all_debug_views:
		await _capture_debug_views(base_name)
	if _capture_comparison:
		await _capture_comparison_grid(base_name)

	print("RTGI quality metrics: %s" % JSON.stringify(metrics))
	return failures


# The mode label used in per-config protocol lines and cross-run reference file
# names: the multi-config suffix when present, else the single-mode override,
# else "scene" for runs that keep the scene-authored mode.
func _config_mode_label(base_suffix: String) -> String:
	if not base_suffix.is_empty():
		return base_suffix.trim_prefix("_")
	if not _rtgi_mode_override.is_empty():
		return _rtgi_mode_override
	return "scene"


func _animate_camera(frame: int) -> void:
	var t: float = float(frame) / maxf(float(_warmup_frames - 1), 1.0)
	if _scene_mode == "cornell":
		_camera.position = _cornell_point(lerpf(268.0, 288.0, t), 273.0, -800.0)
		_camera.look_at(_cornell_point(278.0, 273.0, 0.0), Vector3.UP)
		return
	if _scene_mode == "sponza":
		_camera.position = Vector3(lerpf(-12.6, -11.4, t), 3.4, lerpf(-0.25, 0.25, t))
		_camera.look_at(Vector3(2.0, 3.0, 0.0), Vector3.UP)
		return
	if _scene_mode == "specular_stability":
		var specular_t := float(frame) / maxf(float(_warmup_frames + maxi(_sparkle_frames, 1) - 1), 1.0)
		_camera.position = Vector3(lerpf(-0.42, 0.48, specular_t), 1.38, lerpf(1.48, 1.72, specular_t))
		_camera.look_at(Vector3(lerpf(-0.28, 0.18, specular_t), 1.04, -4.10), Vector3.UP)
		return
	_camera.position.x = lerpf(-0.18, 0.32, t)
	_camera.look_at(Vector3(-0.05, 1.15, -2.5), Vector3.UP)


func _animate_specular_objects(frame: int) -> void:
	if _is_packed_test_scene():
		_advance_packed_scene_animation(frame)
		return
	if _scene_mode != "specular_stability" or not _specular_object_motion:
		return
	var t := float(frame) * 0.045
	for i in range(_specular_motion_nodes.size()):
		var node := _specular_motion_nodes[i]
		if not is_instance_valid(node):
			continue
		if i == 0:
			node.position = Vector3(-1.45 + sin(t) * 0.36, 0.62 + sin(t * 1.7) * 0.045, -3.25 + cos(t * 0.7) * 0.22)
			node.rotation_degrees.y = fposmod(float(frame) * 2.1, 360.0)
		else:
			node.position = Vector3(1.35 + cos(t * 0.8) * 0.22, 0.50, -3.85 + sin(t * 1.1) * 0.28)
			node.rotation_degrees = Vector3(fposmod(float(frame) * 1.3, 360.0), fposmod(float(frame) * 2.7, 360.0), 0.0)


# Steps a committed test scene's frame-counter animation. The specular_motion
# and reflective_pool roots expose advance_to_frame() so the harness drives them
# in lock-step with the warmup/capture loop, keeping captures deterministic.
func _advance_packed_scene_animation(frame: int) -> void:
	if _packed_scene_root != null and _packed_scene_root.has_method("advance_to_frame"):
		_packed_scene_root.advance_to_frame(frame)


func _capture_debug_views(base_name: String) -> void:
	var views := ["beauty", "noisy", "raw_radiance", "diffuse_noisy", "specular_noisy", "diffuse_final", "specular_final", "specular_guide", "specular_reflection_direction", "specular_reflected_hit_distance", "specular_reflected_hit_normal", "direct_light_regime", "specular_roughness_bucket", "specular_history_length", "specular_rejection", "normal_roughness", "normal_deviation", "viewz_hitdist", "motion_vectors", "signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "source_candidate", "source_history", "source_temporal_delta", "source_rejection", "secondary_cache_source", "secondary_cache_rejection", "secondary_cache_surface", "surface_feedback", "surface_key", "cache_raw_diffuse", "cache_filtered_diffuse", "cache_hit_confidence", "cache_age", "cache_rejection", "strc_radiance", "strc_confidence", "strc_updates", "strc_visibility", "strc_age", "strc_variance", "strc_rejection", "variance", "history_length", "rejection", "final", "denoised_radiance", "reconstructed_radiance", "reconstruction_reactivity", "reconstruction_signal_confidence", "reconstruction_guide_mismatch", "reconstruction_fill_source"]
	for view in views:
		if view.begins_with("cache_") and not _cache_debug_available():
			continue
		_apply_debug_view(view)
		await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		image.convert(Image.FORMAT_RGBA8)
		image.save_png("%s/%s_%s.png" % [_output_dir, base_name, view])
	_apply_debug_view(_debug_view)


func _collect_rtgi_render_diagnostics(viewport: Viewport) -> Dictionary:
	var rid := viewport.get_viewport_rid()
	var guide_quality := RenderingServer.viewport_get_render_info(rid, RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTION_GUIDE_QUALITY)
	return {
		"rtgi_reconstructed_copy_count": RenderingServer.viewport_get_render_info(rid, RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTED_COPY_COUNT),
		"rtgi_raw_fallback_copy_count": RenderingServer.viewport_get_render_info(rid, RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_RTGI_RAW_FALLBACK_COPY_COUNT),
		"rtgi_reconstruction_guide_quality": guide_quality,
		"rtgi_reconstruction_guide_quality_label": _rtgi_reconstruction_guide_quality_label(guide_quality),
	}


# Records one GPU/CPU measured-frame-time reading. The measured render time reports
# 0.0 until a few frames have rendered, so zero readings are dropped to keep the
# steady-state average honest.
func _sample_frame_perf() -> void:
	var rid := get_viewport().get_viewport_rid()
	var gpu_msec := RenderingServer.viewport_get_measured_render_time_gpu(rid)
	var cpu_msec := RenderingServer.viewport_get_measured_render_time_cpu(rid)
	if gpu_msec > 0.0:
		_perf_gpu_samples.append(gpu_msec)
	if cpu_msec > 0.0:
		_perf_cpu_samples.append(cpu_msec)


# Assembles the per-run performance metrics: the GPU/CPU frame-time samples gathered
# over the warmup tail plus the video-memory and draw-call readings taken at capture
# time. Min GPU time is the most-representative uncontended frame; avg smooths jitter.
func _collect_perf_metrics() -> Dictionary:
	var perf := {}
	if not _perf_gpu_samples.is_empty():
		var gpu_sum := 0.0
		var gpu_min: float = _perf_gpu_samples[0]
		for sample in _perf_gpu_samples:
			gpu_sum += sample
			gpu_min = minf(gpu_min, sample)
		perf["perf_gpu_frame_msec_avg"] = gpu_sum / float(_perf_gpu_samples.size())
		perf["perf_gpu_frame_msec_min"] = gpu_min
	if not _perf_cpu_samples.is_empty():
		var cpu_sum := 0.0
		for sample in _perf_cpu_samples:
			cpu_sum += sample
		perf["perf_cpu_frame_msec_avg"] = cpu_sum / float(_perf_cpu_samples.size())
	perf["perf_video_mem_used_bytes"] = int(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED))
	perf["perf_draw_calls"] = int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
	return perf


# Reports the effective per-run override values so each metrics file is
# self-describing. Mode/denoiser read back the applied enum on _environment; the
# viewport scaling values read back from the live root viewport.
func _collect_applied_override_metrics() -> Dictionary:
	var applied := {}
	if _environment != null:
		applied["applied_rtgi_mode"] = int(_environment.rtgi_mode)
		applied["applied_rtgi_denoiser"] = int(_environment.rtgi_denoiser)
		applied["applied_rtgi_resolution_scale"] = _environment.rtgi_resolution_scale
		applied["applied_rtgi_energy"] = _environment.rtgi_energy
		applied["applied_rtgi_samples_per_pixel"] = int(_environment.rtgi_samples_per_pixel)
		applied["applied_rtgi_quality_preset"] = int(_environment.rtgi_quality_preset)
	var viewport := get_viewport()
	if viewport != null:
		applied["applied_upscaler_scaling_3d_mode"] = int(viewport.scaling_3d_mode)
		applied["applied_use_taa"] = viewport.use_taa
		applied["applied_scaling_3d_scale"] = viewport.scaling_3d_scale
	# Echo the raw flag selections so a sweep run is filterable by intent.
	applied["override_rtgi_mode"] = _rtgi_mode_override
	applied["override_rtgi_denoiser"] = _rtgi_denoiser_override
	applied["override_rtgi_resolution_scale"] = _rtgi_resolution_scale_override if not is_nan(_rtgi_resolution_scale_override) else null
	applied["override_rtgi_energy"] = _rtgi_energy_override if not is_nan(_rtgi_energy_override) else null
	applied["override_rtgi_samples_per_pixel"] = _rtgi_samples_per_pixel_override if _rtgi_samples_per_pixel_override > 0 else null
	applied["override_rtgi_ris_candidates"] = _rtgi_ris_candidates_override if _rtgi_ris_candidates_override > 0 else null
	# Effective read-back: the renderer resolves the active tier live, so echo what the
	# Project Setting now holds (all three tiers were set identically by the override). At
	# the default (no flag) this is the registered tier default (16 at landing).
	applied["applied_rtgi_ris_candidates"] = int(ProjectSettings.get_setting("rendering/rtgi/direct_light/balanced/ris_candidates", 16))
	applied["override_upscaler"] = _upscaler_override
	applied["override_scale_3d"] = _scale_3d_override if not is_nan(_scale_3d_override) else null
	return applied


func _rtgi_reconstruction_guide_quality_label(quality: int) -> String:
	match quality:
		0:
			return "none"
		1:
			return "depth"
		2:
			return "depth + normal/roughness"
		3:
			return "depth + normal/roughness + material guides"
		_:
			return "unknown"


func _measure_signal_debug_views() -> Dictionary:
	var result := {}
	var previous_view := _debug_view
	var signal_views := ["signal_direct", "signal_emissive", "signal_indirect", "signal_sky", "signal_confidence", "source_candidate", "source_history", "source_temporal_delta", "source_rejection", "secondary_cache_source", "secondary_cache_rejection", "secondary_cache_surface", "surface_feedback", "surface_key", "cache_raw_diffuse", "cache_filtered_diffuse", "cache_hit_confidence", "cache_age", "cache_rejection", "strc_radiance", "strc_confidence", "strc_updates", "strc_visibility", "strc_age", "strc_variance", "strc_rejection", "noisy", "raw_radiance", "final", "denoised_radiance", "reconstructed_radiance", "reconstruction_reactivity", "reconstruction_signal_confidence", "reconstruction_guide_mismatch", "reconstruction_fill_source"]
	for view in signal_views:
		if view.begins_with("cache_") and not _cache_debug_available():
			continue
		_apply_debug_view(view)
		await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		if image == null:
			continue
		image.convert(Image.FORMAT_RGBA8)
		var view_metrics := _measure_image(image)
		for key in view_metrics.keys():
			result["rtgi_%s_%s" % [view, key]] = view_metrics[key]
		if view.begins_with("cache_"):
			var channel_means := _measure_channel_means(image)
			for key in channel_means.keys():
				result["rtgi_%s_%s" % [view, key]] = channel_means[key]
		if view == "cache_hit_confidence":
			result.merge(_measure_cache_hit_diagnostic(image, "rtgi_cache"), true)
		elif view == "cache_rejection":
			result.merge(_measure_cache_rejection_diagnostic(image, "rtgi_cache"), true)
		elif view == "source_candidate":
			result.merge(_measure_source_candidate_diagnostic(image), true)
		elif view == "source_history":
			result.merge(_measure_source_history_diagnostic(image), true)
		elif view == "source_temporal_delta":
			result.merge(_measure_source_temporal_delta_diagnostic(image), true)
		elif view == "source_rejection":
			result.merge(_measure_source_rejection_diagnostic(image), true)
		elif view == "secondary_cache_source":
			result.merge(_measure_secondary_cache_source_diagnostic(image), true)
		elif view == "secondary_cache_rejection":
			result.merge(_measure_secondary_cache_rejection_diagnostic(image), true)
		elif view == "secondary_cache_surface":
			result.merge(_measure_surface_cache_query_diagnostic(image), true)
		elif view == "surface_feedback":
			result.merge(_measure_surface_feedback_diagnostic(image), true)
		elif view == "surface_key":
			result.merge(_measure_surface_key_diagnostic(image), true)
	_apply_debug_view(previous_view)
	return result


func _cache_debug_available() -> bool:
	return _diffuse_cache and _split_signals and _denoise_strength > 0.001 and _history_weight > 0.001


func _rtgi_diffuse_cache_budget_metrics(output_size: Vector2i) -> Dictionary:
	var budget := clampi(_diffuse_cache_max_entries, 4096, 4194304)
	var output_pixels := maxi(output_size.x * output_size.y, 1)
	var cache_size := output_size
	if output_pixels > budget:
		var scale := sqrt(float(budget) / float(output_pixels))
		cache_size = Vector2i(maxi(1, int(floor(float(output_size.x) * scale))), maxi(1, int(floor(float(output_size.y) * scale))))
		while cache_size.x * cache_size.y > budget:
			if cache_size.x >= cache_size.y and cache_size.x > 1:
				cache_size.x -= 1
			elif cache_size.y > 1:
				cache_size.y -= 1
			else:
				break
	var cache_pixels := maxi(cache_size.x * cache_size.y, 1)
	var persistent_history_bytes := cache_pixels * 6 * 8
	var full_resolution_bytes := output_pixels * ((2 * 8) + 4 + 1 + 1)
	return {
		"diffuse_radiance_cache_budget_entries": budget,
		"diffuse_radiance_cache_width": cache_size.x,
		"diffuse_radiance_cache_height": cache_size.y,
		"diffuse_radiance_cache_entries": cache_pixels,
		"diffuse_radiance_cache_persistent_history_bytes": persistent_history_bytes,
		"diffuse_radiance_cache_fullres_output_and_diagnostic_bytes": full_resolution_bytes,
		"diffuse_radiance_cache_total_effect_bytes": persistent_history_bytes + full_resolution_bytes,
	}


func _measure_channel_means(image: Image) -> Dictionary:
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
		"mean_r": sum_r / float(count),
		"mean_g": sum_g / float(count),
		"mean_b": sum_b / float(count),
		"mean_a": sum_a / float(count),
	}


func _measure_cache_hit_diagnostic(image: Image, prefix: String) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var metrics := _measure_cache_hit_region(image, prefix, 0, 0, width, height)
	metrics.merge(_measure_cache_hit_region(image, "%s_dark_roi" % prefix, int(width * 0.62), int(height * 0.18), int(width * 0.94), int(height * 0.84)), true)
	metrics.merge(_measure_cache_hit_region(image, "%s_detail_roi" % prefix, int(width * 0.07), int(height * 0.30), int(width * 0.42), int(height * 0.76)), true)
	metrics.merge(_measure_cache_hit_region(image, "%s_specular_roi" % prefix, int(width * 0.48), int(height * 0.46), int(width * 0.78), int(height * 0.75)), true)
	return metrics


func _measure_cache_hit_region(image: Image, prefix: String, x0: int, y0: int, x1: int, y1: int) -> Dictionary:
	x0 = clampi(x0, 0, image.get_width() - 1)
	y0 = clampi(y0, 0, image.get_height() - 1)
	x1 = clampi(x1, x0 + 1, image.get_width())
	y1 = clampi(y1, y0 + 1, image.get_height())
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


func _measure_source_candidate_diagnostic(image: Image) -> Dictionary:
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


func _measure_source_history_diagnostic(image: Image) -> Dictionary:
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


func _measure_source_temporal_delta_diagnostic(image: Image) -> Dictionary:
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
		"rtgi_source_direct_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["direct"], count),
		"rtgi_source_direct_candidate_temporal_delta_p95": direct_accepted[direct_p95_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_candidate_temporal_delta_p99": direct_accepted[direct_p99_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_candidate_rejected_temporal_delta_p99": direct_rejected[direct_rejected_p99_idx] if not direct_rejected.is_empty() else 0.0,
		"rtgi_source_direct_reuse_temporal_delta_p95": direct_accepted[direct_p95_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_direct_reuse_temporal_delta_p99": direct_accepted[direct_p99_idx] if not direct_accepted.is_empty() else 0.0,
		"rtgi_source_emissive_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["emissive"], count),
		"rtgi_source_indirect_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["indirect"], count),
		"rtgi_source_sky_delta_fireflies_per_mp": _per_megapixel(class_delta_counts["sky"], count),
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


func _measure_source_rejection_diagnostic(image: Image) -> Dictionary:
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


func _measure_secondary_cache_source_diagnostic(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var queried := 0
	var accepted := 0
	var receiver := 0
	var strc := 0
	var base_spg := 0
	var refined_spg := 0
	var surface := 0
	var screen_trace := 0
	var secondary_hit := 0
	var screen_trace_accepted := 0
	var screen_trace_base_spg := 0
	var screen_trace_refined_spg := 0
	var screen_trace_surface := 0
	var secondary_hit_accepted := 0
	var secondary_hit_base_spg := 0
	var secondary_hit_refined_spg := 0
	var secondary_hit_surface := 0
	var weight_sum := 0.0
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.b <= 0.0 and c.r <= 0.0:
				continue
			queried += 1
			if c.b >= 0.5:
				screen_trace += 1
			else:
				secondary_hit += 1
			var source := clampi(int(round(c.r * 5.0)), 0, 5)
			if source <= 0:
				continue
			accepted += 1
			if c.b >= 0.5:
				screen_trace_accepted += 1
			else:
				secondary_hit_accepted += 1
			weight_sum += c.g
			match source:
				1:
					receiver += 1
				2:
					strc += 1
				3:
					base_spg += 1
					if c.b >= 0.5:
						screen_trace_base_spg += 1
					else:
						secondary_hit_base_spg += 1
				4:
					refined_spg += 1
					if c.b >= 0.5:
						screen_trace_refined_spg += 1
					else:
						secondary_hit_refined_spg += 1
				5:
					surface += 1
					if c.b >= 0.5:
						screen_trace_surface += 1
					else:
						secondary_hit_surface += 1
	var queried_denom := maxf(float(queried), 1.0)
	var accepted_denom := maxf(float(accepted), 1.0)
	var screen_trace_accepted_denom := maxf(float(screen_trace_accepted), 1.0)
	var secondary_hit_accepted_denom := maxf(float(secondary_hit_accepted), 1.0)
	return {
		"rtgi_secondary_cache_query_rate": float(queried) / float(count),
		"rtgi_secondary_cache_accept_rate": float(accepted) / queried_denom,
		"rtgi_secondary_cache_no_source_rate": float(queried - accepted) / queried_denom,
		"rtgi_secondary_cache_receiver_fraction": float(receiver) / accepted_denom,
		"rtgi_secondary_cache_strc_fraction": float(strc) / accepted_denom,
		"rtgi_secondary_cache_base_spg_fraction": float(base_spg) / accepted_denom,
		"rtgi_secondary_cache_refined_spg_fraction": float(refined_spg) / accepted_denom,
		"rtgi_secondary_cache_spg_fraction": float(base_spg + refined_spg) / accepted_denom,
		"rtgi_secondary_cache_surface_fraction": float(surface) / accepted_denom,
		"rtgi_secondary_cache_screen_trace_query_fraction": float(screen_trace) / queried_denom,
		"rtgi_secondary_cache_secondary_hit_query_fraction": float(secondary_hit) / queried_denom,
		"rtgi_secondary_cache_screen_trace_accept_fraction": float(screen_trace_accepted) / accepted_denom,
		"rtgi_secondary_cache_screen_trace_spg_fraction": float(screen_trace_base_spg + screen_trace_refined_spg) / screen_trace_accepted_denom,
		"rtgi_secondary_cache_screen_trace_surface_fraction": float(screen_trace_surface) / screen_trace_accepted_denom,
		"rtgi_secondary_cache_secondary_hit_accept_fraction": float(secondary_hit_accepted) / accepted_denom,
		"rtgi_secondary_cache_secondary_hit_spg_fraction": float(secondary_hit_base_spg + secondary_hit_refined_spg) / secondary_hit_accepted_denom,
		"rtgi_secondary_cache_secondary_hit_surface_fraction": float(secondary_hit_surface) / secondary_hit_accepted_denom,
		"rtgi_secondary_cache_weight_mean": weight_sum / accepted_denom,
	}


func _measure_secondary_cache_rejection_diagnostic(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var rejected := 0
	var screen_trace := 0
	var secondary_hit := 0
	var detail_sum := 0.0
	var buckets := []
	for i in range(10):
		buckets.append(0)
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			var reason := clampi(int(round(c.r * 9.0)), 0, 9)
			if reason <= 0:
				continue
			rejected += 1
			buckets[reason] += 1
			detail_sum += c.b
			if c.g >= 0.5:
				screen_trace += 1
			else:
				secondary_hit += 1
	var rejected_denom := maxf(float(rejected), 1.0)
	return {
		"rtgi_secondary_cache_rejection_rate": float(rejected) / float(count),
		"rtgi_secondary_cache_rejection_ineligible_fraction": float(buckets[1]) / rejected_denom,
		"rtgi_secondary_cache_rejection_no_source_fraction": float(buckets[2]) / rejected_denom,
		"rtgi_secondary_cache_rejection_receiver_weak_fraction": float(buckets[3]) / rejected_denom,
		"rtgi_secondary_cache_rejection_refined_spg_weak_fraction": float(buckets[4]) / rejected_denom,
		"rtgi_secondary_cache_rejection_base_spg_weak_fraction": float(buckets[5]) / rejected_denom,
		"rtgi_secondary_cache_rejection_strc_weak_fraction": float(buckets[6]) / rejected_denom,
		"rtgi_secondary_cache_rejection_screen_no_hit_fraction": float(buckets[7]) / rejected_denom,
		"rtgi_secondary_cache_rejection_screen_low_weight_fraction": float(buckets[8]) / rejected_denom,
		"rtgi_secondary_cache_rejection_surface_weak_fraction": float(buckets[9]) / rejected_denom,
		"rtgi_secondary_cache_rejection_screen_trace_fraction": float(screen_trace) / rejected_denom,
		"rtgi_secondary_cache_rejection_secondary_hit_fraction": float(secondary_hit) / rejected_denom,
		"rtgi_secondary_cache_rejection_detail_mean": detail_sum / rejected_denom,
	}


func _measure_surface_cache_query_diagnostic(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var queried := 0
	var screen_trace := 0
	var secondary_hit := 0
	var detail_sum := 0.0
	var source_none := 0
	var source_receiver := 0
	var source_base_spg := 0
	var source_refined_spg := 0
	var source_visible_current := 0
	var source_direct := 0
	var source_emissive := 0
	var source_sky := 0
	var source_strc := 0
	var source_mixed := 0
	var buckets := []
	for i in range(15):
		buckets.append(0)
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			var reason := clampi(int(round(c.r * 14.0)), 0, 14)
			if reason <= 0:
				continue
			queried += 1
			buckets[reason] += 1
			detail_sum += c.a
			var source := clampi(int(round(c.g * 9.0)), 0, 9)
			match source:
				0:
					source_none += 1
				1:
					source_receiver += 1
				2:
					source_base_spg += 1
				3:
					source_refined_spg += 1
				4:
					source_visible_current += 1
				5:
					source_direct += 1
				6:
					source_emissive += 1
				7:
					source_sky += 1
				8:
					source_strc += 1
				9:
					source_mixed += 1
			if c.b >= 0.5:
				screen_trace += 1
			else:
				secondary_hit += 1
	var queried_denom := maxf(float(queried), 1.0)
	return {
		"rtgi_surface_cache_query_rate": float(queried) / float(count),
		"rtgi_surface_cache_query_accept_fraction": float(buckets[1]) / queried_denom,
		"rtgi_surface_cache_query_early_source_fraction": float(buckets[2]) / queried_denom,
		"rtgi_surface_cache_query_ineligible_fraction": float(buckets[3]) / queried_denom,
		"rtgi_surface_cache_query_no_key_fraction": float(buckets[4]) / queried_denom,
		"rtgi_surface_cache_query_dynamic_ineligible_fraction": float(buckets[5]) / queried_denom,
		"rtgi_surface_cache_query_no_page_fraction": float(buckets[6]) / queried_denom,
		"rtgi_surface_cache_query_id_mismatch_fraction": float(buckets[7]) / queried_denom,
		"rtgi_surface_cache_query_low_confidence_fraction": float(buckets[8]) / queried_denom,
		"rtgi_surface_cache_query_low_support_fraction": float(buckets[9]) / queried_denom,
		"rtgi_surface_cache_query_low_variance_fraction": float(buckets[10]) / queried_denom,
		"rtgi_surface_cache_query_stale_fraction": float(buckets[11]) / queried_denom,
		"rtgi_surface_cache_query_normal_mismatch_fraction": float(buckets[12]) / queried_denom,
		"rtgi_surface_cache_query_weak_radiance_fraction": float(buckets[13]) / queried_denom,
		"rtgi_surface_cache_query_weak_quality_fraction": float(buckets[14]) / queried_denom,
		"rtgi_surface_cache_query_screen_trace_fraction": float(screen_trace) / queried_denom,
		"rtgi_surface_cache_query_secondary_hit_fraction": float(secondary_hit) / queried_denom,
		"rtgi_surface_cache_query_detail_mean": detail_sum / queried_denom,
		"rtgi_surface_cache_query_source_none_fraction": float(source_none) / queried_denom,
		"rtgi_surface_cache_query_source_receiver_fraction": float(source_receiver) / queried_denom,
		"rtgi_surface_cache_query_source_base_spg_fraction": float(source_base_spg) / queried_denom,
		"rtgi_surface_cache_query_source_refined_spg_fraction": float(source_refined_spg) / queried_denom,
		"rtgi_surface_cache_query_source_visible_current_fraction": float(source_visible_current) / queried_denom,
		"rtgi_surface_cache_query_source_direct_fraction": float(source_direct) / queried_denom,
		"rtgi_surface_cache_query_source_emissive_fraction": float(source_emissive) / queried_denom,
		"rtgi_surface_cache_query_source_sky_fraction": float(source_sky) / queried_denom,
		"rtgi_surface_cache_query_source_strc_fraction": float(source_strc) / queried_denom,
		"rtgi_surface_cache_query_source_mixed_fraction": float(source_mixed) / queried_denom,
	}


func _measure_surface_feedback_diagnostic(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var active := 0
	var selected := 0
	var invalid := 0
	var no_radiance := 0
	var budget_starved := 0
	var collision := 0
	var atomic_skipped := 0
	var low_confidence := 0
	var low_quality := 0
	var stale := 0
	var dynamic := 0
	var source_none := 0
	var source_receiver := 0
	var source_base_spg := 0
	var source_refined_spg := 0
	var source_visible_current := 0
	var source_direct := 0
	var source_emissive := 0
	var source_sky := 0
	var source_strc := 0
	var source_mixed := 0
	var selected_source_visible_current := 0
	var selected_source_direct := 0
	var selected_source_emissive := 0
	var selected_source_sky := 0
	var selected_source_strc := 0
	var selected_source_mixed := 0
	var detail_sum := 0.0
	var quality_sum := 0.0
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			var status := clampi(int(round(c.r * 10.0)), 0, 10)
			if status <= 0:
				continue
			active += 1
			detail_sum += c.a
			quality_sum += c.b
			var source := clampi(int(round(c.g * 9.0)), 0, 9)
			match source:
				0:
					source_none += 1
				1:
					source_receiver += 1
				2:
					source_base_spg += 1
				3:
					source_refined_spg += 1
				4:
					source_visible_current += 1
				5:
					source_direct += 1
				6:
					source_emissive += 1
				7:
					source_sky += 1
				8:
					source_strc += 1
				9:
					source_mixed += 1
			match status:
				1:
					selected += 1
				2:
					invalid += 1
				3:
					no_radiance += 1
				4:
					budget_starved += 1
				5:
					collision += 1
				6:
					atomic_skipped += 1
				7:
					low_confidence += 1
				8:
					low_quality += 1
				9:
					stale += 1
					selected += 1
				10:
					dynamic += 1
			if status == 1 or status == 9:
				match source:
					4:
						selected_source_visible_current += 1
					5:
						selected_source_direct += 1
					6:
						selected_source_emissive += 1
					7:
						selected_source_sky += 1
					8:
						selected_source_strc += 1
					9:
						selected_source_mixed += 1
	var active_denom := maxf(float(active), 1.0)
	var selected_denom := maxf(float(selected), 1.0)
	return {
		"rtgi_surface_feedback_active_rate": float(active) / float(count),
		"rtgi_surface_feedback_selected_fraction": float(selected) / active_denom,
		"rtgi_surface_feedback_invalid_fraction": float(invalid) / active_denom,
		"rtgi_surface_feedback_no_radiance_fraction": float(no_radiance) / active_denom,
		"rtgi_surface_feedback_budget_starved_fraction": float(budget_starved) / active_denom,
		"rtgi_surface_feedback_collision_fraction": float(collision) / active_denom,
		"rtgi_surface_feedback_atomic_skipped_fraction": float(atomic_skipped) / active_denom,
		"rtgi_surface_feedback_low_confidence_fraction": float(low_confidence) / active_denom,
		"rtgi_surface_feedback_low_quality_fraction": float(low_quality) / active_denom,
		"rtgi_surface_feedback_stale_refresh_fraction": float(stale) / active_denom,
		"rtgi_surface_feedback_dynamic_ineligible_fraction": float(dynamic) / active_denom,
		"rtgi_surface_feedback_source_none_fraction": float(source_none) / active_denom,
		"rtgi_surface_feedback_source_receiver_fraction": float(source_receiver) / active_denom,
		"rtgi_surface_feedback_source_base_spg_fraction": float(source_base_spg) / active_denom,
		"rtgi_surface_feedback_source_refined_spg_fraction": float(source_refined_spg) / active_denom,
		"rtgi_surface_feedback_source_visible_current_fraction": float(source_visible_current) / active_denom,
		"rtgi_surface_feedback_source_direct_fraction": float(source_direct) / active_denom,
		"rtgi_surface_feedback_source_emissive_fraction": float(source_emissive) / active_denom,
		"rtgi_surface_feedback_source_sky_fraction": float(source_sky) / active_denom,
		"rtgi_surface_feedback_source_strc_fraction": float(source_strc) / active_denom,
		"rtgi_surface_feedback_source_mixed_fraction": float(source_mixed) / active_denom,
		"rtgi_surface_feedback_selected_source_visible_current_fraction": float(selected_source_visible_current) / selected_denom,
		"rtgi_surface_feedback_selected_source_direct_fraction": float(selected_source_direct) / selected_denom,
		"rtgi_surface_feedback_selected_source_emissive_fraction": float(selected_source_emissive) / selected_denom,
		"rtgi_surface_feedback_selected_source_sky_fraction": float(selected_source_sky) / selected_denom,
		"rtgi_surface_feedback_selected_source_strc_fraction": float(selected_source_strc) / selected_denom,
		"rtgi_surface_feedback_selected_source_mixed_fraction": float(selected_source_mixed) / selected_denom,
		"rtgi_surface_feedback_detail_mean": detail_sum / active_denom,
		"rtgi_surface_feedback_source_quality_mean": quality_sum / active_denom,
	}


func _measure_surface_key_diagnostic(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var count: int = max(width * height, 1)
	var covered := 0
	var static_eligible := 0
	var buckets := []
	for i in range(7):
		buckets.append(0)
	for y in range(height):
		for x in range(width):
			var c := image.get_pixel(x, y)
			if c.r > 0.5:
				covered += 1
			if c.b > 0.5:
				static_eligible += 1
			var reason := clampi(int(round(c.g * 6.0)), 0, 6)
			buckets[reason] += 1
	return {
		"rtgi_surface_key_coverage": float(covered) / float(count),
		"rtgi_surface_key_static_eligible_fraction": float(static_eligible) / float(count),
		"rtgi_surface_key_reason_valid_fraction": float(buckets[0]) / float(count),
		"rtgi_surface_key_reason_empty_fraction": float(buckets[1]) / float(count),
		"rtgi_surface_key_reason_raster_fraction": float(buckets[2]) / float(count),
		"rtgi_surface_key_reason_history_invalid_fraction": float(buckets[3]) / float(count),
		"rtgi_surface_key_reason_deformed_fraction": float(buckets[4]) / float(count),
		"rtgi_surface_key_reason_procedural_fraction": float(buckets[5]) / float(count),
		"rtgi_surface_key_reason_zero_key_fraction": float(buckets[6]) / float(count),
	}


func _measure_cache_rejection_diagnostic(image: Image, prefix: String) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var metrics := _measure_cache_rejection_region(image, prefix, 0, 0, width, height)
	metrics.merge(_measure_cache_rejection_region(image, "%s_dark_roi" % prefix, int(width * 0.62), int(height * 0.18), int(width * 0.94), int(height * 0.84)), true)
	metrics.merge(_measure_cache_rejection_region(image, "%s_detail_roi" % prefix, int(width * 0.07), int(height * 0.30), int(width * 0.42), int(height * 0.76)), true)
	metrics.merge(_measure_cache_rejection_region(image, "%s_specular_roi" % prefix, int(width * 0.48), int(height * 0.46), int(width * 0.78), int(height * 0.75)), true)
	return metrics


func _measure_cache_rejection_region(image: Image, prefix: String, x0: int, y0: int, x1: int, y1: int) -> Dictionary:
	x0 = clampi(x0, 0, image.get_width() - 1)
	y0 = clampi(y0, 0, image.get_height() - 1)
	x1 = clampi(x1, x0 + 1, image.get_width())
	y1 = clampi(y1, y0 + 1, image.get_height())
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


func _source_attribution_summary(metrics: Dictionary) -> String:
	var sources := {
		"analytic direct": _source_score(metrics, "signal_direct"),
		"visible/secondary emissive": _source_score(metrics, "signal_emissive"),
		"indirect/throughput": _source_score(metrics, "signal_indirect"),
		"sky/environment": _source_score(metrics, "signal_sky"),
		"clamp/weight risk": _source_score(metrics, "signal_confidence"),
		"final/post amplification": float(metrics.get("rtgi_final_visible_speckles_per_megapixel", 0.0)) + float(metrics.get("rtgi_final_luma_p99", 0.0)) * 120.0,
		"reconstruction guide mismatch": _source_score(metrics, "reconstruction_guide_mismatch"),
		"reconstruction disocclusion fill": _source_score(metrics, "reconstruction_fill_source"),
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


func _capture_comparison_grid(base_name: String) -> void:
	if _environment == null or DisplayServer.get_name().to_lower() == "headless":
		return

	var configs := [
		{
			"name": "random_emissive_discovery_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": false,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "analytic_direct_only_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": true,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "analytic_explicit_emissive_raw",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 5,
			"split_signals": _split_signals,
			"analytic_light_sampling": true,
			"explicit_emissive_sampling": true,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_asvfg_split_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": true,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_internal_signal_decomposition_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION,
			"max_bounces": 3,
			"split_signals": true,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_asvfg_single_beauty_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": false,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "reflections_only_asvfg_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_REFLECTIONS_RT_ONLY,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "reflections_only_internal_signal_decomposition_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_REFLECTIONS_RT_ONLY,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION,
			"max_bounces": 3,
			"split_signals": true,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_asvfg_strc_off_1spp",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
			"strc_enabled": false,
		},
		{
			"name": "vendor_denoiser_reactive",
			"enabled": true,
			"backend": Environment.RTGI_BACKEND_VULKAN_GENERIC,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_REACTIVE,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "strc_static_layers_only",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
			"strc_enabled": true,
			"strc_static_layers": 1,
			"strc_dynamic_layers": 0,
		},
		{
			"name": "strc_ignored_layers",
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_ASVFG_EXPERIMENTAL,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": _analytic_light_sampling,
			"explicit_emissive_sampling": _explicit_emissive_sampling,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
			"strc_enabled": true,
			"strc_static_layers": 0,
			"strc_dynamic_layers": 0,
		},
		{
			"name": "no_rtgi",
			"enabled": false,
			"mode": Environment.RTGI_MODE_REFLECTIONS_RT_ONLY,
			"spp": 1,
			"denoiser": Environment.RTGI_DENOISER_NONE,
			"max_bounces": 3,
			"split_signals": _split_signals,
			"analytic_light_sampling": false,
			"explicit_emissive_sampling": false,
			"ray_firefly_suppression": _ray_firefly_suppression,
			"ray_max_radiance": _ray_max_radiance,
		},
		{
			"name": "path_traced_reference_%dspp" % _reference_spp,
			"enabled": true,
			"mode": Environment.RTGI_MODE_FULL_PATH_TRACING,
			"spp": _reference_spp,
			"denoiser": Environment.RTGI_DENOISER_NONE,
				"max_bounces": 8,
				"split_signals": false,
				"analytic_light_sampling": true,
				"explicit_emissive_sampling": false,
				"ray_firefly_suppression": 0.0,
			"ray_max_radiance": 0.0,
		},
	]

	var captures: Array[Image] = []
	var manifest := {
		"base_name": base_name,
		"reference_spp": _reference_spp,
		"captures": [],
	}
	var comparison_warmup := mini(_warmup_frames, 45)
	for config in configs:
		_environment.rtgi_enabled = false
		await _wait_render_frame()
		_environment.rtgi_backend = config.get("backend", Environment.RTGI_BACKEND_VULKAN_GENERIC)
		_environment.rtgi_enabled = config["enabled"]
		_environment.rtgi_mode = config["mode"]
		_environment.rtgi_samples_per_pixel = config["spp"]
		_environment.rtgi_denoiser = config["denoiser"]
		_environment.rtgi_max_bounces = config["max_bounces"]
		_environment.rtgi_analytic_light_sampling_enabled = config["analytic_light_sampling"]
		_environment.rtgi_explicit_emissive_sampling_enabled = config["explicit_emissive_sampling"]
		_environment.rtgi_ray_firefly_suppression = config["ray_firefly_suppression"]
		_environment.rtgi_ray_max_radiance = config["ray_max_radiance"]
		_apply_debug_view("beauty")
		for frame in range(comparison_warmup):
			await _wait_render_frame()
		var image := get_viewport().get_texture().get_image()
		image.convert(Image.FORMAT_RGBA8)
		var path := "%s/%s_compare_%s.png" % [_output_dir, base_name, config["name"]]
		image.save_png(path)
		captures.append(image)
		var active_denoiser := int(_environment.rtgi_denoiser)
		manifest["captures"].append({
			"name": config["name"],
			"path": ProjectSettings.globalize_path(path),
			"rtgi_enabled": config["enabled"],
			"rtgi_backend": config.get("backend", Environment.RTGI_BACKEND_VULKAN_GENERIC),
			"rtgi_mode": config["mode"],
			"spp": config["spp"],
			"denoiser": config["denoiser"],
			"active_denoiser": active_denoiser,
			"active_denoiser_path": _rtgi_denoiser_path_name(active_denoiser),
			"max_bounces": config["max_bounces"],
			"split_signals": config["split_signals"],
			"analytic_light_sampling": config["analytic_light_sampling"],
			"explicit_emissive_sampling": config["explicit_emissive_sampling"],
			"ray_firefly_suppression": config["ray_firefly_suppression"],
			"strc_enabled": config.get("strc_enabled", _strc_enabled),
			"strc_static_layers": config.get("strc_static_layers", 1048575),
			"strc_dynamic_layers": config.get("strc_dynamic_layers", 1048575),
			"ray_max_radiance": config["ray_max_radiance"],
		})

	if captures.is_empty():
		return
	var tile_w := captures[0].get_width()
	var tile_h := captures[0].get_height()
	var cols := 2
	var rows := int(ceil(float(captures.size()) / float(cols)))
	var grid := Image.create_empty(tile_w * cols, tile_h * rows, false, Image.FORMAT_RGBA8)
	grid.fill(Color(0, 0, 0, 1))
	for i in range(captures.size()):
		var x := (i % cols) * tile_w
		var y := int(i / cols) * tile_h
		grid.blit_rect(captures[i], Rect2i(Vector2i.ZERO, Vector2i(tile_w, tile_h)), Vector2i(x, y))
	var grid_path := "%s/%s_comparison_grid.png" % [_output_dir, base_name]
	grid.save_png(grid_path)
	manifest["grid_path"] = ProjectSettings.globalize_path(grid_path)
	_write_json("%s/%s_comparison_manifest.json" % [_output_dir, base_name], manifest)


func _wait_render_frame() -> void:
	if DisplayServer.get_name() == "headless":
		await get_tree().process_frame
	else:
		await RenderingServer.frame_post_draw


func _apply_debug_view(view: String) -> void:
	if _environment != null:
		if view == "normal_deviation":
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_NORMAL_DEVIATION
		elif view == "specular_reflection_direction":
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_SPECULAR_REFLECTION_DIRECTION
		elif view == "specular_reflected_hit_distance":
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_SPECULAR_REFLECTED_HIT_DISTANCE
		elif view == "specular_reflected_hit_normal":
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_SPECULAR_REFLECTED_HIT_NORMAL
		elif view == "direct_light_regime":
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_DIRECT_LIGHT_REGIME
		else:
			_environment.rtgi_debug_mode = Environment.RT_DEBUG_DISABLED
	RenderingServer.viewport_set_debug_draw(get_viewport().get_viewport_rid(), _debug_draw_value(view))


# Maps a harness debug-view name to its RenderingServer.VIEWPORT_DEBUG_DRAW_RTGI_*
# constant NAME. The engine's RTGI debug-draw enum set churns across RTGI architecture
# revisions (e.g. the denoiser/signal/cache/STRC views were replaced by WRC/SPG/RESOLVE
# views), so this is resolved by name at runtime in _debug_draw_value() rather than
# referenced directly — a direct reference to a constant a given build doesn't expose
# fails to PARSE and takes the whole harness (incl. the beauty-frame gate) down with it.
const _RTGI_DEBUG_VIEW_CONSTANTS := {
	"raw_radiance": "VIEWPORT_DEBUG_DRAW_RTGI_RAW_RADIANCE",
	"noisy": "VIEWPORT_DEBUG_DRAW_RTGI_NOISY",
	"diffuse_noisy": "VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_NOISY",
	"specular_noisy": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_NOISY",
	"diffuse_final": "VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_FINAL",
	"specular_final": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_FINAL",
	"specular_guide": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_GUIDE",
	"specular_reflection_direction": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTION_DIRECTION",
	"specular_reflected_hit_distance": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_DISTANCE",
	"specular_reflected_hit_normal": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_NORMAL",
	"specular_roughness_bucket": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_ROUGHNESS_BUCKET",
	"specular_history_length": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_HISTORY_LENGTH",
	"specular_rejection": "VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REJECTION",
	"normal_roughness": "VIEWPORT_DEBUG_DRAW_RTGI_NORMAL_ROUGHNESS",
	"viewz_hitdist": "VIEWPORT_DEBUG_DRAW_RTGI_VIEWZ_HITDIST",
	"motion_vectors": "VIEWPORT_DEBUG_DRAW_RTGI_MOTION_VECTORS",
	"signal_direct": "VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_DIRECT_LIGHT",
	"signal_emissive": "VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_EMISSIVE",
	"signal_indirect": "VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_INDIRECT",
	"signal_sky": "VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_SKY",
	"signal_confidence": "VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_CONFIDENCE",
	"source_candidate": "VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_CANDIDATE",
	"source_history": "VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_HISTORY",
	"source_temporal_delta": "VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_TEMPORAL_DELTA",
	"source_rejection": "VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_REJECTION",
	"secondary_cache_source": "VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SOURCE",
	"secondary_cache_rejection": "VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_REJECTION",
	"secondary_cache_surface": "VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SURFACE",
	"surface_feedback": "VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_FEEDBACK",
	"surface_key": "VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_KEY",
	"cache_raw_diffuse": "VIEWPORT_DEBUG_DRAW_RTGI_CACHE_RAW_DIFFUSE",
	"cache_filtered_diffuse": "VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE",
	"cache_hit_confidence": "VIEWPORT_DEBUG_DRAW_RTGI_CACHE_HIT_CONFIDENCE",
	"cache_age": "VIEWPORT_DEBUG_DRAW_RTGI_CACHE_AGE",
	"cache_rejection": "VIEWPORT_DEBUG_DRAW_RTGI_CACHE_REJECTION",
	"strc_radiance": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_RADIANCE",
	"strc_confidence": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_CONFIDENCE",
	"strc_updates": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_UPDATES",
	"strc_visibility": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_VISIBILITY",
	"strc_age": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_AGE",
	"strc_variance": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_VARIANCE",
	"strc_rejection": "VIEWPORT_DEBUG_DRAW_RTGI_STRC_REJECTION",
	"variance": "VIEWPORT_DEBUG_DRAW_RTGI_VARIANCE",
	"history_length": "VIEWPORT_DEBUG_DRAW_RTGI_HISTORY_LENGTH",
	"rejection": "VIEWPORT_DEBUG_DRAW_RTGI_REJECTION",
	"final": "VIEWPORT_DEBUG_DRAW_RTGI_FINAL",
	"denoised_radiance": "VIEWPORT_DEBUG_DRAW_RTGI_DENOISED_RADIANCE",
	"reconstructed_radiance": "VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_RADIANCE",
	"reconstruction_reactivity": "VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_REACTIVITY",
	"reconstruction_signal_confidence": "VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_SIGNAL_CONFIDENCE",
	"reconstruction_guide_mismatch": "VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_GUIDE_MISMATCH",
	"reconstruction_fill_source": "VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_FILL_SOURCE",
}

static var _warned_missing_rtgi_debug: Dictionary = {}

func _debug_draw_value(view: String) -> int:
	if view == "beauty" or view == "disabled":
		return 0
	var constant_name: String = _RTGI_DEBUG_VIEW_CONSTANTS.get(view, "")
	if constant_name.is_empty():
		return 0
	if ClassDB.class_has_integer_constant("RenderingServer", constant_name):
		return ClassDB.class_get_integer_constant("RenderingServer", constant_name)
	if not _warned_missing_rtgi_debug.has(constant_name):
		_warned_missing_rtgi_debug[constant_name] = true
		push_warning("RTGI debug view '%s' (%s) is not exposed by this engine build; capturing beauty instead." % [view, constant_name])
	return 0


func _measure_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var full_frame_fireflies: int = _count_isolated_hot_pixels(image, 0, 0, width, height)
	var visible_speckles: int = _count_visible_speckles(image, 0, 0, width, height)
	var visible_speckle_samples := _sampled_grid_pixel_count(width, height, 2, 2)
	var dark_x0 := int(width * 0.62)
	var dark_y0 := int(height * 0.18)
	var dark_x1 := int(width * 0.94)
	var dark_y1 := int(height * 0.84)
	var detail_x0 := int(width * 0.07)
	var detail_y0 := int(height * 0.30)
	var detail_x1 := int(width * 0.42)
	var detail_y1 := int(height * 0.76)
	var specular_x0 := int(width * 0.48)
	var specular_y0 := int(height * 0.46)
	var specular_x1 := int(width * 0.78)
	var specular_y1 := int(height * 0.75)

	var fireflies: int = _count_isolated_hot_pixels(image, dark_x0, dark_y0, dark_x1, dark_y1)
	var dark_pixels: int = maxi((dark_x1 - dark_x0) * (dark_y1 - dark_y0), 1)
	var detail: Dictionary = _measure_detail_region(image, detail_x0, detail_y0, detail_x1, detail_y1)
	var specular: Dictionary = _measure_detail_region(image, specular_x0, specular_y0, specular_x1, specular_y1)
	var luma_stats: Dictionary = _measure_luma_stats(image)
	var dark_mean: float = _measure_mean_luma(image, dark_x0, dark_y0, dark_x1, dark_y1)
	var specular_fireflies: int = _count_isolated_hot_pixels(image, specular_x0, specular_y0, specular_x1, specular_y1)

	return {
		"full_frame_fireflies": full_frame_fireflies,
		"full_frame_fireflies_per_megapixel": _per_megapixel(full_frame_fireflies, width * height),
		"visible_speckles": visible_speckles,
		"visible_speckles_per_megapixel": _per_megapixel(visible_speckles, visible_speckle_samples),
		"dark_fireflies": fireflies,
		"dark_fireflies_per_megapixel": _per_megapixel(fireflies, dark_pixels),
		"dark_roi_mean_luma": dark_mean,
		"detail_edge_energy": detail["edge_energy"],
		"detail_luma_stddev": detail["luma_stddev"],
		"specular_fireflies": specular_fireflies,
		"specular_edge_energy": specular["edge_energy"],
		"specular_luma_stddev": specular["luma_stddev"],
		"luma_p95": luma_stats["p95"],
		"luma_p99": luma_stats["p99"],
		"luma_max": luma_stats["max"],
		"saturated_luma_fraction": luma_stats["saturated_fraction"],
	}


func _measure_coexistence_image(rt_image: Image, base_name: String) -> Dictionary:
	var roi := _coexistence_owner_roi(rt_image)
	var rt_luma := _measure_mean_luma(rt_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var rt_color := _measure_mean_color(rt_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var metrics := {
		"coexistence_rt_owner_luma": rt_luma,
		"coexistence_rt_owner_blue_green_margin": maxf(rt_color.b, rt_color.g) - rt_color.r,
		"coexistence_mode": _environment.rtgi_mode if _environment != null else -1,
	}
	if _environment == null:
		return metrics

	var previous_enabled := _environment.rtgi_enabled
	var previous_mode := _environment.rtgi_mode
	_environment.rtgi_enabled = false
	_apply_debug_view("beauty")
	for frame in range(mini(_warmup_frames, 24)):
		await _wait_render_frame()
	var raster_image := get_viewport().get_texture().get_image()
	raster_image.convert(Image.FORMAT_RGBA8)
	var raster_path := "%s/%s_raster_fallback.png" % [_output_dir, base_name]
	raster_image.save_png(raster_path)
	var raster_luma := _measure_mean_luma(raster_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)
	var raster_color := _measure_mean_color(raster_image, roi.position.x, roi.position.y, roi.end.x, roi.end.y)

	_environment.rtgi_enabled = previous_enabled
	_environment.rtgi_mode = previous_mode
	for frame in range(mini(_warmup_frames, 24)):
		await _wait_render_frame()

	metrics["coexistence_raster_owner_luma"] = raster_luma
	metrics["coexistence_raster_owner_blue_green_margin"] = maxf(raster_color.b, raster_color.g) - raster_color.r
	metrics["coexistence_owner_luma_delta"] = rt_luma - raster_luma
	metrics["coexistence_owner_luma_ratio"] = rt_luma / maxf(raster_luma, 0.001)
	metrics["coexistence_raster_fallback_path"] = ProjectSettings.globalize_path(raster_path)
	return metrics


func _coexistence_owner_roi(image: Image) -> Rect2i:
	var width := image.get_width()
	var height := image.get_height()
	return Rect2i(
			Vector2i(int(width * 0.36), int(height * 0.29)),
			Vector2i(int(width * 0.28), int(height * 0.34)))


func _measure_cornell_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var center_detail := _measure_detail_region(image, int(width * 0.30), int(height * 0.46), int(width * 0.72), int(height * 0.86))
	var ceiling_hot_pixels := _count_isolated_hot_pixels(image, int(width * 0.34), int(height * 0.02), int(width * 0.66), int(height * 0.28))
	var red_wall := _measure_mean_luma(image, int(width * 0.03), int(height * 0.16), int(width * 0.22), int(height * 0.78))
	var green_wall := _measure_mean_luma(image, int(width * 0.78), int(height * 0.16), int(width * 0.97), int(height * 0.78))
	var back_wall := _measure_mean_luma(image, int(width * 0.28), int(height * 0.16), int(width * 0.72), int(height * 0.56))
	var floor_luma := _measure_mean_luma(image, int(width * 0.15), int(height * 0.70), int(width * 0.86), int(height * 0.96))
	var ceiling_luma := _measure_mean_luma(image, int(width * 0.16), int(height * 0.02), int(width * 0.86), int(height * 0.25))
	var wall_min: float = minf(minf(red_wall, green_wall), minf(back_wall, minf(floor_luma, ceiling_luma)))
	var black_002 := _measure_black_fraction(image, 0, 0, width, height, 0.02)
	var black_005 := _measure_black_fraction(image, 0, 0, width, height, 0.05)
	var red_wall_black := _measure_black_fraction(image, int(width * 0.04), int(height * 0.18), int(width * 0.20), int(height * 0.68), 0.05)
	var green_wall_black := _measure_black_fraction(image, int(width * 0.80), int(height * 0.18), int(width * 0.96), int(height * 0.68), 0.05)
	var back_wall_black := _measure_black_fraction(image, int(width * 0.28), int(height * 0.16), int(width * 0.72), int(height * 0.56), 0.05)
	var floor_black := _measure_black_fraction(image, int(width * 0.15), int(height * 0.70), int(width * 0.86), int(height * 0.96), 0.05)
	var flat_noise := _measure_flat_patch_noise(image, int(width * 0.56), int(height * 0.26), int(width * 0.72), int(height * 0.48))
	var red_chroma := _measure_mean_color(image, int(width * 0.03), int(height * 0.16), int(width * 0.22), int(height * 0.78))
	var green_chroma := _measure_mean_color(image, int(width * 0.78), int(height * 0.16), int(width * 0.97), int(height * 0.78))
	return {
		"cornell_center_edge_energy": center_detail["edge_energy"],
		"cornell_center_luma_stddev": center_detail["luma_stddev"],
		"cornell_ceiling_hot_pixels": ceiling_hot_pixels,
		"cornell_black_fraction_luma_002": black_002,
		"cornell_black_fraction_luma_005": black_005,
		"cornell_red_wall_black_fraction": red_wall_black,
		"cornell_green_wall_black_fraction": green_wall_black,
		"cornell_back_wall_black_fraction": back_wall_black,
		"cornell_floor_black_fraction": floor_black,
		"cornell_flat_patch_noise": flat_noise,
		"cornell_red_wall_chroma_margin": red_chroma.r - maxf(red_chroma.g, red_chroma.b),
		"cornell_green_wall_chroma_margin": green_chroma.g - maxf(green_chroma.r, green_chroma.b),
		"cornell_red_wall_mean_luma": red_wall,
		"cornell_green_wall_mean_luma": green_wall,
		"cornell_back_wall_mean_luma": back_wall,
		"cornell_floor_mean_luma": floor_luma,
		"cornell_ceiling_mean_luma": ceiling_luma,
		"cornell_wall_min_luma": wall_min,
		"cornell_left_right_luma_delta": red_wall - green_wall,
		"cornell_public_data_url": CORNELL_PUBLIC_DATA_URL,
		"cornell_reference_url": CORNELL_REFERENCE_URL,
	}


# Color-bleed measurement for the committed cornell_box scene. The walls and
# floor are neutral white, so the left half of the floor should pick up a red
# tint from the red wall and the right half a green tint from the green wall.
# This is the RTGI color-bleed ground truth, and the chroma margins below stay
# positive only when that indirect bounce reaches the floor. ROIs are tuned to
# the cornell_box camera framing (centered box, opening toward the camera).
func _measure_cornell_box_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var left_wall := _measure_mean_color(image, int(width * 0.04), int(height * 0.30), int(width * 0.16), int(height * 0.70))
	var right_wall := _measure_mean_color(image, int(width * 0.84), int(height * 0.30), int(width * 0.96), int(height * 0.70))
	# Floor strips hugging each colored wall, outside the central interior blocks,
	# where the wall color bleed onto the neutral floor is strongest and cleanest.
	var left_floor := _measure_mean_color(image, int(width * 0.07), int(height * 0.66), int(width * 0.24), int(height * 0.84))
	var right_floor := _measure_mean_color(image, int(width * 0.76), int(height * 0.66), int(width * 0.93), int(height * 0.84))
	var back_wall := _measure_mean_color(image, int(width * 0.36), int(height * 0.30), int(width * 0.64), int(height * 0.58))
	var floor_x0 := int(width * 0.22)
	var floor_y0 := int(height * 0.74)
	var floor_x1 := int(width * 0.78)
	var floor_y1 := int(height * 0.94)
	var floor_luma := _measure_mean_luma(image, floor_x0, floor_y0, floor_x1, floor_y1)
	var ceiling_hot_pixels := _count_isolated_hot_pixels(image, int(width * 0.36), int(height * 0.02), int(width * 0.64), int(height * 0.14))
	# Front face of the short block (the face turned toward the camera). Its
	# normal points away from both ceiling lights (N dot L < 0 for the area
	# panel and the key omni), so the face receives no direct light at all:
	# everything on it is indirect bounce. The rect is the projected face span
	# (u 0.237..0.433, v 0.665..0.920) inset for safety. This encoded mean is
	# informational; the linear twin below is the one the energy check gates on.
	var face_x0 := int(width * 0.27)
	var face_y0 := int(height * 0.70)
	var face_x1 := int(width * 0.40)
	var face_y1 := int(height * 0.88)
	var occluded_face := _measure_mean_luma(image, face_x0, face_y0, face_x1, face_y1)
	# Linear-space twins of the floor / occluded-face lumas for the rtgi_energy
	# scaling check. The capture is the tonemapped, sRGB-ENCODED final frame, so a
	# 2x linear radiance change reads as ~1.42x in the encoded means above; ratios
	# that should track rtgi_energy linearly must be formed from per-pixel
	# linearized lumas instead (mean of linearized pixels, not a linearized mean).
	# Since the occluded face is indirect-only, its linear luma is the cleanest
	# probe for GI-only behavior such as rtgi_energy scaling.
	var floor_linear_luma := _measure_mean_linear_luma(image, floor_x0, floor_y0, floor_x1, floor_y1)
	var occluded_face_linear := _measure_mean_linear_luma(image, face_x0, face_y0, face_x1, face_y1)
	return {
		"cornell_box_red_wall_chroma_margin": left_wall.r - maxf(left_wall.g, left_wall.b),
		"cornell_box_green_wall_chroma_margin": right_wall.g - maxf(right_wall.r, right_wall.b),
		"cornell_box_left_floor_red_bleed_margin": left_floor.r - maxf(left_floor.g, left_floor.b),
		"cornell_box_right_floor_green_bleed_margin": right_floor.g - maxf(right_floor.r, right_floor.b),
		"cornell_box_floor_mean_luma": floor_luma,
		"cornell_box_back_wall_mean_luma": _luma(back_wall),
		"cornell_box_occluded_face_mean_luma": occluded_face,
		"cornell_box_floor_mean_linear_luma": floor_linear_luma,
		"cornell_box_occluded_face_mean_linear_luma": occluded_face_linear,
		"cornell_box_ceiling_hot_pixels": ceiling_hot_pixels,
	}


# Cross-run rtgi_energy scaling check for the cornell_box scene, following the
# FOGPAR protocol: a --rtgi-energy=1.0 run records per-mode reference rect
# lumas into the shared output dir, and a run at any other energy compares its
# rect lumas against that reference. The occluded block face receives indirect
# light only, so its LINEAR mean luma must scale linearly with rtgi_energy. The
# captured frame is sRGB-encoded, so the ratio is formed from the linearized
# rect lumas (the encoded ratio of a clean 2x reads only ~1.42 and can never
# reach the multiplier); the verdict checks that ratio against the requested
# multiplier. Hybrid gets a wider corridor than FPT because its indirect
# signal is probe-filtered. Prints one machine-readable ENERGY line per
# config; FAIL routes into the exit code.
# Configs that composite no RTGI GI print a SKIP verdict instead, and a
# reference recorded at a different resolution, denoiser, or RTGI resolution
# scale is rejected as stale (mirroring the FOGPAR probe-layout check).
func _measure_energy_scaling(metrics: Dictionary, mode_label: String, image_size: Vector2i) -> void:
	var energy := _rtgi_energy_override
	# Guard on the live environment state, not the label string: when RTGI is
	# disabled (the off config) or the mode composites no GI (reflections only
	# replaces the specular signal), the occluded face can never scale with
	# rtgi_energy, so neither record a reference nor verdict the config.
	if _environment == null or not _environment.rtgi_enabled or _environment.rtgi_mode == Environment.RTGI_MODE_REFLECTIONS_RT_ONLY:
		print("ENERGY mode=%s energy=%.4f verdict=SKIP reason=no-rtgi-gi" % [mode_label, energy])
		metrics["rtgi_energy_verdict"] = "SKIP"
		return
	var floor_luma := float(metrics.get("cornell_box_floor_mean_linear_luma", 0.0))
	var occluded_luma := float(metrics.get("cornell_box_occluded_face_mean_linear_luma", 0.0))
	var run_denoiser := int(metrics.get("applied_rtgi_denoiser", -1))
	var run_resolution_scale := float(metrics.get("applied_rtgi_resolution_scale", -1.0))
	var reference_path := "%s/energy_reference_%s.json" % [_output_dir, mode_label]
	if is_equal_approx(energy, 1.0):
		_write_json(reference_path, {
			"mode": mode_label,
			"energy": energy,
			"floor_mean_linear_luma": floor_luma,
			"occluded_face_mean_linear_luma": occluded_luma,
			"image_width": image_size.x,
			"image_height": image_size.y,
			"applied_rtgi_denoiser": run_denoiser,
			"applied_rtgi_resolution_scale": run_resolution_scale,
		})
		print("ENERGY mode=%s reference recorded" % mode_label)
		metrics["rtgi_energy_reference_recorded"] = true
		return
	var reference := _read_json_dictionary(reference_path)
	# References from before the linear-luma fix carry encoded-space lumas under
	# the old key names; the has() checks reject them as missing so a stale
	# encoded reference can never silently shift the ratio baseline.
	if reference.is_empty() or not reference.has("occluded_face_mean_linear_luma") or not reference.has("floor_mean_linear_luma"):
		push_error("rtgi_energy check: reference '%s' is missing or unreadable. Run the same scene into this output dir with --rtgi-energy=1.0 first." % reference_path)
		print("ENERGY mode=%s energy=%.4f floor_ratio=nan occluded_ratio=nan expected=%.4f verdict=FAIL" % [mode_label, energy, energy])
		metrics["rtgi_energy_verdict"] = "FAIL"
		metrics["rtgi_energy_reference_missing"] = true
		return
	# A reference from a different capture resolution, denoiser, or RTGI
	# resolution scale is stale; comparing against it would silently shift the
	# luma baseline (the FOGPAR gate rejects layout mismatches the same way).
	var ref_width := int(reference.get("image_width", -1))
	var ref_height := int(reference.get("image_height", -1))
	var ref_denoiser := int(reference.get("applied_rtgi_denoiser", -2))
	var ref_resolution_scale := float(reference.get("applied_rtgi_resolution_scale", -2.0))
	if ref_width != image_size.x or ref_height != image_size.y or ref_denoiser != run_denoiser or not is_equal_approx(ref_resolution_scale, run_resolution_scale):
		push_error("rtgi_energy check: stale reference '%s' (image %dx%d vs %dx%d, denoiser %d vs %d, resolution_scale %.3f vs %.3f). Re-record it into this output dir with --rtgi-energy=1.0." % [
				reference_path, ref_width, ref_height, image_size.x, image_size.y,
				ref_denoiser, run_denoiser, ref_resolution_scale, run_resolution_scale])
		print("ENERGY mode=%s energy=%.4f floor_ratio=nan occluded_ratio=nan expected=%.4f verdict=FAIL" % [mode_label, energy, energy])
		metrics["rtgi_energy_verdict"] = "FAIL"
		metrics["rtgi_energy_reference_stale"] = true
		return
	var floor_ratio := floor_luma / maxf(float(reference["floor_mean_linear_luma"]), 1e-6)
	var occluded_ratio := occluded_luma / maxf(float(reference["occluded_face_mean_linear_luma"]), 1e-6)
	# Corridor width follows the live applied mode enum, not the config label, so
	# a scene-authored Hybrid run gets the Hybrid corridor too.
	var tolerance := 0.15 if int(_environment.rtgi_mode) == Environment.RTGI_MODE_HYBRID else 0.05
	# Near zero the relative error degenerates (dividing by ~0 amplifies any
	# residual into a guaranteed FAIL), so the corridor turns absolute there.
	var rel_err := absf(occluded_ratio - energy)
	if energy >= 0.05:
		rel_err /= energy
	var verdict := "PASS" if rel_err <= tolerance else "FAIL"
	print("ENERGY mode=%s energy=%.4f floor_ratio=%.6f occluded_ratio=%.6f expected=%.4f verdict=%s" % [
			mode_label, energy, floor_ratio, occluded_ratio, energy, verdict])
	metrics["rtgi_energy_floor_ratio"] = floor_ratio
	metrics["rtgi_energy_occluded_ratio"] = occluded_ratio
	metrics["rtgi_energy_expected_ratio"] = energy
	metrics["rtgi_energy_rel_err"] = rel_err
	metrics["rtgi_energy_verdict"] = verdict


# Reflection-quality measurement for the committed reflective_pool scene. The
# near-mirror plane fills the lower half of the frame, so the edge energy there
# tracks how sharply the emissive spheres reflect, and the pool mean luma tracks
# the emissive GI bleeding onto the surface. Both should stay above their floors
# when reflections and bleed are intact.
func _measure_reflective_pool_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var reflection := _measure_detail_region(image, int(width * 0.20), int(height * 0.56), int(width * 0.80), int(height * 0.96))
	var pool_luma := _measure_mean_luma(image, int(width * 0.10), int(height * 0.60), int(width * 0.90), int(height * 0.98))
	var reflection_fireflies := _count_isolated_hot_pixels(image, int(width * 0.10), int(height * 0.56), int(width * 0.90), int(height * 0.98))
	return {
		"reflective_pool_reflection_edge_energy": reflection["edge_energy"],
		"reflective_pool_reflection_luma_stddev": reflection["luma_stddev"],
		"reflective_pool_surface_mean_luma": pool_luma,
		"reflective_pool_reflection_fireflies": reflection_fireflies,
	}


# Fog-parity measurement for the committed fog_corridor scene. The probe-rect
# math and the machine-readable FOGPAR verdict line live in
# scripts/fog_corridor_metric.gd on the scene root; the harness hands over the
# capture, the output dir, and the live Environment. One invocation measures
# one config: --rtgi-mode=off records the raster reference JSON, and the
# hybrid/fpt runs compare their per-depth floor luminances against it.
func _measure_fog_corridor_image(image: Image) -> Dictionary:
	if _packed_scene_root != null and _packed_scene_root.has_method("measure_fog_parity"):
		return _packed_scene_root.measure_fog_parity(image, _output_dir, _environment)
	push_warning("fog_corridor scene root does not expose measure_fog_parity; regenerate the scene with tools/generate_test_scenes.gd.")
	return {}


# Toggle/orbit temporal measurement for the committed light_grid scene, in two
# phases over live renders (the passed capture only fixes the frame size).
#
# Phase 1 (orbit steady state): steps the scene animation for
# LIGHT_GRID_PHASE_FRAMES frames while the camera orbit keeps re-sorting the
# 24-light grid by camera distance and the emissive sphere keeps moving. On a
# healthy temporal pipeline the per-frame-pair whole-image mean luma delta
# stays small (slow orbit, converged history); a direct-light history that
# resets every frame shows up directly as an elevated
# light_grid_orbit_delta_avg and orbit sparkle.
#
# Phase 2 (toggle recovery): freezes the animation, flips ToggleLight off (a
# REAL light-set change; the history reset it causes is correct), and records
# another LIGHT_GRID_PHASE_FRAMES-pair series. light_grid_toggle_spike is the
# whole-image mean delta on the toggle frame;
# light_grid_toggle_recovery_frames is the first post-toggle pair whose
# whole-image mean delta drops within LIGHT_GRID_RECOVERY_FACTOR times the
# frozen static-noise floor of the Phase-2 series (light_grid_static_tail_delta,
# measured at the tail of the same toggle run). Anchoring on the static floor
# rather than the Phase-1 orbit mean is what makes the factor (3.0) meaningful:
# the orbit mean is inflated by camera motion, so a threshold derived from it
# cleared almost immediately, whereas a wrongly global reset that keeps the whole
# image churning pushes the frame count up against the static floor. The window
# length when the image never settles is the gateable worst case. The local/far
# rect split separates a correct LOCAL
# response from a wrongly GLOBAL reset: the local rect is projected around the
# floor patch under the toggled light, the far rect sits in the image corner
# farthest from it, and on a correct estimator the toggle delta concentrates in
# the local rect.
#
# All five light_grid_* metrics are emitted unconditionally on every run of
# this scene (a threshold key whose metric is missing is itself a gate
# failure). The per-pair series of both phases goes to
# <base>_light_grid_toggle_curve.json next to the other per-frame curves.
func _measure_light_grid(final_image: Image, base_name: String) -> Dictionary:
	_apply_debug_view("beauty")
	var width := final_image.get_width()
	var height := final_image.get_height()
	var sampled_pixels := maxi((width / 2) * (height / 2), 1)

	# Phase 1: orbit steady state.
	_animate_specular_objects(_scene_frame)
	await _wait_render_frame()
	_scene_frame += 1
	var previous := get_viewport().get_texture().get_image()
	previous.convert(Image.FORMAT_RGBA8)
	var orbit_series := []
	var orbit_delta_sum := 0.0
	var orbit_sparkle_max := 0
	for pair in range(1, LIGHT_GRID_PHASE_FRAMES):
		_animate_specular_objects(_scene_frame)
		await _wait_render_frame()
		_scene_frame += 1
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		var delta := _mean_abs_luma_delta(previous, current)
		var sparkles := _count_temporal_sparkles(previous, current)
		orbit_delta_sum += delta
		orbit_sparkle_max = maxi(orbit_sparkle_max, sparkles)
		orbit_series.append({
			"pair": pair,
			"mean_abs_luma_delta": delta,
			"sparkle_per_megapixel": _per_megapixel(sparkles, sampled_pixels),
		})
		previous = current
	var orbit_delta_avg := orbit_delta_sum / maxf(float(LIGHT_GRID_PHASE_FRAMES - 1), 1.0)

	# Phase 2: freeze the orbit, flip one real light off, watch the recovery.
	# The rects are projected with the camera frozen at its toggle-time pose,
	# so they track the actual orbit phase instead of assuming frame-0 framing.
	var rects := _light_grid_toggle_rects(width, height)
	if _packed_scene_root != null and _packed_scene_root.has_method("set_toggle_light"):
		_packed_scene_root.set_toggle_light(false)
	else:
		push_warning("light_grid scene root does not expose set_toggle_light; regenerate the scene with tools/generate_test_scenes.gd.")
	var toggle_series := []
	var toggle_spike := 0.0
	var local_rect_delta := 0.0
	var far_rect_delta := 0.0
	var tail_delta_sum := 0.0
	for pair in range(1, LIGHT_GRID_PHASE_FRAMES + 1):
		await _wait_render_frame()
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		var delta := _mean_abs_luma_delta(previous, current)
		if pair == 1:
			toggle_spike = delta
			local_rect_delta = _mean_abs_luma_delta_rect(previous, current, rects["local"])
			far_rect_delta = _mean_abs_luma_delta_rect(previous, current, rects["far"])
		if pair > LIGHT_GRID_PHASE_FRAMES - LIGHT_GRID_TAIL_PAIRS:
			tail_delta_sum += delta
		toggle_series.append({
			"pair": pair,
			"mean_abs_luma_delta": delta,
			"sparkle_per_megapixel": _per_megapixel(_count_temporal_sparkles(previous, current), sampled_pixels),
		})
		previous = current
	var static_tail_delta := tail_delta_sum / float(LIGHT_GRID_TAIL_PAIRS)
	# Recovery is measured against the frozen static-noise floor of this same
	# Phase-2 series (known only after the loop), not the camera-inflated Phase-1
	# orbit mean. recovery_frames is the first post-toggle pair (pair 1 is the
	# spike itself) whose whole-image delta drops within the threshold; the full
	# window length is the gateable worst case when the image never settles.
	var recovery_threshold := maxf(static_tail_delta * LIGHT_GRID_RECOVERY_FACTOR, LIGHT_GRID_RECOVERY_FLOOR)
	var recovery_frames := LIGHT_GRID_PHASE_FRAMES
	for entry in toggle_series:
		if int(entry["pair"]) == 1:
			continue
		if float(entry["mean_abs_luma_delta"]) <= recovery_threshold:
			recovery_frames = int(entry["pair"])
			break
	# Restore the authored light set and absorb the restore transient here so
	# the later generic sparkle loop does not start on the flip-back frame.
	if _packed_scene_root != null and _packed_scene_root.has_method("set_toggle_light"):
		_packed_scene_root.set_toggle_light(true)
		await _wait_render_frame()
		await _wait_render_frame()

	var curve_path := "%s/%s_light_grid_toggle_curve.json" % [_output_dir, base_name]
	_write_json(curve_path, {
		"phase_frames": LIGHT_GRID_PHASE_FRAMES,
		"recovery_threshold": recovery_threshold,
		"static_tail_delta": static_tail_delta,
		"local_rect": [rects["local"].position.x, rects["local"].position.y, rects["local"].size.x, rects["local"].size.y],
		"far_rect": [rects["far"].position.x, rects["far"].position.y, rects["far"].size.x, rects["far"].size.y],
		"orbit_series": orbit_series,
		"toggle_series": toggle_series,
	})
	return {
		"light_grid_orbit_delta_avg": orbit_delta_avg,
		"light_grid_orbit_sparkle_max": _per_megapixel(orbit_sparkle_max, sampled_pixels),
		"light_grid_toggle_spike": toggle_spike,
		"light_grid_toggle_recovery_frames": recovery_frames,
		"light_grid_toggle_local_rect_delta": local_rect_delta,
		"light_grid_toggle_far_rect_delta": far_rect_delta,
		"light_grid_static_tail_delta": static_tail_delta,
		"light_grid_toggle_curve_path": ProjectSettings.globalize_path(curve_path),
	}


# Penumbra-band measurement for the committed sun_penumbra_ramp scene. The ROI is a horizontal
# strip across the soft shadow edge; columns run from fully lit to fully occluded.
# penumbra_width_px = count of columns whose normalized luma is in the transition band [0.1,0.9];
# a wider source spreads the edge over more columns, so the soft raster reference reads a wider
# band than a one-sample ray-traced pass. penumbra_band_hf = mean |luma[x-1]-2*luma[x]+luma[x+1]|
# over the strip, the second-difference high-frequency content of the column profile (the ramp
# curvature plus any per-sample shadow noise). lit/occluded_luma are the bright and dark plateaus
# the band is normalized against.
func _measure_sun_penumbra_ramp(final_image: Image, base_name: String) -> Dictionary:
	var width := final_image.get_width()
	var height := final_image.get_height()
	var roi_x0 := int(width * 0.30)
	var roi_x1 := int(width * 0.62)
	var roi_y0 := int(height * 0.40)
	var roi_y1 := int(height * 0.60)
	var cols := roi_x1 - roi_x0
	var col_luma := PackedFloat32Array()
	col_luma.resize(cols)
	for cx in range(cols):
		var x := roi_x0 + cx
		var acc := 0.0
		for y in range(roi_y0, roi_y1):
			var c := final_image.get_pixel(x, y)
			acc += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
		col_luma[cx] = acc / float(roi_y1 - roi_y0)
	var lo: float = col_luma[0]
	var hi: float = col_luma[0]
	for cx in range(cols):
		lo = minf(lo, col_luma[cx])
		hi = maxf(hi, col_luma[cx])
	var span: float = maxf(hi - lo, 1e-6)
	var width_px := 0
	for cx in range(cols):
		var n: float = (col_luma[cx] - lo) / span
		if n > 0.1 and n < 0.9:
			width_px += 1
	var hf := 0.0
	for cx in range(1, cols - 1):
		hf += abs(col_luma[cx - 1] - 2.0 * col_luma[cx] + col_luma[cx + 1])
	hf /= float(max(cols - 2, 1))
	return {
		"sun_penumbra_width_px": width_px,
		"sun_penumbra_band_hf": hf,
		"sun_penumbra_lit_luma": hi,
		"sun_penumbra_occluded_luma": lo,
	}


# Local/far measurement rects for the light_grid toggle split. The local rect
# is centered on the camera projection of the floor point under ToggleLight
# (the patch whose direct light genuinely changes); the far rect fills the
# image corner farthest from that center (geometry whose light set did not
# change, so on a correct estimator its toggle-frame delta stays near the
# steady-state level). Falls back to an image-center local rect when the scene
# root or camera cannot provide the projection.
func _light_grid_toggle_rects(width: int, height: int) -> Dictionary:
	var local_center := Vector2(float(width) * 0.5, float(height) * 0.5)
	if _camera != null and _packed_scene_root != null and _packed_scene_root.has_method("get_toggle_light_floor_point"):
		var floor_point: Vector3 = _packed_scene_root.get_toggle_light_floor_point()
		if not _camera.is_position_behind(floor_point):
			var projected := _camera.unproject_position(floor_point)
			local_center = Vector2(
					clampf(projected.x, float(width) * 0.12, float(width) * 0.88),
					clampf(projected.y, float(height) * 0.12, float(height) * 0.88))
	var local_size := Vector2i(int(width * 0.18), int(height * 0.18))
	var local_pos := Vector2i(
			clampi(int(local_center.x) - local_size.x / 2, 0, width - local_size.x),
			clampi(int(local_center.y) - local_size.y / 2, 0, height - local_size.y))
	var local_rect := Rect2i(local_pos, local_size)
	var far_size := Vector2i(int(width * 0.20), int(height * 0.20))
	var far_x := 0 if local_center.x > float(width) * 0.5 else width - far_size.x
	var far_y := 0 if local_center.y > float(height) * 0.5 else height - far_size.y
	var far_rect := Rect2i(Vector2i(far_x, far_y), far_size)
	return { "local": local_rect, "far": far_rect }


# Whole-image mean absolute luma delta between two frames, on the same stride-2
# grid the sparkle counter samples.
func _mean_abs_luma_delta(previous: Image, current: Image) -> float:
	var width := mini(previous.get_width(), current.get_width())
	var height := mini(previous.get_height(), current.get_height())
	var sum := 0.0
	var count := 0
	for y in range(0, height, 2):
		for x in range(0, width, 2):
			sum += absf(_luma(current.get_pixel(x, y)) - _luma(previous.get_pixel(x, y)))
			count += 1
	return sum / maxf(float(count), 1.0)


func _mean_abs_luma_delta_rect(previous: Image, current: Image, rect: Rect2i) -> float:
	var x1 := mini(rect.end.x, mini(previous.get_width(), current.get_width()))
	var y1 := mini(rect.end.y, mini(previous.get_height(), current.get_height()))
	var sum := 0.0
	var count := 0
	for y in range(maxi(rect.position.y, 0), y1):
		for x in range(maxi(rect.position.x, 0), x1):
			sum += absf(_luma(current.get_pixel(x, y)) - _luma(previous.get_pixel(x, y)))
			count += 1
	return sum / maxf(float(count), 1.0)


func _measure_black_fraction(image: Image, x0: int, y0: int, x1: int, y1: int, threshold: float) -> float:
	var black := 0
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height())):
		for x in range(maxi(x0, 0), mini(x1, image.get_width())):
			if _luma(image.get_pixel(x, y)) < threshold:
				black += 1
			count += 1
	return float(black) / maxf(float(count), 1.0)


func _measure_flat_patch_noise(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height() - 1)):
		for x in range(maxi(x0, 0), mini(x1, image.get_width() - 1)):
			var l := _luma(image.get_pixel(x, y))
			sum += absf(l - _luma(image.get_pixel(x + 1, y)))
			sum += absf(l - _luma(image.get_pixel(x, y + 1)))
			count += 2
	return sum / maxf(float(count), 1.0)


func _measure_mean_color(image: Image, x0: int, y0: int, x1: int, y1: int) -> Color:
	var sum := Color(0, 0, 0, 0)
	var count := 0
	for y in range(maxi(y0, 0), mini(y1, image.get_height())):
		for x in range(maxi(x0, 0), mini(x1, image.get_width())):
			sum += image.get_pixel(x, y)
			count += 1
	return sum / maxf(float(count), 1.0)


func _measure_temporal_sparkle(base_name: String) -> Dictionary:
	_apply_debug_view("beauty")
	var motion_scene := _scene_mode == "specular_stability" or _is_packed_test_scene()
	if motion_scene:
		if _camera_pan:
			_animate_camera(_scene_frame)
		_animate_specular_objects(_scene_frame)
	await _wait_render_frame()
	_scene_frame += 1
	var previous := get_viewport().get_texture().get_image()
	previous.convert(Image.FORMAT_RGBA8)
	var first := previous.duplicate()
	var max_sparkle_pixels := 0
	var total_sparkle_pixels := 0
	var sampled_pixels := 0
	var last := previous
	for frame in range(1, _sparkle_frames):
		if motion_scene:
			if _camera_pan:
				_animate_camera(_scene_frame)
			_animate_specular_objects(_scene_frame)
		await _wait_render_frame()
		_scene_frame += 1
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		var frame_sparkles := _count_temporal_sparkles(previous, current)
		max_sparkle_pixels = maxi(max_sparkle_pixels, frame_sparkles)
		total_sparkle_pixels += frame_sparkles
		sampled_pixels = maxi(sampled_pixels, (current.get_width() / 2) * (current.get_height() / 2))
		previous = current
		last = current
	var first_path := "%s/%s_sparkle_first.png" % [_output_dir, base_name]
	var last_path := "%s/%s_sparkle_last.png" % [_output_dir, base_name]
	first.save_png(first_path)
	last.save_png(last_path)
	var metrics := {
		"temporal_sparkle_pixels_max": max_sparkle_pixels,
		"temporal_sparkle_pixels_avg": float(total_sparkle_pixels) / maxf(float(_sparkle_frames - 1), 1.0),
		"temporal_sparkle_per_megapixel_max": _per_megapixel(max_sparkle_pixels, sampled_pixels),
		"temporal_sparkle_per_megapixel_avg": _per_megapixel(total_sparkle_pixels, sampled_pixels * maxi(_sparkle_frames - 1, 1)),
	}
	if _scene_mode == "specular_stability":
		metrics.merge(await _measure_specular_temporal_metrics(base_name), true)
		_apply_debug_view("beauty")
	return metrics


func _measure_convergence_curve(base_name: String) -> Dictionary:
	_apply_debug_view("beauty")
	var previous_enabled := false
	if _environment != null:
		previous_enabled = _environment.rtgi_enabled
		_environment.rtgi_enabled = false
		await _wait_render_frame()
		_environment.rtgi_enabled = previous_enabled
	var previous: Image = null
	var first: Image = null
	var last: Image = null
	var curve := []
	for frame in range(_convergence_frames):
		await _wait_render_frame()
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		if first == null:
			first = current.duplicate()
		last = current.duplicate()
		var frame_metrics := _measure_image(current)
		var sampled_pixels := int(current.get_width() / 2) * int(current.get_height() / 2)
		var delta_sparkles := 0
		if previous != null:
			delta_sparkles = _count_temporal_sparkles(previous, current)
		curve.append({
			"frame": frame,
			"luma_p95": frame_metrics["luma_p95"],
			"luma_p99": frame_metrics["luma_p99"],
			"luma_max": frame_metrics["luma_max"],
			"full_frame_fireflies_per_megapixel": frame_metrics["full_frame_fireflies_per_megapixel"],
			"visible_speckles_per_megapixel": frame_metrics["visible_speckles_per_megapixel"],
			"frame_delta_sparkle_per_megapixel": _per_megapixel(delta_sparkles, sampled_pixels),
		})
		previous = current
	var first_path := "%s/%s_convergence_first.png" % [_output_dir, base_name]
	var last_path := "%s/%s_convergence_last.png" % [_output_dir, base_name]
	if first != null:
		first.save_png(first_path)
	if last != null:
		last.save_png(last_path)
	var curve_path := "%s/%s_convergence_curve.json" % [_output_dir, base_name]
	var payload := {
		"frames": _convergence_frames,
		"curve": curve,
		"first_path": ProjectSettings.globalize_path(first_path),
		"last_path": ProjectSettings.globalize_path(last_path),
	}
	_write_json(curve_path, payload)
	return {
		"convergence_curve_path": ProjectSettings.globalize_path(curve_path),
		"convergence_first_path": ProjectSettings.globalize_path(first_path),
		"convergence_last_path": ProjectSettings.globalize_path(last_path),
		"convergence_last_luma_p99": curve.back()["luma_p99"] if not curve.is_empty() else 0.0,
		"convergence_last_fireflies_per_megapixel": curve.back()["full_frame_fireflies_per_megapixel"] if not curve.is_empty() else 0.0,
		"convergence_last_frame_delta_sparkle_per_megapixel": curve.back()["frame_delta_sparkle_per_megapixel"] if not curve.is_empty() else 0.0,
	}


func _measure_specular_temporal_metrics(base_name: String) -> Dictionary:
	var previous: Image = null
	var previous_reflection_direction: Image = null
	var first: Image = null
	var last: Image = null
	var all_deltas: Array[float] = []
	var all_edge_deltas: Array[float] = []
	var all_reflection_direction_deltas: Array[float] = []
	var all_luma: Array[float] = []
	var bin_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var bin_reflection_direction_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var total_specular_samples := 0
	var total_sampled_pixels := 0
	var mirror_samples := 0
	var glossy_samples := 0
	var rough_samples := 0
	var rejection_samples := 0
	var rejection_pixels := 0
	var rejection_sum := 0.0
	var mirror_rejection_samples := 0
	var mirror_rejection_pixels := 0
	var mirror_rejection_sum := 0.0
	var glossy_rejection_samples := 0
	var glossy_rejection_pixels := 0
	var glossy_rejection_sum := 0.0
	var max_sparkle_pixels := 0
	var total_sparkle_pixels := 0
	var max_fireflies_per_mp := 0.0
	var max_speckles_per_mp := 0.0
	for frame in range(_sparkle_frames):
		var scene_frame := _warmup_frames + _sparkle_frames + frame
		if _camera_pan:
			_animate_camera(scene_frame)
		_animate_specular_objects(scene_frame)
		_apply_debug_view("specular_final")
		await _wait_render_frame()
		var current := get_viewport().get_texture().get_image()
		current.convert(Image.FORMAT_RGBA8)
		if first == null:
			first = current.duplicate()
		last = current.duplicate()
		_apply_debug_view("specular_roughness_bucket")
		await _wait_render_frame()
		var normal_roughness := get_viewport().get_texture().get_image()
		normal_roughness.convert(Image.FORMAT_RGBA8)
		_apply_debug_view("specular_rejection")
		await _wait_render_frame()
		var rejection := get_viewport().get_texture().get_image()
		rejection.convert(Image.FORMAT_RGBA8)
		_apply_debug_view("specular_reflection_direction")
		await _wait_render_frame()
		var reflection_direction := get_viewport().get_texture().get_image()
		reflection_direction.convert(Image.FORMAT_RGBA8)
		var surface_metrics := _measure_specular_surface_metrics(current, normal_roughness)
		var rejection_metrics := _measure_specular_history_rejection_metrics(rejection, normal_roughness)
		total_specular_samples += int(surface_metrics["specular_samples"])
		total_sampled_pixels += int(surface_metrics["sampled_pixels"])
		mirror_samples += int(surface_metrics["mirror_samples"])
		glossy_samples += int(surface_metrics["glossy_samples"])
		rough_samples += int(surface_metrics["rough_samples"])
		rejection_samples += int(rejection_metrics["specular_samples"])
		rejection_pixels += int(rejection_metrics["rejection_pixels"])
		rejection_sum += float(rejection_metrics["rejection_sum"])
		mirror_rejection_samples += int(rejection_metrics["mirror_samples"])
		mirror_rejection_pixels += int(rejection_metrics["mirror_rejection_pixels"])
		mirror_rejection_sum += float(rejection_metrics["mirror_rejection_sum"])
		glossy_rejection_samples += int(rejection_metrics["glossy_samples"])
		glossy_rejection_pixels += int(rejection_metrics["glossy_rejection_pixels"])
		glossy_rejection_sum += float(rejection_metrics["glossy_rejection_sum"])
		max_fireflies_per_mp = maxf(max_fireflies_per_mp, surface_metrics["highlight_fireflies_per_mp"])
		max_speckles_per_mp = maxf(max_speckles_per_mp, surface_metrics["visible_speckles_per_mp"])
		all_luma.append_array(surface_metrics["luma_values"])
		if previous != null:
			var delta_metrics := _measure_specular_delta_metrics(previous, current, normal_roughness)
			all_deltas.append_array(delta_metrics["deltas"])
			all_edge_deltas.append_array(delta_metrics["edge_deltas"])
			for key in bin_deltas.keys():
				bin_deltas[key].append_array(delta_metrics["bin_deltas"][key])
			max_sparkle_pixels = maxi(max_sparkle_pixels, int(delta_metrics["sparkle_pixels"]))
			total_sparkle_pixels += int(delta_metrics["sparkle_pixels"])
		if previous_reflection_direction != null:
			var direction_metrics := _measure_specular_reflection_direction_metrics(previous_reflection_direction, reflection_direction, normal_roughness)
			all_reflection_direction_deltas.append_array(direction_metrics["deltas"])
			for key in bin_reflection_direction_deltas.keys():
				bin_reflection_direction_deltas[key].append_array(direction_metrics["bin_deltas"][key])
		previous = current
		previous_reflection_direction = reflection_direction
	if first != null:
		first.save_png("%s/%s_specular_temporal_first.png" % [_output_dir, base_name])
	if last != null:
		last.save_png("%s/%s_specular_temporal_last.png" % [_output_dir, base_name])
	all_deltas.sort()
	all_edge_deltas.sort()
	all_reflection_direction_deltas.sort()
	all_luma.sort()
	var pair_count := maxi(_sparkle_frames - 1, 1)
	var metrics := {
		"rtgi_specular_temporal_sparkle_per_mp": _per_megapixel(total_sparkle_pixels, total_specular_samples),
		"rtgi_specular_temporal_sparkle_max_per_mp": _per_megapixel(max_sparkle_pixels, maxi(int(ceil(float(total_specular_samples) / float(pair_count))), 1)),
		"rtgi_specular_temporal_delta_p95": _percentile_sorted(all_deltas, 0.95),
		"rtgi_specular_temporal_delta_p99": _percentile_sorted(all_deltas, 0.99),
		"rtgi_specular_highlight_fireflies_per_mp": max_fireflies_per_mp,
		"rtgi_specular_visible_speckles_per_mp": max_speckles_per_mp,
		"rtgi_specular_reflection_edge_delta_p95": _percentile_sorted(all_edge_deltas, 0.95),
		"rtgi_specular_reflection_edge_delta_p99": _percentile_sorted(all_edge_deltas, 0.99),
		"rtgi_specular_reflection_direction_delta_p95": _percentile_sorted(all_reflection_direction_deltas, 0.95),
		"rtgi_specular_reflection_direction_delta_p99": _percentile_sorted(all_reflection_direction_deltas, 0.99),
		"rtgi_specular_luma_p95": _percentile_sorted(all_luma, 0.95),
		"rtgi_specular_luma_p99": _percentile_sorted(all_luma, 0.99),
		"rtgi_specular_luma_max": all_luma.back() if not all_luma.is_empty() else 0.0,
		"rtgi_specular_pixel_fraction": float(total_specular_samples) / maxf(float(total_sampled_pixels), 1.0),
		"rtgi_specular_mirror_pixel_fraction": float(mirror_samples) / maxf(float(total_sampled_pixels), 1.0),
		"rtgi_specular_glossy_pixel_fraction": float(glossy_samples) / maxf(float(total_sampled_pixels), 1.0),
		"rtgi_specular_rough_pixel_fraction": float(rough_samples) / maxf(float(total_sampled_pixels), 1.0),
		"rtgi_specular_history_rejection_fraction": float(rejection_pixels) / maxf(float(rejection_samples), 1.0),
		"rtgi_specular_history_rejection_mean": rejection_sum / maxf(float(rejection_samples), 1.0),
		"rtgi_specular_mirror_history_rejection_fraction": float(mirror_rejection_pixels) / maxf(float(mirror_rejection_samples), 1.0),
		"rtgi_specular_mirror_history_rejection_mean": mirror_rejection_sum / maxf(float(mirror_rejection_samples), 1.0),
		"rtgi_specular_glossy_history_rejection_fraction": float(glossy_rejection_pixels) / maxf(float(glossy_rejection_samples), 1.0),
		"rtgi_specular_glossy_history_rejection_mean": glossy_rejection_sum / maxf(float(glossy_rejection_samples), 1.0),
	}
	for key in bin_deltas.keys():
		var values: Array = bin_deltas[key]
		values.sort()
		metrics["rtgi_specular_roughness_%s_delta_p99" % key] = _percentile_sorted(values, 0.99)
		var direction_values: Array = bin_reflection_direction_deltas[key]
		direction_values.sort()
		metrics["rtgi_specular_roughness_%s_reflection_direction_delta_p99" % key] = _percentile_sorted(direction_values, 0.99)
	metrics.merge(await _measure_specular_reflected_hit_distance_temporal_metrics(), true)
	metrics.merge(await _measure_specular_reflected_hit_normal_temporal_metrics(), true)
	return metrics


func _measure_specular_reflected_hit_distance_temporal_metrics() -> Dictionary:
	var previous_reflected_hit_distance: Image = null
	var all_reflected_hit_distance_deltas: Array[float] = []
	var bin_reflected_hit_distance_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var reflected_hit_distance_disocclusion_pixels := 0
	var reflected_hit_distance_valid_mismatch_pixels := 0
	var reflected_hit_distance_valid_pairs := 0
	var reflected_hit_distance_samples := 0
	for frame in range(_sparkle_frames):
		var scene_frame := _warmup_frames + _sparkle_frames + frame
		if _camera_pan:
			_animate_camera(scene_frame)
		_animate_specular_objects(scene_frame)
		_apply_debug_view("specular_roughness_bucket")
		await _wait_render_frame()
		var normal_roughness := get_viewport().get_texture().get_image()
		normal_roughness.convert(Image.FORMAT_RGBA8)
		_apply_debug_view("specular_reflected_hit_distance")
		await _wait_render_frame()
		var reflected_hit_distance := get_viewport().get_texture().get_image()
		reflected_hit_distance.convert(Image.FORMAT_RGBA8)
		if previous_reflected_hit_distance != null:
			var hit_distance_metrics := _measure_specular_reflected_hit_distance_metrics(previous_reflected_hit_distance, reflected_hit_distance, normal_roughness)
			all_reflected_hit_distance_deltas.append_array(hit_distance_metrics["deltas"])
			reflected_hit_distance_disocclusion_pixels += int(hit_distance_metrics["disocclusion_pixels"])
			reflected_hit_distance_valid_mismatch_pixels += int(hit_distance_metrics["valid_mismatch_pixels"])
			reflected_hit_distance_valid_pairs += int(hit_distance_metrics["valid_pairs"])
			reflected_hit_distance_samples += int(hit_distance_metrics["samples"])
			for key in bin_reflected_hit_distance_deltas.keys():
				bin_reflected_hit_distance_deltas[key].append_array(hit_distance_metrics["bin_deltas"][key])
		previous_reflected_hit_distance = reflected_hit_distance
	all_reflected_hit_distance_deltas.sort()
	var metrics := {
		"rtgi_specular_reflected_hit_distance_delta_p95": _percentile_sorted(all_reflected_hit_distance_deltas, 0.95),
		"rtgi_specular_reflected_hit_distance_delta_p99": _percentile_sorted(all_reflected_hit_distance_deltas, 0.99),
		"rtgi_specular_disocclusion_pixel_fraction": float(reflected_hit_distance_disocclusion_pixels) / maxf(float(reflected_hit_distance_samples), 1.0),
		"rtgi_specular_reflected_hit_distance_valid_mismatch_fraction": float(reflected_hit_distance_valid_mismatch_pixels) / maxf(float(reflected_hit_distance_samples), 1.0),
		"rtgi_specular_reflected_hit_distance_valid_pair_fraction": float(reflected_hit_distance_valid_pairs) / maxf(float(reflected_hit_distance_samples), 1.0),
	}
	for key in bin_reflected_hit_distance_deltas.keys():
		var hit_distance_values: Array = bin_reflected_hit_distance_deltas[key]
		hit_distance_values.sort()
		metrics["rtgi_specular_roughness_%s_reflected_hit_distance_delta_p99" % key] = _percentile_sorted(hit_distance_values, 0.99)
	return metrics


func _measure_specular_reflected_hit_normal_temporal_metrics() -> Dictionary:
	var previous_reflected_hit_normal: Image = null
	var all_reflected_hit_normal_deltas: Array[float] = []
	var bin_reflected_hit_normal_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var reflected_hit_normal_valid_mismatch_pixels := 0
	var reflected_hit_normal_valid_pairs := 0
	var reflected_hit_normal_samples := 0
	for frame in range(_sparkle_frames):
		var scene_frame := _warmup_frames + _sparkle_frames * 2 + frame
		if _camera_pan:
			_animate_camera(scene_frame)
		_animate_specular_objects(scene_frame)
		_apply_debug_view("specular_roughness_bucket")
		await _wait_render_frame()
		var normal_roughness := get_viewport().get_texture().get_image()
		normal_roughness.convert(Image.FORMAT_RGBA8)
		_apply_debug_view("specular_reflected_hit_normal")
		await _wait_render_frame()
		var reflected_hit_normal := get_viewport().get_texture().get_image()
		reflected_hit_normal.convert(Image.FORMAT_RGBA8)
		if previous_reflected_hit_normal != null:
			var hit_normal_metrics := _measure_specular_reflected_hit_normal_metrics(previous_reflected_hit_normal, reflected_hit_normal, normal_roughness)
			all_reflected_hit_normal_deltas.append_array(hit_normal_metrics["deltas"])
			reflected_hit_normal_valid_mismatch_pixels += int(hit_normal_metrics["valid_mismatch_pixels"])
			reflected_hit_normal_valid_pairs += int(hit_normal_metrics["valid_pairs"])
			reflected_hit_normal_samples += int(hit_normal_metrics["samples"])
			for key in bin_reflected_hit_normal_deltas.keys():
				bin_reflected_hit_normal_deltas[key].append_array(hit_normal_metrics["bin_deltas"][key])
		previous_reflected_hit_normal = reflected_hit_normal
	all_reflected_hit_normal_deltas.sort()
	var metrics := {
		"rtgi_specular_reflected_hit_normal_delta_p95": _percentile_sorted(all_reflected_hit_normal_deltas, 0.95),
		"rtgi_specular_reflected_hit_normal_delta_p99": _percentile_sorted(all_reflected_hit_normal_deltas, 0.99),
		"rtgi_specular_reflected_hit_normal_valid_mismatch_fraction": float(reflected_hit_normal_valid_mismatch_pixels) / maxf(float(reflected_hit_normal_samples), 1.0),
		"rtgi_specular_reflected_hit_normal_valid_pair_fraction": float(reflected_hit_normal_valid_pairs) / maxf(float(reflected_hit_normal_samples), 1.0),
	}
	for key in bin_reflected_hit_normal_deltas.keys():
		var hit_normal_values: Array = bin_reflected_hit_normal_deltas[key]
		hit_normal_values.sort()
		metrics["rtgi_specular_roughness_%s_reflected_hit_normal_delta_p99" % key] = _percentile_sorted(hit_normal_values, 0.99)
	return metrics


func _measure_specular_history_rejection_metrics(rejection_image: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(rejection_image.get_width(), normal_roughness_image.get_width())
	var height := mini(rejection_image.get_height(), normal_roughness_image.get_height())
	var specular := 0
	var rejected := 0
	var rejection_sum := 0.0
	var mirror := 0
	var mirror_rejected := 0
	var mirror_rejection_sum := 0.0
	var glossy := 0
	var glossy_rejected := 0
	var glossy_rejection_sum := 0.0
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			if roughness > 0.60:
				continue
			specular += 1
			var rejection := rejection_image.get_pixel(x, y).r
			rejection_sum += rejection
			var is_rejected := rejection > 0.05
			if is_rejected:
				rejected += 1
			if roughness <= 0.05:
				mirror += 1
				mirror_rejection_sum += rejection
				if is_rejected:
					mirror_rejected += 1
			elif roughness <= 0.30:
				glossy += 1
				glossy_rejection_sum += rejection
				if is_rejected:
					glossy_rejected += 1
	return {
		"specular_samples": specular,
		"rejection_pixels": rejected,
		"rejection_sum": rejection_sum,
		"mirror_samples": mirror,
		"mirror_rejection_pixels": mirror_rejected,
		"mirror_rejection_sum": mirror_rejection_sum,
		"glossy_samples": glossy,
		"glossy_rejection_pixels": glossy_rejected,
		"glossy_rejection_sum": glossy_rejection_sum,
	}


func _measure_specular_surface_metrics(specular_image: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(specular_image.get_width(), normal_roughness_image.get_width())
	var height := mini(specular_image.get_height(), normal_roughness_image.get_height())
	var sampled := 0
	var specular := 0
	var mirror := 0
	var glossy := 0
	var rough := 0
	var fireflies := 0
	var speckles := 0
	var luma_values: Array[float] = []
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			sampled += 1
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			if roughness > 0.60:
				continue
			specular += 1
			if roughness <= 0.05:
				mirror += 1
			elif roughness <= 0.30:
				glossy += 1
			else:
				rough += 1
			var center := _luma(specular_image.get_pixel(x, y))
			luma_values.append(center)
			var neighbor := _local_neighbor_mean_luma(specular_image, x, y)
			var range := _local_neighbor_luma_range(specular_image, x, y)
			if center > maxf(neighbor * 3.0 + 0.030, 0.10):
				fireflies += 1
			if center > maxf(neighbor * 2.0 + 0.020, range * 1.55 + 0.030):
				speckles += 1
	return {
		"sampled_pixels": sampled,
		"specular_samples": specular,
		"mirror_samples": mirror,
		"glossy_samples": glossy,
		"rough_samples": rough,
		"highlight_fireflies_per_mp": _per_megapixel(fireflies, specular),
		"visible_speckles_per_mp": _per_megapixel(speckles, specular),
		"luma_values": luma_values,
	}


func _measure_specular_delta_metrics(previous: Image, current: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(mini(previous.get_width(), current.get_width()), normal_roughness_image.get_width())
	var height := mini(mini(previous.get_height(), current.get_height()), normal_roughness_image.get_height())
	var deltas: Array[float] = []
	var edge_deltas: Array[float] = []
	var bin_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var sparkles := 0
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			var prev_luma := _luma(previous.get_pixel(x, y))
			var curr_luma := _luma(current.get_pixel(x, y))
			var delta := absf(curr_luma - prev_luma)
			if roughness <= 0.60:
				deltas.append(delta)
				var support := maxf(prev_luma, curr_luma)
				if support > 0.04 and delta > maxf(0.035, support * 0.36):
					sparkles += 1
				if maxf(_local_neighbor_luma_range(previous, x, y), _local_neighbor_luma_range(current, x, y)) > 0.055:
					edge_deltas.append(delta)
			var bucket := _roughness_delta_bucket(roughness)
			if not bucket.is_empty():
				bin_deltas[bucket].append(delta)
	return {
		"deltas": deltas,
		"edge_deltas": edge_deltas,
		"bin_deltas": bin_deltas,
		"sparkle_pixels": sparkles,
	}


func _measure_specular_reflection_direction_metrics(previous: Image, current: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(mini(previous.get_width(), current.get_width()), normal_roughness_image.get_width())
	var height := mini(mini(previous.get_height(), current.get_height()), normal_roughness_image.get_height())
	var deltas: Array[float] = []
	var bin_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			if roughness > 0.60:
				continue
			var prev_dir := _reflection_direction_from_pixel(previous.get_pixel(x, y))
			var curr_dir := _reflection_direction_from_pixel(current.get_pixel(x, y))
			var delta := 1.0 - clampf(prev_dir.dot(curr_dir), -1.0, 1.0)
			deltas.append(delta)
			var bucket := _roughness_delta_bucket(roughness)
			if not bucket.is_empty():
				bin_deltas[bucket].append(delta)
	return {
		"deltas": deltas,
		"bin_deltas": bin_deltas,
	}


func _measure_specular_reflected_hit_distance_metrics(previous: Image, current: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(mini(previous.get_width(), current.get_width()), normal_roughness_image.get_width())
	var height := mini(mini(previous.get_height(), current.get_height()), normal_roughness_image.get_height())
	var deltas: Array[float] = []
	var bin_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var disocclusion_pixels := 0
	var valid_mismatch_pixels := 0
	var valid_pairs := 0
	var samples := 0
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			if roughness > 0.60:
				continue
			var prev_pixel := previous.get_pixel(x, y)
			var curr_pixel := current.get_pixel(x, y)
			var prev_valid := _reflected_hit_distance_pixel_valid(prev_pixel)
			var curr_valid := _reflected_hit_distance_pixel_valid(curr_pixel)
			samples += 1
			if prev_valid != curr_valid:
				valid_mismatch_pixels += 1
				continue
			if not prev_valid:
				continue
			var prev_hit := _hit_distance_from_debug_pixel(prev_pixel)
			var curr_hit := _hit_distance_from_debug_pixel(curr_pixel)
			var delta: float = absf(curr_hit - prev_hit)
			deltas.append(delta)
			valid_pairs += 1
			if delta > 0.25:
				disocclusion_pixels += 1
			var bucket := _roughness_delta_bucket(roughness)
			if not bucket.is_empty():
				bin_deltas[bucket].append(delta)
	return {
		"deltas": deltas,
		"bin_deltas": bin_deltas,
		"disocclusion_pixels": disocclusion_pixels,
		"valid_mismatch_pixels": valid_mismatch_pixels,
		"valid_pairs": valid_pairs,
		"samples": samples,
	}


func _measure_specular_reflected_hit_normal_metrics(previous: Image, current: Image, normal_roughness_image: Image) -> Dictionary:
	var width := mini(mini(previous.get_width(), current.get_width()), normal_roughness_image.get_width())
	var height := mini(mini(previous.get_height(), current.get_height()), normal_roughness_image.get_height())
	var deltas: Array[float] = []
	var bin_deltas := {
		"00_05": [],
		"05_15": [],
		"15_30": [],
		"30_60": [],
		"60_100": [],
	}
	var valid_mismatch_pixels := 0
	var valid_pairs := 0
	var samples := 0
	for y in range(2, height - 2, 2):
		for x in range(2, width - 2, 2):
			var roughness := _roughness_from_normal_roughness_pixel(normal_roughness_image.get_pixel(x, y))
			if roughness > 0.60:
				continue
			var prev_pixel := previous.get_pixel(x, y)
			var curr_pixel := current.get_pixel(x, y)
			var prev_valid := _reflected_hit_normal_pixel_valid(prev_pixel)
			var curr_valid := _reflected_hit_normal_pixel_valid(curr_pixel)
			samples += 1
			if prev_valid != curr_valid:
				valid_mismatch_pixels += 1
				continue
			if not prev_valid:
				continue
			var prev_normal := _reflection_direction_from_pixel(prev_pixel)
			var curr_normal := _reflection_direction_from_pixel(curr_pixel)
			var delta := 1.0 - clampf(prev_normal.dot(curr_normal), -1.0, 1.0)
			deltas.append(delta)
			valid_pairs += 1
			var bucket := _roughness_delta_bucket(roughness)
			if not bucket.is_empty():
				bin_deltas[bucket].append(delta)
	return {
		"deltas": deltas,
		"bin_deltas": bin_deltas,
		"valid_mismatch_pixels": valid_mismatch_pixels,
		"valid_pairs": valid_pairs,
		"samples": samples,
	}


func _measure_sponza_image(image: Image) -> Dictionary:
	var width := image.get_width()
	var height := image.get_height()
	var corridor_detail := _measure_detail_region(image, int(width * 0.16), int(height * 0.18), int(width * 0.84), int(height * 0.84))
	var deep_shadow_fireflies := _count_isolated_hot_pixels(image, int(width * 0.08), int(height * 0.12), int(width * 0.92), int(height * 0.90))
	return {
		"sponza_asset_path": _resolve_sponza_path(),
		"sponza_external_asset_missing": not _sponza_asset_loaded,
		"sponza_khronos_source_url": SPONZA_KHRONOS_SOURCE_URL,
		"sponza_source": "khronos_gltf_sample_assets" if _is_khronos_sponza_path(_resolve_sponza_path()) else "external",
		"sponza_canonical_asset": _is_khronos_sponza_path(_resolve_sponza_path()),
		"sponza_normal_y_mode": _sponza_normal_y_mode,
		"sponza_normal_y_flipped": _sponza_normal_y_flipped,
		"sponza_flipped_normal_texture_count": _sponza_flipped_normal_texture_count,
		"sponza_corridor_edge_energy": corridor_detail["edge_energy"],
		"sponza_corridor_luma_stddev": corridor_detail["luma_stddev"],
		"sponza_deep_shadow_fireflies": deep_shadow_fireflies,
	}


func _compare_cornell_reference(rendered_image: Image, base_name: String) -> Dictionary:
	var reference_path := _resolve_cornell_reference_path()
	var metrics := {
		"cornell_reference_available": false,
		"cornell_reference_path": reference_path,
		"cornell_reference_url": CORNELL_REFERENCE_URL,
		"cornell_public_data_url": CORNELL_PUBLIC_DATA_URL,
	}
	if reference_path.is_empty() or not FileAccess.file_exists(reference_path):
		push_warning("Cornell reference image was not found. Place Cornell's simulated image at '%s' or pass --rtgi-cornell-reference-image=<path>." % reference_path)
		return metrics

	var reference := Image.new()
	var load_error := reference.load(reference_path)
	if load_error != OK:
		push_warning("Could not load Cornell reference image '%s'." % reference_path)
		return metrics

	var rendered := rendered_image.duplicate()
	rendered.convert(Image.FORMAT_RGBA8)
	reference.convert(Image.FORMAT_RGBA8)
	if reference.get_width() != rendered.get_width() or reference.get_height() != rendered.get_height():
		reference.resize(rendered.get_width(), rendered.get_height(), Image.INTERPOLATE_LANCZOS)

	metrics.merge(_measure_image_difference(rendered, reference, "cornell_reference"), true)
	metrics["cornell_reference_available"] = true
	_write_cornell_reference_images(rendered, reference, base_name)
	return metrics


func _measure_image_difference(rendered: Image, reference: Image, prefix: String) -> Dictionary:
	var width := mini(rendered.get_width(), reference.get_width())
	var height := mini(rendered.get_height(), reference.get_height())
	var rgb_abs_sum := 0.0
	var rgb_sq_sum := 0.0
	var luma_abs_sum := 0.0
	var luma_sq_sum := 0.0
	var max_luma_error := 0.0
	var count := maxi(width * height, 1)
	for y in range(height):
		for x in range(width):
			var a := rendered.get_pixel(x, y)
			var b := reference.get_pixel(x, y)
			var dr := a.r - b.r
			var dg := a.g - b.g
			var db := a.b - b.b
			var rgb_abs := (absf(dr) + absf(dg) + absf(db)) / 3.0
			var rgb_sq := (dr * dr + dg * dg + db * db) / 3.0
			var dl := _luma(a) - _luma(b)
			rgb_abs_sum += rgb_abs
			rgb_sq_sum += rgb_sq
			luma_abs_sum += absf(dl)
			luma_sq_sum += dl * dl
			max_luma_error = maxf(max_luma_error, absf(dl))
	return {
		"%s_rgb_mae" % prefix: rgb_abs_sum / float(count),
		"%s_rgb_rmse" % prefix: sqrt(rgb_sq_sum / float(count)),
		"%s_luma_mae" % prefix: luma_abs_sum / float(count),
		"%s_luma_rmse" % prefix: sqrt(luma_sq_sum / float(count)),
		"%s_luma_max_error" % prefix: max_luma_error,
	}


func _write_cornell_reference_images(rendered: Image, reference: Image, base_name: String) -> void:
	var width := rendered.get_width()
	var height := rendered.get_height()
	var diff := Image.create_empty(width, height, false, Image.FORMAT_RGBA8)
	for y in range(height):
		for x in range(width):
			var a := rendered.get_pixel(x, y)
			var b := reference.get_pixel(x, y)
			var error := clampf(absf(_luma(a) - _luma(b)) * 4.0, 0.0, 1.0)
			diff.set_pixel(x, y, Color(error, error * 0.35, 1.0 - error, 1.0))
	var reference_path := "%s/%s_cornell_reference_resized.png" % [_output_dir, base_name]
	var diff_path := "%s/%s_cornell_reference_diff.png" % [_output_dir, base_name]
	var grid_path := "%s/%s_cornell_reference_grid.png" % [_output_dir, base_name]
	reference.save_png(reference_path)
	diff.save_png(diff_path)
	var grid := Image.create_empty(width * 3, height, false, Image.FORMAT_RGBA8)
	grid.fill(Color.BLACK)
	grid.blit_rect(rendered, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i.ZERO)
	grid.blit_rect(reference, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i(width, 0))
	grid.blit_rect(diff, Rect2i(Vector2i.ZERO, Vector2i(width, height)), Vector2i(width * 2, 0))
	grid.save_png(grid_path)


func _count_isolated_hot_pixels(image: Image, x0: int, y0: int, x1: int, y1: int) -> int:
	var count := 0
	for y in range(max(y0, 1), min(y1, image.get_height() - 1)):
		for x in range(max(x0, 1), min(x1, image.get_width() - 1)):
			var center := _luma(image.get_pixel(x, y))
			var neighbor_sum := 0.0
			var neighbor_count := 0
			for ny in range(-1, 2):
				for nx in range(-1, 2):
					if nx == 0 and ny == 0:
						continue
					neighbor_sum += _luma(image.get_pixel(x + nx, y + ny))
					neighbor_count += 1
			var neighbor_mean := neighbor_sum / float(neighbor_count)
			if center > max(0.16, neighbor_mean * 4.0 + 0.035):
				count += 1
	return count


func _count_visible_speckles(image: Image, x0: int, y0: int, x1: int, y1: int) -> int:
	var count := 0
	for y in range(max(y0, 2), min(y1, image.get_height() - 2), 2):
		for x in range(max(x0, 2), min(x1, image.get_width() - 2), 2):
			var center := _luma(image.get_pixel(x, y))
			var neighbor_sum := 0.0
			var neighbor_max := 0.0
			var neighbor_count := 0
			for ny in range(-2, 3):
				for nx in range(-2, 3):
					if nx == 0 and ny == 0:
						continue
					var tap_luma := _luma(image.get_pixel(x + nx, y + ny))
					neighbor_sum += tap_luma
					neighbor_max = maxf(neighbor_max, tap_luma)
					neighbor_count += 1
			var neighbor_mean := neighbor_sum / maxf(float(neighbor_count), 1.0)
			var local_threshold := maxf(0.045, maxf(neighbor_mean * 2.15 + 0.018, neighbor_max * 1.28 + 0.006))
			if center > local_threshold:
				count += 1
	return count


func _count_temporal_sparkles(previous: Image, current: Image) -> int:
	var width := mini(previous.get_width(), current.get_width())
	var height := mini(previous.get_height(), current.get_height())
	var count := 0
	for y in range(1, height - 1, 2):
		for x in range(1, width - 1, 2):
			var prev_luma := _luma(previous.get_pixel(x, y))
			var curr_luma := _luma(current.get_pixel(x, y))
			var delta := absf(curr_luma - prev_luma)
			var support := maxf(prev_luma, curr_luma)
			if support <= 0.060 or delta <= maxf(0.055, support * 0.48):
				continue
			var prev_neighbor := _local_neighbor_mean_luma(previous, x, y)
			var curr_neighbor := _local_neighbor_mean_luma(current, x, y)
			var local_support := maxf(prev_neighbor, curr_neighbor)
			var local_edge_range := maxf(_local_neighbor_luma_range(previous, x, y), _local_neighbor_luma_range(current, x, y))
			if local_edge_range > 0.10 and delta < local_edge_range * 1.00:
				continue
			var prev_isolated := prev_luma > maxf(prev_neighbor * 1.65 + 0.020, prev_neighbor + 0.055)
			var curr_isolated := curr_luma > maxf(curr_neighbor * 1.65 + 0.020, curr_neighbor + 0.055)
			var dark_flash := local_support < 0.12 and delta > maxf(0.050, support * 0.42)
			if prev_isolated or curr_isolated or dark_flash:
				count += 1
	return count


func _sampled_grid_pixel_count(width: int, height: int, stride: int, margin: int) -> int:
	var sample_w := int(ceil(float(maxi(width - margin * 2, 1)) / float(maxi(stride, 1))))
	var sample_h := int(ceil(float(maxi(height - margin * 2, 1)) / float(maxi(stride, 1))))
	return maxi(sample_w * sample_h, 1)


func _local_neighbor_mean_luma(image: Image, x: int, y: int) -> float:
	var sum := 0.0
	var count := 0
	for ny in range(-1, 2):
		for nx in range(-1, 2):
			if nx == 0 and ny == 0:
				continue
			sum += _luma(image.get_pixel(x + nx, y + ny))
			count += 1
	return sum / maxf(float(count), 1.0)


func _local_neighbor_luma_range(image: Image, x: int, y: int) -> float:
	var min_luma := 1.0
	var max_luma := 0.0
	for ny in range(-1, 2):
		for nx in range(-1, 2):
			var luma := _luma(image.get_pixel(x + nx, y + ny))
			min_luma = minf(min_luma, luma)
			max_luma = maxf(max_luma, luma)
	return max_luma - min_luma


func _per_megapixel(count: int, pixels: int) -> float:
	return float(count) / maxf(float(pixels) / 1000000.0, 1e-5)


func _roughness_from_normal_roughness_pixel(pixel: Color) -> float:
	return clampf(pixel.r, 0.0, 1.0)


func _roughness_delta_bucket(roughness: float) -> String:
	if roughness <= 0.05:
		return "00_05"
	if roughness <= 0.15:
		return "05_15"
	if roughness <= 0.30:
		return "15_30"
	if roughness <= 0.60:
		return "30_60"
	if roughness <= 1.0:
		return "60_100"
	return ""


func _reflection_direction_from_pixel(pixel: Color) -> Vector3:
	var direction := Vector3(pixel.r * 2.0 - 1.0, pixel.g * 2.0 - 1.0, pixel.b * 2.0 - 1.0)
	var length := direction.length()
	if length <= 0.0001:
		return Vector3(0.0, 0.0, 1.0)
	return direction / length


func _hit_distance_from_debug_pixel(pixel: Color) -> float:
	return (pixel.r + pixel.g + pixel.b) / 3.0


func _reflected_hit_distance_pixel_valid(pixel: Color) -> bool:
	return maxf(maxf(pixel.r, pixel.g), pixel.b) > 0.001


func _reflected_hit_normal_pixel_valid(pixel: Color) -> bool:
	return maxf(maxf(pixel.r, pixel.g), pixel.b) > 0.001


func _percentile_sorted(values: Array, percentile: float) -> float:
	if values.is_empty():
		return 0.0
	var idx := clampi(int(round(float(values.size() - 1) * percentile)), 0, values.size() - 1)
	return float(values[idx])


func _measure_detail_region(image: Image, x0: int, y0: int, x1: int, y1: int) -> Dictionary:
	var edge_sum := 0.0
	var edge_count := 0
	var luma_sum := 0.0
	var luma_sq_sum := 0.0
	var count := 0
	for y in range(y0, min(y1, image.get_height() - 1)):
		for x in range(x0, min(x1, image.get_width() - 1)):
			var luma := _luma(image.get_pixel(x, y))
			var luma_x := _luma(image.get_pixel(x + 1, y))
			var luma_y := _luma(image.get_pixel(x, y + 1))
			edge_sum += absf(luma - luma_x) + absf(luma - luma_y)
			edge_count += 2
			luma_sum += luma
			luma_sq_sum += luma * luma
			count += 1
	var mean: float = luma_sum / maxf(float(count), 1.0)
	return {
		"edge_energy": edge_sum / max(float(edge_count), 1.0),
		"luma_stddev": sqrt(max(luma_sq_sum / max(float(count), 1.0) - mean * mean, 0.0)),
	}


func _measure_mean_luma(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			sum += _luma(image.get_pixel(x, y))
			count += 1
	return sum / max(float(count), 1.0)


# Mean luma of the rect in LINEAR space: each pixel is decoded from the capture's
# sRGB encoding before the luma weighting, so a k-times change in rendered
# radiance reads as a k-times change here (the encoded mean above compresses it).
# Per-pixel decode, then mean: linearizing the encoded mean would be wrong, the
# transfer function does not commute with averaging. The proportionality claim
# also relies on the scene using a linear tonemapper (cornell_box pins it in
# the committed .tscn); a nonlinear tonemap would break it before the encode.
func _measure_mean_linear_luma(image: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			sum += _luma(image.get_pixel(x, y).srgb_to_linear())
			count += 1
	return sum / max(float(count), 1.0)


func _measure_luma_stats(image: Image) -> Dictionary:
	var values: Array[float] = []
	var max_luma := 0.0
	var step := 2
	for y in range(0, image.get_height(), step):
		for x in range(0, image.get_width(), step):
			if _is_intentional_emitter_metric_pixel(x, y, image.get_width(), image.get_height()):
				continue
			var luma := _luma(image.get_pixel(x, y))
			values.append(luma)
			max_luma = maxf(max_luma, luma)
	values.sort()
	if values.is_empty():
		return { "p95": 0.0, "p99": 0.0, "max": 0.0, "saturated_fraction": 0.0 }
	var p95_idx := clampi(int(round(float(values.size() - 1) * 0.95)), 0, values.size() - 1)
	var p99_idx := clampi(int(round(float(values.size() - 1) * 0.99)), 0, values.size() - 1)
	var saturated := 0
	for value in values:
		if value >= 0.985:
			saturated += 1
	return {
		"p95": values[p95_idx],
		"p99": values[p99_idx],
		"max": max_luma,
		"saturated_fraction": float(saturated) / maxf(float(values.size()), 1.0),
	}


func _is_intentional_emitter_metric_pixel(x: int, y: int, width: int, height: int) -> bool:
	var nx := float(x) / maxf(float(width), 1.0)
	var ny := float(y) / maxf(float(height), 1.0)
	if _scene_mode == "stress" or _scene_mode == "convergence":
		return nx >= 0.34 and nx <= 0.49 and ny >= 0.37 and ny <= 0.60
	if _scene_mode == "cornell":
		return nx >= 0.37 and nx <= 0.63 and ny >= 0.06 and ny <= 0.22
	return false


func _luma(color: Color) -> float:
	return max(color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722, 0.0)


func _load_expected_metrics() -> Dictionary:
	return _read_json_dictionary(EXPECTED_METRICS_PATH)


func _read_json_dictionary(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var parsed = JSON.parse_string(file.get_as_text())
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _expected_metrics_for_scene(expected: Dictionary) -> Dictionary:
	if _scene_mode == "sponza" and not _sponza_asset_loaded:
		return {}
	if expected.has("scenes"):
		var scenes = expected["scenes"]
		if typeof(scenes) == TYPE_DICTIONARY and scenes.has(_scene_mode):
			var scene_expected = scenes[_scene_mode]
			return scene_expected if typeof(scene_expected) == TYPE_DICTIONARY else {}
		if _scene_mode == "many_light_emissive":
			return {}
	return expected


func _compare_metrics(metrics: Dictionary, expected: Dictionary) -> Array[String]:
	var failures: Array[String] = []
	if _gate_profile == "strict" and _scene_mode == "cornell" and _cornell_compare and not bool(metrics.get("cornell_reference_available", false)):
		failures.append("cornell_reference_available is false; strict Cornell comparison requires the external reference image")
	var thresholds: Dictionary = expected.get("thresholds", {})
	_check_max_threshold(metrics, thresholds, "dark_fireflies_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "full_frame_fireflies_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "visible_speckles_per_megapixel", failures)
	_check_max_threshold(metrics, thresholds, "temporal_sparkle_per_megapixel_max", failures)
	_check_max_threshold(metrics, thresholds, "dark_roi_mean_luma", failures)
	_check_min_threshold(metrics, thresholds, "detail_edge_energy", failures)
	_check_min_threshold(metrics, thresholds, "detail_luma_stddev", failures)
	_check_max_threshold(metrics, thresholds, "specular_fireflies", failures)
	_check_min_threshold(metrics, thresholds, "specular_edge_energy", failures)
	_check_max_threshold(metrics, thresholds, "luma_p99", failures)
	_check_max_threshold(metrics, thresholds, "luma_max", failures)
	_check_max_threshold(metrics, thresholds, "saturated_luma_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_ceiling_hot_pixels", failures)
	_check_min_threshold(metrics, thresholds, "cornell_center_edge_energy", failures)
	_check_max_threshold(metrics, thresholds, "cornell_reference_luma_mae", failures)
	_check_max_threshold(metrics, thresholds, "cornell_black_fraction_luma_002", failures)
	_check_max_threshold(metrics, thresholds, "cornell_black_fraction_luma_005", failures)
	_check_max_threshold(metrics, thresholds, "cornell_red_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_green_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_back_wall_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_floor_black_fraction", failures)
	_check_max_threshold(metrics, thresholds, "cornell_flat_patch_noise", failures)
	_check_min_threshold(metrics, thresholds, "cornell_red_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_green_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_wall_min_luma", failures)
	_check_min_threshold(metrics, thresholds, "sponza_corridor_edge_energy", failures)
	_check_min_threshold(metrics, thresholds, "sponza_corridor_luma_stddev", failures)
	_check_max_threshold(metrics, thresholds, "sponza_deep_shadow_fireflies", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_rt_owner_luma", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_raster_owner_luma", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_rt_owner_blue_green_margin", failures)
	_check_min_threshold(metrics, thresholds, "coexistence_raster_owner_blue_green_margin", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_rt_owner_luma", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_owner_luma_ratio", failures)
	_check_max_threshold(metrics, thresholds, "coexistence_owner_luma_delta", failures)
	_check_min_threshold(metrics, thresholds, "cornell_box_red_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_box_green_wall_chroma_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_box_left_floor_red_bleed_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_box_right_floor_green_bleed_margin", failures)
	_check_min_threshold(metrics, thresholds, "cornell_box_floor_mean_luma", failures)
	_check_max_threshold(metrics, thresholds, "cornell_box_ceiling_hot_pixels", failures)
	_check_min_threshold(metrics, thresholds, "reflective_pool_reflection_edge_energy", failures)
	_check_min_threshold(metrics, thresholds, "reflective_pool_surface_mean_luma", failures)
	_check_max_threshold(metrics, thresholds, "reflective_pool_reflection_fireflies", failures)
	_check_max_threshold(metrics, thresholds, "light_grid_orbit_delta_avg", failures)
	_check_max_threshold(metrics, thresholds, "light_grid_orbit_sparkle_max", failures)
	_check_max_threshold(metrics, thresholds, "light_grid_static_tail_delta", failures)
	_check_max_threshold(metrics, thresholds, "light_grid_toggle_recovery_frames", failures)
	_check_max_threshold(metrics, thresholds, "light_grid_toggle_far_rect_delta", failures)
	_check_max_threshold(metrics, thresholds, "sun_penumbra_width_px", failures)
	_check_max_threshold(metrics, thresholds, "sun_penumbra_band_hf", failures)
	_check_max_threshold(metrics, thresholds, "sun_penumbra_lit_luma", failures)
	_check_max_threshold(metrics, thresholds, "sun_penumbra_occluded_luma", failures)
	return failures


func _check_max_threshold(metrics: Dictionary, thresholds: Dictionary, key: String, failures: Array[String]) -> void:
	if not thresholds.has("max_%s" % key):
		return
	if not metrics.has(key):
		failures.append("%s is missing; required max threshold %.6f was not measured" % [key, float(thresholds["max_%s" % key])])
		return
	var limit: float = thresholds["max_%s" % key]
	var actual: float = metrics[key]
	if actual > limit:
		failures.append("%s %.6f exceeds %.6f" % [key, actual, limit])


func _check_min_threshold(metrics: Dictionary, thresholds: Dictionary, key: String, failures: Array[String]) -> void:
	if not thresholds.has("min_%s" % key):
		return
	if not metrics.has(key):
		failures.append("%s is missing; required min threshold %.6f was not measured" % [key, float(thresholds["min_%s" % key])])
		return
	var limit: float = thresholds["min_%s" % key]
	var actual: float = metrics[key]
	if actual < limit:
		failures.append("%s %.6f below %.6f" % [key, actual, limit])


func _reference_from_metrics(metrics: Dictionary) -> Dictionary:
	return {
		"description": "Baseline thresholds for the RTGI quality harness. Regenerate intentionally after visual review.",
		"thresholds": {
			"max_dark_fireflies_per_megapixel": metrics["dark_fireflies_per_megapixel"] * 1.15 + 4.0,
			"max_dark_roi_mean_luma": metrics["dark_roi_mean_luma"] * 1.10 + 0.005,
			"min_detail_edge_energy": metrics["detail_edge_energy"] * 0.85,
			"min_detail_luma_stddev": metrics["detail_luma_stddev"] * 0.85,
			"max_specular_fireflies": metrics["specular_fireflies"] * 1.25 + 4.0,
			"min_specular_edge_energy": metrics["specular_edge_energy"] * 0.75,
			"max_luma_p99": metrics["luma_p99"] * 1.20 + 0.02,
			"max_luma_max": metrics["luma_max"] * 1.35 + 0.05,
		},
	}


func _write_json(path: String, payload: Dictionary) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("Could not write RTGI quality JSON: %s" % path)
		get_tree().quit(2)
		return
	file.store_string(JSON.stringify(payload, "\t"))


func _resolve_cornell_reference_path() -> String:
	if not _cornell_reference_image.is_empty():
		return _cornell_reference_image
	var env_path := OS.get_environment("GODOT_RTGI_CORNELL_REFERENCE")
	if not env_path.is_empty():
		return env_path
	return "D:/dev/rtgi_external_assets/cornell/simulated.jpg"


func _resolve_sponza_path() -> String:
	if not _sponza_path.is_empty():
		return _sponza_path
	var env_path := OS.get_environment("GODOT_RTGI_SPONZA_PATH")
	if not env_path.is_empty():
		return env_path
	var default_gltf := "D:/dev/rtgi_external_assets/sponza/Models/Sponza/glTF/Sponza.gltf"
	if FileAccess.file_exists(default_gltf):
		return default_gltf
	var flat_default_gltf := "D:/dev/rtgi_external_assets/sponza/Sponza.gltf"
	if FileAccess.file_exists(flat_default_gltf):
		return flat_default_gltf
	var default_glb := "D:/dev/rtgi_external_assets/sponza/Sponza.glb"
	if FileAccess.file_exists(default_glb):
		return default_glb
	return default_gltf


func _is_khronos_sponza_path(path: String) -> bool:
	return path.replace("\\", "/").ends_with(SPONZA_KHRONOS_CANONICAL_SUFFIX)


func _should_flip_sponza_normal_y(asset_path: String) -> bool:
	match _sponza_normal_y_mode:
		"directx":
			return true
		"opengl":
			return false
		_:
			# glTF normal maps are +Y tangent-space. The Khronos Sponza package notes
			# that its normals were manually inspected, so auto mode keeps them as-is.
			return false


func _flip_sponza_normal_maps(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		var mesh := mesh_instance.mesh
		if mesh != null:
			for surface_index in range(mesh.get_surface_count()):
				var material := mesh_instance.get_surface_override_material(surface_index)
				if material == null:
					material = mesh.surface_get_material(surface_index)
				var flipped_material := _material_with_flipped_normal_y(material)
				if flipped_material != null and flipped_material != material:
					mesh_instance.set_surface_override_material(surface_index, flipped_material)
	for child in node.get_children():
		_flip_sponza_normal_maps(child)


func _material_with_flipped_normal_y(material: Material) -> Material:
	if material == null or not (material is StandardMaterial3D):
		return material
	var standard_material := material as StandardMaterial3D
	var normal_texture := standard_material.get_texture(BaseMaterial3D.TEXTURE_NORMAL)
	if normal_texture == null:
		return material
	var flipped_texture := _flipped_normal_y_texture(normal_texture)
	if flipped_texture == normal_texture:
		return material
	var flipped_material := standard_material.duplicate(true) as StandardMaterial3D
	flipped_material.set_texture(BaseMaterial3D.TEXTURE_NORMAL, flipped_texture)
	flipped_material.set_feature(BaseMaterial3D.FEATURE_NORMAL_MAPPING, true)
	return flipped_material


func _flipped_normal_y_texture(texture: Texture2D) -> Texture2D:
	var key := texture.get_instance_id()
	if _normal_flip_cache.has(key):
		return _normal_flip_cache[key]
	var image := texture.get_image()
	if image == null or image.is_empty():
		return texture
	if image.is_compressed():
		var decompress_error := image.decompress()
		if decompress_error != OK:
			push_warning("Could not decompress Sponza normal texture for green-channel flip.")
			return texture
	image.convert(Image.FORMAT_RGBA8)
	var width := image.get_width()
	var height := image.get_height()
	var data := image.get_data()
	var index := 1
	while index < data.size():
		data[index] = 255 - data[index]
		index += 4
	image.set_data(width, height, false, Image.FORMAT_RGBA8, data)
	var flipped_texture := ImageTexture.create_from_image(image)
	_normal_flip_cache[key] = flipped_texture
	_sponza_flipped_normal_texture_count += 1
	return flipped_texture


func _mark_geometry_static(node: Node) -> void:
	if node is GeometryInstance3D:
		node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	for child in node.get_children():
		_mark_geometry_static(child)


func _cornell_point(x: float, y: float, z: float) -> Vector3:
	return Vector3(x, y, z) * 0.01


func _make_lambert_material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 1.0
	material.metallic = 0.0
	material.diffuse_mode = StandardMaterial3D.DIFFUSE_LAMBERT
	material.specular_mode = StandardMaterial3D.SPECULAR_DISABLED
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	return material


func _make_lambert_emissive_material(color: Color, energy: float) -> StandardMaterial3D:
	var material := _make_lambert_material(color)
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = energy
	return material


func _add_cornell_block(block_name: String, faces: Array, material: Material, block_center: Vector3) -> void:
	for i in range(faces.size()):
		var face: Array = faces[i]
		var center := Vector3.ZERO
		for point in face:
			center += point
		center /= 4.0
		_add_cornell_quad("%sFace%d" % [block_name, i], face, material, (center - block_center).normalized())


func _add_cornell_solid(node_name: String, points: Array, outward_normal: Vector3, thickness: float, material: Material) -> void:
	var front: Array = points.duplicate()
	var back: Array = []
	for point in front:
		back.append(point + outward_normal.normalized() * thickness)
	_add_cornell_quad("%sInner" % node_name, front, material, -outward_normal)
	_add_cornell_quad("%sOuter" % node_name, [back[3], back[2], back[1], back[0]], material, outward_normal)
	for i in range(4):
		var j := (i + 1) % 4
		_add_cornell_quad("%sEdge%d" % [node_name, i], [front[i], front[j], back[j], back[i]], material, (front[j] - front[i]).cross(outward_normal).normalized())


func _add_cornell_quad(node_name: String, points: Array, material: Material, desired_normal: Vector3) -> MeshInstance3D:
	var quad: Array = points.duplicate()
	if quad.size() != 4:
		push_error("Cornell quad '%s' does not have four vertices." % node_name)
		return null
	_orient_quad(quad, desired_normal)

	var normal := _quad_normal(quad)
	var vertices := PackedVector3Array([quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]])
	var normals := PackedVector3Array([normal, normal, normal, normal, normal, normal])
	var uvs := PackedVector2Array([
		Vector2(0.0, 0.0),
		Vector2(1.0, 0.0),
		Vector2(1.0, 1.0),
		Vector2(0.0, 0.0),
		Vector2(1.0, 1.0),
		Vector2(0.0, 1.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance


func _orient_quad(quad: Array, desired_normal: Vector3) -> void:
	var normal := _quad_normal(quad)
	if normal.dot(desired_normal.normalized()) < 0.0:
		var b = quad[1]
		quad[1] = quad[3]
		quad[3] = b


func _quad_normal(quad: Array) -> Vector3:
	return (quad[1] - quad[0]).cross(quad[2] - quad[0]).normalized()


func _make_brick_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_texture = _make_brick_texture()
	material.albedo_color = Color(1.0, 1.0, 1.0)
	material.roughness = 0.88
	material.metallic = 0.0
	material.uv1_scale = Vector3(5.0, 5.0, 1.0)
	return material


func _make_flat_material(color: Color, roughness: float, metallic: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	return material


func _make_emissive_material(color: Color, energy: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = energy
	material.roughness = 0.35
	return material


func _make_normal_mapped_glossy_material() -> StandardMaterial3D:
	var material := _make_flat_material(Color(0.40, 0.43, 0.47), 0.085, 0.0)
	material.set_feature(BaseMaterial3D.FEATURE_NORMAL_MAPPING, true)
	material.set_texture(BaseMaterial3D.TEXTURE_NORMAL, _make_wave_normal_texture())
	material.normal_scale = 1.0
	material.uv1_scale = Vector3(3.5, 3.5, 1.0)
	return material


func _make_shader_light_material() -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = "\n".join(PackedStringArray([
		"shader_type spatial;",
		"render_mode specular_schlick_ggx;",
		"void fragment() {",
		"	ALBEDO = vec3(0.18, 0.42, 0.72);",
		"	ROUGHNESS = 0.42;",
		"	METALLIC = 0.0;",
		"	EMISSION = vec3(0.0);",
		"}",
		"void light() {",
		"	DIFFUSE_LIGHT += ATTENUATION * LIGHT_COLOR * max(dot(NORMAL, LIGHT), 0.0) * vec3(0.2, 0.5, 1.0);",
		"}",
	]))
	var material := ShaderMaterial.new()
	material.shader = shader
	return material


func _make_brick_texture() -> ImageTexture:
	var image := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in range(256):
		for x in range(256):
			var row := int(y / 32)
			var offset := 32 if row % 2 == 1 else 0
			var local_x := (x + offset) % 64
			var local_y := y % 32
			var mortar := local_x < 3 or local_y < 3
			var seed := (x * 17 + y * 31 + (x * y) % 29) % 23
			var grain := float(seed) / 22.0
			var base := Color(0.47 + grain * 0.10, 0.29 + grain * 0.055, 0.16 + grain * 0.035)
			var color := Color(0.10, 0.085, 0.070) if mortar else base
			image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _make_wave_normal_texture() -> ImageTexture:
	var image := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in range(256):
		for x in range(256):
			var fx := float(x) / 255.0
			var fy := float(y) / 255.0
			var sx := sin(fx * TAU * 9.0) * 0.34 + sin((fx + fy) * TAU * 5.0) * 0.20
			var sy := cos(fy * TAU * 7.0) * 0.32 + sin((fx - fy) * TAU * 4.0) * 0.18
			var n := Vector3(sx, sy, 1.0).normalized()
			image.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0))
	return ImageTexture.create_from_image(image)


func _add_box(node_name: String, position: Vector3, size: Vector3, material: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.subdivide_width = 3
	mesh.subdivide_height = 3
	mesh.subdivide_depth = 3
	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance


func _add_sphere(node_name: String, position: Vector3, radius: float, material: Material) -> MeshInstance3D:
	var mesh := SphereMesh.new()
	mesh.radius = radius
	mesh.height = radius * 2.0
	mesh.radial_segments = 48
	mesh.rings = 24
	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = mesh
	instance.position = position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	instance.set_surface_override_material(0, material)
	add_child(instance)
	return instance
