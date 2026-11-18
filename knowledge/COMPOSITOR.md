# Plan: non-separable blend modes on plates

Done another way: the one-pass ink renderer blends nonseparably across the
process planes (`SEPARATIONS.md`, Status). Kept for the record.

## The problem

pdfium renders a plate a pass at a time, as 8-bit gray (255 - ink). Every
separable thing is exact that way: a plate's ink depends on that ink alone
(Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
HardLight, SoftLight, Difference, Exclusion, opacity, soft masks, knockout,
overprint, ICC).

Hue, Saturation, Color and Luminosity are not: in a CMYK blending space
(ISO 32000-2 11.3.5.3) C, M and Y are blended together as their
complementary R, G, B (`B(Cb, Cs)` on `(1 - C, 1 - M, 1 - Y)`), and K is the
backdrop's for Hue, Saturation and Color, the source's for Luminosity. A pass
that has one ink cannot know the other two. On spot plates these modes are
Normal (11.7.4.2), as `PlateBlendMode` already does.

Evidence: GWG Output Suite 5.0, `knowledge/gwg/gwg.py` ->
`out/gwg/blend-modes.pdf`. 16.0 (non-knockout), 16.1 (knockout), 16.2
(isolated), 16.4 (ICCBased CMYK): Ghostscript (`tiffsep`, all inks in one
buffer) matches GWG's reference in every cell; pdfium crosses exactly the
non-separable ones (Luminosity in 16.0, 16.1, 16.4; all four in 16.2).

pdfium itself has no CMYK or N-channel device: bitmaps are gray, RGB or
BGRA, and its compositor (`CFX_ScanlineCompositor`, `blend.cpp`) blends
non-separable modes in RGB only. That RGB code is exactly 11.3.5.3's rule
once C, M, Y sit in R, G, B as 255 - ink — what the design below uses.

## Options

| | size | upstream files | rebase cost | covers |
|---|---|---|---|---|
| A. CMY pass for the non-separable object only (chosen) | ~250 lines | 1 hook line | none | all four modes, groups, knockout |
| B. CMY pass for the whole page | ~500-900 | agg spans, scanline compositor, glyphs, shadings (a per-channel write mask for overprint) | high | same, plus 3 passes -> 1 |
| C. N-channel CMYK + spots device, as gs | ~3000-6000 | 20+ (bitmap formats, every compositor, group and soft mask buffers) | very high | everything, gs's model |

B needs a per-channel write mask because overprint (OPM 1 zero
components, common in print) leaves single channels as they were; pdfium
has nothing like it. C is a second rendering pipeline. A keeps the
per-plate passes (exact for everything else) and only re-renders where a
non-separable mode is used, which is rare in print jobs.

## Design (A)

On a process plate, when `ProcessTransparency` meets an object (or form
group) whose blend mode is Hue, Saturation, Color or Luminosity:

1. Its device box `R` (the object's transformed bbox, clipped), as
   `GroupRect` does.
2. The CMY backdrop in `R`: the page rendered up to the object (the
   existing stop-object support, `SetStopObject`) three times, as plates
   Cyan, Magenta and Yellow, into gray bitmaps of `R`, packed into one BGRx
   bitmap (R = 255 - C, G = 255 - M, B = 255 - Y). Within a group, the
   group's backdrop (non-isolated) or clear (isolated), as now.
3. The object rendered on that BGRx bitmap in a new plate mode, `CMY`:
   `CPDF_Plate::Ink` for each of the three process inks at once, so
   `PlateArgb` / `CPDF_PlateCS` give `(255 - C, 255 - M, 255 - Y)`; with
   its blend mode as is: pdfium's RGB compositor applies 11.3.5.3.
4. This plate's channel (Cyan R, Magenta G, Yellow B) copied back into the
   plate bitmap in `R`, where the object painted (its shape, as
   `RenderKnockoutGroup` keeps it).
5. On the Black plate no CMY pass: Hue, Saturation, Color leave K as the
   backdrop (the object paints nothing on K); Luminosity paints K Normal.
   Both in `PlateBlendMode` (it takes the plate already).

Overprint inside such an object is not honoured per ink (the CMY pass
paints all three); gs's own pdf14 compositor is approximate there too.
Note it in `separations/README.md`.

Cost: per such object, three backdrop renders of `R` plus one object
render, only on process plates C, M, Y; nothing anywhere else. A cache of
the CMY backdrop per object across the three plate passes is not possible
(each pass is its own render); acceptable since the modes are rare.

## Where

- `core/fpdfapi/page/cpdf_plate.{h,cpp}`: `CMY` plate (name `"CMY"`,
  `index_` -2): `Inks(cs, comps, ...)` -> three inks; `PlateArgb` and
  `CPDF_PlateCS` return `255 - ink` per channel for it. `PlateBlendMode`:
  Black plate rule (step 5). ~60 lines.
- `core/fpdfapi/render/cpdf_renderstatus_plate.cpp`:
  `RenderNonSeparable(pObj, mtObj2Device)`: steps 1-4. ~150-200 lines.
  Shares `GroupRect`, `CopyPixels` with the knockout code.
- `core/fpdfapi/render/cpdf_renderstatus.cpp`, `ProcessTransparency`: one
  hook line, before the generic path:
  `if (PlateNonSeparable(pPageObj)) { RenderNonSeparable(...); return true; }`
- `cpdf_renderstatus.h`: two private declarations.

## Tests

1. `separations/make.py`: `15-nonseparable`: each of the four modes over
   CMYK, RGB and spot backdrops; plain, in a non-isolated, isolated and
   knockout group; with opacity .5; process and spot sources. gs `tiffsep`
   is the reference (it passes GWG 16.x): byte-identical in cells, or
   documented in the README.
2. GWG: `gwg.py`, `blend-modes.pdf`: no cross in 16.0, 16.1, 16.2, 16.4;
   `review.pdf` page 5 16.1 (ICCBased RGB group) by eye.
3. Every other separations test byte-identical (the hook only fires on
   the four modes).

## Steps

1. `15-nonseparable` in `make.py`; run it: the failures, recorded.
2. Black plate rule in `PlateBlendMode`; suite.
3. `CMY` plate in `cpdf_plate`; unit check by rendering a page with
   `--plate=CMY` against gs's C, M, Y plates merged (separable content
   only, no overprint): identical.
4. `RenderNonSeparable` + hook; suite, GWG.
5. README: the overprint-inside gap; this plan's status.
