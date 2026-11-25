// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// pdfking: the fork's own API (public/fpdf_pdfking.h).

#include "public/fpdf_pdfking.h"

#include <math.h>

#include <memory>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_inks.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/render/cpdf_inkdriver.h"
#include "core/fpdfapi/render/cpdf_pagerendercontext.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxge/cfx_fontmapper.h"
#include "core/fxge/cfx_fontmgr.h"
#include "core/fxge/cfx_gemodule.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_renderpage.h"

FPDF_EXPORT void FPDF_CALLCONV PDFKING_SetPlate(FPDF_DOCUMENT document,
                                                FPDF_BYTESTRING plate) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (doc) {
    CPDF_DocPageData::FromDocument(doc)->SetPlate(
        plate && *plate ? pdfium::MakeRetain<CPDF_Plate>(doc, plate) : nullptr);
  }
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
PDFKING_GetPageInks(FPDF_PAGE page, char* buffer, unsigned long length) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 0;
  }
  ByteString names;
  for (const ByteString& name : PageSpotInks(pPage)) {
    names += name;
    names += '\0';
  }
  const unsigned long size = names.GetLength();
  if (buffer && length >= size) {
    UNSAFE_TODO(FXSYS_memcpy(buffer, names.c_str(), size));
  }
  return size;
}

FPDF_EXPORT PDFKING_INKS FPDF_CALLCONV
PDFKING_LoadInks(FPDF_PAGE page, const FPDF_BYTESTRING* names, int count) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || !names || count <= 0) {
    return nullptr;
  }
  std::vector<ByteString> inks_names;
  for (FPDF_BYTESTRING name : UNSAFE_TODO(pdfium::span(names, static_cast<size_t>(count)))) {
    if (!name) {
      return nullptr;
    }
    inks_names.emplace_back(name);
  }
  return reinterpret_cast<PDFKING_INKS>(
      pdfium::MakeRetain<CPDF_Inks>(pPage, inks_names).Leak());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV PDFKING_RenderInks(PDFKING_INKS inks,
                                                       FPDF_PAGE page,
                                                       FPDF_BITMAP* planes,
                                                       const FS_MATRIX* matrix,
                                                       const FS_RECTF* clipping,
                                                       int flags) {
  CPDF_Inks* ink_set = reinterpret_cast<CPDF_Inks*>(inks);
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!ink_set || !pPage || !planes) {
    return false;
  }
  std::vector<std::unique_ptr<CFX_RenderDevice>> devices;
  std::vector<CFX_RenderDevice*> plane_devices;
  for (FPDF_BITMAP plane : UNSAFE_TODO(pdfium::span(planes, ink_set->asked()))) {
    RetainPtr<CFX_DIBitmap> bitmap(CFXDIBitmapFromFPDFBitmap(plane));
    if (!bitmap || bitmap->GetFormat() != FXDIB_Format::k8bppRgb ||
        (!devices.empty() &&
         (bitmap->GetWidth() != devices[0]->GetWidth() ||
          bitmap->GetHeight() != devices[0]->GetHeight()))) {
      return false;
    }
    devices.push_back(CFX_RenderDevice::CreateForBitmap(std::move(bitmap)));
    if (!devices.back()) {
      return false;
    }
    plane_devices.push_back(devices.back().get());
  }
  // the plates not asked for, rendered all the same (CPDF_Inks::size())
  while (devices.size() < ink_set->size()) {
    auto hidden = pdfium::MakeRetain<CFX_DIBitmap>();
    if (devices.empty() ||
        !hidden->Create(devices[0]->GetWidth(), devices[0]->GetHeight(),
                        FXDIB_Format::k8bppRgb)) {
      return false;
    }
    hidden->Clear(0xFFFFFFFF);
    devices.push_back(CFX_RenderDevice::CreateForBitmap(std::move(hidden)));
    plane_devices.push_back(devices.back().get());
  }
  RetainPtr<CPDF_Inks> retained(ink_set);
  std::unique_ptr<CFX_RenderDevice> device = CFX_RenderDevice::CreateWithDriver(
      std::make_unique<CPDF_InkDriver>(retained, plane_devices));
  if (!device) {
    return false;
  }
  ink_set->SetDevices(device.get(), plane_devices);
  ink_set->ClearFailed();
  // the resolution as gs's -r: the matrix's scale, the page's UserUnit out
  ink_set->set_dpi(matrix ? 72.0f * hypotf(matrix->a, matrix->b) /
                                PDFKING_GetPageUserUnit(page)
                          : 72.0f);

  CPDF_Document* doc = pPage->GetDocument();
  CPDF_DocPageData* data = CPDF_DocPageData::FromDocument(doc);
  RetainPtr<const CPDF_Plate> plate(data->GetPlate());
  data->SetPlate(nullptr);
  data->SetInks(retained);
  {
    auto owned_context = std::make_unique<CPDF_PageRenderContext>();
    CPDF_PageRenderContext* context = owned_context.get();
    CPDF_Page::RenderContextClearer clearer(pPage);
    pPage->SetRenderContext(std::move(owned_context));
    context->device_ = std::move(device);

    CFX_FloatRect clipping_rect;
    if (clipping) {
      clipping_rect = CFXFloatRectFromFSRectF(*clipping);
    }
    CFX_Matrix transform_matrix = pPage->GetDisplayMatrix();
    if (matrix) {
      transform_matrix *= CFXMatrixFromFSMatrix(*matrix);
    }
    // the page's top left on the device: the display matrix puts it at 0, 0
    ink_set->set_page_origin(
        matrix ? CFXMatrixFromFSMatrix(*matrix).Transform(CFX_PointF())
               : CFX_PointF());
    CPDFSDK_RenderPage(context, pPage, transform_matrix,
                       clipping_rect.ToFxRect(), flags,
                       /*color_scheme=*/nullptr);
  }
  data->SetInks(nullptr);
  data->SetPlate(std::move(plate));
  // an RGB page group: the process plates from its channels, as gs puts it
  // on the device
  ink_set->ConvertBlendSpace();
  ink_set->SetDevices(nullptr, {});
  return !ink_set->failed();
}

