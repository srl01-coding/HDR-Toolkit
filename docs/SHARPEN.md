# HDR Sharpen (`hdr_toolkit_sharpen_v1`, schema 1)

User-directed (7 Nov 2026). OBS's built-in Sharpen is SDR-only:
- it skips sources in `GS_CS_709_EXTENDED` (OBS 32.2.2 `sharpness-filter.c`);
- it clamps its result with `saturate()`.

## Method

An unsharp mask on **log luma**, applied to RGB as a **gain**. The CPU reference and float
mirror are `src/sharpen/sharpen-math.cpp`; the shader is `data/effects/hdr-sharpen.effect`.
They are kept in lock-step.

```text
Y   = luma of the un-premultiplied pixel (Rec.709 weights, working units)
F   = log2(max(Y, 0) + K)            K = dark floor
Fb  = alpha-weighted Gaussian of F    (sum w a F / sum w a)
d   = F - Fb                          detail, stops
c   = d^3 / (d^2 + t^2)               soft threshold t
e   = L * s(A c / L),  s(x) = x / sqrt(1 + x^2)
out = rgb + (rgb + a K)(2^e - 1)      per channel, premultiplied
```

Why each step:
- **Ratio domain.** Above the floor, a detail is sharpened by the same number of stops at
  any brightness: an edge at 20→40 nits and the same edge at 200→400 nits get the same
  change. A linear-light unsharp mask instead puts its biggest halos around speculars and
  can push values below zero.
- **Halo limit.** No pixel moves by more than ±L stops. The soft curve s() is
  monotonic and odd.
- **Dark floor.** Below K, the change becomes nearly additive, so shadow noise is not
  multiplied.
- **Alpha weighting.** Transparent neighbours do not pull the blur, so source edges get no
  halo.
- **HDR-safe.** No clamp, no `saturate`, values above 1.0 kept, and no sign flips (the
  gain is applied around −aK). The output is the exact input when e = 0, because
  2^0 − 1 = 0.

## Passes

1. Capture the filter input (shared `filter-io`).
2. `BlurH` → RG32F (Σ w a F, Σ w a).
3. `Combine`: vertical Gaussian, detail, gain → RGBA16F.
4. Draw like libobs (premultiplied, ONE / INVSRCALPHA).

Additional details:
- 8-bit SDR input is read through its sRGB view, so the maths is linear, and the
  output is written as `SRGB_16F`.
- Taps per side are ceil(3σ), at most 24.
- The pass runs once per video frame; repeated renders (preview, multiview) reuse the
  result.
- VRAM at 3840 x 2160 is about 63 MiB for the input texture, plus 63 MiB for RG32F and
  63 MiB for the RGBA16F output.

## Settings

| Key | Default | Range | Meaning |
|---|---|---|---|
| `amount` | 0 | 0-300 % | A = amount / 100; 0 = neutral (pass-through, no shader) |
| `radius` | 1.0 | 0.5-8 px | Gaussian sigma |
| `threshold` | 0.02 | 0-0.5 stops | coring: smaller detail is left alone |
| `halo_limit` | 0.5 | 0.05-2 stops | largest change per pixel |
| `dark_floor_nits` | 2 | 0.01-50 nits | K; HDR Denoise's measured noise floor is a good value |
| `debug_view` | 0 | 0/1/2 | normal / detail added (grey = none) / change x gain |
| `debug_gain` | 4 | 1-256 | |
| `force_render` | false | | run the shader at amount 0 (identity proof, SH-1) |

## Recommended chain

Camera → HDR Denoise → HDR Color → **HDR Sharpen** → HDR Transform.

- **After denoise:** sharpening first would amplify the noise that denoise then has to
  remove.
- **After grading:** sharpening sees the final contrast.
- **Before the corner pin:** the transform resamples the image.

## Tests

CPU tests are in `tests/cpu/test_sharpen.cpp`. They cover:
- exact identity at amount 0;
- a flat field left unchanged;
- edge steepening that is the same in stops at 10x the level;
- the halo limit on a 1→1000 nit edge;
- finiteness, alpha preservation and no sign flips;
- no halo at an alpha edge;
- coring;
- float mirror against double;
- sanitize.

Gates SH-1 to SH-4 in VALIDATION.md are NOT RUN.
