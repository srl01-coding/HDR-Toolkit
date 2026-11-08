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
 * Schema 3 (user-directed 8 Nov 2026): output size after the corner pin (HDR Scale's
 * Fill / Fit / Stretch / desqueeze / pan layout), so rotation + distortion + corner pin +
 * resize are one resample; and High quality sampling (warp-math.hpp; default): 2-D
 * Lanczos-3 at the mapped point, widened where the warp reduces, PQ domain with
 * anti-ringing where it enlarges. Bilinear and Point remain selectable.
 *
 * Render contract (docs/COLOR_PIPELINE.md): native colour space, float
 * staging for every non-8-bit space, input captured like libobs (no direct
 * rendering), premultiplied output, outside the quad = transparent black, no
 * clamp of RGB. Rendered once per video frame.
 */

#include "quad-math.hpp"
#include "warp-math.hpp"
#include "../scale/scale-math.hpp"
#include "../shared/filter-io.hpp"
#include "../shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
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

// 2: pre-warp keys (neutral defaults; schema 1 unchanged)
// 3: output size keys (neutral defaults) and sampling_mode High quality (new default)
constexpr int kSchemaVersion = 3;

enum WarpModel : int { WarpBilinear = 0, WarpProjective = 1 };
enum Sampling : int { SampleBilinear = 0, SamplePoint = 1, SampleHQ = 2 };

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
	PreWarp pre;                 // uniforms depend on the frame size: computed at render
	hdrtk::scale::Params resize; // output size (layout depends on the frame size)
	float antiring = 0.8f;
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
	std::atomic<uint64_t> generation{0};
	Quad last_valid; // identity until a valid quad is set
	std::string status;

	// graphics thread only
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
	PreWarp pre_cached;
	uint32_t pre_w = 0, pre_h = 0;
	bool pre_valid = false;
	PreWarpUniforms pre_uni{};
	hdrtk::FilterInput input;
	gs_texture_t *pq = nullptr, *out = nullptr;
	uint32_t pq_w = 0, pq_h = 0, out_w = 0, out_h = 0;
	uint64_t last_time = 0, last_gen = ~0ull;
	bool out_valid = false;
	std::string last_layout;
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
	if (sampling != SampleBilinear && sampling != SamplePoint && sampling != SampleHQ)
		sampling = SampleHQ;

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
	s.resize.out_w = (int)obs_data_get_int(settings, "out_w");
	s.resize.out_h = (int)obs_data_get_int(settings, "out_h");
	s.resize.fit = (hdrtk::scale::Fit)obs_data_get_int(settings, "out_fit");
	s.resize.desqueeze = obs_data_get_double(settings, "out_desqueeze");
	s.resize.pan_x = obs_data_get_double(settings, "out_pan_x") / 100.0;
	s.resize.pan_y = obs_data_get_double(settings, "out_pan_y") / 100.0;
	hdrtk::scale::sanitize(s.resize);
	s.antiring = (float)std::min(1.0, std::max(0.0, obs_data_get_double(settings, "antiring") / 100.0));
	s.bil = hdrtk::quad::bilinear_uniforms(use);
	if (!hdrtk::quad::projective_uniforms(use, s.proj)) {
		// validated above, so this is an internal error: fall back to identity
		hdrtk::quad::projective_uniforms(Quad(), s.proj);
		s.bil = hdrtk::quad::bilinear_uniforms(Quad());
		s.identity = true;
	}
	f->snapshot = s;
	f->generation++;
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
	f->input.destroy();
	gs_texture_destroy(f->pq);
	gs_texture_destroy(f->out);
	obs_leave_graphics();
	delete f;
}

void transform_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "schema_version", kSchemaVersion);
	obs_data_set_default_int(settings, "warp_model", WarpBilinear);
	obs_data_set_default_int(settings, "sampling_mode", SampleHQ);
	obs_data_set_default_double(settings, "antiring", 80.0);
	obs_data_set_default_int(settings, "out_w", 0);
	obs_data_set_default_int(settings, "out_h", 0);
	obs_data_set_default_int(settings, "out_fit", hdrtk::scale::FitFill);
	obs_data_set_default_double(settings, "out_desqueeze", 1.0);
	obs_data_set_default_double(settings, "out_pan_x", 0.0);
	obs_data_set_default_double(settings, "out_pan_y", 0.0);
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

