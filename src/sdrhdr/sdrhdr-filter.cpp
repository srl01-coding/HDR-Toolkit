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
 * SDR in HDR (hdr_toolkit_sdr_in_hdr_v1, schema 1), docs/SDR_IN_HDR.md. Per-source level for
 * SDR sources (lyrics, slides, browser and image sources, SDR video) in an HDR programme,
 * instead of OBS's single global SDR white level.
 *
 * Active only on an HDR canvas (Rec.2100 HLG / PQ) and on SDR input (GS_CS_SRGB /
 * GS_CS_SRGB_16F); otherwise the source passes through untouched. The result is display
 * light in OBS units (nits / OBS SDR white level) reported as GS_CS_SRGB_16F, which libobs
 * copies into the HDR target unchanged (values above 1.0 kept).
 */

#include "sdrhdr-math.hpp"
#include "shared/filter-io.hpp"
#include "shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>

namespace {

namespace sh = hdrtk::sdrhdr;

constexpr const char *kId = "hdr_toolkit_sdr_in_hdr_v1";
constexpr int kSchemaVersion = 1;

struct Filter {
	obs_source_t *context = nullptr;
	std::mutex mutex;
	sh::Params params;
	bool force_render = false;
	std::atomic<uint64_t> generation{0};

	// graphics thread only
	gs_effect_t *effect = nullptr;
	hdrtk::FilterInput input;
	gs_texture_t *out = nullptr;
	uint32_t w = 0, h = 0;
	uint64_t last_time = 0, last_gen = ~0ull;
	bool out_valid = false;
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
	std::string last_state;

	explicit Filter(obs_source_t *src) : context(src) {}
};

bool canvas_hdr()
{
	struct obs_video_info ovi;
	return obs_get_video_info(&ovi) && (ovi.colorspace == VIDEO_CS_2100_HLG || ovi.colorspace == VIDEO_CS_2100_PQ);
}

bool sdr_input(enum gs_color_space s)
{
	return s == GS_CS_SRGB || s == GS_CS_SRGB_16F;
}

bool active(const Filter *f, enum gs_color_space native)
{
	return f->effect && canvas_hdr() && sdr_input(native);
}

sh::Params read_params(obs_data_t *s)
{
	sh::Params p;
	p.mode = (int)obs_data_get_int(s, "mode");
	p.white_nits = obs_data_get_double(s, "white_nits");
	const double peak = obs_data_get_double(s, "peak_nits");
	p.peak_nits = peak > 0 ? peak : p.white_nits;
	p.knee = obs_data_get_double(s, "knee") / 100.0;
	sh::sanitize(p);
	return p;
}

const char *name(void *)
{
	return obs_module_text("SdrHdr.Name");
}

void update(void *data, obs_data_t *s)
{
	auto *f = static_cast<Filter *>(data);
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->params = read_params(s);
		f->force_render = obs_data_get_bool(s, "force_render");
	}
	f->generation++;
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new Filter(source);
	char *path = obs_module_file("effects/hdr-sdr-in-hdr.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "[sdr-in-hdr] failed to load %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
	bfree(errors);
	bfree(path);
	update(f, settings);
	return f;
}

void destroy(void *data)
{
	auto *f = static_cast<Filter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	f->input.destroy();
	gs_texture_destroy(f->out);
	obs_leave_graphics();
	delete f;
}

void defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "mode", sh::ModeGraphics);
	obs_data_set_default_double(s, "white_nits", 203.0);
	obs_data_set_default_double(s, "peak_nits", 0.0);
	obs_data_set_default_double(s, "knee", 75.0);
	obs_data_set_default_bool(s, "force_render", false);
}

std::string level_text(double white, double peak)
{
	const double lw = obs_get_video_hdr_nominal_peak_level();
	char t[400];
	snprintf(t, sizeof(t),
		 "%s %.0f nits = %.1f%% HLG on this OBS's %.0f-nit HDR peak (BT.2408 reference white: "
		 "203 nits = 75%%). OBS's global SDR white level is %.0f nits.",
		 obs_module_text("SdrHdr.Level"), white, 100.0 * sh::nits_to_hlg(white, lw), lw,
		 (double)obs_get_video_sdr_white_level());
	std::string r = t;
	if (peak > white) {
		snprintf(t, sizeof(t), " Expanded SDR 100%%: %.0f nits = %.1f%% HLG.", peak,
			 100.0 * sh::nits_to_hlg(peak, lw));
		r += t;
	}
	return r;
}

// Refreshing the properties view on every slider step would steal focus while typing, so
// only a mode change (which changes visibility) refreshes; the level text is updated in
// place and shown in full on the next refresh.
bool level_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const sh::Params p = read_params(settings);
	obs_property_t *lv = obs_properties_get(props, "level");
	if (lv)
		obs_property_set_description(
			lv, level_text(p.white_nits, p.mode == sh::ModeVideo ? p.peak_nits : 0).c_str());
	const bool video = p.mode == sh::ModeVideo;
	obs_property_set_visible(obs_properties_get(props, "peak_nits"), video);
	obs_property_set_visible(obs_properties_get(props, "knee"), video);
	return strcmp(obs_property_name(prop), "mode") == 0;
}

