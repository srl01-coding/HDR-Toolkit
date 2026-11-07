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

#include "input-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hdrtk {
namespace input {

namespace {

constexpr double kPi = 3.14159265358979323846;

void kr_kb(Matrix m, double &kr, double &kb)
{
	switch (m) {
	case Bt601:
		kr = 0.299;
		kb = 0.114;
		break;
	case Bt709:
		kr = 0.2126;
		kb = 0.0722;
		break;
	default:
		kr = 0.2627;
		kb = 0.0593;
		break;
	}
}

} // namespace

Vec3 luma_row(Matrix m)
{
	double kr, kb;
	kr_kb(m, kr, kb);
	return {kr, 1.0 - kr - kb, kb};
}

Mat3 ycbcr_to_rgb(Matrix m)
{
	double kr, kb;
	kr_kb(m, kr, kb);
	const double kg = 1.0 - kr - kb;
	const double rcr = 2.0 * (1.0 - kr), bcb = 2.0 * (1.0 - kb);
	return Mat3{Vec3{1.0, 0.0, rcr}, Vec3{1.0, -kb * bcb / kg, -kr * rcr / kg}, Vec3{1.0, bcb, 0.0}};
}

Mat3 rgb_to_ycbcr(Matrix m)
{
	double kr, kb;
	kr_kb(m, kr, kb);
	const double kg = 1.0 - kr - kb;
	const double sb = 0.5 / (1.0 - kb), sr = 0.5 / (1.0 - kr);
	return Mat3{Vec3{kr, kg, kb}, Vec3{-kr * sb, -kg * sb, (1.0 - kb) * sb},
		    Vec3{(1.0 - kr) * sr, -kg * sr, -kb * sr}};
}

Mat3 mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 r{};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
	return r;
}

Vec3 mul(const Mat3 &a, const Vec3 &v)
{
	return {a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2], a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2],
		a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2]};
}

Mat3 matrix_correction(Matrix decoded_with)
{
	if (decoded_with == Bt2020)
		return Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
	return mul(ycbcr_to_rgb(Bt2020), rgb_to_ycbcr(decoded_with));
}

double srgb_encode(double u)
{
	return (u <= 0.0031308) ? (12.92 * u) : ((1.055 * std::pow(u, 1.0 / 2.4)) - 0.055);
}

double srgb_decode(double u)
{
	return (u <= 0.04045) ? (u / 12.92) : std::pow(u * (1.0 / 1.055) + 0.055 / 1.055, 2.4);
}

namespace {
const double kLn2i = 1.0 / std::log(2.0);
const double kHlgM = kLn2i / 0.17883277;
const double kHlgA = -kLn2i * 0.55991073 / 0.17883277;
const Vec3 kYs2020 = {0.2627, 0.678, 0.0593}; // OBS hlg_to_linear
} // namespace

double hlg_eotf_channel(double u)
{
	return (u <= 0.5) ? ((u * u) / 3.0) : ((std::exp2(u * kHlgM + kHlgA) + 0.28466892) / 12.0);
}

double hlg_oetf_channel(double s)
{
	if (s <= 1.0 / 12.0)
		return std::sqrt(3.0 * std::max(s, 0.0));
	return (std::log2(12.0 * s - 0.28466892) - kHlgA) / kHlgM;
}

Vec3 hlg_to_linear(const Vec3 &e, double exponent)
{
	Vec3 rgb = {hlg_eotf_channel(e[0]), hlg_eotf_channel(e[1]), hlg_eotf_channel(e[2])};
	const double ys = rgb[0] * kYs2020[0] + rgb[1] * kYs2020[1] + rgb[2] * kYs2020[2];
	const double k = std::pow(ys, exponent);
	return {rgb[0] * k, rgb[1] * k, rgb[2] * k};
}

