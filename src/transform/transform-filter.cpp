/*
OBS HDR Toolkit
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
 * HDR Transform (hdr_toolkit_transform_v1) - P1: Corner Pin, with a pre-warp
 * (schema 2, user-directed 1 Nov 2026): rotation, then barrel / pincushion, both
 * applied to the source before the corner pin (quad-math.hpp PreWarp).
 *
 * Brief section 4. Two warp models, selectable:
 *   Bilinear (StreamFX-style) - inverse-bilinear quad mapping (default)
 *   Projective                - homography, perspective-correct
 * Geometry is validated on the CPU; an invalid quad keeps the last valid one
 * and reports why. Settings store canonical normalised corners (0..1,
 * identity (0,0)-(1,1)); the UI shows percent and writes through.
 *
 * Render contract (docs/COLOR_PIPELINE.md): native colour space, float
 * staging for every non-8-bit space, OBS_NO_DIRECT_RENDERING, premultiplied
 * output, outside the quad = transparent black, no RGB arithmetic.
 */

#include "quad-math.hpp"
#include "../shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using hdrtk::quad::BilinearUniforms;
using hdrtk::quad::ProjectiveUniforms;
using hdrtk::quad::PreWarp;
using hdrtk::quad::PreWarpUniforms;
using hdrtk::quad::Quad;

constexpr int kSchemaVersion = 2; // 2: pre-warp keys (neutral defaults; schema 1 unchanged)

enum WarpModel : int { WarpBilinear = 0, WarpProjective = 1 };
enum Sampling : int { SampleBilinear = 0, SamplePoint = 1 };

struct CornerKey {
	const char *canon_x, *canon_y, *ui_x, *ui_y, *label;
};
// Perimeter order A=TL, B=TR, C=BR, D=BL (brief 4.2); the UI lists TL, TR, BL, BR.
const CornerKey kCorners[4] = {
	{"corner_tl_x", "corner_tl_y", "ui_tl_x", "ui_tl_y", "Transform.Corner.TL"},
	{"corner_tr_x", "corner_tr_y", "ui_tr_x", "ui_tr_y", "Transform.Corner.TR"},
	{"corner_br_x", "corner_br_y", "ui_br_x", "ui_br_y", "Transform.Corner.BR"},
	{"corner_bl_x", "corner_bl_y", "ui_bl_x", "ui_bl_y", "Transform.Corner.BL"},
};
const double kIdentity[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
const int kUiOrder[4] = {0, 1, 3, 2}; // TL, TR, BL, BR

// Immutable per-frame snapshot (brief 10.6).
struct Snapshot {
	bool identity = true;
	int warp = WarpBilinear;
	int sampling = SampleBilinear;
	bool force_render = false;
	BilinearUniforms bil{};
	ProjectiveUniforms proj{};
	PreWarp pre; // uniforms depend on the frame size: computed at render
};

struct TransformFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *p_a = nullptr, *p_e = nullptr, *p_f = nullptr, *p_g = nullptr;
	gs_eparam_t *p_h0 = nullptr, *p_h1 = nullptr, *p_h2 = nullptr, *p_wsign = nullptr;
	gs_eparam_t *p_pre_on = nullptr, *p_pre_cs = nullptr, *p_pre_k = nullptr, *p_pre_inv_hd2 = nullptr,
		    *p_pre_scale = nullptr, *p_pre_size = nullptr;

	std::mutex mutex; // guards snapshot, last_valid, status
	Snapshot snapshot;
	Quad last_valid; // identity until a valid quad is set
	std::string status;

	// graphics thread only
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
	PreWarp pre_cached;
	uint32_t pre_w = 0, pre_h = 0;
	bool pre_valid = false;
	PreWarpUniforms pre_uni{};
};

PreWarp read_prewarp(obs_data_t *s)
{
	PreWarp p;
	p.rotation_deg = obs_data_get_double(s, "pre_rotation_deg");
	p.distortion = obs_data_get_double(s, "pre_distortion");
	p.autofill = obs_data_get_bool(s, "pre_autofill");
	hdrtk::quad::sanitize(p);
	return p;
}

Quad read_quad(obs_data_t *s)
{
	Quad q;
	hdrtk::quad::Vec2 *pts[4] = {&q.a, &q.b, &q.c, &q.d};
	for (int i = 0; i < 4; i++) {
		pts[i]->x = obs_data_get_double(s, kCorners[i].canon_x);
		pts[i]->y = obs_data_get_double(s, kCorners[i].canon_y);
	}
	return q;
}

void write_quad(obs_data_t *s, const Quad &q)
{
	const hdrtk::quad::Vec2 *pts[4] = {&q.a, &q.b, &q.c, &q.d};
	for (int i = 0; i < 4; i++) {
		obs_data_set_double(s, kCorners[i].canon_x, pts[i]->x);
		obs_data_set_double(s, kCorners[i].canon_y, pts[i]->y);
		obs_data_set_double(s, kCorners[i].ui_x, pts[i]->x * 100.0);
		obs_data_set_double(s, kCorners[i].ui_y, pts[i]->y * 100.0);
	}
}

