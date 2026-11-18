// pdfking: one press plate rendered as gray, white no ink, black full ink.

#ifndef CORE_FPDFAPI_PAGE_CPDF_PLATE_H_
#define CORE_FPDFAPI_PAGE_CPDF_PLATE_H_

#include <optional>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"

class CPDF_ColorSpace;

// Whether `plate` is a spot's, not one of the four process plates.
bool PlateIsSpot(const ByteString& plate);

// Whether anything painted in `cs` paints `plate` (Cyan, Magenta, Yellow,
// Black or a spot's name), its ink or none: a Separation or DeviceN only its
// own colorants, any process space (CMYK, gray, RGB, the CIE spaces) all four
// process plates, as gs separates them under overprint.
bool PlateMarked(const CPDF_ColorSpace* cs, const ByteString& plate);

// The ink `comps` in `cs` put on `plate`, 0 to 1: process colors as they
// are, gray on Black, RGB and the CIE spaces as gs -dUseFastColor separates
// them (C = 1 - R, M = 1 - G, Y = 1 - B, no black), a spot on its own plate
// alone. Nothing when the plate is left as it was: a None colorant always;
// with `overprint` a plate the space does not mark, and with `opm` 1 a
// DeviceCMYK component at 0.
std::optional<float> PlateInk(const CPDF_ColorSpace* cs,
                              pdfium::span<const float> comps,
                              const ByteString& plate,
                              bool overprint,
                              int opm);

// The 8-bit ink of `ink` as gs puts it on a device: a frac (15 bits, 32760
// full) rounded, its 16-bit color value, the high byte. 0.7 is 179 where
// round(0.7 * 255) would be 178.
int PlateByte(float ink);

// The gray, 0 to 1, of the 8-bit ink `byte`: 255 - byte for whichever reads
// it, truncating (images) or rounding (shadings).
float PlateGray(int byte);

// `base` read as the gray of `plate`: images and shadings drawn through it
// come out as the plate does.
RetainPtr<CPDF_ColorSpace> PlateColorSpace(RetainPtr<CPDF_ColorSpace> base,
                                           const ByteString& plate);

#endif  // CORE_FPDFAPI_PAGE_CPDF_PLATE_H_
