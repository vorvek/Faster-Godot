// Environment fog helper for raytracing hit shaders.
// Requires: scene_data_inc.glsl, raytracing_inc.glsl, raytracing_lights_inc.glsl.

vec4 fog_process(SceneData scene_data, vec3 vertex) {
	vec3 fog_color = scene_data.fog_light_color;

#ifdef FOG_HAS_RADIANCE
	if (scene_data.fog_aerial_perspective > 0.0) {
		float mip_level = mix(1.0 / MAX_ROUGHNESS_LOD, 1.0, 1.0 - (abs(vertex.z) - scene_data.z_near) / (scene_data.z_far - scene_data.z_near));
		vec3 sky_fog_color = fog_sample_radiance(vertex, mip_level);
		fog_color = mix(fog_color, sky_fog_color, scene_data.fog_aerial_perspective);
	}
#endif

	if (scene_data.fog_sun_scatter > 0.001) {
		vec3 view = normalize(vertex);
		uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));

		for (uint i = 0u; i < light_count; i++) {
			if (rt_lights[i].type != RT_LIGHT_TYPE_DIRECTIONAL) {
				continue;
			}

			vec3 light_color = fog_get_directional_color(i);
			float light_amount = pow(max(dot(view, fog_get_directional_direction(i)), 0.0), 8.0);
			fog_color += light_color * light_amount * scene_data.fog_sun_scatter;
		}
	}

	float fog_amount;
	if ((RT_FLAGS & RT_FLAG_FOG_DEPTH_MODE) != 0u) {
		// Raster depth-fog parity: scene_forward_clustered.glsl:1140-1146 is the single source
		// of truth for this formula. fog_depth_begin/end/curve are already in SceneData.
		float fog_z = smoothstep(scene_data.fog_depth_begin, scene_data.fog_depth_end, length(vertex));
		fog_amount = pow(fog_z, scene_data.fog_depth_curve) * scene_data.fog_density;
	} else {
		fog_amount = 1.0 - exp(min(0.0, -length(vertex) * scene_data.fog_density));
	}
	// The RT consumers use fog.a to attenuate throughput; keep it a valid opacity even when
	// density > 1 pushes the depth formula past 1 (the raster's blend saturates implicitly).
	fog_amount = clamp(fog_amount, 0.0, 1.0);

	if (abs(scene_data.fog_height_density) >= 0.0001) {
		mat4 inv_view_matrix = transpose(mat4(
				scene_data.inv_view_matrix[0],
				scene_data.inv_view_matrix[1],
				scene_data.inv_view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));

		float y = (inv_view_matrix * vec4(vertex, 1.0)).y;
		float y_dist = y - scene_data.fog_height;
		float vfog_amount = 1.0 - exp(min(0.0, y_dist * scene_data.fog_height_density));
		fog_amount = max(vfog_amount, fog_amount);
	}

	return vec4(fog_color, fog_amount);
}
