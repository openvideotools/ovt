// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/virtual_file/segment_cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::vfile {
namespace {

namespace fs = std::filesystem;

constexpr std::array<uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

void compress(std::array<uint32_t, 8>& h, std::span<const uint8_t, 64> block) {
  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    const auto word = block.subspan(4 * i, 4);
    w[i] = (uint32_t{word[0]} << 24) | (uint32_t{word[1]} << 16) | (uint32_t{word[2]} << 8) | uint32_t{word[3]};
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0];
  uint32_t b = h[1];
  uint32_t c = h[2];
  uint32_t d = h[3];
  uint32_t e = h[4];
  uint32_t f = h[5];
  uint32_t g = h[6];
  uint32_t hh = h[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

std::atomic<uint64_t> g_temp_counter{0};

}  // namespace

std::string sha256_hex(std::string_view data) {
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const std::span<const uint8_t> in(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  const size_t n = in.size();
  size_t pos = 0;
  for (; n - pos >= 64; pos += 64) {
    compress(h, in.subspan(pos, 64).first<64>());
  }
  std::array<uint8_t, 128> tail{};
  const size_t rest = n - pos;
  if (rest > 0) {
    std::memcpy(tail.data(), in.subspan(pos).data(), rest);
  }
  tail[rest] = 0x80;
  const size_t tail_len = rest < 56 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(n) * 8;
  for (size_t i = 0; i < 8; ++i) {
    tail[tail_len - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
  }
  for (size_t off = 0; off < tail_len; off += 64) {
    compress(h, std::span<const uint8_t>(tail).subspan(off, 64).first<64>());
  }
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const uint32_t v : h) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(v >> shift) & 0xF]);
    }
  }
  return out;
}

SegmentCache::SegmentCache(fs::path dir, uint64_t cap_bytes) : dir_(std::move(dir)), cap_(cap_bytes) {}

std::string SegmentCache::key(std::string_view url, const mpd::ByteRange& range) {
  if (range.offset == 0 && range.length == 0) {
    return sha256_hex(url);
  }
  std::string s(url);
  s += '\n';
  s += std::to_string(range.offset);
  s += '-';
  s += std::to_string(range.length);
  return sha256_hex(s);
}

std::optional<fs::path> SegmentCache::lookup(const std::string& key) {
  const std::scoped_lock lock(mutex_);
  std::error_code ec;
  fs::path p = dir_ / key;
  if (!fs::is_regular_file(p, ec)) {
    return std::nullopt;
  }
  fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // recency; failure only costs eviction accuracy
  return p;
}

fs::path SegmentCache::store(const std::string& key, std::span<const uint8_t> bytes) {
  const std::scoped_lock lock(mutex_);
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const fs::path final_path = dir_ / key;
  const fs::path temp = dir_ / (key + ".tmp" + std::to_string(g_temp_counter.fetch_add(1)));
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
      fs::remove(temp, ec);
      throw io::IoError(OVT_E_IO, 0, "cannot write segment cache file " + temp.string());
    }
  }
  fs::rename(temp, final_path, ec);
  if (ec) {
    fs::remove(temp, ec);
    throw io::IoError(OVT_E_IO, 0, "cannot move segment cache file to " + final_path.string());
  }
  evict_locked(final_path);
  return final_path;
}

uint64_t SegmentCache::total_bytes() const {
  uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (it->is_regular_file(ec2)) {
      total += it->file_size(ec2);
    }
  }
  return total;
}

void SegmentCache::evict_locked(const fs::path& keep) {
  struct Entry {
    fs::path path;
    uint64_t size;
    fs::file_time_type time;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (!it->is_regular_file(ec2)) {
      continue;
    }
    const uint64_t size = it->file_size(ec2);
    entries.push_back({it->path(), size, it->last_write_time(ec2)});
    total += size;
  }
  if (total <= cap_) {
    return;
  }
  std::ranges::sort(entries, [](const Entry& a, const Entry& b) { return a.time < b.time; });
  for (const Entry& e : entries) {
    if (total <= cap_) {
      break;
    }
    if (e.path == keep) {
      continue;
    }
    std::error_code rm;
    if (fs::remove(e.path, rm)) {
      total -= e.size;
    }
  }
}

}  // namespace ovt::container::vfile
