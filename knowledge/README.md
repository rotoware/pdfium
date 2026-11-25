# pdfking: the fork's own files

Everything here is ours, not upstream pdfium's: tools, test suites, plans.
Downloads go to `knowledge/.cache/`, results to `knowledge/out/` (both
ignored by git).

- `bench/` — `plates_bench`: a PDF's plates rendered as switch reads them
  (8-bit gray, in bands, the page on its bottom edge as gs places it); one
  render per plate (`PDFKING_SetPlate`), or all plates in one render a band
  with `--ink` (`PDFKING_RenderInks`); plates `auto`: CMYK and the page's
  spots (`PDFKING_GetPageInks`).
- `separations/` — synthetic test PDFs, one topic each, pdfium plates vs gs
  `tiffsep`, byte-compared per cell. See `separations/README.md`.
- `gwg/` — the Ghent Workgroup PDF Output Suite 5.0 (the industry's RIP
  conformance pages), pdfium plates vs gs vs GWG's reference, by eye.
- `SEPARATIONS.md` — plan: one-pass ink renderer (N planes, real
  compositor), gs's design mapped onto pdfium.
- `COMPOSITOR.md` — plan: non-separable blend modes on today's per-plate
  renderer (the cheap fix, superseded by `SEPARATIONS.md` if done).
- `gs/source.sh` — Ghostscript's source, downloaded once to `.cache/gs`.
- `corpus/download.sh` — public rendering test PDFs (Altona, PDF
  Association pdf-differences + pdf20examples, pdf.js corpus), downloaded
  once to `.cache/corpus`; what each set covers is in the script.
- `corpus/corpus.py` — every corpus page (~1170) separated by gs and by
  `plates_bench --ink auto`, classified (same, near, edges, off,
  group-cs, missing plate, size, errors...); `corpus/probe.py` — one page,
  the (gs, pdfium) values most often off per plate. See `corpus/README.md`.

## Setup (macOS, arm64)

1. Tools: `brew install ghostscript qpdf ninja`, and
   [uv](https://docs.astral.sh/uv/) for Python (scripts run with
   `uv run --with pillow --with numpy ...`).
2. The pdfium checkout: gclient root `~/Git/.gclient`, solution `pdfium`
   (this repo), synced with `gclient sync`. The macOS SDK 26.1 at
   `~/.cache/pdfking-sdk/MacOSX26.1.sdk` (chromium's linker rejects the
   macOS 27 SDK's stubs).
3. Configure `out/native` once:

       gn gen out/native --args='is_debug=false treat_warnings_as_errors=false
         pdf_use_skia=false pdf_enable_xfa=false pdf_enable_v8=false
         is_component_build=false clang_use_chrome_plugins=false
         pdf_is_standalone=true use_debug_fission=false use_custom_libcxx=false
         use_sysroot=false pdf_is_complete_lib=true pdf_use_partition_alloc=false
         is_clang=true symbol_level=0 target_os="mac" target_cpu="arm64"
         use_system_xcode=false mac_sdk_official_version="26.1"
         mac_sdk_official_universal_version="26.1"'

4. Build `pdfium_test` (the separations suite) and `plates_bench` (gwg):

       export SDKROOT=~/.cache/pdfking-sdk/MacOSX26.1.sdk
       ninja -C out/native pdfium_test
       knowledge/bench/build.sh

## Running

    cd knowledge/separations
    uv run --with pillow --with numpy --with fonttools python make.py ../out/separations/pdf
    uv run --with pillow --with numpy python run.py ../out/separations [--ink]
    # -> knowledge/out/separations/out/SUMMARY.md, a folder per test;
    # --ink: pdfium's plates from the one-pass ink render (needs plates_bench)

    cd knowledge/gwg
    uv run --with pillow --with numpy python gwg.py
    # downloads the suite once (~150 MB) -> knowledge/out/gwg/review.pdf,
    # knowledge/out/gwg/blend-modes.pdf

    uv run --with pillow --with numpy python plates.py <pdf> ../out/plates
    # any one-page PDF (e.g. a GWG patch from knowledge/.cache/gwg), per
    # plate gs | pdfium | diff

    cd knowledge/corpus
    sh download.sh            # once, ~575 MB
    uv run --with pillow --with numpy python corpus.py [--only=name,...] [set ...]
    # -> knowledge/out/corpus/SUMMARY.md, results.json, a sheet per page off
    uv run --with pillow --with numpy python probe.py <pdf> [page] [dpi]

A change to the renderer must leave the separations suite byte-identical
(compare `SUMMARY.md` and the `work/*/*/page.pdf.0.png` renders against a
run before it) unless it means to change it; GWG is checked by eye: a patch
shows a cross or a mismatch when a renderer gets it wrong.

## What the plates follow

- Without an output intent: gs `-dUseFastColor` (as switch separates
  today): RGB as C = 1 - R, M = 1 - G, Y = 1 - B, no black.
- With one (PDF/X): ICC based, RGB and Lab colors through it (lcms, the
  object's rendering intent, black point compensation), as GWG's
  reference; gs `-dUseFastColor` does not, so those are judged against GWG.
