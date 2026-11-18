// Copyright 2023 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fxge/dib/blend.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/fxcrt/check_op.h"
#include "core/fxcrt/notreached.h"
#include "core/fxge/dib/fx_dib.h"

namespace fxge {

namespace {

// pdfking: gs's SoftLight (gxblend.c): floor((x - x * x / 255.0) * 65536 /
// 255 + .5), and SoftLight(x, 255) - x sampled from Illustrator 9
constexpr uint32_t kSquareDifference[256] = {
    0, 256, 510, 762, 1012, 1260, 1506, 1750, 1992, 2231, 2469, 2705,
    2939, 3171, 3401, 3628, 3854, 4078, 4300, 4519, 4737, 4953, 5166,
    5378, 5588, 5795, 6001, 6204, 6406, 6606, 6803, 6999, 7192, 7384,
    7573, 7761, 7946, 8129, 8311, 8490, 8668, 8843, 9016, 9188, 9357,
    9524, 9690, 9853, 10014, 10173, 10331, 10486, 10639, 10790, 10939,
    11086, 11232, 11375, 11516, 11655, 11792, 11927, 12060, 12191, 12320,
    12447, 12572, 12695, 12816, 12935, 13052, 13167, 13280, 13390, 13499,
    13606, 13711, 13814, 13914, 14013, 14110, 14205, 14297, 14388, 14477,
    14564, 14648, 14731, 14811, 14890, 14967, 15041, 15114, 15184, 15253,
    15319, 15384, 15446, 15507, 15565, 15622, 15676, 15729, 15779, 15827,
    15874, 15918, 15960, 16001, 16039, 16075, 16110, 16142, 16172, 16200,
    16227, 16251, 16273, 16293, 16311, 16327, 16341, 16354, 16364, 16372,
    16378, 16382, 16384, 16384, 16382, 16378, 16372, 16364, 16354, 16341,
    16327, 16311, 16293, 16273, 16251, 16227, 16200, 16172, 16142, 16110,
    16075, 16039, 16001, 15960, 15918, 15874, 15827, 15779, 15729, 15676,
    15622, 15565, 15507, 15446, 15384, 15319, 15253, 15184, 15114, 15041,
    14967, 14890, 14811, 14731, 14648, 14564, 14477, 14388, 14297, 14205,
    14110, 14013, 13914, 13814, 13711, 13606, 13499, 13390, 13280, 13167,
    13052, 12935, 12816, 12695, 12572, 12447, 12320, 12191, 12060, 11927,
    11792, 11655, 11516, 11375, 11232, 11086, 10939, 10790, 10639, 10486,
    10331, 10173, 10014, 9853, 9690, 9524, 9357, 9188, 9016, 8843, 8668,
    8490, 8311, 8129, 7946, 7761, 7573, 7384, 7192, 6999, 6803, 6606,
    6406, 6204, 6001, 5795, 5588, 5378, 5166, 4953, 4737, 4519, 4300,
    4078, 3854, 3628, 3401, 3171, 2939, 2705, 2469, 2231, 1992, 1750,
    1506, 1260, 1012, 762, 510, 256, 0
};
constexpr uint8_t kSoftLight[256] = {
    0, 3, 6, 9, 11, 14, 16, 19, 21, 23, 26, 28, 30, 32, 33, 35, 37, 39,
    40, 42, 43, 45, 46, 47, 48, 49, 51, 52, 53, 53, 54, 55, 56, 57, 57,
    58, 58, 59, 60, 60, 60, 61, 61, 62, 62, 62, 62, 63, 63, 63, 63, 63,
    63, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 62, 62, 62,
    62, 62, 62, 62, 61, 61, 61, 61, 61, 61, 60, 60, 60, 60, 60, 59, 59,
    59, 59, 59, 58, 58, 58, 58, 57, 57, 57, 57, 56, 56, 56, 56, 55, 55,
    55, 55, 54, 54, 54, 54, 53, 53, 53, 52, 52, 52, 51, 51, 51, 51, 50,
    50, 50, 49, 49, 49, 48, 48, 48, 47, 47, 47, 46, 46, 46, 45, 45, 45,
    44, 44, 43, 43, 43, 42, 42, 42, 41, 41, 40, 40, 40, 39, 39, 39, 38,
    38, 37, 37, 37, 36, 36, 35, 35, 35, 34, 34, 33, 33, 33, 32, 32, 31,
    31, 31, 30, 30, 29, 29, 28, 28, 28, 27, 27, 26, 26, 25, 25, 25, 24,
    24, 23, 23, 22, 22, 21, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16,
    16, 15, 15, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7,
    7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0
};

}  // namespace

int Blend(BlendMode blend_mode, int back_color, int src_color) {
  // Intentionally using DCHECKs, as Blend() is oftentimes called 3 times per
  // pixel.
  DCHECK_GE(back_color, 0);
  DCHECK_LE(back_color, 255);
  DCHECK_GE(src_color, 0);
  DCHECK_LE(src_color, 255);

  switch (blend_mode) {
    case BlendMode::kNormal:
      return src_color;
    case BlendMode::kMultiply:
      return (src_color * back_color + 127) / 255;  // pdfking: rounded
    case BlendMode::kScreen:
      return src_color + back_color - (src_color * back_color + 127) / 255;
    case BlendMode::kOverlay:
      return Blend(BlendMode::kHardLight, src_color, back_color);
    case BlendMode::kDarken:
      return std::min(src_color, back_color);
    case BlendMode::kLighten:
      return std::max(src_color, back_color);
    case BlendMode::kColorDodge: {
      // pdfking: as gs, rounded
      const int s = 255 - src_color;
      if (back_color == 0) {
        return 0;
      }
      if (back_color >= s) {
        return 255;
      }
      return (0x1fe * back_color + s) / (s << 1);
    }
    case BlendMode::kColorBurn: {
      if (src_color == 0) {
        return 0;
      }
      return 255 - std::min((255 - back_color) * 255 / src_color, 255);
    }
    case BlendMode::kHardLight:
      if (src_color < 128) {
        return (src_color * back_color * 2 + 127) / 255;
      }
      return Blend(BlendMode::kScreen, back_color, 2 * src_color - 255);
    case BlendMode::kSoftLight: {
      // pdfking: as gs blends it
      if (src_color < 0x80) {
        const uint32_t t =
            (0xff - (src_color << 1)) * kSquareDifference[back_color] + 0x8000;
        return back_color - static_cast<int>(t >> 16);
      }
      uint32_t t = ((src_color << 1) - 0xff) * uint32_t{kSoftLight[back_color]};
      t += 0x80;
      t += t >> 8;
      return back_color + static_cast<int>(t >> 8);
    }
    case BlendMode::kDifference:
      return std::abs(back_color - src_color);
    case BlendMode::kExclusion:
      return back_color + src_color - (2 * back_color * src_color + 127) / 255;
    default:
      // This function does not handle non-separable blend modes.
      NOTREACHED();
  }
}

}  // namespace fxge
