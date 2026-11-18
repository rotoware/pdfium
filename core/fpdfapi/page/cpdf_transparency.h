// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_PAGE_CPDF_TRANSPARENCY_H_
#define CORE_FPDFAPI_PAGE_CPDF_TRANSPARENCY_H_

class CPDF_Transparency {
 public:
  CPDF_Transparency();

  CPDF_Transparency(const CPDF_Transparency& other);
  CPDF_Transparency& operator=(const CPDF_Transparency& other);

  bool IsGroup() const { return group_; }
  bool IsIsolated() const { return isolated_; }
  // pdfking: /K, each object painted on the group's backdrop alone
  bool IsKnockout() const { return knockout_; }

  void SetGroup() { group_ = true; }
  void SetIsolated() { isolated_ = true; }
  void SetKnockout() { knockout_ = true; }

 private:
  bool group_ = false;
  bool isolated_ = false;
  bool knockout_ = false;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_TRANSPARENCY_H_
