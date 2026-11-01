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

// HDR denoise core: implemented from docs/HQDN3D_DESIGN.md (independent of FFmpeg code).

#include "denoise-core.hpp"

#include "shared/obs-color-context.hpp"

#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hdrtk {
namespace denoise {

// ---- settings model ----------------------------------------------------------------

void core_defaults(obs_data_t *s, Placement placement)
{
	obs_data_set_default_double(s, "mix", 1.0);
	// HQDN3D-style starting values; tuning happens in OBS (brief 24), not copied from FFmpeg
	obs_data_set_default_double(s, "temporal_luma", 4.0);
	obs_data_set_default_double(s, "temporal_chroma", 6.0);
	obs_data_set_default_double(s, "spatial_luma", 0.0);
	obs_data_set_default_double(s, "spatial_chroma", 0.0);
	obs_data_set_default_int(s, "spatial_radius", 8);
	obs_data_set_default_bool(s, "force_spatial", false);
	obs_data_set_default_int(s, "noise_profile", ProfileIdentity);
	obs_data_set_default_double(s, "noise_profile_max", 3.0);
	obs_data_set_default_double(s, "comparison_knee_nits", 0.1);
	// a single camera does not cut between unrelated scenes; reset stays on as a safety
	// net (input switching, looping media), transition protection is a program concern
	obs_data_set_default_bool(s, "scene_cut_reset", true);
	obs_data_set_default_double(s, "cut_sensitivity", 50.0);
	obs_data_set_default_bool(s, "transition_protection", placement == PlacementProgram);
	obs_data_set_default_double(s, "protection_amount", 0.8);
	obs_data_set_default_int(s, "debug_view", ViewNormal);
	obs_data_set_default_double(s, "debug_gain", 16.0);
	obs_data_set_default_bool(s, "log_counters", false);
}

CoreSettings core_parse(obs_data_t *data)
{
	CoreSettings s;
	s.p.temporal_luma = obs_data_get_double(data, "temporal_luma");
	s.p.temporal_chroma = obs_data_get_double(data, "temporal_chroma");
	s.p.k_nits = obs_data_get_double(data, "comparison_knee_nits");
	s.p.cut_reset = obs_data_get_bool(data, "scene_cut_reset");
	s.p.cut_sensitivity = obs_data_get_double(data, "cut_sensitivity");
	s.p.protection = obs_data_get_bool(data, "transition_protection");
	s.p.protection_amount = obs_data_get_double(data, "protection_amount");
	sanitize(s.p);
	s.sp.luma = obs_data_get_double(data, "spatial_luma");
	s.sp.chroma = obs_data_get_double(data, "spatial_chroma");
	s.sp.radius = (int)obs_data_get_int(data, "spatial_radius");
	s.sp.k_nits = s.p.k_nits;
	sanitize(s.sp);
	s.force_spatial = obs_data_get_bool(data, "force_spatial");
	s.noise_profile = (int)obs_data_get_int(data, "noise_profile");
	if (s.noise_profile < 0 || s.noise_profile >= ProfileCount)
		s.noise_profile = ProfileIdentity;
	s.noise_profile_max = std::clamp(obs_data_get_double(data, "noise_profile_max"), 1.0, 8.0);
	s.p.profile = builtin_profile(s.noise_profile, s.noise_profile_max);
	s.sp.profile = s.p.profile;
	s.mix = std::clamp(obs_data_get_double(data, "mix"), 0.0, 1.0);
	s.debug_view = (int)obs_data_get_int(data, "debug_view");
	if (s.debug_view < ViewNormal || s.debug_view > ViewRemovedNoise)
		s.debug_view = ViewNormal;
	s.debug_gain = std::clamp(obs_data_get_double(data, "debug_gain"), 1.0, kDebugGainMax);
	s.log_counters = obs_data_get_bool(data, "log_counters");
	return s;
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double lo, double hi,
			      double step)
{
	return obs_properties_add_float_slider(props, key, obs_module_text(text), lo, hi, step);
}

obs_properties_t *core_properties(obs_properties_t *props, Placement placement, obs_properties_t **dbg_out)
{
	slider(props, "mix", "Denoise.Mix", 0.0, 1.0, 0.01);

	obs_properties_t *hq = obs_properties_create();
	slider(hq, "temporal_luma", "Denoise.TemporalLuma", 0.0, kStrengthMax, 0.1);
	slider(hq, "temporal_chroma", "Denoise.TemporalChroma", 0.0, kStrengthMax, 0.1);
	slider(hq, "spatial_luma", "Denoise.SpatialLuma", 0.0, kStrengthMax, 0.1);
	slider(hq, "spatial_chroma", "Denoise.SpatialChroma", 0.0, kStrengthMax, 0.1);
	obs_properties_add_bool(hq, "scene_cut_reset", obs_module_text("Denoise.SceneCutReset"));
	slider(hq, "cut_sensitivity", "Denoise.CutSensitivity", 0.0, 100.0, 1.0);
	obs_properties_add_bool(hq, "transition_protection",
				obs_module_text(placement == PlacementProgram ? "Denoise.TransitionProtection"
									      : "Denoise.ChangeProtection"));
	slider(hq, "protection_amount", "Denoise.ProtectionAmount", 0.0, 1.0, 0.01);
	slider(hq, "comparison_knee_nits", "Denoise.Knee", kKneeMin, kKneeMax, 0.01);
	obs_properties_add_group(props, "hqdn3d", obs_module_text("Denoise.Hqdn3d"), OBS_GROUP_NORMAL, hq);

	obs_properties_t *dbg = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(dbg, "debug_view", obs_module_text("Denoise.DebugView"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Normal"), ViewNormal);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Difference"), ViewDifference);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.RemovedNoise"), ViewRemovedNoise);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Weight"), ViewWeight);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.History"), ViewHistory);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Metric"), ViewMetric);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Exact"), ViewExact);
	slider(dbg, "debug_gain", "Denoise.DebugGain", 1.0, kDebugGainMax, 1.0);
	obs_properties_add_bool(dbg, "log_counters", obs_module_text("Denoise.LogCounters"));
	obs_properties_add_group(props, "debug", obs_module_text("Denoise.Debug"), OBS_GROUP_NORMAL, dbg);
	if (dbg_out)
		*dbg_out = dbg;

	// development options (radius chosen by measurement; noise-profile curve not frozen)
	obs_properties_t *dev = obs_properties_create();
	p = obs_properties_add_list(dev, "spatial_radius", obs_module_text("Denoise.SpatialRadius"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int r : {6, 8, 12}) {
		char name[16];
		snprintf(name, sizeof(name), "%d", r);
		obs_property_list_add_int(p, name, r);
	}
	obs_properties_add_bool(dev, "force_spatial", obs_module_text("Denoise.ForceSpatial"));
	p = obs_properties_add_list(dev, "noise_profile", obs_module_text("Denoise.NoiseProfile"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.NoiseProfile.Identity"), ProfileIdentity);
	obs_property_list_add_int(p, obs_module_text("Denoise.NoiseProfile.Measured"), ProfileMeasured20261005);
	slider(dev, "noise_profile_max", "Denoise.NoiseProfileMax", 1.0, 8.0, 0.1);
	obs_properties_add_group(props, "dev", obs_module_text("Denoise.Dev"), OBS_GROUP_NORMAL, dev);
	return dev;
}

// ---- shared effect ------------------------------------------------------------------

namespace {
gs_effect_t *g_effect = nullptr;
bool g_effect_failed = false;
} // namespace

gs_effect_t *core_effect()
{
	if (g_effect || g_effect_failed)
		return g_effect;
	char *path = obs_module_file("effects/hdr-program-denoise.effect");
	char *errors = nullptr;
	g_effect = gs_effect_create_from_file(path, &errors);
	if (!g_effect) {
		obs_log(LOG_ERROR, "[denoise] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		g_effect_failed = true; // pass-through from now on, warn once
	}
	bfree(errors);
	bfree(path);
	return g_effect;
}

void core_effect_free()
{
	gs_effect_destroy(g_effect);
	g_effect = nullptr;
	g_effect_failed = false;
}

// ---- telemetry ------------------------------------------------------------------------

void telemetry_free(Telemetry &t)
{
	for (int i = 0; i < kTelemetryRing; i++) {
		gs_timer_range_destroy(t.range[i]);
		gs_timer_destroy(t.timer[i]);
		gs_stagesurface_destroy(t.stage[i]);
		t.range[i] = nullptr;
		t.timer[i] = nullptr;
		t.stage[i] = nullptr;
		t.pending[i] = false;
		t.staged[i] = false;
	}
}

int telemetry_begin(Telemetry &t, bool on)
{
	const int slot = t.idx;
	if (!on)
		return slot;
	if (!t.range[slot])
		t.range[slot] = gs_timer_range_create();
	if (!t.timer[slot])
		t.timer[slot] = gs_timer_create();
	if (t.range[slot])
		gs_timer_range_begin(t.range[slot]);
	if (t.timer[slot])
		gs_timer_begin(t.timer[slot]);
	return slot;
}

void telemetry_end(Telemetry &t, int slot, bool on, gs_texture_t *metric)
{
	if (!on)
		return;
	if (metric) {
		if (!t.stage[slot])
			t.stage[slot] = gs_stagesurface_create(1, 1, GS_RGBA32F);
		if (t.stage[slot]) {
			gs_stage_texture(t.stage[slot], metric);
			t.staged[slot] = true;
		}
	}
	if (t.timer[slot])
		gs_timer_end(t.timer[slot]);
	if (t.range[slot])
		gs_timer_range_end(t.range[slot]);
	t.pending[slot] = true;
	t.idx = (t.idx + 1) % kTelemetryRing;
}

bool telemetry_collect(Telemetry &t, double m_cut, bool cut_enabled)
{
	// the oldest slot (written kTelemetryRing - 1 frames ago)
	const int i = (t.idx + 1) % kTelemetryRing;
	if (!t.pending[i])
		return false;
	t.pending[i] = false;
	const bool staged = t.staged[i];
	t.staged[i] = false;
	uint64_t ticks = 0, freq = 0;
	bool disjoint = true;
	if (t.range[i] && t.timer[i] && gs_timer_range_get_data(t.range[i], &disjoint, &freq) && !disjoint && freq &&
	    gs_timer_get_data(t.timer[i], &ticks)) {
		t.ms.push_back((double)ticks * 1000.0 / (double)freq);
		if (t.ms.size() > 3000)
			t.ms.erase(t.ms.begin(), t.ms.begin() + 1000);
	}
	bool cut = false;
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (staged && t.stage[i] && gs_stagesurface_map(t.stage[i], &data, &linesize)) {
		float v[4];
		memcpy(v, data, sizeof(v));
		gs_stagesurface_unmap(t.stage[i]);
		if (v[2] > 0.5f) { // metric valid (not a reset frame)
			t.m_sum += v[0];
			t.m_max = std::max(t.m_max, (double)v[0]);
			t.m_n++;
			cut = cut_enabled && v[0] >= m_cut;
		}
	}
	return cut;
}

std::string timing_summary(Telemetry &t)
{
	if (t.ms.empty())
		return "gpu time n/a";
	std::vector<double> v = t.ms;
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double x : v)
		sum += x;
	const double p95 = v[std::min(v.size() - 1, (size_t)(0.95 * (double)v.size()))];
	char buf[160];
	snprintf(buf, sizeof(buf), "gpu ms avg %.2f p95 %.2f max %.2f (n=%zu)", sum / (double)v.size(), p95, v.back(),
		 v.size());
	t.ms.clear();
	return buf;
}

std::string metric_summary(Telemetry &t)
{
	char buf[96] = "metric n/a";
	if (t.m_n > 0)
		snprintf(buf, sizeof(buf), "metric avg %.4f max %.4f", t.m_sum / t.m_n, t.m_max);
	t.m_sum = t.m_max = 0;
	t.m_n = 0;
	return buf;
}

const char *format_name(enum gs_color_format f)
{
	switch (f) {
	case GS_RGBA16F:
		return "RGBA16F";
	case GS_RGBA32F:
		return "RGBA32F";
	case GS_RGBA:
		return "RGBA8";
	case GS_BGRA:
		return "BGRA8";
	case GS_R10G10B10A2:
		return "R10G10B10A2";
	case GS_RGBA16:
		return "RGBA16";
	default:
		return "other";
	}
}

// ---- core -----------------------------------------------------------------------------

namespace {

void set_tex(gs_effect_t *e, const char *name, gs_texture_t *t)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(e, name), t);
}

void set_f(gs_effect_t *e, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, name), v);
}

