#ifndef RTGI_SPG_INC_H
#define RTGI_SPG_INC_H
// GLSL counterpart of rtgi_spg_math.h. Hemi-oct (upper hemisphere, local +Z = anchor normal).
void spg_build_basis(vec3 n, out vec3 t, out vec3 b) {
	float s = (n.z >= 0.0) ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float bb = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
	b = vec3(bb, s + n.y * n.y * a, -n.y);
}
vec2 spg_hemioct_encode(vec3 v) {
	float denom = abs(v.x) + abs(v.y) + max(v.z, 0.0);
	float inv = (denom > 0.0) ? (1.0 / denom) : 0.0;
	float px = v.x * inv;
	float py = v.y * inv;
	return vec2((px + py) * 0.5 + 0.5, (px - py) * 0.5 + 0.5);
}
vec3 spg_hemioct_decode(vec2 oct) {
	float ex = oct.x * 2.0 - 1.0;
	float ey = oct.y * 2.0 - 1.0;
	vec2 t = vec2(ex + ey, ex - ey) * 0.5;
	vec3 v = vec3(t.x, t.y, 1.0 - abs(t.x) - abs(t.y));
	float len = length(v);
	return (len > 0.0) ? (v / len) : vec3(0.0, 0.0, 1.0);
}
vec3 spg_local_to_world(vec3 l, vec3 t, vec3 b, vec3 n) {
	vec3 w = t * l.x + b * l.y + n * l.z;
	float len = length(w);
	return (len > 0.0) ? (w / len) : n;
}
#endif // RTGI_SPG_INC_H
