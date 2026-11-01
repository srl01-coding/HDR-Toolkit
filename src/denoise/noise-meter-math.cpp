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

#include "noise-meter-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hdrtk {
namespace denoise {

namespace {

constexpr double kBandLo = -6.0; // log2 nits of the first band's lower edge (1/64 nit)
constexpr double kBandsPerStop = 4.0;
constexpr int kBandCount = 72;          // up to 4096 nits
constexpr double kHalfNoiseRatio = 5.5; // T / sigma_F for ~50% residual (STRENGTH_SCALES.md simulations)

int band_index(double y)
{
	if (!(y > 0))
		return -1;
	const int i = (int)std::floor((std::log2(y) - kBandLo) * kBandsPerStop);
	return i >= 0 && i < kBandCount ? i : -1;
}

double median(std::vector<double> &v)
{
	if (v.empty())
		return 0;
	const size_t m = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + (ptrdiff_t)m, v.end());
	double r = v[m];
	if (v.size() % 2 == 0) {
		const double lo = *std::max_element(v.begin(), v.begin() + (ptrdiff_t)m);
		r = 0.5 * (r + lo);
	}
	return r;
}

} // namespace

void NoiseAnalysis::reset(size_t pixels)
{
	mean_.assign(pixels, 0.0);
	m2_.assign(pixels, 0.0);
	valid_.assign(pixels, 1);
	gains_.clear();
	ref_mean_ = 0;
	frames_ = 0;
}

void NoiseAnalysis::add_frame(const float *luma, size_t n)
{
	if (n != mean_.size() || n == 0)
		return;
	// global gain against the first frame, over pixels still valid
	double sum = 0;
	size_t cnt = 0;
	for (size_t i = 0; i < n; i++) {
		if (!std::isfinite(luma[i]))
			valid_[i] = 0;
		if (valid_[i]) {
			sum += luma[i];
			cnt++;
		}
	}
	const double m = cnt ? sum / (double)cnt : 0;
	if (frames_ == 0)
		ref_mean_ = m;
	const double g = ref_mean_ > 0 && m > 0 ? m / ref_mean_ : 1.0;
	gains_.push_back(g);
	const double inv = 1.0 / g;
	frames_++;
	const double k = (double)frames_;
	for (size_t i = 0; i < n; i++) {
		if (!valid_[i])
			continue;
		const double v = luma[i] * inv;
		const double d = v - mean_[i];
		mean_[i] += d / k;
		m2_[i] += d * (v - mean_[i]);
	}
}

double NoiseAnalysis::gain_std() const
{
	if (gains_.size() < 2)
		return 0;
	double s = 0, s2 = 0;
	for (double g : gains_) {
		s += g;
		s2 += g * g;
	}
	const double n = (double)gains_.size();
	const double mu = s / n;
	return std::sqrt(std::max(0.0, s2 / n - mu * mu)) / mu;
}

double NoiseAnalysis::gain_range() const
{
	if (gains_.empty())
		return 0;
	const auto mm = std::minmax_element(gains_.begin(), gains_.end());
	return *mm.second / *mm.first - 1.0;
}

std::vector<NoiseBand> NoiseAnalysis::bands(int min_count) const
{
	std::vector<NoiseBand> out;
	if (frames_ < 2)
		return out;
	std::vector<std::vector<double>> sig(kBandCount), lum(kBandCount);
	for (size_t i = 0; i < mean_.size(); i++) {
		if (!valid_[i])
			continue;
		const int b = band_index(mean_[i]);
		if (b < 0)
			continue;
		sig[(size_t)b].push_back(std::sqrt(m2_[i] / (frames_ - 1)));
		lum[(size_t)b].push_back(mean_[i]);
	}
	for (int b = 0; b < kBandCount; b++) {
		const int c = (int)sig[(size_t)b].size();
		if (c < min_count)
			continue;
		NoiseBand nb;
		nb.count = c;
		nb.sigma = median(sig[(size_t)b]);
		nb.nits = median(lum[(size_t)b]);
		out.push_back(nb);
	}
	return out;
}

double NoiseAnalysis::outlier_fraction() const
{
	if (frames_ < 2)
		return 0;
	std::vector<std::vector<double>> sig(kBandCount);
	for (size_t i = 0; i < mean_.size(); i++) {
		const int b = valid_[i] ? band_index(mean_[i]) : -1;
		if (b >= 0)
			sig[(size_t)b].push_back(std::sqrt(m2_[i] / (frames_ - 1)));
	}
	size_t total = 0, out = 0;
	for (auto &v : sig) {
		if (v.empty())
			continue;
		std::vector<double> tmp = v;
		const double med = median(tmp);
		for (double s : v)
			out += s > 4.0 * med;
		total += v.size();
	}
	return total ? (double)out / (double)total : 0;
}

double band_sigma_f(const NoiseBand &b, double knee)
{
	return b.sigma / ((b.nits + knee) * std::log(2.0));
}

