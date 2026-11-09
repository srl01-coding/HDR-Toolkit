# SDR in HDR

`hdr_toolkit_sdr_in_hdr_v1`, schema 1. User-directed 9 Nov 2026.

## Why

On an HDR canvas OBS places every SDR source (lyrics, slides, browser and image sources,
SDR cameras and footage) at one global *SDR White Level* (Settings > Advanced, 300 nits by
default). Report ITU-R BT.2408 puts HDR reference white and graphics white at **203 nits =
75 % HLG**; at 300 nits, white lyrics sit at 81 % HLG, brighter than faces lit to reference
white. Lowering the global setting to 203 fixes graphics but changes every SDR source at
once, and SDR video placed with OBS's sRGB decode looks flat next to HDR cameras. This
filter sets the level per source.

## What it does

Active only on an HDR canvas (Rec.2100 HLG or PQ) and only on SDR input (`GS_CS_SRGB`,
`GS_CS_SRGB_16F`). On an SDR canvas, or on an HDR source, it passes the source through and
logs why. The result is reported as `GS_CS_SRGB_16F` in OBS units (nits / OBS SDR white
level), which libobs copies into the HDR target unchanged, values above 1.0 included.

| Mode | Mapping | For |
|---|---|---|
| **Graphics** (default) | `L = white x V`: OBS's own sRGB decode, scaled; a pure gain, alpha-exact | lyrics, slides, lower thirds, logos |
| **Video** | `L = white x E'^2.4`, `E' = srgb_encode(V)`: BT.1886 display light with SDR 100 % at `white` (BT.2408's display-light mapping of SDR into HDR) | an SDR camera, SDR footage |

**White level** (default 203 nits): where SDR 100 % lands. The properties panel shows the
HLG signal it corresponds to on OBS's HDR peak (203 nits = 75.0 %, 300 = 81.3 %, on 1000).

**Highlight expansion** (Video, default off): lifts the top of the SDR range above a knee
(default 75 % SDR signal, on max(R,G,B)) towards a chosen peak, `L x (1 + e t^2)` with
`t = (s - knee) / (1 - knee)`, `e = peak / white - 1`: ratio-preserving (hue kept),
monotonic, C1 at the knee, SDR 100 % -> peak. This is **not a standardised curve**; it
adds some sparkle to SDR footage next to HDR cameras. Off keeps the BT.2408 mapping.

Graphics vs video at the same white level (203 nits): an SDR 50 % grey sits at 43.5 nits
in Graphics (sRGB) and 38.5 nits in Video (BT.1886); the difference is the darker BT.1886
toe, which is how SDR video is mastered.

Identity: Graphics at OBS's own SDR white level is what OBS already does; the filter skips
it unless *Force shader render* is on (gate SI-1).

## Placement

On the SDR source itself (or a group / nested scene of graphics). After any SDR-domain
filters (chroma key, crop, colour correction made for SDR), before HDR Toolkit filters
that expect HDR values. Check with the Color Monitor false colour (cyan = 73-77 % HLG):
white lyrics at 203 nits should read cyan.

## Tests

CPU (`tests/cpu/test_sdrhdr.cpp`, PASS): HLG levels (203 nits = 74.99 %, 1000 = 100 %);
graphics white exact and a pure gain; video white and mid grey (BT.1886); expansion
reaches the peak, leaves the range below the knee unchanged, is C1 at the knee, monotonic
and ratio-preserving; float mirror vs double over 600 000 premultiplied samples, max
relative error 1.7e-6. Shader: DXC compile OK.

OBS gates SI-1 to SI-4 (`docs/VALIDATION.md`): NOT RUN.
