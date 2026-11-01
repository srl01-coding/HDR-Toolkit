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

#include "noise-profile.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hdrtk {
namespace denoise {

void sanitize(NoiseProfile &p)
{
	p.n = std::clamp(p.n, 0, kProfileAnchors);
	for (int i = 0; i < p.n; i++)
		if (!std::isfinite(p.l[i]) || !std::isfinite(p.m[i])) {
			p.n = 0;
			break;
		}
	// insertion sort by l
	for (int i = 1; i < p.n; i++)
		for (int j = i; j > 0 && p.l[j] < p.l[j - 1]; j--) {
			std::swap(p.l[j], p.l[j - 1]);
			std::swap(p.m[j], p.m[j - 1]);
		}
	for (int i = 1; i < p.n; i++)
		if (p.l[i] < p.l[i - 1] + 0.01)
			p.l[i] = p.l[i - 1] + 0.01;
	p.min_mult = std::isfinite(p.min_mult) ? std::clamp(p.min_mult, 0.1, 1.0) : 0.5;
	p.max_mult = std::isfinite(p.max_mult) ? std::clamp(p.max_mult, 1.0, 16.0) : 3.0;
}

NoiseProfile builtin_profile(int id, double max_mult)
{
	NoiseProfile p;
	p.max_mult = max_mult;
	if (id == ProfileMeasured20261005) {
		// docs/denoise/NOISE_PROFILE.md: temporal sigma in the comparison domain F at four
		// static ROIs (codes 184 / 442 / 544 / 598 -> 2.27 / 35.6 / 64.4 / 89.4 nits),
		// relative to the wall (64.4 nits).
		const double nits[4] = {2.27, 35.6, 64.4, 89.4};
		const double mult[4] = {4.67, 1.30, 1.00, 0.84};
		p.n = 4;
		for (int i = 0; i < 4; i++) {
			p.l[i] = std::log2(nits[i]);
			p.m[i] = mult[i];
		}
	}
	sanitize(p);
	return p;
}

// Same structure as the shader: the last segment whose start lies below l wins, with t
// clamped to [0, 1]; below the first anchor the first multiplier applies.
double profile_multiplier(const NoiseProfile &p, double y)
{
	if (p.n <= 0)
		return 1.0;
	const double l = std::log2(std::max(std::fabs(y), kProfileFloorNits));
	double m = p.m[0];
	for (int i = 0; i + 1 < p.n; i++)
		if (l > p.l[i]) {
			const double t = std::clamp((l - p.l[i]) / (p.l[i + 1] - p.l[i]), 0.0, 1.0);
			m = p.m[i] + t * (p.m[i + 1] - p.m[i]);
		}
	return std::clamp(m, p.min_mult, p.max_mult);
}

ShaderProfile make_shader_profile(const NoiseProfile &p)
{
	ShaderProfile s;
	s.count = (float)p.n;
	for (int i = 0; i < kProfileAnchors; i++) {
		s.a[i][0] = (float)p.l[i];
		s.a[i][1] = (float)p.m[i];
	}
	s.min_mult = (float)p.min_mult;
	s.max_mult = (float)p.max_mult;
	return s;
}

float profile_multiplier_f(const ShaderProfile &s, float y)
{
	if (s.count < 0.5f)
		return 1.0f;
	const float l = std::log2(std::max(std::fabs(y), (float)kProfileFloorNits));
	const int n = (int)(s.count + 0.5f);
	float m = s.a[0][1];
	for (int i = 0; i < kProfileAnchors - 1; i++)
		if (i + 1 < n && l > s.a[i][0]) {
			const float t = std::clamp((l - s.a[i][0]) / (s.a[i + 1][0] - s.a[i][0]), 0.0f, 1.0f);
			m = s.a[i][1] + t * (s.a[i + 1][1] - s.a[i][1]);
		}
	return std::clamp(m, s.min_mult, s.max_mult);
}

} // namespace denoise
} // namespace hdrtk
