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
 * HDR Input (hdr_toolkit_input_v1, schema 1), docs/HDR_INPUT.md. Put it first on a capture
 * source.
 *  - Reinterpret: an HLG signal that OBS decoded as SDR (DeckLink without HDR metadata) is
 *    turned back into exactly what OBS produces for a correctly tagged HLG frame.
 *  - Analyse signal: luma bit depth (6-10), clamp shares, brightest level.
 * This filter is the deliberate exception to "do not decode HLG": it performs the decode
 * OBS should have performed, with OBS's own formulas, and nothing else.
 */

#include "input-math.hpp"
#include "signal-meter.hpp"
#include "shared/filter-io.hpp"
#include "shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <atomic>
#include <mutex>
#include <string>

namespace {

namespace in = hdrtk::input;

constexpr const char *kId = "hdr_toolkit_input_v1";
constexpr int kSchemaVersion = 1;
enum Mode { ModePass = 0, ModeHlgAsSdr = 1 };

struct Settings {
	int mode = ModePass;
	in::Matrix matrix = in::Bt2020;
	bool limited = true;
};

struct InputFilter {
	obs_source_t *context = nullptr;
	std::mutex mutex;
	Settings settings;
	std::atomic<uint64_t> generation{0};
	in::SignalMeter meter;

	// graphics thread only
	gs_effect_t *effect = nullptr;
	hdrtk::FilterInput input;
	gs_texture_t *out = nullptr;
	uint32_t w = 0, h = 0;
	uint64_t last_time = 0, last_gen = ~0ull;
	bool out_valid = false;
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
	int last_state = -1;

	explicit InputFilter(obs_source_t *src)
		: context(src),
		  meter(std::string("[input:") + obs_source_get_name(src) + "]")
	{
	}
};

Settings read_settings(obs_data_t *s)
{
	Settings r;
	r.mode = (int)obs_data_get_int(s, "mode") == ModeHlgAsSdr ? ModeHlgAsSdr : ModePass;
	const long long m = obs_data_get_int(s, "matrix");
	r.matrix = m == in::Bt601 ? in::Bt601 : (m == in::Bt709 ? in::Bt709 : in::Bt2020);
	r.limited = !obs_data_get_bool(s, "full_range");
	return r;
}

Settings snapshot(InputFilter *f)
{
	std::lock_guard<std::mutex> lock(f->mutex);
	return f->settings;
}

// Reinterpretation applies only to SDR-decoded 16-bit float input (DeckLink 10-bit YUV).
// The output keeps the parent's colour space (SRGB_16F, 1.0 = SDR white) and carries the
// HDR values above 1.0 in it: libobs asks the async parent, not the filter chain, for the
// item's colour space, so reporting 709_EXTENDED here would make libobs tone-map our
// output back into SDR (source_render: 709_EXTENDED inside an SRGB_16F target ->
// DrawTonemap). SRGB_16F -> 709_EXTENDED is a plain copy, so the values reach an HDR canvas
// unchanged. An 8-bit input (GS_CS_SRGB) cannot carry them at all.
bool reinterpreting(const Settings &s, enum gs_color_space native)
{
	return s.mode == ModeHlgAsSdr && native == GS_CS_SRGB_16F;
}

const char *input_name(void *)
{
	return obs_module_text("Input.Name");
}

void input_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<InputFilter *>(data);
	const Settings r = read_settings(s);
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		f->settings = r;
	}
	f->generation++;
}

void *input_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new InputFilter(source);
	char *path = obs_module_file("effects/hdr-input.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "[input] failed to load %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
	bfree(errors);
	bfree(path);
	input_update(f, settings);
	return f;
}

void input_destroy(void *data)
{
	auto *f = static_cast<InputFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	f->input.destroy();
	gs_texture_destroy(f->out);
	f->meter.free();
	obs_leave_graphics();
	delete f;
}

void input_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "mode", ModePass);
	obs_data_set_default_int(s, "matrix", in::Bt2020);
	obs_data_set_default_bool(s, "full_range", false);
}

bool analyse_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<InputFilter *>(data);
	const Settings s = snapshot(f);
	in::AnalysisSetup a;
	a.matrix = s.matrix;
	a.limited = s.limited;
	a.hlg = in::hlg_params(obs_get_video_sdr_white_level(), obs_get_video_hdr_nominal_peak_level());
	f->meter.request(a);
	return false;
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

