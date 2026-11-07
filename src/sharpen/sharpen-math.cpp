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

#include "sharpen-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace sharpen {

namespace {

template<typename T> void clampv(T &v, T lo, T hi, T fallback)
{
	if (!std::isfinite(v))
		v = fallback;
	v = std::clamp(v, lo, hi);
}

constexpr double kLr = 0.2126, kLg = 0.7152, kLb = 0.0722;

double luma_f(const Rgba &c, double k)
{
	double y = kLr * c.r + kLg * c.g + kLb * c.b;
	y = c.a > 0.0 ? y / c.a : 0.0;
	return std::log2(std::max(y, 0.0) + k);
}

double soft_limit(double x)
{
	return x / std::sqrt(1.0 + x * x);
}

} // namespace

void sanitize(Params &p)
{
	clampv(p.amount, 0.0, kAmountMax, 0.0);
	clampv(p.radius, kRadiusMin, kRadiusMax, 1.0);
	clampv(p.threshold, 0.0, kThresholdMax, 0.02);
	clampv(p.limit, kLimitMin, kLimitMax, 0.5);
	clampv(p.floor_nits, kFloorMin, kFloorMax, 2.0);
}

bool neutral(const Params &p)
{
	return p.amount <= 0.0;
}

int taps_for(double radius)
{
	return std::clamp((int)std::ceil(3.0 * radius), 1, kMaxTaps);
}

double detail_transfer(const Params &p, double d)
{
	const double t2 = p.threshold * p.threshold;
	const double c = d * d * d / (d * d + t2 + 1e-20);
	return p.limit * soft_limit(p.amount * c / p.limit);
}

void sharpen(const Params &p, double npu, const Image &in, Image &out)
{
	const int w = in.w, h = in.h, taps = taps_for(p.radius);
	const double k = p.floor_nits / npu;
	const double inv_2s2 = 1.0 / (2.0 * p.radius * p.radius);
	std::vector<double> hf((size_t)w * h), ha((size_t)w * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double sf = 0, sa = 0;
			for (int i = -taps; i <= taps; i++) {
				const Rgba &c = in.at(std::clamp(x + i, 0, w - 1), y);
				const double wt = std::exp(-(double)(i * i) * inv_2s2);
				sf += wt * luma_f(c, k) * c.a;
				sa += wt * c.a;
			}
			hf[(size_t)y * w + x] = sf;
			ha[(size_t)y * w + x] = sa;
		}
	out = in;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double sf = 0, sa = 0;
			for (int i = -taps; i <= taps; i++) {
				const size_t j = (size_t)std::clamp(y + i, 0, h - 1) * w + x;
				const double wt = std::exp(-(double)(i * i) * inv_2s2);
				sf += wt * hf[j];
				sa += wt * ha[j];
			}
			const Rgba &c = in.at(x, y);
			double e = 0.0;
			if (sa > 1e-6 && c.a > 0.0)
				e = detail_transfer(p, luma_f(c, k) - sf / sa);
			const double g = std::exp2(e) - 1.0;
			Rgba &o = out.px[(size_t)y * w + x];
			o.r = c.r + (c.r + c.a * k) * g;
			o.g = c.g + (c.g + c.a * k) * g;
			o.b = c.b + (c.b + c.a * k) * g;
			o.a = c.a;
		}
}

ShaderParams make_shader_params(const Params &p, double npu)
{
	ShaderParams s{};
	s.k_units = (float)(p.floor_nits / npu);
	s.taps = (float)taps_for(p.radius);
	s.inv_2s2 = (float)(1.0 / (2.0 * p.radius * p.radius));
	s.amount = (float)p.amount;
	s.limit = (float)p.limit;
	s.thresh2 = (float)(p.threshold * p.threshold);
	return s;
}

namespace {

float luma_ff(const RgbaF &c, float k)
{
	float y = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	y = c.a > 0.0f ? y / c.a : 0.0f;
	return std::log2(std::max(y, 0.0f) + k);
}

} // namespace

void sharpen_f(const ShaderParams &s, int w, int h, const std::vector<RgbaF> &in, std::vector<RgbaF> &out)
{
	std::vector<float> bf((size_t)w * h), ba((size_t)w * h);
	// pass BlurH
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float af = 0.0f, aa = 0.0f;
			for (int i = -kMaxTaps; i <= kMaxTaps; i++) {
				if (std::fabs((float)i) > s.taps)
					continue;
				const RgbaF &c = in[(size_t)y * w + (size_t)std::clamp(x + i, 0, w - 1)];
				const float wt = std::exp(-(float)(i * i) * s.inv_2s2);
				af += wt * (luma_ff(c, s.k_units) * c.a);
				aa += wt * c.a;
			}
			bf[(size_t)y * w + x] = af;
			ba[(size_t)y * w + x] = aa;
		}
	// pass Combine (vertical blur + gain)
	out.resize(in.size());
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float af = 0.0f, aa = 0.0f;
			for (int i = -kMaxTaps; i <= kMaxTaps; i++) {
				if (std::fabs((float)i) > s.taps)
					continue;
				const size_t j = (size_t)std::clamp(y + i, 0, h - 1) * w + x;
				const float wt = std::exp(-(float)(i * i) * s.inv_2s2);
				af += wt * bf[j];
				aa += wt * ba[j];
			}
			const RgbaF &c = in[(size_t)y * w + x];
			float e = 0.0f;
			if (aa > 1e-6f && c.a > 0.0f) {
				const float d = luma_ff(c, s.k_units) - af / aa;
				const float cd = d * d * d / (d * d + s.thresh2 + 1e-20f);
				const float x2 = s.amount * cd / s.limit;
				e = s.limit * (x2 / std::sqrt(1.0f + x2 * x2));
			}
			const float g = std::exp2(e) - 1.0f;
			RgbaF &o = out[(size_t)y * w + x];
			o.r = c.r + (c.r + c.a * s.k_units) * g;
			o.g = c.g + (c.g + c.a * s.k_units) * g;
			o.b = c.b + (c.b + c.a * s.k_units) * g;
			o.a = c.a;
		}
}

} // namespace sharpen
} // namespace hdrtk
