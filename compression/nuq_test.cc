// Copyright 2023 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// SFP uses ConcatEven/Odd which are not supported. Use HWY_EMU128 instead.
#ifndef HWY_DISABLED_TARGETS
#define HWY_DISABLED_TARGETS HWY_SCALAR
#endif

#include "compression/nuq.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>  // std::shuffle
#include <random>

#include "compression/test_util.h"
#include "hwy/aligned_allocator.h"
#include "hwy/base.h"
#include "hwy/tests/test_util.h"
#include "hwy/timer.h"

// clang-format off
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "compression/nuq_test.cc"  // NOLINT
// clang-format on
#include "hwy/foreach_target.h"  // IWYU pragma: keep
// Other headers that include Highway must come after foreach_target.h
#include "compression/nuq-inl.h"
#include "hwy/highway.h"
#include "hwy/tests/hwy_gtest.h"
#include "hwy/tests/test_util-inl.h"

HWY_BEFORE_NAMESPACE();
namespace gcpp {
namespace HWY_NAMESPACE {

// All-equal inputs: only one cluster
struct TestFlat {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);
    for (size_t i = 0; i < kGroupSize; ++i) {
      in[i] = 0.5f;
    }
    ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters =
        NuqClustering::ClusterExactL2(df, in.get(), buf, centers, indices);
    HWY_ASSERT(unused_clusters == kClusters - 1);

    for (size_t i = 0; i < unused_clusters; ++i) {
      HWY_ASSERT(centers[i] == 0.0f);
    }
    HWY_ASSERT(centers[unused_clusters] == 0.5f);
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] == unused_clusters);
    }
  }
};

void TestAllFlat() { hn::ForGEVectors<64, TestFlat>()(float()); }

// Generate shuffled plateaus, one per cluster
struct TestPlateaus {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    for (size_t i = 0; i < kGroupSize; ++i) {
      const size_t idx_cluster = i / (kGroupSize / kClusters);
      HWY_ASSERT(idx_cluster < kClusters);
      in[i] = (1.0f * idx_cluster / kClusters) - 0.5f;
      HWY_ASSERT(-0.5f <= in[i] && in[i] < 0.5f);
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(in.get(), in.get() + kGroupSize, rng);

    ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters =
        NuqClustering::ClusterExactL2(df, in.get(), buf, centers, indices);
    HWY_ASSERT(unused_clusters == 0);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }
    // Zero error.
    HWY_ASSERT_EQ(kGroupSize, stats.NumExact());
    HWY_ASSERT_EQ(0, stats.NumSignFlip());
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT_EQ(0.0, stats.SumL1());
    HWY_ASSERT_EQ(0.0f, stats.GeomeanValueDivL1());
    HWY_ASSERT_EQ(0.0f, stats.WeightedAverageL1());
    // Input was symmetric and zero-mean.
    HWY_ASSERT(gcpp::IsInside(-0.05, 0.05, stats.Original().Mean()));
    HWY_ASSERT(gcpp::IsNear(0.0, stats.Original().Skewness()));
  }
};

void TestAllPlateaus() { hn::ForGEVectors<64, TestPlateaus>()(float()); }

struct TestRamp {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    for (size_t i = 0; i < kGroupSize; ++i) {
      in[i] = (1.0f * i / kGroupSize) - 0.45f;  // slightly asymmetric
      HWY_ASSERT(-0.45f <= in[i] && in[i] < 0.55f);
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(in.get(), in.get() + kGroupSize, rng);

    ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters =
        NuqClustering::ClusterExactL2(df, in.get(), buf, centers, indices);
    HWY_ASSERT(unused_clusters == 0);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }

