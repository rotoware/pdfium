// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: annotation appearances drawn as gs draws them, when the document
// renders on inks (separations). Included by cpdf_generateap.cpp after its
// anonymous namespace, whose helpers (GenerateExtGStateDict,
// GenerateResourcesDict, GenerateAndSetAPDict, kGSDictName) it uses; kept
// out of that file so upstream's changes to it merge cleanly.

#include <math.h>

#include <array>
#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_inks.h"

namespace {


// pdfking: a Line, Polygon or PolyLine with no appearance drawn as gs draws
// it (pdf_annot.c pdfi_annot_draw_Line / _Polygon / _PolyLine): the path in
// page space; a Polygon's interior filled in /IC (at /CA, which then holds
// for its border too), the border stroked in /C at /BS /W (or /Border's
// width), dashed by /BS /D or /Border's dash; nothing in a color missing or
// empty. Line endings other than /None are not drawn.
ByteString GsColor(const CPDF_Array* color, bool stroke) {
  if (!color) {
    return ByteString();
  }
  fxcrt::ostringstream out;
  const char* op = nullptr;
  switch (color->size()) {
    case 1:
      op = stroke ? "G" : "g";
      break;
    case 3:
      op = stroke ? "RG" : "rg";
      break;
    case 4:
      op = stroke ? "K" : "k";
      break;
    default:
      return ByteString();
  }
  for (size_t i = 0; i < color->size(); ++i) {
    WriteFloat(out, color->GetFloatAt(i)) << " ";
  }
  out << op << "\n";
  return ByteString(out);
}

// The border's width as gs takes it: /BS /W (1 by default), else /Border's
// third number, else 1
float GsBorderWidth(const CPDF_Dictionary* annot_dict) {
  if (RetainPtr<const CPDF_Dictionary> bs = annot_dict->GetDictFor("BS")) {
    return bs->KeyExist("W") ? bs->GetFloatFor("W") : 1.0f;
  }
  RetainPtr<const CPDF_Array> border =
      annot_dict->GetArrayFor(pdfium::annotation::kBorder);
  return border && border->size() >= 3 ? border->GetFloatAt(2) : 1.0f;
}

// The border's width and dash operators (pdfi_annot_draw_BS / _Border): a
// dashed /BS's /D (3 by default), else /Border's fourth element; nothing
// when /Border is too short (gs draws no border then)
std::optional<ByteString> GsBorder(const CPDF_Dictionary* annot_dict) {
  RetainPtr<const CPDF_Array> dash;
  if (RetainPtr<const CPDF_Dictionary> bs = annot_dict->GetDictFor("BS")) {
    if (bs->GetNameFor("S") == "D") {
      dash = bs->GetArrayFor("D");
      if (!dash) {
        auto three = pdfium::MakeRetain<CPDF_Array>();
        three->AppendNew<CPDF_Number>(3);
        dash = std::move(three);
      }
    }
  } else if (RetainPtr<const CPDF_Array> border =
                 annot_dict->GetArrayFor(pdfium::annotation::kBorder)) {
    if (border->size() < 3) {
      return std::nullopt;
    }
    if (border->size() > 3) {
      dash = border->GetArrayAt(3);
    }
  }
  fxcrt::ostringstream out;
  WriteFloat(out, GsBorderWidth(annot_dict)) << " w\n[";
  for (size_t i = 0; dash && i < dash->size(); ++i) {
    WriteFloat(out, dash->GetFloatAt(i)) << " ";
  }
  out << "] 0 d\n";
  return ByteString(out);
}

// The annotation's box grown to take in what gs draws unclipped, set as its
// /Rect (and so its appearance's /BBox)
void GsGrowRect(CPDF_Dictionary* annot_dict,
                CFX_FloatRect rect,
                float width) {
  rect.Union(annot_dict->GetRectFor(pdfium::annotation::kRect));
  rect.Inflate(width * 10, width * 10);
  annot_dict->SetRectFor(pdfium::annotation::kRect, rect);
}

// pdfking: a Square or Circle with no appearance drawn as gs draws it
// (pdfi_annot_draw_Square / _Circle). The box: /Rect less /RD.
//  - Square with /IC: the box filled, clipped to /Rect, at /CA (which holds
//    for the border too), then the border stroked inside the box (rect
//    inset by half the width);
//  - Square without: the border stroked on the box's edge;
//  - Circle: gs's four-curve ellipse in the box, filled in /IC at /CA, then
//    stroked on its edge.
bool GenerateGsSquareCircleAP(CPDF_Document* doc,
                              CPDF_Dictionary* annot_dict,
                              bool circle) {
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  rect.Normalize();
  const CFX_FloatRect clip = rect;
  if (RetainPtr<const CPDF_Array> rd = annot_dict->GetArrayFor("RD")) {
    if (rd->size() >= 4) {
      rect.left += rd->GetFloatAt(0);
      rect.bottom += rd->GetFloatAt(1);
      rect.right -= rd->GetFloatAt(2);
      rect.top -= rd->GetFloatAt(3);
    }
  }
  const std::optional<ByteString> border = GsBorder(annot_dict);
  const float width = GsBorderWidth(annot_dict);
  const ByteString interior =
      GsColor(annot_dict->GetArrayFor("IC").Get(), /*stroke=*/false);
  const ByteString stroke =
      GsColor(annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
              /*stroke=*/true);
  const bool opacity = annot_dict->KeyExist("CA");
  auto point = [](fxcrt::ostringstream& out, float x, float y) -> auto& {
    WriteFloat(out, x) << " ";
    return WriteFloat(out, y) << " ";
  };
  auto rect_path = [&](fxcrt::ostringstream& out, const CFX_FloatRect& r) {
    point(out, r.left, r.bottom) << "m\n";
    point(out, r.right, r.bottom) << "l\n";
    point(out, r.right, r.top) << "l\n";
    point(out, r.left, r.top) << "l\nh\n";
  };
  fxcrt::ostringstream app_stream;
  if (circle) {
    // gs_translate to the center, then pdfi_annot_drawellipse
    const float half_w = rect.Width() / 2;
    const float half_h = rect.Height() / 2;
    const double magic = .55228475 - .00045;
    const float xmag = static_cast<float>(half_w * magic);
    const float ymag = static_cast<float>(half_h * magic);
    fxcrt::ostringstream path;
    point(path, -half_w, 0) << "m\n";
    point(path, -half_w, ymag);
    point(path, -xmag, half_h);
    point(path, 0, half_h) << "c\n";
    point(path, xmag, half_h);
    point(path, half_w, ymag);
    point(path, half_w, 0) << "c\n";
    point(path, half_w, -ymag);
    point(path, xmag, -half_h);
    point(path, 0, -half_h) << "c\n";
    point(path, -xmag, -half_h);
    point(path, -half_w, -ymag);
    point(path, -half_w, 0) << "c\n";
    const ByteString ellipse(path);
    app_stream << "1 0 0 1 ";
    point(app_stream, rect.left + half_w, rect.bottom + half_h) << "cm\n";
    if (!interior.IsEmpty()) {
      app_stream << "q\n";
      if (opacity) {
        app_stream << "/" << kGSDictName << " gs\n";
      }
      app_stream << interior << ellipse << "f\nQ\n";
    }
    if (!stroke.IsEmpty() && border.has_value() && width > 0) {
      app_stream << stroke << border.value() << ellipse << "S\n";
    }
  } else if (!interior.IsEmpty()) {
    if (opacity) {
      app_stream << "/" << kGSDictName << " gs\n";
    }
    app_stream << "q\n";
    rect_path(app_stream, clip);
    app_stream << "W n\n" << interior;
    rect_path(app_stream, rect);
    app_stream << "f\nQ\n";
    if (!stroke.IsEmpty() && border.has_value() && width > 0) {
      CFX_FloatRect inset = rect;
      inset.Deflate(width / 2, width / 2);
      app_stream << stroke << border.value();
      rect_path(app_stream, inset);
      app_stream << "S\n";
    }
  } else if (!stroke.IsEmpty() && border.has_value() && width > 0) {
    app_stream << stroke << border.value();
    rect_path(app_stream, rect);
    app_stream << "S\n";
  }
  GsGrowRect(annot_dict, rect, width);
  auto gs_dict = GenerateExtGStateDict(*annot_dict, "Normal");
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

// gs's QuadPoints basis (pdfi_annot_quadpoints2basis): the lowest (then
// leftmost) vertex, the next one counterclockwise, the one across
struct GsQuadBasis {
  double x0, y0, dx1, dy1, dx2, dy2;
};

GsQuadBasis GsQuadPointsBasis(const std::array<double, 8>& a) {
  double minx = a[0];
  double miny = a[1];
  int mindex = 0;
  for (int i = 2; i < 8; i += 2) {
    if ((a[i + 1] == miny && a[i] < minx) || a[i + 1] < miny) {
      miny = a[i + 1];
      minx = a[i];
      mindex = i;
    }
  }
  int i = mindex;
  const double x0 = a[i];
  const double y0 = a[i + 1];
  i = (i + 2) % 8;
  double x1 = a[i];
  double y1 = a[i + 1];
  i = (i + 2) % 8;
  const double x2 = a[i];
  const double y2 = a[i + 1];
  i = (i + 2) % 8;
  const double x3 = a[i];
  const double y3 = a[i + 1];
  if (y3 < y1) {
    y1 = y3;
    x1 = x3;
  }
  return {x0, y0, x1 - x0, y1 - y0, x2 - x0, y2 - y0};
}

// gs_atan2_degrees
double GsAtan2Degrees(double y, double x) {
  if (y == 0) {
    return x < 0 ? 180 : 0;
  }
  double result = atan2(y, x) * (180.0 / M_PI);
  return result < 0 ? result + 360 : result;
}

// gs_sincos_degrees: exact on multiples of 90
struct GsSinCos {
  double sin, cos;
  bool orthogonal;
};
GsSinCos GsSinCosDegrees(double ang) {
  const double quot = ang / 90;
  if (floor(quot) == quot) {
    static constexpr double kIsincos[5] = {0, 1, 0, -1, 0};
    const int quads = static_cast<int>(fmod(quot, 4.0)) & 3;
    return {kIsincos[quads], kIsincos[quads + 1], true};
  }
  const double arad = ang * (M_PI / 180);
  return {sin(arad), cos(arad), false};
}

// gs_arcn as gspath1.c adds it (gs_gstate_arc_add, next_arc_curve,
// next_arc_quadrant, arc_add) in user space: a line to its start, then its
// curves
void GsArcN(fxcrt::ostringstream& out,
            double xc,
            double yc,
            double r,
            double ang1,
            double ang2) {
  constexpr double kQuarterArcFraction = 0.55228474983079334;
  struct {
    double p0x, p0y, ptx, pty, p3x, p3y, angle;
    GsSinCos sincos;
  } arc;
  auto write = [&out](double x, double y) -> auto& {
    WriteFloat(out, static_cast<float>(x)) << " ";
    return WriteFloat(out, static_cast<float>(y)) << " ";
  };
  bool first = true;
  auto add = [&](bool quadrant) {
    if (first) {
      write(arc.p0x, arc.p0y) << "l\n";
      first = false;
    }
    double fraction = kQuarterArcFraction;
    if (!quadrant) {
      const double dx = arc.ptx - arc.p0x;
      const double dy = arc.pty - arc.p0y;
      const double dist = dx * dx + dy * dy;
      const double r2 = r * r;
      fraction = dist >= r2 * 1.0e8 ? 0.0
                                    : (4.0 / 3.0) / (1 + sqrt(1 + dist / r2));
    }
    write(arc.p0x + (arc.ptx - arc.p0x) * fraction,
          arc.p0y + (arc.pty - arc.p0y) * fraction);
    write(arc.p3x + (arc.ptx - arc.p3x) * fraction,
          arc.p3y + (arc.pty - arc.p3y) * fraction);
    write(arc.p3x, arc.p3y) << "c\n";
  };
  auto next_curve = [&](double anext) {
    arc.p0x = arc.p3x;
    arc.p0y = arc.p3y;
    const double trad = r * tan((anext - arc.angle) * (M_PI / 180 / 2));
    arc.ptx = arc.p0x - trad * arc.sincos.sin;
    arc.pty = arc.p0y + trad * arc.sincos.cos;
    arc.sincos = GsSinCosDegrees(anext);
    arc.p3x = xc + r * arc.sincos.cos;
    arc.p3y = yc + r * arc.sincos.sin;
    arc.angle = anext;
    add(false);
  };
  auto next_quadrant = [&](double anext) {
    double x0 = arc.p0x = arc.p3x;
    double y0 = arc.p0y = arc.p3y;
    switch ((static_cast<int>(anext) >> 1) & 3) {
      case 0:
        arc.sincos = {0, 1, true};
        arc.p3x = x0 = xc + r;
        arc.p3y = yc;
        break;
      case 1:
        arc.sincos = {1, 0, true};
        arc.p3x = xc;
        arc.p3y = y0 = yc + r;
        break;
      case 2:
        arc.sincos = {0, -1, true};
        arc.p3x = x0 = xc - r;
        arc.p3y = yc;
        break;
      default:
        arc.sincos = {-1, 0, true};
        arc.p3x = xc;
        arc.p3y = y0 = yc - r;
        break;
    }
    arc.ptx = x0;
    arc.pty = y0;
    arc.angle = anext;
    add(true);
  };
  if (r < 0) {
    ang1 += 180;
    ang2 += 180;
    r = -r;
  }
  arc.sincos = GsSinCosDegrees(fmod(ang1, 360));
  arc.p3x = xc + r * arc.sincos.cos;
  arc.p3y = yc + r * arc.sincos.sin;
  arc.p0x = arc.p3x;
  arc.p0y = arc.p3y;
  if (ang1 < ang2) {
    ang2 -= ceil((ang2 - ang1) / 360) * 360;
  }
  if (ang2 < 0) {
    const double adjust = ceil(-ang2 / 360) * 360;
    ang1 += adjust;
    ang2 += adjust;
  }
  arc.angle = ang1;
  if (ang1 != ang2) {
    if (!arc.sincos.orthogonal) {
      const double anext = floor(arc.angle / 90) * 90;
      if (anext < ang2) {
        next_curve(ang2);
        return;
      }
      next_curve(anext);
    }
    double anext;
    while ((anext = arc.angle - 90) >= ang2) {
      next_quadrant(anext);
    }
    if (arc.angle == ang2) {
      return;
    }
  }
  next_curve(ang2);
}

// pdfking: text markups with no appearance drawn as gs draws them (pdf
// _annot.c): for each quad of /QuadPoints, in /C (nothing without it)
//  - Underline, StrikeOut: a line 1/7 or 3/7 up the quad, 1/16 of its height
//    wide (pdfi_annot_draw_line_offset);
//  - Squiggly: a zigzag clipped to the quad, round caps and joins;
//  - Highlight: the quad with arcs on its ends, filled with Multiply on a
//    page with transparency, else its outline stroked with round caps.
bool GenerateGsMarkupAP(CPDF_Document* doc,
                        CPDF_Dictionary* annot_dict,
                        CPDF_Annot::Subtype subtype) {
  const ByteString stroke =
      GsColor(annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
              /*stroke=*/true);
  RetainPtr<const CPDF_Array> quads = annot_dict->GetArrayFor("QuadPoints");
  if (stroke.IsEmpty() || !quads) {
    return false;
  }
  const bool transparent = GetInks(doc) && GetInks(doc)->page_transparent();
  auto write = [](fxcrt::ostringstream& out, double x, double y) -> auto& {
    WriteFloat(out, static_cast<float>(x)) << " ";
    return WriteFloat(out, static_cast<float>(y)) << " ";
  };
  fxcrt::ostringstream app_stream;
  CFX_FloatRect box;
  bool any = false;
  if (subtype == CPDF_Annot::Subtype::HIGHLIGHT) {
    // the fill color for a fill, the stroke color for a stroke
    const ByteString fill = GsColor(
        annot_dict->GetArrayFor(pdfium::annotation::kC).Get(), false);
    app_stream << (transparent ? fill : stroke) << "1 J\n";
    if (transparent) {
      app_stream << "/" << kGSDictName << " gs\n";
    }
  } else if (subtype != CPDF_Annot::Subtype::SQUIGGLY) {
    app_stream << stroke;
  }
  for (size_t q = 0; q + 8 <= quads->size(); q += 8) {
    std::array<double, 8> a;
    for (size_t i = 0; i < 8; ++i) {
      a[i] = quads->GetFloatAt(q + i);
    }
    for (size_t i = 0; i < 8; i += 2) {
      const CFX_FloatRect point(static_cast<float>(a[i]),
                                static_cast<float>(a[i + 1]),
                                static_cast<float>(a[i]),
                                static_cast<float>(a[i + 1]));
      if (any) {
        box.Union(point);
      } else {
        box = point;
        any = true;
      }
    }
    const GsQuadBasis b = GsQuadPointsBasis(a);
    if (subtype == CPDF_Annot::Subtype::UNDERLINE ||
        subtype == CPDF_Annot::Subtype::STRIKEOUT) {
      const double offset =
          subtype == CPDF_Annot::Subtype::UNDERLINE ? 1 / 7. : 3 / 7.;
      const double width = sqrt(b.dx2 * b.dx2 + b.dy2 * b.dy2) / 16.;
      WriteFloat(app_stream, static_cast<float>(width)) << " w\n";
      write(app_stream, b.dx2 * offset + b.x0, b.dy2 * offset + b.y0) << "m\n";
      write(app_stream, b.dx2 * offset + b.x0 + b.dx1,
            b.dy2 * offset + b.y0 + b.dy1)
          << "l\nS\n";
    } else if (subtype == CPDF_Annot::Subtype::SQUIGGLY) {
      const double p2 = sqrt(b.dx2 * b.dx2 + b.dy2 * b.dy2);
      const double p1 = sqrt(b.dx1 * b.dx1 + b.dy1 * b.dy1);
      app_stream << "q\n";
      write(app_stream, a[6], a[7]) << "m\n";
      write(app_stream, a[4], a[5]) << "l\n";
      write(app_stream, a[0], a[1]) << "l\n";
      write(app_stream, a[2], a[3]) << "l\nh W n\n";
      app_stream << "1 0 0 1 ";
      write(app_stream, b.x0 + b.dx2 / 56 + b.dx2 / 72,
            b.y0 + b.dy2 / 56 + b.dy2 / 72)
          << "cm\n";
      if (p2 > 0 && p1 > 0) {
        const double p12 = p2 / p1;
        int count = static_cast<int>((1 / p12) * 4 + 1);
        write(app_stream, b.dx1 * p12, b.dy1 * p12);
        write(app_stream, b.dx2, b.dy2) << "0 0 cm\n";
        app_stream << "0.025 0 0 ";
        WriteFloat(app_stream, static_cast<float>(1 / 72.)) << " 0 0 cm\n";
        app_stream << stroke << "1 J 1 j 1 w\n0 0 m\n";
        for (int k = 0; count >= 0; --count, ++k) {
          write(app_stream, 10.0 * k + 5, 10) << "l\n";
          write(app_stream, 10.0 * k + 10, 0) << "l\n";
        }
        app_stream << "S\n";
      }
      app_stream << "Q\n";
    } else {
      // Highlight: pdfi_annot_highlight_arc on both ends
      auto highlight_arc = [&](double x0, double y0, double x1, double y1) {
        const double dx = x1 - x0;
        const double dy = y1 - y0;
        const double yc = (y1 + y0) / 2. - 15. / 16. * dx;
        const double xc = (x1 + x0) / 2. + 15. / 16. * dy;
        const double r = sqrt((y1 - yc) * (y1 - yc) + (x1 - xc) * (x1 - xc));
        const double a1 = GsAtan2Degrees(y1 - yc, x1 - xc);
        const double a2 = GsAtan2Degrees(y0 - yc, x0 - xc);
        GsArcN(app_stream, xc, yc, r, a2, a1);
      };
      write(app_stream, a[2], a[3]) << "m\n";
      highlight_arc(a[2], a[3], a[6], a[7]);
      write(app_stream, a[4], a[5]) << "l\n";
      highlight_arc(a[4], a[5], a[0], a[1]);
      app_stream << "h\n" << (transparent ? "f\n" : "S\n");
    }
  }
  if (!any) {
    return false;
  }
  GsGrowRect(annot_dict, box, box.Height() + 1);
  auto gs_dict = GenerateExtGStateDict(
      *annot_dict, transparent && subtype == CPDF_Annot::Subtype::HIGHLIGHT
                       ? "Multiply"
                       : "Normal");
  // gs sets no opacity for these
  gs_dict->GetMutableDictFor(kGSDictName)->SetNewFor<CPDF_Number>("CA", 1);
  gs_dict->GetMutableDictFor(kGSDictName)->SetNewFor<CPDF_Number>("ca", 1);
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

bool GenerateGsLineAP(CPDF_Document* doc,
                      CPDF_Dictionary* annot_dict,
                      CPDF_Annot::Subtype subtype) {
  std::vector<CFX_PointF> points;
  RetainPtr<const CPDF_Array> coords =
      subtype == CPDF_Annot::Subtype::LINE ? annot_dict->GetArrayFor("L")
                                           : annot_dict->GetArrayFor("Vertices");
  if (!coords) {
    return false;
  }
  for (size_t i = 0; i + 1 < coords->size(); i += 2) {
    points.emplace_back(coords->GetFloatAt(i), coords->GetFloatAt(i + 1));
  }
  if (points.size() < 2) {
    return false;
  }
  std::optional<ByteString> border = GsBorder(annot_dict);
  if (!border.has_value()) {
    return false;
  }
  const float width = GsBorderWidth(annot_dict);
  fxcrt::ostringstream path;
  for (size_t i = 0; i < points.size(); ++i) {
    WriteFloat(path, points[i].x) << " ";
    WriteFloat(path, points[i].y) << (i == 0 ? " m\n" : " l\n");
  }
  if (subtype == CPDF_Annot::Subtype::POLYGON) {
    path << "h\n";
  }
  const ByteString path_ops(path);
  fxcrt::ostringstream app_stream;
  bool opacity = false;
  if (subtype == CPDF_Annot::Subtype::POLYGON) {
    const ByteString interior =
        GsColor(annot_dict->GetArrayFor("IC").Get(), /*stroke=*/false);
    if (!interior.IsEmpty()) {
      opacity = annot_dict->KeyExist("CA");
      app_stream << "q\n";
      if (opacity) {
        app_stream << "/" << kGSDictName << " gs\n";
      }
      app_stream << interior << path_ops << "f\nQ\n";
    }
  }
  const ByteString stroke =
      GsColor(annot_dict->GetArrayFor(pdfium::annotation::kC).Get(),
              /*stroke=*/true);
  if (!stroke.IsEmpty() && width > 0) {
    if (opacity) {
      app_stream << "/" << kGSDictName << " gs\n";
    }
    app_stream << stroke << border.value() << path_ops << "S\n";
  }
  // drawn unclipped by gs: the box takes in the whole path and its border
  CFX_FloatRect rect(points[0].x, points[0].y, points[0].x, points[0].y);
  for (const CFX_PointF& point : points) {
    rect.Union(CFX_FloatRect(point.x, point.y, point.x, point.y));
  }
  GsGrowRect(annot_dict, rect, width);
  auto gs_dict = GenerateExtGStateDict(*annot_dict, "Normal");
  auto resources_dict = GenerateResourcesDict(doc, std::move(gs_dict), nullptr);
  GenerateAndSetAPDict(doc, annot_dict, &app_stream, std::move(resources_dict),
                       false /*IsTextMarkupAnnotation*/);
  return true;
}

// The appearance of `subtype` as gs draws it, or nullopt for a subtype
// pdfium's own generator keeps.
std::optional<bool> GenerateGsAnnotAP(CPDF_Document* doc,
                                      CPDF_Dictionary* annot_dict,
                                      CPDF_Annot::Subtype subtype) {
  switch (subtype) {
    case CPDF_Annot::Subtype::CIRCLE:
      return GenerateGsSquareCircleAP(doc, annot_dict, /*circle=*/true);
    case CPDF_Annot::Subtype::SQUARE:
      return GenerateGsSquareCircleAP(doc, annot_dict, /*circle=*/false);
    case CPDF_Annot::Subtype::HIGHLIGHT:
    case CPDF_Annot::Subtype::SQUIGGLY:
    case CPDF_Annot::Subtype::STRIKEOUT:
    case CPDF_Annot::Subtype::UNDERLINE:
      return GenerateGsMarkupAP(doc, annot_dict, subtype);
    case CPDF_Annot::Subtype::LINE:
    case CPDF_Annot::Subtype::POLYGON:
    case CPDF_Annot::Subtype::POLYLINE:
      return GenerateGsLineAP(doc, annot_dict, subtype);
    default:
      return std::nullopt;
  }
}

}  // namespace
