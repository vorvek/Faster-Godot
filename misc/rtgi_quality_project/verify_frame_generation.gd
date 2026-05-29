extends SceneTree

func _init():
	print("--- Viewport Frame Generation Verification Script ---")
	
	var vp = SubViewport.new()
	if not vp:
		print("FAILED: Could not create SubViewport instance.")
		quit(1)
		return

	# 1. Test properties get/set
	print("Testing default property values...")
	if vp.frame_generation_mode != SubViewport.FRAME_GENERATION_DISABLED:
		print("FAILED: Default frame_generation_mode should be FRAME_GENERATION_DISABLED")
		quit(1)
		return
	print("Default mode is correct: ", vp.frame_generation_mode)

	if vp.frame_generation_warp_scale != 1.0:
		print("FAILED: Default frame_generation_warp_scale should be 1.0")
		quit(1)
		return
	print("Default warp_scale is correct: ", vp.frame_generation_warp_scale)

	if vp.frame_generation_target_fps != 0:
		print("FAILED: Default frame_generation_target_fps should be 0")
		quit(1)
		return
	print("Default target_fps is correct: ", vp.frame_generation_target_fps)

	# 2. Test modifying properties
	print("\nModifying property values...")
	vp.frame_generation_mode = SubViewport.FRAME_GENERATION_INTERPOLATED
	if vp.frame_generation_mode != SubViewport.FRAME_GENERATION_INTERPOLATED:
		print("FAILED: Could not set frame_generation_mode to FRAME_GENERATION_INTERPOLATED")
		quit(1)
		return
	print("Successfully set mode to: ", vp.frame_generation_mode)

	vp.frame_generation_warp_scale = 1.5
	if vp.frame_generation_warp_scale != 1.5:
		print("FAILED: Could not set frame_generation_warp_scale to 1.5")
		quit(1)
		return
	print("Successfully set warp_scale to: ", vp.frame_generation_warp_scale)

	vp.frame_generation_target_fps = 60
	if vp.frame_generation_target_fps != 60:
		print("FAILED: Could not set frame_generation_target_fps to 60")
		quit(1)
		return
	print("Successfully set target_fps to: ", vp.frame_generation_target_fps)

	# 3. Test monitoring APIs
	print("\nTesting viewport monitoring APIs (no-crash checks)...")
	var is_active = vp.is_frame_generation_active()
	var real_fps = vp.get_frame_generation_real_fps()
	var output_fps = vp.get_frame_generation_output_fps()
	var latency = vp.get_frame_generation_latency()

	print("is_active: ", is_active)
	print("real_fps: ", real_fps)
	print("output_fps: ", output_fps)
	print("latency: ", latency)

	print("\nSUCCESS: All Viewport Frame Generation property bindings and monitoring APIs verified successfully!")
	quit(0)
