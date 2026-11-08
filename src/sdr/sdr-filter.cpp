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
 * SDR Conversion (hdr_toolkit_sdr_v1, schema 1), docs/SDR_CONVERSION.md. Shows the SDR
 * rendition of an HDR picture (BT.2446-1 Method A, or what OBS itself does on an SDR
 * canvas), for checking before broadcast; on an SDR canvas it is also a proper HDR -> SDR
 * conversion for an HLG camera.
 *
 * Colour space: the SDR result is reported as GS_CS_SRGB_16F, whatever the input. libobs
 * then copies it into an HDR target (1.0 = SDR white) and passes it unchanged into an SDR
 * target, so OBS's own tone map (DrawTonemap, applied to 709_EXTENDED in an SDR target) is
 * never applied on top. Only the split compare on an HDR canvas reports 709_EXTENDED, to
 * carry the HDR original on the left.
 */

#include "sdr-math.hpp"
#include "shared/filter-io.hpp"
#include "shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace {

namespace sdr = hdrtk::sdr;

constexpr const char *kId = "hdr_toolkit_sdr_v1";
constexpr int kSchemaVersion = 1;
enum Method { MethodBt2446A = 0, MethodObs = 1 };

struct Settings {
	int method = MethodBt2446A;
	int peak = 0; // nits; 0 = OBS's HDR nominal peak
	bool split = false;
	int split_pos = 50; // percent of the width
};

struct SdrFilter {
	obs_source_t *context = nullptr;
	std::mutex mutex;
	Settings settings;
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

	explicit SdrFilter(obs_source_t *src) : context(src) {}
};

Settings read_settings(obs_data_t *s)
{
	Settings r;
	r.method = (int)obs_data_get_int(s, "method") == MethodObs ? MethodObs : MethodBt2446A;
	r.peak = (int)std::clamp<long long>(obs_data_get_int(s, "peak"), 0, 10000);
	r.split = obs_data_get_bool(s, "split");
	r.split_pos = (int)std::clamp<long long>(obs_data_get_int(s, "split_pos"), 0, 100);
	return r;
}

Settings snapshot(SdrFilter *f)
{
	std::lock_guard<std::mutex> lock(f->mutex);
	return f->settings;
}

bool canvas_hdr()
{
	struct obs_video_info ovi;
	return obs_get_video_info(&ovi) && (ovi.colorspace == VIDEO_CS_2100_HLG || ovi.colorspace == VIDEO_CS_2100_PQ);
}

// 8-bit SDR input (GS_CS_SRGB) cannot hold HDR values: nothing to convert.
bool converting(const SdrFilter *f, enum gs_color_space native)
{
	return f->effect && native != GS_CS_SRGB;
}

enum gs_color_space output_space(const Settings &s, bool hdr_canvas)
{
	return s.split && hdr_canvas ? GS_CS_709_EXTENDED : GS_CS_SRGB_16F;
}

double conversion_peak(const Settings &s)
{
	return s.peak > 0 ? std::max(100, s.peak) : (double)obs_get_video_hdr_nominal_peak_level();
}

const char *sdr_name(void *)
{
	return obs_module_text("Sdr.Name");
}

void sdr_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<SdrFilter *>(data);
	const Settings r = read_settings(s);
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->settings = r;
	}
	f->generation++;
}

void *sdr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new SdrFilter(source);
	char *path = obs_module_file("effects/hdr-sdr.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "[sdr] failed to load %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
	bfree(errors);
	bfree(path);
	sdr_update(f, settings);
	return f;
}

void sdr_destroy(void *data)
{
	auto *f = static_cast<SdrFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	f->input.destroy();
	gs_texture_destroy(f->out);
	obs_leave_graphics();
	delete f;
}

void sdr_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "method", MethodBt2446A);
	obs_data_set_default_int(s, "peak", 0);
	obs_data_set_default_bool(s, "split", false);
	obs_data_set_default_int(s, "split_pos", 50);
}

