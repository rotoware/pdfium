// pdfking: tiling pattern instances and steps as Ghostscript 10.08's (see
// cpdf_gstiling.h). Ported from Ghostscript 10.08 (AGPL).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html.

#include "core/fpdfapi/render/cpdf_gstiling.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include <algorithm>

namespace {

// gs_matrix: floats, as gs keeps them
struct Matrix {
  float xx, xy, yx, yy, tx, ty;
};

struct Rect {
  double px, py, qx, qy;
};

struct Point {
  double x, y;
};

int fastfloor(float x) {
  return static_cast<int>(x) - ((x < 0) && (x != static_cast<float>(static_cast<int>(x))));
}

int fastfloord(double x) {
  return static_cast<int>(x) - ((x < 0) && (x != static_cast<double>(static_cast<int>(x))));
}

// gsmatrix.c gs_point_transform, its float casts
Point Transform(double x, double y, const Matrix& m) {
  Point p;
  p.x = static_cast<float>(x * m.xx) + m.tx;
  p.y = static_cast<float>(y * m.yy) + m.ty;
  if (m.yx != 0) {
    p.x += static_cast<float>(y * m.yx);
  }
  if (m.xy != 0) {
    p.y += static_cast<float>(x * m.xy);
  }
  return p;
}

// gs_matrix_invert
bool Invert(const Matrix& m, Matrix* r) {
  if (m.xy == 0 && m.yx == 0) {
    if (m.xx == 0 || m.yy == 0) {
      return false;
    }
    r->xx = 1.0 / m.xx;
    r->tx = -r->xx * m.tx;
    r->xy = 0;
    r->yx = 0;
    r->yy = 1.0 / m.yy;
    r->ty = -r->yy * m.ty;
    return true;
  }
  float mxx = m.xx, myy = m.yy, mxy = m.xy, myx = m.yx;
  float mtx = m.tx, mty = m.ty;
  double det = static_cast<float>(mxx * myy) - static_cast<float>(mxy * myx);
  if (det == 0) {
    return false;
  }
  r->xx = myy / det;
  r->xy = -mxy / det;
  r->yx = -myx / det;
  r->yy = mxx / det;
  r->tx = ((static_cast<float>(mty * myx) - static_cast<float>(mtx * myy))) /
          det;
  r->ty = ((static_cast<float>(mtx * mxy) - static_cast<float>(mty * mxx))) /
          det;
  return true;
}

// gs_point_transform_inverse
bool TransformInverse(double x, double y, const Matrix& m, Point* p) {
  if (m.xy == 0 && m.yx == 0) {
    if (m.xx == 0 || m.yy == 0) {
      return false;
    }
    p->x = (x - m.tx) / m.xx;
    p->y = (y - m.ty) / m.yy;
    return true;
  }
  if (m.xx == 0 && m.yy == 0) {
    if (m.xy == 0 || m.yx == 0) {
      return false;
    }
    p->x = (y - m.ty) / m.xy;
    p->y = (x - m.tx) / m.yx;
    return true;
  }
  Matrix inv;
  if (!Invert(m, &inv)) {
    return false;
  }
  *p = Transform(x, y, inv);
  return true;
}

// gs_points_bbox
Rect PointsBBox(const Point pts[4]) {
  auto min_max = [](double v0, double v1, double* lo, double* hi) {
    if (v0 < v1) {
      *lo = v0, *hi = v1;
    } else {
      *lo = v1, *hi = v0;
    }
  };
  Rect r;
  double min01, max01, min23, max23;
  min_max(pts[0].x, pts[1].x, &min01, &max01);
  min_max(pts[2].x, pts[3].x, &min23, &max23);
  r.px = std::min(min01, min23);
  r.qx = std::max(max01, max23);
  min_max(pts[0].y, pts[1].y, &min01, &max01);
  min_max(pts[2].y, pts[3].y, &min23, &max23);
  r.py = std::min(min01, min23);
  r.qy = std::max(max01, max23);
  return r;
}

// gs_bbox_transform
Rect BBoxTransform(const Rect& in, const Matrix& m) {
  const Point pts[4] = {Transform(in.px, in.py, m), Transform(in.px, in.qy, m),
                        Transform(in.qx, in.py, m),
                        Transform(in.qx, in.qy, m)};
  return PointsBBox(pts);
}

// gs_bbox_transform_inverse
bool BBoxTransformInverse(const Rect& in, const Matrix& m, Rect* out) {
  Point pts[4];
  if (!TransformInverse(in.px, in.py, m, &pts[0]) ||
      !TransformInverse(in.px, in.qy, m, &pts[1]) ||
      !TransformInverse(in.qx, in.py, m, &pts[2]) ||
      !TransformInverse(in.qx, in.qy, m, &pts[3])) {
    return false;
  }
  *out = PointsBBox(pts);
  return true;
}

constexpr float kSaneThreshold = 0.001f;

int SaneCeil(float f) {
  int i = static_cast<int>(f);
  if (f - i < kSaneThreshold) {
    return i;
  }
  return i + 1;
}

float SaneClampFloat(float f) {
  int i = fastfloor(f);
  if (f - i < kSaneThreshold) {
    return static_cast<float>(i);
  }
  if (f - i > (1 - kSaneThreshold)) {
    return static_cast<float>(i + 1);
  }
  return f;
}

double SaneClampDouble(double d) {
  int i = fastfloord(d);
  if (d - i < kSaneThreshold) {
    return static_cast<double>(i);
  }
  if (d - i > (1 - kSaneThreshold)) {
    return static_cast<double>(i + 1);
  }
  return d;
}

void SaneClampRect(Rect* r) {
  double x0 = SaneClampDouble(r->px);
  double y0 = SaneClampDouble(r->py);
  double x1 = SaneClampDouble(r->qx);
  double y1 = SaneClampDouble(r->qy);
  // Be careful not to round stuff to zero (fts_15_1529.pdf)
  if (x0 != x1) {
    r->px = x0, r->qx = x1;
  }
  if (y0 != y1) {
    r->py = y0, r->qy = y1;
  }
}

void SaneClampMatrix(Matrix* m) {
  m->xx = SaneClampFloat(m->xx);
  m->xy = SaneClampFloat(m->xy);
  m->yx = SaneClampFloat(m->yx);
  m->yy = SaneClampFloat(m->yy);
  m->tx = SaneClampFloat(m->tx);
  m->ty = SaneClampFloat(m->ty);
}

// fixed: 24.8, as gs
constexpr float kFixedScale = 256.0f;

int64_t Float2FixedRounded(double f) {
  return static_cast<int64_t>(floor(f * kFixedScale + 0.5));
}

double Fixed2Float(int64_t x) {
  return x * (1.0 / kFixedScale);
}

struct Instance {
  // the saved gstate's ctm (its translation as gx_translate_to_fixed leaves
  // it)
  Matrix ctm;
  Matrix step;
  Rect bbox;  // in step space
  float xstep, ystep;
  Rect templ_bbox;
  int tiling_type;
  int size_x = 0, size_y = 0;
};

// gx_translate_to_fixed: the ctm's translation to fixed px, py
void TranslateToFixed(Matrix* ctm, int64_t px, int64_t py) {
  ctm->tx = Fixed2Float(px);
  ctm->ty = Fixed2Float(py);
}

// adjust_bbox_to_pixel_origin
void AdjustBBoxToPixelOrigin(Instance* inst, Rect* bbox) {
  float dx = bbox->px - floor(bbox->px + 0.5);
  float dy = bbox->py - floor(bbox->py + 0.5);
  if (dx != 0 || dy != 0) {
    bbox->px -= dx;
    bbox->py -= dy;
    bbox->qx -= dx;
    bbox->qy -= dy;
    TranslateToFixed(&inst->ctm, Float2FixedRounded(inst->ctm.tx - dx),
                     Float2FixedRounded(inst->ctm.ty - dy));
  }
  inst->step.tx = inst->ctm.tx;
  inst->step.ty = inst->ctm.ty;
}

// clamp_pattern_bbox: the bbox cut to the cells on the page
void ClampPatternBBox(const Instance& inst,
                      Rect* pbbox,
                      const FX_RECT& page,
                      const Matrix& pmat) {
  const int width = page.Width();
  const int height = page.Height();
  double xstep = inst.xstep;
  double ystep = inst.ystep;
  double xmin = pbbox->qx;
  double xmax = pbbox->px;
  double ymin = pbbox->qy;
  double ymax = pbbox->py;
  double xepsilon = FLT_EPSILON * width;
  double yepsilon = FLT_EPSILON * height;
  if (xstep < 0) {
    xstep *= -1;
  }
  if (ystep < 0) {
    ystep *= -1;
  }
  Rect dev_page = {static_cast<double>(page.left),
                   static_cast<double>(page.top),
                   static_cast<double>(page.right),
                   static_cast<double>(page.bottom)};
  Rect pat_page;
  if (!BBoxTransformInverse(dev_page, pmat, &pat_page)) {
    return;
  }
  Point dev_pat_origin = Transform(0.0, 0.0, pmat);
  int ixpat = static_cast<int>(floor((pat_page.px - inst.templ_bbox.qx) / xstep));
  int iystart =
      static_cast<int>(floor((pat_page.py - inst.templ_bbox.qy) / ystep));
  for (;; ixpat++) {
    double xpat = ixpat * xstep;
    for (int iypat = iystart;; iypat++) {
      double ypat = iypat * ystep;
      Point dev_step = Transform(xpat, ypat, pmat);
      double xdev = dev_step.x - dev_pat_origin.x;
      double ydev = dev_step.y - dev_pat_origin.y;
      double xlower =
          (xdev + pbbox->px > page.left) ? pbbox->px : -xdev + page.left;
      double xupper =
          (xdev + pbbox->qx < page.right) ? pbbox->qx : -xdev + page.right;
      double ylower =
          (ydev + pbbox->py > page.top) ? pbbox->py : -ydev + page.top;
      double yupper =
          (ydev + pbbox->qy < page.bottom) ? pbbox->qy : -ydev + page.bottom;
      if (xlower + xepsilon < xupper && ylower + yepsilon < yupper) {
        xmin = std::min(xmin, xlower);
        xmax = std::max(xmax, xupper);
        ymin = std::min(ymin, ylower);
        ymax = std::max(ymax, yupper);
      }
      if (ypat > pat_page.qy - inst.templ_bbox.py) {
        break;
      }
    }
    if (xpat > pat_page.qx - inst.templ_bbox.px) {
      break;
    }
  }
  if (xmin < xmax && ymin < ymax) {
    pbbox->px = xmin;
    pbbox->qx = xmax;
    pbbox->py = ymin;
    pbbox->qy = ymax;
  } else {
    pbbox->px = page.left;
    pbbox->py = page.top;
    pbbox->qx = page.left + 1;
    pbbox->qy = page.top + 1;
  }
}

// compute_inst_matrix
bool ComputeInstMatrix(Instance* inst,
                       Rect* pbbox,
                       const FX_RECT& page,
                       float* pbbw,
                       float* pbbh) {
  Matrix m = inst->ctm;
  m.tx = 0;
  m.ty = 0;
  *pbbox = BBoxTransform(inst->templ_bbox, m);
  SaneClampRect(pbbox);
  *pbbw = pbbox->qx - pbbox->px;
  *pbbh = pbbox->qy - pbbox->py;
  pbbox->px += inst->ctm.tx;
  pbbox->py += inst->ctm.ty;
  pbbox->qx += inst->ctm.tx;
  pbbox->qy += inst->ctm.ty;
  AdjustBBoxToPixelOrigin(inst, pbbox);

  const double kMaxFloat = FLT_MAX;
  double mult = static_cast<double>(inst->xstep) * inst->ctm.xx;
  if (mult < -kMaxFloat || mult > kMaxFloat) {
    return false;
  }
  float xx = static_cast<float>(mult);
  mult = static_cast<double>(inst->xstep) * inst->ctm.xy;
  if (mult < -kMaxFloat || mult > kMaxFloat) {
    return false;
  }
  float xy = mult;
  mult = static_cast<double>(inst->ystep) * inst->ctm.yx;
  if (mult < -kMaxFloat || mult > kMaxFloat) {
    return false;
  }
  float yx = mult;
  mult = static_cast<double>(inst->ystep) * inst->ctm.yy;
  if (mult < -kMaxFloat || mult > kMaxFloat) {
    return false;
  }
  float yy = mult;
  // all coefficients >= 0
  if (xx == 0 || yy == 0) {
    std::swap(xx, yx);
    std::swap(xy, yy);
  }
  if (xx < 0) {
    xx = -xx, xy = -xy;
  }
  if (yy < 0) {
    yx = -yx, yy = -yy;
  }
  inst->step.xx = xx;
  inst->step.xy = xy;
  inst->step.yx = yx;
  inst->step.yy = yy;
  SaneClampMatrix(&inst->step);
  // a cell larger than the page: cut to the cells on it
  if (pbbox->qx - pbbox->px > page.Width() ||
      pbbox->qy - pbbox->py > page.Height()) {
    ClampPatternBBox(*inst, pbbox, page, inst->ctm);
  }
  return true;
}

// fix_bbox_after_matrix_adjustment
void FixBBoxAfterMatrixAdjustment(Instance* inst, Rect* pbbox) {
  *pbbox = BBoxTransform(inst->templ_bbox, inst->ctm);
  AdjustBBoxToPixelOrigin(inst, pbbox);
}

// gs_scale on the saved gstate
void Scale(Matrix* ctm, double sx, double sy) {
  ctm->xx *= sx;
  ctm->xy *= sx;
  ctm->yx *= sy;
  ctm->yy *= sy;
}

float MatrixExpansion(const Matrix& m) {
  float e = fabsf(m.xx);
  e = std::max(e, fabsf(m.xy));
  e = std::max(e, fabsf(m.yx));
  e = std::max(e, fabsf(m.yy));
  return e * e;
}

int ThresholdCeil(float f, float e) {
  int i = fastfloor(f);
  if (f - i < e) {
    return i;
  }
  return i + 1;
}

int ThresholdFloor(float f, float e) {
  int i = fastfloor(f);
  if (f - i > 1 - e) {
    return i + 1;
  }
  return i;
}

Matrix ToGs(const CFX_Matrix& m) {
  return {m.a, m.b, m.c, m.d, m.e, m.f};
}

CFX_Matrix FromGs(const Matrix& m) {
  return CFX_Matrix(m.xx, m.xy, m.yx, m.yy, m.tx, m.ty);
}

}  // namespace

