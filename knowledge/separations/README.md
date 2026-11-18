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
  whole image, then the 1 pixels.
- Lab: gs converts through ICC with black generation, pdfium does not
  (with no output intent; the suite's PDFs have none).

With an output intent (PDF/X), ICC based, RGB and Lab colors go on the
process plates through it (lcms, the object's rendering intent, black point
compensation), as GWG Output Suite 5.0's reference does; gs -dUseFastColor
does not, so such PDFs are checked against GWG's reference, not gs.
- `/Interpolate true`: gs smooths the image, pdfium (no smoothing) not.
- shadings: pdfium steps them in 256, radial off up to 12 (08, 17).
- strokes: gs paints the pixel the edge lies on, pdfium does not (one pixel
  wider in gs; 07 OP stroke K).
- text (12, 13, 14): gs fills glyphs hinted, through its character cache
  (stems snapped to whole pixels); pdfium fills the outlines unhinted (any
  part of a pixel, as paths) or draws FreeType's bitmaps: edges a pixel
  off. Neither rule tried (any part, half) matches; needs gs's hinting.
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