void set_v2(gs_effect_t *e, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, name), &v);
}

void set_v4(gs_effect_t *e, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &v);
}

// Pushes and restores everything the passes change: render target, colour space,
// viewport, projection, matrix, blend state, sRGB framebuffer.
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

gs_texture_t *make_like(uint32_t w, uint32_t h, enum gs_color_format f)
{
	return gs_texture_create(w, h, f, 1, nullptr, GS_RENDER_TARGET);
}

void destroy(gs_texture_t *&t)
{
	gs_texture_destroy(t);
	t = nullptr;
}

} // namespace

struct Core::Frame {
	const CoreSettings *s;
	ShaderParams sp;
	SpatialShaderParams ssp;
	float npu;
	bool reset;
	gs_effect_t *e;
};

void Core::free_spatial()
{
	destroy(sp_tmp_);
	destroy(sp_out_);
}

void Core::free_all()
{
	free_spatial();
	for (gs_texture_t **t : {&cur_, &hist_[0], &hist_[1], &l1_, &l2_, &metric_[0], &metric_[1], &scratch_, &out_})
		destroy(*t);
	telemetry_free(tel);
	history_valid_ = false;
	width_ = height_ = 0;
	format_ = GS_UNKNOWN;
}

bool sample_grid(gs_texture_t *input, gs_texture_t *grid, int step, enum gs_color_space space)
{
	gs_effect_t *e = core_effect();
	if (!e || !input || !grid || step < 1)
		return false;
	StateGuard guard;
	const uint32_t w = gs_texture_get_width(grid), h = gs_texture_get_height(grid);
	set_tex(e, "image", input);
	set_f(e, "grid_step", (float)step);
	set_v2(e, "out_size", (float)w, (float)h);
	gs_set_render_target_with_color_space(grid, nullptr, space);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(e, "SampleGrid"))
		gs_draw_sprite(nullptr, 0, w, h);
	return true;
}

