# HDR Toolkit - working notes for Claude

Governing spec: *OBS HDR Toolkit: implementation brief for Claude Code* v1.0
(3 Oct 2026; project renamed "HDR Toolkit", package hdr-toolkit, on 1 Nov - OBS forum naming policy). It supersedes earlier conversations. Work packages P0-P5 (brief 12.1).
Program denoise: `docs/denoise/BRIEF_v1.2.md` governs (its own P0-P5); design notes in
`docs/denoise/DENOISE.md`. HQDN3D must be written independently from
`docs/HQDN3D_DESIGN.md` - never copy/port FFmpeg/MPlayer code (brief 28).

## Non-negotiables (brief 3, 12.4)
- No `GS_RGBA` intermediate for non-SRGB spaces; no `saturate`/`max(rgb,0)` in HDR paths.
- Do not decode HLG/PQ; do not treat 940 or 1.0 as a ceiling.
- Space policy: preserve native (`docs/COLOR_PIPELINE.md`); libobs converts at the consumer.
- `OBS_NO_DIRECT_RENDERING`; premultiplied output, blend ONE/INVSRCALPHA, push/pop state.
- Identity must be proven with forced shader render, not with a skip path.
- New instances neutral; soft clips/gamut limiter default OFF.
- Production IDs `hdr_toolkit_transform_v1` / `hdr_toolkit_color_v1`: register only when
  their schema is real; any processing-order change needs a versioned migration.
- Mark tests NOT RUN unless actually executed; never infer results.

## Environment
- No OBS/GPU here. Build = GitHub Actions (template), push to `main`.
- Syntax check locally against OBS 32.2.2 headers; CPU tests in `tests/cpu`.
- Format: clang-format 19 (`.clang-format`), gersemi for CMake (CI check-format on main).

## Status
P0 done (G0, N1 qualitative, N3b observed in OBS 32.2.2). P1 Corner Pin implemented
(transform-filter.cpp; shader mirrored in quad-math.cpp - keep in lock-step). Pre-warp (1 Nov, user): rotation then barrel/pincushion
before the corners, schema 2 (quad-math PreWarp, shader pre_warp() mirrored by shader_prewarp - lock-step; T9-T12). P2 global Color
implemented (color-filter.cpp; hdr-color.effect mirrored in color-math.cpp shader_grade - keep in
lock-step). P3 tonal zones implemented; user-directed changes (4 Oct): seven zones
(Midtones added), handover chain Dark..Highlight at -4/-1/+1/+3 (weights sum to 1), Specular full
from +5; schema 4, older settings not migrated (user will regrade). Offset (user-directed,
before Grade Mix) and P3b soft clips done. Next: P4/P5 only after the user's go-ahead.
Denoise: P0 implemented (main-rendered hook, main-mix + frame-time dedupe, Identity pass,
counters, Tools menu controller; user saw Identity unchanged, formal gates NOT RUN). P1 HQDN3D-style
temporal implemented (hqdn3d-math.cpp mirrors hdr-program-denoise.effect - keep in lock-step);
gates H-* NOT RUN. Shader pre-flight: tools/check-effect-hlsl.py with DXC.
Denoise P2 per docs/denoise/P2_DECISION_RESPONSE.md (Option D; governs P2): spatial B implemented
(spatial-math.cpp mirrors SpatialH/V in hdr-program-denoise.effect - keep in lock-step; spatial before
temporal; strengths default 0), A = CPU reference only, D3D11 compute identity spike implemented
(d3d11-compute.cpp, Windows-only, 3 isolation variants). Gates SP-*/CS-* NOT RUN. Next: user runs P1 on real
footage + CS gates; A compute only after the spike passes; no P2 defaults/A-vs-B choice before P1 real footage.
Denoise P2.5 (docs/denoise/ROADMAP_CORRECTION_MOTION_AWARE.md + P2_5_AMENDMENT.md, amendment wins): NLMeans
design + CPU refs (nlm-math, candidates (dx,dy,dt), policies A/B/H, tgain), tools/denoise-sim comparison with oracle-flow
MC (no optical-flow code may be written without authorisation), NVOF capability probe (nvof-probe.cpp, vendored BSD-3
headers in third_party/nvof). Findings: docs/denoise/MOTION_AWARE_TEMPORAL.md; measured noise: NOISE_MODEL.md (sample
clip is post-encoder; footage never committed - real people). GPU NLM/NVOF integration waits for CS gates + user.
5 Oct: HIGH_BITRATE_ANALYSIS.md (instructing AI) supersedes the 20 Mbit/s noise model for calibration. Decisions:
A TNLM GPU deprioritised; B NVOFA after NV-G0; C T(Y) noise profile implemented (noise-profile.cpp, shader
profile_mult, identity default, development curve "measured 2026-10-05", max multiplier; curve NOT frozen); D P1 continues,
HQ response = MC confidence; E spatial NLM lower priority. Next motion sample = motion footage (user).
5 Oct (PLACEMENT_UPDATE.md): NLMeans PARKED (no GPU/UI/benchmark work). Placement implemented: denoise-core.cpp
(shared core + settings model + telemetry + shared effect) with two front-ends: denoise-filter.cpp ("HDR Denoise",
hdr_toolkit_denoise_filter_v1, per-frame dedupe, continuity reset >2.5 frame intervals, protection default off) and
program-denoise.cpp. Never duplicate algorithm code between front-ends. Gates PL-* NOT RUN. Debug gain up to 1024;
Temporal weight debug view. 7 Oct: STRENGTH_SCALES.md (equal S is not equal strength; temporal
needs S 6-8 on measured noise); spatial-only skips metric/temporal passes (temporal_skipped, PL-G8).
10 Oct: darks - knee K is the noise floor (fit K~8 nits); knee range 0.01-20, default 0.1 kept;
noise-normalised removal view; do not combine K~8 with the measured profile (KN-1).
10 Oct (user): strengths 0-100 (0-20 unchanged); T linear; temporal beta cap 0.9 -> 0.99 above S 20
(history_beta, RGBA32F history above 20); SR-1.
10 Oct (user): live noise measurement in HDR Denoise (noise-meter*.cpp, SampleGrid, NOISE_MEASUREMENT.md);
Apply sets knee + identity profile; knee range 0.01-50; label "Noise floor (comparison knee)"; NM-1.
1 Nov: renamed to HDR Toolkit (package/DLL hdr-toolkit, repo renamed srl01-coding/HDR-Toolkit; work in /home/claude/hdr-toolkit); filter IDs unchanged;
Program Denoise migrates its config from plugin_config/obs-hdr-toolkit. Historical docs/denoise briefs keep the old name.
7 Nov (user): HDR Sharpen (src/sharpen, hdr-sharpen.effect mirrored by sharpen_f - lock-step; docs/SHARPEN.md;
SH-1..4). Shared capture/draw in src/shared/filter-io (denoise filter uses it too).
Windows compile check: python3 -m ziglang c++ -target x86_64-windows-gnu -c FILE -o out.o.
