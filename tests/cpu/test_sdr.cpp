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

// SDR Conversion CPU tests (docs/SDR_CONVERSION.md). Build:
//   g++ -std=c++17 -O2 -Isrc tests/cpu/test_sdr.cpp src/sdr/sdr-math.cpp src/input/input-math.cpp

#include "sdr/sdr-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace hdrtk;
using sdr::Vec3;

static int failures = 0;
#define CHECK(cond, ...)                               \
	do {                                           \
		if (!(cond)) {                         \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

// SDR signal (BT.709 E') that OBS outputs for OBS-linear v: srgb_encode(v)
static double signal(double v)
{
	return input::srgb_encode(v);
}

// OBS linear for an achromatic display luminance in nits
static Vec3 grey(double nits, double sdr_white)
{
	return {nits / sdr_white, nits / sdr_white, nits / sdr_white};
}

int main()
{
	// 1. Report BT.2446-1 worked numbers (100 nits from a 1000-nit master): Y' 0.3831,
	//    Y'p 0.6732, Y'c 0.7250, Y'sdr 0.5387.
	{
		const double rh = sdr::rho(1000), rs = sdr::rho(100);
		CHECK(std::fabs(rh - 13.2598) < 1e-3 && std::fabs(rs - 5.6970) < 1e-3, "rho %f %f", rh, rs);
		const double y = std::pow(0.1, 1 / 2.4);
		const double yp = std::log(1 + (rh - 1) * y) / std::log(rh);
		CHECK(std::fabs(y - 0.3831) < 1e-4 && std::fabs(yp - 0.6732) < 1e-4, "Y' %f Y'p %f", y, yp);
		const double ys = sdr::tone_luma(y, rh, rs);
		CHECK(std::fabs(ys - 0.5387) < 2e-4, "Y'sdr %f", ys);
	}
	// 2. Knee function continuity at both joints and end points.
	{
		const double a = 0.7399, b = 0.9909, d = 1e-9;
		// the published constants leave a step of 5.5e-4 at the first joint (1.077 x 0.7399 vs the quadratic)
		CHECK(std::fabs(sdr::tone_curve_yp(a) - sdr::tone_curve_yp(a + d)) < 6e-4, "joint 1");
		CHECK(std::fabs(sdr::tone_curve_yp(b - d) - sdr::tone_curve_yp(b)) < 2e-4, "joint 2");
		CHECK(std::fabs(sdr::tone_curve_yp(1.0) - 1.0) < 1e-12, "Y'p 1 -> 1");
		CHECK(sdr::tone_curve_yp(0.0) == 0.0, "0 -> 0");
	}
	// 3. Achromatic: neutral stays neutral, monotonic, black -> 0, peak -> SDR peak.
	sdr::Params p;
	for (double sw : {203.0, 300.0}) {
		p.sdr_white = sw;
		double prev = -1;
		bool mono = true, neutral = true;
		for (int i = 0; i <= 400; i++) {
			const double nits = 1000.0 * std::pow(10.0, -4.0 + 4.0 * i / 400.0);
			const Vec3 o = sdr::bt2446a(grey(nits, sw), p);
			neutral = neutral && std::fabs(o[0] - o[1]) < 1e-9 && std::fabs(o[1] - o[2]) < 1e-9;
			mono = mono && o[1] >= prev;
			prev = o[1];
		}
		CHECK(mono && neutral, "grey ramp: monotonic %d neutral %d (SDR white %.0f)", mono, neutral, sw);
		CHECK(sdr::bt2446a({0, 0, 0}, p)[1] == 0.0, "black");
		const double pk = signal(sdr::bt2446a(grey(1000, sw), p)[1]);
		CHECK(std::fabs(pk - 1.0) < 1e-6, "1000 nits -> SDR signal %f", pk);
	}
	// 4. Reference levels for the doc (SDR signal %), Lhdr 1000.
	p.sdr_white = 203;
	std::printf("BT.2446-1 Method A, Lhdr 1000, achromatic (SDR signal, %%):\n");
	for (double nits : {1.0, 5.0, 26.0, 58.0, 100.0, 203.0, 400.0, 600.0, 1000.0})
		std::printf("  %7.1f nits -> %5.1f%%   (OBS default %5.1f%% at SDR white 203)\n", nits,
			    100 * signal(sdr::bt2446a(grey(nits, 203), p)[1]),
			    100 * signal(sdr::obs_reinhard(grey(nits, 203))[1]));
	// 5. OBS default: SDR white -> 0.5 linear in 2.4 gamma, i.e. signal 0.5^(1/2.4).
	{
		const double s = signal(sdr::obs_reinhard({1, 1, 1})[1]);
		CHECK(std::fabs(s - std::pow(0.5, 1 / 2.4)) < 1e-6, "OBS default SDR white -> %f", s);
	}
	// 6. Float mirror (shader) vs double reference; outputs in range for random colours,
	//    including saturated BT.2020 colours (negative in 709) and super-whites.
	{
		std::mt19937 rng(7);
		std::uniform_real_distribution<double> u(0.0, 1.0);
		double maxerr = 0, maxerr_r = 0;
		bool range = true;
		for (double sw : {100.0, 203.0, 300.0}) {
			sdr::Params q;
			q.sdr_white = sw;
			const sdr::ShaderParams sp = sdr::make_shader_params(q);
			for (int i = 0; i < 200000; i++) {
				// random BT.2020 display light up to 1.5x the peak, converted to OBS 709
				const double s = 1500.0 / sw * std::pow(u(rng), 3.0);
				const Vec3 c2020 = {s * u(rng), s * u(rng), s * u(rng)};
				const Vec3 v = input::rec2020_to_rec709(c2020);
				const Vec3 r = sdr::bt2446a(v, q);
				const float vf[3] = {(float)v[0], (float)v[1], (float)v[2]};
				float of[3], orf[3];
				sdr::bt2446a_f(vf, sp, of);
				sdr::obs_reinhard_f(vf, orf);
				const Vec3 rr = sdr::obs_reinhard(v);
				for (int k = 0; k < 3; k++) {
					range = range && r[k] >= 0.0 && r[k] <= 1.0 && of[k] >= 0.0f && of[k] <= 1.0f;
					maxerr = std::max(maxerr, std::fabs(signal(r[k]) - signal(of[k])));
					maxerr_r = std::max(maxerr_r, std::fabs(signal(std::max(0.0, rr[k])) -
										signal(std::max(0.0f, orf[k]))));
				}
			}
		}
		std::printf("float mirror: max SDR signal error %.2e (Method A), %.2e (OBS default)\n", maxerr,
			    maxerr_r);
		CHECK(range, "output outside 0..1");
		CHECK(maxerr < 2e-4 && maxerr_r < 2e-4, "mirror error %g %g", maxerr, maxerr_r);
	}
	// 7. Hue direction kept for a moderately saturated colour (no channel order swap).
	{
		p.sdr_white = 203;
		const Vec3 o = sdr::bt2446a({1.5, 0.6, 0.3}, p);
		CHECK(o[0] > o[1] && o[1] > o[2], "orange keeps channel order: %f %f %f", o[0], o[1], o[2]);
	}
	std::printf(failures ? "%d FAILURES\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
