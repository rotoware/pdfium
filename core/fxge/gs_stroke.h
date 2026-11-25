// core/fxge/gs_stroke.h (pdfking: Ghostscript's stroker, gxstroke.c, ported)
//
// Ported from Ghostscript 10.08 base/gxstroke.c (AGPL), together with what
// the stroker needs on a plain (non-antialiased, abits == 1) memory device:
// gxpcopy.c (gx_path_copy_reducing for stroke, accurate curves), gxpflat.c
// (curve subdivision), gxpdash.c (dash expansion), gsline.c (gx_set_dash,
// miter_check), gdevddrw.c (gx_default_draw_thin_line, default
// fill_trapezoid / gxdtfill.h).
//
// Path of a PDF `S` in gs (pdfi): gs_stroke -> do_stroke (abits == 1) ->
// gx_stroke_fill (params.traditional = false, flatness = gstate flatness) ->
// gx_default_stroke_path -> gx_stroke_path_only with line_proc = stroke_fill.
// Because pdfi/gs_init.ps set fill adjust 0.5 (non-zero), stroke_fill never
// takes its parallelogram fast path: every wide piece goes through stroke_add
// and is filled immediately (FILL_STROKE_PATH after each segment, since the
// logical op is idempotent), and thin pieces go to draw_thin_line.

#ifndef CORE_FXGE_GS_STROKE_H_
#define CORE_FXGE_GS_STROKE_H_

#include <array>
#include <cstdint>
#include <vector>

namespace gs_stroke {

using fixed = int32_t;  // 24.8, device space, y down

struct Segment {
  enum class Type { kMove, kLine, kCurve, kClose } type;
  fixed x, y;
  fixed c1x, c1y, c2x, c2y;
};

// gs keeps the "stroke adjustment gradient recognizer" (dev->sgr) on the
// device: it survives from one stroke to the next and is reset by fill,
// eofill, image and text. Only consulted when stroke_adjust is true. Pass the
// same object for every stroke of a page (and Reset() it on those ops) to
// reproduce gs exactly; nullptr = a fresh state per Stroke() call.
struct StrokeAdjustMemory {
  bool stroke_stored = false;
  fixed orig[4][2] = {};
  fixed adjusted[4][2] = {};
  void Reset() { stroke_stored = false; }
};

struct Params {
  double width;   // line width in user space
  double ctm[4];  // user->device linear part (xx, xy, yx, yy), gs float CTM
  int cap, join;  // PDF values 0,1,2 (gs: butt/round/square, miter/round/bevel)
  double miter_limit;                           // pdfi clamps to >= 1
  std::vector<double> dash;                     // user space
  double dash_phase;                            // user space
  bool stroke_adjust;  // gs strokeadjust: true below 150 dpi, false at >= 150
  double flatness;     // gs flatness (pdfi: default 1.0, `i` clamps to [0.2,1])
  StrokeAdjustMemory* sgr = nullptr;
};

// What gs paints for the stroke.
//  - fill_paths: each entry is one gx_fill_path_only call gs makes, in order.
//    Rule: nonzero winding. Adjust: fill_adjust (0.5, 0.5) in both axes
//    (STROKE_ADJUSTMENT(always_thin=false) == pgs->fill_adjust), flatness for
//    curves = Params::flatness. Curves in these paths are round caps/joins
//    (gx_path_add_partial_arc); the scan converter flattens them as for any
//    fill. gs fills each piece separately (not one combined path); since the
//    adjusted fill is not exactly a Minkowski dilation per piece, keep them
//    separate and union the painted pixels.
//  - rects: painted directly via fill_rectangle (draw_thin_line and its
//    trapezoid filler, center-of-pixel rule), already clipped to the clip box.
struct Output {
  std::vector<std::vector<Segment>> fill_paths;
  std::vector<std::array<int, 4>> rects;  // x, y, w, h
};

// clip box is in device pixels [x0, x1) x [y0, y1). It acts as the device
// bounds (gs memory device fill_rectangle clipping) and as the clip path box
// used for gs's early cull. Fill paths are NOT clipped: clip them in the
// scan converter.
Output Stroke(const std::vector<Segment>& path,
              const Params& p,
              int clip_x0,
              int clip_y0,
              int clip_x1,
              int clip_y1);

}  // namespace gs_stroke

#endif  // CORE_FXGE_GS_STROKE_H_
