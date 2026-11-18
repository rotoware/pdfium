// pdfking: the ink device: a page drawn once onto one gray plane per plate.

#include "core/fpdfapi/render/cpdf_inkdriver.h"

#include <utility>

#include "core/fxcrt/check.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibbase.h"

namespace {

std::vector<UnownedPtr<CFX_RenderDevice>> Unowned(
    const std::vector<CFX_RenderDevice*>& planes) {
  std::vector<UnownedPtr<CFX_RenderDevice>> result;
  for (CFX_RenderDevice* plane : planes) {
    result.emplace_back(plane);
  }
  return result;
}

}  // namespace

CPDF_InkDriver::CPDF_InkDriver(RetainPtr<CPDF_Inks> inks,
                               std::vector<CFX_RenderDevice*> planes)
    : inks_(std::move(inks)), planes_(Unowned(planes)) {
  CHECK(!planes_.empty());
  CHECK_EQ(planes_.size(), inks_->size());
}

CPDF_InkDriver::~CPDF_InkDriver() = default;

RenderDeviceDriverIface* CPDF_InkDriver::Driver(size_t i) const {
  return planes_[i]->GetDeviceDriver();
}

std::optional<uint32_t> CPDF_InkDriver::Color(uint32_t color, size_t i) const {
  std::optional<FX_ARGB> argb = inks_->Resolve(color, i);
  if (!argb.has_value()) {
    inks_->Fail();
  }
  DCHECK(argb.has_value());
  return argb;
}

DeviceType CPDF_InkDriver::GetDeviceType() const {
  return Driver(0)->GetDeviceType();
}

bool CPDF_InkDriver::RenderCapGetBits() const {
  return Driver(0)->RenderCapGetBits();
}

bool CPDF_InkDriver::RenderCapAlphaImage() const {
  return Driver(0)->RenderCapAlphaImage();
}

bool CPDF_InkDriver::RenderCapBlendMode() const {
  return Driver(0)->RenderCapBlendMode();
}

bool CPDF_InkDriver::RenderCapSoftClip() const {
  return Driver(0)->RenderCapSoftClip();
}

bool CPDF_InkDriver::RenderCapAlphaOutput() const {
  return Driver(0)->RenderCapAlphaOutput();
}

bool CPDF_InkDriver::RenderCapByteMaskOutput() const {
  return Driver(0)->RenderCapByteMaskOutput();
}

int CPDF_InkDriver::GetPixelWidth() const {
  return Driver(0)->GetPixelWidth();
}

int CPDF_InkDriver::GetPixelHeight() const {
  return Driver(0)->GetPixelHeight();
}

int CPDF_InkDriver::GetBitsPerPixel() const {
  return Driver(0)->GetBitsPerPixel();
}

int CPDF_InkDriver::GetDriverType() const {
  return Driver(0)->GetDriverType();
}

void CPDF_InkDriver::Clear(uint32_t color) {
  for (size_t i = 0; i < planes_.size(); ++i) {
    if (std::optional<uint32_t> c = Color(color, i)) {
      Driver(i)->Clear(c.value());
    }
  }
}

// State and clips through the planes' devices: they keep their clip boxes,
// which the renderer reads when it draws on a plane alone.
void CPDF_InkDriver::SaveState() {
  for (auto& plane : planes_) {
    plane->SaveState();
  }
}

void CPDF_InkDriver::RestoreState(bool bKeepSaved) {
  for (auto& plane : planes_) {
    plane->RestoreState(bKeepSaved);
  }
}

void CPDF_InkDriver::SetBaseClip(const FX_RECT& rect) {
  for (auto& plane : planes_) {
    plane->SetBaseClip(rect);
  }
}

bool CPDF_InkDriver::SetClip_PathFill(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_FillRenderOptions& fill_options) {
  bool result = true;
  for (auto& plane : planes_) {
    result &= plane->SetClip_PathFill(path, pObject2Device, fill_options);
  }
  return result;
}

bool CPDF_InkDriver::SetClip_PathStroke(const CFX_Path& path,
                                        const CFX_Matrix* pObject2Device,
                                        const CFX_GraphStateData* pGraphState) {
  bool result = true;
  for (auto& plane : planes_) {
    result &= plane->SetClip_PathStroke(path, pObject2Device, pGraphState);
  }
  return result;
}

