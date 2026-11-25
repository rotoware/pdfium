// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FXGE_CFX_GSFONTS_H_
#define CORE_FXGE_CFX_GSFONTS_H_

#include <stdint.h>

#include <map>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/cfx_face.h"

// pdfking: fonts not embedded found as gs finds them (pdfi_load_font_file)
// in gs's resources (its share/ghostscript: Resource/Init/Fontmap.GS and
// cidfmap, Resource/Font, Resource/CIDFSubst).
class CFX_GsFonts {
 public:
  CFX_GsFonts();
  ~CFX_GsFonts();

  // gs's resources at `dir`; empty for none (pdfium's own substitution).
  void SetDir(const ByteString& dir);
  bool HasDir() const { return !dir_.IsEmpty(); }

  // The font gs loads for `name` with descriptor `flags` (kFontCidSubst for
  // a CIDFont), null for none.
  RetainPtr<CFX_Face> Face(const ByteString& name, uint32_t flags);

 private:
  // The font file Fontmap.GS (or a file of the name) gives `name`, empty
  // for none
  ByteString FontFile(const ByteString& name) const;
  // a font file, relative to gs's Resource directory or absolute, loaded
  RetainPtr<CFX_Face> LoadFace(const ByteString& file);

  ByteString dir_;
  std::map<ByteString, ByteString> fontmap_;
  std::map<ByteString, ByteString> cidfmap_;
  std::map<ByteString, RetainPtr<CFX_Face>> faces_;
};

#endif  // CORE_FXGE_CFX_GSFONTS_H_
