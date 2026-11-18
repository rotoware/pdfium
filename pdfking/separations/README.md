# Separations: pdfium --plate vs Ghostscript

Goal: pdfium's plates (`PDFKING_SetPlate`, `pdfium_test --plate=<ink>`)
byte-identical to Ghostscript's separations as `switch separate` renders
them (`-dUseFastColor`, overprint simulated).

- `make.py <dir>`: the test PDFs, one topic each (`00-sheet` all inks + RGB
  together, tints, blends, RGB/CIE, images, overprint, shadings, groups,
  ICC, `11-images`: every filter, color space, depth, Decode, mask and
  inline image, each drawn knockout, overprint OPM 1 / 0, ca .5, Multiply,
  overprint + ca .5 over a base inked on all six plates), every cell on
  whole points: at 144dpi a 30pt cell is 60x60px. Rows and columns are
  labelled (left out of the comparison). Each PDF has a `.json` of its cells.
- `run.py <dir> [test...]`: per PDF gs `tiffsep` and pdfium per plate at
  144dpi, no anti-aliasing; writes `<dir>/out/<test>/` (composites, per
  plate gs / pdfium / diff, orange off by 1, red more) and
  `<dir>/out/SUMMARY.md`. Needs `out/native/pdfium_test` built and gs.

    uv run --with pillow --with numpy python make.py /tmp/separations/pdf
    uv run --with pillow --with numpy python run.py /tmp/separations

Known differences left (everything else byte-identical, plates in cells):

- gs (tiffsep and pam, /simulate or not) ignores overprint of process
  colors on a page until a spot color has been painted on it: before, a
  process color knocks the others out (00-sheet: the first three rows,
  over cyan; a CMYK-only page: everywhere). pdfium overprints per the spec.
- gs (tiffsep, and pam with /simulate): a 1-bit image (gray, indexed, CCITT,
  JBIG2) with ca < 1 or a blend mode is painted as its 0 color over the
  whole image, then the 1 pixels.
- Lab: gs converts through ICC with black generation, pdfium does not.
- `/Interpolate true`: gs smooths the image, pdfium (no smoothing) not.
- shadings: pdfium steps them in 256, radial off up to 12.
- soft masks under a moved CTM (pdfium
  drops them), strokes: gs paints the pixel the edge lies on, pdfium does not (one
  pixel wider in gs).
- composites: gs renders CMYK + spots and converts to RGB (a print
  preview), pdfium renders in RGB.
