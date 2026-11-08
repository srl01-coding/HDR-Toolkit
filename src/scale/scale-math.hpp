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
 * HDR Scale (docs/HDR_SCALE.md): geometry and resampling maths, no libobs dependency
 * (tests/cpu/test_scale.cpp).
 *
 * Geometry: the source (in_w x in_h, pixel aspect `desqueeze`) is fitted to the output
 * (out_w x out_h) by Fill (crop the excess), Fit (transparent borders) or Stretch, then
 * zoomed and panned. Each axis is an affine map from output pixel o to the continuous
 * input coordinate u = (o + 0.5) / scale + offset (texel i covers [i, i + 1)).
 *
 * Resampling: separable, one pass per axis. Enlarging axes interpolate in a perceptual
 * domain (ST 2084 PQ of absolute luminance, sign-symmetric) so edges against highlights
 * do not ring in linear light; reducing axes filter in linear light with the kernel
 * widened by 1 / scale (area-correct, no aliasing). Anti-ringing clamps the result
 * towards the range of the two nearest input samples. Mirrored in
 * data/effects/hdr-scale.effect (resample_f) - keep in lock-step.
 */

#include <cstdint>

namespace hdrtk {
namespace scale {

enum Fit { FitFill = 0, FitFit = 1, FitStretch = 2 };
enum Kernel { KernelLanczos3 = 0, KernelCatmullRom = 1, KernelMitchell = 2, KernelBilinear = 3 };
enum Domain { DomainAuto = 0, DomainLinear = 1, DomainPerceptual = 2 };

struct Params {
	int out_w = 0, out_h = 0; // 0: from the source (desqueezed)
	Fit fit = FitFill;
	double desqueeze = 1.0;          // horizontal pixel aspect (anamorphic squeeze factor)
	double zoom = 1.0;               // >= 1: punch-in
	double pan_x = 0.0, pan_y = 0.0; // -1..1 of the available slack
	Kernel kernel = KernelLanczos3;
	double antiring = 0.8; // 0..1
	Domain domain = DomainAuto;
};
void sanitize(Params &p);

struct AxisMap {
	double scale = 1.0;  // output pixels per input pixel
	double offset = 0.0; // u = (o + 0.5) / scale + offset
};

struct Layout {
	uint32_t out_w = 0, out_h = 0;
	AxisMap x, y;
};

Layout compute_layout(uint32_t in_w, uint32_t in_h, const Params &p);
bool axis_identity(const AxisMap &a);

// Kernel support radius in input pixels at scale >= 1.
float kernel_radius(Kernel k);
float kernel_weight(Kernel k, float x);

// ST 2084, Y = luminance / 10000 nits
float pq_encode(float y);
float pq_decode(float e);

struct AxisShader {
	float scale, offset;
	int kernel;
	float radius;   // support in input pixels (widened when reducing)
	float wscale;   // kernel argument multiplier: min(scale, 1)
	int perceptual; // 1: PQ domain
	float pq_k;     // nits per unit / 10000
	float antiring;
	float in_len; // input length along the axis
};
AxisShader make_axis_shader(const AxisMap &a, const Params &p, uint32_t in_len, double nits_per_unit);

// One output sample along an axis; `line` is the input row/column (premultiplied RGBA).
// Mirror of PSResample in hdr-scale.effect.
void resample_f(const float (*line)[4], int n, int o, const AxisShader &s, float out[4]);

} // namespace scale
} // namespace hdrtk
