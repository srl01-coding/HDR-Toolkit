// CPU tests for HDR Input (src/input/input-math.cpp). Build from the repo root:
//   g++ -O2 -std=c++17 -Isrc tests/cpu/test_input.cpp src/input/input-math.cpp
// Simulates libobs's capture paths (format_conversion.effect, color.effect) including the
// RGBA16F storage, then checks the reinterpretation and the bit-depth analysis.

#include "input/input-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace hdrtk::input;

static int failures = 0;
#define CHECK(c, ...)                                  \
	do {                                           \
		if (!(c)) {                            \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");             \
			failures++;                    \
		}                                      \
	} while (0)

static double half(double v) // round to IEEE half precision (normal range)
{
	if (v == 0.0 || !std::isfinite(v))
		return v;
	int e;
	const double m = std::frexp(v, &e);
	return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
}

static Vec3 half3(const Vec3 &v)
{
	return {half(v[0]), half(v[1]), half(v[2])};
}

// One camera pixel: Y' quantised to `bits` (limited range), padded into 10-bit codes.
struct Pix {
	double y, cb, cr; // normalised as libobs sees them after range expansion
};

static Pix camera_pixel(std::mt19937 &rng, int bits, double base)
{
	std::normal_distribution<double> N(0, 1);
	std::uniform_real_distribution<double> U(-1, 1);
	double y = std::clamp(base + 0.01 * N(rng), 0.0, 1.0);
	const double s = std::ldexp(1.0, bits - 8);
	const double code_b = std::round(16.0 * s + 219.0 * s * y);
	const double code10 = code_b * std::ldexp(1.0, 10 - bits);
	const double cb10 = std::round(512 + 896 * 0.08 * U(rng)), cr10 = std::round(512 + 896 * 0.08 * U(rng));
	return {(code10 - 64.0) / 876.0, (cb10 - 512.0) / 896.0, (cr10 - 512.0) / 896.0};
}

// What OBS delivers for that pixel.
static Vec3 obs_sdr(const Pix &p, Matrix m)
{
	const Vec3 e = mul(ycbcr_to_rgb(m), Vec3{p.y, p.cb, p.cr});
	return half3({srgb_decode(e[0]), srgb_decode(e[1]), srgb_decode(e[2])});
}

static Vec3 obs_hlg(const Pix &p, const HlgParams &h)
{
	const Vec3 e = mul(ycbcr_to_rgb(Bt2020), Vec3{p.y, p.cb, p.cr});
	Vec3 lin = hlg_to_linear(e, h.exponent);
	for (double &c : lin)
		c *= h.max_over_sdr;
	return half3(rec2020_to_rec709(lin));
}

static DepthResult analyse(int bits, Decoded d, Matrix m, const HlgParams &h, bool dither = false)
{
	std::mt19937 rng(bits * 7 + (int)d);
	std::uniform_real_distribution<double> U(0, 1);
	SignalAnalysis a;
	a.reset(true);
	for (int i = 0; i < 300000; i++) {
		Pix p = camera_pixel(rng, bits, 0.05 + 0.85 * U(rng));
		if (dither)
			p.y += (U(rng) - 0.5) / 876.0;
		const Vec3 v = d == Decoded::Hlg ? obs_hlg(p, h) : obs_sdr(p, m);
		a.add(recover_luma(d, m, h, v[0], v[1], v[2]));
	}
	return a.finish();
}

int main()
{
	const HlgParams h = hlg_params(203.0, 1000.0);
	CHECK(std::fabs(h.exponent - 0.2) < 1e-12 && std::fabs(h.max_over_sdr - 1000.0 / 203.0) < 1e-12, "hlg params");

	// 1. Reinterpretation equals OBS's own HLG decode of the same signal (DeckLink set to Rec. 2020),
	//    and with matrix correction when the source was decoded with Rec. 709.
	{
		std::mt19937 rng(3);
		double worst = 0, worst709 = 0;
		for (int i = 0; i < 20000; i++) {
			const Pix p = camera_pixel(rng, 10, std::uniform_real_distribution<double>(0, 1)(rng));
			const Vec3 ref = obs_hlg(p, h);
			const Vec3 got = reinterpret(obs_sdr(p, Bt2020), matrix_correction(Bt2020), h);
			const Vec3 got709 = reinterpret(obs_sdr(p, Bt709), matrix_correction(Bt709), h);
			// relative to the pixel's largest component: the 2020 -> 709 matrix subtracts large
			// terms, so a small output channel carries the absolute error of the large ones
			const double den =
				std::fmax(std::fabs(ref[0]), std::fmax(std::fabs(ref[1]), std::fabs(ref[2]))) + 1e-3;
			for (int c = 0; c < 3; c++) {
				worst = std::fmax(worst, std::fabs(got[c] - ref[c]) / den);
				worst709 = std::fmax(worst709, std::fabs(got709[c] - ref[c]) / den);
			}
		}
		std::printf(
			"reinterpret vs OBS HLG path: max relative error %.2g (Rec.2020 source), %.2g (Rec.709 source)\n",
			worst, worst709);
		CHECK(worst < 3e-3 && worst709 < 3e-3, "reinterpretation does not match OBS's HLG path");
	}
	// 2. Float mirror vs double.
	{
		const ShaderParams sp = make_shader_params(matrix_correction(Bt709), h);
		std::mt19937 rng(5);
		double worst = 0;
		for (int i = 0; i < 20000; i++) {
			const Pix p = camera_pixel(rng, 10, std::uniform_real_distribution<double>(0, 1)(rng));
			const Vec3 v = obs_sdr(p, Bt709);
			const Vec3 ref = reinterpret(v, matrix_correction(Bt709), h);
			const auto got = reinterpret_f(sp, (float)v[0], (float)v[1], (float)v[2]);
			for (int c = 0; c < 3; c++)
				worst = std::fmax(worst, std::fabs(got[c] - ref[c]) / (std::fabs(ref[c]) + 1e-3));
		}
		std::printf("float mirror: max relative error %.2g\n", worst);
		CHECK(worst < 1e-4, "float mirror");
	}
	// 3. Bit depth through each capture path, after RGBA16F storage.
	struct Case {
		const char *name;
		Decoded d;
		Matrix m;
	} cases[] = {{"SDR-decoded, Rec.2020 matrix", Decoded::SdrFloat, Bt2020},
		     {"SDR-decoded, Rec.709 matrix", Decoded::SdrFloat, Bt709},
		     {"HLG-decoded (Cam Link path)", Decoded::Hlg, Bt2020}};
	for (const Case &c : cases)
		for (int bits = 6; bits <= 10; bits++) {
			const DepthResult r = analyse(bits, c.d, c.m, h);
			std::printf("%-30s true %2d-bit -> detected %d   (", c.name, bits, r.bits);
			for (int n = 6; n <= 12; n++)
				std::printf("%d:%.2f ", n, r.coherence[n]);
			std::printf(")\n");
			CHECK(r.ok && r.bits == bits, "%s %d-bit", c.name, bits);
		}
	// 4. Dithered / re-processed signal: no grid.
	{
		const DepthResult r = analyse(10, Decoded::SdrFloat, Bt2020, h, true);
		std::printf("dithered: detected %d (10-bit coherence %.2f)\n", r.bits, r.coherence[10]);
		CHECK(r.ok && r.bits == 0, "dithered signal reported as quantised");
	}
	// 5. Too little variety: no verdict.
	{
		SignalAnalysis a;
		a.reset(true);
		for (int i = 0; i < 100000; i++)
			a.add((i % 10) / 876.0 + 0.3);
		const DepthResult r = a.finish();
		CHECK(!r.ok, "flat picture gave a verdict");
	}
	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
