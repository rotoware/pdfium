// pdfking: one press plate rendered as gray, white no ink, black full ink.

#include "core/fpdfapi/page/cpdf_plate.h"

#include <algorithm>
#include <set>
#include <vector>

#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_generalstate.h"
#include "core/fpdfapi/page/cpdf_iccprofile.h"
#include "core/fpdfapi/page/cpdf_indexedcs.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcodec/icc/icc_transform.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/numerics/safe_conversions.h"

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

struct CmsProfileDeleter {
  void operator()(cmsHPROFILE p) { cmsCloseProfile(p); }
};
using ScopedCmsProfile = std::unique_ptr<void, CmsProfileDeleter>;

ScopedCmsProfile OpenProfile(pdfium::span<const uint8_t> data) {
  return ScopedCmsProfile(cmsOpenProfileFromMem(
      data.data(), pdfium::checked_cast<cmsUInt32Number>(data.size())));
}

// The CMYK profile of `doc`'s first output intent with one, empty for none
DataVector<uint8_t> OutputProfile(const CPDF_Document* doc) {
  const CPDF_Dictionary* root = doc->GetRoot();
  RetainPtr<const CPDF_Array> intents =
      root ? root->GetArrayFor("OutputIntents") : nullptr;
  for (size_t i = 0; intents && i < intents->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> intent = intents->GetDictAt(i);
    RetainPtr<const CPDF_Stream> stream =
        intent ? intent->GetStreamFor("DestOutputProfile") : nullptr;
    if (!stream) {
      continue;
    }
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(stream));
    acc->LoadAllDataFiltered();
    ScopedCmsProfile profile = OpenProfile(acc->GetSpan());
    if (profile && cmsGetColorSpace(profile.get()) == cmsSigCmykData) {
      return DataVector<uint8_t>(acc->GetSpan().begin(), acc->GetSpan().end());
    }
  }
  return {};
}

// The gray of the plate `cs` reads as, components in 0..255 (indices for an
// Indexed base), each image line through GetRGB.
class CPDF_PlateCS final : public CPDF_ColorSpace {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  std::optional<FX_RGB_STRUCT<float>> GetRGB(
      pdfium::span<const float> pBuf) const override {
    float gray =
        PlateGray(PlateByte(
            plate_->Ink(base_.Get(), pBuf, false, 0, intent_).value_or(0.0f)));
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
      uint8_t gray = static_cast<uint8_t>(rgb.red * 255);
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
  // components through this space; Indexed over an indexed base, whose
  // images are read as raw indices by that family
  CPDF_PlateCS(RetainPtr<CPDF_ColorSpace> base,
               const CPDF_Plate* plate,
               int intent)
      : CPDF_ColorSpace(base->AsIndexedCS() ? Family::kIndexed
                                             : Family::kDeviceN),
        base_(std::move(base)),
        plate_(plate),
        intent_(intent) {
    SetComponentsForStockCS(base_->ComponentCount());
  }
  ~CPDF_PlateCS() override = default;

  RetainPtr<CPDF_ColorSpace> base_;
  RetainPtr<const CPDF_Plate> plate_;
  const int intent_;
};

}  // namespace

struct CPDF_Plate::Transform {
  ~Transform() {
    if (handle) {
      cmsDeleteTransform(handle);
    }
  }
  cmsHTRANSFORM handle = nullptr;
  uint32_t components = 0;
  bool lab = false;
};

CPDF_Plate::CPDF_Plate(const CPDF_Document* doc, const ByteString& name)
    : name_(name), index_(ProcessIndex(name)), output_(OutputProfile(doc)) {}

CPDF_Plate::~CPDF_Plate() = default;

const CPDF_Plate* GetPlate(const CPDF_Document* doc) {
  return CPDF_DocPageData::FromDocument(doc)->GetPlate();
}

// static
int CPDF_Plate::Intent(const ByteString& name) {
  return name == "AbsoluteColorimetric" ? 1
         : name == "Saturation"         ? 2
         : name == "Perceptual"         ? 3
                                        : 0;
}

