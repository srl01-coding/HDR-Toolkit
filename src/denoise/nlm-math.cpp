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

// Implemented from docs/denoise/NLMEANS_DESIGN.md (independent of FFmpeg/HandBrake code).

#include "nlm-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace denoise {

void sanitize(NlmParams &p)
{
	auto cl = [](double &v, double lo, double hi, double def) {
		v = std::isfinite(v) ? std::clamp(v, lo, hi) : def;
	};
	cl(p.luma, 0, 20, 0);
	cl(p.chroma, 0, 20, 0);
	cl(p.k_nits, 0.001, 10, 0.1);
	cl(p.lambda, 0, 10, 0);
	cl(p.cap, 0, 1, 0.9);
	cl(p.tgain, 0, 16, 1);
	sanitize(p.profile);
	p.patch = std::clamp(p.patch, 0, 4);
	p.search = std::clamp(p.search, 0, 10);
	p.tsearch = std::clamp(p.tsearch, 0, 10);
	if (p.policy < NlmPolicyA || p.policy > NlmPolicyH)
		p.policy = NlmPolicyA;
}

namespace {

// Per-image planes used by the distance and the reconstruction.
struct Planes {
	int w = 0, h = 0;
	std::vector<double> g, y, c0, c1, c2;
	const Image *src = nullptr;
	void build(const Image &im, double k)
	{
		src = &im;
		w = im.w;
		h = im.h;
		const size_t n = im.px.size();
		g.resize(n);
		y.resize(n);
		c0.resize(n);
		c1.resize(n);
		c2.resize(n);
		for (size_t i = 0; i < n; i++) {
			const Rgba &v = im.px[i];
			const double Y = kLumaR * v.r + kLumaG * v.g + kLumaB * v.b;
			y[i] = Y;
			c0[i] = v.r - Y;
			c1[i] = v.g - Y;
			c2[i] = v.b - Y;
			g[i] = comp(Y, k);
		}
	}
	size_t at(int x, int yy) const
	{
		x = std::clamp(x, 0, w - 1);
		yy = std::clamp(yy, 0, h - 1);
		return (size_t)yy * w + x;
	}
};

struct Candidate {
	int dx, dy;
	bool temporal;
};

std::vector<Candidate> candidates(const NlmParams &p, bool temporal_ok)
{
	std::vector<Candidate> v;
	for (int dy = -p.search; dy <= p.search; dy++)
		for (int dx = -p.search; dx <= p.search; dx++)
			if (dx || dy)
				v.push_back({dx, dy, false});
	if (p.temporal && temporal_ok)
		for (int dy = -p.tsearch; dy <= p.tsearch; dy++)
			for (int dx = -p.tsearch; dx <= p.tsearch; dx++)
				v.push_back({dx, dy, true});
	return v;
}

double pixel_e(const NlmParams &p, const Planes &a, size_t ia, const Planes &b, size_t ib)
{
	const double d = a.g[ia] - b.g[ib];
	double e = d * d;
	if (p.lambda > 0) {
		const double ca[3] = {a.c0[ia], a.c1[ia], a.c2[ia]};
		const double cb[3] = {b.c0[ib], b.c1[ib], b.c2[ib]};
		const double dc = chroma_distance(ca, cb, a.y[ia], b.y[ib], p.k_nits);
		e += p.lambda * dc * dc;
	}
	return e;
}

double weight(double D, double h)
{
	if (!(h > 0))
		return 0.0;
	const double r = D / (h * h);
	return r > kNlmCutoff ? 0.0 : std::exp(-r);
}

// Accumulators for one pixel, spatial and temporal kept apart for the share cap.
struct Acc {
	double sl = 1, sc = 1;            // spatial (incl. self) weight sums
	double ny = 0, nc[3] = {0, 0, 0}; // spatial numerators (self added at the end)
	double tl = 0, tc = 0, ty = 0, tcc[3] = {0, 0, 0};
	void add(bool temporal, double wl, double wc, const Planes &v, size_t iv)
	{
		if (temporal) {
			tl += wl;
			tc += wc;
			ty += wl * v.y[iv];
			tcc[0] += wc * v.c0[iv];
			tcc[1] += wc * v.c1[iv];
			tcc[2] += wc * v.c2[iv];
		} else {
			sl += wl;
			sc += wc;
			ny += wl * v.y[iv];
			nc[0] += wc * v.c0[iv];
			nc[1] += wc * v.c1[iv];
			nc[2] += wc * v.c2[iv];
		}
	}
	// share scale factor so that temporal / total <= cap
	static double cap_scale(double t, double s, double cap)
	{
		if (t <= 0)
			return 1.0;
		const double share = t / (t + s);
		if (share <= cap)
			return 1.0;
		return cap * s / ((1.0 - cap) * t);
	}
	Rgba finish(const Planes &cur, size_t i, bool recursive, double cap, double *share_out)
	{
		const Rgba &src = cur.src->px[i];
		double kl = 1, kc = 1;
		if (recursive) {
			kl = cap_scale(tl, sl, cap);
			kc = cap_scale(tc, sc, cap);
		}
		const double dl = sl + kl * tl, dc = sc + kc * tc;
		if (share_out)
			*share_out = dl > 0 ? kl * tl / dl : 0;
		if (dl == 1.0 && dc == 1.0)
			return src; // only the self candidate: exact identity
		const double yo = (cur.y[i] + ny + kl * ty) / dl;
		Rgba o;
		o.r = yo + (cur.c0[i] + nc[0] + kc * tcc[0]) / dc;
		o.g = yo + (cur.c1[i] + nc[1] + kc * tcc[1]) / dc;
		o.b = yo + (cur.c2[i] + nc[2] + kc * tcc[2]) / dc;
		o.a = src.a;
		return o;
	}
};

struct Refs {
	Planes cur, din, val; // din: temporal distance reference; val: temporal value reference
	bool temporal_ok = false;
};

void build_refs(const NlmParams &p, const NlmFrame &f, Refs &r)
{
	r.cur.build(*f.cur, p.k_nits);
	r.temporal_ok = false;
	if (!p.temporal || !(f.g > 0))
		return;
	const Image *din = p.policy == NlmPolicyB ? f.prev_out : f.prev_in;
	const Image *val = p.policy == NlmPolicyA ? f.prev_in : f.prev_out;
	if (!din || !val || din->w != f.cur->w || din->h != f.cur->h || val->w != f.cur->w || val->h != f.cur->h)
		return;
	r.din.build(*din, p.k_nits);
	r.val.build(*val, p.k_nits);
	r.temporal_ok = true;
}

} // namespace

