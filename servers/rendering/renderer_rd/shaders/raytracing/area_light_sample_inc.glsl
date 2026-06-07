// Spherical-rectangle (Urena et al. 2013) solid-angle sampling for area lights.
// Reference implementation tested in tests/servers/rendering/test_spherical_rectangle.h.
struct SphQuad {
	vec3 o, x, y, z;
	float z0, z0sq, x0, y0, y0sq, x1, y1, y1sq, b0, b1, b0sq, k, S;
};

// s = a corner of the rectangle, ex/ey = full edge vectors, o = shading point.
SphQuad sph_quad_init(vec3 s, vec3 ex, vec3 ey, vec3 o) {
	SphQuad sq;
	sq.o = o;
	float exl = length(ex);
	float eyl = length(ey);
	sq.x = ex / max(exl, 1e-8);
	sq.y = ey / max(eyl, 1e-8);
	sq.z = cross(sq.x, sq.y);
	vec3 d = s - o;
	sq.z0 = dot(d, sq.z);
	if (sq.z0 > 0.0) { sq.z = -sq.z; sq.z0 = -sq.z0; }
	sq.z0sq = sq.z0 * sq.z0;
	sq.x0 = dot(d, sq.x);
	sq.y0 = dot(d, sq.y);
	sq.x1 = sq.x0 + exl;
	sq.y1 = sq.y0 + eyl;
	sq.y0sq = sq.y0 * sq.y0;
	sq.y1sq = sq.y1 * sq.y1;
	vec3 v00 = vec3(sq.x0, sq.y0, sq.z0);
	vec3 v01 = vec3(sq.x0, sq.y1, sq.z0);
	vec3 v10 = vec3(sq.x1, sq.y0, sq.z0);
	vec3 v11 = vec3(sq.x1, sq.y1, sq.z0);
	vec3 n0 = normalize(cross(v00, v10));
	vec3 n1 = normalize(cross(v10, v11));
	vec3 n2 = normalize(cross(v11, v01));
	vec3 n3 = normalize(cross(v01, v00));
	float g0 = acos(clamp(-dot(n0, n1), -1.0, 1.0));
	float g1 = acos(clamp(-dot(n1, n2), -1.0, 1.0));
	float g2 = acos(clamp(-dot(n2, n3), -1.0, 1.0));
	float g3 = acos(clamp(-dot(n3, n0), -1.0, 1.0));
	sq.b0 = n0.z; sq.b1 = n2.z;
	sq.b0sq = sq.b0 * sq.b0;
	sq.k = 2.0 * PI - g2 - g3;
	sq.S = g0 + g1 - sq.k;
	return sq;
}

vec3 sph_quad_sample(SphQuad sq, float u, float v) {
	float au = u * sq.S + sq.k;
	float fu = (cos(au) * sq.b0 - sq.b1) / sin(au);
	float cu = 1.0 / sqrt(fu * fu + sq.b0sq) * (fu > 0.0 ? 1.0 : -1.0);
	cu = clamp(cu, -1.0, 1.0);
	float xu = -(cu * sq.z0) / sqrt(max(1.0 - cu * cu, 1e-10));
	xu = clamp(xu, sq.x0, sq.x1);
	float d2 = xu * xu + sq.z0sq;
	float h0 = sq.y0 / sqrt(d2 + sq.y0sq);
	float h1 = sq.y1 / sqrt(d2 + sq.y1sq);
	float hv = h0 + v * (h1 - h0);
	float hv2 = hv * hv;
	float yv = (hv2 < 1.0 - 1e-6) ? (hv * sqrt(d2)) / sqrt(1.0 - hv2) : sq.y1;
	return sq.o + xu * sq.x + yv * sq.y + sq.z0 * sq.z;
}