std::optional<std::array<float, 4>> CPDF_Plate::ToCmyk(
    const CPDF_ColorSpace* cs,
    pdfium::span<const float> comps,
    int intent) const {
  if (output_.empty()) {
    return std::nullopt;
  }
  const void* key = nullptr;
  pdfium::span<const uint8_t> source;
  switch (cs->GetFamily()) {
    case Family::kICCBased: {
      RetainPtr<CPDF_IccProfile> profile = cs->GetIccProfile();
      if (!profile) {
        return std::nullopt;
      }
      source = profile->GetStreamAcc()->GetSpan();
      // CMYK in the output intent's own profile goes on as it is
      if (std::ranges::equal(source, output_)) {
        return std::nullopt;
      }
      key = profile.Get();
      break;
    }
    case Family::kDeviceRGB:
    case Family::kCalRGB:
      break;  // as sRGB
    case Family::kLab:
      key = cs;
      break;
    default:
      return std::nullopt;
  }
  std::unique_ptr<Transform>& transform = transforms_[{key, intent}];
  if (!transform) {
    transform = std::make_unique<Transform>();
    transform->lab = cs->GetFamily() == Family::kLab;
    ScopedCmsProfile src(transform->lab  ? cmsCreateLab4Profile(nullptr)
                         : source.empty() ? cmsCreate_sRGBProfile()
                                          : OpenProfile(source).release());
    ScopedCmsProfile dst = OpenProfile(output_);
    if (src && dst) {
      transform->components = cmsChannelsOf(cmsGetColorSpace(src.get()));
      const cmsUInt32Number format =
          transform->lab ? TYPE_Lab_DBL
                         : COLORSPACE_SH(PT_ANY) |
                               CHANNELS_SH(transform->components) |
                               BYTES_SH(2);
      // CPDF_GeneralState's intents in lcms's
      constexpr cmsUInt32Number kIntents[] = {
          INTENT_RELATIVE_COLORIMETRIC, INTENT_ABSOLUTE_COLORIMETRIC,
          INTENT_SATURATION, INTENT_PERCEPTUAL};
      transform->handle = cmsCreateTransform(
          src.get(), format, dst.get(), TYPE_CMYK_16, kIntents[intent & 3],
          cmsFLAGS_BLACKPOINTCOMPENSATION);
    }
  }
  if (!transform->handle || comps.size() < transform->components) {
    return std::nullopt;
  }
  std::array<uint16_t, 4> out;
  if (transform->lab) {
    const std::array<double, 3> lab = {comps[0], comps[1], comps[2]};
    cmsDoTransform(transform->handle, lab.data(), out.data(), 1);
  } else {
    std::array<uint16_t, 4> in = {};
    for (uint32_t i = 0; i < transform->components; ++i) {
      in[i] = static_cast<uint16_t>(Clamp(comps[i]) * 65535.0f + 0.5f);
    }
    cmsDoTransform(transform->handle, in.data(), out.data(), 1);
  }
  return std::array<float, 4>{out[0] / 65535.0f, out[1] / 65535.0f,
                              out[2] / 65535.0f, out[3] / 65535.0f};
}

int PlateByte(float ink) {
  const int frac = static_cast<int>(Clamp(ink) * 32760.0f + 0.5f);
  const int cv = (frac << 1) + (frac >> 14);
  return cv >> 8;
}

float PlateGray(int byte) {
  return (255 - byte + 0.25f) / 255.0f;
}

bool CPDF_Plate::Marks(const CPDF_ColorSpace* cs) const {
  if (!cs) {
    return false;
  }
  switch (cs->GetFamily()) {
    case Family::kIndexed:
      return Marks(cs->AsIndexedCS()->GetBaseCS());
    case Family::kSeparation:
    case Family::kDeviceN:
      for (const ByteString& name : Colorants(cs)) {
        if (name == name_ || name == "All") {
          return true;
        }
      }
      return false;
    case Family::kPattern:
      return true;
    default:
      // every process space paints all four process plates, its ink or none
      return index_ >= 0;
  }
}

