// pdfking: a shading on a plate of an ink render as Ghostscript 10.08 fills
// it, through gs_shade (core/fxge/gs_shade.h), with the shading's function's
// monotonicity as gs's functions give it (ported from Ghostscript 10.08
// base/gsfunc0.c, base/gsfunc3.c, base/gsfunc4.c).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file (and the matching
// .cpp) is AGPL code, not under the PDFium BSD license.

#ifndef CORE_FPDFAPI_RENDER_CPDF_GSSHADING_H_
#define CORE_FPDFAPI_RENDER_CPDF_GSSHADING_H_

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"

class CFX_DIBitmap;
class CPDF_ColorSpace;
class CPDF_Dictionary;
class CPDF_Inks;
class CPDF_Plate;
class CPDF_ShadingPattern;

// Whether DrawGsShading() fills `pattern`'s shading type.
bool GsFillsShading(const CPDF_ShadingPattern* pattern);

// On inks, `pattern`'s shading (`dict`, in its own space `cs`; types 4 to 7
// read from its stream) on `plate` (null: in color, as gs's soft mask, a
// gray device, takes it)
// as gs fills it: its patch decomposition, linear color trapezoids and
// device color quantization, the ink of every plate of `inks` the device
// color gs subdivides by. `bitmap` covers `rect` of the device `matrix`
// maps to; `box` is gs's clip box; `intent` the rendering intent, `alpha`
// the opacity the pixels get.
void DrawGsShading(const RetainPtr<CFX_DIBitmap>& bitmap,
                   const FX_RECT& rect,
                   const FX_RECT& box,
                   const CFX_Matrix& matrix,
                   const CPDF_ShadingPattern* pattern,
                   const CPDF_Dictionary* dict,
                   const RetainPtr<CPDF_ColorSpace>& cs,
                   const CPDF_Inks* inks,
                   const CPDF_Plate* plate,
                   int intent,
                   int alpha);

#endif  // CORE_FPDFAPI_RENDER_CPDF_GSSHADING_H_