std::string status_text(const hdrtk::quad::Validation &v)
{
	if (v.ok)
		return obs_module_text("Transform.Status.Valid");
	return std::string(obs_module_text("Transform.Status.Invalid")) + " " + v.reason;
}

const char *transform_get_name(void *)
{
	return obs_module_text("Transform.Name");
}

void transform_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<TransformFilter *>(data);

	int warp = (int)obs_data_get_int(settings, "warp_model");
	if (warp != WarpBilinear && warp != WarpProjective)
		warp = WarpBilinear; // unknown future value: fail safe
	int sampling = (int)obs_data_get_int(settings, "sampling_mode");
	if (sampling != SampleBilinear && sampling != SamplePoint)
		sampling = SampleBilinear;

	const Quad q = read_quad(settings);
	const hdrtk::quad::Validation v = hdrtk::quad::validate(q, warp == WarpProjective);

	// Keep the UI percent mirror in step with the canonical values (scripts
	// may set only the canonical keys).
	for (int i = 0; i < 4; i++) {
		obs_data_set_double(settings, kCorners[i].ui_x,
				    obs_data_get_double(settings, kCorners[i].canon_x) * 100);
		obs_data_set_double(settings, kCorners[i].ui_y,
				    obs_data_get_double(settings, kCorners[i].canon_y) * 100);
	}

	std::lock_guard<std::mutex> lock(f->mutex);
	if (v.ok)
		f->last_valid = q;
	else
		obs_log(LOG_WARNING, "[transform] '%s': %s; keeping last valid geometry",
			obs_source_get_name(f->context), v.reason.c_str());
	f->status = status_text(v);

	// The last valid quad may have been validated for the other warp model.
	Quad use = f->last_valid;
	if (!hdrtk::quad::validate(use, warp == WarpProjective).ok)
		use = Quad();

	Snapshot s;
	s.pre = read_prewarp(settings);
	s.identity = hdrtk::quad::is_identity(use) && hdrtk::quad::prewarp_identity(s.pre);
	s.warp = warp;
	s.sampling = sampling;
	s.force_render = obs_data_get_bool(settings, "force_render_identity");
	s.bil = hdrtk::quad::bilinear_uniforms(use);
	if (!hdrtk::quad::projective_uniforms(use, s.proj)) {
		// validated above, so this is an internal error: fall back to identity
		hdrtk::quad::projective_uniforms(Quad(), s.proj);
		s.bil = hdrtk::quad::bilinear_uniforms(Quad());
		s.identity = true;
	}
	f->snapshot = s;
}

void *transform_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new TransformFilter();
	f->context = source;

	char *path = obs_module_file("effects/hdr-transform.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	if (f->effect) {
		f->p_a = gs_effect_get_param_by_name(f->effect, "quad_a");
		f->p_e = gs_effect_get_param_by_name(f->effect, "quad_e");
		f->p_f = gs_effect_get_param_by_name(f->effect, "quad_f");
		f->p_g = gs_effect_get_param_by_name(f->effect, "quad_g");
		f->p_h0 = gs_effect_get_param_by_name(f->effect, "hinv0");
		f->p_h1 = gs_effect_get_param_by_name(f->effect, "hinv1");
		f->p_h2 = gs_effect_get_param_by_name(f->effect, "hinv2");
		f->p_wsign = gs_effect_get_param_by_name(f->effect, "w_sign");
		f->p_pre_on = gs_effect_get_param_by_name(f->effect, "pre_on");
		f->p_pre_cs = gs_effect_get_param_by_name(f->effect, "pre_cs");
		f->p_pre_k = gs_effect_get_param_by_name(f->effect, "pre_k");
		f->p_pre_inv_hd2 = gs_effect_get_param_by_name(f->effect, "pre_inv_hd2");
		f->p_pre_scale = gs_effect_get_param_by_name(f->effect, "pre_scale");
		f->p_pre_size = gs_effect_get_param_by_name(f->effect, "pre_size");
	}
	obs_leave_graphics();

	if (!f->effect) {
		obs_log(LOG_ERROR, "[transform] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		bfree(errors);
		bfree(path);
		delete f;
		return nullptr;
	}
	bfree(errors);
	bfree(path);

	transform_update(f, settings);
	return f;
}

void transform_destroy(void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	delete f;
}

