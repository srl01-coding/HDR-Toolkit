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
 * HDR Program Denoise (denoise brief v1.2).
 *
 * P0 - the hook (docs/denoise/DENOISE.md). After each video mix has drawn its
 * texture, OBS (32.2.2 libobs/obs-video.c, render_main_texture) calls the
 * main-rendered callbacks with that mix's texture still bound as the render
 * target. That is once per *mix*, so the hook acts only when the bound target is
 * the main canvas texture and the video frame time is new. Previews, projectors
 * and multiview draw the main texture without calling the callback.
 *
 * P1/P2 - HQDN3D-style spatial + temporal: the shared denoise core
 * (denoise-core.cpp, docs/denoise/PLACEMENT.md), run on the main texture once per
 * unique frame. The same core serves the per-source filter (denoise-filter.cpp).
 *
 * P2 compute spike - "Compute identity (D3D11 spike)": a native D3D11 compute round
 * trip on the program texture (src/denoise/d3d11-compute.cpp), Windows only, with a
 * transparent fallback.
 *
 * Telemetry (only while counter logging is on): GPU timer queries and a 1x1 staged
 * copy of the metric, read 2-3 frames later, for timing and cut counts.
 *
 * Controls: Tools > HDR Program Denoise (private controller source); settings in
 * the plugin config folder (program-denoise.json), development schema.
 */

#include "denoise-core.hpp"
#include "d3d11-compute.hpp"
#include "nvof-probe.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

namespace dn = hdrtk::denoise;
namespace compute = hdrtk::denoise::compute;

constexpr const char *kSourceId = "hdr_toolkit_program_denoise_v1";
constexpr const char *kConfigFile = "program-denoise.json";
constexpr int kSchemaVersion = 1; // development schema (brief 2.7): no migrations yet
constexpr uint64_t kLogIntervalNs = 10ull * 1000000000ull;

enum Algorithm { AlgoOff = 0, AlgoIdentity = 1, AlgoHqdn3d = 2, AlgoComputeIdentity = 3 };

struct Counters {
	std::atomic<uint64_t> callbacks{0};          // every main-rendered callback (all mixes)
	std::atomic<uint64_t> other_mix_skipped{0};  // render target was not the main texture
	std::atomic<uint64_t> unique_frames{0};      // distinct main-mix frames (program_frames_seen)
	std::atomic<uint64_t> duplicates_skipped{0}; // same frame time again on the main texture
	std::atomic<uint64_t> dispatches{0};         // processing passes drawn into the main texture
	std::atomic<uint64_t> failures{0};           // frames passed through for lack of resources
};

struct Settings {
	int algorithm = AlgoOff;
	int compute_variant = compute::VariantTwoCopies;
	dn::CoreSettings core;
};

struct Denoise {
	obs_source_t *controller = nullptr;

	std::mutex mutex; // guards settings
	Settings settings;
	std::atomic<bool> log_now{false};
	std::atomic<bool> reset_counters{false};
	std::atomic<bool> reset_history{false};

	Counters c;

	// graphics thread only
	dn::Core core{"[denoise]"};
	compute::Spike *spike = nullptr;
	std::string compute_fail_reason;
	int last_algo = AlgoOff;
	bool logged_canvas = false;
	bool have_frame_time = false;
	uint64_t last_frame_time = 0;
	uint64_t last_log_ns = 0;
	uint32_t total_frames_at_reset = 0;
	uint32_t lagged_frames_at_reset = 0;
	bool warned = false;
};

Denoise *g = nullptr;

const char *colorspace_name(enum video_colorspace cs)
{
	switch (cs) {
	case VIDEO_CS_2100_HLG:
		return "Rec.2100 HLG";
	case VIDEO_CS_2100_PQ:
		return "Rec.2100 PQ";
	case VIDEO_CS_709:
		return "Rec.709";
	case VIDEO_CS_SRGB:
		return "sRGB";
	case VIDEO_CS_601:
		return "Rec.601";
	default:
		return "default";
	}
}

const char *algo_name(int a)
{
	switch (a) {
	case AlgoIdentity:
		return "identity";
	case AlgoHqdn3d:
		return "hqdn3d";
	case AlgoComputeIdentity:
		return "compute-identity";
	default:
		return "off";
	}
}

