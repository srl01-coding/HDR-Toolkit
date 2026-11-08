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

#include "warp-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace quad {

namespace {

float lanczos3(float x)
{
	const float ax = std::fabs(x);
	if (ax < 0.00001f)
		return 1.0f;
	if (ax >= 3.0f)
		return 0.0f;
	const float px = 3.14159265358979f * x;
	return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
}

float snap(float v)
{
	const float r = std::round(v);
	return std::fabs(v - r) < 0.001f ? r : v;
}

float enc1(float v, float k)
{
	const float e = shader_pq_encode(std::fabs(v) * k);
	return v < 0.0f ? -e : e;
}

float dec1(float v, float k)
{
	const float d = shader_pq_decode(std::fabs(v)) / k;
	return v < 0.0f ? -d : d;
}

float smoothstep(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

std::array<float, 3> full_map(const WarpShader &s, std::array<float, 3> m)
{
	if (m[2] < 0.5f || s.pre.on < 0.5f)
		return m;
	return shader_prewarp(s.pre, std::clamp(m[0], 0.0f, 1.0f), std::clamp(m[1], 0.0f, 1.0f));
}

} // namespace

float shader_pq_encode(float y)
{
	const float p = std::pow(std::max(y, 0.0f), 0.1593017578125f);
	return std::pow((0.8359375f + 18.8515625f * p) / (1.0f + 18.6875f * p), 78.84375f);
}

float shader_pq_decode(float e)
{
	const float p = std::pow(std::max(e, 0.0f), 1.0f / 78.84375f);
	return std::pow(std::max(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p), 1.0f / 0.1593017578125f);
}

std::array<float, 3> shader_map_px(const WarpShader &s, float cx, float cy)
{
	const float fx = (cx / s.res_scale[0] + s.res_offset[0]) / s.in_size[0];
	const float fy = (cy / s.res_scale[1] + s.res_offset[1]) / s.in_size[1];
	const std::array<float, 3> m = s.warp == 1 ? shader_inverse_projective(s.proj, fx, fy)
						   : shader_inverse_bilinear(s.bil, fx, fy);
	return full_map(s, m);
}

Image shader_encode_pq(const Image &in, float pq_k)
{
	Image o = in;
	for (size_t i = 0; i < o.px.size(); i += 4)
		for (int c = 0; c < 3; c++)
			o.px[i + (size_t)c] = enc1(in.px[i + (size_t)c], pq_k);
	return o;
}

std::array<float, 4> shader_sample_hq(const WarpShader &s, const Image &img, const Image &pq, int ox, int oy)
{
	const float cx = (float)ox + 0.5f, cy = (float)oy + 0.5f;
	const std::array<float, 3> m0 = shader_map_px(s, cx, cy);
	if (m0[2] < 0.5f)
		return {0, 0, 0, 0};
	std::array<float, 3> mx = shader_map_px(s, cx + 1.0f, cy);
	float sgx = 1.0f;
	if (mx[2] < 0.5f) {
		mx = shader_map_px(s, cx - 1.0f, cy);
		sgx = -1.0f;
	}
	std::array<float, 3> my = shader_map_px(s, cx, cy + 1.0f);
	float sgy = 1.0f;
	if (my[2] < 0.5f) {
		my = shader_map_px(s, cx, cy - 1.0f);
		sgy = -1.0f;
	}
	float jx[2], jy[2];
	if (mx[2] > 0.5f) {
		jx[0] = (mx[0] - m0[0]) * s.in_size[0] * sgx;
		jx[1] = (mx[1] - m0[1]) * s.in_size[1] * sgx;
	} else {
		jx[0] = 1.0f / s.res_scale[0];
		jx[1] = 0.0f;
	}
	if (my[2] > 0.5f) {
		jy[0] = (my[0] - m0[0]) * s.in_size[0] * sgy;
		jy[1] = (my[1] - m0[1]) * s.in_size[1] * sgy;
	} else {
		jy[0] = 0.0f;
		jy[1] = 1.0f / s.res_scale[1];
	}
	const float ma = jx[0] * jx[0] + jy[0] * jy[0];
	const float mb = jx[0] * jx[1] + jy[0] * jy[1];
	const float md = jx[1] * jx[1] + jy[1] * jy[1];
	const float tr = 0.5f * (ma + md);
	const float dt = std::sqrt(std::max(0.25f * (ma - md) * (ma - md) + mb * mb, 0.0f));
	const float s1 = std::sqrt(std::max(tr + dt, 0.0f));
	const float s2 = std::sqrt(std::max(tr - dt, 0.0f));
	float e1[2];
	if (std::fabs(mb) > 0.000001f) {
		const float vx = tr + dt - md, vy = mb;
		const float n = std::sqrt(vx * vx + vy * vy);
		e1[0] = vx / n;
		e1[1] = vy / n;
	} else if (ma >= md) {
		e1[0] = 1.0f;
		e1[1] = 0.0f;
	} else {
		e1[0] = 0.0f;
		e1[1] = 1.0f;
	}
	const float e2[2] = {-e1[1], e1[0]};
	const float w1 = s1 < 1.001f ? 1.0f : std::min(s1, 2.0f);
	const float w2 = s2 < 1.001f ? 1.0f : std::min(s2, 2.0f);
	const float fi00 = e1[0] * e1[0] / w1 + e2[0] * e2[0] / w2;
	const float fi01 = e1[0] * e1[1] / w1 + e2[0] * e2[1] / w2;
	const float fi11 = e1[1] * e1[1] / w1 + e2[1] * e2[1] / w2;
	const float f00 = e1[0] * e1[0] * w1 + e2[0] * e2[0] * w2;
	const float f01 = e1[0] * e1[1] * w1 + e2[0] * e2[1] * w2;
	const float f11 = e1[1] * e1[1] * w1 + e2[1] * e2[1] * w2;
	const float tdom = smoothstep(1.001f, 1.5f, s1);

	float qx = snap(m0[0] * s.in_size[0] - 0.5f), qy = snap(m0[1] * s.in_size[1] - 0.5f);
	const int nw = (int)s.in_size[0], nh = (int)s.in_size[1];
	auto cl = [](int v, int n) {
		return std::min(n - 1, std::max(0, v));
	};
	if (qx == std::round(qx) && qy == std::round(qy) && w1 == 1.0f && w2 == 1.0f) {
		const float *t = img.at(cl((int)qx, nw), cl((int)qy, nh));
		return {t[0], t[1], t[2], t[3]};
	}
	const float ex = 3.0f * (std::fabs(f00) + std::fabs(f01));
	const float ey = 3.0f * (std::fabs(f01) + std::fabs(f11));
	const int x0 = (int)std::floor(qx - ex) + 1;
	const int y0 = (int)std::floor(qy - ey) + 1;
	const int nx = std::min((int)std::floor(qx + ex) - x0 + 1, 20);
	const int ny = std::min((int)std::floor(qy + ey) - y0 + 1, 20);
	const int count = nx * ny;
	float accl[4] = {0, 0, 0, 0}, accp[3] = {0, 0, 0}, wsum = 0.0f;
	for (int k = 0; k < 400; k++) {
		if (k >= count)
			break;
		const int i = x0 + k % nx, j = y0 + k / nx;
		const float dx0 = qx - (float)i, dy0 = qy - (float)j;
		const float dx = fi00 * dx0 + fi01 * dy0;
		const float dy = fi01 * dx0 + fi11 * dy0;
		const float w = lanczos3(dx) * lanczos3(dy);
		const float *a = img.at(cl(i, nw), cl(j, nh));
		const float *b = pq.at(cl(i, nw), cl(j, nh));
		for (int c = 0; c < 4; c++)
			accl[c] += w * a[c];
		for (int c = 0; c < 3; c++)
			accp[c] += w * b[c];
		wsum += w;
	}
	const float inv = wsum != 0.0f ? 1.0f / wsum : 0.0f;
	const int ix = (int)std::floor(qx), iy = (int)std::floor(qy);
	const float *n00 = pq.at(cl(ix, nw), cl(iy, nh)), *n10 = pq.at(cl(ix + 1, nw), cl(iy, nh)),
		    *n01 = pq.at(cl(ix, nw), cl(iy + 1, nh)), *n11 = pq.at(cl(ix + 1, nw), cl(iy + 1, nh));
	std::array<float, 4> out;
	for (int c = 0; c < 3; c++) {
		float rp = accp[c] * inv;
		const float lo = std::min(std::min(n00[c], n10[c]), std::min(n01[c], n11[c]));
		const float hi = std::max(std::max(n00[c], n10[c]), std::max(n01[c], n11[c]));
		rp = rp + (std::clamp(rp, lo, hi) - rp) * s.antiring;
		const float dp = dec1(rp, s.pq_k);
		const float rl = accl[c] * inv;
		out[(size_t)c] = dp + (rl - dp) * tdom;
	}
	out[3] = std::clamp(accl[3] * inv, 0.0f, 1.0f);
	return out;
}

std::array<float, 4> shader_sample_linear(const WarpShader &s, const Image &img, int ox, int oy)
{
	const std::array<float, 3> m = shader_map_px(s, (float)ox + 0.5f, (float)oy + 0.5f);
	if (m[2] < 0.5f)
		return {0, 0, 0, 0};
	// hardware bilinear with clamp addressing
	const float u = std::clamp(m[0], 0.0f, 1.0f) * s.in_size[0] - 0.5f;
	const float v = std::clamp(m[1], 0.0f, 1.0f) * s.in_size[1] - 0.5f;
	const int x = (int)std::floor(u), y = (int)std::floor(v);
	const float fx = u - (float)x, fy = v - (float)y;
	const int nw = img.w, nh = img.h;
	auto cl = [](int t, int n) {
		return std::min(n - 1, std::max(0, t));
	};
	std::array<float, 4> o;
	for (int c = 0; c < 4; c++) {
		const float a = img.at(cl(x, nw), cl(y, nh))[c], b = img.at(cl(x + 1, nw), cl(y, nh))[c];
		const float cc = img.at(cl(x, nw), cl(y + 1, nh))[c], d = img.at(cl(x + 1, nw), cl(y + 1, nh))[c];
		o[(size_t)c] = (a + (b - a) * fx) + ((cc + (d - cc) * fx) - (a + (b - a) * fx)) * fy;
	}
	return o;
}

} // namespace quad
} // namespace hdrtk
