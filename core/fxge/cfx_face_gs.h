// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FXGE_CFX_FACE_GS_H_
#define CORE_FXGE_CFX_FACE_GS_H_

// pdfking: glyphs loaded as gs's FAPI loads them, for CFX_Face
// (cfx_face_gs.cpp).

#include <stdint.h>

#if defined(PDF_ENABLE_FREETYPE)
#include "core/fxge/freetype/fx_freetype.h"

// The glyph loaded as gs loads it (fapi_ft.c FF_get_glyph): TrueType hinted
// by FreeType's v35 interpreter, Type 1 and CFF unhinted.
FT_Error GsLoadGlyph(FT_FaceRec* rec, uint32_t glyph_index, bool truetype);

// `*matrix` (the glyph to device, in 16.16 of a 64 pixel em) decomposed as
// gs sizes FreeType for it at `dpi`: the face's char size set, the rest of
// the transform left in `*matrix`.
void GsDecompose(FT_FaceRec* rec, float dpi, bool truetype, FT_Matrix* matrix);
#endif  // defined(PDF_ENABLE_FREETYPE)

#endif  // CORE_FXGE_CFX_FACE_GS_H_
