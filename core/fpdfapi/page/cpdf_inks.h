// pdfking: the separations a page renders to at once, one 8-bit gray plane
// each, white no ink, black full ink.

#ifndef CORE_FPDFAPI_PAGE_CPDF_INKS_H_
#define CORE_FPDFAPI_PAGE_CPDF_INKS_H_

#include <stdint.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/dib/fx_dib.h"

class CFX_DIBitmap;
class CFX_RenderDevice;
class CPDF_Dictionary;
class CPDF_Color;
class CPDF_Document;
class CPDF_GeneralState;
class CPDF_Page;
class CPDF_PageObjectHolder;
class CPDF_PageImageCache;

// The plates asked for, in order, of a page rendered (in bands) to them, and
// the colors of its renders: each an array of one ink per plate (or none:
// the plate left as it was) with an opacity.
// A color reaches the ink device as a handle in the FX_ARGB the renderer
// already threads: its opacity, a tag, an index into this table. Nothing
// reads it as RGB; the ink device turns it into each plane's gray.
class CPDF_Inks final : public Retainable {
 public:
  CONSTRUCT_VIA_MAKE_RETAIN;

  // The plates rendered: those asked, then any process plate not asked when
  // the page blends nonseparably (Hue, Saturation, Color, Luminosity: they
  // mix the process plates), so that a plate is the same whichever are asked
  size_t size() const { return plates_.size(); }
  size_t asked() const { return asked_; }
  const CPDF_Plate* plate(size_t i) const { return plates_[i].Get(); }

  // The ink device a render draws on, and its planes, one per plate; none
  // between renders
  void SetDevices(CFX_RenderDevice* device,
                  std::vector<CFX_RenderDevice*> planes);
  CFX_RenderDevice* device() const { return device_; }
  CFX_RenderDevice* plane(size_t i) const { return planes_[i]; }

  // `color` filled (or stroked) with `state` as a handle; 0 when it leaves
  // every plate as it was.
  FX_ARGB Handle(const CPDF_Color* color,
                 const CPDF_GeneralState& state,
                 bool stroke);

  // Whether `handle` paints every plane with the same opacity: what decides
  // how a path or text is drawn (fill and stroke, a rectangle, a line) is
  // then the same on each, and the ink device draws it once for all.
  bool IsUniform(FX_ARGB handle) const;

  // `handle` on plane `i`: its gray and opacity, transparent where it leaves
  // the plate. Nothing for a value that is not a handle of this render.
  std::optional<FX_ARGB> Resolve(FX_ARGB handle, size_t i) const;

  // Whether the page group blends in RGB (gs pushes it: the page's
  // resources have transparency): the process plates asked are then
  // derived, painted by ConvertBlendSpace() from the blend channel plates
  bool blends_in_rgb() const { return !!blend_; }

  // The process planes asked, from the blend channels' (R, G, B) through
  // the blend profiles, as gs converts its page group to the device
  void ConvertBlendSpace() const;

  // The render's device resolution, as gs's -r: glyphs are sized from it
  float dpi() const { return dpi_; }
  // Whether gs takes the page as having transparency (its pdf14 device)
  bool page_transparent() const { return page_transparent_; }
  void set_dpi(float dpi) { dpi_ = dpi; }
  // Where the page's top left is on the device (a band offsets it)
  const CFX_PointF& page_origin() const { return page_origin_; }
  void set_page_origin(const CFX_PointF& origin) { page_origin_ = origin; }

  // Whether an isolated group `form` blends in RGB and the inks can do as
  // gs: render it on the blend channels (SwapChannels), then ConvertGroup
  bool GroupBlendsInRgb(const CPDF_Dictionary* form) const;
  // The process plates swapped with blend channel plates (R, G, B, a white
  // K), or back, image caches with them
  void SwapChannels();
  // A group's planes, BGRA, rendered on the blend channels, to CMYK in
  // place, alpha kept, through its profile (`form`'s group's) as gs
  // converts a group's color space to its parent's
  void ConvertGroup(const CPDF_Dictionary* form,
                    const std::vector<RetainPtr<CFX_DIBitmap>>& bitmaps);
  // The same for an isolated group blending in DeviceGray: rendered on a
  // gray channel (SwapGrayChannels), then to CMYK (ConvertGrayGroup)
  bool GroupBlendsInGray(const CPDF_Dictionary* form) const;
  void SwapGrayChannels();
  void ConvertGrayGroup(const std::vector<RetainPtr<CFX_DIBitmap>>& bitmaps);