void transform_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "schema_version", kSchemaVersion);
	obs_data_set_default_int(settings, "warp_model", WarpBilinear);
	obs_data_set_default_int(settings, "sampling_mode", SampleBilinear);
	obs_data_set_default_bool(settings, "force_render_identity", false);
	obs_data_set_default_double(settings, "pre_rotation_deg", 0.0);
	obs_data_set_default_double(settings, "pre_distortion", 0.0);
	obs_data_set_default_bool(settings, "pre_autofill", true);
	for (int i = 0; i < 4; i++) {
		obs_data_set_default_double(settings, kCorners[i].canon_x, kIdentity[i][0]);
		obs_data_set_default_double(settings, kCorners[i].canon_y, kIdentity[i][1]);
		obs_data_set_default_double(settings, kCorners[i].ui_x, kIdentity[i][0] * 100);
		obs_data_set_default_double(settings, kCorners[i].ui_y, kIdentity[i][1] * 100);
	}
}

// UI percent field changed: write through to the canonical key. Refresh the
// property view only when the validity message changes (refreshing on every
// edit would steal focus while typing).
bool corner_modified(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	auto *f = static_cast<TransformFilter *>(data);
	const char *name = obs_property_name(property);
	for (int i = 0; i < 4; i++) {
		if (strcmp(name, kCorners[i].ui_x) == 0)
			obs_data_set_double(settings, kCorners[i].canon_x, obs_data_get_double(settings, name) / 100.0);
		if (strcmp(name, kCorners[i].ui_y) == 0)
			obs_data_set_double(settings, kCorners[i].canon_y, obs_data_get_double(settings, name) / 100.0);
	}
	const bool projective = obs_data_get_int(settings, "warp_model") == WarpProjective;
	const std::string text = status_text(hdrtk::quad::validate(read_quad(settings), projective));
	obs_property_t *status = obs_properties_get(props, "geometry_status");
	const char *old = status ? obs_property_description(status) : nullptr;
	if (status && (!old || text != old)) {
		obs_property_set_description(status, text.c_str());
		return true;
	}
	UNUSED_PARAMETER(f);
	return false;
}

bool warp_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool projective = obs_data_get_int(settings, "warp_model") == WarpProjective;
	const std::string text = status_text(hdrtk::quad::validate(read_quad(settings), projective));
	obs_property_t *status = obs_properties_get(props, "geometry_status");
	if (status)
		obs_property_set_description(status, text.c_str());
	return true;
}

bool reset_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_data_t *settings = obs_source_get_settings(f->context);
	write_quad(settings, Quad());
	obs_source_update(f->context, settings);
	obs_data_release(settings);
	obs_property_t *status = obs_properties_get(props, "geometry_status");
	if (status)
		obs_property_set_description(status, obs_module_text("Transform.Status.Valid"));
	return true;
}

bool reset_prewarp_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_double(settings, "pre_rotation_deg", 0.0);
	obs_data_set_double(settings, "pre_distortion", 0.0);
	obs_source_update(f->context, settings);
	obs_data_release(settings);
	return true;
}

