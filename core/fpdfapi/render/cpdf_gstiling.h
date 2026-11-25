// pdfking: a tiling pattern's cell and steps on a plate of an ink render as
// Ghostscript 10.08 makes and tiles them (ported from Ghostscript 10.08
// base/gsptype1.c gs_pattern1_make_pattern, compute_inst_matrix,
// clamp_pattern_bbox; base/gxp1fill.c tile_by_steps; base/gsmatrix.c).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file (and the matching
// .cpp) is AGPL code, not under the PDFium BSD license.

#ifndef CORE_FPDFAPI_RENDER_CPDF_GSTILING_H_
#define CORE_FPDFAPI_RENDER_CPDF_GSTILING_H_

#include <functional>
#include <optional>

#include "core/fxcrt/fx_coordinates.h"

// gs's pattern instance: the cell rendered once, `width` x `height` device
// pixels, its pattern space mapped by `ctm` (the cell's (0, 0) the pixel
// (0, 0)), and stepped over the device by `step` (gs's step_matrix: the
// device offset of cell (i, j) is (i, j) through it, translation included).
struct GsTile {
  CFX_Matrix ctm;
  CFX_Matrix step;
  int width = 0;
  int height = 0;
  // the steps are whole pixels and the cell is one step: the device is the
  // cell repeated, (x, y) its pixel ((x + phase_x) mod width, ...)
  bool simple = false;
  // the cell in step space (gs's inst.bbox)
  double bbox[4] = {};
};

// The instance of a pattern with `bbox`, `xstep`, `ystep`, `tiling_type`
// whose pattern space maps to the device by `pattern2device`, on a device
// `page` (gs's device size, at its origin: the band's offset in it).
// Nothing for a pattern gs leaves empty or refuses (a singular step).
std::optional<GsTile> MakeGsTile(const CFX_Matrix& pattern2device,
                                 const CFX_FloatRect& bbox,
                                 float xstep,
                                 float ystep,
                                 int tiling_type,
                                 const FX_RECT& page);

// The cells of `tile` that may reach `rect` (device pixels), in gs's order
// (tile_by_steps): `place` is called with each cell's device origin.
void GsTileSteps(const GsTile& tile,
                 const FX_RECT& rect,
                 const std::function<void(int x, int y)>& place);

#endif  // CORE_FPDFAPI_RENDER_CPDF_GSTILING_H_
