// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_smoke: BitReader throughput (chapter 14 §14.9, WI-0.560). It has no budget; it exists to prove the whole
// harness (build, tools/perf_run.py, ovt_meta, tools/perf_compare.py, baselines) on every reference machine.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt_perf.hpp"

namespace {

constexpr size_t kBufferBytes = 1U << 20;  // 1 MiB per pass
constexpr int64_t kBufferBytesSigned = 1 << 20;

// Deterministic xorshift64 stream, so every machine reads the same bits.
class Rng {
 public:
  uint64_t next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

 private:
  uint64_t state_ = 0x9E3779B97F4A7C15ULL;
};

std::vector<uint8_t> random_bytes() {
  Rng rng;
  std::vector<uint8_t> bytes(kBufferBytes);
  for (auto& b : bytes) {
    b = static_cast<uint8_t>(rng.next() >> 24U);
  }
  return bytes;
}

// ue(v) codes of values below 1000, back to back (exp-Golomb order 0, H.264 §9.1), padded with a zero byte.
std::vector<uint8_t> exp_golomb_bytes() {
  Rng rng;
  std::vector<uint8_t> bytes;
  bytes.reserve(kBufferBytes + 8);
  uint64_t acc = 0;
  unsigned used = 0;
  const auto put = [&](uint64_t value, unsigned bits) {
    for (unsigned i = bits; i-- > 0;) {
      acc = (acc << 1U) | ((value >> i) & 1U);
      if (++used == 8) {
        bytes.push_back(static_cast<uint8_t>(acc));
        acc = 0;
        used = 0;
      }
    }
  };
  while (bytes.size() < kBufferBytes) {
    const uint64_t code = (rng.next() >> 40U) % 1000U + 1U;  // codeNum + 1
    unsigned len = 0;
    while ((code >> len) > 1U) {
      ++len;
    }
    put(0, len);
    put(code, len + 1);
  }
  bytes.resize(bytes.size() + 8, 0);
  return bytes;
}

// u(n) for n cycling 1..32: the fixed-length descriptor path of every parameter set parser.
void bench_core_bitreader_u(benchmark::State& state) {
  const std::vector<uint8_t> data = random_bytes();
  int64_t processed = 0;
  for (auto _ : state) {
    processed += kBufferBytesSigned;
    ovt::BitReader br(data.data(), data.size());
    unsigned n = 1;
    uint64_t sum = 0;
    while (br.bits_left() >= 64) {
      sum += br.u(n);
      n = n % 32U + 1U;
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetBytesProcessed(processed);
}
OVT_BENCHMARK(bench_core_bitreader_u);

// ue(v): the variable-length descriptor path (leading-zero count plus a read).
void bench_core_bitreader_ue(benchmark::State& state) {
  const std::vector<uint8_t> data = exp_golomb_bytes();
  int64_t processed = 0;
  for (auto _ : state) {
    processed += kBufferBytesSigned;
    ovt::BitReader br(data.data(), data.size());
    uint64_t sum = 0;
    while (br.bits_left() >= 64) {
      sum += br.ue();
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetBytesProcessed(processed);
}
OVT_BENCHMARK(bench_core_bitreader_ue);

}  // namespace

BENCHMARK_MAIN();
