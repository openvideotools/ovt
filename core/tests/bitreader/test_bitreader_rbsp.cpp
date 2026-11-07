// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of rbsp_from_nal and the raw <-> RBSP offset mapping, rows `rbsp_golden` and
// `rbsp_mapping_bijection` of chapter 7 §7.2.7 plus the random round-trips of WI-1.104.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "ovt/cxx/rbsp.hpp"

namespace {

using Bytes = std::vector<uint8_t>;
using Positions = std::vector<uint32_t>;

struct Encoded {
  Bytes raw;
  Positions epb_positions;
};

// Test-only reference EPB inserter (H.264 §7.4.1 / H.265 §7.4.2): inside the NAL, `00 00` followed by a
// byte in 00..03 gets an emulation_prevention_three_byte; an RBSP that ends in `00 00` (cabac_zero_word
// padding) gets a final 0x03. Records the raw position of every inserted byte.
Encoded insert_epbs(const Bytes& rbsp) {
  Encoded e;
  unsigned zeros = 0;
  for (const uint8_t b : rbsp) {
    if (zeros >= 2 && b <= 0x03) {
      e.epb_positions.push_back(static_cast<uint32_t>(e.raw.size()));
      e.raw.push_back(0x03);
      zeros = 0;
    }
    e.raw.push_back(b);
    zeros = (b == 0x00) ? zeros + 1 : 0;
  }
  if (zeros >= 2) {
    e.epb_positions.push_back(static_cast<uint32_t>(e.raw.size()));
    e.raw.push_back(0x03);
  }
  return e;
}

// The §7.2.5 prose algorithm, linear: walk the ascending positions, each one at or before the frontier
// shifts it by one.
uint64_t naive_rbsp_bit_to_raw_bit(uint64_t rbsp_bit, const Positions& epb) {
  uint64_t raw_byte = rbsp_bit / 8;
  for (const uint32_t p : epb) {
    if (p <= raw_byte) {
      ++raw_byte;
    } else {
      break;
    }
  }
  return (raw_byte * 8) + (rbsp_bit % 8);
}

// A random RBSP biased towards 0x00..0x03 so that EPBs are frequent. Byte 0 is a non-zero header byte.
Bytes random_rbsp(std::mt19937_64& rng) {
  std::uniform_int_distribution<size_t> len(1, 300);
  std::uniform_int_distribution<int> pick(0, 9);
  std::uniform_int_distribution<int> any(0, 255);
  Bytes out(len(rng));
  for (auto& b : out) {
    const int k = pick(rng);
    if (k < 5) {
      b = 0x00;  // half the bytes are zero
    } else if (k < 8) {
      b = static_cast<uint8_t>(k - 4);  // 0x01..0x03: the bytes that need an EPB after 00 00
    } else {
      b = static_cast<uint8_t>(any(rng));
    }
  }
  out[0] = 0x67;
  return out;
}

// Every RBSP byte b maps to a raw byte that is not an EPB and maps back to b; every raw byte that is not
// an EPB is hit exactly once (bijection between RBSP bytes and non-EPB raw bytes).
void check_bijection(const Bytes& raw, const Bytes& rbsp, const Positions& epb) {
  std::vector<int> hits(raw.size(), 0);
  for (uint64_t b = 0; b < rbsp.size(); ++b) {
    for (const uint64_t bit : {uint64_t{0}, uint64_t{7}}) {
      const uint64_t raw_bit = ovt::rbsp_bit_to_raw_bit((b * 8) + bit, epb);
      REQUIRE(raw_bit == naive_rbsp_bit_to_raw_bit((b * 8) + bit, epb));
      REQUIRE(raw_bit % 8 == bit);
    }
    const uint64_t raw_byte = ovt::rbsp_bit_to_raw_bit(b * 8, epb) / 8;
    REQUIRE(raw_byte < raw.size());
    REQUIRE(raw[raw_byte] == rbsp[b]);
    REQUIRE(ovt::raw_byte_to_rbsp_byte(raw_byte, epb) == std::pair<uint64_t, bool>{b, false});
    ++hits[raw_byte];
  }
  size_t epb_index = 0;
  for (uint64_t r = 0; r < raw.size(); ++r) {
    const bool is_epb = epb_index < epb.size() && epb[epb_index] == r;
    REQUIRE(hits[r] == (is_epb ? 0 : 1));
    const auto [mapped, flag] = ovt::raw_byte_to_rbsp_byte(r, epb);
    REQUIRE(flag == is_epb);
    REQUIRE(mapped == r - epb_index);  // for an EPB: the position the next RBSP byte has
    if (is_epb) {
      ++epb_index;
    }
  }
}

}  // namespace

