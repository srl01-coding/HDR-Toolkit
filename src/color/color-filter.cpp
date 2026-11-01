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
 * HDR Color (hdr_toolkit_color_v1) - global stages (P2) and tonal zones (P3).
 *
 * Brief sections 5-7: white balance (Bradford, daylight locus), global
 * exposure, six tonal zones (Black..Specular) with frozen masks, contrast
 * around a pivot, global and zone colour wheels, saturation and grade mix, on
 * straight linear Rec.709 RGB in nominal nits, in the fixed v1 order. Soft
 * clipping slots in after grade mix; every new stage defaults to neutral, so
 * grades made with an earlier build are unchanged.
 *
 * Zone ranges are stored as their actual edges (zone_<name>_a..d, stops from
 * the gray reference, brief 10.4). The UI edits a mirror (centre / width /
 * lower / upper falloff; Black and Specular: boundary / falloff) that the
 * modified callbacks write through to the edges, as Transform does for corners.
 *
 * Render contract identical to the other toolkit filters
 * (docs/COLOR_PIPELINE.md).
 */

#include "color-math.hpp"
#include "../pattern/pattern-math.hpp"
#include "../shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using hdrtk::color::GlobalParams;
using hdrtk::color::kZoneCount;
using hdrtk::color::ShaderParams;
using hdrtk::color::ZoneParams;

// 1-3: development builds (zone defaults changed each time). 4: seven zones
// (Midtones added), handover defaults. Older settings are not migrated: the user
// chose to regrade (4 Oct 2026); unknown keys are ignored and missing ones take
// the current defaults.
constexpr int kSchemaVersion = 4;
constexpr float kAlphaEpsilon = 1e-6f;

enum DiagView { DiagOff = 0, DiagZoneMask = 1, DiagAllZones = 2 };

struct Snapshot {
	bool neutral = true;
	bool force_render = false;
	int diag_view = DiagOff;
	int diag_zone = hdrtk::color::ZoneShadow;
	ShaderParams sp{};
};

const char *const kZoneLabels[kZoneCount] = {"Color.Zone.Black",    "Color.Zone.Dark",  "Color.Zone.Shadow",
					     "Color.Zone.Midtones", "Color.Zone.Light", "Color.Zone.Highlight",
					     "Color.Zone.Specular"};

std::string zkey(int zone, const char *suffix)
{
	return std::string("zone_") + hdrtk::color::kZoneNames[zone] + "_" + suffix;
}

// Black is always open below (only c, d stored), Specular always open above
// (only a, b stored). The four interior zones store all edges and two open flags.
bool low_tail(int zone)
{
	return hdrtk::color::zone_fixed_open_low(zone);
}
bool high_tail(int zone)
{
	return hdrtk::color::zone_fixed_open_high(zone);
}
bool interior(int zone)
{
	return !low_tail(zone) && !high_tail(zone);
}

// Zone index from a settings key "zone_<name>_...", or -1.
int zone_of_key(const char *key)
{
	for (int i = 0; i < kZoneCount; i++) {
		const std::string pre = std::string("zone_") + hdrtk::color::kZoneNames[i] + "_";
		if (std::strncmp(key, pre.c_str(), pre.size()) == 0)
			return i;
	}
	return -1;
}

struct ColorFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;

	gs_eparam_t *p_npu = nullptr, *p_upn = nullptr, *p_alpha_eps = nullptr;
	gs_eparam_t *p_wb[3] = {nullptr, nullptr, nullptr};
	gs_eparam_t *p_exposure = nullptr, *p_cm1 = nullptr, *p_pivot = nullptr, *p_gray = nullptr;
	gs_eparam_t *p_wheel = nullptr, *p_sat = nullptr, *p_mix = nullptr;
	gs_eparam_t *p_use_wb = nullptr, *p_use_contrast = nullptr, *p_use_wheel = nullptr, *p_use_sat = nullptr,
		    *p_use_mix = nullptr, *p_use_zones = nullptr;
	gs_eparam_t *p_zone_edges[kZoneCount] = {}, *p_zone_es[kZoneCount] = {}, *p_zone_wd[kZoneCount] = {};
	gs_eparam_t *p_diag_mode = nullptr, *p_diag_zone = nullptr;
	gs_eparam_t *p_offset = nullptr, *p_low_knee = nullptr, *p_low_strength = nullptr, *p_high_knee = nullptr,
		    *p_high_headroom = nullptr, *p_high_peak = nullptr;
	gs_eparam_t *p_use_offset = nullptr, *p_use_low = nullptr, *p_use_high = nullptr;

	std::mutex mutex; // guards snapshot / have_valid / last_clips
	Snapshot snapshot;
	bool have_valid = false;
	// Last non-conflicting soft-clip settings (brief 8.3: keep the last valid curve).
	GlobalParams last_clips;
	bool have_clips = false;

	// graphics thread only
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
};

