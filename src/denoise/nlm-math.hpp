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
 * NLMeans with generalised candidates (dx, dy, dt), dt in {0, -1}: CPU references
 * (docs/denoise/NLMEANS_DESIGN.md). Independent implementation; no FFmpeg/HandBrake
 * code. No libobs dependency. Values are linear RGB in nominal nits.
 *
 *   nlm_naive   - reads the whole patch per (pixel, candidate); the ground truth
 *   nlm_offset  - offset-major: one difference image + separable box mean per candidate
 *                 (the formulation the GPU kernel will use)
 */

#include "hqdn3d-math.hpp"

namespace hdrtk {
namespace denoise {

enum NlmPolicy {
	NlmPolicyA = 0, // temporal candidates matched against and taken from C(t-1)
	NlmPolicyB = 1, // matched against and taken from D(t-1) (recursive)
	NlmPolicyH = 2, // matched against C(t-1), values from D(t-1)
};

struct NlmParams {
	double luma = 0.0;   // S_L 0..20 -> h_L = 0.01 S_L (log2 units)
	double chroma = 0.0; // S_C 0..20
	double k_nits = 0.1; // comparison knee
	int patch = 1;       // patch half-size P (patch (2P+1)^2)
	int search = 3;      // spatial search half-size Rs (dt = 0)
	int tsearch = 3;     // temporal search half-size Rt (dt = -1), used when temporal
	bool temporal = false;
	int policy = NlmPolicyA;
	double lambda = 0.0; // weight of the chroma term in the patch distance
	double cap = 0.9;    // maximum temporal share for the recursive policies (B, H)
	// temporal candidate weight multiplier (1..9): with the self weight fixed at 1, a single
	// temporal match could otherwise never exceed a 50% share
	double tgain = 1.0;
	NoiseProfile profile; // h multiplier vs the centre pixel's luminance (identity by default)
};

constexpr double kNlmCutoff = 12.0; // D / h^2 above this -> weight 0

void sanitize(NlmParams &p);

// Temporal inputs: prev_in = C(t-1), prev_out = D(t-1) (either may be null when the
// policy does not need it). g = global temporal factor (0 on cut/reset; HQDN3D metric).
struct NlmFrame {
	const Image *cur = nullptr;
	const Image *prev_in = nullptr;
	const Image *prev_out = nullptr;
	double g = 1.0;
};

void nlm_naive(const NlmParams &p, const NlmFrame &f, Image &out);
void nlm_offset(const NlmParams &p, const NlmFrame &f, Image &out);

// Optional diagnostics from nlm_offset: per-pixel temporal share of the luma weights
// (after the cap), for tests.
void nlm_offset_share(const NlmParams &p, const NlmFrame &f, Image &out, std::vector<double> *share);

} // namespace denoise
} // namespace hdrtk
