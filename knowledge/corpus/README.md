# Corpus: real PDFs, gs plates against pdfium's

`download.sh` fetches public test PDFs once (~575 MB, `.cache/corpus`):
the ECI Altona suite, the PDF Association's pdf-differences and
pdf20examples, and pdf.js's regression corpus (~1100 real-world and broken
files). `corpus.py` separates every page with gs (`tiffsep
-dUseFastColor -dOverprint=/simulate -dUseCropBox`) and with `plates_bench
--ink auto` (PDFKING_GetPageInks then PDFKING_RenderInks, all plates in one
render), 72 dpi, and sorts each page into a status; `probe.py` shows, for
one page, the (gs, pdfium) values most often off per plate, the quickest way
to see which conversion or compositing rule differs.

    uv run --with pillow --with numpy python corpus.py            # all, ~10 min
    uv run --with pillow --with numpy python corpus.py --only=issue840,franz_2
    uv run --with pillow --with numpy python probe.py <pdf> [page] [dpi] [--out=dir]

`knowledge/out/corpus/SUMMARY.md` lists the pages off, worst first;
`<page>.png` is gs | pdfium | diff of the worst plate.

## Results (1168 pages)

| status | first run | 2026-11-19 (1) | 2026-11-19 (2) | now |
|---|---|---|---|---|
| same | 182 | 191 | 363 | 731 |
| near (off by <= 8, or < 0.1% of the page more) | 209 | 233 | 290 | 159 |
| edges (off only along glyph and stroke edges) | 547 | 566 | 373 | 141 |
| off | 184 | 95 | 92 | 50 |
| group-cs (gray / non-isolated RGB groups left) | — | 48 | 15 | 15 |
| gs-bug (gs wrong, pdfium followed) | — | — | 36 | 36 |
| size (/UserUnit) | 4 | 0 | 0 | 0 |
| missing plate | 1 | 0 | 0 | 0 |
| timeout (crashes show as hangs here) | 9 | 4 | 4 | 4 |
| pdfium can't open | 3 | 2 | 2 | 2 |
| gs errors (password, broken) | 28 | 28 | 28 | 28 |

gs runs with `-dNONATIVEFONTMAP`: without it gs takes fonts not embedded
from the machine's own fonts (fontconfig), so its plates depend on what is
installed; with it gs uses only its own font set, which pdfium follows
(`--gsfonts`). Each run prints the pages that changed status since the last
(`results.prev.json` keeps the previous results).

The scripts pass the bench gs's default ICC profiles (`--profiles`,
`.cache/gs`) and gs's share directory (`--gsfonts`, of the gs on PATH).

## Fixed (each with a cell in separations' 18-corpus or a probe repro)

- JPX with no /ColorSpace on a plate: palette read out of bounds (hang).
- `CPDF_GeneralState::GetOPMode` on an empty state: crash (pattern and
  Type3 text; upstream).
- `CPDF_ShadingPattern::Load` took an invalid shading as loaded the second
  time the page was loaded (painted black; upstream).
- ICC RGB with no output intent: uncalibrated, as gs -dUseFastColor takes
  every non-CIE source (`gsicc_nocm_get_link`).
- spots in resources the page inherits from the page tree were not found.
- /UserUnit: `PDFKING_GetPageUserUnit`; the caller scales by it.
- printable form fields (Print flag) are drawn when printing with
  FPDF_ANNOT, in /Annots order with the other annotations, as gs prints.
- ColorBurn: gs's formula (a white backdrop stays white).
- Indexed: an index off the table clamped, a fill's rounded, an image's
  truncated, as gs.
- Tiling patterns as gs tiles them (`cpdf_gstiling.*`, ported from
  gsptype1.c / gxp1fill.c): the cell at gs's size, matrix (ADJUST_AS_ADOBE
  bend) and pixel origin, its painted pixels copied at gs's whole-pixel
  steps into the object's gs fill or stroke; text with a pattern in its gs
  glyph pixels; images in a cell decoded whole (no JPEG downscale to the
  small cell device) and grid fit as gs's pattern accumulator does; a
  pattern fill's clip as gs's (`19-patterns`).
- sampled functions (Type 0) with several inputs: multilinear, as gs
  (upstream added per-input deltas).
- uncolored tiling patterns: the cell in the color given, through the
  pattern space's base (was the RGB value on every plate).
- transfer functions: per plate, 1 - f(1 - ink), C, M, Y by the red, green,
  blue functions, K and spots by the gray one; images too.
- a colored Type3 glyph (d0) at ca < 1 was painted opaque (upstream).
- non-isolated groups with a blend mode (Multiply in Multiply: InDesign
  shadows and feathers): the backdrop taken out before blending
  (ISO 32000-2 11.4.8), was applied twice (Altona Q).
- a group mixed back with a soft mask and opacity could wrap a plate to
  full ink (Altona F); an overprinting image in an isolated group read the
  group's untouched (transparent) pixels as full ink (Altona K).
- Lab images on plates: components scaled to their ranges (L 0..100).
- plates_bench reads /BrotliDecode (pdfium_test does).

- RGB blending groups as gs: the page group and isolated form groups in
  R, G, B, then to CMYK through gs's default profiles
  (`PDFKING_SetBlendProfiles`).
