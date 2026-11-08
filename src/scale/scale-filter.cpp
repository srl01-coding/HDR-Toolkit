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
 * HDR Scale (hdr_toolkit_scale_v1, schema 1), docs/HDR_SCALE.md. Resizes, desqueezes,
 * crops and punches in on a source, so the scene item can sit at 1:1 and OBS does no
 * resampling of its own. OBS's Lanczos works in linear light without a ringing clamp,
 * which puts black halos next to HDR highlights; this filter interpolates enlarging axes
 * in the PQ domain with anti-ringing and filters reducing axes in linear light.
 *
 * Passes: capture input -> ResampleH (RGBA32F, out_w x in_h) -> ResampleV (RGBA16F,
 * out_w x out_h) -> draw like libobs. Native colour space kept, no clamp. Rendered once
 * per video frame.
 */

#include "scale-math.hpp"
#include "shared/filter-io.hpp"
#include "shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace {

namespace sc = hdrtk::scale;

constexpr const char *kId = "hdr_toolkit_scale_v1";
constexpr int kSchemaVersion = 1;

struct Settings {
	sc::Params p;
	bool force_render = false;
};

struct ScaleFilter {
	obs_source_t *context = nullptr;
	std::mutex mutex;
	Settings settings;
	std::atomic<uint64_t> generation{0};

	// graphics thread only
	gs_effect_t *effect = nullptr;
	hdrtk::FilterInput input;
	gs_texture_t *mid = nullptr, *out = nullptr;
	uint32_t mid_w = 0, mid_h = 0, out_w = 0, out_h = 0;
	uint64_t last_time = 0, last_gen = ~0ull;
	bool out_valid = false;
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
	std::string last_layout;

	explicit ScaleFilter(obs_source_t *src) : context(src) {}
};

Settings read_settings(obs_data_t *s)
{
	Settings r;
	r.p.out_w = (int)obs_data_get_int(s, "out_w");
	r.p.out_h = (int)obs_data_get_int(s, "out_h");
	r.p.fit = (sc::Fit)obs_data_get_int(s, "fit");
	r.p.desqueeze = obs_data_get_double(s, "desqueeze");
	r.p.zoom = obs_data_get_double(s, "zoom");
	r.p.pan_x = obs_data_get_double(s, "pan_x") / 100.0;
	r.p.pan_y = obs_data_get_double(s, "pan_y") / 100.0;
	r.p.kernel = (sc::Kernel)obs_data_get_int(s, "kernel");
	r.p.antiring = obs_data_get_double(s, "antiring") / 100.0;
	r.p.domain = (sc::Domain)obs_data_get_int(s, "domain");
	sc::sanitize(r.p);
	r.force_render = obs_data_get_bool(s, "force_render");
	return r;
}

Settings snapshot(ScaleFilter *f)
{
	std::lock_guard<std::mutex> lock(f->mutex);
	return f->settings;
}

sc::Layout layout_for(ScaleFilter *f, const Settings &s)
{
	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return sc::Layout();
	return sc::compute_layout(obs_source_get_base_width(target), obs_source_get_base_height(target), s.p);
}

bool is_identity(const sc::Layout &l)
{
	return sc::axis_identity(l.x) && sc::axis_identity(l.y) && std::fabs(l.x.offset) < 1e-9 &&
	       std::fabs(l.y.offset) < 1e-9;
}

const char *scale_name(void *)
{
	return obs_module_text("Scale.Name");
}

void scale_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<ScaleFilter *>(data);
	const Settings r = read_settings(s);
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->settings = r;
	}
	f->generation++;
}

void *scale_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new ScaleFilter(source);
	char *path = obs_module_file("effects/hdr-scale.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "[scale] failed to load %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
	bfree(errors);
	bfree(path);
	scale_update(f, settings);
	return f;
}

void scale_destroy(void *data)
{
	auto *f = static_cast<ScaleFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	f->input.destroy();
	gs_texture_destroy(f->mid);
	gs_texture_destroy(f->out);
	obs_leave_graphics();
	delete f;
}

