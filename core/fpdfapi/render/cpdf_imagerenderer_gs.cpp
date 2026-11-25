// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CPDF_ImageRenderer's images on a plate as gs places them, kept
// out of cpdf_imagerenderer.cpp so upstream's changes to it merge cleanly.
//
// GsPatternCellImage() is ported from Ghostscript 10.08 base/gxipixel.c:
// Copyright (C) 2001-2025 Artifex Software, Inc., licensed under the GNU
// Affero General Public License (AGPL) v3, not under the PDFium BSD license.

#include <stdint.h>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_indexedcs.h"
#include "core/fpdfapi/page/cpdf_inks.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/render/cpdf_imagerenderer.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"

namespace {

// pdfking: how gs tells apart the samples of the image `dict`, of
// `components` components, when it places them (gs_image::Draw)
FXDIB_GsSamples GsSamples(const CPDF_Dictionary* dict, uint32_t components) {
  if (dict->GetBooleanFor("ImageMask", false)) {
    return FXDIB_GsSamples::kMask;
  }
  if (components > 1) {
    return FXDIB_GsSamples::kColor;
  }
  return dict->GetIntegerFor("BitsPerComponent") == 1
             ? FXDIB_GsSamples::kMonobit
             : FXDIB_GsSamples::kMono;
}

// pdfking: whether gs interpolates the samples of `cs` (an Indexed one's
// base's) as the light they let through: gray, RGB and Lab, a JPX's own
// (null: RGB, gray); else (CMYK, Separation, DeviceN) as their ink
bool IsAdditive(const CPDF_ColorSpace* cs) {
  if (cs && cs->AsIndexedCS()) {
    cs = cs->AsIndexedCS()->GetBaseCS();
  }
  if (!cs) {
    return true;
  }
  switch (cs->GetFamily()) {
    case CPDF_ColorSpace::Family::kDeviceGray:
    case CPDF_ColorSpace::Family::kDeviceRGB:
    case CPDF_ColorSpace::Family::kCalGray:
    case CPDF_ColorSpace::Family::kCalRGB:
    case CPDF_ColorSpace::Family::kLab:
      return true;
    case CPDF_ColorSpace::Family::kICCBased:
      return cs->ComponentCount() != 4;
    default:
      return false;
  }
}

}  // namespace

// pdfking: in a tiling pattern's cell on inks, an image (not an image mask)
// as gs's pattern accumulator takes it (gx_image_enum_begin): orthogonal,
// grid fit, its edges out to whole pixels. (gs also interpolates one
// downscaled there, as it does an /Interpolate image: not ported.)
// Ported from Ghostscript 10.08 base/gxipixel.c (AGPL).
void CPDF_ImageRenderer::GsPatternCellImage() {
  if (!GetRenderOptions().GetOptions().bGsPatternCell ||
      !GetInks(render_status_->GetContext()->GetDocument()) ||
      image_object_->GetImage()->IsMask()) {
    return;
  }
  const int width = image_object_->GetImage()->GetPixelWidth();
  const int height = image_object_->GetImage()->GetPixelHeight();
  if (width <= 0 || height <= 0) {
    return;
  }
  CFX_Matrix& m = image_matrix_;
  // gs's image matrix: sample space (rows down) to device
  double xx = m.a / width, xy = m.b / width;
  double yx = -m.c / height, yy = -m.d / height;
  double tx = m.e + m.c, ty = m.f + m.d;
  constexpr double kFixed = 256.0;
  auto to_fixed = [](double v) {
    return static_cast<int64_t>(v * kFixed);
  };
  auto floor_px = [](int64_t f) { return (f >> 8) << 8; };
  auto ceil_px = [](int64_t f) { return ((f + 255) >> 8) << 8; };
  // one side: its start t and extent v over n samples, out to whole pixels
  auto fit = [&](double& t, double& v, int n) {
    if (v > 0) {
      const int64_t i0 = floor_px(to_fixed(t));
      const double t1 = t + v * n;
      const int64_t i1 = ceil_px(to_fixed(t1));
      t = i0 / kFixed;
      v = (i1 - i0) / kFixed / n;
    } else if (v < 0) {
      const int64_t i0 = ceil_px(to_fixed(t));
      const double t1 = t + v * n;
      const int64_t i1 = floor_px(to_fixed(t1));
      t = i0 / kFixed;
      v = (i1 - i0) / kFixed / n;
    }
  };
  if (xy == 0 && yx == 0) {
    fit(tx, xx, width);
    fit(ty, yy, height);
    m = CFX_Matrix(xx * width, 0, 0, -yy * height, tx, ty + yy * height);
  } else if (xx == 0 && yy == 0) {
    fit(tx, yx, height);
    fit(ty, xy, width);
    m = CFX_Matrix(0, xy * width, -yx * height, 0, tx + yx * height, ty);
  }
}

