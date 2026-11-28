// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of ovt_syntax_trace (chapter 12 §12.2.3.1, WI-1.504a; chapter 14 §14.7.1). The first byte picks the
// mode:
//   even: the rest is handed to ovt_syntax_trace as a SyntaxNode FlatBuffer (the verifier must reject garbage);
//   odd:  the rest is a program for SyntaxBuilder. Each program byte picks one read (u, f, flag, i, ue, se, te,
//         uvlc, leb128, su, ns, le, bytes, st), a nested struct or array element, a derived value or a diagnostic,
//         and the reads consume bits from the same bytes. The resulting tree must trace with OVT_OK, and every
//         element whose bits were rebuilt must show exactly the bits at its offset.
// A crash, a sanitizer report or a mismatch is a finding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/status.h"
#include "ovt/syntax.h"
#include "syntax/common/syntax_node_buffer.h"

namespace {

// The target links the module objects, not the shared library that exports ovt_buffer_free(): a caller-owned
// buffer is a new[] array (core/src/api/buffer.cpp), released here the way ovt_buffer_free() does.
void release(ovt_Buffer& b) {
  const std::unique_ptr<const uint8_t[]> owner(b.data);  // NOLINT(*-avoid-c-arrays)
  b = ovt_Buffer{nullptr, 0};
}

constexpr int kMaxOps = 64;
constexpr int kMaxNesting = 8;

using Read = void (*)(ovt::syntax::SyntaxBuilder&, unsigned);

// Ops 0..11 and 16..18 of a program byte: one read, derived value, diagnostic or enum name each.
const std::array<Read, 15> kReads = {
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_u("u", a); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_f("f", a, 0); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.read_flag("flag"); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_i("i", a + 1); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.read_ue("ue"); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.read_se("se"); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_te("te", a % 3U + 1U); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.read_uvlc("uvlc"); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.read_leb128("leb128"); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_su("su", a + 1); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_ns("ns", a + 1); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.read_le("le", a % 4U + 1U); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) { b.derived("D", a); },
    [](ovt::syntax::SyntaxBuilder& b, unsigned a) {
      b.diag(ovt::syntax::Severity::Warning, "SYN-TOY-0001", "fuzz", {}, (a & 1U) != 0);
    },
    [](ovt::syntax::SyntaxBuilder& b, unsigned /*a*/) { b.set_enum("E"); },
};

void run_program(std::span<const uint8_t> bytes, ovt::BitReader& br, ovt::syntax::SyntaxBuilder& b) {
  int depth = 0;
  for (size_t i = 0; i < bytes.size() && i < kMaxOps; ++i) {
    const unsigned op = bytes[i] % 20U;
    const unsigned arg = bytes[i] / 20U + 1U;  // 1..13
    if (op <= 11) {
      kReads.at(op)(b, arg);
    } else if (op >= 16 && op <= 18) {
      kReads.at(op - 4)(b, arg);
    } else if (op == 12 && br.byte_aligned()) {
      b.read_bytes("bytes", arg % 4U);
    } else if (op == 19 && br.byte_aligned()) {
      b.read_st("st");
    } else if (op == 12 || op == 19) {
      continue;  // byte reads need byte alignment
    } else if (op == 15) {
      if (depth > 0) {
        b.end_struct();  // also closes an array element (same node stack)
        --depth;
      }
    } else if (depth < kMaxNesting && op == 13) {
      b.begin_struct("s", "TOY 1");
      ++depth;
    } else if (depth < kMaxNesting) {
      b.begin_array_element("a", static_cast<int>(arg));
      ++depth;
    }
  }
  while (depth-- > 0) {
    b.end_struct();
  }
}

void check_trace(const std::vector<uint8_t>& tree, std::span<const uint8_t> bits) {
  ovt_Buffer out{nullptr, 0};
  if (ovt_syntax_trace(tree.data(), tree.size(), &out) != OVT_OK) {
    std::abort();
  }
  const std::string text(reinterpret_cast<const char*>(out.data), out.size);
  release(out);
  std::istringstream lines(text);
  for (std::string l; std::getline(lines, l);) {
    if (l.rfind("[ovt]", 0) != 0) {
      std::abort();
    }
    std::istringstream in(l.substr(5));
    std::string off;
    std::string name;
    std::string got;
    std::string eq;
    in >> off >> name >> got >> eq;
    if (eq != "=" || name == "->" || got == "--" || off.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    const uint64_t at = std::stoull(off);
    for (size_t k = 0; k < got.size(); ++k) {
      const uint64_t pos = at + k;
      if (pos / 8 >= bits.size() || (((bits[pos / 8] >> (7 - pos % 8)) & 1U) != 0) != (got[k] == '1')) {
        std::abort();
      }
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::span<const uint8_t> rest(data + 1, size - 1);
  if ((data[0] & 1U) == 0) {
    const std::vector<uint8_t> fb(rest.begin(), rest.end());  // operator new: aligned for the Verifier
    ovt_Buffer out{nullptr, 0};
    static_cast<void>(ovt_syntax_trace(fb.data(), fb.size(), &out));
    release(out);
    return 0;
  }
  ovt::BitReader br(rest);
  std::vector<uint8_t> tree;
  {
    ovt::syntax::SyntaxBuilder b(br, "TOY 0");
    try {
      run_program(rest, br, b);
    } catch (const ovt::ParseError&) {
      // A read past the end: the partial tree is still traced.
    }
    tree = b.finish().to_flatbuffer();
  }
  if (!tree.empty()) {
    check_trace(tree, rest);
  }
  return 0;
}
