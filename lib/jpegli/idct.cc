// Copyright (c) the JPEG XL Project Authors.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "lib/jpegli/idct.h"

#include <cmath>

#include "lib/base/status.h"
#include "lib/jpegli/decode_internal.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jpegli/idct.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jpegli/transpose-inl.h"

HWY_BEFORE_NAMESPACE();
namespace jpegli {
namespace HWY_NAMESPACE {

// These templates are not found via ADL.
using hwy::HWY_NAMESPACE::Abs;
using hwy::HWY_NAMESPACE::Add;
using hwy::HWY_NAMESPACE::Gt;
using hwy::HWY_NAMESPACE::IfThenElseZero;
using hwy::HWY_NAMESPACE::Mul;
using hwy::HWY_NAMESPACE::MulAdd;
using hwy::HWY_NAMESPACE::NegMulAdd;
using hwy::HWY_NAMESPACE::Rebind;
using hwy::HWY_NAMESPACE::Sub;
using hwy::HWY_NAMESPACE::Vec;
using hwy::HWY_NAMESPACE::Xor;

using D = HWY_FULL(float);
using DI = HWY_FULL(int32_t);
constexpr D d;
constexpr DI di;

using D8 = HWY_CAPPED(float, 8);
constexpr D8 d8;

void DequantBlock(const int16_t* JXL_RESTRICT qblock,
                  const float* JXL_RESTRICT dequant,
                  const float* JXL_RESTRICT biases, float* JXL_RESTRICT block) {
  for (size_t k = 0; k < 64; k += Lanes(d)) {
    const auto mul = Load(d, dequant + k);
    const auto bias = Load(d, biases + k);
    const Rebind<int16_t, DI> di16;
    const Vec<DI> quant_i = PromoteTo(di, Load(di16, qblock + k));
    const Rebind<float, DI> df;
    const auto quant = ConvertTo(df, quant_i);
    const auto abs_quant = Abs(quant);
    const auto not_0 = Gt(abs_quant, Zero(df));
    const auto sign_quant = Xor(quant, abs_quant);
    const auto biased_quant = Sub(quant, Xor(bias, sign_quant));
    const auto dequant = IfThenElseZero(not_0, Mul(biased_quant, mul));
    Store(dequant, d, block + k);
  }
}

template <size_t N>
void ForwardEvenOdd(const float* JXL_RESTRICT a_in, size_t a_in_stride,
                    float* JXL_RESTRICT a_out) {
  for (size_t i = 0; i < N / 2; i++) {
    auto in1 = LoadU(d8, a_in + 2 * i * a_in_stride);
    Store(in1, d8, a_out + i * 8);
  }
  for (size_t i = N / 2; i < N; i++) {
    auto in1 = LoadU(d8, a_in + (2 * (i - N / 2) + 1) * a_in_stride);
    Store(in1, d8, a_out + i * 8);
  }
}

template <size_t N>
void BTranspose(float* JXL_RESTRICT coeff) {
  for (size_t i = N - 1; i > 0; i--) {
    auto in1 = Load(d8, coeff + i * 8);
    auto in2 = Load(d8, coeff + (i - 1) * 8);
    Store(Add(in1, in2), d8, coeff + i * 8);
  }
  constexpr float kSqrt2 = 1.41421356237f;
  auto sqrt2 = Set(d8, kSqrt2);
  auto in1 = Load(d8, coeff);
  Store(Mul(in1, sqrt2), d8, coeff);
}

// Constants for DCT implementation. Generated by the following snippet:
// for i in range(N // 2):
//    print(1.0 / (2 * math.cos((i + 0.5) * math.pi / N)), end=", ")
template <size_t N>
struct WcMultipliers;

template <>
struct WcMultipliers<4> {
  static constexpr float kMultipliers[] = {
      0.541196100146197,
      1.3065629648763764,
  };
};

template <>
struct WcMultipliers<8> {
  static constexpr float kMultipliers[] = {
      0.5097955791041592,
      0.6013448869350453,
      0.8999762231364156,
      2.5629154477415055,
  };
};

constexpr float WcMultipliers<4>::kMultipliers[];
constexpr float WcMultipliers<8>::kMultipliers[];

template <size_t N>
void MultiplyAndAdd(const float* JXL_RESTRICT coeff, float* JXL_RESTRICT out,
                    size_t out_stride) {
  for (size_t i = 0; i < N / 2; i++) {
    auto mul = Set(d8, WcMultipliers<N>::kMultipliers[i]);
    auto in1 = Load(d8, coeff + i * 8);
    auto in2 = Load(d8, coeff + (N / 2 + i) * 8);
    auto out1 = MulAdd(mul, in2, in1);
    auto out2 = NegMulAdd(mul, in2, in1);
    StoreU(out1, d8, out + i * out_stride);
    StoreU(out2, d8, out + (N - i - 1) * out_stride);
  }
}

template <size_t N>
struct IDCT1DImpl;

template <>
struct IDCT1DImpl<1> {
  JXL_INLINE void operator()(const float* from, size_t from_stride, float* to,
                             size_t to_stride) {
    StoreU(LoadU(d8, from), d8, to);
  }
};

template <>
struct IDCT1DImpl<2> {
  JXL_INLINE void operator()(const float* from, size_t from_stride, float* to,
                             size_t to_stride) {
    JXL_DASSERT(from_stride >= 8);
    JXL_DASSERT(to_stride >= 8);
    auto in1 = LoadU(d8, from);
    auto in2 = LoadU(d8, from + from_stride);
    StoreU(Add(in1, in2), d8, to);
    StoreU(Sub(in1, in2), d8, to + to_stride);
  }
};

template <size_t N>
struct IDCT1DImpl {
  void operator()(const float* from, size_t from_stride, float* to,
                  size_t to_stride) {
    JXL_DASSERT(from_stride >= 8);
    JXL_DASSERT(to_stride >= 8);
    HWY_ALIGN float tmp[64];
    ForwardEvenOdd<N>(from, from_stride, tmp);
    IDCT1DImpl<N / 2>()(tmp, 8, tmp, 8);
    BTranspose<N / 2>(tmp + N * 4);
    IDCT1DImpl<N / 2>()(tmp + N * 4, 8, tmp + N * 4, 8);
    MultiplyAndAdd<N>(tmp, to, to_stride);
  }
};

template <size_t N>
void IDCT1D(float* JXL_RESTRICT from, float* JXL_RESTRICT output,
            size_t output_stride) {
  for (size_t i = 0; i < 8; i += Lanes(d8)) {
    IDCT1DImpl<N>()(from + i, 8, output + i, output_stride);
  }
}

void ComputeScaledIDCT(float* JXL_RESTRICT block0, float* JXL_RESTRICT block1,
                       float* JXL_RESTRICT output, size_t output_stride) {
  Transpose8x8Block(block0, block1);
  IDCT1D<8>(block1, block0, 8);
  Transpose8x8Block(block0, block1);
  IDCT1D<8>(block1, output, output_stride);
}

void InverseTransformBlock8x8(const int16_t* JXL_RESTRICT qblock,
                              const float* JXL_RESTRICT dequant,
                              const float* JXL_RESTRICT biases,
                              float* JXL_RESTRICT scratch_space,
                              float* JXL_RESTRICT output, size_t output_stride,
                              size_t dctsize) {
  float* JXL_RESTRICT block0 = scratch_space;
  float* JXL_RESTRICT block1 = scratch_space + DCTSIZE2;
  DequantBlock(qblock, dequant, biases, block0);
  ComputeScaledIDCT(block0, block1, output, output_stride);
}

// Computes the N-point IDCT of in[], and stores the result in out[]. The in[]
// array is at most 8 values long, values in[8:N-1] are assumed to be 0.
void Compute1dIDCT(const float* in, float* out, size_t N) {
  switch (N) {
    case 3: {
      static constexpr float kC3[3] = {
          1.414213562373,
          1.224744871392,
          0.707106781187,
      };
      float even0 = in[0] + kC3[2] * in[2];
      float even1 = in[0] - kC3[0] * in[2];
      float odd0 = kC3[1] * in[1];
      out[0] = even0 + odd0;
      out[2] = even0 - odd0;
      out[1] = even1;
      break;
    }
    case 5: {
      static constexpr float kC5[5] = {
          1.414213562373, 1.344997023928, 1.144122805635,
          0.831253875555, 0.437016024449,
      };
      float even0 = in[0] + kC5[2] * in[2] + kC5[4] * in[4];
      float even1 = in[0] - kC5[4] * in[2] - kC5[2] * in[4];
      float even2 = in[0] - kC5[0] * in[2] + kC5[0] * in[4];
      float odd0 = kC5[1] * in[1] + kC5[3] * in[3];
      float odd1 = kC5[3] * in[1] - kC5[1] * in[3];
      out[0] = even0 + odd0;
      out[4] = even0 - odd0;
      out[1] = even1 + odd1;
      out[3] = even1 - odd1;
      out[2] = even2;
      break;
    }
    case 6: {
      static constexpr float kC6[6] = {
          1.414213562373, 1.366025403784, 1.224744871392,
          1.000000000000, 0.707106781187, 0.366025403784,
      };
      float even0 = in[0] + kC6[2] * in[2] + kC6[4] * in[4];
      float even1 = in[0] - kC6[0] * in[4];
      float even2 = in[0] - kC6[2] * in[2] + kC6[4] * in[4];
      float odd0 = kC6[1] * in[1] + kC6[3] * in[3] + kC6[5] * in[5];
      float odd1 = kC6[3] * in[1] - kC6[3] * in[3] - kC6[3] * in[5];
      float odd2 = kC6[5] * in[1] - kC6[3] * in[3] + kC6[1] * in[5];
      out[0] = even0 + odd0;
      out[5] = even0 - odd0;
      out[1] = even1 + odd1;
      out[4] = even1 - odd1;
      out[2] = even2 + odd2;
      out[3] = even2 - odd2;
      break;
    }
    case 7: {
      static constexpr float kC7[7] = {
          1.414213562373, 1.378756275744, 1.274162392264, 1.105676685997,
          0.881747733790, 0.613604268353, 0.314692122713,
      };
      float even0 = in[0] + kC7[2] * in[2] + kC7[4] * in[4] + kC7[6] * in[6];
      float even1 = in[0] + kC7[6] * in[2] - kC7[2] * in[4] - kC7[4] * in[6];
      float even2 = in[0] - kC7[4] * in[2] - kC7[6] * in[4] + kC7[2] * in[6];
      float even3 = in[0] - kC7[0] * in[2] + kC7[0] * in[4] - kC7[0] * in[6];
      float odd0 = kC7[1] * in[1] + kC7[3] * in[3] + kC7[5] * in[5];
      float odd1 = kC7[3] * in[1] - kC7[5] * in[3] - kC7[1] * in[5];
      float odd2 = kC7[5] * in[1] - kC7[1] * in[3] + kC7[3] * in[5];
      out[0] = even0 + odd0;
      out[6] = even0 - odd0;
      out[1] = even1 + odd1;
      out[5] = even1 - odd1;
      out[2] = even2 + odd2;
      out[4] = even2 - odd2;
      out[3] = even3;
      break;
    }
    case 9: {
      static constexpr float kC9[9] = {
          1.414213562373, 1.392728480640, 1.328926048777,
          1.224744871392, 1.083350440839, 0.909038955344,
          0.707106781187, 0.483689525296, 0.245575607938,
      };
      float even0 = in[0] + kC9[2] * in[2] + kC9[4] * in[4] + kC9[6] * in[6];
      float even1 = in[0] + kC9[6] * in[2] - kC9[6] * in[4] - kC9[0] * in[6];
      float even2 = in[0] - kC9[8] * in[2] - kC9[2] * in[4] + kC9[6] * in[6];
      float even3 = in[0] - kC9[4] * in[2] + kC9[8] * in[4] + kC9[6] * in[6];
      float even4 = in[0] - kC9[0] * in[2] + kC9[0] * in[4] - kC9[0] * in[6];
      float odd0 =
          kC9[1] * in[1] + kC9[3] * in[3] + kC9[5] * in[5] + kC9[7] * in[7];
      float odd1 = kC9[3] * in[1] - kC9[3] * in[5] - kC9[3] * in[7];
      float odd2 =
          kC9[5] * in[1] - kC9[3] * in[3] - kC9[7] * in[5] + kC9[1] * in[7];
      float odd3 =
          kC9[7] * in[1] - kC9[3] * in[3] + kC9[1] * in[5] - kC9[5] * in[7];
      out[0] = even0 + odd0;
      out[8] = even0 - odd0;
      out[1] = even1 + odd1;
      out[7] = even1 - odd1;
      out[2] = even2 + odd2;
      out[6] = even2 - odd2;
      out[3] = even3 + odd3;
      out[5] = even3 - odd3;
      out[4] = even4;
      break;
    }
    case 10: {
      static constexpr float kC10[10] = {
          1.414213562373, 1.396802246667, 1.344997023928, 1.260073510670,
          1.144122805635, 1.000000000000, 0.831253875555, 0.642039521920,
          0.437016024449, 0.221231742082,
      };
      float even0 = in[0] + kC10[2] * in[2] + kC10[4] * in[4] + kC10[6] * in[6];
      float even1 = in[0] + kC10[6] * in[2] - kC10[8] * in[4] - kC10[2] * in[6];
      float even2 = in[0] - kC10[0] * in[4];
      float even3 = in[0] - kC10[6] * in[2] - kC10[8] * in[4] + kC10[2] * in[6];
      float even4 = in[0] - kC10[2] * in[2] + kC10[4] * in[4] - kC10[6] * in[6];
      float odd0 =
          kC10[1] * in[1] + kC10[3] * in[3] + kC10[5] * in[5] + kC10[7] * in[7];
      float odd1 =
          kC10[3] * in[1] + kC10[9] * in[3] - kC10[5] * in[5] - kC10[1] * in[7];
      float odd2 =
          kC10[5] * in[1] - kC10[5] * in[3] - kC10[5] * in[5] + kC10[5] * in[7];
      float odd3 =
          kC10[7] * in[1] - kC10[1] * in[3] + kC10[5] * in[5] + kC10[9] * in[7];
      float odd4 =
          kC10[9] * in[1] - kC10[7] * in[3] + kC10[5] * in[5] - kC10[3] * in[7];
      out[0] = even0 + odd0;
      out[9] = even0 - odd0;
      out[1] = even1 + odd1;
      out[8] = even1 - odd1;
      out[2] = even2 + odd2;
      out[7] = even2 - odd2;
      out[3] = even3 + odd3;
      out[6] = even3 - odd3;
      out[4] = even4 + odd4;
      out[5] = even4 - odd4;
      break;
    }
    case 11: {
      static constexpr float kC11[11] = {
          1.414213562373, 1.399818907436, 1.356927976287, 1.286413904599,
          1.189712155524, 1.068791297809, 0.926112931411, 0.764581576418,
          0.587485545401, 0.398430002847, 0.201263574413,
      };
      float even0 = in[0] + kC11[2] * in[2] + kC11[4] * in[4] + kC11[6] * in[6];
      float even1 =
          in[0] + kC11[6] * in[2] - kC11[10] * in[4] - kC11[4] * in[6];
      float even2 =
          in[0] + kC11[10] * in[2] - kC11[2] * in[4] - kC11[8] * in[6];
      float even3 = in[0] - kC11[8] * in[2] - kC11[6] * in[4] + kC11[2] * in[6];
      float even4 =
          in[0] - kC11[4] * in[2] + kC11[8] * in[4] + kC11[10] * in[6];
      float even5 = in[0] - kC11[0] * in[2] + kC11[0] * in[4] - kC11[0] * in[6];
      float odd0 =
          kC11[1] * in[1] + kC11[3] * in[3] + kC11[5] * in[5] + kC11[7] * in[7];
      float odd1 =
          kC11[3] * in[1] + kC11[9] * in[3] - kC11[7] * in[5] - kC11[1] * in[7];
      float odd2 =
          kC11[5] * in[1] - kC11[7] * in[3] - kC11[3] * in[5] + kC11[9] * in[7];
      float odd3 =
          kC11[7] * in[1] - kC11[1] * in[3] + kC11[9] * in[5] + kC11[5] * in[7];
      float odd4 =
          kC11[9] * in[1] - kC11[5] * in[3] + kC11[1] * in[5] - kC11[3] * in[7];
      out[0] = even0 + odd0;
      out[10] = even0 - odd0;
      out[1] = even1 + odd1;
      out[9] = even1 - odd1;
      out[2] = even2 + odd2;
      out[8] = even2 - odd2;
      out[3] = even3 + odd3;
      out[7] = even3 - odd3;
      out[4] = even4 + odd4;
      out[6] = even4 - odd4;
      out[5] = even5;
      break;
    }
    case 12: {
      static constexpr float kC12[12] = {
          1.414213562373, 1.402114769300, 1.366025403784, 1.306562964876,
          1.224744871392, 1.121971053594, 1.000000000000, 0.860918669154,
          0.707106781187, 0.541196100146, 0.366025403784, 0.184591911283,
      };
      float even0 = in[0] + kC12[2] * in[2] + kC12[4] * in[4] + kC12[6] * in[6];
      float even1 = in[0] + kC12[6] * in[2] - kC12[6] * in[6];
      float even2 =
          in[0] + kC12[10] * in[2] - kC12[4] * in[4] - kC12[6] * in[6];
      float even3 =
          in[0] - kC12[10] * in[2] - kC12[4] * in[4] + kC12[6] * in[6];
      float even4 = in[0] - kC12[6] * in[2] + kC12[6] * in[6];
      float even5 = in[0] - kC12[2] * in[2] + kC12[4] * in[4] - kC12[6] * in[6];
      float odd0 =
          kC12[1] * in[1] + kC12[3] * in[3] + kC12[5] * in[5] + kC12[7] * in[7];
      float odd1 =
          kC12[3] * in[1] + kC12[9] * in[3] - kC12[9] * in[5] - kC12[3] * in[7];
      float odd2 = kC12[5] * in[1] - kC12[9] * in[3] - kC12[1] * in[5] -
                   kC12[11] * in[7];
      float odd3 = kC12[7] * in[1] - kC12[3] * in[3] - kC12[11] * in[5] +
                   kC12[1] * in[7];
      float odd4 =
          kC12[9] * in[1] - kC12[3] * in[3] + kC12[3] * in[5] - kC12[9] * in[7];
      float odd5 = kC12[11] * in[1] - kC12[9] * in[3] + kC12[7] * in[5] -
                   kC12[5] * in[7];
      out[0] = even0 + odd0;
      out[11] = even0 - odd0;
      out[1] = even1 + odd1;
      out[10] = even1 - odd1;
      out[2] = even2 + odd2;
      out[9] = even2 - odd2;
      out[3] = even3 + odd3;
      out[8] = even3 - odd3;
      out[4] = even4 + odd4;
      out[7] = even4 - odd4;
      out[5] = even5 + odd5;
      out[6] = even5 - odd5;
      break;
    }
    case 13: {
      static constexpr float kC13[13] = {
          1.414213562373, 1.403902353238, 1.373119086479, 1.322312651445,
          1.252223920364, 1.163874944761, 1.058554051646, 0.937797056801,
          0.803364869133, 0.657217812653, 0.501487040539, 0.338443458124,
          0.170464607981,
      };
      float even0 = in[0] + kC13[2] * in[2] + kC13[4] * in[4] + kC13[6] * in[6];
      float even1 =
          in[0] + kC13[6] * in[2] + kC13[12] * in[4] - kC13[8] * in[6];
      float even2 =
          in[0] + kC13[10] * in[2] - kC13[6] * in[4] - kC13[4] * in[6];
      float even3 =
          in[0] - kC13[12] * in[2] - kC13[2] * in[4] + kC13[10] * in[6];
      float even4 =
          in[0] - kC13[8] * in[2] - kC13[10] * in[4] + kC13[2] * in[6];
      float even5 =
          in[0] - kC13[4] * in[2] + kC13[8] * in[4] - kC13[12] * in[6];
      float even6 = in[0] - kC13[0] * in[2] + kC13[0] * in[4] - kC13[0] * in[6];
      float odd0 =
          kC13[1] * in[1] + kC13[3] * in[3] + kC13[5] * in[5] + kC13[7] * in[7];
      float odd1 = kC13[3] * in[1] + kC13[9] * in[3] - kC13[11] * in[5] -
                   kC13[5] * in[7];
      float odd2 = kC13[5] * in[1] - kC13[11] * in[3] - kC13[1] * in[5] -
                   kC13[9] * in[7];
      float odd3 =
          kC13[7] * in[1] - kC13[5] * in[3] - kC13[9] * in[5] + kC13[3] * in[7];
      float odd4 = kC13[9] * in[1] - kC13[1] * in[3] + kC13[7] * in[5] +
                   kC13[11] * in[7];
      float odd5 = kC13[11] * in[1] - kC13[7] * in[3] + kC13[3] * in[5] -
                   kC13[1] * in[7];
      out[0] = even0 + odd0;
      out[12] = even0 - odd0;
      out[1] = even1 + odd1;
      out[11] = even1 - odd1;
      out[2] = even2 + odd2;
      out[10] = even2 - odd2;
      out[3] = even3 + odd3;
      out[9] = even3 - odd3;
      out[4] = even4 + odd4;
      out[8] = even4 - odd4;
      out[5] = even5 + odd5;
      out[7] = even5 - odd5;
      out[6] = even6;
      break;
    }
    case 14: {
      static constexpr float kC14[14] = {
          1.414213562373, 1.405321284327, 1.378756275744, 1.334852607020,
          1.274162392264, 1.197448846138, 1.105676685997, 1.000000000000,
          0.881747733790, 0.752406978226, 0.613604268353, 0.467085128785,
          0.314692122713, 0.158341680609,
      };
      float even0 = in[0] + kC14[2] * in[2] + kC14[4] * in[4] + kC14[6] * in[6];
      float even1 =
          in[0] + kC14[6] * in[2] + kC14[12] * in[4] - kC14[10] * in[6];
      float even2 =
          in[0] + kC14[10] * in[2] - kC14[8] * in[4] - kC14[2] * in[6];
      float even3 = in[0] - kC14[0] * in[4];
      float even4 =
          in[0] - kC14[10] * in[2] - kC14[8] * in[4] + kC14[2] * in[6];
      float even5 =
          in[0] - kC14[6] * in[2] + kC14[12] * in[4] + kC14[10] * in[6];
      float even6 = in[0] - kC14[2] * in[2] + kC14[4] * in[4] - kC14[6] * in[6];
      float odd0 =
          kC14[1] * in[1] + kC14[3] * in[3] + kC14[5] * in[5] + kC14[7] * in[7];
      float odd1 = kC14[3] * in[1] + kC14[9] * in[3] - kC14[13] * in[5] -
                   kC14[7] * in[7];
      float odd2 = kC14[5] * in[1] - kC14[13] * in[3] - kC14[3] * in[5] -
                   kC14[7] * in[7];
      float odd3 =
          kC14[7] * in[1] - kC14[7] * in[3] - kC14[7] * in[5] + kC14[7] * in[7];
      float odd4 = kC14[9] * in[1] - kC14[1] * in[3] + kC14[11] * in[5] +
                   kC14[7] * in[7];
      float odd5 = kC14[11] * in[1] - kC14[5] * in[3] + kC14[1] * in[5] -
                   kC14[7] * in[7];
      float odd6 = kC14[13] * in[1] - kC14[11] * in[3] + kC14[9] * in[5] -
                   kC14[7] * in[7];
      out[0] = even0 + odd0;
      out[13] = even0 - odd0;
      out[1] = even1 + odd1;
      out[12] = even1 - odd1;
      out[2] = even2 + odd2;
      out[11] = even2 - odd2;
      out[3] = even3 + odd3;
      out[10] = even3 - odd3;
      out[4] = even4 + odd4;
      out[9] = even4 - odd4;
      out[5] = even5 + odd5;
      out[8] = even5 - odd5;
      out[6] = even6 + odd6;
      out[7] = even6 - odd6;
      break;
    }
    case 15: {
      static constexpr float kC15[15] = {
          1.414213562373, 1.406466352507, 1.383309602960, 1.344997023928,
          1.291948376043, 1.224744871392, 1.144122805635, 1.050965490998,
          0.946293578512, 0.831253875555, 0.707106781187, 0.575212476952,
          0.437016024449, 0.294031532930, 0.147825570407,
      };
      float even0 = in[0] + kC15[2] * in[2] + kC15[4] * in[4] + kC15[6] * in[6];
      float even1 =
          in[0] + kC15[6] * in[2] + kC15[12] * in[4] - kC15[12] * in[6];
      float even2 =
          in[0] + kC15[10] * in[2] - kC15[10] * in[4] - kC15[0] * in[6];
      float even3 =
          in[0] + kC15[14] * in[2] - kC15[2] * in[4] - kC15[12] * in[6];
      float even4 =
          in[0] - kC15[12] * in[2] - kC15[6] * in[4] + kC15[6] * in[6];
      float even5 =
          in[0] - kC15[8] * in[2] - kC15[14] * in[4] + kC15[6] * in[6];
      float even6 =
          in[0] - kC15[4] * in[2] + kC15[8] * in[4] - kC15[12] * in[6];
      float even7 = in[0] - kC15[0] * in[2] + kC15[0] * in[4] - kC15[0] * in[6];
      float odd0 =
          kC15[1] * in[1] + kC15[3] * in[3] + kC15[5] * in[5] + kC15[7] * in[7];
      float odd1 = kC15[3] * in[1] + kC15[9] * in[3] - kC15[9] * in[7];
      float odd2 = kC15[5] * in[1] - kC15[5] * in[5] - kC15[5] * in[7];
      float odd3 = kC15[7] * in[1] - kC15[9] * in[3] - kC15[5] * in[5] +
                   kC15[11] * in[7];
      float odd4 = kC15[9] * in[1] - kC15[3] * in[3] + kC15[3] * in[7];
      float odd5 = kC15[11] * in[1] - kC15[3] * in[3] + kC15[5] * in[5] -
                   kC15[13] * in[7];
      float odd6 = kC15[13] * in[1] - kC15[9] * in[3] + kC15[5] * in[5] -
                   kC15[1] * in[7];
      out[0] = even0 + odd0;
      out[14] = even0 - odd0;
      out[1] = even1 + odd1;
      out[13] = even1 - odd1;
      out[2] = even2 + odd2;
      out[12] = even2 - odd2;
      out[3] = even3 + odd3;
      out[11] = even3 - odd3;
      out[4] = even4 + odd4;
      out[10] = even4 - odd4;
      out[5] = even5 + odd5;
      out[9] = even5 - odd5;
      out[6] = even6 + odd6;
      out[8] = even6 - odd6;
      out[7] = even7;
      break;
    }
    case 16: {
      static constexpr float kC16[16] = {
          1.414213562373, 1.407403737526, 1.387039845322, 1.353318001174,
          1.306562964876, 1.247225012987, 1.175875602419, 1.093201867002,
          1.000000000000, 0.897167586343, 0.785694958387, 0.666655658478,
          0.541196100146, 0.410524527522, 0.275899379283, 0.138617169199,
      };
      float even0 = in[0] + kC16[2] * in[2] + kC16[4] * in[4] + kC16[6] * in[6];
      float even1 =
          in[0] + kC16[6] * in[2] + kC16[12] * in[4] - kC16[14] * in[6];
      float even2 =
          in[0] + kC16[10] * in[2] - kC16[12] * in[4] - kC16[2] * in[6];
      float even3 =
          in[0] + kC16[14] * in[2] - kC16[4] * in[4] - kC16[10] * in[6];
      float even4 =
          in[0] - kC16[14] * in[2] - kC16[4] * in[4] + kC16[10] * in[6];
      float even5 =
          in[0] - kC16[10] * in[2] - kC16[12] * in[4] + kC16[2] * in[6];
      float even6 =
          in[0] - kC16[6] * in[2] + kC16[12] * in[4] + kC16[14] * in[6];
      float even7 = in[0] - kC16[2] * in[2] + kC16[4] * in[4] - kC16[6] * in[6];
      float odd0 = (kC16[1] * in[1] + kC16[3] * in[3] + kC16[5] * in[5] +
                    kC16[7] * in[7]);
      float odd1 = (kC16[3] * in[1] + kC16[9] * in[3] + kC16[15] * in[5] -
                    kC16[11] * in[7]);
      float odd2 = (kC16[5] * in[1] + kC16[15] * in[3] - kC16[7] * in[5] -
                    kC16[3] * in[7]);
      float odd3 = (kC16[7] * in[1] - kC16[11] * in[3] - kC16[3] * in[5] +
                    kC16[15] * in[7]);
      float odd4 = (kC16[9] * in[1] - kC16[5] * in[3] - kC16[13] * in[5] +
                    kC16[1] * in[7]);
      float odd5 = (kC16[11] * in[1] - kC16[1] * in[3] + kC16[9] * in[5] +
                    kC16[13] * in[7]);
      float odd6 = (kC16[13] * in[1] - kC16[7] * in[3] + kC16[1] * in[5] -
                    kC16[5] * in[7]);
      float odd7 = (kC16[15] * in[1] - kC16[13] * in[3] + kC16[11] * in[5] -
                    kC16[9] * in[7]);
      out[0] = even0 + odd0;
      out[15] = even0 - odd0;
      out[1] = even1 + odd1;
      out[14] = even1 - odd1;
      out[2] = even2 + odd2;
      out[13] = even2 - odd2;
      out[3] = even3 + odd3;
      out[12] = even3 - odd3;
      out[4] = even4 + odd4;
      out[11] = even4 - odd4;
      out[5] = even5 + odd5;
      out[10] = even5 - odd5;
      out[6] = even6 + odd6;
      out[9] = even6 - odd6;
      out[7] = even7 + odd7;
      out[8] = even7 - odd7;
      break;
    }
    default:
      JXL_ABORT("Compute1dIDCT does not support N=%d", static_cast<int>(N));
      break;
  }
}

void InverseTransformBlockGeneric(const int16_t* JXL_RESTRICT qblock,
                                  const float* JXL_RESTRICT dequant,
                                  const float* JXL_RESTRICT biases,
                                  float* JXL_RESTRICT scratch_space,
                                  float* JXL_RESTRICT output,
                                  size_t output_stride, size_t dctsize) {
  float* JXL_RESTRICT block0 = scratch_space;
  float* JXL_RESTRICT block1 = scratch_space + DCTSIZE2;
  DequantBlock(qblock, dequant, biases, block0);
  if (dctsize == 1) {
    *output = *block0;
  } else if (dctsize == 2 || dctsize == 4) {
    float* JXL_RESTRICT block2 = scratch_space + 2 * DCTSIZE2;
    ComputeScaledIDCT(block0, block1, block2, 8);
    if (dctsize == 4) {
      for (size_t iy = 0; iy < 4; ++iy) {
        for (size_t ix = 0; ix < 4; ++ix) {
          float* block = &block2[16 * iy + 2 * ix];
          output[iy * output_stride + ix] =
              0.25f * (block[0] + block[1] + block[8] + block[9]);
        }
      }
    } else {
      for (size_t iy = 0; iy < 2; ++iy) {
        for (size_t ix = 0; ix < 2; ++ix) {
          float* block = &block2[32 * iy + 4 * ix];
          output[iy * output_stride + ix] =
              0.0625f *
              (block[0] + block[1] + block[2] + block[3] + block[8] + block[9] +
               block[10] + block[11] + block[16] + block[17] + block[18] +
               block[19] + block[24] + block[25] + block[26] + block[27]);
        }
      }
    }
  } else {
    float dctin[DCTSIZE];
    float dctout[DCTSIZE * 2];
    size_t insize = std::min<size_t>(dctsize, DCTSIZE);
    for (size_t ix = 0; ix < insize; ++ix) {
      for (size_t iy = 0; iy < insize; ++iy) {
        dctin[iy] = block0[iy * DCTSIZE + ix];
      }
      Compute1dIDCT(dctin, dctout, dctsize);
      for (size_t iy = 0; iy < dctsize; ++iy) {
        block1[iy * dctsize + ix] = dctout[iy];
      }
    }
    for (size_t iy = 0; iy < dctsize; ++iy) {
      Compute1dIDCT(block1 + iy * dctsize, output + iy * output_stride,
                    dctsize);
    }
  }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jpegli
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jpegli {

HWY_EXPORT(InverseTransformBlock8x8);
HWY_EXPORT(InverseTransformBlockGeneric);

void ChooseInverseTransform(j_decompress_ptr cinfo) {
  jpeg_decomp_master* m = cinfo->master;
  for (int c = 0; c < cinfo->num_components; ++c) {
    if (m->scaled_dct_size[c] == DCTSIZE) {
      m->inverse_transform[c] = HWY_DYNAMIC_DISPATCH(InverseTransformBlock8x8);
    } else {
      m->inverse_transform[c] =
          HWY_DYNAMIC_DISPATCH(InverseTransformBlockGeneric);
    }
  }
}

}  // namespace jpegli
#endif  // HWY_ONCE