const char *color_get_name(void *)
{
	return obs_module_text("Color.Name");
}

GlobalParams read_params(obs_data_t *s)
{
	GlobalParams p;
	p.exposure_ev = obs_data_get_double(s, "global_exposure_ev");
	p.contrast = obs_data_get_double(s, "contrast_factor");
	p.pivot_nits = obs_data_get_double(s, "pivot_nits");
	p.gray_nits = obs_data_get_double(s, "gray_reference_nits");
	p.saturation = obs_data_get_double(s, "global_saturation");
	p.wb_mired = obs_data_get_double(s, "wb_mired_shift");
	p.wb_tint = obs_data_get_double(s, "wb_tint");
	p.wheel_x = obs_data_get_double(s, "global_wheel_x");
	p.wheel_y = obs_data_get_double(s, "global_wheel_y");
	p.grade_mix = obs_data_get_double(s, "grade_mix");
	p.offset_nits = obs_data_get_double(s, "offset_nits");
	p.low_clip = obs_data_get_bool(s, "low_clip_enabled");
	p.low_knee_nits = obs_data_get_double(s, "low_clip_knee_nits");
	p.low_strength = obs_data_get_double(s, "low_clip_strength");
	p.high_clip = obs_data_get_bool(s, "high_clip_enabled");
	p.high_peak_nits = obs_data_get_double(s, "high_clip_peak_nits");
	p.high_softness = obs_data_get_double(s, "high_clip_softness");
	for (int i = 0; i < kZoneCount; i++) {
		ZoneParams &z = p.zones[i];
		z.enabled = obs_data_get_bool(s, zkey(i, "enabled").c_str());
		z.exposure_ev = obs_data_get_double(s, zkey(i, "exposure_ev").c_str());
		z.saturation = obs_data_get_double(s, zkey(i, "saturation").c_str());
		z.wheel_x = obs_data_get_double(s, zkey(i, "wheel_x").c_str());
		z.wheel_y = obs_data_get_double(s, zkey(i, "wheel_y").c_str());
		if (!low_tail(i)) {
			z.a = obs_data_get_double(s, zkey(i, "a").c_str());
			z.b = obs_data_get_double(s, zkey(i, "b").c_str());
		}
		if (!high_tail(i)) {
			z.c = obs_data_get_double(s, zkey(i, "c").c_str());
			z.d = obs_data_get_double(s, zkey(i, "d").c_str());
		}
		if (interior(i)) {
			z.open_low = obs_data_get_bool(s, zkey(i, "open_low").c_str());
			z.open_high = obs_data_get_bool(s, zkey(i, "open_high").c_str());
		}
	}
	return p;
}

// UI mirror <-> stored edges. Every mirror key is derived from the edges:
//   centre = (b + c) / 2, width = c - b, falloff below = b - a, falloff above = d - c,
//   full strength below = c (open below), full strength above = b (open above).
void write_zone_ui(obs_data_t *s, int i, const ZoneParams &z, bool as_default)
{
	auto set = [&](const char *suffix, double v) {
		const std::string k = zkey(i, suffix);
		if (as_default)
			obs_data_set_default_double(s, k.c_str(), v);
		else
			obs_data_set_double(s, k.c_str(), v);
	};
	if (!low_tail(i)) {
		set("full_above", z.b);
		set("fall_lo", z.b - z.a);
	}
	if (!high_tail(i)) {
		set("full_below", z.c);
		set("fall_hi", z.d - z.c);
	}
	if (interior(i)) {
		set("center", 0.5 * (z.b + z.c));
		set("width", z.c - z.b);
	}
}

// One mirror field changed: move only the edges it controls.
void ui_to_edges(obs_data_t *s, int i, const char *suffix)
{
	auto get = [&](const char *k) {
		return obs_data_get_double(s, zkey(i, k).c_str());
	};
	auto set = [&](const char *k, double v) {
		obs_data_set_double(s, zkey(i, k).c_str(), v);
	};
	const std::string sx = suffix;
	if (sx == "center" || sx == "width") {
		const double b = get("center") - 0.5 * get("width");
		const double c = get("center") + 0.5 * get("width");
		set("a", b - get("fall_lo"));
		set("b", b);
		set("c", c);
		set("d", c + get("fall_hi"));
	} else if (sx == "full_below") {
		set("c", get("full_below"));
		set("d", get("full_below") + get("fall_hi"));
	} else if (sx == "full_above") {
		set("b", get("full_above"));
		set("a", get("full_above") - get("fall_lo"));
	} else if (sx == "fall_lo") {
		set("a", get("b") - get("fall_lo"));
	} else if (sx == "fall_hi") {
		set("d", get("c") + get("fall_hi"));
	}
}