    // Low error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < 10);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT_EQ(kGroupSize / kClusters / 4.0, stats.SumL1());
    HWY_ASSERT(gcpp::IsInside(17.0, 18.0, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.005, 0.010, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.04f);
    // Input was symmetric about 0.05.
    HWY_ASSERT(gcpp::IsNear(0.05, stats.Original().Mean(), 0.01));
    HWY_ASSERT(gcpp::IsNear(0.0, stats.Original().Skewness(), 1E-4));
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestAllRamp() { hn::ForGEVectors<64, TestRamp>()(float()); }

struct TestNormal {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    hwy::RandomState rng;
    hwy::Stats in_stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      const double r = RandomGaussian(rng);
      in_stats.Notify(r);
      in[i] = hwy::ConvertScalarTo<T>(r);
    }
    VerifyGaussian(in_stats);

    ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    double elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < 100; ++rep) {
      const double t0 = hwy::platform::Now();
      const size_t unused_clusters =
          NuqClustering::ClusterExactL2(df, in.get(), buf, centers, indices);
      HWY_ASSERT(unused_clusters == 0);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Enc %.2f MB/s\n", Lanes(df) * 4,
            kGroupSize * sizeof(float) * 1E-6 / elapsed);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }

    // Moderate error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < kGroupSize / kClusters);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT(gcpp::IsInside(5.0, 6.0, stats.SumL1()));
    HWY_ASSERT(gcpp::IsInside(12.7, 12.8, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.036, 0.037, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.10f);
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestAllNormal() { hn::ForGEVectors<64, TestNormal>()(float()); }

// Can encode and decode sub-regions.
struct TestOffset {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<float, D> df;
    const size_t total = 10 * kGroupSize;
    const size_t kMidLen = 2 * kGroupSize;  // length of middle piece

    auto in = hwy::AllocateAligned<float>(total);  // Enc() requires f32
    auto dec1 = hwy::AllocateAligned<T>(total);
    auto dec2 = hwy::AllocateAligned<T>(kMidLen);
    auto nuq = hwy::AllocateAligned<NuqStream>(NuqStream::PackedEnd(total));
    HWY_ASSERT(in && dec1 && dec2 && nuq);

    hwy::RandomState rng;
    for (size_t i = 0; i < total; ++i) {
      in[i] = static_cast<float>(RandomGaussian(rng));
    }

    // Encode + decode everything
    ClusterBuf buf;
    (void)NuqCodec::Enc(df, in.get(), total, buf, total, nuq.get(), 0);
    NuqCodec::Dec(d, total, nuq.get(), 0, dec1.get(), total);

    // Overwrite middle with first inputs
    const size_t offset = 5 * kGroupSize;
    (void)NuqCodec::Enc(df, in.get(), kMidLen, buf, total, nuq.get(), offset);

    // Decoded middle now matches previously decoded first
    NuqCodec::Dec(d, total, nuq.get(), offset, dec2.get(), kMidLen);
    for (size_t i = 0; i < kMidLen; ++i) {
      HWY_ASSERT(dec1[i] == dec2[i]);
    }
  }
};

void TestAllOffsetF32() {
  const hn::ForGEVectors<128, TestOffset> test;
  test(float());
}

void TestAllOffsetBF16() {
  const hn::ForGEVectors<128, TestOffset> test;
  test(hwy::bfloat16_t());
}

struct TestStream {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<float, D> df;
    const size_t num = 4 * kGroupSize;
    auto in = hwy::AllocateAligned<float>(num);  // Enc() requires f32
    auto out = hwy::AllocateAligned<T>(num);
    auto nuq = hwy::AllocateAligned<NuqStream>(NuqStream::PackedEnd(num));
    HWY_ASSERT(in && out && nuq);

    hwy::RandomState rng;
    hwy::Stats in_stats;
    for (size_t i = 0; i < num; ++i) {
      in[i] = static_cast<float>(RandomGaussian(rng));
      in_stats.Notify(in[i]);
    }
    VerifyGaussian(in_stats);