Vec3 rec2020_to_rec709(const Vec3 &v)
{
	static const Mat3 m = {Vec3{1.6604910021084345, -0.58764113878854951, -0.072849863319884883},
			       Vec3{-0.12455047452159074, 1.1328998971259603, -0.0083494226043694768},
			       Vec3{-0.018150763354905303, -0.10057889800800739, 1.1187296613629127}};
	return mul(m, v);
}

Vec3 rec709_to_rec2020(const Vec3 &v)
{
	static const Mat3 m = {Vec3{0.62740389593469903, 0.32928303837788370, 0.043313065687417225},
			       Vec3{0.069097289358232075, 0.91954039507545871, 0.011362315566309178},
			       Vec3{0.016391438875150280, 0.088013307877225749, 0.89559525324762401}};
	return mul(m, v);
}

HlgParams hlg_params(double sdr_white_nits, double hdr_peak_nits)
{
	HlgParams h;
	const double peak = std::min(hdr_peak_nits, 1000.0); // libobs: BT.2408 comfort limit
	h.exponent = 0.2 + 0.42 * std::log10(peak / 1000.0);
	h.max_over_sdr = peak / sdr_white_nits;
	return h;
}

Vec3 reinterpret(const Vec3 &v, const Mat3 &corr, const HlgParams &h)
{
	const Vec3 e = mul(corr, Vec3{srgb_encode(v[0]), srgb_encode(v[1]), srgb_encode(v[2])});
	Vec3 lin = hlg_to_linear(e, h.exponent);
	for (double &c : lin)
		c *= h.max_over_sdr;
	return rec2020_to_rec709(lin);
}

ShaderParams make_shader_params(const Mat3 &corr, const HlgParams &h)
{
	ShaderParams s{};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			s.corr[i][j] = (float)corr[i][j];
	s.exponent = (float)h.exponent;
	s.max_over_sdr = (float)h.max_over_sdr;
	return s;
}

namespace {

float srgb_encode_f(float u)
{
	return (u <= 0.0031308f) ? (12.92f * u) : ((1.055f * std::pow(u, 1.0f / 2.4f)) - 0.055f);
}

float hlg_eotf_f(float u)
{
	const float ln2_i = 1.0f / std::log(2.0f);
	const float m = ln2_i / 0.17883277f;
	const float a = -ln2_i * 0.55991073f / 0.17883277f;
	return (u <= 0.5f) ? ((u * u) / 3.0f) : ((std::exp2(u * m + a) + 0.28466892f) / 12.0f);
}

} // namespace

std::array<float, 3> reinterpret_f(const ShaderParams &s, float r, float g, float b)
{
	const float er = srgb_encode_f(r), eg = srgb_encode_f(g), eb = srgb_encode_f(b);
	const float hr = s.corr[0][0] * er + s.corr[0][1] * eg + s.corr[0][2] * eb;
	const float hg = s.corr[1][0] * er + s.corr[1][1] * eg + s.corr[1][2] * eb;
	const float hb = s.corr[2][0] * er + s.corr[2][1] * eg + s.corr[2][2] * eb;
	float lr = hlg_eotf_f(hr), lg = hlg_eotf_f(hg), lb = hlg_eotf_f(hb);
	const float ys = lr * 0.2627f + lg * 0.678f + lb * 0.0593f;
	const float k = std::pow(ys, s.exponent) * s.max_over_sdr;
	lr *= k;
	lg *= k;
	lb *= k;
	return {lr * 1.6604910021084345f + lg * -0.58764113878854951f + lb * -0.072849863319884883f,
		lr * -0.12455047452159074f + lg * 1.1328998971259603f + lb * -0.0083494226043694768f,
		lr * -0.018150763354905303f + lg * -0.10057889800800739f + lb * 1.1187296613629127f};
}

