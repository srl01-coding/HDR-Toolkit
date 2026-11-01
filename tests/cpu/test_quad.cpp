// CPU tests for corner-pin geometry (brief 4.3-4.6, 11.7). No libobs needed.
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_quad.cpp src/transform/quad-math.cpp -o test_quad
#include "transform/quad-math.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace hdrtk::quad;

static int failures = 0;
#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");                \
			failures++;                       \
		}                                         \
	} while (0)

static Quad Q(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy)
{
	Quad q;
	q.a = {ax, ay};
	q.b = {bx, by};
	q.c = {cx, cy};
	q.d = {dx, dy};
	return q;
}

int main()
{
	// Brief section 15 quads.
	const std::vector<Quad> quads = {
		Q(0, 0, 1, 0, 1, 1, 0, 1),
		Q(.2, 0, .8, 0, 1, 1, 0, 1),
		Q(-.3, .2, 1.1, -.1, .9, 1.2, .1, .8),
		Q(0, 0, 1, .2, .9, 1.2, -.1, 1),
	};

	// 1. Distinguishing fixture (brief 4.5).
	{
		const Quad &t = quads[1];
		const Vec2 cb = forward_bilinear(t, .5, .5);
		Mat3 h;
		CHECK(solve_homography(t, h), "homography trapezoid");
		const Vec2 cp = project(h, .5, .5);
		std::printf("center fixture: bilinear (%.6f, %.6f)  projective (%.6f, %.6f)\n", cb.x, cb.y, cp.x, cp.y);
		CHECK(std::fabs(cb.x - .5) < 1e-12 && std::fabs(cb.y - .5) < 1e-12, "bilinear centre");
		CHECK(std::fabs(cp.x - .5) < 1e-12 && std::fabs(cp.y - .375) < 1e-12, "projective centre");
	}

	// 2. Double-precision round trips, 4000 interior samples.
	std::mt19937_64 rng(7);
	std::uniform_real_distribution<double> uni(0.0, 1.0);
	double max_bil = 0, max_hom = 0;
	for (const Quad &q : quads) {
		Mat3 h, hi;
		CHECK(solve_homography(q, h) && invert3(h, hi), "homography solve");
		for (int i = 0; i < 1000; i++) {
			const double u = uni(rng), v = uni(rng);
			double ru, rv;
			const Vec2 p = forward_bilinear(q, u, v);
			CHECK(inverse_bilinear(q, p, ru, rv), "inverse_bilinear found no root");
			max_bil = std::fmax(max_bil, std::fmax(std::fabs(ru - u), std::fabs(rv - v)));
			const Vec2 pp = project(h, u, v);
			const Vec2 back = project(hi, pp.x, pp.y);
			max_hom = std::fmax(max_hom, std::fmax(std::fabs(back.x - u), std::fabs(back.y - v)));
		}
	}
	std::printf("double: inverse bilinear max err %.3g, homography round trip max err %.3g\n", max_bil, max_hom);
	CHECK(max_bil < 1e-10 && max_hom < 1e-10, "double round trips");

	// 3. Float shader mirrors vs double reference, in 4K source pixels.
	const double W = 3840, H = 2160;
	double max_px_bil = 0, max_px_proj = 0;
	int miss_bil = 0, miss_proj = 0, n = 0;
	std::vector<Quad> fquads = quads;
	fquads.push_back(Q(0.1, 0, 1.1, 0, 0.9, 1, -0.1, 1.000001)); // near-parallelogram (g ~ 1e-6)
	fquads.push_back(Q(0.1, 0, 1.1, 0, 0.9, 1, -0.1, 1.0001));   // near-parallelogram (g ~ 1e-4)
	fquads.push_back(Q(0.45, 0, 0.55, 0, 1, 1, 0, 1));           // strong keystone
	fquads.push_back(Q(0, 0, 1, 0.45, 1, 0.55, 0, 1));           // strong keystone, rotated
	for (size_t qi = 0; qi < fquads.size(); qi++) {
		const Quad &q = fquads[qi];
		const BilinearUniforms bu = bilinear_uniforms(q);
		ProjectiveUniforms pu;
		CHECK(projective_uniforms(q, pu), "projective uniforms");
		Mat3 h;
		solve_homography(q, h);
		for (int i = 0; i < 50000; i++) {
			// keep 0.5% away from edges (edges are tested separately)
			const double u = 0.005 + 0.99 * uni(rng), v = 0.005 + 0.99 * uni(rng);
			const Vec2 pb = forward_bilinear(q, u, v);
			const auto rb = shader_inverse_bilinear(bu, (float)pb.x, (float)pb.y);
			if (rb[2] != 1)
				miss_bil++;
			else
				max_px_bil = std::fmax(max_px_bil,
						       std::fmax(std::fabs(rb[0] - u) * W, std::fabs(rb[1] - v) * H));
			const Vec2 pp = project(h, u, v);
			const auto rp = shader_inverse_projective(pu, (float)pp.x, (float)pp.y);
			if (rp[2] != 1)
				miss_proj++;
			else
				max_px_proj = std::fmax(max_px_proj,
							std::fmax(std::fabs(rp[0] - u) * W, std::fabs(rp[1] - v) * H));
			n++;
		}
	}
	std::printf("float32 shader mirror over %d interior samples at 3840x2160: bilinear max %.4f px (%d misses), "
		    "projective max %.4f px (%d misses)\n",
		    n, max_px_bil, miss_bil, max_px_proj, miss_proj);
	CHECK(miss_bil == 0 && miss_proj == 0, "interior points rejected by shader mirror");
	CHECK(max_px_bil < 0.05 && max_px_proj < 0.05, "float precision budget (0.05 px; brief target 0.01 px)");

	// 4. Outside points are transparent for both models.
	{
		const Quad &t = quads[1];
		const BilinearUniforms bu = bilinear_uniforms(t);
		ProjectiveUniforms pu;
		projective_uniforms(t, pu);
		const float outside[][2] = {{0.05f, 0.05f}, {0.95f, 0.02f}, {-0.1f, 0.5f}, {0.5f, 1.1f}, {1.2f, 0.5f}};
		for (auto &p : outside) {
			CHECK(shader_inverse_bilinear(bu, p[0], p[1])[2] == 0, "bilinear outside (%g,%g) accepted",
			      p[0], p[1]);
			CHECK(shader_inverse_projective(pu, p[0], p[1])[2] == 0, "projective outside (%g,%g) accepted",
			      p[0], p[1]);
		}
		double u, v;
		CHECK(!inverse_bilinear(t, {0.05, 0.05}, u, v), "double bilinear outside accepted");
	}

	// 5. Identity: shader mirrors return the pixel's own coordinates.
	{
		const BilinearUniforms bu = bilinear_uniforms(quads[0]);
		ProjectiveUniforms pu;
		projective_uniforms(quads[0], pu);
		float worst = 0;
		for (int y = 0; y < 2160; y += 7)
			for (int x = 0; x < 3840; x += 7) {
				const float px = (x + 0.5f) / 3840.0f, py = (y + 0.5f) / 2160.0f;
				const auto rb = shader_inverse_bilinear(bu, px, py);
				const auto rp = shader_inverse_projective(pu, px, py);
				worst = std::fmax(worst, std::fmax(std::fabs(rb[0] - px), std::fabs(rb[1] - py)));
				worst = std::fmax(worst, std::fmax(std::fabs(rp[0] - px), std::fabs(rp[1] - py)));
				if (rb[2] != 1 || rp[2] != 1)
					failures++;
			}
		std::printf("identity: max |uv - pixel| %.3g (normalised)\n", worst);
		CHECK(worst < 1e-6f, "identity not exact enough");
	}

	// 6. Validation cases (brief 4.6, 11.7).
	struct Case {
		const char *name;
		Quad q;
		bool ok;
	};
	const Case cases[] = {
		{"identity", quads[0], true},
		{"trapezoid", quads[1], true},
		{"brief quad 3", quads[2], true},
		{"brief quad 4", quads[3], true},
		{"mirrored", Q(1, 0, 0, 0, 0, 1, 1, 1), true},
		{"offscreen corners", Q(-0.5, -0.4, 1.6, -0.3, 1.4, 1.5, -0.6, 1.3), true},
		{"parallelogram", Q(0.1, 0, 1.1, 0, 0.9, 1, -0.1, 1), true},
		{"near-parallelogram", Q(0.1, 0, 1.1, 0, 0.9, 1, -0.1, 1.000001), true},
		{"crossed (bow-tie)", Q(0, 0, 1, 0, 0, 1, 1, 1), false},
		{"duplicate corner", Q(0, 0, 0, 0, 1, 1, 0, 1), false},
		{"concave", Q(0, 0, 1, 0, 0.3, 0.3, 0, 1), false},
		{"very thin", Q(0, 0, 1, 0, 1, 0.00005, 0, 0.00005), false},
		{"collinear", Q(0, 0, 0.5, 0.5, 1, 1, 0.2, 0.2), false},
		{"out of range", Q(0, 0, 6, 0, 1, 1, 0, 1), false},
		{"NaN", Q(0, 0, NAN, 0, 1, 1, 0, 1), false},
	};
	for (const Case &c : cases) {
		const Validation vb = validate(c.q, false);
		const Validation vp = validate(c.q, true);
		std::printf("validate %-20s bilinear=%s projective=%s %s\n", c.name, vb.ok ? "ok" : "REJECT",
			    vp.ok ? "ok" : "REJECT", vp.ok ? "" : vp.reason.c_str());
		CHECK(vb.ok == c.ok, "bilinear validation of %s", c.name);
		CHECK(vp.ok == c.ok, "projective validation of %s", c.name);
	}

	// 7. Pre-warp: rotation then barrel / pincushion (schema 2).
	{
		PreWarp id;
		CHECK(prewarp_identity(id) && prewarp_uniforms(id, 3840, 2160).on == 0.0f, "pre-warp identity");
		double su, sv;
		// 90 degrees clockwise, square frame: the output's right shows the source's top
		PreWarp r90;
		r90.rotation_deg = 90;
		r90.autofill = false;
		CHECK(prewarp_sample(r90, 1000, 1000, 0.75, 0.5, su, sv) && std::fabs(su - 0.5) < 1e-12 &&
			      std::fabs(sv - 0.25) < 1e-12,
		      "rotation direction (%.6f, %.6f)", su, sv);
		// aspect-correct: pixel distances from the centre are preserved on 16:9
		PreWarp r30;
		r30.rotation_deg = 30;
		r30.autofill = false;
		double worst_len = 0;
		for (int i = 0; i < 200; i++) {
			const double u = 0.3 + 0.4 * uni(rng), v = 0.3 + 0.4 * uni(rng);
			if (!prewarp_sample(r30, 3840, 2160, u, v, su, sv))
				continue;
			const double a = std::hypot((u - 0.5) * 3840, (v - 0.5) * 2160);
			const double b = std::hypot((su - 0.5) * 3840, (sv - 0.5) * 2160);
			worst_len = std::fmax(worst_len, std::fabs(a - b));
		}
		CHECK(worst_len < 1e-9, "rotation not isotropic in pixels (%.3g px)", worst_len);
		// barrel (+): sampling radius grows faster than linear; axis points stay on the axis
		PreWarp bar;
		bar.distortion = 100;
		bar.autofill = false;
		double prev = 0;
		bool mono = true;
		for (int i = 1; i <= 50; i++) {
			const double u = 0.5 + 0.5 * i / 50.0;
			prewarp_sample(bar, 3840, 2160, u, 0.5, su, sv);
			const double ratio = (su - 0.5) / (u - 0.5);
			mono = mono && ratio > prev && ratio >= 1.0 && std::fabs(sv - 0.5) < 1e-12;
			prev = ratio;
		}
		CHECK(mono, "barrel map not monotonic outward");
		// pincushion at -100 stays monotonic (k = -0.3 > -1/3)
		PreWarp pin;
		pin.distortion = -100;
		pin.autofill = false;
		double prev_r = -1;
		bool mono2 = true;
		for (int i = 0; i <= 200; i++) {
			const double t = i / 200.0;
			prewarp_sample(pin, 3840, 2160, 0.5 + 0.5 * t, 0.5 + 0.5 * t, su, sv);
			const double r = std::hypot((su - 0.5) * 3840, (sv - 0.5) * 2160);
			mono2 = mono2 && r > prev_r;
			prev_r = r;
		}
		CHECK(mono2, "pincushion fold");
		// auto-fill: every boundary point samples inside, and the scale is tight
		const PreWarp fills[] = {{10, 0, true},   {-45, 0, true},   {0, 100, true},
					 {0, -100, true}, {7.5, -40, true}, {180, 60, true}};
		for (const PreWarp &f : fills) {
			const double s = prewarp_fill_scale(f, 3840, 2160);
			bool inside = true;
			double margin = 1e9;
			for (int i = 0; i <= 2000; i++) {
				const double t = i / 2000.0;
				const double pts[4][2] = {{t, 0}, {1, t}, {1 - t, 1}, {0, 1 - t}};
				for (const auto &pt : pts) {
					inside = inside && prewarp_sample(f, 3840, 2160, pt[0], pt[1], su, sv);
					margin = std::fmin(margin,
							   std::fmin(std::fmin(su, 1 - su), std::fmin(sv, 1 - sv)));
				}
			}
			std::printf("pre-warp fill rot %.1f dist %.0f: scale %.4f, min boundary margin %.2g\n",
				    f.rotation_deg, f.distortion, s, margin);
			CHECK(inside && margin < 1e-4, "fill scale not tight/covering for rot %.1f dist %.0f",
			      f.rotation_deg, f.distortion);
		}
		// float mirror vs double, in 4K source pixels
		const PreWarp mix{-12.3, 55, true};
		const PreWarpUniforms pu = prewarp_uniforms(mix, 3840, 2160);
		double worst = 0;
		int agree = 0, n = 0;
		for (int i = 0; i < 20000; i++) {
			const double u = uni(rng), v = uni(rng);
			const bool okd = prewarp_sample(mix, 3840, 2160, u, v, su, sv);
			const auto rf = shader_prewarp(pu, (float)u, (float)v);
			n++;
			if (okd != (rf[2] == 1.0f))
				continue; // only at the boundary within tolerance
			agree++;
			if (okd)
				worst = std::fmax(worst, std::fmax(std::fabs(rf[0] - su) * 3840,
								   std::fabs(rf[1] - sv) * 2160));
		}
		std::printf("pre-warp float mirror: max error %.4f source px, validity agreement %d / %d\n", worst,
			    agree, n);
		CHECK(worst < 0.01 && agree >= n - 5, "pre-warp float mirror");
		PreWarp bad{NAN, 1e9, true};
		sanitize(bad);
		CHECK(bad.rotation_deg == 0 && bad.distortion == kDistortionMax, "pre-warp sanitize");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
