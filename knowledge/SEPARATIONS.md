# Plan: a separation renderer (one pass, every plate, a real compositor)

Today: one render per plate, 8-bit gray, color forced per object to that
plate's ink (`CPDF_Plate`, `PDFKING_SetPlate`), pdfium's RGB/gray
compositor doing the rest. Exact for everything separable; wrong where a
plate depends on another ink (non-separable blend modes, see
`COMPOSITOR.md`), and N renders per page.

Goal: the caller names the separations it wants; one render collects all
of them. Every color is an array of N ink values, N = the separations asked
for, fixed for the render; bitmaps are N planes. No RGB anywhere on the
ink path; the API asks for separations, gets gray plates back.

A plate does not depend on which others are asked: every colorant a
Separation/DeviceN names is treated as a plate of its own (as per plate
today), asking stores only the requested ones. So Cyan alone == Cyan out of
CMYK + spots, and the caller can render the job's inks in one go or split
them.

gs source for reference: `gs/source.sh` (cached in `.cache/gs`). File:line
refs below are ghostscript-10.08.0.

## Status (2026-11-19)

Done, first stage: one render a band to every plate asked,
`PDFKING_LoadInks` / `PDFKING_RenderInks` / `PDFKING_CloseInks`
(`plates_bench --ink`, `separations/run.py --ink`). Byte-identical to the
per-plate renders on the whole separations suite (81 plates, same
`SUMMARY.md` against gs) and the 6 GWG pages; a plate alone == the same
plate among all; as fast or faster than per plate, banded.

How: `CPDF_Inks` (`core/fpdfapi/page/cpdf_inks.*`) holds the plates and the
render's colors, each an array of per-plate inks reaching the device as a
handle (opacity, tag, 16-bit index); `CPDF_InkDriver`
(`core/fpdfapi/render/cpdf_inkdriver.*`) draws paths, rects, lines, glyph
masks and clips on all planes at once and refuses anything that is not a
handle (the render returns false). What the ink device cannot draw at once
(images, shadings, patterns, Type3 text, soft masks, blends, opacity on
fill + stroke, isolated/knockout groups, a color whose opacity differs
between plates: overprint) the render status draws plane by plane, each as
its plate (`CPDF_RenderStatus::InkPlanes`), with today's per-plate code:
which is why it is exact. Images are decoded per plate, cached per plate
across bands.

Second stage (2026-11-18), gs parity for what per plate could not do,
each with its suite page (`separations/make.py` 15, 16, 17):

- Nonseparable blends (Hue, Saturation, Color, Luminosity): the object's
  layer made on each process plate (`KeepInkLayer`), then C, M, Y blended
  together and K by gs's rule, a port of gs's 8-bit math
  (`cpdf_inkblend.*`); spots Normal, in a knockout group as K (gs's
  knockout path blends every channel). The inks always render the four
  process plates when the page blends so (`CPDF_Inks`), so a plate stays
  the same whichever are asked. Knockout groups and groups mixed onto the
  page (opacity, soft mask) are drawn on all planes at once; an isolated
  group with such a blend inside on an ink device of its own (`InkGroup`).
  15-nonseparable (608 cells, every mode plain, ca .5, in groups of each
  kind, the group's own mode) and GWG 16.0, 16.1, 16.2, 16.4 match gs.
- Overprint in a group (CompatibleOverprint): an object overprinting a
  plate it leaves still gives the group its shape and opacity, the
  backdrop beneath (`PlateOverprintsBeneath`), on plates too. 16 matches.
- Spot discovery: `PDFKING_GetPageInks` (`plates_bench ... auto`).
- SoftLight, ColorDodge as gs rounds them; soft masks under a moved CTM
  (an upstream bug: the inherited mask placed twice) and a form that is no
  group passing its mask to its objects alone; a soft mask's colors and
  opacity quantized as gs does. 09 matches but for one cell.

Third stage (2026-11-19), real PDFs: `corpus/` runs ~1170 public pages
(Altona, PDF Association, pdf.js) through gs and the ink render; 184 pages
off became 95 (+48 set apart: groups blending in RGB/gray, which gs
converts through and pdfium does not). Fixed: crashes (JPX with no color
space, GetOPMode, invalid shading reloaded), ICC RGB without an output
intent as gs -dUseFastColor, spots in inherited resources, /UserUnit
(`PDFKING_GetPageUserUnit`), printable form fields, ColorBurn, Indexed
index rules, multi-input sampled functions, uncolored tiling patterns,
transfer functions per plate, Type3 d0 opacity, Multiply-in-Multiply groups
(backdrop removal, `BlendGroup`), Lab images. Each has a cell in
`separations/make.py` 18-corpus (byte-identical to gs); the rest is sorted
by cause in `corpus/README.md`.

Left (all in `separations/README.md`): text (gs hints glyphs through its
character cache), shadings' steps, Lab without an output intent, gs's own
bugs; a nonseparable blend in a non-isolated group with a blend mode of its
own is still per plate. Not parity, cost: images decoded per plate; groups
other than the above still drawn plane by plane (exact, slower).

## How gs does it (tiffsep)

1. **Device**: planar, one 8-bit plane per component, stored as ink
   (0 = none). CMYK first, spots after in order found, MaxSpots 10, hard
   cap 64 (`gdevdevn.c:284-388`, `gdevppla.c:40`). Fills carry a DeviceN
   color (16-bit value per component); `mem_planar_fill_rectangle_hl_color`
   (`gdevmpla.c:308`) writes each plane. Plates out as `255 - ink`
   (`gdevtsep.c:2341`).
2. **Spot discovery**: a pre-scan of the page's resources (ColorSpace,
   Shading, XObject, Pattern, Font) sets `PageSpotColors` before rendering
   (`pdf/pdf_check.c:1545-1660`).