obs_properties_t *transform_properties(void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_properties_t *props = obs_properties_create();

	std::string status;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		status = f->status;
	}
	obs_properties_add_text(props, "geometry_status", status.c_str(), OBS_TEXT_INFO);

	obs_property_t *p = obs_properties_add_list(props, "warp_model", obs_module_text("Transform.WarpModel"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transform.WarpModel.Bilinear"), WarpBilinear);
	obs_property_list_add_int(p, obs_module_text("Transform.WarpModel.Projective"), WarpProjective);
	obs_property_set_modified_callback(p, warp_modified);

	// pre-warp: applied to the source before the corners (processing order top to bottom)
	obs_properties_t *pre = obs_properties_create();
	p = obs_properties_add_float_slider(pre, "pre_rotation_deg", obs_module_text("Transform.Pre.Rotation"),
					    -hdrtk::quad::kRotationMax, hdrtk::quad::kRotationMax, 0.01);
	obs_property_float_set_suffix(p, "\xC2\xB0"); // degree sign, UTF-8
	p = obs_properties_add_float_slider(pre, "pre_distortion", obs_module_text("Transform.Pre.Distortion"),
					    -hdrtk::quad::kDistortionMax, hdrtk::quad::kDistortionMax, 0.1);
	obs_properties_add_bool(pre, "pre_autofill", obs_module_text("Transform.Pre.Autofill"));
	obs_properties_add_button2(pre, "reset_prewarp", obs_module_text("Transform.Pre.Reset"), reset_prewarp_clicked,
				   f);
	obs_properties_add_group(props, "prewarp", obs_module_text("Transform.Pre"), OBS_GROUP_NORMAL, pre);

	obs_properties_t *corners = obs_properties_create();
	for (int k = 0; k < 4; k++) {
		const CornerKey &c = kCorners[kUiOrder[k]];
		const std::string lx = std::string(obs_module_text(c.label)) + " X";
		const std::string ly = std::string(obs_module_text(c.label)) + " Y";
		p = obs_properties_add_float(corners, c.ui_x, lx.c_str(), -400.0, 500.0, 0.001);
		obs_property_float_set_suffix(p, " %");
		obs_property_set_modified_callback2(p, corner_modified, f);
		p = obs_properties_add_float(corners, c.ui_y, ly.c_str(), -400.0, 500.0, 0.001);
		obs_property_float_set_suffix(p, " %");
		obs_property_set_modified_callback2(p, corner_modified, f);
	}
	obs_properties_add_group(props, "corners", obs_module_text("Transform.Corners"), OBS_GROUP_NORMAL, corners);
	obs_properties_add_button2(props, "reset_corners", obs_module_text("Transform.ResetCorners"), reset_clicked, f);

	p = obs_properties_add_list(props, "sampling_mode", obs_module_text("Transform.Sampling"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transform.Sampling.Bilinear"), SampleBilinear);
	obs_property_list_add_int(p, obs_module_text("Transform.Sampling.Point"), SamplePoint);

	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_bool(adv, "force_render_identity", obs_module_text("Transform.ForceRender"));
	obs_properties_add_group(props, "advanced", obs_module_text("Transform.Advanced"), OBS_GROUP_NORMAL, adv);
	return props;
}

const char *technique_for(const Snapshot &s)
{
	if (s.warp == WarpProjective)
		return s.sampling == SamplePoint ? "ProjectivePoint" : "ProjectiveLinear";
	return s.sampling == SamplePoint ? "BilinearPoint" : "BilinearLinear";
}

void set_vec2(gs_eparam_t *p, const float v[2])
{
	struct vec2 x;
	vec2_set(&x, v[0], v[1]);
	gs_effect_set_vec2(p, &x);
}

void set_vec3(gs_eparam_t *p, const float v[3])
{
	struct vec3 x;
	vec3_set(&x, v[0], v[1], v[2]);
	gs_effect_set_vec3(p, &x);
}

void transform_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<TransformFilter *>(data);

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
		obs_log(LOG_INFO, "[transform] '%s': %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
	}
	if (!ctx.width || !ctx.height) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (s.identity && !s.force_render) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin_with_color_space(f->context, ctx.staging_format, ctx.input_space,
							      OBS_NO_DIRECT_RENDERING))
		return;

	set_vec2(f->p_a, s.bil.a);
	set_vec2(f->p_e, s.bil.e);
	set_vec2(f->p_f, s.bil.f);
	set_vec2(f->p_g, s.bil.g);
	set_vec3(f->p_h0, s.proj.row0);
	set_vec3(f->p_h1, s.proj.row1);
	set_vec3(f->p_h2, s.proj.row2);
	gs_effect_set_float(f->p_wsign, s.proj.w_sign);

	// pre-warp uniforms depend on the source size; recomputed only when it or the
	// settings change (the fill scale samples the frame boundary)
	if (!f->pre_valid || f->pre_w != ctx.width || f->pre_h != ctx.height ||
	    f->pre_cached.rotation_deg != s.pre.rotation_deg || f->pre_cached.distortion != s.pre.distortion ||
	    f->pre_cached.autofill != s.pre.autofill) {
		f->pre_uni = hdrtk::quad::prewarp_uniforms(s.pre, ctx.width, ctx.height);
		f->pre_cached = s.pre;
		f->pre_w = ctx.width;
		f->pre_h = ctx.height;
		f->pre_valid = true;
		if (f->pre_uni.on > 0.5f)
			obs_log(LOG_INFO,
				"[transform] '%s': pre-warp rotation %.2f deg, distortion %.1f (k %.4f), fill scale %.4f, %ux%u",
				obs_source_get_name(f->context), s.pre.rotation_deg, s.pre.distortion, f->pre_uni.k,
				f->pre_uni.scale, ctx.width, ctx.height);
	}
	gs_effect_set_float(f->p_pre_on, f->pre_uni.on);
	set_vec2(f->p_pre_cs, f->pre_uni.cs);
	gs_effect_set_float(f->p_pre_k, f->pre_uni.k);
	gs_effect_set_float(f->p_pre_inv_hd2, f->pre_uni.inv_hd2);
	gs_effect_set_float(f->p_pre_scale, f->pre_uni.scale);
	set_vec2(f->p_pre_size, f->pre_uni.size);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, technique_for(s));
	gs_blend_state_pop();
}

enum gs_color_space transform_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<TransformFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_transform_filter(void)
{
	struct obs_source_info info = {};
	info.id = "hdr_toolkit_transform_v1";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = transform_get_name;
	info.create = transform_create;
	info.destroy = transform_destroy;
	info.update = transform_update;
	info.get_defaults = transform_defaults;
	info.get_properties = transform_properties;
	info.video_render = transform_render;
	info.video_get_color_space = transform_color_space;
	obs_register_source(&info);
}
