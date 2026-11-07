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

// HDR Input signal analysis on the GPU side (docs/HDR_INPUT.md): kFrames frames of the
// filter input, point-sampled on a sparse grid (denoise core SampleGrid, RGBA32F),
// staged with two frames of latency, luma code values rebuilt and tested by
// input-math.cpp SignalAnalysis.

#include "input-math.hpp"

#include <obs-module.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace hdrtk {
namespace input {

struct AnalysisSetup {
	Matrix matrix = Bt2020; // matrix the capture source was decoded with
	bool limited = true;
	HlgParams hlg;
};

class SignalMeter {
public:
	static constexpr int kFrames = 30;
	static constexpr int kRing = 3;
	static constexpr uint32_t kTargetSamples = 300000;

	explicit SignalMeter(std::string tag) : tag_(std::move(tag)) {}
	SignalMeter(const SignalMeter &) = delete;
	SignalMeter &operator=(const SignalMeter &) = delete;

	void request(const AnalysisSetup &setup); // any thread
	bool active() const { return running_ || requested_; }
	// Graphics thread, once per unique video frame while active().
	void sample(gs_texture_t *input, enum gs_color_space space, const std::function<void()> &on_done);
	void free(); // graphics context
	std::string summary() const;

private:
	void fail(const std::string &why, const std::function<void()> &on_done);
	void finish(const std::function<void()> &on_done);

	std::string tag_;
	std::atomic<bool> requested_{false};
	std::mutex setup_m_;
	AnalysisSetup pending_;
	AnalysisSetup setup_;
	Decoded decoded_ = Decoded::SdrFloat;
	bool running_ = false;
	gs_texture_t *grid_ = nullptr;
	gs_stagesurf_t *stage_[kRing] = {};
	uint32_t gw_ = 0, gh_ = 0, in_w_ = 0, in_h_ = 0;
	int step_ = 1, submitted_ = 0, consumed_ = 0;
	SignalAnalysis an_;

	mutable std::mutex m_;
	std::string summary_;
};

} // namespace input
} // namespace hdrtk
