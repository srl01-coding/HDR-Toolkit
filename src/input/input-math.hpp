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

#pragma once

/*
 * HDR Input (docs/HDR_INPUT.md): signal-level maths, no libobs dependency.
 *
 * 1. Reinterpretation. A capture source that delivers HLG without HDR metadata (DeckLink
 *    over HDMI) is decoded by OBS as SDR:  V = srgb_decode(E'),  E' = T_m (Y'CbCr).
 *    We undo that exactly and apply OBS's own HLG decode (format_conversion.effect
 *    PSV210_HLG_2020_709_Reverse, color.effect hlg_to_linear):
 *      E'    = srgb_encode(V)                      (OBS's formula)
 *      E'2020 = T_2020 F_m E'                      (identity when m = BT.2020)
 *      out   = rec2020_to_rec709(hlg_to_linear(E'2020, exponent) * peak / sdr_white)
 *    with peak = min(HDR nominal peak, 1000) and exponent = 0.2 + 0.42 log10(peak / 1000),
 *    exactly as libobs sets them for HLG frames.
 *
 * 2. Signal analysis. From what OBS delivers we rebuild the luma code value Y' of the
 *    original YCbCr signal (the Y row of the forward matrix recovers Y' exactly from any
 *    R'G'B' = T_m Y'CbCr), and test it against N-bit code lattices (N = 6..10): a signal
 *    quantised to D bits sits on the D-bit lattice, so phase coherence is ~1 for N >= D
 *    and ~0 for N < D.
 */

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hdrtk {
namespace input {

enum Matrix { Bt601 = 0, Bt709 = 1, Bt2020 = 2 };
using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>; // rows

Vec3 luma_row(Matrix m);     // (Kr, Kg, Kb)
Mat3 ycbcr_to_rgb(Matrix m); // Y in 0..1, Cb/Cr in -0.5..0.5
Mat3 rgb_to_ycbcr(Matrix m);
Mat3 mul(const Mat3 &a, const Mat3 &b);
Vec3 mul(const Mat3 &a, const Vec3 &v);
Mat3 matrix_correction(Matrix decoded_with); // T_2020 F_m

// OBS's transfer functions (libobs data/color.effect), double precision.
double srgb_encode(double v);      // srgb_linear_to_nonlinear_channel
double srgb_decode(double u);      // srgb_nonlinear_to_linear_channel
double hlg_eotf_channel(double u); // hlg_to_linear_channel
double hlg_oetf_channel(double s); // its inverse
Vec3 hlg_to_linear(const Vec3 &e, double exponent);
Vec3 rec2020_to_rec709(const Vec3 &v);
Vec3 rec709_to_rec2020(const Vec3 &v);

struct HlgParams {
	double exponent = 0.2;
	double max_over_sdr = 1000.0 / 203.0;
};
HlgParams hlg_params(double sdr_white_nits, double hdr_peak_nits);

// Reinterpretation (double reference) and its float mirror (hdr-input.effect).
Vec3 reinterpret(const Vec3 &v, const Mat3 &corr, const HlgParams &h);
struct ShaderParams {
	float corr[3][3];
	float exponent, max_over_sdr;
};
ShaderParams make_shader_params(const Mat3 &corr, const HlgParams &h);
std::array<float, 3> reinterpret_f(const ShaderParams &s, float r, float g, float b);

// What OBS did to the signal before this filter, for the analysis.
enum class Decoded {
	SdrFloat, // SRGB_16F: V = srgb_decode(T_m Y'CbCr)
	Sdr8,     // 8-bit SRGB texture read without sRGB decode: values are T_m Y'CbCr
	Hlg,      // 709_EXTENDED from OBS's HLG path (matrix BT.2020)
};
// Y' (range-normalised, 0 = black, 1 = nominal white) from one delivered pixel.
double recover_luma(Decoded d, Matrix m, const HlgParams &h, double r, double g, double b);

struct DepthResult {
	bool ok = false;
	std::string why;
	int bits = 0;                       // detected quantisation (6..10), 0 = none found
	double coherence[13] = {};          // index = N (6..12)
	int occupied_codes = 0;             // distinct 10-bit codes seen
	double clip_low = 0, clip_high = 0; // share of samples at the range clamp
	double peak_code = 0;               // highest 10-bit code (excluding the clamp)
	double samples = 0;
};

class SignalAnalysis {
public:
	void reset(bool limited_range);
	// One luma value Y' per sample (NaN = skip).
	void add(double y);
	DepthResult finish() const;

private:
	bool limited_ = true;
	std::vector<double> fine_; // 1/64-code bins over code10 -64 .. 1152
	double n_ = 0, clip_lo_ = 0, clip_hi_ = 0;
};

std::string depth_report(const DepthResult &r, bool limited);
std::string depth_summary(const DepthResult &r, bool limited);

} // namespace input
} // namespace hdrtk
