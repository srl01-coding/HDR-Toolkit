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
 * HDR Sharpen (hdr_toolkit_sharpen_v1, schema 1): unsharp mask on log luma, applied to
 * RGB as a gain (sharpen-math.hpp, docs/SHARPEN.md). Works in the source's native
 * space (RGBA16F on HDR) with no clamp. OBS's own Sharpen filter skips HDR sources.
 *
 * Passes: capture input -> BlurH (RG32F) -> Combine (RGBA16F) -> draw like libobs.
 * Rendered once per video frame; repeated renders (preview, multiview) reuse the result.
 */

#include "sharpen-math.hpp"
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

namespace sh = hdrtk::sharpen;

constexpr const char *kId = "hdr_toolkit_sharpen_v1";
constexpr int kSchemaVersion = 1;

struct Settings {
	sh::Params p;
	int debug_view = 0; // 0 normal, 1 detail, 2 change
	double debug_gain = 4.0;
	bool force_render = false;
};

struct SharpenFilter {
	obs_source_t *context = nullptr;
	std::mutex mutex;
	Settings settings;
	std::atomic<uint64_t> generation{0};

	// graphics thread only
	gs_effect_t *effect = nullptr;
	hdrtk::FilterInput input;
	gs_texture_t *blur = nullptr, *out = nullptr;
	uint32_t w = 0, h = 0;
	uint64_t last_time = 0, last_gen = ~0ull;
	bool out_valid = false;
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
};

Settings read_settings(obs_data_t *s)
{
	Settings r;
	r.p.amount = obs_data_get_double(s, "amount") / 100.0;
	r.p.radius = obs_data_get_double(s, "radius");
	r.p.threshold = obs_data_get_double(s, "threshold");
	r.p.limit = obs_data_get_double(s, "halo_limit");
	r.p.floor_nits = obs_data_get_double(s, "dark_floor_nits");
	sh::sanitize(r.p);
	r.debug_view = (int)obs_data_get_int(s, "debug_view");
	r.debug_gain = obs_data_get_double(s, "debug_gain");
	r.force_render = obs_data_get_bool(s, "force_render");
	return r;
}

const char *sharpen_name(void *)
{
	return obs_module_text("Sharpen.Name");
}

void sharpen_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<SharpenFilter *>(data);
	const Settings r = read_settings(s);
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->settings = r;
	}
	f->generation++;
}

void *sharpen_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new SharpenFilter();
	f->context = source;
	char *path = obs_module_file("effects/hdr-sharpen.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "[sharpen] failed to load %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
	bfree(errors);
	bfree(path);
	sharpen_update(f, settings);
	return f;
}

void sharpen_destroy(void *data)
{
	auto *f = static_cast<SharpenFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	f->input.destroy();
	gs_texture_destroy(f->blur);
	gs_texture_destroy(f->out);
	obs_leave_graphics();
	delete f;
}

void sharpen_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_double(s, "amount", 0.0);
	obs_data_set_default_double(s, "radius", 1.0);
	obs_data_set_default_double(s, "threshold", 0.02);
	obs_data_set_default_double(s, "halo_limit", 0.5);
	obs_data_set_default_double(s, "dark_floor_nits", 2.0);
	obs_data_set_default_int(s, "debug_view", 0);
	obs_data_set_default_double(s, "debug_gain", 4.0);
	obs_data_set_default_bool(s, "force_render", false);
}

obs_properties_t *sharpen_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Sharpen.Info"), OBS_TEXT_INFO);
	obs_property_t *p =
		obs_properties_add_float_slider(props, "amount", obs_module_text("Sharpen.Amount"), 0.0, 300.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "radius", obs_module_text("Sharpen.Radius"), sh::kRadiusMin,
					    sh::kRadiusMax, 0.05);
	obs_property_float_set_suffix(p, " px");
	obs_properties_add_float_slider(props, "threshold", obs_module_text("Sharpen.Threshold"), 0.0,
					sh::kThresholdMax, 0.005);
	obs_properties_add_float_slider(props, "halo_limit", obs_module_text("Sharpen.HaloLimit"), sh::kLimitMin,
					sh::kLimitMax, 0.01);
	obs_properties_add_float_slider(props, "dark_floor_nits", obs_module_text("Sharpen.DarkFloor"), sh::kFloorMin,
					sh::kFloorMax, 0.01);

	obs_properties_t *dbg = obs_properties_create();
	p = obs_properties_add_list(dbg, "debug_view", obs_module_text("Sharpen.DebugView"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Sharpen.DebugView.Normal"), 0);
	obs_property_list_add_int(p, obs_module_text("Sharpen.DebugView.Detail"), 1);
	obs_property_list_add_int(p, obs_module_text("Sharpen.DebugView.Change"), 2);
	obs_properties_add_float_slider(dbg, "debug_gain", obs_module_text("Sharpen.DebugGain"), 1.0, 256.0, 1.0);
	obs_properties_add_bool(dbg, "force_render", obs_module_text("Sharpen.ForceRender"));
	obs_properties_add_group(props, "diagnostics", obs_module_text("Sharpen.Diagnostics"), OBS_GROUP_NORMAL, dbg);
	return props;
}

void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, n), v);
}

