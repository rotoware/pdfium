#!/bin/sh
# Public rendering test PDFs, downloaded once to knowledge/.cache/corpus
# (~575 MB). GWG lives in gwg/ (gwg.py downloads it). Untrusted files: read
# them, never run anything from inside.
#
# - altona/          ECI Altona Test Suite (PDF/X-3, X-4): overprint,
#                    spot/DeviceN, ICC, transparency, blend modes, shadings.
#                    technical2_one-patch-per-page_x4 = one patch a page;
#                    the documentation PDFs say what each must look like.
#                    Visual/Measure 2.0 + reference prints are paid (bvdm).
# - pdf-differences/ PDF Association: spec edge cases viewers disagree on
#                    (dash/cap degenerates, ColorBurn/Dodge, DefaultCS,
#                    negative font size, Type3 spacing, vertical text...).
# - pdf20examples/   PDF Association: PDF 2.0 features.
# - pdfjs/test/pdfs  mozilla/pdf.js regression corpus (~1100 real-world
#                    PDFs: broken xref, odd fonts, shading 4-7, JBIG2, JPX);
#                    *.link files point to external PDFs, not fetched.
set -e
dir=$(dirname "$0")/../.cache/corpus
mkdir -p "$dir"
cd "$dir"

mkdir -p altona
for f in \
  eci_altona-test-suite-v2_technical2_x4.pdf \
  eci_altona-test-suite-v2_technical2_one-patch-per-page_x4.pdf \
  eci_altona-test-suite-v2_technical2_documentation_eng-4.pdf \
  altona_technical_1v2_x3.pdf \
  altona_visual_1v2a_x3.pdf \
  altona_measure_1v1a.pdf \
  altonatestsuite_documentation_eng.pdf; do
  [ -f "altona/$f" ] || curl -fsSL -o "altona/$f" "https://www.eci.org/lib/exe/$f"
done

for r in pdf-differences pdf20examples; do
  [ -d "$r" ] || git clone -q --depth 1 "https://github.com/pdf-association/$r"
done

if [ ! -d pdfjs ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/mozilla/pdf.js pdfjs
  git -C pdfjs sparse-checkout set test/pdfs
fi

pwd
