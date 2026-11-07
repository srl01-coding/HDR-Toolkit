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

/*
 * HDR Denoise - source-filter placement (docs/denoise/PLACEMENT.md).
 *
 * The same denoise core as HDR Program Denoise, attached to one camera/source:
 *   input  : the filter's input rendered into a private texrender in the source's
 *            native space (RGBA16F on HDR), exactly as libobs renders a filter input
 *   core   : spatial (when on) -> temporal -> Output (Mix, debug views) into `out`
 *   output : `out` drawn with the default effect, premultiplied, ONE / INVSRCALPHA
 *
 * Once per video frame: OBS may render a source several times per frame (program,
 * preview, multiview, projectors). The first render of a frame processes; later ones
 * redraw `out`, so the history advances exactly once per frame.
 *
 * Continuity: if this source was not rendered for more than 2.5 frame intervals, its
 * history is stale; it is reset and the current frame passes through.
 */

#include "denoise-core.hpp"
#include "noise-meter.hpp"

#include "shared/filter-io.hpp"
#include "shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

namespace dn = hdrtk::denoise;

constexpr const char *kFilterId = "hdr_toolkit_denoise_filter_v1"; // development schema
constexpr uint64_t kLogIntervalNs = 10ull * 1000000000ull;
constexpr double kContinuityFrames = 2.5;

struct DenoiseFilter {
	obs_source_t *context = nullptr;

	std::mutex mutex; // guards settings
	dn::CoreSettings settings;
	std::atomic<bool> reset_history{false};
	std::atomic<bool> log_now{false};
	std::atomic<bool> reset_counters{false};

	// graphics thread only
	dn::Core core;
	dn::NoiseMeter meter;
	hdrtk::FilterInput input;
	bool have_ctx = false;
	hdrtk::RenderContext last_ctx;
	bool have_time = false;
	uint64_t last_time = 0;
	bool out_valid = false;
	gs_texture_t *last_out = nullptr; // owned by core; valid while out_valid
	uint64_t last_log_ns = 0;
	bool warned = false;

	// counters
	uint64_t renders = 0, unique_frames = 0, reused = 0, dispatches = 0, continuity_resets = 0, failures = 0,
		 passthrough = 0, measure_only = 0;

	explicit DenoiseFilter(obs_source_t *src)
		: context(src),
		  core(std::string("[denoise:") + obs_source_get_name(src) + "]"),
		  meter(std::string("[denoise:") + obs_source_get_name(src) + "]")
	{
	}
};

const char *filter_name(void *)
{
	return obs_module_text("DenoiseFilter.Name");
}

uint64_t frame_interval_ns()
{
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.fps_num)
		return 33333333ull;
	return (uint64_t)(1000000000.0 * (double)ovi.fps_den / (double)ovi.fps_num);
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<DenoiseFilter *>(data);
	const dn::CoreSettings s = dn::core_parse(settings);
	std::lock_guard<std::mutex> lock(f->mutex);
	f->settings = s;
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new DenoiseFilter(source);
	filter_update(f, settings);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<DenoiseFilter *>(data);
	obs_enter_graphics();
	f->core.free_all();
	f->meter.free();
	f->input.destroy();
	obs_leave_graphics();
	delete f;
}

void filter_defaults(obs_data_t *s)
{
	dn::core_defaults(s, dn::PlacementSource);
}

bool reset_history_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<DenoiseFilter *>(data)->reset_history = true;
	return false;
}

bool log_now_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<DenoiseFilter *>(data)->log_now = true;
	return false;
}

bool measure_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<DenoiseFilter *>(data)->meter.request();
	return false;
}

