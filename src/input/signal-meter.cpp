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

#include "signal-meter.hpp"

#include "denoise/denoise-core.hpp"

#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hdrtk {
namespace input {

void SignalMeter::request(const AnalysisSetup &setup)
{
	{
		std::lock_guard<std::mutex> lock(setup_m_);
		pending_ = setup;
	}
	requested_ = true;
}

void SignalMeter::free()
{
	gs_texture_destroy(grid_);
	grid_ = nullptr;
	for (gs_stagesurf_t *&s : stage_) {
		gs_stagesurface_destroy(s);
		s = nullptr;
	}
	running_ = false;
}

std::string SignalMeter::summary() const
{
	std::lock_guard<std::mutex> lock(m_);
	return summary_;
}

void SignalMeter::fail(const std::string &why, const std::function<void()> &on_done)
{
	obs_log(LOG_WARNING, "%s signal analysis failed: %s", tag_.c_str(), why.c_str());
	{
		std::lock_guard<std::mutex> lock(m_);
		summary_ = "Analysis failed: " + why;
	}
	free();
	if (on_done)
		on_done();
}

void SignalMeter::sample(gs_texture_t *input, enum gs_color_space space, const std::function<void()> &on_done)
{
	if (!input)
		return;
	const uint32_t w = gs_texture_get_width(input), h = gs_texture_get_height(input);
	if (requested_.exchange(false)) {
		free();
		{
			std::lock_guard<std::mutex> lock(setup_m_);
			setup_ = pending_;
		}
		switch (space) {
		case GS_CS_SRGB:
			decoded_ = Decoded::Sdr8; // sampled without sRGB decode: the R'G'B' values
			break;
		case GS_CS_SRGB_16F:
			decoded_ = Decoded::SdrFloat;
			break;
		case GS_CS_709_EXTENDED:
			decoded_ = Decoded::Hlg; // assumed: OBS's HLG path (BT.2020 matrix)
			break;
		default:
			fail("unsupported input colour space (scRGB)", on_done);
			return;
		}
		step_ = std::max(1, (int)std::ceil(std::sqrt((double)w * h / (double)kTargetSamples)));
		gw_ = w / (uint32_t)step_;
		gh_ = h / (uint32_t)step_;
		in_w_ = w;
		in_h_ = h;
		grid_ = gs_texture_create(gw_, gh_, GS_RGBA32F, 1, nullptr, GS_RENDER_TARGET);
		bool ok = grid_ && gw_ && gh_;
		for (gs_stagesurf_t *&s : stage_) {
			s = gs_stagesurface_create(gw_, gh_, GS_RGBA32F);
			ok = ok && s;
		}
		if (!ok) {
			fail("could not create the sampling textures", on_done);
			return;
		}
		an_.reset(setup_.limited);
		submitted_ = consumed_ = 0;
		running_ = true;
		{
			std::lock_guard<std::mutex> lock(m_);
			summary_ = "Analysing...";
		}
		obs_log(LOG_INFO, "%s signal analysis started: %ux%u, grid step %d, %d frames, input %s", tag_.c_str(),
			w, h, step_, kFrames,
			decoded_ == Decoded::Hlg ? "HLG-decoded"
						 : (decoded_ == Decoded::Sdr8 ? "8-bit SDR" : "SDR-decoded"));
	}
	if (!running_)
		return;
	if (w != in_w_ || h != in_h_) {
		fail("the input size changed during the analysis", on_done);
		return;
	}
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
				if (p[3] < 0.99f)
					continue;
				an_.add(recover_luma(decoded_, setup_.matrix, setup_.hlg, p[0], p[1], p[2]));
			}
		}
		gs_stagesurface_unmap(s);
		consumed_++;
	}
	if (submitted_ < kFrames) {
		if (!hdrtk::denoise::sample_grid(input, grid_, step_, space)) {
			fail("the sampling pass failed", on_done);
			return;
		}
		gs_stage_texture(stage_[submitted_ % kRing], grid_);
		submitted_++;
	}
	if (consumed_ == kFrames)
		finish(on_done);
}

void SignalMeter::finish(const std::function<void()> &on_done)
{
	const DepthResult r = an_.finish();
	const std::string report = depth_report(r, setup_.limited);
	obs_log(LOG_INFO, "%s signal analysis result:", tag_.c_str());
	std::istringstream lines(report);
	for (std::string line; std::getline(lines, line);)
		obs_log(LOG_INFO, "%s   %s", tag_.c_str(), line.c_str());
	{
		std::lock_guard<std::mutex> lock(m_);
		summary_ = depth_summary(r, setup_.limited);
	}
	free();
	an_.reset(setup_.limited);
	if (on_done)
		on_done();
}

} // namespace input
} // namespace hdrtk
