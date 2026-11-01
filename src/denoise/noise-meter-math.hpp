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
 * Live noise measurement, CPU analysis (docs/denoise/NOISE_MEASUREMENT.md). No libobs
 * dependency (tests/cpu/test_noise_meter.cpp).
 *
 * Input: N frames of luma in nits at the same sparse grid of pixels (point samples, no
 * averaging, so the noise is intact). Per pixel: temporal mean and standard deviation
 * after removing a global per-frame gain (lighting flicker). Pixels are grouped into
 * quarter-stop brightness bands; the median sigma of each band is robust to a minority
 * of moving or edge pixels. The comparison knee K is the value that makes
 * sigma / (Y + K) - the noise in the denoiser's comparison domain - most uniform.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace hdrtk {
namespace denoise {

struct NoiseBand {
	double nits = 0;  // median temporal mean of the band's pixels
	double sigma = 0; // median temporal standard deviation, nits
	int count = 0;    // pixels in the band
};

struct NoiseFit {
	bool ok = false;
	std::string why; // reason when !ok
	double knee = 0; // fitted comparison knee K, nits
	bool knee_at_limit = false;
	double spread_fit = 0;         // max/min of sigma_F over the bands at the fitted K
	double spread_default = 0;     // ... at K = 0.1 (the default)
	double sigma_f = 0;            // band-weighted mean sigma_F at the fitted K, log2 units
	double suggested_temporal = 0; // strength for roughly half the noise left (simulation-based)
	double nits_lo = 0, nits_hi = 0;
	int bands = 0;         // bands used in the fit
	int bands_dropped = 0; // inconsistent bands left out (robust pass)
};

constexpr double kMeterKneeMin = 0.01, kMeterKneeMax = 100.0;
constexpr int kMeterMinBandCount = 200;

class NoiseAnalysis {
public:
	void reset(size_t pixels);
	// One frame: luma in nits per grid pixel. Non-finite values mark a pixel invalid.
	void add_frame(const float *luma, size_t n);
	int frames() const { return frames_; }
	size_t pixels() const { return mean_.size(); }
	// Relative std of the per-frame global gain (flicker / exposure drift), and its range.
	double gain_std() const;
	double gain_range() const;
	// Fraction of valid pixels whose sigma exceeds 4x their band's median (motion).
	double outlier_fraction() const;
	std::vector<NoiseBand> bands(int min_count = kMeterMinBandCount) const;

private:
	std::vector<double> mean_, m2_;
	std::vector<uint8_t> valid_;
	std::vector<double> gains_;
	double ref_mean_ = 0;
	int frames_ = 0;
};

// sigma_F of a band in the comparison domain F = log2(1 + Y / K)
double band_sigma_f(const NoiseBand &b, double knee);
NoiseFit fit_knee(const std::vector<NoiseBand> &bands);

// Multi-line report for the log, and a short summary for the properties panel.
std::string noise_report(const std::vector<NoiseBand> &bands, const NoiseFit &fit, double gain_std, double gain_range,
			 double outliers, int frames, size_t pixels);
std::string noise_summary(const NoiseFit &fit, double gain_range, double outliers);

} // namespace denoise
} // namespace hdrtk
