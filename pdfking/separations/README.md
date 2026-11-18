# Separations: pdfium --plate vs Ghostscript

Goal: pdfium's plates (`PDFKING_SetPlate`, `pdfium_test --plate=<ink>`)
byte-identical to Ghostscript's separations as `switch separate` renders
them (`-dUseFastColor`, overprint simulated).

- `make.py <dir>`: the test PDFs, one topic each (`00-sheet` all inks + RGB
  together, tints, blends, RGB/CIE, images, overprint, shadings, groups,
  ICC), every cell on whole points: at 144dpi a 30pt cell is 60x60px.
  Each PDF has a `.json` of its cells.
- `run.py <dir> [test...]`: per PDF gs `tiffsep` and pdfium per plate at
  144dpi, no anti-aliasing; writes `<dir>/out/<test>/` (composites, per
  plate gs / pdfium / diff, orange off by 1, red more) and
  `<dir>/out/SUMMARY.md`. Needs `out/native/pdfium_test` built and gs.

    python3 make.py /tmp/separations/pdf
    python3 run.py /tmp/separations           # numpy + Pillow
