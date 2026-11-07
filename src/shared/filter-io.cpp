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

#include "filter-io.hpp"

#include <graphics/vec4.h>

namespace hdrtk {

void FilterInput::destroy()
{
	gs_texrender_destroy(texrender);
	texrender = nullptr;
	format = GS_UNKNOWN;
}

gs_texture_t *capture_filter_input(obs_source_t *filter, FilterInput &in, const RenderContext &ctx)
{
	if (in.texrender && in.format != ctx.staging_format)
		in.destroy();
	if (!in.texrender) {
		in.texrender = gs_texrender_create(ctx.staging_format, GS_ZS_NONE);
		in.format = ctx.staging_format;
	}
	if (!in.texrender)
		return nullptr;
	gs_texrender_reset(in.texrender);
	if (!gs_texrender_begin_with_color_space(in.texrender, ctx.width, ctx.height, ctx.input_space))
		return nullptr;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)ctx.width, 0.0f, (float)ctx.height, -100.0f, 100.0f);
	obs_source_skip_video_filter(filter);
	gs_blend_state_pop();
	gs_texrender_end(in.texrender);
	return gs_texrender_get_texture(in.texrender);
}

void draw_filter_result(gs_texture_t *tex)
{
	gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(e, "image");
	const bool prev_linear = gs_set_linear_srgb(true); // OBS_SOURCE_SRGB filter
	const bool linear_srgb = gs_get_linear_srgb();
	const bool prev_fb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);
	if (linear_srgb)
		gs_effect_set_texture_srgb(image, tex);
	else
		gs_effect_set_texture(image, tex);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(tex, 0, gs_texture_get_width(tex), gs_texture_get_height(tex));
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_fb);
	gs_set_linear_srgb(prev_linear);
}

} // namespace hdrtk
