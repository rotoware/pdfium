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
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcodec/icc/icc_transform.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"

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

// Whether gs takes `page` as having transparency, from what its resources
// (and its annotations') hold, as pdfi_check_page: an ExtGState or
// annotation blending other than Normal or Compatible, with a soft mask or
// a constant alpha below 1, an image with a soft mask, a form that is a
// group, a Highlight annotation.
bool Transparent(const CPDF_Object* obj, std::set<const CPDF_Object*>* seen) {
  if (!obj || !seen->insert(obj).second) {
    return false;
  }
  if (const CPDF_Reference* ref = obj->AsReference()) {
    return Transparent(ref->GetDirect().Get(), seen);
  }
  if (const CPDF_Array* array = obj->AsArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      if (Transparent(array->GetObjectAt(i).Get(), seen)) {
        return true;
      }
    }
    return false;
  }
  const CPDF_Dictionary* dict =
      obj->AsStream() ? obj->AsStream()->GetDict().Get() : obj->AsDictionary();
  if (!dict) {
    return false;
  }
  const ByteString bm = dict->GetNameFor("BM");
  if (!bm.IsEmpty() && bm != "Normal" && bm != "Compatible") {
    return true;
  }
  RetainPtr<const CPDF_Object> smask = dict->GetDirectObjectFor("SMask");
  if (smask && !(smask->IsName() && smask->GetString() == "None")) {
    return true;
  }
  if (dict->GetFloatFor("SMaskInData") != 0) {
    return true;
  }
  for (const char* key : {"CA", "ca"}) {
    RetainPtr<const CPDF_Object> alpha = dict->GetDirectObjectFor(key);
    if (alpha && alpha->IsNumber() && alpha->GetNumber() != 1.0f) {
      return true;
    }
  }
  const ByteString subtype = dict->GetNameFor("Subtype");
  if ((subtype == "Form" && dict->GetDictFor("Group")) ||
      subtype == "Highlight") {
    return true;
  }
  CPDF_DictionaryLocker locker(dict);
  for (const auto& [key, value] : locker) {
    if (key != "Parent" && key != "P" && Transparent(value.Get(), seen)) {
      return true;
    }
  }
  return false;
}

bool PageTransparent(const CPDF_Page* page) {
  std::set<const CPDF_Object*> seen;
  return Transparent(page->GetResources().Get(), &seen) ||
         Transparent(page->GetAnnotsArray().Get(), &seen);
}

// The profile of the group `dict` (a page's or a form's) has when it blends
// in RGB (its own, or empty for DeviceRGB, CalRGB: the default), nothing
// when it does not
std::optional<DataVector<uint8_t>> RgbGroupProfile(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> group = dict->GetDictFor("Group");
  if (!group || group->GetNameFor("S") != "Transparency") {
    return std::nullopt;
  }
  RetainPtr<const CPDF_Object> cs = group->GetDirectObjectFor("CS");
  if (!cs) {
    return std::nullopt;
  }
  if (cs->IsName()) {
    return cs->GetString() == "DeviceRGB"
               ? std::optional<DataVector<uint8_t>>(DataVector<uint8_t>())
               : std::nullopt;
  }
  const CPDF_Array* array = cs->AsArray();
  if (!array) {
    return std::nullopt;
  }
  const ByteString family = array->GetByteStringAt(0);
  if (family == "CalRGB") {
    return DataVector<uint8_t>();
  }
  RetainPtr<const CPDF_Stream> stream = array->GetStreamAt(1);
  if (family != "ICCBased" || !stream ||
      stream->GetDict()->GetIntegerFor("N") != 3) {
    return std::nullopt;
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(stream));
  acc->LoadAllDataFiltered();
  return DataVector<uint8_t>(acc->GetSpan().begin(), acc->GetSpan().end());
}

