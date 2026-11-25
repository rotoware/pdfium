// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CFX_RenderDevice's glyphs as gs places and draws them, kept out
// of cfx_renderdevice.cpp so upstream's changes to it merge cleanly.

#include "core/fxge/cfx_renderdevice_gs.h"

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "core/fxcrt/span.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_glyphbitmap.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/gs_scanconvert.h"
#include "core/fxge/text_char_pos.h"

// pdfking: the pixel gs puts a glyph's origin `v` on: the current point in
// gs's fixed point (1/256, rounded), then rounded to a pixel
// (fixed2int_var_rounded)
int GsPixel(double v) {
  const double fixed = floor(v * 256.0 + 0.5);
  return static_cast<int>(floor((fixed + 128.0) / 256.0));
}

// pdfking: whether gs caches a glyph (gxchar.c set_cache_device,
// gxccman.c gx_alloc_char_bits): its box, the glyph's (`bitmap` at its
// origin) with the font's FontBBox (gxfapi.c: FreeType's, the face's) through
// `matrix` (an em to the device), each side in fixed point plus 3 pixels, a
// 1 bit bitmap of rows padded to 64 bits no taller than the cache's upper
// limit (10000 bytes, gs_init.ps setcacheparams) over its row size
bool GsCachesGlyph(const CFX_Face& face,
                   const CFX_Matrix& matrix,
                   const CFX_GlyphBitmap& bitmap) {
  const FX_RECT font_box = face.GetBBox();
  const uint16_t units = face.GetUnitsPerEm();
  // the glyph's box, device pixels from its origin
  const RetainPtr<const CFX_DIBitmap>& dib = bitmap.GetBitmap();
  double x0 = bitmap.top_left_origin().x;
  double x1 = x0 + dib->GetWidth();
  double y0 = -bitmap.top_left_origin().y;
  double y1 = y0 + dib->GetHeight();
  // (GetBBox: top the font's lowest, bottom its highest; `matrix` y up)
  if (units && font_box.right > font_box.left &&
      font_box.bottom != font_box.top) {
    for (int corner = 0; corner < 4; ++corner) {
      const CFX_PointF p = matrix.Transform(CFX_PointF(
          static_cast<float>(corner & 1 ? font_box.right : font_box.left) /
              units,
          static_cast<float>(corner & 2 ? font_box.top : font_box.bottom) /
              units));
      x0 = std::min<double>(x0, p.x);
      x1 = std::max<double>(x1, p.x);
      y0 = std::min<double>(y0, -p.y);
      y1 = std::max<double>(y1, -p.y);
    }
  }
  constexpr uint32_t kUpper = 10000;
  const uint32_t width = static_cast<uint32_t>(floor(x1 - x0)) + 3;
  const uint32_t height = static_cast<uint32_t>(floor(y1 - y0)) + 3;
  const uint32_t raster = ((width + 63) >> 6) << 3;
  return raster == 0 || height <= kUpper / raster;
}

