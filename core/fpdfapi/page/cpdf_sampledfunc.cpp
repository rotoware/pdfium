// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/page/cpdf_sampledfunc.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/cfx_bitstream.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_ceil_div.h"
#include "core/fxcrt/fx_memory_wrappers.h"
#include "core/fxcrt/fx_safe_types.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace {

// See PDF Reference 1.7, page 170, table 3.36.
bool IsValidBitsPerSample(uint32_t x) {
  switch (x) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 12:
    case 16:
    case 24:
    case 32:
      return true;
    default:
      return false;
  }
}

}  // namespace

CPDF_SampledFunc::CPDF_SampledFunc() : CPDF_Function(Type::kType0Sampled) {}

CPDF_SampledFunc::~CPDF_SampledFunc() = default;

bool CPDF_SampledFunc::v_Init(const CPDF_Object* pObj, VisitedSet* pVisited) {
  RetainPtr<const CPDF_Stream> pStream(pObj->AsStream());
  if (!pStream) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dict = pStream->GetDict();
  RetainPtr<const CPDF_Array> pSize = dict->GetArrayFor("Size");
  if (!pSize || pSize->IsEmpty()) {
    return false;
  }

  bits_per_sample_ = dict->GetIntegerFor("BitsPerSample");
  if (!IsValidBitsPerSample(bits_per_sample_)) {
    return false;
  }

  FX_SAFE_UINT32 nTotalSampleBits = bits_per_sample_;
  nTotalSampleBits *= outputs_;
  RetainPtr<const CPDF_Array> pEncode = dict->GetArrayFor("Encode");
  encode_info_.resize(inputs_);
  for (uint32_t i = 0; i < inputs_; i++) {
    int size = pSize->GetIntegerAt(i);
    if (size <= 0) {
      return false;
    }

    encode_info_[i].sizes = size;
    nTotalSampleBits *= encode_info_[i].sizes;
    if (pEncode) {
      encode_info_[i].encode_min = pEncode->GetFloatAt(i * 2);
      encode_info_[i].encode_max = pEncode->GetFloatAt(i * 2 + 1);
    } else {
      encode_info_[i].encode_min = 0;
      encode_info_[i].encode_max =
          encode_info_[i].sizes == 1 ? 1 : encode_info_[i].sizes - 1;
    }
  }
  FX_SAFE_UINT32 nTotalSampleBytes = fxcrt::CeilDiv(nTotalSampleBits, 8);
  if (!nTotalSampleBytes.IsValid() || nTotalSampleBytes.ValueOrDie() == 0) {
    return false;
  }

  sample_max_ = 0xffffffff >> (32 - bits_per_sample_);
  sample_stream_ = pdfium::MakeRetain<CPDF_StreamAcc>(std::move(pStream));
  sample_stream_->LoadAllDataFiltered();
  if (nTotalSampleBytes.ValueOrDie() > sample_stream_->GetSize()) {
    return false;
  }

  RetainPtr<const CPDF_Array> pDecode = dict->GetArrayFor("Decode");
  decode_info_.resize(outputs_);
  for (uint32_t i = 0; i < outputs_; i++) {
    if (pDecode) {
      decode_info_[i].decode_min = pDecode->GetFloatAt(2 * i);
      decode_info_[i].decode_max = pDecode->GetFloatAt(2 * i + 1);
    } else {
      decode_info_[i].decode_min = ranges_[i * 2];
      decode_info_[i].decode_max = ranges_[i * 2 + 1];
    }
  }
  return true;
}

bool CPDF_SampledFunc::v_Call(pdfium::span<const float> inputs,
                              pdfium::span<float> results) const {
  int pos = 0;
  absl::InlinedVector<float, 16, FxAllocAllocator<float>> encoded_input(
      inputs_);
  absl::InlinedVector<uint32_t, 16, FxAllocAllocator<uint32_t>> index(inputs_);
  absl::InlinedVector<uint32_t, 16, FxAllocAllocator<uint32_t>> blocksize(
      inputs_);
  for (uint32_t i = 0; i < inputs_; i++) {
    if (i == 0) {
      blocksize[i] = 1;
    } else {
      blocksize[i] = blocksize[i - 1] * encode_info_[i - 1].sizes;
    }
    encoded_input[i] =
        Interpolate(inputs[i], domains_[i * 2], domains_[i * 2 + 1],
                    encode_info_[i].encode_min, encode_info_[i].encode_max);
    index[i] = std::clamp(static_cast<uint32_t>(encoded_input[i]), 0U,
                          encode_info_[i].sizes - 1);
    pos += index[i] * blocksize[i];
  }
  FX_SAFE_INT32 bits_to_output = outputs_;
  bits_to_output *= bits_per_sample_;
  if (!bits_to_output.IsValid()) {
    return false;
  }

  pdfium::span<const uint8_t> pSampleData = sample_stream_->GetSpan();
  if (pSampleData.empty()) {
    return false;
  }

  // pdfking: multilinear between the 2^m samples around the input, as gs
  // interpolates (gsfunc0.c fn_interpolate_linear): only along the inputs
  // that fall between two samples
  absl::InlinedVector<uint32_t, 16, FxAllocAllocator<uint32_t>> between;
  for (uint32_t j = 0; j < inputs_; ++j) {
    if (index[j] + 1 < encode_info_[j].sizes &&
        encoded_input[j] > static_cast<float>(index[j])) {
      between.push_back(j);
    }
  }
  if (between.size() > 16) {
    return false;
  }
  auto sample_at = [&](int at, uint32_t i) -> std::optional<float> {
    FX_SAFE_INT32 bitpos = at;
    bitpos *= outputs_;
    bitpos += i;
    bitpos *= bits_per_sample_;
    FX_SAFE_INT32 end_bit = bitpos;
    end_bit += bits_per_sample_;
    const int start = bitpos.ValueOrDefault(-1);
    const int end = end_bit.ValueOrDefault(-1);
    if (start < 0 || end < 0 ||
        static_cast<size_t>(end) > pSampleData.size() * 8) {
      return std::nullopt;
    }
    CFX_BitStream bitstream(pSampleData);
    bitstream.SkipBits(start);
    return static_cast<float>(bitstream.GetBits(bits_per_sample_));
  };
  for (uint32_t i = 0; i < outputs_; ++i) {
    float encoded = 0;
    for (uint32_t corner = 0; corner < (1u << between.size()); ++corner) {
      float weight = 1;
      int at = pos;
      for (size_t k = 0; k < between.size(); ++k) {
        const uint32_t j = between[k];
        const float fraction = encoded_input[j] - index[j];
        if (corner & (1u << k)) {
          weight *= fraction;
          at += blocksize[j];
        } else {
          weight *= 1 - fraction;
        }
      }
      std::optional<float> sample = sample_at(at, i);
      if (!sample.has_value()) {
        return false;
      }
      encoded += weight * sample.value();
    }
    results[i] =
        Interpolate(encoded, 0, sample_max_, decode_info_[i].decode_min,
                    decode_info_[i].decode_max);
  }
  return true;
}

#if defined(PDF_USE_SKIA)
RetainPtr<CPDF_StreamAcc> CPDF_SampledFunc::GetSampleStream() const {
  return sample_stream_;
}
#endif
