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
 * Developer HDR pattern source (hdr_toolkit_pattern_dev).
 *
 * Produces exact extended-linear Rec.709 values in GS_CS_709_EXTENDED with no
 * video decode involved, so tests are not affected by OBS's limited-range
 * input clamp, HDMI, codecs or players. Values are generated in nominal nits
 * and divided by the live SDR white level W, so a patch labelled N nits is
 * N/W working units whatever W is set to.
 */

#include "pattern-math.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <mutex>
#include <vector>

namespace {

using hdrtk::pattern::Mode;
using hdrtk::pattern::Rgba;
using hdrtk::pattern::Settings;

struct PatternSource {
	obs_source_t *context = nullptr;

	std::mutex mutex; // guards `settings` and `dirty`
	Settings settings;
	bool dirty = true;

	// graphics thread only
	gs_texture_t *texture = nullptr;
	uint32_t tex_width = 0;
	uint32_t tex_height = 0;
	float built_for_white = 0.0f;
};

const char *pattern_get_name(void *)
{
	return obs_module_text("Pattern.Name");
}

Settings read_settings(obs_data_t *data)
{
	Settings s;
	const int mode = (int)obs_data_get_int(data, "mode");
	s.mode = hdrtk::pattern::mode_valid(mode) ? (Mode)mode : Mode::Chart;
	s.flat_nits = obs_data_get_double(data, "flat_nits");
	s.width = (uint32_t)obs_data_get_int(data, "width");
	s.height = (uint32_t)obs_data_get_int(data, "height");
	return s;
}

void pattern_update(void *data, obs_data_t *settings)
{
	auto *p = static_cast<PatternSource *>(data);
	Settings s = read_settings(settings);
	std::lock_guard<std::mutex> lock(p->mutex);
	p->settings = s;
	p->dirty = true;
}

void *pattern_create(obs_data_t *settings, obs_source_t *source)
{
	auto *p = new PatternSource();
	p->context = source;
	pattern_update(p, settings);
	return p;
}

void pattern_destroy(void *data)
{
	auto *p = static_cast<PatternSource *>(data);
	obs_enter_graphics();
	gs_texture_destroy(p->texture);
	obs_leave_graphics();
	delete p;
}

void pattern_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", (int)Mode::Chart);
	obs_data_set_default_double(settings, "flat_nits", 203.0);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

bool mode_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool flat = obs_data_get_int(settings, "mode") == (int)Mode::FlatField;
	obs_property_set_visible(obs_properties_get(props, "flat_nits"), flat);
	return true;
}

obs_properties_t *pattern_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Pattern.Info"), OBS_TEXT_INFO);

	obs_property_t *p = obs_properties_add_list(props, "mode", obs_module_text("Pattern.Mode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Chart"), (int)Mode::Chart);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Hlg"), (int)Mode::HlgSteps);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Neutral"), (int)Mode::NeutralSteps);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Highlight"), (int)Mode::HighlightSteps);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Ramp"), (int)Mode::Ramp);
	obs_property_list_add_int(p, obs_module_text("Pattern.Mode.Flat"), (int)Mode::FlatField);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_float(props, "flat_nits", obs_module_text("Pattern.FlatNits"), 0.0, 10000.0, 0.01);
	obs_property_float_set_suffix(p, " nits");

	obs_properties_add_int(props, "width", obs_module_text("Pattern.Width"), 64, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Pattern.Height"), 64, 8192, 1);
	return props;
}

// Graphics thread: (re)build the RGBA32F texture when settings or W change.
void rebuild_if_needed(PatternSource *p)
{
	const float white = obs_get_video_sdr_white_level();
	Settings s;
	bool dirty;
	{
		std::lock_guard<std::mutex> lock(p->mutex);
		s = p->settings;
		dirty = p->dirty;
		p->dirty = false;
	}
	if (!dirty && p->texture && white == p->built_for_white)
		return;

	std::vector<Rgba> px;
	if (!hdrtk::pattern::generate(s, px)) {
		obs_log(LOG_WARNING, "[pattern] invalid settings %ux%u flat=%g; keeping previous image", s.width,
			s.height, s.flat_nits);
		return;
	}
	const float inv_w = 1.0f / white;
	for (auto &c : px) {
		c.r *= inv_w;
		c.g *= inv_w;
		c.b *= inv_w;
	}

	gs_texture_destroy(p->texture);
	const uint8_t *bits = reinterpret_cast<const uint8_t *>(px.data());
	p->texture = gs_texture_create(s.width, s.height, GS_RGBA32F, 1, &bits, 0);
	p->tex_width = p->texture ? s.width : 0;
	p->tex_height = p->texture ? s.height : 0;
	p->built_for_white = white;
	obs_log(LOG_INFO, "[pattern] built %s %ux%u, SDR white %.1f nits", hdrtk::pattern::mode_label(s.mode), s.width,
		s.height, white);
}

void pattern_render(void *data, gs_effect_t *)
{
	auto *p = static_cast<PatternSource *>(data);
	rebuild_if_needed(p);
	if (!p->texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), p->texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(p->texture, 0, p->tex_width, p->tex_height);
}

uint32_t pattern_width(void *data)
{
	auto *p = static_cast<PatternSource *>(data);
	std::lock_guard<std::mutex> lock(p->mutex);
	return p->settings.width;
}

uint32_t pattern_height(void *data)
{
	auto *p = static_cast<PatternSource *>(data);
	std::lock_guard<std::mutex> lock(p->mutex);
	return p->settings.height;
}

enum gs_color_space pattern_color_space(void *, size_t, const enum gs_color_space *)
{
	// Always native HDR working RGB; libobs converts for SDR consumers.
	return GS_CS_709_EXTENDED;
}

} // namespace

extern "C" void hdrtk_register_pattern_source(void)
{
	struct obs_source_info info = {};
	info.id = "hdr_toolkit_pattern_dev";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = pattern_get_name;
	info.create = pattern_create;
	info.destroy = pattern_destroy;
	info.update = pattern_update;
	info.get_defaults = pattern_defaults;
	info.get_properties = pattern_properties;
	info.video_render = pattern_render;
	info.get_width = pattern_width;
	info.get_height = pattern_height;
	info.video_get_color_space = pattern_color_space;
	info.icon_type = OBS_ICON_TYPE_COLOR;
	obs_register_source(&info);
}
