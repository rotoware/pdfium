// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: fonts not embedded found as gs finds them (pdfi_load_font_file),
// in gs's resources. Owned by CFX_FontMapper.

#include "core/fxge/cfx_gsfonts.h"

#include <stdint.h>

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "core/fxcrt/cfx_fileaccess_stream.h"
#include "core/fxcrt/cfx_read_only_container_stream.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fixed_size_data_vector.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxge/fx_font.h"

CFX_GsFonts::CFX_GsFonts() = default;

CFX_GsFonts::~CFX_GsFonts() = default;

namespace {

// pdfi's base font names and the names it takes for each (pdf_font.c)
constexpr std::array<std::array<const char*, 9>, 14> kGsBaseFontNames = {{
    {"Courier", "CourierNew", "CourierNewPSMT", "CourierStd"},
    {"Courier-Bold", "CourierNew,Bold", "Courier,Bold", "CourierNewPS-BoldMT",
     "CourierNew-Bold"},
    {"Courier-Oblique", "CourierNew,Italic", "Courier,Italic",
     "CourierNewPS-ItalicMT", "CourierNew-Italic"},
    {"Courier-BoldOblique", "CourierNew,BoldItalic", "Courier,BoldItalic",
     "CourierNewPS-BoldItalicMT", "CourierNew-BoldItalic"},
    {"Helvetica", "ArialMT", "Arial"},
    {"Helvetica-Bold", "Arial-BoldMT", "Arial,Bold", "Arial-Bold",
     "Helvetica,Bold"},
    {"Helvetica-Oblique", "Arial-ItalicMT", "Arial,Italic", "Arial-Italic",
     "Helvetica,Italic", "Helvetica-Italic"},
    {"Helvetica-BoldOblique", "Arial-BoldItalicMT", "Arial,BoldItalic",
     "Arial-BoldItalic", "Helvetica,BoldItalic", "Helvetica-BoldItalic"},
    {"Times-Roman", "TimesNewRomanPSMT", "TimesNewRoman", "TimesNewRomanPS"},
    {"Times-Bold", "TimesNewRomanPS-BoldMT", "TimesNewRoman,Bold",
     "TimesNewRomanPS-Bold", "TimesNewRoman-Bold"},
    {"Times-Italic", "TimesNewRomanPS-ItalicMT", "TimesNewRoman,Italic",
     "TimesNewRomanPS-Italic", "TimesNewRoman-Italic"},
    {"Times-BoldItalic", "TimesNewRomanPS-BoldItalicMT",
     "TimesNewRoman,BoldItalic", "TimesNewRomanPS-BoldItalic",
     "TimesNewRoman-BoldItalic"},
    {"Symbol", "Symbol,Italic", "Symbol,Bold", "Symbol,BoldItalic", "SymbolMT",
     "SymbolMT,Italic", "SymbolMT,Bold", "SymbolMT,BoldItalic"},
    {"ZapfDingbats"},
}};

// `a` and `b` alike but for spaces (strncmp_ignore_space)
bool SameIgnoringSpaces(ByteStringView a, ByteStringView b) {
  size_t i = 0;
  size_t j = 0;
  while (true) {
    while (i < a.GetLength() && a[i] == ' ') {
      ++i;
    }
    while (j < b.GetLength() && b[j] == ' ') {
      ++j;
    }
    if (i == a.GetLength() || j == b.GetLength()) {
      return i == a.GetLength() && j == b.GetLength();
    }
    if (a[i] != b[j]) {
      return false;
    }
    ++i;
    ++j;
  }
}

// pdfi_clean_font_name
ByteString GsCleanName(const ByteString& name) {
  for (const auto& names : kGsBaseFontNames) {
    for (const char* alias : names) {
      if (alias && SameIgnoringSpaces(alias, name.AsStringView())) {
        return names[0];
      }
    }
  }
  return ByteString();
}

// pdfi_font_substitute_by_flags
ByteString GsNameByFlags(uint32_t flags) {
  const bool fixed = flags & 0x1;
  const bool serif = flags & 0x2;
  const bool italic = flags & 0x40;
  const bool bold = flags & 0x40000;
  const int base = fixed ? 0 : serif ? 8 : 4;
  const int style = bold ? (italic ? 3 : 1) : (italic ? 2 : 0);
  return kGsBaseFontNames[base + style][0];
}

}  // namespace

