// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CJPX_Decoder's samples as gs reads them (base/sjpx_openjpeg.c),
// kept out of cjpx_decoder.cpp so upstream's changes to it merge cleanly.

#include <stdint.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "core/fxcodec/jpx/cjpx_decoder.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "core/fxge/calculate_pitch.h"

namespace fxcodec {

namespace {

pdfium::span<opj_image_comp_t> components_span(opj_image_t* img) {
  // SAFETY: required from third-party library.
  return UNSAFE_BUFFERS(pdfium::span(img->comps, img->numcomps));
}

}  // namespace

std::vector<uint32_t> CJPX_Decoder::GsComponents() const {
  // decode_image: the components not alpha (cdef), as many as the space
  // has but an unknown one's
  std::vector<uint32_t> primary;
  const auto components = components_span(image_.get());
  for (uint32_t i = 0; i < components.size(); ++i) {
    if (components[i].alpha == 0) {
      primary.push_back(i);
    }
  }
  size_t most = primary.size();
  switch (image_->color_space) {
    case OPJ_CLRSPC_GRAY:
      most = 1;
      break;
    case OPJ_CLRSPC_SRGB:
    case OPJ_CLRSPC_SYCC:
    case OPJ_CLRSPC_EYCC:
      most = 3;
      break;
    case OPJ_CLRSPC_CMYK:
      most = 4;
      break;
    default:
      break;
  }
  primary.resize(std::min(primary.size(), most));
  return primary;
}

bool CJPX_Decoder::DecodeGs(pdfium::span<uint8_t> dest_buf,
                            uint32_t pitch,
                            bool swap_rgb,
                            uint32_t channel_count) {
  const JpxImageInfo info = GetInfo();
  std::optional<uint32_t> calculated_pitch =
      fxge::CalculatePitch32(8 * channel_count, info.width);
  if (!calculated_pitch.has_value() || pitch < calculated_pitch.value()) {
    return false;
  }
  const pdfium::span<opj_image_comp_t> components =
      components_span(image_.get());
  // gs reads every component at the first's precision (sjpx_openjpeg.c
  // decode_image refuses others): 8 bits taken as they are, else scaled as
  // pdfium scales them
  for (const opj_image_comp_t& comp : components) {
    if (comp.prec != components[0].prec || !comp.data || !comp.dx ||
        !comp.dy) {
      return false;
    }
  }
  // components all alike read one to one, else each sample at its
  // component's dx, dy (gs's `samescale`)
  bool samescale = true;
  for (const opj_image_comp_t& comp : components) {
    samescale &= comp.dx == components[0].dx && comp.dy == components[0].dy;
  }
  // the components gs reads, then the others (an ARGB decode's alpha)
  std::vector<uint32_t> primary = GsComponents();
  for (uint32_t i = 0; i < components.size(); ++i) {
    if (std::ranges::find(primary, i) == primary.end()) {
      primary.push_back(i);
    }
  }
  if (channel_count == 3 && components.size() == 4) {
    channel_count = 4;
  }
  if (channel_count > primary.size() || (swap_rgb && channel_count < 3)) {
    return false;
  }
  if (swap_rgb) {
    std::swap(primary[0], primary[2]);
  }
  std::ranges::fill(dest_buf.first(info.height * pitch), 0xff);
  for (uint32_t channel = 0; channel < channel_count; ++channel) {
    const opj_image_comp_t& comp = components[primary[channel]];
    const uint32_t dx = samescale ? 1 : comp.dx;
    const uint32_t dy = samescale ? 1 : comp.dy;
    const int adjust = comp.prec - 8;
    const int offset = comp.sgnd ? 1 << (comp.prec - 1) : 0;
    for (uint32_t row = 0; row < info.height; ++row) {
      pdfium::span<uint8_t> scan = dest_buf.subspan(row * pitch);
      const uint32_t src_row = std::min(row / dy, comp.h - 1);
      for (uint32_t col = 0; col < info.width; ++col) {
        const uint32_t src_col = std::min(col / dx, comp.w - 1);
        const int src =
            UNSAFE_TODO(comp.data[src_row * comp.w + src_col]) + offset;
        int pixel = adjust < 0    ? src << -adjust
                    : adjust == 0 ? src
                                  : (src >> adjust) + ((src >> (adjust - 1)) % 2);
        scan[col * channel_count + channel] =
            static_cast<uint8_t>(std::clamp(pixel, 0, 255));
      }
    }
  }
  return true;
}

}  // namespace fxcodec
