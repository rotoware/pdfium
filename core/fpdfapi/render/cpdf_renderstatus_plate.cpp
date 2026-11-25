// pdfking: CPDF_RenderStatus on a plate or on inks, and the groups it
// composites as gs does (knockout, the page showing through).
// cpdf_renderstatus.cpp only calls in here, so a rebase touches one line per
// hook.

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_inks.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fpdfapi/page/cpdf_shadingobject.h"
#include "core/fpdfapi/page/cpdf_shadingpattern.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_tilingpattern.h"
#include "core/fpdfapi/render/cpdf_gstiling.h"
#include "core/fpdfapi/render/cpdf_inkblend.h"
#include "core/fpdfapi/render/cpdf_inkdriver.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"
#include "core/fpdfapi/render/cpdf_textrenderer.h"
#include "core/fxcrt/autorestorer.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/blend.h"
#include "core/fxge/dib/cfx_dibitmap.h"

namespace {

// The pixels `box` of `dst` from `src`, whose (0, 0) is (x, y) of `dst`
void CopyPixels(const CFX_DIBitmap& src,
                int x,
                int y,
                CFX_DIBitmap& dst,
                const FX_RECT& box) {
  const size_t bytes = dst.GetBPP() / 8;
  for (int row = box.top; row < box.bottom; ++row) {
    fxcrt::Copy(src.GetScanline(row - y).subspan((box.left - x) * bytes,
                                                 box.Width() * bytes),
                dst.GetWritableScanline(row).subspan(box.left * bytes,
                                                     box.Width() * bytes));
  }
}

// The pixels of `device`'s bitmap `form` covers, clipped
FX_RECT GroupRect(CFX_RenderDevice* device,
                  const CPDF_FormObject* form,
                  const CFX_Matrix& mtObj2Device) {
  FX_RECT rect = form->GetTransformedBBox(mtObj2Device);
  rect.Intersect(device->GetClipBox());
  rect.Intersect(FX_RECT(0, 0, device->GetWidth(), device->GetHeight()));
  return rect;
}

// An opaque plate's gray `gray` of a page pixel of opacity `alpha` (gs's page
// group, flattened on white) as gs holds it: the gray of its ink before it
// was flattened
int Unflatten(int gray, int alpha) {
  if (alpha == 0 || alpha == 255) {
    return gray;
  }
  return 255 - std::min(255, ((255 - gray) * 255 + alpha / 2) / alpha);
}

// The gray `gray` at opacity `alpha` on the plate as gs puts its page group
// on the device: its ink over no ink (gx_blend_image_buffer)
int Flatten(int gray, int alpha) {
  const int ink = 255 - gray;
  const int tmp = -ink * (255 - alpha) + 0x80;
  return 255 - (ink + ((tmp + (tmp >> 8)) >> 8));
}

}  // namespace

// A layer ProcessTransparency made of an object, left uncomposited
struct CPDF_InkLayer {
  RetainPtr<CFX_DIBitmap> bitmap;  // BGRA, the plate's gray, its alpha
  FX_RECT rect;                    // where on the device
};

// What `color` filled (or stroked) with `state` paints: on a plate its gray,
// on inks a handle of its inks; nothing in color.
std::optional<FX_ARGB> CPDF_RenderStatus::SeparationArgb(
    const CPDF_Color* color,
    const CPDF_GeneralState& state,
    bool stroke) const {
  if (CPDF_Inks* inks = Inks()) {
    if (!color || color->IsPattern()) {
      return std::nullopt;
    }
    return inks->Handle(color, state, stroke);
  }
  // a soft mask's colors and opacity as gs quantizes them (frac, rounded)
  if ((load_mask_ || options_.ColorModeIs(CPDF_RenderOptions::kAlpha)) &&
      color && !color->IsPattern() && color->GetColorSpace()) {
    std::optional<FX_RGB_STRUCT<float>> rgb =
        color->GetColorSpace()->GetRGB(color->GetComponents());
    if (rgb.has_value()) {
      const float alpha =
          stroke ? state.GetStrokeAlpha() : state.GetFillAlpha();
      return ArgbEncode(FXSYS_roundf(alpha * 255), PlateByte(rgb->red),
                        PlateByte(rgb->green), PlateByte(rgb->blue));
    }
  }
  return PlateArgb(GetPlate(context_->GetDocument()), color, state, stroke);
}

// The inks this status draws on, when its device is the ink device (not a
// plane of it, nor any bitmap of the render)
CPDF_Inks* CPDF_RenderStatus::Inks() const {
  CPDF_Inks* inks = GetInks(context_->GetDocument());
  return inks && !GetPlate(context_->GetDocument()) &&
                 inks->device() == device_.get()
             ? inks
             : nullptr;
}

// `obj` drawn as separations need, when they differ from the color render:
// on inks (InkPlanes), or overprinting a plate it leaves in a group
// (PlateOverprintsBeneath). Whether it was.
bool CPDF_RenderStatus::SeparateObject(CPDF_PageObject* obj,
                                       const CFX_Matrix& mtObj2Device) {
  return InkPlanes(obj, mtObj2Device) ||
         PlateOverprintsBeneath(obj, mtObj2Device);
}

// Whether `obj` overprints and leaves the plate in all it paints
bool CPDF_RenderStatus::PlateLeavesObject(CPDF_PageObject* obj) const {
  const CPDF_Plate* plate = GetPlate(context_->GetDocument());
  const CPDF_GeneralState& state = obj->general_state();
  if (!plate) {
    return false;
  }
  auto leaves = [&](const CPDF_Color* color, bool overprint) {
    return overprint && color && !color->IsPattern() &&
           !plate
                ->Ink(color->GetColorSpace(), color->GetComponents(), true,
                      state.GetOPMode(), state.GetRenderIntent())
                .has_value();
  };
  switch (obj->GetType()) {
    case CPDF_PageObject::Type::kPath: {
      const CPDF_PathObject* path = obj->AsPath();
      const bool fill =
          path->filltype() != CFX_FillRenderOptions::FillType::kNoFill;
      return (fill || path->stroke()) &&
             (!fill ||
              leaves(obj->color_state().GetFillColor(), state.GetFillOP())) &&
             (!path->stroke() ||
              leaves(obj->color_state().GetStrokeColor(), state.GetStrokeOP()));
    }
    case CPDF_PageObject::Type::kText:
      return leaves(obj->color_state().GetFillColor(), state.GetFillOP()) &&
             leaves(obj->color_state().GetStrokeColor(), state.GetStrokeOP());
    case CPDF_PageObject::Type::kImage:
      return PlateLeavesImage(obj->AsImage());
    case CPDF_PageObject::Type::kShading:
      return state.GetFillOP() &&
             !plate->Marks(obj->AsShading()->pattern()->GetCS().Get());
    case CPDF_PageObject::Type::kForm:
      return false;
  }
}