bool canvas_size_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_int(settings, "out_w", ovi.base_width);
	obs_data_set_int(settings, "out_h", ovi.base_height);
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

	// output size: after the corners (processing order top to bottom)
	obs_properties_t *res = obs_properties_create();
	p = obs_properties_add_int(res, "out_w", obs_module_text("Transform.Out.Width"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int(res, "out_h", obs_module_text("Transform.Out.Height"), 0, 16384, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_button2(res, "out_canvas", obs_module_text("Transform.Out.Canvas"), canvas_size_clicked, f);
	p = obs_properties_add_list(res, "out_fit", obs_module_text("Transform.Out.Fit"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transform.Out.Fit.Fill"), hdrtk::scale::FitFill);
	obs_property_list_add_int(p, obs_module_text("Transform.Out.Fit.Fit"), hdrtk::scale::FitFit);
	obs_property_list_add_int(p, obs_module_text("Transform.Out.Fit.Stretch"), hdrtk::scale::FitStretch);
	p = obs_properties_add_float(res, "out_desqueeze", obs_module_text("Transform.Out.Desqueeze"), 0.25, 4.0,
				     0.001);
	obs_property_set_long_description(p, obs_module_text("Transform.Out.Desqueeze.Help"));
	p = obs_properties_add_float_slider(res, "out_pan_x", obs_module_text("Transform.Out.PanX"), -100.0, 100.0,
					    0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(res, "out_pan_y", obs_module_text("Transform.Out.PanY"), -100.0, 100.0,
					    0.1);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_group(props, "output", obs_module_text("Transform.Out"), OBS_GROUP_NORMAL, res);

	p = obs_properties_add_list(props, "sampling_mode", obs_module_text("Transform.Sampling"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transform.Sampling.HQ"), SampleHQ);
	obs_property_list_add_int(p, obs_module_text("Transform.Sampling.Bilinear"), SampleBilinear);
	obs_property_list_add_int(p, obs_module_text("Transform.Sampling.Point"), SamplePoint);
	p = obs_properties_add_float_slider(props, "antiring", obs_module_text("Transform.AntiRing"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");

	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_bool(adv, "force_render_identity", obs_module_text("Transform.ForceRender"));
	obs_properties_add_group(props, "advanced", obs_module_text("Transform.Advanced"), OBS_GROUP_NORMAL, adv);
	return props;
}

hdrtk::scale::Layout layout_for(const Snapshot &s, uint32_t w, uint32_t h)
{
	return hdrtk::scale::compute_layout(w, h, s.resize);
}

bool layout_identity(const hdrtk::scale::Layout &l)
{
	return hdrtk::scale::axis_identity(l.x) && hdrtk::scale::axis_identity(l.y) && std::fabs(l.x.offset) < 1e-9 &&
	       std::fabs(l.y.offset) < 1e-9;
}

Snapshot snapshot_of(TransformFilter *f)
{
	std::lock_guard<std::mutex> lock(f->mutex);
	return f->snapshot;
}

uint32_t transform_width(void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return 0;
	return layout_for(snapshot_of(f), obs_source_get_base_width(target), obs_source_get_base_height(target)).out_w;
}

uint32_t transform_height(void *data)
{
	auto *f = static_cast<TransformFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return 0;
	return layout_for(snapshot_of(f), obs_source_get_base_width(target), obs_source_get_base_height(target)).out_h;
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

void transform_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<TransformFilter *>(data);

	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const Snapshot s = snapshot_of(f);
	const uint64_t gen = f->generation.load();

	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[transform] '%s': %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
		f->out_valid = false;
	}
	if (!ctx.width || !ctx.height) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	const hdrtk::scale::Layout l = layout_for(s, ctx.width, ctx.height);
	{
		char line[192];
		snprintf(line, sizeof(line), "output %ux%u from %ux%u (scale x %.4f y %.4f), sampling %s", l.out_w,
			 l.out_h, ctx.width, ctx.height, l.x.scale, l.y.scale,
			 s.sampling == SampleHQ ? "high quality" : (s.sampling == SamplePoint ? "point" : "bilinear"));
		if (f->last_layout != line) {
			f->last_layout = line;
			obs_log(LOG_INFO, "[transform] '%s': %s", obs_source_get_name(f->context), line);
		}
	}
	if (!l.out_w || !l.out_h || (s.identity && layout_identity(l) && !s.force_render)) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (f->out_valid && t == f->last_time && gen == f->last_gen && f->out_w == l.out_w && f->out_h == l.out_h) {
		hdrtk::draw_filter_result(f->out);
		return;
	}

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

	gs_texture_t *in = hdrtk::capture_filter_input(f->context, f->input, ctx);
	const bool hq = s.sampling == SampleHQ;
	if (!in || !ensure_tex(f->out, f->out_w, f->out_h, l.out_w, l.out_h, GS_RGBA16F) ||
	    (hq && !ensure_tex(f->pq, f->pq_w, f->pq_h, ctx.width, ctx.height, GS_RGBA32F))) {
		f->out_valid = false;
		obs_source_skip_video_filter(f->context);
		return;
	}
	const bool srgb_view = ctx.staging_format == GS_RGBA || ctx.staging_format == GS_BGRA ||
			       ctx.staging_format == GS_BGRX;
	// float output: linear values; an 8-bit SDR input is written as SRGB_16F
	const enum gs_color_space out_space = ctx.input_space == GS_CS_SRGB ? GS_CS_SRGB_16F : ctx.input_space;
	const float pq_k = (float)(hdrtk::nits_per_unit(ctx.input_space) / 10000.0);
	gs_effect_t *e = f->effect;

	// libobs resets effect parameters after each technique: set everything before each pass
	auto set_all = [&](uint32_t pw, uint32_t ph) {
		set_vec2(f->p_a, s.bil.a);
		set_vec2(f->p_e, s.bil.e);
		set_vec2(f->p_f, s.bil.f);
		set_vec2(f->p_g, s.bil.g);
		set_vec3(f->p_h0, s.proj.row0);
		set_vec3(f->p_h1, s.proj.row1);
		set_vec3(f->p_h2, s.proj.row2);
		gs_effect_set_float(f->p_wsign, s.proj.w_sign);
		gs_effect_set_float(f->p_pre_on, f->pre_uni.on);
		set_vec2(f->p_pre_cs, f->pre_uni.cs);
		gs_effect_set_float(f->p_pre_k, f->pre_uni.k);
		gs_effect_set_float(f->p_pre_inv_hd2, f->pre_uni.inv_hd2);
		gs_effect_set_float(f->p_pre_scale, f->pre_uni.scale);
		set_vec2(f->p_pre_size, f->pre_uni.size);
		const float os[2] = {(float)pw, (float)ph};
		const float is[2] = {(float)ctx.width, (float)ctx.height};
		const float rs[2] = {(float)l.x.scale, (float)l.y.scale};
		const float ro[2] = {(float)l.x.offset, (float)l.y.offset};
		set_vec2(gs_effect_get_param_by_name(e, "out_size"), os);
		set_vec2(gs_effect_get_param_by_name(e, "in_size"), is);
		set_vec2(gs_effect_get_param_by_name(e, "res_scale"), rs);
		set_vec2(gs_effect_get_param_by_name(e, "res_offset"), ro);
		gs_effect_set_int(gs_effect_get_param_by_name(e, "warp"), s.warp == WarpProjective ? 1 : 0);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "antiring"), s.antiring);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "pq_k"), pq_k);
		gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
		if (srgb_view)
			gs_effect_set_texture_srgb(img, in);
		else
			gs_effect_set_texture(img, in);
	};
	auto run = [&](gs_texture_t *rt, uint32_t pw, uint32_t ph, const char *tech) {
		gs_set_render_target_with_color_space(rt, nullptr, out_space);
		gs_set_viewport(0, 0, (int)pw, (int)ph);
		gs_ortho(0.0f, (float)pw, 0.0f, (float)ph, -100.0f, 100.0f);
		while (gs_effect_loop(e, tech))
			gs_draw_sprite(nullptr, 0, pw, ph);
	};
	{
		StateGuard guard;
		if (hq) {
			set_all(ctx.width, ctx.height);
			run(f->pq, ctx.width, ctx.height, "EncodePQ");
			set_all(l.out_w, l.out_h);
			gs_effect_set_texture(gs_effect_get_param_by_name(e, "pq_image"), f->pq);
			run(f->out, l.out_w, l.out_h, "SampleHQ");
		} else {
			set_all(l.out_w, l.out_h);
			run(f->out, l.out_w, l.out_h, s.sampling == SamplePoint ? "SamplePoint" : "SampleLinear");
		}
	}
	f->out_valid = true;
	f->last_time = t;
	f->last_gen = gen;
	hdrtk::draw_filter_result(f->out);
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
	info.get_width = transform_width;
	info.get_height = transform_height;
	info.video_render = transform_render;
	info.video_get_color_space = transform_color_space;
	obs_register_source(&info);
}
