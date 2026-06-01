/**************************************************************************/
/*  rendering_server.h                                                    */
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

#include "core/io/image.h"
#include "core/math/geometry_3d.h"
#include "core/math/transform_2d.h"
#include "core/templates/rid.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_enums.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"

// Helper macros for code outside of the rendering server, but that is
// called by the rendering server.
#ifdef DEBUG_ENABLED
#define ERR_NOT_ON_RENDER_THREAD \
	RenderingServer *rendering_server = RenderingServer::get_singleton(); \
	ERR_FAIL_NULL(rendering_server); \
	ERR_FAIL_COND(!rendering_server->is_on_render_thread());
#define ERR_NOT_ON_RENDER_THREAD_V(m_ret) \
	RenderingServer *rendering_server = RenderingServer::get_singleton(); \
	ERR_FAIL_NULL_V(rendering_server, m_ret); \
	ERR_FAIL_COND_V(!rendering_server->is_on_render_thread(), m_ret);
#else
#define ERR_NOT_ON_RENDER_THREAD
#define ERR_NOT_ON_RENDER_THREAD_V(m_ret)
#endif

class RenderingDevice;

class RenderingServer : public Object {
	GDCLASS(RenderingServer, Object);

	static RenderingServer *singleton;

	int mm_policy = 0;
	bool render_loop_enabled = true;

	Array _get_array_from_surface(uint64_t p_format, Vector<uint8_t> p_vertex_data, Vector<uint8_t> p_attrib_data, Vector<uint8_t> p_skin_data, int p_vertex_len, Vector<uint8_t> p_index_data, int p_index_len, const AABB &p_aabb, const Vector4 &p_uv_scale) const;

	const Vector2 SMALL_VEC2 = Vector2(CMP_EPSILON, CMP_EPSILON);
	const Vector3 SMALL_VEC3 = Vector3(CMP_EPSILON, CMP_EPSILON, CMP_EPSILON);

	virtual TypedArray<StringName> _global_shader_parameter_get_list() const;

protected:
	RID _make_test_cube();
	void _free_internal_rids();
	RID test_texture;
	RID white_texture;
	RID test_material;

	Error _surface_set_data(Array p_arrays, uint64_t p_format, uint32_t *p_offsets, uint32_t p_vertex_stride, uint32_t p_normal_stride, uint32_t p_attrib_stride, uint32_t p_skin_stride, Vector<uint8_t> &r_vertex_array, Vector<uint8_t> &r_attrib_array, Vector<uint8_t> &r_skin_array, int p_vertex_array_len, Vector<uint8_t> &r_index_array, int p_index_array_len, AABB &r_aabb, Vector<AABB> &r_bone_aabb, Vector4 &r_uv_scale);

	static RenderingServer *(*create_func)();
	static void _bind_methods();

#ifndef DISABLE_DEPRECATED
	void _environment_set_fog_bind_compat_84792(RID p_env, bool p_enable, const Color &p_light_color, float p_light_energy, float p_sun_scatter, float p_density, float p_height, float p_height_density, float p_aerial_perspective, float p_sky_affect);
	void _canvas_item_add_multiline_bind_compat_84523(RID p_item, const Vector<Point2> &p_points, const Vector<Color> &p_colors, float p_width = -1.0);
	void _canvas_item_add_rect_bind_compat_84523(RID p_item, const Rect2 &p_rect, const Color &p_color);
	void _canvas_item_add_circle_bind_compat_84523(RID p_item, const Point2 &p_pos, float p_radius, const Color &p_color);
	void _instance_set_interpolated_bind_compat_104269(RID p_instance, bool p_interpolated);
	void _instance_reset_physics_interpolation_bind_compat_104269(RID p_instance);
	void _viewport_set_size_compat_115799(RID p_viewport, int p_width, int p_height);
	void _particles_request_process_time_bind_compat_109142(RID p_particles, real_t p_request_process_time);

	static void _bind_compatibility_methods();
#endif

public:
	using TextureDetectCallback = RenderingServerTypes::TextureDetectCallback;
	using TextureDetectRoughnessCallback = RenderingServerTypes::TextureDetectRoughnessCallback;
	using TextureInfo = RenderingServerTypes::TextureInfo;
	using ShaderNativeSourceCode = RenderingServerTypes::ShaderNativeSourceCode;
	using SurfaceData = RenderingServerTypes::SurfaceData;
	using MeshInfo = RenderingServerTypes::MeshInfo;
	using FrameProfileArea = RenderingServerTypes::FrameProfileArea;
	using BlitToScreen = RenderingServerTypes::BlitToScreen;
	using RenderInfo = RenderingServerTypes::RenderInfo;

	// Compatibility aliases for code that still refers to RenderingServer:: enums/constants.
	static constexpr int NO_INDEX_ARRAY = RSE::NO_INDEX_ARRAY;
	static constexpr int ARRAY_WEIGHTS_SIZE = RSE::ARRAY_WEIGHTS_SIZE;
	static constexpr int CANVAS_ITEM_Z_MIN = RSE::CANVAS_ITEM_Z_MIN;
	static constexpr int CANVAS_ITEM_Z_MAX = RSE::CANVAS_ITEM_Z_MAX;
	static constexpr int CANVAS_LAYER_MIN = RSE::CANVAS_LAYER_MIN;
	static constexpr int CANVAS_LAYER_MAX = RSE::CANVAS_LAYER_MAX;
	static constexpr int MAX_GLOW_LEVELS = RSE::MAX_GLOW_LEVELS;
	static constexpr int MAX_CURSORS = RSE::MAX_CURSORS;
	static constexpr int MAX_2D_DIRECTIONAL_LIGHTS = RSE::MAX_2D_DIRECTIONAL_LIGHTS;
	static constexpr int MAX_MESH_SURFACES = RSE::MAX_MESH_SURFACES;

	using TextureType = RSE::TextureType;
	static constexpr TextureType TEXTURE_TYPE_2D = RSE::TEXTURE_TYPE_2D;
	static constexpr TextureType TEXTURE_TYPE_LAYERED = RSE::TEXTURE_TYPE_LAYERED;
	static constexpr TextureType TEXTURE_TYPE_3D = RSE::TEXTURE_TYPE_3D;

	using TextureLayeredType = RSE::TextureLayeredType;
	static constexpr TextureLayeredType TEXTURE_LAYERED_2D_ARRAY = RSE::TEXTURE_LAYERED_2D_ARRAY;
	static constexpr TextureLayeredType TEXTURE_LAYERED_CUBEMAP = RSE::TEXTURE_LAYERED_CUBEMAP;
	static constexpr TextureLayeredType TEXTURE_LAYERED_CUBEMAP_ARRAY = RSE::TEXTURE_LAYERED_CUBEMAP_ARRAY;

	using CubeMapLayer = RSE::CubeMapLayer;
	static constexpr CubeMapLayer CUBEMAP_LAYER_LEFT = RSE::CUBEMAP_LAYER_LEFT;
	static constexpr CubeMapLayer CUBEMAP_LAYER_RIGHT = RSE::CUBEMAP_LAYER_RIGHT;
	static constexpr CubeMapLayer CUBEMAP_LAYER_BOTTOM = RSE::CUBEMAP_LAYER_BOTTOM;
	static constexpr CubeMapLayer CUBEMAP_LAYER_TOP = RSE::CUBEMAP_LAYER_TOP;
	static constexpr CubeMapLayer CUBEMAP_LAYER_FRONT = RSE::CUBEMAP_LAYER_FRONT;
	static constexpr CubeMapLayer CUBEMAP_LAYER_BACK = RSE::CUBEMAP_LAYER_BACK;

	using TextureDrawableFormat = RSE::TextureDrawableFormat;
	static constexpr TextureDrawableFormat TEXTURE_DRAWABLE_FORMAT_RGBA8 = RSE::TEXTURE_DRAWABLE_FORMAT_RGBA8;
	static constexpr TextureDrawableFormat TEXTURE_DRAWABLE_FORMAT_RGBA8_SRGB = RSE::TEXTURE_DRAWABLE_FORMAT_RGBA8_SRGB;
	static constexpr TextureDrawableFormat TEXTURE_DRAWABLE_FORMAT_RGBAH = RSE::TEXTURE_DRAWABLE_FORMAT_RGBAH;
	static constexpr TextureDrawableFormat TEXTURE_DRAWABLE_FORMAT_RGBAF = RSE::TEXTURE_DRAWABLE_FORMAT_RGBAF;

	using TextureDetectRoughnessChannel = RSE::TextureDetectRoughnessChannel;
	static constexpr TextureDetectRoughnessChannel TEXTURE_DETECT_ROUGHNESS_R = RSE::TEXTURE_DETECT_ROUGHNESS_R;
	static constexpr TextureDetectRoughnessChannel TEXTURE_DETECT_ROUGHNESS_G = RSE::TEXTURE_DETECT_ROUGHNESS_G;
	static constexpr TextureDetectRoughnessChannel TEXTURE_DETECT_ROUGHNESS_B = RSE::TEXTURE_DETECT_ROUGHNESS_B;
	static constexpr TextureDetectRoughnessChannel TEXTURE_DETECT_ROUGHNESS_A = RSE::TEXTURE_DETECT_ROUGHNESS_A;
	static constexpr TextureDetectRoughnessChannel TEXTURE_DETECT_ROUGHNESS_GRAY = RSE::TEXTURE_DETECT_ROUGHNESS_GRAY;

	using PipelineSource = RSE::PipelineSource;
	static constexpr PipelineSource PIPELINE_SOURCE_CANVAS = RSE::PIPELINE_SOURCE_CANVAS;
	static constexpr PipelineSource PIPELINE_SOURCE_MESH = RSE::PIPELINE_SOURCE_MESH;
	static constexpr PipelineSource PIPELINE_SOURCE_SURFACE = RSE::PIPELINE_SOURCE_SURFACE;
	static constexpr PipelineSource PIPELINE_SOURCE_DRAW = RSE::PIPELINE_SOURCE_DRAW;
	static constexpr PipelineSource PIPELINE_SOURCE_SPECIALIZATION = RSE::PIPELINE_SOURCE_SPECIALIZATION;
	static constexpr PipelineSource PIPELINE_SOURCE_MAX = RSE::PIPELINE_SOURCE_MAX;

	using ShaderMode = RSE::ShaderMode;
	static constexpr ShaderMode SHADER_SPATIAL = RSE::SHADER_SPATIAL;
	static constexpr ShaderMode SHADER_CANVAS_ITEM = RSE::SHADER_CANVAS_ITEM;
	static constexpr ShaderMode SHADER_PARTICLES = RSE::SHADER_PARTICLES;
	static constexpr ShaderMode SHADER_SKY = RSE::SHADER_SKY;
	static constexpr ShaderMode SHADER_FOG = RSE::SHADER_FOG;
	static constexpr ShaderMode SHADER_TEXTURE_BLIT = RSE::SHADER_TEXTURE_BLIT;
	static constexpr ShaderMode SHADER_MAX = RSE::SHADER_MAX;

	using CullMode = RSE::CullMode;
	static constexpr CullMode CULL_MODE_DISABLED = RSE::CULL_MODE_DISABLED;
	static constexpr CullMode CULL_MODE_FRONT = RSE::CULL_MODE_FRONT;
	static constexpr CullMode CULL_MODE_BACK = RSE::CULL_MODE_BACK;

	static constexpr int MATERIAL_RENDER_PRIORITY_MIN = RSE::MATERIAL_RENDER_PRIORITY_MIN;
	static constexpr int MATERIAL_RENDER_PRIORITY_MAX = RSE::MATERIAL_RENDER_PRIORITY_MAX;

	using ArrayType = RSE::ArrayType;
	static constexpr ArrayType ARRAY_VERTEX = RSE::ARRAY_VERTEX;
	static constexpr ArrayType ARRAY_NORMAL = RSE::ARRAY_NORMAL;
	static constexpr ArrayType ARRAY_TANGENT = RSE::ARRAY_TANGENT;
	static constexpr ArrayType ARRAY_COLOR = RSE::ARRAY_COLOR;
	static constexpr ArrayType ARRAY_TEX_UV = RSE::ARRAY_TEX_UV;
	static constexpr ArrayType ARRAY_TEX_UV2 = RSE::ARRAY_TEX_UV2;
	static constexpr ArrayType ARRAY_CUSTOM0 = RSE::ARRAY_CUSTOM0;
	static constexpr ArrayType ARRAY_CUSTOM1 = RSE::ARRAY_CUSTOM1;
	static constexpr ArrayType ARRAY_CUSTOM2 = RSE::ARRAY_CUSTOM2;
	static constexpr ArrayType ARRAY_CUSTOM3 = RSE::ARRAY_CUSTOM3;
	static constexpr ArrayType ARRAY_BONES = RSE::ARRAY_BONES;
	static constexpr ArrayType ARRAY_WEIGHTS = RSE::ARRAY_WEIGHTS;
	static constexpr ArrayType ARRAY_INDEX = RSE::ARRAY_INDEX;
	static constexpr ArrayType ARRAY_MAX = RSE::ARRAY_MAX;

	static constexpr int ARRAY_CUSTOM_COUNT = RSE::ARRAY_CUSTOM_COUNT;

	using ArrayCustomFormat = RSE::ArrayCustomFormat;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RGBA8_UNORM = RSE::ARRAY_CUSTOM_RGBA8_UNORM;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RGBA8_SNORM = RSE::ARRAY_CUSTOM_RGBA8_SNORM;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RG_HALF = RSE::ARRAY_CUSTOM_RG_HALF;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RGBA_HALF = RSE::ARRAY_CUSTOM_RGBA_HALF;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_R_FLOAT = RSE::ARRAY_CUSTOM_R_FLOAT;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RG_FLOAT = RSE::ARRAY_CUSTOM_RG_FLOAT;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RGB_FLOAT = RSE::ARRAY_CUSTOM_RGB_FLOAT;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_RGBA_FLOAT = RSE::ARRAY_CUSTOM_RGBA_FLOAT;
	static constexpr ArrayCustomFormat ARRAY_CUSTOM_MAX = RSE::ARRAY_CUSTOM_MAX;

	using ArrayFormat = RSE::ArrayFormat;
	static constexpr ArrayFormat ARRAY_FORMAT_VERTEX = RSE::ARRAY_FORMAT_VERTEX;
	static constexpr ArrayFormat ARRAY_FORMAT_NORMAL = RSE::ARRAY_FORMAT_NORMAL;
	static constexpr ArrayFormat ARRAY_FORMAT_TANGENT = RSE::ARRAY_FORMAT_TANGENT;
	static constexpr ArrayFormat ARRAY_FORMAT_COLOR = RSE::ARRAY_FORMAT_COLOR;
	static constexpr ArrayFormat ARRAY_FORMAT_TEX_UV = RSE::ARRAY_FORMAT_TEX_UV;
	static constexpr ArrayFormat ARRAY_FORMAT_TEX_UV2 = RSE::ARRAY_FORMAT_TEX_UV2;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM0 = RSE::ARRAY_FORMAT_CUSTOM0;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM1 = RSE::ARRAY_FORMAT_CUSTOM1;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM2 = RSE::ARRAY_FORMAT_CUSTOM2;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM3 = RSE::ARRAY_FORMAT_CUSTOM3;
	static constexpr ArrayFormat ARRAY_FORMAT_BONES = RSE::ARRAY_FORMAT_BONES;
	static constexpr ArrayFormat ARRAY_FORMAT_WEIGHTS = RSE::ARRAY_FORMAT_WEIGHTS;
	static constexpr ArrayFormat ARRAY_FORMAT_INDEX = RSE::ARRAY_FORMAT_INDEX;
	static constexpr ArrayFormat ARRAY_FORMAT_BLEND_SHAPE_MASK = RSE::ARRAY_FORMAT_BLEND_SHAPE_MASK;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM_BASE = RSE::ARRAY_FORMAT_CUSTOM_BASE;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM_BITS = RSE::ARRAY_FORMAT_CUSTOM_BITS;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM_MASK = RSE::ARRAY_FORMAT_CUSTOM_MASK;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM0_SHIFT = RSE::ARRAY_FORMAT_CUSTOM0_SHIFT;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM1_SHIFT = RSE::ARRAY_FORMAT_CUSTOM1_SHIFT;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM2_SHIFT = RSE::ARRAY_FORMAT_CUSTOM2_SHIFT;
	static constexpr ArrayFormat ARRAY_FORMAT_CUSTOM3_SHIFT = RSE::ARRAY_FORMAT_CUSTOM3_SHIFT;
	static constexpr ArrayFormat ARRAY_COMPRESS_FLAGS_BASE = RSE::ARRAY_COMPRESS_FLAGS_BASE;
	static constexpr ArrayFormat ARRAY_FLAG_USE_2D_VERTICES = RSE::ARRAY_FLAG_USE_2D_VERTICES;
	static constexpr ArrayFormat ARRAY_FLAG_USE_DYNAMIC_UPDATE = RSE::ARRAY_FLAG_USE_DYNAMIC_UPDATE;
	static constexpr ArrayFormat ARRAY_FLAG_USE_8_BONE_WEIGHTS = RSE::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
	static constexpr ArrayFormat ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY = RSE::ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY;
	static constexpr ArrayFormat ARRAY_FLAG_COMPRESS_ATTRIBUTES = RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_VERSION_BASE = RSE::ARRAY_FLAG_FORMAT_VERSION_BASE;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_VERSION_SHIFT = RSE::ARRAY_FLAG_FORMAT_VERSION_SHIFT;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_VERSION_1 = RSE::ARRAY_FLAG_FORMAT_VERSION_1;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_VERSION_2 = RSE::ARRAY_FLAG_FORMAT_VERSION_2;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_CURRENT_VERSION = RSE::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	static constexpr ArrayFormat ARRAY_FLAG_FORMAT_VERSION_MASK = RSE::ARRAY_FLAG_FORMAT_VERSION_MASK;

	using PrimitiveType = RSE::PrimitiveType;
	static constexpr PrimitiveType PRIMITIVE_POINTS = RSE::PRIMITIVE_POINTS;
	static constexpr PrimitiveType PRIMITIVE_LINES = RSE::PRIMITIVE_LINES;
	static constexpr PrimitiveType PRIMITIVE_LINE_STRIP = RSE::PRIMITIVE_LINE_STRIP;
	static constexpr PrimitiveType PRIMITIVE_TRIANGLES = RSE::PRIMITIVE_TRIANGLES;
	static constexpr PrimitiveType PRIMITIVE_TRIANGLE_STRIP = RSE::PRIMITIVE_TRIANGLE_STRIP;
	static constexpr PrimitiveType PRIMITIVE_MAX = RSE::PRIMITIVE_MAX;

	using BlendShapeMode = RSE::BlendShapeMode;
	static constexpr BlendShapeMode BLEND_SHAPE_MODE_NORMALIZED = RSE::BLEND_SHAPE_MODE_NORMALIZED;
	static constexpr BlendShapeMode BLEND_SHAPE_MODE_RELATIVE = RSE::BLEND_SHAPE_MODE_RELATIVE;

	using MultimeshTransformFormat = RSE::MultimeshTransformFormat;
	static constexpr MultimeshTransformFormat MULTIMESH_TRANSFORM_2D = RSE::MULTIMESH_TRANSFORM_2D;
	static constexpr MultimeshTransformFormat MULTIMESH_TRANSFORM_3D = RSE::MULTIMESH_TRANSFORM_3D;

	using MultimeshPhysicsInterpolationQuality = RSE::MultimeshPhysicsInterpolationQuality;
	static constexpr MultimeshPhysicsInterpolationQuality MULTIMESH_INTERP_QUALITY_FAST = RSE::MULTIMESH_INTERP_QUALITY_FAST;
	static constexpr MultimeshPhysicsInterpolationQuality MULTIMESH_INTERP_QUALITY_HIGH = RSE::MULTIMESH_INTERP_QUALITY_HIGH;

	using LightType = RSE::LightType;
	static constexpr LightType LIGHT_DIRECTIONAL = RSE::LIGHT_DIRECTIONAL;
	static constexpr LightType LIGHT_OMNI = RSE::LIGHT_OMNI;
	static constexpr LightType LIGHT_SPOT = RSE::LIGHT_SPOT;
	static constexpr LightType LIGHT_AREA = RSE::LIGHT_AREA;

