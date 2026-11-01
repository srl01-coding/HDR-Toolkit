# Validation report

Status values: **PASS** / **FAIL** / **NOT RUN**. A build with gates NOT RUN is a
development build, not a verified release (brief 11.9). Record environment and
evidence for every result; never infer a result from a different layer.

## Environment

| Item | Value |
|---|---|
| Authoring/CI environment | Linux container (no GPU, no OBS) + GitHub Actions (template workflows) |
| Build SDK | OBS 31.1.1 (template buildspec) |
| User's OBS version / OS / GPU / backend | NOT RECORDED - fill in when running OBS tests |
| Canvas colour space / format, SDR white, HDR nominal peak | Canvas base 2090x990 (from preview status bar); colour settings NOT RECORDED |
| Capture device, pixel format, source range, camera profile | NOT RECORDED |

## Layer 1 - CPU maths (run in authoring environment)

| ID | Test | Status | Evidence / measured |
|---|---|---|---|
| C1 | Brief section 15 `math_check.py` (geometry round trips, toe/shoulder, WB, luma preservation) | PASS | 2026-10-03, Python 3 + NumPy 2.4.4: bilinear inverse max err 6.7e-16 class, homography round trip 1.2e-15 class, shoulder knee slope 0.99999997, centres (0.5,0.5) vs (0.5,0.375), `all_checks_passed: true` |
| C2 | `test_pattern.cpp`: HLG OETF round trip 0-110% | PASS | max abs error 1.1e-16 |
| C3 | `test_pattern.cpp`: HLG level -> nits -> OBS encoder model -> level | PASS | error < 1e-12; 75% = 203.15 nits, 100% = 1000.00003 nits (BT.2100 constant rounding) |
| C4 | `test_pattern.cpp`: chart neutral patches exact, colour row luminance 100 nits, Rec.2020 primaries have negative Rec.709 components, alpha levels, invalid settings rejected | PASS | |

