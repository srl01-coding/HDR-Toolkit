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

// HDR Scale CPU tests (docs/HDR_SCALE.md). Build:
//   g++ -std=c++17 -O2 -Isrc tests/cpu/test_scale.cpp src/scale/scale-math.cpp

#include "scale/scale-math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace hdrtk::scale;

static int failures = 0;
#define CHECK(cond, ...)                               \
	do {                                           \
		if (!(cond)) {                         \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

static double half(double v) // IEEE half rounding (normal range)
{
	if (v == 0.0 || !std::isfinite(v))
		return v;
	int e;
	const double m = std::frexp(v, &e);
	return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
}

using Line = std::vector<std::array<float, 4>>;

static std::vector<std::array<float, 4>> run(const std::vector<std::array<float, 4>> &in, int out_n,
					     const AxisShader &s)
{
	std::vector<std::array<float, 4>> out((size_t)out_n);
	for (int o = 0; o < out_n; o++)
		resample_f(reinterpret_cast<const float(*)[4]>(in.data()), (int)in.size(), o, s, out[(size_t)o].data());
	return out;
}

static std::vector<std::array<float, 4>> grey_line(const std::vector<float> &v)
{
	std::vector<std::array<float, 4>> l;
	for (float x : v)
		l.push_back({x, x, x, 1.0f});
	return l;
}

int main()
{
	const double npu = 203.0; // OBS linear 1.0 = SDR white

	// 1. Layout for the church canvas 4560 x 2160 (19:9).
	{
		Params p;
		p.out_w = 4560;
		p.out_h = 2160;
		Layout l = compute_layout(3840, 2160, p); // 16:9 camera, Fill
		CHECK(l.out_w == 4560 && l.out_h == 2160, "size");
		CHECK(std::fabs(l.x.scale - 1.1875) < 1e-12 && std::fabs(l.y.scale - 1.1875) < 1e-12,
		      "16:9 scale %f %f", l.x.scale, l.y.scale);
		// vertical crop is centred: output row 0 samples input row (2565 - 2160) / 2 / 1.1875
		const double u0 = 0.5 / l.y.scale + l.y.offset;
		CHECK(std::fabs(u0 - (202.5 + 0.5) / 1.1875) < 1e-9, "centred crop u0 %f", u0);
		p.desqueeze = 1.25; // anamorphic 1.25x
		l = compute_layout(3840, 2160, p);
		CHECK(std::fabs(l.x.scale - 1.25) < 1e-12 && std::fabs(l.y.scale - 1.0) < 1e-12,
		      "anamorphic scale %f %f", l.x.scale, l.y.scale);
		CHECK(axis_identity(l.y), "anamorphic vertical is identity");
		// pan +1 shows the right edge: last output column samples inside the last input texel
		p.pan_x = 1.0;
		l = compute_layout(3840, 2160, p);
		const double ulast = (4559 + 0.5) / l.x.scale + l.x.offset;
		CHECK(std::fabs(ulast - (3840 - 0.5 / 1.25)) < 1e-9, "pan right edge %f", ulast);
		// Fit: 16:9 into 19:9 leaves borders left/right
		Params f;
		f.out_w = 4560;
		f.out_h = 2160;
		f.fit = FitFit;
		l = compute_layout(3840, 2160, f);
		CHECK(std::fabs(l.x.scale - 1.0) < 1e-12 && l.x.offset < 0, "fit borders");
		// defaults: same size, identity on both axes
		l = compute_layout(1920, 1080, Params());
		CHECK(l.out_w == 1920 && l.out_h == 1080 && axis_identity(l.x) && axis_identity(l.y),
		      "default identity");
		// zoom 2 punch-in on 4K into 4K
		Params z;
		z.zoom = 2.0;
		l = compute_layout(3840, 2160, z);
		CHECK(std::fabs(l.x.scale - 2.0) < 1e-12 && std::fabs((0.5 / 2.0 + l.x.offset) - 960.25) < 1e-9,
		      "zoom 2 centre crop");
	}
	// 2. Kernels: interpolating at integers, normalised (partition of unity).
	for (Kernel k : {KernelLanczos3, KernelCatmullRom, KernelBilinear}) {
		CHECK(std::fabs(kernel_weight(k, 0.0f) - 1.0f) < 1e-6, "k(0)");
		for (int i = 1; i <= 3; i++)
			CHECK(std::fabs(kernel_weight(k, (float)i)) < 1e-6, "k(%d)=%g kernel %d", i,
			      kernel_weight(k, (float)i), (int)k);
	}
	{
		double worst = 0;
		for (Kernel k : {KernelLanczos3, KernelCatmullRom, KernelMitchell, KernelBilinear})
			for (int j = 0; j < 100; j++) {
				const float f = j / 100.0f;
				double s = 0;
				for (int i = -4; i <= 4; i++)
					s += kernel_weight(k, f - i);
				if (k != KernelLanczos3)
					worst = std::max(worst, std::fabs(s - 1.0));
			}
		CHECK(worst < 1e-5, "cubic/bilinear partition of unity %g", worst);
	}
	// 3. PQ round trip.
	{
		double e = 0;
		for (float nits : {0.005f, 0.1f, 1.0f, 26.0f, 203.0f, 1000.0f, 4000.0f}) {
			const float y = nits / 10000.0f;
			e = std::max(e, std::fabs(pq_decode(pq_encode(y)) / y - 1.0));
		}
		CHECK(e < 2e-4, "PQ round trip %g", e);
	}
	// 4. Identity at scale 1 is exact after RGBA16F storage (forced shader path, both domains).
	{
		Params p;
		Layout l = compute_layout(64, 1, p);
		std::vector<float> v;
		for (int i = 0; i < 64; i++)
			v.push_back((float)half(std::pow(2.0, -8.0 + i * 0.25))); // 0.004 .. 280 x SDR white
		const Line in = grey_line(v);
		bool exact = true;
		for (Domain d : {DomainLinear, DomainPerceptual}) {
			p.domain = d;
			const AxisShader s = make_axis_shader(l.x, p, 64, npu);
			const Line out = run(in, 64, s);
			for (int i = 0; i < 64; i++)
				exact = exact && half(out[(size_t)i][0]) == v[(size_t)i] && out[(size_t)i][3] == 1.0f;
		}
		CHECK(exact, "identity not exact after half rounding");
	}
	// 5. Constant field stays constant (DC), including HDR levels and when reducing.
	{
		Params p;
		for (double sc : {1.1875, 1.25, 2.0, 0.8, 0.5}) {
			AxisMap a;
			a.scale = sc;
			a.offset = 0;
			const AxisShader s = make_axis_shader(a, p, 100, npu);
			const Line out = run(grey_line(std::vector<float>(100, 4.926f)), (int)(100 * sc), s);
			double e = 0;
			for (size_t i = 1; i + 1 < out.size(); i++)
				e = std::max(e, std::fabs(out[i][0] / 4.926 - 1.0));
			CHECK(e < 1e-4, "DC at scale %.3f: %g", sc, e);
		}
	}
	// 6. Ringing on an HDR edge (wall 26 nits next to a 1000-nit light), x1.1875 Lanczos-3:
	//    report undershoot on the dark side, linear vs perceptual vs anti-ringing.
	{
		std::vector<float> v(60, 26.0f / 203.0f);
		for (int i = 30; i < 60; i++)
			v[(size_t)i] = 1000.0f / 203.0f;
		const Line in = grey_line(v);
		AxisMap a;
		a.scale = 1.1875;
		a.offset = 0.137; // arbitrary phase
		auto dark_min = [&](Domain d, double ar) {
			Params p;
			p.domain = d;
			p.antiring = ar;
			const AxisShader s = make_axis_shader(a, p, 60, npu);
			const Line out = run(in, 71, s);
			double lo = 1e9, hi = -1e9;
			for (const auto &px : out) {
				lo = std::min(lo, (double)px[0]);
				hi = std::max(hi, (double)px[0]);
			}
			return std::make_pair(lo * 203.0, hi * 203.0);
		};
		const auto lin = dark_min(DomainLinear, 0.0), per = dark_min(DomainPerceptual, 0.0),
			   ar = dark_min(DomainPerceptual, 0.8), ar1 = dark_min(DomainPerceptual, 1.0);
		std::printf("26|1000-nit edge x1.1875 Lanczos-3: min / max nits\n");
		std::printf("  linear, no anti-ringing:     %8.2f / %8.1f\n", lin.first, lin.second);
		std::printf("  perceptual, no anti-ringing: %8.2f / %8.1f\n", per.first, per.second);
		std::printf("  perceptual, anti-ring 0.8:   %8.2f / %8.1f\n", ar.first, ar.second);
		std::printf("  perceptual, anti-ring 1.0:   %8.2f / %8.1f\n", ar1.first, ar1.second);
		CHECK(lin.first < 0.0, "linear Lanczos should undershoot below black on this edge (got %f)", lin.first);
		CHECK(per.first > 13.0, "perceptual undershoot more than a stop: %f nits",
		      per.first); // linear goes negative
		CHECK(ar1.first >= 26.0 * (1 - 5e-4) && ar1.second <= 1000.0 * (1 + 5e-4),
		      "anti-ring 1.0 must stay in range (PQ round trip tolerance): %f %f", ar1.first, ar1.second);
		CHECK(ar.first > 22.0 && ar.second < 1100.0, "default anti-ring 0.8: %f %f", ar.first, ar.second);
	}
	// 7. Reducing (0.8x, anamorphic alternative): a 1-pixel stripe pattern (Nyquist) is averaged,
	//    not aliased: output near the mean.
	{
		std::vector<float> v;
		for (int i = 0; i < 200; i++)
			v.push_back(i % 2 ? 2.0f : 0.0f);
		AxisMap a;
		a.scale = 0.8;
		a.offset = 0;
		Params p;
		const AxisShader s = make_axis_shader(a, p, 200, npu);
		const Line out = run(grey_line(v), 160, s);
		double dev = 0;
		for (size_t i = 5; i + 5 < out.size(); i++)
			dev = std::max(dev, std::fabs(out[i][0] - 1.0));
		std::printf("Nyquist stripes reduced x0.8: max deviation from mean %.3f (of 1.0)\n", dev);
		CHECK(dev < 0.25, "aliasing on reduction: %f", dev);
		CHECK(s.perceptual == 0 && s.antiring == 0.0f, "reducing must be linear without anti-ringing");
	}
	// 8. Fit borders are transparent; alpha resampled.
	{
		AxisMap a;
		a.scale = 1.0;
		a.offset = -10.0;
		const AxisShader s = make_axis_shader(a, Params(), 20, npu);
		const Line out = run(grey_line(std::vector<float>(20, 1.0f)), 40, s);
		CHECK(out[0][3] == 0.0f && out[5][3] == 0.0f && out[15][3] == 1.0f && out[35][3] == 0.0f, "borders");
	}
	std::printf(failures ? "%d FAILURES\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
