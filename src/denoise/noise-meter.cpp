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

#include "noise-meter.hpp"

#include "denoise-core.hpp"
#include "hqdn3d-math.hpp"
#include "shared/obs-color-context.hpp"

#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hdrtk {
namespace denoise {

void NoiseMeter::free()
{
	gs_texture_destroy(grid_);
	grid_ = nullptr;
	for (gs_stagesurf_t *&s : stage_) {
		gs_stagesurface_destroy(s);
		s = nullptr;
	}
	gw_ = gh_ = 0;
	running_ = false;
}

std::string NoiseMeter::summary() const
{
	std::lock_guard<std::mutex> lock(m_);
	return summary_;
}

bool NoiseMeter::result(double *knee) const
{
	std::lock_guard<std::mutex> lock(m_);
	if (ok_ && knee)
		*knee = knee_;
	return ok_;
}

void NoiseMeter::fail(const std::string &why, const std::function<void()> &on_done)
{
	obs_log(LOG_WARNING, "%s noise measurement failed: %s", tag_.c_str(), why.c_str());
	{
		std::lock_guard<std::mutex> lock(m_);
		summary_ = "Measurement failed: " + why;
		ok_ = false;
	}
	free();
	if (on_done)
		on_done();
}

void NoiseMeter::sample(gs_texture_t *input, enum gs_color_space space, const std::function<void()> &on_done)
{
	if (!input)
		return;
	const uint32_t w = gs_texture_get_width(input), h = gs_texture_get_height(input);

	if (requested_.exchange(false)) {
		free();
		if (space == GS_CS_SRGB) {
			fail("the input is 8-bit SDR; measure on an HDR (or 16-bit linear) canvas", on_done);
			return;
		}
		step_ = std::max(1, (int)std::ceil(std::sqrt((double)w * h / (double)kTargetSamples)));
		gw_ = w / (uint32_t)step_;
		gh_ = h / (uint32_t)step_;
		in_w_ = w;
		in_h_ = h;
		grid_ = gs_texture_create(gw_, gh_, GS_RGBA32F, 1, nullptr, GS_RENDER_TARGET);
		bool ok = grid_ != nullptr && gw_ > 0 && gh_ > 0;
		for (gs_stagesurf_t *&s : stage_) {
			s = gs_stagesurface_create(gw_, gh_, GS_RGBA32F);
			ok = ok && s;
		}
		if (!ok) {
			fail("could not create the sampling textures", on_done);
			return;
		}
		npu_ = hdrtk::nits_per_unit(space);
		an_.reset((size_t)gw_ * gh_);
		luma_.assign((size_t)gw_ * gh_, 0.0f);
		submitted_ = consumed_ = 0;
		running_ = true;
		{
			std::lock_guard<std::mutex> lock(m_);
			summary_ = "Measuring...";
			ok_ = false;
		}
		obs_log(LOG_INFO,
			"%s noise measurement started: %u x %u input, grid step %d (%u x %u samples), %d frames, "
			"%.0f nits per unit",
			tag_.c_str(), w, h, step_, gw_, gh_, kFrames, npu_);
	}
	if (!running_)
		return;
	if (w != in_w_ || h != in_h_) {
		fail("the input size changed during the measurement", on_done);
		return;
	}

	// read back the oldest staged frame (staged two frames ago: no stall)
	const int outstanding = submitted_ - consumed_;
	if (outstanding > 0 && (outstanding >= kRing - 1 || submitted_ == kFrames)) {
		gs_stagesurf_t *s = stage_[consumed_ % kRing];
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(s, &data, &linesize)) {
			fail("could not read the staged frame", on_done);
			return;
		}
		for (uint32_t y = 0; y < gh_; y++) {
			const float *row = reinterpret_cast<const float *>(data + (size_t)y * linesize);
			for (uint32_t x = 0; x < gw_; x++) {
				const float *p = row + 4 * x;
				const double yl = kLumaR * p[0] + kLumaG * p[1] + kLumaB * p[2];
				luma_[(size_t)y * gw_ + x] = p[3] >= 0.99f ? (float)(yl * npu_) : NAN;
			}
		}
		gs_stagesurface_unmap(s);
		an_.add_frame(luma_.data(), luma_.size());
		consumed_++;
	}
	if (submitted_ < kFrames) {
		if (!sample_grid(input, grid_, step_, space)) {
			fail("the sampling pass failed", on_done);
			return;
		}
		gs_stage_texture(stage_[submitted_ % kRing], grid_);
		submitted_++;
	}
	if (consumed_ == kFrames)
		finish(on_done);
}

void NoiseMeter::finish(const std::function<void()> &on_done)
{
	const std::vector<NoiseBand> bands = an_.bands();
	const NoiseFit fit = fit_knee(bands);
	const double gstd = an_.gain_std(), grange = an_.gain_range(), outl = an_.outlier_fraction();
	const std::string report = noise_report(bands, fit, gstd, grange, outl, an_.frames(), an_.pixels());
	obs_log(LOG_INFO, "%s noise measurement result:", tag_.c_str());
	std::istringstream lines(report);
	for (std::string line; std::getline(lines, line);)
		obs_log(LOG_INFO, "%s   %s", tag_.c_str(), line.c_str());
	{
		std::lock_guard<std::mutex> lock(m_);
		summary_ = noise_summary(fit, grange, outl);
		ok_ = fit.ok;
		knee_ = fit.knee;
	}
	free();
	an_.reset(0);
	luma_.clear();
	luma_.shrink_to_fit();
	if (on_done)
		on_done();
}

} // namespace denoise
} // namespace hdrtk
