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

#include "sdrhdr-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace sdrhdr {

namespace {

float srgb_encode_f(float v)
{
	return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Video expansion on the BT.1886 SDR signal of max(R, G, B): returns the factor that
// multiplies the BT.2408 display light.
double expansion(double s, double knee, double expand)
{
	if (expand <= 0.0 || s <= knee)
		return 1.0;
	const double t = (s - knee) / (1.0 - knee);
	// L(s) = s^2.4 (1 + expand t^2): C1 at the knee (t = 0, slope of t^2 is 0),
	// monotonic for expand >= 0, L(1) = 1 + expand
	return 1.0 + expand * t * t;
}

} // namespace

void sanitize(Params &p)
{
	p.mode = p.mode == ModeVideo ? ModeVideo : ModeGraphics;
	p.white_nits = std::clamp(p.white_nits, 20.0, 1000.0);
	p.peak_nits = std::clamp(p.peak_nits, p.white_nits, 4000.0);
	p.knee = std::clamp(p.knee, 0.5, 0.95);
	p.obs_sdr_white = std::clamp(p.obs_sdr_white, 80.0, 1000.0);
}

double nits_to_hlg(double nits, double lw)
{
	if (!(nits > 0.0))
		return 0.0;
	const double gamma = 1.2 + 0.42 * std::log10(lw / 1000.0);
	const double scene = std::pow(nits / lw, 1.0 / gamma);
	return input::hlg_oetf_channel(scene);
}

Vec3 convert(const Vec3 &v, const Params &p)
{
	const double gain = p.white_nits / p.obs_sdr_white;
	if (p.mode == ModeGraphics)
		return {v[0] * gain, v[1] * gain, v[2] * gain};
	Vec3 e, l;
	for (int i = 0; i < 3; i++) {
		e[i] = input::srgb_encode(std::max(0.0, v[i]));
		l[i] = std::pow(e[i], 2.4);
	}
	const double m = std::max(e[0], std::max(e[1], e[2]));
	const double f = expansion(m, p.knee, (p.peak_nits - p.white_nits) / p.white_nits);
	return {l[0] * gain * f, l[1] * gain * f, l[2] * gain * f};
}

ShaderParams make_shader_params(const Params &p)
{
	ShaderParams s;
	s.mode = p.mode;
	s.gain = (float)(p.white_nits / p.obs_sdr_white);
	s.knee = (float)p.knee;
	s.expand = (float)std::max(0.0, (p.peak_nits - p.white_nits) / p.white_nits);
	return s;
}

void place_f(const float in[4], const ShaderParams &sp, float out[4])
{
	const float a = in[3];
	if (sp.mode == ModeGraphics) {
		for (int i = 0; i < 3; i++)
			out[i] = in[i] * sp.gain;
		out[3] = a;
		return;
	}
	if (!(a > 0.0f)) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	float e[3];
	for (int i = 0; i < 3; i++)
		e[i] = srgb_encode_f(std::max(in[i] / a, 0.0f));
	const float m = std::max(e[0], std::max(e[1], e[2]));
	float f = 1.0f;
	if (sp.expand > 0.0f && m > sp.knee) {
		const float t = (m - sp.knee) / (1.0f - sp.knee);
		f = 1.0f + sp.expand * t * t;
	}
	for (int i = 0; i < 3; i++)
		out[i] = std::pow(e[i], 2.4f) * sp.gain * f * a;
	out[3] = a;
}

} // namespace sdrhdr
} // namespace hdrtk