    ClusterBuf buf;
    double elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < 100; ++rep) {
      const double t0 = hwy::platform::Now();
      const size_t unused_clusters =
          NuqCodec::Enc(df, in.get(), num, buf, num, nuq.get(), 0);
      HWY_ASSERT(unused_clusters == 0);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Enc %.2f MB/s\n", Lanes(d) * sizeof(T),
            num * sizeof(float) * 1E-6 / elapsed);

    elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < 100; ++rep) {
      const double t0 = hwy::platform::Now();
      NuqCodec::Dec(d, num, nuq.get(), 0, out.get(), num);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Dec %.2f MB/s\n", Lanes(d) * sizeof(T),
            num * sizeof(T) * 1E-6 / elapsed);

    DistortionStats stats;
    for (size_t i = 0; i < num; ++i) {
      stats.Notify(in[i], hwy::ConvertScalarTo<float>(out[i]));
    }

    // Moderate error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < num / kClusters);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT(gcpp::IsInside(23.0, 24.0, stats.SumL1()));
    HWY_ASSERT(gcpp::IsInside(13.0, 13.3, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.034, 0.035, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.11f);
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestAllStreamF32() {
  const hn::ForGEVectors<128, TestStream> test;
  test(float());
}

void TestAllStreamBF16() {
  const hn::ForGEVectors<128, TestStream> test;
  test(hwy::bfloat16_t());
}

struct TestDot {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<float, D> df;
    const size_t num = 4 * kGroupSize;
    auto in = hwy::AllocateAligned<float>(num);
    auto dec = hwy::AllocateAligned<float>(num);
    auto vec = hwy::AllocateAligned<T>(num);
    auto nuq = hwy::AllocateAligned<NuqStream>(NuqStream::PackedEnd(num));
    HWY_ASSERT(in && dec && vec && nuq);

    // Generate inputs and verify their distribution.
    hwy::RandomState rng;
    hwy::Stats in_stats;
    for (size_t i = 0; i < num; ++i) {
      in[i] = static_cast<float>(RandomGaussian(rng));
      in_stats.Notify(in[i]);
    }
    for (size_t i = 0; i < num; ++i) {
      const float r = static_cast<float>(RandomGaussian(rng));
      in_stats.Notify(r);
      vec[i] = hwy::ConvertScalarTo<T>(r);
    }
    VerifyGaussian(in_stats);

    ClusterBuf buf;
    const size_t unused_clusters =
        NuqCodec::Enc(df, in.get(), num, buf, num, nuq.get(), 0);
    HWY_ASSERT(unused_clusters == 0);

    // Compute dot product without decompression.
    float actual = 0.0f;
    double elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < 20; ++rep) {
      hn::Vec<decltype(df)> sum0 = hn::Zero(df);
      hn::Vec<decltype(df)> sum1 = hn::Zero(df);
      hn::Vec<decltype(df)> sum2 = hn::Zero(df);
      hn::Vec<decltype(df)> sum3 = hn::Zero(df);
      const double t0 = hwy::platform::Now();
      NuqCodec::Dot(df, num, nuq.get(), 0, vec.get(), num, sum0, sum1, sum2,
                    sum3);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
      sum0 = hn::Add(hn::Add(sum0, sum1), hn::Add(sum2, sum3));
      actual = hn::ReduceSum(df, sum0);
    }

    NuqCodec::Dec(df, num, nuq.get(), 0, dec.get(), num);
    fprintf(stderr, "Vec %zu Dec %.2f MB/s\n", Lanes(d) * sizeof(T),
            num * sizeof(in[0]) * 1E-6 / elapsed);

    // Exact and decompressed dot products for comparison.
    float exact = 0.0f;     // using original input
    float expected = 0.0f;  // using decoded NUQ
    DistortionStats dec_stats;
    hwy::Stats ratios;
    for (size_t i = 0; i < num; ++i) {
      dec_stats.Notify(in[i], dec[i]);
      const float v1 = hwy::ConvertScalarTo<float>(vec[i]);
      exact += in[i] * v1;
      expected += dec[i] * v1;
      if (expected != 0.0f) {
        ratios.Notify(exact / expected);
      }
    }
    const bool isBF = sizeof(T) == 2;
    const double dec_snr = dec_stats.GeomeanValueDivL1();
    const double dec_wl1 = dec_stats.WeightedAverageL1();
    const double dot_snr = 1.0 / hwy::ScalarAbs(1.0 - ratios.GeometricMean());
    // exact and actual fluctuate due to the combination of NUQ imprecision,
    // and whether vec[i] is negative or positive, so this is quite loose.
    const float final_ratio = HWY_MIN(exact / actual, actual / exact);
    if (HWY_ONCE) {
      fprintf(stderr, "ratios %s\n", ratios.ToString().c_str());
      fprintf(stderr,
              "exact %.3f e2 %.4f actual %.4f final_ratio %.3f dec_snr %.2f "
              "dot_snr %.2f dec_wl1 %.4f\n",
              exact, expected, actual, final_ratio, dec_snr, dot_snr, dec_wl1);
    }
    // Final values are not too far apart.
    HWY_ASSERT(gcpp::IsInside(0.88f, 1.0f, final_ratio));
    // Decompressed and uncompressed dot should match exactly.
    HWY_ASSERT(gcpp::IsNear(expected, actual, 1E-4f));
    // Geomean of ratios for each i should be very close to one.
    HWY_ASSERT(dot_snr >= (isBF ? 17.7 : 14.3));

    // dec[] is close to in[], but we already check that in TestStream with the
    // same input distribution.
    HWY_ASSERT(gcpp::IsNear(13.1, dec_snr, 0.1));
    HWY_ASSERT(gcpp::IsNear(0.034, dec_wl1, 0.001));
    HWY_ASSERT(gcpp::IsNear(23.5, dec_stats.SumL1(), 0.1));
    HWY_ASSERT(dec_stats.NumSignFlip() < num / kClusters);
    HWY_ASSERT_EQ(0, dec_stats.NumExact());
    HWY_ASSERT_EQ(0, dec_stats.NumRoundedToZero());
    HWY_ASSERT_EQ(0.0, dec_stats.SumL1Rounded());
    // Absolute decode errors are in [0, 0.11], and somewhat right-tailed.
    HWY_ASSERT(gcpp::IsInside(0.0f, 2E-5f, dec_stats.L1().Min()));
    HWY_ASSERT(gcpp::IsInside(0.09f, 0.11f, dec_stats.L1().Max()));
    HWY_ASSERT(gcpp::IsInside(0.02, 0.03, dec_stats.L1().Mean()));
    HWY_ASSERT(gcpp::IsInside(1.0, 1.1, dec_stats.L1().Skewness()));
    HWY_ASSERT(gcpp::IsInside(4.0, 5.0, dec_stats.L1().Kurtosis()));
    static_assert(kGroupSize == 256, "Update expected*");
  }
};

void TestAllDotF32() {
  const hn::ForGEVectors<128, TestDot> test;
  test(float());
}
void TestAllDotBF16() {
  const hn::ForGEVectors<128, TestDot> test;
  test(hwy::bfloat16_t());
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace gcpp {
HWY_BEFORE_TEST(NuqTest);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllFlat);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllPlateaus);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllRamp);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllNormal);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllOffsetF32);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllOffsetBF16);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllStreamF32);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllStreamBF16);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllDotF32);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllDotBF16);
#ifdef HWY_AFTER_TEST
HWY_AFTER_TEST();
#endif
}  // namespace gcpp

#endif