void log_counters(Denoise *d, const char *why)
{
	const uint32_t total = obs_get_total_frames() - d->total_frames_at_reset;
	const uint32_t lagged = obs_get_lagged_frames() - d->lagged_frames_at_reset;
	const uint64_t unique = d->c.unique_frames.load();
	const uint64_t disp = d->c.dispatches.load();
	Settings s;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		s = d->settings;
	}
	const int algo = s.algorithm;
	char detail[320] = "";
	if (algo == AlgoHqdn3d)
		snprintf(detail, sizeof(detail),
			 " T_L %.2f T_C %.2f spatial S_L %.2f S_C %.2f R %d%s spatial_passes=%" PRIu64
			 " temporal_skipped=%" PRIu64 " noise_profile=%s (max %.1f)",
			 s.core.p.temporal_luma, s.core.p.temporal_chroma, s.core.sp.luma, s.core.sp.chroma,
			 s.core.sp.radius, s.core.force_spatial ? " (forced)" : "", d->core.spatial_passes,
			 d->core.temporal_skipped,
			 s.core.noise_profile == dn::ProfileMeasured20261005 ? "measured-2026-10-05" : "identity",
			 s.core.noise_profile_max);
	else if (algo == AlgoComputeIdentity)
		snprintf(detail, sizeof(detail), " compute_variant=%s",
			 s.compute_variant == compute::VariantOneCopy    ? "one-copy"
			 : s.compute_variant == compute::VariantDeferred ? "deferred-context"
									 : "two-copies");
	const std::string timing = dn::timing_summary(d->core.tel);
	const std::string metric = dn::metric_summary(d->core.tel);

	const char *verdict = "off (no dispatch expected)";
	if (algo != AlgoOff)
		verdict = disp == unique ? "dispatches == unique frames: OK" : "MISMATCH dispatches != unique frames";
	if (algo == AlgoHqdn3d && d->core.history_updates != disp)
		verdict = "MISMATCH history_updates != dispatches";
	obs_log(LOG_INFO,
		"[denoise] %s: algorithm=%s%s callbacks=%" PRIu64 " other_mix_skipped=%" PRIu64
		" unique_program_frames=%" PRIu64 " duplicate_callbacks_skipped=%" PRIu64 " denoise_dispatches=%" PRIu64
		" history_updates=%" PRIu64 " history_resets=%" PRIu64 " cut_resets=%" PRIu64 " failures=%" PRIu64
		" | obs_total_frames=%u obs_lagged_frames=%u | %s | %s | VRAM about %.0f MiB | %s",
		why, algo_name(algo), detail, d->c.callbacks.load(), d->c.other_mix_skipped.load(), unique,
		d->c.duplicates_skipped.load(), disp, d->core.history_updates, d->core.history_resets,
		d->core.cut_resets, d->c.failures.load(), total, lagged, timing.c_str(), metric.c_str(),
		d->core.vram_mib(), verdict);
}

void reset_counters_now(Denoise *d)
{
	d->c.callbacks = 0;
	d->c.other_mix_skipped = 0;
	d->c.unique_frames = 0;
	d->c.duplicates_skipped = 0;
	d->c.dispatches = 0;
	d->c.failures = 0;
	d->core.history_updates = 0;
	d->core.history_resets = 0;
	d->core.cut_resets = 0;
	d->core.spatial_passes = 0;
	d->core.temporal_skipped = 0;
	d->core.tel.ms.clear();
	d->total_frames_at_reset = obs_get_total_frames();
	d->lagged_frames_at_reset = obs_get_lagged_frames();
}

void log_canvas_once(Denoise *d, gs_texture_t *main_tex)
{
	if (d->logged_canvas)
		return;
	d->logged_canvas = true;
	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	obs_log(LOG_INFO,
		"[denoise] program frame %ux%u %s, canvas colour space %s, SDR white %.0f nits, HDR nominal peak %.0f nits",
		gs_texture_get_width(main_tex), gs_texture_get_height(main_tex),
		dn::format_name(gs_texture_get_color_format(main_tex)), colorspace_name(ovi.colorspace),
		obs_get_video_sdr_white_level(), obs_get_video_hdr_nominal_peak_level());
}

// Compute identity spike. Returns false (frame untouched) when the compute path is
// unavailable. With the Difference or Exact view, the result is compared with a copy
// of the input taken before the round trip.
bool draw_compute_identity(Denoise *d, gs_texture_t *main_tex, const Settings &s)
{
	const bool compare = s.core.debug_view == dn::ViewDifference || s.core.debug_view == dn::ViewExact;
	gs_texture_t *before = nullptr, *after = nullptr;
	if (compare) {
		before = d->core.copy_input(main_tex);
		after = d->core.scratch(main_tex);
		if (!before || !after)
			return false;
	}
	d->core.collect_telemetry(s.core);
	const int slot = dn::telemetry_begin(d->core.tel, s.core.log_counters);
	const char *why = "";
	const bool ok = compute::identity(d->spike, main_tex, s.compute_variant, &why);
	dn::telemetry_end(d->core.tel, slot, s.core.log_counters, nullptr);
	if (!ok) {
		if (d->compute_fail_reason != why) {
			obs_log(LOG_WARNING, "[denoise] compute identity unavailable (%s): passing the program through",
				why);
			d->compute_fail_reason = why;
		}
		return false;
	}
	d->compute_fail_reason.clear();
	if (compare) {
		gs_copy_texture(after, main_tex);
		d->core.draw_output(before, after, main_tex, gs_get_color_space(), s.core);
	}
	return true;
}

