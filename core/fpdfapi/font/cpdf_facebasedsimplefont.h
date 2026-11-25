// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_FONT_CPDF_FACEBASEDSIMPLEFONT_H_
#define CORE_FPDFAPI_FONT_CPDF_FACEBASEDSIMPLEFONT_H_

#include <stdint.h>

#include <array>

#include "core/fpdfapi/font/cpdf_fontencoding.h"
#include "core/fpdfapi/font/cpdf_simplefont.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Document;
class CPDF_Dictionary;

// Base class for simple fonts backed by a font face (e.g. Type1, TrueType).
// Note that Type3 fonts are the only non-face based simple font.
class CPDF_FaceBasedSimpleFont : public CPDF_SimpleFont {
 public:
  // pdfking: no /Widths: the widths are the font program's
  bool UsesFontWidths() const { return use_font_width_; }
  // pdfking: the /Widths (or /MissingWidth) width as written, fractions kept
  float GetCharWidthF(uint32_t charcode);
  ~CPDF_FaceBasedSimpleFont() override;

  // CPDF_Font:
  int GetCharWidth(uint32_t charcode) override;
  FX_RECT GetCharBBox(uint32_t charcode) override;
  int GlyphFromCharCode(uint32_t charcode, bool* pVertGlyph) override;
  bool HasFontWidths() const override;

 protected:
  CPDF_FaceBasedSimpleFont(CPDF_Document* document,
                           RetainPtr<CPDF_Dictionary> font_dict);

  virtual void LoadGlyphMap() = 0;

  bool LoadCommon();
  void LoadSubstFont();
  void LoadCharMetrics(int charcode);
  void LoadCharWidths(const CPDF_Dictionary* font_desc);

  bool use_font_width_ = false;
  std::array<uint16_t, kInternalTableSize> glyph_index_;
  std::array<uint16_t, kInternalTableSize> char_width_;
  std::array<float, kInternalTableSize> char_width_f_;  // pdfking; -1: none
  std::array<FX_RECT, kInternalTableSize> char_bbox_;
};

#endif  // CORE_FPDFAPI_FONT_CPDF_FACEBASEDSIMPLEFONT_H_