void color_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<ColorFilter *>(data);
	GlobalParams p = read_params(settings);
	const std::string problems = hdrtk::color::sanitize(p);
	if (!problems.empty())
		obs_log(LOG_WARNING, "[color] '%s': %s(clamped)", obs_source_get_name(f->context), problems.c_str());

	// Keep the UI mirrors in step with the stored values (scripts may set only those).
	for (int i = 0; i < kZoneCount; i++)
		write_zone_ui(settings, i, p.zones[i], false);
	obs_data_set_double(settings, "high_clip_knee_nits", p.high_knee_nits());

	{
		std::lock_guard<std::mutex> lock(f->mutex);
		if (hdrtk::color::clips_conflict(p)) {
			obs_log(LOG_WARNING,
				"[color] '%s': soft clip conflict (low knee %.4g nits above high knee %.4g nits); %s",
				obs_source_get_name(f->context), p.low_knee_nits, p.high_knee_nits(),
				f->have_clips ? "keeping the previous curves" : "low clip held off");
			if (f->have_clips) {
				const GlobalParams &l = f->last_clips;
				p.low_clip = l.low_clip;
				p.low_knee_nits = l.low_knee_nits;
				p.low_strength = l.low_strength;
				p.high_clip = l.high_clip;
				p.high_peak_nits = l.high_peak_nits;
				p.high_softness = l.high_softness;
			} else {
				p.low_clip = false;
			}
		} else {
			f->last_clips = p;
			f->have_clips = true;
		}
	}

	Snapshot s;
	std::string error;
	if (!hdrtk::color::make_shader_params(p, s.sp, &error)) {
		obs_log(LOG_WARNING, "[color] '%s': %s; keeping the last valid grade", obs_source_get_name(f->context),
			error.c_str());
		std::lock_guard<std::mutex> lock(f->mutex);
		if (f->have_valid)
			return;
		GlobalParams neutral;
		hdrtk::color::make_shader_params(neutral, s.sp);
		p = neutral;
	}
	s.neutral = hdrtk::color::is_neutral(p);
	s.force_render = obs_data_get_bool(settings, "force_render_identity");
	s.diag_view = (int)obs_data_get_int(settings, "diag_view");
	if (s.diag_view != DiagZoneMask && s.diag_view != DiagAllZones)
		s.diag_view = DiagOff; // unknown future value: fail safe
	s.diag_zone = (int)obs_data_get_int(settings, "diag_zone");
	if (s.diag_zone < 0 || s.diag_zone >= kZoneCount)
		s.diag_zone = hdrtk::color::ZoneShadow;

	std::lock_guard<std::mutex> lock(f->mutex);
	f->snapshot = s;
	f->have_valid = true;
}

gs_eparam_t *param(gs_effect_t *e, const char *name)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (!p)
		obs_log(LOG_ERROR, "[color] effect parameter '%s' missing", name);
	return p;
}