// gs_pattern1_make_pattern (ADJUST_AS_ADOBE), the device's fill adjust
// being gs's 0.5 (any part of a pixel)
std::optional<GsTile> MakeGsTile(const CFX_Matrix& pattern2device,
                                 const CFX_FloatRect& bbox,
                                 float xstep,
                                 float ystep,
                                 int tiling_type,
                                 const FX_RECT& page) {
  if (xstep == 0 || ystep == 0) {
    return std::nullopt;
  }
  Instance inst;
  inst.ctm = ToGs(pattern2device);
  inst.xstep = xstep;
  inst.ystep = ystep;
  inst.tiling_type = tiling_type;
  // pdfi_pattern_get_rect: normalized, an empty side widened (FixPatternBox)
  inst.templ_bbox = {std::min(bbox.left, bbox.right),
                     std::min(bbox.bottom, bbox.top),
                     std::max(bbox.left, bbox.right),
                     std::max(bbox.bottom, bbox.top)};
  if (inst.templ_bbox.px - inst.templ_bbox.qx == 0) {
    inst.templ_bbox.qx += .00000001;
  }
  if (inst.templ_bbox.py - inst.templ_bbox.qy == 0) {
    inst.templ_bbox.qy += .00000001;
  }
  Rect dbox;
  float bbw, bbh;
  if (!ComputeInstMatrix(&inst, &dbox, page, &bbw, &bbh)) {
    return std::nullopt;
  }
  if (tiling_type == 2) {
    inst.size_x = SaneCeil(bbw);
    inst.size_y = SaneCeil(bbh);
  } else {
    inst.size_x = static_cast<int>(floor(bbw + 0.5));
    inst.size_y = static_cast<int>(floor(bbh + 0.5));
  }
  if (inst.size_x <= 0) {
    inst.size_x = bbw > 0 ? 1 : 0;
  }
  if (inst.size_y <= 0) {
    inst.size_y = bbh > 0 ? 1 : 0;
  }
  Matrix& step = inst.step;
  if (inst.size_x == 0 || inst.size_y == 0) {
    // empty: paints nothing
    return std::nullopt;
  }
  if (fabs(step.xx * step.yy - step.xy * step.yx) < 1.0e-9) {
    return std::nullopt;
  }
  if (tiling_type != 2) {
    if (step.xy == 0 && step.yx == 0 && fabs(step.xx - bbw) < 0.5 &&
        fabs(step.yy - bbh) < 0.5) {
      if (step.xx <= 2) {
        Scale(&inst.ctm, fabs(inst.size_x / step.xx), 1);
        step.xx = static_cast<float>(inst.size_x);
      } else {
        // the step whole pixels, the cell's content bent to fill it, the
        // middle of the cell kept, by whole pixels
        Scale(&inst.ctm, (inst.size_x - 1.0 / kFixedScale) / step.xx, 1);
        float cx = (dbox.px + dbox.qx) / 2;
        float ox = (cx - step.tx) / step.xx;
        float dx = ox * (inst.size_x - step.xx);
        dx = floor(dx + 0.5);
        step.xx = static_cast<float>(inst.size_x);
        inst.ctm.tx -= dx;
      }
      if (step.yy <= 2) {
        Scale(&inst.ctm, 1, inst.size_y / step.yy);
        step.yy = static_cast<float>(inst.size_y);
      } else {
        Scale(&inst.ctm, 1, (inst.size_y - 1.0 / kFixedScale) / step.yy);
        float cy = (dbox.py + dbox.qy) / 2;
        float oy = (cy - step.ty) / step.yy;
        float dy = oy * (inst.size_y - step.yy);
        dy = floor(dy + 0.5);
        step.yy = static_cast<float>(inst.size_y);
        inst.ctm.ty -= dy;
      }
      FixBBoxAfterMatrixAdjustment(&inst, &dbox);
    }
  }
  // (TilingType 2 shifts only with no fill adjust; gs's is 0.5)
  GsTile tile;
  Rect ibox;
  if (!BBoxTransformInverse(dbox, step, &ibox)) {
    return std::nullopt;
  }
  tile.simple = step.xx == inst.size_x && step.xy == 0 && step.yx == 0 &&
                step.yy == inst.size_y;
  TranslateToFixed(&inst.ctm, Float2FixedRounded(step.tx - dbox.px),
                   Float2FixedRounded(step.ty - dbox.py));
  step.tx = dbox.px;
  step.ty = dbox.py;
  tile.ctm = FromGs(inst.ctm);
  tile.step = FromGs(step);
  tile.width = inst.size_x;
  tile.height = inst.size_y;
  tile.bbox[0] = ibox.px;
  tile.bbox[1] = ibox.py;
  tile.bbox[2] = ibox.qx;
  tile.bbox[3] = ibox.qy;
  return tile;
}

