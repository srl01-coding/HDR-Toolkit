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

// Implemented from docs/HQDN3D_DESIGN.md section 8 (independent of FFmpeg/MPlayer code).

#include "spatial-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace denoise {

void sanitize(SpatialParams &p)
{
	auto cl = [](double &v, double lo, double hi, double def) {
		v = std::isfinite(v) ? std::clamp(v, lo, hi) : def;
	};
	cl(p.luma, 0, kStrengthMax, 0);
	cl(p.chroma, 0, kStrengthMax, 0);
	cl(p.k_nits, 0.001, 50, 0.1);
	p.radius = std::clamp(p.radius, 1, kSpatialRadiusMax);
	sanitize(p.profile);
}

namespace {

struct Yc {
	double y, c[3];
};

Yc split(const Rgba &v)
{
	Yc o;
	o.y = kLumaR * v.r + kLumaG * v.g + kLumaB * v.b;
	o.c[0] = v.r - o.y;
	o.c[1] = v.g - o.y;
	o.c[2] = v.b - o.y;
	return o;
}

// Line accessor: element i of line `line` along the chosen axis.
struct Line {
	const Image &im;
	bool vertical;
	int line;
	int size() const { return vertical ? im.h : im.w; }
	const Rgba &at(int i) const { return vertical ? im.at(line, i) : im.at(i, line); }
};

Rgba &out_at(Image &im, bool vertical, int line, int i)
{
	return vertical ? im.px[(size_t)i * im.w + line] : im.px[(size_t)line * im.w + i];
}

int line_count(const Image &im, bool vertical)
{
	return vertical ? im.w : im.h;
}

} // namespace

// ---- B: double reference ---------------------------------------------------------

void spatial_b_pass(const SpatialParams &p, const Image &in, Image &out, bool vertical)
{
	out.w = in.w;
	out.h = in.h;
	out.px.resize(in.px.size());
	const double tl0 = kThresholdPerUnit * p.luma, tc0 = kThresholdPerUnit * p.chroma;
	for (int line = 0; line < line_count(in, vertical); line++) {
		const Line L{in, vertical, line};
		const int n = L.size();
		for (int x = 0; x < n; x++) {
			const Rgba &ctr = L.at(x);
			const Yc c0 = split(ctr);
			const double mp = profile_multiplier(p.profile, std::fabs(c0.y));
			const double tl = tl0 * mp, tc = tc0 * mp;
			const double f0 = comp(c0.y, p.k_nits);
			double sl = 1, sc = 1, yacc = c0.y, cacc[3] = {c0.c[0], c0.c[1], c0.c[2]};
			double dec = 1;
			for (int k = 1; k <= p.radius; k++) {
				dec *= kBetaS;
				for (int side = 0; side < 2; side++) {
					const int j = side == 0 ? x - k : x + k;
					if (j < 0 || j >= n)
						continue;
					const Yc q = split(L.at(j));
					const double wl =
						tl > 0 ? dec * response(std::fabs(comp(q.y, p.k_nits) - f0) / tl) : 0.0;
					const double wc = tc > 0 ? dec * response(chroma_distance(q.c, c0.c, q.y, c0.y,
												  p.k_nits) /
										  tc)
								 : 0.0;
					sl += wl;
					yacc += wl * q.y;
					sc += wc;
					for (int i = 0; i < 3; i++)
						cacc[i] += wc * q.c[i];
				}
			}
			Rgba &o = out_at(out, vertical, line, x);
			if (sl == 1 && sc == 1) {
				o = ctr; // exact identity
				continue;
			}
			const double yo = yacc / sl;
			o.r = yo + cacc[0] / sc;
			o.g = yo + cacc[1] / sc;
			o.b = yo + cacc[2] / sc;
			o.a = ctr.a;
		}
	}
}

void spatial_b(const SpatialParams &p, const Image &in, Image &out)
{
	Image h;
	spatial_b_pass(p, in, h, false);
	spatial_b_pass(p, h, out, true);
}

// ---- B: float mirror (operation order of PSSpatialH / PSSpatialV) -----------------------

SpatialShaderParams make_spatial_shader_params(const SpatialParams &p)
{
	SpatialShaderParams s;
	s.t_luma = (float)(kThresholdPerUnit * p.luma);
	s.t_chroma = (float)(kThresholdPerUnit * p.chroma);
	s.k = (float)p.k_nits;
	s.radius = p.radius;
	s.profile = make_shader_profile(p.profile);
	return s;
}

namespace {

float comp_sf(float y, float k)
{
	const float a = std::log2(1.0f + std::fabs(y) / k);
	return y < 0.0f ? -a : a;
}

float response_sf(float x)
{
	const float u = 1.0f - x * x;
	return x < 1.0f ? u * u : 0.0f;
}

float luma_f(const float c[3])
{
	return (float)kLumaR * c[0] + (float)kLumaG * c[1] + (float)kLumaB * c[2];
}

} // namespace