void *color_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new ColorFilter();
	f->context = source;

	char *path = obs_module_file("effects/hdr-color.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	if (f->effect) {
		gs_effect_t *e = f->effect;
		f->p_npu = param(e, "nits_per_unit");
		f->p_upn = param(e, "units_per_nit");
		f->p_alpha_eps = param(e, "alpha_epsilon");
		f->p_wb[0] = param(e, "wb_row0");
		f->p_wb[1] = param(e, "wb_row1");
		f->p_wb[2] = param(e, "wb_row2");
		f->p_exposure = param(e, "exposure_gain");
		f->p_cm1 = param(e, "contrast_minus_one");
		f->p_pivot = param(e, "log2_pivot_over_gray");
		f->p_gray = param(e, "gray_nits");
		f->p_wheel = param(e, "wheel_delta");
		f->p_sat = param(e, "saturation");
		f->p_mix = param(e, "grade_mix");
		f->p_use_wb = param(e, "use_wb");
		f->p_use_contrast = param(e, "use_contrast");
		f->p_use_zones = param(e, "use_zones");
		for (int i = 0; i < kZoneCount; i++) {
			const std::string n = std::to_string(i);
			f->p_zone_edges[i] = param(e, ("zone_edges" + n).c_str());
			f->p_zone_es[i] = param(e, ("zone_es" + n).c_str());
			f->p_zone_wd[i] = param(e, ("zone_wd" + n).c_str());
		}
		f->p_diag_mode = param(e, "diag_mode");
		f->p_diag_zone = param(e, "diag_zone");
		f->p_offset = param(e, "offset_nits");
		f->p_low_knee = param(e, "low_knee");
		f->p_low_strength = param(e, "low_strength");
		f->p_high_knee = param(e, "high_knee");
		f->p_high_headroom = param(e, "high_headroom");
		f->p_high_peak = param(e, "high_peak");
		f->p_use_offset = param(e, "use_offset");
		f->p_use_low = param(e, "use_low_clip");
		f->p_use_high = param(e, "use_high_clip");
		f->p_use_wheel = param(e, "use_wheel");
		f->p_use_sat = param(e, "use_saturation");
		f->p_use_mix = param(e, "use_mix");
	}
	obs_leave_graphics();

	if (!f->effect) {
		obs_log(LOG_ERROR, "[color] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		bfree(errors);
		bfree(path);
		delete f;
		return nullptr;
	}
	bfree(errors);
	bfree(path);

	obs_data_set_int(settings, "schema_version", kSchemaVersion);
	color_update(f, settings);
	return f;
}

void color_destroy(void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	delete f;
}

void color_defaults(obs_data_t *s)
{
	// Every new instance is neutral (brief 1.3).
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_double(s, "global_exposure_ev", 0.0);
	obs_data_set_default_double(s, "contrast_factor", 1.0);
	obs_data_set_default_double(s, "pivot_nits", 18.0);
	obs_data_set_default_double(s, "gray_reference_nits", 18.0);
	obs_data_set_default_double(s, "global_saturation", 1.0);
	obs_data_set_default_double(s, "wb_mired_shift", 0.0);
	obs_data_set_default_double(s, "wb_tint", 0.0);
	obs_data_set_default_double(s, "global_wheel_x", 0.0);
	obs_data_set_default_double(s, "global_wheel_y", 0.0);
	obs_data_set_default_double(s, "grade_mix", 1.0);
	obs_data_set_default_double(s, "offset_nits", 0.0);
	// Soft clips: stored defaults per brief 8.1, not applied until enabled.
	obs_data_set_default_bool(s, "low_clip_enabled", false);
	obs_data_set_default_double(s, "low_clip_knee_nits", 0.1);
	obs_data_set_default_double(s, "low_clip_strength", 0.5);
	obs_data_set_default_bool(s, "high_clip_enabled", false);
	obs_data_set_default_double(s, "high_clip_peak_nits", 1000.0);
	obs_data_set_default_double(s, "high_clip_softness", 0.25);
	obs_data_set_default_double(s, "high_clip_knee_nits", 750.0);
	obs_data_set_default_bool(s, "force_render_identity", false);
	obs_data_set_default_int(s, "diag_view", DiagOff);
	obs_data_set_default_int(s, "diag_zone", hdrtk::color::ZoneShadow);
	for (int i = 0; i < kZoneCount; i++) {
		const ZoneParams z = hdrtk::color::default_zone(i);
		obs_data_set_default_bool(s, zkey(i, "enabled").c_str(), true);
		obs_data_set_default_double(s, zkey(i, "exposure_ev").c_str(), 0.0);
		obs_data_set_default_double(s, zkey(i, "saturation").c_str(), 1.0);
		obs_data_set_default_double(s, zkey(i, "wheel_x").c_str(), 0.0);
		obs_data_set_default_double(s, zkey(i, "wheel_y").c_str(), 0.0);
		if (!low_tail(i)) {
			obs_data_set_default_double(s, zkey(i, "a").c_str(), z.a);
			obs_data_set_default_double(s, zkey(i, "b").c_str(), z.b);
		}
		if (!high_tail(i)) {
			obs_data_set_default_double(s, zkey(i, "c").c_str(), z.c);
			obs_data_set_default_double(s, zkey(i, "d").c_str(), z.d);
		}
		if (interior(i)) {
			obs_data_set_default_bool(s, zkey(i, "open_low").c_str(), z.open_low);
			obs_data_set_default_bool(s, zkey(i, "open_high").c_str(), z.open_high);
		}
		write_zone_ui(s, i, z, true);
	}
}

