// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CFX_Face's glyphs loaded as gs's FAPI loads them (fapi_ft.c,
// gxfapi.c), kept out of cfx_face.cpp so upstream's changes to it merge
// cleanly.

#include "core/fxge/cfx_face_gs.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <memory>

#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_path.h"

#if defined(PDF_ENABLE_FREETYPE)

namespace {

// pdfking: gs's FAPI outline (fapi_ft.c move_to / line_to / conic_to /
// cubic_to, then gxfapi.c add_*): device fixed point, y down, from the
// glyph's origin; points kept as fixed / 256 (exact in a float).
struct GsOutline {
  UnownedPtr<CFX_Path> path_;
  int64_t x = 0;  // the previous point, 32.32 (FF_path_info)
  int64_t y = 0;
  FT_Vector current = {0, 0};
  bool need_close = false;
};

void GsAppend(GsOutline* o, int64_t x, int64_t y, CFX_Path::Point::Type type) {
  // import_shift(v, -24), y negated
  o->path_->AppendPoint(CFX_PointF(static_cast<float>((x >> 24) / 256.0),
                                   static_cast<float>(-(y >> 24) / 256.0)),
                        type);
}

int GsMoveTo(const FT_Vector* to, void* user) {
  GsOutline* o = static_cast<GsOutline*>(user);
  if (o->need_close) {
    o->path_->ClosePath();
    o->need_close = false;
  }
  o->current = *to;
  o->x = static_cast<int64_t>(to->x) << 26;
  o->y = static_cast<int64_t>(to->y) << 26;
  GsAppend(o, o->x, o->y, CFX_Path::Point::Type::kMove);
  return 0;
}

bool GsEqual(const FT_Vector& a, const FT_Vector& b) {
  return a.x == b.x && a.y == b.y;
}

int GsLineTo(const FT_Vector* to, void* user) {
  GsOutline* o = static_cast<GsOutline*>(user);
  if (GsEqual(o->current, *to)) {
    return 0;
  }
  o->current = *to;
  o->x = static_cast<int64_t>(to->x) << 26;
  o->y = static_cast<int64_t>(to->y) << 26;
  GsAppend(o, o->x, o->y, CFX_Path::Point::Type::kLine);
  o->need_close = true;
  return 0;
}

// float2fixed: truncated toward zero
int64_t GsFloat2Fixed(double v) {
  return static_cast<int64_t>(static_cast<int32_t>(v * 256.0f));
}

int GsConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
  GsOutline* o = static_cast<GsOutline*>(user);
  if (GsEqual(o->current, *control) && GsEqual(o->current, *to) &&
      GsEqual(*control, *to)) {
    return 0;
  }
  o->current = *to;
  // the start as gs takes it: the previous point's integer pixel (p->x >> 32)
  const double sx = static_cast<double>(o->x >> 32);
  const double sy = static_cast<double>(o->y >> 32);
  const double x = to->x / 64.0;
  const double y = to->y / 64.0;
  const double cx = control->x / 64.0;
  const double cy = control->y / 64.0;
  o->x = GsFloat2Fixed(x) << 24;
  o->y = GsFloat2Fixed(y) << 24;
  GsAppend(o, GsFloat2Fixed((sx + cx * 2) / 3) << 24,
           GsFloat2Fixed((sy + cy * 2) / 3) << 24,
           CFX_Path::Point::Type::kBezier);
  GsAppend(o, GsFloat2Fixed((x + cx * 2) / 3) << 24,
           GsFloat2Fixed((y + cy * 2) / 3) << 24,
           CFX_Path::Point::Type::kBezier);
  GsAppend(o, o->x, o->y, CFX_Path::Point::Type::kBezier);
  o->need_close = true;
  return 0;
}

int GsCubicTo(const FT_Vector* c1,
              const FT_Vector* c2,
              const FT_Vector* to,
              void* user) {
  GsOutline* o = static_cast<GsOutline*>(user);
  if (GsEqual(o->current, *c1) && GsEqual(o->current, *c2) &&
      GsEqual(o->current, *to) && GsEqual(*c1, *c2) && GsEqual(*c1, *to) &&
      GsEqual(*c2, *to)) {
    return 0;
  }
  o->current = *to;
  o->x = static_cast<int64_t>(to->x) << 26;
  o->y = static_cast<int64_t>(to->y) << 26;
  GsAppend(o, static_cast<int64_t>(c1->x) << 26,
           static_cast<int64_t>(c1->y) << 26, CFX_Path::Point::Type::kBezier);
  GsAppend(o, static_cast<int64_t>(c2->x) << 26,
           static_cast<int64_t>(c2->y) << 26, CFX_Path::Point::Type::kBezier);
  GsAppend(o, o->x, o->y, CFX_Path::Point::Type::kBezier);
  o->need_close = true;
  return 0;
}

}  // namespace