// Whether a form `obj` holds (or a form in it, a pattern's...) is a group
// blending in RGB
bool HasRgbFormGroup(const CPDF_Object* obj,
                     std::set<const CPDF_Object*>* seen) {
  if (!obj || !seen->insert(obj).second) {
    return false;
  }
  if (const CPDF_Reference* ref = obj->AsReference()) {
    return HasRgbFormGroup(ref->GetDirect().Get(), seen);
  }
  if (const CPDF_Array* array = obj->AsArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      if (HasRgbFormGroup(array->GetObjectAt(i).Get(), seen)) {
        return true;
      }
    }
    return false;
  }
  const CPDF_Dictionary* dict =
      obj->AsStream() ? obj->AsStream()->GetDict().Get() : obj->AsDictionary();
  if (!dict) {
    return false;
  }
  if (dict->GetNameFor("Subtype") == "Form" &&
      RgbGroupProfile(dict).has_value()) {
    return true;
  }
  CPDF_DictionaryLocker locker(dict);
  for (const auto& [key, value] : locker) {
    if (key != "Parent" && key != "P" && HasRgbFormGroup(value.Get(), seen)) {
      return true;
    }
  }
  return false;
}

// Whether `dict` (a form's) is a transparency group blending in DeviceGray
bool IsGrayGroup(const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Dictionary> group = dict->GetDictFor("Group");
  return group && group->GetNameFor("S") == "Transparency" &&
         group->GetNameFor("CS") == "DeviceGray";
}

// Whether a form `obj` holds (or a form in it...) is a gray group
bool HasGrayFormGroup(const CPDF_Object* obj,
                      std::set<const CPDF_Object*>* seen) {
  if (!obj || !seen->insert(obj).second) {
    return false;
  }
  if (const CPDF_Reference* ref = obj->AsReference()) {
    return HasGrayFormGroup(ref->GetDirect().Get(), seen);
  }
  if (const CPDF_Array* array = obj->AsArray()) {
    for (size_t i = 0; i < array->size(); ++i) {
      if (HasGrayFormGroup(array->GetObjectAt(i).Get(), seen)) {
        return true;
      }
    }
    return false;
  }
  const CPDF_Dictionary* dict =
      obj->AsStream() ? obj->AsStream()->GetDict().Get() : obj->AsDictionary();
  if (!dict) {
    return false;
  }
  if (dict->GetNameFor("Subtype") == "Form" && IsGrayGroup(dict)) {
    return true;
  }
  CPDF_DictionaryLocker locker(dict);
  for (const auto& [key, value] : locker) {
    if (key != "Parent" && key != "P" && HasGrayFormGroup(value.Get(), seen)) {
      return true;
    }
  }
  return false;
}

// An lcms transform RGB (or gray) 8 to CMYK 8 from `source` to `dest`, as gs
// makes its links (gsicc_lcms2mt.c gscms_get_link): 16-bit, high resolution
// precalculation, black point compensation, then on 8-bit buffers
cmsHTRANSFORM ToCmyk(pdfium::span<const uint8_t> source,
                     pdfium::span<const uint8_t> dest,
                     bool gray) {
  cmsHPROFILE src = cmsOpenProfileFromMem(
      source.data(), pdfium::checked_cast<cmsUInt32Number>(source.size()));
  cmsHPROFILE dst = cmsOpenProfileFromMem(
      dest.data(), pdfium::checked_cast<cmsUInt32Number>(dest.size()));
  cmsHTRANSFORM transform = nullptr;
  if (src && dst) {
    transform = cmsCreateTransform(
        src, gray ? TYPE_GRAY_16 : TYPE_RGB_16, dst, TYPE_CMYK_16,
        INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (transform && !cmsChangeBuffersFormat(
                         transform, gray ? TYPE_GRAY_8 : TYPE_RGB_8,
                         TYPE_CMYK_8)) {
      cmsDeleteTransform(transform);
      transform = nullptr;
    }
  }
  if (src) {
    cmsCloseProfile(src);
  }
  if (dst) {
    cmsCloseProfile(dst);
  }
  return transform;
}

bool IsProcess(const ByteString& name) {
  return name == "Cyan" || name == "Magenta" || name == "Yellow" ||
         name == "Black";
}

}  // namespace

struct CPDF_Inks::Blend {
  ~Blend() {
    if (transform) {
      cmsDeleteTransform(transform);
    }
  }
  cmsHTRANSFORM transform = nullptr;
  // the blend channel plates (R, G, B), and per CMYK component the plate
  // asked for it, if any
  std::array<size_t, 3> channels = {};
  std::array<std::optional<size_t>, 4> process;
};