- text as gs renders it: FreeType sized and transformed as gs's FAPI
  (fixed-point decomposition at the render's dpi), TrueType hinted for
  mono (`FT_LOAD_TARGET_MONO`), bitmaps up to gs's cache limit, origins
  rounded as gs's current point, no glyph spacing adjustment; Type 3
  glyphs that take their color drawn from a pixel-aligned origin.
- fonts not embedded (or broken) loaded as gs loads them: Fontmap.GS, the
  base-14 names, the descriptor's flags, from gs's own font directory
  (`PDFKING_SetGsFontDir`).
- Lab without an output intent through gs's default CMYK profile.
- aliased fills: any area of a pixel at all paints it.
- fills, strokes and clip paths through gs's own code, ported (AGPL):
  `core/fxge/gs_scanconvert` (gxscanc.c edge buffer, its trapezoid variants
  and accurate-curve flattening, fill adjust 0.5) and `gs_stroke`
  (gxstroke.c: joins, caps, dashes, stroke adjust below 150 dpi, thin
  lines); path points as gs's path operators make them (float casts,
  1/256 rounding); rectangle clips to gs's pixel box.
- glyph outlines (stroked text, uncached large glyphs, text on transparency
  pages) as gs's FAPI hands them over (26.6 to 1/256, conics to cubics with
  gs's integer-pixel start), filled by the ported converter at fill adjust 0.
- text as gs's FAPI loads it: TrueType through FreeType's v35 interpreter
  with FT_LOAD_MONOCHROME and no target (was TARGET_MONO: the label text of
  the pdf.js annotation pages); a glyph gs does not cache (its box with the
  FontBBox over the 10000 byte cache limit) placed by its page coordinates
  truncated; Type 3 glyphs at gs's current point, their images through gs's
  image code; text clips (Tr 4-7) from gs's outlines, flattened and filled
  at fill adjust 0; substituted CIDFonts mapped as pdfi (ToUnicode, else the
  CID's Unicode, the space's glyph when none); no fixed pitch guessed from
  equal widths, no Chinese-named TrueType taken as CID; an embedded TrueType
  gs rejects (gs_type42_font_init's reads) substituted; ExtGState /Font by
  its dictionary; "1e2" numbers and run-together operators ("ETBT") as pdfi.
- text positions as gs's current point: each glyph's width in device 1/256
  pixels truncated (gs_distance_transform2fixed), TJ kerns exact, a show
  with no Tm/Td/T* before it starting where the last one left gs's point;
  /Widths and /W with their fractions, CID widths W/1000 through Trm x CTM,
  simple fonts with no /Widths by the font program's advance.
- shadings (types 1-3) through gs's patch subdivision, ported
  (`core/fxge/gs_shade`, gxshade1/6.c, linear-color trapezoids); ink to
  16 bits as gs, `(frac << 1) + (frac >> 11)`.

- transparency on the page as gs's page group (transparent until put on
  white): groups with a blend mode or soft mask and nonseparable blends
  blend with nothing where nothing was painted (`PageBackdrop`, then
  `Flatten`); soft masks rasterized as gs (no antialiasing, shadings
  through gs_shade on a gray device); a group rendered as a backdrop for
  an object in it (GetBackdrop) is not composited yet.

## Left, by cause

- images: /Interpolate on images rotated or skewed (gs does not
  interpolate them: the placement port's ±1) and downscaled ones (±1 in a
  few pixels); a JPX whose samples end before the image (gs leaves the rest
  unpainted, pdfium paints it 0: jpx_subsampling); a /SMaskInData 2 pixel
  (jpx_smaskindata).
- mesh shadings (types 4-7): pdfium's own path (gs's patch core is ported,
  its mesh stream readers not yet).
- group-cs (15 pages): gray page groups, non-isolated RGB form groups, and
  CMYK content in RGB groups (gs bug).
- text: Type 1 widths with fractions in the font program when /Widths is
  missing (FreeType rounds them).
- gs wrong or a broken file (follow pdfium): JBIG2 refinement/intermediate
  regions jbig2dec can't decode, Brotli streams, JPX variants gs garbles, a
  JPEG with RGB component ids, stitching functions out of domain, soft masks
  in Type3 glyphs, a Type1 with two Subrs, PostScript functions that
  underflow (bug2086012), pattern clip state.
- transparency: an object with an opacity or soft mask (no group) where
  the page is untouched rounds as over white (gs: over nothing, then put
  on white, off by 1); groups whose objects have opacity there likewise;
  images in soft masks are not sampled as gs (bug1755507); gs applies the
  transfer function to `sh` and not inside colored tiling patterns
  (transfer_maps), pdfium the other way round.
- still pdfium: Tx/Ch fields and Polygon/PolyLine annotations with no
  appearance (gs draws one); widgets missing /FT or /T (gs skips them);
  transfer functions on a page with transparency (gs flips the mapping).
- images, fixed: on a plate DCT and JPX decoded at full resolution (as gs:
  issue12841, perf_codecs_jpx); /Interpolate (11-images, issue9713, images,
  pr12564); JPX samples as gs reads them (`CJPX_Decoder::UseGsSamples`:
  subsampled components repeated, no YCC guess, alpha left out, a stream of
  other than the space's components or the image's size read on as bytes:
  jpx_numcomps, which crashed, jpx_subsampling); a bare JPX codestream with
  no /ColorSpace or with /SMaskInData not drawn (gs's header scan fails).