obs_properties_t *input_properties(void *data)
{
	auto *f = static_cast<InputFilter *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Input.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "mode", obs_module_text("Input.Mode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Input.Mode.Pass"), ModePass);
	obs_property_list_add_int(p, obs_module_text("Input.Mode.HlgAsSdr"), ModeHlgAsSdr);
	p = obs_properties_add_list(props, "matrix", obs_module_text("Input.Matrix"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Input.Matrix.2020"), in::Bt2020);
	obs_property_list_add_int(p, obs_module_text("Input.Matrix.709"), in::Bt709);
	obs_property_list_add_int(p, obs_module_text("Input.Matrix.601"), in::Bt601);
	obs_properties_add_bool(props, "full_range", obs_module_text("Input.FullRange"));

	obs_properties_t *an = obs_properties_create();
	obs_properties_add_text(an, "analysis_info", obs_module_text("Input.Analysis.Info"), OBS_TEXT_INFO);
	const std::string res = f ? f->meter.summary() : std::string();
	obs_properties_add_text(an, "analysis_result",
				res.empty() ? obs_module_text("Input.Analysis.None") : res.c_str(), OBS_TEXT_INFO);
	obs_properties_add_button2(an, "analyse", obs_module_text("Input.Analysis.Start"), analyse_clicked, data);
	obs_properties_add_group(props, "analysis", obs_module_text("Input.Analysis"), OBS_GROUP_NORMAL, an);
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

void set_vec3(gs_effect_t *e, const char *n, const float *v)
{
	struct vec3 x;
	vec3_set(&x, v[0], v[1], v[2]);
	gs_effect_set_vec3(gs_effect_get_param_by_name(e, n), &x);
}

void input_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<InputFilter *>(data);
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
		obs_log(LOG_INFO, "[input:%s] %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	const bool reint = reinterpreting(s, ctx.input_space) && f->effect;
	const in::HlgParams hp = in::hlg_params(ctx.sdr_white_nits, ctx.hdr_peak_nits);
	const int state = (s.mode << 4) | (reint ? 1 : 0) | ((int)s.matrix << 2);
	if (state != f->last_state) {
		f->last_state = state;
		if (s.mode == ModeHlgAsSdr && ctx.input_space == GS_CS_SRGB)
			obs_log(LOG_WARNING,
				"[input:%s] 'HLG decoded as SDR' selected but the input is 8-bit SDR: it cannot carry HDR "
				"values. Set the capture source's pixel format to 10-bit YUV. Passing it through "
				"unchanged",
				obs_source_get_name(f->context));
		else if (s.mode == ModeHlgAsSdr && !reint)
			obs_log(LOG_WARNING,
				"[input:%s] 'HLG decoded as SDR' selected but the input is %s: OBS already decoded it "
				"as HDR, passing it through unchanged",
				obs_source_get_name(f->context), hdrtk::space_name(ctx.input_space));
		else if (reint)
			obs_log(LOG_INFO,
				"[input:%s] reinterpreting the SDR-decoded signal as HLG: matrix %s, HLG exponent %.4f, "
				"peak / SDR white %.4f (peak %.0f, SDR white %.0f nits)",
				obs_source_get_name(f->context),
				s.matrix == in::Bt2020
					? "BT.2020"
					: (s.matrix == in::Bt709 ? "BT.709 (corrected)" : "BT.601 (corrected)"),
				hp.exponent, hp.max_over_sdr, ctx.hdr_peak_nits, ctx.sdr_white_nits);
	}
	const bool measuring = f->meter.active();
	if (!ctx.width || !ctx.height || (!reint && !measuring)) {
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
	if (measuring) {
		obs_source_t *src = f->context;
		f->meter.sample(tex, ctx.input_space, [src]() {
			obs_queue_task(OBS_TASK_UI, refresh_properties_task, obs_source_get_weak_source(src), false);
		});
	}
	if (!reint) {
		// analysing only: the input is drawn unchanged
		f->out_valid = false;
		hdrtk::draw_filter_result(tex);
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
	const in::ShaderParams sp = in::make_shader_params(in::matrix_correction(s.matrix), hp);
	{
		StateGuard guard;
		gs_effect_t *e = f->effect;
		gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
		gs_effect_set_texture(img, tex); // RGBA16F, linear
		set_vec3(e, "corr0", sp.corr[0]);
		set_vec3(e, "corr1", sp.corr[1]);
		set_vec3(e, "corr2", sp.corr[2]);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "hlg_exp"), sp.exponent);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "max_over_sdr"), sp.max_over_sdr);
		struct vec2 os;
		vec2_set(&os, (float)ctx.width, (float)ctx.height);
		gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &os);
		gs_set_render_target_with_color_space(f->out, nullptr, GS_CS_SRGB_16F);
		gs_set_viewport(0, 0, (int)ctx.width, (int)ctx.height);
		gs_ortho(0.0f, (float)ctx.width, 0.0f, (float)ctx.height, -100.0f, 100.0f);
		while (gs_effect_loop(e, "ReinterpretHLG"))
			gs_draw_sprite(nullptr, 0, ctx.width, ctx.height);
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
}

// Always the parent's space; see reinterpreting().
enum gs_color_space input_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<InputFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_input_filter(void)
{
	struct obs_source_info info = {};
	info.id = kId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = input_name;
	info.create = input_create;
	info.destroy = input_destroy;
	info.update = input_update;
	info.get_defaults = input_defaults;
	info.get_properties = input_properties;
	info.video_render = input_render;
	info.video_get_color_space = input_color_space;
	obs_register_source(&info);
}
