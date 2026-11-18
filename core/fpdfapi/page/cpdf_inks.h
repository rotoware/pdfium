// pdfking: the separations a page renders to at once, one 8-bit gray plane
// each, white no ink, black full ink.

#ifndef CORE_FPDFAPI_PAGE_CPDF_INKS_H_
#define CORE_FPDFAPI_PAGE_CPDF_INKS_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/dib/fx_dib.h"

class CFX_RenderDevice;
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

  std::vector<RetainPtr<const CPDF_Plate>> plates_;
  size_t asked_ = 0;
  std::vector<std::unique_ptr<CPDF_PageImageCache>> image_caches_;
  UnownedPtr<CFX_RenderDevice> device_;
  std::vector<UnownedPtr<CFX_RenderDevice>> planes_;
  // each color: one FX_ARGB per plate, as PlateArgb() puts it on the plate
  std::vector<std::vector<FX_ARGB>> colors_;
  std::map<std::vector<FX_ARGB>, uint32_t> indices_;
  bool failed_ = false;
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
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_INKS_H_
