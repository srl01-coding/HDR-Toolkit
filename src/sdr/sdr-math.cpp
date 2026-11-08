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

#include "sdr-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace sdr {

namespace {

constexpr double kKr = 0.2627, kKg = 0.6780, kKb = 0.0593;

double clip01(double v)
{
	return std::min(1.0, std::max(0.0, v));
}

float clip01f(float v)
{
	return std::min(1.0f, std::max(0.0f, v));
}

// libobs color.effect matrices (float, as the shader)
const float kM709to2020[3][3] = {{0.62740389593469903f, 0.32928303837788370f, 0.043313065687417225f},
				 {0.069097289358232075f, 0.91954039507545871f, 0.011362315566309178f},
				 {0.016391438875150280f, 0.088013307877225749f, 0.89559525324762401f}};
const float kM2020to709[3][3] = {{1.6604910021084345f, -0.58764113878854951f, -0.072849863319884883f},
				 {-0.12455047452159074f, 1.1328998971259603f, -0.0083494226043694768f},
				 {-0.018150763354905303f, -0.10057889800800739f, 1.1187296613629127f}};

void mulf(const float m[3][3], const float v[3], float o[3])
{
	for (int i = 0; i < 3; i++)
		o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

float srgb_decode_f(float u)
{
	return (u <= 0.04045f) ? (u / 12.92f) : std::pow(u * (1.0f / 1.055f) + 0.055f / 1.055f, 2.4f);
}

} // namespace

double rho(double l)
{
	return 1.0 + 32.0 * std::pow(l / 10000.0, 1.0 / 2.4);
}

double tone_curve_yp(double yp)
{
	if (yp <= 0.7399)
		return 1.077 * yp;
	if (yp < 0.9909)
		return -1.1510 * yp * yp + 2.7811 * yp - 0.6302;
	return 0.5 * yp + 0.5;
}

double tone_luma(double y, double rho_hdr, double rho_sdr)
{
	const double yp = std::log(1.0 + (rho_hdr - 1.0) * y) / std::log(rho_hdr);
	const double yc = tone_curve_yp(yp);
	return (std::pow(rho_sdr, yc) - 1.0) / (rho_sdr - 1.0);
}

Vec3 bt2446a(const Vec3 &v709, const Params &p)
{
	const Vec3 lin = input::rec709_to_rec2020(v709);
	Vec3 e;
	for (int i = 0; i < 3; i++)
		e[i] = std::pow(std::max(0.0, lin[i] * p.sdr_white / p.lhdr), 1.0 / 2.4);
	const double y = kKr * e[0] + kKg * e[1] + kKb * e[2];
	if (!(y > 0.0))
		return {0.0, 0.0, 0.0};
	const double ysdr = tone_luma(y, rho(p.lhdr), rho(p.lsdr));
	const double f = ysdr / (1.1 * y);
	const double cb = f * (e[2] - y) / 1.8814;
	const double cr = f * (e[0] - y) / 1.4746;
	const double yt = ysdr - std::max(0.1 * cr, 0.0);
	const Vec3 s2020 = {clip01(yt + 1.4746 * cr), clip01(yt - 0.16455 * cb - 0.57135 * cr),
			    clip01(yt + 1.8814 * cb)};
	const Vec3 l2020 = {std::pow(s2020[0], 2.4), std::pow(s2020[1], 2.4), std::pow(s2020[2], 2.4)};
	const Vec3 l709 = input::rec2020_to_rec709(l2020);
	Vec3 out;
	for (int i = 0; i < 3; i++)
		out[i] = input::srgb_decode(std::pow(clip01(l709[i]), 1.0 / 2.4));
	return out;
}

Vec3 obs_reinhard(const Vec3 &v709)
{
	Vec3 r = input::rec709_to_rec2020(v709);
	for (double &c : r) {
		c = clip01(c / (c + 1.0));
		c = input::srgb_decode(std::pow(c, 1.0 / 2.4));
	}
	return input::rec2020_to_rec709(r);
}

ShaderParams make_shader_params(const Params &p)
{
	ShaderParams sp;
	const double rh = rho(p.lhdr), rs = rho(p.lsdr);
	sp.to_norm = (float)(p.sdr_white / p.lhdr);
	sp.rho_hdr_m1 = (float)(rh - 1.0);
	sp.inv_log_rho_hdr = (float)(1.0 / std::log(rh));
	sp.log_rho_sdr = (float)std::log(rs);
	sp.inv_rho_sdr_m1 = (float)(1.0 / (rs - 1.0));
	return sp;
}

// Line-by-line mirror of PSConvert in hdr-sdr.effect (sdr_2446a).
void bt2446a_f(const float in[3], const ShaderParams &sp, float out[3])
{
	float lin[3];
	mulf(kM709to2020, in, lin);
	float e[3];
	for (int i = 0; i < 3; i++)
		e[i] = std::pow(std::max(lin[i] * sp.to_norm, 0.0f), 1.0f / 2.4f);
	const float y = 0.2627f * e[0] + 0.6780f * e[1] + 0.0593f * e[2];
	if (!(y > 1e-6f)) {
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	const float yp = std::log(1.0f + sp.rho_hdr_m1 * y) * sp.inv_log_rho_hdr;
	const float yc = yp <= 0.7399f ? 1.077f * yp
				       : (yp < 0.9909f ? (-1.1510f * yp + 2.7811f) * yp - 0.6302f : 0.5f * yp + 0.5f);
	const float ysdr = (std::exp(sp.log_rho_sdr * yc) - 1.0f) * sp.inv_rho_sdr_m1;
	const float f = ysdr / (1.1f * y);
	const float cb = f * (e[2] - y) * (1.0f / 1.8814f);
	const float cr = f * (e[0] - y) * (1.0f / 1.4746f);
	const float yt = ysdr - std::max(0.1f * cr, 0.0f);
	float s[3] = {clip01f(yt + 1.4746f * cr), clip01f(yt - 0.16455f * cb - 0.57135f * cr),
		      clip01f(yt + 1.8814f * cb)};
	for (float &c : s)
		c = std::pow(c, 2.4f);
	float l709[3];
	mulf(kM2020to709, s, l709);
	for (int i = 0; i < 3; i++)
		out[i] = srgb_decode_f(std::pow(clip01f(l709[i]), 1.0f / 2.4f));
}

void obs_reinhard_f(const float in[3], float out[3])
{
	float r[3];
	mulf(kM709to2020, in, r);
	for (float &c : r) {
		c = clip01f(c / (c + 1.0f));
		c = srgb_decode_f(std::pow(c, 1.0f / 2.4f));
	}
	mulf(kM2020to709, r, out);
}

} // namespace sdr
} // namespace hdrtk