std::optional<float> CPDF_Plate::Ink(const CPDF_ColorSpace* cs,
                                     pdfium::span<const float> comps,
                                     bool overprint,
                                     int opm,
                                     int intent) const {
  if (!cs) {
    return std::nullopt;
  }
  const Family family = cs->GetFamily();
  if (family == Family::kIndexed) {
    const CPDF_IndexedCS* indexed = cs->AsIndexedCS();
    std::optional<DataVector<float>> base = indexed->GetBaseComponents(comps);
    if (!base.has_value()) {
      return overprint ? std::nullopt : std::optional<float>(0.0f);
    }
    return Ink(indexed->GetBaseCS(), base.value(), overprint, opm, intent);
  }
  if (family == Family::kSeparation || family == Family::kDeviceN) {
    std::vector<ByteString> names = Colorants(cs);
    bool marks = false;
    for (size_t i = 0; i < names.size() && i < comps.size(); ++i) {
      if (names[i] == "None") {
        continue;
      }
      marks = true;
      if (names[i] == name_ || names[i] == "All") {
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
  if (overprint && !Marks(cs)) {
    return std::nullopt;
  }
  if (std::optional<std::array<float, 4>> cmyk = ToCmyk(cs, comps, intent)) {
    return index_ < 0 ? 0.0f : cmyk.value()[index_];
  }
  if (IsCmyk(cs)) {
    if (index_ < 0 || comps.size() < 4) {
      return 0.0f;
    }
    float v = Clamp(comps[index_]);
    if (overprint && opm == 1 && family == Family::kDeviceCMYK && v == 0) {
      return std::nullopt;
    }
    return v;
  }
  if (IsGray(cs)) {
    return index_ == 3 && !comps.empty() ? 1.0f - Clamp(comps[0]) : 0.0f;
  }
  if (index_ < 0 || index_ == 3) {
    return 0.0f;
  }
  // CalRGB as plain RGB, as gs -dUseFastColor takes it: its gamma and matrix
  // only say what the values look like, never what goes on the plate
  if (family == Family::kCalRGB && comps.size() >= 3) {
    return 1.0f - Clamp(comps[index_]);
  }
  auto rgb = cs->GetRGBOrZerosOnError(comps);
  const float channel =
      index_ == 0 ? rgb.red : index_ == 1 ? rgb.green : rgb.blue;
  return 1.0f - Clamp(channel);
}

RetainPtr<CPDF_ColorSpace> PlateColorSpace(RetainPtr<CPDF_ColorSpace> base,
                                           const CPDF_Plate* plate,
                                           int intent) {
  if (!base || base->GetFamily() == Family::kPattern) {
    return base;
  }
  return pdfium::MakeRetain<CPDF_PlateCS>(std::move(base), plate, intent);
}

std::optional<FX_ARGB> PlateArgb(const CPDF_Plate* plate,
                                 const CPDF_Color* color,
                                 const CPDF_GeneralState& state,
                                 bool stroke) {
  if (!plate || !color || color->IsPattern()) {
    return std::nullopt;
  }
  std::optional<float> ink =
      plate->Ink(color->GetColorSpace(), color->GetComponents(),
                 stroke ? state.GetStrokeOP() : state.GetFillOP(),
                 state.GetOPMode(), state.GetRenderIntent());
  if (!ink.has_value()) {
    return 0;
  }
  const int gray = 255 - PlateByte(ink.value());
  const float alpha = stroke ? state.GetStrokeAlpha() : state.GetFillAlpha();
  return ArgbEncode(FXSYS_roundf(alpha * 255), gray, gray, gray);
}

BlendMode PlateBlendMode(BlendMode mode, const CPDF_Plate* plate) {
  if (plate && plate->IsSpot() &&
      (mode == BlendMode::kDifference || mode == BlendMode::kExclusion ||
       mode >= BlendMode::kHue)) {
    return BlendMode::kNormal;
  }
  return mode;
}