// In a group (a bitmap with alpha), an object overprinting a plate it leaves
// still composites there: ISO 32000-2 11.7.4.3, CompatibleOverprint, B(cb,
// cs) = cb. The group keeps its color, taking the object's shape and
// opacity: the backdrop (the group's for a group the page shows through,
// none, no ink, for an isolated one) painted beneath it, as gs does. It
// shows once the group is composited isolated or in a blend; on the page
// leaving the plate is all. Whether it did.
bool CPDF_RenderStatus::PlateOverprintsBeneath(CPDF_PageObject* obj,
                                               const CFX_Matrix& mtObj2Device) {
  RetainPtr<CFX_DIBitmap> device = device_->GetBitmap();
  if (!device || device->GetFormat() != FXDIB_Format::kBgra ||
      !PlateLeavesObject(obj)) {
    return false;
  }
  FX_RECT rect = obj->GetTransformedBBox(mtObj2Device);
  rect.Intersect(device_->GetClipBox());
  if (rect.IsEmpty()) {
    return true;
  }
  // its shape and opacity: drawn in color on a clear layer, in its clip
  auto layer = pdfium::MakeRetain<CFX_DIBitmap>();
  std::unique_ptr<CFX_RenderDevice> layer_device =
      layer->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)
          ? CFX_RenderDevice::CreateForBitmap(layer)
          : nullptr;
  if (!layer_device) {
    return true;
  }
  CFX_Matrix layer_matrix = mtObj2Device;
  layer_matrix.Translate(-rect.left, -rect.top);
  {
    ScopedColorRender color_render(context_->GetDocument());
    CPDF_RenderStatus shape(context_, layer_device.get());
    shape.SetOptions(options_);
    shape.SetFormResource(form_resource_);
    shape.SetInGroup(true);
    shape.Initialize(this, nullptr);
    shape.RenderSingleObject(obj, layer_matrix);
  }
  RetainPtr<const CFX_DIBitmap> backdrop = device_->GetBackDrop();
  for (int row = 0; row < rect.Height(); ++row) {
    pdfium::span<const uint8_t> shape = layer->GetScanline(row);
    pdfium::span<uint8_t> dest =
        device->GetWritableScanline(rect.top + row)
            .subspan(static_cast<size_t>(rect.left) * 4);
    pdfium::span<const uint8_t> back =
        backdrop ? backdrop->GetScanline(rect.top + row)
                 : pdfium::span<const uint8_t>();
    const int back_bytes = backdrop ? backdrop->GetBPP() / 8 : 0;
    for (int col = 0; col < rect.Width(); ++col) {
      const int s = shape[col * 4 + 3];
      if (!s) {
        continue;
      }
      const int a = dest[col * 4 + 3];
      // the backdrop's gray, where it is transparent (an isolated group not
      // painted there yet) no ink, as gs clears a group's buffer
      int b0 = 255;
      if (backdrop) {
        const int gray = back[(rect.left + col) * back_bytes];
        const int alpha =
            back_bytes == 4 ? back[(rect.left + col) * back_bytes + 3] : 255;
        b0 = 255 - ((255 - gray) * alpha + 127) / 255;
      }
      // union, gs's rounding
      const int tmp = (255 - a) * (255 - s) + 0x80;
      const int a2 = 255 - (((tmp >> 8) + tmp) >> 8);
      if (a2 == a) {
        continue;
      }
      const int g = (a * dest[col * 4] + (a2 - a) * b0 + a2 / 2) / a2;
      dest[col * 4] = dest[col * 4 + 1] = dest[col * 4 + 2] =
          static_cast<uint8_t>(g);
      dest[col * 4 + 3] = static_cast<uint8_t>(a2);
    }
  }
  return true;
}

// This status on `plane`, as the same status: its clip is the ink device's
std::unique_ptr<CPDF_RenderStatus> CPDF_RenderStatus::PlaneStatus(
    CFX_RenderDevice* plane) {
  auto status = std::make_unique<CPDF_RenderStatus>(context_, plane);
  status->options_ = options_;
  status->form_resource_ = form_resource_;
  status->page_resource_ = page_resource_;
  status->type3_font_cache_ = type3_font_cache_;
  status->device_matrix_ = device_matrix_;
  status->last_clip_path_ = last_clip_path_;
  status->cur_obj_ = cur_obj_;
  status->stop_obj_ = stop_obj_;
  status->initial_states_ = initial_states_;
  status->type3_char_ = type3_char_;
  status->transparency_ = transparency_;
  status->drop_objects_ = drop_objects_;
  status->load_mask_ = load_mask_;
  status->in_group_ = in_group_;
  status->group_family_ = group_family_;
  status->t3_fill_color_ = t3_fill_color_;
  return status;
}

// On inks, `obj` drawn a plane at a time, each plane as its plate, unless the
// ink device draws it at once (InkAtOnce): what a plate does to an image, a
// shading, a pattern, a group, a soft mask or a blend is done as on the
// plate alone; a nonseparable blend on all process plates together
// (InkNonSeparable). Whether it did.
bool CPDF_RenderStatus::InkPlanes(CPDF_PageObject* obj,
                                  const CFX_Matrix& mtObj2Device) {
  CPDF_Inks* inks = Inks();
  if (!inks || InkAtOnce(obj)) {
    return false;
  }
  if (IsNonSeparable(obj->general_state().GetBlendType())) {
    InkNonSeparable(obj, mtObj2Device);
    return true;
  }
  if (obj->IsForm() && obj->AsForm()->form()->GetTransparency().IsIsolated() &&
      (BlendsNonSeparably(obj->AsForm()->form()) ||
       inks->GroupBlendsInRgb(obj->AsForm()->form()->GetDict().Get()) ||
       inks->GroupBlendsInGray(obj->AsForm()->form()->GetDict().Get()))) {
    InkGroup(obj, mtObj2Device);
    return true;
  }
  CPDF_DocPageData* data =
      CPDF_DocPageData::FromDocument(context_->GetDocument());
  for (size_t i = 0; i < inks->size() && !stopped_; ++i) {
    data->SetPlate(pdfium::WrapRetain(inks->plate(i)));
    std::unique_ptr<CPDF_RenderStatus> status = PlaneStatus(inks->plane(i));
    if (!status->ProcessTransparency(obj, mtObj2Device)) {
      status->ProcessObjectNoClip(obj, mtObj2Device);
    }
    stopped_ = status->stopped_;
  }
  data->SetPlate(nullptr);
  return true;
}

// On inks, `obj` in a nonseparable blend mode (Hue, Saturation, Color,
// Luminosity): its layer made on each process plate as the plate makes it
// (ProcessTransparency, kept by KeepInkLayer), then blended with the four
// process planes at once and composited, as gs does; on a spot plate it is
// Normal (PlateBlendMode), drawn as on the plate alone (in a knockout group as
// K, as gs blends one). The inks always have
// the four process plates when the page has such a blend (CPDF_Inks).
void CPDF_RenderStatus::InkNonSeparable(CPDF_PageObject* obj,
                                        const CFX_Matrix& mtObj2Device) {
  CPDF_Inks* inks = Inks();
  CPDF_DocPageData* data =
      CPDF_DocPageData::FromDocument(context_->GetDocument());
  std::array<CPDF_InkLayer, 4> layers;
  std::array<CFX_RenderDevice*, 4> planes = {};
  for (size_t i = 0; i < inks->size() && !stopped_; ++i) {
    const CPDF_Plate* plate = inks->plate(i);
    data->SetPlate(pdfium::WrapRetain(plate));
    std::unique_ptr<CPDF_RenderStatus> status = PlaneStatus(inks->plane(i));
    const int process = plate->process_index();
    if (process >= 0 && process < 4) {
      planes[process] = inks->plane(i);
      status->ink_layer_ = &layers[process];
    } else if (in_knockout_ &&
               obj->general_state().GetBlendType() != BlendMode::kLuminosity) {
      // gs blends a knockout group's spots as its K (art_blend_pixel_8 on
      // every channel): Hue, Saturation and Color leave them as they were
      continue;
    }
    if (!status->ProcessTransparency(obj, mtObj2Device)) {
      status->ProcessObjectNoClip(obj, mtObj2Device);
    }
    stopped_ = status->stopped_;
  }
  data->SetPlate(nullptr);
  if (!stopped_ && layers[0].bitmap) {
    InkCompositeLayers(layers, planes, obj->general_state().GetBlendType(),
                       PageBackdrop(obj, layers[0].rect).Get());
  }
}