3. **Color**: a Separation/DeviceN whose colorants the device has paints
   them directly, all other planes 0 (`cmap_separation_direct`,
   `cmap_devicen_direct`, `gxcmap.c:1434,1660`); one missing name -> the
   whole space through its alternate. `/All` every plane, `/None` nothing.
   RGB/Gray -> CMYK planes via ICC (fast color opt-in).
4. **Overprint**: native on a separation device, not simulated:
   `drawn_comps` = the planes this color paints (OPM 1: the CMYK
   components > 0); the overprint compositor rewrites only those
   (`gsovrc.c:1040`). Inside transparency: `CompatibleOverprint` blend,
   B = Cs on drawn planes, Cb elsewhere (`gxblend.c:1257-1345`).
5. **Transparency (pdf14)**: planar group buffers, `n_comp + alpha
   (+shape, alpha_g)`, blending space CMYK + spots. Blends in additive form
   (`255 - ink`) per pixel. Separable modes per plane; non-separable on CMY
   as RGB, K backdrop's (Hue/Sat/Color) or source's (Luminosity), spots
   Normal (`gxblend.c:53, 651, 1016, 1294`). Knockout/isolated/soft-mask
   multiply in `template_compose_group` (`gxblend.c:2773-3010`). Soft mask
   luminosity from the mask group into one gray plane
   (`gxblend.c:73-190`). At the end the page group is blended over white
   and copied into the planes (`gdevp14.c:3115-3590`).
6. **Rasterization**: no antialiasing by default (any-part-of-pixel), one
   solid value per pixel; coverage only enters as alpha inside pdf14.

Essential gs code ~8k lines (8-bit gxblend ~2.5k, pdf14 core ~3.5k, devn,
cmap, overprint); clist, 16-bit, composite preview not needed.

## What pdfium has and lacks

| piece | pdfium | gap |
|---|---|---|
| bitmap | gray, BGR(x/a), masks (`FXDIB_Format`) | no N-plane ink bitmap |
| color in the pipeline | `FX_ARGB` everywhere (~180 uses: render, page, fxge) | no N-ink color |
| color spaces | `GetRGB` only; ours: `CPDF_Plate::Ink` (one ink, ICC, overprint, intent) | all inks at once (small: loop the plates) |
| rasterizer | agg, coverage spans, aliased mode (what we use) | reusable as is |
| compositor | `CFX_ScanlineCompositor` (2.7k) gray/RGB, non-separable in RGB | N planes + alpha, CMY non-separable, spots Normal, overprint |
| groups, knockout, smask | `ProcessTransparency` renders to an offscreen BGRA, composites back | offscreen must be N-plane |
| overprint | none (ours fakes it per plate) | drawn-planes mask |
| spot discovery | none (callers pass names, bench takes them from gs) | page pre-scan |
| output | `FPDF_RenderPageBitmap` (one bitmap) | API returning N planes |

## Design

Keep pdfium's interpreter and render status; replace the device under it.

1. **`CFX_InkBitmap`** (new): N 8-bit planes stored as ink + an optional
   alpha plane, the ink names, a band rect. `core/fxge/ink/`.