	using LightParam = RSE::LightParam;
	static constexpr LightParam LIGHT_PARAM_ENERGY = RSE::LIGHT_PARAM_ENERGY;
	static constexpr LightParam LIGHT_PARAM_INDIRECT_ENERGY = RSE::LIGHT_PARAM_INDIRECT_ENERGY;
	static constexpr LightParam LIGHT_PARAM_VOLUMETRIC_FOG_ENERGY = RSE::LIGHT_PARAM_VOLUMETRIC_FOG_ENERGY;
	static constexpr LightParam LIGHT_PARAM_SPECULAR = RSE::LIGHT_PARAM_SPECULAR;
	static constexpr LightParam LIGHT_PARAM_RANGE = RSE::LIGHT_PARAM_RANGE;
	static constexpr LightParam LIGHT_PARAM_SIZE = RSE::LIGHT_PARAM_SIZE;
	static constexpr LightParam LIGHT_PARAM_ATTENUATION = RSE::LIGHT_PARAM_ATTENUATION;
	static constexpr LightParam LIGHT_PARAM_SPOT_ANGLE = RSE::LIGHT_PARAM_SPOT_ANGLE;
	static constexpr LightParam LIGHT_PARAM_SPOT_ATTENUATION = RSE::LIGHT_PARAM_SPOT_ATTENUATION;
	static constexpr LightParam LIGHT_PARAM_SHADOW_MAX_DISTANCE = RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE;
	static constexpr LightParam LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET = RSE::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET;
	static constexpr LightParam LIGHT_PARAM_SHADOW_SPLIT_2_OFFSET = RSE::LIGHT_PARAM_SHADOW_SPLIT_2_OFFSET;
	static constexpr LightParam LIGHT_PARAM_SHADOW_SPLIT_3_OFFSET = RSE::LIGHT_PARAM_SHADOW_SPLIT_3_OFFSET;
	static constexpr LightParam LIGHT_PARAM_SHADOW_FADE_START = RSE::LIGHT_PARAM_SHADOW_FADE_START;
	static constexpr LightParam LIGHT_PARAM_SHADOW_NORMAL_BIAS = RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS;
	static constexpr LightParam LIGHT_PARAM_SHADOW_BIAS = RSE::LIGHT_PARAM_SHADOW_BIAS;
	static constexpr LightParam LIGHT_PARAM_SHADOW_PANCAKE_SIZE = RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE;
	static constexpr LightParam LIGHT_PARAM_SHADOW_OPACITY = RSE::LIGHT_PARAM_SHADOW_OPACITY;
	static constexpr LightParam LIGHT_PARAM_SHADOW_BLUR = RSE::LIGHT_PARAM_SHADOW_BLUR;
	static constexpr LightParam LIGHT_PARAM_TRANSMITTANCE_BIAS = RSE::LIGHT_PARAM_TRANSMITTANCE_BIAS;
	static constexpr LightParam LIGHT_PARAM_INTENSITY = RSE::LIGHT_PARAM_INTENSITY;
	static constexpr LightParam LIGHT_PARAM_MAX = RSE::LIGHT_PARAM_MAX;

	using LightBakeMode = RSE::LightBakeMode;
	static constexpr LightBakeMode LIGHT_BAKE_DISABLED = RSE::LIGHT_BAKE_DISABLED;
	static constexpr LightBakeMode LIGHT_BAKE_STATIC = RSE::LIGHT_BAKE_STATIC;
	static constexpr LightBakeMode LIGHT_BAKE_DYNAMIC = RSE::LIGHT_BAKE_DYNAMIC;

	using LightOmniShadowMode = RSE::LightOmniShadowMode;
	static constexpr LightOmniShadowMode LIGHT_OMNI_SHADOW_DUAL_PARABOLOID = RSE::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID;
	static constexpr LightOmniShadowMode LIGHT_OMNI_SHADOW_CUBE = RSE::LIGHT_OMNI_SHADOW_CUBE;

	using LightDirectionalShadowMode = RSE::LightDirectionalShadowMode;
	static constexpr LightDirectionalShadowMode LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL = RSE::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL;
	static constexpr LightDirectionalShadowMode LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS = RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS;
	static constexpr LightDirectionalShadowMode LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS = RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS;

	using LightDirectionalSkyMode = RSE::LightDirectionalSkyMode;
	static constexpr LightDirectionalSkyMode LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_AND_SKY = RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_AND_SKY;
	static constexpr LightDirectionalSkyMode LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_ONLY = RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_ONLY;
	static constexpr LightDirectionalSkyMode LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY = RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY;

	using ShadowQuality = RSE::ShadowQuality;
	static constexpr ShadowQuality SHADOW_QUALITY_HARD = RSE::SHADOW_QUALITY_HARD;
	static constexpr ShadowQuality SHADOW_QUALITY_SOFT_VERY_LOW = RSE::SHADOW_QUALITY_SOFT_VERY_LOW;
	static constexpr ShadowQuality SHADOW_QUALITY_SOFT_LOW = RSE::SHADOW_QUALITY_SOFT_LOW;
	static constexpr ShadowQuality SHADOW_QUALITY_SOFT_MEDIUM = RSE::SHADOW_QUALITY_SOFT_MEDIUM;
	static constexpr ShadowQuality SHADOW_QUALITY_SOFT_HIGH = RSE::SHADOW_QUALITY_SOFT_HIGH;
	static constexpr ShadowQuality SHADOW_QUALITY_SOFT_ULTRA = RSE::SHADOW_QUALITY_SOFT_ULTRA;
	static constexpr ShadowQuality SHADOW_QUALITY_MAX = RSE::SHADOW_QUALITY_MAX;

	using LightProjectorFilter = RSE::LightProjectorFilter;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_NEAREST = RSE::LIGHT_PROJECTOR_FILTER_NEAREST;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_LINEAR = RSE::LIGHT_PROJECTOR_FILTER_LINEAR;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS = RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS = RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC = RSE::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC;
	static constexpr LightProjectorFilter LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC = RSE::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	using ReflectionProbeUpdateMode = RSE::ReflectionProbeUpdateMode;
	static constexpr ReflectionProbeUpdateMode REFLECTION_PROBE_UPDATE_ONCE = RSE::REFLECTION_PROBE_UPDATE_ONCE;
	static constexpr ReflectionProbeUpdateMode REFLECTION_PROBE_UPDATE_ALWAYS = RSE::REFLECTION_PROBE_UPDATE_ALWAYS;

	using ReflectionProbeAmbientMode = RSE::ReflectionProbeAmbientMode;
	static constexpr ReflectionProbeAmbientMode REFLECTION_PROBE_AMBIENT_DISABLED = RSE::REFLECTION_PROBE_AMBIENT_DISABLED;
	static constexpr ReflectionProbeAmbientMode REFLECTION_PROBE_AMBIENT_ENVIRONMENT = RSE::REFLECTION_PROBE_AMBIENT_ENVIRONMENT;
	static constexpr ReflectionProbeAmbientMode REFLECTION_PROBE_AMBIENT_COLOR = RSE::REFLECTION_PROBE_AMBIENT_COLOR;

	using DecalTexture = RSE::DecalTexture;
	static constexpr DecalTexture DECAL_TEXTURE_ALBEDO = RSE::DECAL_TEXTURE_ALBEDO;
	static constexpr DecalTexture DECAL_TEXTURE_NORMAL = RSE::DECAL_TEXTURE_NORMAL;
	static constexpr DecalTexture DECAL_TEXTURE_ORM = RSE::DECAL_TEXTURE_ORM;
	static constexpr DecalTexture DECAL_TEXTURE_EMISSION = RSE::DECAL_TEXTURE_EMISSION;
	static constexpr DecalTexture DECAL_TEXTURE_MAX = RSE::DECAL_TEXTURE_MAX;

	using DecalFilter = RSE::DecalFilter;
	static constexpr DecalFilter DECAL_FILTER_NEAREST = RSE::DECAL_FILTER_NEAREST;
	static constexpr DecalFilter DECAL_FILTER_LINEAR = RSE::DECAL_FILTER_LINEAR;
	static constexpr DecalFilter DECAL_FILTER_NEAREST_MIPMAPS = RSE::DECAL_FILTER_NEAREST_MIPMAPS;
	static constexpr DecalFilter DECAL_FILTER_LINEAR_MIPMAPS = RSE::DECAL_FILTER_LINEAR_MIPMAPS;
	static constexpr DecalFilter DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC = RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC;
	static constexpr DecalFilter DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC = RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	using VoxelGIQuality = RSE::VoxelGIQuality;
	static constexpr VoxelGIQuality VOXEL_GI_QUALITY_LOW = RSE::VOXEL_GI_QUALITY_LOW;
	static constexpr VoxelGIQuality VOXEL_GI_QUALITY_HIGH = RSE::VOXEL_GI_QUALITY_HIGH;

	using ShadowmaskMode = RSE::ShadowmaskMode;
	static constexpr ShadowmaskMode SHADOWMASK_MODE_NONE = RSE::SHADOWMASK_MODE_NONE;
	static constexpr ShadowmaskMode SHADOWMASK_MODE_REPLACE = RSE::SHADOWMASK_MODE_REPLACE;
	static constexpr ShadowmaskMode SHADOWMASK_MODE_OVERLAY = RSE::SHADOWMASK_MODE_OVERLAY;
	static constexpr ShadowmaskMode SHADOWMASK_MODE_ONLY = RSE::SHADOWMASK_MODE_ONLY;

	using ParticlesMode = RSE::ParticlesMode;
	static constexpr ParticlesMode PARTICLES_MODE_2D = RSE::PARTICLES_MODE_2D;
	static constexpr ParticlesMode PARTICLES_MODE_3D = RSE::PARTICLES_MODE_3D;

	using ParticlesTransformAlign = RSE::ParticlesTransformAlign;
	static constexpr ParticlesTransformAlign PARTICLES_TRANSFORM_ALIGN_DISABLED = RSE::PARTICLES_TRANSFORM_ALIGN_DISABLED;
	static constexpr ParticlesTransformAlign PARTICLES_TRANSFORM_ALIGN_Z_BILLBOARD = RSE::PARTICLES_TRANSFORM_ALIGN_Z_BILLBOARD;
	static constexpr ParticlesTransformAlign PARTICLES_TRANSFORM_ALIGN_Y_TO_VELOCITY = RSE::PARTICLES_TRANSFORM_ALIGN_Y_TO_VELOCITY;
	static constexpr ParticlesTransformAlign PARTICLES_TRANSFORM_ALIGN_Z_BILLBOARD_Y_TO_VELOCITY = RSE::PARTICLES_TRANSFORM_ALIGN_Z_BILLBOARD_Y_TO_VELOCITY;
	static constexpr ParticlesTransformAlign PARTICLES_TRANSFORM_ALIGN_LOCAL_BILLBOARD = RSE::PARTICLES_TRANSFORM_ALIGN_LOCAL_BILLBOARD;

	using ParticlesEmitFlags = RSE::ParticlesEmitFlags;
	static constexpr ParticlesEmitFlags PARTICLES_EMIT_FLAG_POSITION = RSE::PARTICLES_EMIT_FLAG_POSITION;
	static constexpr ParticlesEmitFlags PARTICLES_EMIT_FLAG_ROTATION_SCALE = RSE::PARTICLES_EMIT_FLAG_ROTATION_SCALE;
	static constexpr ParticlesEmitFlags PARTICLES_EMIT_FLAG_VELOCITY = RSE::PARTICLES_EMIT_FLAG_VELOCITY;
	static constexpr ParticlesEmitFlags PARTICLES_EMIT_FLAG_COLOR = RSE::PARTICLES_EMIT_FLAG_COLOR;
	static constexpr ParticlesEmitFlags PARTICLES_EMIT_FLAG_CUSTOM = RSE::PARTICLES_EMIT_FLAG_CUSTOM;

	using ParticlesDrawOrder = RSE::ParticlesDrawOrder;
	static constexpr ParticlesDrawOrder PARTICLES_DRAW_ORDER_INDEX = RSE::PARTICLES_DRAW_ORDER_INDEX;
	static constexpr ParticlesDrawOrder PARTICLES_DRAW_ORDER_LIFETIME = RSE::PARTICLES_DRAW_ORDER_LIFETIME;
	static constexpr ParticlesDrawOrder PARTICLES_DRAW_ORDER_REVERSE_LIFETIME = RSE::PARTICLES_DRAW_ORDER_REVERSE_LIFETIME;
	static constexpr ParticlesDrawOrder PARTICLES_DRAW_ORDER_VIEW_DEPTH = RSE::PARTICLES_DRAW_ORDER_VIEW_DEPTH;

	using ParticlesTransformAlignCustomSrc = RSE::ParticlesTransformAlignCustomSrc;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_DISABLED = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_DISABLED;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_X = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_X;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_Y = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_Y;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_Z = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_Z;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_W = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_W;
	static constexpr ParticlesTransformAlignCustomSrc PARTICLES_ALIGN_CHANNEL_FILTER_MAX = RSE::PARTICLES_ALIGN_CHANNEL_FILTER_MAX;

	using ParticlesTransformAlignAxis = RSE::ParticlesTransformAlignAxis;
	static constexpr ParticlesTransformAlignAxis PARTICLES_ALIGN_AXIS_X = RSE::PARTICLES_ALIGN_AXIS_X;
	static constexpr ParticlesTransformAlignAxis PARTICLES_ALIGN_AXIS_Y = RSE::PARTICLES_ALIGN_AXIS_Y;
	static constexpr ParticlesTransformAlignAxis PARTICLES_ALIGN_AXIS_MAX = RSE::PARTICLES_ALIGN_AXIS_MAX;

	using ParticlesCollisionType = RSE::ParticlesCollisionType;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_SPHERE_ATTRACT = RSE::PARTICLES_COLLISION_TYPE_SPHERE_ATTRACT;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_BOX_ATTRACT = RSE::PARTICLES_COLLISION_TYPE_BOX_ATTRACT;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_VECTOR_FIELD_ATTRACT = RSE::PARTICLES_COLLISION_TYPE_VECTOR_FIELD_ATTRACT;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_SPHERE_COLLIDE = RSE::PARTICLES_COLLISION_TYPE_SPHERE_COLLIDE;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_BOX_COLLIDE = RSE::PARTICLES_COLLISION_TYPE_BOX_COLLIDE;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_SDF_COLLIDE = RSE::PARTICLES_COLLISION_TYPE_SDF_COLLIDE;
	static constexpr ParticlesCollisionType PARTICLES_COLLISION_TYPE_HEIGHTFIELD_COLLIDE = RSE::PARTICLES_COLLISION_TYPE_HEIGHTFIELD_COLLIDE;

	using ParticlesCollisionHeightfieldResolution = RSE::ParticlesCollisionHeightfieldResolution;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_256 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_256;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_512 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_512;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_1024 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_1024;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_2048 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_2048;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_4096 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_4096;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_8192 = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_8192;
	static constexpr ParticlesCollisionHeightfieldResolution PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_MAX = RSE::PARTICLES_COLLISION_HEIGHTFIELD_RESOLUTION_MAX;

	using FogVolumeShape = RSE::FogVolumeShape;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_ELLIPSOID = RSE::FOG_VOLUME_SHAPE_ELLIPSOID;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_CONE = RSE::FOG_VOLUME_SHAPE_CONE;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_CYLINDER = RSE::FOG_VOLUME_SHAPE_CYLINDER;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_BOX = RSE::FOG_VOLUME_SHAPE_BOX;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_WORLD = RSE::FOG_VOLUME_SHAPE_WORLD;
	static constexpr FogVolumeShape FOG_VOLUME_SHAPE_MAX = RSE::FOG_VOLUME_SHAPE_MAX;

	using CanvasItemTextureFilter = RSE::CanvasItemTextureFilter;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_DEFAULT = RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_NEAREST = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_LINEAR = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC = RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC;
	static constexpr CanvasItemTextureFilter CANVAS_ITEM_TEXTURE_FILTER_MAX = RSE::CANVAS_ITEM_TEXTURE_FILTER_MAX;

	using CanvasItemTextureRepeat = RSE::CanvasItemTextureRepeat;
	static constexpr CanvasItemTextureRepeat CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT = RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT;
	static constexpr CanvasItemTextureRepeat CANVAS_ITEM_TEXTURE_REPEAT_DISABLED = RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED;
	static constexpr CanvasItemTextureRepeat CANVAS_ITEM_TEXTURE_REPEAT_ENABLED = RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED;
	static constexpr CanvasItemTextureRepeat CANVAS_ITEM_TEXTURE_REPEAT_MIRROR = RSE::CANVAS_ITEM_TEXTURE_REPEAT_MIRROR;
	static constexpr CanvasItemTextureRepeat CANVAS_ITEM_TEXTURE_REPEAT_MAX = RSE::CANVAS_ITEM_TEXTURE_REPEAT_MAX;

	using ViewportScaling3DMode = RSE::ViewportScaling3DMode;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_BILINEAR = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_FSR = RSE::VIEWPORT_SCALING_3D_MODE_FSR;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_FSR2 = RSE::VIEWPORT_SCALING_3D_MODE_FSR2;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_METALFX_SPATIAL = RSE::VIEWPORT_SCALING_3D_MODE_METALFX_SPATIAL;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL = RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_NEAREST = RSE::VIEWPORT_SCALING_3D_MODE_NEAREST;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_SHARP_BILINEAR = RSE::VIEWPORT_SCALING_3D_MODE_SHARP_BILINEAR;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_BICUBIC = RSE::VIEWPORT_SCALING_3D_MODE_BICUBIC;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_SGSR = RSE::VIEWPORT_SCALING_3D_MODE_SGSR;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_XESS = RSE::VIEWPORT_SCALING_3D_MODE_XESS;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_MAX = RSE::VIEWPORT_SCALING_3D_MODE_MAX;
	static constexpr ViewportScaling3DMode VIEWPORT_SCALING_3D_MODE_OFF = RSE::VIEWPORT_SCALING_3D_MODE_OFF;

	using ViewportFrameGenerationMode = RSE::ViewportFrameGenerationMode;
	static constexpr ViewportFrameGenerationMode VIEWPORT_FRAME_GENERATION_DISABLED = RSE::VIEWPORT_FRAME_GENERATION_DISABLED;
	static constexpr ViewportFrameGenerationMode VIEWPORT_FRAME_GENERATION_INTERPOLATED = RSE::VIEWPORT_FRAME_GENERATION_INTERPOLATED;
	static constexpr ViewportFrameGenerationMode VIEWPORT_FRAME_GENERATION_VENDOR_AUTO = RSE::VIEWPORT_FRAME_GENERATION_VENDOR_AUTO;
	static constexpr ViewportFrameGenerationMode VIEWPORT_FRAME_GENERATION_XESS = RSE::VIEWPORT_FRAME_GENERATION_XESS;
	static constexpr ViewportFrameGenerationMode VIEWPORT_FRAME_GENERATION_MAX = RSE::VIEWPORT_FRAME_GENERATION_MAX;


	using ViewportScaling3DType = RSE::ViewportScaling3DType;
	static constexpr ViewportScaling3DType VIEWPORT_SCALING_3D_TYPE_NONE = RSE::VIEWPORT_SCALING_3D_TYPE_NONE;
	static constexpr ViewportScaling3DType VIEWPORT_SCALING_3D_TYPE_TEMPORAL = RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL;
	static constexpr ViewportScaling3DType VIEWPORT_SCALING_3D_TYPE_SPATIAL = RSE::VIEWPORT_SCALING_3D_TYPE_SPATIAL;
	static constexpr ViewportScaling3DType VIEWPORT_SCALING_3D_TYPE_MAX = RSE::VIEWPORT_SCALING_3D_TYPE_MAX;

	_ALWAYS_INLINE_ static ViewportScaling3DType scaling_3d_mode_type(ViewportScaling3DMode p_mode) {
		return RSE::scaling_3d_mode_type(p_mode);
	}
	_ALWAYS_INLINE_ static bool scaling_3d_mode_is_vendor_temporal(ViewportScaling3DMode p_mode) {
		return RSE::scaling_3d_mode_is_vendor_temporal(p_mode);
	}
	_ALWAYS_INLINE_ static bool frame_generation_mode_is_vendor(ViewportFrameGenerationMode p_mode) {
		return RSE::frame_generation_mode_is_vendor(p_mode);
	}

	using ViewportAnisotropicFiltering = RSE::ViewportAnisotropicFiltering;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_DISABLED = RSE::VIEWPORT_ANISOTROPY_DISABLED;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_2X = RSE::VIEWPORT_ANISOTROPY_2X;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_4X = RSE::VIEWPORT_ANISOTROPY_4X;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_8X = RSE::VIEWPORT_ANISOTROPY_8X;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_16X = RSE::VIEWPORT_ANISOTROPY_16X;
	static constexpr ViewportAnisotropicFiltering VIEWPORT_ANISOTROPY_MAX = RSE::VIEWPORT_ANISOTROPY_MAX;

	using ViewportUpdateMode = RSE::ViewportUpdateMode;
	static constexpr ViewportUpdateMode VIEWPORT_UPDATE_DISABLED = RSE::VIEWPORT_UPDATE_DISABLED;
	static constexpr ViewportUpdateMode VIEWPORT_UPDATE_ONCE = RSE::VIEWPORT_UPDATE_ONCE;
	static constexpr ViewportUpdateMode VIEWPORT_UPDATE_WHEN_VISIBLE = RSE::VIEWPORT_UPDATE_WHEN_VISIBLE;
	static constexpr ViewportUpdateMode VIEWPORT_UPDATE_WHEN_PARENT_VISIBLE = RSE::VIEWPORT_UPDATE_WHEN_PARENT_VISIBLE;
	static constexpr ViewportUpdateMode VIEWPORT_UPDATE_ALWAYS = RSE::VIEWPORT_UPDATE_ALWAYS;

