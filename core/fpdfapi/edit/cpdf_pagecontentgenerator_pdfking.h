// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_PDFKING_H_
#define CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_PDFKING_H_

#include "core/fxcrt/fx_string_wrappers.h"

class CPDF_Color;

namespace pdfking {

// A color in its own space with its operator, upper case to stroke: a
// Separation's "/CS<objnum> cs <tint> scn" (the page's /ColorSpace names it
// so: PDFKING_PageObj_SetFillSeparation), CMYK's "c m y k k", gray's "g g".
// Returns whether it wrote to `buf`; RGB is left to pdfium's own writer.
bool WriteColorInItsSpace(fxcrt::ostringstream& buf,
                          const CPDF_Color* color,
                          bool stroke);

}  // namespace pdfking

#endif  // CORE_FPDFAPI_EDIT_CPDF_PAGECONTENTGENERATOR_PDFKING_H_
