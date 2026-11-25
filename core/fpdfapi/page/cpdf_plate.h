// pdfking: one press plate rendered as gray, white no ink, black full ink.

#ifndef CORE_FPDFAPI_PAGE_CPDF_PLATE_H_
#define CORE_FPDFAPI_PAGE_CPDF_PLATE_H_

#include <stdint.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/fx_dib.h"

class CPDF_Color;
class CPDF_ColorSpace;
class CPDF_Document;
class CPDF_GeneralState;
class CPDF_Object;
class CPDF_TransferFunc;

// The plate (Cyan, Magenta, Yellow, Black or a spot's name) a document's
// pages render as, with the document's output intent: colors in an ICC
// based space, RGB or Lab go on the process plates through it, as a RIP
// separates them (relative colorimetric, black point compensated). Without
// one they go on as gs -dUseFastColor puts them (C = 1 - R, no black).
// The 8-bit ink of `ink` as gs puts it on a device: a frac (15 bits, 32760
// full) rounded, its 16-bit color value, the high byte. 0.7 is 179 where
// round(0.7 * 255) would be 178.
int PlateByte(float ink);

// The 16-bit color value gs gives `ink` on a device (frac2cv of the frac):
// PlateByte's high byte.
uint16_t PlateColorValue(float ink);

class CPDF_Plate final : public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  // A plate as asked (kPlate); a process plate filled after the render from
  // the blend channels (kDerived: nothing paints it); a channel of the page
  // group's RGB blending space (kBlendChannel: R, G, B, its gray the
  // channel's value; 3 a white K for nonseparable blends; 4 a gray group's
  // gray), as gs blends a group in its color space.
  enum class Role { kPlate, kDerived, kBlendChannel };

  // Whether it is a spot's, not one of the four process plates.
  bool IsSpot() const { return index_ < 0; }
  // Its CMYK component, 0 to 3, -1 a spot's
  int process_index() const { return index_; }

  // The gray byte `ink` puts on the plate, 255 none: 255 - its byte, but a
  // blend channel's the byte of its value (1 - ink), as gs quantizes an
  // additive color
  int GrayByte(float ink) const {
    return role_ == Role::kBlendChannel ? PlateByte(1.0f - ink)
                                        : 255 - PlateByte(ink);
  }

  // The 16-bit device value gs gives `ink` on the plate: of the ink, a
  // blend channel's of its value (1 - ink); and the gray byte such a value
  // puts on the plate, as GrayByte() does
  uint16_t ColorValue(float ink) const {
    return PlateColorValue(role_ == Role::kBlendChannel ? 1.0f - ink : ink);
  }
  // The 16-bit device value gs gives `comps` in `cs` on the plate as a
  // shading's vertex: ColorValue(Ink()), but a process space with no output
  // intent through gs's 16-bit link (-dUseFastColor's, the identity for
  // CMYK): the components as 16-bit values, the plate's frac of them.
  uint16_t ColorValue(const CPDF_ColorSpace* cs,
                      pdfium::span<const float> comps,
                      int intent) const;
  int GrayOfValue(uint16_t cv) const {
    return role_ == Role::kBlendChannel ? cv >> 8 : 255 - (cv >> 8);
  }

  // Whether anything painted in `cs` paints the plate, its ink or none: a
  // Separation or DeviceN only its own colorants, any process space all four
  // process plates, as gs separates them under overprint.
  bool Marks(const CPDF_ColorSpace* cs) const;

  // The ink `comps` in `cs` put on the plate, 0 to 1, rendered with
  // `intent` (as CPDF_GeneralState::GetRenderIntent()). Nothing when the
  // plate is left as it was: a None colorant always; with `overprint` a plate
  // the space does not mark, and with `opm` 1 a DeviceCMYK component at 0.
  std::optional<float> Ink(const CPDF_ColorSpace* cs,
                           pdfium::span<const float> comps,
                           bool overprint,
                           int opm,
                           int intent) const;

  // The intent named `name` (an image's /Intent), as Ink() takes it
  static int Intent(const ByteString& name);

 private:
  struct Transform;

  CPDF_Plate(const CPDF_Document* doc,
             const ByteString& name,
             Role role = Role::kPlate,
             int channel = -1);
  ~CPDF_Plate() override;

  // The ink of a blend channel plate: 1 - the channel's value, the color as
  // gs takes it into an RGB group with -dUseFastColor (gray as R = G = B,
  // CMYK as 1 - min(1, C + K) where gs has a bug, other spaces as RGB)
  float BlendChannelInk(const CPDF_ColorSpace* cs,
                        pdfium::span<const float> comps) const;

  // `comps` in `cs` as CMYK through the output intent; nothing for a space
  // that does not go through it, or with no output intent.
  std::optional<std::array<float, 4>> ToCmyk(const CPDF_ColorSpace* cs,
                                             pdfium::span<const float> comps,
                                             int intent) const;

  const ByteString name_;
  const Role role_;
  // the process plate's CMYK component, -1 a spot's (a derived plate's too);
  // a blend channel's channel
  const int index_;
  DataVector<uint8_t> output_;  // the output intent's CMYK profile
  // gs's default CMYK profile (PDFKING_SetBlendProfiles), Lab's without an
  // output intent
  DataVector<uint8_t> default_cmyk_;
  // per source profile (null for sRGB, the Lab space's for Lab) and intent
  mutable std::map<std::pair<const void*, int>, std::unique_ptr<Transform>>
      transforms_;
};

// The CMYK profile of `doc`'s output intent, empty for none
DataVector<uint8_t> OutputIntentProfile(const CPDF_Document* doc);

// The plate `doc`'s pages render as, null for color.
const CPDF_Plate* GetPlate(const CPDF_Document* doc);

// The gray, 0 to 1, of the 8-bit ink `byte`: 255 - byte for whichever reads
// it, truncating (images) or rounding (shadings).
float PlateGray(int byte);

// `base` read as the gray of `plate`, rendered with `intent`: images and
// shadings drawn through it come out as the plate does.
RetainPtr<CPDF_ColorSpace> PlateColorSpace(RetainPtr<CPDF_ColorSpace> base,
                                           const CPDF_Plate* plate,
                                           int intent);

// What an object filled (or stroked) in `color` with `state` puts on
// `plate`: its gray, the opacity rounded as gs composites it (.25 is 64/255,
// not 63), 0 where the plate is left as it was, through the state's
// transfer function. Nothing off a plate or for a pattern but an uncolored
// tiling one (its color's).
// The transfer function /TR `tr` names as it maps `plate`'s gray (as gs
// maps the colorant, PlateArgb's), for an image's samples on the plate.
RetainPtr<CPDF_TransferFunc> PlateTransferFunc(const CPDF_Object* tr,
                                               const CPDF_Plate* plate);

std::optional<FX_ARGB> PlateArgb(const CPDF_Plate* plate,
                                 const CPDF_Color* color,
                                 const CPDF_GeneralState& state,
                                 bool stroke);

// `mode` on `plate`: on a spot's the nonseparable modes are Normal (ISO
// 32000-2 11.7.4.2: they apply to process colorants alone), and so, as gs
// blends a spot, are Difference and Exclusion.
BlendMode PlateBlendMode(BlendMode mode, const CPDF_Plate* plate);

#endif  // CORE_FPDFAPI_PAGE_CPDF_PLATE_H_
