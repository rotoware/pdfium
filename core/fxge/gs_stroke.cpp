// core/fxge/gs_stroke.cpp (pdfking: Ghostscript's stroker, gxstroke.c, ported)
//
// Ported from Ghostscript 10.08 base/gxstroke.c (AGPL). Also ports the parts
// of gxpcopy.c, gxpflat.c, gxpdash.c, gsline.c, gsmatrix.c, gsmisc.c and
// gdevddrw.c/gxdtfill.h that the stroker uses. See gs_stroke.h.
// Copyright (C) 2001-2025 Artifex Software, Inc. (original code)

#include "core/fxge/gs_stroke.h"

#include <math.h>

#include <algorithm>
#include <cstdlib>

namespace gs_stroke {
namespace {

// ---------------------------------------------------------------- gxfixed.h
constexpr int kShift = 8;
constexpr fixed fixed_0 = 0;
constexpr fixed fixed_epsilon = 1;
constexpr fixed fixed_1 = 1 << kShift;
constexpr fixed fixed_half = fixed_1 >> 1;
constexpr fixed max_fixed = INT32_MAX;
constexpr fixed min_fixed = INT32_MIN;
constexpr int max_int_in_fixed = max_fixed >> kShift;

inline fixed int2fixed(int i) {
  return static_cast<fixed>(static_cast<uint32_t>(i) << kShift);
}
inline int fixed2int_var(fixed x) { return x >> kShift; }
inline long fixed2long(fixed x) { return x >> kShift; }
inline fixed fixed_floor(fixed x) { return x & -(1 << kShift); }
inline fixed fixed_rounded(fixed x) {
  return (x + (fixed_1 >> 1)) & -(1 << kShift);
}
inline fixed fixed_pixround(fixed x) {
  return (x + (fixed_half - fixed_epsilon)) & -(1 << kShift);
}
inline bool fixed_is_int(fixed x) { return !(x & (fixed_1 - 1)); }
inline fixed float2fixed(double f) {
  return static_cast<fixed>(static_cast<float>(f) * static_cast<float>(256));
}
inline fixed arith_rshift_1(fixed x) { return x >> 1; }
template <typename T>
inline T any_abs(T x) {
  return x < 0 ? -x : x;
}

// floor(A * B / C), C > 0 (gsmisc.c fixed_mult_quo, exact).
fixed fixed_mult_quo(fixed a, fixed b, fixed c) {
  int64_t p = static_cast<int64_t>(a) * b;
  int64_t q = p / c;
  if ((p % c) != 0 && ((p < 0) != (c < 0)))
    q--;
  return static_cast<fixed>(q);
}
// gdevddrw.c fixed_mult_rem: 32-bit wrapping arithmetic like the C original.
fixed fixed_mult_rem(fixed a, fixed b, fixed c) {
  uint32_t r = static_cast<uint32_t>(a) * static_cast<uint32_t>(b) -
               static_cast<uint32_t>(fixed_mult_quo(a, b, c)) *
                   static_cast<uint32_t>(c);
  return static_cast<fixed>(r);
}

bool check_diff_overflow(fixed v0, fixed v1) {
  if (v1 > 0)
    return (v0 < min_fixed + v1);
  else if (v1 < 0)
    return (v0 > max_fixed + v1);
  return false;
}
bool check_sum_overflow(fixed v0, fixed v1) {
  return check_diff_overflow(v0, -v1);
}

// ---------------------------------------------------------------- params
enum Cap { gs_cap_butt = 0, gs_cap_round = 1, gs_cap_square = 2,
           gs_cap_triangle = 3 };
enum Join { gs_join_miter = 0, gs_join_round = 1, gs_join_bevel = 2,
            gs_join_none = 3, gs_join_triangle = 4 };

struct Matrix {  // gs_matrix linear part (floats)
  float xx, xy, yx, yy;
};

struct DashParams {
  std::vector<float> pattern;
  float pattern_length = 0;
  bool init_ink_on = true;
  int init_index = 0;
  float init_dist_left = 0;
};

struct LineParams {
  float half_width;
  int start_cap, end_cap, dash_cap;
  int join;
  int curve_join = -1;
  float miter_limit;
  float miter_check;
  float dot_length = 0;
  DashParams dash;
};

struct GState {
  Matrix ctm;
  LineParams lp;
  fixed fill_adjust_x = fixed_half, fill_adjust_y = fixed_half;
  bool stroke_adjust;
  float flatness;
  bool accurate_curves = true;
};

// gsline.c gx_set_miter_limit
void SetMiterLimit(LineParams* lp, double limit) {
  lp->miter_limit = static_cast<float>(limit);
  double limit_squared = limit * limit;
  if (limit_squared < 2.0001 && limit_squared > 1.9999)
    lp->miter_check = 1.0e6f;
  else
    lp->miter_check =
        static_cast<float>(sqrt(limit_squared - 1) * 2 / (limit_squared - 2));
}

// gsline.c gx_set_dash. Returns false on rangecheck (pdfi then keeps the
// previous dash; we stroke solid).
bool SetDash(DashParams* dash, const std::vector<float>& pattern,
             double offset) {
  bool ink = true;
  int index = 0;
  float pattern_length = 0.0f;
  float dist_left;
  size_t length = pattern.size();
  for (float elt : pattern) {
    if (elt < 0)
      return false;
    pattern_length += elt;
  }
  if (length == 0) {
    dist_left = 0.0f;
  } else {
    if (pattern_length == 0)
      return false;
#define f_mod(a, b) ((a) - floor((a) / (b)) * (b))
    if (length & 1) {
      float length2 = pattern_length * 2;
      dist_left = static_cast<float>(f_mod(offset, length2));
      dist_left = static_cast<float>(f_mod(dist_left, length2));
      if (dist_left >= pattern_length)
        dist_left -= pattern_length, ink = !ink;
    } else {
      dist_left = static_cast<float>(f_mod(offset, pattern_length));
      dist_left = static_cast<float>(f_mod(dist_left, pattern_length));
    }
#undef f_mod
    if (dist_left > pattern_length)
      return false;
    while ((dist_left -= pattern[index]) >= 0 &&
           (dist_left > 0 || pattern[index] != 0))
      ink = !ink, index++;
  }
  dash->pattern = pattern;
  dash->pattern_length = pattern_length;
  dash->init_ink_on = ink;
  dash->init_index = index;
  dash->init_dist_left = -dist_left;
  return true;
}

// gsmatrix.c gs_distance_transform_inverse
bool DistanceTransformInverse(double dx, double dy, const Matrix& m,
                              double* px, double* py) {
  if (m.xy == 0.0f && m.yx == 0.0f) {
    if (m.xx == 0.0f || m.yy == 0.0f)
      return false;
    *px = dx / m.xx;
    *py = dy / m.yy;
  } else if (m.xx == 0.0f && m.yy == 0.0f) {
    if (m.xy == 0.0f || m.yx == 0.0f)
      return false;
    *px = dy / m.xy;
    *py = dx / m.yx;
  } else {
    double det = m.xx * m.yy - m.xy * m.yx;
    if (det == 0)
      return false;
    *px = (dx * m.yy - dy * m.yx) / det;
    *py = (dy * m.xx - dx * m.xy) / det;
  }
  return true;
}

// ---------------------------------------------------------------- gx_path
enum SegType { s_start, s_line, s_line_close, s_curve, s_gap, s_dash };
enum Notes { sn_none = 0, sn_not_first = 1, sn_from_arc = 2,
             sn_dash_head = 4, sn_dash_tail = 8 };
constexpr double quarter_arc_fraction = 0.55228474983079334;

struct Point {
  fixed x, y;
};

struct Seg {
  SegType type;
  Point pt;
  Point p1{0, 0}, p2{0, 0};  // curve control points
  Point tangent{0, 0};       // s_dash
  int notes = sn_none;
  bool is_closed = false;  // s_start only
  int last = -1;           // s_start only: index of last segment of subpath
};

constexpr int NIL = -1;

// A gs path held as one flat array of segments; subpaths begin at s_start.
class Path {
 public:
  std::vector<Seg> s;
  Point position{0, 0};
  bool subpath_open = false;    // path_subpath_open
  bool last_is_moveto = false;  // path_last_is_moveto
  int current_subpath = NIL;
  int curve_count = 0;

  bool is_void() const { return current_subpath == NIL; }
  int next(int i) const { return i + 1 < static_cast<int>(s.size()) ? i + 1 : NIL; }
  int prev(int i) const { return i - 1; }
  int first_subpath() const { return s.empty() ? NIL : 0; }
  int last_index() const { return static_cast<int>(s.size()) - 1; }

  void add_point(fixed x, fixed y) {
    position = {x, y};
    subpath_open = true;
    last_is_moveto = true;
  }
  void new_subpath() {
    Seg st;
    st.type = s_start;
    st.pt = position;
    s.push_back(st);
    current_subpath = last_index();
    s[current_subpath].last = current_subpath;
    subpath_open = true;
    last_is_moveto = false;
  }
  // path_open: lazily create the subpath for a drawing operation.
  void open() {
    if (last_is_moveto || !subpath_open)
      new_subpath();
  }
  void push(const Seg& sg) {
    s.push_back(sg);
    s[current_subpath].last = last_index();
    position = sg.pt;
    last_is_moveto = false;
  }
  void add_line_notes(fixed x, fixed y, int notes) {
    open();
    Seg sg;
    sg.type = s_line;
    sg.pt = {x, y};
    sg.notes = notes;
    push(sg);
  }
  void add_lines_notes(const Point* pts, int count, int notes) {
    for (int i = 0; i < count; i++)
      add_line_notes(pts[i].x, pts[i].y, notes);
  }
  void add_gap_notes(fixed x, fixed y, int notes) {
    open();
    Seg sg;
    sg.type = s_gap;
    sg.pt = {x, y};
    sg.notes = notes;
    push(sg);
  }
  void add_dash_notes(fixed x, fixed y, fixed dx, fixed dy, int notes) {
    open();
    Seg sg;
    sg.type = s_dash;
    sg.pt = {x, y};
    sg.tangent = {dx, dy};
    sg.notes = notes;
    push(sg);
  }
  void add_curve_notes(fixed x1, fixed y1, fixed x2, fixed y2, fixed x3,
                       fixed y3, int notes) {
    open();
    Seg sg;
    sg.type = s_curve;
    sg.p1 = {x1, y1};
    sg.p2 = {x2, y2};
    sg.pt = {x3, y3};
    sg.notes = notes;
    push(sg);
    curve_count++;
  }
  // gxpath.c gx_path_add_partial_arc_notes
  void add_partial_arc(fixed x3, fixed y3, fixed xt, fixed yt,
                       double fraction) {
    fixed x0 = position.x, y0 = position.y;
    add_curve_notes(x0 + static_cast<fixed>((xt - x0) * fraction),
                    y0 + static_cast<fixed>((yt - y0) * fraction),
                    x3 + static_cast<fixed>((xt - x3) * fraction),
                    y3 + static_cast<fixed>((yt - y3) * fraction), x3, y3,
                    sn_from_arc);
  }
  void close_subpath_notes(int notes) {
    if (!subpath_open)
      return;
    if (last_is_moveto)
      new_subpath();
    Seg sg;
    sg.type = s_line_close;
    sg.pt = s[current_subpath].pt;
    sg.notes = notes;
    s[current_subpath].is_closed = true;
    push(sg);
    subpath_open = false;
  }
  void close_subpath() { close_subpath_notes(sn_none); }