// Applies the measured noise floor: comparison knee = fitted K (clamped to the UI range)
// and the identity noise profile (the measured T(Y) profile assumes K = 0.1 and would
// compensate the darks twice).
bool apply_measured_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<DenoiseFilter *>(data);
	double k = 0;
	if (!f->meter.result(&k))
		return false;
	k = std::clamp(k, dn::kKneeMin, dn::kKneeMax);
	obs_data_t *s = obs_source_get_settings(f->context);
	obs_data_set_double(s, "comparison_knee_nits", std::round(k * 100.0) / 100.0);
	obs_data_set_int(s, "noise_profile", dn::ProfileIdentity);
	obs_source_update(f->context, s);
	obs_data_release(s);
	obs_log(LOG_INFO,
		"[denoise:%s] applied measured noise floor: comparison knee %.2f nits, identity noise profile",
		obs_source_get_name(f->context), k);
	return true;
}

void refresh_properties_task(void *param)
{
	obs_weak_source_t *weak = static_cast<obs_weak_source_t *>(param);
	obs_source_t *src = obs_weak_source_get_source(weak);
	if (src) {
		obs_source_update_properties(src);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
}

bool reset_counters_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<DenoiseFilter *>(data)->reset_counters = true;
	return false;
}

obs_properties_t *filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("DenoiseFilter.Info"), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "reset_history", obs_module_text("Denoise.ResetHistory"),
				   reset_history_clicked, data);

	auto *f = static_cast<DenoiseFilter *>(data);
	obs_properties_t *nm = obs_properties_create();
	const std::string res = f ? f->meter.summary() : std::string();
	obs_properties_add_text(nm, "noise_measure_info", obs_module_text("DenoiseFilter.Measure.Info"), OBS_TEXT_INFO);
	obs_properties_add_text(nm, "noise_measure_result",
				res.empty() ? obs_module_text("DenoiseFilter.Measure.None") : res.c_str(),
				OBS_TEXT_INFO);
	obs_properties_add_button2(nm, "noise_measure", obs_module_text("DenoiseFilter.Measure.Start"), measure_clicked,
				   data);
	obs_property_t *apply = obs_properties_add_button2(nm, "noise_measure_apply",
							   obs_module_text("DenoiseFilter.Measure.Apply"),
							   apply_measured_clicked, data);
	obs_property_set_enabled(apply, f && f->meter.result(nullptr));
	obs_properties_add_group(props, "noise_measure_group", obs_module_text("DenoiseFilter.Measure.Group"),
				 OBS_GROUP_NORMAL, nm);
	obs_properties_t *dbg = nullptr;
	dn::core_properties(props, dn::PlacementSource, &dbg);
	obs_properties_add_button2(dbg, "log_now", obs_module_text("Denoise.LogNow"), log_now_clicked, data);
	obs_properties_add_button2(dbg, "reset_counters", obs_module_text("Denoise.ResetCounters"),
				   reset_counters_clicked, data);
	return props;
}

void log_counters(DenoiseFilter *f, const dn::CoreSettings &s, const char *why)
{
	const std::string timing = dn::timing_summary(f->core.tel);
	const std::string metric = dn::metric_summary(f->core.tel);
	const char *verdict = f->dispatches + f->measure_only == f->unique_frames
				      ? "dispatches (+ measurement-only frames) == unique frames: OK"
				      : "MISMATCH dispatches != unique frames";
	if (f->core.history_updates != f->dispatches)
		verdict = "MISMATCH history_updates != dispatches";
	obs_log(LOG_INFO,
		"[denoise:%s] %s: T_L %.2f T_C %.2f S_L %.2f S_C %.2f%s noise_profile=%s | renders=%" PRIu64
		" unique_frames=%" PRIu64 " reused_same_frame=%" PRIu64 " dispatches=%" PRIu64
		" history_updates=%" PRIu64 " history_resets=%" PRIu64 " temporal_skipped=%" PRIu64
		" continuity_resets=%" PRIu64 " cut_resets=%" PRIu64 " passthrough=%" PRIu64 " failures=%" PRIu64
		" | %ux%u | %s | %s | VRAM about %.0f MiB | %s",
		obs_source_get_name(f->context), why, s.p.temporal_luma, s.p.temporal_chroma, s.sp.luma, s.sp.chroma,
		s.force_spatial ? " (forced spatial)" : "",
		s.noise_profile == dn::ProfileMeasured20261005 ? "measured-2026-10-05" : "identity", f->renders,
		f->unique_frames, f->reused, f->dispatches, f->core.history_updates, f->core.history_resets,
		f->core.temporal_skipped, f->continuity_resets, f->core.cut_resets, f->passthrough, f->failures,
		f->core.width(), f->core.height(), timing.c_str(), metric.c_str(), f->core.vram_mib(), verdict);
}

