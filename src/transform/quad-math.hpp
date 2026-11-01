/*
OBS HDR Toolkit
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
 * Corner-pin geometry (brief section 4). Pure CPU code, no libobs, so it is
 * unit-tested in tests/cpu/test_quad.cpp. Written from the brief's equations;
 * no StreamFX code is used.
 *
 * Conventions:
 *   normalised output rectangle, x right, y down, identity A=(0,0) B=(1,0) C=(1,1) D=(0,1)
 *   corner order A=TL, B=TR, C=BR, D=BL (perimeter order)
 *   source square (u,v) in [0,1]^2 maps to the corresponding corners
 */

#include <array>
#include <string>

namespace hdrtk {
namespace quad {

struct Vec2 {
	double x = 0.0, y = 0.0;
};

struct Quad {
	Vec2 a{0, 0}, b{1, 0}, c{1, 1}, d{0, 1}; // TL, TR, BR, BL
};

using Mat3 = std::array<std::array<double, 3>, 3>;

// Documented coordinate limits: -400% .. 500% of the output rectangle.
constexpr double kCoordMin = -4.0;
constexpr double kCoordMax = 5.0;

bool is_identity(const Quad &q);

// Forward bilinear map P(u,v) (StreamFX-style warp model).
Vec2 forward_bilinear(const Quad &q, double u, double v);

// Brief 4.3: inverse of the bilinear map for destination point p, double
// precision CPU reference. Returns false when p is outside the warped quad.
bool inverse_bilinear(const Quad &q, Vec2 p, double &u, double &v);

// Brief 4.4: homography H mapping source (u,v,1) to destination (x_h,y_h,w_h),
// h33 = 1, solved by pivoted Gaussian elimination. Returns false if singular.
bool solve_homography(const Quad &q, Mat3 &h);
bool invert3(const Mat3 &m, Mat3 &inv);
Vec2 project(const Mat3 &h, double u, double v);

struct Validation {
	bool ok = false;
	std::string reason; // empty when ok
	double area = 0.0;  // signed (positive = clockwise in y-down coordinates)
};

// Brief 4.6. `projective` additionally checks the homography (residual,
// homogeneous denominator sign over the source square, invertibility).
Validation validate(const Quad &q, bool projective);

// Uniforms for the shader, single precision as uploaded.
struct BilinearUniforms {
	float a[2], e[2], f[2], g[2];
};
struct ProjectiveUniforms {
	float row0[3], row1[3], row2[3]; // H^-1 rows, normalised
	float w_sign;                    // sign of the homogeneous denominator inside the quad
};
BilinearUniforms bilinear_uniforms(const Quad &q);
bool projective_uniforms(const Quad &q, ProjectiveUniforms &out);

// Float mirrors of the shader functions in data/effects/hdr-transform.effect.
// Kept in lock-step with the shader so tests measure GPU-like precision.
// Return value: (u, v, valid) with valid = 1 or 0.
std::array<float, 3> shader_inverse_bilinear(const BilinearUniforms &uni, float px, float py);
std::array<float, 3> shader_inverse_projective(const ProjectiveUniforms &uni, float px, float py);

// ---- Pre-warp (user-directed, 1 Nov 2026): rotation, then barrel / pincushion, then
// the corner pin. docs/PARAMETER_SCHEMA.md (schema 2). Both are centred on the frame
// and isotropic in pixels, so they commute; only their place before the corner pin
// matters. The functions below are the *sampling* map: a point (u, v) of the
// pre-warped image (what the corner pin sees as its source square) -> the source
// texture coordinate it shows.
//   d   = ((u - 0.5) W, (v - 0.5) H)                pixels from the centre
//   r^2 = |d|^2 / (W^2 + H^2) * 4                   1 at the frame corners
//   e   = s d (1 + k r^2)                           k > 0: barrel, k < 0: pincushion
//   q   = R(-theta) e                               theta > 0: clockwise on screen
//   (su, sv) = q / (W, H) + 0.5
// s is the fill scale (1, or the largest view with no empty edges or corners).
struct PreWarp {
	double rotation_deg = 0.0; // -180..180, clockwise on screen
	double distortion = 0.0;   // -100..100: + barrel (lines bow outward), - pincushion
	bool autofill = true;      // scale to fill the frame
};
constexpr double kRotationMax = 180.0;
constexpr double kDistortionMax = 100.0;
constexpr double kDistortionK = 0.3; // |k| at |distortion| = 100; k > -1/3 keeps the map monotonic

void sanitize(PreWarp &p);
bool prewarp_identity(const PreWarp &p);
double distortion_k(const PreWarp &p);
// Fill scale s for a W x H frame (1 when autofill is off).
double prewarp_fill_scale(const PreWarp &p, double w, double h);
// Double reference of the sampling map; false when the point falls outside the source.
bool prewarp_sample(const PreWarp &p, double w, double h, double u, double v, double &su, double &sv);

struct PreWarpUniforms {
	float on;      // 0 = skip (corner pin only, bit-identical to schema 1)
	float cs[2];   // cos, sin of theta
	float k;       // radial coefficient
	float inv_hd2; // 4 / (W^2 + H^2)
	float scale;   // fill scale
	float size[2]; // W, H in pixels
};
PreWarpUniforms prewarp_uniforms(const PreWarp &p, double w, double h);
// Float mirror of pre_warp() in data/effects/hdr-transform.effect: (su, sv, valid).
std::array<float, 3> shader_prewarp(const PreWarpUniforms &uni, float u, float v);

} // namespace quad
} // namespace hdrtk