double recover_luma(Decoded d, Matrix m, const HlgParams &h, double r, double g, double b)
{
	Vec3 e;
	Matrix mm = m;
	switch (d) {
	case Decoded::Sdr8:
		e = {r, g, b};
		break;
	case Decoded::SdrFloat:
		e = {srgb_encode(r), srgb_encode(g), srgb_encode(b)};
		break;
	default: {
		Vec3 dd = rec709_to_rec2020({r, g, b});
		for (double &c : dd)
			c /= h.max_over_sdr;
		const double ysd = dd[0] * kYs2020[0] + dd[1] * kYs2020[1] + dd[2] * kYs2020[2];
		if (!(ysd > 0.0))
			return 0.0;
		const double k = std::pow(std::pow(ysd, 1.0 / (1.0 + h.exponent)), h.exponent);
		e = {hlg_oetf_channel(dd[0] / k), hlg_oetf_channel(dd[1] / k), hlg_oetf_channel(dd[2] / k)};
		mm = Bt2020;
		break;
	}
	}
	const Vec3 row = luma_row(mm);
	return row[0] * e[0] + row[1] * e[1] + row[2] * e[2];
}

// ---- quantisation analysis -------------------------------------------------------

namespace {
constexpr double kLo = -64.0, kHi = 1152.0; // code10 histogram span
constexpr int kSub = 64;                    // bins per 10-bit code
constexpr int kBins = (int)((kHi - kLo) * kSub);
} // namespace

void SignalAnalysis::reset(bool limited_range)
{
	limited_ = limited_range;
	fine_.assign((size_t)kBins, 0.0);
	n_ = clip_lo_ = clip_hi_ = 0;
}

void SignalAnalysis::add(double y)
{
	if (!std::isfinite(y))
		return;
	const double c = limited_ ? y * 876.0 + 64.0 : y * 1023.0;
	const double lo = limited_ ? 64.0 : 0.0, hi = limited_ ? 940.0 : 1023.0;
	n_ += 1;
	// OBS clamps Y'CbCr to the nominal range before the matrix: those samples carry no
	// quantisation information
	if (std::fabs(c - lo) < 0.25) {
		clip_lo_ += 1;
		return;
	}
	if (std::fabs(c - hi) < 0.25) {
		clip_hi_ += 1;
		return;
	}
	const int i = (int)std::floor((c - kLo) * kSub);
	if (i >= 0 && i < kBins)
		fine_[(size_t)i] += 1;
}

DepthResult SignalAnalysis::finish() const
{
	DepthResult r;
	r.samples = n_;
	if (n_ <= 0) {
		r.why = "no samples";
		return r;
	}
	r.clip_low = clip_lo_ / n_;
	r.clip_high = clip_hi_ / n_;
	// counts per 10-bit code (bins centred on the code)
	const int ncodes = (int)(kHi - kLo);
	std::vector<double> coarse((size_t)ncodes, 0.0);
	auto code_of = [](int bin) {
		return (int)std::floor(((bin + 0.5) / kSub + kLo) + 0.5);
	};
	for (int b = 0; b < kBins; b++) {
		const int k = code_of(b) - (int)kLo;
		if (k >= 0 && k < ncodes)
			coarse[(size_t)k] += fine_[(size_t)b];
	}
	double total = 0;
	for (int k = 0; k < ncodes; k++) {
		total += coarse[(size_t)k];
		if (coarse[(size_t)k] >= 2) {
			r.occupied_codes++;
			r.peak_code = k + kLo;
		}
	}
	if (total < 20000) {
		r.why = "too few usable samples (most of the picture is at black or at the clip level)";
		return r;
	}
	int lo_code = 1 << 30, hi_code = -(1 << 30);
	for (int k = 0; k < ncodes; k++)
		if (coarse[(size_t)k] >= 2) {
			lo_code = std::min(lo_code, k);
			hi_code = std::max(hi_code, k);
		}
	if (r.occupied_codes < 32 || hi_code - lo_code < 128) {
		r.why = "not enough tonal variety (the picture needs a spread of dark to bright levels)";
		return r;
	}
	// phase coherence against each N-bit lattice; each code's weight grows only as the
	// square root of its count, so a few large flat areas cannot dominate
	for (int n = 6; n <= 12; n++) {
		double sw = 0, sc = 0, ss = 0;
		for (int b = 0; b < kBins; b++) {
			const double cnt = fine_[(size_t)b];
			if (cnt <= 0)
				continue;
			const int k = code_of(b) - (int)kLo;
			const double w = cnt / std::sqrt(coarse[(size_t)k]);
			const double c = (b + 0.5) / kSub + kLo;
			const double units = limited_ ? c * std::ldexp(1.0, n - 10)
						      : c / 1023.0 * (std::ldexp(1.0, n) - 1.0);
			const double ph = 2.0 * kPi * units;
			sw += w;
			sc += w * std::cos(ph);
			ss += w * std::sin(ph);
		}
		r.coherence[n] = std::sqrt(sc * sc + ss * ss) / sw;
	}
	// D = the coarsest lattice the signal sits on: coherent at N and clearly less so at N - 1
	for (int n = 6; n <= 10; n++) {
		if (r.coherence[n] >= 0.5 && (n == 6 || r.coherence[n - 1] < 0.5 * r.coherence[n])) {
			r.bits = n;
			break;
		}
	}
	r.ok = true;
	return r;
}