void draw_result(gs_texture_t *tex)
{
	hdrtk::draw_filter_result(tex);
}

gs_texture_t *capture_input(DenoiseFilter *f, const hdrtk::RenderContext &ctx)
{
	return hdrtk::capture_filter_input(f->context, f->input, ctx);
}

void filter_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<DenoiseFilter *>(data);
	f->renders++;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	dn::CoreSettings s;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->settings;
	}
	if (f->reset_counters.exchange(false)) {
		f->renders = f->unique_frames = f->reused = f->dispatches = f->continuity_resets = f->failures =
			f->passthrough = f->measure_only = 0;
		f->core.history_updates = f->core.history_resets = f->core.cut_resets = f->core.spatial_passes =
			f->core.temporal_skipped = 0;
		f->core.tel.ms.clear();
	}

	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[denoise:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
		f->core.invalidate();
	}
	const uint64_t now = os_gettime_ns();
	if (f->log_now.exchange(false)) {
		log_counters(f, s, "on request");
	} else if (s.log_counters && now - f->last_log_ns >= kLogIntervalNs) {
		log_counters(f, s, "periodic");
		f->last_log_ns = now;
	}

	// Neutral settings: draw the input unchanged without any pass. Identity through
	// the shaders is proven with Development > "Run spatial passes at strength 0".
	const bool measuring = f->meter.active();
	if (!ctx.width || !ctx.height || (s.neutral() && !measuring)) {
		f->passthrough++;
		f->out_valid = false;
		f->have_time = false;
		f->core.invalidate(); // history from before the pause is stale
		obs_source_skip_video_filter(f->context);
		return;
	}

	const uint64_t t = obs_get_video_frame_time();
	if (f->have_time && t == f->last_time && f->out_valid) {
		f->reused++; // second render of the same frame: same result, history untouched
		draw_result(f->last_out);
		return;
	}
	if (f->reset_history.exchange(false))
		f->core.invalidate();
	if (f->have_time && t > f->last_time &&
	    (double)(t - f->last_time) > kContinuityFrames * (double)frame_interval_ns()) {
		f->core.invalidate(); // not rendered for a while: the history is stale
		f->continuity_resets++;
	}
	f->have_time = true;
	f->last_time = t;
	f->unique_frames++;

	gs_texture_t *in = capture_input(f, ctx);
	if (in && measuring) {
		obs_source_t *src = f->context;
		f->meter.sample(in, ctx.input_space, [src]() {
			// refresh the properties panel (result text, Apply button) on the UI thread
			obs_queue_task(OBS_TASK_UI, refresh_properties_task, obs_source_get_weak_source(src), false);
		});
	}
	if (in && s.neutral()) {
		// measuring with neutral settings: the input is drawn unchanged
		f->measure_only++;
		f->core.invalidate();
		f->out_valid = true;
		f->last_out = in;
		draw_result(in);
		return;
	}
	if (!in || !f->core.process(in, nullptr, ctx.input_space, s)) {
		f->failures++;
		f->out_valid = false;
		if (!f->warned) {
			obs_log(LOG_WARNING, "[denoise:%s] resources unavailable: passing the source through",
				obs_source_get_name(f->context));
			f->warned = true;
		}
		if (in)
			draw_result(in);
		else
			obs_source_skip_video_filter(f->context);
		return;
	}
	f->dispatches++;
	f->out_valid = true;
	f->last_out = f->core.last_output();
	draw_result(f->last_out);
}

enum gs_color_space filter_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<DenoiseFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_denoise_filter(void)
{
	struct obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = filter_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.video_render = filter_render;
	info.video_get_color_space = filter_color_space;
	obs_register_source(&info);
}
