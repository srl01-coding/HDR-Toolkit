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

// SDR in HDR CPU tests (docs/SDR_IN_HDR.md). Build:
//   g++ -std=c++17 -O2 -Isrc tests/cpu/test_sdrhdr.cpp src/sdrhdr/sdrhdr-math.cpp src/input/input-math.cpp

#include "sdrhdr/sdrhdr-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace hdrtk;
using sdrhdr::Vec3;

static int failures = 0;
#define CHECK(cond, ...)                               \
	do {                                           \
		if (!(cond)) {                         \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

int main()
{
	sdrhdr::Params p;
	p.obs_sdr_white = 300.0;
	// 1. HLG levels of the reference whites (1000-nit display).
	{
		const double h203 = sdrhdr::nits_to_hlg(203.0, 1000.0);
		const double h1000 = sdrhdr::nits_to_hlg(1000.0, 1000.0);
		std::printf("HLG signal: 203 nits %.2f%%, 300 nits %.2f%%, 1000 nits %.2f%%\n", 100 * h203,
			    100 * sdrhdr::nits_to_hlg(300.0, 1000.0), 100 * h1000);
		CHECK(std::fabs(h203 - 0.75) < 0.003 && std::fabs(h1000 - 1.0) < 1e-6, "HLG levels %f %f", h203, h1000);
	}
	// 2. Graphics: SDR white -> white_nits exactly (OBS units white / OBS SDR white), linear.
	{
		const Vec3 w = sdrhdr::convert({1, 1, 1}, p);
		CHECK(std::fabs(w[1] * 300.0 - 203.0) < 1e-9, "graphics white %f nits", w[1] * 300.0);
		const Vec3 g = sdrhdr::convert({0.2, 0.5, 0.8}, p);
		CHECK(std::fabs(g[0] / 0.2 - g[2] / 0.8) < 1e-12, "graphics is a pure gain");
	}
	// 3. Video, no expansion: BT.1886 display of the signal at white_nits; SDR 100 % -> 203.
	{
		p.mode = sdrhdr::ModeVideo;
		const Vec3 w = sdrhdr::convert({1, 1, 1}, p);
		CHECK(std::fabs(w[1] * 300.0 - 203.0) < 1e-9, "video white");
		const double v18 = input::srgb_decode(0.5); // signal 50 %
		const Vec3 m = sdrhdr::convert({v18, v18, v18}, p);
		const double expect = std::pow(0.5, 2.4) * 203.0;
		CHECK(std::fabs(m[1] * 300.0 - expect) < 1e-9, "video mid %f vs %f", m[1] * 300.0, expect);
		std::printf("video: SDR 50%% signal -> %.1f nits (graphics mode %.1f nits)\n", expect,
			    input::srgb_decode(0.5) * 203.0);
	}
	// 4. Expansion: SDR 100 % -> peak, unchanged below the knee, C1 at the knee, monotonic.
	{
		p.peak_nits = 600.0;
		p.knee = 0.75;
		sdrhdr::sanitize(p);
		const Vec3 w = sdrhdr::convert({1, 1, 1}, p);
		CHECK(std::fabs(w[1] * 300.0 - 600.0) < 1e-6, "expanded white %f", w[1] * 300.0);
		const double vk = input::srgb_decode(0.7);
		CHECK(std::fabs(sdrhdr::convert({vk, vk, vk}, p)[1] * 300.0 - std::pow(0.7, 2.4) * 203.0) < 1e-9,
		      "below knee unchanged");
		auto L = [&](double s) {
			const double v = input::srgb_decode(s);
			return sdrhdr::convert({v, v, v}, p)[1];
		};
		const double h = 1e-6, k = 0.75;
		const double dl = (L(k) - L(k - h)) / h, dr = (L(k + h) - L(k)) / h;
		CHECK(std::fabs(dl / dr - 1.0) < 1e-3, "C1 at knee %f %f", dl, dr);
		bool mono = true;
		for (int i = 1; i <= 1000; i++)
			mono = mono && L(i / 1000.0) >= L((i - 1) / 1000.0);
		CHECK(mono, "expansion not monotonic");
		// ratio preserved for a coloured highlight
		const Vec3 c = sdrhdr::convert(
			{input::srgb_decode(0.95), input::srgb_decode(0.6), input::srgb_decode(0.3)}, p);
		const Vec3 c0 = {std::pow(0.95, 2.4), std::pow(0.6, 2.4), std::pow(0.3, 2.4)};
		CHECK(std::fabs(c[0] / c[1] - c0[0] / c0[1]) < 1e-9 && std::fabs(c[1] / c[2] - c0[1] / c0[2]) < 1e-9,
		      "hue ratios");
	}
	// 5. Float mirror vs double reference, premultiplied alpha.
	{
		std::mt19937 rng(11);
		std::uniform_real_distribution<double> u(0.0, 1.0);
		double maxerr = 0;
		for (int mode : {0, 1})
			for (double peak : {203.0, 600.0, 1000.0}) {
				sdrhdr::Params q;
				q.mode = mode;
				q.peak_nits = peak;
				q.obs_sdr_white = 300.0;
				sdrhdr::sanitize(q);
				const sdrhdr::ShaderParams sp = sdrhdr::make_shader_params(q);
				for (int i = 0; i < 100000; i++) {
					const Vec3 v = {u(rng), u(rng), u(rng)};
					const double a = i % 4 == 0 ? u(rng) : 1.0;
					const Vec3 r = sdrhdr::convert(v, q);
					const float in[4] = {(float)(v[0] * a), (float)(v[1] * a), (float)(v[2] * a),
							     (float)a};
					float o[4];
					sdrhdr::place_f(in, sp, o);
					for (int k = 0; k < 3; k++) {
						const double ref = r[k] * a;
						maxerr = std::max(maxerr, std::fabs(o[k] - ref) / std::max(1e-3, ref));
					}
					CHECK(o[3] == (float)a, "alpha");
				}
			}
		std::printf("float mirror: max relative error %.2e\n", maxerr);
		// in_scale: scRGB input (1.0 = 80 nits) equals SDR-white units after scaling
		sdrhdr::Params q;
		q.obs_sdr_white = 203.0;
		const sdrhdr::ShaderParams a1 = sdrhdr::make_shader_params(q, 1.0f);
		const sdrhdr::ShaderParams a2 = sdrhdr::make_shader_params(q, 80.0f / 203.0f);
		const float w1[4] = {1, 1, 1, 1}, w2[4] = {203.0f / 80.0f, 203.0f / 80.0f, 203.0f / 80.0f, 1};
		float o1[4], o2[4];
		sdrhdr::place_f(w1, a1, o1);
		sdrhdr::place_f(w2, a2, o2);
		CHECK(std::fabs(o1[0] - o2[0]) < 1e-6f, "in_scale scRGB %f %f", o1[0], o2[0]);
		CHECK(maxerr < 2e-4, "mirror error %g", maxerr);
	}
	std::printf(failures ? "%d FAILURES\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