	using ViewportClearMode = RSE::ViewportClearMode;
	static constexpr ViewportClearMode VIEWPORT_CLEAR_ALWAYS = RSE::VIEWPORT_CLEAR_ALWAYS;
	static constexpr ViewportClearMode VIEWPORT_CLEAR_NEVER = RSE::VIEWPORT_CLEAR_NEVER;
	static constexpr ViewportClearMode VIEWPORT_CLEAR_ONLY_NEXT_FRAME = RSE::VIEWPORT_CLEAR_ONLY_NEXT_FRAME;

	using ViewportEnvironmentMode = RSE::ViewportEnvironmentMode;
	static constexpr ViewportEnvironmentMode VIEWPORT_ENVIRONMENT_DISABLED = RSE::VIEWPORT_ENVIRONMENT_DISABLED;
	static constexpr ViewportEnvironmentMode VIEWPORT_ENVIRONMENT_ENABLED = RSE::VIEWPORT_ENVIRONMENT_ENABLED;
	static constexpr ViewportEnvironmentMode VIEWPORT_ENVIRONMENT_INHERIT = RSE::VIEWPORT_ENVIRONMENT_INHERIT;
	static constexpr ViewportEnvironmentMode VIEWPORT_ENVIRONMENT_MAX = RSE::VIEWPORT_ENVIRONMENT_MAX;

	using ViewportSDFOversize = RSE::ViewportSDFOversize;
	static constexpr ViewportSDFOversize VIEWPORT_SDF_OVERSIZE_100_PERCENT = RSE::VIEWPORT_SDF_OVERSIZE_100_PERCENT;
	static constexpr ViewportSDFOversize VIEWPORT_SDF_OVERSIZE_120_PERCENT = RSE::VIEWPORT_SDF_OVERSIZE_120_PERCENT;
	static constexpr ViewportSDFOversize VIEWPORT_SDF_OVERSIZE_150_PERCENT = RSE::VIEWPORT_SDF_OVERSIZE_150_PERCENT;
	static constexpr ViewportSDFOversize VIEWPORT_SDF_OVERSIZE_200_PERCENT = RSE::VIEWPORT_SDF_OVERSIZE_200_PERCENT;
	static constexpr ViewportSDFOversize VIEWPORT_SDF_OVERSIZE_MAX = RSE::VIEWPORT_SDF_OVERSIZE_MAX;

	using ViewportSDFScale = RSE::ViewportSDFScale;
	static constexpr ViewportSDFScale VIEWPORT_SDF_SCALE_100_PERCENT = RSE::VIEWPORT_SDF_SCALE_100_PERCENT;
	static constexpr ViewportSDFScale VIEWPORT_SDF_SCALE_50_PERCENT = RSE::VIEWPORT_SDF_SCALE_50_PERCENT;
	static constexpr ViewportSDFScale VIEWPORT_SDF_SCALE_25_PERCENT = RSE::VIEWPORT_SDF_SCALE_25_PERCENT;
	static constexpr ViewportSDFScale VIEWPORT_SDF_SCALE_MAX = RSE::VIEWPORT_SDF_SCALE_MAX;

	using ViewportMSAA = RSE::ViewportMSAA;
	static constexpr ViewportMSAA VIEWPORT_MSAA_DISABLED = RSE::VIEWPORT_MSAA_DISABLED;
	static constexpr ViewportMSAA VIEWPORT_MSAA_2X = RSE::VIEWPORT_MSAA_2X;
	static constexpr ViewportMSAA VIEWPORT_MSAA_4X = RSE::VIEWPORT_MSAA_4X;
	static constexpr ViewportMSAA VIEWPORT_MSAA_8X = RSE::VIEWPORT_MSAA_8X;
	static constexpr ViewportMSAA VIEWPORT_MSAA_MAX = RSE::VIEWPORT_MSAA_MAX;

	using ViewportScreenSpaceAA = RSE::ViewportScreenSpaceAA;
	static constexpr ViewportScreenSpaceAA VIEWPORT_SCREEN_SPACE_AA_DISABLED = RSE::VIEWPORT_SCREEN_SPACE_AA_DISABLED;
	static constexpr ViewportScreenSpaceAA VIEWPORT_SCREEN_SPACE_AA_FXAA = RSE::VIEWPORT_SCREEN_SPACE_AA_FXAA;
	static constexpr ViewportScreenSpaceAA VIEWPORT_SCREEN_SPACE_AA_SMAA = RSE::VIEWPORT_SCREEN_SPACE_AA_SMAA;
	static constexpr ViewportScreenSpaceAA VIEWPORT_SCREEN_SPACE_AA_MAX = RSE::VIEWPORT_SCREEN_SPACE_AA_MAX;

	using ViewportOcclusionCullingBuildQuality = RSE::ViewportOcclusionCullingBuildQuality;
	static constexpr ViewportOcclusionCullingBuildQuality VIEWPORT_OCCLUSION_BUILD_QUALITY_LOW = RSE::VIEWPORT_OCCLUSION_BUILD_QUALITY_LOW;
	static constexpr ViewportOcclusionCullingBuildQuality VIEWPORT_OCCLUSION_BUILD_QUALITY_MEDIUM = RSE::VIEWPORT_OCCLUSION_BUILD_QUALITY_MEDIUM;
	static constexpr ViewportOcclusionCullingBuildQuality VIEWPORT_OCCLUSION_BUILD_QUALITY_HIGH = RSE::VIEWPORT_OCCLUSION_BUILD_QUALITY_HIGH;

	using ViewportRenderInfo = RSE::ViewportRenderInfo;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME = RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME = RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME = RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES = RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS = RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RT_BLAS_REFITS = RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT = RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT = RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTED_COPY_COUNT = RSE::VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTED_COPY_COUNT;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RTGI_RAW_FALLBACK_COPY_COUNT = RSE::VIEWPORT_RENDER_INFO_RTGI_RAW_FALLBACK_COPY_COUNT;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTION_GUIDE_QUALITY = RSE::VIEWPORT_RENDER_INFO_RTGI_RECONSTRUCTION_GUIDE_QUALITY;
	static constexpr ViewportRenderInfo VIEWPORT_RENDER_INFO_MAX = RSE::VIEWPORT_RENDER_INFO_MAX;

	using ViewportRenderInfoType = RSE::ViewportRenderInfoType;
	static constexpr ViewportRenderInfoType VIEWPORT_RENDER_INFO_TYPE_VISIBLE = RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE;
	static constexpr ViewportRenderInfoType VIEWPORT_RENDER_INFO_TYPE_SHADOW = RSE::VIEWPORT_RENDER_INFO_TYPE_SHADOW;
	static constexpr ViewportRenderInfoType VIEWPORT_RENDER_INFO_TYPE_CANVAS = RSE::VIEWPORT_RENDER_INFO_TYPE_CANVAS;
	static constexpr ViewportRenderInfoType VIEWPORT_RENDER_INFO_TYPE_MAX = RSE::VIEWPORT_RENDER_INFO_TYPE_MAX;

	using ViewportDebugDraw = RSE::ViewportDebugDraw;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_DISABLED = RSE::VIEWPORT_DEBUG_DRAW_DISABLED;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_UNSHADED = RSE::VIEWPORT_DEBUG_DRAW_UNSHADED;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_LIGHTING = RSE::VIEWPORT_DEBUG_DRAW_LIGHTING;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_OVERDRAW = RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_WIREFRAME = RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER = RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_VOXEL_GI_ALBEDO = RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_ALBEDO;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING = RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_LIGHTING;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION = RSE::VIEWPORT_DEBUG_DRAW_VOXEL_GI_EMISSION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SHADOW_ATLAS = RSE::VIEWPORT_DEBUG_DRAW_SHADOW_ATLAS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_DIRECTIONAL_SHADOW_ATLAS = RSE::VIEWPORT_DEBUG_DRAW_DIRECTIONAL_SHADOW_ATLAS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SCENE_LUMINANCE = RSE::VIEWPORT_DEBUG_DRAW_SCENE_LUMINANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SSAO = RSE::VIEWPORT_DEBUG_DRAW_SSAO;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SSIL = RSE::VIEWPORT_DEBUG_DRAW_SSIL;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_PSSM_SPLITS = RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_DECAL_ATLAS = RSE::VIEWPORT_DEBUG_DRAW_DECAL_ATLAS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SDFGI = RSE::VIEWPORT_DEBUG_DRAW_SDFGI;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_SDFGI_PROBES = RSE::VIEWPORT_DEBUG_DRAW_SDFGI_PROBES;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_GI_BUFFER = RSE::VIEWPORT_DEBUG_DRAW_GI_BUFFER;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_DISABLE_LOD = RSE::VIEWPORT_DEBUG_DRAW_DISABLE_LOD;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS = RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS = RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS = RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES = RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_REFLECTION_PROBES;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_OCCLUDERS = RSE::VIEWPORT_DEBUG_DRAW_OCCLUDERS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_MOTION_VECTORS = RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_INTERNAL_BUFFER = RSE::VIEWPORT_DEBUG_DRAW_INTERNAL_BUFFER;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS = RSE::VIEWPORT_DEBUG_DRAW_CLUSTER_AREA_LIGHTS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_AREA_LIGHT_ATLAS = RSE::VIEWPORT_DEBUG_DRAW_AREA_LIGHT_ATLAS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RECONSTRUCTED_DEPTH = RSE::VIEWPORT_DEBUG_DRAW_RECONSTRUCTED_DEPTH;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_NOISY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_NOISY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_NORMAL_ROUGHNESS = RSE::VIEWPORT_DEBUG_DRAW_RTGI_NORMAL_ROUGHNESS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_VIEWZ_HITDIST = RSE::VIEWPORT_DEBUG_DRAW_RTGI_VIEWZ_HITDIST;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_MOTION_VECTORS = RSE::VIEWPORT_DEBUG_DRAW_RTGI_MOTION_VECTORS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_VARIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_VARIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_HISTORY_LENGTH = RSE::VIEWPORT_DEBUG_DRAW_RTGI_HISTORY_LENGTH;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_FINAL = RSE::VIEWPORT_DEBUG_DRAW_RTGI_FINAL;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_REACTIVITY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_REACTIVITY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_NOISY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_NOISY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_NOISY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_NOISY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_FINAL = RSE::VIEWPORT_DEBUG_DRAW_RTGI_DIFFUSE_FINAL;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_FINAL = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_FINAL;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_GUIDE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_GUIDE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTION_DIRECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTION_DIRECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_DISTANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_DISTANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_NORMAL = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REFLECTED_HIT_NORMAL;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_ROUGHNESS_BUCKET = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_ROUGHNESS_BUCKET;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_HISTORY_LENGTH = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_HISTORY_LENGTH;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SPECULAR_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_DIRECT_LIGHT = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_DIRECT_LIGHT;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_EMISSIVE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_EMISSIVE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_INDIRECT = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_INDIRECT;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_SKY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_SKY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SIGNAL_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_RAW_DIFFUSE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_RAW_DIFFUSE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_HIT_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_HIT_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_AGE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_AGE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_FILTERED_DIFFUSE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_STATS = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_STATS;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_PLANE_QUALITY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_PLANE_QUALITY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_VISIBILITY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_VISIBILITY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINEMENT = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINEMENT;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINED_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_CACHE_SPG_REFINED_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_UPDATES = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_UPDATES;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_VISIBILITY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_VISIBILITY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_AGE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_AGE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_VARIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_VARIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_STRC_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_STRC_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_WRC_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_WRC_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_WRC_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_WRC_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_CANDIDATE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_CANDIDATE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_HISTORY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_HISTORY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_TEMPORAL_DELTA = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_TEMPORAL_DELTA;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SOURCE_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SOURCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SOURCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_REJECTION = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_REJECTION;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SURFACE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SECONDARY_CACHE_SURFACE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_FEEDBACK = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_FEEDBACK;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_KEY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_SURFACE_KEY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RAW_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RAW_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_DENOISED_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_DENOISED_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_RADIANCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTED_RADIANCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_REACTIVITY = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_REACTIVITY;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_SIGNAL_CONFIDENCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_SIGNAL_CONFIDENCE;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_GUIDE_MISMATCH = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_GUIDE_MISMATCH;
	static constexpr ViewportDebugDraw VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_FILL_SOURCE = RSE::VIEWPORT_DEBUG_DRAW_RTGI_RECONSTRUCTION_FILL_SOURCE;

	using ViewportVRSMode = RSE::ViewportVRSMode;
	static constexpr ViewportVRSMode VIEWPORT_VRS_DISABLED = RSE::VIEWPORT_VRS_DISABLED;
	static constexpr ViewportVRSMode VIEWPORT_VRS_TEXTURE = RSE::VIEWPORT_VRS_TEXTURE;
	static constexpr ViewportVRSMode VIEWPORT_VRS_XR = RSE::VIEWPORT_VRS_XR;
	static constexpr ViewportVRSMode VIEWPORT_VRS_MAX = RSE::VIEWPORT_VRS_MAX;

	using ViewportVRSUpdateMode = RSE::ViewportVRSUpdateMode;
	static constexpr ViewportVRSUpdateMode VIEWPORT_VRS_UPDATE_DISABLED = RSE::VIEWPORT_VRS_UPDATE_DISABLED;
	static constexpr ViewportVRSUpdateMode VIEWPORT_VRS_UPDATE_ONCE = RSE::VIEWPORT_VRS_UPDATE_ONCE;
	static constexpr ViewportVRSUpdateMode VIEWPORT_VRS_UPDATE_ALWAYS = RSE::VIEWPORT_VRS_UPDATE_ALWAYS;
	static constexpr ViewportVRSUpdateMode VIEWPORT_VRS_UPDATE_MAX = RSE::VIEWPORT_VRS_UPDATE_MAX;

	using SkyMode = RSE::SkyMode;
	static constexpr SkyMode SKY_MODE_AUTOMATIC = RSE::SKY_MODE_AUTOMATIC;
	static constexpr SkyMode SKY_MODE_QUALITY = RSE::SKY_MODE_QUALITY;
	static constexpr SkyMode SKY_MODE_INCREMENTAL = RSE::SKY_MODE_INCREMENTAL;
	static constexpr SkyMode SKY_MODE_REALTIME = RSE::SKY_MODE_REALTIME;

	using CompositorEffectFlags = RSE::CompositorEffectFlags;
	static constexpr CompositorEffectFlags COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR = RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_COLOR;
	static constexpr CompositorEffectFlags COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH = RSE::COMPOSITOR_EFFECT_FLAG_ACCESS_RESOLVED_DEPTH;
	static constexpr CompositorEffectFlags COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS = RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_MOTION_VECTORS;
	static constexpr CompositorEffectFlags COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS = RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_ROUGHNESS;
	static constexpr CompositorEffectFlags COMPOSITOR_EFFECT_FLAG_NEEDS_SEPARATE_SPECULAR = RSE::COMPOSITOR_EFFECT_FLAG_NEEDS_SEPARATE_SPECULAR;

	using CompositorEffectCallbackType = RSE::CompositorEffectCallbackType;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_SKY;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_MAX = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_MAX;
	static constexpr CompositorEffectCallbackType COMPOSITOR_EFFECT_CALLBACK_TYPE_ANY = RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_ANY;

	using EnvironmentBG = RSE::EnvironmentBG;
	static constexpr EnvironmentBG ENV_BG_CLEAR_COLOR = RSE::ENV_BG_CLEAR_COLOR;
	static constexpr EnvironmentBG ENV_BG_COLOR = RSE::ENV_BG_COLOR;
	static constexpr EnvironmentBG ENV_BG_SKY = RSE::ENV_BG_SKY;
	static constexpr EnvironmentBG ENV_BG_CANVAS = RSE::ENV_BG_CANVAS;
	static constexpr EnvironmentBG ENV_BG_KEEP = RSE::ENV_BG_KEEP;
	static constexpr EnvironmentBG ENV_BG_CAMERA_FEED = RSE::ENV_BG_CAMERA_FEED;
	static constexpr EnvironmentBG ENV_BG_MAX = RSE::ENV_BG_MAX;

	using EnvironmentAmbientSource = RSE::EnvironmentAmbientSource;
	static constexpr EnvironmentAmbientSource ENV_AMBIENT_SOURCE_BG = RSE::ENV_AMBIENT_SOURCE_BG;
	static constexpr EnvironmentAmbientSource ENV_AMBIENT_SOURCE_DISABLED = RSE::ENV_AMBIENT_SOURCE_DISABLED;
	static constexpr EnvironmentAmbientSource ENV_AMBIENT_SOURCE_COLOR = RSE::ENV_AMBIENT_SOURCE_COLOR;
	static constexpr EnvironmentAmbientSource ENV_AMBIENT_SOURCE_SKY = RSE::ENV_AMBIENT_SOURCE_SKY;

	using EnvironmentReflectionSource = RSE::EnvironmentReflectionSource;
	static constexpr EnvironmentReflectionSource ENV_REFLECTION_SOURCE_BG = RSE::ENV_REFLECTION_SOURCE_BG;
	static constexpr EnvironmentReflectionSource ENV_REFLECTION_SOURCE_DISABLED = RSE::ENV_REFLECTION_SOURCE_DISABLED;
	static constexpr EnvironmentReflectionSource ENV_REFLECTION_SOURCE_SKY = RSE::ENV_REFLECTION_SOURCE_SKY;

	using EnvironmentGlowBlendMode = RSE::EnvironmentGlowBlendMode;
	static constexpr EnvironmentGlowBlendMode ENV_GLOW_BLEND_MODE_ADDITIVE = RSE::ENV_GLOW_BLEND_MODE_ADDITIVE;
	static constexpr EnvironmentGlowBlendMode ENV_GLOW_BLEND_MODE_SCREEN = RSE::ENV_GLOW_BLEND_MODE_SCREEN;
	static constexpr EnvironmentGlowBlendMode ENV_GLOW_BLEND_MODE_SOFTLIGHT = RSE::ENV_GLOW_BLEND_MODE_SOFTLIGHT;
	static constexpr EnvironmentGlowBlendMode ENV_GLOW_BLEND_MODE_REPLACE = RSE::ENV_GLOW_BLEND_MODE_REPLACE;
	static constexpr EnvironmentGlowBlendMode ENV_GLOW_BLEND_MODE_MIX = RSE::ENV_GLOW_BLEND_MODE_MIX;

	using EnvironmentToneMapper = RSE::EnvironmentToneMapper;
	static constexpr EnvironmentToneMapper ENV_TONE_MAPPER_LINEAR = RSE::ENV_TONE_MAPPER_LINEAR;
	static constexpr EnvironmentToneMapper ENV_TONE_MAPPER_REINHARD = RSE::ENV_TONE_MAPPER_REINHARD;
	static constexpr EnvironmentToneMapper ENV_TONE_MAPPER_FILMIC = RSE::ENV_TONE_MAPPER_FILMIC;
	static constexpr EnvironmentToneMapper ENV_TONE_MAPPER_ACES = RSE::ENV_TONE_MAPPER_ACES;
	static constexpr EnvironmentToneMapper ENV_TONE_MAPPER_AGX = RSE::ENV_TONE_MAPPER_AGX;

	using EnvironmentSSRRoughnessQuality = RSE::EnvironmentSSRRoughnessQuality;
	static constexpr EnvironmentSSRRoughnessQuality ENV_SSR_ROUGHNESS_QUALITY_DISABLED = RSE::ENV_SSR_ROUGHNESS_QUALITY_DISABLED;
	static constexpr EnvironmentSSRRoughnessQuality ENV_SSR_ROUGHNESS_QUALITY_LOW = RSE::ENV_SSR_ROUGHNESS_QUALITY_LOW;
	static constexpr EnvironmentSSRRoughnessQuality ENV_SSR_ROUGHNESS_QUALITY_MEDIUM = RSE::ENV_SSR_ROUGHNESS_QUALITY_MEDIUM;
	static constexpr EnvironmentSSRRoughnessQuality ENV_SSR_ROUGHNESS_QUALITY_HIGH = RSE::ENV_SSR_ROUGHNESS_QUALITY_HIGH;