  std::vector<Segment> ToSegments() const {
    std::vector<Segment> out;
    for (const Seg& sg : s) {
      Segment o{};
      o.x = sg.pt.x;
      o.y = sg.pt.y;
      switch (sg.type) {
        case s_start:
          o.type = Segment::Type::kMove;
          break;
        case s_line_close:
          o.type = Segment::Type::kClose;
          break;
        case s_curve:
          o.type = Segment::Type::kCurve;
          o.c1x = sg.p1.x, o.c1y = sg.p1.y, o.c2x = sg.p2.x, o.c2y = sg.p2.y;
          break;
        default:
          o.type = Segment::Type::kLine;
          break;
      }
      out.push_back(o);
    }
    return out;
  }
};

// gxpath2.c gx_path_bbox (box of all points incl. control points)
void PathBBox(const Path& p, fixed* px, fixed* py, fixed* qx, fixed* qy) {
  if (p.s.empty()) {
    *px = *qx = p.position.x;
    *py = *qy = p.position.y;
    return;
  }
  *px = *qx = p.s[0].pt.x;
  *py = *qy = p.s[0].pt.y;
  auto add = [&](Point pt) {
    *px = std::min(*px, pt.x);
    *qx = std::max(*qx, pt.x);
    *py = std::min(*py, pt.y);
    *qy = std::max(*qy, pt.y);
  };
  for (const Seg& sg : p.s) {
    add(sg.pt);
    if (sg.type == s_curve) {
      add(sg.p1);
      add(sg.p2);
    }
  }
}

// ---------------------------------------------------------------- gxpflat.c
int curve_log2_samples(fixed x0, fixed y0, const Seg& pc, fixed fixed_flat) {
  fixed x03 = pc.pt.x - x0, y03 = pc.pt.y - y0;
  int k;
  if (x03 < 0)
    x03 = -x03;
  if (y03 < 0)
    y03 = -y03;
  if ((x03 | y03) < int2fixed(16))
    fixed_flat >>= 1;
  if (fixed_flat == 0) {
    fixed m = std::max(x03, y03);
    for (k = 1; m > fixed_1;)
      k++, m >>= 1;
  } else {
    const fixed x12 = pc.p1.x - pc.p2.x, y12 = pc.p1.y - pc.p2.y,
                dx0 = x0 - pc.p1.x - x12, dy0 = y0 - pc.p1.y - y12,
                dx1 = x12 - pc.p2.x + pc.pt.x, dy1 = y12 - pc.p2.y + pc.pt.y,
                adx0 = any_abs(dx0), ady0 = any_abs(dy0),
                adx1 = any_abs(dx1), ady1 = any_abs(dy1);
    fixed d = std::max(adx0, adx1) + std::max(ady0, ady1);
    uint32_t qtmp = d - (d >> 2) + fixed_flat - 1;
    uint32_t q = qtmp / static_cast<uint32_t>(fixed_flat);
    for (k = 0; q > 1;)
      k++, q = (q + 3) >> 2;
  }
  return k;
}

void split_curve_midpoint(fixed x0, fixed y0, const Seg& pc_in, Seg* pc1,
                          Seg* pc2) {
#define midpoint(a, b) (arith_rshift_1(a) + arith_rshift_1(b) + (((a) | (b)) & 1))
  Seg pc = pc_in;  // pc2 may alias pc_in
  fixed x12 = midpoint(pc.p1.x, pc.p2.x);
  fixed y12 = midpoint(pc.p1.y, pc.p2.y);
  pc1->p1.x = midpoint(x0, pc.p1.x);
  pc1->p1.y = midpoint(y0, pc.p1.y);
  pc2->p2.x = midpoint(pc.p2.x, pc.pt.x);
  pc2->p2.y = midpoint(pc.p2.y, pc.pt.y);
  pc1->p2.x = midpoint(pc1->p1.x, x12);
  pc1->p2.y = midpoint(pc1->p1.y, y12);
  pc2->p1.x = midpoint(x12, pc2->p2.x);
  pc2->p1.y = midpoint(y12, pc2->p2.y);
  pc2->pt = pc.pt;
  pc1->pt.x = midpoint(pc1->p2.x, pc2->p1.x);
  pc1->pt.y = midpoint(pc1->p2.y, pc2->p1.y);
#undef midpoint
}

constexpr int k_sample_max = 10;

bool curve_coeffs_ranged(fixed x0, fixed x1, fixed x2, fixed x3, fixed y0,
                         fixed y1, fixed y2, fixed y3, fixed* ax, fixed* bx,
                         fixed* cx, fixed* ay, fixed* by, fixed* cy, int k) {
  fixed t01, t12;
  t01 = x1 - x0, *cx = (t01 << 1) + t01;
  t12 = x2 - x1, *bx = (t12 << 1) + t12 - *cx;
  *ax = x3 - *bx - *cx - x0;
  t01 = y1 - y0, *cy = (t01 << 1) + t01;
  t12 = y2 - y1, *by = (t12 << 1) + t12 - *cy;
  *ay = y3 - *by - *cy - y0;
  const fixed max_fast = max_fixed / 6, min_fast = -max_fast;
  auto in_range = [&](fixed v) { return v < max_fast && v > min_fast; };
  if (k > k_sample_max || !in_range(*ax) || !in_range(*ay) ||
      !in_range(*bx) || !in_range(*by) || !in_range(*cx) || !in_range(*cy))
    return false;
  return true;
}

struct FlatIter {
  fixed x0, y0, x3, y3;
  fixed cx, bx, ax, cy, by, ay;
  fixed x, y;
  uint32_t i, k;
  uint32_t rmask;
  fixed idx, idy, id2x, id2y, id3x, id3y;
  uint32_t rx, ry, rdx, rdy, rd2x, rd2y, rd3x, rd3y;
  fixed lx0, ly0, lx1, ly1;

  bool init(fixed px0, fixed py0, const Seg& pc, int kk) {
    fixed x1 = pc.p1.x, y1 = pc.p1.y, x2 = pc.p2.x, y2 = pc.p2.y;
    x0 = lx0 = lx1 = px0;
    y0 = ly0 = ly1 = py0;
    x3 = pc.pt.x;
    y3 = pc.pt.y;
    if (!curve_coeffs_ranged(x0, x1, x2, x3, y0, y1, y2, y3, &ax, &bx, &cx,
                             &ay, &by, &cy, kk))
      return false;
    k = kk;
    if (kk == -1)
      return true;
    const int k2 = kk << 1, k3 = k2 + kk;
    rmask = (1u << k3) - 1;
    i = (1u << kk);
    rx = ry = 0;
    fixed bx2 = bx << 1, by2 = by << 1;
    fixed ax6 = ((ax << 1) + ax) << 1, ay6 = ((ay << 1) + ay) << 1;
    idx = cx >> kk;
    idy = cy >> kk;
    rdx = (static_cast<uint32_t>(cx) << k2) & rmask;
    rdy = (static_cast<uint32_t>(cy) << k2) & rmask;
    id2x = bx2 >> k2;
    id2y = by2 >> k2;
    rd2x = (static_cast<uint32_t>(bx2) << kk) & rmask;
    rd2y = (static_cast<uint32_t>(by2) << kk) & rmask;
#define adjust_rem(r, q, rm) \
  if (r > rm)                \
  q++, r &= rm
    idx += arith_rshift_1(id2x);
    idy += arith_rshift_1(id2y);
    rdx += (static_cast<uint32_t>(bx) << kk) & rmask;
    rdy += (static_cast<uint32_t>(by) << kk) & rmask;
    adjust_rem(rdx, idx, rmask);
    adjust_rem(rdy, idy, rmask);
    idx += ax >> k3;
    idy += ay >> k3;
    rdx += static_cast<uint32_t>(ax) & rmask;
    rdy += static_cast<uint32_t>(ay) & rmask;
    adjust_rem(rdx, idx, rmask);
    adjust_rem(rdy, idy, rmask);
    id2x += id3x = ax6 >> k3;
    id2y += id3y = ay6 >> k3;
    rd2x += rd3x = static_cast<uint32_t>(ax6) & rmask;
    rd2y += rd3y = static_cast<uint32_t>(ay6) & rmask;
    adjust_rem(rd2x, id2x, rmask);
    adjust_rem(rd2y, id2y, rmask);
#undef adjust_rem
    return true;
  }

