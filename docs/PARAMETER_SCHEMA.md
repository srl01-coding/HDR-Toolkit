# Parameter schema

Persisted numeric parameters only (brief 10.4). Values are validated on load
and on every update; unknown enum values fall back to the default. Any change
to processing order or meaning needs a new `schema_version` and a migration.

## hdr_toolkit_transform_v1 (schema_version 2: P1 corner pin + pre-warp)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `schema_version` | int | 2 | 1 = corner pin only. 2 adds the pre-warp keys below with neutral defaults, so schema-1 settings load unchanged (no migration). |
| `warp_model` | int | 0 | 0 = Bilinear (StreamFX-style inverse-bilinear), 1 = Projective (homography) |
| `corner_tl_x`, `corner_tl_y` | double | 0, 0 | **Canonical** corner A, normalised to the output rectangle (x right, y down) |
| `corner_tr_x`, `corner_tr_y` | double | 1, 0 | corner B |
| `corner_br_x`, `corner_br_y` | double | 1, 1 | corner C |
| `corner_bl_x`, `corner_bl_y` | double | 0, 1 | corner D |
| `ui_tl_x` ... `ui_bl_y` | double | canonical x 100 | UI mirror in percent. Written through to the canonical keys by the property callbacks; regenerated from the canonical keys on every update. Scripts should set the canonical keys. |
| `sampling_mode` | int | 0 | 0 = bilinear, 1 = point (diagnostic) |
| `force_render_identity` | bool | false | Diagnostic: run the shader even for the identity quad |
| `pre_rotation_deg` | double | 0 | Pre-warp step 1 (user-directed, 1 Nov 2026): rotation of the source about the frame centre, degrees, clockwise on screen, isotropic in pixels. -180..180 |
| `pre_distortion` | double | 0 | Pre-warp step 2: radial barrel (+) / pincushion (-), -100..100 -> k = 0.3 x value / 100. Sampling map e = s d (1 + k r^2), r = 1 at the frame corners. k >= -0.3 keeps the map monotonic (fold-free needs k > -1/3) |
| `pre_autofill` | bool | true | Scale s = the largest view with no empty corners or edges (boundary of the frame sampled at 4 x 513 points, then 1 ppm inside); off: s = 1 |

**Processing order (schema 2):** source -> rotation -> barrel/pincushion -> corner pin ->
output. Rotation and the radial distortion are both centred on the frame and isotropic in
pixels, so they commute: their relative order does not change the image, only that both
come before the corner pin. With both at 0 the pre-warp is skipped (`pre_on = 0`) and the
output is bit-identical to schema 1.

Validity (brief 4.6): coordinates finite and within -4..5 (-400%..500%); no
coincident corners (1e-4); convex in either winding (mirroring allowed);
area >= 1e-4 x longest edge^2; bilinear Jacobian sign consistent at the
four source corners; projective additionally: corner residual <= 1e-9,
homogeneous denominator one sign over the source square, H^-1 H = I to 1e-9.
An invalid quad is persisted as entered (the user's numbers are never
rewritten) but rendering uses the last valid quad, or identity after reload.

Converting StreamFX centred percent: `canonical = (streamfx_percent + 100) / 200`.

Not yet in the schema (later packages, additive with defaults matching v1
behaviour): `transform_mode` (Corner Pin / Perspective 3D / Orthographic 3D),
3D parameters, `edge_aa`, pixel / legacy-centred unit display.

## hdr_toolkit_color_v1 (schema_version 4: global stages, tonal zones, offset, soft clips)

