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

#include <obs-module.h>

#include <cstdint>
#include <string>

namespace hdrtk {

/*
 * Color-space contract (docs/COLOR_PIPELINE.md, brief section 3):
 *
 *   input_space     what the upstream target supplies (native space query)
 *   staging_space   space the target is captured into for this pass
 *   output_space    what this filter promises in video_get_color_space
 *   consumer_space  gs_get_color_space() at render entry
 *
 * P0 policy: staging_space = output_space = input_space ("preserve native").
 * libobs source_render() converts output_space -> consumer_space at the
 * boundary (verified in obs-source.c at OBS 32.2.2).
 */

// Spaces the toolkit filters accept as input, in libobs preference order.
extern const enum gs_color_space kAllSpaces[4];

// Native space of a filter target: the target is asked with every space we
// support so it can report what it actually produces.
enum gs_color_space query_native_space(obs_source_t *target);

const char *space_name(enum gs_color_space space);

// Nominal nits represented by working RGB value 1.0 in `space`.
//   709_EXTENDED, SRGB, SRGB_16F : SDR white level W (relative reference for SDR)
//   709_SCRGB                    : 80
double nits_per_unit(enum gs_color_space space);

// Snapshot of everything that determines how a pass is rendered. Compared
// between frames so changes are logged once, not every frame.
struct RenderContext {
	enum gs_color_space input_space = GS_CS_SRGB;
	enum gs_color_space consumer_space = GS_CS_SRGB;
	enum gs_color_format staging_format = GS_UNKNOWN;
	uint32_t width = 0;
	uint32_t height = 0;
	float sdr_white_nits = 0.0f;
	float hdr_peak_nits = 0.0f;

	bool operator==(const RenderContext &o) const;
	bool operator!=(const RenderContext &o) const { return !(*this == o); }
	std::string describe() const;
};

RenderContext capture_context(obs_source_t *filter_context, obs_source_t *target);

} // namespace hdrtk