double Core::vram_mib() const
{
	auto bpp = [](enum gs_color_format f) {
		return f == GS_RGBA16F ? 8.0 : f == GS_RGBA32F ? 16.0 : 4.0;
	};
	int frames = 0;
	for (gs_texture_t *t : {cur_, sp_tmp_, sp_out_, scratch_, out_})
		frames += t != nullptr;
	const int hist = (hist_[0] != nullptr) + (hist_[1] != nullptr);
	return ((double)frames * bpp(format_) + (double)hist * bpp(hist_format_)) * width_ * height_ /
	       (1024.0 * 1024.0);
}

bool Core::ensure(gs_texture_t *like, bool spatial, bool precise_history)
{
	const uint32_t w = gs_texture_get_width(like), h = gs_texture_get_height(like);
	const enum gs_color_format f = gs_texture_get_color_format(like);
	if (w != width_ || h != height_ || f != format_) {
		free_all(); // never reuse old-size history (brief 18)
		width_ = w;
		height_ = h;
		format_ = f;
		obs_log(LOG_INFO, "%s frame %ux%u %s", tag_.c_str(), w, h, format_name(f));
	}
	// strengths above 20 raise the history weight cap towards 0.99: keep that history in
	// RGBA32F (a half-float history would stall on small differences). A format change
	// restarts the history.
	const enum gs_color_format hf = precise_history ? GS_RGBA32F : f;
	if (hist_[0] && hf != hist_format_) {
		destroy(hist_[0]);
		destroy(hist_[1]);
	}
	if (!(hist_[0] && hist_[1] && l1_ && l2_ && metric_[0] && metric_[1])) {
		const uint32_t w1 = (w + 15) / 16, h1 = (h + 15) / 16;
		const uint32_t w2 = (w1 + 15) / 16, h2 = (h1 + 15) / 16;
		for (int i = 0; i < 2; i++) {
			if (!hist_[i])
				hist_[i] = make_like(w, h, hf);
			if (!metric_[i])
				metric_[i] = gs_texture_create(1, 1, GS_RGBA32F, 1, nullptr, GS_RENDER_TARGET);
		}
		if (!l1_)
			l1_ = gs_texture_create(w1, h1, GS_R32F, 1, nullptr, GS_RENDER_TARGET);
		if (!l2_)
			l2_ = gs_texture_create(w2, h2, GS_R32F, 1, nullptr, GS_RENDER_TARGET);
		const bool ok = hist_[0] && hist_[1] && l1_ && l2_ && metric_[0] && metric_[1];
		hist_format_ = hf;
		obs_log(LOG_INFO, "%s temporal resources %s: history 2 x %ux%u %s", tag_.c_str(),
			ok ? "created" : "FAILED", w, h, format_name(hf));
		history_valid_ = false;
		if (!ok)
			return false;
	}
	if (spatial && !(sp_tmp_ && sp_out_)) {
		if (!sp_tmp_)
			sp_tmp_ = make_like(w, h, f);
		if (!sp_out_)
			sp_out_ = make_like(w, h, f);
		const bool ok = sp_tmp_ && sp_out_;
		obs_log(LOG_INFO, "%s spatial resources %s: 2 x %ux%u %s", tag_.c_str(), ok ? "created" : "FAILED", w,
			h, format_name(f));
		if (!ok)
			return false;
	} else if (!spatial && sp_tmp_) {
		free_spatial(); // spatial switched off: release its two frames
	}
	return true;
}

