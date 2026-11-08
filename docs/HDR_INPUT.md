# HDR Input (`hdr_toolkit_input_v1`, schema 1)

User-directed (7 Nov 2026). Put it **first** on a capture source.

## 1. Reinterpret an HLG signal that OBS decoded as SDR

### Problem

This was confirmed on the user's system.
- **Which source is affected.** OBS's DeckLink source takes the transfer function only from
  per-frame HDMI HDR metadata (`decklink-device-instance.cpp`, EOTF 3 = HLG). Its *Color
  Space* list sets the YCbCr matrix only.
- **What OBS does with it.** Without that metadata, a 10-bit v210 frame goes through
  `V210_SRGB_Reverse`: V = srgb_decode(T_m Y'CbCr), stored as `GS_CS_SRGB_16F`.
- **What the user sees.** On an HLG canvas the whole HLG signal sits below SDR white:
  about 75%, flat. Forcing a correct HLG Cam Link source to Rec. 709 reproduces the
  same picture.

### Fix

The filter undoes OBS's sRGB decode with OBS's own formula, then applies OBS's own HLG
decode, the same path as `V210_HLG_2020_709_Reverse`:

```text
E'     = srgb_linear_to_nonlinear(V)
E'2020 = T_2020 F_m E'                 (identity when the source used Rec. 2020)
out    = rec2020_to_rec709(hlg_to_linear(E'2020, exponent) * peak / SDR white)
peak   = min(HDR nominal peak, 1000);  exponent = 0.2 + 0.42 log10(peak / 1000)
```

**Output space (fix, 8 Nov).** The first build reported `GS_CS_709_EXTENDED` and the user
saw no change. libobs asks the async *parent* source, not the filter chain, for a scene
item's colour space (SRGB_16F here). Inside that SRGB_16F render, a filter that reports
709_EXTENDED is tone-mapped back to SDR (`source_render` → `DrawTonemap`).

The filter now reports the parent's space and writes the HDR result into it:
- linear Rec.709 with 1.0 = SDR white, the same scale as 709_EXTENDED;
- values above 1.0 kept;
- SRGB_16F → 709_EXTENDED is a plain copy, so the values reach an HLG canvas unchanged.

An 8-bit input (`GS_CS_SRGB`, DeckLink *8-bit YUV*) cannot carry values above 1.0, so the
filter passes it through and logs a warning.

**Accuracy (CPU, through RGBA16F storage).** The result matches OBS's HLG path to
0.2% of the pixel's largest component. That is the fp16 storage error OBS's own path also
has. It holds with matrix correction too, when the source was left on Rec. 709.

**Exception to "do not decode HLG".** This filter performs exactly the decode that OBS
should have done for a correctly tagged frame, with OBS's formulas. Nothing else in the
toolkit decodes HLG.

### Setup

**DeckLink source:**
- *Pixel Format*: 10-bit YUV (required: 8-bit cannot carry HDR values);
- *Color Space*: Rec. 2020 (or say what it is set to, in the filter);
- *Color Range*: Default / Partial.

**HDR Input:** set *Signal* to "HLG that OBS decoded as SDR".

**If OBS later receives the metadata,** the input becomes `709_EXTENDED`. The filter then
passes it through unchanged and logs a warning; it never decodes twice.

## 2. Signal analysis (bit depth)

**What it measures.** OBS has already converted the signal before any filter sees it.
That conversion is known exactly, though, so the luma code value is rebuilt:
- the Y row of the forward matrix recovers Y' from any R'G'B' = T_m Y'CbCr, whatever the
  chroma interpolation did;
- for HLG-decoded input, the inverse of OBS's 2020→709 conversion, OOTF and HLG EOTF is
  applied first.

**How it decides.** The rebuilt codes are tested against N-bit lattices:
- a signal quantised to D bits has phase coherence close to 1 for N ≥ D and close to 0
  for N < D;
- coherence is weighted by √count per code, so large flat areas do not dominate;
- samples at OBS's range clamp (64 / 940) are excluded;
- a verdict needs at least 32 codes spread over 128 or more.

**Limits:**
- **10 bits at most.** DeckLink v210 and P010 carry 10 bits, and RGBA16F storage adds
  about 0.1-0.2 code of error at 10 bits. Coherence at 11 and 12 bits is reported but no
  verdict is given above 10.
- **8-bit capture.** In *8-bit YUV* pixel format OBS converts to 8-bit RGB itself; the
  result then reflects OBS's own quantisation.
- **Settings must match.** A wrong matrix or range setting, or processing between camera
  and card (scaling, a converter), gives "not on a grid".

**CPU tests** (`tests/cpu/test_input.cpp`), simulating libobs's paths with fp16 storage:
- true 6, 7, 8, 9 and 10 bit are each detected correctly through three paths:
  SDR-decoded with Rec. 2020, SDR-decoded with Rec. 709, and the HLG path;
- a dithered signal gives no grid;
- a flat picture gives no verdict.

Gates IN-1 to IN-4 are NOT RUN.
