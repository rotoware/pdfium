// pdfking: where Ghostscript puts an image's samples on the device, ported
// from Ghostscript 10.08 base/gxipixel.c (gx_image_enum_begin: the image
// matrix, its fixed point extents and DDAs), base/gxidata.c (each row's
// device rows or columns) and the renderers that place the samples of a
// row: base/gxicolor.c (image_render_color_DeviceN), base/gximono.c
// (image_render_mono) and base/gxifast.c (image_render_simple,
// image_render_landscape); and for an image gs interpolates base/gxiscale.c
// and base/siscale.c (the Mitchell filter).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file (and the matching
// .cpp) is AGPL code, not under the PDFium BSD license.

#ifndef CORE_FXGE_GS_IMAGE_H_
#define CORE_FXGE_GS_IMAGE_H_

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/dib/fx_dib.h"

class CFX_DIBBase;
class CFX_DIBitmap;

namespace gs_image {

// `source` (row 0 the image's first, its top) drawn by `matrix` (the unit
// square to the device, as pdfium's image matrix) with its samples placed
// as gs places those of an image of `samples` (drawn in a soft mask, as
// pdfi draws an image's /SMask, when `soft_mask`): in `*bitmap`, of `source`'s
// format, at `*left`, `*top`, each device pixel the image paints within
// `clip` the sample gs paints there (a skewed image's with alpha: BGRA, or
// a mask); none when it paints none there. With `interpolate` (/Interpolate)
// its samples smoothed as gs's Mitchell scaler smooths them (gxiscale.c,
// siscale.c) where gs does so, `source` the gray of a plate (the samples of
// an `additive` space, else the ink: inverted) or a soft mask.
void Draw(RetainPtr<const CFX_DIBBase> source,
          const CFX_Matrix& matrix,
          FXDIB_GsSamples samples,
          bool soft_mask,
          bool interpolate,
          bool additive,
          const FX_RECT& clip,
          RetainPtr<CFX_DIBitmap>* bitmap,
          int* left,
          int* top);

}  // namespace gs_image

#endif  // CORE_FXGE_GS_IMAGE_H_