  bool next() {
    fixed xx = lx1, yy = ly1;
    lx0 = lx1;
    ly0 = ly1;
    if (k <= 1) {
      --i;
      if (i == 0)
        goto last;
#define poly2(a, b, c) arith_rshift_1(arith_rshift_1(arith_rshift_1(a) + b) + c)
      xx += poly2(ax, bx, cx);
      yy += poly2(ay, by, cy);
#undef poly2
      lx1 = xx, ly1 = yy;
      return true;
    } else {
      --i;
      if (i == 0)
        goto last;
#define accum(I, r, di, dr, rm) \
  if ((r += dr) > rm)           \
    r &= rm, I += di + 1;       \
  else                          \
    I += di
      accum(xx, rx, idx, rdx, rmask);
      accum(yy, ry, idy, rdy, rmask);
      accum(idx, rdx, id2x, rd2x, rmask);
      accum(idy, rdy, id2y, rd2y, rmask);
      accum(id2x, rd2x, id3x, rd3x, rmask);
      accum(id2y, rd2y, id3y, rd3y, rmask);
#undef accum
      lx1 = x = xx;
      ly1 = y = yy;
      return true;
    }
  last:
    lx1 = x3;
    ly1 = y3;
    return false;
  }
};

constexpr int max_points = 50;

void generate_segments(Path* ppath, const Point* points, int count,
                       int notes) {
  if (notes & sn_not_first) {
    ppath->add_lines_notes(points, count, notes);
  } else {
    ppath->add_line_notes(points[0].x, points[0].y, notes);
    ppath->add_lines_notes(points + 1, count - 1, notes | sn_not_first);
  }
}

void subdivide_curve_rec(FlatIter* self, Path* ppath, int k, Seg* pc,
                         int notes, Point* points) {
top:
  if (!self->init(ppath->position.x, ppath->position.y, *pc, k)) {
    Seg cseg = *pc;
    k--;
    split_curve_midpoint(ppath->position.x, ppath->position.y, *pc, &cseg, pc);
    subdivide_curve_rec(self, ppath, k, &cseg, notes, points);
    notes |= sn_not_first;
    goto top;
  } else if (k < 0) {
    ppath->add_curve_notes(pc->p1.x, pc->p1.y, pc->p2.x, pc->p2.y, pc->pt.x,
                           pc->pt.y, notes);
  } else {
    Point* ppt = points;
    for (;;) {
      bool more = self->next();
      ppt->x = self->lx1;
      ppt->y = self->ly1;
      ppt++;
      if (ppt == &points[max_points] || !more) {
        Point* pe = (more ? ppt - 2 : ppt);
        generate_segments(ppath, points, static_cast<int>(pe - points), notes);
        if (!more)
          return;
        notes |= sn_not_first;
        std::copy(pe, ppt, points);
        ppt = points + (ppt - pe);
      }
    }
  }
}

void subdivide_curve(Path* ppath, int k, Seg* pc, int notes) {
  Point points[max_points + 1];
  FlatIter iter;
  subdivide_curve_rec(&iter, ppath, k, pc, notes, points);
}

// ---------------------------------------------------------------- gxpcopy.c
void break_line_if_long(Path* ppath, const Seg& pseg, bool gap) {
  fixed x0 = ppath->position.x, y0 = ppath->position.y;
  if (check_diff_overflow(pseg.pt.x, x0) ||
      check_diff_overflow(pseg.pt.y, y0)) {
    fixed x, y;
    if (check_sum_overflow(pseg.pt.x, x0))
      x = (pseg.pt.x >> 1) + (x0 >> 1);
    else
      x = (pseg.pt.x + x0) >> 1;
    if (check_sum_overflow(pseg.pt.y, y0))
      y = (pseg.pt.y >> 1) + (y0 >> 1);
    else
      y = (pseg.pt.y + y0) >> 1;
    if (gap)
      ppath->add_gap_notes(x, y, pseg.notes);
    else
      ppath->add_line_notes(x, y, pseg.notes);
  }
}

void adjust_point_to_tangent(Seg* pseg, const Seg& next, const Point& p1) {
  const fixed x0 = pseg->pt.x, y0 = pseg->pt.y;
  const fixed fC = p1.x - x0, fD = p1.y - y0;
  if (fC == 0) {
    const fixed DT = (next.pt.y - y0) >> 2;
    if (fD == 0)
      return;
    if ((DT ^ fD) > 0)
      pseg->pt.y = DT + y0;
  } else if (fD == 0) {
    const fixed CT = (next.pt.x - x0) >> 2;
    if ((CT ^ fC) > 0)
      pseg->pt.x = CT + x0;
  } else {
    const double C = fC, D = fD;
    double T = (C * (next.pt.x - x0) + D * (next.pt.y - y0)) / (C * C + D * D);
    if (T > 0) {
      if (T > 1)
        T = 1;
      pseg->pt.x = (static_cast<fixed>(C * T) >> 2) + x0;
      pseg->pt.y = (static_cast<fixed>(D * T) >> 2) + y0;
    }
  }
}

// gx_path_copy_reducing(old, new, float2fixed(flatness), pgs,
//                       pco_for_stroke | (accurate ? pco_accurate : 0))
void PathAddFlattenedForStroke(const Path& old, Path* ppath,
                               fixed fixed_flatness, const GState& gs) {
  fixed flat = fixed_flatness;
  double width = gs.lp.half_width;
  Point expansion;
  expansion.x = float2fixed((fabs(gs.ctm.xx) + fabs(gs.ctm.yx)) * width) * 2;
  expansion.y = float2fixed((fabs(gs.ctm.xy) + fabs(gs.ctm.yy)) * width) * 2;
  for (int i = 0; i < static_cast<int>(old.s.size()); i++) {
    const Seg& pseg = old.s[i];
    switch (pseg.type) {
      case s_start:
        ppath->add_point(pseg.pt.x, pseg.pt.y);
        break;
      case s_curve: {
        fixed x0 = ppath->position.x, y0 = ppath->position.y;
        int notes = pseg.notes;
        int k;
        fixed min01, max01, min23, max23, ex, ey, flat_x, flat_y;
#define SET_EXTENT(r, c0, c1, c2, c3)          \
  if (c0 < c1) min01 = c0, max01 = c1;         \
  else min01 = c1, max01 = c0;                 \
  if (c2 < c3) min23 = c2, max23 = c3;         \
  else min23 = c3, max23 = c2;                 \
  r = std::max(max01, max23) - std::min(min01, min23)
        SET_EXTENT(ex, x0, pseg.p1.x, pseg.p2.x, pseg.pt.x);
        SET_EXTENT(ey, y0, pseg.p1.y, pseg.p2.y, pseg.pt.y);
#undef SET_EXTENT
        if (ex == 0 || ey == 0) {
          if (ex != 0) {
            flat = fixed_mult_quo(fixed_flatness, ex, ex + expansion.x);
            k = curve_log2_samples(x0, y0, pseg, flat);
          } else if (ey != 0) {
            flat = fixed_mult_quo(fixed_flatness, ey, ey + expansion.y);
            k = curve_log2_samples(x0, y0, pseg, flat);
          } else {
            k = 0;
          }
        } else {
          flat_x = fixed_mult_quo(fixed_flatness, ex, ex + expansion.x);
          flat_y = fixed_mult_quo(fixed_flatness, ey, ey + expansion.y);
          flat = std::min(flat_x, flat_y);
          k = curve_log2_samples(x0, y0, pseg, flat);
        }
        Seg cseg = pseg;
        if (gs.accurate_curves) {
          ppath->add_line_notes(x0, y0, notes);
          int start = ppath->last_index();
          notes |= sn_not_first;
          subdivide_curve(ppath, k, &cseg, notes);
          int end = ppath->last_index();
          ppath->add_line_notes(ppath->position.x, ppath->position.y,
                                pseg.notes | sn_not_first);
          std::vector<Seg>& S = ppath->s;
          if (S[start + 1].pt.x != pseg.p1.x || S[start + 1].pt.y != pseg.p1.y)
            adjust_point_to_tangent(&S[start], S[start + 1], pseg.p1);
          else if (S[start + 1].pt.x != pseg.p2.x ||
                   S[start + 1].pt.y != pseg.p2.y)
            adjust_point_to_tangent(&S[start], S[start + 1], pseg.p2);
          else
            adjust_point_to_tangent(&S[start], S[start + 1], S[end - 1].pt);
          if (S[end - 1].pt.x != pseg.p2.x || S[end - 1].pt.y != pseg.p2.y)
            adjust_point_to_tangent(&S[end], S[end - 1], pseg.p2);
          else if (S[end - 1].pt.x != pseg.p1.x || S[end - 1].pt.y != pseg.p1.y)
            adjust_point_to_tangent(&S[end], S[end - 1], pseg.p1);
          else
            adjust_point_to_tangent(&S[end], S[end - 1], S[start].pt);
        } else {
          subdivide_curve(ppath, k, &cseg, notes);
        }
        break;
      }
      case s_line:
        break_line_if_long(ppath, pseg, false);
        ppath->add_line_notes(pseg.pt.x, pseg.pt.y, pseg.notes);
        break;
      case s_gap:
        break_line_if_long(ppath, pseg, true);
        ppath->add_gap_notes(pseg.pt.x, pseg.pt.y, pseg.notes);
        break;
      case s_dash:
        ppath->add_dash_notes(pseg.pt.x, pseg.pt.y, pseg.tangent.x,
                              pseg.tangent.y, pseg.notes);
        break;
      case s_line_close:
        break_line_if_long(ppath, pseg, false);
        ppath->close_subpath();
        break;
    }
  }
  if (old.last_is_moveto)
    ppath->add_point(old.position.x, old.position.y);
}

bool PathHasLongSegments(const Path& p) {
  Point pt0{0, 0};
  for (const Seg& sg : p.s) {
    if (sg.type != s_start) {
      if (check_diff_overflow(sg.pt.x, pt0.x) ||
          check_diff_overflow(sg.pt.y, pt0.y))
        return true;
    }
    pt0 = sg.pt;
  }
  return false;
}

// ---------------------------------------------------------------- gxpdash.c
void subpath_expand_dashes(const Path& old, int psub, Path* ppath,
                           const GState& gs) {
  const DashParams& dash = gs.lp.dash;
  const std::vector<Seg>& S = old.s;
  const float* pattern = dash.pattern.data();
  int count, index;
  bool ink_on;
  double elt_length;
  fixed x0 = S[psub].pt.x, y0 = S[psub].pt.y;
  fixed x, y;
  int pseg;
  int wrap = (dash.init_ink_on && S[psub].is_closed ? -1 : 0);
  int drawing = wrap;
  int notes = ~sn_not_first;
  const LineParams& lp = gs.lp;
  bool zero_length = true;
  int cap;
  int start_notes, end_notes;

  if (wrap) {
    cap = lp.dash_cap;
    start_notes = sn_dash_head;
  } else {
    cap = lp.start_cap;
    start_notes = 0;
  }
  ppath->add_point(x0, y0);
top:
  count = static_cast<int>(dash.pattern.size());
  ink_on = dash.init_ink_on;
  index = dash.init_index;
  elt_length = dash.init_dist_left;
  x = x0, y = y0;
  pseg = psub;
  while ((pseg = old.next(pseg)) != NIL && S[pseg].type != s_start) {
    fixed sx = S[pseg].pt.x, sy = S[pseg].pt.y;
    fixed udx = sx - x, udy = sy - y;
    double length, dx, dy;
    double scale = 1;
    double left;
    int gap = S[pseg].type == s_gap;

    if (!(udx | udy)) {
      if (lp.dot_length == 0 && cap != gs_cap_round) {
        if (zero_length || S[pseg].type != s_line_close)
          continue;
      }
      dx = 0, dy = 0, length = 0;
    } else {
      double ddx, ddy;
      zero_length = false;
      dx = udx, dy = udy;
      if (!DistanceTransformInverse(dx, dy, gs.ctm, &ddx, &ddy))
        ddx = 0, ddy = 0;
      length = hypot(ddx, ddy) * (1.0 / fixed_1);
      // dash adapt is false for pdfi.
    }
    left = length;
    while (left > elt_length) {
      double fraction = elt_length / length;
      fixed fx = static_cast<fixed>(dx * fraction);
      fixed fy = static_cast<fixed>(dy * fraction);
      fixed nx = x + fx;
      fixed ny = y + fy;

      if (ink_on && !gap) {
        if (drawing >= 0) {
          if (left >= elt_length && any_abs(fx) + any_abs(fy) < fixed_half)
            ppath->add_dash_notes(
                nx, ny, udx, udy,
                ((notes & S[pseg].notes) | start_notes | sn_dash_tail));
          else
            ppath->add_line_notes(
                nx, ny,
                ((notes & S[pseg].notes) | start_notes | sn_dash_tail));
        }
        notes |= sn_not_first;
      } else {
        if (drawing > 0)
          return;
        ppath->add_point(nx, ny);
        notes &= ~sn_not_first;
        drawing = 0;
      }
      left -= elt_length;
      ink_on = !ink_on;
      start_notes = sn_dash_head;
      if (++index == count)
        index = 0;
      elt_length = pattern[index] * scale;
      x = nx, y = ny;
    }
    elt_length -= left;
    if (wrap) {
      end_notes = sn_dash_tail;
    } else {
      // Note: gs compares pt.x against sy here (sic); ported as is.
      int pseg2 = old.next(pseg);
      end_notes = 0;
      while (pseg2 != NIL && S[pseg2].type != s_start) {
        if ((S[pseg2].pt.x != sx) || (S[pseg2].pt.x != sy)) {
          end_notes = sn_dash_tail;
          break;
        }
        pseg2 = old.next(pseg2);
      }
    }
  on:
    if (ink_on && !gap) {
      if (drawing >= 0) {
        if (S[pseg].type == s_line_close && drawing > 0)
          ppath->close_subpath_notes(
              ((notes & S[pseg].notes) | start_notes | end_notes));
        else if ((any_abs(sx - x) + any_abs(sy - y) < fixed_half) &&
                 (udx | udy))
          ppath->add_dash_notes(
              sx, sy, udx, udy,
              ((notes & S[pseg].notes) | start_notes | end_notes));
        else
          ppath->add_line_notes(
              sx, sy, ((notes & S[pseg].notes) | start_notes | end_notes));
        notes |= sn_not_first;
      }
    } else {
      ppath->add_point(sx, sy);
      notes &= ~sn_not_first;
      int nxt = old.next(pseg);
      if (elt_length < 1.0 / fixed_1 &&
          (nxt == NIL || S[nxt].type == s_start || S[nxt].type == s_gap ||
           elt_length == 0)) {
        double elt_length1;
        if (++index == count)
          index = 0;
        elt_length1 = pattern[index] * scale;
        if (nxt == NIL || S[nxt].type == s_start || S[nxt].type == s_gap) {
          elt_length = elt_length1;
          left = 0;
          ink_on = true;
          goto on;
        }
        if (elt_length1 == 0) {
          left = 0;
          ppath->add_dash_notes(
              sx, sy, udx, udy,
              ((notes & S[pseg].notes) | start_notes | end_notes));
          if (++index == count)
            index = 0;
          elt_length = pattern[index] * scale;
          ink_on = false;
        } else if (--index == 0) {
          index = count - 1;
        }
      }
      if (drawing > 0)
        return;
      drawing = 0;
    }
    x = sx, y = sy;
    cap = lp.dash_cap;
  }
  if (wrap && drawing <= 0) {
    drawing = 1;
    goto top;
  }
}

void PathAddDashExpansion(const Path& old, Path* ppath, const GState& gs) {
  for (int psub = old.first_subpath(); psub != NIL;
       psub = old.next(old.s[psub].last))
    subpath_expand_dashes(old, psub, ppath, gs);
}

// ---------------------------------------------------------------- devices
struct Device {
  int width, height;          // memory device size (from clip box)
  int cx0, cy0, cx1, cy1;     // clip
  Output* out;
  StrokeAdjustMemory* sgr;

