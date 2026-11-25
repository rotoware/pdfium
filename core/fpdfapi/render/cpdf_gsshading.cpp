// pdfking: see cpdf_gsshading.h.
//
// Copyright (C) 2001-2025 Artifex Software, Inc.
// Licensed under the GNU Affero General Public License (AGPL) v3; see
// https://www.gnu.org/licenses/agpl-3.0.html. This file is AGPL code, not
// under the PDFium BSD license.

#include "core/fpdfapi/render/cpdf_gsshading.h"

#include <math.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_expintfunc.h"
#include "core/fpdfapi/page/cpdf_function.h"
#include "core/fpdfapi/page/cpdf_indexedcs.h"
#include "core/fpdfapi/page/cpdf_inks.h"
#include "core/fpdfapi/page/cpdf_plate.h"
#include "core/fpdfapi/page/cpdf_sampledfunc.h"
#include "core/fpdfapi/page/cpdf_shadingpattern.h"
#include "core/fpdfapi/page/cpdf_stitchfunc.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "core/fxge/gs_shade.h"

namespace {

constexpr int kRangeCheck = -15;  // gs_error_rangecheck

int IsMonotonic(const CPDF_Function* func,
                const float* lower,
                const float* upper,
                unsigned* mask);

// gsfunc3.c fn_ElIn_is_monotonic: an Exponential function always is
int ExpIntIsMonotonic(const CPDF_Function* func,
                      const float* lower,
                      const float* upper,
                      unsigned* mask) {
  if (lower[0] > func->GetDomain(1) || upper[0] < func->GetDomain(0)) {
    return kRangeCheck;
  }
  *mask = 0;
  return 1;
}

// gsfunc3.c fn_1ItSg_is_monotonic
int StitchIsMonotonic(const CPDF_StitchFunc* func,
                      const float* lower,
                      const float* upper,
                      unsigned* mask) {
  float v0 = lower[0];
  float v1 = upper[0];
  const float d0 = func->GetDomain(0);
  const float d1 = func->GetDomain(1);
  const int k = static_cast<int>(func->GetSubFunctions().size());

  *mask = 0;
  if (v0 == v1) {
    return 1;
  }
  if (v0 > v1) {
    v0 = v1;
    v1 = lower[0];
  }
  if (v0 > d1 || v1 < d0) {
    return kRangeCheck;
  }
  if (v0 < d0) {
    v0 = d0;
  }
  if (v1 > d1) {
    v1 = d1;
  }
  for (int i = 0; i < k; ++i) {
    const float b0 = i == 0 ? d0 : func->GetBound(i);
    const float b1 = i == k - 1 ? d1 : func->GetBound(i + 1);
    const float bsmall = 1e-6f * (b1 - b0);
    if (v0 >= b1 - bsmall) {
      continue;  // Ignore a small noise
    }
    const float vv0 = std::max(b0, v0);
    float vv1 = std::max(b0, v1);
    if (vv1 > b1 && v1 < b1 + bsmall) {
      vv1 = b1;  // Ignore a small noise
    }
    if (vv0 == vv1) {
      return 1;
    }
    if (vv0 < b1 && vv1 > b1) {
      *mask = 1;
      return 0;  // Consider stitches as monotony breaks.
    }
    const float e0 = func->GetEncode(2 * i);
    const float e1 = func->GetEncode(2 * i + 1);
    const float esmall = 1e-6f * fabsf(e1 - e0);
    const float vb0 = std::max(vv0, b0);
    const float vb1 = std::min(vv1, b1);
    if (b1 == b0) {
      return 1;  // function is monotonous in a point
    }
    float w0 = (vb0 - b0) * (e1 - e0) / (b1 - b0) + e0;
    float w1 = (vb1 - b0) * (e1 - e0) / (b1 - b0) + e0;
    // Note that w0 > w1 is now possible if e0 > e1.
    if (e0 > e1) {
      if (w0 > e0 && w0 - esmall <= e0) {
        w0 = e0;  // Suppress a small noise
      }
      if (w1 < e1 && w1 + esmall >= e1) {
        w1 = e1;  // Suppress a small noise
      }
    } else {
      if (w0 < e0 && w0 + esmall >= e0) {
        w0 = e0;  // Suppress a small noise
      }
      if (w1 > e1 && w1 - esmall <= e1) {
        w1 = e1;  // Suppress a small noise
      }
    }
    const CPDF_Function* sub = func->GetSubFunctions()[i].get();
    if (!sub) {
      return kRangeCheck;
    }
    return w0 > w1 ? IsMonotonic(sub, &w1, &w0, mask)
                   : IsMonotonic(sub, &w0, &w1, mask);
  }
  // v0 is equal to the range end.
  *mask = 0;
  return 1;
}

// gsfunc0.c fn_Sd_1arg_linear_monotonic_rec: bit 3i (rising) and 3i + 1
// (falling) per output between samples i0 and i1, the samples read as the
// function's values at them (pdfking: the decode keeps their order)
int SampledMonotonicRec(const CPDF_SampledFunc* func,
                        int i0,
                        int i1,
                        const std::vector<float>& v0,
                        const std::vector<float>& v1,
                        const std::vector<float>& index_inputs) {
  const size_t n = v0.size();
  if (i1 - i0 <= 1) {
    int code = 0;
    for (size_t i = 0; i < n; ++i) {
      if (v0[i] < v1[i]) {
        code |= 1 << (i * 3);
      } else if (v0[i] > v1[i]) {
        code |= 2 << (i * 3);
      }
    }
    return code;
  }
  const int ii = (i0 + i1) / 2;
  std::vector<float> vv(n);
  func->Call(pdfium::span_from_ref(index_inputs[ii]), vv);
  const int code = SampledMonotonicRec(func, i0, ii, v0, vv, index_inputs);
  if (code & (code >> 1)) {
    return code;  // Not monotonic by some component of the result.
  }
  return code | SampledMonotonicRec(func, ii, i1, vv, v1, index_inputs);
}

// gsfunc0.c fn_Sd_is_monotonic for one input (get_scaled_range,
// fn_Sd_1arg_linear_monotonic); pdfking: more inputs as "don't know"
int SampledIsMonotonic(const CPDF_SampledFunc* func,
                       const float* lower,
                       const float* upper,
                       unsigned* mask) {
  if (func->InputCount() != 1) {
    *mask = (1u << func->InputCount()) - 1;
    return 0;
  }
  const auto& encode = func->GetEncodeInfo()[0];
  const float d0 = func->GetDomain(0);
  const float d1 = func->GetDomain(1);
  const float size = static_cast<float>(encode.sizes);
  if (lower[0] < d0 || lower[0] > d1) {
    return kRangeCheck;
  }
  auto scaled = [&](float v) {
    float w = (v - d0) * (encode.encode_max - encode.encode_min) / (d1 - d0) +
              encode.encode_min;
    return w < 0 ? 0 : w >= size - 1 ? size - 1 : w;
  };
  float w0 = scaled(lower[0]);
  float w1 = scaled(upper[0]);
  if (w0 > w1) {
    std::swap(w0, w1);
  }
  const int i0 = static_cast<int>(floor(w0));
  const int i1 = static_cast<int>(ceil(w1));
  if (i1 - i0 > 1) {
    // the input each sample is the value at
    std::vector<float> index_inputs(encode.sizes);
    for (uint32_t i = 0; i < encode.sizes; ++i) {
      index_inputs[i] = d0 + (i - encode.encode_min) * (d1 - d0) /
                                 (encode.encode_max - encode.encode_min);
    }
    std::vector<float> v0(func->OutputCount());
    std::vector<float> v1(func->OutputCount());
    func->Call(pdfium::span_from_ref(index_inputs[i0]), v0);
    func->Call(pdfium::span_from_ref(index_inputs[i1]), v1);
    const int code = SampledMonotonicRec(func, i0, i1, v0, v1, index_inputs);
    if (code & (code >> 1)) {
      *mask = 1;
      return 0;
    }
  }
  *mask = 0;
  return 1;
}

// gs_function_is_monotonic of the function gs makes of `func`
int IsMonotonic(const CPDF_Function* func,
                const float* lower,
                const float* upper,
                unsigned* mask) {
  switch (func->GetType()) {
    case CPDF_Function::Type::kType2ExponentialInterpolation:
      return ExpIntIsMonotonic(func, lower, upper, mask);
    case CPDF_Function::Type::kType3Stitching:
      return StitchIsMonotonic(func->ToStitchFunc(), lower, upper, mask);
    case CPDF_Function::Type::kType0Sampled:
      return SampledIsMonotonic(func->ToSampledFunc(), lower, upper, mask);
    case CPDF_Function::Type::kType4PostScript:
    case CPDF_Function::Type::kTypeInvalid:
      // gsfunc4.c fn_PtCr_is_monotonic: no reasonable way to tell
      *mask = 0x49249249;
      return 0;
  }
}

}  // namespace

