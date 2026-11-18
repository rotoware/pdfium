// pdfking: the nonseparable blend modes on the process plates, as gs blends
// them. A port of gs 10.08's gxblend.c art_blend_luminosity_rgb_8,
// art_blend_saturation_rgb_8, their cmyk wrappers, art_blend_pixel_8's
// dispatch and art_pdf_composite_pixel_alpha_8's mixing: byte for byte.

#include "core/fpdfapi/render/cpdf_inkblend.h"

namespace {

using Pixel = std::array<uint8_t, 4>;

// SetLum(src, Lum(backdrop)) on CMY, K from `src`
Pixel Luminosity(const Pixel& backdrop, const Pixel& src) {
  const int rb = backdrop[0], gb = backdrop[1], bb = backdrop[2];
  const int rs = src[0], gs = src[1], bs = src[2];
  const int delta_y =
      ((rs - rb) * 77 + (gs - gb) * 151 + (bs - bb) * 28 + 0x80) >> 8;
  int r = rb + delta_y;
  int g = gb + delta_y;
  int b = bb + delta_y;
  if ((r | g | b) & 0x100) {
    const int y = (rs * 77 + gs * 151 + bs * 28 + 0x80) >> 8;
    int scale;
    if (delta_y > 0) {
      int max = r > g ? r : g;
      max = b > max ? b : max;
      scale = ((255 - y) << 16) / (max - y);
    } else {
      int min = r < g ? r : g;
      min = b < min ? b : min;
      scale = (y << 16) / (y - min);
    }
    r = y + (((r - y) * scale + 0x8000) >> 16);
    g = y + (((g - y) * scale + 0x8000) >> 16);
    b = y + (((b - y) * scale + 0x8000) >> 16);
  }
  return {static_cast<uint8_t>(r), static_cast<uint8_t>(g),
          static_cast<uint8_t>(b), src[3]};
}

// SetLum(SetSat(backdrop, Sat(src)), Lum(backdrop)) on CMY, K from
// `backdrop`
Pixel Saturation(const Pixel& backdrop, const Pixel& src) {
  int rb = backdrop[0], gb = backdrop[1], bb = backdrop[2];
  const int rs = src[0], gs = src[1], bs = src[2];
  if (rb == gb && gb == bb) {
    return {static_cast<uint8_t>(gb), static_cast<uint8_t>(gb),
            static_cast<uint8_t>(gb), backdrop[3]};
  }
  const int lum_cb = (rb * 77 + gb * 151 + bb * 28 + 0x80) >> 8;
  int mins = rs < gs ? rs : gs;
  int maxs = rs < gs ? gs : rs;
  mins = mins < bs ? mins : bs;
  maxs = maxs < bs ? bs : maxs;
  const int sat_cs = maxs - mins;
  int minb = rb < gb ? rb : gb;
  int maxb = rb < gb ? gb : rb;
  minb = minb < bb ? minb : bb;
  maxb = maxb < bb ? bb : maxb;
  int scale = (sat_cs << 8) / (maxb - minb);
  rb = ((rb - minb) * scale + 0x80) >> 8;
  gb = ((gb - minb) * scale + 0x80) >> 8;
  bb = ((bb - minb) * scale + 0x80) >> 8;
  const int lum_c = (rb * 77 + gb * 151 + bb * 28 + 0x80) >> 8;
  const int d = lum_cb - lum_c;
  if (d < 0 || d + sat_cs > 255) {
    scale = d < 0 ? (lum_cb << 8) / lum_c
                  : ((255 - lum_cb) << 8) / (sat_cs - lum_c);
    rb = lum_cb + (((rb - lum_c) * scale + 0x80) >> 8);
    gb = lum_cb + (((gb - lum_c) * scale + 0x80) >> 8);
    bb = lum_cb + (((bb - lum_c) * scale + 0x80) >> 8);
  } else {
    rb += d;
    gb += d;
    bb += d;
  }
  return {static_cast<uint8_t>(rb), static_cast<uint8_t>(gb),
          static_cast<uint8_t>(bb), backdrop[3]};
}

}  // namespace

bool IsNonSeparable(BlendMode mode) {
  return mode == BlendMode::kHue || mode == BlendMode::kSaturation ||
         mode == BlendMode::kColor || mode == BlendMode::kLuminosity;
}

std::array<uint8_t, 4> InkBlend(BlendMode mode,
                                const std::array<uint8_t, 4>& backdrop,
                                const std::array<uint8_t, 4>& source) {
  switch (mode) {
    case BlendMode::kLuminosity:
      return Luminosity(backdrop, source);
    case BlendMode::kColor:
      return Luminosity(source, backdrop);
    case BlendMode::kSaturation:
      return Saturation(backdrop, source);
    case BlendMode::kHue:
      return Saturation(Luminosity(source, backdrop), backdrop);
    default:
      return source;
  }
}

uint8_t InkUnion(uint8_t backdrop_alpha, uint8_t source_alpha) {
  const int tmp = (0xff - backdrop_alpha) * (0xff - source_alpha) + 0x80;
  return static_cast<uint8_t>(0xff - (((tmp >> 8) + tmp) >> 8));
}

uint8_t InkComposite(uint8_t backdrop,
                     uint8_t backdrop_alpha,
                     uint8_t source,
                     uint8_t source_alpha,
                     uint8_t blend) {
  if (source_alpha == 0) {
    return backdrop;
  }
  if (backdrop_alpha == 0) {
    return source;
  }
  const int union_alpha = InkUnion(backdrop_alpha, source_alpha);
  const int src_scale =
      ((source_alpha << 16) + (union_alpha >> 1)) / union_alpha;
  int tmp = backdrop_alpha * (blend - static_cast<int>(source)) + 0x80;
  const int mix = source + (((tmp >> 8) + tmp) >> 8);
  tmp = (backdrop << 16) + src_scale * (mix - backdrop) + 0x8000;
  return static_cast<uint8_t>(tmp >> 16);
}
