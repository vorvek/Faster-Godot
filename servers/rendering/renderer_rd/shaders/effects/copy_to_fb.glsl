#[vertex]

#version 450

#VERSION_DEFINES

#ifdef USE_MULTIVIEW
#extension GL_EXT_multiview : enable
#define ViewIndex gl_ViewIndex
#endif // USE_MULTIVIEW

#define FLAG_FLIP_Y (1 << 0)
#define FLAG_USE_SECTION (1 << 1)
#define FLAG_FORCE_LUMINANCE (1 << 2)
#define FLAG_ALPHA_TO_ZERO (1 << 3)
#define FLAG_SRGB (1 << 4)
#define FLAG_ALPHA_TO_ONE (1 << 5)
#define FLAG_LINEAR (1 << 6)
#define FLAG_NORMAL (1 << 7)
#define FLAG_USE_SRC_SECTION (1 << 8)
#define FLAG_ALPHA_TO_LUMINANCE (1 << 9)

#ifdef USE_MULTIVIEW
layout(location = 0) out vec3 uv_interp;
#else
layout(location = 0) out vec2 uv_interp;
#endif

layout(push_constant, std430) uniform Params {
	vec4 section;
	vec2 pixel_size;
	float luminance_multiplier;
	uint flags;

	vec4 color;
}
params;

