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

#include "pattern-math.hpp"

#include <cmath>

namespace hdrtk {
namespace pattern {

// BT.2100 HLG constants
static constexpr double kA = 0.17883277;
static constexpr double kB = 0.28466892; // 1 - 4a
static constexpr double kC = 0.55991073; // 0.5 - a*ln(4a)

double hlg_inverse_oetf(double e)
{
	if (e <= 0.0)
		return 0.0;
	if (e <= 0.5)
		return e * e / 3.0;
	return (std::exp((e - kC) / kA) + kB) / 12.0;
}

double hlg_oetf(double scene)
{
	if (scene <= 0.0)
		return 0.0;
	if (scene <= 1.0 / 12.0)
		return std::sqrt(3.0 * scene);
	return kA * std::log(12.0 * scene - kB) + kC;
}

double hlg_level_to_obs_nits(double e)
{
	return 1000.0 * std::pow(hlg_inverse_oetf(e), 1.2);
}

double obs_nits_to_hlg_level(double nits)
{
	// libobs linear_to_hlg (nominal peak <= 1000): rgb *= 10 after /10000,
	// i.e. display light normalised to 1000 nits; for grey Yd = rgb, so
	// scene = Yd * Yd^(-1/6) = Yd^(5/6).
	const double yd = nits / 1000.0;
	if (yd <= 0.0)
		return 0.0;
	return hlg_oetf(std::pow(yd, 5.0 / 6.0));
}

void rec2020_to_rec709(const double in[3], double out[3])
{
	static const double m[3][3] = {
		{1.6604910021084345, -0.58764113878854951, -0.072849863319884883},
		{-0.12455047452159074, 1.1328998971259603, -0.0083494226043694768},
		{-0.018150763354905303, -0.10057889800800739, 1.1187296613629127},
	};
	for (int i = 0; i < 3; i++)
		out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
}

static Rgba grey(double nits, double alpha = 1.0)
{
	return Rgba{(float)nits, (float)nits, (float)nits, (float)alpha};
}

static Rgba scaled_to_luminance(const double rgb[3], double y_nits)
{
	const double y = kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2];
	const double k = y_nits / y;
	return Rgba{(float)(rgb[0] * k), (float)(rgb[1] * k), (float)(rgb[2] * k), 1.0f};
}

// Colour fixtures at 100 nits luminance: Rec.709 primaries/secondaries, then
// Rec.2020 primaries expressed in Rec.709 coordinates (negative components).
static std::vector<Rgba> color_patches()
{
	std::vector<Rgba> v;
	const double p709[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
	for (auto &p : p709)
		v.push_back(scaled_to_luminance(p, 100.0));
	const double p2020[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	for (auto &p : p2020) {
		double out[3];
		rec2020_to_rec709(p, out);
		v.push_back(scaled_to_luminance(out, 100.0));
	}
	return v;
}

static std::vector<Rgba> alpha_patches()
{
	std::vector<Rgba> v;
	for (int i = 0; i < kAlphaCount; i++) {
		v.push_back(Rgba{1000.0f, 800.0f, 600.0f, (float)kAlphaLevels[i]});
		v.push_back(grey(4000.0, kAlphaLevels[i]));
	}
	return v;
}

// Patch index for column x when a row is split into n equal patches with a
// 2-pixel black separator; -1 for the separator.
static int patch_at(uint32_t x, uint32_t width, int n)
{
	const uint32_t i = (uint32_t)((uint64_t)x * (uint64_t)n / width);
	const uint32_t start = (uint32_t)((uint64_t)i * width / (uint64_t)n);
	if (x - start < 2 && i > 0)
		return -1;
	return (int)i;
}

bool mode_valid(int mode)
{
	return mode >= (int)Mode::Chart && mode <= (int)Mode::Ramp;
}

const char *mode_label(Mode mode)
{
	switch (mode) {
	case Mode::Chart:
		return "chart";
	case Mode::FlatField:
		return "flat";
	case Mode::HlgSteps:
		return "hlg-steps";
	case Mode::NeutralSteps:
		return "neutral-steps";
	case Mode::HighlightSteps:
		return "highlight-steps";
	case Mode::Ramp:
		return "ramp";
	}
	return "unknown";
}

bool generate(const Settings &s, std::vector<Rgba> &out)
{
	if (!mode_valid((int)s.mode))
		return false;
	if (s.width < 64 || s.height < 64 || s.width > 8192 || s.height > 8192)
		return false;
	if (!std::isfinite(s.flat_nits) || s.flat_nits < 0.0 || s.flat_nits > 100000.0)
		return false;

	out.assign((size_t)s.width * s.height, Rgba{0, 0, 0, 1});

	if (s.mode == Mode::FlatField) {
		for (auto &px : out)
			px = grey(s.flat_nits);
		return true;
	}

	if (s.mode != Mode::Chart) {
		for (uint32_t y = 0; y < s.height; y++) {
			Rgba *row = &out[(size_t)y * s.width];
			for (uint32_t x = 0; x < s.width; x++) {
				Rgba px{0, 0, 0, 1};
				int i;
				switch (s.mode) {
				case Mode::HlgSteps:
					i = patch_at(x, s.width, kHlgCount);
					if (i >= 0)
						px = grey(hlg_level_to_obs_nits(kHlgLevels[i]));
					break;
				case Mode::NeutralSteps:
					i = patch_at(x, s.width, kNeutralCount);
					if (i >= 0)
						px = grey(kNeutralNits[i]);
					break;
				case Mode::HighlightSteps:
					i = patch_at(x, s.width, kStairCount);
					if (i >= 0)
						px = grey(kStairBaseNits * std::exp2(i / 6.0));
					break;
				case Mode::Ramp:
					px = grey(std::pow(10.0, -2.0 + 6.0 * ((x + 0.5) / s.width)));
					break;
				default:
					break;
				}
				row[x] = px;
			}
		}
		return true;
	}

	const std::vector<Rgba> colors = color_patches();
	const std::vector<Rgba> alphas = alpha_patches();
	const Rgba sep{0, 0, 0, 1};

	for (uint32_t y = 0; y < s.height; y++) {
		const double fy = (y + 0.5) / s.height;
		Rgba *row = &out[(size_t)y * s.width];
		for (uint32_t x = 0; x < s.width; x++) {
			Rgba px = sep;
			if (fy < ChartRows::neutral_end) {
				const int i = patch_at(x, s.width, kNeutralCount);
				if (i >= 0)
					px = grey(kNeutralNits[i]);
			} else if (fy < ChartRows::ramp_end) {
				// continuous log ramp 0.01 .. 10000 nits
				const double t = (x + 0.5) / s.width;
				px = grey(std::pow(10.0, -2.0 + 6.0 * t));
			} else if (fy < ChartRows::stair_end) {
				const int i = patch_at(x, s.width, kStairCount);
				if (i >= 0)
					px = grey(kStairBaseNits * std::exp2(i / 6.0));
			} else if (fy < ChartRows::hlg_end) {
				const int i = patch_at(x, s.width, kHlgCount);
				if (i >= 0)
					px = grey(hlg_level_to_obs_nits(kHlgLevels[i]));
			} else if (fy < ChartRows::color_end) {
				const int i = patch_at(x, s.width, (int)colors.size());
				if (i >= 0)
					px = colors[(size_t)i];
			} else {
				const int i = patch_at(x, s.width, (int)alphas.size());
				if (i >= 0)
					px = alphas[(size_t)i];
			}
			row[x] = px;
		}
	}
	return true;
}

} // namespace pattern
} // namespace hdrtk