// pdfking: a glyph loaded as gs's FAPI loads it (fapi_ft.c
// FF_get_glyph / gs_fapi_ft_init): TrueType hinted by FreeType's v35
// interpreter (gs sets it on its library; here only for the load) with
// FT_LOAD_MONOCHROME and no target (hinted for a grayscale target), Type 1
// and CFF unhinted; on a bytecode error loaded again unhinted, not tricky.
FT_Error GsLoadGlyph(FT_FaceRec* rec, uint32_t glyph_index, bool truetype) {
  FT_Int32 load_flags =
      truetype ? FT_LOAD_MONOCHROME | FT_LOAD_NO_BITMAP |
                     FT_LOAD_LINEAR_DESIGN | FT_LOAD_PEDANTIC
               : FT_LOAD_MONOCHROME | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP |
                     FT_LOAD_LINEAR_DESIGN;
  FT_Library library = rec->glyph->library;
  FT_UInt version = TT_INTERPRETER_VERSION_40;
  FT_Property_Get(library, "truetype", "interpreter-version", &version);
  FT_UInt v35 = TT_INTERPRETER_VERSION_35;
  FT_Property_Set(library, "truetype", "interpreter-version", &v35);
  FT_Error error = FT_Load_Glyph(rec, glyph_index, load_flags);
  if (error == FT_Err_Invalid_Argument || error == FT_Err_Invalid_Reference ||
      error == FT_Err_Invalid_Glyph_Index ||
      error == FT_Err_DEF_In_Glyf_Bytecode ||
      (error >= FT_Err_Invalid_Opcode &&
       error <= FT_Err_Too_Many_Instruction_Defs)) {
    const FT_Long flags = rec->face_flags;
    rec->face_flags &= ~FT_FACE_FLAG_TRICKY;
    error = FT_Load_Glyph(rec, glyph_index, load_flags | FT_LOAD_NO_HINTING);
    rec->face_flags = flags;
  }
  FT_Property_Set(library, "truetype", "interpreter-version", &version);
  return error;
}

// pdfking: `*matrix` (the glyph to device, in 16.16 of a 64 pixel em) as gs
// sizes FreeType for it at `dpi` (gxfapi.c, fapi_ft.c transform_decompose):
// the matrix in points, 16.16 rounded; its scale, the smaller of its rows',
// the char size in points at `dpi`; the rest, in gs's fixed point, the
// transform left in `*matrix`.
void GsDecompose(FT_FaceRec* rec, float dpi, bool truetype, FT_Matrix* matrix) {
  auto fracint = [](double v) {
    return static_cast<FT_Fixed>(floor(v + 0.5));
  };
  // pixels per em = 16.16 / 1024; points = pixels * 72 / dpi. A Type 1 or
  // CFF font gs hands FreeType re-serialized, a unit its em (units_per_EM
  // 1), the font's matrix in the transform instead: the same here, the
  // face's units per em set to 1 while the size is set
  const FT_UShort units = rec->units_per_EM;
  const double unit = truetype || units == 0 ? 1.0 : 1.0 / units;
  if (!truetype) {
    rec->units_per_EM = 1;
  }
  const double to_points = 64.0 / 65536.0 * 72.0 / dpi * 65536.0 * unit;
  FT_Matrix transform = {fracint(matrix->xx * to_points),
                         fracint(matrix->xy * to_points),
                         fracint(matrix->yx * to_points),
                         fracint(matrix->yy * to_points)};
  FT_UInt xres = static_cast<FT_UInt>(dpi);
  FT_UInt yres = xres;
  double scalex = hypot(static_cast<double>(transform.xx),
                        static_cast<double>(transform.xy));
  double scaley = hypot(static_cast<double>(transform.yx),
                        static_cast<double>(transform.yy));
  double fact = 1.0;
  double factx = 1.0;
  double facty = 1.0;
  if (scalex < 64.0 || scaley < 64.0) {
    factx = 64.0 / scalex;
    facty = 64.0 / scaley;
    FT_Matrix scaled = {static_cast<FT_Fixed>(transform.xx * factx),
                        static_cast<FT_Fixed>(transform.xy * facty),
                        static_cast<FT_Fixed>(transform.yx * factx),
                        static_cast<FT_Fixed>(transform.yy * facty)};
    transform = scaled;
    scalex = hypot(static_cast<double>(transform.xx),
                   static_cast<double>(transform.xy));
    scaley = hypot(static_cast<double>(transform.yx),
                   static_cast<double>(transform.yy));
  }
  xres = static_cast<FT_UInt>(xres / factx);
  yres = static_cast<FT_UInt>(yres / facty);
  scalex *= 1.0 / 65536.0;
  scaley *= 1.0 / 65536.0;
  scalex = scaley = std::min(scalex, scaley);
  if (scalex < 1.0) {
    fact = 1.016 / scalex;
    scalex *= fact;
    scaley *= fact;
  }
  if (scalex * xres < 2268.0 / 64.0) {
    fact = (2400.0 / 64.0) / (xres * scalex);
    scaley *= fact;
    scalex *= fact;
  }
  const double upe =
      rec->units_per_EM > 512 ? static_cast<double>(rec->units_per_EM) : 512.0;
  fact = 1.0;
  while (scaley * yres > upe * 72.0 && xres > 0 && yres > 0 && scalex > 0.0 &&
         scaley > 0.0) {
    if (scaley < yres) {
      xres >>= 1;
      yres >>= 1;
      fact *= 2.0;
    } else {
      scalex /= 1.25;
      scaley /= 1.25;
    }
  }
  FT_Matrix scale = {static_cast<FT_Fixed>((65536.0 / scalex) * fact), 0, 0,
                     static_cast<FT_Fixed>((65536.0 / scaley) * fact)};
  FT_Matrix_Multiply(&transform, &scale);
  *matrix = scale;
  FT_Set_Char_Size(rec, static_cast<FT_F26Dot6>(scalex * 64),
                   static_cast<FT_F26Dot6>(scaley * 64), xres, yres);
  rec->units_per_EM = units;
}