void main() {
	vec2 base_arr[4] = vec2[](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
	uv_interp.xy = base_arr[gl_VertexIndex];
#ifdef USE_MULTIVIEW
	uv_interp.z = ViewIndex;
#endif
	vec2 vpos = uv_interp.xy;
	if (bool(params.flags & FLAG_USE_SECTION)) {
		vpos = params.section.xy + vpos * params.section.zw;
	}

	gl_Position = vec4(vpos * 2.0 - 1.0, 0.0, 1.0);

	if (bool(params.flags & FLAG_FLIP_Y)) {
		uv_interp.y = 1.0 - uv_interp.y;
	}

	if (bool(params.flags & FLAG_USE_SRC_SECTION)) {
		uv_interp.xy = params.section.xy + uv_interp.xy * params.section.zw;
	}
}

#[fragment]

#version 450

#VERSION_DEFINES

#define FLAG_FLIP_Y (1 << 0)
#define FLAG_USE_SECTION (1 << 1)
#define FLAG_FORCE_LUMINANCE (1 << 2)
#define FLAG_ALPHA_TO_ZERO (1 << 3)
#define FLAG_SRGB (1 << 4)
#define FLAG_ALPHA_TO_ONE (1 << 5)
#define FLAG_LINEAR (1 << 6)
#define FLAG_NORMAL (1 << 7)
#define FLAG_ALPHA_TO_LUMINANCE (1 << 9)

layout(push_constant, std430) uniform Params {
	vec4 section;
	vec2 pixel_size;
	float luminance_multiplier;
	uint flags;

	vec4 color;
}
params;

#if !defined(MODE_SET_COLOR)
#ifdef USE_MULTIVIEW
layout(location = 0) in vec3 uv_interp;
#else
layout(location = 0) in vec2 uv_interp;
#endif

#ifdef MODE_FRAME_GEN
layout(set = 0, binding = 0) uniform sampler2D current_color;
layout(set = 1, binding = 0) uniform sampler2D previous_color;
layout(set = 2, binding = 0) uniform sampler2D velocity_texture;
#else
#ifdef USE_MULTIVIEW
layout(set = 0, binding = 0) uniform sampler2DArray source_color;
#ifdef MODE_TWO_SOURCES
layout(set = 1, binding = 0) uniform sampler2DArray source_depth;
layout(location = 1) out float depth;
#endif /* MODE_TWO_SOURCES */
#else /* USE_MULTIVIEW */
layout(set = 0, binding = 0) uniform sampler2D source_color;
#ifdef MODE_TWO_SOURCES
layout(set = 1, binding = 0) uniform sampler2D source_color2;
#endif /* MODE_TWO_SOURCES */
#endif /* USE_MULTIVIEW */
#endif /* MODE_FRAME_GEN */
#endif /* !SET_COLOR */

#ifndef MODE_COPY_DEPTH
layout(location = 0) out vec4 frag_color;
#endif

vec3 linear_to_srgb(vec3 color) {
	//if going to srgb, clamp from 0 to 1.
	color = clamp(color, vec3(0.0), vec3(1.0));
	const vec3 a = vec3(0.055f);
	return mix((vec3(1.0f) + a) * pow(color.rgb, vec3(1.0f / 2.4f)) - a, 12.92f * color.rgb, lessThan(color.rgb, vec3(0.0031308f)));
}

vec3 srgb_to_linear(vec3 color) {
	const vec3 a = vec3(0.055f);
	return mix(pow((color.rgb + a) * (1.0f / (vec3(1.0f) + a)), vec3(2.4f)), color.rgb * (1.0f / 12.92f), lessThan(color.rgb, vec3(0.04045f)));
}

#ifdef MODE_SHARP_BILINEAR
vec4 sample_sharp_bilinear(sampler2D tex, vec2 uv) {
	vec2 texel = uv / params.pixel_size;
	vec2 texel_floor = floor(texel);
	vec2 texel_fract = texel - texel_floor - 0.5;
	vec2 scale = 1.0 / max(vec2(0.0001), fwidth(texel));
	vec2 edge = clamp(texel_fract * scale, -0.5, 0.5);
	vec2 sharp_uv = (texel_floor + 0.5 + edge) * params.pixel_size;
	return textureLod(tex, sharp_uv, 0.0);
}
#endif

#ifdef MODE_BICUBIC
vec4 sample_bicubic_catmull_rom(sampler2D tex, vec2 uv) {
	vec2 texel = uv / params.pixel_size - 0.5;
	vec2 fcase = floor(texel);
	vec2 f = texel - fcase;

	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);

	vec2 s0 = w0;
	vec2 s1 = w1 + w2;
	vec2 s2 = w3;

	vec2 o0 = vec2(-1.0);
	vec2 o1 = w2 / s1;
	vec2 o2 = vec2(2.0);

	vec2 tc0 = (fcase + o0 + 0.5) * params.pixel_size;
	vec2 tc1 = (fcase + o1 + 0.5) * params.pixel_size;
	vec2 tc2 = (fcase + o2 + 0.5) * params.pixel_size;

	vec4 color = vec4(0.0);

	// Grid of 9 samples:
	color += textureLod(tex, vec2(tc0.x, tc0.y), 0.0) * (s0.x * s0.y);
	color += textureLod(tex, vec2(tc1.x, tc0.y), 0.0) * (s1.x * s0.y);
	color += textureLod(tex, vec2(tc2.x, tc0.y), 0.0) * (s2.x * s0.y);

	color += textureLod(tex, vec2(tc0.x, tc1.y), 0.0) * (s0.x * s1.y);
	color += textureLod(tex, vec2(tc1.x, tc1.y), 0.0) * (s1.x * s1.y);
	color += textureLod(tex, vec2(tc2.x, tc1.y), 0.0) * (s2.x * s1.y);

	color += textureLod(tex, vec2(tc0.x, tc2.y), 0.0) * (s0.x * s2.y);
	color += textureLod(tex, vec2(tc1.x, tc2.y), 0.0) * (s1.x * s2.y);
	color += textureLod(tex, vec2(tc2.x, tc2.y), 0.0) * (s2.x * s2.y);

	// CAS (Contrast Adaptive Sharpening) integration
	vec2 d_uv = fwidth(uv);
	if (d_uv.x == 0.0 || d_uv.y == 0.0) {
		d_uv = params.pixel_size * 0.5;
	}

	vec4 n = textureLod(tex, uv + vec2(0.0, -d_uv.y), 0.0);
	vec4 s = textureLod(tex, uv + vec2(0.0, d_uv.y), 0.0);
	vec4 w = textureLod(tex, uv + vec2(-d_uv.x, 0.0), 0.0);
	vec4 e = textureLod(tex, uv + vec2(d_uv.x, 0.0), 0.0);

	float luma_c = dot(color.rgb, vec3(0.299, 0.587, 0.114));
	float luma_n = dot(n.rgb, vec3(0.299, 0.587, 0.114));
	float luma_s = dot(s.rgb, vec3(0.299, 0.587, 0.114));
	float luma_w = dot(w.rgb, vec3(0.299, 0.587, 0.114));
	float luma_e = dot(e.rgb, vec3(0.299, 0.587, 0.114));

	float mn = min(luma_c, min(min(luma_n, luma_s), min(luma_w, luma_e)));
	float mx = max(luma_c, max(max(luma_n, luma_s), max(luma_w, luma_e)));

	float sharpness = 0.5; // Premium customizable sharpness default
	float amp = mix(-0.125, -0.05, sharpness);
	float r_mx = mx > 0.0001 ? 1.0 / mx : 1.0;
	float peak = sqrt(min(mn, 1.0 - mx) * r_mx);
	float weight = peak * amp;

	vec4 sharp_color = (color + (n + s + w + e) * weight) / (1.0 + 4.0 * weight);
	return clamp(sharp_color, vec4(0.0), vec4(1.0));
}
#endif