void set_v2(gs_effect_t *e, const char *n, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, n), &v);
}

// Pushes and restores everything the passes change.
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

bool ensure_textures(SharpenFilter *f, uint32_t w, uint32_t h)
{
	if (f->blur && f->w == w && f->h == h)
		return f->out != nullptr;
	gs_texture_destroy(f->blur);
	gs_texture_destroy(f->out);
	f->blur = gs_texture_create(w, h, GS_RG32F, 1, nullptr, GS_RENDER_TARGET);
	f->out = gs_texture_create(w, h, GS_RGBA16F, 1, nullptr, GS_RENDER_TARGET);
	f->w = w;
	f->h = h;
	f->out_valid = false;
	return f->blur && f->out;
}

// Input binding: 8-bit SDR through its sRGB view (linear values in the shader).
void bind_image(gs_effect_t *e, gs_texture_t *in, enum gs_color_format fmt)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, "image");
	if (fmt == GS_RGBA || fmt == GS_BGRA || fmt == GS_BGRX)
		gs_effect_set_texture_srgb(p, in);
	else
		gs_effect_set_texture(p, in);
}

void run(gs_effect_t *e, gs_texture_t *target, enum gs_color_space space, const char *tech, uint32_t w, uint32_t h)
{
	gs_set_render_target_with_color_space(target, nullptr, space);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	set_v2(e, "out_size", (float)w, (float)h);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(nullptr, 0, w, h);
}

void sharpen_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<SharpenFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent || !f->effect) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	Settings s;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->settings;
	}
	const uint64_t gen = f->generation.load();
	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[sharpen:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	if (!ctx.width || !ctx.height || (sh::neutral(s.p) && s.debug_view == 0 && !s.force_render)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (f->out_valid && t == f->last_time && gen == f->last_gen) {
		hdrtk::draw_filter_result(f->out);
		return;
	}
	gs_texture_t *in = hdrtk::capture_filter_input(f->context, f->input, ctx);
	if (!in || !ensure_textures(f, ctx.width, ctx.height)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const double npu = hdrtk::nits_per_unit(ctx.input_space);
	const sh::ShaderParams sp = sh::make_shader_params(s.p, npu);
	// float output: linear values; an 8-bit SDR input is written as SRGB_16F
	const enum gs_color_space out_space = ctx.input_space == GS_CS_SRGB ? GS_CS_SRGB_16F : ctx.input_space;
	{
		StateGuard guard;
		gs_effect_t *e = f->effect;
		set_v2(e, "in_size", (float)ctx.width, (float)ctx.height);
		set_f(e, "k_units", sp.k_units);
		set_f(e, "taps", sp.taps);
		set_f(e, "inv_2s2", sp.inv_2s2);
		set_f(e, "amount", sp.amount);
		set_f(e, "limit", sp.limit);
		set_f(e, "thresh2", sp.thresh2);
		set_f(e, "debug_mode", (float)s.debug_view);
		set_f(e, "debug_gain", (float)s.debug_gain);
		bind_image(e, in, ctx.staging_format);
		run(e, f->blur, out_space, "BlurH", ctx.width, ctx.height);
		// parameters are reset after each technique: set them again
		set_v2(e, "in_size", (float)ctx.width, (float)ctx.height);
		set_f(e, "k_units", sp.k_units);
		set_f(e, "taps", sp.taps);
		set_f(e, "inv_2s2", sp.inv_2s2);
		set_f(e, "amount", sp.amount);
		set_f(e, "limit", sp.limit);
		set_f(e, "thresh2", sp.thresh2);
		set_f(e, "debug_mode", (float)s.debug_view);
		set_f(e, "debug_gain", (float)s.debug_gain);
		bind_image(e, in, ctx.staging_format);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "blur_tex"), f->blur);
		run(e, f->out, out_space, "Combine", ctx.width, ctx.height);
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
}

enum gs_color_space sharpen_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<SharpenFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_sharpen_filter(void)
{
	struct obs_source_info info = {};
	info.id = kId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = sharpen_name;
	info.create = sharpen_create;
	info.destroy = sharpen_destroy;
	info.update = sharpen_update;
	info.get_defaults = sharpen_defaults;
	info.get_properties = sharpen_properties;
	info.video_render = sharpen_render;
	info.video_get_color_space = sharpen_color_space;
	obs_register_source(&info);
}
