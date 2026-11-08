# Architecture

Status: P0 (HDR plumbing). Governing spec: *HDR Toolkit: implementation brief for
Claude Code*, v1.0, 3 Oct 2026 (referred to below as "the brief"; section numbers
refer to it).

## Product

One OBS plugin package (`hdr-toolkit`) with two production video filters,
sharing a small colour/rendering support layer:

| Filter | ID (stable) | Status |
|---|---|---|
| HDR Transform | `hdr_toolkit_transform_v1` | P1 Corner Pin (bilinear + projective) implemented; pre-warp rotation + barrel/pincushion before the corners (schema 2, user-directed); 3D modes, edge AA, mipmapping in P4 |
| HDR Color | `hdr_toolkit_color_v1` | P2 global stages (WB, exposure, contrast, wheel, saturation, Grade Mix) P3 six tonal zones (frozen masks, open ends, mask diagnostic), linear offset (user-directed) and low/high soft clip implemented; wheel UI and gamut containment in P5 |
| HDR Input | `hdr_toolkit_input_v1` | Reinterpret SDR-decoded HLG (DeckLink without HDR metadata) with OBS's own formulas; luma bit-depth analysis (user-directed, 7 Nov); docs/HDR_INPUT.md |
| HDR Sharpen | `hdr_toolkit_sharpen_v1` | Log-luma unsharp mask with halo limit and dark floor (user-directed, 7 Nov); docs/SHARPEN.md |
| SDR Conversion | `hdr_toolkit_sdr_v1` | HDR -> SDR (BT.2446-1 Method A / OBS default), output reported SRGB_16F so libobs never tone-maps on top; split compare (user-directed, 8 Nov); docs/SDR_CONVERSION.md |
| HDR Scale | `hdr_toolkit_scale_v1` | Size / desqueeze / crop / punch-in; separable PQ-domain enlarging with anti-ringing, linear-light reducing (user-directed, 8 Nov, 4560 x 2160 canvas); docs/HDR_SCALE.md |

Developer-only objects present in P0:

| Object | ID | Purpose |
|---|---|---|
| Neutral filter | `hdr_toolkit_neutral_dev` | Proves the HDR filter render path with a real shader (brief 3.5, P0 exit). |
| Test pattern source | `hdr_toolkit_pattern_dev` | Exact extended-linear values with no video decode (brief 11.3). |

Both production IDs are registered with real v1 schemas (`docs/PARAMETER_SCHEMA.md`).
Later packages add keys whose defaults reproduce v1 output exactly, so saved scenes
are unchanged by updates; anything else needs a schema version and migration.

## Pinned baseline

| Item | Value |
|---|---|
| Plugin template | `obsproject/obs-plugintemplate` @ `3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc` (2025-12-09) |
| Build SDK (CI) | OBS **31.1.1** sources + obs-deps 2025-07-11, as pinned with SHA-256 in `buildspec.json` by the template |
| Runtime target | OBS 32.x on Windows x64 (plugins built against an older libobs load in newer OBS; the reverse is not true) |
| Source-reading reference | OBS **32.2.2**, commit `ba2f32bdf791005443988a4955e963663e16b1ed` |
| StreamFX reference | `Vhonowslend/StreamFX-Public` @ `99352c80405a64693ca8c967fafabe141e768e57` (behaviour reference only; no code copied) |
| Language | C++17 (template default), libobs C API |

Every API used in P0 exists in both 31.1.1 and 32.2.2 (`obs_source_process_filter_begin_with_color_space`,
`video_get_color_space`, `obs_get_video_sdr_white_level`, `obs_get_video_hdr_nominal_peak_level`).
Source files were syntax-checked against the 32.2.2 headers; CI compiles against 31.1.1.

The user's installed OBS version is **not known to this repository**; it must be
recorded in `docs/VALIDATION.md` when tests are run (brief 11.2).

## Layout

```text
src/
  plugin-main.cpp                 module load/unload, registration
  shared/obs-color-context.*      space query, nits scale, change logging (brief 3.2-3.3)
  neutral/neutral-filter.cpp      P0 neutral HDR filter
  pattern/pattern-math.*          pure CPU pattern generation (unit-tested, no libobs)
  pattern/pattern-source.cpp      developer HDR pattern source
  transform/quad-math.*           corner-pin geometry: validation, inverse bilinear, homography (no libobs)
  transform/transform-filter.cpp  HDR Transform filter
  color/color-math.*              WB (Bradford), wheel, grade reference + float shader mirror (no libobs)
  color/color-filter.cpp          HDR Color filter
data/
  effects/hdr-neutral.effect
  effects/hdr-transform.effect
  effects/hdr-color.effect
  locale/en-US.ini
tests/cpu/
  math_check.py                   brief section 15, verbatim
  test_pattern.cpp                pattern + HLG model tests
  test_quad.cpp                   geometry, float shader mirror, validation
  test_color.cpp                  colour stages, float shader mirror
  check_wb_vs_brief.py            WB matrices vs the brief's Python reference
  plot_zones.py                   plot of the default zone windows (stops / nits / HLG %)
docs/                             this file, COLOR_PIPELINE, STREAMFX_REFERENCE, VALIDATION
```

The brief's suggested layout (10.5) is followed as modules are added
(`transform/`, `color/`, `ui/`, `tests/gpu/`).

## CPU / GPU split (brief 10.6)

CPU: settings validation, space decisions, unit constants, (later) geometry
solves and WB matrices. GPU: sampling, (un)premultiply, per-pixel maths.
Settings are copied under a mutex into an immutable per-frame snapshot; the
graphics thread never reads half-updated UI state. No per-frame heap
allocation, no CPU readback in the render path.

## Build and test

CI only (no local OBS/Windows toolchain in the authoring environment):
GitHub Actions from the template, triggered on push to `main`. Artifacts:
`hdr-toolkit-<version>-windows-x64*`. CPU tests run anywhere:

```sh
g++ -std=c++17 -O2 -Isrc tests/cpu/test_pattern.cpp src/pattern/pattern-math.cpp -o test_pattern && ./test_pattern
python3 tests/cpu/math_check.py
```