#ifdef MODE_SGSR
vec4 sample_sgsr(sampler2D tex, vec2 uv) {
	vec2 texel = uv / params.pixel_size - 0.5;
	vec2 fcase = floor(texel);
	vec2 f = texel - fcase;

	vec2 tc0 = (fcase + 0.5) * params.pixel_size;
	vec2 tc1 = tc0 + params.pixel_size;

	vec4 c00 = textureLod(tex, vec2(tc0.x, tc0.y), 0.0);
	vec4 c10 = textureLod(tex, vec2(tc1.x, tc0.y), 0.0);
	vec4 c01 = textureLod(tex, vec2(tc0.x, tc1.y), 0.0);
	vec4 c11 = textureLod(tex, vec2(tc1.x, tc1.y), 0.0);

	const vec3 luma_weight = vec3(0.299, 0.587, 0.114);
	float l00 = dot(c00.rgb, luma_weight);
	float l10 = dot(c10.rgb, luma_weight);
	float l01 = dot(c01.rgb, luma_weight);
	float l11 = dot(c11.rgb, luma_weight);

	float d1 = abs(l00 - l11);
	float d2 = abs(l10 - l01);

	vec4 color;
	if (d1 < d2) {
		float k = f.x + f.y;
		if (k < 1.0) {
			color = mix(c00, mix(c10, c01, 0.5), k);
		} else {
			color = mix(c11, mix(c10, c01, 0.5), 2.0 - k);
		}
	} else {
		float k = f.x - f.y;
		if (k > 0.0) {
			color = mix(c10, mix(c00, c11, 0.5), 1.0 - k);
		} else {
			color = mix(c01, mix(c00, c11, 0.5), 1.0 + k);
		}
	}

	vec4 bilinear_color = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
	float edge_strength = abs(d1 - d2) / (0.01 + d1 + d2);
	color = mix(bilinear_color, color, edge_strength);

	float min_l = min(min(l00, l10), min(l01, l11));
	float max_l = max(max(l00, l10), max(l01, l11));
	float r_max = max_l > 0.0001 ? 1.0 / max_l : 1.0;
	float peak = sqrt(min(min_l, 1.0 - max_l) * r_max);
	float sharpen_weight = peak * -0.15;

	vec4 sharp_color = color + sharpen_weight * (c00 + c10 + c01 + c11 - 4.0 * color);
	return clamp(sharp_color, vec4(0.0), vec4(1.0));
}
#endif

