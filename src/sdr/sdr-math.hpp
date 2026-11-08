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
 * SDR Conversion (docs/SDR_CONVERSION.md): HDR -> SDR maths, no libobs dependency
 * (tests/cpu/test_sdr.cpp). Input is OBS linear light, Rec.709 primaries, 1.0 = SDR white
 * (display light, as libobs decodes HLG/PQ). Output is OBS linear SDR: srgb_decode of the
 * BT.709 SDR signal, so OBS's own SDR output encode (sRGB) reproduces that signal exactly.
 *
 * Method A of Report ITU-R BT.2446-1 (BT.2020 display light normalised to the HDR peak):
 *   E'      = (E / Lhdr)^(1/2.4)                        per BT.2020 channel
 *   Y'      = 0.2627 R' + 0.6780 G' + 0.0593 B'
 *   rho(L)  = 1 + 32 (L / 10000)^(1/2.4)
 *   Y'p     = log(1 + (rho_hdr - 1) Y') / log(rho_hdr)
 *   Y'c     = 1.077 Y'p                                 Y'p <= 0.7399
 *             -1.1510 Y'p^2 + 2.7811 Y'p - 0.6302       Y'p <  0.9909
 *             0.5 Y'p + 0.5                             otherwise
 *   Y'sdr   = (rho_sdr^Y'c - 1) / (rho_sdr - 1)         Lsdr = 100
 *   f       = Y'sdr / (1.1 Y');  Cb = f (B' - Y') / 1.8814;  Cr = f (R' - Y') / 1.4746
 *   Y'tmo   = Y'sdr - max(0.1 Cr, 0)
 * then BT.2020 SDR R'G'B' (clipped to 0..1) -> linear (2.4) -> BT.709 (clipped) -> 1/2.4.
 *
 * "OBS default" mirrors libobs default.effect DrawTonemap (what OBS shows for an HDR source
 * on an SDR canvas): 709 -> 2020, x / (x + 1), clip, ^(1/2.4), srgb_decode, 2020 -> 709.
 */

#include "input/input-math.hpp"

namespace hdrtk {
namespace sdr {

using Vec3 = input::Vec3;

struct Params {
	double sdr_white = 300.0; // nits of OBS linear 1.0
	double lhdr = 1000.0;     // HDR nominal peak (Lhdr)
	double lsdr = 100.0;      // SDR peak (Lsdr)
};

double rho(double l);
// Luma path: Y' (HDR, 0..1) -> Y'sdr. Exposed for the tests and the doc table.
double tone_curve_yp(double yp); // Y'p -> Y'c
double tone_luma(double y_hdr, double rho_hdr, double rho_sdr);

Vec3 bt2446a(const Vec3 &v709, const Params &p); // double reference
Vec3 obs_reinhard(const Vec3 &v709);             // libobs DrawTonemap

// Float mirror of data/effects/hdr-sdr.effect (keep in lock-step).
struct ShaderParams {
	float to_norm;    // sdr_white / Lhdr
	float rho_hdr_m1; // rho_hdr - 1
	float inv_log_rho_hdr;
	float log_rho_sdr; // ln(rho_sdr)
	float inv_rho_sdr_m1;
};
ShaderParams make_shader_params(const Params &p);
void bt2446a_f(const float in[3], const ShaderParams &sp, float out[3]);
void obs_reinhard_f(const float in[3], float out[3]);

} // namespace sdr
} // namespace hdrtk
