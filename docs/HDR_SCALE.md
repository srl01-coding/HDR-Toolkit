# HDR Scale

`hdr_toolkit_scale_v1`, schema 1. User-directed 8 Nov 2026: the church canvas is
4560 x 2160 (19:9); every 16:9 camera has to be enlarged (x1.1875, top and bottom
cropped), and the anamorphic camera (1.25x adapter) has to be desqueezed. The YouTube live
2160p tier needs a 2160-pixel height (the user's repeated tests: anything less drops the
top quality to 1440p), and letterboxing is not acceptable.

## Why not OBS's own scaling

When a scene item is scaled, libobs resamples it with the item's scale filter. Its Lanczos
(`libobs/data/lanczos_scale.effect`, OBS 32.2.2) works on the HDR texture in linear light
and has no ringing clamp. In linear light, Lanczos undershoots next to a bright
edge in proportion to the highlight. CPU simulation (`tests/cpu/test_scale.cpp`): a 26-nit
wall next to a 1000-nit light, enlarged x1.1875, dips to **-84 nits**, which shows as a
black halo after clipping:

| Method | Darkest / brightest near the edge |
|---|---|
| Linear light, Lanczos 3 (as OBS) | -84 / 1115 nits |
| PQ domain, Lanczos 3 | 16 / 1493 nits |
| PQ domain, anti-ringing 80 % (default) | 23.7 / 1083 nits |
| PQ domain, anti-ringing 100 % | 26 / 1000 nits |

## What it does

The filter takes the source to its final size, so the scene item sits at 1:1 and OBS does
no resampling. Use *Transform > Reset Transform* on the item after adding it.

- **Output width / height**: 0 takes the size from the source (desqueezed). *Set output
  size to the canvas* fills both in from the OBS base resolution.
- **Fit**: Fill (scale to cover, crop the excess, centred), Fit (scale to fit,
  transparent borders) or Stretch.
- **Anamorphic desqueeze**: horizontal pixel aspect of the source (1.25 for the Sirui
  1.25x).
- **Zoom / Pan**: punch-in and reframing; pan moves within the cropped-away area (-100 %
  to 100 %), so the frame never leaves the picture.

Church canvas 4560 x 2160:

| Source | Settings | Result |
|---|---|---|
| 16:9 camera, 3840 x 2160 | 4560 x 2160, Fill | x1.1875 both axes, about 202 rows cropped top and bottom (pan vertical to reframe) |
| a6400 + 1.25x anamorphic, 3840 x 2160 | 4560 x 2160, Fill, desqueeze 1.25 | x1.25 horizontal only, 120 columns cropped each side; vertical untouched |
| punch-in on a 16:9 camera | as above, zoom 2 | x2.375; pan to the subject |

## Resampling

Separable, one pass per axis (horizontal into an RGBA32F intermediate, then vertical into
RGBA16F).

- **Enlarging axes** interpolate in the ST 2084 (PQ) domain of absolute luminance
  (`nits = OBS value x SDR white`), sign-symmetric for out-of-gamut negative components.
  PQ is close to perceptually uniform from 0.005 to 10 000 nits, so ringing is spread
  evenly over brightness instead of concentrating at dark edges.
- **Anti-ringing** (enlarging only, default 80 %) pulls each result towards the range of
  the two nearest input samples, in the same domain.
- **Reducing axes** filter in linear light (area-correct averaging) with the kernel
  widened by 1/scale, so fine detail averages instead of aliasing; no anti-ringing there.
- **Kernels**: Lanczos 3 (default), Catmull-Rom, Mitchell (soft; not exact at 1:1),
  bilinear.
- Alpha is resampled with the colour (premultiplied). Fit borders are transparent. HDR
  values are not clamped; the native colour space is kept (8-bit SDR input is written as
  SRGB_16F, like the other filters).

The default (output size from the source, zoom 1) is an identity and passes the source
through; *Force shader render at 1:1* runs both passes anyway, for the identity gate.

**Placement:** after HDR Denoise and HDR Color (cheaper at source size, and denoise needs
the camera's own pixels), before HDR Sharpen (sharpen at the output size makes up for the
softness of a 1.19x enlargement). HDR Transform, if used, goes before HDR Scale.

**Cost:** two passes. Lanczos 3 enlarging reads 6 texels per pass with a PQ encode per tap
(about 2 % of a GTX 1650 Super's arithmetic at 4560 x 2160 x 30 fps by estimate, not
measured). The RGBA32F intermediate is 4560 x 2160 x 16 bytes, about 158 MB per frame.

## Tests

CPU (`tests/cpu/test_scale.cpp`, PASS): layout for the church canvas (x1.1875 and centred
crop; anamorphic x1.25 / x1.0 with vertical identity; pan to the edge; Fit borders; zoom 2);
kernels interpolating at integers and partition of unity; PQ round trip < 2e-4; identity
bit-exact after RGBA16F rounding in both domains; constant field kept at x1.1875 / 1.25 / 2
/ 0.8 / 0.5; the ringing table above; Nyquist stripes reduced x0.8 within 0.22 of the mean;
transparent Fit borders. Shader: DXC compile OK.

OBS gates SC-1 to SC-4 (`docs/VALIDATION.md`): NOT RUN.
