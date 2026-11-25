// Copyright 2020 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FXGE_CFX_TEXTRENDEROPTIONS_H_
#define CORE_FXGE_CFX_TEXTRENDEROPTIONS_H_

#include "core/fxcrt/fx_coordinates.h"

struct CFX_TextRenderOptions {
  // AliasingType defines the options for drawing pixels on the edges of the
  // text. The values are defined in an incrementing order due to the latter
  // aliasing type's dependency on the previous one.
  enum AliasingType {
    // No transparent pixels on glyph edges.
    kAliasing,

    // May have transparent pixels on glyph edges.
    kAntiAliasing,

    // LCD optimization, can be enabled when anti-aliasing is allowed.
    kLcd,
  };

  constexpr CFX_TextRenderOptions() = default;
  constexpr explicit CFX_TextRenderOptions(AliasingType type)
      : aliasing_type(type) {}
  constexpr CFX_TextRenderOptions(const CFX_TextRenderOptions& other) = default;
  CFX_TextRenderOptions& operator=(const CFX_TextRenderOptions& other) =
      default;

  friend constexpr bool operator==(const CFX_TextRenderOptions&,
                                   const CFX_TextRenderOptions&) = default;

  // Indicates whether anti-aliasing is enabled.
  bool IsSmooth() const {
    return aliasing_type == kAntiAliasing || aliasing_type == kLcd;
  }

  // Aliasing option for fonts.
  AliasingType aliasing_type = kAntiAliasing;

  // Font is CID font.
  bool font_is_cid = false;

  // Using the native text output available on some platforms.
  bool native_text = true;

  // pdfking: aliased glyphs as gs renders them at this device resolution
  // (its size in points through FreeType, fapi_ft.c); 0: pdfium's own
  float gs_dpi = 0;
  // pdfking: glyphs filled as outlines, as gs draws text on a page with
  // transparency (its pdf14 device)
  bool gs_outline = false;

  // pdfking: where the page's top left is on the device (a band's render is
  // offset): gs places an uncached glyph by its page coordinates
  CFX_PointF gs_page_origin;
  // pdfking: the glyph origins are gs's current points already (the caller
  // followed gs's advances); otherwise each advance is taken as gs takes a
  // glyph's width
  bool gs_positioned = false;
};

#endif  // CORE_FXGE_CFX_TEXTRENDEROPTIONS_H_
