# SDR Conversion

`hdr_toolkit_sdr_v1`, schema 1. User-directed 8 Nov 2026: "instead of separate canvas, just a
SDR Conversion filter that does the conversion so someone can check it pre-broadcast ...
if someone wanted to take HLG input as is, and then map to SDR in OBS, this would work".

## What it is for

1. **Pre-broadcast check on an HLG canvas.** Put it on a nested scene shown in a projector,
   or enable it briefly on a source or scene, and see what an SDR viewer will get from a
   broadcast down-mapper. While enabled, everything downstream (including the stream)
   carries the SDR version: it is a check, not a monitoring feed.
2. **HLG camera on an SDR canvas.** OBS's own conversion of an HDR source on an SDR
   canvas is a per-channel Reinhard curve (`default.effect` DrawTonemap) that puts
   1000 nits at about 90% and depends on the SDR white level setting. This filter replaces
   it with a broadcast down-mapping.

It is not a second canvas and does not produce a simultaneous SDR output.

## Methods

**BT.2446-1 Method A** (default). Report ITU-R BT.2446-1, Method A, a display-light
down-mapping (BT.2408-7 section 5.2: display-light down-mapping "is usually preferred").
Input: OBS linear light (display light, Rec.709 primaries, 1.0 = SDR white), converted to
BT.2020 and normalised to the conversion peak Lhdr:

    E'    = (E / Lhdr)^(1/2.4)
    Y'    = 0.2627 R' + 0.6780 G' + 0.0593 B'
    rho   = 1 + 32 (L / 10000)^(1/2.4)            rho_hdr at Lhdr, rho_sdr at 100 nits
    Y'p   = log(1 + (rho_hdr - 1) Y') / log(rho_hdr)
    Y'c   = 1.077 Y'p                             Y'p <= 0.7399
            -1.1510 Y'p^2 + 2.7811 Y'p - 0.6302   Y'p <  0.9909
            0.5 Y'p + 0.5                         otherwise
    Y'sdr = (rho_sdr^Y'c - 1) / (rho_sdr - 1)
    f     = Y'sdr / (1.1 Y'),  Cb = f (B' - Y') / 1.8814,  Cr = f (R' - Y') / 1.4746
    Y'tmo = Y'sdr - max(0.1 Cr, 0)

then BT.2020 SDR R'G'B' (clipped to 0..1), linear (power 2.4), BT.2020 -> BT.709, clipped
to 0..1 (hard gamut clip), power 1/2.4. The filter writes `srgb_decode` of that BT.709 SDR
signal, so OBS's own SDR output encode reproduces the signal exactly.

**OBS default.** A mirror of libobs DrawTonemap: 709 -> 2020, x / (x + 1), clip, power
1/2.4, sRGB decode, 2020 -> 709. Shows what OBS itself does with HDR on an SDR canvas.

**Conversion peak** (0 = OBS's HDR nominal peak, normally 1000). This is Lhdr in the
standard: the brightest level kept. Lowering it brightens the SDR picture and clips above it.

Achromatic levels (SDR signal %, `tests/cpu/test_sdr.cpp`):

| HDR display light | Method A, peak 1000 | peak 600 | peak 400 | OBS default, SDR white 203 | OBS default, SDR white 300 |
|---|---|---|---|---|---|
| 26 nits (18% grey, 38% HLG) | 33.5 | 37.8 | 42.1 | 40.4 | 34.9 |
| 100 nits | 53.9 | 62.0 | 70.0 | 63.0 | 56.1 |
| 203 nits (reference white, 75% HLG) | 68.7 | 78.4 | 86.6 | 74.9 | 68.5 |
| 400 nits | 83.3 | 92.8 | 100 | 84.3 | - |
| 1000 nits | 100 | 100 | 100 | 92.6 | 89.6 |

Method A at the standard peak puts HDR reference white at about 69% SDR. That is darker
than SDR graphics are normally made (BT.2408-7 section 7.2 notes that SDR graphics placed
at 75% HLG can come back from a down-mapper "as little as 86%"); whether a particular
broadcaster's down-mapper behaves like Method A is not known here. The peak setting is the
knob for that trade-off. The published curve constants leave a step of 5.5e-4 in Y'c at
Y'p = 0.7399 (`1.077 x 0.7399` vs the quadratic); it is invisible.

## Colour spaces

The SDR result is reported as `GS_CS_SRGB_16F` for every input. libobs copies SRGB_16F into
an HDR target (1.0 = SDR white, so the SDR picture sits at SDR white on an HLG canvas, the
way OBS shows any SDR source) and passes it unchanged into an SDR target. OBS's own tone map
is applied only to 709_EXTENDED in an SDR target (`obs-source.c` source_render), so it is
never stacked on top of this conversion.

Input: 709_EXTENDED, scRGB and SRGB_16F are converted (SRGB_16F with values above 1.0 is
what HDR Input produces from a DeckLink). 8-bit SRGB input is passed through with a log
warning (it cannot hold HDR). Put the filter on HDR sources or scenes only: true SDR content
in SRGB_16F would be treated as HDR at SDR white and darkened.

**Split compare.** Left of the split: the original as this canvas shows it. On an HDR
canvas the filter then reports 709_EXTENDED to carry the HDR original; on an SDR canvas the
left side is OBS's own tone map (what you would see without the filter). A 2-pixel grey line
marks the split.

## Tests

CPU (`tests/cpu/test_sdr.cpp`, PASS): BT.2446-1 worked numbers (rho 13.2598 / 5.6970;
100 nits -> Y'p 0.6732, Y'sdr 0.5387); knee continuity; grey ramp monotonic and neutral;
black -> 0; 1000 nits -> 100%; OBS default puts SDR white at 0.5^(1/2.4); float mirror vs
double reference over 600 000 random BT.2020 colours up to 1.5x peak: max SDR signal error
1.3e-4 (Method A), 5.9e-6 (OBS default), all outputs in 0..1. Shader: DXC compile OK.

OBS gates SD-1 to SD-4 (`docs/VALIDATION.md`): NOT RUN.