std::vector<ByteString> PageSpotInks(const CPDF_Page* page) {
  std::set<const CPDF_Object*> seen;
  std::vector<ByteString> spots;
  // its resources as it renders them: inherited from the page tree too
  FindSpots(page->GetResources().Get(), &seen, &spots);
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
    : asked_(names.size()),
      page_doc_(page->GetDocument()),
      page_transparent_(PageTransparent(page)) {
  const CPDF_DocPageData* data =
      CPDF_DocPageData::FromDocument(page->GetDocument());
  std::optional<DataVector<uint8_t>> group_profile;
  if (!data->blend_rgb().empty() && !data->blend_cmyk().empty() &&
      std::ranges::any_of(names, IsProcess)) {
    group_profile = RgbGroupProfile(page->GetDict().Get());
    if (group_profile.has_value() && !PageTransparent(page)) {
      group_profile.reset();
    }
  }
  if (group_profile.has_value()) {
    InitBlend(page, names, group_profile.value());
    return;
  }
  std::vector<ByteString> all = names;
  std::set<const CPDF_Object*> seen;
  const bool rgb_groups =
      !data->blend_rgb().empty() && !data->blend_cmyk().empty() &&
      HasRgbFormGroup(page->GetResources().Get(), &seen);
  std::set<const CPDF_Object*> seen_gray;
  const bool gray_groups =
      !data->blend_gray().empty() && !data->blend_cmyk().empty() &&
      HasGrayFormGroup(page->GetResources().Get(), &seen_gray);
  if (BlendsNonSeparably(page) || rgb_groups || gray_groups) {
    for (const char* process : {"Cyan", "Magenta", "Yellow", "Black"}) {
      if (std::find(all.begin(), all.end(), process) == all.end()) {
        all.emplace_back(process);
      }
    }
  }
  for (const ByteString& name : all) {
    if (IsProcess(name)) {
      process_[name == "Cyan" ? 0 : name == "Magenta" ? 1 : name == "Yellow" ? 2 : 3] =
          plates_.size();
    }
    plates_.push_back(
        pdfium::MakeRetain<CPDF_Plate>(page->GetDocument(), name));
    image_caches_.push_back(std::make_unique<CPDF_PageImageCache>(page));
  }
  if (rgb_groups) {
    for (int channel = 0; channel < 4; ++channel) {
      channel_plates_.push_back(pdfium::MakeRetain<CPDF_Plate>(
          page->GetDocument(), ByteString("#") + "RGBK"[channel],
          CPDF_Plate::Role::kBlendChannel, channel));
      channel_caches_.push_back(std::make_unique<CPDF_PageImageCache>(page));
    }
  }
  if (gray_groups) {
    // the gray channel where C goes, white ones for M, Y, K
    for (int channel = 0; channel < 4; ++channel) {
      gray_plates_.push_back(pdfium::MakeRetain<CPDF_Plate>(
          page->GetDocument(), channel == 0 ? "#Gray" : "#White",
          CPDF_Plate::Role::kBlendChannel, channel == 0 ? 4 : 3));
      gray_caches_.push_back(std::make_unique<CPDF_PageImageCache>(page));
    }
  }
}

bool CPDF_Inks::GroupBlendsInGray(const CPDF_Dictionary* form) const {
  return !gray_plates_.empty() && IsGrayGroup(form);
}

void CPDF_Inks::SwapGrayChannels() {
  for (int c = 0; c < 4; ++c) {
    std::swap(plates_[process_[c]], gray_plates_[c]);
    std::swap(image_caches_[process_[c]], gray_caches_[c]);
  }
}

void CPDF_Inks::ConvertGrayGroup(
    const std::vector<RetainPtr<CFX_DIBitmap>>& bitmaps) {
  // gray to CMYK through gs's default gray and CMYK profiles, as it
  // converts a gray group's buffer to its parent's color space
  if (!gray_link_) {
    const CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(page_doc_);
    gray_link_ = std::make_unique<Blend>();
    gray_link_->transform =
        ToCmyk(data->blend_gray(), data->blend_cmyk(), /*gray=*/true);
  }
  if (!gray_link_->transform) {
    Fail();
    return;
  }
  const RetainPtr<CFX_DIBitmap>& gray = bitmaps[process_[0]];
  const int width = gray->GetWidth();
  std::vector<uint8_t> in(width);
  std::vector<uint8_t> alpha(width);
  std::vector<uint8_t> out(width * 4);
  for (int row = 0; row < gray->GetHeight(); ++row) {
    pdfium::span<const uint8_t> line = gray->GetScanline(row);
    for (int x = 0; x < width; ++x) {
      in[x] = line[x * 4];
      alpha[x] = line[x * 4 + 3];
    }
    cmsDoTransform(gray_link_->transform, in.data(), out.data(), width);
    for (int p = 0; p < 4; ++p) {
      pdfium::span<uint8_t> plane =
          bitmaps[process_[p]]->GetWritableScanline(row);
      for (int x = 0; x < width; ++x) {
        plane[x * 4] = plane[x * 4 + 1] = plane[x * 4 + 2] =
            255 - out[x * 4 + p];
        plane[x * 4 + 3] = alpha[x];
      }
    }
  }
}

bool CPDF_Inks::GroupBlendsInRgb(const CPDF_Dictionary* form) const {
  return !channel_plates_.empty() && RgbGroupProfile(form).has_value();
}

void CPDF_Inks::SwapChannels() {
  for (int c = 0; c < 4; ++c) {
    std::swap(plates_[process_[c]], channel_plates_[c]);
    std::swap(image_caches_[process_[c]], channel_caches_[c]);
  }
}

void CPDF_Inks::ConvertGroup(
    const CPDF_Dictionary* form,
    const std::vector<RetainPtr<CFX_DIBitmap>>& bitmaps) {
  RetainPtr<const CPDF_Dictionary> group = form->GetDictFor("Group");
  RetainPtr<const CPDF_Object> cs = group->GetDirectObjectFor("CS");
  const void* key = cs && cs->IsArray() ? cs.Get() : nullptr;
  std::unique_ptr<Blend>& blend = group_blends_[key];
  if (!blend) {
    blend = std::make_unique<Blend>();
    const CPDF_DocPageData* data =
        CPDF_DocPageData::FromDocument(page_doc_);
    DataVector<uint8_t> profile = RgbGroupProfile(form).value_or(
        DataVector<uint8_t>());
    DataVector<uint8_t> output = OutputIntentProfile(page_doc_);
    blend->transform = ToCmyk(
        profile.empty() ? pdfium::span(data->blend_rgb())
                        : pdfium::span(profile),
        output.empty() ? pdfium::span(data->blend_cmyk())
                       : pdfium::span(output),
        /*gray=*/false);
  }
  if (!blend->transform) {
    Fail();
    return;
  }
  const RetainPtr<CFX_DIBitmap>& r = bitmaps[process_[0]];
  const int width = r->GetWidth();
  std::vector<uint8_t> in(width * 3);
  std::vector<uint8_t> out(width * 4);
  for (int row = 0; row < r->GetHeight(); ++row) {
    for (int c = 0; c < 3; ++c) {
      pdfium::span<const uint8_t> line = bitmaps[process_[c]]->GetScanline(row);
      for (int x = 0; x < width; ++x) {
        in[x * 3 + c] = line[x * 4];
      }
    }
    cmsDoTransform(blend->transform, in.data(), out.data(), width);
    // the group's alpha: R's (every channel painted together)
    std::vector<uint8_t> alpha(width);
    pdfium::span<const uint8_t> red = r->GetScanline(row);
    for (int x = 0; x < width; ++x) {
      alpha[x] = red[x * 4 + 3];
    }
    for (int p = 0; p < 4; ++p) {
      pdfium::span<uint8_t> line =
          bitmaps[process_[p]]->GetWritableScanline(row);
      for (int x = 0; x < width; ++x) {
        const uint8_t gray = 255 - out[x * 4 + p];
        line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = gray;
        line[x * 4 + 3] = alpha[x];
      }
    }
  }
}

CPDF_Inks::~CPDF_Inks() = default;

// The process plates asked derived, the blend channels R, G, B and a white
// K after them, the conversion RGB to CMYK: from the group's profile (the
// default RGB one for DeviceRGB) to the output intent's, else the default
// CMYK one, relative colorimetric, black point compensated, as gs's link
void CPDF_Inks::InitBlend(CPDF_Page* page,
                          const std::vector<ByteString>& names,
                          const DataVector<uint8_t>& group_profile) {
  CPDF_Document* doc = page->GetDocument();
  const CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc);
  blend_ = std::make_unique<Blend>();
  for (size_t i = 0; i < names.size(); ++i) {
    const bool process = IsProcess(names[i]);
    plates_.push_back(pdfium::MakeRetain<CPDF_Plate>(
        doc, names[i],
        process ? CPDF_Plate::Role::kDerived : CPDF_Plate::Role::kPlate));
    if (process) {
      const int component = names[i] == "Cyan"      ? 0
                            : names[i] == "Magenta" ? 1
                            : names[i] == "Yellow"  ? 2
                                                    : 3;
      blend_->process[component] = i;
    }
  }
  for (int channel = 0; channel < 4; ++channel) {
    if (channel < 3) {
      blend_->channels[channel] = plates_.size();
    }
    plates_.push_back(pdfium::MakeRetain<CPDF_Plate>(
        doc, ByteString("#") + "RGBK"[channel],
        CPDF_Plate::Role::kBlendChannel, channel));
  }
  for (size_t i = 0; i < plates_.size(); ++i) {
    image_caches_.push_back(std::make_unique<CPDF_PageImageCache>(page));
  }
  pdfium::span<const uint8_t> source = group_profile.empty()
                                           ? pdfium::span(data->blend_rgb())
                                           : pdfium::span(group_profile);
  DataVector<uint8_t> output = OutputIntentProfile(doc);
  pdfium::span<const uint8_t> dest = output.empty()
                                         ? pdfium::span(data->blend_cmyk())
                                         : pdfium::span(output);
  blend_->transform = ToCmyk(source, dest, /*gray=*/false);
  if (!blend_->transform) {
    Fail();
  }
}

