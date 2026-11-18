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

// The plate (Cyan, Magenta, Yellow, Black or a spot's name) a document's
// pages render as, with the document's output intent: colors in an ICC
// based space, RGB or Lab go on the process plates through it, as a RIP
// separates them (relative colorimetric, black point compensated). Without
// one they go on as gs -dUseFastColor puts them (C = 1 - R, no black).
class CPDF_Plate final : public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  // Whether it is a spot's, not one of the four process plates.
  bool IsSpot() const { return index_ < 0; }
  // Its CMYK component, 0 to 3, -1 a spot's
  int process_index() const { return index_; }

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

  CPDF_Plate(const CPDF_Document* doc, const ByteString& name);
  ~CPDF_Plate() override;

  // `comps` in `cs` as CMYK through the output intent; nothing for a space
  // that does not go through it, or with no output intent.
  std::optional<std::array<float, 4>> ToCmyk(const CPDF_ColorSpace* cs,
                                             pdfium::span<const float> comps,
                                             int intent) const;

  const ByteString name_;
  const int index_;  // the process plate's CMYK component, -1 a spot's
  DataVector<uint8_t> output_;  // the output intent's CMYK profile
  // per source profile (null for sRGB, the Lab space's for Lab) and intent
  mutable std::map<std::pair<const void*, int>, std::unique_ptr<Transform>>
      transforms_;
};

// The plate `doc`'s pages render as, null for color.
const CPDF_Plate* GetPlate(const CPDF_Document* doc);

// The 8-bit ink of `ink` as gs puts it on a device: a frac (15 bits, 32760
// full) rounded, its 16-bit color value, the high byte. 0.7 is 179 where
// round(0.7 * 255) would be 178.
int PlateByte(float ink);

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
// not 63), 0 where the plate is left as it was. Nothing off a plate or for a
// pattern.
std::optional<FX_ARGB> PlateArgb(const CPDF_Plate* plate,
                                 const CPDF_Color* color,
                                 const CPDF_GeneralState& state,
                                 bool stroke);

// `mode` on `plate`: on a spot's the nonseparable modes are Normal (ISO
// 32000-2 11.7.4.2: they apply to process colorants alone), and so, as gs
// blends a spot, are Difference and Exclusion.
BlendMode PlateBlendMode(BlendMode mode, const CPDF_Plate* plate);

#endif  // CORE_FPDFAPI_PAGE_CPDF_PLATE_H_
