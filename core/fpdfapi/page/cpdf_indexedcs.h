// Copyright 2022 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_PAGE_CPDF_INDEXEDCS_H_
#define CORE_FPDFAPI_PAGE_CPDF_INDEXEDCS_H_

#include <stdint.h>

#include <optional>
#include <set>

#include "core/fpdfapi/page/cpdf_basedcs.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Document;

struct IndexedColorMinMax {
  static constexpr bool kIsDataPartitionSafe = true;

  float min;
  float max;
};

class CPDF_IndexedCS final : public CPDF_BasedCS {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;
  ~CPDF_IndexedCS() override;

  // CPDF_ColorSpace:
  std::optional<FX_RGB_STRUCT<float>> GetRGB(
      pdfium::span<const float> pBuf) const override;
  const CPDF_IndexedCS* AsIndexedCS() const override;
  uint32_t v_Load(CPDF_Document* doc,
                  const CPDF_Array* pArray,
                  std::set<const CPDF_Object*>* pVisited) override;

  int GetMaxIndex() const { return max_index_; }

  // pdfking: the base color space, and the base components entry `pBuf[0]`
  // of the lookup table is, nothing for an index out of the table.
  const CPDF_ColorSpace* GetBaseCS() const { return base_cs_.Get(); }
  std::optional<DataVector<float>> GetBaseComponents(
      pdfium::span<const float> pBuf) const;

 private:
  CPDF_IndexedCS();

  int max_index_ = 0;
  DataVector<uint8_t> lookup_table_;
  DataVector<IndexedColorMinMax> component_min_max_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_INDEXEDCS_H_
