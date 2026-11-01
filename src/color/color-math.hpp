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
 * HDR Color maths (brief sections 5-7), CPU side. No libobs, unit-tested in
 * tests/cpu/test_color.cpp. Written from the brief's equations.
 *
 * Working units: straight linear Rec.709-primary RGB in nominal nits.
 * Fixed v1 order (brief 5.3):
 *
 *   unpremultiply -> nits -> C0 -> white balance -> global exposure
 *   -> zone masks (computed here and frozen) -> combined zone exposure
 *   -> contrast around pivot -> global + zone wheel -> global x zone saturation
 *   -> offset (user-directed addition, 4 Oct 2026) -> grade mix
 *   -> low soft clip -> high soft clip (brief 8) -> [gamut containment: P5]
 *   -> working units -> premultiply
 */

#include <array>
#include <string>

namespace hdrtk {
namespace color {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>; // row-major, applied to column vectors

// Rec.709 / D65 RGB -> XYZ (brief 5.1, section 15). Y row is the luminance row.
extern const Mat3 kRgbToXyz;
extern const Vec3 kLuma;
extern const Mat3 kBradford;

Mat3 identity3();
Mat3 mul(const Mat3 &a, const Mat3 &b);
Vec3 mul(const Mat3 &m, const Vec3 &v);
bool invert(const Mat3 &m, Mat3 &out);
double dot(const Vec3 &a, const Vec3 &b);

// ---- White balance (brief 6) ----------------------------------------------

// CIE daylight locus x,y for 4000..25000 K (Colour's CCT_to_xy_CIE_D). Returns
// false outside the domain.
bool daylight_xy(double cct, double &x, double &y);
// CIE 1960 UCS
void xy_to_uv(double x, double y, double &u, double &v);
void uv_to_xy(double u, double v, double &x, double &y);

// Target white XYZ (Y = 1) for a mired shift and a tint in -1..1 (UI value / 100).
bool target_white(double mired_shift, double tint, Vec3 &xyz, std::string *error = nullptr);

// Bradford adaptation from D65 to the target white, expressed in linear
// Rec.709 RGB. Exactly the identity matrix at (0, 0).
bool white_balance_matrix(double mired_shift, double tint, Mat3 &rgb, std::string *error = nullptr);

// ---- Colour wheel (brief 7.4) ---------------------------------------------

// Luminance-orthogonal unit hue direction for angle theta (radians); red at
// 0, yellow 60 deg, green 120, cyan 180, blue 240, magenta 300.
Vec3 hue_direction(double theta);
// Wheel point (x, y) in the unit disk (radius clamped to 1) times the
// sensitivity k -> RGB balance delta. Zero radius -> exactly zero.
Vec3 wheel_delta(double x, double y, double k = 0.5);

// ---- Tonal zones (brief 7.1) -------------------------------------------------

// Seven zones (user-directed 4 Oct 2026: Midtones added to the brief's six).
constexpr int kZoneCount = 7;
enum Zone { ZoneBlack = 0, ZoneDark, ZoneShadow, ZoneMidtones, ZoneLight, ZoneHighlight, ZoneSpecular };
// Stable machine names used in settings keys (zone_<name>_...).
extern const char *const kZoneNames[kZoneCount];

// Effective edges of an open end. Far outside any reachable stop value
// (|s| < 40 for every finite float luminance, s >= log2(1e-6 / gray) for
// Y <= 0), so open windows need no special case in the shader.
constexpr double kOpenLow = -1000.0;
constexpr double kOpenHigh = 1000.0;
constexpr double kMinFalloff = 0.05; // stops (brief 7.1)
constexpr double kEdgeLimit = 20.0;  // stops either side of the gray reference

struct ZoneParams {
	bool enabled = true;
	double exposure_ev = 0.0; // +-4
	double saturation = 1.0;  // 0 .. 2
	double wheel_x = 0.0;
	double wheel_y = 0.0;
	// Stops relative to gray_nits, a < b <= c < d.
	double a = 0, b = 0, c = 0, d = 0;
	// Open ends: full strength all the way down to black (and Y <= 0) / up to any
	// peak. Black is always open below, Specular always open above. The stored
	// edges on an open side are kept (re-closing restores them) but not used.
	bool open_low = false;
	bool open_high = false;
	bool active() const { return enabled && (exposure_ev != 0 || saturation != 1 || wheel_x != 0 || wheel_y != 0); }
	void effective_edges(double e[4]) const
	{
		e[0] = open_low ? kOpenLow : a;
		e[1] = open_low ? kOpenLow + 1 : b;
		e[2] = open_high ? kOpenHigh : c;
		e[3] = open_high ? kOpenHigh + 1 : d;
	}
};

// Defaults (user-directed 4 Oct 2026). Dark, Shadow, Midtones, Light and Highlight
// hand over at -4 / -1 / +1 / +3 stops with each pair sharing one mirrored fade
// (2 stops at -4, 1.5 stops elsewhere), so their weights sum to exactly 1:
// equal pushes on neighbours are an even push, with no doubling at the seams.
// Black (full <= -7) and Specular (full >= +5) sit on top of Dark / Highlight.
ZoneParams default_zone(int zone);
std::array<ZoneParams, kZoneCount> default_zones();
bool zone_fixed_open_low(int zone);  // Black
bool zone_fixed_open_high(int zone); // Specular

// smooth01(t) = t^2 (3 - 2t) on the clamped mask coordinate.
double smooth01(double t);
// Window weight at stop value s for edges a < b <= c < d.
double zone_weight(double s, double a, double b, double c, double d);
double zone_weight(double s, const ZoneParams &z); // effective edges
// Tonal coordinate of a luminance: log2(max(Y, eps) / gray). Nonpositive Y maps
// to the lowest coordinate (Black tail) without a log of a negative number.
double tonal_stop(double y_nits, double gray_nits);

// ---- Parameters -------------------------------------------------------------

struct GlobalParams {
	double exposure_ev = 0.0; // +-6
	double contrast = 1.0;    // 0.5 .. 2
	double pivot_nits = 18.0; // > 0
	double gray_nits = 18.0;  // zone/stop reference (brief 5.2)
	double saturation = 1.0;  // 0 .. 2
	double wb_mired = 0.0;    // -90 .. 90
	double wb_tint = 0.0;     // -100 .. 100 (UI units)
	double wheel_x = 0.0;     // unit disk
	double wheel_y = 0.0;
	double grade_mix = 1.0; // 0 .. 1
	std::array<ZoneParams, kZoneCount> zones = default_zones();
	// Straight linear offset, nits, added equally to R, G, B (part of the grade:
	// before Grade Mix). Not black-preserving by design.
	double offset_nits = 0.0; // +-10
	// Soft clips (brief 8): after Grade Mix, on straight linear luminance.
	bool low_clip = false;
	double low_knee_nits = 0.1; // L > 0
	double low_strength = 0.5;  // beta 0..1
	bool high_clip = false;
	double high_peak_nits = 1000.0; // P > 0 (asymptote)
	double high_softness = 0.25;    // q 0..0.95; 0 = hard cap at P
	double high_knee_nits() const { return high_peak_nits * (1.0 - high_softness); }
};

// Both clips enabled with the low knee above the high knee (brief 8.3: no
// unmodified middle region). The caller keeps the last valid curves.
bool clips_conflict(const GlobalParams &p);

// Brief 8.2 / 8.3 curves on luminance (double reference).
double toe_curve(double y, double knee, double strength);
double shoulder_curve(double y, double peak, double softness);

// Clamp to documented ranges, replace non-finite values with defaults, and
// make zone edges ordered with the minimum falloff (deterministic push upward:
// b >= a + 0.05, c >= b, d >= c + 0.05). Returns a list of problems (empty when clean).
std::string sanitize(GlobalParams &p);

bool is_neutral(const GlobalParams &p);

// Values uploaded to the shader (precomputed on the CPU, brief 10.6).
struct ShaderParams {
	float wb_row[3][3];
	float exposure_gain;
	float contrast_minus_one;   // (contrast - 1)
	float log2_pivot_over_gray; // p
	float gray_nits;
	float wheel_delta[3];
	float saturation;
	float grade_mix;
	// Zones. Disabled zones upload neutral values (ev 0, sat 1, wheel 0).
	float zone_edges[kZoneCount][4];
	float zone_ev[kZoneCount];
	float zone_sat[kZoneCount];
	float zone_wheel[kZoneCount][3];
	float offset_nits;
	float low_knee, low_strength;              // L, beta
	float high_knee, high_headroom, high_peak; // H = P - D, D = P q, P
	// Stage enables (uniform branches give exact identity for neutral stages).
	float use_wb, use_contrast, use_zones, use_wheel, use_saturation, use_mix;
	float use_offset, use_low_clip, use_high_clip;
};

bool make_shader_params(const GlobalParams &p, ShaderParams &out, std::string *error = nullptr);

// Reference pixel pipeline on straight linear RGB in nits (double), and the
// float mirror of data/effects/hdr-color.effect (PSGrade, nits domain).
Vec3 reference_grade(const GlobalParams &p, const Vec3 &c_nits);
std::array<float, 3> shader_grade(const ShaderParams &s, const std::array<float, 3> &c_nits);

// Frozen zone weights for a pixel (after WB and global exposure), double.
std::array<double, kZoneCount> reference_weights(const GlobalParams &p, const Vec3 &c_nits);

constexpr double kYEpsilonNits = 1e-6;

} // namespace color
} // namespace hdrtk
