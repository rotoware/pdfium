// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: the page content generator's colors in their own spaces, kept out
// of cpdf_pagecontentgenerator.cpp so upstream's changes to it merge cleanly.

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator_pdfking.h"

#include <stddef.h>
#include <stdint.h>

#include <sstream>

#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stream_write_utils.h"

namespace pdfking {

bool WriteColorInItsSpace(fxcrt::ostringstream& buf,
                          const CPDF_Color* color,
                          bool stroke) {
  if (!color) {
    return false;
  }
  pdfium::span<const float> components = color->GetComponents();
  if (uint32_t objnum = color->GetSeparationObjNum()) {
    if (components.empty()) {
      return false;
    }
    buf << "/CS" << objnum << (stroke ? " CS " : " cs ");
    WriteFloat(buf, components[0]) << (stroke ? " SCN " : " scn ");
    return true;
  }
  if (color->IsColorSpaceCMYK()) {
    if (components.size() < 4) {
      return false;
    }
    for (size_t i = 0; i < 4; ++i) {
      WriteFloat(buf, components[i]) << (i < 3 ? " " : "");
    }
    buf << (stroke ? " K " : " k ");
    return true;
  }
  if (color->IsColorSpaceGray()) {
    if (components.empty()) {
      return false;
    }
    WriteFloat(buf, components[0]) << (stroke ? " G " : " g ");
    return true;
  }
  return false;
}

}  // namespace pdfking
