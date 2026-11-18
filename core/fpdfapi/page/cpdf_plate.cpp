// pdfking: one press plate rendered as gray, white no ink, black full ink.

#include "core/fpdfapi/page/cpdf_plate.h"

#include <algorithm>
#include <set>
#include <vector>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_indexedcs.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fxcrt/data_vector.h"

namespace {

using Family = CPDF_ColorSpace::Family;

// a process plate's component in CMYK, -1 for a spot
int ProcessIndex(const ByteString& plate) {
  if (plate == "Cyan") {
    return 0;
  }
  if (plate == "Magenta") {
    return 1;
  }
  if (plate == "Yellow") {
    return 2;
  }
  if (plate == "Black") {
    return 3;
  }
  return -1;
}

// The colorants of a Separation or DeviceN, in component order
std::vector<ByteString> Colorants(const CPDF_ColorSpace* cs) {
  std::vector<ByteString> names;
  const CPDF_Array* array = cs->GetArray();
  if (!array) {
    return names;
  }
  if (cs->GetFamily() == Family::kSeparation) {
    names.push_back(array->GetByteStringAt(1));
    return names;
  }
  RetainPtr<const CPDF_Array> colorants = array->GetArrayAt(1);
  for (size_t i = 0; colorants && i < colorants->size(); ++i) {
    names.push_back(colorants->GetByteStringAt(i));
  }
  return names;
}

bool IsCmyk(const CPDF_ColorSpace* cs) {
  return cs->GetFamily() == Family::kDeviceCMYK ||
         (cs->GetFamily() == Family::kICCBased && cs->ComponentCount() == 4);
}

bool IsGray(const CPDF_ColorSpace* cs) {
  Family family = cs->GetFamily();
  return family == Family::kDeviceGray || family == Family::kCalGray ||
         (family == Family::kICCBased && cs->ComponentCount() == 1);
}

float Clamp(float v) {
  return std::clamp(v, 0.0f, 1.0f);
}

// The gray of the plate `cs` reads as, components in 0..255 (indices for an
// Indexed base), each image line through GetRGB.
class CPDF_PlateCS final : public CPDF_ColorSpace {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  std::optional<FX_RGB_STRUCT<float>> GetRGB(
      pdfium::span<const float> pBuf) const override {
    float gray =
        1.0f - PlateInk(base_.Get(), pBuf, plate_, false, 0).value_or(0.0f);
    return FX_RGB_STRUCT<float>{gray, gray, gray};
  }

  void GetDefaultValue(int iComponent,
                       float* value,
                       float* min,
                       float* max) const override {
    base_->GetDefaultValue(iComponent, value, min, max);
  }

  void TranslateImageLine(pdfium::span<uint8_t> dest_span,
                          pdfium::span<const uint8_t> src_span,
                          int pixels,
                          int image_width,
                          int image_height,
                          bool is_transparency_mask) const override {
    const uint32_t n = ComponentCount();
    const float divisor = base_->AsIndexedCS() ? 1.0f : 255.0f;
    std::vector<float> src(std::max(n, 16u));
    for (int i = 0; i < pixels; ++i) {
      for (uint32_t j = 0; j < n; ++j) {
        src[j] = src_span[i * n + j] / divisor;
      }
      auto rgb = GetRGBOrZerosOnError(src);
      uint8_t gray = static_cast<uint8_t>(rgb.red * 255 + 0.5f);
      dest_span[i * 3] = gray;
      dest_span[i * 3 + 1] = gray;
      dest_span[i * 3 + 2] = gray;
    }
  }

  bool IsNormal() const override { return base_->IsNormal(); }

  const CPDF_IndexedCS* AsIndexedCS() const override {
    return base_->AsIndexedCS();
  }

 protected:
  uint32_t v_Load(CPDF_Document* doc,
                  const CPDF_Array* pArray,
                  std::set<const CPDF_Object*>* pVisited) override {
    return 0;
  }