// The process plates' `layers` (BGRA: each its plate's gray and alpha)
// composited on their `planes` in the nonseparable `mode`, as gs does: C, M,
// Y blended together, K as InkBlend() takes it, each mixed by its own alpha
// with the backdrop's (a group's pixels, or the page's: `page`'s alpha, as
// gs's page group has it, null for opaque).
void CPDF_RenderStatus::InkCompositeLayers(
    const std::array<CPDF_InkLayer, 4>& layers,
    const std::array<CFX_RenderDevice*, 4>& planes,
    BlendMode mode,
    const CFX_DIBitmap* page) {
  if (!planes[0] || !planes[1] || !planes[2] || !planes[3]) {
    return;
  }
  // every plate's layer covers the same pixels: the object's box, clipped
  const FX_RECT rect = layers[0].rect;
  std::array<RetainPtr<CFX_DIBitmap>, 4> results;
  std::array<RetainPtr<CFX_DIBitmap>, 4> devices;
  for (int p = 0; p < 4; ++p) {
    devices[p] = planes[p]->GetBitmap();
    results[p] = pdfium::MakeRetain<CFX_DIBitmap>();
    if (!layers[p].bitmap || layers[p].rect != rect || !devices[p] ||
        !results[p]->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)) {
      return;
    }
  }
  // what each pixel becomes, and its opacity
  std::vector<uint8_t> alphas(4 * rect.Width() * rect.Height());
  for (int row = 0; row < rect.Height(); ++row) {
    for (int col = 0; col < rect.Width(); ++col) {
      std::array<uint8_t, 4> cb;
      std::array<uint8_t, 4> ab;
      std::array<uint8_t, 4> cs;
      std::array<uint8_t, 4> as;
      std::array<uint8_t, 4> back_gray;
      bool painted = false;
      for (int p = 0; p < 4; ++p) {
        const size_t bytes = devices[p]->GetBPP() / 8;
        pdfium::span<const uint8_t> back =
            devices[p]
                ->GetScanline(rect.top + row)
                .subspan((rect.left + col) * bytes, bytes);
        // on the page, its opacity as gs's page group has it
        ab[p] = bytes == 4 ? back[3]
                : page   ? page->GetScanline(row)[col * 4 + 3]
                         : 255;
        cb[p] = bytes == 4 ? back[0] : Unflatten(back[0], ab[p]);
        back_gray[p] = back[0];
        pdfium::span<const uint8_t> src =
            layers[p].bitmap->GetScanline(row).subspan(
                static_cast<size_t>(col) * 4, 4u);
        as[p] = src[3];
        // a plate the object leaves: its backdrop
        cs[p] = as[p] ? src[0] : cb[p];
        painted |= as[p] != 0;
      }
      if (!painted) {
        continue;
      }
      const std::array<uint8_t, 4> blend = InkBlend(mode, cb, cs);
      for (int p = 0; p < 4; ++p) {
        pdfium::span<uint8_t> out =
            results[p]->GetWritableScanline(row).subspan(
                static_cast<size_t>(col) * 4, 4u);
        const uint8_t composited =
            InkComposite(cb[p], ab[p], cs[p], as[p], blend[p]);
        // on the page, put on white as gs puts its page group
        out[0] = out[1] = out[2] =
            devices[p]->GetBPP() == 32 || !as[p]
                ? (as[p] ? composited : back_gray[p])
                : static_cast<uint8_t>(
                      Flatten(composited, InkUnion(ab[p], as[p])));
        out[3] = as[p] ? 255 : 0;
        alphas[(row * rect.Width() + col) * 4 + p] =
            as[p] ? InkUnion(ab[p], as[p]) : ab[p];
      }
    }
  }
  // through each plane's clip: painted opaque, then given its opacity where
  // the clip let it through (it is opaque there now)
  for (int p = 0; p < 4; ++p) {
    planes[p]->SetDIBits(results[p], rect.left, rect.top);
    if (devices[p]->GetBPP() != 32) {
      continue;
    }
    for (int row = 0; row < rect.Height(); ++row) {
      pdfium::span<uint8_t> dest =
          devices[p]
              ->GetWritableScanline(rect.top + row)
              .subspan(static_cast<size_t>(rect.left) * 4);
      pdfium::span<const uint8_t> written = results[p]->GetScanline(row);
      for (int col = 0; col < rect.Width(); ++col) {
        if (written[col * 4 + 3] && dest[col * 4 + 3] == 255 &&
            dest[col * 4] == written[col * 4]) {
          dest[col * 4 + 3] = alphas[(row * rect.Width() + col) * 4 + p];
        }
      }
    }
  }
}

// On inks, an isolated group with a nonseparable blend in it: rendered on an
// ink device of its own (a clear plane per plate), its objects blending
// across the process planes, then composited as ProcessTransparency does,
// plane by plane as each plate (CompositeDIBitmap), or across them in a
// nonseparable mode.
void CPDF_RenderStatus::InkGroup(CPDF_PageObject* obj,
                                 const CFX_Matrix& mtObj2Device) {
  CPDF_Inks* inks = Inks();
  const CPDF_GeneralState& state = obj->general_state();
  FX_RECT rect = obj->GetTransformedBBox(mtObj2Device);
  rect.Intersect(device_->GetClipBox());
  if (rect.IsEmpty()) {
    return;
  }
  std::vector<CFX_RenderDevice*> outer;
  for (size_t i = 0; i < inks->size(); ++i) {
    outer.push_back(inks->plane(i));
  }
  std::vector<RetainPtr<CFX_DIBitmap>> bitmaps;
  std::vector<std::unique_ptr<CFX_RenderDevice>> devices;
  std::vector<CFX_RenderDevice*> group_planes;
  for (size_t i = 0; i < inks->size(); ++i) {
    bitmaps.push_back(pdfium::MakeRetain<CFX_DIBitmap>());
    if (!bitmaps.back()->Create(rect.Width(), rect.Height(),
                                FXDIB_Format::kBgra)) {
      return;
    }
    devices.push_back(CFX_RenderDevice::CreateForBitmap(bitmaps.back()));
    if (!devices.back()) {
      return;
    }
    group_planes.push_back(devices.back().get());
  }
  std::unique_ptr<CFX_RenderDevice> group_device =
      CFX_RenderDevice::CreateWithDriver(std::make_unique<CPDF_InkDriver>(
          pdfium::WrapRetain(inks), group_planes));
  if (!group_device) {
    return;
  }
  CFX_Matrix group_matrix = mtObj2Device;
  group_matrix.Translate(-rect.left, -rect.top);
  CFX_RenderDevice* outer_device = inks->device();
  inks->SetDevices(group_device.get(), group_planes);
  // a group blending in RGB: its objects on R, G, B, then to CMYK, as gs
  // renders a group in its color space and converts it to its parent's
  const CPDF_Dictionary* form_dict = obj->AsForm()->form()->GetDict().Get();
  const bool rgb = inks->GroupBlendsInRgb(form_dict);
  const bool gray = !rgb && inks->GroupBlendsInGray(form_dict);
  if (rgb) {
    inks->SwapChannels();
  } else if (gray) {
    inks->SwapGrayChannels();
  }
  {
    CPDF_RenderStatus group(context_, group_device.get());
    group.SetOptions(options_);
    group.SetStopObject(stop_obj_);
    group.SetDropObjects(drop_objects_);
    group.SetFormResource(
        obj->AsForm()->form()->GetDict()->GetDictFor("Resources"));
    group.SetInGroup(true);
    group.Initialize(nullptr, nullptr);
    group.ProcessObjectNoClip(obj, group_matrix);
    stopped_ = group.stopped_;
  }
  if (rgb) {
    inks->SwapChannels();
    inks->ConvertGroup(form_dict, bitmaps);
  } else if (gray) {
    inks->SwapGrayChannels();
    inks->ConvertGrayGroup(bitmaps);
  }
  inks->SetDevices(outer_device, outer);
  RetainPtr<CPDF_Dictionary> smask_dict =
      obj->mutable_general_state().GetMutableSoftMask();
  RetainPtr<CFX_DIBitmap> mask =
      smask_dict ? LoadSMask(smask_dict.Get(), rect,
                             *state.GetSMaskMatrix() * mtObj2Device)
                 : nullptr;
  for (auto& device : devices) {
    if (mask) {
      device->MultiplyAlphaMask(mask);
    }
    device->MultiplyAlpha(state.GetFillAlpha());
    if (initial_states_.general_state().GetFillAlpha() != 1.0f && !in_group_) {
      device->MultiplyAlpha(initial_states_.general_state().GetFillAlpha());
    }
  }
  const BlendMode mode = state.GetBlendType();
  if (IsNonSeparable(mode)) {
    std::array<CPDF_InkLayer, 4> layers;
    std::array<CFX_RenderDevice*, 4> planes = {};
    for (size_t i = 0; i < inks->size(); ++i) {
      const int process = inks->plate(i)->process_index();
      if (process >= 0 && process < 4) {
        layers[process] = {bitmaps[i], rect};
        planes[process] = outer[i];
      }
    }
    InkCompositeLayers(layers, planes, mode, PageBackdrop(obj, rect).Get());
  }
  CPDF_Transparency transparency = transparency_;
  transparency.SetGroup();
  CPDF_DocPageData* data =
      CPDF_DocPageData::FromDocument(context_->GetDocument());
  for (size_t i = 0; i < inks->size(); ++i) {
    const CPDF_Plate* plate = inks->plate(i);
    if (IsNonSeparable(mode) && !plate->IsSpot()) {
      continue;
    }
    data->SetPlate(pdfium::WrapRetain(plate));
    PlaneStatus(outer[i])->CompositeDIBitmap(bitmaps[i], rect.left, rect.top, 0,
                                             1.0f, PlateBlendMode(mode, plate),
                                             transparency);
  }
  data->SetPlate(nullptr);
}