// pdfking: `path` (device pixels) filled nonzero into `mask` (over `rect`)
// by gs's scan converter with fill adjust 0: the pixels whose centers are
// inside, as gs fills a glyph's outline.
void GsCenterFill(const CFX_Path& path,
                  const FX_RECT& rect,
                  RetainPtr<CFX_DIBitmap> mask) {
  auto fix = [](float v) {
    return static_cast<gs_scan::fixed>(floor(v * 256.0 + 0.5));
  };
  std::vector<gs_scan::Segment> segs;
  pdfium::span<const CFX_Path::Point> points = path.GetPoints();
  for (size_t i = 0; i < points.size(); ++i) {
    const CFX_Path::Point& p = points[i];
    gs_scan::Segment seg{};
    seg.x = fix(p.point_.x);
    seg.y = fix(p.point_.y);
    if (p.type_ == CFX_Path::Point::Type::kMove) {
      seg.type = gs_scan::Segment::Type::kMove;
    } else if (p.type_ == CFX_Path::Point::Type::kLine) {
      seg.type = gs_scan::Segment::Type::kLine;
    } else {
      if (i + 2 >= points.size()) {
        break;
      }
      seg.type = gs_scan::Segment::Type::kCurve;
      seg.c1x = seg.x;
      seg.c1y = seg.y;
      seg.c2x = fix(points[i + 1].point_.x);
      seg.c2y = fix(points[i + 1].point_.y);
      seg.x = fix(points[i + 2].point_.x);
      seg.y = fix(points[i + 2].point_.y);
      i += 2;
    }
    segs.push_back(seg);
    if (points[i].close_figure_) {
      segs.push_back({gs_scan::Segment::Type::kClose, 0, 0, 0, 0, 0, 0});
    }
  }
  gs_scan::ScanConvertAndFill(
      segs, rect.left, rect.top, rect.right, rect.bottom,
      /*flatness=*/0.2,  // gs_char_flatness: 0 for any usual size, set 0.2
      gs_scan::Rule::kWinding, /*any_part=*/false,
      [&](int x, int y, int w, int h) {
        for (int row = y; row < y + h; ++row) {
          pdfium::span<uint8_t> scan = mask->GetWritableScanline(row - rect.top);
          for (int col = x; col < x + w; ++col) {
            scan[col - rect.left] = 255;
          }
        }
      });
}

  // pdfking: as gs's charpath: each glyph's outline as gs's FreeType gives
  // it at the device resolution, at gs's current point (the run's start plus
  // each advance in 1/256 device pixels), then filled / stroked in user space
bool GsDrawTextPath(CFX_RenderDevice* device,
                    pdfium::span<const TextCharPos> pCharPos,
                    CFX_Font* font,
                    float font_size,
                    const CFX_Matrix& mtText2User,
                    const CFX_Matrix& user2device,
                    const CFX_GraphStateData* pGraphState,
                    uint32_t fill_color,
                    uint32_t stroke_color,
                    CFX_Path* pClippingPath,
                    const CFX_FillRenderOptions& fill_options,
                    float gs_dpi) {
  const CFX_Matrix text2device = mtText2User * user2device;
  CFX_Matrix char2device = text2device;
  char2device.Scale(font_size, -font_size);
  if (fabs(char2device.a * char2device.d - char2device.b * char2device.c) <
      1e-6) {
    return true;  // a zero size: no outline at all
  }
  const CFX_Matrix device2user = user2device.GetInverse();
  CFX_Path outlines;
  CFX_PointF previous;
  double gs_x = 0;
  double gs_y = 0;
  bool first = true;
  for (const auto& charpos : pCharPos) {
    const CFX_PointF origin = text2device.Transform(charpos.origin_);
    if (fill_options.gs_positioned) {
      gs_x = origin.x;
      gs_y = origin.y;
    } else if (first) {
      gs_x = floor(origin.x * 256.0 + 0.5) / 256.0;
      gs_y = floor(origin.y * 256.0 + 0.5) / 256.0;
      first = false;
    } else {
      gs_x += trunc((origin.x - previous.x) * 256.0) / 256.0;
      gs_y += trunc((origin.y - previous.y) * 256.0) / 256.0;
    }
    previous = origin;
    std::unique_ptr<CFX_Path> outline = font->GetFace()->GsGlyphOutline(
        charpos.glyph_index_, charpos.GetEffectiveMatrix(char2device),
        gs_dpi);
    if (outline) {
      const CFX_Matrix place(1, 0, 0, 1, static_cast<float>(gs_x),
                             static_cast<float>(gs_y));
      outlines.Append(*outline, &place);
    }
  }
  // a clip (text rendering modes 4-7): the outlines in device space
  if (pClippingPath) {
    pClippingPath->Append(outlines, nullptr);
  }
  if (!fill_color && !stroke_color) {
    return true;
  }
  CFX_FillRenderOptions options(fill_options);
  // in device space, unless drawn through a layer (fill and translucent
  // stroke) that places it by the matrix
  if (fill_color && FXARGB_A(stroke_color) < 0xff) {
    outlines.Transform(device2user);
  } else {
    options.gs_device_path = true;
  }
  if (fill_color) {
    options.fill_type = CFX_FillRenderOptions::FillType::kWinding;
  }
  options.text_mode = true;
  return outlines.GetPoints().empty() ||
         device->DrawPath(outlines, &user2device, pGraphState, fill_color,
                  stroke_color, options);
}