void CPDF_Inks::ConvertBlendSpace() const {
  if (!blend_ || !blend_->transform) {
    return;
  }
  std::array<RetainPtr<CFX_DIBitmap>, 3> rgb;
  for (int c = 0; c < 3; ++c) {
    rgb[c] = planes_[blend_->channels[c]]->GetBitmap();
  }
  const int width = rgb[0]->GetWidth();
  std::vector<uint8_t> in(width * 3);
  std::vector<uint8_t> out(width * 4);
  for (int row = 0; row < rgb[0]->GetHeight(); ++row) {
    for (int c = 0; c < 3; ++c) {
      pdfium::span<const uint8_t> line = rgb[c]->GetScanline(row);
      for (int x = 0; x < width; ++x) {
        in[x * 3 + c] = line[x];
      }
    }
    cmsDoTransform(blend_->transform, in.data(), out.data(), width);
    for (int p = 0; p < 4; ++p) {
      if (!blend_->process[p].has_value()) {
        continue;
      }
      pdfium::span<uint8_t> line =
          planes_[blend_->process[p].value()]->GetBitmap()->GetWritableScanline(
              row);
      for (int x = 0; x < width; ++x) {
        line[x] = 255 - out[x * 4 + p];
      }
    }
  }
}

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

CPDF_Inks* GetGsInks(const CPDF_Document* doc) {
  CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc);
  return data->GetInks() ? data->GetInks() : data->GetColorRenderInks();
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
  color_render_inks_.Reset(data->GetColorRenderInks());
  data->SetColorRenderInks(pdfium::WrapRetain(GetGsInks(doc)));
  data->SetPlate(nullptr);
  data->SetInks(nullptr);
}

ScopedColorRender::~ScopedColorRender() {
  CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc_);
  data->SetPlate(std::move(plate_));
  data->SetInks(std::move(inks_));
  data->SetColorRenderInks(std::move(color_render_inks_));
}
