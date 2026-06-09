#[compute]

#version 450

#VERSION_DEFINES

// World Radiance Cache update compute shader.
//
// Camera-centered cascaded clipmap of octahedral-radiance probes packed into a
// roughly-square atlas (tile layout mirrors RtgiWrc::atlas_coord in
// servers/rendering/renderer_rd/effects/rtgi_wrc_math.h). A single shader runs
// two modes selected by the `mode` push-constant:
//   mode 0 -- scroll/recenter the previous-frame cache into the write atlas.
//   mode 1 -- accumulate this frame's probe-ray radiance into the write atlas.
//
// Task 6a fills in both kernel bodies (Task 4 shipped empty stubs). The
// binding-agnostic query API in ../raytracing/rtgi_wrc_inc.glsl is for the
// downstream consumers (Task 7); it is intentionally NOT included here -- the
// tile layout below is replicated inline to match RtgiWrc::atlas_coord exactly.
//
// Atlas layout (mirror of RtgiWrc::atlas_coord / wrc_atlas_tile_origin):
//   linear        = ((cascade * grid + z) * grid + y) * grid + x
//   tiles_per_row = ceil(sqrt(cascade_count * grid^3))
//   tile          = (linear % tiles_per_row, linear / tiles_per_row)
//   texel         = tile * oct_res + (dir % oct_res, dir / oct_res)
// This is the SAME `probe_index = (((cascade*grid+z)*grid+y)*grid+x)` the Task-5b
// producer packs into update_index = (probe_index << 6) | dir, so the forward
// map (mode 1) and inverse map (mode 0) round-trip a texel <-> probe exactly.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint mode; // 0 = scroll/recenter, 1 = accumulate.
	uint cascade_count;
	uint grid;
	uint oct_res;
	uint atlas_width;
	uint atlas_height;
	uint rays_this_frame;
	uint frame_index;
	float base_spacing;
	float temporal_n_cap;
	float feedback_damping;
	float view_prioritization;
	vec3 camera_pos;
	uint pad0;
	ivec4 scroll_delta[4]; // .xyz = integer probe-space recenter delta per cascade.
}
params;

// Front (read) atlases: previous-frame cache.
layout(set = 0, binding = 0, rgba16f) uniform image2D radiance_read_image;
layout(set = 0, binding = 1, rg16f) uniform image2D distance_read_image;
layout(set = 0, binding = 2, rgba8) uniform image2D metadata_read_image;
// Back (write) atlases: this-frame cache.
layout(set = 0, binding = 3, rgba16f) uniform image2D radiance_write_image;
layout(set = 0, binding = 4, rg16f) uniform image2D distance_write_image;
layout(set = 0, binding = 5, rgba8) uniform image2D metadata_write_image;

// Per-frame probe-ray results produced by the Task-5b WRC probe-update raygen.
// One entry per ray; matches RTGIWRCProbeRayResult in
// servers/rendering/renderer_rd/shaders/raytracing/raytracing_common_inc.glsl
// (3 x vec4 = 48 B). Only bound (and only read by mode 1) when rays_this_frame>0.
//   radiance_distance : .rgb = radiance, .a = first-hit distance.
//   normal_confidence : .rgb = encoded normal, .a = ray confidence.
//   metadata          : .x = unused (reserved), .y = confidence, .z = source_mask,
//                       .w = update_index = (probe_index << 6) | dir_index.
struct RTGIWRCProbeRayResult {
	vec4 radiance_distance;
	vec4 normal_confidence;
	vec4 metadata;
};

layout(set = 0, binding = 6, std430) readonly buffer WRCRayResults {
	RTGIWRCProbeRayResult results[];
};

// Number of probe tiles per atlas row. Mirrors RtgiWrc::atlas_tiles_per_row and
// the GLSL wrc_atlas_tiles_per_row(): ceil(sqrt(cascade_count * grid^3)). Bound
// to >= 1 so the modulo / divide below never hit zero.
uint wrc_tiles_per_row() {
	uint grid = max(params.grid, 1u);
	uint total_tiles = max(params.cascade_count, 1u) * grid * grid * grid;
	uint n = uint(ceil(sqrt(float(max(total_tiles, 1u)))));
	return max(n, 1u);
}

// Forward map: (cascade, probe x/y/z, dir) -> BACK-atlas texel. Replicates
// RtgiWrc::atlas_coord / wrc_atlas_tile_origin exactly (row-major probe
// linearization, row-major tile placement, dir -> in-tile (dir%res, dir/res)).
ivec2 wrc_atlas_coord(uint cascade, uvec3 probe, uint dir, uint tiles_per_row) {
	uint grid = max(params.grid, 1u);
	uint oct_res = max(params.oct_res, 1u);
	uint linear = ((cascade * grid + probe.z) * grid + probe.y) * grid + probe.x;
	uint tile_col = linear % tiles_per_row;
	uint tile_row = linear / tiles_per_row;
	uint texel_x = tile_col * oct_res + (dir % oct_res);
	uint texel_y = tile_row * oct_res + (dir / oct_res);
	return ivec2(int(texel_x), int(texel_y));
}

