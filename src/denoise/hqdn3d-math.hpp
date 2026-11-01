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
 * HQDN3D-style temporal denoise: CPU reference (double) and float mirror of
 * data/effects/hdr-program-denoise.effect. Implemented from docs/HQDN3D_DESIGN.md;
 * no FFmpeg/MPlayer code was used. No libobs dependency (tests/cpu/test_hqdn3d.cpp).
 * Values are linear RGB in nominal nits.
 */

#include "noise-profile.hpp"

#include <cstdint>
#include <vector>

namespace hdrtk {
namespace denoise {

constexpr double kLumaR = 0.2126, kLumaG = 0.7152, kLumaB = 0.0722;
constexpr double kBeta = 0.9;              // maximum history weight up to strength 20
constexpr double kStrengthMax = 100.0;     // UI/sanitize range of all four strengths
constexpr double kStrengthBetaKnee = 20.0; // above this, the temporal history weight cap rises
constexpr double kBetaMax = 0.99;          // ... to this at kStrengthMax
constexpr double kThresholdPerUnit = 0.01; // T = 0.01 * strength, log2 units
constexpr double kFloorRise = 1.005;       // static-noise floor upward recovery per frame (~12 s per 6x)
constexpr double kFloorAdd = 0.0001;       // ... plus this, so a zero floor recovers
constexpr double kProtectRamp = 0.02;      // m - 1.5 f range over which protection ramps in
constexpr double kProtectFloorMul = 1.5;
constexpr int kBlock = 16; // metric reduction block
constexpr int kSub = 4;    // 4 x 4 samples per first-level block

struct Params {
	double temporal_luma = 0.0;   // 0..20
	NoiseProfile profile;         // threshold multiplier vs luminance (identity by default)
	double temporal_chroma = 0.0; // 0..20
	double k_nits = 0.1;          // comparison knee
	bool cut_reset = true;
	double cut_sensitivity = 50.0; // 0..100
	bool protection = true;
	double protection_amount = 0.8; // 0..1
};

// Clamp to documented ranges; non-finite values revert to the default.
void sanitize(Params &p);

double comp(double y, double k);          // F(y) = sign(y) log2(1 + |y|/k)
double response(double x);                // W(x) = (1 - x^2)^2 on [0, 1), else 0
double cut_threshold(double sensitivity); // m_cut = 1.2 * 2^(-s/25)
// History weight cap for a temporal strength: kBeta up to 20, then 1 - beta falls
// geometrically from 0.1 (S 20) to 0.01 (S 100). Static residual ~ sqrt((1-b)/(1+b)):
// 0.23 at S <= 20, 0.13 at S 60, 0.07 at S 100.
double history_beta(double strength);
// True when either cap exceeds kBeta: the history is then kept in RGBA32F (with 1 - b
// = 0.01, an RGBA16F history would stall on differences below ~2.4%).
bool needs_precise_history(const Params &p);
double smoothstep(double e0, double e1, double x);
// Chroma distance (design section 2), shared by temporal and spatial:
// |ca - cb| / ((|ya| + |yb|) / 2 + k) / ln 2
double chroma_distance(const double ca[3], const double cb[3], double ya, double yb, double k);

struct Rgba {
	double r = 0, g = 0, b = 0, a = 1;
};

// Global factor g from the frame metric m and the (already updated) floor f.
// reset: history invalid this frame -> g = 0. cut is set when a hard cut fires.
double global_factor(const Params &p, double m, double f, bool reset, bool *cut = nullptr);

// One pixel of the temporal recurrence (design section 3), double reference.
Rgba temporal_pixel(const Params &p, const Rgba &cur, const Rgba &hist, double g);

// Frame metric over an image (design section 4), same grid as the shader.
struct Image {
	int w = 0, h = 0;
	std::vector<Rgba> px; // row-major
	const Rgba &at(int x, int y) const { return px[(size_t)y * (size_t)w + (size_t)x]; }
};
double frame_metric(const Image &cur, const Image &hist, double k);
// Floor update: f_next = valid ? min(m, f * 1.005 + 0.0001) : m
double floor_update(double m, double f_prev, bool f_valid);

// ---- float32 mirror of the shader (operation order as in the .effect) -----
struct ShaderParams {
	float t_luma, t_chroma, k, m_cut, protect_amount, cut_enabled;
	float beta_luma, beta_chroma;
	ShaderProfile profile;
};
ShaderParams make_shader_params(const Params &p);
float global_factor_f(const ShaderParams &s, float m, float f, bool reset);
void temporal_pixel_f(const ShaderParams &s, const float cur[4], const float hist[4], float g, float out[4]);
float frame_metric_f(const Image &cur, const Image &hist, float k);

} // namespace denoise
} // namespace hdrtk
