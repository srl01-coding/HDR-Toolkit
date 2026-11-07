// CPU tests for HDR Sharpen (src/sharpen/sharpen-math.cpp). Build from the repo root:
//   g++ -O2 -std=c++17 -Isrc tests/cpu/test_sharpen.cpp src/sharpen/sharpen-math.cpp

#include "sharpen/sharpen-math.hpp"

#include <cmath>
#include <cstdio>
#include <random>

using namespace hdrtk::sharpen;

static int failures = 0;
#define CHECK(c, ...)                                  \
	do {                                           \
		if (!(c)) {                            \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

static std::vector<RgbaF> to_f(const Image &im)
{
	std::vector<RgbaF> v;
	for (const Rgba &p : im.px)
		v.push_back({(float)p.r, (float)p.g, (float)p.b, (float)p.a});
	return v;
}

static double luma(const Rgba &c)
{
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

// Horizontal soft step from lo to hi nits (working units at npu), opaque.
static Image edge(int w, int h, double lo, double hi, double npu, double softness)
{
	Image im;
	im.w = w;
	im.h = h;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const double t = 1.0 / (1.0 + std::exp(-(x - w / 2 + 0.5) / softness));
			const double v = (lo + (hi - lo) * t) / npu;
			im.px.push_back({v, v, v, 1.0});
		}
	return im;
}

int main()
{
	const double npu = 203.0;
	std::mt19937 rng(11);
	std::uniform_real_distribution<double> U(0, 1);

	// random HDR image: values up to 10x SDR white, some negative channels, mixed alpha
	Image rnd;
	rnd.w = 48;
	rnd.h = 32;
	for (int i = 0; i < rnd.w * rnd.h; i++) {
		const double a = i % 17 == 0 ? 0.0 : (i % 11 == 0 ? 0.4 : 1.0);
		const double base = std::exp2(U(rng) * 14 - 8);
		Rgba p{base * (0.7 + 0.6 * U(rng)), base, base * (0.5 + U(rng)), a};
		if (i % 23 == 0)
			p.r = -0.05 * base;
		p.r *= a;
		p.g *= a;
		p.b *= a;
		rnd.px.push_back(p);
	}

	// 1. Neutral: amount 0 is bit-exact through the float mirror (the shader path).
	{
		Params p;
		const auto in = to_f(rnd);
		std::vector<RgbaF> out;
		sharpen_f(make_shader_params(p, npu), rnd.w, rnd.h, in, out);
		bool exact = true;
		for (size_t i = 0; i < in.size(); i++)
			exact = exact && out[i].r == in[i].r && out[i].g == in[i].g && out[i].b == in[i].b &&
				out[i].a == in[i].a;
		CHECK(exact, "amount 0 not bit-exact");
	}
	// 2. Flat field: no change at full amount.
	{
		Params p;
		p.amount = 3;
		p.threshold = 0;
		Image flat = edge(40, 8, 120, 120, npu, 1.0), o;
		sharpen(p, npu, flat, o);
		double worst = 0;
		for (size_t i = 0; i < o.px.size(); i++)
			worst = std::fmax(worst, std::fabs(o.px[i].g / flat.px[i].g - 1));
		CHECK(worst < 1e-9, "flat field changed (%.3g)", worst);
	}
	// 3. Sharpening steepens a soft edge, and the effect in stops is brightness-invariant
	//    well above the floor (same edge at 10x the level).
	{
		Params p;
		p.amount = 1.0;
		p.radius = 2.0;
		p.threshold = 0;
		p.floor_nits = 0.01;
		Image a = edge(64, 4, 20, 40, npu, 2.0), b = edge(64, 4, 200, 400, npu, 2.0), oa, ob;
		sharpen(p, npu, a, oa);
		sharpen(p, npu, b, ob);
		double worst = 0, over = 0;
		for (int x = 0; x < 64; x++) {
			const double sa = std::log2(oa.at(x, 1).g / a.at(x, 1).g);
			const double sb = std::log2(ob.at(x, 1).g / b.at(x, 1).g);
			worst = std::fmax(worst, std::fabs(sa - sb));
			over = std::fmax(over, sa);
		}
		const double before = a.at(34, 1).g - a.at(29, 1).g, after = oa.at(34, 1).g - oa.at(29, 1).g;
		std::printf("edge 20->40 nits: contrast across the edge %.4f -> %.4f, max overshoot %.3f stops; "
			    "x10 level max difference %.2g stops\n",
			    before, after, over, worst);
		CHECK(after > before * 1.1, "edge not steepened");
		CHECK(worst < 1e-3, "not brightness-invariant (%.3g stops)", worst);
	}
	// 4. Halo limit: a 1 -> 1000 nit edge at maximum amount stays within +/- limit stops.
	{
		Params p;
		p.amount = 3;
		p.radius = 3;
		p.limit = 0.5;
		Image a = edge(64, 4, 1, 1000, npu, 0.5), o;
		sharpen(p, npu, a, o);
		const double k = p.floor_nits / npu;
		double worst = 0;
		for (size_t i = 0; i < o.px.size(); i++)
			worst = std::fmax(worst, std::fabs(std::log2((o.px[i].g + k) / (a.px[i].g + k))));
		std::printf("1->1000 nit edge, amount 300%%: largest change %.4f stops (limit %.2f)\n", worst, p.limit);
		CHECK(worst <= p.limit + 1e-9, "halo exceeds limit");
	}
	// 5. HDR safety: finite, alpha unchanged, no sign flips, nothing clamped at 1.0.
	{
		Params p;
		p.amount = 3;
		p.radius = 4;
		Image o;
		sharpen(p, npu, rnd, o);
		bool ok = true, above_one = false;
		const double k = p.floor_nits / npu;
		for (size_t i = 0; i < o.px.size(); i++) {
			const Rgba &c = rnd.px[i], &r = o.px[i];
			ok = ok && std::isfinite(r.r) && std::isfinite(r.g) && std::isfinite(r.b) && r.a == c.a;
			ok = ok && ((c.g + c.a * k) >= 0) == ((r.g + r.a * k) >= 0);
			above_one = above_one || r.g > 1.5;
		}
		CHECK(ok && above_one, "HDR safety");
	}
	// 6. Transparent neighbours do not create a halo: opaque block next to alpha 0.
	{
		Params p;
		p.amount = 3;
		p.radius = 3;
		p.threshold = 0;
		Image im;
		im.w = 32;
		im.h = 4;
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 32; x++)
				im.px.push_back(x < 16 ? Rgba{0.6, 0.6, 0.6, 1.0} : Rgba{0, 0, 0, 0});
		Image o;
		sharpen(p, npu, im, o);
		double worst = 0;
		for (size_t i = 0; i < o.px.size(); i++)
			worst = std::fmax(worst, std::fabs(o.px[i].g - im.px[i].g));
		CHECK(worst < 1e-9, "alpha edge halo (%.3g)", worst);
	}
	// 7. Threshold cores small detail; transfer is odd and monotonic.
	{
		Params p;
		p.amount = 1;
		p.threshold = 0.04;
		const double small = detail_transfer(p, 0.01), big = detail_transfer(p, 0.2);
		CHECK(small < 0.1 * 0.01 && big > 0.15, "coring (%.4g, %.4g)", small, big);
		bool mono = true;
		double prev = -1e9;
		for (double d = -1; d <= 1; d += 0.001) {
			const double e = detail_transfer(p, d);
			mono = mono && e >= prev && std::fabs(e + detail_transfer(p, -d)) < 1e-12;
			prev = e;
		}
		CHECK(mono, "transfer not monotonic / odd");
	}
	// 8. Float mirror vs double reference.
	{
		Params p;
		p.amount = 1.7;
		p.radius = 2.3;
		p.threshold = 0.03;
		p.limit = 0.7;
		p.floor_nits = 3;
		Image o;
		sharpen(p, npu, rnd, o);
		std::vector<RgbaF> of;
		sharpen_f(make_shader_params(p, npu), rnd.w, rnd.h, to_f(rnd), of);
		double worst = 0;
		const double k = p.floor_nits / npu;
		for (size_t i = 0; i < o.px.size(); i++) {
			if (rnd.px[i].a <= 0)
				continue;
			const double ref = luma(o.px[i]) + o.px[i].a * k;
			const double got = 0.2126 * of[i].r + 0.7152 * of[i].g + 0.0722 * of[i].b + of[i].a * k;
			worst = std::fmax(worst, std::fabs(std::log2(got / ref)));
		}
		std::printf("float mirror vs double: max %.2g stops\n", worst);
		CHECK(worst < 1e-4, "float mirror");
	}
	// 9. Sanitize.
	{
		Params p{NAN, 100, -1, 0, 1e9};
		sanitize(p);
		CHECK(p.amount == 0 && p.radius == kRadiusMax && p.threshold == 0 && p.limit == kLimitMin &&
			      p.floor_nits == kFloorMax,
		      "sanitize");
	}
	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