// Clamp a colour to finite, non-negative values so a NaN/Inf ray result cannot
// poison the accumulated radiance (RGBA16F max is 65504).
vec3 wrc_sanitize_color(vec3 c) {
	c = mix(c, vec3(0.0), isnan(c));
	c = mix(c, vec3(65504.0), isinf(c));
	return clamp(c, vec3(0.0), vec3(65504.0));
}

float wrc_sanitize_scalar(float v) {
	v = isnan(v) ? 0.0 : v;
	v = isinf(v) ? 65504.0 : v;
	return clamp(v, 0.0, 65504.0);
}

// Mode 0: scroll/recenter the previous-frame (FRONT) cache into the (BACK) write
// atlas. One thread == one BACK-atlas texel. We INVERT the atlas layout to find
// which (cascade, probe, dir) this texel owns, shift the probe by this cascade's
// integer scroll delta, and copy the matching FRONT texel back. Out-of-grid (or
// frontier tiles beyond cascade_count*grid^3) are cleared so stale data never
// leaks across a recenter. When scroll_delta is zero this is the identity copy
// FRONT->BACK that the ping-pong relies on (NO skip-when-zero here -- Task 9).
void wrc_scroll_main(ivec2 coord) {
	uint oct_res = max(params.oct_res, 1u);
	uint grid = max(params.grid, 1u);
	uint cascade_count = max(params.cascade_count, 1u);
	uint tiles_per_row = wrc_tiles_per_row();

	// Invert the texel -> tile -> linear -> (cascade, probe) mapping.
	uint tile_col = uint(coord.x) / oct_res;
	uint dir_x = uint(coord.x) % oct_res;
	uint tile_row = uint(coord.y) / oct_res;
	uint dir_y = uint(coord.y) % oct_res;
	uint dir = dir_y * oct_res + dir_x;
	uint linear = tile_row * tiles_per_row + tile_col;

	// Frontier texels: tiles past the last real probe (padding to fill the
	// square atlas) own no probe -> clear and bail.
	uint probe_total = cascade_count * grid * grid * grid;
	if (linear >= probe_total) {
		imageStore(radiance_write_image, coord, vec4(0.0));
		imageStore(distance_write_image, coord, vec4(0.0));
		imageStore(metadata_write_image, coord, vec4(0.0));
		return;
	}

	uint cube = grid * grid * grid;
	uint cascade = min(linear / cube, cascade_count - 1u);
	uint rem = linear - cascade * cube;
	uint x = rem % grid;
	uint y = (rem / grid) % grid;
	uint z = rem / (grid * grid);

	// Recenter: the probe that lives at (x,y,z) this frame held its data at
	// (x,y,z) + scroll_delta in last frame's (FRONT) grid.
	ivec3 old_probe = ivec3(int(x), int(y), int(z)) + params.scroll_delta[cascade].xyz;
	if (any(lessThan(old_probe, ivec3(0))) || any(greaterThanEqual(old_probe, ivec3(int(grid))))) {
		// Newly-revealed probe: no history to scroll in -> clear (marks
		// radiance/distance unwritten so the accumulate takes a fresh sample).
		imageStore(radiance_write_image, coord, vec4(0.0));
		imageStore(distance_write_image, coord, vec4(0.0));
		imageStore(metadata_write_image, coord, vec4(0.0));
		return;
	}

	ivec2 old_coord = wrc_atlas_coord(cascade, uvec3(old_probe), dir, tiles_per_row);
	imageStore(radiance_write_image, coord, imageLoad(radiance_read_image, old_coord));
	imageStore(distance_write_image, coord, imageLoad(distance_read_image, old_coord));
	imageStore(metadata_write_image, coord, imageLoad(metadata_read_image, old_coord));
}

