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
 * P2 spatial denoise (docs/HQDN3D_DESIGN.md section 8). Independent implementation, no
 * FFmpeg/MPlayer code. No libobs dependency (tests/cpu/test_spatial.cpp).
 *
 *   B - "HQDN3D-style spatial": symmetric edge-aware kernel, separable H then V.
 *       Double reference + float mirror of the SpatialH/SpatialV techniques in
 *       data/effects/hdr-program-denoise.effect (same operation order).
 *   A - bidirectional separable recursion (forward and backward from the same input,
 *       averaged). Double reference only until the D3D11 compute path exists.
 *
 * Values are linear RGB in nominal nits (the shader multiplies by nits_per_unit first).
 */

#include "hqdn3d-math.hpp"

namespace hdrtk {
namespace denoise {

constexpr double kBetaS = 0.9; // spatial decay per tap (B) / maximum recursion weight (A)
constexpr int kSpatialRadiusMax = 12;

struct SpatialParams {
	double luma = 0.0;    // S_L, 0..20 -> T_L = 0.01 S_L
	double chroma = 0.0;  // S_C, 0..20
	double k_nits = 0.1;  // comparison knee (shared with temporal)
	int radius = 8;       // B only: 1..12 (tested 6, 8, 12)
	NoiseProfile profile; // threshold multiplier vs luminance (identity by default)
};

void sanitize(SpatialParams &p);

// ---- B: double reference ---------------------------------------------------------
// One axis. vertical = false filters along rows.
void spatial_b_pass(const SpatialParams &p, const Image &in, Image &out, bool vertical);
// H then V.
void spatial_b(const SpatialParams &p, const Image &in, Image &out);

// ---- B: float mirror of the shader ---------------------------------------------------
struct SpatialShaderParams {
	float t_luma, t_chroma, k;
	int radius;
	ShaderProfile profile;
};
SpatialShaderParams make_spatial_shader_params(const SpatialParams &p);
// Values are rounded to float on read; the result is stored as float values.
void spatial_b_pass_f(const SpatialShaderParams &s, const Image &in, Image &out, bool vertical);
void spatial_b_f(const SpatialShaderParams &s, const Image &in, Image &out);

// ---- A: bidirectional separable recursion, double reference --------------------------
void spatial_a_pass(const SpatialParams &p, const Image &in, Image &out, bool vertical);
void spatial_a(const SpatialParams &p, const Image &in, Image &out);

} // namespace denoise
} // namespace hdrtk