void spatial_b_pass_f(const SpatialShaderParams &s, const Image &in, Image &out, bool vertical)
{
	out.w = in.w;
	out.h = in.h;
	out.px.resize(in.px.size());
	for (int line = 0; line < line_count(in, vertical); line++) {
		const Line L{in, vertical, line};
		const int n = L.size();
		for (int x = 0; x < n; x++) {
			const Rgba &ctr = L.at(x);
			const float c0[3] = {(float)ctr.r, (float)ctr.g, (float)ctr.b};
			const float y0 = luma_f(c0);
			const float ch0[3] = {c0[0] - y0, c0[1] - y0, c0[2] - y0};
			const float mp = profile_multiplier_f(s.profile, std::fabs(y0));
			const float tl = s.t_luma * mp, tc = s.t_chroma * mp;
			const float f0 = comp_sf(y0, s.k);
			float sl = 1.0f, sc = 1.0f, yacc = y0;
			float cacc[3] = {ch0[0], ch0[1], ch0[2]};
			float dec = 1.0f;
			for (int k = 1; k <= kSpatialRadiusMax; k++) {
				if ((float)k > (float)s.radius)
					break;
				dec = dec * 0.9f;
				for (int side = 0; side < 2; side++) {
					const int j = side == 0 ? x - k : x + k;
					if (!(j >= 0 && j < n))
						continue;
					const Rgba &qv = L.at(j);
					const float q[3] = {(float)qv.r, (float)qv.g, (float)qv.b};
					const float yq = luma_f(q);
					const float cq[3] = {q[0] - yq, q[1] - yq, q[2] - yq};
					const float wl =
						tl > 0.0f ? dec * response_sf(std::fabs(comp_sf(yq, s.k) - f0) / tl)
							  : 0.0f;
					float dd = 0.0f;
					for (int i = 0; i < 3; i++)
						dd += (cq[i] - ch0[i]) * (cq[i] - ch0[i]);
					const float dc = std::sqrt(dd) /
							 (0.5f * (std::fabs(yq) + std::fabs(y0)) + s.k) * 1.442695041f;
					const float wc = tc > 0.0f ? dec * response_sf(dc / tc) : 0.0f;
					sl = sl + wl;
					yacc = yacc + wl * yq;
					sc = sc + wc;
					for (int i = 0; i < 3; i++)
						cacc[i] = cacc[i] + wc * cq[i];
				}
			}
			Rgba &o = out_at(out, vertical, line, x);
			if (sl == 1.0f && sc == 1.0f) {
				o.r = c0[0];
				o.g = c0[1];
				o.b = c0[2];
				o.a = (float)ctr.a;
				continue;
			}
			const float yo = yacc / sl;
			o.r = yo + cacc[0] / sc;
			o.g = yo + cacc[1] / sc;
			o.b = yo + cacc[2] / sc;
			o.a = (float)ctr.a;
		}
	}
}

void spatial_b_f(const SpatialShaderParams &s, const Image &in, Image &out)
{
	Image h;
	spatial_b_pass_f(s, in, h, false);
	spatial_b_pass_f(s, h, out, true);
}

// ---- A: bidirectional separable recursion ------------------------------------------

void spatial_a_pass(const SpatialParams &p, const Image &in, Image &out, bool vertical)
{
	out.w = in.w;
	out.h = in.h;
	out.px.resize(in.px.size());
	const double tl0 = kThresholdPerUnit * p.luma, tc0 = kThresholdPerUnit * p.chroma;
	std::vector<Yc> fw, bw;
	std::vector<char> fpass, bpass; // the step at i used w = 0 for both luma and chroma

	auto run = [&](const Line &L, int start, int step, std::vector<Yc> &st, std::vector<char> &passed) {
		const int n = L.size();
		st.assign((size_t)n, Yc{});
		passed.assign((size_t)n, 1);
		Yc q = split(L.at(start));
		st[(size_t)start] = q;
		for (int i = start + step; i >= 0 && i < n; i += step) {
			const Yc v = split(L.at(i));
			const double mp = profile_multiplier(p.profile, 0.5 * (std::fabs(v.y) + std::fabs(q.y)));
			const double tl = tl0 * mp, tc = tc0 * mp;
			const double wl =
				tl > 0 ? kBetaS * response(std::fabs(comp(v.y, p.k_nits) - comp(q.y, p.k_nits)) / tl)
				       : 0.0;
			const double wc = tc > 0 ? kBetaS * response(chroma_distance(v.c, q.c, v.y, q.y, p.k_nits) / tc)
						 : 0.0;
			q.y = (1 - wl) * v.y + wl * q.y;
			for (int k = 0; k < 3; k++)
				q.c[k] = (1 - wc) * v.c[k] + wc * q.c[k];
			st[(size_t)i] = q;
			passed[(size_t)i] = (wl == 0.0 && wc == 0.0);
		}
	};

	for (int line = 0; line < line_count(in, vertical); line++) {
		const Line L{in, vertical, line};
		const int n = L.size();
		run(L, 0, 1, fw, fpass);
		run(L, n - 1, -1, bw, bpass);
		for (int i = 0; i < n; i++) {
			const Rgba &src = L.at(i);
			Rgba &o = out_at(out, vertical, line, i);
			if (fpass[(size_t)i] && bpass[(size_t)i]) {
				o = src; // both directions restarted here: exact identity
				continue;
			}
			const double yo = 0.5 * (fw[(size_t)i].y + bw[(size_t)i].y);
			o.r = yo + 0.5 * (fw[(size_t)i].c[0] + bw[(size_t)i].c[0]);
			o.g = yo + 0.5 * (fw[(size_t)i].c[1] + bw[(size_t)i].c[1]);
			o.b = yo + 0.5 * (fw[(size_t)i].c[2] + bw[(size_t)i].c[2]);
			o.a = src.a;
		}
	}
}

void spatial_a(const SpatialParams &p, const Image &in, Image &out)
{
	Image h;
	spatial_a_pass(p, in, h, false);
	spatial_a_pass(p, h, out, true);
}

} // namespace denoise
} // namespace hdrtk
