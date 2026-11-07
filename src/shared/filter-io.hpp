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

#pragma once

// Filter input capture and result drawing shared by the multi-pass filters (HDR Denoise,
// HDR Sharpen): the filter renders everything before it into its own texrender, the
// way libobs does for filters, and draws its result the way libobs draws a filter
// result (premultiplied, blend ONE / INVSRCALPHA).

#include "obs-color-context.hpp"

#include <obs-module.h>

namespace hdrtk {

struct FilterInput {
	gs_texrender_t *texrender = nullptr;
	enum gs_color_format format = GS_UNKNOWN;
	void destroy(); // graphics context
};

// Renders the filter's input (obs_source_skip_video_filter) into `in` in the target's
// native space and staging format. Returns the texture, or nullptr on failure.
gs_texture_t *capture_filter_input(obs_source_t *filter, FilterInput &in, const RenderContext &ctx);

// Draws `tex` into the current target (default effect "Draw", premultiplied,
// sRGB-aware for OBS_SOURCE_SRGB filters).
void draw_filter_result(gs_texture_t *tex);

} // namespace hdrtk