gs_texture_t *Core::copy_input(gs_texture_t *frame)
{
	const uint32_t w = gs_texture_get_width(frame), h = gs_texture_get_height(frame);
	const enum gs_color_format f = gs_texture_get_color_format(frame);
	if (cur_ && (gs_texture_get_width(cur_) != w || gs_texture_get_height(cur_) != h ||
		     gs_texture_get_color_format(cur_) != f))
		destroy(cur_);
	if (!cur_)
		cur_ = make_like(w, h, f);
	if (cur_)
		gs_copy_texture(cur_, frame);
	return cur_;
}

gs_texture_t *Core::scratch(gs_texture_t *like)
{
	const uint32_t w = gs_texture_get_width(like), h = gs_texture_get_height(like);
	const enum gs_color_format f = gs_texture_get_color_format(like);
	if (scratch_ && (gs_texture_get_width(scratch_) != w || gs_texture_get_height(scratch_) != h ||
			 gs_texture_get_color_format(scratch_) != f))
		destroy(scratch_);
	if (!scratch_)
		scratch_ = make_like(w, h, f);
	return scratch_;
}

gs_texture_t *Core::out_texture(gs_texture_t *like)
{
	const uint32_t w = gs_texture_get_width(like), h = gs_texture_get_height(like);
	const enum gs_color_format f = gs_texture_get_color_format(like);
	if (out_ && (gs_texture_get_width(out_) != w || gs_texture_get_height(out_) != h ||
		     gs_texture_get_color_format(out_) != f))
		destroy(out_);
	if (!out_)
		out_ = make_like(w, h, f);
	return out_;
}

