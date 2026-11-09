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

// HDR Transform schema 3 CPU tests: output size + High quality sampling (warp-math.cpp,
// mirror of hdr-transform.effect). Build:
//   g++ -std=c++17 -O2 -Isrc tests/cpu/test_warp.cpp src/transform/warp-math.cpp
//       src/transform/quad-math.cpp src/scale/scale-math.cpp

#include "transform/warp-math.hpp"
#include "scale/scale-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>

using namespace hdrtk::quad;

static int failures = 0;
#define CHECK(cond, ...)                               \
	do {                                           \
		if (!(cond)) {                         \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

static double half(double v)
{
	if (v == 0.0 || !std::isfinite(v))
		return v;
	int e;
	const double m = std::frexp(v, &e);
	return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
}

static const float kPqK = 203.0f / 10000.0f;

static Image make(int w, int h, const std::function<float(double, double)> &f)
{
	Image im;
	im.w = w;
	im.h = h;
	im.px.resize((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const float v = f(x + 0.5, y + 0.5);
			float *p = &im.px[((size_t)y * w + x) * 4];
			p[0] = p[1] = p[2] = v;
			p[3] = 1.0f;
		}
	return im;
}

static WarpShader shader(int in_w, int in_h, const Quad &q, bool projective, const PreWarp &pre,
			 const hdrtk::scale::Params &rp)
{
	WarpShader s;
	const hdrtk::scale::Layout l = hdrtk::scale::compute_layout(in_w, in_h, rp);
	s.out_size[0] = (float)l.out_w;
	s.out_size[1] = (float)l.out_h;
	s.in_size[0] = (float)in_w;
	s.in_size[1] = (float)in_h;
	s.res_scale[0] = (float)l.x.scale;
	s.res_scale[1] = (float)l.y.scale;
	s.res_offset[0] = (float)l.x.offset;
	s.res_offset[1] = (float)l.y.offset;
	s.warp = projective ? 1 : 0;
	s.bil = bilinear_uniforms(q);
	projective_uniforms(q, s.proj);
	s.pre = prewarp_uniforms(pre, in_w, in_h);
	s.antiring = 0.8f;
	s.pq_k = kPqK;
	return s;
}

// RMS error of HQ and of bilinear against an analytic image f evaluated at the exact mapped point.
struct Err {
	double hq = 0, lin = 0, bic = 0;
};
static Err compare(const WarpShader &s, const Image &img, const std::function<float(double, double)> &f, int margin)
{
	const Image pq = shader_encode_pq(img, s.pq_k);
	double eh = 0, el = 0, eb = 0;
	int n = 0;
	for (int y = margin; y < (int)s.out_size[1] - margin; y++)
		for (int x = margin; x < (int)s.out_size[0] - margin; x++) {
			const auto m = shader_map_px(s, x + 0.5f, y + 0.5f);
			if (m[2] < 0.5f)
				continue;
			const double ref = f(m[0] * s.in_size[0], m[1] * s.in_size[1]);
			const auto h = shader_sample_hq(s, img, pq, x, y);
			const auto l = shader_sample_linear(s, img, x, y);
			const auto b = shader_sample_bicubic(s, img, x, y);
			eb += (b[0] - ref) * (b[0] - ref);
			eh += (h[0] - ref) * (h[0] - ref);
			el += (l[0] - ref) * (l[0] - ref);
			n++;
		}
	return {std::sqrt(eh / n), std::sqrt(el / n), std::sqrt(eb / n)};
}

int main()
{
	const hdrtk::scale::Params no_resize;
	// 1. Identity is exact (HQ, both warp models, forced render), on an HDR image.
	{
		std::mt19937 rng(3);
		std::uniform_real_distribution<double> u(-8.0, 3.0);
		Image img = make(48, 27, [&](double, double) { return (float)half(std::pow(2.0, u(rng))); });
		bool exact = true;
		for (bool proj : {false, true}) {
			const WarpShader s = shader(48, 27, Quad(), proj, PreWarp(), no_resize);
			const Image pq = shader_encode_pq(img, s.pq_k);
			for (int y = 0; y < 27; y++)
				for (int x = 0; x < 48; x++)
					exact = exact && shader_sample_hq(s, img, pq, x, y)[0] == img.at(x, y)[0];
		}
		CHECK(exact, "HQ identity not exact");
		bool bexact = true;
		const WarpShader sb = shader(48, 27, Quad(), false, PreWarp(), no_resize);
		for (int y = 0; y < 27; y++)
			for (int x = 0; x < 48; x++)
				bexact = bexact && shader_sample_bicubic(sb, img, x, y)[0] == img.at(x, y)[0];
		CHECK(bexact, "bicubic identity not exact");
	}
	// 2. Constant HDR field stays constant under rotation + keystone + resize.
	{
		Image img = make(96, 54, [](double, double) { return 4.926f; });
		PreWarp pre;
		pre.rotation_deg = 7.0;
		Quad q;
		q.a = {0.04, 0.0};
		q.b = {0.96, 0.02};
		hdrtk::scale::Params rp;
		rp.out_w = 114;
		rp.out_h = 54;
		const WarpShader s = shader(96, 54, q, false, pre, rp);
		const Image pq = shader_encode_pq(img, s.pq_k);
		double e = 0;
		for (int y = 6; y < 48; y++)
			for (int x = 6; x < 108; x++) {
				const auto h = shader_sample_hq(s, img, pq, x, y);
				if (h[3] > 0.999f)
					e = std::max(e, std::fabs(h[0] / 4.926 - 1.0));
			}
		double eb = 0;
		for (int y = 6; y < 48; y++)
			for (int x = 6; x < 108; x++) {
				const auto b = shader_sample_bicubic(s, img, x, y);
				if (b[3] > 0.999f)
					eb = std::max(eb, std::fabs(b[0] / 4.926 - 1.0));
			}
		std::printf("constant 1000-nit field: max error HQ %.2e (PQ copy in half precision), bicubic %.2e\n", e,
			    eb);
		CHECK(e < 3e-3 && eb < 1e-5, "constant field error %g %g", e, eb);
	}
	// 3. Sharpness and accuracy: a smooth band-limited image (period 5 px), rotated 5 deg and
	//    resized x1.1875 (the church canvas), HQ vs bilinear against the exact image.
	{
		auto f = [](double x, double y) {
			return (float)(1.0 + 0.5 * std::sin(2 * M_PI * x / 5.0) * std::cos(2 * M_PI * y / 7.0));
		};
		Image img = make(96, 54, f);
		PreWarp pre;
		pre.rotation_deg = 5.0;
		pre.autofill = true;
		hdrtk::scale::Params rp;
		rp.out_w = 114;
		rp.out_h = 54;
		const WarpShader s = shader(96, 54, Quad(), false, pre, rp);
		WarpShader sl = s;
		const Err e = compare(sl, img, f, 8);
		std::printf(
			"period-5 pattern, rotate 5 deg + x1.1875: RMS error HQ %.4f, bicubic %.4f, bilinear %.4f (amplitude "
			"0.5)\n",
			e.hq, e.bic, e.lin);
		CHECK(e.bic < 0.75 * e.lin && e.bic > e.hq, "bicubic should sit between: %f", e.bic);
		CHECK(e.hq < 0.5 * e.lin, "HQ not clearly more accurate than bilinear: %f vs %f", e.hq, e.lin);
		// keystone only, near 1:1
		Quad q;
		q.a = {0.03, 0.0};
		q.b = {0.97, 0.0};
		const WarpShader s2 = shader(96, 54, q, true, PreWarp(), no_resize);
		const Err e2 = compare(s2, img, f, 8);
		std::printf(
			"period-5 pattern, keystone 3%% (projective): RMS error HQ %.4f, bicubic %.4f, bilinear %.4f\n",
			e2.hq, e2.bic, e2.lin);
		CHECK(e2.hq < 0.5 * e2.lin, "keystone: HQ %f vs bilinear %f", e2.hq, e2.lin);
	}
	// 4. Reducing: corner pin to 50 % on 1-pixel stripes (Nyquist): HQ averages, bilinear aliases.
	{
		Image img = make(96, 54, [](double x, double) { return ((int)x % 2) ? 2.0f : 0.0f; });
		Quad q;
		q.a = {0.275, 0.275}; // 45 %: a non-integer ratio, so bilinear cannot land between stripes
		q.b = {0.725, 0.275};
		q.c = {0.725, 0.725};
		q.d = {0.275, 0.725};
		const WarpShader s = shader(96, 54, q, false, PreWarp(), no_resize);
		const Image pq = shader_encode_pq(img, s.pq_k);
		double dh = 0, dl = 0;
		for (int y = 18; y < 36; y++)
			for (int x = 28; x < 68; x++) {
				dh = std::max(dh, std::fabs(shader_sample_hq(s, img, pq, x, y)[0] - 1.0));
				dl = std::max(dl, std::fabs(shader_sample_linear(s, img, x, y)[0] - 1.0));
			}
		std::printf("stripes reduced to 45%%: max deviation from mean HQ %.3f, bilinear %.3f\n", dh, dl);
		CHECK(dh < 0.2 && dl > 0.5, "reduction: HQ %f bilinear %f", dh, dl);
	}
	// 5. HDR edge (26 | 1000 nits) rotated 3 deg: no black halo (min stays near the wall level).
	{
		Image img = make(96, 54, [](double x, double) { return (float)((x < 48 ? 26.0 : 1000.0) / 203.0); });
		PreWarp pre;
		pre.rotation_deg = 3.0;
		const WarpShader s = shader(96, 54, Quad(), false, pre, no_resize);
		const Image pq = shader_encode_pq(img, s.pq_k);
		double lo = 1e9, hi = 0;
		for (int y = 10; y < 44; y++)
			for (int x = 10; x < 86; x++) {
				const auto h = shader_sample_hq(s, img, pq, x, y);
				if (h[3] < 0.999f)
					continue;
				lo = std::min(lo, (double)h[0] * 203.0);
				hi = std::max(hi, (double)h[0] * 203.0);
			}
		std::printf("26|1000-nit edge rotated 3 deg (HQ): min %.2f, max %.1f nits\n", lo, hi);
		for (float ar : {0.8f, 1.0f}) {
			WarpShader sb = s;
			sb.antiring = ar;
			double blo = 1e9, bhi = 0;
			for (int y = 10; y < 44; y++)
				for (int x = 10; x < 86; x++) {
					const auto b = shader_sample_bicubic(sb, img, x, y);
					blo = std::min(blo, (double)b[0] * 203.0);
					bhi = std::max(bhi, (double)b[0] * 203.0);
				}
			std::printf("  bicubic, anti-ringing %.0f%%: min %.2f, max %.1f nits\n", 100.0 * ar, blo, bhi);
			if (ar == 1.0f)
				CHECK(blo >= 26.0 * (1 - 1e-5) && bhi <= 1000.0 * (1 + 1e-5),
				      "bicubic full anti-ring range");
			else
				CHECK(blo > 0.0, "bicubic at 80%% goes negative: %f", blo);
		}
		CHECK(lo > 20.0 && hi < 1100.0, "edge ringing %f %f", lo, hi);
	}
	// 6. Outside the quad: transparent; Fit borders transparent.
	{
		Image img = make(48, 27, [](double, double) { return 1.0f; });
		Quad q;
		q.a = {0.25, 0.0};
		q.d = {0.25, 1.0};
		const WarpShader s = shader(48, 27, q, false, PreWarp(), no_resize);
		const Image pq = shader_encode_pq(img, s.pq_k);
		const float a_out = shader_sample_hq(s, img, pq, 2, 10)[3],
			    a_in = shader_sample_hq(s, img, pq, 40, 10)[3];
		CHECK(a_out == 0.0f && a_in > 0.99999f, "outside quad: %g inside: %g", a_out, a_in);
		hdrtk::scale::Params rp;
		rp.out_w = 57;
		rp.out_h = 27;
		rp.fit = hdrtk::scale::FitFit;
		const WarpShader s2 = shader(48, 27, Quad(), false, PreWarp(), rp);
		CHECK(shader_sample_hq(s2, img, pq, 1, 10)[3] == 0.0f &&
			      shader_sample_hq(s2, img, pq, 28, 10)[3] > 0.99999f,
		      "fit borders");
	}
	std::printf(failures ? "%d FAILURES\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