| Key | Type | Default | UI range | Meaning |
|---|---|---|---|---|
| `schema_version` | int | 4 | | Written on create. 1-3 = development builds with other zone defaults; 4 = seven zones. Settings from 1-3 are **not migrated** (user decision, 4 Oct 2026: regrade) |
| `global_exposure_ev` | double | 0 | -6..6 | Linear gain 2^EV |
| `contrast_factor` | double | 1 | 0.5..2 | Power k on luminance around the pivot: `Y' = pivot (Y/pivot)^k`, applied as an RGB gain (hue/ratio preserving, sign-safe) |
| `pivot_nits` | double | 18 | 0.1..1000 | Contrast pivot in nominal nits |
| `gray_reference_nits` | double | 18 | 1..100 | Reference grey (brief 5.2); scales the log domain, and will be the tonal-zone reference in P3 |
| `global_saturation` | double | 1 | 0..2 | Linear-Y-preserving saturation |
| `wb_mired_shift` | double | 0 | -90..90 | Temperature: target white on the CIE daylight locus at (1e6/6504 + shift) mired, offset from D65 in CIE 1960 uv; Bradford adaptation D65 -> target (+ = warmer) |
| `wb_tint` | double | 0 | -100..100 | Tint: offset along the locus normal, 0.01 uv per 100 (+ = magenta, - = green) |
| `global_wheel_x`, `global_wheel_y` | double | 0, 0 | -1..1 | Colour-balance wheel; hue = atan2(y, x) with 0 deg = red, radius clamped to 1, sensitivity k = 0.5. `C += k r d(hue) Y`, d luminance-orthogonal |
| `grade_mix` | double | 1 | 0..1 | Linear-light mix of the original and graded colour (brief 5.4); soft clips (P3) will run after it |
| `force_render_identity` | bool | false | | Diagnostic: run the shader even when the grade is neutral |
| `offset_nits` | double | 0 | -1..1 (script -10..10) | Straight linear offset in nits added to R, G, B (user-directed addition, 4 Oct 2026). After saturation, before Grade Mix; not black-preserving; negative results kept |
| `low_clip_enabled` | bool | false | | Low soft clip (brief 8.2 toe) |
| `low_clip_knee_nits` | double | 0.1 | 0.001..10 (script 0.0001..100) | Knee L |
| `low_clip_strength` | double | 0.5 | 0..1 | beta; 0 = exact identity |
| `high_clip_enabled` | bool | false | | High soft clip (brief 8.3 shoulder) |
| `high_clip_peak_nits` | double | 1000 | 100..10000 (script 1..10000) | Asymptote P |
| `high_clip_softness` | double | 0.25 | 0..0.95 | **Canonical** q; knee H = P (1 - q); q = 0 = hard cap at P |
| `high_clip_knee_nits` | double | 750 | 5..10000 | UI mirror of H. Editing peak or knee sets q = clamp(1 - knee/peak, 0, 0.95) (the knee stays where it is in nits); regenerated from P, q on every update |
| `diag_view` | int | 0 | | 0 off, 1 one zone's mask as grey (1.0 = SDR white), 2 all zones in false colour. Unknown values -> 0 |
| `diag_zone` | int | 2 | 0..5 | Zone shown by `diag_view` 1 |

### Tonal zones

`<z>` is one of `black`, `dark`, `shadow`, `midtones`, `light`, `highlight`, `specular`
(seven zones: Midtones added to the brief's six at the user's request).

| Key | Type | Default | UI range | Meaning |
|---|---|---|---|---|
| `zone_<z>_enabled` | bool | true | | Disabled zones contribute neutral values; their settings are kept |
| `zone_<z>_exposure_ev` | double | 0 | -4..4 | `C *= 2^(sum w_i EV_i)`, masks frozen after WB + global exposure |
| `zone_<z>_saturation` | double | 1 | 0..2 | `S = S_global * prod(1 + w_i (S_i - 1))` |
| `zone_<z>_wheel_x`, `_wheel_y` | double | 0 | -1..1 | Added to the global wheel delta weighted by w_i |
| `zone_<z>_a`, `_b`, `_c`, `_d` | double | see below | -20..20 | **Canonical** window edges in stops from `gray_reference_nits`, a < b <= c < d. Black stores only `c`, `d`; Specular only `a`, `b` |
| `zone_<z>_open_low`, `_open_high` | bool | Dark low = true, Highlight high = true, others false | | Interior zones only (Black is always open below, Specular always open above). Open side: weight 1 all the way to black (incl. Y <= 0) / to any peak; the stored edges on that side are kept for re-closing but unused |
| `zone_<z>_center`, `_width`, `_fall_lo`, `_fall_hi`, `_full_below`, `_full_above` | double | from edges | | UI mirror, all derived from the edges: centre = (b+c)/2, width = c-b, fall_lo = b-a, fall_hi = d-c, full_below = c, full_above = b. Each UI field writes through only the edges it controls. Visible fields depend on the open flags |

