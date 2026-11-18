// pdfking: CPDF_RenderStatus on a plate, and the groups it composites as gs
// does (knockout, the page showing through). cpdf_renderstatus.cpp only
// calls in here, so a rebase touches one line per hook.

#include <stdint.h>

#include <memory>

#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
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
void CopyPixels(const CFX_DIBitmap& src, int x, int y, CFX_DIBitmap& dst,
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

// Whether an overprinting image is in inks all off the plate, which it then
// leaves as it was. Its color space may be a resource name (inline images).
// One with a soft mask knocks out, as gs paints it.
bool CPDF_RenderStatus::PlateLeavesImage(CPDF_ImageObject* image) const {
  CPDF_Document* doc = context_->GetDocument();
  const ByteString& plate = doc->GetPlate();
  if (plate.IsEmpty() || !image->general_state().GetFillOP()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> dict = image->GetImage()->GetDict();
  if (dict->KeyExist("SMask")) {
    return false;
  }
  RetainPtr<const CPDF_Object> cs_obj = dict->GetDirectObjectFor("ColorSpace");
  // none but an image mask's: a JPX's own, process colors
  if (!cs_obj) {
    return !dict->GetBooleanFor("ImageMask", false) && PlateIsSpot(plate);
  }
  auto* page_data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<CPDF_ColorSpace> cs;
  if (GetFormResource()) {
    cs = page_data->GetColorSpace(cs_obj.Get(), GetFormResource());
  }
  if (!cs) {
    cs = page_data->GetColorSpace(cs_obj.Get(), GetPageResource());
  }
  return cs && !PlateMarked(cs.Get(), plate);
}

// Whether `form` is, on a plate, a group the page shows through (not
// isolated) with a soft mask or an opacity, composited Normal: RenderGroup
// paints it on the page and mixes it with what was there (MixGroup), so its
// objects blend with the page under them, as gs blends them.
bool CPDF_RenderStatus::PlateMixesGroup(const CPDF_FormObject* form) const {
  if (context_->GetDocument()->GetPlate().IsEmpty() || !device_->GetBitmap()) {
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
  RetainPtr<CFX_DIBitmap> backdrop;
  if (PlateMixesGroup(form)) {
    rect = GroupRect(device_, form, mtObj2Device);
    if (rect.IsEmpty()) {
      return;
    }
    backdrop = device_->GetBitmap()->ClipTo(rect);
    // its opacity is mixed in below, never on its objects too
    status->SetInGroup(true);
  }
  if (form->form()->GetTransparency().IsKnockout() && device_->GetBitmap()) {
    status->RenderKnockoutGroup(form, matrix, mtObj2Device);
  } else {
    status->RenderObjectList(form->form(), matrix);
  }
  if (backdrop) {
    MixGroup(form, mtObj2Device, rect, *backdrop);
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
  RetainPtr<CFX_DIBitmap> device = device_->GetBitmap();
  const FX_RECT rect = GroupRect(device_, form, mtObj2Device);
  if (rect.IsEmpty()) {
    return;
  }
  const size_t bytes = device->GetBPP() / 8;
  RetainPtr<CFX_DIBitmap> backdrop = device->ClipTo(rect);
  RetainPtr<CFX_DIBitmap> group = device->ClipTo(rect);
  if (!backdrop || !group) {
    return;
  }
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
    // its shape: alone on a clear layer, where it leaves any alpha
    auto layer = pdfium::MakeRetain<CFX_DIBitmap>();
    std::unique_ptr<CFX_RenderDevice> layer_device =
        layer->Create(box.Width(), box.Height(), FXDIB_Format::kBgra)
            ? CFX_RenderDevice::CreateForBitmap(layer)
            : nullptr;
    if (!layer_device) {
      return;
    }
    CFX_Matrix layer_matrix = matrix;
    layer_matrix.Translate(-box.left, -box.top);
    CPDF_RenderStatus shape(context_, layer_device.get());
    shape.SetOptions(options_);
    shape.SetFormResource(form_resource_);
    shape.SetInGroup(true);
    shape.Initialize(this, nullptr);
    shape.RenderSingleObject(pObj, layer_matrix);
    // painted on the group's backdrop
    pObj->mutable_general_state() = painted_state;
    CopyPixels(*backdrop, rect.left, rect.top, *device, box);
    RenderSingleObject(pObj, matrix);
    if (stopped_) {
      return;
    }
    // kept in the group where it paints
    for (int row = 0; row < box.Height(); ++row) {
      pdfium::span<const uint8_t> painted =
          device->GetScanline(box.top + row).subspan(box.left * bytes);
      pdfium::span<uint8_t> kept =
          group->GetWritableScanline(y + row).subspan(x * bytes);
      pdfium::span<const uint8_t> alpha = layer->GetScanline(row);
      for (int col = 0; col < box.Width(); ++col) {
        if (alpha[col * 4 + 3]) {
          fxcrt::Copy(painted.subspan(col * bytes, bytes),
                      kept.subspan(col * bytes, bytes));
        }
      }
    }
  }
  CopyPixels(*group, rect.left, rect.top, *device, rect);
}

// The group painted on the page, mixed with the page as it was by its soft
// mask times its opacity, `w`: P = (1 - w) P0 + w Pn, alpha and
// alpha-weighted color. ISO 32000-2 11.4.8's backdrop removal then
// compositing come to that (the group's alpha over the backdrop's is w times
// what the group adds).
void CPDF_RenderStatus::MixGroup(const CPDF_FormObject* form,
                                 const CFX_Matrix& mtObj2Device,
                                 const FX_RECT& rect,
                                 const CFX_DIBitmap& backdrop) {
  RetainPtr<CFX_DIBitmap> device = device_->GetBitmap();
  RetainPtr<CFX_DIBitmap> mask;
  RetainPtr<CPDF_Dictionary> smask_dict =
      const_cast<CPDF_FormObject*>(form)
          ->mutable_general_state()
          .GetMutableSoftMask();
  if (smask_dict) {
    mask = LoadSMask(smask_dict.Get(), rect,
                     *form->general_state().GetSMaskMatrix() * mtObj2Device);
  }
  const int alpha = FXSYS_roundf(form->general_state().GetFillAlpha() * 255);
  const size_t bytes = device->GetBPP() / 8;
  // alpha last when there is one, and the color not already weighted by it
  const bool weigh = device->IsAlphaFormat() && !device->IsPremultiplied();
  for (int row = 0; row < rect.Height(); ++row) {
    pdfium::span<const uint8_t> before = backdrop.GetScanline(row);
    pdfium::span<uint8_t> after =
        device->GetWritableScanline(rect.top + row).subspan(rect.left * bytes);
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