RetainPtr<CPDF_ColorSpace> CPDF_ImageRenderer::ImageColorSpace() const {
  RetainPtr<const CPDF_Dictionary> dict =
      image_object_->GetImage()->GetStream()->GetDict();
  RetainPtr<const CPDF_Object> cs_obj = dict->GetDirectObjectFor("ColorSpace");
  auto* data = CPDF_DocPageData::FromDocument(
      render_status_->GetContext()->GetDocument());
  RetainPtr<CPDF_ColorSpace> cs;
  if (cs_obj && render_status_->GetFormResource()) {
    cs = data->GetColorSpace(cs_obj.Get(), render_status_->GetFormResource());
  }
  if (cs_obj && !cs) {
    cs = data->GetColorSpace(cs_obj.Get(), render_status_->GetPageResource());
  }
  return cs;
}

FXDIB_GsSamples CPDF_ImageRenderer::ImageGsSamples() const {
  RetainPtr<CPDF_ColorSpace> cs = ImageColorSpace();
  return GsSamples(image_object_->GetImage()->GetStream()->GetDict().Get(),
                   cs ? cs->ComponentCount() : 3);
}

// gs draws a soft mask as a gray image of its own, a stencil mask as an
// image mask (of an image of type 3: not grid fit), and a color key mask
// leaves the image's samples as they are
FXDIB_GsSamples CPDF_ImageRenderer::MaskGsSamples() const {
  RetainPtr<const CPDF_Dictionary> dict =
      image_object_->GetImage()->GetStream()->GetDict();
  if (RetainPtr<const CPDF_Stream> smask = dict->GetStreamFor("SMask")) {
    return GsSamples(smask->GetDict().Get(), 1);
  }
  if (dict->GetStreamFor("Mask")) {
    return FXDIB_GsSamples::kMonobit;
  }
  return resample_options_.gs;
}

// pdfking: on a plate, the samples of an image where gs places them, as
// where pdfi draws it in a soft mask: its /SMask, and the image in the
// group gs pushes for it, or for a soft mask of the graphics state
// (`soft_mask_state`); with /Interpolate smoothed as gs does, but an image
// mask's (pdfi's own ImScaleDecode) and an image with a color key mask (gs
// does not)
void CPDF_ImageRenderer::SetGsResampleOptions(bool soft_mask_state) {
  RetainPtr<const CPDF_Dictionary> dict =
      image_object_->GetImage()->GetStream()->GetDict();
  RetainPtr<CPDF_ColorSpace> cs = ImageColorSpace();
  resample_options_.gs = ImageGsSamples();
  resample_options_.gs_soft_mask = soft_mask_state ||
                                   dict->GetStreamFor("SMask");
  resample_options_.gs_interpolate =
      image_object_->GetImage()->IsInterpol() &&
      resample_options_.gs != FXDIB_GsSamples::kMask &&
      !dict->GetArrayFor("Mask");
  resample_options_.gs_additive = IsAdditive(cs.Get());
}
