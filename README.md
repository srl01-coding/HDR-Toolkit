# HDR Toolkit

HDR-safe video filters for OBS Studio, working in OBS's linear HDR space without
8-bit intermediates or hidden clamps:

* **HDR Transform** - four-corner pin, bilinear (StreamFX-style) or projective, with
  rotation and barrel / pincushion correction applied to the source before the corners,
  and an optional scale-to-fill so no empty corners or edges show. 3D modes are planned.
* **HDR Sharpen** - unsharp mask that works on HDR (OBS's Sharpen skips HDR sources):
  sharpens luma in a log domain and applies it as a gain, so highlights are kept, halos are
  limited in stops, and shadow noise is not boosted. See `docs/SHARPEN.md`.
* **HDR Color** - white balance (temperature / tint), exposure, contrast and pivot,
  saturation, colour-balance wheel, seven tonal zones, linear offset, Grade Mix,
  and low / high soft clip.

* **HDR Denoise** (source filter) and **HDR Program Denoise** (Tools menu), in
  development: HQDN3D-style spatial + temporal denoise in OBS's HDR working space (an
  independent implementation), with scene-cut reset and transition protection,
  strengths from subtle to fully "plastic" (0-100), debug views (what was removed,
  temporal weight), and **Measure noise**: point the camera at a static shot and the
  filter measures its noise floor and sets the comparison knee to match.

  **Placement.**
  - For the highest denoise quality, add *HDR Denoise* directly to each camera source,
    before HDR Color, perspective correction and scaling. This gives temporal processing
    a continuous, camera-specific history and avoids denoising noise that has already
    been resampled.
  - If GPU load is a concern, use *HDR Program Denoise* to process the final composited
    output once.
  - A useful hybrid is source-level temporal denoise on each camera, with one spatial
    cleanup pass at program level.

Developer tools in the same package: **HDR Toolkit: Test Pattern (developer)**
(exact HDR values: neutral patches 0-10,000 nits, log ramp, HLG 0-109% levels,
wide-gamut colours, alpha fixtures; no video decode) and **HDR Toolkit: Neutral
(developer test)** (a real HDR shader that must leave the image unchanged).

**Status: beta.** Windows x64, tested with OBS 32.2.2 on a Rec.2100 HLG canvas; the
macOS and Linux packages build but are untested. Try it in a separate profile or scene
collection before using it live. `docs/VALIDATION.md` lists what has been verified
(CPU maths, builds) and what is still NOT RUN in OBS.

## HDR Color: tonal zones

![Default tonal-zone windows](docs/images/zone-windows.png)

Zones are placed in stops from a gray reference (18 nits by default, Advanced).
Each tick shows stops / nits / HLG % (1000-nit peak) / 10-bit narrow-range code.
The chart stops at -9 stops (below that is camera noise; Black and Dark carry on
down to true black) and at code 1023, OBS's HLG encode ceiling.

| Zone | Full strength | Hands over at |
|---|---|---|
| Black | below -7 (fades out by -5) | sits on top of Dark |
| Dark | down to black, up to -5 | -4 (HLG 10%, code 154) |
| Shadow | -3 to -1.75 | -1 (24%, 277) |
| Midtones | -0.25 to +0.25 | +1 (43%, 444) |
| Light | +1.75 to +2.25 | +3 (69%, 671) |
| Highlight | +3.75 up to peak | |
| Specular | from +5 (92%), fading in from +3 | sits on top of Highlight |

* Dark, Shadow, Midtones, Light and Highlight share mirrored fades at each
  handover, so their weights always add up to 1: the same push on neighbouring
  zones is an even push, with no doubling at the seams.
* Masks are measured after white balance and global exposure, then frozen, so a
  zone adjustment never moves a pixel into another zone.
* Zone exposure is a gain: it cannot lift true black. Use **Offset** for that.
* Keep a single zone push within about +-1 EV (1.5-stop fades); beyond that,
  tones inside a fade can cross over. Widen the fade for bigger pushes.
* Every range, open end and fade is adjustable per zone; *Advanced > Diagnostic
  view* shows one zone's mask or all zones in false colour.

## Install (Windows)

1. Download `hdr-toolkit-<version>-windows-x64.zip` from
   [Releases](https://github.com/srl01-coding/HDR-Toolkit/releases) and unzip it.
   (Development builds: the `...-windows-x64-<commit>` artifact of a successful *Push*
   run under **Actions**; it contains a second zip, unzip that too.)
2. Close OBS. Copy the `hdr-toolkit` folder (containing `bin\64bit\hdr-toolkit.dll`
   and `data\`) into `C:\ProgramData\obs-studio\plugins\`. **Create the `obs-studio\plugins`
   folders if they do not exist** (they don't on a fresh install; `ProgramData` is hidden).
   Alternative, Program Files layout (the folder structure is different):
   `bin\64bit\hdr-toolkit.dll` -> `C:\Program Files\obs-studio\obs-plugins\64bit\`, and the
   *contents* of `data\` -> `C:\Program Files\obs-studio\data\obs-plugins\hdr-toolkit\`.
3. To uninstall, delete `C:\ProgramData\obs-studio\plugins\hdr-toolkit`.

**Upgrading from a build named `obs-hdr-toolkit`** (before 1 Nov 2026): delete the old
`obs-hdr-toolkit` folder (and `obs-hdr-toolkit.dll` / `data\obs-plugins\obs-hdr-toolkit`
in the Program Files layout) before installing. Both register the same filters and must not
be loaded together. Scenes keep their filters and settings (the filter IDs are unchanged),
and HDR Program Denoise copies its saved settings from the old plugin folder on first start.

This location is separate from the OBS install folder, so it does not touch
other plugins.

## Documentation

* `docs/ARCHITECTURE.md` - pinned versions, layout, build
* `docs/COLOR_PIPELINE.md` - colour-space, units and alpha contract
* `docs/STREAMFX_REFERENCE.md` - StreamFX provenance and licence audit
* `docs/PARAMETER_SCHEMA.md` - every saved setting, defaults and ranges
* `docs/denoise/` - program denoise brief and design notes
* `docs/VALIDATION.md` - PASS / FAIL / NOT RUN table

## Licence

GPL-2.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
