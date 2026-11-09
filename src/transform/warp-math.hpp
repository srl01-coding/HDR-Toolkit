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
 * HDR Transform schema 3 (8 Nov 2026, user): output size and High quality sampling.
 * Float mirror of data/effects/hdr-transform.effect map_px() / PSSampleHQ() /
 * PSEncodePQ() - keep in lock-step. No libobs dependency (tests/cpu/test_warp.cpp).
 *
 * The output is a resize of the corner-pinned frame (frame = source size): frame
 * pixel = output pixel / res_scale + res_offset (HDR Scale's layout, so Fill / Fit /
 * Stretch / desqueeze / pan behave the same). One resample covers pre-warp, corner pin
 * and resize.
 *
 * SampleBicubic (9 Nov, after HQ overloaded a GTX 1650 Super with rotation + distortion):
 * Catmull-Rom 4 x 4 at the mapped point, linear light, anti-ringing towards the centre
 * 2 x 2; no pre-pass, constant cost.
 *
 * SampleHQ, per output pixel: the local footprint J (source texels per output pixel)
 * from forward differences of the map; footprint widths w = clamp(singular value, 1, 2)
 * along J J^T's eigenvectors; a 2-D Lanczos-3 product kernel on F^-1 (q - texel) in the
 * source grid (exact interpolation where the warp does not reduce, widened area filter
 * where it does). Enlarging: PQ domain with anti-ringing over the 2 x 2 nearest texels;
 * reducing: linear light; smoothstep(1.001, 1.5, largest singular value) blends the two.
 * Where nothing is reduced (both widths 1) a separable fast path reads only the RGBA16F PQ
 * copy (36 taps, 12 kernel evaluations).
 */

#include "quad-math.hpp"

#include <array>
#include <vector>

namespace hdrtk {
namespace quad {

struct WarpShader {
	float out_size[2] = {1920, 1080};
	float in_size[2] = {1920, 1080};
	float res_scale[2] = {1, 1};
	float res_offset[2] = {0, 0};
	int warp = 0; // 0 bilinear corner pin, 1 projective
	BilinearUniforms bil{};
	ProjectiveUniforms proj{};
	PreWarpUniforms pre{};
	float antiring = 0.8f;
	float pq_k = 0.0203f;
};

// map_px(): output pixel position (centres at +0.5) -> source (u, v, valid)
std::array<float, 3> shader_map_px(const WarpShader &s, float cx, float cy);

struct Image {
	int w = 0, h = 0;
	std::vector<float> px; // RGBA, row-major
	const float *at(int x, int y) const { return &px[((size_t)y * (size_t)w + (size_t)x) * 4]; }
};

float shader_pq_encode(float y);
float shader_pq_decode(float e);
Image shader_encode_pq(const Image &in, float pq_k); // PSEncodePQ (into RGBA16F: half-rounded)
// PSSampleHQ for output pixel (ox, oy)
std::array<float, 4> shader_sample_hq(const WarpShader &s, const Image &img, const Image &pq, int ox, int oy);
// PSSampleBicubic: Catmull-Rom 4 x 4, linear light, anti-ringing (centre 2 x 2)
std::array<float, 4> shader_sample_bicubic(const WarpShader &s, const Image &img, int ox, int oy);
// PSSampleLinear (bilinear texture filtering, clamp addressing) for comparison
std::array<float, 4> shader_sample_linear(const WarpShader &s, const Image &img, int ox, int oy);

} // namespace quad
} // namespace hdrtk