std::string depth_report(const DepthResult &r, bool limited)
{
	char line[256];
	std::string s;
	snprintf(line, sizeof(line),
		 "%.0f samples, %s range; %d distinct 10-bit luma codes; at clamp: %.2f%% black, %.2f%% white",
		 r.samples, limited ? "limited" : "full", r.occupied_codes, 100.0 * r.clip_low, 100.0 * r.clip_high);
	s += line;
	s += "\nlattice coherence (1 = on the grid, 0 = not):";
	for (int n = 6; n <= 12; n++) {
		snprintf(line, sizeof(line), "  %d-bit %.3f", n, r.coherence[n]);
		s += line;
	}
	s += "\n";
	if (!r.ok)
		s += "no verdict: " + r.why;
	else if (r.bits) {
		snprintf(line, sizeof(line), "verdict: %d-bit quantisation; peak luma code %.0f (%.1f%%)", r.bits,
			 r.peak_code, limited ? 100.0 * (r.peak_code - 64.0) / 876.0 : 100.0 * r.peak_code / 1023.0);
		s += line;
	} else
		s += "verdict: not on any 6-10-bit grid (finer than 10 bit cannot be seen here, or processing between "
		     "camera and OBS, or the matrix / range setting does not match the capture source)";
	return s;
}

std::string depth_summary(const DepthResult &r, bool limited)
{
	char s[512];
	if (!r.ok) {
		snprintf(s, sizeof(s), "No verdict: %s.", r.why.c_str());
		return s;
	}
	const double peak = limited ? 100.0 * (r.peak_code - 64.0) / 876.0 : 100.0 * r.peak_code / 1023.0;
	if (r.bits)
		snprintf(s, sizeof(s),
			 "Luma is quantised to %d bits (lattice coherence %.2f at %d bit, %.2f at %d bit). %d distinct "
			 "10-bit levels; brightest luma level %.0f%%; %.1f%% of samples at the white clamp.",
			 r.bits, r.coherence[r.bits], r.bits, r.bits > 6 ? r.coherence[r.bits - 1] : 0.0,
			 r.bits > 6 ? r.bits - 1 : 6, r.occupied_codes, peak, 100.0 * r.clip_high);
	else
		snprintf(s, sizeof(s),
			 "Not on a 6-10-bit grid (coherence at 10 bit %.2f). Either the signal is finer than this path "
			 "can show, something between camera and OBS re-processed it, or the matrix / range setting "
			 "below does not match the capture source.",
			 r.coherence[10]);
	return s;
}

} // namespace input
} // namespace hdrtk
