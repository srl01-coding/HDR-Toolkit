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
 * Luminance-dependent threshold multiplier T(Y) = T * m(Y) (docs/denoise/NOISE_PROFILE.md;
 * decision C of docs/denoise/HIGH_BITRATE_ANALYSIS.md). Shared by HQDN3D temporal, spatial B,
 * NLMeans and motion-compensated temporal.
 *
 * m(Y): piecewise-linear in log2(nits) between up to 6 anchors, constant outside them,
 * clamped to [min_mult, max_mult]. n = 0 is the identity profile: m = 1 exactly, so every
 * filter is bit-identical to the profile-free version. The production curve is NOT
 * frozen; built-in profiles are development options.
 *
 * The double and float versions use the same operation order as profile_mult() in
 * data/effects/hdr-program-denoise.effect.
 */

namespace hdrtk {
namespace denoise {

constexpr int kProfileAnchors = 6;
constexpr double kProfileFloorNits = 0.001; // |Y| below this uses the first anchor

enum ProfileId {
	ProfileIdentity = 0,
	ProfileMeasured20261005 = 1, // 4 ROIs of the 5 Oct 2026 high-bitrate clip, relative to the wall
	ProfileCount
};

struct NoiseProfile {
	int n = 0;                      // anchors in use (0 = identity)
	double l[kProfileAnchors] = {}; // log2(nits), strictly increasing
	double m[kProfileAnchors] = {}; // multiplier at each anchor
	double min_mult = 0.5, max_mult = 3.0;
};

// Sorts anchors, enforces a minimum spacing (0.01 in log2), clamps n and the bounds
// (0.1 <= min <= 1 <= max <= 16). Non-finite anchors drop the profile to identity.
void sanitize(NoiseProfile &p);
NoiseProfile builtin_profile(int id, double max_mult);
double profile_multiplier(const NoiseProfile &p, double y_nits);

struct ShaderProfile {
	float count = 0;                  // np_count
	float a[kProfileAnchors][2] = {}; // (log2 nits, multiplier) -> np0.xy np0.zw np1.xy ...
	float min_mult = 0.5f, max_mult = 3.0f;
};
ShaderProfile make_shader_profile(const NoiseProfile &p);
float profile_multiplier_f(const ShaderProfile &s, float y_nits);

} // namespace denoise
} // namespace hdrtk
