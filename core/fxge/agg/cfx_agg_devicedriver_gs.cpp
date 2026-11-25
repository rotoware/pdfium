// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CFX_AggDeviceDriver's paths and clips as gs paints them
// (CFX_FillRenderOptions::gs), kept out of cfx_agg_devicedriver.cpp so
// upstream's changes to it merge cleanly.

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fxcrt/check.h"
#include "core/fxcrt/span.h"
#include "core/fxge/agg/cfx_agg_cliprgn.h"
#include "core/fxge/agg/cfx_agg_devicedriver.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/cfx_graphstatedata.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/gs_scanconvert.h"
#include "core/fxge/gs_stroke.h"

// Ignore fallthrough warnings in agg23 headers.
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/agg23/agg_path_storage.h"
#include "third_party/agg23/agg_rasterizer_scanline_aa.h"
#if defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace pdfium {
namespace {

// pdfking: a path in gs's device fixed point, as gs's path operators build
// it (gs_point_transform with its float casts, then float2fixed_rounded).
template <typename Segment>
std::vector<Segment> GsPath(const CFX_Path& path, const CFX_Matrix& m) {
  auto fix = [&m](const CFX_PointF& p) {
    double x = static_cast<float>(p.x * static_cast<double>(m.a)) +
               static_cast<double>(m.e);
    double y = static_cast<float>(p.y * static_cast<double>(m.d)) +
               static_cast<double>(m.f);
    if (m.c != 0) {
      x += static_cast<float>(p.y * static_cast<double>(m.c));
    }
    if (m.b != 0) {
      y += static_cast<float>(p.x * static_cast<double>(m.b));
    }
    return std::pair<int32_t, int32_t>(
        static_cast<int32_t>(floor(x * 256.0f + 0.5)),
        static_cast<int32_t>(floor(y * 256.0f + 0.5)));
  };
  std::vector<Segment> out;
  pdfium::span<const CFX_Path::Point> points = path.GetPoints();
  for (size_t i = 0; i < points.size(); ++i) {
    const CFX_Path::Point& point = points[i];
    auto [x, y] = fix(point.point_);
    Segment seg{};
    seg.x = x;
    seg.y = y;
    if (point.type_ == CFX_Path::Point::Type::kMove) {
      seg.type = Segment::Type::kMove;
    } else if (point.type_ == CFX_Path::Point::Type::kLine) {
      seg.type = Segment::Type::kLine;
    } else {
      if (i + 2 >= points.size()) {
        break;
      }
      seg.type = Segment::Type::kCurve;
      seg.c1x = x;
      seg.c1y = y;
      std::tie(seg.c2x, seg.c2y) = fix(points[i + 1].point_);
      std::tie(seg.x, seg.y) = fix(points[i + 2].point_);
      i += 2;
    }
    out.push_back(seg);
    if (points[i].close_figure_) {
      out.push_back(Segment{Segment::Type::kClose, 0, 0, 0, 0, 0, 0});
    }
  }
  return out;
}

void AddGsRect(agg::path_storage& rects, int x, int y, int w, int h) {
  rects.move_to(x, y);
  rects.line_to(x + w, y);
  rects.line_to(x + w, y + h);
  rects.line_to(x, y + h);
  rects.end_poly();
}

}  // namespace

// pdfking: clipped as gs clips (gx_cpath_intersect): a rectangle to the
// pixels its fill adjustment (0.5) reaches, any other path to the pixels its
// fill inks (gs's scan converter, any part of a pixel; a text clip's centre
// of a pixel, pdfi clipping with fill adjust 0)
void CFX_AggDeviceDriver::SetClipAsGs(
    const CFX_Path& path,
    const CFX_Matrix& matrix,
    const CFX_FillRenderOptions& fill_options) {
  const int width = GetPixelWidth();
  const int height = GetPixelHeight();
  std::vector<gs_scan::Segment> segs = GsPath<gs_scan::Segment>(path, matrix);
  // gx_path_is_rectangle: one closed subpath of four axis-aligned sides
  std::vector<gs_scan::Segment> corners;
  for (const gs_scan::Segment& seg : segs) {
    if (seg.type != gs_scan::Segment::Type::kClose) {
      corners.push_back(seg);
    }
  }
  if (corners.size() == 5 && corners[4].x == corners[0].x &&
      corners[4].y == corners[0].y) {
    corners.pop_back();
  }
  const bool rect =
      corners.size() == 4 && corners[0].type == gs_scan::Segment::Type::kMove &&
      std::all_of(corners.begin() + 1, corners.end(),
                  [](const gs_scan::Segment& seg) {
                    return seg.type == gs_scan::Segment::Type::kLine;
                  }) &&
      ((corners[0].x == corners[1].x && corners[1].y == corners[2].y &&
        corners[2].x == corners[3].x && corners[3].y == corners[0].y) ||
       (corners[0].y == corners[1].y && corners[1].x == corners[2].x &&
        corners[2].y == corners[3].y && corners[3].x == corners[0].x));
  if (rect) {
    const gs_scan::fixed px = std::min(corners[0].x, corners[2].x);
    const gs_scan::fixed py = std::min(corners[0].y, corners[2].y);
    const gs_scan::fixed qx = std::max(corners[0].x, corners[2].x);
    const gs_scan::fixed qy = std::max(corners[0].y, corners[2].y);
    // fixed2int_pixround(p - (0.5 - epsilon)), fixed2int_pixround(q + 0.5)
    // (pixround adds 0.5 - epsilon): floor(p), ceil(q)
    FX_RECT box(px >> 8, py >> 8, (qx + 255) >> 8, (qy + 255) >> 8);
    box.Intersect(FX_RECT(0, 0, width, height));
    if (box.IsEmpty()) {
      box = FX_RECT();
    }
    clip_rgn_->IntersectRect(box);
    return;
  }
  auto mask = pdfium::MakeRetain<CFX_DIBitmap>();
  CHECK(mask->Create(width, height, FXDIB_Format::k8bppMask));
  gs_scan::ScanConvertAndFill(
      segs, 0, 0, width, height, 1.0,
      fill_options.fill_type == CFX_FillRenderOptions::FillType::kEvenOdd
          ? gs_scan::Rule::kEvenOdd
          : gs_scan::Rule::kWinding,
      /*any_part=*/!fill_options.text_mode, [&mask](int x, int y, int w, int h) {
        for (int row = y; row < y + h; ++row) {
          pdfium::span<uint8_t> scan = mask->GetWritableScanline(row);
          for (int col = x; col < x + w; ++col) {
            scan[col] = 255;
          }
        }
      },
      /*clip=*/true);
  clip_rgn_->IntersectMask(0, 0, std::move(mask));
}

