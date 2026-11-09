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
 * SDR in HDR (docs/SDR_IN_HDR.md): places an SDR source in an HDR programme at a chosen
 * level, per source. OBS places every SDR source at the one global SDR white level; this
 * filter sets it per source. No libobs dependency (tests/cpu/test_sdrhdr.cpp).
 *
 * Input: OBS linear SDR (srgb_decode of the signal), Rec.709. Output: display light in
 * nits / OBS's SDR white level (OBS units; reported as SRGB_16F, which libobs copies into
 * an HDR target unchanged). Premultiplied alpha.
 *
 *   Graphics: L = white x V                    (OBS's own sRGB decode, scaled; linear, so
 *                                               it works on premultiplied values)
 *   Video:    L = white x E'^2.4,  E' = srgb_encode(V)   (BT.1886 display, as BT.2408's
 *             display-light mapping of SDR into HDR: SDR 100 % -> HDR reference white)
 *             optional highlight expansion above a knee on max(R,G,B): ratio-preserving,
 *             quadratic, C1 at the knee, SDR 100 % -> `peak` nits. Not a standardised
 *             curve; 0 % expansion (peak = white) is the BT.2408 mapping.
 */

#include "input/input-math.hpp"

namespace hdrtk {
namespace sdrhdr {

using Vec3 = input::Vec3;

enum Mode { ModeGraphics = 0, ModeVideo = 1 };

struct Params {
	int mode = ModeGraphics;
	double white_nits = 203.0;    // SDR 100 % lands here (BT.2408 reference / graphics white)
	double peak_nits = 203.0;     // Video: SDR 100 % with expansion (= white: none)
	double knee = 0.75;           // Video: expansion starts at this SDR signal level (max RGB)
	double obs_sdr_white = 300.0; // OBS's SDR white level (nits of OBS linear 1.0)
};
void sanitize(Params &p);

// Display light (nits, achromatic) -> HLG signal 0..1 on a display with peak `lw` (BT.2100,
// inverse OOTF with OBS's gamma formula). 203 nits on 1000 -> 0.75.
double nits_to_hlg(double nits, double lw);

Vec3 convert(const Vec3 &v, const Params &p); // double reference, straight (non-premultiplied)

// Float mirror of data/effects/hdr-sdr-in-hdr.effect PSPlace (keep in lock-step).
struct ShaderParams {
	int mode;
	float in_scale; // native units -> OBS linear with 1.0 = SDR white (scRGB: 80 / SDR white)
	float gain;     // white_nits / obs_sdr_white
	float knee;     // SDR signal (Video)
	float expand;   // (peak - white) / white, >= 0 (Video)
};
ShaderParams make_shader_params(const Params &p, float in_scale = 1.0f);
void place_f(const float in[4], const ShaderParams &sp, float out[4]); // premultiplied RGBA

} // namespace sdrhdr
} // namespace hdrtk