// On a plate rendering for InkNonSeparable, the object's layer kept for it
// rather than composited. Whether it was.
bool CPDF_RenderStatus::KeepInkLayer(RetainPtr<CFX_DIBitmap> bitmap,
                                     const FX_RECT& rect) {
  if (!ink_layer_) {
    return false;
  }
  ink_layer_->bitmap = std::move(bitmap);
  ink_layer_->rect = rect;
  return true;
}

// Whether the ink device draws `obj` on every plane at once: a path or text
// in plain colors, each painting all planes with one opacity, or a form, with
// nothing a plate composites (a soft mask, a blend, opacity, an isolated
// group; a knockout group is, plane by plane: RenderKnockoutGroup); the
// form's objects decide each for itself.
bool CPDF_RenderStatus::InkAtOnce(CPDF_PageObject* obj) {
  const CPDF_GeneralState& state = obj->general_state();
  // a group the page shows through mixed back with it, on every plane
  if (obj->IsForm() && PlateMixesGroup(obj->AsForm()) &&
      initial_states_.general_state().GetFillAlpha() == 1.0f) {
    return true;
  }
  if (state.GetBlendType() != BlendMode::kNormal || state.GetSoftMask()) {
    return false;
  }
  if (!IsPrint() && obj->clip_path().HasRef() &&
      obj->clip_path().GetTextCount() > 0 && !device_->RenderCapSoftClip()) {
    return false;
  }
  bool fill = false;
  bool stroke = false;
  switch (obj->GetType()) {
    case CPDF_PageObject::Type::kForm: {
      const CPDF_Transparency& group = obj->AsForm()->form()->GetTransparency();
      return state.GetFillAlpha() == 1.0f && !group.IsIsolated() &&
             initial_states_.general_state().GetFillAlpha() == 1.0f;
    }
    case CPDF_PageObject::Type::kPath:
      fill =
          obj->AsPath()->filltype() != CFX_FillRenderOptions::FillType::kNoFill;
      stroke = obj->AsPath()->stroke();
      break;
    case CPDF_PageObject::Type::kText:
      if (obj->AsText()->text_state().GetFont()->IsType3Font()) {
        return false;
      }
      fill = stroke = true;
      break;
    case CPDF_PageObject::Type::kImage:
    case CPDF_PageObject::Type::kShading:
      return false;
  }
  // a pattern is a plate's; fill and stroke see-through, a bitmap's
  if ((fill && obj->color_state().GetFillColor()->IsPattern()) ||
      (stroke && obj->color_state().GetStrokeColor()->IsPattern())) {
    return false;
  }
  if (fill && stroke &&
      (state.GetFillAlpha() != 1.0f || state.GetStrokeAlpha() != 1.0f)) {
    return false;
  }
  CPDF_Inks* inks = Inks();
  return (!fill || inks->IsUniform(GetFillArgb(obj))) &&
         (!stroke || inks->IsUniform(GetStrokeArgb(obj)));
}

// Whether an overprinting image is in inks all off the plate, which it then
// leaves as it was. Its color space may be a resource name (inline images).
// One with a soft mask knocks out, as gs paints it.
bool CPDF_RenderStatus::PlateLeavesImage(CPDF_ImageObject* image) const {
  CPDF_Document* doc = context_->GetDocument();
  const CPDF_Plate* plate = GetPlate(doc);
  if (!plate || !image->general_state().GetFillOP()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> dict = image->GetImage()->GetDict();
  if (dict->KeyExist("SMask")) {
    return false;
  }
  RetainPtr<const CPDF_Object> cs_obj = dict->GetDirectObjectFor("ColorSpace");
  // none but an image mask's: a JPX's own, process colors
  if (!cs_obj) {
    return !dict->GetBooleanFor("ImageMask", false) && plate->IsSpot();
  }
  auto* page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_ColorSpace> cs;
  if (GetFormResource()) {
    cs = page_data->GetColorSpace(cs_obj.Get(), GetFormResource());
  }
  if (!cs) {
    cs = page_data->GetColorSpace(cs_obj.Get(), GetPageResource());
  }
  return cs && !plate->Marks(cs.Get());
}

// The bitmaps this status paints as plates: the plate's, on inks each
// plane's; none in color
std::vector<RetainPtr<CFX_DIBitmap>> CPDF_RenderStatus::PlateBitmaps() const {
  std::vector<RetainPtr<CFX_DIBitmap>> bitmaps;
  if (CPDF_Inks* inks = Inks()) {
    for (size_t i = 0; i < inks->size(); ++i) {
      bitmaps.push_back(inks->plane(i)->GetBitmap());
    }
  } else if (GetPlate(context_->GetDocument()) && device_->GetBitmap()) {
    bitmaps.push_back(device_->GetBitmap());
  }
  return bitmaps;
}

// Whether `form` is, on a plate, a group the page shows through (not
// isolated) with a soft mask or an opacity, composited Normal, or with a
// separable blend mode: RenderGroup paints it on the page and mixes it with
// what was there (MixGroup, BlendGroup), so its objects blend with the page
// under them, as gs blends them.
bool CPDF_RenderStatus::PlateMixesGroup(const CPDF_FormObject* form) const {
  if (PlateBitmaps().empty()) {
    return false;
  }
  const CPDF_GeneralState& state = form->general_state();
  const CPDF_Transparency& group = form->form()->GetTransparency();
  const BlendMode mode = state.GetBlendType();
  if (!group.IsGroup() || group.IsIsolated() || IsNonSeparable(mode)) {
    return false;
  }
  return mode != BlendMode::kNormal || state.GetSoftMask() ||
         state.GetFillAlpha() != 1.0f;
}

// `form` rendered in color on a clear layer over `rect`, in its clip, with
// its opacity and soft mask: its alpha the group's shape and opacity times
// them, what it adds to the page
RetainPtr<CFX_DIBitmap> CPDF_RenderStatus::GroupShape(
    const CPDF_FormObject* form,
    const CFX_Matrix& mtObj2Device,
    const FX_RECT& rect) {
  auto layer = pdfium::MakeRetain<CFX_DIBitmap>();
  std::unique_ptr<CFX_RenderDevice> layer_device =
      layer->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)
          ? CFX_RenderDevice::CreateForBitmap(layer)
          : nullptr;
  if (!layer_device) {
    return nullptr;
  }
  CFX_Matrix layer_matrix = mtObj2Device;
  layer_matrix.Translate(-rect.left, -rect.top);
  ScopedColorRender color_render(context_->GetDocument());
  CPDF_RenderStatus shape(context_, layer_device.get());
  shape.SetOptions(options_);
  shape.SetFormResource(form_resource_);
  shape.SetInGroup(true);
  shape.Initialize(this, nullptr);
  shape.RenderSingleObject(const_cast<CPDF_FormObject*>(form), layer_matrix);
  return layer;
}

