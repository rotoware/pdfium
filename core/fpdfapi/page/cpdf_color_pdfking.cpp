// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: CPDF_Color's accessors for the fork's content generator colors
// (cpdf_pagecontentgenerator_pdfking.cpp), kept out of cpdf_color.cpp so
// upstream's changes to it merge cleanly.

#include <stdint.h>

#include <variant>
#include <vector>

#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fxcrt/span.h"

bool CPDF_Color::IsColorSpaceCMYK() const {
  return cs_ ==
         CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceCMYK);
}

uint32_t CPDF_Color::GetSeparationObjNum() const {
  if (!cs_ || cs_->GetFamily() != CPDF_ColorSpace::Family::kSeparation) {
    return 0;
  }
  const CPDF_Array* array = cs_->GetArray();
  return array ? array->GetObjNum() : 0;
}

pdfium::span<const float> CPDF_Color::GetComponents() const {
  if (std::holds_alternative<std::vector<float>>(color_data_)) {
    return pdfium::span<const float>(std::get<std::vector<float>>(color_data_));
  }
  return {};
}