// Every key a zone has ever used ("boundary"/"falloff": schema-1 UI mirror of Black/Specular).
const char *const kZoneSuffixes[] = {"enabled", "exposure_ev", "saturation", "wheel_x",   "wheel_y", "a",     "b",
				     "c",       "d",           "open_low",   "open_high", "center",  "width", "fall_lo",
				     "fall_hi", "full_below",  "full_above", "boundary",  "falloff"};

void unset_zone(obs_data_t *s, int i)
{
	for (const char *suffix : kZoneSuffixes)
		obs_data_unset_user_value(s, zkey(i, suffix).c_str());
}

bool zone_reset_clicked(obs_properties_t *, obs_property_t *property, void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	const int zone = zone_of_key(obs_property_name(property));
	if (zone < 0)
		return false;
	obs_data_t *s = obs_source_get_settings(f->context);
	unset_zone(s, zone);
	obs_source_update(f->context, s);
	obs_data_release(s);
	return true;
}

// A zone range field changed in the UI: write through to the stored edges.
// No property refresh (it would steal focus while typing or dragging).
bool zone_range_modified(void *, obs_properties_t *, obs_property_t *property, obs_data_t *settings)
{
	const char *name = obs_property_name(property);
	const int zone = zone_of_key(name);
	if (zone >= 0) {
		const std::string pre = zkey(zone, "");
		ui_to_edges(settings, zone, name + pre.size());
	}
	return false;
}

void set_range_visibility(obs_properties_t *props, obs_data_t *settings, int i)
{
	if (!interior(i))
		return;
	const bool lo = obs_data_get_bool(settings, zkey(i, "open_low").c_str());
	const bool hi = obs_data_get_bool(settings, zkey(i, "open_high").c_str());
	auto vis = [&](const char *suffix, bool v) {
		obs_property_t *p = obs_properties_get(props, zkey(i, suffix).c_str());
		if (p)
			obs_property_set_visible(p, v);
	};
	vis("center", !lo && !hi);
	vis("width", !lo && !hi);
	vis("full_below", lo && !hi);
	vis("full_above", hi && !lo);
	vis("fall_lo", !lo);
	vis("fall_hi", !hi);
}

// Open-end checkbox toggled: show the matching range fields (a toggle, not typing,
// so refreshing the view is fine).
bool zone_open_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const int zone = zone_of_key(obs_property_name(property));
	if (zone < 0)
		return false;
	set_range_visibility(props, settings, zone);
	return true;
}

bool reset_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_data_t *s = obs_source_get_settings(f->context);
	const char *keys[] = {"global_exposure_ev", "contrast_factor", "pivot_nits",     "global_saturation",
			      "wb_mired_shift",     "wb_tint",         "global_wheel_x", "global_wheel_y",
			      "grade_mix",          "offset_nits"};
	for (const char *k : keys)
		obs_data_unset_user_value(s, k);
	for (int i = 0; i < kZoneCount; i++)
		unset_zone(s, i);
	obs_source_update(f->context, s);
	obs_data_release(s);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double lo, double hi, double step,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), lo, hi, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

std::string clip_status(obs_data_t *s)
{
	GlobalParams p;
	p.low_clip = obs_data_get_bool(s, "low_clip_enabled");
	p.low_knee_nits = obs_data_get_double(s, "low_clip_knee_nits");
	p.high_clip = obs_data_get_bool(s, "high_clip_enabled");
	p.high_peak_nits = obs_data_get_double(s, "high_clip_peak_nits");
	p.high_softness = obs_data_get_double(s, "high_clip_softness");
	if (hdrtk::color::clips_conflict(p))
		return obs_module_text("Color.Clip.Conflict");
	return obs_module_text("Color.Clip.Info");
}

// Peak or knee changed: softness = 1 - knee / peak (the stored value). Refresh the
// view only when the conflict status changes (refreshing steals slider focus).
bool clip_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *s)
{
	const char *name = obs_property_name(property);
	if (!std::strcmp(name, "high_clip_peak_nits") || !std::strcmp(name, "high_clip_knee_nits")) {
		const double peak = obs_data_get_double(s, "high_clip_peak_nits");
		const double knee = obs_data_get_double(s, "high_clip_knee_nits");
		if (peak > 0)
			obs_data_set_double(s, "high_clip_softness", std::clamp(1.0 - knee / peak, 0.0, 0.95));
	}
	obs_property_t *st = obs_properties_get(props, "clip_status");
	const std::string text = clip_status(s);
	const char *old = st ? obs_property_description(st) : nullptr;
	if (st && (!old || text != old)) {
		obs_property_set_description(st, text.c_str());
		return true;
	}
	return false;
}

