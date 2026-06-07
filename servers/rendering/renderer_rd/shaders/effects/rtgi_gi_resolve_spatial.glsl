#[compute]

#version 450

#VERSION_DEFINES

// RTGI GI Resolve SPATIAL a-trous, split out of rtgi_gi_resolve.glsl so its LDS tile lives ONLY on the
// spatial pipeline. The unified resolve shader specializes its other modes by a constant, but a
// module-scope `shared` declaration is allocated on every pipeline built from a module regardless of
// per-pipeline dead-code elimination, so keeping the LDS here (a standalone single-mode shader) leaves
// the INTEGRATE/TEMPORAL/DEBUG/COMPOSITE pipelines with no shared memory. The math is identical to the
// SPATIAL mode it replaces.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

// Same 80-byte std430 Params block as rtgi_gi_resolve.glsl (the C++ pushes the full PushConstant). This
// shader reads only screen_w/h, spatial_iter, cur_iter; the rest keep the layout identical.
layout(push_constant, std430) uniform Params {
	uint mode;
	uint frame_index;
	uint screen_w;
	uint screen_h;
	uint spatial_iter;
	uint cur_iter;
	uint spg_grid_w;
	uint spg_grid_h;
	uint spg_oct_res;
	uint spg_spacing_f;
	float temporal_n_cap;
	float rough_cutoff;
	uint rough_enabled;
	uint wrc_grid;
	uint wrc_cascade_count;
	float wrc_base_spacing;
	uint debug_channel;
	float history_rejection;
	uint write_reactive;
	uint pad2;
}
pc;

// SPARSE subset of the unified set-0 layout: only the bindings SPATIAL touches, at the SAME numbers so
// run_resolve sub-selects from its existing uniform array. depth + normal-roughness G-buffers (0-1),
// the GI write images 9/10 (DEST), the GI history samplers 11/12 (SOURCE), and the reconstruction UBO
// (14). No guide/velocity/SPG/WRC bindings.
layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_buffer;
layout(set = 0, binding = 9, rgba16f) uniform restrict image2D diffuse_gi_rw;
layout(set = 0, binding = 10, rgba16f) uniform restrict image2D spec_gi_rw;
layout(set = 0, binding = 11) uniform sampler2D diffuse_history;
layout(set = 0, binding = 12) uniform sampler2D spec_history;
layout(set = 0, binding = 14, std140) uniform GiResolveUBO {
	mat4 inv_projection; // clip -> view.
	mat4 inv_view; // view -> world (camera transform).
}
ubo;

// Cheap view-space LINEAR depth from raw reverse-Z, for the edge gate (lever B): a 2-term rational off
// inv_projection, dropping the off-axis terms (zero for a symmetric frustum, tiny+similar between
// nearby taps otherwise), skipping the full mat4 x vec4 reconstruct.
float resolve_cheap_linear_depth(float raw_depth) {
	float vz = ubo.inv_projection[2][2] * raw_depth + ubo.inv_projection[3][2];
	float vw = ubo.inv_projection[2][3] * raw_depth + ubo.inv_projection[3][3];
	return -vz / vw; // -view_z, positive in front.
}

// Per-tap surface read (levers A/B/C): ONE depth fetch (-> cheap linz) + ONE normal_roughness fetch
// (-> VIEW normal, no mat3; roughness decoded from .w, no separate guide_orm fetch). The edge dot is
// rotation-invariant, so VIEW normals give the same gate as the old world normals. Returns false for a
// background (raw <= 0) or degenerate-normal texel.
bool resolve_tap_surface(ivec2 p, out vec3 view_n, out float rough, out float linz) {
	view_n = vec3(0.0);
	rough = 0.0;
	linz = 0.0;
	float raw = texelFetch(depth_buffer, p, 0).r;
	if (raw <= 0.0) {
		return false; // background / sky.
	}
	vec4 nr = texelFetch(normal_roughness_buffer, p, 0);
	vec3 vn = nr.xyz * 2.0 - 1.0;
	if (dot(vn, vn) < 0.0001) {
		return false; // degenerate normal (no surface).
	}
	view_n = normalize(vn);
	// Decode the packed roughness from normal_roughness.w (engine idiom:
	// scene_forward_clustered_inc.glsl::normal_roughness_compatibility), avoiding a separate guide_orm fetch.
	float r = nr.w;
	if (r > 0.5) {
		r = 1.0 - r;
	}
	rough = r / (127.0 / 255.0);
	linz = resolve_cheap_linear_depth(raw);
	return true;
}