void main() {
#ifdef MODE_FRAME_GEN
	vec2 uv = uv_interp;
	vec2 velocity = textureLod(velocity_texture, uv, 0.0).xy;

	float warp = params.luminance_multiplier; // Repurposed for warp_scale.
	vec2 uv_curr = uv - 0.5 * velocity * warp;
	vec2 uv_prev = uv + 0.5 * velocity * warp;

	// Clamp UVs to avoid wrapping artifacts
	uv_curr = clamp(uv_curr, vec2(0.001), vec2(0.999));
	uv_prev = clamp(uv_prev, vec2(0.001), vec2(0.999));

	vec4 color_curr = textureLod(current_color, uv_curr, 0.0);
	vec4 color_prev = textureLod(previous_color, uv_prev, 0.0);

	// Premium discrepancy-based disocclusion masking
	// Compare color difference in RGB
	float diff = distance(color_curr.rgb, color_prev.rgb);

	// Adaptive threshold based on luma to avoid blending issues in dark vs bright regions
	float luma_curr = dot(color_curr.rgb, vec3(0.299, 0.587, 0.114));
	float luma_prev = dot(color_prev.rgb, vec3(0.299, 0.587, 0.114));
	float luma_avg = max(0.001, 0.5 * (luma_curr + luma_prev));
	float threshold = 0.12 * (1.0 + luma_avg * 1.5); // Smooth premium threshold scaling

	// If discrepancy is high, smoothly fall back to standard unwarped blending
	float mask = smoothstep(threshold * 1.5, threshold, diff);

	vec4 unwarped_curr = textureLod(current_color, uv, 0.0);
	vec4 unwarped_prev = textureLod(previous_color, uv, 0.0);
	vec4 fallback_blend = 0.5 * unwarped_curr + 0.5 * unwarped_prev;
	vec4 warped_blend = 0.5 * color_curr + 0.5 * color_prev;

	frag_color = mix(fallback_blend, warped_blend, mask);
#elif defined(MODE_SET_COLOR)
	frag_color = params.color;
#else

#ifdef USE_MULTIVIEW
	vec3 uv = uv_interp;
#else
	vec2 uv = uv_interp;
#endif

#ifdef MODE_PANORAMA_TO_DP
	// Note, multiview and panorama should not be mixed at this time

	//obtain normal from dual paraboloid uv
#define M_PI 3.14159265359

	float side;
	uv.y = modf(uv.y * 2.0, side);
	side = side * 2.0 - 1.0;
	vec3 normal = vec3(uv * 2.0 - 1.0, 0.0);
	normal.z = 0.5 - 0.5 * ((normal.x * normal.x) + (normal.y * normal.y));
	normal *= -side;
	normal = normalize(normal);

	//now convert normal to panorama uv

	vec2 st = vec2(atan(normal.x, normal.z), acos(normal.y));

	if (st.x < 0.0) {
		st.x += M_PI * 2.0;
	}

	uv = st / vec2(M_PI * 2.0, M_PI);

	if (side < 0.0) {
		//uv.y = 1.0 - uv.y;
		uv = 1.0 - uv;
	}
#endif /* MODE_PANORAMA_TO_DP */

#ifdef MODE_COPY_DEPTH
	gl_FragDepth = textureLod(source_color, uv, 0.0).r;
#else

#ifdef USE_MULTIVIEW
#ifdef MODE_SHARP_BILINEAR
	vec4 color = sample_sharp_bilinear(source_color, uv);
#elif defined(MODE_BICUBIC)
	vec4 color = sample_bicubic_catmull_rom(source_color, uv);
#elif defined(MODE_SGSR)
	vec4 color = sample_sgsr(source_color, uv);
#else
	vec4 color = textureLod(source_color, uv, 0.0);
#endif
#ifdef MODE_TWO_SOURCES
	// In multiview our 2nd input will be our depth map
	depth = textureLod(source_depth, uv, 0.0).r;
#endif /* MODE_TWO_SOURCES */

#else /* USE_MULTIVIEW */
#ifdef MODE_SHARP_BILINEAR
	vec4 color = sample_sharp_bilinear(source_color, uv);
#elif defined(MODE_BICUBIC)
	vec4 color = sample_bicubic_catmull_rom(source_color, uv);
#elif defined(MODE_SGSR)
	vec4 color = sample_sgsr(source_color, uv);
#else
	vec4 color = textureLod(source_color, uv, 0.0);
#endif
#ifdef MODE_TWO_SOURCES
	color += textureLod(source_color2, uv, 0.0);
#endif /* MODE_TWO_SOURCES */
#endif /* USE_MULTIVIEW */

	if (bool(params.flags & FLAG_FORCE_LUMINANCE)) {
		color.rgb = vec3(max(max(color.r, color.g), color.b));
	}
	if (bool(params.flags & FLAG_ALPHA_TO_ZERO)) {
		color.rgb *= color.a;
	}
	if (bool(params.flags & FLAG_SRGB)) {
		color.rgb = linear_to_srgb(color.rgb);
	}
	if (bool(params.flags & FLAG_ALPHA_TO_ONE)) {
		color.a = 1.0;
	}
	if (bool(params.flags & FLAG_LINEAR)) {
		color.rgb = srgb_to_linear(color.rgb);
	}
	if (bool(params.flags & FLAG_NORMAL)) {
		color.rgb = normalize(color.rgb * 2.0 - 1.0) * 0.5 + 0.5;
	}
	if (bool(params.flags & FLAG_ALPHA_TO_LUMINANCE)) {
		color.rgb = vec3(color.a);
		color.a = 1.0;
	}

	frag_color = color / params.luminance_multiplier;
#endif /* MODE_COPY_DEPTH */
#endif // MODE_SET_COLOR
}
