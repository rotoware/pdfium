// pdfking: the ink device: a page drawn once onto one gray plane per plate.

#ifndef CORE_FPDFAPI_RENDER_CPDF_INKDRIVER_H_
#define CORE_FPDFAPI_RENDER_CPDF_INKDRIVER_H_

#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_inks.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/renderdevicedriver_iface.h"

class CFX_RenderDevice;

// Draws what the renderer hands it (paths, rectangles, lines, glyph masks,
// clips) on every plane at once, a color being a handle of `inks`: each plane
// gets the gray of its plate. Anything else (an RGB color, a bitmap of
// colors) is refused: the renderer gives the ink device those objects plane
// by plane (CPDF_RenderStatus::InkPlanes), each plane as its plate.
class CPDF_InkDriver final : public RenderDeviceDriverIface {
 public:
  CPDF_InkDriver(RetainPtr<CPDF_Inks> inks,
                 std::vector<CFX_RenderDevice*> planes);
  ~CPDF_InkDriver() override;

  // RenderDeviceDriverIface:
  DeviceType GetDeviceType() const override;
  bool RenderCapGetBits() const override;
  bool RenderCapAlphaImage() const override;
  bool RenderCapBlendMode() const override;
  bool RenderCapSoftClip() const override;
  bool RenderCapAlphaOutput() const override;
  bool RenderCapByteMaskOutput() const override;
  int GetPixelWidth() const override;
  int GetPixelHeight() const override;
  int GetBitsPerPixel() const override;
  void Clear(uint32_t color) override;
  void SaveState() override;
  void RestoreState(bool bKeepSaved) override;
  void SetBaseClip(const FX_RECT& rect) override;
  bool SetClip_PathFill(const CFX_Path& path,
                        const CFX_Matrix* pObject2Device,
                        const CFX_FillRenderOptions& fill_options) override;
  bool SetClip_PathStroke(const CFX_Path& path,
                          const CFX_Matrix* pObject2Device,
                          const CFX_GraphStateData* pGraphState) override;
  bool DrawPath(const CFX_Path& path,
                const CFX_Matrix* pObject2Device,
                const CFX_GraphStateData* pGraphState,
                uint32_t fill_color,
                uint32_t stroke_color,
                bool group_knockout,
                const CFX_FillRenderOptions& fill_options) override;
  bool FillRect(const FX_RECT& rect, uint32_t fill_color) override;
  bool DrawCosmeticLine(const CFX_PointF& ptMoveTo,
                        const CFX_PointF& ptLineTo,
                        uint32_t color) override;
  FX_RECT GetClipBox() const override;
  bool SetDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                 uint32_t color,
                 const FX_RECT& src_rect,
                 int dest_left,
                 int dest_top,
                 BlendMode blend_type) override;
  bool StretchDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                     uint32_t color,
                     int dest_left,
                     int dest_top,
                     int dest_width,
                     int dest_height,
                     const FX_RECT* pClipRect,
                     const FXDIB_ResampleOptions& options,
                     BlendMode blend_type) override;
  StartResult StartDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                          float alpha,
                          uint32_t color,
                          const CFX_Matrix& matrix,
                          const FXDIB_ResampleOptions& options,
                          BlendMode blend_type) override;
  int GetDriverType() const override;
  bool MultiplyAlpha(float alpha) override;
  bool MultiplyAlphaMask(RetainPtr<const CFX_DIBitmap> mask) override;

 private:
  // `color` on plane `i`, the render failed when it is no handle
  std::optional<uint32_t> Color(uint32_t color, size_t i) const;
  RenderDeviceDriverIface* Driver(size_t i) const;

  RetainPtr<CPDF_Inks> const inks_;
  std::vector<UnownedPtr<CFX_RenderDevice>> const planes_;
};

#endif  // CORE_FPDFAPI_RENDER_CPDF_INKDRIVER_H_
