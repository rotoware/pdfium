// pdfking: where Ghostscript puts an image's samples on the device, ported
// from Ghostscript 10.08 base/gxipixel.c, base/gxidata.c, base/gxicolor.c,
// base/gximono.c and base/gxifast.c (see gs_image.h), with gs's fixed point
// macros (base/gxfixed.h) and DDAs (base/gxdda.h).
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file is AGPL code, not
// under the PDFium BSD license.
//
// gs renders an image a row of samples at a time: a DDA steps the device
// position of each row's start (dda.row), another each sample's within the
// row (dda.strip / pixel0). In portrait or landscape a row covers the
// device rows (columns) whose centres lie in it (fixed2int_pixround_perfect
// of its two edges, gxidata.c); a sample covers the device columns (rows)
// between the rounded positions of its two edges, rounded by
// fixed2int_var_rounded (image_render_color_DeviceN) or fixed2int_pixround
// (image_render_mono, and image_simple_expand for one bit images, which
// copies the samples one to one when the image's width in device pixels is
// its width in samples).

#include "core/fxge/gs_image.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "core/fxcrt/check.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/gs_scanconvert.h"

namespace gs_image {

namespace {

/* ---------------- gxfixed.h ---------------- */

using fixed = int32_t;
constexpr int _fixed_shift = 8;
constexpr double fixed_scale = 1 << _fixed_shift;
constexpr fixed fixed_epsilon = 1;
constexpr fixed fixed_1 = fixed_epsilon << _fixed_shift;
constexpr fixed fixed_half = fixed_1 >> 1;
constexpr fixed _fixed_round_v = fixed_half;
constexpr fixed _fixed_pixround_v = _fixed_round_v - fixed_epsilon;
constexpr fixed _fixed_fraction_v = fixed_1 - fixed_epsilon;
constexpr fixed fixed_0 = 0;
constexpr fixed max_fixed = INT_MAX;
constexpr fixed min_fixed = INT_MIN;

fixed int2fixed(int i) {
  return static_cast<fixed>(static_cast<uint32_t>(i) << _fixed_shift);
}
int fixed2int(fixed x) {
  return x >> _fixed_shift;
}
int fixed2int_rounded(fixed x) {
  return (x + _fixed_round_v) >> _fixed_shift;
}
int fixed2int_ceiling(fixed x) {
  return (x + _fixed_fraction_v) >> _fixed_shift;
}
int fixed2int_pixround(fixed x) {
  return (x + _fixed_pixround_v) >> _fixed_shift;
}
int fixed2int_pixround_perfect(fixed x) {
  return x < 0 && (x & (fixed_1 - fixed_epsilon)) == fixed_half
             ? (x >> _fixed_shift) + 1
             : fixed2int_pixround(x);
}
long fixed2long_pixround(fixed x) {
  return static_cast<long>((static_cast<int64_t>(x) + _fixed_pixround_v) >>
                           _fixed_shift);
}
// float2fixed(f): ((fixed)((f)*(float)fixed_scale)), f a double here
fixed float2fixed(double f) {
  return static_cast<fixed>(f * static_cast<float>(fixed_scale));
}
double fixed2float(fixed x) {
  return x * (1.0 / fixed_scale);
}
fixed arith_rshift_1(fixed x) {
  return x >> 1;
}

/* ---------------- gxdda.h ---------------- */

struct gx_dda_fixed {
  struct {
    fixed Q;
    uint32_t R;
  } state;
  struct {
    fixed dQ;
    uint32_t dR, N;
  } step;
};

void dda_init(gx_dda_fixed& dda, fixed init, fixed D, int N_) {
  dda.state.Q = init;
  dda.state.R = N_ - 1;
  if (N_ == 0) {
    dda.step.dQ = 0, dda.step.dR = 0;
  } else if (D < 0) {
    dda.step.dQ = -static_cast<int>(static_cast<uint32_t>(-D) /
                                    static_cast<uint32_t>(N_));
    if ((dda.step.dR = -D % N_) != 0) {
      --dda.step.dQ, dda.step.dR = N_ - dda.step.dR;
    }
  } else {
    dda.step.dQ = D / N_;
    dda.step.dR = D % N_;
  }
  dda.step.N = N_;
}

fixed dda_current(const gx_dda_fixed& dda) {
  return dda.state.Q;
}

void dda_next(gx_dda_fixed& dda) {
  dda.state.R -= dda.step.dR;
  if (static_cast<int32_t>(dda.state.R) < 0) {
    dda.state.Q++;
    dda.state.R += dda.step.N;
  }
  dda.state.Q += dda.step.dQ;
}

/* ---------------- gxipixel.c ---------------- */

/* Convert and restrict to a valid range. */
fixed float2fixed_rounded_boxed(double src) {
  float v = floor(src * fixed_scale + 0.5);

  if (v <= static_cast<float>(min_fixed)) {
    return min_fixed;
  } else if (v >= static_cast<float>(max_fixed)) {
    return max_fixed;
  } else {
    return (fixed)v;
  }
}

struct gs_matrix_double {
  double xx, xy, yx, yy, tx, ty;
};

// gx_image_compute_mat for pdfi's ImageMatrix [W 0 0 -H 0 H] and the ctm
// `pmat` (gs_matrix_invert_to_double then gs_matrix_multiply_double, both
// with an ImageMatrix of no xy, yx).
gs_matrix_double gx_image_compute_mat(const CFX_Matrix& pmat,
                                      int width,
                                      int height) {
  const float im_xx = static_cast<float>(width);
  const float im_yy = static_cast<float>(height * -1);
  const float im_tx = 0;
  const float im_ty = static_cast<float>(height);
  gs_matrix_double rmat;
  if (im_xx == pmat.a && 0 == pmat.b && 0 == pmat.c && im_yy == pmat.d) {
    /* Process common special case separately to accept singular matrix. */
    rmat.xx = rmat.yy = 1.;
    rmat.xy = rmat.yx = 0.;
    rmat.tx = pmat.e - im_tx;
    rmat.ty = pmat.f - im_ty;
    return rmat;
  }
  // gs_matrix_invert_to_double
  const double xx1 = 1.0 / im_xx;
  const double tx1 = -xx1 * im_tx;
  const double yy1 = 1.0 / im_yy;
  const double ty1 = -yy1 * im_ty;
  // gs_matrix_multiply_double (pm1 is_xxyy)
  const double xx2 = pmat.a, yy2 = pmat.d;
  const double xy2 = pmat.b, yx2 = pmat.c;
  rmat.tx = tx1 * xx2 + pmat.e;
  rmat.ty = ty1 * yy2 + pmat.f;
  if (xy2 == 0.0) {
    rmat.xy = 0;
  } else {
    rmat.xy = xx1 * xy2, rmat.ty += tx1 * xy2;
  }
  rmat.xx = xx1 * xx2;
  if (yx2 == 0.0) {
    rmat.yx = 0;
  } else {
    rmat.yx = yy1 * yx2, rmat.tx += ty1 * yx2;
  }
  rmat.yy = yy1 * yy2;
  return rmat;
}

// The grid fit gx_image_enum_begin gives an orthogonal image drawn in a soft
// mask (`gridfitimages`), and an image mask (of type 1, outside a pattern, a
// soft mask, a type 3 glyph) one sample wide or high: stretched to the device
// pixels it touches (gs's fill adjust is not 0).
void GridFit(gs_matrix_double& mat,
             int width,
             int height,
             bool gridfitimages) {
  int orthogonal;
  if (mat.xy == 0 && mat.yx == 0) {
    orthogonal = 1;
  } else if (mat.xx == 0 && mat.yy == 0) {
    orthogonal = 2;
  } else {
    orthogonal = 0;
  }
  if (orthogonal == 1) {
    if (width == 1 || gridfitimages) {
      if (mat.xx > 0) {
        fixed ix0 = int2fixed(fixed2int(float2fixed(mat.tx)));
        double x1 = mat.tx + mat.xx * width;
        fixed ix1 = int2fixed(fixed2int_ceiling(float2fixed(x1)));
        mat.tx = (double)fixed2float(ix0);
        mat.xx = (double)(fixed2float(ix1 - ix0) / width);
      } else if (mat.xx < 0) {
        fixed ix0 = int2fixed(fixed2int_ceiling(float2fixed(mat.tx)));
        double x1 = mat.tx + mat.xx * width;
        fixed ix1 = int2fixed(fixed2int(float2fixed(x1)));
        mat.tx = (double)fixed2float(ix0);
        mat.xx = (double)(fixed2float(ix1 - ix0) / width);
      }
    }
    if (height == 1 || gridfitimages) {
      if (mat.yy > 0) {
        fixed iy0 = int2fixed(fixed2int(float2fixed(mat.ty)));
        double y1 = mat.ty + mat.yy * height;
        fixed iy1 = int2fixed(fixed2int_ceiling(float2fixed(y1)));
        mat.ty = (double)fixed2float(iy0);
        mat.yy = (double)(fixed2float(iy1 - iy0) / height);
      } else if (mat.yy < 0) {
        fixed iy0 = int2fixed(fixed2int_ceiling(float2fixed(mat.ty)));
        double y1 = mat.ty + mat.yy * height;
        fixed iy1 = int2fixed(fixed2int(float2fixed(y1)));
        mat.ty = (double)fixed2float(iy0);
        mat.yy = ((double)fixed2float(iy1 - iy0) / height);
      }
    }
  } else if (orthogonal == 2) {
    if (height == 1 || gridfitimages) {
      if (mat.yx > 0) {
        fixed ix0 = int2fixed(fixed2int(float2fixed(mat.tx)));
        double x1 = mat.tx + mat.yx * height;
        fixed ix1 = int2fixed(fixed2int_ceiling(float2fixed(x1)));
        mat.tx = (double)fixed2float(ix0);
        mat.yx = (double)(fixed2float(ix1 - ix0) / height);
      } else if (mat.yx < 0) {
        fixed ix0 = int2fixed(fixed2int_ceiling(float2fixed(mat.tx)));
        double x1 = mat.tx + mat.yx * height;
        fixed ix1 = int2fixed(fixed2int(float2fixed(x1)));
        mat.tx = (double)fixed2float(ix0);
        mat.yx = (double)(fixed2float(ix1 - ix0) / height);
      }
    }
    if (width == 1 || gridfitimages) {
      if (mat.xy > 0) {
        fixed iy0 = int2fixed(fixed2int(float2fixed(mat.ty)));
        double y1 = mat.ty + mat.xy * width;
        fixed iy1 = int2fixed(fixed2int_ceiling(float2fixed(y1)));
        mat.ty = (double)fixed2float(iy0);
        mat.xy = (double)(fixed2float(iy1 - iy0) / width);
      } else if (mat.xy < 0) {
        fixed iy0 = int2fixed(fixed2int_ceiling(float2fixed(mat.ty)));
        double y1 = mat.ty + mat.xy * width;
        fixed iy1 = int2fixed(fixed2int(float2fixed(y1)));
        mat.ty = (double)fixed2float(iy0);
        mat.xy = ((double)fixed2float(iy1 - iy0) / width);
      }
    }
  }
}

enum image_posture { image_portrait, image_landscape, image_skewed };

// Device span [lo, hi) of each sample along one device axis
struct Spans {
  std::vector<int> lo, hi;
};

void AddSpan(Spans& spans, int a, int b) {
  spans.lo.push_back(std::min(a, b));
  spans.hi.push_back(std::max(a, b));
}

// Each device pixel of [start, start + size) the index of the span covering
// it, -1 where none does
std::vector<int> Coverage(const Spans& spans, int start, int size) {
  std::vector<int> map(size, -1);
  for (size_t i = 0; i < spans.lo.size(); ++i) {
    for (int p = std::max(spans.lo[i], start);
         p < std::min(spans.hi[i], start + size); ++p) {
      map[p - start] = static_cast<int>(i);
    }
  }
  return map;
}

// A skewed image: each sample the parallelogram between its edges along
// its row and the row's next (image_render_color_DeviceN, image_render_mono
// "not orthogonal"), filled through gx_default_fill_parallelogram, in order
// (a later sample over an earlier). In a bitmap with alpha (BGRA, or a mask)
// as the parallelograms leave pixels of their box unpainted.
void DrawSkewed(const RetainPtr<const CFX_DIBBase>& source,
                FXDIB_GsSamples samples,
                gx_dda_fixed row_x,
                gx_dda_fixed row_y,
                const gx_dda_fixed& strip_x,
                const gx_dda_fixed& strip_y,
                const FX_RECT& clip,
                RetainPtr<CFX_DIBitmap>* bitmap,
                int* left,
                int* top) {
  const int width = source->GetWidth();
  const int height = source->GetHeight();
  // each sample's start along a row, from the row's start
  std::vector<fixed> dx(width + 1), dy(width + 1);
  {
    gx_dda_fixed px = strip_x, py = strip_y;
    for (int i = 0; i <= width; ++i) {
      dx[i] = dda_current(px) - dda_current(strip_x);
      dy[i] = dda_current(py) - dda_current(strip_y);
      dda_next(px);
      dda_next(py);
    }
  }
  std::vector<fixed> rx(height + 1), ry(height + 1);
  for (int j = 0; j <= height; ++j) {
    rx[j] = dda_current(row_x);
    ry[j] = dda_current(row_y);
    dda_next(row_x);
    dda_next(row_y);
  }
  // the box of the corners, to the pixels whose centres it may hold
  fixed x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  for (int j : {0, height}) {
    for (int i : {0, width}) {
      x0 = std::min(x0, rx[j] + dx[i]);
      x1 = std::max(x1, rx[j] + dx[i]);
      y0 = std::min(y0, ry[j] + dy[i]);
      y1 = std::max(y1, ry[j] + dy[i]);
    }
  }
  FX_RECT rect(fixed2int(x0), fixed2int(y0), fixed2int_ceiling(x1) + 1,
               fixed2int_ceiling(y1) + 1);
  rect.Intersect(clip);
  *bitmap = nullptr;
  *left = rect.left;
  *top = rect.top;
  if (rect.IsEmpty()) {
    return;
  }
  const FXDIB_Format format = source->IsMaskFormat() ? FXDIB_Format::k8bppMask
                                                     : FXDIB_Format::kBgra;
  RetainPtr<CFX_DIBitmap> src = source->Realize();
  auto out = pdfium::MakeRetain<CFX_DIBitmap>();
  // (a one bit image's colors through BGR: no conversion takes them to BGRA)
  if (!src ||
      (src->GetBPP() == 1 && !src->IsMaskFormat() &&
       !src->ConvertFormat(FXDIB_Format::kBgr)) ||
      !src->ConvertFormat(format) ||
      !out->Create(rect.Width(), rect.Height(), format)) {
    return;
  }
  out->Clear(0);
  const int bytes = src->GetBPP() / 8;
  for (int j = 0; j < height; ++j) {
    pdfium::span<const uint8_t> scan = src->GetScanline(j);
    const fixed pdyx = rx[j + 1] - rx[j];
    const fixed pdyy = ry[j + 1] - ry[j];
    for (int i = 0; i < width; ++i) {
      gs_scan::FillParallelogram(
          rx[j] + dx[i], ry[j] + dy[i], dx[i + 1] - dx[i], dy[i + 1] - dy[i],
          pdyx, pdyy, rect.left, rect.top, rect.right, rect.bottom,
          [&](int x, int y, int w, int h) {
            for (int yy = y; yy < y + h; ++yy) {
              pdfium::span<uint8_t> dest =
                  out->GetWritableScanline(yy - rect.top);
              for (int xx = x; xx < x + w; ++xx) {
                for (int k = 0; k < bytes; ++k) {
                  dest[(xx - rect.left) * bytes + k] = scan[i * bytes + k];
                }
              }
            }
          });
    }
  }
  *bitmap = std::move(out);
}


/* ---------------- siscale.c (the Mitchell scaler, /Interpolate) ---------------- */

constexpr int CONTRIB_SHIFT = 12;
constexpr int CONTRIB_SCALE = 1 << CONTRIB_SHIFT;
constexpr int CONTRIB_ROUND = 1 << (CONTRIB_SHIFT - 1);
constexpr int frac_1 = 0x7ff8;
constexpr int MAX_ISCALE_SUPPORT = 1 << 3;

constexpr int Mitchell_support = 2;
constexpr double Mitchell_min_scale =
    (Mitchell_support * 2) / (MAX_ISCALE_SUPPORT - 1.01);
constexpr double B = 1.0 / 3.0;
constexpr double C = 1.0 / 3.0;
double Mitchell_filter(double t) {
  double t2 = t * t;

  if (t < 0) {
    t = -t;
  }

  if (t < 1) {
    return ((12 - 9 * B - 6 * C) * (t * t2) + (-18 + 12 * B + 6 * C) * t2 +
            (6 - 2 * B)) /
           6;
  } else if (t < 2) {
    return ((-1 * B - 6 * C) * (t * t2) + (6 * B + 30 * C) * t2 +
            (-12 * B - 48 * C) * t + (8 * B + 24 * C)) /
           6;
  } else {
    return 0;
  }
}

constexpr int Interp_support = 1;
constexpr double Interp_min_scale = 0;
double Interp_filter(double t) {
  if (t < 0) {
    t = -t;
  }

  if (t >= 1) {
    return 0;
  }
  return 1 + (2 * t - 3) * t * t;
}

struct filter_defn_s {
  double (*filter)(double);
  int filter_width;
  double min_scale;
};
constexpr filter_defn_s Mitchell_defn = {Mitchell_filter, Mitchell_support,
                                         Mitchell_min_scale};
constexpr filter_defn_s Interp_defn = {Interp_filter, Interp_support,
                                       Interp_min_scale};

// One output sample's contributors: the first input sample, and a weight
// for each of the following (calculate_contrib's CLIST and CONTRIB, the
// input index plain: no stride, no ring buffer)
struct Contrib {
  int first_pixel;
  std::vector<int> weight;
};

// calculate_contrib for output samples [start, start + size) of an axis of
// `src_size` input samples scaled to `dst_size` (the subimage at 0)
std::vector<Contrib> calculate_contrib(double scale,
                                       int start,
                                       int size,
                                       int dst_size,
                                       int src_size,
                                       double rescale_factor,
                                       const filter_defn_s& defn) {
  double WidthIn, fscale;
  bool squeeze;
  const int limit = src_size;
  auto fproc = defn.filter;

  if (scale < 1.0) {
    double clamped_scale = std::max(scale, defn.min_scale);
    WidthIn = ((double)defn.filter_width) / clamped_scale;
    fscale = 1.0 / clamped_scale;
    squeeze = true;
  } else {
    WidthIn = (double)defn.filter_width;
    fscale = 1.0;
    squeeze = false;
  }

  std::vector<Contrib> contrib(size);
  for (int i = 0; i < size; ++i) {
    int center_denom = dst_size * 2;
    int64_t center_num = (start + i) * (int64_t)src_size * 2 + src_size - dst_size;
    int left = (int)ceil((center_num - WidthIn * center_denom) / center_denom);
    int right = (int)floor((center_num + WidthIn * center_denom) / center_denom);
    double center = (double)center_num / center_denom;
    auto clamp_pixel = [&](int j) {
      return j < 0 ? 0 : j >= limit ? limit - 1 : j;
    };
    int first_pixel = clamp_pixel(left);
    int last_pixel = clamp_pixel(right);
    Contrib& p = contrib[i];
    p.first_pixel = first_pixel;
    p.weight.assign(last_pixel - first_pixel + 1, 0);
    double sum = 0;
    double e = 0;
    for (int j = left; j <= right; ++j) {
      sum += squeeze ? fproc((center - j) / fscale) / fscale
                     : fproc(center - j);
    }
    for (int j = left; j <= right; ++j) {
      double weight = (squeeze ? fproc((center - j) / fscale) / fscale
                               : fproc(center - j)) /
                      sum;
      int n = clamp_pixel(j);
      int k = n - first_pixel;
      int ie;

      e += (weight * rescale_factor) * CONTRIB_SCALE;
      ie = (int)(e + 0.5);
      p.weight[k] += ie;
      e -= ie;
    }
  }
  return contrib;
}

// fixed2int_pixround_perfect of a 64-bit fixed (safe64fixed2int, no
// overflow check)
int pixround_perfect64(int64_t v) {
  return fixed2int_pixround_perfect(static_cast<fixed>(v));
}

// The device value gs gives an interpolated frac: its 16-bit color value's
// high byte (frac2cv)
int FracByte(int f) {
  return ((f << 1) + (f >> 11)) >> 8;
}

// gs_image_class_0_interpolate and image_render_interpolate{,_landscape}
// (gxiscale.c), interpolate_control 1, a whole image (rect = rrect = drect)
// through s_IScale (siscale.c): the image's gray samples (a plate's, or a
// soft mask's), output a device pixel each. False where gs does not
// interpolate.
bool DrawInterpolated(const RetainPtr<const CFX_DIBBase>& source,
                      const gs_matrix_double& mat,
                      image_posture posture,
                      fixed mtx,
                      fixed mty,
                      fixed row_extent_x,
                      fixed row_extent_y,
                      fixed col_extent_x,
                      fixed col_extent_y,
                      bool additive,
                      const FX_RECT& clip,
                      RetainPtr<CFX_DIBitmap>* bitmap,
                      int* left,
                      int* top) {
  const int Width = source->GetWidth();
  const int Height = source->GetHeight();
  if (posture == image_skewed || Width == 0 || Height == 0 ||
      (Width == 1 && Height == 1)) {
    return false;
  }
  const bool portrait = posture == image_portrait;
  const fixed dst_width = portrait ? row_extent_x : col_extent_x;
  const fixed dst_height = portrait ? col_extent_y : row_extent_y;
  const int64_t dw = std::abs(dst_width);
  const int64_t dh = std::abs(dst_height);
  // the output along a source row and across them
  const int WidthIn = Width;
  const int HeightIn = Height;
  const int WidthOut = pixround_perfect64(portrait ? dw : dh);
  const int HeightOut = pixround_perfect64(portrait ? dh : dw);
  if (WidthOut == 0 || HeightOut == 0 ||
      (WidthOut == WidthIn && HeightOut == HeightIn)) {
    return false;
  }
  // where the output's rows (`ry`) and samples (`xo` on) go
  const int yi0 = fixed2int_pixround_perfect(mty);
  int xo, yo, dy;
  if (portrait) {
    xo = fixed2int_pixround(mat.xx < 0 ? mtx + row_extent_x : mtx);
    yo = yi0;
    dy = mat.yy > 0 ? 1 : -1;
  } else {
    xo = yi0 + pixround_perfect64(mat.xy < 0 ? dst_height : 0);
    yo = fixed2int_pixround(mtx);
    dy = mat.yx > 0 ? 1 : -1;
  }
  if (dy < 0) {
    yo--;
  }
  const bool reversed = (portrait ? mat.xx : mat.xy) < 0;

  // the device box, clipped; the output samples and rows it holds
  FX_RECT rect = portrait
                     ? FX_RECT(xo, std::min(yo, yo + (HeightOut - 1) * dy),
                               xo + WidthOut,
                               std::max(yo, yo + (HeightOut - 1) * dy) + 1)
                     : FX_RECT(std::min(yo, yo + (HeightOut - 1) * dy), xo,
                               std::max(yo, yo + (HeightOut - 1) * dy) + 1,
                               xo + WidthOut);
  rect.Intersect(clip);
  *bitmap = nullptr;
  *left = rect.left;
  *top = rect.top;
  if (rect.IsEmpty()) {
    return true;
  }
  const int x_lo = (portrait ? rect.left : rect.top) - xo;
  const int x_hi = (portrait ? rect.right : rect.bottom) - xo;
  const int lo_line_dev = portrait ? rect.top : rect.left;
  const int hi_line_dev = portrait ? rect.bottom : rect.right;

  // the samples, as bytes: a soft mask's as they are, a plate's gray as it
  // is (an additive space's samples) or inverted (the ink: a subtractive
  // space's)
  RetainPtr<CFX_DIBitmap> src = source->Realize();
  const bool mask = source->IsMaskFormat();
  if (!src || (!mask && !src->ConvertFormat(FXDIB_Format::kBgr)) ||
      (mask && !src->ConvertFormat(FXDIB_Format::k8bppMask))) {
    return false;
  }
  const int bytes = mask ? 1 : 3;
  auto sample = [&](int row, int col) -> int {
    const int v = src->GetScanline(row)[(reversed ? WidthIn - 1 - col : col) *
                                        bytes];
    return mask || additive ? v : 255 - v;
  };

  // s_IScale_init: Mitchell, but the simple interpolation filter along an
  // axis scaled down
  const filter_defn_s& horiz =
      WidthOut < WidthIn ? Interp_defn : Mitchell_defn;
  const filter_defn_s& vert =
      HeightOut < HeightIn ? Interp_defn : Mitchell_defn;
  // zoom_x1: each source row to bytes, the output samples [x_lo, x_hi)
  const std::vector<Contrib> xc =
      calculate_contrib((double)WidthOut / WidthIn, x_lo, x_hi - x_lo,
                        WidthOut, WidthIn, 255. / 0xff, horiz);
  std::vector<std::vector<uint8_t>> tmp(HeightIn);
  auto tmp_row = [&](int y) -> const std::vector<uint8_t>& {
    std::vector<uint8_t>& t = tmp[y];
    if (t.empty()) {
      t.resize(xc.size());
      for (size_t i = 0; i < xc.size(); ++i) {
        int weight = 0;
        for (size_t j = 0; j < xc[i].weight.size(); ++j) {
          weight += sample(y, xc[i].first_pixel + static_cast<int>(j)) *
                    xc[i].weight[j];
        }
        weight = (weight + CONTRIB_ROUND) >> CONTRIB_SHIFT;
        t[i] = static_cast<uint8_t>(std::clamp(weight, 0, 255));
      }
    }
    return t;
  };

  auto out = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!out->Create(rect.Width(), rect.Height(),
                   mask ? FXDIB_Format::k8bppMask : FXDIB_Format::kBgr)) {
    return false;
  }
  for (int line = 0; line < HeightOut; ++line) {
    const int ry = yo + line * dy;
    if (ry < lo_line_dev || ry >= hi_line_dev) {
      continue;
    }
    // zoom_y2_frac: the output row, fracs
    const Contrib yc = calculate_contrib((double)HeightOut / HeightIn, line, 1,
                                         HeightOut, HeightIn,
                                         (double)frac_1 / 255, vert)[0];
    std::vector<const std::vector<uint8_t>*> rows;
    for (size_t j = 0; j < yc.weight.size(); ++j) {
      rows.push_back(&tmp_row(yc.first_pixel + static_cast<int>(j)));
    }
    for (int x = x_lo; x < x_hi; ++x) {
      int weight = 0;
      for (size_t j = 0; j < rows.size(); ++j) {
        weight += (*rows[j])[x - x_lo] * yc.weight[j];
      }
      const int v = std::clamp((weight + CONTRIB_ROUND) >> CONTRIB_SHIFT, 0,
                               frac_1);
      // the frac as the device takes it: a soft mask's gray, else the
      // plate's ink (frac_1 - v of an additive space), its gray 255 - it
      const int value = mask       ? FracByte(v)
                        : additive ? 255 - FracByte(frac_1 - v)
                                   : 255 - FracByte(v);
      const int px = (portrait ? xo + x : ry) - rect.left;
      const int py = (portrait ? ry : xo + x) - rect.top;
      pdfium::span<uint8_t> dest = out->GetWritableScanline(py);
      for (int k = 0; k < bytes; ++k) {
        dest[px * bytes + k] = static_cast<uint8_t>(value);
      }
    }
  }
  *bitmap = std::move(out);
  return true;
}

}  // namespace