// `form`'s objects rendered by `status`: a knockout group's each on the
// group's backdrop, a group the page shows through mixed back with the page
void CPDF_RenderStatus::RenderGroup(CPDF_RenderStatus* status,
                                    const CPDF_FormObject* form,
                                    const CFX_Matrix& matrix,
                                    const CFX_Matrix& mtObj2Device) {
  FX_RECT rect;
  std::vector<RetainPtr<CFX_DIBitmap>> backdrops;
  RetainPtr<CFX_DIBitmap> shape;
  RetainPtr<CFX_DIBitmap> page;
  if (PlateMixesGroup(form)) {
    rect = GroupRect(device_, form, mtObj2Device);
    if (rect.IsEmpty()) {
      return;
    }
    for (const auto& bitmap : PlateBitmaps()) {
      backdrops.push_back(bitmap->ClipTo(rect));
    }
    if (form->general_state().GetBlendType() != BlendMode::kNormal) {
      shape = GroupShape(form, mtObj2Device, rect);
      if (!shape) {
        return;
      }
    }
    page = PageBackdrop(form, rect);
    // its opacity is mixed in below, never on its objects too
    status->SetInGroup(true);
  }
  if (form->form()->GetTransparency().IsKnockout() &&
      (device_->GetBitmap() || status->Inks())) {
    status->RenderKnockoutGroup(form, matrix, mtObj2Device);
  } else {
    status->RenderObjectList(form->form(), matrix);
  }
  // rendered up to an object in it (a backdrop for it, GetBackdrop): the
  // group's pixels as they are in the group, not yet composited
  if (status->IsStopped()) {
    return;
  }
  if (shape) {
    BlendGroup(form, mtObj2Device, rect, backdrops, *shape, page.Get());
  } else if (!backdrops.empty()) {
    MixGroup(form, mtObj2Device, rect, backdrops, page.Get());
  }
}

// On opaque plates (the page), the page under `obj` over `rect` as gs's
// page group has it: rendered up to it in color on a clear layer, its alpha
// gs's (none where nothing painted yet: a group blends with nothing there,
// as gs's page group starts transparent and is put on white at the end).
// Null on plates with their own alpha (a group's).
RetainPtr<CFX_DIBitmap> CPDF_RenderStatus::PageBackdrop(
    const CPDF_PageObject* obj,
    const FX_RECT& rect) {
  const std::vector<RetainPtr<CFX_DIBitmap>> bitmaps = PlateBitmaps();
  if (bitmaps.empty() || bitmaps[0]->IsAlphaFormat()) {
    return nullptr;
  }
  auto backdrop = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!backdrop->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)) {
    return nullptr;
  }
  std::unique_ptr<CFX_RenderDevice> device =
      CFX_RenderDevice::CreateForBitmap(backdrop);
  if (!device) {
    return nullptr;
  }
  const CFX_Matrix matrix(1, 0, 0, 1, -rect.left, -rect.top);
  ScopedColorRender color_render(context_->GetDocument());
  context_->Render(device.get(), obj, &options_, &matrix);
  return backdrop;
}

// The group painted on the page (Cn, alpha an over the backdrop's C0, a0)
// composited with its blend mode, ISO 32000-2 11.4.8: the backdrop taken
// out, C = Cn + (Cn - C0)(a0 / ag - a0) with ag the group's own alpha, then
// C over C0 with the blend, at s = ag times its soft mask and opacity (the
// shape layer's alpha): r = a0 + s - a0 s,
// Cr = ((1 - s) a0 C0 + s ((1 - a0) C + a0 B(C0, C))) / r.
void CPDF_RenderStatus::BlendGroup(
    const CPDF_FormObject* form,
    const CFX_Matrix& mtObj2Device,
    const FX_RECT& rect,
    const std::vector<RetainPtr<CFX_DIBitmap>>& backdrops,
    const CFX_DIBitmap& shape,
    const CFX_DIBitmap* page) {
  const BlendMode mode = form->general_state().GetBlendType();
  // the opacity the group's objects were not painted with: its soft mask's
  // and constant's, to find ag from s
  const float alpha = form->general_state().GetFillAlpha();
  RetainPtr<CFX_DIBitmap> mask;
  RetainPtr<CPDF_Dictionary> smask_dict = const_cast<CPDF_FormObject*>(form)
                                              ->mutable_general_state()
                                              .GetMutableSoftMask();
  if (smask_dict) {
    mask = LoadSMask(smask_dict.Get(), rect,
                     *form->general_state().GetSMaskMatrix() * mtObj2Device);
  }
  CPDF_Inks* inks = Inks();
  const CPDF_Plate* plate = GetPlate(context_->GetDocument());
  const std::vector<RetainPtr<CFX_DIBitmap>> devices = PlateBitmaps();
  for (size_t plane = 0; plane < devices.size(); ++plane) {
    const BlendMode plane_mode =
        PlateBlendMode(mode, inks ? inks->plate(plane) : plate);
    const CFX_DIBitmap& backdrop = *backdrops[plane];
    RetainPtr<CFX_DIBitmap> device = devices[plane];
    const size_t bytes = device->GetBPP() / 8;
    const bool has_alpha = device->IsAlphaFormat();
    for (int row = 0; row < rect.Height(); ++row) {
      pdfium::span<const uint8_t> before = backdrop.GetScanline(row);
      pdfium::span<const uint8_t> layer = shape.GetScanline(row);
      pdfium::span<const uint8_t> m =
          mask ? mask->GetScanline(row) : pdfium::span<const uint8_t>();
      pdfium::span<uint8_t> after = device->GetWritableScanline(rect.top + row)
                                        .subspan(rect.left * bytes);
      for (size_t col = 0; col < static_cast<size_t>(rect.Width()); ++col) {
        const uint8_t* b = before.subspan(col * bytes).data();
        uint8_t* a = after.subspan(col * bytes).data();
        const float s = layer[col * 4 + 3] / 255.0f;
        UNSAFE_TODO({
          const float a0 = has_alpha ? b[3] / 255.0f : 1.0f;
          const float an = has_alpha ? a[3] / 255.0f : 1.0f;
          if (s == 0) {
            for (size_t i = 0; i < bytes; ++i) {
              a[i] = b[i];
            }
            continue;
          }
          // the group's own alpha: what it adds over the backdrop's
          const float w = alpha * (mask ? m[col] / 255.0f : 1.0f);
          const float ag = a0 < 1 ? (an - a0) / (1 - a0) : s / w;
          // on the page, its opacity there as gs's page group has it; a
          // mode that blends white to the source (Multiply, Darken) gives
          // the same on the white page, as gs rounds it
          const bool on_page = page && plane_mode != BlendMode::kMultiply &&
                               plane_mode != BlendMode::kDarken;
          const int page_alpha =
              on_page ? page->GetScanline(row)[col * 4 + 3] : 255;
          const float p0 = page_alpha / 255.0f;
          const float r = on_page ? p0 + s - p0 * s : a0 + s - a0 * s;
          for (size_t i = 0; i < std::min<size_t>(bytes, 3); ++i) {
            const float cn = a[i];
            const float c =
                ag > 0 ? std::clamp(cn + (cn - b[i]) * (a0 / ag - a0), 0.0f,
                                    255.0f)
                       : b[i];
            if (on_page) {
              const int c0 = Unflatten(b[i], page_alpha);
              const int blend =
                  fxge::Blend(plane_mode, c0, FXSYS_roundf(c));
              const float cr =
                  ((1 - s) * p0 * c0 + s * ((1 - p0) * c + p0 * blend)) / r;
              a[i] = static_cast<uint8_t>(Flatten(
                  std::clamp(FXSYS_roundf(cr), 0, 255), FXSYS_roundf(r * 255)));
              continue;
            }
            const float c0 = b[i];
            const int blend = fxge::Blend(plane_mode, b[i], FXSYS_roundf(c));
            const float cr =
                ((1 - s) * a0 * c0 + s * ((1 - a0) * c + a0 * blend)) / r;
            a[i] = static_cast<uint8_t>(
                std::clamp(FXSYS_roundf(cr), 0, 255));
          }
          if (has_alpha) {
            a[3] = static_cast<uint8_t>(FXSYS_roundf(r * 255));
          }
        });
      }
    }
  }
}