TEST_CASE("rbsp_golden", "[bitreader][rbsp][fast]") {
  SECTION("chapter 7 §7.2.6 worked example") {
    const Bytes raw{0x67, 0x42, 0x00, 0x1E, 0x00, 0x00, 0x03, 0x01, 0xE9};
    const ovt::Rbsp r = ovt::rbsp_from_nal(raw);
    CHECK(r.rbsp == Bytes{0x67, 0x42, 0x00, 0x1E, 0x00, 0x00, 0x01, 0xE9});
    CHECK(r.epb_positions == Positions{6});
    CHECK(ovt::rbsp_bit_to_raw_bit(51, r.epb_positions) == 59);  // 6*8+3 → 7*8+3
    CHECK(ovt::raw_byte_to_rbsp_byte(6, r.epb_positions) == std::pair<uint64_t, bool>{6, true});
    CHECK(ovt::raw_byte_to_rbsp_byte(7, r.epb_positions) == std::pair<uint64_t, bool>{6, false});
    check_bijection(raw, r.rbsp, r.epb_positions);
  }
  SECTION("20 generated NALs round-trip through the reference inserter") {
    std::mt19937_64 rng(0x0E7B'1104);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds keep the tests deterministic
    for (int n = 0; n < 20; ++n) {
      const Bytes payload = random_rbsp(rng);
      const Encoded e = insert_epbs(payload);
      const ovt::Rbsp r = ovt::rbsp_from_nal(e.raw);
      REQUIRE(r.rbsp == payload);
      REQUIRE(r.epb_positions == e.epb_positions);
    }
  }
}

TEST_CASE("rbsp_epb_edge_cases", "[bitreader][rbsp][fast]") {
  SECTION("00 00 03 at the very end (cabac_zero_word padding) is still an EPB") {
    const ovt::Rbsp r = ovt::rbsp_from_nal(Bytes{0x65, 0x80, 0x00, 0x00, 0x03});
    CHECK(r.rbsp == Bytes{0x65, 0x80, 0x00, 0x00});
    CHECK(r.epb_positions == Positions{4});
  }
  SECTION("an EPB followed by a byte > 0x03 is still removed") {
    const ovt::Rbsp r = ovt::rbsp_from_nal(Bytes{0x41, 0x00, 0x00, 0x03, 0xFF});
    CHECK(r.rbsp == Bytes{0x41, 0x00, 0x00, 0xFF});
    CHECK(r.epb_positions == Positions{3});
  }
  SECTION("consecutive EPBs: the zero counter restarts after each removal") {
    const Bytes raw{0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x03};
    const ovt::Rbsp r = ovt::rbsp_from_nal(raw);
    CHECK(r.rbsp == Bytes{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03});
    CHECK(r.epb_positions == Positions{3, 6});
    check_bijection(raw, r.rbsp, r.epb_positions);
  }
  SECTION("00 03 (one zero) and 00 00 00 03 (three zeros)") {
    CHECK(ovt::rbsp_from_nal(Bytes{0x01, 0x00, 0x03}).epb_positions.empty());
    const ovt::Rbsp r = ovt::rbsp_from_nal(Bytes{0x01, 0x00, 0x00, 0x00, 0x03, 0x01});
    CHECK(r.rbsp == Bytes{0x01, 0x00, 0x00, 0x00, 0x01});
    CHECK(r.epb_positions == Positions{4});
  }
  SECTION("a 03 in the header position cannot match") {
    const ovt::Rbsp r = ovt::rbsp_from_nal(Bytes{0x03, 0x03});
    CHECK(r.rbsp == Bytes{0x03, 0x03});
    CHECK(r.epb_positions.empty());
  }
  SECTION("empty NAL") {
    const ovt::Rbsp r = ovt::rbsp_from_nal(std::span<const uint8_t>{});
    CHECK(r.rbsp.empty());
    CHECK(r.epb_positions.empty());
  }
  SECTION("no EPBs (AV1 units): both mappings are the identity") {
    const Positions none;
    for (const uint64_t v : {uint64_t{0}, uint64_t{13}, uint64_t{1} << 40}) {
      CHECK(ovt::rbsp_bit_to_raw_bit(v, none) == v);
      CHECK(ovt::raw_byte_to_rbsp_byte(v, none) == std::pair<uint64_t, bool>{v, false});
    }
  }
}

TEST_CASE("rbsp_mapping_bijection", "[bitreader][rbsp][fast]") {
  // §7.2.7: for every RBSP byte, raw_byte_to_rbsp_byte(rbsp_bit_to_raw_bit(b*8)/8) == {b,false}.
  std::mt19937_64 rng(0xB17EC7);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds keep the tests deterministic
  for (int n = 0; n < 50; ++n) {
    const Encoded e = insert_epbs(random_rbsp(rng));
    const ovt::Rbsp r = ovt::rbsp_from_nal(e.raw);
    check_bijection(e.raw, r.rbsp, r.epb_positions);
  }
}

TEST_CASE("rbsp_random_round_trip", "[bitreader][rbsp][fast]") {
  // WI-1.104 acceptance test 3: 1 000 random NALs with inserted EPBs strip back to the pre-insertion
  // payload, and every RBSP byte maps back bijectively.
  std::mt19937_64 rng(0x1104'0003);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds keep the tests deterministic
  size_t total_epbs = 0;
  for (int n = 0; n < 1000; ++n) {
    const Bytes payload = random_rbsp(rng);
    const Encoded e = insert_epbs(payload);
    const ovt::Rbsp r = ovt::rbsp_from_nal(e.raw);
    REQUIRE(r.rbsp == payload);
    REQUIRE(r.epb_positions == e.epb_positions);
    check_bijection(e.raw, r.rbsp, r.epb_positions);
    total_epbs += r.epb_positions.size();
  }
  CHECK(total_epbs > 1000);  // the generator really exercises EPBs
}