// Mode 1: accumulate this frame's probe rays into the (BACK) write atlas.
// One thread == one ray result (1D dispatch over rays_this_frame). Each ray
// carries update_index = (probe_index << 6) | dir, which decodes to exactly one
// BACK-atlas texel via the forward layout above.
//
// In-place imageLoad/imageStore on the BACK atlas. The producer's view-biased +
// round-robin scheduler CAN map more than one ray to the same update_index in a
// frame (view rays fold onto a small camera-front probe set via modulo), so
// colliding threads resolve last-writer-wins (some samples dropped / confidence
// under-counts that frame). This matches STRC's accepted, self-correcting per-ray
// scatter-accumulate; all writes are in-bounds (memory-safe) and the 1/n temporal
// accumulation converges over subsequent frames.
//
// Mode 0 already recentered FRONT->BACK before this pass (barrier between the two
// dispatches), so the BACK texel here holds the prior accumulated state. We blend
// with a SAMPLE-COUNTED 1/n weight (NOT a fixed alpha): confidence .a encodes the
// running sample count as n = conf * temporal_n_cap, so a never-written texel
// (conf == 0) yields w == 1 and takes the new sample outright.
void wrc_accumulate_main() {
	// The local workgroup is 8x8 (shared with mode 0's 2D atlas dispatch), but
	// mode 1 is a 1D dispatch: compute_list_dispatch_threads(rays, 1, 1) still
	// rounds the Y group count up to 1, launching 8 thread-rows. Only row y==0
	// processes rays so each ray result is consumed by EXACTLY one thread (no
	// 8x over-accumulation / same-texel write race).
	if (gl_GlobalInvocationID.y != 0u) {
		return;
	}
	uint ray = gl_GlobalInvocationID.x;
	if (ray >= params.rays_this_frame) {
		return;
	}

	uint oct_res = max(params.oct_res, 1u);
	uint grid = max(params.grid, 1u);
	uint cascade_count = max(params.cascade_count, 1u);
	uint tiles_per_row = wrc_tiles_per_row();

	RTGIWRCProbeRayResult r = results[ray];
	uint update_index = uint(max(r.metadata.w, 0.0) + 0.5);
	uint probe_index = update_index >> 6u;
	uint dir = update_index & 63u; // 6-bit dir field assumes oct_res==8 (64 dirs); pinned by the producer packing.

	// Guard a stale/garbage update_index against the addressable probe range so a
	// bad index can never scribble outside the cache.
	uint probe_total = cascade_count * grid * grid * grid;
	if (probe_index >= probe_total) {
		return;
	}

	// Forward map probe_index (== WRC linear) + dir -> BACK-atlas texel.
	uint cube = grid * grid * grid;
	uint cascade = min(probe_index / cube, cascade_count - 1u);
	uint rem = probe_index - cascade * cube;
	uvec3 probe = uvec3(rem % grid, (rem / grid) % grid, rem / (grid * grid));
	ivec2 coord = wrc_atlas_coord(cascade, probe, dir, tiles_per_row);

	vec3 radiance_new = wrc_sanitize_color(r.radiance_distance.rgb);
	float hit_dist = wrc_sanitize_scalar(r.radiance_distance.a);

	float n_cap = max(params.temporal_n_cap, 1.0);

	// Read the current BACK texel in place (already recentered by mode 0).
	vec4 prev = imageLoad(radiance_write_image, coord);
	vec3 radiance_old = prev.rgb;
	float conf_old = clamp(prev.a, 0.0, 1.0);

	// Firefly clamp: once a texel has history, limit a single incoming sample's
	// luminance to a few times the accumulated luminance. The de-gated WRC query now
	// presents young texels at full weight, so an unclamped bright probe ray would flash
	// the cell for a frame; this tames it while the 1/n blend converges. Only the incoming
	// sample is clamped, never the stored radiance; a small floor still lets a dark texel
	// brighten when a light genuinely turns on.
	if (conf_old > 0.0) {
		const vec3 luma_w = vec3(0.2126, 0.7152, 0.0722);
		float lum_old = max(dot(radiance_old, luma_w), 0.0);
		float lum_new = max(dot(radiance_new, luma_w), 0.0);
		float lum_max = lum_old * 8.0 + 0.05;
		if (lum_new > lum_max) {
			radiance_new *= lum_max / max(lum_new, 1e-6);
		}
	}

	// Sample-counted 1/n blend. conf encodes n / n_cap, so n = conf * n_cap.
	float n = conf_old * n_cap;
	// Adaptive hysteresis (DDGI-style): if the (firefly-clamped) sample disagrees with
	// history, the indirect at this world point changed -- a moving light, or the dynamic
	// character passing through -- so collapse the effective sample count and re-converge
	// in a couple frames instead of trailing for n_cap. This is the dynamic-object
	// ghosting fix; static, noise-only texels see a small rel_change and keep the full
	// n_cap for stability. The reduced n raises the blend weight for radiance AND the
	// distance moments below, so the whole probe response speeds up together.
	if (conf_old > 0.0) {
		const vec3 luma_w2 = vec3(0.2126, 0.7152, 0.0722);
		float lo = max(dot(radiance_old, luma_w2), 0.0);
		float ln = max(dot(radiance_new, luma_w2), 0.0);
		float rel_change = abs(ln - lo) / (ln + lo + 1e-4);
		n = min(n, mix(n_cap, 2.0, smoothstep(0.3, 0.6, rel_change)));
	}
	float n_new = min(n + 1.0, n_cap);
	float w = 1.0 / max(n_new, 1.0);
	vec3 radiance_acc = mix(radiance_old, radiance_new, w);
	float conf_new = n_new / n_cap;
	imageStore(radiance_write_image, coord, vec4(radiance_acc, conf_new));

	// Distance moments (mean, mean^2) for Chebyshev visibility, blended 1/n with
	// the same weight. .zw stay 0 (RG16F ignores them, but keep them defined).
	vec2 dm = imageLoad(distance_write_image, coord).xy;
	float mean = mix(dm.x, hit_dist, w);
	float mean2 = mix(dm.y, hit_dist * hit_dist, w);
	imageStore(distance_write_image, coord, vec4(mean, mean2, 0.0, 0.0));
}

void main() {
	if (params.mode == 0u) {
		ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
		if (uint(coord.x) >= params.atlas_width || uint(coord.y) >= params.atlas_height) {
			return;
		}
		wrc_scroll_main(coord);
		return;
	}

	// Mode 1 is a 1D dispatch of rays_this_frame threads (one per ray result).
	wrc_accumulate_main();
}
