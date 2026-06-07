#pragma once
#include "core/math/vector3.h"
#include "tests/test_macros.h"

namespace TestSphericalRectangle {

struct SphQuad {
	Vector3 o, x, y, z;
	real_t z0, z0sq, x0, y0, y0sq, x1, y1, y1sq, b0, b1, b0sq, k, S;
};

// Reference port of Urena et al. 2013. MUST match area_light_sample_inc.glsl.
static SphQuad sph_quad_init(const Vector3 &s, const Vector3 &ex, const Vector3 &ey, const Vector3 &o) {
	SphQuad sq;
	sq.o = o;
	real_t exl = ex.length();
	real_t eyl = ey.length();
	sq.x = ex / exl;
	sq.y = ey / eyl;
	sq.z = sq.x.cross(sq.y);
	Vector3 d = s - o;
	sq.z0 = d.dot(sq.z);
	if (sq.z0 > 0.0) { sq.z = -sq.z; sq.z0 = -sq.z0; }
	sq.z0sq = sq.z0 * sq.z0;
	sq.x0 = d.dot(sq.x);
	sq.y0 = d.dot(sq.y);
	sq.x1 = sq.x0 + exl;
	sq.y1 = sq.y0 + eyl;
	sq.y0sq = sq.y0 * sq.y0;
	sq.y1sq = sq.y1 * sq.y1;
	Vector3 v00(sq.x0, sq.y0, sq.z0), v01(sq.x0, sq.y1, sq.z0);
	Vector3 v10(sq.x1, sq.y0, sq.z0), v11(sq.x1, sq.y1, sq.z0);
	Vector3 n0 = v00.cross(v10).normalized();
	Vector3 n1 = v10.cross(v11).normalized();
	Vector3 n2 = v11.cross(v01).normalized();
	Vector3 n3 = v01.cross(v00).normalized();
	real_t g0 = Math::acos(CLAMP(-n0.dot(n1), -1.0, 1.0));
	real_t g1 = Math::acos(CLAMP(-n1.dot(n2), -1.0, 1.0));
	real_t g2 = Math::acos(CLAMP(-n2.dot(n3), -1.0, 1.0));
	real_t g3 = Math::acos(CLAMP(-n3.dot(n0), -1.0, 1.0));
	sq.b0 = n0.z; sq.b1 = n2.z;
	sq.b0sq = sq.b0 * sq.b0;
	sq.k = 2.0 * Math::PI - g2 - g3;
	sq.S = g0 + g1 - sq.k;
	return sq;
}

static Vector3 sph_quad_sample(const SphQuad &sq, real_t u, real_t v) {
	real_t au = u * sq.S + sq.k;
	real_t fu = (Math::cos(au) * sq.b0 - sq.b1) / Math::sin(au);
	real_t cu = 1.0 / Math::sqrt(fu * fu + sq.b0sq) * (fu > 0.0 ? 1.0 : -1.0);
	cu = CLAMP(cu, -1.0, 1.0);
	real_t xu = -(cu * sq.z0) / Math::sqrt(1.0 - cu * cu);
	xu = CLAMP(xu, sq.x0, sq.x1);
	real_t d2 = xu * xu + sq.z0sq;
	real_t h0 = sq.y0 / Math::sqrt(d2 + sq.y0sq);
	real_t h1 = sq.y1 / Math::sqrt(d2 + sq.y1sq);
	real_t hv = h0 + v * (h1 - h0);
	real_t hv2 = hv * hv;
	real_t yv = (hv2 < 1.0 - 1e-6) ? (hv * Math::sqrt(d2)) / Math::sqrt(1.0 - hv2) : sq.y1;
	return sq.o + xu * sq.x + yv * sq.y + sq.z0 * sq.z;
}

// Dense-quadrature ground truth for the test rectangle (no magic constants):
// rect is x,y in [-0.5,0.5] at z=1 with emitter normal facing down (-z); observer
// at the origin with shading normal N=+z. Solid-angle element dw = cos_light/r^2 dA;
// here cos_light = cos_recv = 1/r, so S = sum dA/r^3 and E = sum dA/r^4.
static void rect_quadrature(real_t &out_S, real_t &out_E) {
	const int Q = 400;
	const real_t cell = 1.0 / Q;
	out_S = 0.0;
	out_E = 0.0;
	for (int i = 0; i < Q; i++) {
		for (int j = 0; j < Q; j++) {
			real_t x = -0.5 + (i + 0.5) * cell;
			real_t y = -0.5 + (j + 0.5) * cell;
			real_t r2 = x * x + y * y + 1.0;
			real_t r = Math::sqrt(r2);
			real_t dw = (1.0 / r) / r2 * (cell * cell); // cos_light/r^2 dA
			out_S += dw;
			out_E += (1.0 / r) * dw; // cos_recv * dw
		}
	}
}

TEST_CASE("[SphericalRectangle] analytic solid angle matches dense quadrature") {
	// 1x1 rect (corner at s, full edges ex/ey) centered over the origin at height 1.
	Vector3 s(-0.5, -0.5, 1.0), ex(1, 0, 0), ey(0, 1, 0), o(0, 0, 0);
	SphQuad sq = sph_quad_init(s, ex, ey, o);
	real_t S_quad, E_quad;
	rect_quadrature(S_quad, E_quad);
	CHECK(sq.S == doctest::Approx((double)S_quad).epsilon(0.01)); // ~0.8054 sr
}

TEST_CASE("[SphericalRectangle] samples land on the rectangle plane and within span") {
	Vector3 s(-0.5, -0.5, 1.0), ex(1, 0, 0), ey(0, 1, 0), o(0, 0, 0);
	SphQuad sq = sph_quad_init(s, ex, ey, o);
	for (int i = 0; i <= 8; i++) {
		for (int j = 0; j <= 8; j++) {
			Vector3 p = sph_quad_sample(sq, i / 8.0, j / 8.0);
			CHECK(p.z == doctest::Approx(1.0).epsilon(1e-4)); // on the plane
			CHECK(p.x >= -0.5 - 1e-4); CHECK(p.x <= 0.5 + 1e-4); // within span
			CHECK(p.y >= -0.5 - 1e-4); CHECK(p.y <= 0.5 + 1e-4);
		}
	}
}

TEST_CASE("[SphericalRectangle] Monte Carlo irradiance matches dense quadrature") {
	// Irradiance from a unit-radiance rect via 1/S sampling must equal the
	// dense-quadrature ground truth E_quad (within MC tolerance). No magic constant.
	Vector3 s(-0.5, -0.5, 1.0), ex(1, 0, 0), ey(0, 1, 0), o(0, 0, 0);
	SphQuad sq = sph_quad_init(s, ex, ey, o);
	real_t S_quad, E_quad;
	rect_quadrature(S_quad, E_quad);
	Vector3 N(0, 0, 1);
	double acc = 0.0;
	int M = 40000;
	uint32_t rng = 12345u;
	auto randf = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0 / 16777216.0); };
	for (int i = 0; i < M; i++) {
		Vector3 p = sph_quad_sample(sq, randf(), randf());
		Vector3 L = (p - o).normalized();
		acc += MAX(0.0, N.dot(L)); // integrand cos(theta); pdf = 1/S so estimator = mean*S
	}
	double E_mc = (acc / M) * sq.S;
	CHECK(E_mc == doctest::Approx((double)E_quad).epsilon(0.03));
}

} // namespace TestSphericalRectangle
