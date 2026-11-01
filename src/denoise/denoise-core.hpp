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
 * HDR denoise core (docs/denoise/PLACEMENT.md): the HQDN3D-style spatial + temporal
 * processing, its history, telemetry, debug views and the shared settings model. Two
 * front-ends use it unchanged:
 *   program-denoise.cpp - final program frame (main-rendered hook)
 *   denoise-filter.cpp  - per-source filter
 * Graphics thread only, except the settings helpers.
 */

#include "hqdn3d-math.hpp"
#include "noise-profile.hpp"
#include "spatial-math.hpp"

#include <obs-module.h>

#include <string>
#include <vector>

namespace hdrtk {
namespace denoise {

enum DebugView {
	ViewNormal = 0,
	ViewDifference = 1,
	ViewHistory = 2,
	ViewMetric = 3,
	ViewExact = 4,
	ViewWeight = 5,
	ViewRemovedNoise = 6, // |F(out) - F(in)| luma, comparison domain: noise-normalised
};

enum Placement { PlacementProgram = 0, PlacementSource = 1 };

constexpr double kDebugGainMax = 1024.0;
constexpr double kKneeMin = 0.01, kKneeMax = 50.0; // comparison knee UI range, nits

// The shared parameter model (same obs_data keys in both placements).
struct CoreSettings {
	Params p;
	SpatialParams sp;
	bool force_spatial = false; // development: run the spatial passes even at strength 0
	int noise_profile = ProfileIdentity;
	double noise_profile_max = 3.0;
	double mix = 1.0;
	int debug_view = ViewNormal;
	double debug_gain = 16.0;
	bool log_counters = false;

	bool spatial_on() const { return sp.luma > 0 || sp.chroma > 0 || force_spatial; }
	// The metric + temporal passes are needed: temporal strength set, a view that shows
	// temporal state, or the development forced-pass mode (identity coverage). Otherwise
	// (e.g. the hybrid's spatial-only program instance) they are skipped.
	bool temporal_passes() const
	{
		return p.temporal_luma > 0 || p.temporal_chroma > 0 || force_spatial || debug_view == ViewHistory ||
		       debug_view == ViewMetric || debug_view == ViewWeight;
	}
	// all strengths zero, no forced passes, no debug view: the frame would pass unchanged
	bool neutral() const
	{
		return p.temporal_luma <= 0 && p.temporal_chroma <= 0 && !spatial_on() && debug_view == ViewNormal;
	}
};

void core_defaults(obs_data_t *s, Placement placement);
CoreSettings core_parse(obs_data_t *data);
// Adds mix, the HQDN3D group, the diagnostics group (view, gain, logging) and the
// development group (radius, forced passes, noise profile). Returns the development
// group so a front-end can append its own items; `dbg` receives the diagnostics group.
obs_properties_t *core_properties(obs_properties_t *props, Placement placement, obs_properties_t **dbg);

// Shared effect (data/effects/hdr-program-denoise.effect), loaded on first use.
gs_effect_t *core_effect();
void core_effect_free(); // module unload, graphics context

// Point-sample `input` into `grid` (RGBA32F, input size / step): grid pixel (x, y) =
// input pixel (x * step + step / 2, y * step + step / 2). For noise measurement.
bool sample_grid(gs_texture_t *input, gs_texture_t *grid, int step, enum gs_color_space space);

constexpr int kTelemetryRing = 4;
struct Telemetry {
	gs_timer_range_t *range[kTelemetryRing] = {};
	gs_timer_t *timer[kTelemetryRing] = {};
	gs_stagesurf_t *stage[kTelemetryRing] = {};
	bool pending[kTelemetryRing] = {};
	bool staged[kTelemetryRing] = {};
	int idx = 0;
	std::vector<double> ms;
	double m_sum = 0, m_max = 0;
	int m_n = 0;
};
void telemetry_free(Telemetry &t);
int telemetry_begin(Telemetry &t, bool on);
void telemetry_end(Telemetry &t, int slot, bool on, gs_texture_t *metric);
// Reads the oldest slot; returns true if a cut was seen there (for counting).
bool telemetry_collect(Telemetry &t, double m_cut, bool cut_enabled);
std::string timing_summary(Telemetry &t); // consumes the collected timings
std::string metric_summary(Telemetry &t); // consumes the collected metrics

const char *format_name(enum gs_color_format f);

class Core {
public:
	explicit Core(std::string tag) : tag_(std::move(tag)) {}
	Core(const Core &) = delete;
	Core &operator=(const Core &) = delete;

	// Denoise `input` into `output` (Mix and debug views applied). input == output is
	// allowed (program placement: the frame is copied first). output == nullptr writes
	// into the core's own output texture (source placement; see last_output()).
	// Returns false when resources are missing; nothing is drawn then.
	bool process(gs_texture_t *input, gs_texture_t *output, enum gs_color_space space, const CoreSettings &s);
	gs_texture_t *last_output() const { return out_; }

	// Program-placement helpers.
	bool draw_identity(gs_texture_t *frame, enum gs_color_space space, const CoreSettings &s);
	gs_texture_t *copy_input(gs_texture_t *frame); // into the internal copy (created on demand)
	gs_texture_t *scratch(gs_texture_t *like);     // spare texture (created on demand)
	// Output pass (Mix / debug views) of `filt` against `image`, into `target`.
	void draw_output(gs_texture_t *image, gs_texture_t *filt, gs_texture_t *target, enum gs_color_space space,
			 const CoreSettings &s);

	void invalidate() { history_valid_ = false; }
	void free_all();     // graphics context
	void free_spatial(); // release the spatial intermediates
	void collect_telemetry(const CoreSettings &s);

	// counters (graphics thread writes, logging reads)
	// history_updates counts processed frames (one per dispatch); temporal_skipped counts
	// those processed without the metric/temporal passes.
	uint64_t history_updates = 0, history_resets = 0, spatial_passes = 0, cut_resets = 0, temporal_skipped = 0;
	Telemetry tel;
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	double vram_mib() const;

private:
	bool ensure(gs_texture_t *like, bool spatial, bool precise_history);
	gs_texture_t *out_texture(gs_texture_t *like);
	struct Frame;
	void set_common(const Frame &f, uint32_t w, uint32_t h);
	void run_pass(gs_texture_t *target, enum gs_color_space space, const char *tech);

	std::string tag_;
	gs_texture_t *cur_ = nullptr; // copy of the input (program placement)
	gs_texture_t *hist_[2] = {nullptr, nullptr};
	gs_texture_t *l1_ = nullptr, *l2_ = nullptr;
	gs_texture_t *metric_[2] = {nullptr, nullptr};
	gs_texture_t *sp_tmp_ = nullptr, *sp_out_ = nullptr;
	gs_texture_t *scratch_ = nullptr, *out_ = nullptr;
	int hi_ = 0, mi_ = 0;
	bool history_valid_ = false;
	uint32_t width_ = 0, height_ = 0;
	enum gs_color_format format_ = GS_UNKNOWN;
	enum gs_color_format hist_format_ = GS_UNKNOWN; // format_, or RGBA32F above strength 20
};

} // namespace denoise
} // namespace hdrtk