void add_clip_group(obs_properties_t *props, ColorFilter *f)
{
	obs_data_t *settings = f ? obs_source_get_settings(f->context) : nullptr;
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_text(g, "clip_status", settings ? clip_status(settings).c_str() : "", OBS_TEXT_INFO);

	obs_properties_t *lo = obs_properties_create();
	obs_property_t *p = slider(lo, "low_clip_knee_nits", "Color.Clip.LowKnee", 0.001, 10.0, 0.001, " nits");
	obs_property_set_modified_callback(p, clip_modified);
	slider(lo, "low_clip_strength", "Color.Clip.LowStrength", 0.0, 1.0, 0.01, nullptr);
	p = obs_properties_add_group(g, "low_clip_enabled", obs_module_text("Color.Clip.Low"), OBS_GROUP_CHECKABLE, lo);
	obs_property_set_modified_callback(p, clip_modified);

	obs_properties_t *hi = obs_properties_create();
	p = slider(hi, "high_clip_peak_nits", "Color.Clip.HighPeak", 100.0, 10000.0, 1.0, " nits");
	obs_property_set_modified_callback(p, clip_modified);
	p = slider(hi, "high_clip_knee_nits", "Color.Clip.HighKnee", 5.0, 10000.0, 1.0, " nits");
	obs_property_set_modified_callback(p, clip_modified);
	p = obs_properties_add_group(g, "high_clip_enabled", obs_module_text("Color.Clip.High"), OBS_GROUP_CHECKABLE,
				     hi);
	obs_property_set_modified_callback(p, clip_modified);

	obs_properties_add_group(props, "soft_clip", obs_module_text("Color.Clip"), OBS_GROUP_NORMAL, g);
	if (settings)
		obs_data_release(settings);
}

std::string stop_legend(double gray)
{
	std::string out = obs_module_text("Color.Zones.Info");
	out += "\n";
	char buf[96];
	for (int st = -8; st <= 6; st += 2) {
		const double nits = gray * std::exp2((double)st);
		const double hlg = 100.0 * hdrtk::pattern::obs_nits_to_hlg_level(nits);
		snprintf(buf, sizeof(buf), "%s%+d = %.3g nits (HLG %.0f%%)", st == -8 ? "" : ",  ", st, nits, hlg);
		out += buf;
	}
	return out;
}

obs_property_t *range_slider(obs_properties_t *g, int zone, const char *suffix, const char *text, double lo, double hi,
			     ColorFilter *f)
{
	obs_property_t *p = slider(g, zkey(zone, suffix).c_str(), text, lo, hi, 0.01, " stops");
	obs_property_set_modified_callback2(p, zone_range_modified, f);
	return p;
}

void add_zone_groups(obs_properties_t *props, ColorFilter *f)
{
	obs_data_t *settings = f ? obs_source_get_settings(f->context) : nullptr;
	double gray = settings ? obs_data_get_double(settings, "gray_reference_nits") : 18.0;
	if (!(gray > 0))
		gray = 18.0;
	obs_properties_t *zones = obs_properties_create();
	obs_properties_add_text(zones, "zones_info", stop_legend(gray).c_str(), OBS_TEXT_INFO);
	for (int i = 0; i < kZoneCount; i++) {
		obs_properties_t *g = obs_properties_create();
		slider(g, zkey(i, "exposure_ev").c_str(), "Color.Exposure", -4.0, 4.0, 0.01, " EV");
		slider(g, zkey(i, "saturation").c_str(), "Color.Saturation", 0.0, 2.0, 0.001, nullptr);
		slider(g, zkey(i, "wheel_x").c_str(), "Color.Wheel.X", -1.0, 1.0, 0.001, nullptr);
		slider(g, zkey(i, "wheel_y").c_str(), "Color.Wheel.Y", -1.0, 1.0, 0.001, nullptr);
		// Ranges keep every stored edge within +-20 stops.
		if (interior(i)) {
			obs_property_t *o = obs_properties_add_bool(g, zkey(i, "open_low").c_str(),
								    obs_module_text("Color.Zone.OpenLow"));
			obs_property_set_modified_callback(o, zone_open_modified);
			o = obs_properties_add_bool(g, zkey(i, "open_high").c_str(),
						    obs_module_text("Color.Zone.OpenHigh"));
			obs_property_set_modified_callback(o, zone_open_modified);
			range_slider(g, i, "center", "Color.Zone.Center", -12.0, 12.0, f);
			range_slider(g, i, "width", "Color.Zone.Width", 0.0, 6.0, f);
		}
		if (!high_tail(i))
			range_slider(g, i, "full_below", "Color.Zone.FullBelow", -15.0, 15.0, f);
		if (!low_tail(i))
			range_slider(g, i, "full_above", "Color.Zone.FullAbove", -15.0, 15.0, f);
		if (!low_tail(i))
			range_slider(g, i, "fall_lo", "Color.Zone.FalloffLow", 0.05, 5.0, f);
		if (!high_tail(i))
			range_slider(g, i, "fall_hi", "Color.Zone.FalloffHigh", 0.05, 5.0, f);
		if (settings)
			set_range_visibility(g, settings, i);
		obs_properties_add_button2(g, zkey(i, "reset").c_str(), obs_module_text("Color.Zone.Reset"),
					   zone_reset_clicked, f);
		// Checkable group: its name is the persisted enable key.
		obs_properties_add_group(zones, zkey(i, "enabled").c_str(), obs_module_text(kZoneLabels[i]),
					 OBS_GROUP_CHECKABLE, g);
	}
	obs_properties_add_group(props, "tonal_zones", obs_module_text("Color.Zones"), OBS_GROUP_NORMAL, zones);
	obs_data_release(settings);
}