FPDF_EXPORT void FPDF_CALLCONV
PDFKING_SetBlendProfiles(FPDF_DOCUMENT document,
                         const unsigned char* rgb,
                         unsigned long rgb_size,
                         const unsigned char* cmyk,
                         unsigned long cmyk_size,
                         const unsigned char* gray,
                         unsigned long gray_size) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return;
  }
  auto bytes = [](const unsigned char* data, unsigned long size) {
    return data ? DataVector<uint8_t>(data, UNSAFE_TODO(data + size))
                : DataVector<uint8_t>();
  };
  CPDF_DocPageData::FromDocument(doc)->SetBlendProfiles(
      bytes(rgb, rgb_size), bytes(cmyk, cmyk_size), bytes(gray, gray_size));
}

FPDF_EXPORT void FPDF_CALLCONV PDFKING_CloseInks(PDFKING_INKS inks) {
  // Take it back across the API and throw it away
  RetainPtr<CPDF_Inks> owned;
  owned.Unleak(reinterpret_cast<CPDF_Inks*>(inks));
}

FPDF_EXPORT void FPDF_CALLCONV PDFKING_SetGsFontDir(FPDF_BYTESTRING dir) {
  CFX_GEModule::Get()->GetFontMgr()->GetBuiltinMapper()->SetGsFontDir(
      dir ? ByteString(dir) : ByteString());
}

FPDF_EXPORT float FPDF_CALLCONV PDFKING_GetPageUserUnit(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 1.0f;
  }
  const float unit = pPage->GetDict()->GetFloatFor("UserUnit");
  return unit > 0 ? unit : 1.0f;
}