Default edges (stops; user-directed 4 Oct 2026, departing from the brief 7.1 table).
Dark, Shadow, Midtones, Light and Highlight form a handover chain: each neighbour
pair shares one mirrored fade centred on the handover point (2 stops at -4, 1.5
stops at -1, +1, +3), so the chain's weights sum to exactly 1 at every level
(tested, C15): equal pushes on neighbours give an even push and nothing is
counted twice at the seams. Black and Specular sit on top of Dark and Highlight.

| Zone | Open | Fade in | Full | Fade out | Handover |
|---|---|---|---|---|---|
| Black | below | - | to -7 | -7..-5 | on top of Dark |
| Dark | below | - | to -5 | -5..-3 | -4 |
| Shadow | | -5..-3 | -3..-1.75 | -1.75..-0.25 | -4, -1 |
| Midtones | | -1.75..-0.25 | -0.25..+0.25 | +0.25..+1.75 | -1, +1 |
| Light | | +0.25..+1.75 | +1.75..+2.25 | +2.25..+3.75 | +1, +3 |
| Highlight | above | +2.25..+3.75 | from +3.75 | - | +3 |
| Specular | above | +3..+5 | from +5 | - | on top of Highlight |

At an 18-nit gray: -7 = 0.14 (HLG 4.3%, code 102), -4 = 1.1 (10%, 154), -1 = 9 (24%, 277),
0 = 18 (32%, 349), +1 = 36 (43%, 444), +3 = 144 (69%, 671), +5 = 576 (92%, 866) nits.
Closed fall-back edges for the open sides: Dark a, b = -9, -7; Highlight c, d = 5, 7.
Tonal-order limits with these fades: about +-1 EV (1.5-stop fades), +-1.3 EV on
the -4 handover.

Window: `w = smooth01((s-a)/(b-a)) * (1 - smooth01((s-c)/(d-c)))`, smooth01 =
smoothstep on the clamped mask coordinate; `s = log2(max(Y, 1e-6) / gray)`, so
Y <= 0 falls in the Black (and open-low) tail. Weights are not normalised. Edge
validation is deterministic: clamp to +-20, then push the edges in use upward
(b >= a + 0.05, c >= b, d >= c + 0.05; logged), then move the unused edges of an
open side out of the way (silently).
The UI mirror is regenerated from the edges on every update, so scripts should
set the edges.

Tonal-order property (tested, C19): a single zone push keeps tonal order iff
|EV| <= falloff / 1.5 on the falloff it pushes against (smoothstep slope peaks at
1.5 / falloff). Larger pushes are allowed (brief range +-4 EV) and can make
tones in the falloff cross over; the UI says so.

Processing order (brief 5.3 plus the user-directed offset; implemented stages marked *):
unpremultiply -> nominal nits -> C0 -> WB* -> exposure* -> zone masks (frozen)* ->
zone exposure* -> contrast* -> global + zone wheel* -> global x zone saturation* ->
offset* -> Grade Mix* -> low soft clip* -> high soft clip* -> [gamut containment: P5] ->
working units -> premultiply.

Soft clips: one scalar gain F(Y)/Y on RGB, positive Y only (Y <= 0 untouched).
Toe scale `(1-beta) + beta t (2-t)`, t = Y/L, for 0 < Y < L. Shoulder
`H + D x/(D+x)`, x = Y - H, D = P q, for Y > H; q = 0 -> min(Y, P). Both enabled
with L > H is a conflict: the last valid clip settings stay in use (low clip held
off if there are none yet), the UI shows the conflict and the log warns.
Clips run after Grade Mix, so they still work at Mix 0 (brief 5.4).

The zone stage was added with neutral defaults, and the soft clips will
default to off, so a scene saved by the P2 build renders identically. Any change to the order, WB mapping, wheel sensitivity or
contrast law needs schema 2 and a migration (brief 10.4).

Out-of-range values (e.g. a hand-edited scene file) are clamped to the UI range
and logged; non-finite values revert to the default. If the white-balance
matrix cannot be built, the last valid grade is kept (neutral after reload).

