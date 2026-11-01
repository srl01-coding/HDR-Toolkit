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

/*
 * Native D3D11 compute, P2 identity spike (docs/denoise/D3D11_COMPUTE.md).
 * Windows only; on other platforms available() is false and every call fails
 * cleanly so the caller passes the frame through. Graphics thread only.
 */

#include <obs-module.h>

namespace hdrtk {
namespace denoise {
namespace compute {

enum Variant {
	VariantTwoCopies = 0, // copy main -> private SRV texture, CS, copy private UAV texture -> main
	VariantOneCopy = 1,   // CS reads the main texture directly (OM unbound around it), copy back
	VariantDeferred = 2,  // as OneCopy, recorded on a deferred context, executed with state restore
};

struct Spike;

bool available(); // built with D3D11 support (Windows)
Spike *create();
void destroy(Spike *s); // releases every D3D11 object (graphics thread)

// Identity round trip on the main texture, which must be the bound render target.
// Returns false with a reason before touching the main texture when anything is
// missing; the caller then leaves the frame as it is (transparent fallback).
bool identity(Spike *s, gs_texture_t *main_tex, int variant, const char **why);

// One-line description of the device and resources, for the log.
const char *describe(Spike *s);

} // namespace compute
} // namespace denoise
} // namespace hdrtk