obs_properties_t *sdr_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Sdr.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "method", obs_module_text("Sdr.Method"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Sdr.Method.Bt2446A"), MethodBt2446A);
	obs_property_list_add_int(p, obs_module_text("Sdr.Method.Obs"), MethodObs);
	p = obs_properties_add_int(props, "peak", obs_module_text("Sdr.Peak"), 0, 10000, 10);
	obs_property_int_set_suffix(p, " nits");
	obs_property_set_long_description(p, obs_module_text("Sdr.Peak.Help"));
	obs_properties_add_bool(props, "split", obs_module_text("Sdr.Split"));
	p = obs_properties_add_int_slider(props, "split_pos", obs_module_text("Sdr.SplitPos"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	return props;
}

// Pushes and restores everything the pass changes.
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

void set_float(gs_effect_t *e, const char *n, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, n), v);
}

void sdr_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<SdrFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	const Settings s = snapshot(f);
	const uint64_t gen = f->generation.load();
	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[sdr:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	const bool conv = converting(f, ctx.input_space);
	const bool hdr_canvas = canvas_hdr();
	const enum gs_color_space out_space = output_space(s, hdr_canvas);
	sdr::Params prm;
	prm.sdr_white = ctx.sdr_white_nits > 0 ? ctx.sdr_white_nits : 300.0;
	prm.lhdr = conversion_peak(s);
	{
		char line[320];
		if (!conv)
			snprintf(line, sizeof(line), "input is %s: nothing to convert, passing it through",
				 hdrtk::space_name(ctx.input_space));
		else
			snprintf(line, sizeof(line),
				 "converting %s input to SDR: %s, conversion peak %.0f nits, SDR white %.0f nits, %s "
				 "canvas, output %s%s",
				 hdrtk::space_name(ctx.input_space),
				 s.method == MethodObs ? "OBS default (Reinhard)" : "BT.2446-1 Method A", prm.lhdr,
				 prm.sdr_white, hdr_canvas ? "HDR" : "SDR", hdrtk::space_name(out_space),
				 s.split ? ", split compare" : "");
		if (f->last_state != line) {
			f->last_state = line;
			obs_log(conv ? LOG_INFO : LOG_WARNING, "[sdr:%s] %s", obs_source_get_name(f->context), line);
		}
	}
	if (!ctx.width || !ctx.height || !conv) {
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
		f->out_valid = false;
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
	const sdr::ShaderParams sp = sdr::make_shader_params(prm);
	{
		StateGuard guard;
		gs_effect_t *e = f->effect;
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
		set_float(e, "in_scale", (float)(hdrtk::nits_per_unit(ctx.input_space) / prm.sdr_white));
		gs_effect_set_int(gs_effect_get_param_by_name(e, "method"), s.method);
		set_float(e, "to_norm", sp.to_norm);
		set_float(e, "rho_hdr_m1", sp.rho_hdr_m1);
		set_float(e, "inv_log_rho_hdr", sp.inv_log_rho_hdr);
		set_float(e, "log_rho_sdr", sp.log_rho_sdr);
		set_float(e, "inv_rho_sdr_m1", sp.inv_rho_sdr_m1);
		set_float(e, "split_x", s.split ? (float)(ctx.width * s.split_pos / 100.0) : -1.0f);
		gs_effect_set_int(gs_effect_get_param_by_name(e, "left_mode"), hdr_canvas ? 0 : 1);
		struct vec2 os;
		vec2_set(&os, (float)ctx.width, (float)ctx.height);
		gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &os);
		gs_set_render_target_with_color_space(f->out, nullptr, out_space);
		gs_set_viewport(0, 0, (int)ctx.width, (int)ctx.height);
		gs_ortho(0.0f, (float)ctx.width, 0.0f, (float)ctx.height, -100.0f, 100.0f);
		while (gs_effect_loop(e, "Convert"))
			gs_draw_sprite(nullptr, 0, ctx.width, ctx.height);
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
}

enum gs_color_space sdr_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<SdrFilter *>(data);
	const enum gs_color_space native = hdrtk::query_native_space(obs_filter_get_target(f->context));
	if (!converting(f, native))
		return native;
	return output_space(snapshot(f), canvas_hdr());
}

} // namespace

extern "C" void hdrtk_register_sdr_filter(void)
{
	struct obs_source_info info = {};
	info.id = kId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = sdr_name;
	info.create = sdr_create;
	info.destroy = sdr_destroy;
	info.update = sdr_update;
	info.get_defaults = sdr_defaults;
	info.get_properties = sdr_properties;
	info.video_render = sdr_render;
	info.video_get_color_space = sdr_color_space;
	obs_register_source(&info);
}
