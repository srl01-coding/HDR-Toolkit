/*
HDR Toolkit
Copyright (C) 2026 srl01-coding

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "quad-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace quad {

namespace {

inline Vec2 sub(Vec2 a, Vec2 b)
{
	return {a.x - b.x, a.y - b.y};
}
inline Vec2 add(Vec2 a, Vec2 b)
{
	return {a.x + b.x, a.y + b.y};
}
inline Vec2 mul(Vec2 a, double s)
{
	return {a.x * s, a.y * s};
}
inline double cross(Vec2 a, Vec2 b)
{
	return a.x * b.y - a.y * b.x;
}
inline double dot(Vec2 a, Vec2 b)
{
	return a.x * b.x + a.y * b.y;
}
inline double len(Vec2 a)
{
	return std::sqrt(dot(a, a));
}
inline bool finite(Vec2 a)
{
	return std::isfinite(a.x) && std::isfinite(a.y);
}

struct BilinearTerms {
	Vec2 e, f, g;
};
BilinearTerms terms(const Quad &q)
{
	return {sub(q.b, q.a), sub(q.d, q.a), add(sub(q.a, q.b), sub(q.c, q.d))};
}

constexpr double kUvTol = 1e-8; // CPU reference tolerance on (u, v)

} // namespace

bool is_identity(const Quad &q)
{
	return q.a.x == 0 && q.a.y == 0 && q.b.x == 1 && q.b.y == 0 && q.c.x == 1 && q.c.y == 1 && q.d.x == 0 &&
	       q.d.y == 1;
}

Vec2 forward_bilinear(const Quad &q, double u, double v)
{
	const BilinearTerms t = terms(q);
	return add(add(add(q.a, mul(t.e, u)), mul(t.f, v)), mul(t.g, u * v));
}

bool inverse_bilinear(const Quad &q, Vec2 p, double &u_out, double &v_out)
{
	const BilinearTerms t = terms(q);
	const Vec2 h = sub(p, q.a);
	const double qa = cross(t.g, t.f);
	const double qb = cross(t.e, t.f) + cross(h, t.g);
	const double qc = cross(h, t.e);

	double roots[2];
	int n = 0;
	if (std::fabs(qa) <= 1e-12 * std::max(std::fabs(qb), 1e-300)) {
		if (std::fabs(qb) < 1e-300)
			return false;
		roots[n++] = -qc / qb; // linear case (parallelogram)
	} else {
		double disc = qb * qb - 4.0 * qa * qc;
		if (disc < -1e-12 * (qb * qb + std::fabs(4.0 * qa * qc)))
			return false;
		disc = std::max(disc, 0.0);
		const double z = -0.5 * (qb + std::copysign(std::sqrt(disc), qb));
		if (std::fabs(z) < 1e-300) {
			roots[n++] = -qb / (2.0 * qa);
		} else {
			roots[n++] = z / qa;
			roots[n++] = qc / z;
		}
	}

	bool found = false;
	double best = 0;
	for (int i = 0; i < n; i++) {
		const double v = roots[i];
		const Vec2 k = add(t.e, mul(t.g, v));
		const double kk = dot(k, k);
		if (kk < 1e-24 || !std::isfinite(v))
			continue;
		const double u = dot(sub(h, mul(t.f, v)), k) / kk;
		if (!(u >= -kUvTol && u <= 1 + kUvTol && v >= -kUvTol && v <= 1 + kUvTol))
			continue;
		const double r = len(sub(forward_bilinear(q, u, v), p));
		if (!found || r < best) {
			found = true;
			best = r;
			u_out = u;
			v_out = v;
		}
	}
	return found;
}

bool solve_homography(const Quad &q, Mat3 &h)
{
	const double src[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	const Vec2 dst[4] = {q.a, q.b, q.c, q.d};
	double m[8][9];
	for (int i = 0; i < 4; i++) {
		const double u = src[i][0], v = src[i][1], x = dst[i].x, y = dst[i].y;
		const double r0[9] = {u, v, 1, 0, 0, 0, -x * u, -x * v, x};
		const double r1[9] = {0, 0, 0, u, v, 1, -y * u, -y * v, y};
		std::copy(r0, r0 + 9, m[2 * i]);
		std::copy(r1, r1 + 9, m[2 * i + 1]);
	}
	// Gaussian elimination with partial pivoting.
	for (int col = 0; col < 8; col++) {
		int piv = col;
		for (int r = col + 1; r < 8; r++)
			if (std::fabs(m[r][col]) > std::fabs(m[piv][col]))
				piv = r;
		if (std::fabs(m[piv][col]) < 1e-12)
			return false;
		if (piv != col)
			for (int k = 0; k < 9; k++)
				std::swap(m[piv][k], m[col][k]);
		for (int r = 0; r < 8; r++) {
			if (r == col)
				continue;
			const double fct = m[r][col] / m[col][col];
			for (int k = col; k < 9; k++)
				m[r][k] -= fct * m[col][k];
		}
	}
	double sol[8];
	for (int i = 0; i < 8; i++)
		sol[i] = m[i][8] / m[i][i];
	h = {{{sol[0], sol[1], sol[2]}, {sol[3], sol[4], sol[5]}, {sol[6], sol[7], 1.0}}};
	for (auto &row : h)
		for (double x : row)
			if (!std::isfinite(x))
				return false;
	return true;
}

bool invert3(const Mat3 &m, Mat3 &inv)
{
	const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	double norm = 0;
	for (auto &row : m)
		for (double x : row)
			norm = std::max(norm, std::fabs(x));
	if (!std::isfinite(det) || std::fabs(det) < 1e-12 * norm * norm * norm)
		return false;
	const double id = 1.0 / det;
	inv[0][0] = c00 * id;
	inv[1][0] = c01 * id;
	inv[2][0] = c02 * id;
	inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
	inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
	inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
	inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
	inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
	inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
	return true;
}

Vec2 project(const Mat3 &h, double u, double v)
{
	const double x = h[0][0] * u + h[0][1] * v + h[0][2];
	const double y = h[1][0] * u + h[1][1] * v + h[1][2];
	const double w = h[2][0] * u + h[2][1] * v + h[2][2];
	return {x / w, y / w};
}

Validation validate(const Quad &q, bool projective)
{
	Validation r;
	const Vec2 p[4] = {q.a, q.b, q.c, q.d};
	static const char *names[4] = {"top-left", "top-right", "bottom-right", "bottom-left"};

	for (int i = 0; i < 4; i++) {
		if (!finite(p[i])) {
			r.reason = std::string(names[i]) + " corner is not a finite number";
			return r;
		}
		if (p[i].x < kCoordMin || p[i].x > kCoordMax || p[i].y < kCoordMin || p[i].y > kCoordMax) {
			r.reason = std::string(names[i]) + " corner outside -400%..500%";
			return r;
		}
	}

	// Duplicate corners / zero-length edges (incl. diagonals).
	double max_edge = 0;
	for (int i = 0; i < 4; i++)
		for (int j = i + 1; j < 4; j++) {
			const double l = len(sub(p[j], p[i]));
			if (l < 1e-4) {
				r.reason = std::string(names[i]) + " and " + names[j] + " corners coincide";
				return r;
			}
			if (j == i + 1 || (i == 0 && j == 3))
				max_edge = std::max(max_edge, l);
		}

	// Convexity and orientation: consecutive edge cross products must share a
	// sign (either winding allowed, so mirroring is supported). This also
	// rejects crossed opposite edges and folded shapes.
	double s[4];
	for (int i = 0; i < 4; i++) {
		const Vec2 e0 = sub(p[(i + 1) % 4], p[i]);
		const Vec2 e1 = sub(p[(i + 2) % 4], p[(i + 1) % 4]);
		s[i] = cross(e0, e1);
	}
	const bool all_pos = s[0] > 0 && s[1] > 0 && s[2] > 0 && s[3] > 0;
	const bool all_neg = s[0] < 0 && s[1] < 0 && s[2] < 0 && s[3] < 0;
	if (!all_pos && !all_neg) {
		r.reason = "corners cross or fold (quad is not convex)";
		return r;
	}

	// Signed area (shoelace) and thinness relative to size.
	double area = 0;
	for (int i = 0; i < 4; i++)
		area += cross(p[i], p[(i + 1) % 4]);
	area *= 0.5;
	r.area = area;
	if (std::fabs(area) < 1e-4 * max_edge * max_edge) {
		r.reason = "quad is too thin";
		return r;
	}

	// Bilinear Jacobian determinant at the source-square corners (affine in
	// (u,v), so consistent nonzero corner signs exclude a zero inside).
	const BilinearTerms t = terms(q);
	const double j[4] = {cross(t.e, t.f), cross(t.e, add(t.f, t.g)), cross(add(t.e, t.g), t.f),
			     cross(add(t.e, t.g), add(t.f, t.g))};
	const double jtol = 1e-6 * max_edge * max_edge;
	const bool jpos = j[0] > jtol && j[1] > jtol && j[2] > jtol && j[3] > jtol;
	const bool jneg = j[0] < -jtol && j[1] < -jtol && j[2] < -jtol && j[3] < -jtol;
	if (!jpos && !jneg) {
		r.reason = "bilinear map folds or is near-singular";
		return r;
	}

	if (projective) {
		Mat3 h, hi;
		if (!solve_homography(q, h) || !invert3(h, hi)) {
			r.reason = "perspective mapping is singular";
			return r;
		}
		const double src[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		double wmax = 0;
		double w[4];
		for (int i = 0; i < 4; i++) {
			w[i] = h[2][0] * src[i][0] + h[2][1] * src[i][1] + h[2][2];
			wmax = std::max(wmax, std::fabs(w[i]));
			const Vec2 back = project(h, src[i][0], src[i][1]);
			if (len(sub(back, p[i])) > 1e-9 * std::max(1.0, max_edge)) {
				r.reason = "perspective mapping residual too large";
				return r;
			}
		}
		const bool wpos = w[0] > 1e-6 * wmax && w[1] > 1e-6 * wmax && w[2] > 1e-6 * wmax && w[3] > 1e-6 * wmax;
		const bool wneg = w[0] < -1e-6 * wmax && w[1] < -1e-6 * wmax && w[2] < -1e-6 * wmax &&
				  w[3] < -1e-6 * wmax;
		if (!wpos && !wneg) {
			r.reason = "perspective mapping crosses the horizon";
			return r;
		}
		// H^-1 * H must be the identity (conditioning check).
		double err = 0, scale = 0;
		for (int i = 0; i < 3; i++)
			for (int k = 0; k < 3; k++) {
				double acc = 0;
				for (int m = 0; m < 3; m++)
					acc += hi[i][m] * h[m][k];
				scale = std::max(scale, std::fabs(acc));
				err = std::max(err, std::fabs(acc - (i == k ? 1.0 : 0.0)));
			}
		if (err > 1e-9 * std::max(1.0, scale)) {
			r.reason = "perspective mapping is ill-conditioned";
			return r;
		}
	}

	r.ok = true;
	return r;
}

BilinearUniforms bilinear_uniforms(const Quad &q)
{
	const BilinearTerms t = terms(q);
	BilinearUniforms u;
	u.a[0] = (float)q.a.x;
	u.a[1] = (float)q.a.y;
	u.e[0] = (float)t.e.x;
	u.e[1] = (float)t.e.y;
	u.f[0] = (float)t.f.x;
	u.f[1] = (float)t.f.y;
	u.g[0] = (float)t.g.x;
	u.g[1] = (float)t.g.y;
	return u;
}

bool projective_uniforms(const Quad &q, ProjectiveUniforms &out)
{
	Mat3 h, hi;
	if (!solve_homography(q, h) || !invert3(h, hi))
		return false;
	double norm = 0;
	for (auto &row : hi)
		for (double x : row)
			norm = std::max(norm, std::fabs(x));
	if (!(norm > 0) || !std::isfinite(norm))
		return false;
	for (auto &row : hi)
		for (double &x : row)
			x /= norm; // positive scale: keeps the sign of w
	float *rows[3] = {out.row0, out.row1, out.row2};
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
			rows[i][k] = (float)hi[i][k];
	// Sign of the homogeneous coordinate at the image of the source centre.
	const Vec2 c = project(h, 0.5, 0.5);
	const double w = hi[2][0] * c.x + hi[2][1] * c.y + hi[2][2];
	out.w_sign = w >= 0 ? 1.0f : -1.0f;
	return true;
}

// ---------------------------------------------------------------------------
// Float mirrors of hdr-transform.effect. Keep identical to the shader.
// ---------------------------------------------------------------------------

namespace {
constexpr float kShaderUvTol = 1e-5f;

inline float crossf(float ax, float ay, float bx, float by)
{
	return ax * by - ay * bx;
}
} // namespace

std::array<float, 3> shader_inverse_bilinear(const BilinearUniforms &uni, float px, float py)
{
	const float hx = px - uni.a[0], hy = py - uni.a[1];
	const float ex = uni.e[0], ey = uni.e[1], fx = uni.f[0], fy = uni.f[1], gx = uni.g[0], gy = uni.g[1];
	const float qa = crossf(gx, gy, fx, fy);
	const float qb = crossf(ex, ey, fx, fy) + crossf(hx, hy, gx, gy);
	const float qc = crossf(hx, hy, ex, ey);

	float v1, v2;
	if (std::fabs(qa) <= 1e-6f * std::fabs(qb)) {
		if (std::fabs(qb) < 1e-12f)
			return {0, 0, 0};
		v1 = -qc / qb;
		v2 = v1;
	} else {
		float disc = qb * qb - 4.0f * qa * qc;
		if (disc < -1e-5f * (qb * qb + std::fabs(4.0f * qa * qc)))
			return {0, 0, 0};
		disc = std::max(disc, 0.0f);
		const float sgn = qb >= 0.0f ? 1.0f : -1.0f;
		const float z = -0.5f * (qb + sgn * std::sqrt(disc));
		if (std::fabs(z) < 1e-12f) {
			v1 = -qb / (2.0f * qa);
			v2 = v1;
		} else {
			v1 = z / qa;
			v2 = qc / z;
		}
	}

	float best_r = 1e6f, bu = 0, bv = 0, ok = 0;
	const float vs[2] = {v1, v2};
	for (float v : vs) {
		const float kx = ex + gx * v, ky = ey + gy * v;
		const float kk = kx * kx + ky * ky;
		if (!(kk > 1e-12f))
			continue;
		const float u = ((hx - fx * v) * kx + (hy - fy * v) * ky) / kk;
		if (!(u >= -kShaderUvTol && u <= 1.0f + kShaderUvTol && v >= -kShaderUvTol && v <= 1.0f + kShaderUvTol))
			continue;
		const float rx = uni.a[0] + ex * u + fx * v + gx * u * v - px;
		const float ry = uni.a[1] + ey * u + fy * v + gy * u * v - py;
		const float r = rx * rx + ry * ry;
		if (r < best_r) {
			best_r = r;
			bu = u;
			bv = v;
			ok = 1;
		}
	}
	return {bu, bv, ok};
}

std::array<float, 3> shader_inverse_projective(const ProjectiveUniforms &uni, float px, float py)
{
	const float qx = uni.row0[0] * px + uni.row0[1] * py + uni.row0[2];
	const float qy = uni.row1[0] * px + uni.row1[1] * py + uni.row1[2];
	const float qz = uni.row2[0] * px + uni.row2[1] * py + uni.row2[2];
	if (!(qz * uni.w_sign > 1e-7f))
		return {0, 0, 0};
	const float u = qx / qz, v = qy / qz;
	if (!(u >= -kShaderUvTol && u <= 1.0f + kShaderUvTol && v >= -kShaderUvTol && v <= 1.0f + kShaderUvTol))
		return {0, 0, 0};
	return {u, v, 1};
}

// ---- pre-warp ---------------------------------------------------------------------

void sanitize(PreWarp &p)
{
	if (!std::isfinite(p.rotation_deg))
		p.rotation_deg = 0.0;
	if (!std::isfinite(p.distortion))
		p.distortion = 0.0;
	p.rotation_deg = std::clamp(p.rotation_deg, -kRotationMax, kRotationMax);
	p.distortion = std::clamp(p.distortion, -kDistortionMax, kDistortionMax);
}

bool prewarp_identity(const PreWarp &p)
{
	return p.rotation_deg == 0.0 && p.distortion == 0.0;
}

double distortion_k(const PreWarp &p)
{
	return kDistortionK * p.distortion / kDistortionMax;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Unscaled sampling vector for pixel offset d (pixels from the centre).
Vec2 prewarp_vector(double k, double c, double sn, double inv_hd2, Vec2 d)
{
	const double r2 = (d.x * d.x + d.y * d.y) * inv_hd2;
	const Vec2 e = mul(d, 1.0 + k * r2);
	return {c * e.x + sn * e.y, -sn * e.x + c * e.y};
}

} // namespace

double prewarp_fill_scale(const PreWarp &p, double w, double h)
{
	if (!p.autofill || prewarp_identity(p) || !(w > 0) || !(h > 0))
		return 1.0;
	const double th = p.rotation_deg * kPi / 180.0;
	const double c = std::cos(th), sn = std::sin(th), k = distortion_k(p);
	const double inv_hd2 = 4.0 / (w * w + h * h);
	// The map is radial-monotonic (k > -1/3) composed with a rotation, so the frame is
	// covered iff its boundary is: the largest s that keeps every boundary sample inside.
	double s = 1e30;
	const int n = 512;
	for (int edge = 0; edge < 4; edge++)
		for (int i = 0; i <= n; i++) {
			const double t = (double)i / n;
			Vec2 d;
			switch (edge) {
			case 0:
				d = {(t - 0.5) * w, -0.5 * h};
				break;
			case 1:
				d = {0.5 * w, (t - 0.5) * h};
				break;
			case 2:
				d = {(0.5 - t) * w, 0.5 * h};
				break;
			default:
				d = {-0.5 * w, (0.5 - t) * h};
				break;
			}
			const Vec2 q = prewarp_vector(k, c, sn, inv_hd2, d);
			if (std::fabs(q.x) > 1e-12)
				s = std::min(s, 0.5 * w / std::fabs(q.x));
			if (std::fabs(q.y) > 1e-12)
				s = std::min(s, 0.5 * h / std::fabs(q.y));
		}
	// boundary sampled at 4 x 513 points: shrink by 1 ppm so the gaps between samples
	// cannot reach outside the source
	return s < 1e29 ? s * (1.0 - 1e-6) : 1.0;
}

bool prewarp_sample(const PreWarp &p, double w, double h, double u, double v, double &su, double &sv)
{
	const double th = p.rotation_deg * kPi / 180.0;
	const double inv_hd2 = 4.0 / (w * w + h * h);
	const double s = prewarp_fill_scale(p, w, h);
	const Vec2 q = mul(prewarp_vector(distortion_k(p), std::cos(th), std::sin(th), inv_hd2,
					  {(u - 0.5) * w, (v - 0.5) * h}),
			   s);
	su = q.x / w + 0.5;
	sv = q.y / h + 0.5;
	const double tol = 1e-9;
	return su >= -tol && su <= 1.0 + tol && sv >= -tol && sv <= 1.0 + tol;
}

PreWarpUniforms prewarp_uniforms(const PreWarp &p, double w, double h)
{
	PreWarpUniforms u{};
	const double th = p.rotation_deg * kPi / 180.0;
	u.on = prewarp_identity(p) ? 0.0f : 1.0f;
	u.cs[0] = (float)std::cos(th);
	u.cs[1] = (float)std::sin(th);
	u.k = (float)distortion_k(p);
	u.inv_hd2 = (float)(4.0 / (w * w + h * h));
	u.scale = (float)prewarp_fill_scale(p, w, h);
	u.size[0] = (float)w;
	u.size[1] = (float)h;
	return u;
}

std::array<float, 3> shader_prewarp(const PreWarpUniforms &uni, float u, float v)
{
	u = std::clamp(u, 0.0f, 1.0f); // the shader clamps the corner-pin result first
	v = std::clamp(v, 0.0f, 1.0f);
	const float dx = (u - 0.5f) * uni.size[0], dy = (v - 0.5f) * uni.size[1];
	const float r2 = (dx * dx + dy * dy) * uni.inv_hd2;
	const float f = (1.0f + uni.k * r2) * uni.scale;
	const float ex = dx * f, ey = dy * f;
	const float sx = uni.cs[0] * ex + uni.cs[1] * ey;
	const float sy = -uni.cs[1] * ex + uni.cs[0] * ey;
	const float qx = sx / uni.size[0] + 0.5f, qy = sy / uni.size[1] + 0.5f;
	if (!(qx >= -kShaderUvTol && qx <= 1.0f + kShaderUvTol && qy >= -kShaderUvTol && qy <= 1.0f + kShaderUvTol))
		return {0, 0, 0};
	return {qx, qy, 1};
}

} // namespace quad
} // namespace hdrtk
