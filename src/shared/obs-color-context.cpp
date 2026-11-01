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

#include "obs-color-context.hpp"

#include <cstdio>

namespace hdrtk {

const enum gs_color_space kAllSpaces[4] = {
	GS_CS_SRGB,
	GS_CS_SRGB_16F,
	GS_CS_709_EXTENDED,
	GS_CS_709_SCRGB,
};

enum gs_color_space query_native_space(obs_source_t *target)
{
	if (!target)
		return GS_CS_SRGB;
	return obs_source_get_color_space(target, OBS_COUNTOF(kAllSpaces), kAllSpaces);
}

const char *space_name(enum gs_color_space space)
{
	switch (space) {
	case GS_CS_SRGB:
		return "SRGB";
	case GS_CS_SRGB_16F:
		return "SRGB_16F";
	case GS_CS_709_EXTENDED:
		return "709_EXTENDED";
	case GS_CS_709_SCRGB:
		return "709_SCRGB";
	}
	return "unknown";
}

double nits_per_unit(enum gs_color_space space)
{
	switch (space) {
	case GS_CS_709_SCRGB:
		return 80.0;
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
	case GS_CS_709_EXTENDED:
		break;
	}
	return (double)obs_get_video_sdr_white_level();
}

static const char *format_name(enum gs_color_format f)
{
	switch (f) {
	case GS_RGBA:
		return "RGBA8";
	case GS_RGBA16F:
		return "RGBA16F";
	case GS_RGBA32F:
		return "RGBA32F";
	case GS_UNKNOWN:
		return "none";
	default:
		return "other";
	}
}

bool RenderContext::operator==(const RenderContext &o) const
{
	return input_space == o.input_space && consumer_space == o.consumer_space &&
	       staging_format == o.staging_format && width == o.width && height == o.height &&
	       sdr_white_nits == o.sdr_white_nits && hdr_peak_nits == o.hdr_peak_nits;
}

std::string RenderContext::describe() const
{
	char buf[256];
	snprintf(buf, sizeof(buf), "input=%s consumer=%s staging=%s size=%ux%u sdr_white=%.1f nits hdr_peak=%.1f nits",
		 space_name(input_space), space_name(consumer_space), format_name(staging_format), width, height,
		 sdr_white_nits, hdr_peak_nits);
	return buf;
}

RenderContext capture_context(obs_source_t *filter_context, obs_source_t *target)
{
	RenderContext c;
	c.input_space = query_native_space(target);
	c.consumer_space = gs_get_color_space();
	c.staging_format = gs_get_format_from_space(c.input_space);
	c.width = target ? obs_source_get_base_width(target) : 0;
	c.height = target ? obs_source_get_base_height(target) : 0;
	c.sdr_white_nits = obs_get_video_sdr_white_level();
	c.hdr_peak_nits = obs_get_video_hdr_nominal_peak_level();
	UNUSED_PARAMETER(filter_context);
	return c;
}

} // namespace hdrtk