// pdfking: filled with gs's scan converter (any part of a pixel, flatness
// 1, gs's default) and stroked with gs's stroker; the pixels they paint go
// through agg as whole-pixel rects, clipped and composited as any path.
bool CFX_AggDeviceDriver::DrawPathAsGs(
    const CFX_Path& path,
    const CFX_Matrix& matrix,
    const CFX_GraphStateData* pGraphState,
    uint32_t fill_color,
    uint32_t stroke_color,
    bool group_knockout,
    const CFX_FillRenderOptions& fill_options) {
  const int width = GetPixelWidth();
  const int height = GetPixelHeight();
  const CFX_Matrix path_matrix =
      fill_options.gs_device_path ? CFX_Matrix() : matrix;
  auto render = [&](agg::path_storage& rects, uint32_t color,
                    bool knockout) {
    agg::rasterizer_scanline_aa rasterizer;
    rasterizer.clip_box(0.0f, 0.0f, static_cast<float>(width),
                        static_cast<float>(height));
    rasterizer.add_path(rects);
    rasterizer.filling_rule(agg::fill_non_zero);
    RenderRasterizer(rasterizer, color, fill_options.full_cover, knockout);
  };
  if (fill_options.fill_type != CFX_FillRenderOptions::FillType::kNoFill &&
      fill_color) {
    agg::path_storage rects;
    gs_scan::ScanConvertAndFill(
        GsPath<gs_scan::Segment>(path, path_matrix), 0, 0, width, height, 1.0,
        fill_options.fill_type == CFX_FillRenderOptions::FillType::kEvenOdd
            ? gs_scan::Rule::kEvenOdd
            : gs_scan::Rule::kWinding,
        /*any_part=*/true,
        [&rects](int x, int y, int w, int h) { AddGsRect(rects, x, y, w, h); });
    render(rects, fill_color, /*knockout=*/false);
  }
  if (!pGraphState || !FXARGB_A(stroke_color)) {
    return true;
  }
  gs_stroke::Params params;
  params.width = pGraphState->line_width();
  params.ctm[0] = matrix.a;
  params.ctm[1] = matrix.b;
  params.ctm[2] = matrix.c;
  params.ctm[3] = matrix.d;
  params.cap = static_cast<int>(pGraphState->line_cap());
  params.join = static_cast<int>(pGraphState->line_join());
  params.miter_limit = std::max(1.0f, pGraphState->miter_limit());
  params.dash.assign(pGraphState->dash_array().begin(),
                     pGraphState->dash_array().end());
  params.dash_phase = pGraphState->dash_phase();
  params.stroke_adjust = fill_options.adjust_stroke;
  params.flatness = 1.0;
  gs_stroke::Output out = gs_stroke::Stroke(
      GsPath<gs_stroke::Segment>(path, path_matrix), params, 0, 0, width, height);
  agg::path_storage rects;
  auto add = [&rects](int x, int y, int w, int h) {
    AddGsRect(rects, x, y, w, h);
  };
  for (const auto& piece : out.fill_paths) {
    std::vector<gs_scan::Segment> segs;
    segs.reserve(piece.size());
    for (const gs_stroke::Segment& seg : piece) {
      segs.push_back(gs_scan::Segment{
          static_cast<gs_scan::Segment::Type>(seg.type), seg.x, seg.y, seg.c1x,
          seg.c1y, seg.c2x, seg.c2y});
    }
    gs_scan::ScanConvertAndFill(segs, 0, 0, width, height, params.flatness,
                                gs_scan::Rule::kWinding, /*any_part=*/true,
                                add);
  }
  for (const auto& rect : out.rects) {
    add(rect[0], rect[1], rect[2], rect[3]);
  }
  render(rects, stroke_color, group_knockout);
  return true;
}

}  // namespace pdfium