// Edge-stopping weight between the center surface and a tap, on depth + normal + roughness (a same-
// surface gate: a tap on a different plane / facing away / much rougher gets ~0). `cdepth`/`tdepth` are
// view-space linear depths (positive in front), `cn`/`tn` VIEW normals (the dot is rotation-invariant),
// `crough`/`trough` the surface roughness.
float resolve_edge_weight(float cdepth, vec3 cn, float crough, float tdepth, vec3 tn, float trough) {
	float depth_rel = abs(tdepth - cdepth) / max(cdepth, 1e-4);
	float w_depth = max(1.0 - depth_rel / 0.05, 0.0);
	float ndot = max(dot(cn, tn), 0.0);
	float w_normal = ndot * ndot * ndot * ndot;
	float w_rough = max(1.0 - abs(trough - crough) / 0.5, 0.0);
	return w_depth * w_normal * w_rough;
}

// LDS tile (step-1 iteration only): the 8x8 workgroup's 5x5 taps span a contiguous
// (GROUP_SIZE + 2*radius) square. The cooperative load fills these once; each pixel filters from shared
// memory. s_linz <= 0 marks an invalid texel (background / degenerate / off-screen halo).
#define SPATIAL_RADIUS 2
#define SPATIAL_TILE (GROUP_SIZE + 2 * SPATIAL_RADIUS)
#define SPATIAL_TILE_AREA (SPATIAL_TILE * SPATIAL_TILE)
shared float s_linz[SPATIAL_TILE_AREA];
shared vec3 s_vn[SPATIAL_TILE_AREA];
shared float s_rough[SPATIAL_TILE_AREA];
shared vec4 s_diff[SPATIAL_TILE_AREA];
shared vec4 s_spec[SPATIAL_TILE_AREA];