// A knockout group (ISO 32000-2 11.4.6.2): each object painted on the
// group's backdrop as it was when the group began, not on the objects before
// it, and the result kept where the object paints (its shape) alone. Each
// object is drawn twice, in its own box: alone on a clear layer, opaque and
// Normal, its shape; then on the backdrop put back, where its opacity and
// blend apply. Overprint is off in the group, as gs paints one.
void CPDF_RenderStatus::RenderKnockoutGroup(const CPDF_FormObject* form,
                                            const CFX_Matrix& matrix,
                                            const CFX_Matrix& mtObj2Device) {
  // the bitmaps it paints: the plate's, or on inks each plane's
  CPDF_Inks* inks = Inks();
  std::vector<RetainPtr<CFX_DIBitmap>> devices;
  for (size_t i = 0; i < (inks ? inks->size() : 1); ++i) {
    devices.push_back(inks ? inks->plane(i)->GetBitmap()
                           : device_->GetBitmap());
  }
  const FX_RECT rect = GroupRect(device_, form, mtObj2Device);
  if (rect.IsEmpty()) {
    return;
  }
  std::vector<RetainPtr<CFX_DIBitmap>> backdrops;
  std::vector<RetainPtr<CFX_DIBitmap>> groups;
  for (const auto& device : devices) {
    backdrops.push_back(device->ClipTo(rect));
    groups.push_back(device->ClipTo(rect));
    if (!backdrops.back() || !groups.back()) {
      return;
    }
  }
  CPDF_DocPageData* data =
      CPDF_DocPageData::FromDocument(context_->GetDocument());
  // blended on the pixels there (the backdrop put back), not on the page
  // rendered again up to the object, which brings back what it knocks out
  transparency_ = CPDF_Transparency();
  transparency_.SetGroup();
  for (const auto& obj : *form->form()) {
    if (obj.get() == stop_obj_) {
      stopped_ = true;
      return;
    }
    if (!obj || !obj->IsActive()) {
      continue;
    }
    // the pixels it may paint, a pixel more for its edges
    const FX_RECT bbox = obj->GetTransformedBBox(matrix);
    FX_RECT box(bbox.left - 1, bbox.top - 1, bbox.right + 1, bbox.bottom + 1);
    box.Intersect(rect);
    if (box.IsEmpty()) {
      continue;
    }
    const int x = box.left - rect.left;
    const int y = box.top - rect.top;
    CPDF_PageObject* pObj = obj.get();
    AutoRestorer<CPDF_GeneralState> restorer(&pObj->mutable_general_state());
    pObj->mutable_general_state().SetFillOP(false);
    pObj->mutable_general_state().SetStrokeOP(false);
    const CPDF_GeneralState painted_state = pObj->general_state();
    pObj->mutable_general_state().SetBlendType(BlendMode::kNormal);
    pObj->mutable_general_state().SetFillAlpha(1.0f);
    pObj->mutable_general_state().SetStrokeAlpha(1.0f);
    // its shape on each: alone on a clear layer, as the plate, where it
    // leaves any alpha
    std::vector<RetainPtr<CFX_DIBitmap>> layers;
    CFX_Matrix layer_matrix = matrix;
    layer_matrix.Translate(-box.left, -box.top);
    for (size_t i = 0; i < devices.size(); ++i) {
      auto layer = pdfium::MakeRetain<CFX_DIBitmap>();
      std::unique_ptr<CFX_RenderDevice> layer_device =
          layer->Create(box.Width(), box.Height(), FXDIB_Format::kBgra)
              ? CFX_RenderDevice::CreateForBitmap(layer)
              : nullptr;
      if (!layer_device) {
        return;
      }
      if (inks) {
        data->SetPlate(pdfium::WrapRetain(inks->plate(i)));
      }
      CPDF_RenderStatus shape(context_, layer_device.get());
      shape.SetOptions(options_);
      shape.SetFormResource(form_resource_);
      shape.SetInGroup(true);
      shape.Initialize(this, nullptr);
      shape.RenderSingleObject(pObj, layer_matrix);
      layers.push_back(std::move(layer));
    }
    if (inks) {
      data->SetPlate(nullptr);
    }
    // painted on the group's backdrop
    pObj->mutable_general_state() = painted_state;
    for (size_t i = 0; i < devices.size(); ++i) {
      CopyPixels(*backdrops[i], rect.left, rect.top, *devices[i], box);
    }
    {
      AutoRestorer<bool> knockout(&in_knockout_);
      in_knockout_ = true;
      RenderSingleObject(pObj, matrix);
    }
    if (stopped_) {
      return;
    }
    // kept in the group where it paints
    for (size_t i = 0; i < devices.size(); ++i) {
      const size_t bytes = devices[i]->GetBPP() / 8;
      for (int row = 0; row < box.Height(); ++row) {
        pdfium::span<const uint8_t> painted =
            devices[i]->GetScanline(box.top + row).subspan(box.left * bytes);
        pdfium::span<uint8_t> kept =
            groups[i]->GetWritableScanline(y + row).subspan(x * bytes);
        pdfium::span<const uint8_t> alpha = layers[i]->GetScanline(row);
        for (int col = 0; col < box.Width(); ++col) {
          if (alpha[col * 4 + 3]) {
            fxcrt::Copy(painted.subspan(col * bytes, bytes),
                        kept.subspan(col * bytes, bytes));
          }
        }
      }
    }
  }
  for (size_t i = 0; i < devices.size(); ++i) {
    CopyPixels(*groups[i], rect.left, rect.top, *devices[i], rect);
  }
}

