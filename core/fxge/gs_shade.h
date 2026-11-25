// pdfking: Ghostscript's smooth shading fill, ported from Ghostscript 10.08
// base/gxshade6.c (the patch / wedge / triangle decomposition every shading
// type ends in), base/gxshade1.c (function based, axial and radial
// shadings), base/gxshade4.c (triangle meshes), the shading parts of
// base/gxshade.c, and the device side they paint through: the linear color
// trapezoids and triangles of base/gdevddrw.c / base/gxdtfill.h and the
// linear color scanlines of base/gdevdsha.c.
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file (and the matching
// .cpp) is AGPL code, not under the PDFium BSD license.

#ifndef CORE_FXGE_GS_SHADE_H_
#define CORE_FXGE_GS_SHADE_H_

#include <stdint.h>

#include <functional>
#include <vector>

namespace gs_shade {

// gs's gs_matrix: device = [x y 1] * matrix, floats.
struct Matrix {
  float xx, xy, yx, yy, tx, ty;
};

// What a shading paints with: its color space and function, as gs's
// shading fill state has them.
struct Colors {
  // The color space's components and their ranges (restrict_color clamps
  // to them; their spans are pfs->color_domain).
  int num_components = 0;
  std::vector<float> range_min, range_max;
  // The shading's Function (null: colors are the space's own): t (1 or 2
  // parameters) to the space's components.
  std::function<void(const float* in, float* out)> function;
  // gs_function_is_monotonic over [lower, upper] (per parameter): 1 with
  // *mask 0 monotonic, 0 with the bits of the parameters it is not (or does
  // not know) monotonic by, < 0 an error.
  std::function<int(const float* lower, const float* upper, unsigned* mask)>
      function_is_monotonic;
};

// The device a shading paints on, as gs's tiffsep (a DeviceN device of
// num_components 8-bit components) takes it.
struct Device {
  int num_components = 0;
  // The space's components to the device color: each component's 16-bit
  // color value (gs's gx_color_value, frac2cv of the concrete color).
  std::function<void(const float* cc, uint16_t* cv)> remap;
  // How gs tests the space for linearity (cs_is_linear): never (gs's
  // pfs->cs_always_linear: no ICC link or an identity one), through the
  // ICC link (gx_cspace_is_linear_ICC, `remap` standing in for the link),
  // or with device colors (gx_cspace_is_linear_default).
  enum class Linearity { kAlways, kIcc, kDefault } linearity =
      Linearity::kAlways;
  // A rectangle of pixels in one color (each component's 16-bit value), as
  // gs's fill_rectangle_hl_color gets it, in painting order. Not clipped
  // but to the clip box.
  std::function<void(int x, int y, int w, int h, const uint16_t* cv)> fill;
};

// The shading dictionary's geometry (ISO 32000 8.7.4.5).
struct Shading {
  int type = 0;  // 1 to 7
  // Type 1: Domain [x0 x1 y0 y1] and Matrix; types 2, 3: Coords, Domain
  // [t0 t1] and Extend.
  float domain[4] = {0, 1, 0, 1};
  Matrix matrix = {1, 0, 0, 1, 0, 0};
  float coords[6] = {};
  bool extend[2] = {false, false};
  // Types 4 to 7: the decoded stream gs reads its mesh from (packed bits),
  // BitsPerCoordinate, BitsPerComponent, BitsPerFlag, Decode and (type 5)
  // VerticesPerRow; the Function's domain, a vertex's parameter clamped to
  // it.
  std::vector<uint8_t> data;
  int bits_per_coordinate = 0;
  int bits_per_component = 0;
  int bits_per_flag = 0;
  std::vector<float> decode;
  int vertices_per_row = 0;
  float function_domain[2] = {0, 1};
  // An Indexed space's highest index and its entries in the base space
  // (Colors is the base's); hival < 0 for any other space.
  int hival = -1;
  std::function<void(int index, float* out)> lookup;
};

// Fills `shading` as gs's gs_shading_<type>_fill_rectangle does for a
// device clip box `clip` (gs fixed, 24.8: the clip path's pixel bounds)
// under `ctm` (shading space to device pixels), with gs's smoothness and
// flatness. Returns 0 or a negative gs error code.
int Fill(const Shading& shading,
         const Colors& colors,
         const Device& device,
         const Matrix& ctm,
         int32_t clip_x0,
         int32_t clip_y0,
         int32_t clip_x1,
         int32_t clip_y1,
         float smoothness,
         float flatness);

}  // namespace gs_shade

#endif  // CORE_FXGE_GS_SHADE_H_
