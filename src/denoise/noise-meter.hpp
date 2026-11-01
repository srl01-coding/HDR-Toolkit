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
 * Live noise measurement (docs/denoise/NOISE_MEASUREMENT.md): 60 frames of the filter
 * input, point-sampled on a sparse grid on the GPU, staged to the CPU with two frames
 * of latency (no pipeline stall), analysed by noise-meter-math.cpp. Front-end agnostic.
 */

#include "noise-meter-math.hpp"

#include <obs-module.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace hdrtk {
namespace denoise {

class NoiseMeter {
public:
	static constexpr int kFrames = 60;
	static constexpr int kRing = 3;
	static constexpr uint32_t kTargetSamples = 300000;

	explicit NoiseMeter(std::string tag) : tag_(std::move(tag)) {}
	NoiseMeter(const NoiseMeter &) = delete;
	NoiseMeter &operator=(const NoiseMeter &) = delete;

	void request() { requested_ = true; } // any thread
	// graphics thread: the front-end must provide the input on every unique frame
	bool active() const { return running_ || requested_; }
	// Graphics thread, once per unique video frame while active(). `on_done` runs on the
	// graphics thread when a measurement finishes (or fails).
	void sample(gs_texture_t *input, enum gs_color_space space, const std::function<void()> &on_done);
	void free(); // graphics context

	// Results (any thread).
	std::string summary() const;
	bool result(double *knee) const; // true when a fit exists

private:
	void finish(const std::function<void()> &on_done);
	void fail(const std::string &why, const std::function<void()> &on_done);

	std::string tag_;
	std::atomic<bool> requested_{false};
	bool running_ = false;
	gs_texture_t *grid_ = nullptr;
	gs_stagesurf_t *stage_[kRing] = {};
	uint32_t gw_ = 0, gh_ = 0;
	int step_ = 1;
	int submitted_ = 0, consumed_ = 0;
	double npu_ = 1.0;
	uint32_t in_w_ = 0, in_h_ = 0;
	NoiseAnalysis an_;
	std::vector<float> luma_;

	mutable std::mutex m_;
	std::string summary_;
	bool ok_ = false;
	double knee_ = 0;
};

} // namespace denoise
} // namespace hdrtk