obs_properties_t *color_properties(void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *g = obs_properties_create();
	slider(g, "global_exposure_ev", "Color.Exposure", -6.0, 6.0, 0.01, " EV");
	slider(g, "contrast_factor", "Color.Contrast", 0.5, 2.0, 0.001, nullptr);
	slider(g, "pivot_nits", "Color.Pivot", 0.1, 1000.0, 0.1, " nits");
	slider(g, "global_saturation", "Color.Saturation", 0.0, 2.0, 0.001, nullptr);
	obs_properties_add_group(props, "global", obs_module_text("Color.Global"), OBS_GROUP_NORMAL, g);

	obs_properties_t *wb = obs_properties_create();
	slider(wb, "wb_mired_shift", "Color.Temperature", -90.0, 90.0, 0.5, " mired");
	slider(wb, "wb_tint", "Color.Tint", -100.0, 100.0, 0.5, nullptr);
	obs_properties_add_group(props, "white_balance", obs_module_text("Color.WhiteBalance"), OBS_GROUP_NORMAL, wb);

	obs_properties_t *wh = obs_properties_create();
	obs_properties_add_text(wh, "wheel_info", obs_module_text("Color.Wheel.Info"), OBS_TEXT_INFO);
	slider(wh, "global_wheel_x", "Color.Wheel.X", -1.0, 1.0, 0.001, nullptr);
	slider(wh, "global_wheel_y", "Color.Wheel.Y", -1.0, 1.0, 0.001, nullptr);
	obs_properties_add_group(props, "wheel", obs_module_text("Color.Wheel"), OBS_GROUP_NORMAL, wh);

	add_zone_groups(props, f);

	obs_properties_t *og = obs_properties_create();
	obs_properties_add_text(og, "offset_info", obs_module_text("Color.Offset.Info"), OBS_TEXT_INFO);
	slider(og, "offset_nits", "Color.Offset", -1.0, 1.0, 0.0001, " nits");
	obs_properties_add_group(props, "offset_group", obs_module_text("Color.Offset.Group"), OBS_GROUP_NORMAL, og);

	slider(props, "grade_mix", "Color.GradeMix", 0.0, 1.0, 0.001, nullptr);
	obs_properties_add_button2(props, "reset_grade", obs_module_text("Color.Reset"), reset_clicked, f);

	add_clip_group(props, f);

	obs_properties_t *adv = obs_properties_create();
	slider(adv, "gray_reference_nits", "Color.GrayReference", 1.0, 100.0, 0.1, " nits");
	obs_property_t *dv = obs_properties_add_list(adv, "diag_view", obs_module_text("Color.Diag"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dv, obs_module_text("Color.Diag.Off"), DiagOff);
	obs_property_list_add_int(dv, obs_module_text("Color.Diag.ZoneMask"), DiagZoneMask);
	obs_property_list_add_int(dv, obs_module_text("Color.Diag.AllZones"), DiagAllZones);
	obs_property_t *dz = obs_properties_add_list(adv, "diag_zone", obs_module_text("Color.Diag.Zone"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < kZoneCount; i++)
		obs_property_list_add_int(dz, obs_module_text(kZoneLabels[i]), i);
	obs_properties_add_text(adv, "diag_info", obs_module_text("Color.Diag.Info"), OBS_TEXT_INFO);
	obs_properties_add_bool(adv, "force_render_identity", obs_module_text("Color.ForceRender"));
	obs_properties_add_group(props, "advanced", obs_module_text("Color.Advanced"), OBS_GROUP_NORMAL, adv);
	return props;
}

void set3(gs_eparam_t *p, const float v[3])
{
	struct vec3 x;
	vec3_set(&x, v[0], v[1], v[2]);
	gs_effect_set_vec3(p, &x);
}

void color_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	Snapshot s;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->snapshot;
	}

	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[color] '%s': %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
	}
	if (!ctx.width || !ctx.height) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (s.neutral && !s.force_render && s.diag_view == DiagOff) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin_with_color_space(f->context, ctx.staging_format, ctx.input_space,
							      OBS_NO_DIRECT_RENDERING))
		return;

	// Nits scale is read live so a change of OBS SDR white level applies immediately.
	const double npu = hdrtk::nits_per_unit(ctx.input_space);
	gs_effect_set_float(f->p_npu, (float)npu);
	gs_effect_set_float(f->p_upn, (float)(1.0 / npu));
	gs_effect_set_float(f->p_alpha_eps, kAlphaEpsilon);
	for (int i = 0; i < 3; i++)
		set3(f->p_wb[i], s.sp.wb_row[i]);
	gs_effect_set_float(f->p_exposure, s.sp.exposure_gain);
	gs_effect_set_float(f->p_cm1, s.sp.contrast_minus_one);
	gs_effect_set_float(f->p_pivot, s.sp.log2_pivot_over_gray);
	gs_effect_set_float(f->p_gray, s.sp.gray_nits);
	set3(f->p_wheel, s.sp.wheel_delta);
	gs_effect_set_float(f->p_sat, s.sp.saturation);
	gs_effect_set_float(f->p_mix, s.sp.grade_mix);
	gs_effect_set_float(f->p_use_wb, s.sp.use_wb);
	gs_effect_set_float(f->p_use_contrast, s.sp.use_contrast);
	gs_effect_set_float(f->p_use_zones, s.sp.use_zones);
	for (int i = 0; i < kZoneCount; i++) {
		struct vec4 e;
		vec4_set(&e, s.sp.zone_edges[i][0], s.sp.zone_edges[i][1], s.sp.zone_edges[i][2],
			 s.sp.zone_edges[i][3]);
		gs_effect_set_vec4(f->p_zone_edges[i], &e);
		struct vec2 es;
		vec2_set(&es, s.sp.zone_ev[i], s.sp.zone_sat[i]);
		gs_effect_set_vec2(f->p_zone_es[i], &es);
		set3(f->p_zone_wd[i], s.sp.zone_wheel[i]);
	}
	gs_effect_set_float(f->p_offset, s.sp.offset_nits);
	gs_effect_set_float(f->p_low_knee, s.sp.low_knee);
	gs_effect_set_float(f->p_low_strength, s.sp.low_strength);
	gs_effect_set_float(f->p_high_knee, s.sp.high_knee);
	gs_effect_set_float(f->p_high_headroom, s.sp.high_headroom);
	gs_effect_set_float(f->p_high_peak, s.sp.high_peak);
	gs_effect_set_float(f->p_use_offset, s.sp.use_offset);
	gs_effect_set_float(f->p_use_low, s.sp.use_low_clip);
	gs_effect_set_float(f->p_use_high, s.sp.use_high_clip);
	gs_effect_set_float(f->p_diag_mode, (float)s.diag_view);
	gs_effect_set_float(f->p_diag_zone, (float)s.diag_zone);
	gs_effect_set_float(f->p_use_wheel, s.sp.use_wheel);
	gs_effect_set_float(f->p_use_sat, s.sp.use_saturation);
	gs_effect_set_float(f->p_use_mix, s.sp.use_mix);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, s.diag_view == DiagOff ? "Grade" : "Mask");
	gs_blend_state_pop();
}

enum gs_color_space color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<ColorFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_color_filter(void)
{
	struct obs_source_info info = {};
	info.id = "hdr_toolkit_color_v1";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = color_get_name;
	info.create = color_create;
	info.destroy = color_destroy;
	info.update = color_update;
	info.get_defaults = color_defaults;
	info.get_properties = color_properties;
	info.video_render = color_render;
	info.video_get_color_space = color_space;
	obs_register_source(&info);
}