// The group painted on the page, mixed with the page as it was by its soft
// mask and its opacity, as gs composites a non-isolated group
// (template_compose_group): its opacity (gs's 16 bits' high byte) times the
// mask, the group's own alpha (what it adds over the backdrop's) times
// that, the group's color with the backdrop taken out
// (art_pdf_recomposite_group_8) composited Normal on the backdrop
// (art_pdf_composite_pixel_alpha_8). On an opaque page the group's alpha is
// full wherever it painted, and the backdrop's that of gs's page group
// (`page`'s alpha: none where nothing painted yet), the result put on white.
void CPDF_RenderStatus::MixGroup(
    const CPDF_FormObject* form,
    const CFX_Matrix& mtObj2Device,
    const FX_RECT& rect,
    const std::vector<RetainPtr<CFX_DIBitmap>>& backdrops,
    const CFX_DIBitmap* page) {
  RetainPtr<CFX_DIBitmap> mask;
  RetainPtr<CPDF_Dictionary> smask_dict = const_cast<CPDF_FormObject*>(form)
                                              ->mutable_general_state()
                                              .GetMutableSoftMask();
  if (smask_dict) {
    mask = LoadSMask(smask_dict.Get(), rect,
                     *form->general_state().GetSMaskMatrix() * mtObj2Device);
  }
  const int alpha =
      static_cast<int>(floor(65535 * form->general_state().GetFillAlpha() +
                             0.5)) >>
      8;
  // a * b / 255 as gs rounds it
  auto mul = [](int a, int b) {
    const int tmp = a * b + 0x80;
    return (tmp + (tmp >> 8)) >> 8;
  };
  const std::vector<RetainPtr<CFX_DIBitmap>> devices = PlateBitmaps();
  for (size_t plane = 0; plane < devices.size(); ++plane) {
    const CFX_DIBitmap& backdrop = *backdrops[plane];
    RetainPtr<CFX_DIBitmap> device = devices[plane];
    const size_t bytes = device->GetBPP() / 8;
    const bool has_alpha = device->IsAlphaFormat();
    const size_t colors = std::min<size_t>(bytes, 3);
    for (int row = 0; row < rect.Height(); ++row) {
      pdfium::span<const uint8_t> before = backdrop.GetScanline(row);
      pdfium::span<uint8_t> after = device->GetWritableScanline(rect.top + row)
                                        .subspan(rect.left * bytes);
      pdfium::span<const uint8_t> m =
          mask ? mask->GetScanline(row) : pdfium::span<const uint8_t>();
      pdfium::span<const uint8_t> p =
          page ? page->GetScanline(row) : pdfium::span<const uint8_t>();
      for (size_t col = 0; col < static_cast<size_t>(rect.Width()); ++col) {
        const int pix_alpha = mask ? mul(alpha, m[col]) : alpha;
        if (pix_alpha == 255) {
          continue;
        }
        const uint8_t* b = before.subspan(col * bytes).data();
        uint8_t* a = after.subspan(col * bytes).data();
        UNSAFE_TODO({
          const int a0 = has_alpha ? b[3] : page ? p[col * 4 + 3] : 255;
          const int an = has_alpha ? a[3] : 255;
          bool painted = false;
          for (size_t i = 0; i < colors; ++i) {
            painted |= a[i] != b[i];
          }
          // the group's own alpha: what it adds over the backdrop's
          const int ag =
              !has_alpha ? (painted || a0 == 255 ? 255 : 0)
              : a0 == 255
                  ? 255
                  : 255 - ((255 - an) * 255 + (255 - a0) / 2) / (255 - a0);
          if (ag <= 0) {
            for (size_t i = 0; i < bytes; ++i) {
              a[i] = b[i];
            }
            continue;
          }
          // the page's colors as gs's page group holds them
          std::array<int, 3> c0;
          for (size_t i = 0; i < colors; ++i) {
            c0[i] = has_alpha ? b[i] : Unflatten(b[i], a0);
          }
          std::array<uint8_t, 3> src;
          for (size_t i = 0; i < colors; ++i) {
            src[i] = a[i];
          }
          if (has_alpha && ag != 255 && a0 != 0) {
            const int scale = (a0 * 255 * 2 + ag) / (ag << 1) - a0;
            for (size_t i = 0; i < colors; ++i) {
              const int tmp = (a[i] - b[i]) * scale + 0x80;
              src[i] = static_cast<uint8_t>(
                  std::clamp(a[i] + ((tmp + (tmp >> 8)) >> 8), 0, 255));
            }
          }
          const int as = mul(ag, pix_alpha);
          if (as == 0) {
            for (size_t i = 0; i < bytes; ++i) {
              a[i] = b[i];
            }
            continue;
          }
          const int ar = a0 ? InkUnion(a0, as) : as;
          for (size_t i = 0; i < colors; ++i) {
            const int c =
                a0 ? InkComposite(c0[i], a0, src[i], as, src[i]) : src[i];
            a[i] = static_cast<uint8_t>(has_alpha ? c : Flatten(c, ar));
          }
          if (has_alpha) {
            a[3] = static_cast<uint8_t>(ar);
          }
        });
      }
    }
  }
}

