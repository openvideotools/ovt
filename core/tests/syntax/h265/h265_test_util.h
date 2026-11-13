// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared helpers of the H.265 header parser tests (WI-1.112): a bit writer for hand-assembled structures, a tree
// lookup and the NAL -> RBSP -> parser plumbing. Header-only, test code only.

#ifndef OVT_TESTS_SYNTAX_H265_TEST_UTIL_H
#define OVT_TESTS_SYNTAX_H265_TEST_UTIL_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::test::h265 {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;

// A small bit writer for hand-assembled structures.
class BitWriter {
 public:
  void u(unsigned n, uint64_t v) {
    for (unsigned i = n; i-- > 0;) {
      bits_.push_back(((v >> i) & 1U) != 0);
    }
  }
  void flag(bool v) { u(1, v ? 1 : 0); }
  void ue(uint32_t v) {
    const uint64_t code = uint64_t{v} + 1;
    unsigned len = 0;
    while ((code >> len) > 1) {
      ++len;
    }
    u(len, 0);
    u(len + 1, code);
  }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
  void trailing() {
    u(1, 1);
    while (bits_.size() % 8 != 0) {
      u(1, 0);
    }
  }
  [[nodiscard]] std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
      if (bits_[i]) {
        out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
      }
    }
    return out;
  }
  [[nodiscard]] size_t size_bits() const { return bits_.size(); }

 private:
  std::vector<bool> bits_;
};

// Writes profile_tier_level() general block for Main (1) / Main 10 (2) with the given compat flags; 88 bits.
inline void write_general_profile(BitWriter& w, unsigned profile_idc, std::initializer_list<unsigned> compat,
                                  bool one_picture_only = false) {
  w.u(2, 0);  // profile_space
  w.u(1, 0);  // tier
  w.u(5, profile_idc);
  for (unsigned j = 0; j < 32; ++j) {
    bool set = false;
    for (const unsigned c : compat) {
      set = set || c == j;
    }
    w.flag(set);
  }
  w.u(4, 0b1010);  // progressive, interlaced, non_packed, frame_only
  // Main 10 layout (profile_idc 2 or compat[2]): 7 reserved + one_picture_only + 35 reserved
  w.u(7, 0);
  w.flag(one_picture_only);
  w.u(35, 0);
  w.u(1, 0);  // inbld / reserved
}

inline const SyntaxNodeData* find_child(const SyntaxNodeData* node, std::string_view name) {
  for (const SyntaxNodeData* c = node->first_child; c != nullptr; c = c->next_sibling) {
    if (c->name == name) {
      return c;
    }
  }
  return nullptr;
}

// Strips emulation prevention, parses the NAL unit header and hands the reader to `parser`; the finished tree
// (root "nal_unit") is stored in *tree_out when given.
template <class Parser>
auto parse_nal(const std::vector<uint8_t>& nal, Parser parser, SyntaxNodeBuffer* tree_out = nullptr) {
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  const ovt::syntax::h265::NalHeader h = ovt::syntax::h265::parse_nal_header(br, b);
  auto result = parser(br, b, h);
  SyntaxNodeBuffer tree = b.finish();
  tree.set_root_name("nal_unit");
  if (tree_out != nullptr) {
    *tree_out = std::move(tree);
  }
  return result;
}

}  // namespace ovt::test::h265

#endif  // OVT_TESTS_SYNTAX_H265_TEST_UTIL_H
