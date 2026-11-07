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
 * HDR Sharpen (docs/SHARPEN.md): unsharp mask on luma in a log domain, applied to RGB as a
 * gain, so it works on HDR values without clamping and never changes a value's sign.
 *
 *   Y   = luma of the un-premultiplied pixel, working units (1.0 = SDR white)
 *   F   = log2(max(Y, 0) + K)                 K = dark floor in working units
 *   Fb  = alpha-weighted Gaussian blur of F   (sum w a F / sum w a; transparent pixels do
 *                                              not pull edges)
 *   d   = F - Fb                              detail, stops
 *   c   = d^3 / (d^2 + t^2)                   soft threshold (coring), t in stops
 *   e   = L s(A c / L),  s(x) = x / sqrt(1 + x^2)   amount A, halo limit L stops
 *   out = rgb + (rgb + a K) (2^e - 1)         per channel; exact identity when e = 0
 *
 * Above the floor the change is a ratio (2^e), so the same detail is sharpened the same in
 * stops at every brightness and halos are bounded by +/- L stops. Below the floor it is
 * nearly additive, which keeps dark noise from being multiplied.
 * The float mirror follows data/effects/hdr-sharpen.effect step by step - keep in lock-step.
 */

#include <cstdint>
#include <vector>

namespace hdrtk {
namespace sharpen {

struct Params {
	double amount = 0.0;     // 0..3 (UI 0..300 %); 0 = neutral
	double radius = 1.0;     // Gaussian sigma, pixels, 0.5..8
	double threshold = 0.02; // coring, stops, 0..0.5
	double limit = 0.5;      // halo limit, stops, 0.05..2
	double floor_nits = 2.0; // dark floor K, nits, 0.01..50
};
constexpr double kAmountMax = 3.0, kRadiusMin = 0.5, kRadiusMax = 8.0, kThresholdMax = 0.5;
constexpr double kLimitMin = 0.05, kLimitMax = 2.0, kFloorMin = 0.01, kFloorMax = 50.0;
constexpr int kMaxTaps = 24;

void sanitize(Params &p);
bool neutral(const Params &p);
int taps_for(double radius); // ceil(3 sigma), 1..kMaxTaps

struct Rgba {
	double r, g, b, a;
};
struct Image {
	int w = 0, h = 0;
	std::vector<Rgba> px; // premultiplied, working units
	const Rgba &at(int x, int y) const { return px[(size_t)y * (size_t)w + (size_t)x]; }
};

// Double-precision reference. npu = nits per working unit (for K).
void sharpen(const Params &p, double npu, const Image &in, Image &out);
// Detail e (stops) for a detail d (stops): the transfer curve on its own.
double detail_transfer(const Params &p, double d);

// Shader uniforms and the float mirror of the two passes.
struct ShaderParams {
	float k_units, taps, inv_2s2, amount, limit, thresh2;
};
ShaderParams make_shader_params(const Params &p, double npu);
struct RgbaF {
	float r, g, b, a;
};
void sharpen_f(const ShaderParams &s, int w, int h, const std::vector<RgbaF> &in, std::vector<RgbaF> &out);

} // namespace sharpen
} // namespace hdrtk
