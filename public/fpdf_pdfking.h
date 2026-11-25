// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: the fork's own API, separations (press plates) as gs renders
// them. Nothing here is upstream pdfium's.

#ifndef PUBLIC_FPDF_PDFKING_H_
#define PUBLIC_FPDF_PDFKING_H_

// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

typedef struct pdfking_inks_t__* PDFKING_INKS;

#ifdef __cplusplus
extern "C" {
#endif

// pdfking: render `document`'s pages as the press plate `plate` (Cyan,
// Magenta, Yellow, Black or a spot's name), gray from white (no ink) to
// black (full ink), overprint honored; NULL or "" to render in color. Set
// before any page is rendered.
FPDF_EXPORT void FPDF_CALLCONV PDFKING_SetPlate(FPDF_DOCUMENT document,
                                                FPDF_BYTESTRING plate);

// pdfking: the spot inks `page` paints, as gs finds them: the colorants of
// the Separation and DeviceN spaces its resources (and forms', patterns',
// shadings', images', Type3 fonts', annotation appearances') name, but the
// process inks, All and None, in the order found. Each name ends in a NUL,
// written to `buffer` if `length` holds them all. Returns the bytes they
// take, 0 for none.
FPDF_EXPORT unsigned long FPDF_CALLCONV
PDFKING_GetPageInks(FPDF_PAGE page, char* buffer, unsigned long length);

// pdfking: the separations `names` (Cyan, Magenta, Yellow, Black or spots'
// names), `count` of them, that `page` renders to in one go, its images
// decoded for each kept across renders (bands). NULL on a bad argument.
// Close it with PDFKING_CloseInks() before the page.
FPDF_EXPORT PDFKING_INKS FPDF_CALLCONV
PDFKING_LoadInks(FPDF_PAGE page, const FPDF_BYTESTRING* names, int count);

// pdfking: render `page` once to `inks`, each onto its 8-bit gray bitmap in
// `planes` (FPDFBitmap_Gray, one size, as many as inks, filled white by the
// caller) from white (no ink) to black (full ink), overprint honored, placed
// as FPDF_RenderPageBitmapWithMatrix() places the page. A plate is the same
// whichever others are asked. Returns false on a bad argument or when the
// render went wrong (the planes are then not to be used).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV PDFKING_RenderInks(PDFKING_INKS inks,
                                                       FPDF_PAGE page,
                                                       FPDF_BITMAP* planes,
                                                       const FS_MATRIX* matrix,
                                                       const FS_RECTF* clipping,
                                                       int flags);

// pdfking: the RGB and CMYK ICC profiles (gs's iccprofiles/default_rgb.icc
// and default_cmyk.icc) through which a page whose transparency group
// blends in RGB goes to the process plates, as gs separates it: such a page
// (with transparency in its resources) renders its process colors in RGB,
// R, G, B blended, then each pixel to CMYK (to the output intent's profile
// when the document has one). Isolated form groups blending in RGB, or in
// DeviceGray through `gray` (default_gray.icc), the same way. Without them,
// or NULL, the page blends on the plates. Set before PDFKING_LoadInks().
FPDF_EXPORT void FPDF_CALLCONV
PDFKING_SetBlendProfiles(FPDF_DOCUMENT document,
                         const unsigned char* rgb,
                         unsigned long rgb_size,
                         const unsigned char* cmyk,
                         unsigned long cmyk_size,
                         const unsigned char* gray,
                         unsigned long gray_size);

// pdfking: fonts a PDF does not embed (or embeds broken) loaded as gs
// loads them, from gs's resources at `dir` (its share/ghostscript, holding
// Resource/Init/Fontmap.GS and Resource/Font): BaseFont through Fontmap.GS,
// then pdfi's base font name for it, then one chosen by the descriptor's
// flags; pdfium's own glyph width fitting off. NULL or "" for pdfium's own
// substitution. Process-wide; set before documents are loaded.
FPDF_EXPORT void FPDF_CALLCONV PDFKING_SetGsFontDir(FPDF_BYTESTRING dir);

// pdfking: release `inks`.
FPDF_EXPORT void FPDF_CALLCONV PDFKING_CloseInks(PDFKING_INKS inks);

// pdfking: the size of `page`'s unit in points, its /UserUnit (1 when it
// has none or a bad one): a page FPDF_GetPageWidthF() says is w wide prints
// w * unit points wide, as gs sizes it. Scale the matrix by it.
FPDF_EXPORT float FPDF_CALLCONV PDFKING_GetPageUserUnit(FPDF_PAGE page);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PUBLIC_FPDF_PDFKING_H_