// ---- the hook ----------------------------------------------------------------

void on_main_rendered(void *param)
{
	auto *d = static_cast<Denoise *>(param);
	d->c.callbacks++;

	if (d->reset_counters.exchange(false)) {
		reset_counters_now(d);
		obs_log(LOG_INFO, "[denoise] counters reset");
	}

	gs_texture_t *main_tex = obs_get_main_texture();
	if (!main_tex || gs_get_render_target() != main_tex) {
		d->c.other_mix_skipped++;
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (d->have_frame_time && t == d->last_frame_time) {
		d->c.duplicates_skipped++;
		return;
	}
	d->have_frame_time = true;
	d->last_frame_time = t;
	d->c.unique_frames++;

	Settings s;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		s = d->settings;
	}
	if (s.algorithm != d->last_algo) {
		if (s.algorithm == AlgoHqdn3d)
			d->core.invalidate(); // entering a temporal mode: start from the current frame
		if (d->last_algo == AlgoHqdn3d || d->last_algo == AlgoComputeIdentity)
			d->core.free_all(); // release history/spatial VRAM when not in use
		if (d->last_algo == AlgoComputeIdentity) {
			compute::destroy(d->spike);
			d->spike = nullptr;
			d->compute_fail_reason.clear();
		}
		d->last_algo = s.algorithm;
	}
	if (d->reset_history.exchange(false))
		d->core.invalidate();

	if (s.algorithm != AlgoOff) {
		log_canvas_once(d, main_tex);
		const enum gs_color_space space = gs_get_color_space();
		bool ok = false;
		if (s.algorithm == AlgoComputeIdentity) {
			if (!d->spike)
				d->spike = compute::create();
			ok = draw_compute_identity(d, main_tex, s);
		} else if (s.algorithm == AlgoIdentity) {
			ok = d->core.draw_identity(main_tex, space, s.core);
		} else {
			ok = d->core.process(main_tex, main_tex, space, s.core);
		}
		if (ok) {
			d->c.dispatches++;
		} else {
			d->c.failures++; // brief 18: pass the frame through untouched, warn once
			if (!d->warned && s.algorithm != AlgoComputeIdentity) {
				obs_log(LOG_WARNING, "[denoise] resources unavailable: passing the program through");
				d->warned = true;
			}
		}
	}

	const uint64_t now = os_gettime_ns();
	if (d->log_now.exchange(false)) {
		log_counters(d, "on request");
	} else if (s.core.log_counters && now - d->last_log_ns >= kLogIntervalNs) {
		log_counters(d, "periodic");
		d->last_log_ns = now;
	}
}

// ---- controller source (settings UI) --------------------------------------

const char *ctl_get_name(void *)
{
	return obs_module_text("Denoise.Name");
}

void save_settings(obs_data_t *settings)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *file = obs_module_config_path(kConfigFile);
	if (file) {
		obs_data_save_json_safe(settings, file, "tmp", "bak");
		bfree(file);
	}
}

void ctl_update(void *, obs_data_t *data)
{
	if (!g)
		return;
	Settings s;
	s.algorithm = (int)obs_data_get_int(data, "algorithm");
	if (s.algorithm != AlgoOff && s.algorithm != AlgoIdentity && s.algorithm != AlgoHqdn3d &&
	    !(s.algorithm == AlgoComputeIdentity && compute::available()))
		s.algorithm = AlgoOff; // unknown or unavailable value: fail safe
	s.core = dn::core_parse(data);
	s.compute_variant = (int)obs_data_get_int(data, "compute_variant");
	if (s.compute_variant < compute::VariantTwoCopies || s.compute_variant > compute::VariantDeferred)
		s.compute_variant = compute::VariantTwoCopies;

	int prev;
	{
		std::lock_guard<std::mutex> lock(g->mutex);
		prev = g->settings.algorithm;
		g->settings = s;
	}
	if (prev != s.algorithm)
		obs_log(LOG_INFO, "[denoise] algorithm %s", algo_name(s.algorithm));
	save_settings(data);
}

void *ctl_create(obs_data_t *settings, obs_source_t *source)
{
	if (g)
		g->controller = source;
	ctl_update(nullptr, settings);
	return g;
}

void ctl_destroy(void *) {}

void ctl_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "algorithm", AlgoOff); // new installs are neutral
	dn::core_defaults(s, dn::PlacementProgram);
	obs_data_set_default_int(s, "compute_variant", compute::VariantTwoCopies);
}