void Draw(RetainPtr<const CFX_DIBBase> source,
          const CFX_Matrix& matrix,
          FXDIB_GsSamples samples,
          bool soft_mask,
          bool interpolate,
          bool additive,
          const FX_RECT& clip,
          RetainPtr<CFX_DIBitmap>* bitmap,
          int* left,
          int* top) {
  const int width = source->GetWidth();
  const int height = source->GetHeight();
  gs_matrix_double mat = gx_image_compute_mat(matrix, width, height);
  if (soft_mask || samples == FXDIB_GsSamples::kMask) {
    GridFit(mat, width, height, soft_mask);
  }

  /*
   * Compute extents with distance transformation.
   */
  fixed mtx, mty;
  if (mat.tx > 0) {
    mtx = float2fixed(mat.tx);
  } else { /* Use positive values to ensure round down. */
    int f = (int)-mat.tx + 1;

    mtx = float2fixed(mat.tx + f) - int2fixed(f);
  }
  if (mat.ty > 0) {
    mty = float2fixed(mat.ty);
  } else { /* Use positive values to ensure round down. */
    int f = (int)-mat.ty + 1;

    mty = float2fixed(mat.ty + f) - int2fixed(f);
  }

  struct {
    fixed x, y;
  } row_extent, col_extent;
  row_extent.x = float2fixed_rounded_boxed(width * mat.xx);
  row_extent.y =
      (mat.xy == 0.0 ? fixed_0 : float2fixed_rounded_boxed(width * mat.xy));
  col_extent.x =
      (mat.yx == 0.0 ? fixed_0 : float2fixed_rounded_boxed(height * mat.yx));
  col_extent.y = float2fixed_rounded_boxed(height * mat.yy);
  // the whole image: x_extent = row_extent, y_extent = col_extent
  const image_posture posture =
      ((row_extent.y | col_extent.x) == 0   ? image_portrait
       : (row_extent.x | col_extent.y) == 0 ? image_landscape
                                            : image_skewed);
  {
    fixed epx = std::min(row_extent.x, 0) + std::min(col_extent.x, 0),
          eqx = std::max(row_extent.x, 0) + std::max(col_extent.x, 0),
          epy = std::min(row_extent.y, 0) + std::min(col_extent.y, 0),
          eqy = std::max(row_extent.y, 0) + std::max(col_extent.y, 0);
    int hwx, hwy;

    switch (posture) {
      case image_portrait:
        hwx = width, hwy = height;
        break;
      case image_landscape:
        hwx = height, hwy = width;
        break;
      default:
        hwx = hwy = 0;
    }
    /*
     * If the image is only 1 sample wide or high,
     * and is less than 1 device pixel wide or high,
     * move it slightly so that it covers pixel centers.
     */
    if (hwx == 1 && eqx - epx < fixed_1) {
      fixed diff = arith_rshift_1(row_extent.x + col_extent.x);

      mtx = (((mtx + diff) | fixed_half) & -fixed_half) - diff;
    }
    if (hwy == 1 && eqy - epy < fixed_1) {
      fixed diff = arith_rshift_1(row_extent.y + col_extent.y);

      mty = (((mty + diff) | fixed_half) & -fixed_half) - diff;
    }
  }
  /* These DDAs enumerate the starting position of each source pixel
   * row in device space. */
  gx_dda_fixed row_x, row_y;
  dda_init(row_x, mtx, col_extent.x, height);
  dda_init(row_y, mty, col_extent.y, height);
  /* These DDAs enumerate the starting positions of each row of our
   * source pixel data. A row's strip only moves along the axis its rows
   * step (update_strip), so the samples of every row fall alike. */
  gx_dda_fixed strip_x, strip_y;
  dda_init(strip_x, dda_current(row_x), row_extent.x, width);
  dda_init(strip_y, dda_current(row_y), row_extent.y, width);

  if (interpolate &&
      DrawInterpolated(source, mat, posture, mtx, mty, row_extent.x,
                       row_extent.y, col_extent.x, col_extent.y, additive,
                       clip, bitmap, left, top)) {
    return;
  }

  if (posture == image_skewed) {
    DrawSkewed(source, samples, row_x, row_y, strip_x, strip_y, clip, bitmap,
               left, top);
    return;
  }

  // gxidata.c: each row's device rows (portrait) or columns (landscape),
  // those whose centres it covers; empty for a row gs skips
  Spans rows;
  for (int y = 0; y < height; ++y) {
    gx_dda_fixed& row = posture == image_portrait ? row_y : row_x;
    fixed c = dda_current(row);
    dda_next(row);
    fixed n = dda_current(row);
    if (n < c) {
      std::swap(n, c);
    }
    const int ci = fixed2int_pixround_perfect(c);
    AddSpan(rows, ci, fixed2int_pixround_perfect(n));
  }

  // each sample's device columns (portrait) or rows (landscape): between
  // its edges rounded as the renderer rounds them
  gx_dda_fixed& pixel = posture == image_portrait ? strip_x : strip_y;
  const fixed extent = posture == image_portrait ? row_extent.x : row_extent.y;
  const fixed o = dda_current(pixel);
  const bool one_bit = samples == FXDIB_GsSamples::kMonobit ||
                       samples == FXDIB_GsSamples::kMask;
  // image_render_simple without a line buffer: one device pixel a sample
  const bool copy = one_bit && posture == image_portrait &&
                    fixed2long_pixround(o + extent) - fixed2long_pixround(o) ==
                        width;
  auto edge = [&](fixed x, int i) {
    if (copy) {
      return fixed2int_pixround(o) + i;
    }
    return samples == FXDIB_GsSamples::kColor ? fixed2int_rounded(x)
                                              : fixed2int_pixround(x);
  };
  Spans cols;
  int prev = edge(dda_current(pixel), 0);
  for (int i = 0; i < width; ++i) {
    dda_next(pixel);
    const int next = edge(dda_current(pixel), i + 1);
    AddSpan(cols, prev, next);
    prev = next;
  }

  const Spans& xs = posture == image_portrait ? cols : rows;
  const Spans& ys = posture == image_portrait ? rows : cols;
  FX_RECT rect(*std::min_element(xs.lo.begin(), xs.lo.end()),
               *std::min_element(ys.lo.begin(), ys.lo.end()),
               *std::max_element(xs.hi.begin(), xs.hi.end()),
               *std::max_element(ys.hi.begin(), ys.hi.end()));
  rect.Intersect(clip);
  *bitmap = nullptr;
  *left = rect.left;
  *top = rect.top;
  if (rect.IsEmpty()) {
    return;
  }
  RetainPtr<CFX_DIBitmap> src = source->Realize();
  // the colors of a one bit image, or of a gray one, which the device does
  // not composite but as BGR
  if (src && !src->IsMaskFormat() &&
      (src->GetBPP() == 1 || (src->GetBPP() == 8 && !src->HasPalette())) &&
      !src->ConvertFormat(FXDIB_Format::kBgr)) {
    return;
  }
  auto out = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!src || !out->Create(rect.Width(), rect.Height(), src->GetFormat())) {
    return;
  }
  if (src->HasPalette()) {
    out->SetPalette(src->GetPaletteSpan());
  }
  const std::vector<int> xmap = Coverage(xs, rect.left, rect.Width());
  const std::vector<int> ymap = Coverage(ys, rect.top, rect.Height());
  const int bpp = src->GetBPP();
  const int bytes = bpp / 8;
  for (int y = 0; y < rect.Height(); ++y) {
    if (ymap[y] < 0) {
      continue;
    }
    pdfium::span<uint8_t> dest = out->GetWritableScanline(y);
    for (int x = 0; x < rect.Width(); ++x) {
      if (xmap[x] < 0) {
        continue;
      }
      // the sample of source row `sy`, column `sx`
      const int sy = posture == image_portrait ? ymap[y] : xmap[x];
      const int sx = posture == image_portrait ? xmap[x] : ymap[y];
      pdfium::span<const uint8_t> scan = src->GetScanline(sy);
      if (bpp == 1) {
        if (scan[sx / 8] & (0x80 >> (sx % 8))) {
          dest[x / 8] |= 0x80 >> (x % 8);
        } else {
          dest[x / 8] &= ~(0x80 >> (x % 8));
        }
      } else {
        for (int k = 0; k < bytes; ++k) {
          dest[x * bytes + k] = scan[sx * bytes + k];
        }
      }
    }
  }
  *bitmap = std::move(out);
}

}  // namespace gs_image
