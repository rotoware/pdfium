// pdfking: CPDF_RenderStatus on a plate or on inks, and the groups it
// composites as gs does (knockout, the page showing through).
// cpdf_renderstatus.cpp only calls in here, so a rebase touches one line per
// hook.

#include <stdint.h>

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
#include "core/fpdfapi/render/cpdf_inkblend.h"
#include "core/fpdfapi/render/cpdf_inkdriver.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"
#include "core/fxcrt/autorestorer.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxge/cfx_renderdevice.h"
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
      const int b0 = backdrop ? back[(rect.left + col) * back_bytes] : 255;
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
      BlendsNonSeparably(obj->AsForm()->form())) {
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
    if (process >= 0) {
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
  if (!stopped_) {
    InkCompositeLayers(layers, planes, obj->general_state().GetBlendType());
  }
}

// The process plates' `layers` (BGRA: each its plate's gray and alpha)
// composited on their `planes` in the nonseparable `mode`, as gs does: C, M,
// Y blended together, K as InkBlend() takes it, each mixed by its own alpha
// with the backdrop's (an opaque page, or a group's pixels).
void CPDF_RenderStatus::InkCompositeLayers(
    const std::array<CPDF_InkLayer, 4>& layers,
    const std::array<CFX_RenderDevice*, 4>& planes,
    BlendMode mode) {
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
      bool painted = false;
      for (int p = 0; p < 4; ++p) {
        const size_t bytes = devices[p]->GetBPP() / 8;
        pdfium::span<const uint8_t> back =
            devices[p]
                ->GetScanline(rect.top + row)
                .subspan((rect.left + col) * bytes, bytes);
        cb[p] = back[0];
        ab[p] = bytes == 4 ? back[3] : 255;
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
        out[0] = out[1] = out[2] =
            InkComposite(cb[p], ab[p], cs[p], as[p], blend[p]);
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
      if (process >= 0) {
        layers[process] = {bitmaps[i], rect};
        planes[process] = outer[i];
      }
    }
    InkCompositeLayers(layers, planes, mode);
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
// isolated) with a soft mask or an opacity, composited Normal: RenderGroup
// paints it on the page and mixes it with what was there (MixGroup), so its
// objects blend with the page under them, as gs blends them.
bool CPDF_RenderStatus::PlateMixesGroup(const CPDF_FormObject* form) const {
  if (PlateBitmaps().empty()) {
    return false;
  }
  const CPDF_GeneralState& state = form->general_state();
  const CPDF_Transparency& group = form->form()->GetTransparency();
  return group.IsGroup() && !group.IsIsolated() &&
         state.GetBlendType() == BlendMode::kNormal &&
         (state.GetSoftMask() || state.GetFillAlpha() != 1.0f);
}

// `form`'s objects rendered by `status`: a knockout group's each on the
// group's backdrop, a group the page shows through mixed back with the page
void CPDF_RenderStatus::RenderGroup(CPDF_RenderStatus* status,
                                    const CPDF_FormObject* form,
                                    const CFX_Matrix& matrix,
                                    const CFX_Matrix& mtObj2Device) {
  FX_RECT rect;
  std::vector<RetainPtr<CFX_DIBitmap>> backdrops;
  if (PlateMixesGroup(form)) {
    rect = GroupRect(device_, form, mtObj2Device);
    if (rect.IsEmpty()) {
      return;
    }
    for (const auto& bitmap : PlateBitmaps()) {
      backdrops.push_back(bitmap->ClipTo(rect));
    }
    // its opacity is mixed in below, never on its objects too
    status->SetInGroup(true);
  }
  if (form->form()->GetTransparency().IsKnockout() &&
      (device_->GetBitmap() || status->Inks())) {
    status->RenderKnockoutGroup(form, matrix, mtObj2Device);
  } else {
    status->RenderObjectList(form->form(), matrix);
  }
  if (!backdrops.empty()) {
    MixGroup(form, mtObj2Device, rect, backdrops);
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
// mask times its opacity, `w`: P = (1 - w) P0 + w Pn, alpha and
// alpha-weighted color. ISO 32000-2 11.4.8's backdrop removal then
// compositing come to that (the group's alpha over the backdrop's is w times
// what the group adds).
void CPDF_RenderStatus::MixGroup(
    const CPDF_FormObject* form,
    const CFX_Matrix& mtObj2Device,
    const FX_RECT& rect,
    const std::vector<RetainPtr<CFX_DIBitmap>>& backdrops) {
  RetainPtr<CFX_DIBitmap> mask;
  RetainPtr<CPDF_Dictionary> smask_dict = const_cast<CPDF_FormObject*>(form)
                                              ->mutable_general_state()
                                              .GetMutableSoftMask();
  if (smask_dict) {
    mask = LoadSMask(smask_dict.Get(), rect,
                     *form->general_state().GetSMaskMatrix() * mtObj2Device);
  }
  const int alpha = FXSYS_roundf(form->general_state().GetFillAlpha() * 255);
  const std::vector<RetainPtr<CFX_DIBitmap>> devices = PlateBitmaps();
  for (size_t plane = 0; plane < devices.size(); ++plane) {
    const CFX_DIBitmap& backdrop = *backdrops[plane];
    RetainPtr<CFX_DIBitmap> device = devices[plane];
    const size_t bytes = device->GetBPP() / 8;
    // alpha last when there is one, and the color not already weighted by it
    const bool weigh = device->IsAlphaFormat() && !device->IsPremultiplied();
    for (int row = 0; row < rect.Height(); ++row) {
      pdfium::span<const uint8_t> before = backdrop.GetScanline(row);
      pdfium::span<uint8_t> after = device->GetWritableScanline(rect.top + row)
                                        .subspan(rect.left * bytes);
      pdfium::span<const uint8_t> m =
          mask ? mask->GetScanline(row) : pdfium::span<const uint8_t>();
      for (size_t col = 0; col < static_cast<size_t>(rect.Width()); ++col) {
        // w out of 255 * 255
        const int w = (mask ? m[col] : 255) * alpha;
        const int v = 255 * 255 - w;
        const uint8_t* b = before.subspan(col * bytes).data();
        uint8_t* a = after.subspan(col * bytes).data();
        if (!weigh) {
          for (size_t i = 0; i < bytes; ++i) {
            UNSAFE_TODO(a[i] = (b[i] * v + a[i] * w + 32512) / 65025);
          }
          continue;
        }
        UNSAFE_TODO({
          const int a0 = b[3];
          const int an = a[3];
          const int af = (a0 * v + an * w + 32512) / 65025;
          for (int i = 0; i < 3; ++i) {
            const int64_t p = int64_t{b[i]} * a0 * v + int64_t{a[i]} * an * w;
            const int64_t d = int64_t{af} * 65025;
            a[i] = af ? static_cast<uint8_t>((p + d / 2) / d) : 0;
          }
          a[3] = static_cast<uint8_t>(af);
        });
      }
    }
  }
}
