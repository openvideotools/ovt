// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// StreamIndex.source_hash (chapter 6 §6.2.8) and the SHA-256 it needs (FIPS 180-4 §6.2, written from the standard).

#include "index/source_hash.h"

#include <algorithm>
#include <bit>
#include <cstddef>

namespace ovt::index {

namespace {

constexpr std::array<uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint64_t kHashedPrefix = uint64_t{1} << 20U;  // 1 MiB

std::array<uint8_t, 8> le64(uint64_t v) {
  std::array<uint8_t, 8> out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out.at(i) = static_cast<uint8_t>(v >> (8 * i));
  }
  return out;
}

}  // namespace

void Sha256::compress(std::span<const uint8_t, 64> block) {
  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    w.at(i) = (uint32_t{block[4 * i]} << 24U) | (uint32_t{block[(4 * i) + 1]} << 16U) |
              (uint32_t{block[(4 * i) + 2]} << 8U) | uint32_t{block[(4 * i) + 3]};
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = std::rotr(w.at(i - 15), 7) ^ std::rotr(w.at(i - 15), 18) ^ (w.at(i - 15) >> 3U);
    const uint32_t s1 = std::rotr(w.at(i - 2), 17) ^ std::rotr(w.at(i - 2), 19) ^ (w.at(i - 2) >> 10U);
    w.at(i) = w.at(i - 16) + s0 + w.at(i - 7) + s1;
  }
  std::array<uint32_t, 8> v = state_;  // a, b, c, d, e, f, g, h
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
    const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const uint32_t t1 = v[7] + s1 + ch + kRound.at(i) + w.at(i);
    const uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
    const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const uint32_t t2 = s0 + maj;
    std::rotate(v.begin(), v.end() - 1, v.end());  // h = g, g = f, …, b = a
    v[4] += t1;                                    // e = d + T1
    v[0] = t1 + t2;                                // a = T1 + T2
  }
  for (size_t i = 0; i < state_.size(); ++i) {
    state_.at(i) += v.at(i);
  }
}

void Sha256::update(std::span<const uint8_t> data) {
  bytes_ += data.size();
  while (!data.empty()) {
    const size_t n = std::min(data.size(), block_.size() - fill_);
    std::copy_n(data.begin(), n, block_.begin() + static_cast<std::ptrdiff_t>(fill_));
    fill_ += n;
    data = data.subspan(n);
    if (fill_ == block_.size()) {
      compress(block_);
      fill_ = 0;
    }
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  const uint64_t bits = bytes_ * 8;
  const std::array<uint8_t, 1> one = {0x80};
  update(one);
  const std::array<uint8_t, 64> zeros{};
  update(std::span(zeros).first((block_.size() + 56 - fill_) % block_.size()));
  std::array<uint8_t, 8> length{};
  for (size_t i = 0; i < length.size(); ++i) {
    length.at(i) = static_cast<uint8_t>(bits >> (8 * (7 - i)));
  }
  update(length);
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out.at(i) = static_cast<uint8_t>(state_.at(i / 4) >> (8 * (3 - (i % 4))));
  }
  return out;
}

std::array<uint8_t, 32> source_hash(const io::RandomAccessFile& file) {
  const io::FileInfo& info = file.info();
  Sha256 sha;
  const auto prefix = file.view(0, static_cast<size_t>(std::min(info.size, kHashedPrefix)));
  sha.update(prefix.bytes);
  sha.update(le64(info.size));
  sha.update(le64(static_cast<uint64_t>(info.mtime_ns)));
  return sha.finish();
}

}  // namespace ovt::index
