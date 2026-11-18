// pdfking: the nonseparable blend modes on the process plates, as gs blends
// them (gxblend.c, 8-bit).

#ifndef CORE_FPDFAPI_RENDER_CPDF_INKBLEND_H_
#define CORE_FPDFAPI_RENDER_CPDF_INKBLEND_H_

#include <stdint.h>

#include <array>

#include "core/fxge/dib/fx_dib.h"

// Hue, Saturation, Color or Luminosity
bool IsNonSeparable(BlendMode mode);

// B(cb, cs) of `mode` (nonseparable) on the four process plates, each as its
// gray (255 - ink, gs's complemented values): C, M, Y as R, G, B
// (ISO 32000-2 11.3.5.3); K the source's for Luminosity, else the backdrop's.
std::array<uint8_t, 4> InkBlend(BlendMode mode,
                                const std::array<uint8_t, 4>& backdrop,
                                const std::array<uint8_t, 4>& source);

// The opacity of `source_alpha` over `backdrop_alpha`, as gs unites them
uint8_t InkUnion(uint8_t backdrop_alpha, uint8_t source_alpha);

// A channel of `source` (opacity `source_alpha`, blend result `blend`)
// composited on `backdrop` (opacity `backdrop_alpha`), as gs does
// (art_pdf_composite_pixel_alpha_8); its opacity is InkUnion()'s.
uint8_t InkComposite(uint8_t backdrop,
                     uint8_t backdrop_alpha,
                     uint8_t source,
                     uint8_t source_alpha,
                     uint8_t blend);

#endif  // CORE_FPDFAPI_RENDER_CPDF_INKBLEND_H_
