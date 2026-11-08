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

#include "scale-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace scale {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kMaxTaps = 64;

double clampd(double v, double lo, double hi)
{
	return std::min(hi, std::max(lo, v));
}

float bc_cubic(float x, float b, float c)
{
	x = std::fabs(x);
	if (x < 1.0f)
		return ((12.0f - 9.0f * b - 6.0f * c) * x * x * x + (-18.0f + 12.0f * b + 6.0f * c) * x * x +
			(6.0f - 2.0f * b)) /
		       6.0f;
	if (x < 2.0f)
		return ((-b - 6.0f * c) * x * x * x + (6.0f * b + 30.0f * c) * x * x + (-12.0f * b - 48.0f * c) * x +
			(8.0f * b + 24.0f * c)) /
		       6.0f;
	return 0.0f;
}

void axis_map(double in_len, double out_len, double scale, double pan, AxisMap &a)
{
	const double scaled = in_len * scale;
	const double slack = std::max(0.0, (scaled - out_len) * 0.5);
	const double left = (out_len - scaled) * 0.5 - pan * slack;
	a.scale = scale;
	a.offset = -left / scale;
}

} // namespace

void sanitize(Params &p)
{
	p.out_w = std::max(0, std::min(p.out_w, 16384));
	p.out_h = std::max(0, std::min(p.out_h, 16384));
	if (p.fit < FitFill || p.fit > FitStretch)
		p.fit = FitFill;
	p.desqueeze = clampd(p.desqueeze, 0.25, 4.0);
	p.zoom = clampd(p.zoom, 1.0, 8.0);
	p.pan_x = clampd(p.pan_x, -1.0, 1.0);
	p.pan_y = clampd(p.pan_y, -1.0, 1.0);
	if (p.kernel < KernelLanczos3 || p.kernel > KernelBilinear)
		p.kernel = KernelLanczos3;
	p.antiring = clampd(p.antiring, 0.0, 1.0);
	if (p.domain < DomainAuto || p.domain > DomainPerceptual)
		p.domain = DomainAuto;
}

Layout compute_layout(uint32_t in_w, uint32_t in_h, const Params &p)
{
	Layout l;
	if (!in_w || !in_h)
		return l;
	const double eff_w = in_w * p.desqueeze, eff_h = in_h;
	double W = p.out_w, H = p.out_h;
	if (W <= 0 && H <= 0) {
		W = std::round(eff_w);
		H = eff_h;
	} else if (W <= 0) {
		W = std::round(H * eff_w / eff_h);
	} else if (H <= 0) {
		H = std::round(W * eff_h / eff_w);
	}
	l.out_w = (uint32_t)std::max(1.0, W);
	l.out_h = (uint32_t)std::max(1.0, H);
	double sx, sy;
	if (p.fit == FitStretch) {
		sx = l.out_w / (double)in_w * p.zoom;
		sy = l.out_h / (double)in_h * p.zoom;
	} else {
		const double fx = l.out_w / eff_w, fy = l.out_h / eff_h;
		const double s = (p.fit == FitFill ? std::max(fx, fy) : std::min(fx, fy)) * p.zoom;
		sx = s * p.desqueeze;
		sy = s;
	}
	axis_map(in_w, l.out_w, sx, p.pan_x, l.x);
	axis_map(in_h, l.out_h, sy, p.pan_y, l.y);
	return l;
}

bool axis_identity(const AxisMap &a)
{
	return std::fabs(a.scale - 1.0) < 1e-9 && std::fabs(a.offset - std::round(a.offset)) < 1e-9;
}

float kernel_radius(Kernel k)
{
	switch (k) {
	case KernelLanczos3:
		return 3.0f;
	case KernelCatmullRom:
	case KernelMitchell:
		return 2.0f;
	default:
		return 1.0f;
	}
}

float kernel_weight(Kernel k, float x)
{
	const float ax = std::fabs(x);
	switch (k) {
	case KernelLanczos3: {
		if (ax < 1e-5f)
			return 1.0f;
		if (ax >= 3.0f)
			return 0.0f;
		const float px = kPi * x;
		return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
	}
	case KernelCatmullRom:
		return bc_cubic(x, 0.0f, 0.5f);
	case KernelMitchell:
		return bc_cubic(x, 1.0f / 3.0f, 1.0f / 3.0f);
	default:
		return std::max(0.0f, 1.0f - ax);
	}
}

float pq_encode(float y)
{
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	const float p = std::pow(std::max(y, 0.0f), m1);
	return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float pq_decode(float e)
{
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	const float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
	return std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

AxisShader make_axis_shader(const AxisMap &a, const Params &p, uint32_t in_len, double nits_per_unit)
{
	AxisShader s;
	s.scale = (float)a.scale;
	s.offset = (float)a.offset;
	s.kernel = (int)p.kernel;
	const double ws = std::min(1.0, std::max(a.scale, 1.0 / 20.0));
	s.wscale = (float)ws;
	s.radius = (float)std::min(kernel_radius(p.kernel) / ws, (kMaxTaps - 1) * 0.5);
	const bool enlarging = a.scale > 1.0 + 1e-9;
	s.perceptual = p.domain == DomainPerceptual || (p.domain == DomainAuto && enlarging) ? 1 : 0;
	s.pq_k = (float)(nits_per_unit / 10000.0);
	s.antiring = enlarging ? (float)p.antiring : 0.0f; // reducing: keep the area average
	s.in_len = (float)in_len;
	return s;
}

namespace {

float enc(float v, const AxisShader &s)
{
	if (!s.perceptual)
		return v;
	const float e = pq_encode(std::fabs(v) * s.pq_k);
	return v < 0.0f ? -e : e;
}

float dec(float v, const AxisShader &s)
{
	if (!s.perceptual)
		return v;
	const float d = pq_decode(std::fabs(v)) / s.pq_k;
	return v < 0.0f ? -d : d;
}

} // namespace

void resample_f(const float (*line)[4], int n, int o, const AxisShader &s, float out[4])
{
	const float u = ((float)o + 0.5f) / s.scale + s.offset;
	if (u < 0.0f || u > s.in_len) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	const float t = u - 0.5f;
	const int first = (int)std::floor(t - s.radius) + 1;
	const int last = (int)std::floor(t + s.radius);
	float acc[4] = {0, 0, 0, 0};
	float wsum = 0.0f;
	for (int j = 0; j < kMaxTaps; j++) {
		const int i = first + j;
		if (i > last)
			break;
		const float w = kernel_weight((Kernel)s.kernel, (t - (float)i) * s.wscale);
		const int ic = std::min(n - 1, std::max(0, i));
		const float *px = line[ic];
		acc[0] += w * enc(px[0], s);
		acc[1] += w * enc(px[1], s);
		acc[2] += w * enc(px[2], s);
		acc[3] += w * px[3];
		wsum += w;
	}
	const float inv = wsum != 0.0f ? 1.0f / wsum : 0.0f;
	// anti-ringing: towards the range of the two nearest samples
	const int i0 = std::min(n - 1, std::max(0, (int)std::floor(t)));
	const int i1 = std::min(n - 1, std::max(0, (int)std::floor(t) + 1));
	for (int c = 0; c < 4; c++) {
		float r = acc[c] * inv;
		const float a = c < 3 ? enc(line[i0][c], s) : line[i0][c];
		const float b = c < 3 ? enc(line[i1][c], s) : line[i1][c];
		const float cl = std::min(std::max(r, std::min(a, b)), std::max(a, b));
		r = r + (cl - r) * s.antiring;
		out[c] = c < 3 ? dec(r, s) : r;
	}
}

} // namespace scale
} // namespace hdrtk
