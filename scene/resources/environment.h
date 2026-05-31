/**************************************************************************/
/*  environment.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_enums.h"

class Sky;

class Environment : public Resource {
	GDCLASS(Environment, Resource);

public:
	enum BGMode {
		BG_CLEAR_COLOR,
		BG_COLOR,
		BG_SKY,
		BG_CANVAS,
		BG_KEEP,
		BG_CAMERA_FEED,
		BG_MAX
	};

	enum AmbientSource {
		AMBIENT_SOURCE_BG,
		AMBIENT_SOURCE_DISABLED,
		AMBIENT_SOURCE_COLOR,
		AMBIENT_SOURCE_SKY,
	};

	enum ReflectionSource {
		REFLECTION_SOURCE_BG,
		REFLECTION_SOURCE_DISABLED,
		REFLECTION_SOURCE_SKY,
	};

	enum ToneMapper {
		TONE_MAPPER_LINEAR,
		TONE_MAPPER_REINHARDT,
		TONE_MAPPER_FILMIC,
		TONE_MAPPER_ACES,
		TONE_MAPPER_AGX,
	};

	enum SDFGIYScale {
		SDFGI_Y_SCALE_50_PERCENT,
		SDFGI_Y_SCALE_75_PERCENT,
		SDFGI_Y_SCALE_100_PERCENT,
	};

	enum RTGIBackend {
		RTGI_BACKEND_VULKAN_GENERIC = RSE::PT_BACKEND_VULKAN_GENERIC,
	};

	enum RTGIMode {
		RTGI_MODE_REFLECTIONS_RT_ONLY,
		RTGI_MODE_FULL_PATH_TRACING,
		RTGI_MODE_HYBRID,
		RTGI_MODE_PATH_TRACED = RTGI_MODE_FULL_PATH_TRACING,
	};

	enum RTGIQualityPreset {
		RTGI_QUALITY_PRESET_CUSTOM,
		RTGI_QUALITY_PRESET_PERFORMANCE,
		RTGI_QUALITY_PRESET_BALANCED,
		RTGI_QUALITY_PRESET_PRODUCTION,
	};

	enum RTGIDenoiser {
		// User-facing serialized values. Keep independent from RSE::PathtracingDenoiser.
		RTGI_DENOISER_ASVFG_EXPERIMENTAL = 8,
		RTGI_DENOISER_NONE = 9,
		RTGI_DENOISER_RESERVED_10 = 10,
		RTGI_DENOISER_NVIDIA = 11,
		RTGI_DENOISER_RESERVED_12 = 12,
		RTGI_DENOISER_RESERVED_13 = 13,
		RTGI_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION = 14,
		RTGI_DENOISER_SVGF = RTGI_DENOISER_ASVFG_EXPERIMENTAL,
	};

	enum PathtracingDebugMode {
		RT_DEBUG_DISABLED,
		RT_DEBUG_MIRROR_REFLECTION,
		RT_DEBUG_GEOMETRY_NORMALS,
		RT_DEBUG_FINAL_NORMALS,
		RT_DEBUG_NORMAL_MAP,
		RT_DEBUG_TANGENT,
		RT_DEBUG_BITANGENT,
		RT_DEBUG_UV,
		RT_DEBUG_ALBEDO,
		RT_DEBUG_ORM,
		RT_DEBUG_DIFFUSE_ALBEDO,
		RT_DEBUG_SPECULAR_ALBEDO,
		RT_DEBUG_NORMAL_ROUGHNESS,
		RT_DEBUG_SPECULAR_HIT_DISTANCE,
		RT_DEBUG_METALNESS,
		RT_DEBUG_ROUGHNESS,
		RT_DEBUG_VIEW_NORMALS,
		RT_DEBUG_DIFFUSE_SPECULAR_SPLIT,
		RT_DEBUG_FRESNEL_F0,
		RT_DEBUG_FRONT_BACK_FACE,
		RT_DEBUG_DEPTH,
		RT_DEBUG_EMISSIVE,
		RT_DEBUG_BRDF_REJECTION,
		RT_DEBUG_NORMAL_DEVIATION,
		RT_DEBUG_SPECULAR_REFLECTION_DIRECTION,
		RT_DEBUG_SPECULAR_REFLECTED_HIT_DISTANCE,
		RT_DEBUG_SPECULAR_REFLECTED_HIT_NORMAL,
		RT_DEBUG_MAX
	};

	enum FogMode {
		FOG_MODE_EXPONENTIAL,
		FOG_MODE_DEPTH,
	};

	enum GlowBlendMode {
		GLOW_BLEND_MODE_ADDITIVE,
		GLOW_BLEND_MODE_SCREEN,
		GLOW_BLEND_MODE_SOFTLIGHT,
		GLOW_BLEND_MODE_REPLACE,
		GLOW_BLEND_MODE_MIX,
	};

private:
	RID environment;

	// Background
	BGMode bg_mode = BG_CLEAR_COLOR;
	Ref<Sky> bg_sky;
	float bg_sky_custom_fov = 0.0;
	Vector3 bg_sky_rotation;
	Color bg_color;
	int bg_canvas_max_layer = 0;
	int bg_camera_feed_id = 1;
	float bg_energy_multiplier = 1.0;
	float bg_intensity = 30000.0; // Measured in nits or candela/m^2
	void _update_bg_energy();

	// Ambient light
	Color ambient_color;
	AmbientSource ambient_source = AMBIENT_SOURCE_BG;
	float ambient_energy = 1.0;
	float ambient_sky_contribution = 1.0;
	ReflectionSource reflection_source = REFLECTION_SOURCE_BG;
	void _update_ambient_light();

	// Tonemap
	ToneMapper tone_mapper = TONE_MAPPER_LINEAR;
	float tonemap_exposure = 1.0;
	float tonemap_white = 1.0;
	float tonemap_agx_white = 16.29; // Default to Blender's AgX white.
	float tonemap_agx_contrast = 1.25; // Default to approximately Blender's AgX contrast.
	void _update_tonemap();

	// SSR
	bool ssr_enabled = false;
	int ssr_max_steps = 64;
	float ssr_fade_in = 0.15;
	float ssr_fade_out = 2.0;
	float ssr_depth_tolerance = 0.5;
	void _update_ssr();

	// SSAO
	bool ssao_enabled = false;
	float ssao_radius = 1.0;
	float ssao_intensity = 2.0;
	float ssao_power = 1.5;
	float ssao_detail = 0.5;
	float ssao_horizon = 0.06;
	float ssao_sharpness = 0.98;
	float ssao_direct_light_affect = 0.0;
	float ssao_ao_channel_affect = 0.0;
	void _update_ssao();

	// SSIL
	bool ssil_enabled = false;
	float ssil_radius = 5.0;
	float ssil_intensity = 1.0;
	float ssil_sharpness = 0.98;
	float ssil_normal_rejection = 1.0;

	void _update_ssil();

	// SDFGI
	bool sdfgi_enabled = false;
	int sdfgi_cascades = 4;
	float sdfgi_min_cell_size = 0.2;
	SDFGIYScale sdfgi_y_scale = SDFGI_Y_SCALE_75_PERCENT;
	bool sdfgi_use_occlusion = false;
	float sdfgi_bounce_feedback = 0.5;
	bool sdfgi_read_sky_light = true;
	float sdfgi_energy = 1.0;
	float sdfgi_normal_bias = 1.1;
	float sdfgi_probe_bias = 1.1;
	void _update_sdfgi();

	// Pathtracing
	bool pathtracing_enabled = false;
	PathtracingDebugMode pathtracing_debug_mode = RT_DEBUG_DISABLED;
	int pathtracing_samples_per_pixel = 1;
	int pathtracing_max_bounces = 4;
	RSE::PathtracingDenoiser pathtracing_denoiser = RSE::PT_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION;
	RTGIBackend rtgi_backend = RTGI_BACKEND_VULKAN_GENERIC;
	RTGIQualityPreset rtgi_quality_preset = RTGI_QUALITY_PRESET_PRODUCTION;
	RTGIMode rtgi_mode = RTGI_MODE_HYBRID;
	float rtgi_energy = 1.0;
	float rtgi_resolution_scale = 0.67f;
	bool rtgi_disable_in_editor = true;
	float rtgi_denoiser_strength = 0.90f;
	float rtgi_denoiser_history_weight = 0.95f;
	float rtgi_denoiser_firefly_suppression = 1.0f;
	float rtgi_denoiser_detail_preservation = 1.0f;
	bool rtgi_denoiser_split_signals = true;
	float rtgi_denoiser_specular_history_weight = 0.92f;
	float rtgi_denoiser_specular_spatial_strength = 1.0f;
	float rtgi_ray_firefly_suppression = 0.85f;
	float rtgi_ray_max_radiance = 48.0f;
	bool rtgi_analytic_light_sampling_enabled = true;
	bool rtgi_explicit_emissive_sampling_enabled = true;
	bool rtgi_diffuse_radiance_cache_enabled = true;
	int rtgi_diffuse_radiance_cache_max_entries = 524288;
	bool rtgi_strc_enabled = true;
	float rtgi_strc_strength = 0.75f;
	int rtgi_strc_cascade_count = 3;
	int rtgi_strc_grid_size = 28;
	float rtgi_strc_base_probe_spacing = 1.25f;
	int rtgi_strc_rays_per_frame = 8192;
	float rtgi_strc_temporal_weight = 0.97f;
	float rtgi_overscan_horizontal = 0.0f;
	float rtgi_overscan_vertical = 0.0f;
	uint32_t rtgi_strc_static_visual_layers = 0xfffff;
	uint32_t rtgi_strc_dynamic_visual_layers = 0xfffff;
	RTGIDenoiser rtgi_denoiser = RTGI_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION;
	bool rtgi_applying_quality_preset = false;
	void _apply_rtgi_quality_preset(RTGIQualityPreset p_preset);
	void _mark_rtgi_quality_preset_custom();
	void _update_pathtracing();

	// Glow
	bool glow_enabled = false;
	Vector<float> glow_levels;
	bool glow_normalize_levels = false;
	float glow_intensity = 0.3;
	float glow_strength = 1.0;
	float glow_mix = 0.05;
	float glow_bloom = 0.0;
	GlowBlendMode glow_blend_mode = GLOW_BLEND_MODE_SCREEN;
	float glow_hdr_bleed_threshold = 1.0;
	float glow_hdr_bleed_scale = 2.0;
	float glow_hdr_luminance_cap = 12.0;
	float glow_map_strength = 0.8f;
	Ref<Texture> glow_map;
	void _update_glow();

	// Fog
	bool fog_enabled = false;
	FogMode fog_mode = FOG_MODE_EXPONENTIAL;
	Color fog_light_color = Color(0.518, 0.553, 0.608);
	float fog_light_energy = 1.0;
	float fog_sun_scatter = 0.0;
	float fog_density = 0.01;
	float fog_height = 0.0;
	float fog_height_density = 0.0; //can be negative to invert effect
	float fog_aerial_perspective = 0.0;
	float fog_sky_affect = 1.0;

	void _update_fog();

	// Depth Fog
	float fog_depth_curve = 1.0;
	float fog_depth_begin = 10.0;
	float fog_depth_end = 100.0;

	void _update_fog_depth();

	// Volumetric Fog
	bool volumetric_fog_enabled = false;
	float volumetric_fog_density = 0.05;
	Color volumetric_fog_albedo = Color(1.0, 1.0, 1.0);
	Color volumetric_fog_emission = Color(0.0, 0.0, 0.0);
	float volumetric_fog_emission_energy = 1.0;
	float volumetric_fog_anisotropy = 0.2;
	float volumetric_fog_length = 64.0;
	float volumetric_fog_detail_spread = 2.0;
	float volumetric_fog_gi_inject = 1.0;
	float volumetric_fog_ambient_inject = 0.0;
	float volumetric_fog_sky_affect = 1.0;
	bool volumetric_fog_temporal_reproject = true;
	float volumetric_fog_temporal_reproject_amount = 0.9;
	void _update_volumetric_fog();

	// Adjustment
	bool adjustment_enabled = false;
	float adjustment_brightness = 1.0;
	float adjustment_contrast = 1.0;
	float adjustment_saturation = 1.0;
	bool use_1d_color_correction = true;
	Ref<Texture> adjustment_color_correction;
	void _update_adjustment();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
#ifndef DISABLE_DEPRECATED
	// Kept for compatibility from 3.x to 4.0.
	bool _set(const StringName &p_name, const Variant &p_value);
#endif

public:
	virtual RID get_rid() const override;

	// Background
	void set_background(BGMode p_bg);
	BGMode get_background() const;
	void set_sky(const Ref<Sky> &p_sky);
	Ref<Sky> get_sky() const;
	void set_sky_custom_fov(float p_scale);
	float get_sky_custom_fov() const;
	void set_sky_rotation(const Vector3 &p_rotation);
	Vector3 get_sky_rotation() const;
	void set_bg_color(const Color &p_color);
	Color get_bg_color() const;
	void set_bg_energy_multiplier(float p_energy);
	float get_bg_energy_multiplier() const;
	void set_bg_intensity(float p_energy);
	float get_bg_intensity() const;
	void set_canvas_max_layer(int p_max_layer);
	int get_canvas_max_layer() const;
	void set_camera_feed_id(int p_id);
	int get_camera_feed_id() const;

	// Ambient light
	void set_ambient_light_color(const Color &p_color);
	Color get_ambient_light_color() const;
	void set_ambient_source(AmbientSource p_source);
	AmbientSource get_ambient_source() const;
	void set_ambient_light_energy(float p_energy);
	float get_ambient_light_energy() const;
	void set_ambient_light_sky_contribution(float p_ratio);
	float get_ambient_light_sky_contribution() const;
	void set_reflection_source(ReflectionSource p_source);
	ReflectionSource get_reflection_source() const;

	// Tonemap
	void set_tonemapper(ToneMapper p_tone_mapper);
	ToneMapper get_tonemapper() const;
	void set_tonemap_exposure(float p_exposure);
	float get_tonemap_exposure() const;
	void set_tonemap_white(float p_white);
	float get_tonemap_white() const;
	void set_tonemap_agx_white(float p_white);
	float get_tonemap_agx_white() const;
	void set_tonemap_agx_contrast(float p_agx_contrast);
	float get_tonemap_agx_contrast() const;

	// SSR
	void set_ssr_enabled(bool p_enabled);
	bool is_ssr_enabled() const;
	void set_ssr_max_steps(int p_steps);
	int get_ssr_max_steps() const;
	void set_ssr_fade_in(float p_fade_in);
	float get_ssr_fade_in() const;
	void set_ssr_fade_out(float p_fade_out);
	float get_ssr_fade_out() const;
	void set_ssr_depth_tolerance(float p_depth_tolerance);
	float get_ssr_depth_tolerance() const;

	// SSAO
	void set_ssao_enabled(bool p_enabled);
	bool is_ssao_enabled() const;
	void set_ssao_radius(float p_radius);
	float get_ssao_radius() const;
	void set_ssao_intensity(float p_intensity);
	float get_ssao_intensity() const;
	void set_ssao_power(float p_power);
	float get_ssao_power() const;
	void set_ssao_detail(float p_detail);
	float get_ssao_detail() const;
	void set_ssao_horizon(float p_horizon);
	float get_ssao_horizon() const;
	void set_ssao_sharpness(float p_sharpness);
	float get_ssao_sharpness() const;
	void set_ssao_direct_light_affect(float p_direct_light_affect);
	float get_ssao_direct_light_affect() const;
	void set_ssao_ao_channel_affect(float p_ao_channel_affect);
	float get_ssao_ao_channel_affect() const;

	// SSIL
	void set_ssil_enabled(bool p_enabled);
	bool is_ssil_enabled() const;
	void set_ssil_radius(float p_radius);
	float get_ssil_radius() const;
	void set_ssil_intensity(float p_intensity);
	float get_ssil_intensity() const;
	void set_ssil_sharpness(float p_sharpness);
	float get_ssil_sharpness() const;
	void set_ssil_normal_rejection(float p_normal_rejection);
	float get_ssil_normal_rejection() const;

	// SDFGI
	void set_sdfgi_enabled(bool p_enabled);
	bool is_sdfgi_enabled() const;
	void set_sdfgi_cascades(int p_cascades);
	int get_sdfgi_cascades() const;
	void set_sdfgi_min_cell_size(float p_size);
	float get_sdfgi_min_cell_size() const;
	void set_sdfgi_max_distance(float p_distance);
	float get_sdfgi_max_distance() const;
	void set_sdfgi_cascade0_distance(float p_distance);
	float get_sdfgi_cascade0_distance() const;
	void set_sdfgi_y_scale(SDFGIYScale p_y_scale);
	SDFGIYScale get_sdfgi_y_scale() const;
	void set_sdfgi_use_occlusion(bool p_enabled);
	bool is_sdfgi_using_occlusion() const;
	void set_sdfgi_bounce_feedback(float p_amount);
	float get_sdfgi_bounce_feedback() const;
	void set_sdfgi_read_sky_light(bool p_enabled);
	bool is_sdfgi_reading_sky_light() const;
	void set_sdfgi_energy(float p_energy);
	float get_sdfgi_energy() const;
	void set_sdfgi_normal_bias(float p_bias);
	float get_sdfgi_normal_bias() const;
	void set_sdfgi_probe_bias(float p_bias);
	float get_sdfgi_probe_bias() const;

	// Pathtracing
	void set_pathtracing_enabled(bool p_enabled);
	bool is_pathtracing_enabled() const;
	void set_pathtracing_debug_mode(PathtracingDebugMode p_mode);
	PathtracingDebugMode get_pathtracing_debug_mode() const;
	void set_pathtracing_samples_per_pixel(int p_samples);
	int get_pathtracing_samples_per_pixel() const;
	void set_pathtracing_max_bounces(int p_bounces);
	int get_pathtracing_max_bounces() const;
	void set_pathtracing_denoiser(RSE::PathtracingDenoiser p_denoiser);
	RSE::PathtracingDenoiser get_pathtracing_denoiser() const;

	void set_rtgi_enabled(bool p_enabled);
	bool is_rtgi_enabled() const;
	void set_rtgi_backend(RTGIBackend p_backend);
	RTGIBackend get_rtgi_backend() const;
	void set_rtgi_quality_preset(RTGIQualityPreset p_preset);
	RTGIQualityPreset get_rtgi_quality_preset() const;
	void set_rtgi_mode(RTGIMode p_mode);
	RTGIMode get_rtgi_mode() const;
	void set_rtgi_samples_per_pixel(int p_samples);
	int get_rtgi_samples_per_pixel() const;
	void set_rtgi_max_bounces(int p_bounces);
	int get_rtgi_max_bounces() const;
	void set_rtgi_energy(float p_energy);
	float get_rtgi_energy() const;
	void set_rtgi_resolution_scale(float p_scale);
	float get_rtgi_resolution_scale() const;
	void set_rtgi_disable_in_editor(bool p_disabled);
	bool is_rtgi_disabled_in_editor() const;
	void set_rtgi_denoiser_strength(float p_strength);
	float get_rtgi_denoiser_strength() const;
	void set_rtgi_denoiser_history_weight(float p_weight);
	float get_rtgi_denoiser_history_weight() const;
	void set_rtgi_denoiser_firefly_suppression(float p_suppression);
	float get_rtgi_denoiser_firefly_suppression() const;
	void set_rtgi_denoiser_detail_preservation(float p_preservation);
	float get_rtgi_denoiser_detail_preservation() const;
	void set_rtgi_denoiser_split_signals(bool p_enabled);
	bool is_rtgi_denoiser_split_signals_enabled() const;
	void set_rtgi_denoiser_specular_history_weight(float p_weight);
	float get_rtgi_denoiser_specular_history_weight() const;
	void set_rtgi_denoiser_specular_spatial_strength(float p_strength);
	float get_rtgi_denoiser_specular_spatial_strength() const;
	void set_rtgi_ray_firefly_suppression(float p_suppression);
	float get_rtgi_ray_firefly_suppression() const;
	void set_rtgi_ray_max_radiance(float p_radiance);
	float get_rtgi_ray_max_radiance() const;
	void set_rtgi_analytic_light_sampling_enabled(bool p_enabled);
	bool is_rtgi_analytic_light_sampling_enabled() const;
	void set_rtgi_explicit_emissive_sampling_enabled(bool p_enabled);
	bool is_rtgi_explicit_emissive_sampling_enabled() const;
	void set_rtgi_diffuse_radiance_cache_enabled(bool p_enabled);
	bool is_rtgi_diffuse_radiance_cache_enabled() const;
	void set_rtgi_diffuse_radiance_cache_max_entries(int p_entries);
	int get_rtgi_diffuse_radiance_cache_max_entries() const;
	void set_rtgi_strc_enabled(bool p_enabled);
	bool is_rtgi_strc_enabled() const;
	void set_rtgi_strc_strength(float p_strength);
	float get_rtgi_strc_strength() const;
	void set_rtgi_strc_cascade_count(int p_count);
	int get_rtgi_strc_cascade_count() const;
	void set_rtgi_strc_grid_size(int p_size);
	int get_rtgi_strc_grid_size() const;
	void set_rtgi_strc_base_probe_spacing(float p_spacing);
	float get_rtgi_strc_base_probe_spacing() const;
	void set_rtgi_strc_rays_per_frame(int p_rays);
	int get_rtgi_strc_rays_per_frame() const;
	void set_rtgi_strc_temporal_weight(float p_weight);
	float get_rtgi_strc_temporal_weight() const;
	void set_rtgi_overscan_horizontal(float p_overscan);
	float get_rtgi_overscan_horizontal() const;
	void set_rtgi_overscan_vertical(float p_overscan);
	float get_rtgi_overscan_vertical() const;
	void set_rtgi_strc_static_visual_layers(uint32_t p_layers);
	uint32_t get_rtgi_strc_static_visual_layers() const;
	void set_rtgi_strc_dynamic_visual_layers(uint32_t p_layers);
	uint32_t get_rtgi_strc_dynamic_visual_layers() const;
	void set_rtgi_denoiser(RTGIDenoiser p_denoiser);
	RTGIDenoiser get_rtgi_denoiser() const;
	void set_rtgi_debug_mode(PathtracingDebugMode p_mode);
	PathtracingDebugMode get_rtgi_debug_mode() const;

	// Glow
	void set_glow_enabled(bool p_enabled);
	bool is_glow_enabled() const;
	void set_glow_level(int p_level, float p_intensity);
	float get_glow_level(int p_level) const;
	void set_glow_normalized(bool p_normalized);
	bool is_glow_normalized() const;
	void set_glow_intensity(float p_intensity);
	float get_glow_intensity() const;
	void set_glow_strength(float p_strength);
	float get_glow_strength() const;
	void set_glow_mix(float p_mix);
	float get_glow_mix() const;
	void set_glow_bloom(float p_threshold);
	float get_glow_bloom() const;
	void set_glow_blend_mode(GlowBlendMode p_mode);
	GlowBlendMode get_glow_blend_mode() const;
	void set_glow_hdr_bleed_threshold(float p_threshold);
	float get_glow_hdr_bleed_threshold() const;
	void set_glow_hdr_bleed_scale(float p_scale);
	float get_glow_hdr_bleed_scale() const;
	void set_glow_hdr_luminance_cap(float p_amount);
	float get_glow_hdr_luminance_cap() const;
	void set_glow_map_strength(float p_strength);
	float get_glow_map_strength() const;
	void set_glow_map(Ref<Texture> p_glow_map);
	Ref<Texture> get_glow_map() const;

	// Fog

	void set_fog_enabled(bool p_enabled);
	bool is_fog_enabled() const;
	void set_fog_mode(FogMode p_mode);
	FogMode get_fog_mode() const;
	void set_fog_light_color(const Color &p_light_color);
	Color get_fog_light_color() const;
	void set_fog_light_energy(float p_amount);
	float get_fog_light_energy() const;
	void set_fog_sun_scatter(float p_amount);
	float get_fog_sun_scatter() const;

	void set_fog_density(float p_amount);
	float get_fog_density() const;
	void set_fog_height(float p_amount);
	float get_fog_height() const;
	void set_fog_height_density(float p_amount);
	float get_fog_height_density() const;
	void set_fog_aerial_perspective(float p_aerial_perspective);
	float get_fog_aerial_perspective() const;
	void set_fog_sky_affect(float p_sky_affect);
	float get_fog_sky_affect() const;

	// Depth Fog
	void set_fog_depth_curve(float p_curve);
	float get_fog_depth_curve() const;
	void set_fog_depth_begin(float p_begin);
	float get_fog_depth_begin() const;
	void set_fog_depth_end(float p_end);
	float get_fog_depth_end() const;

	// Volumetric Fog
	void set_volumetric_fog_enabled(bool p_enable);
	bool is_volumetric_fog_enabled() const;
	void set_volumetric_fog_density(float p_density);
	float get_volumetric_fog_density() const;
	void set_volumetric_fog_albedo(Color p_color);
	Color get_volumetric_fog_albedo() const;
	void set_volumetric_fog_emission(Color p_color);
	Color get_volumetric_fog_emission() const;
	void set_volumetric_fog_emission_energy(float p_begin);
	float get_volumetric_fog_emission_energy() const;
	void set_volumetric_fog_anisotropy(float p_anisotropy);
	float get_volumetric_fog_anisotropy() const;
	void set_volumetric_fog_length(float p_length);
	float get_volumetric_fog_length() const;
	void set_volumetric_fog_detail_spread(float p_detail_spread);
	float get_volumetric_fog_detail_spread() const;
	void set_volumetric_fog_gi_inject(float p_gi_inject);
	float get_volumetric_fog_gi_inject() const;
	void set_volumetric_fog_ambient_inject(float p_ambient_inject);
	float get_volumetric_fog_ambient_inject() const;
	void set_volumetric_fog_sky_affect(float p_sky_affect);
	float get_volumetric_fog_sky_affect() const;
	void set_volumetric_fog_temporal_reprojection_enabled(bool p_enable);
	bool is_volumetric_fog_temporal_reprojection_enabled() const;
	void set_volumetric_fog_temporal_reprojection_amount(float p_amount);
	float get_volumetric_fog_temporal_reprojection_amount() const;

	// Adjustment
	void set_adjustment_enabled(bool p_enabled);
	bool is_adjustment_enabled() const;
	void set_adjustment_brightness(float p_brightness);
	float get_adjustment_brightness() const;
	void set_adjustment_contrast(float p_contrast);
	float get_adjustment_contrast() const;
	void set_adjustment_saturation(float p_saturation);
	float get_adjustment_saturation() const;
	void set_adjustment_color_correction(Ref<Texture> p_color_correction);
	Ref<Texture> get_adjustment_color_correction() const;

	Environment();
	~Environment();
};

VARIANT_ENUM_CAST(Environment::BGMode)
VARIANT_ENUM_CAST(Environment::AmbientSource)
VARIANT_ENUM_CAST(Environment::ReflectionSource)
VARIANT_ENUM_CAST(Environment::ToneMapper)
VARIANT_ENUM_CAST(Environment::SDFGIYScale)
VARIANT_ENUM_CAST(Environment::RTGIBackend)
VARIANT_ENUM_CAST(Environment::RTGIMode)
VARIANT_ENUM_CAST(Environment::RTGIQualityPreset)
VARIANT_ENUM_CAST(Environment::RTGIDenoiser)
VARIANT_ENUM_CAST(Environment::GlowBlendMode)
VARIANT_ENUM_CAST(Environment::PathtracingDebugMode)
VARIANT_ENUM_CAST(Environment::FogMode)