| C5 | `test_quad.cpp`: brief 4.5 centre fixture | PASS | bilinear (0.5, 0.5), projective (0.5, 0.375) |
| C6 | `test_quad.cpp`: double round trips, 4000 samples over brief quads | PASS | inverse bilinear 6.7e-16, homography 7.8e-16 |
| C7 | `test_quad.cpp`: float32 mirror of the shader vs double, 400,000 interior samples incl. near-parallelograms (g ~ 1e-6, 1e-4) and strong keystones, at 3840x2160 | PASS | bilinear max 0.0016 px, projective max 0.0028 px (brief target < 0.01 px); 0 interior misses |
| C8 | `test_quad.cpp`: outside points transparent (both models), identity exact | PASS | identity max error 0 |
| C9 | `test_quad.cpp`: validation - accepts identity, trapezoid, brief quads, mirrored, offscreen corners, parallelogram, near-parallelogram; rejects bow-tie, duplicate, concave, sliver, collinear, out of range, NaN | PASS | |
| C10 | `test_color.cpp`: WB(0,0) is the exact identity; 14 non-neutral WB matrices map D65 white to the brief's target white (1e-12) with white luminance preserved; + temperature warmer, - cooler, + tint magenta, - tint green | PASS | 2026-10-04: +30 mired white RGB (1.1120, 0.9857, 0.8115); tint +100 (1.1367, 0.9481, 1.1109) |
| C11 | `check_wb_vs_brief.py`: C++ WB matrices vs the brief's section 15 Python `wb_matrix`, mired -90..90 x tint -100/0/100 | PASS | 15 matrices, max abs difference 7.8e-16 |
| C12 | `test_color.cpp`: exposure exact (+1 EV doubles, -1 halves); contrast fixes the pivot, follows `pivot*(Y/pivot)^k` over 1e-4..1e4 nits, monotonic, black stays black, negative Y finite | PASS | |
| C13 | `test_color.cpp`: wheel hue directions luminance-orthogonal (0/120/240 deg = R/G/B); radius clamped at 1; wheel + saturation preserve linear Y; saturation 0 gives Y-neutral grey; black preserved by the full grade; Grade Mix 0 = original and linear | PASS | linear-Y error 6.1e-16 (5,000 random grades) |
| C14 | `test_color.cpp`: float32 mirror of `hdr-color.effect` vs double reference, 20,000 random grades x colours 0.01-10,000 nits | PASS | max error 3.6e-6 of the pixel's largest channel; neutral params bit-exact |
| C15 | `test_color.cpp`: zone windows continuous (max change 0.001 per 0.001 stop over -30..30), weights in [0,1], full strength and zeros where the schema-4 table puts them; Black and Dark cover Y = 0, negative, 1e-30 and 0.01 nits; Highlight and Specular cover up to 1.2e7 nits; adjacent pairs overlap >= 1.4 stops; Dark..Highlight weights sum to 1 (max deviation 0) and equal pushes on them equal a global push; brief stop markers | PASS | 2026-10-04 (schema 4, seven zones). Plot: `plot_zones.py` |
| C16 | `test_color.cpp`: isolated zone gain = 2^(w EV) at the input mask (pure RGB scale, all six zones); frozen mask (Shadow +3 does not pick up Light -3 after the lift); overlap sums EV; neutral or disabled zones (incl. a widened neutral Light) leave another zone's response bit-identical | PASS | gain law error 0 |
| C17 | `test_color.cpp`: zone saturation 0 desaturates fully at full weight and leaves out-of-window pixels bit-identical; zone wheel preserves Y and black; disabled zone keeps its value and is neutral; default zones enable no stage | PASS | |
| C18 | `test_color.cpp`: float mirror of the zone shader vs double, 20,000 random global + six-zone grades with random open ends; edge sanitising (reorder, NaN reset, fixed open ends cannot close, an open side's unused edges never override a used edge, re-closing gives a valid window) | PASS | max error 6.0e-6 of the pixel's largest channel; all finite |
| C19 | `test_color.cpp`: tonal order kept for a push of 0.98 x falloff/1.5 and reversed at 1.1 x, every default zone, both directions | PASS | |
| C20 | `test_color.cpp`: toe and shoulder reproduce the brief 8.2 / 8.3 tables; toe F(0)=0, F(L)=L, slope 1 at L, monotonic, never lifts, beta 0 exact identity (several L, beta); shoulder slope 1 at H, monotonic, stays below P over 0.01..1e7 nits (q 0.01..0.95); q = 0 hard cap | PASS | 2026-10-04; `math_check.py` toe/shoulder samples also pass |
| C21 | `test_color.cpp` in the grade: toe/shoulder as a common RGB gain (ratios kept), Y <= 0 and black untouched, middle region bit-identical, clips still act at Grade Mix 0, enabled clips are not "neutral"; offset added exactly to every channel incl. black, negative kept, mixed by Grade Mix; offset + toe; L > H conflict detected | PASS | |
| C22 | `test_color.cpp`: float mirror vs double with random offset, clips (incl. q = 0), mix and a zone | PASS | max error 2.0e-6 |
| C23 | `test_hqdn3d.cpp`: strength 0 and g = 0 are exact identities (double and float mirror); black stays black, constant stays constant, neutral stays neutral; luma and each chroma component never overshoot current/history; finite for negative components and 60,000 nits; a 3-stop change passes untouched | PASS | 2026-10-04 |
| C24 | `test_hqdn3d.cpp`: static noisy field (0.4 % noise, strength 6): output/input noise std 0.300 (first-order IIR limit for beta 0.9: 0.229); float mirror vs double max error 1.1e-6 | PASS | |
| C25 | `test_hqdn3d.cpp`: frame metric float mirror vs double on 100x37, 1000x563, 17x1, 3840x270 (< 1e-5); 1-stop uniform change reads 1.000; identical frames 0 | PASS | |
| C26 | `test_hqdn3d.cpp`: cut sequence (30 frames A, then B): exactly one reset, first B frame equals B exactly (metric 1.50 vs threshold 0.30); 30-frame dissolve A -> A + 0.6 stop: no reset, mean luma lag 0.0036 with transition protection vs 0.0074 without | PASS | protection ramp/floor rate tightened after the first run showed equal lag with and without |
| C27 | `tools/check-effect-hlsl.py` (DXC, ps_6_0) on `hdr-program-denoise.effect`: all six pixel shaders compile | PASS | pre-flight only; the OBS log is the real gate |
| C28 | `test_spatial.cpp`: spatial strength 0 is an exact identity for B (double and float mirror) and A, incl. negative, 60,000-nit and alpha < 1 pixels; black stays black, constant stays constant (1e-12), neutral stays exactly neutral (A and B) | PASS | 2026-10-04 |
| C29 | `test_spatial.cpp`: no overshoot - one pass of B (window R) and A (whole line) keeps Y and each chroma component inside the input range | PASS | |
| C30 | `test_spatial.cpp`: mirror symmetry f(flip(x)) = flip(f(x)) in x and y | PASS | B 1.7e-14 (summation order), A 0 (exact by construction) |
| C31 | `test_spatial.cpp`: B float mirror of SpatialH/SpatialV vs double, R = 6/8/12 | PASS | max rel error 1.4e-5 / 2.5e-5 / 2.9e-5 |
| C32 | `test_spatial.cpp` synthetic gates (decision section 14), 2 % per-channel Gaussian noise, 18 and 203 nits, strengths solved for sigma_out/sigma_in = 0.70 on luma and on chroma separately; steps 18->203 and 203->1000, rising/falling x vertical/horizontal | PASS | matched S_L / S_C: B R6 3.24/6.28, B R8 3.11/6.02, B R12 2.98/5.75, A 4.85/9.26 (18 nits; 203 nits within 0.1). All four: 10-90 % width increase 0.00 px, overshoot <= 0.22 %, undershoot <= 0.04 % (input noise level), directional width spread 0.00 px. Full table: `docs/HQDN3D_DESIGN.md` 8.6 |
| C33 | `tools/check-effect-hlsl.py` on `hdr-program-denoise.effect` (eight pixel shaders incl. SpatialH/V and the Exact view); DXC cs_6_0 on the embedded compute identity shader; Windows compile of `d3d11-compute.cpp` and `program-denoise.cpp` (zig, x86_64-windows-gnu) | PASS | pre-flight only; the OBS log and the user's machine are the real gates |
| C34 | `test_nlm.cpp`: offset-major NLMeans equals the naive reference (18 cases: P 0-2, all three history policies, chroma term on/off, odd sizes); S = 0 exact identity; isolated impulse untouched; constant / black / neutral; convex bounds over all candidates; HDR stress (negative, 12,000 nits) not clamped; cut (g = 0) uses no previous-frame candidate; temporal share capped at 0.9 for policies B/H (cap exercised); spatial mirror symmetry | PASS | 2026-10-05: naive vs offset 1.9e-14; mirror 1e-14 |
| C35 | `tools/denoise-sim` synthetic premium-temporal comparison (encoded and white noise models; static, moving object 0.5-16 px/frame, disocclusion, pan, cut) | RUN (informative, not a gate) | results and reading: `docs/denoise/MOTION_AWARE_TEMPORAL.md` section 2-3 |
| C36 | `tools/denoise-sim/real_eval.py` on six crops of the 4 Oct sample clip (second-generation: encoder residue, not camera noise) | RUN (informative) | `docs/denoise/MOTION_AWARE_TEMPORAL.md` section 6 |
| C37 | `tools/denoise-sim/codec_test.py`: x265 Main10 HLG at 0.07 / 0.035 bpp vs the clean reference | RUN (informative) | `docs/denoise/MOTION_AWARE_TEMPORAL.md` section 6 |
| C38 | `test_profile.cpp`: noise profile T(Y): identity exactly 1 (also all-ones anchors); measured profile anchors, clamp, constant ends, log-linear midpoint 1.15, monotone; float mirror 4.6e-7; sanitize; temporal with the profile: darks (2.27 nits, 5%) use history more in 89% of pixels, reference level unchanged in 99%; temporal/spatial float mirrors with the profile 1.2e-6 / 8e-7; NLM naive = offset with the profile; all earlier CPU tests unchanged with the identity profile | PASS | 2026-10-05 |

## Layer 1b - build

| ID | Test | Status | Evidence |
|---|---|---|---|
| B1 | Sources compile against OBS 32.2.2 headers (`g++ -fsyntax-only -Wall -Wextra`) | PASS | no diagnostics (re-run 2026-10-04 with denoise P2 sources) |
| B2 | CI Windows x64 build (OBS 31.1.1 SDK) | PASS | live noise measurement: run 38041046450, commit 91fd2e4fc, artifact `obs-hdr-toolkit-0.1.0-windows-x64-91fd2e4fc` (earlier: 37587479465, 37301445346, 37253078785, 37247021354, 37207465595, 37203845024, 37202766610, 37193906411, 37192864726, 37168988698, 37168271759, 37166894772, 37165577267) |
| B3 | CI macOS / Ubuntu builds, clang-format 19 + gersemi checks | PASS | same run; builds are not a macOS/Linux support claim (nothing run there) |

## Layer 2 - OBS / GPU (requires the user's machine)

Setup for all: a **test scene collection/profile**, not production. Canvas
Rec.2100 HLG, record SDR white W; HDR nominal peak 1000 nits (expected HLG values below
assume a peak <= 1000, OBS's x10 normalisation path). Source: `HDR Toolkit: Test
Pattern (developer)`, Chart, 1920x1080. Scope: srl01-coding/obs-color-monitor
`hdr-hlg` build (HLG % and 10-bit code scales), target = the pattern scene.

| ID | Test | Expected | Status |
|---|---|---|---|
| G0 | Plugin loads; log shows `plugin loaded (version 0.1.0, built against libobs 31.1.1, running on <ver>)`; no `failed to compile` error | load OK | PASS (2026-10-04, user: OBS 32.2.2 Windows; filter and source both listed and rendering; log line not yet captured) |
| P1 | Pattern alone, mode *HLG levels only*: reads 0,10,...,100% on the lines, 105% at ~984, 109% at ~1019 | exact to scope resolution; proves values above 940 reach the scope without the input-decoder clamp | NOT RUN |
| P2 | Pattern alone: neutral 203-nit patch at 75% only when W = 203; other W moves it (W-relative storage, absolute nits meaning) | consistent with W | NOT RUN |
| N1 | Pattern -> Neutral (force render ON, gain 0): scope identical to P1 (every bar, ramp shape, 105/109% bars) | no change. Note: user saw patch-border differences when the item was scaled - neutral effect used a point sampler for the final (scaled) draw; changed to linear in P1 build, re-check | PASS (qualitative, 2026-10-04: Chart mode, filter off vs on at 0 EV, docked scope after the obs-color-monitor minification fix; traces visually identical. Exact per-level reading in *HLG levels only* mode still to do) |
| N2 | Same with force render OFF (skip path) | identical to N1 | NOT RUN |
| N3 | Neutral with test gain +1 EV (x2 nits) on the HLG row: 50% -> 63.2% (code ~618), 60% -> 72.0% (~695), 70% -> 81.3% (~776), 75% -> 86.1% (~818), 80% -> 90.9% (~860), 90% -> 100.7% (~946); 100/105/109% bars all at code 1023 (OBS HLG encode ceiling, ~110%) | shader live, no clamp at SDR white (a clamp at 1.0 would pile everything >= 75%-at-W=203 onto one level) | NOT RUN |
| N3b | Neutral with negative test gain on the Chart: brightest patch (10,000 nits) brought down to the HLG ceiling. Model: code 1023 (E' = 1.0947) = 1866 nits, so 10,000 nits reaches it at -2.42 EV | consistent with no clamp at SDR white: a clamp at linear 1.0 would cap the chart at 75% at 0 EV and lower for any negative gain | PASS (observational, 2026-10-04: user reports about -2.3 to -2.35 EV puts the top at 109%; within reading precision of -2.42) |
| N4 | Two Neutral instances stacked: identical to N1 (no progressive tone mapping) | | NOT RUN |
| N5 | SDR preview of the HDR chain looks tone-mapped (expected), while the scope (HDR) is unchanged | boundary conversion only | NOT RUN |
| N6 | Alpha row over a bright background in a scene: no dark/bright fringes, alpha 0 patch invisible, 0.0001 patch invisible to the eye | | NOT RUN |
| N7 | Neutral on an SDR source (e.g. image) on HLG canvas: unchanged vs without filter | | NOT RUN |
| N8 | Log: one `[neutral] ... input=... consumer=...` line per change, not per frame | | NOT RUN |

### P1 - HDR Transform (Corner Pin)

Setup as above; pattern source with `HDR Toolkit: Neutral` removed, `HDR Transform` added.

| ID | Test | Expected | Status |
|---|---|---|---|
| T1 | Identity, *Force shader render* ON vs filter disabled, pattern *HLG levels only* | identical scope (levels and order) | NOT RUN |
| T2 | Moderate warp (TL 10%,5% / TR 90%,0% / BR 100%,100% / BL 0%,95%), both warp models, *HLG levels only* | every level still present at the same heights (geometry moves columns, never levels); 105%/109% bars intact; only patch-border pixels show interpolated in-between values with bilinear sampling, none with point | NOT RUN |
| T3 | Brief 4.5 trapezoid TL 20%,0 / TR 80%,0 / BR 100%,100% / BL 0%,100% on the Chart | row boundaries (source v = 0.18 / 0.32 / 0.48 / 0.64 / 0.80) at y = 18 / 32 / 48 / 64 / 80% with Bilinear, and 11.6 / 22.0 / 35.6 / 51.6 / 70.6% with Projective | NOT RUN |
| T4 | Drag TL past TR (bow-tie), and TL onto TR | status "Geometry INVALID ..." with reason; image stays on the last valid shape; typed numbers not reset; fixing them recovers | NOT RUN |
| T5 | Warped chart over a bright coloured background source | outside the quad shows the background; alpha row correct; no dark or bright fringes on edges | NOT RUN |
| T6 | Corners outside the canvas (e.g. TL -20%,-20%) | image clipped at the output rectangle, no smearing of edge pixels | NOT RUN |
| T7 | Transform on an SDR image source on the HLG canvas | warps correctly, colours unchanged vs no filter | NOT RUN |
| T8 | Log: one `[transform] ... input=...` line per change; invalid geometry logs a warning | | NOT RUN |
| T9 | Pre-warp identity: existing scene with a corner pin (schema 1 settings), new build | image unchanged; rotation 0 / distortion 0 in the UI; with *Force shader render* the scope is identical to T1 | NOT RUN |
| T10 | Rotation 5 deg, auto-fill off, then on, on the Chart | clockwise turn about the centre; off: transparent wedges in the corners; on: wedges gone (log: fill scale about 0.87 at 16:9); lines stay straight; levels unchanged (T2 criteria) | NOT RUN |
| T11 | Distortion +50 then -50, auto-fill on, on a grid / Chart | + bows straight lines outward (barrel), - inward (pincushion); centre unchanged; no empty edges; levels unchanged | NOT RUN |
| T12 | Pre-warp + corner pin: rotation -3, distortion -20, trapezoid corners | corners place the rotated/corrected image (the pin acts on the pre-warped picture); invalid-quad handling as T4 still works | NOT RUN |


### P2 - HDR Color (global stages)

Setup as above; pattern source with any other toolkit filter removed, `HDR Color`
added. HLG figures below are for a 1000-nit nominal peak and independent of W
(the pattern row is generated in absolute nits from the OBS HLG model). Use the
waveform in RGB parade for H5. Settle sliders by typing values.

| ID | Test | Expected | Status |
|---|---|---|---|
| H1 | New instance: every control at its default; *HLG levels only* with **Force shader render** ON vs filter disabled | identical scope (all levels incl. 105/109%); proves the neutral grade is a real shader identity | NOT RUN |
| H2 | Exposure +1 EV, *HLG levels only* | same readings as N3: 50% -> 63.2% (~618), 70% -> 81.3% (~776), 75% -> 86.1% (~818), 90% -> 100.7% (~946), 100/105/109% at 1023 | NOT RUN |
| H3 | Contrast 1.5, pivot 203.2 nits, *HLG levels only* | 75% bar fixed at 75% (721); 50% -> 37.4% (392), 60% -> 51.6% (516), 70% -> 67.4% (655), 80% -> 82.5% (786), 90% -> 97.3% (916), 100% and above at 1023 | NOT RUN |
| H4 | Contrast 0.75, pivot 203.2 | 75% fixed; 10% -> 17.3% (215), 50% -> 56.9% (562), 90% -> 86.3% (820), 100% -> 93.9% (886) | NOT RUN |
| H5 | White balance on the 75% bar (RGB parade): temperature +30 / -30 mired; tint +100 / -100 | +30: R 76.7 / G 74.8 / B 71.5%; -30: R 73.2 / G 75.1 / B 78.2%; tint +100: R 77.1 / G 74.1 / B 76.7%; tint -100: R 72.7 / G 75.8 / B 73.1%. Visually: + warmer, + tint magenta | NOT RUN |
| H6 | Saturation 0 on the Chart | colour row (all patches 100 nits luminance) becomes one grey level at 63.0% (~616); neutral rows unchanged | NOT RUN |
| H7 | Saturation 1.5 and wheel (0.5, 0) on the Chart, Rec.2020 primaries row | no black, white, flashing or NaN pixels; colours shift as expected. (Linear-Y preservation is a CPU test, C13: the scope's luma is non-constant-luminance Y' of HLG-encoded R'G'B', so it legitimately moves with saturation) | NOT RUN |
| H8 | Exposure +1 EV with Grade mix 0.5 | x1.5 nits: 50% -> 58.0% (572), 75% -> 81.5% (778), 90% -> 96.3% (907). Grade mix 0 = identical to H1 | NOT RUN |
| H9 | Set non-default values in every control, save, restart OBS (test collection) | all values restored; scope identical before/after restart; *Reset grade* returns all grade controls (not Advanced) to defaults | NOT RUN |
| H10 | HDR Color on an SDR image source on the HLG canvas, exposure +1 EV | brightens; highlights clip at SDR white (75% at W = 203) because an SDR source stays in its native SDR space (brief 5.1: no hidden SDR -> HDR promotion) | NOT RUN |
| H11 | Log: one `[color] ... input=...` line per context change; out-of-range values in a hand-edited scene file log `(clamped)` | | NOT RUN |


### P3 - HDR Color tonal zones

Setup as P2. Figures for gray reference 18 nits, 1000-nit peak, schema-4 defaults
(seven zones, handovers at -4 / -1 / +1 / +3 stops, Specular full from +5).

| ID | Test | Expected | Status |
|---|---|---|---|
| Z1 | Defaults with Force shader render ON vs filter off (*HLG levels only*) | identical (zone stage inactive when every zone is neutral) | NOT RUN |
| Z2 | Diagnostic *All zones*, *HLG levels only* | 0% Black + Dark; 5% mostly Black + Dark; 10% Dark/Shadow half each; 20% Shadow; 30% Midtones (grey); 40% Midtones + some Light; 50-60% Light (yellow); 70% Light/Highlight; 75% Highlight + a little Specular; 90% and up Highlight + Specular | NOT RUN |
| Z3 | Diagnostic *Zone mask*, Midtones (W = 203) | 30% bar reads 75% (w 1); 40% bar 70.6% (w 0.77); 50% bar 30.7% (w 0.08); 20% bar 11% (w 0.01); 10% at 0 | NOT RUN |
| Z4 | Midtones +1 EV | 30% -> 40.0% (415); 40% -> 49.9% (501); 50% -> 51.1% (512); all other bars unchanged | NOT RUN |
| Z5 | Shadow +1 EV | 10% -> 11.4% (164); 20% -> 26.6% (297); others unchanged | NOT RUN |
| Z6 | Light +1 EV | 40% -> 42.8%; 50% -> 62.3% (609); 60% -> 72.0% (695); 70% -> 75.0% (721); 75% -> 75.9% (729). +1 EV is exactly the tonal-order limit of a 1.5-stop fade, so 70-75% nearly flatten | NOT RUN |
| Z7 | Highlight -1 EV | 70% -> 63.3% (618); 75% -> 64.2% (626); 80% -> 68.6%; 90% -> 79.1% (757); 100% -> 89.3% (847); 109% -> 98.5% (927) | NOT RUN |
| Z8 | Dark +1 EV | 0% stays 0 (gain, not lift); 5% -> 6.7%; 10% -> 11.8% (167) | NOT RUN |
| Z9 | Specular -1 EV | 75% -> 73.2%; 80% -> 74.9%; 90% -> 79.2%; 100% -> 89.3%; 109% -> 98.5% | NOT RUN |
| Z10 | Dark, Shadow, Midtones, Light, Highlight all +0.5 EV | identical to global exposure +0.5 EV (chain weights sum to 1) | NOT RUN |
| Z11 | Untick a zone with a non-zero setting | effect disappears, values kept; tick again restores | NOT RUN |
| Z12 | Dark: untick *Extend down to black* | fields switch to centre / width / both falloffs; mask gains a lower fade at -9..-7 stops; tick again restores the open end | NOT RUN |
| Z13 | Move Midtones centre / width / falloffs; then *Reset this zone* | mask moves accordingly; reset restores defaults and only that zone | NOT RUN |
| Z14 | Save, restart OBS | all zone values, enables and open flags restored; scope identical | NOT RUN |
| Z15 | Global exposure +1 EV with diagnostic *All zones* | colour bands shift one stop down the picture (masks follow global exposure by design) | NOT RUN |

### Offset and soft clip

| ID | Test | Expected | Status |
|---|---|---|---|
| S1 | Offset +0.05 nits, *HLG levels only* | 0% -> 2.8% (code 88); 5% -> 5.5% (112); 10% -> 10.2% (153); 20% and up within 0.1% | NOT RUN |
| S2 | Offset -0.05 nits | 0% and 2% bars at 0% (below black internally, clipped at encode); 3% -> 1.4%; 5% -> 4.4%; 10% -> 9.8% | NOT RUN |
| S3 | High soft clip on (peak 1000, knee 750) with exposure +1 EV | 75% -> 86.1% (818, below the knee: unchanged by the clip); 90% -> 98.1% (924); 100% -> 99.4% (934); 105% -> 99.6%; 109% -> 99.7% (937). Nothing reaches 100%: peak 1000 nits = HLG 100% is an asymptote | NOT RUN |
| S4 | High clip, peak 1000, knee 500, 0 EV | 80% unchanged; 100% -> 95.6% (901); 105% -> 97.0%; 109% -> 97.7% (920) | NOT RUN |
| S5 | High clip with knee typed equal to peak (1000) | hard cap: 100% and above all at 100% (940) | NOT RUN |
| S6 | Low soft clip on, knee 1 nit, strength 1 | 2% -> 0.55%; 5% -> 3.3%; 8% -> 7.5%; 10% and up unchanged; 0% stays 0% | NOT RUN |
| S7 | Grade Mix 0 with the high clip on (peak 1000, knee 750) and exposure +1 EV | exposure gone (75% and 90% back at 75% / 90%) but the clip still acts: 100% -> 98.0% (922); 105% -> 98.9%; 109% -> 99.3% | NOT RUN |
| S8 | Both clips on, low knee typed above the high knee | status line shows CONFLICT, picture keeps the previous curves, log warning; fixing the value clears it | NOT RUN |
| S9 | Save, restart | offset and clip settings restored | NOT RUN |


### Program denoise P0 (denoise brief v1.2)

Setup: test profile; HLG canvas as above. Program scene = `HDR Toolkit: Test Pattern
(developer)`, mode *HLG levels only*. Scope: obs-color-monitor waveform/histogram with
target **Program** (not *Main view*). Tools > *HDR Program Denoise...*: enable *Log frame
counters every 10 s*.

| ID | Test | Expected | Status |
|---|---|---|---|
| D-G0 | Plugin loads | log: `[denoise] program hook registered (once per unique main-canvas frame)`; Tools menu shows *HDR Program Denoise...*; no `[denoise] failed to compile` after choosing Identity; first Identity frame logs `[denoise] program frame 3840x2160 RGBA16F, canvas colour space Rec.2100 HLG, SDR white 203 nits, HDR nominal peak 1000 nits` | NOT RUN |
| D-G1a | Algorithm Off, 30 s, preview only | periodic lines: `unique_program_frames` rises ~300 per 10 s at 30 fps and tracks `obs_total_frames`; `denoise_dispatches=0` | NOT RUN |
| D-G1b | Algorithm Identity, preview + recording (+ streaming if possible), *Reset counters*, wait 30 s, *Log counters now* | `denoise_dispatches == unique_program_frames` (`dispatches == unique frames: OK`), `unique_program_frames` = `obs_total_frames` (+-1), `history_updates=0`; opening a projector / multiview does not change the rate | NOT RUN |
| D-G1c | As D-G1b with the recording set to *Rescale output* (a second mix) | `other_mix_skipped` now rises ~30/s; dispatches still == unique frames; the recording is denoise-processed (P0: identical) | NOT RUN |
| D-G2 | Identity vs Off on *HLG levels only* | identical scope: 0% 64, 10% 152, 20% 239, 30% 327, 40% 414, 50% 502, 60% 590, 70% 677, 75% 721, 80% 765, 90% 852, 100% 940, 105% ~984, 109% ~1019; no hue or alpha change, no new banding | QUALITATIVE (2026-10-04, user: Identity makes no visible change to the output; exact-value source and scope check not run) |
| D-G3 | Identity, Chart mode, Studio Mode: transition between the pattern scene and another scene (cut and fade) | Program monitor, scope and recording show the transition unchanged; no frame skipped or doubled (counter check as D-G1b) | NOT RUN |


### Program denoise P1 - HQDN3D-style temporal

Setup as P0. Tools > *HDR Program Denoise...*: Algorithm *HQDN3D-style (spatial +
temporal)* with spatial luma/chroma 0 (the default), defaults (temporal luma 4, chroma 6, cut reset on, sensitivity 50, protection on, 0.8),
*Log counters, GPU time and scene-change metric every 10 s* on.

| ID | Test | Expected | Status |
|---|---|---|---|
| H-G0 | Select HQDN3D-style | log: `[denoise] temporal resources created: history 2 x 3840x2160 RGBA16F + current frame copy, about 190 MiB`; no `[denoise] failed to compile` | NOT RUN |
| H-G3 | Temporal luma and chroma 0, *HLG levels only* | scope identical to Off (exact identity branch) | NOT RUN |
| H1 | Defaults, static *HLG levels only* | scope identical to Off at every bar (0% 64 ... 75% 721 ... 100% 940, 105% ~984, 109% ~1019): constant input stays constant; no drift over minutes | NOT RUN |
| H1b | Periodic log line | `dispatches == unique frames: OK`; `history_updates == denoise_dispatches`; one `history_resets` when the mode was selected; `gpu ms avg / p95 / max` recorded (P1 timing) | NOT RUN |
| H3 | Studio Mode, *Cut* transition between two different scenes, after >= 1 s on each; debug view *Scene-change metric bar* | bar turns red for the cut frame; `cut_resets` +1 per cut in the log; no ghost of the previous scene on the first frame (check *What was removed* view: the cut frame shows nothing removed) | NOT RUN |
| H4 | *Fade* transition, 1 s | bar amber during the fade, no red; `cut_resets` unchanged; no trail or lag at the end of the fade; picture settles without pumping | NOT RUN |
| H5 | Fine text / lower third / logo over camera | no smearing of moving graphics; static text unchanged | NOT RUN |
| H6 | Real camera noise (a6400/a6700), *What was removed* view x16 | grain-like residue only, no edges or detail; *History difference* view shows motion outlines, static areas dark | NOT RUN |
| H7 | Reset history button; change canvas resolution | `history_resets` +1 each; no frame from the old size | NOT RUN |

### Program denoise P2 - spatial B and the D3D11 compute spike

The algorithm is now called *HQDN3D-style (spatial + temporal)*. Spatial strengths
default to 0, so the P1 rows above are unchanged. Spatial strengths and the A/B choice
are **not** final until P1 has run on real footage (decision section 22). Timing is read
with debug view *Normal*, because the compare views add copies.

| ID | Test | Expected | Status |
|---|---|---|---|
| SP-G0 | HQDN3D-style, spatial luma 3 | log: `[denoise] spatial resources created: 2 x 3840x2160 RGBA16F, about 127 MiB`; no `[denoise] failed to compile` | NOT RUN |
| SP1 | All four strengths 0, Development > *Run spatial passes at strength 0* on, debug view *Exact change* | whole frame black (every value identical through the spatial and temporal shaders); periodic log shows `(forced)` and `spatial_passes` rising with dispatches | NOT RUN |
| SP2 | *HLG levels only*, spatial luma 6 / chroma 6, temporal 0 | scope identical to Off at every bar (flat fields stay flat; the bar edges are far above any threshold) | NOT RUN |
| SP3 | Timing: spatial 3/6 with temporal defaults, radius 6, 8 and 12 in turn (*Reset counters*, 30 s each) | `gpu ms ... p95` per radius against P1 alone (tiers in decision section 12: <= 8 ms, 8-15 ms, > 15 ms) | NOT RUN |
| SP4 | Real camera noise, *What was removed* x16, spatial luma 2-6 (temporal 0, then temporal on) | grain only, no edges, hair, fabric or text structure; record the strength where structure first appears | NOT RUN |
| SP5 | Spatial on, fast pan / motion | no smearing beyond the temporal behaviour seen in P1 | NOT RUN |
| CS-G0 | Algorithm *Compute identity (D3D11 spike)* (Windows, D3D11, HDR canvas) | log: `[denoise] D3D11 compute: private RGBA16F 3840x2160 x2 (about 127 MiB), driver command lists yes/no`; no compile error; no warning `compute identity unavailable` | NOT RUN |
| CS1 | Each variant in turn (Development > *Compute spike variant*), debug view *Exact change* | whole frame black for every variant (exact-value identity through the compute round trip) | NOT RUN |
| CS2 | Each variant, debug view *Normal*, counter logging on, 30 s after *Reset counters* | `gpu ms ... p95` per variant (gate: <= 1.0 ms preferred, <= 1.5 ms acceptable, at 4K on the 1650 Super); `dispatches == unique frames: OK`, `failures=0` | NOT RUN |
| CS3 | Compute identity on: Program monitor, preview, multiview, projector, recording and stream (or a local RTMP test) | all identical to Off; recording/stream frames neither duplicated nor stale (counters as D-G1b) | NOT RUN |
| CS4 | HDR Transform + HDR Color on the camera source, compute identity on | grade and transform unchanged versus Off (OBS graphics state not disturbed) | NOT RUN |
| CS5 | Change canvas resolution, then back; switch variants and algorithms repeatedly | resources recreated (log line each time), no black frame, no crash | NOT RUN |
| CS6 | Optional: D3D11 debug layer (Graphics Tools + `--debug`-style run) | no D3D11 errors or hazards from the spike | NOT RUN |
| NP1 | HQDN3D-style on, Development > *Noise profile* = Off, then = Measured, all strengths 0, *Run spatial passes at strength 0* on, debug view *Exact change* | black in both cases (the profile only scales thresholds; at S = 0 the shaders stay exact identities) | NOT RUN |
| NP2 | Temporal luma 4 / chroma 6, a scene with a dark area (piano / TV) and a mid wall; *What was removed* x16; noise profile Off vs Measured (max 3), then max 5 | with Measured, the dark area shows grain removed where Off removed nothing; the wall looks the same; no dark structure (edges, text) appears in the removed view; log line shows `noise_profile=measured-2026-10-05 (max 3.0)` | NOT RUN |
| PL-G1 | **Source HDR identity.** *HDR Denoise* on the pattern source (*HLG levels only*), all strengths 0, Development > *Run spatial passes at strength 0* on, debug view *Exact change* | whole source black (exact). Then debug view Normal: scope identical to the filter disabled at every bar (0% 64 ... 75% 721 ... 100% 940, 105% ~984, 109% ~1019); log `[denoise:<name>] ... staging RGBA16F` with the native space; no SDR conversion | NOT RUN |
| PL-G2 | **Program HDR identity** after the refactor: Tools > HDR Program Denoise, HQDN3D-style, strengths 0, forced spatial, *Exact change* | whole frame black; D-G2 scope readings unchanged | NOT RUN |
| PL-G3 | **Source continuity.** Filter on a camera in scene A only; switch the program to scene B for > 1 s (no preview/multiview showing A), then back | log `continuity_resets` +1; no ghost of the old frame on return (*What was removed* shows nothing structured on the first frame) | NOT RUN |
| PL-G4 | **Three-source temporal.** Filters on three cameras (temporal defaults), counter logging on | each filter's line: `dispatches == unique frames: OK`, `reused_same_frame` rises when preview + program + multiview show the same camera; histories independent (cover one camera: the others unaffected); no render lag | NOT RUN |
| PL-G5 | **Source + Color + Transform.** Chain Denoise -> HDR Color -> HDR Transform on a camera | HDR preserved (scope beyond 100% still present), grade and corner pin unchanged in behaviour, expected denoise | NOT RUN |
| PL-G6 | **Source vs program.** Same strengths: filter on the camera vs HDR Program Denoise | compare *What was removed* x64-x256: texture, transitions, graphics, perspective-scaled areas | NOT RUN |
| PL-G7 | **Hybrid.** Three cameras with *HDR Denoise* temporal only + HDR Program Denoise spatial only (temporal 0) | benchmark as PL-B; priority candidate for the GTX 1650 Super | NOT RUN |
| PL-G8 | **Spatial-only skip.** HDR Program Denoise temporal 0/0, spatial on, counter logging on; then set temporal > 0 | first: `temporal_skipped` rises with `denoise_dispatches`, output identical to before the change (*What was removed* unchanged), GPU ms lower than the same setting on bc7869b; then: `history_resets` +1 and normal temporal behaviour | NOT RUN |
| KN-1 | **Dark-area temporal via the comparison knee.** Camera scene with the black piano; temporal 8/8, spatial 0, identity profile; knee 0.1 then 8; views *Temporal weight* and *What was removed, noise-normalised* x64-x256; then a person moving in front of the piano | knee 8: piano/shadows light grey in the weight view (0.1: dark), noise-normalised removal similar in darks and mids; record any dark trailing on motion | NOT RUN |
| SR-1 | **Strength 0-100.** Temporal 100/100, then spatial 100/100, on the camera scene; counter logging on | log shows history RGBA32F above temporal 20 and a history restart when crossing 20; static areas plastic; trails on motion expected; no NaN/black frames; GPU ms recorded | NOT RUN |
| NM-1 | **Live noise measurement.** HDR Denoise first on the camera; static shot with piano and wall, auto exposure off; *Measure noise*; then *Apply*; repeat twice; repeat at a higher camera gain | log table monotone in sigma vs nits, flicker range < 2%, motion < 5%; the two repeats agree within ~20% on the knee; higher gain gives a higher sigma; after Apply, KN-1 weight view uniform on piano and wall; no lagged frames attributable to the measurement | NOT RUN |
| PL-B | **Benchmark matrix** (`PLACEMENT_UPDATE.md` section 12): 1 source T / S / T+S; 3 sources T / S / T+S; program T / S / T+S; 3 x source T + 1 x program S | per configuration: Task Manager GPU 3D %, plugin `gpu ms avg / p95 / max` (one line per filter + program), OBS *Frames missed due to rendering lag*, VRAM (log), multiview on/off, whether off-air sources executed (`unique_frames` rising), transition peak | NOT RUN |
| NV-G0 | Development > *Probe NVIDIA optical-flow hardware* | log lines `[nvof-probe] ...`: max API version, CUDA/D3D11/D3D12 entry points, and per device either `optical-flow engine available; output grids {...}` or `NOT available`; OBS keeps running normally (the probe uses its own CUDA context) | NOT RUN |
| CS7 | Renderer set to OpenGL (or SDR canvas) | log once: `compute identity unavailable (...): passing the program through`; picture unchanged | NOT RUN |

## Layer 3 - production path

| ID | Test | Status |
|---|---|---|
| E1 | Camera chain A/B with Neutral inserted at each position of the existing filter list (staircase from pattern where possible) | NOT RUN |
| E2 | Encoded HDR output metadata unchanged by plugin | NOT RUN |

## Diagnosis record (brief 11.5)

| Date | Chain | Observation | Classification |
|---|---|---|---|
| 2026-09-27 | 10-bit HLG Media Source -> obs-shaderfilter 2.6.0 (x4.17 linear gain) | all bars >= ~50% HLG identical, histogram spike at SDR white (~code 740 at W=203) | clipping at linear 1.0 inside/after obs-shaderfilter |
| 2026-09-27 | 10-bit HLG Media Source (Color Range Auto) | code 1019 bar identical to 940 | OBS input decoder limited-range clamp (`YUV_to_RGB`) |
