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

namespace hdrtk {
namespace denoise {

// NVIDIA Optical Flow capability probe (Windows): logs per-device engine availability,
// supported grid sizes and limits, and which API entry points the driver exports.
bool nvof_probe_available();
void nvof_probe_async(); // runs on a worker thread; results go to the OBS log

} // namespace denoise
} // namespace hdrtk