bool GsFillsShading(const CPDF_ShadingPattern* pattern) {
  switch (pattern->GetShadingType()) {
    case kFunctionBasedShading:
    case kAxialShading:
    case kRadialShading:
      return true;
    case kFreeFormGouraudTriangleMeshShading:
    case kLatticeFormGouraudTriangleMeshShading:
    case kCoonsPatchMeshShading:
    case kTensorProductPatchMeshShading:
      // the mesh is a stream's data
      return !!ToStream(pattern->GetShadingObject());
    case kInvalidShading:
    case kMaxShading:
      return false;
  }

}

void DrawGsShading(const RetainPtr<CFX_DIBitmap>& bitmap,
                   const FX_RECT& rect,
                   const FX_RECT& box,
                   const CFX_Matrix& matrix,
                   const CPDF_ShadingPattern* pattern,
                   const CPDF_Dictionary* dict,
                   const RetainPtr<CPDF_ColorSpace>& cs,
                   const CPDF_Inks* inks,
                   const CPDF_Plate* plate,
                   int intent,
                   int alpha) {
  const auto& funcs = pattern->GetFuncs();
  // the space colors are in: an Indexed one's base (gs's direct_space)
  const CPDF_IndexedCS* indexed = cs->AsIndexedCS();
  const CPDF_ColorSpace* space = indexed ? indexed->GetBaseCS() : cs.Get();
  const uint32_t n = space->ComponentCount();
  gs_shade::Colors colors;
  colors.num_components = static_cast<int>(n);
  for (uint32_t i = 0; i < n; ++i) {
    float value;
    float min;
    float max;
    space->GetDefaultValue(i, &value, &min, &max);
    colors.range_min.push_back(min);
    colors.range_max.push_back(max);
  }
  uint32_t outputs = 0;
  for (const auto& func : funcs) {
    if (func) {
      outputs += func->OutputCount();
    }
  }
  outputs = outputs ? std::max(outputs, n) : 0;
  if (!funcs.empty()) {
    if (outputs == 0) {
      return;
    }
    const int inputs =
        pattern->GetShadingType() == kFunctionBasedShading ? 2 : 1;
    colors.function = [&funcs, outputs, n, inputs](const float* in,
                                                   float* out) {
      std::vector<float> results(outputs);
      pdfium::span<float> result_span = pdfium::span(results);
      for (const auto& func : funcs) {
        if (!func) {
          continue;
        }
        std::optional<uint32_t> nresults = func->Call(
            UNSAFE_TODO(pdfium::span(in, static_cast<size_t>(inputs))),
            result_span);
        if (nresults.has_value()) {
          result_span = result_span.subspan(nresults.value());
        }
      }
      for (uint32_t i = 0; i < n; ++i) {
        UNSAFE_TODO(out[i]) = results[i];
      }
    };
    // gs makes an array of functions one (fn_AdOt_is_monotonic)
    colors.function_is_monotonic = [&funcs](const float* lower,
                                            const float* upper,
                                            unsigned* mask) {
      int code = 1;
      *mask = 0;
      for (const auto& func : funcs) {
        if (func) {
          code = IsMonotonic(func.get(), lower, upper, mask);
          if (code <= 0) {
            return code;
          }
        }
      }
      return code;
    };
  }

  gs_shade::Device device;
  const int width = bitmap->GetWidth();
  const int height = bitmap->GetHeight();
  // the pixels of the rectangle gs fills, `argb` each
  auto fill = [&bitmap, &rect, width, height](int x, int y, int w, int h,
                                              FX_ARGB argb) {
    const int x0 = std::max(x - rect.left, 0);
    const int x1 = std::min(x + w - rect.left, width);
    for (int row = std::max(y - rect.top, 0);
         row < std::min(y + h - rect.top, height); ++row) {
      auto line = bitmap->GetWritableScanlineAs<uint32_t>(row);
      for (int col = x0; col < x1; ++col) {
        line[col] = argb;
      }
    }
  };
  if (!plate) {
    // in color (a soft mask, a shape): on gs's soft mask device, gray; a
    // gray space through the identity link, any other as gs's -dUseFastColor
    // takes it to gray (gsicc_nocm.c, the luminance of its RGB)
    const bool gray = cs->GetFamily() == CPDF_ColorSpace::Family::kDeviceGray ||
                      cs->GetFamily() == CPDF_ColorSpace::Family::kCalGray ||
                      (cs->GetFamily() == CPDF_ColorSpace::Family::kICCBased &&
                       cs->ComponentCount() == 1);
    device.num_components = 1;
    device.remap = [space, n, gray](const float* cc, uint16_t* cv) {
      pdfium::span<const float> comps =
          UNSAFE_TODO(pdfium::span(cc, static_cast<size_t>(n)));
      float value = comps.empty() ? 0.0f : comps[0];
      if (!gray) {
        FX_RGB_STRUCT<float> rgb = space->GetRGBOrZerosOnError(comps);
        value = 0.30f * rgb.red + 0.59f * rgb.green + 0.11f * rgb.blue;
      }
      // gsicc.c: to 16 bits, truncated, to frac and back
      const int us = static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 65535.0);
      const int frac = (us >> 1) - (us >> 13);
      UNSAFE_TODO(cv[0]) = static_cast<uint16_t>((frac << 1) + (frac >> 11));
    };
    device.linearity = gray ? gs_shade::Device::Linearity::kAlways
                            : gs_shade::Device::Linearity::kIcc;
    device.fill = [&fill, alpha](int x, int y, int w, int h,
                                 const uint16_t* cv) {
      const int value = UNSAFE_TODO(cv[0]) >> 8;
      fill(x, y, w, h, ArgbEncode(alpha, value, value, value));
    };
  } else {
    size_t current = inks->size();
    for (size_t i = 0; i < inks->size(); ++i) {
      if (inks->plate(i) == plate) {
        current = i;
      }
    }
    if (current == inks->size()) {
      return;
    }
    device.num_components = static_cast<int>(inks->size());
    device.remap = [space, n, inks, intent](const float* cc, uint16_t* cv) {
      pdfium::span<const float> comps =
          UNSAFE_TODO(pdfium::span(cc, static_cast<size_t>(n)));
      for (size_t i = 0; i < inks->size(); ++i) {
        const CPDF_Plate* p = inks->plate(i);
        UNSAFE_TODO(cv[i]) = p->ColorValue(space, comps, intent);
      }
    };
    // gs's ICC link from the space to the device: none for the spots it
    // separates, the identity for CMYK on its CMYK device
    switch (space->GetFamily()) {
      case CPDF_ColorSpace::Family::kDeviceCMYK:
      case CPDF_ColorSpace::Family::kSeparation:
      case CPDF_ColorSpace::Family::kDeviceN:
        device.linearity = gs_shade::Device::Linearity::kAlways;
        break;
      default:
        device.linearity = gs_shade::Device::Linearity::kIcc;
        break;
    }
    device.fill = [&fill, plate, current, alpha](int x, int y, int w, int h,
                                                 const uint16_t* cv) {
      const int gray = plate->GrayOfValue(UNSAFE_TODO(cv[current]));
      fill(x, y, w, h, ArgbEncode(alpha, gray, gray, gray));
    };
  }

  gs_shade::Shading shading;
  shading.type = static_cast<int>(pattern->GetShadingType());
  RetainPtr<const CPDF_Array> coords = dict->GetArrayFor("Coords");
  for (size_t i = 0; coords && i < 6 && i < coords->size(); ++i) {
    shading.coords[i] = coords->GetFloatAt(i);
  }
  RetainPtr<const CPDF_Array> domain = dict->GetArrayFor("Domain");
  for (size_t i = 0; domain && i < 4 && i < domain->size(); ++i) {
    shading.domain[i] = domain->GetFloatAt(i);
  }
  RetainPtr<const CPDF_Array> extend = dict->GetArrayFor("Extend");
  shading.extend[0] = extend && extend->GetBooleanAt(0, false);
  shading.extend[1] = extend && extend->GetBooleanAt(1, false);
  const CFX_Matrix m = dict->GetMatrixFor("Matrix");
  shading.matrix = {m.a, m.b, m.c, m.d, m.e, m.f};
  if (RetainPtr<const CPDF_Stream> stream =
          ToStream(pattern->GetShadingObject())) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(stream));
    acc->LoadAllDataFiltered();
    pdfium::span<const uint8_t> data = acc->GetSpan();
    shading.data.assign(data.begin(), data.end());
    shading.bits_per_coordinate = dict->GetIntegerFor("BitsPerCoordinate");
    shading.bits_per_component = dict->GetIntegerFor("BitsPerComponent");
    shading.bits_per_flag = dict->GetIntegerFor("BitsPerFlag");
    shading.vertices_per_row = dict->GetIntegerFor("VerticesPerRow");
    if (RetainPtr<const CPDF_Array> decode = dict->GetArrayFor("Decode")) {
      for (size_t i = 0; i < decode->size(); ++i) {
        shading.decode.push_back(decode->GetFloatAt(i));
      }
    }
    // gs reads coordinates and colors (2 + n pairs, 1 with a Function)
    const size_t pairs = 2 + (funcs.empty() ? (indexed ? 1 : n) : 1);
    if (shading.decode.size() < 2 * pairs) {
      return;
    }
    if (!funcs.empty() && funcs[0]) {
      shading.function_domain[0] = funcs[0]->GetDomain(0);
      shading.function_domain[1] = funcs[0]->GetDomain(1);
    }
  }
  if (indexed) {
    shading.hival = indexed->GetMaxIndex();
    shading.lookup = [indexed, n](int index, float* out) {
      const float in = static_cast<float>(index);
      std::optional<DataVector<float>> comps =
          indexed->GetBaseComponents(pdfium::span_from_ref(in));
      for (uint32_t i = 0; i < n; ++i) {
        UNSAFE_TODO(out[i]) = comps.has_value() && i < comps->size()
                                  ? (*comps)[i]
                                  : 0.0f;
      }
    };
  }
  // gs's smoothness (pdfi's default) and flatness (its default)
  gs_shade::Fill(shading, colors, device,
                 {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f},
                 box.left * 256, box.top * 256, box.right * 256,
                 box.bottom * 256, 0.02f, 1.0f);
}