FX_RECT CPDF_InkDriver::GetClipBox() const {
  return planes_[0]->GetClipBox();
}

bool CPDF_InkDriver::DrawPath(const CFX_Path& path,
                              const CFX_Matrix* pObject2Device,
                              const CFX_GraphStateData* pGraphState,
                              uint32_t fill_color,
                              uint32_t stroke_color,
                              bool group_knockout,
                              const CFX_FillRenderOptions& fill_options) {
  bool result = true;
  for (size_t i = 0; i < planes_.size(); ++i) {
    std::optional<uint32_t> fill = Color(fill_color, i);
    std::optional<uint32_t> stroke = Color(stroke_color, i);
    if (!fill.has_value() || !stroke.has_value()) {
      return false;
    }
    result &=
        Driver(i)->DrawPath(path, pObject2Device, pGraphState, fill.value(),
                            stroke.value(), group_knockout, fill_options);
  }
  return result;
}

bool CPDF_InkDriver::FillRect(const FX_RECT& rect, uint32_t fill_color) {
  bool result = true;
  for (size_t i = 0; i < planes_.size(); ++i) {
    std::optional<uint32_t> fill = Color(fill_color, i);
    if (!fill.has_value()) {
      return false;
    }
    result &= Driver(i)->FillRect(rect, fill.value());
  }
  return result;
}

bool CPDF_InkDriver::DrawCosmeticLine(const CFX_PointF& ptMoveTo,
                                      const CFX_PointF& ptLineTo,
                                      uint32_t color) {
  // all planes or none: on a no the device draws it as a path
  for (size_t i = 0; i < planes_.size(); ++i) {
    std::optional<uint32_t> c = Color(color, i);
    if (!c.has_value()) {
      return false;
    }
    if (!Driver(i)->DrawCosmeticLine(ptMoveTo, ptLineTo, c.value())) {
      CHECK_EQ(i, 0u);
      return false;
    }
  }
  return true;
}

// A mask (glyphs, a stencil) in a handle's color; a bitmap of colors has no
// inks to give
bool CPDF_InkDriver::SetDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                               uint32_t color,
                               const FX_RECT& src_rect,
                               int dest_left,
                               int dest_top,
                               BlendMode blend_type) {
  if (!bitmap->IsMaskFormat()) {
    inks_->Fail();
    return false;
  }
  bool result = true;
  for (size_t i = 0; i < planes_.size(); ++i) {
    std::optional<uint32_t> c = Color(color, i);
    if (!c.has_value()) {
      return false;
    }
    result &= Driver(i)->SetDIBits(bitmap, c.value(), src_rect, dest_left,
                                   dest_top, blend_type);
  }
  return result;
}

bool CPDF_InkDriver::StretchDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                                   uint32_t color,
                                   int dest_left,
                                   int dest_top,
                                   int dest_width,
                                   int dest_height,
                                   const FX_RECT* pClipRect,
                                   const FXDIB_ResampleOptions& options,
                                   BlendMode blend_type) {
  if (!bitmap->IsMaskFormat()) {
    inks_->Fail();
    return false;
  }
  bool result = true;
  for (size_t i = 0; i < planes_.size(); ++i) {
    std::optional<uint32_t> c = Color(color, i);
    if (!c.has_value()) {
      return false;
    }
    result &= Driver(i)->StretchDIBits(bitmap, c.value(), dest_left, dest_top,
                                       dest_width, dest_height, pClipRect,
                                       options, blend_type);
  }
  return result;
}

// Images come plane by plane (CPDF_RenderStatus::InkPlanes), never here
RenderDeviceDriverIface::StartResult CPDF_InkDriver::StartDIBits(
    RetainPtr<const CFX_DIBBase> bitmap,
    float alpha,
    uint32_t color,
    const CFX_Matrix& matrix,
    const FXDIB_ResampleOptions& options,
    BlendMode blend_type) {
  inks_->Fail();
  return {Result::kFailure, nullptr};
}

bool CPDF_InkDriver::MultiplyAlpha(float alpha) {
  inks_->Fail();
  return false;
}

bool CPDF_InkDriver::MultiplyAlphaMask(RetainPtr<const CFX_DIBitmap> mask) {
  inks_->Fail();
  return false;
}
