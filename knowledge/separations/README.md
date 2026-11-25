# Separations: pdfium plates vs Ghostscript

Goal: pdfium's plates (`PDFKING_RenderInks`, all in one render: `run.py
--ink`; or `PDFKING_SetPlate`, `pdfium_test --plate=<ink>`, one each)
byte-identical to Ghostscript's separations as `switch separate` renders
them (`-dUseFastColor`, overprint simulated). The one-pass render is the
reference: nonseparable blends need every ink at once (15 fails per plate).

- `make.py <dir>`: the test PDFs, one topic each (`00-sheet` all inks + RGB
  together, tints, blends, RGB/CIE, images, overprint, shadings, groups,
  ICC, `11-images`: every filter, color space, depth, Decode, mask and
  inline image, each drawn knockout, overprint OPM 1 / 0, ca .5, Multiply,
  overprint + ca .5 over a base inked on all six plates), text, knockout
  groups, `15-nonseparable` (Hue, Saturation, Color, Luminosity plain, at
  ca .5, in every kind of group, as a group's mode; SoftLight,
  ColorDodge), `16-overprint-groups` (overprint inside groups),
  `17-spot-discovery` (spots only a pattern, a shading, a Type3 glyph, a
  nested form, an indexed image, an annotation, a DeviceN name), every cell on
  whole points: at 144dpi a 30pt cell is 60x60px. Rows and columns are
  labelled (left out of the comparison). Each PDF has a `.json` of its cells.
- `run.py <dir> [--ink] [test...]`: per PDF gs `tiffsep` and pdfium per
  plate (`--ink`: all plates in one render, `PDFKING_RenderInks`) at
  144dpi, no anti-aliasing; writes `<dir>/out/<test>/` (composites, per
  plate gs / pdfium / diff, orange off by 1, red more) and
  `<dir>/out/SUMMARY.md`. Needs `out/native/pdfium_test` built and gs.

    uv run --with pillow --with numpy --with fonttools python make.py ../out/separations/pdf
    uv run --with pillow --with numpy python run.py ../out/separations --ink

`bands.py <dir>`: every plate rendered in bands of 7 rows against the page
rendered whole.

With `--ink` SUMMARY.md also says per test whether the plates pdfium finds
on its own (`PDFKING_GetPageInks`) are gs's.

Known differences left (everything else byte-identical, plates in cells):

- gs (tiffsep and pam, /simulate or not) ignores overprint of process
  colors on a page until a spot color has been painted on it: before, a
  process color knocks the others out (00-sheet: the first three rows,
  over cyan; a CMYK-only page: everywhere). pdfium overprints per the spec.
- gs (tiffsep, and pam with /simulate): a 1-bit image (gray, indexed, CCITT,
  JBIG2) with ca < 1 or a blend mode is painted as its 0 color over the
  whole image, then the 1 pixels (gxifast.c copy_portrait: a DeviceN color
  is not pure, so fill_rectangle of color 0, then fill_masked of color 1:
  the 1 pixels composited twice; a gs bug, not followed).
- Lab with no output intent: through gs's default_cmyk.icc as gs does
  (PDFKING_SetBlendProfiles), one level off in two cells.

With an output intent (PDF/X), ICC based, RGB and Lab colors go on the
process plates through it (lcms, the object's rendering intent, black point
compensation), as GWG Output Suite 5.0's reference does; gs -dUseFastColor
does not, so such PDFs are checked against GWG's reference, not gs.
- `/Interpolate true`: smoothed as gs smooths it (gxiscale.c, siscale.c's
  Mitchell filter, `core/fxge/gs_image.cpp`), the plate's gray interpolated
  (the samples of gray, RGB, Lab spaces as they are, of CMYK, spots,
  DeviceN as the ink): exact where gs's conversion to the plate is linear
  (no output intent). Not yet: image masks (pdfi's ImScaleDecode), 16-bit
  samples (gs interpolates fracs, pdfium has bytes).
- shadings: pdfium steps them in 256, radial off up to 12 (08, 17).
- strokes: gs paints the pixel the edge lies on, pdfium does not (one pixel
  wider in gs; 07 OP stroke K).
- text: filled text is byte-identical to gs (glyphs through FreeType as
  gs's FAPI loads them: TrueType hinted mono, gs's fixed-point size and
  transform, origins in gs's 1/256 then rounded); stroked text (Tr 1, 2)
  follows the stroke difference below. A glyph whose origin lies exactly on
  a half pixel can land a pixel off: gs re-derives the text position from
  its 1/256-pixel current point after every Tj.
- 09 group Multiply smask, Cyan: a group mixed onto the page whose objects
  multiply each other comes out 32 off (per plate too, upstream).
- a nonseparable blend inside a non-isolated group that has a blend mode
  of its own: drawn per plate (wrong); every other group kind is right.
- spots: gs makes a plate only for a spot painted; pdfium lists every spot
  the resources name (12: an unused 354, its plate blank). gs never looks
  in a Type3 font's own resources: it drops such a glyph (17); pdfium paints
  it, its spot found.
- bands: a page rendered in bands is the page whole but for a few pixels of
  big text and a Type3 glyph at band edges (both renders, upstream agg).
- composites: gs renders CMYK + spots and converts to RGB (a print
  preview), pdfium renders in RGB.
- transparency groups blending in RGB (/CS /DeviceRGB, CalRGB, an RGB ICC
  profile): as gs, the page group (when the page has transparency) and
  isolated form groups render their process colors as R, G, B, blended,
  then each pixel to CMYK through gs's default profiles (or the output
  intent); spots on their own plates. CMYK content in such a group is
  taken by gs as R = C, G = M, B = Y (a gs bug): pdfium converts it
  (1 - min(1, C + K)). Gray groups and non-isolated RGB form groups are
  still blended on the plates.
- transfer functions (/TR): gs maps an ink through f as 1 - f(1 - ink), but
  on a page with transparency as f(ink); pdfium always the former
  (18-corpus uses symmetric functions).
- printed form fields: drawn by the page render (FPDF_PRINTING |
  FPDF_ANNOT), so pdfium_test, which also draws them through its form
  fill (FPDF_FFLDraw), paints them twice (18-corpus "fields" per plate);
  plates_bench / PDFKING_RenderInks match gs.
