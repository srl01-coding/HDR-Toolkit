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
 * P0 neutral HDR filter (hdr_toolkit_neutral_dev).
 *
 * Purpose: prove the HDR render path before any geometry or grading exists.
 * It executes a real shader (unpremultiply -> nominal nits -> back -> premultiply)
 * through obs_source_process_filter_begin_with_color_space with
 * OBS_NO_DIRECT_RENDERING, in the target's native colour space with a float
 * intermediate for every non-8-bit space. "Force render identity" (default on)
 * guarantees the shader runs even when the result is mathematically identity.
 *
 * The developer test gain exists only to prove the shader is live and that
 * values above SDR white survive this filter; it must be 0 EV in production.
 */

#include "../shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cmath>
#include <mutex>

namespace {

struct Params {
	bool force_render = true;
	double test_gain_ev = 0.0;
};

struct NeutralFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;

	gs_eparam_t *p_nits_per_unit = nullptr;
	gs_eparam_t *p_units_per_nit = nullptr;
	gs_eparam_t *p_test_gain = nullptr;
	gs_eparam_t *p_alpha_epsilon = nullptr;

	std::mutex mutex; // guards `params`
	Params params;

	// graphics thread only
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
};

constexpr float kAlphaEpsilon = 1e-6f;

const char *neutral_get_name(void *)
{
	return obs_module_text("Neutral.Name");
}

void neutral_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<NeutralFilter *>(data);
	Params p;
	p.force_render = obs_data_get_bool(settings, "force_render_identity");
	p.test_gain_ev = obs_data_get_double(settings, "dev_test_gain_ev");
	if (!std::isfinite(p.test_gain_ev) || p.test_gain_ev < -8.0 || p.test_gain_ev > 8.0) {
		obs_log(LOG_WARNING, "[neutral] invalid test gain %g EV ignored", p.test_gain_ev);
		p.test_gain_ev = 0.0;
	}
	std::lock_guard<std::mutex> lock(f->mutex);
	f->params = p;
}

void *neutral_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new NeutralFilter();
	f->context = source;

	char *path = obs_module_file("effects/hdr-neutral.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	if (f->effect) {
		f->p_nits_per_unit = gs_effect_get_param_by_name(f->effect, "nits_per_unit");
		f->p_units_per_nit = gs_effect_get_param_by_name(f->effect, "units_per_nit");
		f->p_test_gain = gs_effect_get_param_by_name(f->effect, "test_gain");
		f->p_alpha_epsilon = gs_effect_get_param_by_name(f->effect, "alpha_epsilon");
	}
	obs_leave_graphics();

	if (!f->effect) {
		// Fail creation cleanly rather than installing a silently black filter.
		obs_log(LOG_ERROR, "[neutral] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		bfree(errors);
		bfree(path);
		delete f;
		return nullptr;
	}
	bfree(errors);
	bfree(path);

	neutral_update(f, settings);
	return f;
}

void neutral_destroy(void *data)
{
	auto *f = static_cast<NeutralFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	delete f;
}

void neutral_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "force_render_identity", true);
	obs_data_set_default_double(settings, "dev_test_gain_ev", 0.0);
}

obs_properties_t *neutral_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Neutral.Info"), OBS_TEXT_INFO);
	obs_properties_add_bool(props, "force_render_identity", obs_module_text("Neutral.ForceRender"));
	obs_property_t *p = obs_properties_add_float_slider(props, "dev_test_gain_ev",
							    obs_module_text("Neutral.TestGain"), -8.0, 8.0, 0.05);
	obs_property_float_set_suffix(p, " EV");
	return props;
}

void neutral_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<NeutralFilter *>(data);

	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	Params params;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		params = f->params;
	}

	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[neutral] '%s': %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
	}

	if (!ctx.width || !ctx.height) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const bool identity = params.test_gain_ev == 0.0;
	if (identity && !params.force_render) {
		// Optimised identity path: no shader. Only valid once the forced path
		// has been shown to be identical (docs/VALIDATION.md, gate N1/N2).
		obs_source_skip_video_filter(f->context);
		return;
	}

	// staging_space = input_space; float storage for every non-8-bit space.
	if (!obs_source_process_filter_begin_with_color_space(f->context, ctx.staging_format, ctx.input_space,
							      OBS_NO_DIRECT_RENDERING))
		return; // begin already skipped/handled; never render twice

	const double npu = hdrtk::nits_per_unit(ctx.input_space);
	gs_effect_set_float(f->p_nits_per_unit, (float)npu);
	gs_effect_set_float(f->p_units_per_nit, (float)(1.0 / npu));
	gs_effect_set_float(f->p_test_gain, (float)std::exp2(params.test_gain_ev));
	gs_effect_set_float(f->p_alpha_epsilon, kAlphaEpsilon);

	// Output is premultiplied, composite with ONE / INVSRCALPHA.
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, "Neutral");
	gs_blend_state_pop();
}

enum gs_color_space neutral_color_space(void *data, size_t, const enum gs_color_space *)
{
	// Preserve native space; libobs converts at the consumer boundary.
	auto *f = static_cast<NeutralFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_neutral_filter(void)
{
	struct obs_source_info info = {};
	info.id = "hdr_toolkit_neutral_dev";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = neutral_get_name;
	info.create = neutral_create;
	info.destroy = neutral_destroy;
	info.update = neutral_update;
	info.get_defaults = neutral_defaults;
	info.get_properties = neutral_properties;
	info.video_render = neutral_render;
	info.video_get_color_space = neutral_color_space;
	obs_register_source(&info);
}