2. **`CFX_InkDriver : RenderDeviceDriverIface`** (new): fills, strokes,
   rects, glyph masks, images, clips; agg's rasterizer for coverage
   (aliased, gs's rule), writes each plane. Groups are just more
   `CFX_InkBitmap`s with alpha.
3. **Colors are ink arrays**: `CPDF_Inks` = the N requested names;
   `CFX_InkColor` = `uint8_t ink[N]` + `drawn` mask (overprint) + alpha.
   `CPDF_Inks::Color(cs, comps, overprint, opm, intent)` fills all N at
   once (today's `CPDF_Plate::Ink` per name: ICC, spots, `/All`, `/None`).
   Transport to the device, two ways:
   - *handle* (recommended): the color slot pdfium already threads
     (`FX_ARGB`: fill/stroke, text, Type3, patterns, image masks) carries
     alpha + an index into the render's table of `CFX_InkColor`; nothing
     reads it as RGB, only the ink driver dereferences it. No upstream
     signature changes. Risk: upstream code making or changing a color on
     the way down without the hook (audit in step 1). Leaks are rejected:
     a handle is alpha, a tag byte, a 16-bit index into the render's
     deduplicated table; the ink driver fails the draw on a wrong tag or
     an index past the table, and `PDFKING_RenderInks` returns an error
     (debug builds: `CHECK`, naming the call). No guessing, no RGB
     fallback.
   - *typed*: `CFX_RenderDevice` / driver / text renderer take a
     `DeviceColor` (ARGB or ink array). Cleanest, but ~150-200 changed
     upstream lines in ~15 files, every rebase.
   Same data model either way: one fixed-size ink array per color.
4. **Images and shadings**: `CPDF_DIB` decodes straight to the N
   planes (`CPDF_Inks` per pixel row, no RGB step), handed to the driver as
   an `CFX_InkBitmap`; shadings evaluate the function, then `Color` to N
   inks per pixel.
5. **Compositor** (`core/fxge/ink/ink_compositor.cpp`): the 8-bit
   gxblend subset: separable blends per plane (reuse `blend.cpp`),
   non-separable on CMY (reuse pdfium's `RgbBlend` on `255 - ink`) + K
   rule, spots Normal, opacity, shape, knockout, isolated, soft-mask
   multiply, `CompatibleOverprint` with the drawn mask.
6. **Offscreens**: the ~40 sites that make an offscreen device or composite
   a bitmap (`cpdf_renderstatus.cpp` 23, `cpdf_imagerenderer.cpp` 10,
   `cpdf_rendertiling.cpp` 4, `cpdf_devicebuffer.cpp` 3,
   `cpdf_scaledrenderbuffer.cpp` 1) get the ink variant from one factory
   hook: same-kind-as-parent device.
   Soft masks too: a luminosity mask group is rendered on an ink device
   whose channels are its group color space (`/CS` Gray: 1, RGB: 3, CMYK
   or none: C, M, Y, K; spots ignored, as gs), then reduced to one gray
   plane with the spec's formula for that space (gs
   `do_smask_luminosity_mapping`, `gxblend.c:73-190`; CMYK:
   `(.3(1-C) + .59(1-M) + .11(1-Y))(1-K)`). Alpha masks: the alpha plane.
   So the ink device is generic N-channel (subtractive inks, or additive
   Gray/RGB for masks); pdfium's RGB compositor is not used on the ink
   path at all.
7. **API**: `PDFKING_RenderInks(page, matrix, clip, names, n, planes[],
   stride, flags)`: the separations asked for in, one band of each out
   (`255 - ink`, 8-bit gray). A name not used on the page comes back
   white (or `/All` marks).
   No composite: nothing on the ink path makes an RGB preview. pdfium's
   own `FPDF_RenderPageBitmap` stays untouched for normal viewing (no
   overprint, no separation semantics). A separation-true preview, if
   wanted later, is a function of the plates: each ink's equivalent CMYK
   (its alternate at tint 1, gs `gsequivc.c`) mixed, then output ICC to
   RGB; ~100-150 lines, or done by the caller.
8. **Discovery** (helper, optional): `PDFKING_PageInks(page)` lists the
   colorants a page uses (resources, recursively through forms, patterns,
   Type3 fonts, annotation APs), for callers that don't know the job's
   inks. `PDFKING_SetPlate` and the
   per-plate machinery (`cpdf_renderstatus_plate.cpp`, `PlateArgb`,
   `CPDF_PlateCS`) go once the suite passes.

Size (C++, ours unless marked; anchors: agg driver 1425, scanline
compositor 2698, `cpdf_plate.cpp` 427, `cpdf_rendershading.cpp` 1138):

| piece | lines |
|---|---|
| `CPDF_Inks` + `CFX_InkColor` (from `CPDF_Plate::Ink`, all N at once) | 300-400 |
| `CFX_InkBitmap` (planes, alpha, band) | 120-180 |
| `CFX_InkDriver` paths/strokes/rects (agg spans, clip, drawn mask) | 450-600 |
| `CFX_InkDriver` glyph masks, image masks | 100-150 |
| `CFX_InkDriver` images (stretch/transform per plane via pdfium's 8bpp gray stretcher) | 250-350 |
| `CPDF_DIB` -> N planes | 200-300 |
| shadings -> N planes (axial, radial, function, meshes) | 250-400 |
| tiling patterns | 60-100 |
| compositor (groups, separable, CMY non-separable + K, spots Normal, knockout, isolated, smask, CompatibleOverprint) | 800-1100 |
| soft mask on the ink device (group space channels, luminosity -> gray) | 80-150 |
| offscreen factory + group plumbing on our side | 200-300 |
| `PDFKING_RenderInks` + band output | 100-150 |
| `PDFKING_PageInks` discovery | 200-300 |
| **new total** | **~3.1-4.5k** |
| upstream hooks: handle / typed | 40-60 / 150-200 |
| removed at the end: per-plate code + its hooks | -700 / -30 |
| **net** | **~2.5-3.8k** |

Tests (Python): `make.py` pages 15-18 ~300-450, bench `--ink` + diff
~80, subset invariance in `run.py` ~60.

Speed: 1 render instead of N (a page with CMYK + 2 spots: 6 -> 1),
memory N bytes/pixel per band.

`COMPOSITOR.md` (option A) becomes unnecessary; it stays the cheap fix if
this is not done.

## Testing

Oracles, strongest first:

1. **Today's per-plate renderer** (kept until the end): on every
   separable page it is already gs-exact, so the ink renderer must be
   byte-identical to it: separations suite (`separations/run.py`, all
   cells) and the 6 GWG pages, plate by plate. A differential check
   `--ink` vs `--plate` in `plates_bench`; any diff is a bug of the new
   code until shown otherwise.
2. **gs `tiffsep`** (as now): the reference where per-plate is wrong:
   non-separable modes, overprint inside groups. Drop
   `-dOverprint=/simulate` (tiffsep does overprint natively; check the
   renders are unchanged first).
3. **GWG reference** (`gwg/gwg.py`): crosses gone in 16.0-16.4; the
   overprint and spot patches as now.
4. New `make.py` pages, each a topic:
   - `15-nonseparable`: four modes x backdrops (CMYK, RGB, spot) x
     plain/non-isolated/isolated/knockout groups, opacity .5, spot sources;
   - `16-overprint-groups`: OP/OPM 1 inside transparency groups;
   - `17-spot-discovery`: spots only in a pattern, a Type3 glyph, a
     shading, a nested form, an annotation; > 10 spots;
   - `18-bands`: a page split in bands = the page whole;
   - subset invariance: each plate rendered alone == the same plate out of
     all of them in one call (every suite page, cheap to run).
5. **Speed/memory**: `plates_bench` timing of both renderers on GWG and
   the suite; the ink one must not be slower in total.

## Steps (each lands with the suite green)

1. Audit: every place an `FX_ARGB` is changed between the render status
   and the device (handle safety); list the offscreen sites. Write
   `15`-`18` in `make.py`, record failures with today's renderer.
2. `CPDF_Inks`, `CFX_InkColor`, `CFX_InkBitmap`, `CFX_InkDriver` (paths,
   rects, text, clips), `PDFKING_RenderInks`, `--ink` in the bench. Pages without images,
   shadings, transparency: identical to per-plate.
3. Images, image masks, shadings, tiling patterns: identical.
4. Compositor, separable: groups, opacity, soft masks (on the ink
   device), knockout: identical, except soft masks with a CMYK group space
   (now the CMYK formula, as gs): gs-identical.
5. Non-separable, `CompatibleOverprint`: gs-identical on `15`, `16`;
   GWG 16.x crosses gone.
6. `PDFKING_PageInks`; switch the bench and rotoapps'
   caller; delete the per-plate code and `PDFKING_SetPlate`.
