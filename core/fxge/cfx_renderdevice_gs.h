// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FXGE_CFX_RENDERDEVICE_GS_H_
#define CORE_FXGE_CFX_RENDERDEVICE_GS_H_

// pdfking: glyphs as gs places and draws them, for CFX_RenderDevice
// (cfx_renderdevice_gs.cpp).

#include <stdint.h>

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

class CFX_DIBitmap;
class CFX_Face;
struct CFX_FillRenderOptions;
class CFX_Font;
class CFX_GlyphBitmap;
class CFX_GraphStateData;
class CFX_Path;
class CFX_RenderDevice;
struct TextCharPos;

// The pixel gs puts a glyph's origin `v` on.
int GsPixel(double v);

// Whether gs caches the glyph `bitmap` of `face` at `matrix`.
bool GsCachesGlyph(const CFX_Face& face,
                   const CFX_Matrix& matrix,
                   const CFX_GlyphBitmap& bitmap);

// `path` filled nonzero into `mask` (over `rect`), the pixels whose centers
// are inside, as gs fills a glyph's outline.
void GsCenterFill(const CFX_Path& path,
                  const FX_RECT& rect,
                  RetainPtr<CFX_DIBitmap> mask);

// CFX_RenderDevice::DrawTextPath() as gs's charpath.
bool GsDrawTextPath(CFX_RenderDevice* device,
                    pdfium::span<const TextCharPos> pCharPos,
                    CFX_Font* font,
                    float font_size,
                    const CFX_Matrix& mtText2User,
                    const CFX_Matrix& user2device,
                    const CFX_GraphStateData* pGraphState,
                    uint32_t fill_color,
                    uint32_t stroke_color,
                    CFX_Path* pClippingPath,
                    const CFX_FillRenderOptions& fill_options,
                    float gs_dpi);

#endif  // CORE_FXGE_CFX_RENDERDEVICE_GS_H_