	using EnvironmentSSAOQuality = RSE::EnvironmentSSAOQuality;
	static constexpr EnvironmentSSAOQuality ENV_SSAO_QUALITY_VERY_LOW = RSE::ENV_SSAO_QUALITY_VERY_LOW;
	static constexpr EnvironmentSSAOQuality ENV_SSAO_QUALITY_LOW = RSE::ENV_SSAO_QUALITY_LOW;
	static constexpr EnvironmentSSAOQuality ENV_SSAO_QUALITY_MEDIUM = RSE::ENV_SSAO_QUALITY_MEDIUM;
	static constexpr EnvironmentSSAOQuality ENV_SSAO_QUALITY_HIGH = RSE::ENV_SSAO_QUALITY_HIGH;
	static constexpr EnvironmentSSAOQuality ENV_SSAO_QUALITY_ULTRA = RSE::ENV_SSAO_QUALITY_ULTRA;

	using EnvironmentSSILQuality = RSE::EnvironmentSSILQuality;
	static constexpr EnvironmentSSILQuality ENV_SSIL_QUALITY_VERY_LOW = RSE::ENV_SSIL_QUALITY_VERY_LOW;
	static constexpr EnvironmentSSILQuality ENV_SSIL_QUALITY_LOW = RSE::ENV_SSIL_QUALITY_LOW;
	static constexpr EnvironmentSSILQuality ENV_SSIL_QUALITY_MEDIUM = RSE::ENV_SSIL_QUALITY_MEDIUM;
	static constexpr EnvironmentSSILQuality ENV_SSIL_QUALITY_HIGH = RSE::ENV_SSIL_QUALITY_HIGH;
	static constexpr EnvironmentSSILQuality ENV_SSIL_QUALITY_ULTRA = RSE::ENV_SSIL_QUALITY_ULTRA;

	using EnvironmentSDFGIYScale = RSE::EnvironmentSDFGIYScale;
	static constexpr EnvironmentSDFGIYScale ENV_SDFGI_Y_SCALE_50_PERCENT = RSE::ENV_SDFGI_Y_SCALE_50_PERCENT;
	static constexpr EnvironmentSDFGIYScale ENV_SDFGI_Y_SCALE_75_PERCENT = RSE::ENV_SDFGI_Y_SCALE_75_PERCENT;
	static constexpr EnvironmentSDFGIYScale ENV_SDFGI_Y_SCALE_100_PERCENT = RSE::ENV_SDFGI_Y_SCALE_100_PERCENT;

	using EnvironmentSDFGIRayCount = RSE::EnvironmentSDFGIRayCount;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_4 = RSE::ENV_SDFGI_RAY_COUNT_4;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_8 = RSE::ENV_SDFGI_RAY_COUNT_8;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_16 = RSE::ENV_SDFGI_RAY_COUNT_16;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_32 = RSE::ENV_SDFGI_RAY_COUNT_32;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_64 = RSE::ENV_SDFGI_RAY_COUNT_64;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_96 = RSE::ENV_SDFGI_RAY_COUNT_96;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_128 = RSE::ENV_SDFGI_RAY_COUNT_128;
	static constexpr EnvironmentSDFGIRayCount ENV_SDFGI_RAY_COUNT_MAX = RSE::ENV_SDFGI_RAY_COUNT_MAX;

	using EnvironmentSDFGIFramesToConverge = RSE::EnvironmentSDFGIFramesToConverge;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_5_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_5_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_10_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_10_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_15_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_15_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_20_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_20_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_25_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_25_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_IN_30_FRAMES = RSE::ENV_SDFGI_CONVERGE_IN_30_FRAMES;
	static constexpr EnvironmentSDFGIFramesToConverge ENV_SDFGI_CONVERGE_MAX = RSE::ENV_SDFGI_CONVERGE_MAX;

	using EnvironmentSDFGIFramesToUpdateLight = RSE::EnvironmentSDFGIFramesToUpdateLight;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_IN_1_FRAME = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_1_FRAME;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_IN_2_FRAMES = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_2_FRAMES;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_IN_4_FRAMES = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_4_FRAMES;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_IN_8_FRAMES = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_8_FRAMES;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_IN_16_FRAMES = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_16_FRAMES;
	static constexpr EnvironmentSDFGIFramesToUpdateLight ENV_SDFGI_UPDATE_LIGHT_MAX = RSE::ENV_SDFGI_UPDATE_LIGHT_MAX;

	using EnvironmentFogMode = RSE::EnvironmentFogMode;
	static constexpr EnvironmentFogMode ENV_FOG_MODE_EXPONENTIAL = RSE::ENV_FOG_MODE_EXPONENTIAL;
	static constexpr EnvironmentFogMode ENV_FOG_MODE_DEPTH = RSE::ENV_FOG_MODE_DEPTH;

	using PathtracingDenoiser = RSE::PathtracingDenoiser;
	static constexpr PathtracingDenoiser PT_DENOISER_NONE = RSE::PT_DENOISER_NONE;
	static constexpr PathtracingDenoiser PT_DENOISER_RESERVED_1 = RSE::PT_DENOISER_RESERVED_1;
	static constexpr PathtracingDenoiser PT_DENOISER_INTERNAL = RSE::PT_DENOISER_INTERNAL;
	static constexpr PathtracingDenoiser PT_DENOISER_RESERVED_4 = RSE::PT_DENOISER_RESERVED_4;
	static constexpr PathtracingDenoiser PT_DENOISER_NVIDIA = RSE::PT_DENOISER_NVIDIA;
	static constexpr PathtracingDenoiser PT_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION = RSE::PT_DENOISER_INTERNAL_SIGNAL_DECOMPOSITION;

	using PathtracingBackend = RSE::PathtracingBackend;
	static constexpr PathtracingBackend PT_BACKEND_VULKAN_GENERIC = RSE::PT_BACKEND_VULKAN_GENERIC;
	static constexpr PathtracingBackend PT_BACKEND_NVIDIA_RTXPT = RSE::PT_BACKEND_NVIDIA_RTXPT;
	static constexpr PathtracingBackend PT_BACKEND_AMD_HIP_RT = RSE::PT_BACKEND_AMD_HIP_RT;
	static constexpr PathtracingBackend PT_BACKEND_INTEL_EMBREE = RSE::PT_BACKEND_INTEL_EMBREE;
	static constexpr PathtracingBackend PT_BACKEND_MAX = RSE::PT_BACKEND_MAX;

	using PathtracingParamIndex = RSE::PathtracingParamIndex;
	static constexpr PathtracingParamIndex PT_PARAM_VIS_MODE = RSE::PT_PARAM_VIS_MODE;
	static constexpr PathtracingParamIndex PT_PARAM_SAMPLE_COUNT = RSE::PT_PARAM_SAMPLE_COUNT;
	static constexpr PathtracingParamIndex PT_PARAM_MAX_BOUNCES = RSE::PT_PARAM_MAX_BOUNCES;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER = RSE::PT_PARAM_DENOISER;
	static constexpr PathtracingParamIndex PT_PARAM_ENERGY = RSE::PT_PARAM_ENERGY;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_RESOLUTION_SCALE = RSE::PT_PARAM_RTGI_RESOLUTION_SCALE;
	static constexpr PathtracingParamIndex PT_PARAM_MODE = RSE::PT_PARAM_MODE;
	static constexpr PathtracingParamIndex PT_PARAM_OVERSCAN_HORIZONTAL = RSE::PT_PARAM_OVERSCAN_HORIZONTAL;
	static constexpr PathtracingParamIndex PT_PARAM_OVERSCAN_VERTICAL = RSE::PT_PARAM_OVERSCAN_VERTICAL;
	static constexpr PathtracingParamIndex PT_PARAM_LIGHT_COUNT = RSE::PT_PARAM_LIGHT_COUNT;
	static constexpr PathtracingParamIndex PT_PARAM_FRAME_INDEX = RSE::PT_PARAM_FRAME_INDEX;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_STRENGTH = RSE::PT_PARAM_DENOISER_STRENGTH;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_HISTORY_WEIGHT = RSE::PT_PARAM_DENOISER_HISTORY_WEIGHT;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_FIREFLY_SUPPRESSION = RSE::PT_PARAM_DENOISER_FIREFLY_SUPPRESSION;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_DETAIL_PRESERVATION = RSE::PT_PARAM_DENOISER_DETAIL_PRESERVATION;
	static constexpr PathtracingParamIndex PT_PARAM_RAY_FIREFLY_SUPPRESSION = RSE::PT_PARAM_RAY_FIREFLY_SUPPRESSION;
	static constexpr PathtracingParamIndex PT_PARAM_RAY_MAX_RADIANCE = RSE::PT_PARAM_RAY_MAX_RADIANCE;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_SPLIT_SIGNALS = RSE::PT_PARAM_DENOISER_SPLIT_SIGNALS;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_SPECULAR_HISTORY_WEIGHT = RSE::PT_PARAM_DENOISER_SPECULAR_HISTORY_WEIGHT;
	static constexpr PathtracingParamIndex PT_PARAM_DENOISER_SPECULAR_SPATIAL_STRENGTH = RSE::PT_PARAM_DENOISER_SPECULAR_SPATIAL_STRENGTH;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_SAMPLING_CONTROLS = RSE::PT_PARAM_RTGI_SAMPLING_CONTROLS;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED = RSE::PT_PARAM_RTGI_DIFFUSE_CACHE_ENABLED;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_ENABLED = RSE::PT_PARAM_RTGI_STRC_ENABLED;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_STRENGTH = RSE::PT_PARAM_RTGI_STRC_STRENGTH;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_CASCADE_COUNT = RSE::PT_PARAM_RTGI_STRC_CASCADE_COUNT;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_GRID_SIZE = RSE::PT_PARAM_RTGI_STRC_GRID_SIZE;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_BASE_PROBE_SPACING = RSE::PT_PARAM_RTGI_STRC_BASE_PROBE_SPACING;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_RAYS_PER_FRAME = RSE::PT_PARAM_RTGI_STRC_RAYS_PER_FRAME;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_TEMPORAL_WEIGHT = RSE::PT_PARAM_RTGI_STRC_TEMPORAL_WEIGHT;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_BACKEND = RSE::PT_PARAM_RTGI_BACKEND;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_STATIC_VISUAL_LAYERS = RSE::PT_PARAM_RTGI_STRC_STATIC_VISUAL_LAYERS;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_STRC_DYNAMIC_VISUAL_LAYERS = RSE::PT_PARAM_RTGI_STRC_DYNAMIC_VISUAL_LAYERS;
	static constexpr PathtracingParamIndex PT_PARAM_RTGI_DIFFUSE_CACHE_MAX_ENTRIES = RSE::PT_PARAM_RTGI_DIFFUSE_CACHE_MAX_ENTRIES;
	static constexpr PathtracingParamIndex PT_PARAM_MAX = RSE::PT_PARAM_MAX;

	using SubSurfaceScatteringQuality = RSE::SubSurfaceScatteringQuality;
	static constexpr SubSurfaceScatteringQuality SUB_SURFACE_SCATTERING_QUALITY_DISABLED = RSE::SUB_SURFACE_SCATTERING_QUALITY_DISABLED;
	static constexpr SubSurfaceScatteringQuality SUB_SURFACE_SCATTERING_QUALITY_LOW = RSE::SUB_SURFACE_SCATTERING_QUALITY_LOW;
	static constexpr SubSurfaceScatteringQuality SUB_SURFACE_SCATTERING_QUALITY_MEDIUM = RSE::SUB_SURFACE_SCATTERING_QUALITY_MEDIUM;
	static constexpr SubSurfaceScatteringQuality SUB_SURFACE_SCATTERING_QUALITY_HIGH = RSE::SUB_SURFACE_SCATTERING_QUALITY_HIGH;

	using DOFBlurQuality = RSE::DOFBlurQuality;
	static constexpr DOFBlurQuality DOF_BLUR_QUALITY_VERY_LOW = RSE::DOF_BLUR_QUALITY_VERY_LOW;
	static constexpr DOFBlurQuality DOF_BLUR_QUALITY_LOW = RSE::DOF_BLUR_QUALITY_LOW;
	static constexpr DOFBlurQuality DOF_BLUR_QUALITY_MEDIUM = RSE::DOF_BLUR_QUALITY_MEDIUM;
	static constexpr DOFBlurQuality DOF_BLUR_QUALITY_HIGH = RSE::DOF_BLUR_QUALITY_HIGH;

	using DOFBokehShape = RSE::DOFBokehShape;
	static constexpr DOFBokehShape DOF_BOKEH_BOX = RSE::DOF_BOKEH_BOX;
	static constexpr DOFBokehShape DOF_BOKEH_HEXAGON = RSE::DOF_BOKEH_HEXAGON;
	static constexpr DOFBokehShape DOF_BOKEH_CIRCLE = RSE::DOF_BOKEH_CIRCLE;

	using InstanceType = RSE::InstanceType;
	static constexpr InstanceType INSTANCE_NONE = RSE::INSTANCE_NONE;
	static constexpr InstanceType INSTANCE_MESH = RSE::INSTANCE_MESH;
	static constexpr InstanceType INSTANCE_MULTIMESH = RSE::INSTANCE_MULTIMESH;
	static constexpr InstanceType INSTANCE_PARTICLES = RSE::INSTANCE_PARTICLES;
	static constexpr InstanceType INSTANCE_PARTICLES_COLLISION = RSE::INSTANCE_PARTICLES_COLLISION;
	static constexpr InstanceType INSTANCE_LIGHT = RSE::INSTANCE_LIGHT;
	static constexpr InstanceType INSTANCE_REFLECTION_PROBE = RSE::INSTANCE_REFLECTION_PROBE;
	static constexpr InstanceType INSTANCE_DECAL = RSE::INSTANCE_DECAL;
	static constexpr InstanceType INSTANCE_VOXEL_GI = RSE::INSTANCE_VOXEL_GI;
	static constexpr InstanceType INSTANCE_LIGHTMAP = RSE::INSTANCE_LIGHTMAP;
	static constexpr InstanceType INSTANCE_OCCLUDER = RSE::INSTANCE_OCCLUDER;
	static constexpr InstanceType INSTANCE_VISIBLITY_NOTIFIER = RSE::INSTANCE_VISIBLITY_NOTIFIER;
	static constexpr InstanceType INSTANCE_FOG_VOLUME = RSE::INSTANCE_FOG_VOLUME;
	static constexpr InstanceType INSTANCE_MAX = RSE::INSTANCE_MAX;
	static constexpr InstanceType INSTANCE_GEOMETRY_MASK = RSE::INSTANCE_GEOMETRY_MASK;

	using InstanceFlags = RSE::InstanceFlags;
	static constexpr InstanceFlags INSTANCE_FLAG_USE_BAKED_LIGHT = RSE::INSTANCE_FLAG_USE_BAKED_LIGHT;
	static constexpr InstanceFlags INSTANCE_FLAG_USE_DYNAMIC_GI = RSE::INSTANCE_FLAG_USE_DYNAMIC_GI;
	static constexpr InstanceFlags INSTANCE_FLAG_DRAW_NEXT_FRAME_IF_VISIBLE = RSE::INSTANCE_FLAG_DRAW_NEXT_FRAME_IF_VISIBLE;
	static constexpr InstanceFlags INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING = RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING;
	static constexpr InstanceFlags INSTANCE_FLAG_MAX = RSE::INSTANCE_FLAG_MAX;

	using ShadowCastingSetting = RSE::ShadowCastingSetting;
	static constexpr ShadowCastingSetting SHADOW_CASTING_SETTING_OFF = RSE::SHADOW_CASTING_SETTING_OFF;
	static constexpr ShadowCastingSetting SHADOW_CASTING_SETTING_ON = RSE::SHADOW_CASTING_SETTING_ON;
	static constexpr ShadowCastingSetting SHADOW_CASTING_SETTING_DOUBLE_SIDED = RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED;
	static constexpr ShadowCastingSetting SHADOW_CASTING_SETTING_SHADOWS_ONLY = RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY;

	using VisibilityRangeFadeMode = RSE::VisibilityRangeFadeMode;
	static constexpr VisibilityRangeFadeMode VISIBILITY_RANGE_FADE_DISABLED = RSE::VISIBILITY_RANGE_FADE_DISABLED;
	static constexpr VisibilityRangeFadeMode VISIBILITY_RANGE_FADE_SELF = RSE::VISIBILITY_RANGE_FADE_SELF;
	static constexpr VisibilityRangeFadeMode VISIBILITY_RANGE_FADE_DEPENDENCIES = RSE::VISIBILITY_RANGE_FADE_DEPENDENCIES;

	using BakeChannels = RSE::BakeChannels;
	static constexpr BakeChannels BAKE_CHANNEL_ALBEDO_ALPHA = RSE::BAKE_CHANNEL_ALBEDO_ALPHA;
	static constexpr BakeChannels BAKE_CHANNEL_NORMAL = RSE::BAKE_CHANNEL_NORMAL;
	static constexpr BakeChannels BAKE_CHANNEL_ORM = RSE::BAKE_CHANNEL_ORM;
	static constexpr BakeChannels BAKE_CHANNEL_EMISSION = RSE::BAKE_CHANNEL_EMISSION;

	using CanvasTextureChannel = RSE::CanvasTextureChannel;
	static constexpr CanvasTextureChannel CANVAS_TEXTURE_CHANNEL_DIFFUSE = RSE::CANVAS_TEXTURE_CHANNEL_DIFFUSE;
	static constexpr CanvasTextureChannel CANVAS_TEXTURE_CHANNEL_NORMAL = RSE::CANVAS_TEXTURE_CHANNEL_NORMAL;
	static constexpr CanvasTextureChannel CANVAS_TEXTURE_CHANNEL_SPECULAR = RSE::CANVAS_TEXTURE_CHANNEL_SPECULAR;

	using NinePatchAxisMode = RSE::NinePatchAxisMode;
	static constexpr NinePatchAxisMode NINE_PATCH_STRETCH = RSE::NINE_PATCH_STRETCH;
	static constexpr NinePatchAxisMode NINE_PATCH_TILE = RSE::NINE_PATCH_TILE;
	static constexpr NinePatchAxisMode NINE_PATCH_TILE_FIT = RSE::NINE_PATCH_TILE_FIT;

	using CanvasGroupMode = RSE::CanvasGroupMode;
	static constexpr CanvasGroupMode CANVAS_GROUP_MODE_DISABLED = RSE::CANVAS_GROUP_MODE_DISABLED;
	static constexpr CanvasGroupMode CANVAS_GROUP_MODE_CLIP_ONLY = RSE::CANVAS_GROUP_MODE_CLIP_ONLY;
	static constexpr CanvasGroupMode CANVAS_GROUP_MODE_CLIP_AND_DRAW = RSE::CANVAS_GROUP_MODE_CLIP_AND_DRAW;
	static constexpr CanvasGroupMode CANVAS_GROUP_MODE_TRANSPARENT = RSE::CANVAS_GROUP_MODE_TRANSPARENT;

	using CanvasLightMode = RSE::CanvasLightMode;
	static constexpr CanvasLightMode CANVAS_LIGHT_MODE_POINT = RSE::CANVAS_LIGHT_MODE_POINT;
	static constexpr CanvasLightMode CANVAS_LIGHT_MODE_DIRECTIONAL = RSE::CANVAS_LIGHT_MODE_DIRECTIONAL;

	using CanvasLightBlendMode = RSE::CanvasLightBlendMode;
	static constexpr CanvasLightBlendMode CANVAS_LIGHT_BLEND_MODE_ADD = RSE::CANVAS_LIGHT_BLEND_MODE_ADD;
	static constexpr CanvasLightBlendMode CANVAS_LIGHT_BLEND_MODE_SUB = RSE::CANVAS_LIGHT_BLEND_MODE_SUB;
	static constexpr CanvasLightBlendMode CANVAS_LIGHT_BLEND_MODE_MIX = RSE::CANVAS_LIGHT_BLEND_MODE_MIX;

	using CanvasLightShadowFilter = RSE::CanvasLightShadowFilter;
	static constexpr CanvasLightShadowFilter CANVAS_LIGHT_FILTER_NONE = RSE::CANVAS_LIGHT_FILTER_NONE;
	static constexpr CanvasLightShadowFilter CANVAS_LIGHT_FILTER_PCF5 = RSE::CANVAS_LIGHT_FILTER_PCF5;
	static constexpr CanvasLightShadowFilter CANVAS_LIGHT_FILTER_PCF13 = RSE::CANVAS_LIGHT_FILTER_PCF13;
	static constexpr CanvasLightShadowFilter CANVAS_LIGHT_FILTER_MAX = RSE::CANVAS_LIGHT_FILTER_MAX;

	using CanvasOccluderPolygonCullMode = RSE::CanvasOccluderPolygonCullMode;
	static constexpr CanvasOccluderPolygonCullMode CANVAS_OCCLUDER_POLYGON_CULL_DISABLED = RSE::CANVAS_OCCLUDER_POLYGON_CULL_DISABLED;
	static constexpr CanvasOccluderPolygonCullMode CANVAS_OCCLUDER_POLYGON_CULL_CLOCKWISE = RSE::CANVAS_OCCLUDER_POLYGON_CULL_CLOCKWISE;
	static constexpr CanvasOccluderPolygonCullMode CANVAS_OCCLUDER_POLYGON_CULL_COUNTER_CLOCKWISE = RSE::CANVAS_OCCLUDER_POLYGON_CULL_COUNTER_CLOCKWISE;