void Core::set_common(const Frame &f, uint32_t w, uint32_t h)
{
	gs_effect_t *e = f.e;
	set_v2(e, "tex_size", (float)w, (float)h);
	set_f(e, "nits_per_unit", f.npu);
	set_f(e, "units_per_nit", 1.0f / f.npu);
	set_f(e, "knee", f.sp.k);
	set_f(e, "t_luma", f.sp.t_luma);
	set_f(e, "t_chroma", f.sp.t_chroma);
	set_f(e, "beta_luma", f.sp.beta_luma);
	set_f(e, "beta_chroma", f.sp.beta_chroma);
	set_f(e, "m_cut", f.sp.m_cut);
	set_f(e, "protect_amount", f.sp.protect_amount);
	set_f(e, "cut_enabled", f.sp.cut_enabled);
	set_f(e, "reset", f.reset ? 1.0f : 0.0f);
	set_f(e, "mix_amount", (float)f.s->mix);
	set_f(e, "debug_mode", (float)f.s->debug_view);
	set_f(e, "debug_gain", (float)f.s->debug_gain);
	set_f(e, "s_t_luma", f.ssp.t_luma);
	set_f(e, "s_t_chroma", f.ssp.t_chroma);
	set_f(e, "s_radius", (float)f.ssp.radius);
	const ShaderProfile &np = f.sp.profile;
	set_v4(e, "np0", np.a[0][0], np.a[0][1], np.a[1][0], np.a[1][1]);
	set_v4(e, "np1", np.a[2][0], np.a[2][1], np.a[3][0], np.a[3][1]);
	set_v4(e, "np2", np.a[4][0], np.a[4][1], np.a[5][0], np.a[5][1]);
	set_f(e, "np_count", np.count);
	set_f(e, "np_min", np.min_mult);
	set_f(e, "np_max", np.max_mult);
}