void scale_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "out_w", 0);
	obs_data_set_default_int(s, "out_h", 0);
	obs_data_set_default_int(s, "fit", sc::FitFill);
	obs_data_set_default_double(s, "desqueeze", 1.0);
	obs_data_set_default_double(s, "zoom", 1.0);
	obs_data_set_default_double(s, "pan_x", 0.0);
	obs_data_set_default_double(s, "pan_y", 0.0);
	obs_data_set_default_int(s, "kernel", sc::KernelLanczos3);
	obs_data_set_default_double(s, "antiring", 80.0);
	obs_data_set_default_int(s, "domain", sc::DomainAuto);
	obs_data_set_default_bool(s, "force_render", false);
}

bool canvas_size_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<ScaleFilter *>(data);
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	obs_data_t *s = obs_source_get_settings(f->context);
	obs_data_set_int(s, "out_w", ovi.base_width);
	obs_data_set_int(s, "out_h", ovi.base_height);
	obs_source_update(f->context, s);
	obs_data_release(s);
	return true;
}

obs_properties_t *scale_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Scale.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_int(props, "out_w", obs_module_text("Scale.Width"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(props, "out_h", obs_module_text("Scale.Height"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_button2(props, "canvas_size", obs_module_text("Scale.CanvasSize"), canvas_size_clicked,
				   data);
	p = obs_properties_add_list(props, "fit", obs_module_text("Scale.Fit"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Scale.Fit.Fill"), sc::FitFill);
	obs_property_list_add_int(p, obs_module_text("Scale.Fit.Fit"), sc::FitFit);
	obs_property_list_add_int(p, obs_module_text("Scale.Fit.Stretch"), sc::FitStretch);
	p = obs_properties_add_float(props, "desqueeze", obs_module_text("Scale.Desqueeze"), 0.25, 4.0, 0.001);
	obs_property_set_long_description(p, obs_module_text("Scale.Desqueeze.Help"));
	obs_properties_add_float_slider(props, "zoom", obs_module_text("Scale.Zoom"), 1.0, 4.0, 0.01);
	p = obs_properties_add_float_slider(props, "pan_x", obs_module_text("Scale.PanX"), -100.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "pan_y", obs_module_text("Scale.PanY"), -100.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");

	obs_properties_t *q = obs_properties_create();
	p = obs_properties_add_list(q, "kernel", obs_module_text("Scale.Kernel"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Scale.Kernel.Lanczos3"), sc::KernelLanczos3);
	obs_property_list_add_int(p, obs_module_text("Scale.Kernel.CatmullRom"), sc::KernelCatmullRom);
	obs_property_list_add_int(p, obs_module_text("Scale.Kernel.Mitchell"), sc::KernelMitchell);
	obs_property_list_add_int(p, obs_module_text("Scale.Kernel.Bilinear"), sc::KernelBilinear);
	p = obs_properties_add_float_slider(q, "antiring", obs_module_text("Scale.AntiRing"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_list(q, "domain", obs_module_text("Scale.Domain"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Scale.Domain.Auto"), sc::DomainAuto);
	obs_property_list_add_int(p, obs_module_text("Scale.Domain.Linear"), sc::DomainLinear);
	obs_property_list_add_int(p, obs_module_text("Scale.Domain.Perceptual"), sc::DomainPerceptual);
	obs_properties_add_bool(q, "force_render", obs_module_text("Scale.ForceRender"));
	obs_properties_add_group(props, "quality", obs_module_text("Scale.Quality"), OBS_GROUP_NORMAL, q);
	return props;
}

uint32_t scale_width(void *data)
{
	auto *f = static_cast<ScaleFilter *>(data);
	return layout_for(f, snapshot(f)).out_w;
}

uint32_t scale_height(void *data)
{
	auto *f = static_cast<ScaleFilter *>(data);
	return layout_for(f, snapshot(f)).out_h;
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

bool ensure_tex(gs_texture_t *&tex, uint32_t &tw, uint32_t &th, uint32_t w, uint32_t h, enum gs_color_format fmt)
{
	if (tex && tw == w && th == h)
		return true;
	gs_texture_destroy(tex);
	tex = gs_texture_create(w, h, fmt, 1, nullptr, GS_RENDER_TARGET);
	tw = w;
	th = h;
	return tex != nullptr;
}

void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, n), v);
}

void set_axis(gs_effect_t *e, const sc::AxisShader &a)
{
	set_f(e, "axis_scale", a.scale);
	set_f(e, "axis_offset", a.offset);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "kernel"), a.kernel);
	set_f(e, "radius", a.radius);
	set_f(e, "wscale", a.wscale);
	gs_effect_set_int(gs_effect_get_param_by_name(e, "perceptual"), a.perceptual);
	set_f(e, "pq_k", a.pq_k);
	set_f(e, "antiring", a.antiring);
	set_f(e, "in_len", a.in_len);
}

void pass(gs_effect_t *e, gs_texture_t *src, bool srgb_view, gs_texture_t *target, enum gs_color_space space,
	  const char *tech, uint32_t w, uint32_t h, const sc::AxisShader &a)
{
	gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
	if (srgb_view)
		gs_effect_set_texture_srgb(img, src);
	else
		gs_effect_set_texture(img, src);
	set_axis(e, a);
	struct vec2 os;
	vec2_set(&os, (float)w, (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &os);
	gs_set_render_target_with_color_space(target, nullptr, space);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(nullptr, 0, w, h);
}

void scale_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<ScaleFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent || !f->effect) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	const Settings s = snapshot(f);
	const uint64_t gen = f->generation.load();
	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[scale:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	const sc::Layout l = sc::compute_layout(ctx.width, ctx.height, s.p);
	{
		char line[256];
		snprintf(line, sizeof(line), "%ux%u -> %ux%u, scale x %.4f y %.4f%s", ctx.width, ctx.height, l.out_w,
			 l.out_h, l.x.scale, l.y.scale, is_identity(l) ? " (identity)" : "");
		if (f->last_layout != line) {
			f->last_layout = line;
			obs_log(LOG_INFO, "[scale:%s] %s", obs_source_get_name(f->context), line);
		}
	}
	if (!ctx.width || !ctx.height || !l.out_w || !l.out_h || (is_identity(l) && !s.force_render)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (f->out_valid && t == f->last_time && gen == f->last_gen && f->out_w == l.out_w && f->out_h == l.out_h) {
		hdrtk::draw_filter_result(f->out);
		return;
	}
	gs_texture_t *in = hdrtk::capture_filter_input(f->context, f->input, ctx);
	if (!in || !ensure_tex(f->mid, f->mid_w, f->mid_h, l.out_w, ctx.height, GS_RGBA32F) ||
	    !ensure_tex(f->out, f->out_w, f->out_h, l.out_w, l.out_h, GS_RGBA16F)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const double npu = hdrtk::nits_per_unit(ctx.input_space);
	const sc::AxisShader ax = sc::make_axis_shader(l.x, s.p, ctx.width, npu);
	const sc::AxisShader ay = sc::make_axis_shader(l.y, s.p, ctx.height, npu);
	const bool srgb_view = ctx.staging_format == GS_RGBA || ctx.staging_format == GS_BGRA ||
			       ctx.staging_format == GS_BGRX;
	// float output: linear values; an 8-bit SDR input is written as SRGB_16F
	const enum gs_color_space out_space = ctx.input_space == GS_CS_SRGB ? GS_CS_SRGB_16F : ctx.input_space;
	{
		StateGuard guard;
		pass(f->effect, in, srgb_view, f->mid, out_space, "ResampleH", l.out_w, ctx.height, ax);
		pass(f->effect, f->mid, false, f->out, out_space, "ResampleV", l.out_w, l.out_h, ay);
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
}

enum gs_color_space scale_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<ScaleFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_scale_filter(void)
{
	struct obs_source_info info = {};
	info.id = kId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = scale_name;
	info.create = scale_create;
	info.destroy = scale_destroy;
	info.update = scale_update;
	info.get_defaults = scale_defaults;
	info.get_properties = scale_properties;
	info.get_width = scale_width;
	info.get_height = scale_height;
	info.video_render = scale_render;
	info.video_get_color_space = scale_color_space;
	obs_register_source(&info);
}