	using GlobalShaderParameterType = RSE::GlobalShaderParameterType;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_BOOL = RSE::GLOBAL_VAR_TYPE_BOOL;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_BVEC2 = RSE::GLOBAL_VAR_TYPE_BVEC2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_BVEC3 = RSE::GLOBAL_VAR_TYPE_BVEC3;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_BVEC4 = RSE::GLOBAL_VAR_TYPE_BVEC4;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_INT = RSE::GLOBAL_VAR_TYPE_INT;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_IVEC2 = RSE::GLOBAL_VAR_TYPE_IVEC2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_IVEC3 = RSE::GLOBAL_VAR_TYPE_IVEC3;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_IVEC4 = RSE::GLOBAL_VAR_TYPE_IVEC4;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_RECT2I = RSE::GLOBAL_VAR_TYPE_RECT2I;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_UINT = RSE::GLOBAL_VAR_TYPE_UINT;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_UVEC2 = RSE::GLOBAL_VAR_TYPE_UVEC2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_UVEC3 = RSE::GLOBAL_VAR_TYPE_UVEC3;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_UVEC4 = RSE::GLOBAL_VAR_TYPE_UVEC4;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_FLOAT = RSE::GLOBAL_VAR_TYPE_FLOAT;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_VEC2 = RSE::GLOBAL_VAR_TYPE_VEC2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_VEC3 = RSE::GLOBAL_VAR_TYPE_VEC3;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_VEC4 = RSE::GLOBAL_VAR_TYPE_VEC4;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_COLOR = RSE::GLOBAL_VAR_TYPE_COLOR;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_RECT2 = RSE::GLOBAL_VAR_TYPE_RECT2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_MAT2 = RSE::GLOBAL_VAR_TYPE_MAT2;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_MAT3 = RSE::GLOBAL_VAR_TYPE_MAT3;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_MAT4 = RSE::GLOBAL_VAR_TYPE_MAT4;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_TRANSFORM_2D = RSE::GLOBAL_VAR_TYPE_TRANSFORM_2D;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_TRANSFORM = RSE::GLOBAL_VAR_TYPE_TRANSFORM;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_SAMPLER2D = RSE::GLOBAL_VAR_TYPE_SAMPLER2D;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_SAMPLER2DARRAY = RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_SAMPLER3D = RSE::GLOBAL_VAR_TYPE_SAMPLER3D;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_SAMPLERCUBE = RSE::GLOBAL_VAR_TYPE_SAMPLERCUBE;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_SAMPLEREXT = RSE::GLOBAL_VAR_TYPE_SAMPLEREXT;
	static constexpr GlobalShaderParameterType GLOBAL_VAR_TYPE_MAX = RSE::GLOBAL_VAR_TYPE_MAX;

	using RenderingInfo = RSE::RenderingInfo;
	static constexpr RenderingInfo RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME = RSE::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME;
	static constexpr RenderingInfo RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME = RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME;
	static constexpr RenderingInfo RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME = RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME;
	static constexpr RenderingInfo RENDERING_INFO_TEXTURE_MEM_USED = RSE::RENDERING_INFO_TEXTURE_MEM_USED;
	static constexpr RenderingInfo RENDERING_INFO_BUFFER_MEM_USED = RSE::RENDERING_INFO_BUFFER_MEM_USED;
	static constexpr RenderingInfo RENDERING_INFO_VIDEO_MEM_USED = RSE::RENDERING_INFO_VIDEO_MEM_USED;
	static constexpr RenderingInfo RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS = RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS;
	static constexpr RenderingInfo RENDERING_INFO_PIPELINE_COMPILATIONS_MESH = RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH;
	static constexpr RenderingInfo RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE = RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE;
	static constexpr RenderingInfo RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW = RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW;
	static constexpr RenderingInfo RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION = RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION;
	static constexpr RenderingInfo RENDERING_INFO_MAX = RSE::RENDERING_INFO_MAX;

	using SplashStretchMode = RSE::SplashStretchMode;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_DISABLED = RSE::SPLASH_STRETCH_MODE_DISABLED;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_KEEP = RSE::SPLASH_STRETCH_MODE_KEEP;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_KEEP_WIDTH = RSE::SPLASH_STRETCH_MODE_KEEP_WIDTH;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_KEEP_HEIGHT = RSE::SPLASH_STRETCH_MODE_KEEP_HEIGHT;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_COVER = RSE::SPLASH_STRETCH_MODE_COVER;
	static constexpr SplashStretchMode SPLASH_STRETCH_MODE_IGNORE = RSE::SPLASH_STRETCH_MODE_IGNORE;

	using Features = RSE::Features;
	static constexpr Features FEATURE_SHADERS = RSE::FEATURE_SHADERS;
	static constexpr Features FEATURE_MULTITHREADED = RSE::FEATURE_MULTITHREADED;

	static RenderingServer *get_singleton();
	static RenderingServer *create();

	/* TEXTURE API */

	virtual RID texture_2d_create(const Ref<Image> &p_image) = 0;
	virtual RID texture_2d_layered_create(const Vector<Ref<Image>> &p_layers, RSE::TextureLayeredType p_layered_type) = 0;
	virtual RID texture_3d_create(Image::Format, int p_width, int p_height, int p_depth, bool p_mipmaps, const Vector<Ref<Image>> &p_data) = 0; //all slices, then all the mipmaps, must be coherent
	virtual RID texture_external_create(int p_width, int p_height, uint64_t p_external_buffer = 0) = 0;
	virtual RID texture_proxy_create(RID p_base) = 0;
	virtual RID texture_drawable_create(int p_width, int p_height, RSE::TextureDrawableFormat p_format, const Color &p_color = Color(1, 1, 1, 1), bool p_with_mipmaps = false) = 0;

	virtual RID texture_create_from_native_handle(RSE::TextureType p_type, Image::Format p_format, uint64_t p_native_handle, int p_width, int p_height, int p_depth, int p_layers = 1, RSE::TextureLayeredType p_layered_type = RSE::TEXTURE_LAYERED_2D_ARRAY) = 0;

	virtual void texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer = 0) = 0;
	virtual void texture_3d_update(RID p_texture, const Vector<Ref<Image>> &p_data) = 0;
	virtual void texture_external_update(RID p_texture, int p_width, int p_height, uint64_t p_external_buffer = 0) = 0;
	virtual void texture_proxy_update(RID p_texture, RID p_proxy_to) = 0;

	virtual void texture_drawable_blit_rect(const TypedArray<RID> &p_textures, const Rect2i &p_rect, RID p_material, const Color &p_modulate, const TypedArray<RID> &p_source_textures, int p_to_mipmap = 0) = 0;

	// These two APIs can be used together or in combination with the others.
	virtual RID texture_2d_placeholder_create() = 0;
	virtual RID texture_2d_layered_placeholder_create(RSE::TextureLayeredType p_layered_type) = 0;
	virtual RID texture_3d_placeholder_create() = 0;

	virtual Ref<Image> texture_2d_get(RID p_texture) const = 0;
	virtual Ref<Image> texture_2d_layer_get(RID p_texture, int p_layer) const = 0;
	virtual Vector<Ref<Image>> texture_3d_get(RID p_texture) const = 0;

	virtual void texture_replace(RID p_texture, RID p_by_texture) = 0;
	virtual void texture_set_size_override(RID p_texture, int p_width, int p_height) = 0;

	virtual void texture_set_path(RID p_texture, const String &p_path) = 0;
	virtual String texture_get_path(RID p_texture) const = 0;

	virtual void texture_drawable_generate_mipmaps(RID p_texture) = 0; // Update mipmaps if modified
	virtual RID texture_drawable_get_default_material() const = 0; // To use with simplified functions in DrawableTexture2D

	virtual Image::Format texture_get_format(RID p_texture) const = 0;

	virtual void texture_set_detect_3d_callback(RID p_texture, RenderingServerTypes::TextureDetectCallback p_callback, void *p_userdata) = 0;
	virtual void texture_set_detect_normal_callback(RID p_texture, RenderingServerTypes::TextureDetectCallback p_callback, void *p_userdata) = 0;
	virtual void texture_set_detect_roughness_callback(RID p_texture, RenderingServerTypes::TextureDetectRoughnessCallback p_callback, void *p_userdata) = 0;

	virtual void texture_debug_usage(List<RenderingServerTypes::TextureInfo> *r_info) = 0;
	Array _texture_debug_usage_bind();

	virtual void texture_set_force_redraw_if_visible(RID p_texture, bool p_enable) = 0;

	virtual RID texture_rd_create(const RID &p_rd_texture, const RSE::TextureLayeredType p_layer_type = RSE::TEXTURE_LAYERED_2D_ARRAY) = 0;
	virtual RID texture_get_rd_texture(RID p_texture, bool p_srgb = false) const = 0;
	virtual uint64_t texture_get_native_handle(RID p_texture, bool p_srgb = false) const = 0;

	/* SHADER API */

	virtual RID shader_create() = 0;
	virtual RID shader_create_from_code(const String &p_code, const String &p_path_hint = String()) = 0;

	virtual void shader_set_code(RID p_shader, const String &p_code) = 0;
	virtual void shader_set_code_rt(RID p_shader, const String &p_code_rt) = 0;
	virtual void shader_set_path_hint(RID p_shader, const String &p_path) = 0;
	virtual String shader_get_code(RID p_shader) const = 0;
	virtual void get_shader_parameter_list(RID p_shader, List<PropertyInfo> *p_param_list) const = 0;
	virtual Variant shader_get_parameter_default(RID p_shader, const StringName &p_param) const = 0;

	virtual void shader_set_default_texture_parameter(RID p_shader, const StringName &p_name, RID p_texture, int p_index = 0) = 0;
	virtual RID shader_get_default_texture_parameter(RID p_shader, const StringName &p_name, int p_index = 0) const = 0;

	virtual RenderingServerTypes::ShaderNativeSourceCode shader_get_native_source_code(RID p_shader) const = 0;

	/* COMMON MATERIAL API */

	virtual RID material_create() = 0;
	virtual RID material_create_from_shader(RID p_next_pass, int p_render_priority, RID p_shader) = 0;

	virtual void material_set_shader(RID p_shader_material, RID p_shader) = 0;

	virtual void material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) = 0;
	virtual Variant material_get_param(RID p_material, const StringName &p_param) const = 0;

	virtual void material_set_render_priority(RID p_material, int priority) = 0;

	virtual void material_set_next_pass(RID p_material, RID p_next_material) = 0;

	virtual void material_set_use_debanding(bool p_enable) = 0;

	/* MESH API */

	virtual RID mesh_create_from_surfaces(const Vector<RenderingServerTypes::SurfaceData> &p_surfaces, int p_blend_shape_count = 0) = 0;
	virtual RID mesh_create() = 0;

	virtual void mesh_set_blend_shape_count(RID p_mesh, int p_blend_shape_count) = 0;

	virtual uint32_t mesh_surface_get_format_offset(BitField<RSE::ArrayFormat> p_format, int p_vertex_len, int p_array_index) const;
	virtual uint32_t mesh_surface_get_format_vertex_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
	virtual uint32_t mesh_surface_get_format_normal_tangent_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
	virtual uint32_t mesh_surface_get_format_attribute_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
	virtual uint32_t mesh_surface_get_format_skin_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
	virtual uint32_t mesh_surface_get_format_index_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;

	/// Returns stride
	virtual void mesh_surface_make_offsets_from_format(uint64_t p_format, int p_vertex_len, int p_index_len, uint32_t *r_offsets, uint32_t &r_vertex_element_size, uint32_t &r_normal_element_size, uint32_t &r_attrib_element_size, uint32_t &r_skin_element_size) const;
	virtual Error mesh_create_surface_data_from_arrays(RenderingServerTypes::SurfaceData *r_surface_data, RSE::PrimitiveType p_primitive, const Array &p_arrays, const Array &p_blend_shapes = Array(), const Dictionary &p_lods = Dictionary(), uint64_t p_compress_format = 0);
	Array mesh_create_arrays_from_surface_data(const RenderingServerTypes::SurfaceData &p_data) const;
	Array mesh_surface_get_arrays(RID p_mesh, int p_surface) const;
	TypedArray<Array> mesh_surface_get_blend_shape_arrays(RID p_mesh, int p_surface) const;
	Dictionary mesh_surface_get_lods(RID p_mesh, int p_surface) const;

	virtual void mesh_add_surface_from_arrays(RID p_mesh, RSE::PrimitiveType p_primitive, const Array &p_arrays, const Array &p_blend_shapes = Array(), const Dictionary &p_lods = Dictionary(), BitField<RSE::ArrayFormat> p_compress_format = 0);
	virtual void mesh_add_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface) = 0;

	virtual int mesh_get_blend_shape_count(RID p_mesh) const = 0;

	virtual void mesh_set_blend_shape_mode(RID p_mesh, RSE::BlendShapeMode p_mode) = 0;
	virtual RSE::BlendShapeMode mesh_get_blend_shape_mode(RID p_mesh) const = 0;

	virtual void mesh_surface_update_vertex_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
	virtual void mesh_surface_update_attribute_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
	virtual void mesh_surface_update_skin_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
	virtual void mesh_surface_update_index_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;

	virtual void mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) = 0;
	virtual RID mesh_surface_get_material(RID p_mesh, int p_surface) const = 0;

	virtual RenderingServerTypes::SurfaceData mesh_get_surface(RID p_mesh, int p_surface) const = 0;

	virtual int mesh_get_surface_count(RID p_mesh) const = 0;

	virtual void mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) = 0;
	virtual AABB mesh_get_custom_aabb(RID p_mesh) const = 0;

	virtual void mesh_set_path(RID p_mesh, const String &p_path) = 0;
	virtual String mesh_get_path(RID p_mesh) const = 0;

	virtual void mesh_set_shadow_mesh(RID p_mesh, RID p_shadow_mesh) = 0;

	virtual void mesh_surface_remove(RID p_mesh, int p_surface) = 0;
	virtual void mesh_clear(RID p_mesh) = 0;

	virtual void mesh_debug_usage(List<RenderingServerTypes::MeshInfo> *r_info) = 0;

	/* MULTIMESH API */

	virtual RID multimesh_create() = 0;

protected:
#ifndef DISABLE_DEPRECATED
	void _multimesh_allocate_data_bind_compat_99455(RID p_multimesh, int p_instances, RSE::MultimeshTransformFormat p_transform_format, bool p_use_colors, bool p_use_custom_data);
