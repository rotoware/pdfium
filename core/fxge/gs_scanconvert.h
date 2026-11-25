// pdfking: Ghostscript's edge-buffer scan converter, ported from
// Ghostscript 10.08 base/gxscanc.c (plus the bits of gxfill.c, gxpath.c,
// gxpcopy.c, gxpflat.c, gdevddrw.c/gxdtfill.h a fill runs through).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file (and the matching
// .cpp) is AGPL code, not under the PDFium BSD license.

#ifndef CORE_FXGE_GS_SCANCONVERT_H_
#define CORE_FXGE_GS_SCANCONVERT_H_

#include <stdint.h>

#include <functional>
#include <vector>

namespace gs_scan {

// gs's fixed: 24.8 fixed point device coordinates.
using fixed = int32_t;

struct Segment {
  // kMove starts a new subpath at (x,y) (gs moveto, lazily: consecutive
  // moves just move the current point). kLine: line to (x,y). kCurve: curve
  // with control points c1, c2 to end point (x,y). kClose closes the current
  // subpath (gs s_line_close); x/y are ignored.
  enum class Type { kMove, kLine, kCurve, kClose } type;
  fixed x, y;
  fixed c1x, c1y, c2x, c2y;
};

enum class Rule { kWinding, kEvenOdd };

// Fills `path` (device fixed coordinates, y down) clipped to integer box
// `clip` (x0,y0 inclusive, x1,y1 exclusive), as gs's gx_general_fill_path ->
// gx_scan_convert_and_fill -> gx_scan_convert[_app|_tr|_tr_app] +
// gx_filter_edgebuffer* + gx_fill_edgebuffer* (+ gx_default_fill_trapezoid)
// do, calling fill_rect(x, y, w, h) for each
// rectangle gs's fill would pass to the device's fill_rectangle (after the
// clip device / fit_fill clipping to `clip`).
// any_part: fill adjust 0.5 ("any part of a pixel", gs's default), else
// fill adjust 0 (centre of pixel). flatness: the gstate flatness in device
// pixels (pgs->flatness, i.e. after gs_setflat's clamp to [0.2, 100]; gs's
// default is 1.0). As gx_general_fill_path does for an opaque fill, paths
// with curves are first flattened with gx_path_copy_reducing (accurate
// curves, unless > 50 subpaths), and the trapezoid converters are used
// unless the path has curves and flatness < 1.
// Returns 0, or a negative gs error code (gs_error_nocurrentpoint -14,
// gs_error_rangecheck -15, gs_error_VMerror -25).
int ScanConvertAndFill(const std::vector<Segment>& path,
                       int clip_x0,
                       int clip_y0,
                       int clip_x1,
                       int clip_y1,
                       double flatness,
                       Rule rule,
                       bool any_part,
                       const std::function<void(int x, int y, int w, int h)>& fill_rect,
                       bool clip = false);

// clip: the fill a clip path makes (gx_cpath_intersect_with_params): the
// path flattened first, every curve, accurately
// (gx_path_add_flattened_accurate), then filled.

// Fills the parallelogram at (px, py) with sides (ax, ay) and (bx, by)
// (device fixed coordinates) as gs's gx_default_fill_parallelogram
// (gdevddrw.c) does, a pixel when its centre is in it, calling fill_rect
// for each rectangle it fills within the clip box.
void FillParallelogram(fixed px, fixed py, fixed ax, fixed ay, fixed bx,
                       fixed by, int clip_x0, int clip_y0, int clip_x1,
                       int clip_y1,
                       const std::function<void(int x, int y, int w, int h)>& fill_rect);

}  // namespace gs_scan

#endif  // CORE_FXGE_GS_SCANCONVERT_H_