void nlm_naive(const NlmParams &p, const NlmFrame &f, Image &out)
{
	const Image &cur = *f.cur;
	out.w = cur.w;
	out.h = cur.h;
	out.px.resize(cur.px.size());
	const double hl = kThresholdPerUnit * p.luma, hc = kThresholdPerUnit * p.chroma;
	if (!(hl > 0) && !(hc > 0)) {
		out.px = cur.px;
		return;
	}
	Refs r;
	build_refs(p, f, r);
	const std::vector<Candidate> cand = candidates(p, r.temporal_ok);
	const bool recursive = p.policy != NlmPolicyA;
	const double inv = 1.0 / ((2 * p.patch + 1) * (2 * p.patch + 1));
	for (int y = 0; y < cur.h; y++)
		for (int x = 0; x < cur.w; x++) {
			Acc a;
			for (const Candidate &c : cand) {
				const int cx = x + c.dx, cy = y + c.dy;
				if (cx < 0 || cy < 0 || cx >= cur.w || cy >= cur.h)
					continue;
				const Planes &dref = c.temporal ? r.din : r.cur;
				double D = 0;
				for (int qy = -p.patch; qy <= p.patch; qy++)
					for (int qx = -p.patch; qx <= p.patch; qx++)
						D += pixel_e(p, r.cur, r.cur.at(x + qx, y + qy), dref,
							     dref.at(x + qx + c.dx, y + qy + c.dy));
				D *= inv;
				const double mp =
					profile_multiplier(p.profile, std::fabs(r.cur.y[(size_t)y * cur.w + x]));
				double wl = weight(D, hl * mp), wc = weight(D, hc * mp);
				if (c.temporal) {
					wl *= f.g * p.tgain;
					wc *= f.g * p.tgain;
				}
				if (wl == 0 && wc == 0)
					continue;
				const Planes &vref = c.temporal ? r.val : r.cur;
				a.add(c.temporal, wl, wc, vref, (size_t)cy * cur.w + cx);
			}
			const size_t i = (size_t)y * cur.w + x;
			out.px[i] = a.finish(r.cur, i, recursive, p.cap, nullptr);
		}
}

