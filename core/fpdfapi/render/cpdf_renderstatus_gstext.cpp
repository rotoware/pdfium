// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CPDF_RenderStatus's text placed as gs places it, kept out of
// cpdf_renderstatus.cpp so upstream's changes to it merge cleanly.

#include <math.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fpdfapi/font/cpdf_facebasedsimplefont.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_truetypefont.h"
#include "core/fpdfapi/font/cpdf_type1font.h"
#include "core/fpdfapi/font/cpdf_type3font.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"
#include "core/fxge/cfx_face.h"
#include "core/fxge/cfx_font.h"

// pdfking: the text's glyph origins where gs puts them (gxchar.c
// show_move, pdfi_Tj / pdfi_TJ): the current point starts at the text
// matrix's origin, or where the last show left it when no Tm / Td / T* came
// in between (pdfi carries gs's current point into the text matrix); each
// glyph's width, with its character and word spacing, moves it by that width
// in device 1/256 pixels, truncated (gs_distance_transform2fixed); a TJ kern
// moves it exactly (gs_rmoveto). Positions in text space, as the text's.
std::vector<float> CPDF_RenderStatus::GsTextPositions(
    const CPDF_TextObject* textobj,
    const CFX_Matrix& text2device) {
  const bool vertical = textobj->GetFont()->IsVertWriting();
  auto vec = [vertical](float v) {
    return vertical ? CFX_PointF(0, v) : CFX_PointF(v, 0);
  };
  const CFX_PointF start = text2device.Transform(CFX_PointF());
  double x = start.x;
  double y = start.y;
  // a show right after the last: from where gs's current point was left
  // (plus a TJ's leading kern)
  if (gs_text_end_ && textobj->gs_continues()) {
    x = gs_text_end_->gs.x + (start.x - gs_text_end_->exact.x);
    y = gs_text_end_->gs.y + (start.y - gs_text_end_->exact.y);
  }
  RetainPtr<CPDF_Font> font = textobj->GetFont();
  CPDF_CIDFont* cid_font = font->AsCIDFont();
  // a simple font with no /Widths: gs takes each width from the font program
  // (TEXT_RETURN_WIDTH), and the spacing apart
  CPDF_FaceBasedSimpleFont* simple =
      font->IsType1Font()
          ? static_cast<CPDF_FaceBasedSimpleFont*>(font->AsType1Font())
          : font->AsTrueTypeFont();
  CFX_Face* face = font->GetFont()->GetFace();
  const bool font_widths = simple && simple->UsesFontWidths() && face;
  const std::vector<uint32_t>& codes = textobj->GetCharCodes();
  const std::vector<float>& kernings = textobj->GetCharKernings();
  const float font_size = textobj->GetFontSize();
  const float char_space = textobj->text_state().GetCharSpace();
  const float word_space = textobj->text_state().GetWordSpace();
  const CFX_Matrix device2text = text2device.GetInverse();
  // gs's float matrices: Trm x CTM (the font size in it) and the char matrix
  // (FontMatrix, 0.001, times that)
  constexpr float kFontMatrix = 0.001f;
  const float ctm_x = static_cast<float>(
      font_size * static_cast<double>(vertical ? text2device.c : text2device.a));
  const float ctm_y = static_cast<float>(
      font_size * static_cast<double>(vertical ? text2device.d : text2device.b));
  const float char_x = static_cast<float>(kFontMatrix * static_cast<double>(ctm_x));
  const float char_y = static_cast<float>(kFontMatrix * static_cast<double>(ctm_y));
  // gs_distance_transform2fixed: each product truncated to 1/256
  auto fixed = [](double v) { return trunc(v * 256.0) / 256.0; };
  auto move_fixed = [&](double v, float mx, float my) {
    x += fixed(v * mx);
    y += fixed(v * my);
  };
  const double spacing_scale = 1.0 / font_size;
  std::vector<float> positions(codes.size());
  for (size_t i = 0; i < codes.size(); ++i) {
    const CFX_PointF here = device2text.Transform(
        CFX_PointF(static_cast<float>(x), static_cast<float>(y)));
    positions[i] = vertical ? here.y : here.x;
    const uint32_t code = codes[i];
    const double space =
        code == ' ' && (!cid_font || cid_font->GetCharSize(' ') == 1)
            ? word_space * spacing_scale
            : 0;
    const double spacing = char_space * spacing_scale;
    if (cid_font) {
      // pdfi_fapi_set_cache: W / 1000 (a float) through the char matrix,
      // then the spacing
      const int width = vertical
                            ? cid_font->GetVertWidth(cid_font->CIDFromCharCode(code))
                            : font->GetCharWidth(code);
      const double units =
          vertical ? width
                   : cid_font->GetCharWidthF(code);
      move_fixed(static_cast<float>(units / 1000.0), ctm_x, ctm_y);
      if (spacing + space != 0) {
        move_fixed(spacing + space, ctm_x, ctm_y);
      }
    } else if (font_widths) {
      // the font program's width (FAPI), then the spacing apart
      bool vertical_glyph = false;
      const int glyph = font->GlyphFromCharCode(code, &vertical_glyph);
      double advance =
          glyph >= 0 ? face->GsAdvance(static_cast<uint32_t>(glyph)) * 1000 : 0;
      if (advance <= 0) {
        advance = font->GetCharWidth(code);
      }
      move_fixed(advance, char_x, char_y);
      if (spacing + space != 0) {
        move_fixed(spacing + space, ctm_x, ctm_y);
      }
    } else {
      // pdfi's x_widths: the width through the FontMatrix plus Tc, a float;
      // show_move adds the word spacing and moves by it through Trm x CTM
      // (pdfi scales /Widths by 0.001 / FontMatrix as it loads them)
      // (a Type 3's /Widths through its own FontMatrix)
      CPDF_Type3Font* type3 = font->AsType3Font();
      const double units =
          simple ? simple->GetCharWidthF(code) : font->GetCharWidth(code);
      float w = type3 ? type3->GetGsWidth(code)
                      : static_cast<float>(units * 0.001);
      w = static_cast<float>(w + spacing);
      move_fixed(w + space, ctm_x, ctm_y);
    }
    // a TJ kern: gs_rmoveto, exact
    const double kern = -kernings[i] / 1000.0;
    x += kern * ctm_x;
    y += kern * ctm_y;
  }
  gs_text_end_ = GsTextEnd{
      text2device.Transform(vec(textobj->GetAdvance())),
      CFX_PointF(static_cast<float>(x), static_cast<float>(y))};
  return positions;
}