// Draw a full-target quad with `tech` into `target`.
void Core::run_pass(gs_texture_t *target, enum gs_color_space space, const char *tech)
{
	gs_effect_t *e = core_effect();
	const uint32_t w = gs_texture_get_width(target), h = gs_texture_get_height(target);
	gs_set_render_target_with_color_space(target, nullptr, space);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	set_v2(e, "out_size", (float)w, (float)h);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(nullptr, 0, w, h);
}

void Core::collect_telemetry(const CoreSettings &s)
{
	if (!s.log_counters)
		return;
	const ShaderParams sp = make_shader_params(s.p);
	if (telemetry_collect(tel, sp.m_cut, sp.cut_enabled > 0.5f))
		cut_resets++;
}

bool Core::process(gs_texture_t *input, gs_texture_t *output, enum gs_color_space space, const CoreSettings &s)
{
	gs_effect_t *e = core_effect();
	if (!e || !input || !ensure(input, s.spatial_on(), s.temporal_passes() && needs_precise_history(s.p)))
		return false;
	if (!output) {
		output = out_texture(input); // after ensure(): a size change frees every texture
		if (!output)
			return false;
	}
	collect_telemetry(s);
	const int slot = telemetry_begin(tel, s.log_counters);
	gs_texture_t *in = input;
	if (input == output) {
		in = copy_input(input); // program placement: Output writes into the frame itself
		if (!in) {
			telemetry_end(tel, slot, s.log_counters, nullptr);
			return false;
		}
	}

	Frame f;
	f.s = &s;
	f.sp = make_shader_params(s.p);
	f.ssp = make_spatial_shader_params(s.sp);
	f.npu = (float)hdrtk::nits_per_unit(space);
	f.reset = !history_valid_;
	f.e = e;

	gs_texture_t *m_next = metric_[mi_ ^ 1];
	const bool skipped = !s.temporal_passes();
	{
		StateGuard guard;
		gs_texture_t *hist = hist_[hi_];
		gs_texture_t *hist_next = hist_[hi_ ^ 1];
		gs_texture_t *m_prev = metric_[mi_];

		// spatial before temporal (design section 8.1); `in` keeps the unfiltered input
		gs_texture_t *src = in;
		if (s.spatial_on()) {
			set_common(f, width_, height_);
			set_tex(e, "image", in);
			run_pass(sp_tmp_, space, "SpatialH");
			set_common(f, width_, height_);
			set_tex(e, "image", sp_tmp_);
			run_pass(sp_out_, space, "SpatialV");
			src = sp_out_;
			spatial_passes++;
		}
		if (skipped) {
			// spatial only: no metric, no temporal pass; Output takes the spatial result
			// directly. The history is not maintained, so it restarts when temporal is set.
			set_common(f, width_, height_);
			set_tex(e, "image", in);
			set_tex(e, "filt_tex", src);
			set_tex(e, "hist_tex", hist);
			set_tex(e, "src_tex", src);
			set_tex(e, "metric_tex", m_prev);
			run_pass(output, space, "Output");
		} else {
			if (!f.reset) {
				set_common(f, width_, height_);
				set_tex(e, "image", src);
				set_tex(e, "hist_tex", hist);
				run_pass(l1_, space, "MetricBlocks");

				set_common(f, width_, height_);
				set_tex(e, "image", l1_);
				set_v2(e, "in_size", (float)gs_texture_get_width(l1_),
				       (float)gs_texture_get_height(l1_));
				run_pass(l2_, space, "MetricReduce");
			}
			set_common(f, width_, height_);
			set_tex(e, "image", l2_);
			set_tex(e, "prev_metric_tex", m_prev);
			set_v2(e, "in_size", (float)gs_texture_get_width(l2_), (float)gs_texture_get_height(l2_));
			run_pass(m_next, space, "MetricFinal");

			set_common(f, width_, height_);
			set_tex(e, "image", src);
			set_tex(e, "hist_tex", hist);
			set_tex(e, "metric_tex", m_next);
			run_pass(hist_next, space, "Temporal");

			set_common(f, width_, height_);
			set_tex(e, "image", in);
			set_tex(e, "filt_tex", hist_next);
			set_tex(e, "hist_tex", hist);
			set_tex(e, "src_tex", src);
			set_tex(e, "metric_tex", m_next);
			run_pass(output, space, "Output");
		}
	}
	history_updates++;
	if (skipped) {
		telemetry_end(tel, slot, s.log_counters, nullptr);
		temporal_skipped++;
		history_valid_ = false;
		return true;
	}
	telemetry_end(tel, slot, s.log_counters, m_next);

	hi_ ^= 1;
	mi_ ^= 1;
	if (f.reset)
		history_resets++;
	history_valid_ = true;
	return true;
}