obs_properties_t *properties(void *data)
{
	auto *f = static_cast<Filter *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("SdrHdr.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "mode", obs_module_text("SdrHdr.Mode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("SdrHdr.Mode.Graphics"), sh::ModeGraphics);
	obs_property_list_add_int(p, obs_module_text("SdrHdr.Mode.Video"), sh::ModeVideo);
	obs_property_set_modified_callback(p, level_modified);
	p = obs_properties_add_float_slider(props, "white_nits", obs_module_text("SdrHdr.White"), 20.0, 1000.0, 1.0);
	obs_property_float_set_suffix(p, " nits");
	obs_property_set_modified_callback(p, level_modified);
	sh::Params cur;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		cur = f->params;
	}
	obs_properties_add_text(props, "level",
				level_text(cur.white_nits, cur.mode == sh::ModeVideo ? cur.peak_nits : 0).c_str(),
				OBS_TEXT_INFO);
	p = obs_properties_add_float_slider(props, "peak_nits", obs_module_text("SdrHdr.Peak"), 0.0, 4000.0, 1.0);
	obs_property_float_set_suffix(p, " nits");
	obs_property_set_long_description(p, obs_module_text("SdrHdr.Peak.Help"));
	obs_property_set_modified_callback(p, level_modified);
	p = obs_properties_add_float_slider(props, "knee", obs_module_text("SdrHdr.Knee"), 50.0, 95.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_bool(props, "force_render", obs_module_text("SdrHdr.ForceRender"));
	return props;
}

struct StateGuard {
	gs_texture_t *rt;
	gs_zstencil_t *zs;
	enum gs_color_space space;
	bool prev_srgb;
	StateGuard()
	{
		rt = gs_get_render_target();
		zs = gs_get_zstencil_target();
		space = gs_get_color_space();
		prev_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();
		gs_enable_blending(false);
		gs_viewport_push();
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
	}
	~StateGuard()
	{
		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(prev_srgb);
		gs_set_render_target_with_color_space(rt, zs, space);
	}
};

void render(void *data, gs_effect_t *)
{
	auto *f = static_cast<Filter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	sh::Params p;
	bool force = false;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		p = f->params;
		force = f->force_render;
	}
	p.obs_sdr_white = obs_get_video_sdr_white_level();
	sh::sanitize(p);
	const uint64_t gen = f->generation.load();
	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[sdr-in-hdr:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	const bool on = active(f, ctx.input_space);
	{
		char line[256];
		if (!canvas_hdr())
			snprintf(line, sizeof(line), "SDR canvas: passing through");
		else if (!sdr_input(ctx.input_space))
			snprintf(line, sizeof(line), "input is %s, not SDR: passing through",
				 hdrtk::space_name(ctx.input_space));
		else
			snprintf(
				line, sizeof(line),
				"%s: SDR white -> %.0f nits (OBS SDR white %.0f), expansion peak %.0f nits, knee %.0f%%",
				p.mode == sh::ModeVideo ? "video (BT.1886)" : "graphics", p.white_nits, p.obs_sdr_white,
				p.mode == sh::ModeVideo ? p.peak_nits : p.white_nits, 100.0 * p.knee);
		if (f->last_state != line) {
			f->last_state = line;
			obs_log(on ? LOG_INFO : LOG_WARNING, "[sdr-in-hdr:%s] %s", obs_source_get_name(f->context),
				line);
		}
	}
	// identity: graphics at OBS's own SDR white level (forced render proves it)
	const bool identity = p.mode == sh::ModeGraphics && std::fabs(p.white_nits - p.obs_sdr_white) < 1e-9;
	if (!on || !ctx.width || !ctx.height || (identity && !force)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (f->out_valid && t == f->last_time && gen == f->last_gen) {
		hdrtk::draw_filter_result(f->out);
		return;
	}
	gs_texture_t *tex = hdrtk::capture_filter_input(f->context, f->input, ctx);
	if (!tex) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (!f->out || f->w != ctx.width || f->h != ctx.height) {
		gs_texture_destroy(f->out);
		f->out = gs_texture_create(ctx.width, ctx.height, GS_RGBA16F, 1, nullptr, GS_RENDER_TARGET);
		f->w = ctx.width;
		f->h = ctx.height;
	}
	if (!f->out) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	const sh::ShaderParams sp = sh::make_shader_params(p);
	const bool srgb_view = ctx.staging_format == GS_RGBA || ctx.staging_format == GS_BGRA ||
			       ctx.staging_format == GS_BGRX;
	{
		StateGuard guard;
		gs_effect_t *e = f->effect;
		gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
		if (srgb_view)
			gs_effect_set_texture_srgb(img, tex);
		else
			gs_effect_set_texture(img, tex);
		gs_effect_set_int(gs_effect_get_param_by_name(e, "mode"), sp.mode);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "gain"), sp.gain);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "knee"), sp.knee);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "expand"), sp.expand);
		struct vec2 os;
		vec2_set(&os, (float)ctx.width, (float)ctx.height);
		gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &os);
		gs_set_render_target_with_color_space(f->out, nullptr, GS_CS_SRGB_16F);
		gs_set_viewport(0, 0, (int)ctx.width, (int)ctx.height);
		gs_ortho(0.0f, (float)ctx.width, 0.0f, (float)ctx.height, -100.0f, 100.0f);
		while (gs_effect_loop(e, "Place"))
			gs_draw_sprite(nullptr, 0, ctx.width, ctx.height);
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
}

enum gs_color_space color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<Filter *>(data);
	const enum gs_color_space native = hdrtk::query_native_space(obs_filter_get_target(f->context));
	return active(f, native) ? GS_CS_SRGB_16F : native;
}

} // namespace

extern "C" void hdrtk_register_sdr_in_hdr_filter(void)
{
	struct obs_source_info info = {};
	info.id = kId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.video_render = render;
	info.video_get_color_space = color_space;
	obs_register_source(&info);
}