// On inks, `obj` filled (or stroked) with tiling `pattern` as gs tiles it
// (cpdf_gstiling.h): the cell rendered once at gs's size, matrix and pixel
// origin, gs's mask its pixels painted; copied at gs's whole-pixel steps
// into the pixels the object paints (its fill or stroke as gs's, a path),
// a later cell over an earlier where they overlap; `shape` (its corner at
// `shape_rect`'s) the pixels instead when given. Whether it did.
bool CPDF_RenderStatus::DrawTilingPatternAsGs(CPDF_TilingPattern* pattern,
                                              CPDF_PageObject* obj,
                                              const CFX_Matrix& mtObj2Device,
                                              bool stroke,
                                              RetainPtr<CFX_DIBitmap> shape,
                                              const FX_RECT& shape_rect) {
  CPDF_Inks* inks = GetInks(context_->GetDocument());
  if (!inks) {
    return false;
  }
  const std::unique_ptr<CPDF_Form> form = pattern->Load(obj);
  if (!form) {
    return true;
  }
  CFX_RenderDevice::StateRestorer restorer(device_);
  const CPDF_PathObject* path_obj = obj->AsPath();
  if (!path_obj && !ClipPattern(obj, mtObj2Device, stroke)) {
    return true;
  }
  FX_RECT rect = device_->GetClipBox();
  if (shape) {
    rect.Intersect(shape_rect);
  } else if (path_obj) {
    FX_RECT bounds = obj->GetTransformedBBox(mtObj2Device);
    bounds = FX_RECT(bounds.left - 2, bounds.top - 2, bounds.right + 2,
                     bounds.bottom + 2);
    rect.Intersect(bounds);
  }
  if (rect.IsEmpty()) {
    return true;
  }
  // gs's device: the page (where the page is drawn), else this bitmap (a
  // cell of a pattern drawing one)
  FX_RECT page(0, 0, device_->GetWidth(), device_->GetHeight());
  if (context_->CountLayers() > 0 &&
      context_->GetLayer(0)->GetObjectHolder()->IsPage()) {
    CPDF_RenderContext::Layer* layer = context_->GetLayer(0);
    const CFX_FloatRect box =
        layer->GetMatrix().TransformRect(layer->GetObjectHolder()->GetBBox());
    page = FX_RECT(FXSYS_roundf(box.left), FXSYS_roundf(box.bottom),
                   FXSYS_roundf(box.right), FXSYS_roundf(box.top));
  }
  const CFX_Matrix pattern2device = pattern->pattern_to_form() * mtObj2Device;
  std::optional<GsTile> tile = MakeGsTile(
      pattern2device, pattern->bbox(), pattern->x_step(), pattern->y_step(),
      pattern->tiling_type(), page);
  if (!tile.has_value()) {
    return true;
  }
  // the pixels the object paints (given: from `shape_rect`'s corner, alpha)
  FX_RECT shape_origin = shape ? shape_rect : rect;
  if (!shape && path_obj) {
    shape = pdfium::MakeRetain<CFX_DIBitmap>();
    if (!shape->Create(rect.Width(), rect.Height(),
                       FXDIB_Format::k8bppMask)) {
      return true;
    }
    std::unique_ptr<CFX_RenderDevice> shape_device =
        CFX_RenderDevice::CreateForBitmap(shape);
    if (!shape_device) {
      return true;
    }
    CFX_Matrix path_matrix = path_obj->matrix() * mtObj2Device;
    path_matrix.Translate(-rect.left, -rect.top);
    CFX_FillRenderOptions fill_options(
        stroke ? CFX_FillRenderOptions::FillType::kNoFill
               : path_obj->filltype());
    fill_options.aliased_path = true;
    fill_options.gs = true;
    fill_options.stroke = stroke;
    fill_options.adjust_stroke =
        path_obj->general_state().GetStrokeAdjust() || inks->dpi() < 150;
    shape_device->DrawPath(*path_obj->path().GetObject(), &path_matrix,
                           path_obj->graph_state().GetObject(),
                           stroke ? 0 : 0xFFFFFFFF, stroke ? 0xFFFFFFFF : 0,
                           fill_options);
  }
  // the cell: colored in the plate's grays, uncolored its shape alone
  const bool colored = pattern->colored();
  auto cell = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!cell->Create(tile->width, tile->height,
                    colored ? FXDIB_Format::kBgra : FXDIB_Format::k8bppMask)) {
    return true;
  }
  {
    std::unique_ptr<CFX_RenderDevice> cell_device =
        CFX_RenderDevice::CreateForBitmap(cell);
    if (!cell_device) {
      return true;
    }
    cell_device->SetGroupKnockout(true);
    // the cell's matrix from the form's space: the device's, moved, unless
    // gs bent the cell (scaled it in pattern space)
    CFX_Matrix content = mtObj2Device;
    if (tile->ctm.a == pattern2device.a && tile->ctm.b == pattern2device.b &&
        tile->ctm.c == pattern2device.c && tile->ctm.d == pattern2device.d) {
      content.Translate(tile->ctm.e - pattern2device.e,
                        tile->ctm.f - pattern2device.f);
    } else {
      content = pattern->pattern_to_form().GetInverse() * tile->ctm;
    }
    CPDF_RenderOptions options;
    if (!colored) {
      options.SetColorMode(CPDF_RenderOptions::kAlpha);
    }
    options.GetOptions() = options_.GetOptions();
    options.GetOptions().bGsPatternCell = true;
    CPDF_RenderContext cell_context(context_->GetDocument(), nullptr,
                                    context_->GetPageCache());
    cell_context.AppendLayer(form.get(), content);
    cell_context.Render(cell_device.get(), nullptr, &options, nullptr);
  }
  const FX_ARGB fill_argb = colored ? 0 : GetFillArgb(obj);
  auto screen = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!screen->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)) {
    return true;
  }
  screen->Clear(0);
  // device pixel (x, y) from cell pixel (cx, cy), where gs's mask has it
  auto put = [&](int x, int y, int cx, int cy) {
    if (shape) {
      const int bytes = shape->GetBPP() / 8;
      if (!shape->GetScanline(y - shape_origin.top)[static_cast<size_t>(
              (x - shape_origin.left) * bytes + bytes - 1)]) {
        return;
      }
    }
    uint8_t* dest =
        screen->GetWritableScanline(y - rect.top)
            .subspan(static_cast<size_t>(x - rect.left) * 4)
            .data();
    if (colored) {
      const uint8_t* src =
          cell->GetScanline(cy).subspan(static_cast<size_t>(cx) * 4).data();
      if (!UNSAFE_TODO(src[3])) {
        return;
      }
      UNSAFE_TODO(FXSYS_memcpy(dest, src, 4));
      return;
    }
    const uint8_t m = cell->GetScanline(cy)[cx];
    if (!m) {
      return;
    }
    UNSAFE_TODO({
      dest[0] = FXARGB_B(fill_argb);
      dest[1] = FXARGB_G(fill_argb);
      dest[2] = FXARGB_R(fill_argb);
      dest[3] = FXARGB_A(fill_argb);
    });
  };
  if (tile->simple) {
    // the device the cell repeated (gx_dc_pattern_fill_rectangle)
    auto imod = [](int a, int b) { return ((a % b) + b) % b; };
    const int px = imod(-static_cast<int>(floorf(tile->step.e + 0.5f)),
                        tile->width);
    const int py = imod(-static_cast<int>(floorf(tile->step.f + 0.5f)),
                        tile->height);
    for (int y = rect.top; y < rect.bottom; ++y) {
      for (int x = rect.left; x < rect.right; ++x) {
        put(x, y, imod(x + px, tile->width), imod(y + py, tile->height));
      }
    }
  } else {
    GsTileSteps(*tile, rect, [&](int x0, int y0) {
      const int top = std::max(y0, rect.top);
      const int bottom = std::min(y0 + tile->height, rect.bottom);
      const int left = std::max(x0, rect.left);
      const int right = std::min(x0 + tile->width, rect.right);
      for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
          put(x, y, x - x0, y - y0);
        }
      }
    });
  }
  CompositeDIBitmap(std::move(screen), rect.left, rect.top, 0, 1.0f,
                    BlendMode::kNormal, CPDF_Transparency());
  return true;
}

// On inks, text filled with a pattern as gs fills it: the pixels of its
// glyphs as gs draws them solid (its glyph bitmaps at its positions), tiled
// (DrawTilingPatternAsGs) or shaded (the shading drawn, the pixels outside
// them put back). `path` is the text's box with its states. Whether it did.
bool CPDF_RenderStatus::DrawTextWithPatternAsGs(const CPDF_TextObject* textobj,
                                               const CFX_Matrix& mtObj2Device,
                                               CPDF_PathObject* path) {
  if (!GetInks(context_->GetDocument()) ||
      !options_.GetOptions().bNoTextSmooth) {
    return false;
  }
  RetainPtr<CPDF_Pattern> pattern =
      textobj->color_state().GetFillColor()->GetPattern();
  CPDF_TilingPattern* tiling = pattern ? pattern->AsTilingPattern() : nullptr;
  CPDF_ShadingPattern* shading =
      pattern ? pattern->AsShadingPattern() : nullptr;
  RetainPtr<CFX_DIBitmap> device = device_->GetBitmap();
  if (!tiling && (!shading || !device)) {
    return false;
  }
  FX_RECT rect = device_->GetClipBox();
  FX_RECT bounds = textobj->GetTransformedBBox(mtObj2Device);
  rect.Intersect(FX_RECT(bounds.left - 2, bounds.top - 2, bounds.right + 2,
                         bounds.bottom + 2));
  if (rect.IsEmpty()) {
    return true;
  }
  auto shape = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!shape->Create(rect.Width(), rect.Height(), FXDIB_Format::kBgra)) {
    return true;
  }
  shape->Clear(0);
  std::unique_ptr<CFX_RenderDevice> shape_device =
      CFX_RenderDevice::CreateForBitmap(shape);
  if (!shape_device) {
    return true;
  }
  CFX_Matrix text_matrix = textobj->GetTextMatrix() * mtObj2Device;
  const std::vector<float> positions = GsTextPositions(textobj, text_matrix);
  text_matrix.Translate(-rect.left, -rect.top);
  RetainPtr<CPDF_Font> font = textobj->GetFont();
  CPDF_TextRenderer::DrawNormalText(
      shape_device.get(), textobj->GetCharCodes(), positions, font.Get(),
      textobj->text_state().GetFontSize(), text_matrix, 0xFFFFFFFF, options_,
      /*gs_positioned=*/true);
  if (tiling) {
    return DrawTilingPatternAsGs(tiling, path, mtObj2Device, /*stroke=*/false,
                                 shape, rect);
  }
  RetainPtr<CFX_DIBitmap> before = device->ClipTo(rect);
  if (!before) {
    return true;
  }
  DrawShadingPattern(shading, path, mtObj2Device, /*stroke=*/false);
  const size_t bytes = device->GetBPP() / 8;
  for (int row = 0; row < rect.Height(); ++row) {
    pdfium::span<const uint8_t> mask = shape->GetScanline(row);
    pdfium::span<const uint8_t> back = before->GetScanline(row);
    pdfium::span<uint8_t> dest = device->GetWritableScanline(rect.top + row)
                                     .subspan(rect.left * bytes);
    for (size_t col = 0; col < static_cast<size_t>(rect.Width()); ++col) {
      if (!mask[col * 4 + 3]) {
        fxcrt::Copy(back.subspan(col * bytes, bytes),
                    dest.subspan(col * bytes, bytes));
      }
    }
  }
  return true;
}