#endif
public:
	virtual void multimesh_allocate_data(RID p_multimesh, int p_instances, RSE::MultimeshTransformFormat p_transform_format, bool p_use_colors = false, bool p_use_custom_data = false, bool p_use_indirect = false) = 0;
	virtual int multimesh_get_instance_count(RID p_multimesh) const = 0;

	virtual void multimesh_set_mesh(RID p_multimesh, RID p_mesh) = 0;
	virtual void multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) = 0;
	virtual void multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) = 0;
	virtual void multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) = 0;
	virtual void multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) = 0;

	virtual RID multimesh_get_mesh(RID p_multimesh) const = 0;
	virtual AABB multimesh_get_aabb(RID p_multimesh) const = 0;

	virtual void multimesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) = 0;
	virtual AABB multimesh_get_custom_aabb(RID p_mesh) const = 0;

	virtual Transform3D multimesh_instance_get_transform(RID p_multimesh, int p_index) const = 0;
	virtual Transform2D multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const = 0;
	virtual Color multimesh_instance_get_color(RID p_multimesh, int p_index) const = 0;
	virtual Color multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const = 0;

	virtual void multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) = 0;
	virtual RID multimesh_get_command_buffer_rd_rid(RID p_multimesh) const = 0;
	virtual RID multimesh_get_buffer_rd_rid(RID p_multimesh) const = 0;
	virtual Vector<float> multimesh_get_buffer(RID p_multimesh) const = 0;

	// Interpolation.
	virtual void multimesh_set_buffer_interpolated(RID p_multimesh, const Vector<float> &p_buffer_curr, const Vector<float> &p_buffer_prev) = 0;
	virtual void multimesh_set_physics_interpolated(RID p_multimesh, bool p_interpolated) = 0;
	virtual void multimesh_set_physics_interpolation_quality(RID p_multimesh, RSE::MultimeshPhysicsInterpolationQuality p_quality) = 0;
	virtual void multimesh_instance_reset_physics_interpolation(RID p_multimesh, int p_index) = 0;
	virtual void multimesh_instances_reset_physics_interpolation(RID p_multimesh) = 0;

	virtual void multimesh_set_visible_instances(RID p_multimesh, int p_visible) = 0;
	virtual int multimesh_get_visible_instances(RID p_multimesh) const = 0;

	/* SKELETON API */

	virtual RID skeleton_create() = 0;
	virtual void skeleton_allocate_data(RID p_skeleton, int p_bones, bool p_2d_skeleton = false) = 0;
	virtual int skeleton_get_bone_count(RID p_skeleton) const = 0;
	virtual void skeleton_bone_set_transform(RID p_skeleton, int p_bone, const Transform3D &p_transform) = 0;
	virtual void skeleton_set_bone_data_3d(RID p_skeleton, const Vector<float> &p_bone_data) = 0;
	virtual Transform3D skeleton_bone_get_transform(RID p_skeleton, int p_bone) const = 0;
	virtual void skeleton_bone_set_transform_2d(RID p_skeleton, int p_bone, const Transform2D &p_transform) = 0;
	virtual Transform2D skeleton_bone_get_transform_2d(RID p_skeleton, int p_bone) const = 0;
	virtual void skeleton_set_base_transform_2d(RID p_skeleton, const Transform2D &p_base_transform) = 0;

	/* LIGHT API */

	virtual RID directional_light_create() = 0;
	virtual RID omni_light_create() = 0;
	virtual RID spot_light_create() = 0;
	virtual RID area_light_create() = 0;

	virtual void light_set_color(RID p_light, const Color &p_color) = 0;
	virtual void light_set_param(RID p_light, RSE::LightParam p_param, float p_value) = 0;
	virtual void light_set_shadow(RID p_light, bool p_enabled) = 0;
	virtual void light_set_projector(RID p_light, RID p_texture) = 0;
	virtual void light_set_negative(RID p_light, bool p_enable) = 0;
	virtual void light_set_cull_mask(RID p_light, uint32_t p_mask) = 0;
	virtual void light_set_distance_fade(RID p_light, bool p_enabled, float p_begin, float p_shadow, float p_length) = 0;
	virtual void light_set_reverse_cull_face_mode(RID p_light, bool p_enabled) = 0;
	virtual void light_set_shadow_caster_mask(RID p_light, uint32_t p_caster_mask) = 0;

	virtual void light_set_bake_mode(RID p_light, RSE::LightBakeMode p_bake_mode) = 0;
	virtual void light_set_max_sdfgi_cascade(RID p_light, uint32_t p_cascade) = 0;

	// Omni light

	virtual void light_omni_set_shadow_mode(RID p_light, RSE::LightOmniShadowMode p_mode) = 0;

	// Directional light

	virtual void light_directional_set_shadow_mode(RID p_light, RSE::LightDirectionalShadowMode p_mode) = 0;
	virtual void light_directional_set_blend_splits(RID p_light, bool p_enable) = 0;
	virtual void light_directional_set_sky_mode(RID p_light, RSE::LightDirectionalSkyMode p_mode) = 0;

	virtual void light_area_set_size(RID p_light, const Vector2 &p_size) = 0;
	virtual void light_area_set_normalize_energy(RID p_light, bool p_enabled) = 0;
	virtual void light_area_set_texture(RID p_light, RID texture) = 0;

	// Shadow atlas

	virtual RID shadow_atlas_create() = 0;
	virtual void shadow_atlas_set_size(RID p_atlas, int p_size, bool p_use_16_bits = true) = 0;
	virtual void shadow_atlas_set_quadrant_subdivision(RID p_atlas, int p_quadrant, int p_subdivision) = 0;

	virtual void directional_shadow_atlas_set_size(int p_size, bool p_16_bits = true) = 0;

	virtual void positional_soft_shadow_filter_set_quality(RSE::ShadowQuality p_quality) = 0;
	virtual void directional_soft_shadow_filter_set_quality(RSE::ShadowQuality p_quality) = 0;

	virtual void light_projectors_set_filter(RSE::LightProjectorFilter p_filter) = 0;

	/* REFLECTION PROBE API */

	virtual RID reflection_probe_create() = 0;

	virtual void reflection_probe_set_update_mode(RID p_probe, RSE::ReflectionProbeUpdateMode p_mode) = 0;
	virtual void reflection_probe_set_intensity(RID p_probe, float p_intensity) = 0;
	virtual void reflection_probe_set_blend_distance(RID p_probe, float p_blend_distance) = 0;

	virtual void reflection_probe_set_ambient_mode(RID p_probe, RSE::ReflectionProbeAmbientMode p_mode) = 0;
	virtual void reflection_probe_set_ambient_color(RID p_probe, const Color &p_color) = 0;
	virtual void reflection_probe_set_ambient_energy(RID p_probe, float p_energy) = 0;
	virtual void reflection_probe_set_max_distance(RID p_probe, float p_distance) = 0;
	virtual void reflection_probe_set_size(RID p_probe, const Vector3 &p_size) = 0;
	virtual void reflection_probe_set_origin_offset(RID p_probe, const Vector3 &p_offset) = 0;
	virtual void reflection_probe_set_as_interior(RID p_probe, bool p_enable) = 0;
	virtual void reflection_probe_set_enable_box_projection(RID p_probe, bool p_enable) = 0;
	virtual void reflection_probe_set_enable_shadows(RID p_probe, bool p_enable) = 0;
	virtual void reflection_probe_set_cull_mask(RID p_probe, uint32_t p_layers) = 0;
	virtual void reflection_probe_set_reflection_mask(RID p_probe, uint32_t p_layers) = 0;
	virtual void reflection_probe_set_resolution(RID p_probe, int p_resolution) = 0;
	virtual void reflection_probe_set_mesh_lod_threshold(RID p_probe, float p_pixels) = 0;

	/* DECAL API */

	virtual RID decal_create() = 0;
	virtual void decal_set_size(RID p_decal, const Vector3 &p_size) = 0;
	virtual void decal_set_texture(RID p_decal, RSE::DecalTexture p_type, RID p_texture) = 0;
	virtual void decal_set_emission_energy(RID p_decal, float p_energy) = 0;
	virtual void decal_set_albedo_mix(RID p_decal, float p_mix) = 0;
	virtual void decal_set_modulate(RID p_decal, const Color &p_modulate) = 0;
	virtual void decal_set_cull_mask(RID p_decal, uint32_t p_layers) = 0;
	virtual void decal_set_distance_fade(RID p_decal, bool p_enabled, float p_begin, float p_length) = 0;
	virtual void decal_set_fade(RID p_decal, float p_above, float p_below) = 0;
	virtual void decal_set_normal_fade(RID p_decal, float p_fade) = 0;

	virtual void decals_set_filter(RSE::DecalFilter p_quality) = 0;

	/* VOXEL GI API */

	virtual RID voxel_gi_create() = 0;

	virtual void voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) = 0;

	virtual AABB voxel_gi_get_bounds(RID p_voxel_gi) const = 0;
	virtual Vector3i voxel_gi_get_octree_size(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_octree_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_data_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_distance_field(RID p_voxel_gi) const = 0;
	virtual Vector<int> voxel_gi_get_level_counts(RID p_voxel_gi) const = 0;
	virtual Transform3D voxel_gi_get_to_cell_xform(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) = 0;
	virtual void voxel_gi_set_propagation(RID p_voxel_gi, float p_range) = 0;
	virtual void voxel_gi_set_energy(RID p_voxel_gi, float p_energy) = 0;
	virtual void voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) = 0;
	virtual void voxel_gi_set_bias(RID p_voxel_gi, float p_bias) = 0;
	virtual void voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) = 0;
	virtual void voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) = 0;
	virtual void voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) = 0;

	virtual void voxel_gi_set_quality(RSE::VoxelGIQuality) = 0;

	virtual void sdfgi_reset() = 0;

	/* LIGHTMAP API */

	virtual RID lightmap_create() = 0;

	virtual void lightmap_set_textures(RID p_lightmap, RID p_light, bool p_uses_spherical_haromics) = 0;
	virtual void lightmap_set_probe_bounds(RID p_lightmap, const AABB &p_bounds) = 0;
	virtual void lightmap_set_probe_interior(RID p_lightmap, bool p_interior) = 0;
	virtual void lightmap_set_probe_capture_data(RID p_lightmap, const PackedVector3Array &p_points, const PackedColorArray &p_point_sh, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree) = 0;
	virtual void lightmap_set_baked_exposure_normalization(RID p_lightmap, float p_exposure) = 0;
	virtual PackedVector3Array lightmap_get_probe_capture_points(RID p_lightmap) const = 0;
	virtual PackedColorArray lightmap_get_probe_capture_sh(RID p_lightmap) const = 0;
	virtual PackedInt32Array lightmap_get_probe_capture_tetrahedra(RID p_lightmap) const = 0;
	virtual PackedInt32Array lightmap_get_probe_capture_bsp_tree(RID p_lightmap) const = 0;

	virtual void lightmap_set_probe_capture_update_speed(float p_speed) = 0;
	virtual void lightmaps_set_bicubic_filter(bool p_enable) = 0;

	virtual void lightmap_set_shadowmask_textures(RID p_lightmap, RID p_shadow) = 0;
	virtual RSE::ShadowmaskMode lightmap_get_shadowmask_mode(RID p_lightmap) = 0;
	virtual void lightmap_set_shadowmask_mode(RID p_lightmap, RSE::ShadowmaskMode p_mode) = 0;

	/* PARTICLES API */

	virtual RID particles_create() = 0;
	virtual void particles_set_mode(RID p_particles, RSE::ParticlesMode p_mode) = 0;

	virtual void particles_set_emitting(RID p_particles, bool p_enable) = 0;
	virtual bool particles_get_emitting(RID p_particles) = 0;
	virtual void particles_set_amount(RID p_particles, int p_amount) = 0;
	virtual void particles_set_amount_ratio(RID p_particles, float p_amount_ratio) = 0;
	virtual void particles_set_lifetime(RID p_particles, double p_lifetime) = 0;
	virtual void particles_set_one_shot(RID p_particles, bool p_one_shot) = 0;
	virtual void particles_set_pre_process_time(RID p_particles, double p_time) = 0;
	virtual void particles_request_process_time(RID p_particles, real_t p_request_process_time, real_t p_request_process_time_residual = 0.0) = 0;
	virtual void particles_set_explosiveness_ratio(RID p_particles, float p_ratio) = 0;
	virtual void particles_set_randomness_ratio(RID p_particles, float p_ratio) = 0;
	virtual void particles_set_custom_aabb(RID p_particles, const AABB &p_aabb) = 0;
	virtual void particles_set_speed_scale(RID p_particles, double p_scale) = 0;
	virtual void particles_set_use_local_coordinates(RID p_particles, bool p_enable) = 0;
	virtual void particles_set_process_material(RID p_particles, RID p_material) = 0;
	virtual void particles_set_fixed_fps(RID p_particles, int p_fps) = 0;
	virtual void particles_set_interpolate(RID p_particles, bool p_enable) = 0;
	virtual void particles_set_fractional_delta(RID p_particles, bool p_enable) = 0;
	virtual void particles_set_collision_base_size(RID p_particles, float p_size) = 0;
	virtual void particles_set_seed(RID p_particles, uint32_t p_seed) = 0;

	virtual void particles_set_transform_align(RID p_particles, RSE::ParticlesTransformAlign p_transform_align) = 0;
	virtual void particles_set_transform_align_channel_filter(RID p_particles, RSE::ParticlesTransformAlignCustomSrc p_transform_align_channel_filter) = 0;
	virtual void particles_set_transform_align_axis(RID p_particles, RSE::ParticlesTransformAlignAxis p_rotation_axis) = 0;

	virtual void particles_set_trails(RID p_particles, bool p_enable, float p_length_sec) = 0;
	virtual void particles_set_trail_bind_poses(RID p_particles, const Vector<Transform3D> &p_bind_poses) = 0;

	virtual bool particles_is_inactive(RID p_particles) = 0;
	virtual void particles_request_process(RID p_particles) = 0;
	virtual void particles_restart(RID p_particles) = 0;

	virtual void particles_set_subemitter(RID p_particles, RID p_subemitter_particles) = 0;

	virtual void particles_emit(RID p_particles, const Transform3D &p_transform, const Vector3 &p_velocity, const Color &p_color, const Color &p_custom, uint32_t p_emit_flags) = 0;

	virtual void particles_set_draw_order(RID p_particles, RSE::ParticlesDrawOrder p_order) = 0;

	virtual void particles_set_draw_passes(RID p_particles, int p_count) = 0;
	virtual void particles_set_draw_pass_mesh(RID p_particles, int p_pass, RID p_mesh) = 0;

	virtual AABB particles_get_current_aabb(RID p_particles) = 0;

	virtual void particles_set_emission_transform(RID p_particles, const Transform3D &p_transform) = 0; // This is only used for 2D, in 3D it's automatic.
	virtual void particles_set_emitter_velocity(RID p_particles, const Vector3 &p_velocity) = 0;
	virtual void particles_set_interp_to_end(RID p_particles, float p_interp) = 0;

	/* PARTICLES COLLISION API */

	virtual RID particles_collision_create() = 0;

	virtual void particles_collision_set_collision_type(RID p_particles_collision, RSE::ParticlesCollisionType p_type) = 0;
	virtual void particles_collision_set_cull_mask(RID p_particles_collision, uint32_t p_cull_mask) = 0;
	virtual void particles_collision_set_sphere_radius(RID p_particles_collision, real_t p_radius) = 0; // For spheres.
	virtual void particles_collision_set_box_extents(RID p_particles_collision, const Vector3 &p_extents) = 0; // For non-spheres.
	virtual void particles_collision_set_attractor_strength(RID p_particles_collision, real_t p_strength) = 0;
	virtual void particles_collision_set_attractor_directionality(RID p_particles_collision, real_t p_directionality) = 0;
	virtual void particles_collision_set_attractor_attenuation(RID p_particles_collision, real_t p_curve) = 0;
	virtual void particles_collision_set_field_texture(RID p_particles_collision, RID p_texture) = 0; // For SDF and vector field, heightfield is dynamic.

	virtual void particles_collision_height_field_update(RID p_particles_collision) = 0; // For SDF and vector field.

	virtual void particles_collision_set_height_field_resolution(RID p_particles_collision, RSE::ParticlesCollisionHeightfieldResolution p_resolution) = 0; // For SDF and vector field.
	virtual void particles_collision_set_height_field_mask(RID p_particles_collision, uint32_t p_heightfield_mask) = 0;

	/* FOG VOLUME API */

	virtual RID fog_volume_create() = 0;

	virtual void fog_volume_set_shape(RID p_fog_volume, RSE::FogVolumeShape p_shape) = 0;
	virtual void fog_volume_set_size(RID p_fog_volume, const Vector3 &p_size) = 0;
	virtual void fog_volume_set_material(RID p_fog_volume, RID p_material) = 0;

	/* VISIBILITY NOTIFIER API */

	virtual RID visibility_notifier_create() = 0;
	virtual void visibility_notifier_set_aabb(RID p_notifier, const AABB &p_aabb) = 0;
	virtual void visibility_notifier_set_callbacks(RID p_notifier, const Callable &p_enter_callbable, const Callable &p_exit_callable) = 0;

	/* OCCLUDER API */

	virtual RID occluder_create() = 0;
	virtual void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) = 0;

	/* CAMERA API */

	virtual RID camera_create() = 0;
	virtual void camera_set_perspective(RID p_camera, float p_fovy_degrees, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_orthogonal(RID p_camera, float p_size, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_frustum(RID p_camera, float p_size, Vector2 p_offset, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_transform(RID p_camera, const Transform3D &p_transform) = 0;
	virtual void camera_set_cull_mask(RID p_camera, uint32_t p_layers) = 0;
	virtual void camera_set_environment(RID p_camera, RID p_env) = 0;
	virtual void camera_set_camera_attributes(RID p_camera, RID p_camera_attributes) = 0;
	virtual void camera_set_compositor(RID p_camera, RID p_compositor) = 0;
	virtual void camera_set_use_vertical_aspect(RID p_camera, bool p_enable) = 0;

	/* VIEWPORT API */

	virtual RID viewport_create() = 0;

#ifndef XR_DISABLED
	virtual void viewport_set_use_xr(RID p_viewport, bool p_use_xr) = 0;
#endif // !XR_DISABLED

	virtual void viewport_set_size(RID p_viewport, int p_width, int p_height, int p_view_count = 1) = 0;
	virtual void viewport_set_active(RID p_viewport, bool p_active) = 0;
	virtual void viewport_set_parent_viewport(RID p_viewport, RID p_parent_viewport) = 0;
	virtual void viewport_set_canvas_cull_mask(RID p_viewport, uint32_t p_canvas_cull_mask) = 0;

	virtual void viewport_attach_to_screen(RID p_viewport, const Rect2 &p_rect = Rect2(), DisplayServerEnums::WindowID p_screen = DisplayServerEnums::MAIN_WINDOW_ID) = 0;
	virtual void viewport_set_render_direct_to_screen(RID p_viewport, bool p_enable) = 0;

	virtual void viewport_set_scaling_3d_mode(RID p_viewport, RSE::ViewportScaling3DMode p_scaling_3d_mode) = 0;
	virtual void viewport_set_scaling_3d_scale(RID p_viewport, float p_scaling_3d_scale) = 0;
	virtual void viewport_set_fsr_sharpness(RID p_viewport, float p_fsr_sharpness) = 0;
	virtual void viewport_set_texture_mipmap_bias(RID p_viewport, float p_texture_mipmap_bias) = 0;
	virtual void viewport_set_anisotropic_filtering_level(RID p_viewport, RSE::ViewportAnisotropicFiltering p_anisotropic_filtering_level) = 0;

	virtual void viewport_set_frame_generation_mode(RID p_viewport, RSE::ViewportFrameGenerationMode p_mode) = 0;
	virtual void viewport_set_frame_generation_warp_scale(RID p_viewport, float p_warp_scale) = 0;
	virtual void viewport_set_frame_generation_target_fps(RID p_viewport, int p_target_fps) = 0;

	virtual bool viewport_is_frame_generation_active(RID p_viewport) const = 0;
	virtual float viewport_get_frame_generation_real_fps(RID p_viewport) const = 0;
	virtual float viewport_get_frame_generation_output_fps(RID p_viewport) const = 0;
	virtual float viewport_get_frame_generation_latency(RID p_viewport) const = 0;

	virtual void viewport_set_update_mode(RID p_viewport, RSE::ViewportUpdateMode p_mode) = 0;
	virtual RSE::ViewportUpdateMode viewport_get_update_mode(RID p_viewport) const = 0;

	virtual void viewport_set_clear_mode(RID p_viewport, RSE::ViewportClearMode p_clear_mode) = 0;

	virtual RID viewport_get_render_target(RID p_viewport) const = 0;
	virtual RID viewport_get_texture(RID p_viewport) const = 0;

	virtual void viewport_set_environment_mode(RID p_viewport, RSE::ViewportEnvironmentMode p_mode) = 0;
	virtual void viewport_set_disable_3d(RID p_viewport, bool p_disable) = 0;
	virtual void viewport_set_disable_2d(RID p_viewport, bool p_disable) = 0;

	virtual void viewport_attach_camera(RID p_viewport, RID p_camera) = 0;
	virtual void viewport_set_scenario(RID p_viewport, RID p_scenario) = 0;
	virtual void viewport_attach_canvas(RID p_viewport, RID p_canvas) = 0;
	virtual void viewport_remove_canvas(RID p_viewport, RID p_canvas) = 0;
	virtual void viewport_set_canvas_transform(RID p_viewport, RID p_canvas, const Transform2D &p_offset) = 0;
	virtual void viewport_set_transparent_background(RID p_viewport, bool p_enabled) = 0;
	virtual void viewport_set_use_hdr_2d(RID p_viewport, bool p_use_hdr) = 0;
	virtual bool viewport_is_using_hdr_2d(RID p_viewport) const = 0;
	virtual void viewport_set_snap_2d_transforms_to_pixel(RID p_viewport, bool p_enabled) = 0;
	virtual void viewport_set_snap_2d_vertices_to_pixel(RID p_viewport, bool p_enabled) = 0;

	virtual void viewport_set_default_canvas_item_texture_filter(RID p_viewport, RSE::CanvasItemTextureFilter p_filter) = 0;
	virtual void viewport_set_default_canvas_item_texture_repeat(RID p_viewport, RSE::CanvasItemTextureRepeat p_repeat) = 0;

	virtual void viewport_set_global_canvas_transform(RID p_viewport, const Transform2D &p_transform) = 0;
	virtual void viewport_set_canvas_stacking(RID p_viewport, RID p_canvas, int p_layer, int p_sublayer) = 0;

	virtual void viewport_set_sdf_oversize_and_scale(RID p_viewport, RSE::ViewportSDFOversize p_oversize, RSE::ViewportSDFScale p_scale) = 0;

	virtual void viewport_set_positional_shadow_atlas_size(RID p_viewport, int p_size, bool p_16_bits = true) = 0;
	virtual void viewport_set_positional_shadow_atlas_quadrant_subdivision(RID p_viewport, int p_quadrant, int p_subdiv) = 0;

	virtual void viewport_set_msaa_3d(RID p_viewport, RSE::ViewportMSAA p_msaa) = 0;
	virtual void viewport_set_msaa_2d(RID p_viewport, RSE::ViewportMSAA p_msaa) = 0;

	virtual void viewport_set_screen_space_aa(RID p_viewport, RSE::ViewportScreenSpaceAA p_mode) = 0;

	virtual void viewport_set_use_taa(RID p_viewport, bool p_use_taa) = 0;
	virtual void viewport_set_taa_sharpness(RID p_viewport, float p_sharpness) = 0;
	virtual void viewport_set_taa_history_weight(RID p_viewport, float p_history_weight) = 0;
	virtual void viewport_set_taa_disocclusion_threshold(RID p_viewport, float p_threshold) = 0;
	virtual void viewport_set_taa_jitter_phase_count(RID p_viewport, int p_jitter_phase_count) = 0;
	virtual void viewport_set_taa_jitter_scale(RID p_viewport, float p_jitter_scale) = 0;

	virtual void viewport_set_use_debanding(RID p_viewport, bool p_use_debanding) = 0;

	virtual void viewport_set_force_motion_vectors(RID p_viewport, bool p_force_motion_vectors) = 0;

	virtual void viewport_set_mesh_lod_threshold(RID p_viewport, float p_pixels) = 0;

	virtual void viewport_set_use_occlusion_culling(RID p_viewport, bool p_use_occlusion_culling) = 0;
	virtual void viewport_set_occlusion_rays_per_thread(int p_rays_per_thread) = 0;

	virtual void viewport_set_occlusion_culling_build_quality(RSE::ViewportOcclusionCullingBuildQuality p_quality) = 0;

	virtual int viewport_get_render_info(RID p_viewport, RSE::ViewportRenderInfoType p_type, RSE::ViewportRenderInfo p_info) = 0;

	virtual void viewport_set_debug_draw(RID p_viewport, RSE::ViewportDebugDraw p_draw) = 0;

	virtual void viewport_set_measure_render_time(RID p_viewport, bool p_enable) = 0;
	virtual double viewport_get_measured_render_time_cpu(RID p_viewport) const = 0;
	virtual double viewport_get_measured_render_time_gpu(RID p_viewport) const = 0;

	virtual RID viewport_find_from_screen_attachment(DisplayServerEnums::WindowID p_id = DisplayServerEnums::MAIN_WINDOW_ID) const = 0;

	virtual void viewport_set_vrs_mode(RID p_viewport, RSE::ViewportVRSMode p_mode) = 0;
	virtual void viewport_set_vrs_update_mode(RID p_viewport, RSE::ViewportVRSUpdateMode p_mode) = 0;
	virtual void viewport_set_vrs_texture(RID p_viewport, RID p_texture) = 0;

	/* SKY API */

	virtual RID sky_create() = 0;
	virtual void sky_set_radiance_size(RID p_sky, int p_radiance_size) = 0;
	virtual void sky_set_mode(RID p_sky, RSE::SkyMode p_mode) = 0;
	virtual void sky_set_material(RID p_sky, RID p_material) = 0;
	virtual Ref<Image> sky_bake_panorama(RID p_sky, float p_energy, bool p_bake_irradiance, const Size2i &p_size) = 0;

	/* COMPOSITOR EFFECTS API */

	virtual RID compositor_effect_create() = 0;
	virtual void compositor_effect_set_enabled(RID p_effect, bool p_enabled) = 0;
	virtual void compositor_effect_set_callback(RID p_effect, RSE::CompositorEffectCallbackType p_callback_type, const Callable &p_callback) = 0;
	virtual void compositor_effect_set_flag(RID p_effect, RSE::CompositorEffectFlags p_flag, bool p_set) = 0;

	/* COMPOSITOR API */

	virtual RID compositor_create() = 0;

	virtual void compositor_set_compositor_effects(RID p_compositor, const TypedArray<RID> &p_effects) = 0;

	/* ENVIRONMENT API */

	virtual RID environment_create() = 0;

	virtual void environment_set_background(RID p_env, RSE::EnvironmentBG p_bg) = 0;
	virtual void environment_set_sky(RID p_env, RID p_sky) = 0;
	virtual void environment_set_sky_custom_fov(RID p_env, float p_scale) = 0;
	virtual void environment_set_sky_orientation(RID p_env, const Basis &p_orientation) = 0;
	virtual void environment_set_bg_color(RID p_env, const Color &p_color) = 0;
	virtual void environment_set_bg_energy(RID p_env, float p_multiplier, float p_exposure_value) = 0;
	virtual void environment_set_canvas_max_layer(RID p_env, int p_max_layer) = 0;
	virtual void environment_set_ambient_light(RID p_env, const Color &p_color, RSE::EnvironmentAmbientSource p_ambient = RSE::ENV_AMBIENT_SOURCE_BG, float p_energy = 1.0, float p_sky_contribution = 0.0, RSE::EnvironmentReflectionSource p_reflection_source = RSE::ENV_REFLECTION_SOURCE_BG) = 0;
	virtual void environment_set_camera_feed_id(RID p_env, int p_camera_feed_id) = 0;

	virtual void environment_set_glow(RID p_env, bool p_enable, Vector<float> p_levels, float p_intensity, float p_strength, float p_mix, float p_bloom_threshold, RSE::EnvironmentGlowBlendMode p_blend_mode, float p_hdr_bleed_threshold, float p_hdr_bleed_scale, float p_hdr_luminance_cap, float p_glow_map_strength, RID p_glow_map) = 0;

	virtual void environment_glow_set_use_bicubic_upscale(bool p_enable) = 0;

	virtual void environment_set_tonemap(RID p_env, RSE::EnvironmentToneMapper p_tone_mapper, float p_exposure, float p_white) = 0;
	virtual void environment_set_tonemap_agx_contrast(RID p_env, float p_agx_contrast) = 0;
	virtual void environment_set_adjustment(RID p_env, bool p_enable, float p_brightness, float p_contrast, float p_saturation, bool p_use_1d_color_correction, RID p_color_correction) = 0;

	virtual void environment_set_ssr(RID p_env, bool p_enable, int p_max_steps, float p_fade_in, float p_fade_out, float p_depth_tolerance) = 0;

	virtual void environment_set_ssr_half_size(bool p_half_size) = 0;

	virtual void environment_set_ssr_roughness_quality(RSE::EnvironmentSSRRoughnessQuality p_quality) = 0;

	virtual void environment_set_ssao(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_power, float p_detail, float p_horizon, float p_sharpness, float p_light_affect, float p_ao_channel_affect) = 0;

	virtual void environment_set_ssao_quality(RSE::EnvironmentSSAOQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) = 0;

	virtual void environment_set_ssil(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_sharpness, float p_normal_rejection) = 0;

	virtual void environment_set_ssil_quality(RSE::EnvironmentSSILQuality p_quality, bool p_half_size, float p_adaptive_target, int p_blur_passes, float p_fadeout_from, float p_fadeout_to) = 0;

	virtual void environment_set_sdfgi(RID p_env, bool p_enable, int p_cascades, float p_min_cell_size, RSE::EnvironmentSDFGIYScale p_y_scale, bool p_use_occlusion, float p_bounce_feedback, bool p_read_sky, float p_energy, float p_normal_bias, float p_probe_bias) = 0;

	virtual void environment_set_sdfgi_ray_count(RSE::EnvironmentSDFGIRayCount p_ray_count) = 0;

	virtual void environment_set_sdfgi_frames_to_converge(RSE::EnvironmentSDFGIFramesToConverge p_frames) = 0;

	virtual void environment_set_sdfgi_frames_to_update_light(RSE::EnvironmentSDFGIFramesToUpdateLight p_update) = 0;

	// Pathtracing
	virtual void environment_set_pathtracing(RID p_env, bool p_enable) = 0;
	virtual void environment_set_pathtracing_params(RID p_env, const RSE::PathtracingParams &p_params) = 0;
	virtual RSE::PathtracingParams environment_get_pathtracing_params(RID p_env) const = 0;
	virtual Dictionary pathtracing_get_backend_status() const = 0;
	virtual Dictionary pathtracing_get_backend_status_for_backend(RSE::PathtracingBackend p_backend) const = 0;
	virtual Array pathtracing_get_backend_capabilities() const = 0;

	virtual void environment_set_fog(RID p_env, bool p_enable, const Color &p_light_color, float p_light_energy, float p_sun_scatter, float p_density, float p_height, float p_height_density, float p_aerial_perspective, float p_sky_affect, RSE::EnvironmentFogMode p_mode = RSE::EnvironmentFogMode::ENV_FOG_MODE_EXPONENTIAL) = 0;
	virtual void environment_set_fog_depth(RID p_env, float p_curve, float p_begin, float p_end) = 0;

	virtual void environment_set_volumetric_fog(RID p_env, bool p_enable, float p_density, const Color &p_albedo, const Color &p_emission, float p_emission_energy, float p_anisotropy, float p_length, float p_detail_spread, float p_gi_inject, bool p_temporal_reprojection, float p_temporal_reprojection_amount, float p_ambient_inject, float p_sky_affect) = 0;
	virtual void environment_set_volumetric_fog_volume_size(int p_size, int p_depth) = 0;
	virtual void environment_set_volumetric_fog_filter_active(bool p_enable) = 0;

	virtual Ref<Image> environment_bake_panorama(RID p_env, bool p_bake_irradiance, const Size2i &p_size) = 0;

	virtual void screen_space_roughness_limiter_set_active(bool p_enable, float p_amount, float p_limit) = 0;

	virtual void sub_surface_scattering_set_quality(RSE::SubSurfaceScatteringQuality p_quality) = 0;
	virtual void sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) = 0;

	/* CAMERA ATTRIBUTES API */

	virtual RID camera_attributes_create() = 0;

	virtual void camera_attributes_set_dof_blur_quality(RSE::DOFBlurQuality p_quality, bool p_use_jitter) = 0;

	virtual void camera_attributes_set_dof_blur_bokeh_shape(RSE::DOFBokehShape p_shape) = 0;

	virtual void camera_attributes_set_dof_blur(RID p_camera_attributes, bool p_far_enable, float p_far_distance, float p_far_transition, bool p_near_enable, float p_near_distance, float p_near_transition, float p_amount) = 0;
	virtual void camera_attributes_set_exposure(RID p_camera_attributes, float p_multiplier, float p_exposure_normalization) = 0;
	virtual void camera_attributes_set_auto_exposure(RID p_camera_attributes, bool p_enable, float p_min_sensitivity, float p_max_sensitivity, float p_speed, float p_scale) = 0;

	/* SCENARIO API */

	virtual RID scenario_create() = 0;

	virtual void scenario_set_environment(RID p_scenario, RID p_environment) = 0;
	virtual void scenario_set_fallback_environment(RID p_scenario, RID p_environment) = 0;
	virtual void scenario_set_camera_attributes(RID p_scenario, RID p_camera_attributes) = 0;
	virtual void scenario_set_compositor(RID p_scenario, RID p_compositor) = 0;

	/* INSTANCING API */

	virtual RID instance_create2(RID p_base, RID p_scenario);

	virtual RID instance_create() = 0;

	virtual void instance_set_base(RID p_instance, RID p_base) = 0;
	virtual void instance_set_scenario(RID p_instance, RID p_scenario) = 0;
	virtual void instance_set_layer_mask(RID p_instance, uint32_t p_mask) = 0;
	virtual void instance_set_pivot_data(RID p_instance, float p_sorting_offset, bool p_use_aabb_center) = 0;
	virtual void instance_set_transform(RID p_instance, const Transform3D &p_transform) = 0;
	virtual void instance_attach_object_instance_id(RID p_instance, ObjectID p_id) = 0;
	virtual void instance_set_blend_shape_weight(RID p_instance, int p_shape, float p_weight) = 0;
	virtual void instance_set_surface_override_material(RID p_instance, int p_surface, RID p_material) = 0;
	virtual void instance_set_visible(RID p_instance, bool p_visible) = 0;

	virtual void instance_teleport(RID p_instance) = 0;

	virtual void instance_set_custom_aabb(RID p_instance, AABB aabb) = 0;

	virtual void instance_set_rt_procedural(RID p_instance, bool p_procedural, AABB p_aabb) = 0;
	virtual void instance_set_rt_procedural_bounds(RID p_instance, const PackedFloat32Array &p_aabb_data, bool p_expose_bounds) = 0;

	virtual void instance_attach_skeleton(RID p_instance, RID p_skeleton) = 0;

	virtual void instance_set_extra_visibility_margin(RID p_instance, real_t p_margin) = 0;
	virtual void instance_set_visibility_parent(RID p_instance, RID p_parent_instance) = 0;

	virtual void instance_set_ignore_culling(RID p_instance, bool p_enabled) = 0;

	// Don't use these in a game!
	virtual Vector<ObjectID> instances_cull_aabb(const AABB &p_aabb, RID p_scenario = RID()) const = 0;
	virtual Vector<ObjectID> instances_cull_ray(const Vector3 &p_from, const Vector3 &p_to, RID p_scenario = RID()) const = 0;
	virtual Vector<ObjectID> instances_cull_convex(const Vector<Plane> &p_convex, RID p_scenario = RID()) const = 0;

	PackedInt64Array _instances_cull_aabb_bind(const AABB &p_aabb, RID p_scenario = RID()) const;
	PackedInt64Array _instances_cull_ray_bind(const Vector3 &p_from, const Vector3 &p_to, RID p_scenario = RID()) const;
	PackedInt64Array _instances_cull_convex_bind(const TypedArray<Plane> &p_convex, RID p_scenario = RID()) const;

	virtual void instance_geometry_set_flag(RID p_instance, RSE::InstanceFlags p_flags, bool p_enabled) = 0;
	virtual void instance_geometry_set_cast_shadows_setting(RID p_instance, RSE::ShadowCastingSetting p_shadow_casting_setting) = 0;
	virtual void instance_geometry_set_material_override(RID p_instance, RID p_material) = 0;
	virtual void instance_geometry_set_material_overlay(RID p_instance, RID p_material) = 0;
	virtual void instance_geometry_set_visibility_range(RID p_instance, float p_min, float p_max, float p_min_margin, float p_max_margin, RSE::VisibilityRangeFadeMode p_fade_mode) = 0;
	virtual void instance_geometry_set_lightmap(RID p_instance, RID p_lightmap, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice) = 0;
	virtual void instance_geometry_set_lod_bias(RID p_instance, float p_lod_bias) = 0;
	virtual void instance_geometry_set_transparency(RID p_instance, float p_transparency) = 0;

	virtual void instance_geometry_set_shader_parameter(RID p_instance, const StringName &, const Variant &p_value) = 0;
	virtual Variant instance_geometry_get_shader_parameter(RID p_instance, const StringName &) const = 0;
	virtual Variant instance_geometry_get_shader_parameter_default_value(RID p_instance, const StringName &) const = 0;
	virtual void instance_geometry_get_shader_parameter_list(RID p_instance, List<PropertyInfo> *p_parameters) const = 0;

	/* BAKE API */

	virtual TypedArray<Image> bake_render_uv2(RID p_base, const TypedArray<RID> &p_material_overrides, const Size2i &p_image_size) = 0;
	virtual PackedByteArray bake_render_area_light_atlas(const TypedArray<RID> &p_area_light_textures, const TypedArray<Rect2> &p_area_light_atlas_texture_rects, const Size2i &p_size, int p_mipmaps) = 0;

	/* CANVAS API (2D) */

	virtual RID canvas_create() = 0;
	virtual void canvas_set_item_mirroring(RID p_canvas, RID p_item, const Point2 &p_mirroring) = 0;
	virtual void canvas_set_item_repeat(RID p_item, const Point2 &p_repeat_size, int p_repeat_times) = 0;
	virtual void canvas_set_modulate(RID p_canvas, const Color &p_color) = 0;
	virtual void canvas_set_parent(RID p_canvas, RID p_parent, float p_scale) = 0;

	virtual void canvas_set_disable_scale(bool p_disable) = 0;

	/* CANVAS TEXTURE API*/

	virtual RID canvas_texture_create() = 0;
	virtual void canvas_texture_set_channel(RID p_canvas_texture, RSE::CanvasTextureChannel p_channel, RID p_texture) = 0;
	virtual void canvas_texture_set_shading_parameters(RID p_canvas_texture, const Color &p_base_color, float p_shininess) = 0;

	// Takes effect only for new draw commands.
	virtual void canvas_texture_set_texture_filter(RID p_canvas_texture, RSE::CanvasItemTextureFilter p_filter) = 0;
	virtual void canvas_texture_set_texture_repeat(RID p_canvas_texture, RSE::CanvasItemTextureRepeat p_repeat) = 0;

	/* CANVAS ITEM API */

	virtual RID canvas_item_create() = 0;
	virtual void canvas_item_set_parent(RID p_item, RID p_parent) = 0;

	virtual void canvas_item_set_default_texture_filter(RID p_item, RSE::CanvasItemTextureFilter p_filter) = 0;
	virtual void canvas_item_set_default_texture_repeat(RID p_item, RSE::CanvasItemTextureRepeat p_repeat) = 0;

	virtual void canvas_item_set_visible(RID p_item, bool p_visible) = 0;
	virtual void canvas_item_set_light_mask(RID p_item, int p_mask) = 0;

	virtual void canvas_item_set_update_when_visible(RID p_item, bool p_update) = 0;

	virtual void canvas_item_set_transform(RID p_item, const Transform2D &p_transform) = 0;
	virtual void canvas_item_set_clip(RID p_item, bool p_clip) = 0;
	virtual void canvas_item_set_distance_field_mode(RID p_item, bool p_enable) = 0;
	virtual void canvas_item_set_custom_rect(RID p_item, bool p_custom_rect, const Rect2 &p_rect = Rect2()) = 0;
	virtual void canvas_item_set_modulate(RID p_item, const Color &p_color) = 0;
	virtual void canvas_item_set_self_modulate(RID p_item, const Color &p_color) = 0;
	virtual void canvas_item_set_visibility_layer(RID p_item, uint32_t p_visibility_layer) = 0;

	virtual void canvas_item_set_draw_behind_parent(RID p_item, bool p_enable) = 0;
	virtual void canvas_item_set_use_identity_transform(RID p_item, bool p_enabled) = 0;

	virtual void canvas_item_add_line(RID p_item, const Point2 &p_from, const Point2 &p_to, const Color &p_color, float p_width = -1.0, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_polyline(RID p_item, const Vector<Point2> &p_points, const Vector<Color> &p_colors, float p_width = -1.0, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_multiline(RID p_item, const Vector<Point2> &p_points, const Vector<Color> &p_colors, float p_width = -1.0, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_rect(RID p_item, const Rect2 &p_rect, const Color &p_color, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_ellipse(RID p_item, const Point2 &p_pos, float p_major, float p_minor, const Color &p_color, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_circle(RID p_item, const Point2 &p_pos, float p_radius, const Color &p_color, bool p_antialiased = false) = 0;
	virtual void canvas_item_add_texture_rect(RID p_item, const Rect2 &p_rect, RID p_texture, bool p_tile = false, const Color &p_modulate = Color(1, 1, 1), bool p_transpose = false) = 0;
	virtual void canvas_item_add_texture_rect_region(RID p_item, const Rect2 &p_rect, RID p_texture, const Rect2 &p_src_rect, const Color &p_modulate = Color(1, 1, 1), bool p_transpose = false, bool p_clip_uv = false) = 0;
	virtual void canvas_item_add_msdf_texture_rect_region(RID p_item, const Rect2 &p_rect, RID p_texture, const Rect2 &p_src_rect, const Color &p_modulate = Color(1, 1, 1), int p_outline_size = 0, float p_px_range = 1.0, float p_scale = 1.0) = 0;
	virtual void canvas_item_add_lcd_texture_rect_region(RID p_item, const Rect2 &p_rect, RID p_texture, const Rect2 &p_src_rect, const Color &p_modulate = Color(1, 1, 1)) = 0;
	virtual void canvas_item_add_nine_patch(RID p_item, const Rect2 &p_rect, const Rect2 &p_source, RID p_texture, const Vector2 &p_topleft, const Vector2 &p_bottomright, RSE::NinePatchAxisMode p_x_axis_mode = RSE::NINE_PATCH_STRETCH, RSE::NinePatchAxisMode p_y_axis_mode = RSE::NINE_PATCH_STRETCH, bool p_draw_center = true, const Color &p_modulate = Color(1, 1, 1)) = 0;
	virtual void canvas_item_add_primitive(RID p_item, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs, RID p_texture) = 0;
	virtual void canvas_item_add_polygon(RID p_item, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs = Vector<Point2>(), RID p_texture = RID()) = 0;
	virtual void canvas_item_add_triangle_array(RID p_item, const Vector<int> &p_indices, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs = Vector<Point2>(), const Vector<int> &p_bones = Vector<int>(), const Vector<float> &p_weights = Vector<float>(), RID p_texture = RID(), int p_count = -1) = 0;
	virtual void canvas_item_add_mesh(RID p_item, const RID &p_mesh, const Transform2D &p_transform = Transform2D(), const Color &p_modulate = Color(1, 1, 1), RID p_texture = RID()) = 0;
	virtual void canvas_item_add_multimesh(RID p_item, RID p_mesh, RID p_texture = RID()) = 0;
	virtual void canvas_item_add_particles(RID p_item, RID p_particles, RID p_texture) = 0;
	virtual void canvas_item_add_set_transform(RID p_item, const Transform2D &p_transform) = 0;
	virtual void canvas_item_add_clip_ignore(RID p_item, bool p_ignore) = 0;
	virtual void canvas_item_add_animation_slice(RID p_item, double p_animation_length, double p_slice_begin, double p_slice_end, double p_offset) = 0;

	virtual void canvas_item_set_sort_children_by_y(RID p_item, bool p_enable) = 0;
	virtual void canvas_item_set_z_index(RID p_item, int p_z) = 0;
	virtual void canvas_item_set_z_as_relative_to_parent(RID p_item, bool p_enable) = 0;
	virtual void canvas_item_set_copy_to_backbuffer(RID p_item, bool p_enable, const Rect2 &p_rect) = 0;

	virtual void canvas_item_attach_skeleton(RID p_item, RID p_skeleton) = 0;

	virtual void canvas_item_clear(RID p_item) = 0;
	virtual void canvas_item_set_draw_index(RID p_item, int p_index) = 0;

	virtual void canvas_item_set_material(RID p_item, RID p_material) = 0;

	virtual void canvas_item_set_use_parent_material(RID p_item, bool p_enable) = 0;

	virtual void canvas_item_set_instance_shader_parameter(RID p_item, const StringName &, const Variant &p_value) = 0;
	virtual Variant canvas_item_get_instance_shader_parameter(RID p_item, const StringName &) const = 0;
	virtual Variant canvas_item_get_instance_shader_parameter_default_value(RID p_item, const StringName &) const = 0;
	virtual void canvas_item_get_instance_shader_parameter_list(RID p_item, List<PropertyInfo> *p_parameters) const = 0;

	virtual void canvas_item_set_visibility_notifier(RID p_item, bool p_enable, const Rect2 &p_area, const Callable &p_enter_callbable, const Callable &p_exit_callable) = 0;

	virtual void canvas_item_set_canvas_group_mode(RID p_item, RSE::CanvasGroupMode p_mode, float p_clear_margin = 5.0, bool p_fit_empty = false, float p_fit_margin = 0.0, bool p_blur_mipmaps = false) = 0;

	virtual void canvas_item_set_debug_redraw(bool p_enabled) = 0;
	virtual bool canvas_item_get_debug_redraw() const = 0;

	virtual void canvas_item_set_interpolated(RID p_item, bool p_interpolated) = 0;
	virtual void canvas_item_reset_physics_interpolation(RID p_item) = 0;
	virtual void canvas_item_transform_physics_interpolation(RID p_item, const Transform2D &p_transform) = 0;

	/* CANVAS LIGHT */

	virtual RID canvas_light_create() = 0;

	virtual void canvas_light_set_mode(RID p_light, RSE::CanvasLightMode p_mode) = 0;

	virtual void canvas_light_attach_to_canvas(RID p_light, RID p_canvas) = 0;
	virtual void canvas_light_set_enabled(RID p_light, bool p_enabled) = 0;
	virtual void canvas_light_set_transform(RID p_light, const Transform2D &p_transform) = 0;
	virtual void canvas_light_set_color(RID p_light, const Color &p_color) = 0;
	virtual void canvas_light_set_height(RID p_light, float p_height) = 0;
	virtual void canvas_light_set_energy(RID p_light, float p_energy) = 0;
	virtual void canvas_light_set_z_range(RID p_light, int p_min_z, int p_max_z) = 0;
	virtual void canvas_light_set_layer_range(RID p_light, int p_min_layer, int p_max_layer) = 0;
	virtual void canvas_light_set_item_cull_mask(RID p_light, int p_mask) = 0;
	virtual void canvas_light_set_item_shadow_cull_mask(RID p_light, int p_mask) = 0;

	virtual void canvas_light_set_directional_distance(RID p_light, float p_distance) = 0;

	virtual void canvas_light_set_texture_scale(RID p_light, float p_scale) = 0;
	virtual void canvas_light_set_texture(RID p_light, RID p_texture) = 0;
	virtual void canvas_light_set_texture_offset(RID p_light, const Vector2 &p_offset) = 0;

	virtual void canvas_light_set_blend_mode(RID p_light, RSE::CanvasLightBlendMode p_mode) = 0;

	virtual void canvas_light_set_shadow_enabled(RID p_light, bool p_enabled) = 0;
	virtual void canvas_light_set_shadow_filter(RID p_light, RSE::CanvasLightShadowFilter p_filter) = 0;
	virtual void canvas_light_set_shadow_color(RID p_light, const Color &p_color) = 0;
	virtual void canvas_light_set_shadow_smooth(RID p_light, float p_smooth) = 0;

	virtual void canvas_light_set_interpolated(RID p_light, bool p_interpolated) = 0;
	virtual void canvas_light_reset_physics_interpolation(RID p_light) = 0;
	virtual void canvas_light_transform_physics_interpolation(RID p_light, const Transform2D &p_transform) = 0;

	/* CANVAS LIGHT OCCLUDER API */

	virtual RID canvas_light_occluder_create() = 0;
	virtual void canvas_light_occluder_attach_to_canvas(RID p_occluder, RID p_canvas) = 0;
	virtual void canvas_light_occluder_set_enabled(RID p_occluder, bool p_enabled) = 0;
	virtual void canvas_light_occluder_set_polygon(RID p_occluder, RID p_polygon) = 0;
	virtual void canvas_light_occluder_set_as_sdf_collision(RID p_occluder, bool p_enable) = 0;
	virtual void canvas_light_occluder_set_transform(RID p_occluder, const Transform2D &p_xform) = 0;
	virtual void canvas_light_occluder_set_light_mask(RID p_occluder, int p_mask) = 0;

	virtual void canvas_light_occluder_set_interpolated(RID p_occluder, bool p_interpolated) = 0;
	virtual void canvas_light_occluder_reset_physics_interpolation(RID p_occluder) = 0;
	virtual void canvas_light_occluder_transform_physics_interpolation(RID p_occluder, const Transform2D &p_transform) = 0;

	/* CANVAS OCCLUDER POLYGON API */

	virtual RID canvas_occluder_polygon_create() = 0;
	virtual void canvas_occluder_polygon_set_shape(RID p_occluder_polygon, const Vector<Vector2> &p_shape, bool p_closed) = 0;

	virtual void canvas_occluder_polygon_set_cull_mode(RID p_occluder_polygon, RSE::CanvasOccluderPolygonCullMode p_mode) = 0;

	virtual void canvas_set_shadow_texture_size(int p_size) = 0;

	Rect2 debug_canvas_item_get_rect(RID p_item);
	virtual Rect2 _debug_canvas_item_get_rect(RID p_item) = 0;

	/* GLOBAL SHADER PARAMETERS API */

	virtual void global_shader_parameter_add(const StringName &p_name, RSE::GlobalShaderParameterType p_type, const Variant &p_value) = 0;
	virtual void global_shader_parameter_remove(const StringName &p_name) = 0;
	virtual Vector<StringName> global_shader_parameter_get_list() const = 0;

	virtual void global_shader_parameter_set(const StringName &p_name, const Variant &p_value) = 0;
	virtual void global_shader_parameter_set_override(const StringName &p_name, const Variant &p_value) = 0;

	virtual Variant global_shader_parameter_get(const StringName &p_name) const = 0;
	virtual RSE::GlobalShaderParameterType global_shader_parameter_get_type(const StringName &p_name) const = 0;

	virtual void global_shader_parameters_load_settings(bool p_load_textures) = 0;
	virtual void global_shader_parameters_clear() = 0;

	static int global_shader_uniform_type_get_shader_datatype(RSE::GlobalShaderParameterType p_type);

	/* FREE */

	virtual void free_rid(RID p_rid) = 0; // Free RIDs associated with the rendering server.
#ifndef DISABLE_DEPRECATED
	[[deprecated("Use `free_rid()` instead.")]] void free(RID p_rid) {
		free_rid(p_rid);
	}
#endif // DISABLE_DEPRECATED

	/* INTERPOLATION */

	virtual void set_physics_interpolation_enabled(bool p_enabled) = 0;

	/* EVENT QUEUING */

	virtual void request_frame_drawn_callback(const Callable &p_callable) = 0;

	virtual void draw(bool p_swap_buffers = true, double frame_step = 0.0) = 0;
	virtual void sync() = 0;
	virtual bool has_changed() const = 0;
	virtual void init();
	virtual void finish() = 0;
	virtual void tick() = 0;
	virtual void pre_draw(bool p_will_draw) = 0;

	/* STATUS INFORMATION */

	virtual uint64_t get_rendering_info(RSE::RenderingInfo p_info) = 0;
	virtual String get_video_adapter_name() const = 0;
	virtual String get_video_adapter_vendor() const = 0;
	virtual RenderingDeviceEnums::DeviceType get_video_adapter_type() const = 0;
	virtual String get_video_adapter_api_version() const = 0;

	virtual void set_frame_profiling_enabled(bool p_enable) = 0;
	virtual Vector<RenderingServerTypes::FrameProfileArea> get_frame_profile() = 0;
	virtual uint64_t get_frame_profile_frame() = 0;

	virtual double get_frame_setup_time_cpu() const = 0;

	virtual void gi_set_use_half_resolution(bool p_enable) = 0;

	/* TESTING */

	virtual RID get_test_cube() = 0;

	virtual RID get_test_texture();
	virtual RID get_white_texture();

	virtual void sdfgi_set_debug_probe_select(const Vector3 &p_position, const Vector3 &p_dir) = 0;

	virtual RID make_sphere_mesh(int p_lats, int p_lons, real_t p_radius);

	virtual void mesh_add_surface_from_mesh_data(RID p_mesh, const Geometry3D::MeshData &p_mesh_data);
	virtual void mesh_add_surface_from_planes(RID p_mesh, const Vector<Plane> &p_planes);

	/* BACKGROUND */

	virtual void set_boot_image_with_stretch(const Ref<Image> &p_image, const Color &p_color, RSE::SplashStretchMode p_stretch_mode, bool p_use_filter = true) = 0;
#ifndef DISABLE_DEPRECATED
	void set_boot_image(const Ref<Image> &p_image, const Color &p_color, bool p_scale, bool p_use_filter = true); // Superseded, but left to preserve compat.
#endif
	_ALWAYS_INLINE_ static RSE::SplashStretchMode map_scaling_option_to_stretch_mode(bool p_scale) {
		return p_scale ? RSE::SPLASH_STRETCH_MODE_KEEP : RSE::SPLASH_STRETCH_MODE_DISABLED;
	}

	_ALWAYS_INLINE_ static Rect2 get_splash_stretched_screen_rect(const Size2 &p_image_size, const Size2 &p_window_size, RSE::SplashStretchMode p_stretch_mode) {
		return RenderingServerTypes::get_splash_stretched_screen_rect(p_image_size, p_window_size, p_stretch_mode);
	}

	virtual Color get_default_clear_color() = 0;
	virtual void set_default_clear_color(const Color &p_color) = 0;

	/* MISC */

#ifndef DISABLE_DEPRECATED
	// Never actually used, should be removed when we can break compatibility.
	virtual bool has_feature(RSE::Features p_feature) const = 0;
#endif
	virtual bool has_os_feature(const String &p_feature) const = 0;

	virtual void set_debug_generate_wireframes(bool p_generate) = 0;

	virtual void call_set_vsync_mode(DisplayServerEnums::VSyncMode p_mode, DisplayServerEnums::WindowID p_window) = 0;

	virtual bool is_low_end() const = 0;

	virtual void set_print_gpu_profile(bool p_enable) = 0;

	virtual Size2i get_maximum_viewport_size() const = 0;

	RenderingDevice *get_rendering_device() const;
	RenderingDevice *create_local_rendering_device() const;

	bool is_render_loop_enabled() const;
	void set_render_loop_enabled(bool p_enabled);

	virtual bool is_on_render_thread() = 0;
	virtual void call_on_render_thread(const Callable &p_callable) = 0;

	String get_current_rendering_driver_name() const;
	String get_current_rendering_method() const;

#ifdef TOOLS_ENABLED
	virtual void get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const override;
#endif

	RenderingServer();
	virtual ~RenderingServer();

#ifdef TOOLS_ENABLED
	typedef void (*SurfaceUpgradeCallback)();
	void set_surface_upgrade_callback(SurfaceUpgradeCallback p_callback);
	void set_warn_on_surface_upgrade(bool p_warn);
#endif

#ifndef DISABLE_DEPRECATED
	void fix_surface_compatibility(RenderingServerTypes::SurfaceData &p_surface, const String &p_path = "");
#endif

private:
	// Binder helpers
	RID _texture_2d_layered_create(const TypedArray<Image> &p_layers, RSE::TextureLayeredType p_layered_type);
	RID _texture_3d_create(Image::Format p_format, int p_width, int p_height, int p_depth, bool p_mipmaps, const TypedArray<Image> &p_data);
	void _texture_3d_update(RID p_texture, const TypedArray<Image> &p_data);
	TypedArray<Image> _texture_3d_get(RID p_texture) const;
	TypedArray<Dictionary> _shader_get_shader_parameter_list(RID p_shader) const;
	RID _mesh_create_from_surfaces(const TypedArray<Dictionary> &p_surfaces, int p_blend_shape_count);
	void _mesh_add_surface(RID p_mesh, const Dictionary &p_surface);
	Dictionary _mesh_get_surface(RID p_mesh, int p_idx);
	TypedArray<Dictionary> _instance_geometry_get_shader_parameter_list(RID p_instance) const;
	TypedArray<Dictionary> _canvas_item_get_instance_shader_parameter_list(RID p_item) const;
	TypedArray<Image> _bake_render_uv2(RID p_base, const TypedArray<RID> &p_material_overrides, const Size2i &p_image_size);
	void _particles_set_trail_bind_poses(RID p_particles, const TypedArray<Transform3D> &p_bind_poses);
#ifdef TOOLS_ENABLED
	SurfaceUpgradeCallback surface_upgrade_callback = nullptr;
	bool warn_on_surface_upgrade = true;
#endif
};

// Make variant understand the enums.

VARIANT_ENUM_CAST_EXT(RSE::TextureType, RenderingServer::TextureType);
VARIANT_ENUM_CAST_EXT(RSE::TextureLayeredType, RenderingServer::TextureLayeredType);
VARIANT_ENUM_CAST_EXT(RSE::CubeMapLayer, RenderingServer::CubeMapLayer);
VARIANT_ENUM_CAST_EXT(RSE::TextureDrawableFormat, RenderingServer::TextureDrawableFormat);
VARIANT_ENUM_CAST_EXT(RSE::PipelineSource, RenderingServer::PipelineSource);
VARIANT_ENUM_CAST_EXT(RSE::ShaderMode, RenderingServer::ShaderMode);
VARIANT_ENUM_CAST_EXT(RSE::ArrayType, RenderingServer::ArrayType);
VARIANT_BITFIELD_CAST_EXT(RSE::ArrayFormat, RenderingServer::ArrayFormat);
VARIANT_ENUM_CAST_EXT(RSE::ArrayCustomFormat, RenderingServer::ArrayCustomFormat);
VARIANT_ENUM_CAST_EXT(RSE::PrimitiveType, RenderingServer::PrimitiveType);
VARIANT_ENUM_CAST_EXT(RSE::BlendShapeMode, RenderingServer::BlendShapeMode);
VARIANT_ENUM_CAST_EXT(RSE::MultimeshTransformFormat, RenderingServer::MultimeshTransformFormat);
VARIANT_ENUM_CAST_EXT(RSE::MultimeshPhysicsInterpolationQuality, RenderingServer::MultimeshPhysicsInterpolationQuality);
VARIANT_ENUM_CAST_EXT(RSE::LightType, RenderingServer::LightType);
VARIANT_ENUM_CAST_EXT(RSE::LightParam, RenderingServer::LightParam);
VARIANT_ENUM_CAST_EXT(RSE::LightBakeMode, RenderingServer::LightBakeMode);
VARIANT_ENUM_CAST_EXT(RSE::LightOmniShadowMode, RenderingServer::LightOmniShadowMode);
VARIANT_ENUM_CAST_EXT(RSE::LightDirectionalShadowMode, RenderingServer::LightDirectionalShadowMode);
VARIANT_ENUM_CAST_EXT(RSE::LightDirectionalSkyMode, RenderingServer::LightDirectionalSkyMode);
VARIANT_ENUM_CAST_EXT(RSE::LightProjectorFilter, RenderingServer::LightProjectorFilter);
VARIANT_ENUM_CAST_EXT(RSE::ReflectionProbeUpdateMode, RenderingServer::ReflectionProbeUpdateMode);
VARIANT_ENUM_CAST_EXT(RSE::ReflectionProbeAmbientMode, RenderingServer::ReflectionProbeAmbientMode);
VARIANT_ENUM_CAST_EXT(RSE::VoxelGIQuality, RenderingServer::VoxelGIQuality);
VARIANT_ENUM_CAST_EXT(RSE::DecalTexture, RenderingServer::DecalTexture);
VARIANT_ENUM_CAST_EXT(RSE::DecalFilter, RenderingServer::DecalFilter);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesMode, RenderingServer::ParticlesMode);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesTransformAlign, RenderingServer::ParticlesTransformAlign);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesTransformAlignCustomSrc, RenderingServer::ParticlesTransformAlignCustomSrc);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesTransformAlignAxis, RenderingServer::ParticlesTransformAlignAxis);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesDrawOrder, RenderingServer::ParticlesDrawOrder);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesEmitFlags, RenderingServer::ParticlesEmitFlags);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesCollisionType, RenderingServer::ParticlesCollisionType);
VARIANT_ENUM_CAST_EXT(RSE::ParticlesCollisionHeightfieldResolution, RenderingServer::ParticlesCollisionHeightfieldResolution);
VARIANT_ENUM_CAST_EXT(RSE::FogVolumeShape, RenderingServer::FogVolumeShape);
VARIANT_ENUM_CAST_EXT(RSE::ViewportScaling3DMode, RenderingServer::ViewportScaling3DMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportFrameGenerationMode, RenderingServer::ViewportFrameGenerationMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportUpdateMode, RenderingServer::ViewportUpdateMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportClearMode, RenderingServer::ViewportClearMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportEnvironmentMode, RenderingServer::ViewportEnvironmentMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportMSAA, RenderingServer::ViewportMSAA);
VARIANT_ENUM_CAST_EXT(RSE::ViewportAnisotropicFiltering, RenderingServer::ViewportAnisotropicFiltering);
VARIANT_ENUM_CAST_EXT(RSE::ViewportScreenSpaceAA, RenderingServer::ViewportScreenSpaceAA);
VARIANT_ENUM_CAST_EXT(RSE::ViewportRenderInfo, RenderingServer::ViewportRenderInfo);
VARIANT_ENUM_CAST_EXT(RSE::ViewportRenderInfoType, RenderingServer::ViewportRenderInfoType);
VARIANT_ENUM_CAST_EXT(RSE::ViewportDebugDraw, RenderingServer::ViewportDebugDraw);
VARIANT_ENUM_CAST_EXT(RSE::ViewportOcclusionCullingBuildQuality, RenderingServer::ViewportOcclusionCullingBuildQuality);
VARIANT_ENUM_CAST_EXT(RSE::ViewportSDFOversize, RenderingServer::ViewportSDFOversize);
VARIANT_ENUM_CAST_EXT(RSE::ViewportSDFScale, RenderingServer::ViewportSDFScale);
VARIANT_ENUM_CAST_EXT(RSE::ViewportVRSMode, RenderingServer::ViewportVRSMode);
VARIANT_ENUM_CAST_EXT(RSE::ViewportVRSUpdateMode, RenderingServer::ViewportVRSUpdateMode);
VARIANT_ENUM_CAST_EXT(RSE::SkyMode, RenderingServer::SkyMode);
VARIANT_ENUM_CAST_EXT(RSE::CompositorEffectCallbackType, RenderingServer::CompositorEffectCallbackType);
VARIANT_ENUM_CAST_EXT(RSE::CompositorEffectFlags, RenderingServer::CompositorEffectFlags);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentBG, RenderingServer::EnvironmentBG);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentAmbientSource, RenderingServer::EnvironmentAmbientSource);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentReflectionSource, RenderingServer::EnvironmentReflectionSource);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentGlowBlendMode, RenderingServer::EnvironmentGlowBlendMode);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentFogMode, RenderingServer::EnvironmentFogMode);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentToneMapper, RenderingServer::EnvironmentToneMapper);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSSRRoughnessQuality, RenderingServer::EnvironmentSSRRoughnessQuality);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSSAOQuality, RenderingServer::EnvironmentSSAOQuality);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSSILQuality, RenderingServer::EnvironmentSSILQuality);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSDFGIFramesToConverge, RenderingServer::EnvironmentSDFGIFramesToConverge);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSDFGIRayCount, RenderingServer::EnvironmentSDFGIRayCount);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSDFGIFramesToUpdateLight, RenderingServer::EnvironmentSDFGIFramesToUpdateLight);
VARIANT_ENUM_CAST_EXT(RSE::EnvironmentSDFGIYScale, RenderingServer::EnvironmentSDFGIYScale);
VARIANT_ENUM_CAST_EXT(RSE::PathtracingDenoiser, RenderingServer::PathtracingDenoiser);
VARIANT_ENUM_CAST_EXT(RSE::PathtracingBackend, RenderingServer::PathtracingBackend);
VARIANT_ENUM_CAST_EXT(RSE::SubSurfaceScatteringQuality, RenderingServer::SubSurfaceScatteringQuality);
VARIANT_ENUM_CAST_EXT(RSE::DOFBlurQuality, RenderingServer::DOFBlurQuality);
VARIANT_ENUM_CAST_EXT(RSE::DOFBokehShape, RenderingServer::DOFBokehShape);
VARIANT_ENUM_CAST_EXT(RSE::ShadowQuality, RenderingServer::ShadowQuality);
VARIANT_ENUM_CAST_EXT(RSE::InstanceType, RenderingServer::InstanceType);
VARIANT_ENUM_CAST_EXT(RSE::InstanceFlags, RenderingServer::InstanceFlags);
VARIANT_ENUM_CAST_EXT(RSE::ShadowCastingSetting, RenderingServer::ShadowCastingSetting);
VARIANT_ENUM_CAST_EXT(RSE::VisibilityRangeFadeMode, RenderingServer::VisibilityRangeFadeMode);
VARIANT_ENUM_CAST_EXT(RSE::NinePatchAxisMode, RenderingServer::NinePatchAxisMode);
VARIANT_ENUM_CAST_EXT(RSE::CanvasItemTextureFilter, RenderingServer::CanvasItemTextureFilter);
VARIANT_ENUM_CAST_EXT(RSE::CanvasItemTextureRepeat, RenderingServer::CanvasItemTextureRepeat);
VARIANT_ENUM_CAST_EXT(RSE::CanvasGroupMode, RenderingServer::CanvasGroupMode);
VARIANT_ENUM_CAST_EXT(RSE::CanvasLightMode, RenderingServer::CanvasLightMode);
VARIANT_ENUM_CAST_EXT(RSE::CanvasLightBlendMode, RenderingServer::CanvasLightBlendMode);
VARIANT_ENUM_CAST_EXT(RSE::CanvasLightShadowFilter, RenderingServer::CanvasLightShadowFilter);
VARIANT_ENUM_CAST_EXT(RSE::CanvasOccluderPolygonCullMode, RenderingServer::CanvasOccluderPolygonCullMode);
VARIANT_ENUM_CAST_EXT(RSE::GlobalShaderParameterType, RenderingServer::GlobalShaderParameterType);
VARIANT_ENUM_CAST_EXT(RSE::RenderingInfo, RenderingServer::RenderingInfo);
VARIANT_ENUM_CAST_EXT(RSE::SplashStretchMode, RenderingServer::SplashStretchMode);
VARIANT_ENUM_CAST_EXT(RSE::CanvasTextureChannel, RenderingServer::CanvasTextureChannel);
VARIANT_ENUM_CAST_EXT(RSE::BakeChannels, RenderingServer::BakeChannels);

#ifndef DISABLE_DEPRECATED
VARIANT_ENUM_CAST_EXT(RSE::Features, RenderingServer::Features);
#endif

// Alias to make it easier to use.
#define RS RenderingServer
