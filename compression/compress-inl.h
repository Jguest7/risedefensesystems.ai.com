// Copyright 2024 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Include guard for headers.
#ifndef THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_
#define THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <array>
#include <cmath>  // lroundf, only if COMPRESS_STATS

#include "compression/blob_store.h"
#include "compression/compress.h"
#include "compression/distortion.h"
#include "hwy/aligned_allocator.h"
#include "hwy/base.h"
#include "hwy/contrib/thread_pool/thread_pool.h"
#include "hwy/timer.h"

#endif  // THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_

// Include guard for (potentially) SIMD code.
#if defined(THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE) == defined(HWY_TARGET_TOGGLE)
#ifdef THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#undef THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#else
#define THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#endif

#include "compression/nuq-inl.h"
#include "compression/sfp-inl.h"
#include "hwy/contrib/dot/dot-inl.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace gcpp {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Enables generic code independent of compression type.
template <typename T>  // primary, must specialize
struct CompressTraits {};

// Useful for backprop/, where weights are currently f32.
template <>
struct CompressTraits<float> {
  using MatT = float;
  static const char* Name() { return "f32"; }
  static constexpr bool kSupportsEvenOdd = false;  // unnecessary

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT in,
                                  size_t num, CompressPerThread& tls,
                                  size_t /*out_capacity*/,
                                  MatT* HWY_RESTRICT out, size_t out_ofs) {
    using VF = hn::Vec<decltype(df)>;
    const size_t N = hn::Lanes(df);
    HWY_DASSERT(num >= 2 * N && num % (2 * N) == 0);

    for (size_t i = 0; i < num; i += 2 * N) {
      const VF in0 = hn::LoadU(df, in + i);
      const VF in1 = hn::LoadU(df, in + i + N);
      hn::StoreU(in0, df, out + out_ofs + i);
      hn::StoreU(in1, df, out + out_ofs + i + N);
    }
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Decompress2(DF df, const MatT* HWY_RESTRICT in,
                                     size_t in_ofs, hn::Vec<DF>& f0,
                                     hn::Vec<DF>& f1) {
    const size_t N = hn::Lanes(df);
    f0 = hn::LoadU(df, in + in_ofs);
    f1 = hn::LoadU(df, in + in_ofs + N);
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Decompress(DF df, size_t /*in_capacity*/,
                                    const MatT* HWY_RESTRICT in, size_t in_ofs,
                                    float* HWY_RESTRICT out, size_t num) {
    using VF = hn::Vec<decltype(df)>;
    const size_t N = hn::Lanes(df);
    HWY_DASSERT(num >= 2 * N && num % (2 * N) == 0);

    for (size_t i = 0; i < num; i += 2 * N) {
      VF in0, in1;
      Decompress2(df, in, in_ofs + i, in0, in1);
      hn::StoreU(in0, df, out + i);
      hn::StoreU(in1, df, out + i + N);
    }
  }

  // VecT can be float or hwy::bfloat16_t.
  template <class DF, typename VecT, HWY_IF_F32_D(DF)>
  static HWY_INLINE float Dot(DF df, size_t /*in_capacity*/,
                              const MatT* HWY_RESTRICT in, size_t in_ofs,
                              const VecT* HWY_RESTRICT vec_aligned,
                              size_t num) {
    HWY_DASSERT(num >= hn::Lanes(df) && (num % hn::Lanes(df)) == 0);
    HWY_DASSERT(hn::IsAligned(df, vec_aligned));
    constexpr int kAssumptions =
        hn::Dot::kAtLeastOneVector | hn::Dot::kMultipleOfVector;
    // vec_aligned must be the second argument because hn::Dot supports f32*bf16
    // and f32*f32.
    return hn::Dot::Compute<kAssumptions>(df, in + in_ofs, vec_aligned, num);
  }
};

template <>
struct CompressTraits<hwy::bfloat16_t> {
  using MatT = hwy::bfloat16_t;
  static const char* Name() { return "bf16"; }
  static constexpr bool kSupportsEvenOdd = true;

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT in,
                                  size_t num, CompressPerThread& tls,
                                  size_t /*out_capacity*/,
                                  MatT* HWY_RESTRICT out, size_t out_ofs) {
    const hn::RebindToUnsigned<decltype(df)> du;
    const hn::Repartition<hwy::bfloat16_t, decltype(df)> dbf;
    using VF = hn::Vec<decltype(df)>;
    const size_t N = hn::Lanes(df);

    hn::Vec<decltype(du)> or_sum = hn::Zero(du);

    size_t i = 0;
    if (num >= 2 * N) {
      for (; i <= num - 2 * N; i += 2 * N) {
        const VF in0 = hn::LoadU(df, in + i);
        const VF in1 = hn::LoadU(df, in + i + N);

        // Sticky bits so we can warn if any lower bits were set.
        or_sum = hn::Or3(or_sum, hn::BitCast(du, in0), hn::BitCast(du, in1));
        hn::StoreU(hn::OrderedDemote2To(dbf, in0, in1), dbf, out + out_ofs + i);

        if (COMPRESS_STATS) {
          DistortionStats stats;
          for (size_t j = 0; j < 2 * N; ++j) {
            stats.Notify(in[i + j], hwy::F32FromBF16(out[out_ofs + i + j]));
          }
          tls.stats.Notify(stats);
        }
      }
    }

    const size_t remaining = num - i;
    if (remaining != 0) {
      const VF in0 = hn::LoadN(df, in + i, remaining);
      const size_t remaining1 = remaining - HWY_MIN(remaining, N / 2);
      const VF in1 = hn::LoadN(df, in + i + N, remaining1);

      // Sticky bits so we can warn if any lower bits were set.
      or_sum = hn::Or3(or_sum, hn::BitCast(du, in0), hn::BitCast(du, in1));
      hn::StoreU(hn::OrderedDemote2To(dbf, in0, in1), dbf, out + out_ofs + i);

      if (COMPRESS_STATS) {
        DistortionStats stats;
        for (size_t j = 0; j < remaining; ++j) {
          stats.Notify(in[i + j], hwy::F32FromBF16(out[out_ofs + i + j]));
        }
        tls.stats.Notify(stats);
      }
    }

    // If the lower 16 bits are not zero, we should implement rounding.
    or_sum = hn::And(or_sum, hn::Set(du, 0xFFFF));
    if (!hn::AllTrue(du, hn::Eq(or_sum, hn::Zero(du)))) {
      // fprintf(stderr, "Warning: Lossy truncation.");
    }
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Decompress2(DF df, const MatT* HWY_RESTRICT in,
                                     size_t in_ofs, hn::Vec<DF>& f0,
                                     hn::Vec<DF>& f1) {
    const hn::Repartition<hwy::bfloat16_t, decltype(df)> dbf;
    using VBF = hn::Vec<decltype(dbf)>;
    const VBF in16 = hn::LoadU(dbf, in + in_ofs);
    f0 = hn::PromoteLowerTo(df, in16);
    f1 = hn::PromoteUpperTo(df, in16);
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Decompress(DF df, size_t /*in_capacity*/,
                                    const MatT* HWY_RESTRICT in, size_t in_ofs,
                                    float* HWY_RESTRICT out, size_t num) {
    const hn::Repartition<hwy::bfloat16_t, decltype(df)> dbf;
    using VBF = hn::Vec<decltype(dbf)>;
    using VF = hn::Vec<decltype(df)>;
    const size_t N16 = hn::Lanes(dbf);

    size_t i = 0;
    if (num >= N16) {
      for (i = 0; i <= num - N16; i += N16) {
        VF in0, in1;
        Decompress2(df, in, in_ofs + i, in0, in1);
        hn::StoreU(in0, df, out + i);
        hn::StoreU(in1, df, out + i + N16 / 2);
      }
    }

    const size_t remaining = num - i;
    if (remaining != 0) {
      const VBF in16 = hn::LoadN(dbf, in + in_ofs + i, remaining);
      const VF in0 = hn::PromoteLowerTo(df, in16);
      const VF in1 = hn::PromoteUpperTo(df, in16);
      hn::StoreN(in0, df, out + i, remaining);
      // Avoid wraparound, potentially store nothing.
      const size_t remaining1 = remaining - HWY_MIN(remaining, N16 / 2);
      hn::StoreN(in1, df, out + i + N16 / 2, remaining1);
    }
  }

  // VecT can be float or hwy::bfloat16_t.
  template <class DF, typename VecT, HWY_IF_F32_D(DF)>
  static HWY_INLINE float Dot(DF df, size_t /*in_capacity*/,
                              const MatT* HWY_RESTRICT in, size_t in_ofs,
                              const VecT* HWY_RESTRICT vec_aligned,
                              size_t num) {
    HWY_DASSERT(num >= hn::Lanes(df) && (num % hn::Lanes(df)) == 0);
    HWY_DASSERT(hn::IsAligned(df, vec_aligned));

    const hn::Repartition<VecT, decltype(df)> d_vec;

    constexpr int kAssumptions =
        hn::Dot::kAtLeastOneVector | hn::Dot::kMultipleOfVector;
    // vec_aligned must be first argument because hn::Dot supports f32*bf16 and
    // bf16*bf16.
    return hn::Dot::Compute<kAssumptions>(d_vec, vec_aligned, in + in_ofs, num);
  }

  // Computes the dot product of an even-odd deinterleaved, f32 `vec_aligned`
  // and a column- major matrix `in`. `vec_aligned` should be aligned and
  // alternate even-indexed `hn::Lanes(df32)` elements followed by odd-indexed
  // `hn::Lanes(df32)` elements.
  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE float DotEO(
      const DF df32, const hwy::bfloat16_t* HWY_RESTRICT in, size_t in_ofs,
      const float* HWY_RESTRICT vec_aligned, size_t num) {
    HWY_DASSERT(num >= (hn::Lanes(df32) * 2) &&
                (num % (hn::Lanes(df32) * 2)) == 0);
    HWY_DASSERT((in_ofs % (hn::Lanes(df32) * 2)) == 0);
    HWY_DASSERT(hn::IsAligned(df32, vec_aligned));

    const hn::Repartition<hwy::bfloat16_t, DF> dbf16;
    using VF32 = decltype(Zero(df32));
    const size_t N = Lanes(dbf16);

    VF32 sum0 = Zero(df32);
    VF32 sum1 = Zero(df32);
    VF32 sum2 = Zero(df32);
    VF32 sum3 = Zero(df32);

    for (size_t i = 0; i < num; /* i += 2 * N */) {
      const auto interleaved0 = hn::LoadU(dbf16, in + in_ofs + i);
      const VF32 ae0 = Load(df32, vec_aligned + i);
      const VF32 ao0 = Load(df32, vec_aligned + i + (N / 2));
      sum0 = hn::MulAdd(ae0, hn::PromoteEvenTo(df32, interleaved0), sum0);
      sum1 = hn::MulAdd(ao0, hn::PromoteOddTo(df32, interleaved0), sum1);
      i += N;

      const auto interleaved1 = hn::LoadU(dbf16, in + in_ofs + i);
      const VF32 ae1 = Load(df32, vec_aligned + i);
      const VF32 ao1 = Load(df32, vec_aligned + i + (N / 2));
      sum2 = hn::MulAdd(ae1, hn::PromoteEvenTo(df32, interleaved1), sum2);
      sum3 = hn::MulAdd(ao1, hn::PromoteOddTo(df32, interleaved1), sum3);
      i += N;
    }

    sum0 = hn::Add(sum0, sum1);
    sum2 = hn::Add(sum2, sum3);
    sum0 = hn::Add(sum0, sum2);
    return hn::ReduceSum(df32, sum0);
  }
};

// Switching floating point: 8-bit, 2..3 mantissa bits.
template <>
struct CompressTraits<SfpStream> {
  using MatT = SfpStream;
  static const char* Name() { return "sfp"; }
  static constexpr bool kSupportsEvenOdd = true;

  // Callers are responsible for scaling `in` such that its magnitudes do not
  // exceed 1.875. See CompressedArray::scale().
  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT in,
                                  size_t num, CompressPerThread& tls,
                                  size_t /*out_capacity*/,
                                  MatT* HWY_RESTRICT out, size_t out_ofs) {
    SfpCodec::Enc(df, in, num, out + out_ofs);

    if (COMPRESS_STATS) {
      const hn::Repartition<hwy::bfloat16_t, DF> dbf;
      auto distorted = hwy::AllocateAligned<hwy::bfloat16_t>(num);
      SfpCodec::Dec(dbf, out + out_ofs, num, distorted.get());
      DistortionStats stats;
      for (size_t i = 0; i < num; ++i) {
        stats.Notify(in[i], hwy::F32FromBF16(distorted[i]));
      }
      tls.stats.Notify(stats);
    }
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Decompress2(DF df, const MatT* HWY_RESTRICT in,
                                     size_t in_ofs, hn::Vec<DF>& f0,
                                     hn::Vec<DF>& f1) {
    const hn::Twice<hn::Rebind<uint8_t, DF>> d8;
    using V8 = hn::Vec<decltype(d8)>;
    const V8 packed = hn::LoadU(d8, &in->byte + in_ofs);
    SfpCodec::Dec2F(df, packed, f0, f1);
  }

  template <class D, typename OutT>
  static HWY_INLINE void Decompress(D d, size_t /*in_capacity*/,
                                    const MatT* HWY_RESTRICT in, size_t in_ofs,
                                    OutT* HWY_RESTRICT out, size_t num) {
    SfpCodec::Dec(d, in + in_ofs, num, out);
  }

  template <class DF, typename VecT, HWY_IF_F32_D(DF)>
  static HWY_INLINE float Dot(DF df, size_t /*in_capacity*/,
                              const MatT* HWY_RESTRICT in, size_t in_ofs,
                              const VecT* HWY_RESTRICT vec_aligned,
                              size_t num) {
    HWY_DASSERT(num >= hn::Lanes(df) && (num % hn::Lanes(df)) == 0);
    HWY_DASSERT((in_ofs % hn::Lanes(df)) == 0);
    HWY_DASSERT(hn::IsAligned(df, vec_aligned));

    using VF = hn::Vec<decltype(df)>;
    VF sum0 = hn::Zero(df);
    VF sum1 = hn::Zero(df);
    VF sum2 = hn::Zero(df);
    VF sum3 = hn::Zero(df);

    SfpCodec::Dot(df, in + in_ofs, num, vec_aligned, sum0, sum1, sum2, sum3);

    // Reduction tree: sum of all accumulators, then their lanes
    sum0 = hn::Add(sum0, sum1);
    sum2 = hn::Add(sum2, sum3);
    sum0 = hn::Add(sum0, sum2);
    return hn::ReduceSum(df, sum0);
  }

  // Computes the dot product of an even-odd deinterleaved, f32 or bf16
  // `vec_aligned` and a column-major matrix `in`. `vec_aligned` should be
  // aligned and alternate even-indexed `hn::Lanes(df)` elements followed by
  // odd-indexed `hn::Lanes(df)` elements.
  template <class DF, typename VecT, HWY_IF_F32_D(DF)>
  static HWY_INLINE float DotEO(const DF df, const MatT* HWY_RESTRICT in,
                                size_t in_ofs,
                                const VecT* HWY_RESTRICT vec_aligned,
                                size_t num) {
    HWY_DASSERT(num >= (hn::Lanes(df) * 2) && (num % (hn::Lanes(df) * 2)) == 0);
    HWY_DASSERT((in_ofs % (hn::Lanes(df) * 2)) == 0);
    HWY_DASSERT(hn::IsAligned(df, vec_aligned));

    using VF = hn::Vec<decltype(df)>;
    VF sum0 = hn::Zero(df);
    VF sum1 = hn::Zero(df);
    VF sum2 = hn::Zero(df);
    VF sum3 = hn::Zero(df);

    SfpCodec::DotEO(df, in + in_ofs, num, vec_aligned, sum0, sum1, sum2, sum3);

    // Reduction tree: sum of all accumulators, then their lanes
    sum0 = hn::Add(sum0, sum1);
    sum2 = hn::Add(sum2, sum3);
    sum0 = hn::Add(sum0, sum2);
    return hn::ReduceSum(df, sum0);
  }
};

// Nonuniform quantization, 4.5 bits per element, two separate streams.
template <>
struct CompressTraits<NuqStream> {
  using MatT = NuqStream;
  static const char* Name() { return "nuq"; }
  static constexpr bool kSupportsEvenOdd = false;

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* in, size_t num,
                                  CompressPerThread& tls, size_t out_capacity,
                                  MatT* out, size_t out_ofs) {
    NuqCodec::Enc(df, in, num, tls.buf, out_capacity, out, out_ofs);

    if (COMPRESS_STATS) {
      for (size_t i = 0; i < num; ++i) {
        tls.stats.NotifyIn(static_cast<int>(lroundf(in[i] * 100.0f + 500.0f)));
      }

      const hn::Repartition<hwy::bfloat16_t, DF> dbf;
      auto distorted = hwy::AllocateAligned<hwy::bfloat16_t>(num);
      NuqCodec::Dec(dbf, out_capacity, out, out_ofs, distorted.get(), num);
      DistortionStats stats;
      for (size_t i = 0; i < num; ++i) {
        stats.Notify(in[i], hwy::F32FromBF16(distorted[i]));
      }
      tls.stats.Notify(stats);
    }
  }

  template <class D, typename OutT>
  static HWY_INLINE void Decompress(D d, size_t in_capacity, const MatT* in,
                                    size_t in_ofs, OutT* out, size_t num) {
    NuqCodec::Dec(d, in_capacity, in, in_ofs, out, num);
  }

  template <class DF, typename VecT, HWY_IF_F32_D(DF)>
  static HWY_INLINE float Dot(DF df, size_t in_capacity, const MatT* in,
                              size_t in_ofs,
                              const VecT* HWY_RESTRICT vec_aligned,
                              size_t num) {
    using VF = hn::Vec<decltype(df)>;
    VF sum0 = hn::Zero(df);
    VF sum1 = hn::Zero(df);
    VF sum2 = hn::Zero(df);
    VF sum3 = hn::Zero(df);

    NuqCodec::Dot(df, in_capacity, in, in_ofs, vec_aligned, num, sum0, sum1,
                  sum2, sum3);

    // Reduction tree: sum of all accumulators, then their lanes
    sum0 = hn::Add(hn::Add(sum0, sum1), hn::Add(sum2, sum3));
    return hn::ReduceSum(df, sum0);
  }
};

// Compresses `num` inputs to `out` starting at `out_ofs`. This can be used for
// compressing sub-regions of an array.
template <typename MatT>
HWY_NOINLINE void Compress(const float* in, size_t num,
                           CompressWorkingSet& work, size_t out_capacity,
                           MatT* out, size_t out_ofs, hwy::ThreadPool& pool) {
  HWY_DASSERT(out_ofs + num <= out_capacity);
  work.tls.resize(pool.NumThreads());
  if (COMPRESS_STATS) {
    for (auto& tls : work.tls) {
      tls.stats.Reset();
    }
  }

  const double t0 = hwy::platform::Now();

  using Traits = CompressTraits<MatT>;
  constexpr size_t kBatch = 8192;
  const size_t num_batches = hwy::DivCeil(num, kBatch);
  pool.Run(0, num_batches,
           [&](const uint32_t idx_batch, size_t thread) HWY_ATTR {
             const hn::ScalableTag<float> df;

             const size_t in_ofs = idx_batch * kBatch;
             const size_t my_num =
                 idx_batch == num_batches - 1 ? (num - in_ofs) : kBatch;
             Traits::Compress(df, in + in_ofs, my_num, work.tls[thread],
                              out_capacity, out, out_ofs + in_ofs);
           });

  const double t1 = hwy::platform::Now();
  const double mb = static_cast<double>(num) * sizeof(in[0]) * 1E-6;
  const double mbps = mb / (t1 - t0);
  fprintf(stderr, "Compress %.1f MB/s\n", mbps);

  if (COMPRESS_STATS) {
    for (size_t i = 1; i < work.tls.size(); ++i) {
      work.tls[0].stats.Assimilate(work.tls[i].stats);
    }
    work.tls[0].stats.PrintAll();
  }
}

// Compresses an entire std::array into `out`, which is assumed to have exactly
// that much capacity.
template <size_t kCapacity, typename MatT>
HWY_INLINE void Compress(const std::array<float, kCapacity>& in,
                         CompressWorkingSet& work,
                         CompressedArray<MatT, kCapacity>& compressed,
                         hwy::ThreadPool& pool) {
  Compress(in.data(), kCapacity, work, kCapacity, compressed.data(), 0, pool);
}

// Decompresses `num` values from `compressed` starting at `compressed_ofs`.
template <typename ArrayT, typename OutT>
HWY_NOINLINE void Decompress(const ArrayT& compressed, size_t compressed_ofs,
                             OutT* out, size_t num) {
  HWY_DASSERT(compressed_ofs + num <= compressed.size());
  const hn::ScalableTag<OutT> d;
  using Traits = CompressTraits<typename ArrayT::value_type>;
  Traits::Decompress(d, compressed.size(), compressed.data(), compressed_ofs,
                     out, num);
}

// As above, but with threading and benchmarking.
template <typename MatT, size_t kCapacity, typename OutT>
HWY_INLINE void Decompress(const CompressedArray<MatT, kCapacity>& compressed,
                           size_t compressed_ofs, OutT* out, size_t num,
                           hwy::ThreadPool& pool) {
  HWY_DASSERT(compressed_ofs + num <= compressed.size());
  const double t0 = hwy::platform::Now();

  using Traits = CompressTraits<MatT>;
  constexpr size_t kBatch = 8192;
  const size_t num_batches = hwy::DivCeil(num, kBatch);
  pool.Run(
      0, num_batches, [&](const uint32_t idx_batch, size_t thread) HWY_ATTR {
        const hn::ScalableTag<OutT> d;

        const size_t ofs = idx_batch * kBatch;
        const size_t batch =
            idx_batch == num_batches - 1 ? (num - ofs) : kBatch;
        Traits::Decompress(d, compressed.size(), compressed.data(),
                           compressed_ofs + ofs, out + ofs, batch);
      });

  const double t1 = hwy::platform::Now();
  const double mb = num * sizeof(MatT) * 1E-6;
  const double mbps = mb / (t1 - t0);
  fprintf(stderr, "Decompress %.1f MB/s\n", mbps);
}

// Returns dot product with `vec_aligned` of length `num`.
template <bool kVecEO, class DF, size_t kCapacity, typename VecT>
HWY_INLINE float Dot(DF df, const std::array<float, kCapacity>& w, size_t ofs,
                     const VecT* x, size_t num) {
  HWY_DASSERT(ofs + num <= kCapacity);
  HWY_DASSERT(hn::IsAligned(df, x));
  using Traits = CompressTraits<float>;
  return Traits::Dot(df, w.size(), w.data(), ofs, x, num);
}

// Returns dot product with `vec_aligned` of length `num`.
template <bool kVecEO, class DF, typename MatT, size_t kCapacity, typename VecT>
HWY_INLINE float Dot(DF df, const CompressedArray<MatT, kCapacity>& compressed,
                     size_t compressed_ofs, const VecT* vec_aligned,
                     size_t num) {
  HWY_DASSERT(compressed_ofs + num <= compressed.size());
  HWY_DASSERT(hn::IsAligned(df, vec_aligned));
  using Traits = CompressTraits<MatT>;
  float dot_result;
  if constexpr (kVecEO) {
    dot_result = Traits::DotEO(df, compressed.data(), compressed_ofs,
                               vec_aligned, num);
  } else {
    dot_result = Traits::Dot(df, compressed.size(), compressed.data(),
                             compressed_ofs, vec_aligned, num);
  }
  return compressed.scale() * dot_result;
}

// Functor called for each tensor, which compresses and stores them along with
// their scaling factors to BlobStore.
class Compressor {
 public:
  explicit Compressor(hwy::ThreadPool& pool) : pool_(pool) {}

  template <typename MatT, size_t kCapacity>
  void operator()(const char* name, const float* weights,
                  CompressedArray<MatT, kCapacity>& compressed) {
    Insert(name, weights, kCapacity, work_, compressed.CompressedSize(),
           compressed.data(), 0, pool_);
  }

  template <typename MatT>
  void Insert(const char* name, const float* weights, size_t weights_count,
              CompressWorkingSet& work, size_t out_capacity, MatT* out,
              size_t out_ofs, hwy::ThreadPool& pool) {
    fprintf(stderr, "Regenerating %s (%zuM), please wait\n", name,
            weights_count / (1000 * 1000));
    Compress(weights, weights_count, work_, weights_count, out, 0, pool_);
    writer_.Add(CacheKey<MatT>(name), out, out_capacity);
  }

  void AddScales(const float* scales, size_t len) {
    if (len) {
      writer_.Add(CacheKey<float>("scales"), scales, len * sizeof(scales[0]));
    }
  }

  void WriteAll(hwy::ThreadPool& pool, const Path& blob_filename) {
    const BlobError err = writer_.WriteAll(pool, blob_filename);
    if (err != 0) {
      fprintf(stderr, "Failed to write blobs to %s (error %d)\n",
              blob_filename.path.c_str(), err);
    }
  }

 private:
  CompressWorkingSet work_;
  hwy::ThreadPool& pool_;
  BlobWriter writer_;
};

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#endif  // NOLINT