void resolve_spatial_main(ivec2 pos) {
	// LDS PATH (step-1 iteration, the default): cooperatively cache the 12x12 surface+signal tile, then
	// filter the 5x5 from shared memory. Byte-identical to the global path below.
	if (pc.cur_iter == 0u && pc.spatial_iter != 0u) {
		ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * GROUP_SIZE - ivec2(SPATIAL_RADIUS);
		for (uint t = gl_LocalInvocationIndex; t < uint(SPATIAL_TILE_AREA); t += uint(GROUP_SIZE * GROUP_SIZE)) {
			ivec2 lp = ivec2(int(t) % SPATIAL_TILE, int(t) / SPATIAL_TILE);
			ivec2 gp = tile_origin + lp;
			bool inb = gp.x >= 0 && gp.y >= 0 && gp.x < int(pc.screen_w) && gp.y < int(pc.screen_h);
			vec3 vn = vec3(0.0);
			float rg = 0.0;
			float lz = -1.0; // invalid sentinel.
			if (inb) {
				if (!resolve_tap_surface(gp, vn, rg, lz)) {
					lz = -1.0;
				}
				s_diff[t] = texelFetch(diffuse_history, gp, 0);
				s_spec[t] = texelFetch(spec_history, gp, 0);
			} else {
				s_diff[t] = vec4(0.0);
				s_spec[t] = vec4(0.0);
			}
			s_linz[t] = lz;
			s_vn[t] = vn;
			s_rough[t] = rg;
		}
		barrier();

		if (pos.x >= int(pc.screen_w) || pos.y >= int(pc.screen_h)) {
			return; // off-screen thread: contributed to the tile, no store.
		}

		ivec2 lc = ivec2(gl_LocalInvocationID.xy) + ivec2(SPATIAL_RADIUS);
		int ci = lc.y * SPATIAL_TILE + lc.x;
		vec4 lcd = s_diff[ci];
		vec4 lcs = s_spec[ci];
		if (s_linz[ci] <= 0.0) {
			imageStore(diffuse_gi_rw, pos, lcd);
			imageStore(spec_gi_rw, pos, lcs);
			return;
		}
		vec3 lcn = s_vn[ci];
		float lcrough = s_rough[ci];
		float lcdepth = s_linz[ci];
		float lboost_d = mix(0.25, 1.0, 1.0 - clamp(lcd.a, 0.0, 1.0));
		float lboost_s = mix(0.25, 1.0, 1.0 - clamp(lcs.a, 0.0, 1.0));
		vec3 lsd = lcd.rgb;
		float lwd = 1.0;
		vec3 lss = lcs.rgb;
		float lws = 1.0;
		for (int dy = -SPATIAL_RADIUS; dy <= SPATIAL_RADIUS; dy++) {
			for (int dx = -SPATIAL_RADIUS; dx <= SPATIAL_RADIUS; dx++) {
				if (dx == 0 && dy == 0) {
					continue;
				}
				int ti = (lc.y + dy) * SPATIAL_TILE + (lc.x + dx);
				if (s_linz[ti] <= 0.0) {
					continue;
				}
				float edge = resolve_edge_weight(lcdepth, lcn, lcrough, s_linz[ti], s_vn[ti], s_rough[ti]);
				if (edge <= 0.0) {
					continue;
				}
				float kernel = 1.0 / (1.0 + float(dx * dx + dy * dy));
				float w_geo = edge * kernel;
				vec4 td = s_diff[ti];
				vec4 ts = s_spec[ti];
				float wnd = w_geo * lboost_d * clamp(td.a, 0.0, 1.0);
				float wns = w_geo * lboost_s * clamp(ts.a, 0.0, 1.0);
				lsd += td.rgb * wnd;
				lwd += wnd;
				lss += ts.rgb * wns;
				lws += wns;
			}
		}
		imageStore(diffuse_gi_rw, pos, vec4((lwd > 0.0) ? (lsd / lwd) : lcd.rgb, lcd.a));
		imageStore(spec_gi_rw, pos, vec4((lws > 0.0) ? (lss / lws) : lcs.rgb, lcs.a));
		return;
	}

	// GLOBAL PATH (cur_iter > 0 escalation, or defensive spatial_iter == 0). main() does NOT early-out
	// off-screen threads (the LDS path needs them at the barrier), so bound-check here.
	if (pos.x >= int(pc.screen_w) || pos.y >= int(pc.screen_h)) {
		return;
	}
	vec4 cd = texelFetch(diffuse_history, pos, 0);
	vec4 cs = texelFetch(spec_history, pos, 0);
	if (pc.spatial_iter == 0u) {
		imageStore(diffuse_gi_rw, pos, cd);
		imageStore(spec_gi_rw, pos, cs);
		return;
	}
	vec3 cn;
	float crough;
	float cdepth;
	if (!resolve_tap_surface(pos, cn, crough, cdepth)) {
		imageStore(diffuse_gi_rw, pos, cd);
		imageStore(spec_gi_rw, pos, cs);
		return;
	}
	int atrous_step = 1 << int(pc.cur_iter);
	int radius = 2;
	float boost_d = mix(0.25, 1.0, 1.0 - clamp(cd.a, 0.0, 1.0));
	float boost_s = mix(0.25, 1.0, 1.0 - clamp(cs.a, 0.0, 1.0));
	vec3 sd = cd.rgb;
	float wd = 1.0;
	vec3 ss = cs.rgb;
	float ws = 1.0;
	for (int dy = -radius; dy <= radius; dy++) {
		for (int dx = -radius; dx <= radius; dx++) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			ivec2 tp = pos + ivec2(dx, dy) * atrous_step;
			if (tp.x < 0 || tp.y < 0 || tp.x >= int(pc.screen_w) || tp.y >= int(pc.screen_h)) {
				continue;
			}
			vec3 tn;
			float trough;
			float tdepth;
			if (!resolve_tap_surface(tp, tn, trough, tdepth)) {
				continue;
			}
			float edge = resolve_edge_weight(cdepth, cn, crough, tdepth, tn, trough);
			if (edge <= 0.0) {
				continue;
			}
			float kernel = 1.0 / (1.0 + float(dx * dx + dy * dy));
			float w_geo = edge * kernel;
			vec4 td = texelFetch(diffuse_history, tp, 0);
			vec4 ts = texelFetch(spec_history, tp, 0);
			float wnd = w_geo * boost_d * clamp(td.a, 0.0, 1.0);
			float wns = w_geo * boost_s * clamp(ts.a, 0.0, 1.0);
			sd += td.rgb * wnd;
			wd += wnd;
			ss += ts.rgb * wns;
			ws += wns;
		}
	}
	imageStore(diffuse_gi_rw, pos, vec4((wd > 0.0) ? (sd / wd) : cd.rgb, cd.a));
	imageStore(spec_gi_rw, pos, vec4((ws > 0.0) ? (ss / ws) : cs.rgb, cs.a));
}

void main() {
	// No off-screen early-return: the step-1 LDS path barriers, so all workgroup threads must enter
	// (resolve_spatial_main bounds-checks after the barrier / at the top of the global path).
	resolve_spatial_main(ivec2(gl_GlobalInvocationID.xy));
}