  // gx_fill_rectangle_device_rop on a clipped memory device
  void fill_rect(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    x = std::max(x, cx0);
    y = std::max(y, cy0);
    x1 = std::min(x1, cx1);
    y1 = std::min(y1, cy1);
    if (x1 <= x || y1 <= y)
      return;
    out->rects.push_back({x, y, x1 - x, y1 - y});
  }
};

struct FixedEdge {
  Point start, end;
};

struct TrapLine {
  fixed h;
  int di;
  fixed df;
  fixed x, xf;
  fixed ldi, ldf;
};

constexpr fixed YMULT_LIMIT = max_fixed / fixed_1;

inline void compute_dx(TrapLine* tl, fixed xd, fixed ys) {
  fixed h = tl->h;
  int di;
  if (xd >= 0) {
    if (xd < h)
      tl->di = 0, tl->df = xd;
    else {
      tl->di = di = static_cast<int>(xd / h);
      tl->df = xd - di * h;
      tl->x += ys * di;
    }
  } else {
    if ((tl->df = xd + h) >= 0)
      tl->di = -1, tl->x -= ys;
    else {
      tl->di = di = static_cast<int>((xd + 1) / h - 1);
      tl->df = xd - di * h;
      tl->x += ys * di;
    }
  }
}

inline void compute_ldx(TrapLine* tl, fixed ys) {
  int di = tl->di;
  fixed df = tl->df;
  fixed h = tl->h;
  if (df < YMULT_LIMIT) {
    if (df == 0)
      tl->ldi = int2fixed(di), tl->ldf = 0, tl->xf = -h;
    else {
      tl->ldi = int2fixed(di) + int2fixed(df) / h;
      tl->ldf = int2fixed(df) % h;
      tl->xf = (ys < fixed_1 ? ys * df % h : fixed_mult_rem(ys, df, h)) - h;
    }
  } else {
    tl->ldi = int2fixed(di) + fixed_mult_quo(fixed_1, df, h);
    tl->ldf = fixed_mult_rem(fixed_1, df, h);
    tl->xf = fixed_mult_rem(ys, df, h) - h;
  }
}

// gxdtfill.h with CONTIGUOUS_FILL 0, LINEAR_COLOR 0, FILL_DIRECT 1.
void fill_trapezoid_body(Device* dev, const FixedEdge& left,
                         const FixedEdge& right, fixed ybot, fixed ytop,
                         bool swap_axes) {
  const fixed ymin = fixed_pixround(ybot) + fixed_half;
  const fixed ymax = fixed_pixround(ytop);
  if (ymin >= ymax)
    return;
  int iy = fixed2int_var(ymin);
  const int iy1 = fixed2int_var(ymax);
  TrapLine l, r;
  int rxl, rxr;
  int ry;
  const fixed x0l = left.start.x, x1l = left.end.x, x0r = right.start.x,
              x1r = right.end.x, dxl = x1l - x0l, dxr = x1r - x0r;
  const fixed ysl = ymin - left.start.y, ysr = ymin - right.start.y;
  fixed fxl;
  auto FILL = [&](int x, int y, int w, int h) {
    if (swap_axes)
      dev->fill_rect(y, x, h, w);
    else
      dev->fill_rect(x, y, w, h);
  };
  auto YMULT_QUO = [](fixed ys, const TrapLine& tl) {
    return (ys < fixed_1 && tl.df < YMULT_LIMIT) ? ys * tl.df / tl.h
                                                 : fixed_mult_quo(ys, tl.df, tl.h);
  };

  l.h = left.end.y - left.start.y;
  if (l.h == 0)
    return;
  r.h = right.end.y - right.start.y;
  if (r.h == 0)
    return;
  l.x = x0l + (fixed_half - fixed_epsilon);
  r.x = x0r + (fixed_half - fixed_epsilon);
  ry = iy;
  if (fixed_floor(l.x) == fixed_pixround(x1l)) {
    l.di = 0, l.df = 0;
    fxl = 0;
  } else {
    compute_dx(&l, dxl, ysl);
    fxl = YMULT_QUO(ysl, l);
    l.x += fxl;
  }
  if (fixed_floor(r.x) == fixed_pixround(x1r)) {
    if (l.di == 0 && l.df == 0) {
      rxl = fixed2int_var(l.x);
      rxr = fixed2int_var(r.x);
      FILL(rxl, ry, rxr - rxl, iy1 - ry);
      return;
    }
    r.di = 0, r.df = 0;
  } else if (dxr == dxl && fxl != 0) {
    if (l.di == 0)
      r.di = 0, r.df = l.df;
    else
      compute_dx(&r, dxr, ysr);
    if (ysr == ysl && r.h == l.h)
      r.x += fxl;
    else
      r.x += YMULT_QUO(ysr, r);
  } else {
    compute_dx(&r, dxr, ysr);
    r.x += YMULT_QUO(ysr, r);
  }
  compute_ldx(&l, ysl);
  compute_ldx(&r, ysr);
  l.x += fixed_epsilon;
  r.x += fixed_epsilon;
#define rational_floor(tl) \
  fixed2int_var(fixed_is_int(tl.x) && tl.xf == -tl.h ? tl.x - fixed_1 : tl.x)
#define STEP_LINE(ix, tl)          \
  tl.x += tl.ldi;                  \
  if ((tl.xf += tl.ldf) >= 0)      \
    tl.xf -= tl.h, tl.x++;         \
  ix = rational_floor(tl)
  rxl = rational_floor(l);
  rxr = rational_floor(r);
  while (++iy != iy1) {
    int ixl, ixr;
    STEP_LINE(ixl, l);
    STEP_LINE(ixr, r);
    if (ixl != rxl || ixr != rxr) {
      FILL(rxl, ry, rxr - rxl, iy - ry);
      rxl = ixl, rxr = ixr, ry = iy;
    }
  }
  FILL(rxl, ry, rxr - rxl, iy - ry);
#undef STEP_LINE
#undef rational_floor
}

// gdevddrw.c gx_default_fill_trapezoid
void fill_trapezoid(Device* dev, const FixedEdge& left, const FixedEdge& right,
                    fixed ybot, fixed ytop, bool swap_axes) {
  int dsize = swap_axes ? dev->width : dev->height;
  if (dsize != 0) {
    int d = dsize > max_int_in_fixed ? max_int_in_fixed : dsize;
    if (ytop < 0)
      return;
    if (ybot < 0)
      ybot = 0;
    fixed df = int2fixed(d);
    if (ybot > df)
      return;
    if (ytop > df)
      ytop = df;
  }
  fill_trapezoid_body(dev, left, right, ybot, ytop, swap_axes);
}

// gdevddrw.c gx_default_draw_thin_line
void draw_thin_line(Device* dev, fixed fx0, fixed fy0, fixed fx1, fixed fy1,
                    fixed adjustx, fixed adjusty) {
  int ix, iy, itox, itoy;
  int epsilon = ((adjustx | adjusty) == 0 ? fixed_epsilon : 0);
  fixed h = fy1 - fy0;
  fixed w = fx1 - fx0;
  bool swap_axes;
  FixedEdge left, right;

  if ((w < 0 ? -w : w) <= (h < 0 ? -h : h)) {
    if (h < 0)
      std::swap(fx0, fx1), std::swap(fy0, fy1), h = -h;
    if (w == 0 && adjusty) {
      int deltay = int2fixed(fixed2int_var(fy1)) + fixed_half - fy1;
      if ((deltay > 0) && (deltay <= fixed_half))
        dev->fill_rect(fixed2int_var(fx1), fixed2int_var(fy1), 1, 1);
      deltay = int2fixed(fixed2int_var(fy0)) + fixed_half - fy0;
      if ((deltay < 0) && (deltay >= -fixed_half))
        dev->fill_rect(fixed2int_var(fx0), fixed2int_var(fy0), 1, 1);
    } else if (w != 0) {
      int deltax, deltay;
      deltay = int2fixed(fixed2int_var(fy1)) + fixed_half - fy1;
      deltax = int2fixed(fixed2int_var(fx1)) + fixed_half - fx1;
      if (deltax < 0)
        deltax = -deltax;
      if ((deltay > 0) && (deltay <= fixed_half) &&
          (deltay + deltax <= fixed_half))
        dev->fill_rect(fixed2int_var(fx1), fixed2int_var(fy1), 1, 1);
      deltay = int2fixed(fixed2int_var(fy0)) + fixed_half - fy0;
      deltax = int2fixed(fixed2int_var(fx0)) + fixed_half - fx0;
      if (deltax < 0)
        deltax = -deltax;
      if ((deltay < 0) && (deltay >= -fixed_half) &&
          (-deltay + deltax <= fixed_half))
        dev->fill_rect(fixed2int_var(fx0), fixed2int_var(fy0), 1, 1);
    }
    ix = fixed2int_var(fx0 - epsilon);
    itox = fixed2int_var(fx1 - epsilon);
    if (itox == ix) {
      iy = fixed2int_var(fy0 + fixed_half - fixed_epsilon);
      itoy = fixed2int_var(fy1 + fixed_half - fixed_epsilon);
      itoy = itoy - iy;
      if (itoy <= 0)
        return;
      dev->fill_rect(ix, iy, 1, itoy);
      return;
    }
    left.start.x = fx0 - fixed_half + fixed_epsilon - epsilon;
    right.start.x = left.start.x + fixed_1;
    left.end.x = fx1 - fixed_half + fixed_epsilon - epsilon;
    right.end.x = left.end.x + fixed_1;
    left.start.y = right.start.y = fy0;
    left.end.y = right.end.y = fy1;
    swap_axes = false;
  } else {
    if (w < 0)
      std::swap(fx0, fx1), std::swap(fy0, fy1), w = -w;
    if (h == 0 && adjustx) {
      int deltax = int2fixed(fixed2int_var(fx1)) + fixed_half - fx1;
      if ((deltax > 0) && (deltax <= fixed_half))
        dev->fill_rect(fixed2int_var(fx1), fixed2int_var(fy1), 1, 1);
      deltax = int2fixed(fixed2int_var(fx0)) + fixed_half - fx0;
      if ((deltax < 0) && (deltax >= -fixed_half))
        dev->fill_rect(fixed2int_var(fx0), fixed2int_var(fy0), 1, 1);
    } else if (h != 0) {
      int deltax, deltay;
      deltax = int2fixed(fixed2int_var(fx1)) + fixed_half - fx1;
      deltay = int2fixed(fixed2int_var(fy1)) + fixed_half - fy1;
      if (deltay < 0)
        deltay = -deltay;
      if ((deltax > 0) && (deltax <= fixed_half) &&
          (deltax + deltay <= fixed_half))
        dev->fill_rect(fixed2int_var(fx1), fixed2int_var(fy1), 1, 1);
      deltax = int2fixed(fixed2int_var(fx0)) + fixed_half - fx0;
      deltay = int2fixed(fixed2int_var(fy0)) + fixed_half - fy0;
      if (deltay < 0)
        deltay = -deltay;
      if ((deltax < 0) && (deltax >= -fixed_half) &&
          (-deltax + deltay <= fixed_half))
        dev->fill_rect(fixed2int_var(fx0), fixed2int_var(fy0), 1, 1);
    }
    iy = fixed2int_var(fy0 - epsilon);
    itoy = fixed2int_var(fy1 - epsilon);
    if (itoy == iy) {
      ix = fixed2int_var(fx0 + fixed_half - fixed_epsilon);
      itox = fixed2int_var(fx1 + fixed_half - fixed_epsilon);
      itox = itox - ix;
      if (itox <= 0)
        return;
      dev->fill_rect(ix, iy, itox, 1);
      return;
    }
    left.start.x = fy0 - fixed_half + fixed_epsilon - epsilon;
    right.start.x = left.start.x + fixed_1;
    left.end.x = fy1 - fixed_half + fixed_epsilon - epsilon;
    right.end.x = left.end.x + fixed_1;
    left.start.y = right.start.y = fx0;
    left.end.y = right.end.y = fx1;
    swap_axes = true;
  }
  fill_trapezoid(dev, left, right, left.start.y, left.end.y, swap_axes);
}

// ---------------------------------------------------------------- gxstroke.c
struct Endpoint {
  Point p;
  Point co, ce;
  Point cdelta;
};
struct PartialLine {
  Endpoint o;
  Endpoint e;
  Point width;
  Point vector;
  bool thin;
};
using pl_ptr = PartialLine*;

enum NoteFlags {
  nf_all_from_arc = 1,
  nf_some_from_arc = 2,
  nf_dash_head = 4,
  nf_dash_tail = 8,
  nf_prev_all_from_arc = 16,
  nf_prev_some_from_arc = 32,
  nf_prev_dash_head = 64,
  nf_prev_dash_tail = 128
};

inline int COMBINE_FLAGS(int F) {
  return ((F >> 4) | (F & nf_some_from_arc)) &
         ((F & nf_all_from_arc) ? ~0 : ~nf_all_from_arc);
}

int join_expansion_factor_x(const GState& gs, int join, float* out) {
  switch (join) {
    case gs_join_miter:
      *out = gs.lp.miter_limit;
      break;
    case gs_join_triangle:
      *out = 2.0f;
      break;
    default:
      *out = 1.0f;
  }
  return 0;
}

// gx_stroke_path_expansion; returns <0 on limitcheck.
int stroke_path_expansion(const GState& gs, const Path& path, Point* ppt) {
  double cx = fabs(gs.ctm.xx) + fabs(gs.ctm.yx);
  double cy = fabs(gs.ctm.xy) + fabs(gs.ctm.yy);
  double expand = gs.lp.half_width;
  int result = 1;
  const LineParams& lp = gs.lp;
  if (lp.start_cap == gs_cap_square || lp.end_cap == gs_cap_square)
    expand *= 1.414213562;
  if ((gs.ctm.xy == 0.0f && gs.ctm.yx == 0.0f) ||
      (gs.ctm.xx == 0.0f && gs.ctm.yy == 0.0f)) {
    bool must_be_closed =
        !(lp.start_cap == gs_cap_square || lp.start_cap == gs_cap_round ||
          lp.end_cap == gs_cap_square || lp.end_cap == gs_cap_round ||
          lp.dash_cap == gs_cap_square || lp.dash_cap == gs_cap_round);
    Point prev{0, 0};
    bool exact = true;
    // gs: curve_count is per subpath; we check for curves in the subpath.
    for (int i = 0; i < static_cast<int>(path.s.size()) && exact; i++) {
      const Seg& sg = path.s[i];
      switch (sg.type) {
        case s_start: {
          bool has_curve = false;
          for (int j = i + 1; j <= sg.last; j++)
            has_curve |= path.s[j].type == s_curve;
          if (has_curve || (must_be_closed && !sg.is_closed))
            exact = false;
          break;
        }
        case s_line:
        case s_dash:
        case s_line_close:
          if (!(sg.pt.x == prev.x || sg.pt.y == prev.y))
            exact = false;
          break;
        default:
          exact = false;
      }
      prev = sg.pt;
    }
    if (exact)
      result = 0;
  }
  if (result) {
    int nsub = 0;
    for (const Seg& sg : path.s)
      nsub += sg.type == s_start;
    int psub = path.first_subpath();
    bool simple = false;
    if (path.curve_count == 0 && nsub <= 1) {
      if (psub == NIL || path.next(psub) == NIL)
        simple = true;
      else {
        int ps = path.next(path.next(psub));
        simple = ps == NIL || path.s[ps].type == s_line_close;
      }
    }
    if (!simple) {
      float factor;
      join_expansion_factor_x(gs, lp.join, &factor);
      if (lp.curve_join >= 0) {
        float f2;
        join_expansion_factor_x(gs, lp.curve_join, &f2);
        factor = std::max(factor, f2);
      }
      expand *= factor;
    }
  }
  float exx = static_cast<float>(expand * cx);
  float exy = static_cast<float>(expand * cy);
  if (exx * 256.0f >= 2147483647.0f || exy * 256.0f >= 2147483647.0f)
    return -1;
  ppt->x = float2fixed(exx);
  ppt->y = float2fixed(exy);
  return result;
}

bool width_is_thin(pl_ptr plp) {
  fixed wx = plp->width.x, wy = plp->width.y;
  if (plp->vector.y == 0)
    return any_abs(wy) < fixed_half;
  if (plp->vector.x == 0)
    return any_abs(wx) < fixed_half;
  return false;  // DISABLED TEST in gs, see bug 702196
}

inline fixed STROKE_ADJUSTMENT(bool thin, const GState& gs, bool x) {
  return thin ? fixed_0 : (x ? gs.fill_adjust_x : gs.fill_adjust_y);
}

void adjust_stroke_transversal(pl_ptr plp, const GState& gs, bool thin,
                               bool horiz) {
  fixed *pw, *pov, *pev;
  fixed w, w2, adj2;
  if (horiz) {
    pw = &plp->width.y, pov = &plp->o.p.y, pev = &plp->e.p.y;
    adj2 = STROKE_ADJUSTMENT(thin, gs, false) << 1;
  } else {
    pw = &plp->width.x, pov = &plp->o.p.x, pev = &plp->e.p.x;
    adj2 = STROKE_ADJUSTMENT(thin, gs, true) << 1;
  }
  w = *pw;
  if (w > 0)
    w2 = fixed_rounded(w << 1);
  else
    w2 = -fixed_rounded(-w << 1);
  if (w2 == 0 && *pw != 0) {
    w2 = (*pw < 0 ? -fixed_1 + adj2 : fixed_1 - adj2);
    *pw = arith_rshift_1(w2);
  }
  if (*pov == *pev) {
    if (w >= 0)
      w2 += adj2;
    else
      w2 = adj2 - w2;
    if (w2 & fixed_1)
      *pov = *pev = fixed_floor(*pov) + fixed_half;
    else
      *pov = *pev = fixed_rounded(*pov);
  }
}

void adjust_stroke_longitude(pl_ptr plp, const GState& gs, bool thin,
                             bool horiz, int start_cap, int end_cap) {
  fixed* pow = (horiz ? &plp->o.p.y : &plp->o.p.x);
  fixed* pew = (horiz ? &plp->e.p.y : &plp->e.p.x);
  if (*pow == *pew) {
    fixed* pov = (horiz ? &plp->o.p.x : &plp->o.p.y);
    fixed* pev = (horiz ? &plp->e.p.x : &plp->e.p.y);
    fixed length = any_abs(*pov - *pev);
    fixed length_r, length_r_2;
    fixed mv = (*pov + *pev) / 2, mv_r;
    fixed adj2 = (horiz ? STROKE_ADJUSTMENT(thin, gs, true)
                        : STROKE_ADJUSTMENT(thin, gs, false))
                 << 1;
    if (length > fixed_1)
      return;
    if (start_cap == gs_cap_butt || end_cap == gs_cap_butt) {
      length_r = fixed_rounded(length);
      if (length_r < fixed_1)
        length_r = fixed_1;
      length_r_2 = length_r / 2;
    } else {
      fixed width = any_abs(horiz ? plp->width.y : plp->width.x);
      length_r = fixed_rounded(length + width * 2 + adj2);
      length_r_2 = fixed_rounded(length) / 2;
    }
    if (length_r & fixed_1)
      mv_r = fixed_floor(mv) + fixed_half;
    else
      mv_r = fixed_floor(mv);
    if (*pov < *pev) {
      *pov = mv_r - length_r_2;
      *pev = mv_r + length_r_2;
    } else {
      *pov = mv_r + length_r_2;
      *pev = mv_r - length_r_2;
    }
  }
}

void set_pt(fixed d[2], Point p) { d[0] = p.x, d[1] = p.y; }

void adjust_stroke(Device* dev, pl_ptr plp, const GState& gs, bool thin,
                   bool adjust_longitude, int flags) {
  StrokeAdjustMemory& sgr = *dev->sgr;
  bool horiz, adjust = true;
  int start_cap = (flags & nf_dash_head ? gs.lp.dash_cap : gs.lp.start_cap);
  int end_cap = (flags & nf_dash_tail ? gs.lp.dash_cap : gs.lp.end_cap);

  if (!gs.stroke_adjust || (plp->width.x != 0 && plp->width.y != 0)) {
    sgr.stroke_stored = false;
    return;
  }
#define O(i, c) sgr.orig[i][c]
#define A(i, c) sgr.adjusted[i][c]
  if (sgr.stroke_stored &&
      (start_cap == gs_cap_butt || end_cap == gs_cap_butt) &&
      O(3, 0) == plp->vector.x && O(3, 1) == plp->vector.y) {
    if (static_cast<int64_t>(plp->o.p.x - O(0, 0)) * plp->vector.x ==
            static_cast<int64_t>(plp->o.p.y - O(0, 1)) * plp->vector.y &&
        static_cast<int64_t>(plp->e.p.x - O(1, 0)) * plp->vector.x ==
            static_cast<int64_t>(plp->e.p.y - O(1, 1)) * plp->vector.y) {
      if (any_abs(plp->o.p.x - O(0, 0)) <= any_abs(plp->width.x + O(2, 0)) &&
          any_abs(plp->o.p.y - O(0, 1)) <= any_abs(plp->width.y + O(2, 1)) &&
          any_abs(plp->e.p.x - O(1, 0)) <= any_abs(plp->width.x + O(2, 0)) &&
          any_abs(plp->e.p.y - O(1, 1)) <= any_abs(plp->width.y + O(2, 1))) {
        if (any_abs(plp->o.p.x - O(0, 0)) >= any_abs(plp->width.x + O(2, 0)) / 2 &&
            any_abs(plp->o.p.y - O(0, 1)) >= any_abs(plp->width.y + O(2, 1)) / 2 &&
            any_abs(plp->e.p.x - O(1, 0)) >= any_abs(plp->width.x + O(2, 0)) / 2 &&
            any_abs(plp->e.p.y - O(1, 1)) >= any_abs(plp->width.y + O(2, 1)) / 2) {
          if (!(any_abs(plp->o.p.x - A(0, 0)) <= any_abs(plp->width.x + A(2, 0)) &&
                any_abs(plp->o.p.y - A(0, 1)) <= any_abs(plp->width.y + A(2, 1)) &&
                any_abs(plp->e.p.x - A(1, 0)) <= any_abs(plp->width.x + A(2, 0)) &&
                any_abs(plp->e.p.y - A(1, 1)) <= any_abs(plp->width.y + A(2, 1)))) {
            fixed delta_w_x = (A(2, 0) - O(2, 0));
            fixed delta_w_y = (A(2, 1) - O(2, 1));
            fixed shift_o_x = (A(0, 0) - O(0, 0));
            fixed shift_o_y = (A(0, 1) - O(0, 1));
            fixed shift_e_x = (A(1, 0) - O(1, 0));
            fixed shift_e_y = (A(1, 1) - O(1, 1));
            if (plp->o.p.x < O(0, 0) ||
                (plp->o.p.x == O(0, 0) && plp->o.p.y < O(0, 1))) {
              plp->width.x += (shift_o_x - delta_w_x) / 2;
              plp->width.y += (shift_o_y - delta_w_y) / 2;
              plp->o.p.x += (shift_o_x - delta_w_x) / 2;
              plp->o.p.y += (shift_o_y - delta_w_y) / 2;
              plp->e.p.x += (shift_e_x - delta_w_x) / 2;
              plp->e.p.y += (shift_e_y - delta_w_y) / 2;
              adjust = false;
            } else {
              plp->width.x -= (shift_o_x + delta_w_x) / 2;
              plp->width.y -= (shift_o_y + delta_w_y) / 2;
              plp->o.p.x += (shift_o_x + delta_w_x) / 2;
              plp->o.p.y += (shift_o_y + delta_w_y) / 2;
              plp->e.p.x += (shift_e_x + delta_w_x) / 2;
              plp->e.p.y += (shift_e_y + delta_w_y) / 2;
              adjust = false;
            }
          }
        }
      }
    }
  }
#undef O
#undef A
  if ((start_cap == gs_cap_butt) || (end_cap == gs_cap_butt)) {
    sgr.stroke_stored = true;
    set_pt(sgr.orig[0], plp->o.p);
    set_pt(sgr.orig[1], plp->e.p);
    set_pt(sgr.orig[2], plp->width);
    set_pt(sgr.orig[3], plp->vector);
  } else {
    sgr.stroke_stored = false;
  }
  if (adjust) {
    horiz = (any_abs(plp->width.x) <= any_abs(plp->width.y));
    adjust_stroke_transversal(plp, gs, thin, horiz);
    if (adjust_longitude)
      adjust_stroke_longitude(plp, gs, thin, horiz, start_cap, end_cap);
  }
  if ((start_cap == gs_cap_butt) || (end_cap == gs_cap_butt)) {
    set_pt(sgr.adjusted[0], plp->o.p);
    set_pt(sgr.adjusted[1], plp->e.p);
    set_pt(sgr.adjusted[2], plp->width);
    set_pt(sgr.adjusted[3], plp->vector);
  }
}

int line_intersect(const Point* pp1, const Point* pd1, const Point* pp2,
                   const Point* pd2, Point* pi) {
  double u1 = pd1->x, v1 = pd1->y;
  double u2 = pd2->x, v2 = pd2->y;
  double denom = u1 * v2 - u2 * v1;
  double xdiff = pp2->x - pp1->x;
  double ydiff = pp2->y - pp1->y;
  double f1;
  double max_result = any_abs(denom) * static_cast<double>(max_fixed);
  if (any_abs(xdiff) >= max_result || any_abs(ydiff) >= max_result)
    return -1;
  f1 = (v2 * xdiff - u2 * ydiff) / denom;
  pi->x = pp1->x + static_cast<fixed>(f1 * u1);
  pi->y = pp1->y + static_cast<fixed>(f1 * v1);
  return (f1 >= 0 && (v1 * xdiff >= u1 * ydiff ? denom >= 0 : denom < 0) ? 0
                                                                          : 1);
}

void set_thin_widths(pl_ptr plp) {
  fixed dx = plp->e.p.x - plp->o.p.x, dy = plp->e.p.y - plp->o.p.y;
#define TRSIGN(v, c) ((v) >= 0 ? (c) : -(c))
  if (any_abs(dx) > any_abs(dy)) {
    plp->width.x = plp->e.cdelta.y = 0;
    plp->width.y = plp->e.cdelta.x = TRSIGN(dx, fixed_half);
  } else {
    plp->width.y = plp->e.cdelta.x = 0;
    plp->width.x = -(plp->e.cdelta.y = TRSIGN(dy, fixed_half));
  }
#undef TRSIGN
}

void compute_caps(pl_ptr plp) {
  fixed wx2 = plp->width.x;
  fixed wy2 = plp->width.y;
  plp->o.co.x = plp->o.p.x + wx2, plp->o.co.y = plp->o.p.y + wy2;
  plp->o.cdelta.x = -plp->e.cdelta.x, plp->o.cdelta.y = -plp->e.cdelta.y;
  plp->o.ce.x = plp->o.p.x - wx2, plp->o.ce.y = plp->o.p.y - wy2;
  plp->e.co.x = plp->e.p.x - wx2, plp->e.co.y = plp->e.p.y - wy2;
  plp->e.ce.x = plp->e.p.x + wx2, plp->e.ce.y = plp->e.p.y + wy2;
}

void add_points(Path* ppath, const Point* points, int npoints,
                bool moveto_first) {
  if (moveto_first) {
    ppath->add_point(points[0].x, points[0].y);
    ppath->add_lines_notes(points + 1, npoints - 1, sn_none);
  } else {
    ppath->add_lines_notes(points, npoints, sn_none);
  }
}

void add_pie_cap(Path* ppath, const Endpoint* endp) {
  const fixed px = endp->p.x, py = endp->p.y, xo = endp->co.x,
              yo = endp->co.y, xe = endp->ce.x, ye = endp->ce.y,
              cdx = endp->cdelta.x, cdy = endp->cdelta.y;
  ppath->add_partial_arc(px + cdx, py + cdy, xo + cdx, yo + cdy,
                         quarter_arc_fraction);
  ppath->add_partial_arc(xe, ye, xe + cdx, ye + cdy, quarter_arc_fraction);
  ppath->add_line_notes(xe, ye, sn_none);
}

int cap_points(int type, const Endpoint* endp, Point* pts) {
  const fixed px = endp->p.x, py = endp->p.y, xo = endp->co.x,
              yo = endp->co.y, xe = endp->ce.x, ye = endp->ce.y,
              cdx = endp->cdelta.x, cdy = endp->cdelta.y;
  switch (type) {
    case gs_cap_butt:
      pts[0] = {xo, yo};
      pts[1] = {xe, ye};
      return 2;
    case gs_cap_square:
      pts[0] = {xo + cdx, yo + cdy};
      pts[1] = {xe + cdx, ye + cdy};
      return 2;
    case gs_cap_triangle:
      pts[0] = {xo, yo};
      pts[1] = {px + cdx, py + cdy};
      pts[2] = {xe, ye};
      return 3;
    default:
      return -1;
  }
}

void do_pie_join(Path* ppath, const Point* centre, const Point* current_orig,
                 const Point* current_tangent, const Point* final,
                 const Point* final_tangent, bool ccw, const Point* width) {
  double rad_squared, dist_squared, F;
  Point current, tangent, tangmeet;
  tangent = *current_tangent;
  current = *current_orig;
  if (static_cast<double>(tangent.x) * final_tangent->x +
          static_cast<double>(tangent.y) * final_tangent->y >
      0) {
    ppath->add_partial_arc(centre->x + tangent.x, centre->y + tangent.y,
                           current.x + tangent.x, current.y + tangent.y,
                           quarter_arc_fraction);
    current.x = centre->x + tangent.x;
    current.y = centre->y + tangent.y;
    if (ccw) {
      int tmp = tangent.x;
      tangent.x = -tangent.y;
      tangent.y = tmp;
    } else {
      int tmp = tangent.x;
      tangent.x = tangent.y;
      tangent.y = -tmp;
    }
  }
  if (line_intersect(&current, &tangent, final, final_tangent, &tangmeet) !=
      0) {
    ppath->add_line_notes(final->x, final->y, sn_none);
    return;
  }
  current.x -= tangmeet.x;
  current.y -= tangmeet.y;
  dist_squared = static_cast<double>(current.x) * current.x +
                 static_cast<double>(current.y) * current.y;
  rad_squared = static_cast<double>(width->x) * width->x +
                static_cast<double>(width->y) * width->y;
  dist_squared /= rad_squared;
  F = (4.0 / 3.0) * (1 / (1 + sqrt(1 + dist_squared)));
  ppath->add_partial_arc(final->x, final->y, tangmeet.x, tangmeet.y, F);
}

void add_pie_join(Path* ppath, pl_ptr plp, pl_ptr nplp, bool reflected,
                  bool cap) {
  const Point *current, *final, *tangent, *final_tangent;
  double l, r;
  bool ccw;
  l = static_cast<double>(plp->width.x) * (nplp->width.y);
  r = static_cast<double>(nplp->width.x) * (plp->width.y);
  if (l == r) {
    if (cap && (static_cast<double>(plp->width.x) * (nplp->width.x) +
                static_cast<double>(nplp->width.y) * (plp->width.y)) < 0)
      add_pie_cap(ppath, &plp->e);
    else
      ppath->add_line_notes(plp->e.ce.x, plp->e.ce.y, sn_none);
    return;
  }
  ccw = (l > r);
  ccw ^= reflected;
  if (ccw) {
    current = &plp->e.co;
    final = &nplp->o.ce;
    tangent = &plp->e.cdelta;
    final_tangent = &nplp->o.cdelta;
    if (current->x == final->x && current->y == final->y) {
      ppath->add_line_notes(plp->e.ce.x, plp->e.ce.y, sn_none);
      return;
    }
  } else {
    current = &nplp->o.co;
    final = &plp->e.ce;
    tangent = &nplp->o.cdelta;
    final_tangent = &plp->e.cdelta;
    ppath->add_line_notes(plp->e.p.x, plp->e.p.y, sn_none);
    ppath->add_line_notes(current->x, current->y, sn_none);
    if (current->x == final->x && current->y == final->y)
      return;
  }
  do_pie_join(ppath, &plp->e.p, current, tangent, final, final_tangent,
              !reflected, &plp->width);
  if (ccw) {
    ppath->add_line_notes(plp->e.p.x, plp->e.p.y, sn_none);
    ppath->add_line_notes(plp->e.ce.x, plp->e.ce.y, sn_none);
  }
}

void join_under_pie(Path* ppath, pl_ptr plp, pl_ptr nplp, bool reflected) {
  Point dirn1, dirn2, tangmeet;
  double l, r;
  bool ccw;
  l = static_cast<double>(plp->width.x) * (nplp->width.y);
  r = static_cast<double>(nplp->width.x) * (plp->width.y);
  if (l == r)
    return;
  ccw = (l > r);
  ccw ^= reflected;
  if (ccw) {
    dirn1.x = -plp->width.x;
    dirn1.y = -plp->width.y;
    dirn2.x = -nplp->width.x;
    dirn2.y = -nplp->width.y;
    if (line_intersect(&plp->o.co, &dirn1, &nplp->e.ce, &dirn2, &tangmeet) !=
        0)
      return;
    ppath->close_subpath();
    ppath->add_point(tangmeet.x, tangmeet.y);
    ppath->add_line_notes(plp->o.co.x, plp->o.co.y, sn_none);
    do_pie_join(ppath, &plp->e.p, &plp->o.co, &plp->o.cdelta, &nplp->e.ce,
                &nplp->e.cdelta, !reflected, &plp->width);
  } else {
    if (line_intersect(&plp->o.ce, &plp->width, &nplp->e.co, &nplp->width,
                       &tangmeet) != 0)
      return;
    ppath->close_subpath();
    ppath->add_point(tangmeet.x, tangmeet.y);
    ppath->add_line_notes(nplp->e.co.x, nplp->e.co.y, sn_none);
    do_pie_join(ppath, &plp->e.p, &nplp->e.co, &nplp->e.cdelta, &plp->o.ce,
                &plp->o.cdelta, !reflected, &plp->width);
  }
}

// Returns 0 = use miter (mpt set), 1 = bevel, <0 error.
int check_miter(const LineParams& lp, pl_ptr plp, pl_ptr nplp,
                const Matrix* pmat, const Point* outp, const Point* np,
                Point* mpt, bool ccw0) {
  float check;
  double u1, v1, u2, v2;
  double num, denom;
  if (fixed2long(outp->x << 1) == fixed2long(np->x << 1) &&
      fixed2long(outp->y << 1) == fixed2long(np->y << 1))
    return 1;
  check = lp.miter_check;
  u1 = plp->vector.y, v1 = plp->vector.x;
  u2 = -nplp->vector.y, v2 = -nplp->vector.x;
  if (pmat) {
    double px, py;
    if (!DistanceTransformInverse(v1, u1, *pmat, &px, &py))
      return -1;
    v1 = px, u1 = py;
    if (!DistanceTransformInverse(v2, u2, *pmat, &px, &py))
      return -1;
    v2 = px, u2 = py;
    ccw0 = v1 * u2 < v2 * u1;
  }
  num = u1 * v2 - u2 * v1;
  denom = u1 * u2 + v1 * v2;
  if (!ccw0)
    num = -num;
  if (num == 0 && denom == 0)
    return -1;
  if (denom < 0)
    num = -num, denom = -denom;
  if (check > 0 ? (num < 0 || num >= denom * check)
                : (num < 0 && num >= denom * check)) {
    Point dirn1, dirn2;
    dirn1 = plp->e.cdelta;
    if ((std::abs(dirn1.x) + std::abs(dirn1.y) < 16) &&
        ((plp->vector.x != 0) || (plp->vector.y != 0))) {
      float scale = 65536.0f;
      if (std::abs(plp->vector.x) > std::abs(plp->vector.y))
        scale /= std::abs(plp->vector.x);
      else
        scale /= std::abs(plp->vector.y);
      dirn1.x = static_cast<fixed>(plp->vector.x * scale);
      dirn1.y = static_cast<fixed>(plp->vector.y * scale);
    }
    dirn2 = nplp->o.cdelta;
    if ((std::abs(dirn2.x) + std::abs(dirn2.y) < 16) &&
        ((nplp->vector.x != 0) || (nplp->vector.y != 0))) {
      float scale = 65536.0f;
      if (std::abs(nplp->vector.x) > std::abs(nplp->vector.y))
        scale /= std::abs(nplp->vector.x);
      else
        scale /= std::abs(nplp->vector.y);
      dirn2.x = static_cast<fixed>(-nplp->vector.x * scale);
      dirn2.y = static_cast<fixed>(-nplp->vector.y * scale);
    }
    if (line_intersect(outp, &dirn1, np, &dirn2, mpt) == 0)
      return 0;
  }
  return 1;
}

int line_join_points(const LineParams& lp, pl_ptr plp, pl_ptr nplp,
                     Point* join_points, const Matrix* pmat, int join,
                     bool reflected) {
  Point& jp1 = join_points[0];
  Point& np1 = join_points[1];
  Point& np2 = join_points[2];
  Point& jp2 = join_points[3];
  Point& jpx = join_points[4];
  bool ccw = static_cast<double>(plp->width.x) * (nplp->width.y) >
             static_cast<double>(nplp->width.x) * (plp->width.y);
  bool ccw0 = ccw;
  Point *outp, *np;
  Point mpt;
  ccw ^= reflected;
  jp1 = plp->e.co;
  jp2 = plp->e.ce;
  if (!ccw) {
    outp = &jp2;
    np2 = nplp->o.co;
    np1 = nplp->o.p;
    np = &np2;
  } else {
    outp = &jp1;
    np1 = nplp->o.ce;
    np2 = nplp->o.p;
    np = &np1;
  }
  if (join == gs_join_triangle) {
    fixed tpx = outp->x - nplp->o.p.x + np->x;
    fixed tpy = outp->y - nplp->o.p.y + np->y;
    jpx = jp2;
    if (!ccw) {
      jp2.x = tpx, jp2.y = tpy;
    } else {
      jp2 = np2;
      np2 = np1;
      np1.x = tpx, np1.y = tpy;
    }
    return 5;
  }
  if (join == gs_join_miter) {
    int code = check_miter(lp, plp, nplp, pmat, outp, np, &mpt, ccw0);
    if (code < 0)
      return code;
    if (code == 0)
      *outp = mpt;
  }
  return 4;
}

struct Ctx {
  const GState* gs;
  Device* dev;
  Matrix ctm;
};

// stroke_add (general case); fills ppath.
int stroke_add(Ctx& c, Path* ppath, int first, pl_ptr plp, pl_ptr nplp,
               int uniform, int join, bool reflected, int flags) {
  const GState& gs = *c.gs;
  const LineParams& lp = gs.lp;
  Point points[8];
  int npoints;
  int code;
  bool moveto_first = true;
  int start_cap = (flags & nf_dash_head ? lp.dash_cap : lp.start_cap);
  int end_cap = (flags & nf_dash_tail ? lp.dash_cap : lp.end_cap);

  if (plp->thin) {
    set_thin_widths(plp);
    adjust_stroke(c.dev, plp, gs, true, first == 0 && nplp == nullptr, flags);
    compute_caps(plp);
  }
  if (first == 0 && start_cap == gs_cap_round) {
    ppath->add_point(plp->o.co.x, plp->o.co.y);
    add_pie_cap(ppath, &plp->o);
    npoints = 0;
    moveto_first = false;
  } else {
    if ((npoints = cap_points((first == 0 ? start_cap : gs_cap_butt), &plp->o,
                              points)) < 0)
      return npoints;
  }
  if (nplp == nullptr) {
    if (end_cap == gs_cap_round) {
      points[npoints] = plp->e.co;
      ++npoints;
      add_points(ppath, points, npoints, moveto_first);
      add_pie_cap(ppath, &plp->e);
      goto done;
    }
    code = cap_points(end_cap, &plp->e, points + npoints);
  } else if (nplp->thin) {
    code = cap_points(gs_cap_butt, &plp->e, points + npoints);
  } else if (join == gs_join_round) {
    points[npoints] = plp->e.co;
    ++npoints;
    add_points(ppath, points, npoints, moveto_first);
    add_pie_join(ppath, plp, nplp, reflected, true);
    goto done;
  } else if (flags & nf_all_from_arc) {
    points[npoints] = plp->e.co;
    ++npoints;
    add_points(ppath, points, npoints, moveto_first);
    add_pie_join(ppath, plp, nplp, reflected, false);
    goto done;
  } else {
    code = line_join_points(lp, plp, nplp, points + npoints,
                            (uniform ? nullptr : &c.ctm), join, reflected);
  }
  if (code < 0)
    return code;
  add_points(ppath, points, npoints + code, moveto_first);
done:
  if ((flags & nf_some_from_arc) && (!plp->thin) && (nplp != nullptr) &&
      (!nplp->thin))
    join_under_pie(ppath, plp, nplp, reflected);
  ppath->close_subpath();
  return 0;
}

// stroke_fill: thin lines go to draw_thin_line; with non-zero fill adjust
// the parallelogram fast path is never taken, so the rest is stroke_add.
int stroke_fill(Ctx& c, Path* ppath, int first, pl_ptr plp, pl_ptr nplp,
                int uniform, int join, bool reflected, int flags) {
  if (plp->thin) {
    draw_thin_line(c.dev, plp->o.p.x, plp->o.p.y, plp->e.p.x, plp->e.p.y,
                   c.gs->fill_adjust_x, c.gs->fill_adjust_y);
    return 0;
  }
  // (fill_adjust.x | fill_adjust.y) != 0: parallelogram path skipped.
  return stroke_add(c, ppath, first, plp, nplp, uniform, join, reflected,
                    flags);
}

void BuildInputPath(const std::vector<Segment>& in, Path* p) {
  for (const Segment& sg : in) {
    switch (sg.type) {
      case Segment::Type::kMove:
        p->add_point(sg.x, sg.y);
        break;
      case Segment::Type::kLine:
        if (!p->subpath_open && p->s.empty() && !p->last_is_moveto)
          break;  // no current point
        p->add_line_notes(sg.x, sg.y, sn_none);
        break;
      case Segment::Type::kCurve:
        if (!p->subpath_open && p->s.empty() && !p->last_is_moveto)
          break;
        p->add_curve_notes(sg.c1x, sg.c1y, sg.c2x, sg.c2y, sg.x, sg.y,
                           sn_none);
        break;
      case Segment::Type::kClose:
        p->close_subpath();
        break;
    }
  }
}

}  // namespace

Output Stroke(const std::vector<Segment>& path_in,
              const Params& prm,
              int clip_x0,
              int clip_y0,
              int clip_x1,
              int clip_y1) {
  Output out;
  StrokeAdjustMemory local_sgr;
  Device devobj{clip_x1, clip_y1, clip_x0, clip_y0, clip_x1, clip_y1, &out,
                prm.sgr ? prm.sgr : &local_sgr};
  Device* dev = &devobj;

  GState gs;
  gs.ctm = {static_cast<float>(prm.ctm[0]), static_cast<float>(prm.ctm[1]),
            static_cast<float>(prm.ctm[2]), static_cast<float>(prm.ctm[3])};
  gs.lp.half_width = static_cast<float>(fabs(prm.width) / 2);
  gs.lp.start_cap = gs.lp.end_cap = gs.lp.dash_cap = prm.cap;
  gs.lp.join = prm.join;
  SetMiterLimit(&gs.lp, prm.miter_limit < 1.0 ? 1.0 : prm.miter_limit);
  {
    std::vector<float> pat;
    for (double d : prm.dash)
      pat.push_back(static_cast<float>(d));
    if (!SetDash(&gs.lp.dash, pat, prm.dash_phase))
      SetDash(&gs.lp.dash, {}, 0);
  }
  gs.stroke_adjust = prm.stroke_adjust;
  {
    double f = prm.flatness;  // gs_gstate_setflat clamp
    if (f <= 0.2)
      f = 0.2;
    else if (f > 100)
      f = 100;
    gs.flatness = static_cast<float>(f);
  }

  Path ppath;
  BuildInputPath(path_in, &ppath);

  Ctx c{&gs, dev, gs.ctm};
  const LineParams& lp = gs.lp;
  int dash_count = static_cast<int>(lp.dash.pattern.size());
  float xx = gs.ctm.xx, xy = gs.ctm.xy, yx = gs.ctm.yx, yy = gs.ctm.yy;
  enum { orient_other, orient_portrait, orient_landscape } orient;
  int uniform;
  bool reflected;
  if (xy == 0.0f && yx == 0.0f) {
    uniform = (xx == yy ? 1 : xx == -yy ? -1 : 0);
    reflected = (uniform ? uniform < 0 : (xx < 0) != (yy < 0));
    orient = orient_portrait;
  } else if (xx == 0.0f && yy == 0.0f) {
    uniform = (xy == yx ? -1 : xy == -yx ? 1 : 0);
    reflected = (uniform ? uniform < 0 : (xy < 0) == (yx < 0));
    orient = orient_landscape;
  } else {
    uniform = 0;
    reflected = xy * yx > xx * yy;
    orient = orient_other;
  }
  int curve_join =
      (lp.curve_join >= 0 ? lp.curve_join
       : lp.join == gs_join_none || lp.join == gs_join_round ? gs_join_bevel
                                                             : lp.join);
  float line_width = lp.half_width;
  bool always_thin;
  double line_width_and_scale;
  double device_line_width_scale = 0;
  double device_dot_length = lp.dot_length * fixed_1;  // 0 for pdfi
  // Raster devices have initial matrix [r/72 0 0 -r/72 0 H]: reflected.
  const bool initial_matrix_reflected = true;
  int flags;

  // Bounding box cull against the clip box.
  {
    fixed bpx, bpy, bqx, bqy;
    PathBBox(ppath, &bpx, &bpy, &bqx, &bqy);
    Point expansion;
    if (stroke_path_expansion(gs, ppath, &expansion) < 0) {
      bpx = bpy = min_fixed;
      bqx = bqy = max_fixed;
    } else {
      expansion.x += gs.fill_adjust_x;
      expansion.y += gs.fill_adjust_y;
      bpx = (bpx < min_fixed + expansion.x ? min_fixed : bpx - expansion.x);
      bpy = (bpy < min_fixed + expansion.y ? min_fixed : bpy - expansion.y);
      bqx = (bqx > max_fixed - expansion.x ? max_fixed : bqx + expansion.x);
      bqy = (bqy > max_fixed - expansion.y ? max_fixed : bqy + expansion.y);
    }
    fixed cpx = int2fixed(clip_x0), cpy = int2fixed(clip_y0),
          cqx = int2fixed(clip_x1), cqy = int2fixed(clip_y1);
    bool within = bpx >= cpx && bpy >= cpy && bqx <= cqx && bqy <= cqy;
    if (!within) {
      bpx = std::max(bpx, cpx);
      bpy = std::max(bpy, cpy);
      bqx = std::min(bqx, cqx);
      bqy = std::min(bqy, cqy);
      if (bpx >= bqx || bpy >= bqy)
        return out;
    }
  }

  if (line_width < 0)
    line_width = -line_width;
  line_width_and_scale = line_width * static_cast<double>(int2fixed(1));
  if (line_width == 0.0f) {
    always_thin = true;
  } else {
    float xa, ya;
    switch (orient) {
      case orient_portrait:
        xa = xx, ya = yy;
        goto sat;
      case orient_landscape:
        xa = xy, ya = yx;
      sat:
        if (xa < 0)
          xa = -xa;
        if (ya < 0)
          ya = -ya;
        always_thin = (std::max(xa, ya) * line_width < 0.5);
        if (!always_thin && uniform)
          device_line_width_scale = line_width_and_scale * xa;
        break;
      default: {
        double max_rr =
            (static_cast<double>(xx * xx + xy * xy + yx * yx + yy * yy) +
             sqrt(static_cast<double>((xy + yx) * (xy + yx) +
                                      (xx - yy) * (xx - yy)) *
                  ((xy - yx) * (xy - yx) + (xx + yy) * (xx + yy)))) /
            2;
        always_thin = max_rr * line_width * line_width < 0.25;
      }
    }
  }

  // Flatten.
  Path fpath, dpath;
  const Path* spath;
  if (ppath.curve_count == 0 && !PathHasLongSegments(ppath)) {
    if (ppath.first_subpath() == NIL)
      return out;
    spath = &ppath;
  } else {
    PathAddFlattenedForStroke(ppath, &fpath, float2fixed(gs.flatness), gs);
    spath = &fpath;
  }
  if (dash_count) {
    float max_dash_len = 0;
    float expand_squared;
    float adjust = static_cast<float>(gs.fill_adjust_x);
    if (adjust > static_cast<float>(gs.fill_adjust_y))
      adjust = static_cast<float>(gs.fill_adjust_y);
    for (int i = 0; i < dash_count; i++)
      if (max_dash_len < lp.dash.pattern[i])
        max_dash_len = lp.dash.pattern[i];
    expand_squared = gs.ctm.xx * gs.ctm.yy - gs.ctm.xy * gs.ctm.yx;
    if (expand_squared < 0)
      expand_squared = -expand_squared;
    expand_squared *= max_dash_len * max_dash_len;
    if (lp.half_width > 1)
      adjust /= lp.half_width;
    if (expand_squared * 65536.0f >= adjust * adjust) {
      PathAddDashExpansion(*spath, &dpath, gs);
      spath = &dpath;
    } else {
      dash_count = 0;
    }
  }

  Path stroke_path_body;
  Path* to_path = &stroke_path_body;
  auto FILL_STROKE_PATH = [&]() {
    if (!stroke_path_body.is_void()) {
      out.fill_paths.push_back(stroke_path_body.ToSegments());
      stroke_path_body = Path();
    }
  };

  const std::vector<Seg>& S = spath->s;
  int psub;
  for (psub = spath->first_subpath(); psub != NIL;) {
    int index = 0;
    int pseg = psub;
    fixed x = S[pseg].pt.x;
    fixed y = S[pseg].pt.y;
    bool is_closed = S[psub].is_closed;
    PartialLine pl{}, pl_prev{}, pl_first{};
    bool zero_length = true;
    int pseg_notes = S[pseg].notes;

    flags = nf_all_from_arc;

    while ((pseg = spath->next(pseg)) != NIL && S[pseg].type != s_start) {
      fixed sx, udx, sy, udy;
      bool is_dash_segment;

      pseg_notes = S[pseg].notes;

    d2:
      is_dash_segment = false;
    d1:
      if (S[pseg].type == s_dash) {
        sx = S[pseg].pt.x;
        sy = S[pseg].pt.y;
        udx = S[pseg].tangent.x;
        udy = S[pseg].tangent.y;
        is_dash_segment = true;
      } else if (S[pseg].type == s_gap) {
        sx = S[pseg].pt.x;
        sy = S[pseg].pt.y;
        udx = sx - x;
        udy = sy - y;
        is_dash_segment = true;
      } else {
        sx = S[pseg].pt.x;
        sy = S[pseg].pt.y;
        udx = sx - x;
        udy = sy - y;
      }
      zero_length &= ((udx | udy) == 0);
      pl.o.p.x = x, pl.o.p.y = y;
    d:
      flags = (((pseg_notes & sn_not_first)
                    ? ((flags & nf_all_from_arc) | nf_some_from_arc)
                    : 0) |
               ((pseg_notes & sn_dash_head) ? nf_dash_head : 0) |
               ((pseg_notes & sn_dash_tail) ? nf_dash_tail : 0) |
               (flags & ~nf_all_from_arc));
      pl.e.p.x = sx, pl.e.p.y = sy;
      if (!(udx | udy) || S[pseg].type == s_dash || S[pseg].type == s_gap) {
        if (index != 0 && S[pseg].type != s_dash && S[pseg].type != s_gap) {
          int nx = spath->next(pseg);
          if (nx == NIL || S[nx].type == s_start)
            continue;
          pseg = nx;
          pseg_notes = (((pseg_notes & sn_not_first) == 0)
                            ? (S[pseg].notes & ~sn_not_first)
                            : S[pseg].notes);
          goto d2;
        }
        while ((pseg = spath->next(pseg)) != NIL && S[pseg].type != s_start) {
          if (is_dash_segment)
            break;
          if (S[pseg].type == s_dash || S[pseg].type == s_gap)
            goto d1;
          sx = S[pseg].pt.x, udx = sx - x;
          sy = S[pseg].pt.y, udy = sy - y;
          if (udx | udy) {
            zero_length = false;
            goto d;
          }
        }
        if (lp.dot_length == 0 && lp.start_cap != gs_cap_round &&
            lp.end_cap != gs_cap_round && !is_dash_segment) {
          break;
        }
        {
          int end = S[psub].last;
          if (is_dash_segment) {
          } else if (end != NIL && (S[end].pt.x != x || S[end].pt.y != y)) {
            sx = S[end].pt.x, sy = S[end].pt.y, udx = sx - x, udy = sy - y;
          }
        }
        if ((udx | udy) == 0) {
          // dot_orientation is identity: portrait.
          udx = fixed_1;
        }
        if (sx == x && sy == y && (pseg == NIL || S[pseg].type == s_start)) {
          double scale = device_dot_length / hypot(static_cast<double>(udx),
                                                   static_cast<double>(udy));
          fixed udx1, udy1;
          if (!always_thin && (lp.start_cap == gs_cap_butt ||
                               lp.end_cap == gs_cap_butt ||
                               lp.dash_cap == gs_cap_butt)) {
            fixed dmax = std::max(any_abs(udx), any_abs(udy));
            if (dmax * scale < fixed_1)
              scale = static_cast<float>(fixed_1) / dmax;
          }
          udx1 = static_cast<fixed>(udx * scale);
          udy1 = static_cast<fixed>(udy * scale);
          sx = x + udx1;
          sy = y + udy1;
        }
        pseg = (pseg != NIL ? spath->prev(pseg) : S[psub].last);
        if (!is_dash_segment)
          goto d;
        pl.e.p.x = sx;
        pl.e.p.y = sy;
      }
      pl.vector.x = udx;
      pl.vector.y = udy;
      if (always_thin) {
        pl.e.cdelta.x = pl.e.cdelta.y = 0;
        pl.width.x = pl.width.y = 0;
        pl.thin = true;
      } else {
        if (uniform != 0) {
          double dpx = udx, dpy = udy;
          double wl = device_line_width_scale / hypot(dpx, dpy);
          pl.e.cdelta.x = static_cast<fixed>(dpx * wl);
          pl.e.cdelta.y = static_cast<fixed>(dpy * wl);
          if (initial_matrix_reflected)
            pl.width.x = pl.e.cdelta.y, pl.width.y = -pl.e.cdelta.x;
          else
            pl.width.x = -pl.e.cdelta.y, pl.width.y = pl.e.cdelta.x;
          pl.thin = false;
        } else {
          double dptx, dpty;
          float wl;
          if (!DistanceTransformInverse(static_cast<float>(udx),
                                        static_cast<float>(udy), gs.ctm, &dptx,
                                        &dpty)) {
            dptx = 0;
            dpty = 0;
          } else {
            wl = static_cast<float>(line_width_and_scale / hypot(dptx, dpty));
            dptx *= wl;
            dpty *= wl;
          }
          pl.e.cdelta.x = static_cast<fixed>(dptx * xx);
          pl.e.cdelta.y = static_cast<fixed>(dpty * yy);
          if (orient != orient_portrait)
            pl.e.cdelta.x += static_cast<fixed>(dpty * yx),
                pl.e.cdelta.y += static_cast<fixed>(dptx * xy);
          if (!reflected ^ initial_matrix_reflected)
            dptx = -dptx, dpty = -dpty;
          pl.width.x = static_cast<fixed>(dpty * xx),
          pl.width.y = -static_cast<fixed>(dptx * yy);
          if (orient != orient_portrait)
            pl.width.x -= static_cast<fixed>(dptx * yx),
                pl.width.y += static_cast<fixed>(dpty * xy);
          pl.thin = width_is_thin(&pl);
        }
        if (!pl.thin) {
          if (index)
            dev->sgr->stroke_stored = false;
          int pp = spath->prev(pseg), pn = spath->next(pseg);
          adjust_stroke(dev, &pl, gs, false,
                        (pp == NIL || S[pp].type == s_start) &&
                            (pn == NIL || S[pn].type == s_start) &&
                            (zero_length || !is_closed),
                        COMBINE_FLAGS(flags));
          compute_caps(&pl);
        }
      }
      if (index++) {
        int join = (pseg_notes & sn_not_first ? curve_join : lp.join);
        int first;
        pl_ptr lptr;
        if (join == gs_join_none) {
          first = 0;
          lptr = nullptr;
          index = 1;
        } else {
          first = (is_closed ? 1 : index - 2);
          lptr = &pl;
        }
        if (S[pseg].type == s_gap) {
          lptr = nullptr;
          index = 0;
        }
        int code = stroke_fill(c, to_path, first, &pl_prev, lptr, uniform,
                               join, initial_matrix_reflected,
                               COMBINE_FLAGS(flags));
        if (code < 0)
          return out;
        FILL_STROKE_PATH();
      } else if (S[pseg].type == s_gap) {
        index = 0;
      } else {
        pl_first = pl;
      }
      pl_prev = pl;
      x = sx, y = sy;
      flags = (flags << 4) | nf_all_from_arc;
    }
    if (index) {
      int notes = S[pseg == NIL ? spath->first_subpath() : pseg].notes;
      int join = (notes & sn_not_first ? curve_join : lp.join);
      pl_ptr lptr = (!is_closed || join == gs_join_none || zero_length
                         ? nullptr
                         : &pl_first);
      int ps1 = spath->next(psub);
      if (lptr && ps1 != NIL && S[ps1].type == s_gap)
        lptr = nullptr;
      flags = (((notes & sn_not_first)
                    ? ((flags & nf_all_from_arc) | nf_some_from_arc)
                    : 0) |
               ((notes & sn_dash_head) ? nf_dash_head : 0) |
               ((notes & sn_dash_tail) ? nf_dash_tail : 0) |
               (flags & ~nf_all_from_arc));
      int code = stroke_fill(c, to_path, index - 1, &pl_prev, lptr, uniform,
                             join, initial_matrix_reflected,
                             COMBINE_FLAGS(flags));
      if (code < 0)
        return out;
      FILL_STROKE_PATH();
      // traditional (CPSI) initial cap: params.traditional = false for S.
    }
    psub = pseg;
  }
  FILL_STROKE_PATH();
  return out;
}

}  // namespace gs_stroke