bool Core::draw_identity(gs_texture_t *frame, enum gs_color_space space, const CoreSettings &s)
{
	gs_effect_t *e = core_effect();
	if (!e)
		return false;
	gs_texture_t *in = copy_input(frame);
	if (!in)
		return false;
	Frame f;
	f.s = &s;
	f.sp = make_shader_params(s.p);
	f.ssp = make_spatial_shader_params(s.sp);
	f.npu = (float)hdrtk::nits_per_unit(space);
	f.reset = true;
	f.e = e;
	StateGuard guard;
	set_common(f, gs_texture_get_width(in), gs_texture_get_height(in));
	set_tex(e, "image", in);
	run_pass(frame, space, "Identity");
	return true;
}

void Core::draw_output(gs_texture_t *image, gs_texture_t *filt, gs_texture_t *target, enum gs_color_space space,
		       const CoreSettings &s)
{
	gs_effect_t *e = core_effect();
	if (!e)
		return;
	Frame f;
	f.s = &s;
	f.sp = make_shader_params(s.p);
	f.ssp = make_spatial_shader_params(s.sp);
	f.npu = (float)hdrtk::nits_per_unit(space);
	f.reset = true;
	f.e = e;
	StateGuard guard;
	set_common(f, gs_texture_get_width(image), gs_texture_get_height(image));
	set_tex(e, "image", image);
	set_tex(e, "filt_tex", filt);
	set_tex(e, "hist_tex", image);
	set_tex(e, "src_tex", image);
	set_tex(e, "metric_tex", image);
	run_pass(target, space, "Output");
}

} // namespace denoise
} // namespace hdrtk