// tile_by_steps (phase 0)
void GsTileSteps(const GsTile& tile,
                 const FX_RECT& rect,
                 const std::function<void(int x, int y)>& place) {
  const Matrix step = ToGs(tile.step);
  const Rect bbox = {static_cast<double>(rect.left),
                     static_cast<double>(rect.top),
                     static_cast<double>(rect.right),
                     static_cast<double>(rect.bottom)};
  Rect ibbox;
  if (!BBoxTransformInverse(bbox, step, &ibbox)) {
    return;
  }
  const double bbw = tile.bbox[2] - tile.bbox[0];
  const double bbh = tile.bbox[3] - tile.bbox[1];
  const float error = 1 / MatrixExpansion(step);
  double u0 = ibbox.px - std::max(tile.bbox[0], 0.0);
  double v0 = ibbox.py - std::max(tile.bbox[1], 0.0);
  double u1 = ibbox.qx - std::min(tile.bbox[2], 0.0);
  double v1 = ibbox.qy - std::min(tile.bbox[3], 0.0);
  if (!tile.simple) {
    u0 -= bbw, v0 -= bbh, u1 += bbw, v1 += bbh;
  }
  const int i0 = ThresholdFloor(u0, error);
  const int j0 = ThresholdFloor(v0, error);
  const int i1 = ThresholdCeil(u1, error);
  const int j1 = ThresholdCeil(v1, error);
  for (int i = i0; i < i1; i++) {
    for (int j = j0; j < j1; j++) {
      const int x = fastfloor(step.xx * i + step.yx * j + step.tx);
      const int y = fastfloor(step.xy * i + step.yy * j + step.ty);
      if (x == INT_MIN || y == INT_MIN) {
        continue;
      }
      if (x >= rect.right || y >= rect.bottom || x + tile.width <= rect.left ||
          y + tile.height <= rect.top) {
        continue;
      }
      place(x, y);
    }
  }
}