#endif  // defined(PDF_ENABLE_FREETYPE)

double CFX_Face::GsAdvance(uint32_t glyph_index) {
#if defined(PDF_ENABLE_FREETYPE)
  FT_FaceRec* rec = GetRec();
  if (!rec || rec->units_per_EM == 0 ||
      FT_Load_Glyph(rec, glyph_index,
                    FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                        FT_LOAD_LINEAR_DESIGN | FT_LOAD_NO_BITMAP)) {
    return 0;
  }
  // with FT_LOAD_LINEAR_DESIGN: font units
  return static_cast<double>(rec->glyph->linearHoriAdvance) /
         rec->units_per_EM;
#else
  return 0;
#endif
}

std::unique_ptr<CFX_Path> CFX_Face::GsGlyphOutline(uint32_t glyph_index,
                                                   const CFX_Matrix& matrix,
                                                   float gs_dpi) {
#if defined(PDF_ENABLE_FREETYPE)
  FT_FaceRec* rec = GetRec();
  if (!rec || gs_dpi <= 0) {
    return nullptr;
  }
  FT_Matrix ft_matrix;
  ft_matrix.xx = matrix.a / 64 * 65536;
  ft_matrix.xy = matrix.c / 64 * 65536;
  ft_matrix.yx = matrix.b / 64 * 65536;
  ft_matrix.yy = matrix.d / 64 * 65536;
  const char* font_format = FT_Get_Font_Format(rec);
  const bool truetype = font_format && strcmp(font_format, "TrueType") == 0;
  GsDecompose(rec, gs_dpi, truetype, &ft_matrix);
  FT_Set_Transform(rec, &ft_matrix, nullptr);
  const int error = GsLoadGlyph(rec, glyph_index, truetype);
  FT_Matrix identity = {0x10000L, 0L, 0L, 0x10000L};
  FT_Set_Transform(rec, &identity, nullptr);
  FT_Set_Pixel_Sizes(rec, 64, 64);
  if (error) {
    return nullptr;
  }
  FT_Outline_Funcs funcs;
  funcs.move_to = GsMoveTo;
  funcs.line_to = GsLineTo;
  funcs.conic_to = GsConicTo;
  funcs.cubic_to = GsCubicTo;
  funcs.shift = 0;
  funcs.delta = 0;
  auto path = std::make_unique<CFX_Path>();
  GsOutline outline;
  outline.path_ = path.get();
  FT_Outline_Decompose(&rec->glyph->outline, &funcs, &outline);
  if (path->GetPoints().empty()) {
    return nullptr;
  }
  if (outline.need_close) {
    path->ClosePath();
  }
  return path;
#else
  return nullptr;
#endif
}