void nlm_offset_share(const NlmParams &p, const NlmFrame &f, Image &out, std::vector<double> *share)
{
	const Image &cur = *f.cur;
	const int W = cur.w, H = cur.h, P = p.patch;
	out.w = W;
	out.h = H;
	out.px.resize(cur.px.size());
	if (share)
		share->assign(cur.px.size(), 0.0);
	const double hl = kThresholdPerUnit * p.luma, hc = kThresholdPerUnit * p.chroma;
	if (!(hl > 0) && !(hc > 0)) {
		out.px = cur.px;
		return;
	}
	Refs r;
	build_refs(p, f, r);
	const std::vector<Candidate> cand = candidates(p, r.temporal_ok);
	const bool recursive = p.policy != NlmPolicyA;
	const double inv = 1.0 / ((2 * P + 1) * (2 * P + 1));

	std::vector<Acc> acc((size_t)W * H);
	std::vector<double> mpv((size_t)W * H);
	for (size_t i = 0; i < mpv.size(); i++)
		mpv[i] = profile_multiplier(p.profile, std::fabs(r.cur.y[i]));
	const int PW = W + 2 * P, PH = H + 2 * P;
	std::vector<double> e((size_t)PW * PH), rows((size_t)W * PH);
	for (const Candidate &c : cand) {
		const Planes &dref = c.temporal ? r.din : r.cur;
		// 1. per-pixel difference on the padded domain z in [-P, W-1+P] x [-P, H-1+P]
		for (int zy = -P; zy < H + P; zy++)
			for (int zx = -P; zx < W + P; zx++)
				e[(size_t)(zy + P) * PW + (zx + P)] =
					pixel_e(p, r.cur, r.cur.at(zx, zy), dref, dref.at(zx + c.dx, zy + c.dy));
		// 2. horizontal running box sum -> rows (W x PH)
		for (int ry = 0; ry < PH; ry++) {
			const double *er = &e[(size_t)ry * PW];
			double s = 0;
			for (int k = 0; k < 2 * P + 1; k++)
				s += er[k];
			double *rr = &rows[(size_t)ry * W];
			rr[0] = s;
			for (int x = 1; x < W; x++) {
				s += er[x + 2 * P] - er[x - 1];
				rr[x] = s;
			}
		}
		// 3. vertical running box sum, weight, accumulate
		for (int x = 0; x < W; x++) {
			double s = 0;
			for (int k = 0; k < 2 * P + 1; k++)
				s += rows[(size_t)k * W + x];
			for (int y = 0; y < H; y++) {
				if (y > 0)
					s += rows[(size_t)(y + 2 * P) * W + x] - rows[(size_t)(y - 1) * W + x];
				const int cx = x + c.dx, cy = y + c.dy;
				if (cx < 0 || cy < 0 || cx >= W || cy >= H)
					continue;
				const double D = s * inv;
				const double mp = mpv[(size_t)y * W + x];
				double wl = weight(D, hl * mp), wc = weight(D, hc * mp);
				if (c.temporal) {
					wl *= f.g * p.tgain;
					wc *= f.g * p.tgain;
				}
				if (wl == 0 && wc == 0)
					continue;
				const Planes &vref = c.temporal ? r.val : r.cur;
				acc[(size_t)y * W + x].add(c.temporal, wl, wc, vref, (size_t)cy * W + cx);
			}
		}
	}
	for (size_t i = 0; i < acc.size(); i++)
		out.px[i] = acc[i].finish(r.cur, i, recursive, p.cap, share ? &(*share)[i] : nullptr);
}

void nlm_offset(const NlmParams &p, const NlmFrame &f, Image &out)
{
	nlm_offset_share(p, f, out, nullptr);
}

} // namespace denoise
} // namespace hdrtk