NoiseFit fit_knee(const std::vector<NoiseBand> &bands)
{
	NoiseFit f;
	std::vector<NoiseBand> use;
	for (const NoiseBand &b : bands)
		if (b.sigma > 0 && b.nits > 0)
			use.push_back(b);
	f.bands = (int)use.size();
	if (use.size() < 3) {
		f.why = "fewer than 3 brightness bands with enough pixels";
		return f;
	}
	f.nits_lo = use.front().nits;
	f.nits_hi = use.back().nits;
	if (f.nits_hi < 4.0 * f.nits_lo) {
		f.why = "brightness range under 2 stops: the floor cannot be separated from the proportional part";
		return f;
	}
	auto spread = [&](double k) {
		if (use.empty())
			return 0.0;
		double lo = 1e300, hi = 0;
		for (const NoiseBand &b : use) {
			const double s = band_sigma_f(b, k);
			lo = std::min(lo, s);
			hi = std::max(hi, s);
		}
		return hi / lo;
	};
	auto cost = [&](double k) {
		double sw = 0, s1 = 0, s2 = 0;
		for (const NoiseBand &b : use) {
			const double w = std::sqrt((double)b.count);
			const double v = std::log(b.sigma / (b.nits + k));
			sw += w;
			s1 += w * v;
			s2 += w * v * v;
		}
		const double mu = s1 / sw;
		return s2 / sw - mu * mu;
	};
	auto search = [&]() {
		double best_k = kMeterKneeMin, best = 1e300;
		const double l0 = std::log10(kMeterKneeMin), l1 = std::log10(kMeterKneeMax);
		for (int i = 0; i <= 400; i++) {
			const double k = std::pow(10.0, l0 + (l1 - l0) * i / 400.0);
			const double c = cost(k);
			if (c < best) {
				best = c;
				best_k = k;
			}
		}
		return best_k;
	};
	double best_k = search();
	// Robust pass: bands more than 1.6x off the fitted level (motion, a light source, an
	// edge-only band) are dropped and the knee is refitted once.
	{
		double sw = 0, s1 = 0;
		for (const NoiseBand &b : use) {
			const double w = std::sqrt((double)b.count);
			sw += w;
			s1 += w * std::log(band_sigma_f(b, best_k));
		}
		const double mu = s1 / sw;
		std::vector<NoiseBand> keep;
		for (const NoiseBand &b : use)
			if (std::fabs(std::log(band_sigma_f(b, best_k)) - mu) <= std::log(1.6))
				keep.push_back(b);
		if (keep.size() >= 3 && keep.size() < use.size() && keep.back().nits >= 4.0 * keep.front().nits) {
			f.bands_dropped = (int)(use.size() - keep.size());
			use = keep;
			best_k = search();
		}
	}
	f.bands = (int)use.size();
	f.nits_lo = use.front().nits;
	f.nits_hi = use.back().nits;
	f.knee = best_k;
	f.knee_at_limit = best_k <= kMeterKneeMin * 1.0001 || best_k >= kMeterKneeMax * 0.9999;
	f.spread_fit = spread(best_k);
	f.spread_default = spread(0.1);
	double sw = 0, s1 = 0;
	for (const NoiseBand &b : use) {
		const double w = std::sqrt((double)b.count);
		sw += w;
		s1 += w * band_sigma_f(b, best_k);
	}
	f.sigma_f = s1 / sw;
	f.suggested_temporal = std::round(2.0 * kHalfNoiseRatio * f.sigma_f / 0.01) / 2.0;
	f.ok = true;
	return f;
}

std::string noise_report(const std::vector<NoiseBand> &bands, const NoiseFit &fit, double gain_std, double gain_range,
			 double outliers, int frames, size_t pixels)
{
	std::string r;
	char line[256];
	snprintf(line, sizeof(line),
		 "%d frames x %zu grid pixels; global gain std %.2f%%, range %.2f%%; motion outliers %.1f%%\n", frames,
		 pixels, 100.0 * gain_std, 100.0 * gain_range, 100.0 * outliers);
	r += line;
	snprintf(line, sizeof(line), "%10s %10s %8s %9s %9s %8s\n", "nits", "sigma", "sigma%", "sF(0.1)",
		 fit.ok ? "sF(fit)" : "-", "pixels");
	r += line;
	for (const NoiseBand &b : bands) {
		snprintf(line, sizeof(line), "%10.3f %10.4f %7.2f%% %9.4f %9.4f %8d\n", b.nits, b.sigma,
			 100.0 * b.sigma / b.nits, band_sigma_f(b, 0.1), fit.ok ? band_sigma_f(b, fit.knee) : 0.0,
			 b.count);
		r += line;
	}
	if (fit.ok)
		snprintf(line, sizeof(line),
			 "fit: noise floor (knee) %.2f nits%s; %d bands used, %d dropped as inconsistent; sigma_F "
			 "spread %.2fx (default knee 0.1: %.2fx); sigma_F %.4f; temporal strength for ~half the "
			 "noise left about %.1f",
			 fit.knee, fit.knee_at_limit ? " (at search limit)" : "", fit.bands, fit.bands_dropped,
			 fit.spread_fit, fit.spread_default, fit.sigma_f, fit.suggested_temporal);
	else
		snprintf(line, sizeof(line), "no fit: %s", fit.why.c_str());
	r += line;
	return r;
}

std::string noise_summary(const NoiseFit &fit, double gain_range, double outliers)
{
	char s[512];
	if (!fit.ok) {
		snprintf(s, sizeof(s), "No result: %s.", fit.why.c_str());
		return s;
	}
	snprintf(s, sizeof(s),
		 "Noise floor about %.1f nits%s, from %d brightness bands %.2f-%.0f nits. "
		 "Noise uniformity: %.1fx at this knee vs %.1fx at the default 0.1. "
		 "Temporal strength for about half the noise: %.1f.",
		 fit.knee, fit.knee_at_limit ? " (search limit)" : "", fit.bands, fit.nits_lo, fit.nits_hi,
		 fit.spread_fit, fit.spread_default, fit.suggested_temporal);
	std::string r = s;
	if (gain_range > 0.02)
		r += " Warning: brightness changed by more than 2% during the measurement (exposure or lighting).";
	if (outliers > 0.05)
		r += " Warning: many moving pixels; repeat on a static shot.";
	return r;
}

} // namespace denoise
} // namespace hdrtk