 private:
  // DeviceN: no image or shading fast path treats it as anything but its
  // components through this space
  CPDF_PlateCS(RetainPtr<CPDF_ColorSpace> base, const ByteString& plate)
      : CPDF_ColorSpace(Family::kDeviceN),
        base_(std::move(base)),
        plate_(plate) {
    SetComponentsForStockCS(base_->ComponentCount());
  }
  ~CPDF_PlateCS() override = default;

  RetainPtr<CPDF_ColorSpace> base_;
  ByteString plate_;
};

}  // namespace

bool PlateMarked(const CPDF_ColorSpace* cs, const ByteString& plate) {
  if (!cs) {
    return false;
  }
  const int index = ProcessIndex(plate);
  switch (cs->GetFamily()) {
    case Family::kIndexed:
      return PlateMarked(cs->AsIndexedCS()->GetBaseCS(), plate);
    case Family::kSeparation:
    case Family::kDeviceN:
      for (const ByteString& name : Colorants(cs)) {
        if (name == plate || name == "All") {
          return true;
        }
      }
      return false;
    case Family::kPattern:
      return true;
    default:
      if (IsCmyk(cs)) {
        return index >= 0;
      }
      if (IsGray(cs)) {
        return index == 3;
      }
      return index >= 0 && index < 3;
  }
}

std::optional<float> PlateInk(const CPDF_ColorSpace* cs,
                              pdfium::span<const float> comps,
                              const ByteString& plate,
                              bool overprint,
                              int opm) {
  if (!cs) {
    return std::nullopt;
  }
  const int index = ProcessIndex(plate);
  const Family family = cs->GetFamily();
  if (family == Family::kIndexed) {
    const CPDF_IndexedCS* indexed = cs->AsIndexedCS();
    std::optional<DataVector<float>> base = indexed->GetBaseComponents(comps);
    if (!base.has_value()) {
      return overprint ? std::nullopt : std::optional<float>(0.0f);
    }
    return PlateInk(indexed->GetBaseCS(), base.value(), plate, overprint,
                    opm);
  }
  if (family == Family::kSeparation || family == Family::kDeviceN) {
    std::vector<ByteString> names = Colorants(cs);
    bool marks = false;
    for (size_t i = 0; i < names.size() && i < comps.size(); ++i) {
      if (names[i] == "None") {
        continue;
      }
      marks = true;
      if (names[i] == plate || names[i] == "All") {
        return Clamp(comps[i]);
      }
    }
    // a None ink never marks; another ink knocks the plate out, but leaves it
    // as it was when it overprints
    if (!marks || overprint) {
      return std::nullopt;
    }
    return 0.0f;
  }
  if (overprint && !PlateMarked(cs, plate)) {
    return std::nullopt;
  }
  if (IsCmyk(cs)) {
    if (index < 0 || comps.size() < 4) {
      return 0.0f;
    }
    float v = Clamp(comps[index]);
    if (overprint && opm == 1 && family == Family::kDeviceCMYK && v == 0) {
      return std::nullopt;
    }
    return v;
  }
  if (IsGray(cs)) {
    return index == 3 && !comps.empty() ? 1.0f - Clamp(comps[0]) : 0.0f;
  }
  if (index < 0 || index == 3) {
    return 0.0f;
  }
  auto rgb = cs->GetRGBOrZerosOnError(comps);
  const float channel = index == 0 ? rgb.red : index == 1 ? rgb.green : rgb.blue;
  return 1.0f - Clamp(channel);
}

RetainPtr<CPDF_ColorSpace> PlateColorSpace(RetainPtr<CPDF_ColorSpace> base,
                                           const ByteString& plate) {
  if (!base || base->GetFamily() == Family::kPattern) {
    return base;
  }
  return pdfium::MakeRetain<CPDF_PlateCS>(std::move(base), plate);
}