bool log_now_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->log_now = true;
	return false;
}

bool reset_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->reset_counters = true;
	return false;
}

bool nvof_probe_clicked(obs_properties_t *, obs_property_t *, void *)
{
	dn::nvof_probe_async();
	return false;
}

bool reset_history_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->reset_history = true;
	return false;
}

obs_properties_t *ctl_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Denoise.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "algorithm", obs_module_text("Denoise.Algorithm"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Off"), AlgoOff);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Hqdn3d"), AlgoHqdn3d);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Identity"), AlgoIdentity);
	if (compute::available())
		obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.ComputeIdentity"), AlgoComputeIdentity);
	obs_properties_add_button2(props, "reset_history", obs_module_text("Denoise.ResetHistory"),
				   reset_history_clicked, nullptr);

	obs_properties_t *dbg = nullptr;
	obs_properties_t *dev = dn::core_properties(props, dn::PlacementProgram, &dbg);
	obs_properties_add_button2(dbg, "log_now", obs_module_text("Denoise.LogNow"), log_now_clicked, nullptr);
	obs_properties_add_button2(dbg, "reset_counters", obs_module_text("Denoise.ResetCounters"), reset_clicked,
				   nullptr);
	if (compute::available()) {
		p = obs_properties_add_list(dev, "compute_variant", obs_module_text("Denoise.ComputeVariant"),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.TwoCopies"),
					  compute::VariantTwoCopies);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.OneCopy"),
					  compute::VariantOneCopy);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.Deferred"),
					  compute::VariantDeferred);
	}
	if (dn::nvof_probe_available())
		obs_properties_add_button2(dev, "nvof_probe", obs_module_text("Denoise.NvofProbe"), nvof_probe_clicked,
					   nullptr);
	return props;
}

void open_properties(void *)
{
	if (g && g->controller)
		obs_frontend_open_source_properties(g->controller);
}

} // namespace

extern "C" void hdrtk_register_denoise_filter(void);

extern "C" void hdrtk_denoise_load(void)
{
	struct obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DO_NOT_DUPLICATE; // controller only, no video
	info.get_name = ctl_get_name;
	info.create = ctl_create;
	info.destroy = ctl_destroy;
	info.update = ctl_update;
	info.get_defaults = ctl_defaults;
	info.get_properties = ctl_properties;
	obs_register_source(&info);

	hdrtk_register_denoise_filter();

	g = new Denoise();
	obs_add_main_rendered_callback(on_main_rendered, g);
	obs_frontend_add_tools_menu_item(obs_module_text("Denoise.Menu"), open_properties, nullptr);
	obs_log(LOG_INFO, "[denoise] program hook registered (once per unique main-canvas frame)");
}

// Settings saved by the plugin under its former package name (obs-hdr-toolkit, renamed
// hdr-toolkit on 1 Nov 2026): <plugin_config>/obs-hdr-toolkit/program-denoise.json.
obs_data_t *load_legacy_settings()
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return nullptr;
	std::string base(dir);
	bfree(dir);
	while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
		base.pop_back();
	const size_t cut = base.find_last_of("/\\");
	if (cut == std::string::npos)
		return nullptr;
	const std::string legacy = base.substr(0, cut + 1) + "obs-hdr-toolkit/" + kConfigFile;
	if (!os_file_exists(legacy.c_str()))
		return nullptr;
	obs_data_t *s = obs_data_create_from_json_file_safe(legacy.c_str(), "bak");
	if (s) {
		obs_log(LOG_INFO, "[denoise] settings migrated from %s", legacy.c_str());
		save_settings(s); // to the new location; the old file is left untouched
	}
	return s;
}

// Create the controller after all modules are loaded (settings from the config file).
extern "C" void hdrtk_denoise_post_load(void)
{
	if (!g || g->controller)
		return;
	char *file = obs_module_config_path(kConfigFile);
	obs_data_t *settings = file && os_file_exists(file) ? obs_data_create_from_json_file_safe(file, "bak")
							    : nullptr;
	bfree(file);
	if (!settings)
		settings = load_legacy_settings();
	if (!settings)
		settings = obs_data_create();
	obs_source_t *src = obs_source_create_private(kSourceId, "HDR Program Denoise", settings);
	obs_data_release(settings);
	g->controller = src;
}

extern "C" void hdrtk_denoise_unload(void)
{
	if (!g)
		return;
	obs_remove_main_rendered_callback(on_main_rendered, g);
	if (g->controller) {
		obs_source_t *src = g->controller;
		g->controller = nullptr;
		obs_source_release(src);
	}
	obs_enter_graphics();
	g->core.free_all();
	compute::destroy(g->spike);
	g->spike = nullptr;
	dn::core_effect_free();
	obs_leave_graphics();
	delete g;
	g = nullptr;
}