void CFX_GsFonts::SetDir(const ByteString& dir) {
  dir_ = dir;
  fontmap_.clear();
  faces_.clear();
  cidfmap_.clear();
  // cidfmap: "/Name << /FileType /TrueType /Path (file) ... >> ;"
  if (RetainPtr<IFX_SeekableReadStream> cidfmap =
          CFX_FileAccessStream::CreateFromFilename(
              (dir + "/Resource/Init/cidfmap").c_str())) {
    DataVector<uint8_t> data(
        pdfium::checked_cast<size_t>(cidfmap->GetSize()));
    if (cidfmap->ReadBlockAtOffset(data, 0)) {
      const ByteString text = ByteString(ByteStringView(data));
      for (const ByteString& line : fxcrt::Split(text, '\n')) {
        ByteString l = line;
        l.TrimWhitespace();
        if (l.IsEmpty() || l[0] != '/') {
          continue;
        }
        std::optional<size_t> space = l.Find(' ');
        std::optional<size_t> path = l.Find("/Path");
        if (!space.has_value() || !path.has_value()) {
          continue;
        }
        std::optional<size_t> open = l.Find('(', path.value());
        std::optional<size_t> close =
            open.has_value() ? l.Find(')', open.value()) : std::nullopt;
        if (close.has_value()) {
          cidfmap_[l.Substr(1, space.value() - 1)] =
              l.Substr(open.value() + 1, close.value() - open.value() - 1);
        }
      }
    }
  }
  RetainPtr<IFX_SeekableReadStream> file =
      CFX_FileAccessStream::CreateFromFilename(
          (dir + "/Resource/Init/Fontmap.GS").c_str());
  if (!file) {
    return;
  }
  DataVector<uint8_t> data(pdfium::checked_cast<size_t>(file->GetSize()));
  if (!file->ReadBlockAtOffset(data, 0)) {
    return;
  }
  // lines "/Alias /Name ;" or "/Name (file) ;", % comments
  const ByteString text = ByteString(ByteStringView(data));
  for (const ByteString& line : fxcrt::Split(text, '\n')) {
    std::vector<ByteString> words;
    ByteString word;
    for (char c : line) {
      if (c == '%') {
        break;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == ';') {
        if (!word.IsEmpty()) {
          words.push_back(word);
          word.clear();
        }
        continue;
      }
      word += c;
    }
    if (!word.IsEmpty()) {
      words.push_back(word);
    }
    if (words.size() >= 2 && words[0].GetLength() > 1 && words[0][0] == '/') {
      fontmap_[words[0].Substr(1)] = words[1];
    }
  }
}

ByteString CFX_GsFonts::FontFile(const ByteString& name) const {
  ByteString n = name;
  for (int i = 0; i < 16; ++i) {
    auto it = fontmap_.find(n);
    if (it == fontmap_.end()) {
      break;
    }
    const ByteString& target = it->second;
    if (target.GetLength() > 1 && target[0] == '/') {
      n = target.Substr(1);
      continue;
    }
    if (target.GetLength() > 2 && target[0] == '(') {
      return target.Substr(1, target.GetLength() - 2);
    }
    break;
  }
  ByteString path = dir_ + "/Resource/Font/" + n;
  return CFX_FileAccessStream::CreateFromFilename(path.c_str()) ? n
                                                                  : ByteString();
}

RetainPtr<CFX_Face> CFX_GsFonts::Face(const ByteString& name,
                                           uint32_t flags) {
  // a CIDFont: cidfmap (by name, CIDFallBack), else gs's fallback
  // (pdfi_open_CIDFont_substitute_file); no Registry-Ordering lookup: the
  // shipped cidfmap has none
  if (flags & kFontCidSubst) {
    ByteString file;
    for (const ByteString& key : {name, ByteString("CIDFallBack")}) {
      auto it = cidfmap_.find(key);
      if (it != cidfmap_.end()) {
        file = it->second;
        break;
      }
    }
    if (file.IsEmpty()) {
      file = "CIDFSubst/DroidSansFallback.ttf";
    }
    return LoadFace(file);
  }
  // the name, then pdfi's base name for it, then one by the flags
  ByteString file = FontFile(name);
  if (file.IsEmpty()) {
    ByteString clean = GsCleanName(name);
    if (!clean.IsEmpty()) {
      file = FontFile(clean);
    }
  }
  if (file.IsEmpty()) {
    file = FontFile(GsNameByFlags(flags));
  }
  if (file.IsEmpty()) {
    return nullptr;
  }
  return LoadFace("Font/" + file);
}

RetainPtr<CFX_Face> CFX_GsFonts::LoadFace(const ByteString& file) {
  RetainPtr<CFX_Face>& face = faces_[file];
  if (!face) {
    RetainPtr<IFX_SeekableReadStream> stream =
        CFX_FileAccessStream::CreateFromFilename(
            (file[0] == '/' ? file : dir_ + "/Resource/" + file).c_str());
    if (!stream) {
      return nullptr;
    }
    auto data = FixedSizeDataVector<uint8_t>::Uninit(
        pdfium::checked_cast<size_t>(stream->GetSize()));
    if (!stream->ReadBlockAtOffset(data.span(), 0)) {
      return nullptr;
    }
    face = CFX_Face::New(
        nullptr,
        pdfium::MakeRetain<CFX_ReadOnlyFixedSizeDataVectorStream>(
            std::move(data)),
        0);
  }
  return face;
}
