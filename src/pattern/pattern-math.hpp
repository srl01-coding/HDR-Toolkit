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
 * Pure CPU math for the developer HDR pattern source. No libobs dependency so
 * it can be unit-tested on any machine (tests/cpu/test_pattern.cpp).
 *
 * All colours are straight (non-premultiplied) linear Rec.709-primary RGB in
 * nominal nits. The source divides by the SDR white level W at upload time to
 * produce GS_CS_709_EXTENDED working values (1.0 = W nits).
 */

#include <cstdint>
#include <vector>

namespace hdrtk {
namespace pattern {

struct Rgba {
	float r, g, b, a;
};

// Rec.709/D65 luminance row, full precision (brief section 5.1).
constexpr double kLuma[3] = {0.212639005871510, 0.715168678767756, 0.072192315360734};

// Neutral patch levels required by the brief (section 11.3), nominal nits.
constexpr double kNeutralNits[] = {0, 0.01, 0.1, 1, 18, 100, 203, 300, 750, 1000, 2000, 4000, 10000};
constexpr int kNeutralCount = sizeof(kNeutralNits) / sizeof(kNeutralNits[0]);

// HLG signal levels for the scope cross-check row (fraction, 1.0 = 100% = code 940).
constexpr double kHlgLevels[] = {0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.75, 0.8, 0.9, 1.0, 1.05, 1.09};
constexpr int kHlgCount = sizeof(kHlgLevels) / sizeof(kHlgLevels[0]);

// Highlight staircase: 1/6-stop steps starting at this level.
constexpr double kStairBaseNits = 150.0;
constexpr int kStairCount = 24;

// Alpha fixture levels (brief section 11.3).
constexpr double kAlphaLevels[] = {0.0, 1e-4, 0.1, 0.5, 1.0};
constexpr int kAlphaCount = sizeof(kAlphaLevels) / sizeof(kAlphaLevels[0]);

// BT.2100 HLG inverse OETF: non-linear signal E' -> normalised scene light [0..1+].
double hlg_inverse_oetf(double e);
// BT.2100 HLG OETF (inverse of the above).
double hlg_oetf(double scene);

// Display nits that OBS's own HLG output encoder (libobs color.effect,
// linear_to_hlg with nominal peak <= 1000 nits) turns into HLG level `e` for a
// neutral grey. OBS normalises by 1000 nits and applies the inverse OOTF with
// system gamma 1.2, so nits = 1000 * scene^1.2.
double hlg_level_to_obs_nits(double e);
// Forward model of the same encoder for a neutral grey (used by tests).
double obs_nits_to_hlg_level(double nits);

// Rec.2020 -> Rec.709 linear RGB (D65), rows applied to column vectors.
void rec2020_to_rec709(const double in[3], double out[3]);

// Isolated modes fill the whole frame with one row of the chart, so a
// waveform shows only those levels (one flat trace per patch).
enum class Mode : int { Chart = 0, FlatField = 1, HlgSteps = 2, NeutralSteps = 3, HighlightSteps = 4, Ramp = 5 };

bool mode_valid(int mode);
const char *mode_label(Mode mode);

struct Settings {
	Mode mode = Mode::Chart;
	double flat_nits = 203.0;
	uint32_t width = 1920;
	uint32_t height = 1080;
};

// Fills `out` (width*height, row-major, row 0 = top) with straight linear
// Rec.709 RGB in nits plus alpha. Returns false on invalid settings.
bool generate(const Settings &s, std::vector<Rgba> &out);

// Row layout of the chart, as fractions of height. Exposed for tests/docs.
struct ChartRows {
	static constexpr double neutral_end = 0.18;
	static constexpr double ramp_end = 0.32;
	static constexpr double stair_end = 0.48;
	static constexpr double hlg_end = 0.64;
	static constexpr double color_end = 0.80;
	// alpha row: color_end .. 1.0
};

} // namespace pattern
} // namespace hdrtk