  // The page's images as decoded for `plate`, kept across renders
  CPDF_PageImageCache* ImageCache(const CPDF_Plate* plate) const;

  // A value reached the ink device that is not a handle: the render is
  // wrong somewhere, its planes are not to be trusted.
  void Fail() { failed_ = true; }
  bool failed() const { return failed_; }
  void ClearFailed() { failed_ = false; }

 private:
  CPDF_Inks(CPDF_Page* page, const std::vector<ByteString>& names);
  ~CPDF_Inks() override;

  void InitBlend(CPDF_Page* page,
                 const std::vector<ByteString>& names,
                 const DataVector<uint8_t>& group_profile);

  std::vector<RetainPtr<const CPDF_Plate>> plates_;
  size_t asked_ = 0;
  UnownedPtr<const CPDF_Document> page_doc_;
  std::vector<std::unique_ptr<CPDF_PageImageCache>> image_caches_;
  UnownedPtr<CFX_RenderDevice> device_;
  std::vector<UnownedPtr<CFX_RenderDevice>> planes_;
  // each color: one FX_ARGB per plate, as PlateArgb() puts it on the plate
  std::vector<std::vector<FX_ARGB>> colors_;
  std::map<std::vector<FX_ARGB>, uint32_t> indices_;
  bool failed_ = false;
  CFX_PointF page_origin_;
  float dpi_ = 0;
  bool page_transparent_ = false;
  // an RGB page group's conversion to the plates, null for none
  struct Blend;
  std::unique_ptr<Blend> blend_;
  // RGB form groups: the blend channel plates and their image caches (in
  // plates_ while swapped), the process plates' indices, the transforms
  // per group profile (null: the default RGB one)
  std::vector<RetainPtr<const CPDF_Plate>> channel_plates_;
  std::vector<std::unique_ptr<CPDF_PageImageCache>> channel_caches_;
  // gray form groups: the gray channel and three white ones, the same way,
  // and the gray to CMYK link
  std::vector<RetainPtr<const CPDF_Plate>> gray_plates_;
  std::vector<std::unique_ptr<CPDF_PageImageCache>> gray_caches_;
  std::unique_ptr<Blend> gray_link_;
  std::array<size_t, 4> process_ = {};
  std::map<const void*, std::unique_ptr<Blend>> group_blends_;
};

// Whether an object of `holder` (or of a form in it) blends nonseparably
// (Hue, Saturation, Color, Luminosity)
bool BlendsNonSeparably(const CPDF_PageObjectHolder* holder);

// The spot inks `page` paints (or may: what its resources name), in the
// order found: the colorants of every Separation and DeviceN in its
// resources, its forms', patterns', shadings', images', Type3 fonts' and its
// annotations' appearances, but the process inks, All and None. As gs
// finds them before rendering (pdfi_check_page).
std::vector<ByteString> PageSpotInks(const CPDF_Page* page);

// The inks `doc` renders to, null when it does not.
CPDF_Inks* GetInks(const CPDF_Document* doc);

// The inks of `doc`'s ink render, also in a color render inside it
// (ScopedColorRender: a soft mask, a shape): what is rasterized as gs does.
// Null outside an ink render.
CPDF_Inks* GetGsInks(const CPDF_Document* doc);

// The image cache a render of `doc` uses: on inks the plate's (none off a
// plate), else `page_cache`.
CPDF_PageImageCache* SeparationImageCache(const CPDF_Document* doc,
                                          CPDF_PageImageCache* page_cache);

// Pages of `doc` render in color (no plate, no inks) while it lives: a soft
// mask is measured in its colors.
class ScopedColorRender {
 public:
  explicit ScopedColorRender(const CPDF_Document* doc);
  ~ScopedColorRender();

 private:
  UnownedPtr<const CPDF_Document> doc_;
  RetainPtr<const CPDF_Plate> plate_;
  RetainPtr<CPDF_Inks> inks_;
  RetainPtr<CPDF_Inks> color_render_inks_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_INKS_H_
