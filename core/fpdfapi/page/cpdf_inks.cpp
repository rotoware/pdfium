// pdfking: the separations a page renders to at once.

#include "core/fpdfapi/page/cpdf_inks.h"

#include <algorithm>
#include <set>
#include <utility>

#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"

namespace {

// A handle: opacity, this tag, a 16-bit index
constexpr uint32_t kTag = 0xA5;
constexpr uint32_t kMaxColors = 0x10000;

}  // namespace

namespace {

// Every colorant name in `obj` and what it holds (not a page's parent)
void FindSpots(const CPDF_Object* obj,
               std::set<const CPDF_Object*>* seen,
               std::vector<ByteString>* spots) {
  if (!obj || !seen->insert(obj).second) {
    return;
  }
  if (const CPDF_Reference* ref = obj->AsReference()) {
    FindSpots(ref->GetDirect().Get(), seen, spots);
    return;
  }
  auto add = [spots](const ByteString& name) {
    if (name != "Cyan" && name != "Magenta" && name != "Yellow" &&
        name != "Black" && name != "All" && name != "None" && !name.IsEmpty() &&
        std::find(spots->begin(), spots->end(), name) == spots->end()) {
      spots->push_back(name);
    }
  };
  if (const CPDF_Array* array = obj->AsArray()) {
    const ByteString family = array->GetByteStringAt(0);
    if (family == "Separation") {
      add(array->GetByteStringAt(1));
    } else if (family == "DeviceN") {
      RetainPtr<const CPDF_Array> names = array->GetArrayAt(1);
      for (size_t i = 0; names && i < names->size(); ++i) {
        add(names->GetByteStringAt(i));
      }
    }
    for (size_t i = 0; i < array->size(); ++i) {
      FindSpots(array->GetObjectAt(i).Get(), seen, spots);
    }
    return;
  }
  const CPDF_Dictionary* dict =
      obj->AsStream() ? obj->AsStream()->GetDict().Get() : obj->AsDictionary();
  if (!dict) {
    return;
  }
  CPDF_DictionaryLocker locker(dict);
  for (const auto& [key, value] : locker) {
    if (key != "Parent" && key != "P") {
      FindSpots(value.Get(), seen, spots);
    }
  }
}

}  // namespace

std::vector<ByteString> PageSpotInks(const CPDF_Page* page) {
  std::set<const CPDF_Object*> seen;
  std::vector<ByteString> spots;
  FindSpots(page->GetDict()->GetDictFor("Resources").Get(), &seen, &spots);
  RetainPtr<const CPDF_Array> annots = page->GetAnnotsArray();
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    if (annot) {
      FindSpots(annot->GetDictFor("AP").Get(), &seen, &spots);
    }
  }
  return spots;
}

bool BlendsNonSeparably(const CPDF_PageObjectHolder* holder) {
  for (const auto& obj : *holder) {
    const BlendMode mode = obj->general_state().GetBlendType();
    if (mode == BlendMode::kHue || mode == BlendMode::kSaturation ||
        mode == BlendMode::kColor || mode == BlendMode::kLuminosity) {
      return true;
    }
    if (obj->IsForm() && BlendsNonSeparably(obj->AsForm()->form())) {
      return true;
    }
  }
  return false;
}

CPDF_Inks::CPDF_Inks(CPDF_Page* page, const std::vector<ByteString>& names)
    : asked_(names.size()) {
  std::vector<ByteString> all = names;
  if (BlendsNonSeparably(page)) {
    for (const char* process : {"Cyan", "Magenta", "Yellow", "Black"}) {
      if (std::find(all.begin(), all.end(), process) == all.end()) {
        all.emplace_back(process);
      }
    }
  }
  for (const ByteString& name : all) {
    plates_.push_back(
        pdfium::MakeRetain<CPDF_Plate>(page->GetDocument(), name));
    image_caches_.push_back(std::make_unique<CPDF_PageImageCache>(page));
  }
}

CPDF_Inks::~CPDF_Inks() = default;

void CPDF_Inks::SetDevices(CFX_RenderDevice* device,
                           std::vector<CFX_RenderDevice*> planes) {
  device_ = device;
  planes_.clear();
  for (CFX_RenderDevice* plane : planes) {
    planes_.emplace_back(plane);
  }
}

FX_ARGB CPDF_Inks::Handle(const CPDF_Color* color,
                          const CPDF_GeneralState& state,
                          bool stroke) {
  std::vector<FX_ARGB> argbs;
  argbs.reserve(plates_.size());
  uint32_t alpha = 0;
  for (const auto& plate : plates_) {
    const FX_ARGB argb =
        PlateArgb(plate.Get(), color, state, stroke).value_or(0);
    alpha = std::max<uint32_t>(alpha, FXARGB_A(argb));
    argbs.push_back(argb);
  }
  if (alpha == 0) {
    return 0;
  }
  auto [it, added] = indices_.try_emplace(std::move(argbs), colors_.size());
  if (added) {
    if (colors_.size() >= kMaxColors) {
      indices_.erase(it);
      Fail();
      return 0;
    }
    colors_.push_back(it->first);
  }
  return (alpha << 24) | (kTag << 16) | it->second;
}

bool CPDF_Inks::IsUniform(FX_ARGB handle) const {
  if (handle == 0) {
    return true;
  }
  if (((handle >> 16) & 0xFF) != kTag || (handle & 0xFFFF) >= colors_.size()) {
    return false;
  }
  for (FX_ARGB argb : colors_[handle & 0xFFFF]) {
    if (FXARGB_A(argb) != FXARGB_A(handle)) {
      return false;
    }
  }
  return true;
}

std::optional<FX_ARGB> CPDF_Inks::Resolve(FX_ARGB handle, size_t i) const {
  if (handle == 0) {
    return 0;
  }
  if (((handle >> 16) & 0xFF) != kTag || (handle & 0xFFFF) >= colors_.size()) {
    return std::nullopt;
  }
  return colors_[handle & 0xFFFF][i];
}

CPDF_PageImageCache* CPDF_Inks::ImageCache(const CPDF_Plate* plate) const {
  for (size_t i = 0; i < plates_.size(); ++i) {
    if (plates_[i].Get() == plate) {
      return image_caches_[i].get();
    }
  }
  return nullptr;
}

CPDF_Inks* GetInks(const CPDF_Document* doc) {
  return CPDF_DocPageData::FromDocument(doc)->GetInks();
}

CPDF_PageImageCache* SeparationImageCache(const CPDF_Document* doc,
                                          CPDF_PageImageCache* page_cache) {
  const CPDF_Inks* inks = GetInks(doc);
  return inks
             ? inks->ImageCache(CPDF_DocPageData::FromDocument(doc)->GetPlate())
             : page_cache;
}

ScopedColorRender::ScopedColorRender(const CPDF_Document* doc) : doc_(doc) {
  CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc);
  plate_.Reset(data->GetPlate());
  inks_.Reset(data->GetInks());
  data->SetPlate(nullptr);
  data->SetInks(nullptr);
}

ScopedColorRender::~ScopedColorRender() {
  CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc_);
  data->SetPlate(std::move(plate_));
  data->SetInks(std::move(inks_));
}
