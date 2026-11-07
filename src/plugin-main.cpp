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

#include <obs-module.h>
#include <plugin-support.h>

extern "C" void hdrtk_register_neutral_filter(void);
extern "C" void hdrtk_register_pattern_source(void);
extern "C" void hdrtk_register_transform_filter(void);
extern "C" void hdrtk_register_color_filter(void);
extern "C" void hdrtk_register_sharpen_filter(void);
extern "C" void hdrtk_denoise_load(void);
extern "C" void hdrtk_denoise_post_load(void);
extern "C" void hdrtk_denoise_unload(void);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern "C" bool obs_module_load(void)
{
	hdrtk_register_neutral_filter();
	hdrtk_register_pattern_source();
	hdrtk_register_transform_filter();
	hdrtk_register_color_filter();
	hdrtk_register_sharpen_filter();
	hdrtk_denoise_load();
	obs_log(LOG_INFO, "plugin loaded (version %s, built against libobs %d.%d.%d, running on %s)", PLUGIN_VERSION,
		LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER, obs_get_version_string());
	return true;
}

extern "C" void obs_module_post_load(void)
{
	hdrtk_denoise_post_load();
}

extern "C" void obs_module_unload(void)
{
	hdrtk_denoise_unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
