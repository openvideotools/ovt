// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_syntax_trace (chapter 12 §12.2.3.1, WI-1.504a): the exact lines of a hand-built tree with every descriptor,
// and over the encoder-made seeds of the index_builder fuzz target (H.264, H.265, AV1): every line starts with
// `[ovt]` (acceptance test 3) and every rebuilt bit string equals the unit's bits at the element's offset.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/index.h"
#include "ovt/status.h"
#include "ovt/syntax.h"
#include "ovt_model_generated.h"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using Bytes = std::vector<uint8_t>;
using ovt::syntax::Severity;
using ovt::syntax::SyntaxBuilder;

// This test links the module objects, not the shared library that exports ovt_buffer_free(): a caller-owned
// buffer is a new[] array (core/src/api/buffer.cpp), released here the way ovt_buffer_free() does.
void release(ovt_Buffer& b) {
  const std::unique_ptr<const uint8_t[]> owner(b.data);  // NOLINT(*-avoid-c-arrays)
  b = ovt_Buffer{nullptr, 0};
}

Bytes take(ovt_Buffer& b) {
  Bytes v(b.data, b.data + b.size);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  release(b);
  return v;
}

std::string trace(const Bytes& tree) {
  ovt_Buffer b{nullptr, 0};
  REQUIRE(ovt_syntax_trace(tree.data(), tree.size(), &b) == OVT_OK);
  const Bytes t = take(b);
  return {t.begin(), t.end()};
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    out.push_back(l);
  }
  return out;
}

Bytes from_bits(std::string_view bits) {
  Bytes out((bits.size() + 7) / 8, 0);
  for (size_t i = 0; i < bits.size(); ++i) {
    if (bits[i] == '1') {
      out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
    }
  }
  return out;
}

// Checks every element line whose bits were rebuilt against `unit`; returns {checked, scalar lines left at "--"}.
std::pair<size_t, size_t> check_bits(const std::string& text, std::span<const uint8_t> unit) {
  size_t checked = 0;
  size_t opaque = 0;
  for (const std::string& l : lines(text)) {
    CAPTURE(l);
    REQUIRE(l.rfind("[ovt]", 0) == 0);
    std::istringstream in(l.substr(5));
    std::string off;
    std::string name;
    std::string bits;
    std::string eq;
    std::string value;
    in >> off >> name >> bits >> eq >> value;
    if (eq != "=" || name == "->" || off.find_first_not_of("0123456789") != std::string::npos) {
      continue;  // structure, derived and diagnostic lines
    }
    if (bits == "--") {
      // Byte payloads print `--` by design; count the scalar values whose bits were not rebuilt (ns(n)).
      const bool scalar = value.find_first_not_of("-0123456789") == std::string::npos &&
                          value.find_first_of("0123456789") != std::string::npos;
      opaque += scalar ? 1U : 0U;
      continue;
    }
    const uint64_t at = std::stoull(off);
    std::string actual;
    for (size_t k = 0; k < bits.size(); ++k) {
      const uint64_t pos = at + k;
      REQUIRE(pos / 8 < unit.size());
      actual += ((unit[pos / 8] >> (7 - pos % 8)) & 1U) != 0 ? '1' : '0';
    }
    CHECK(actual == bits);
    ++checked;
  }
  return {checked, opaque};
}

}  // namespace

TEST_CASE("syntax trace: every descriptor, structures, arrays, derived values and diagnostics",
          "[syntax][trace][fast]") {
  const std::string bits =
      "101"                // u(3) = 5
      "1"                  // flag
      "1100"               // i(4) = -4
      "00101"              // ue = 4
      "00100"              // se = +2 (codeNum 3)
      "011"                // se = -1 (codeNum 2)
      "0"                  // te(cmax 1) = 1
      "011"                // te(cmax 3) = 2
      "0001010"            // uvlc = 9
      "111110"             // su(6) = -2
      "10"                 // ns(5) = 2
      "00000000"           // f(8) (48 bits so far)
      "00110100"           // le(2) low byte 0x34
      "00010010"           // le(2) high byte 0x12
      "10000001"           // leb128 group 1 + continuation
      "00000001"           // leb128 group 2 = 129
      "01001111"           // st "O"
      "00000000"           // st terminator
      "1010101111001101";  // bytes ab cd
  const Bytes unit = from_bits(bits);
  ovt::BitReader br(unit);
  Bytes tree;
  {
    SyntaxBuilder b(br, "TOY 1");
    {
      auto s = b.scoped("seq_parameter_set_rbsp", "TOY 7.3.2.2");
      b.read_u("u3", 3);
      b.set_enum("FIVE");
      b.read_flag("flag");
      b.read_i("i4", 4);
      b.read_ue("ue");
      b.derived("Twice", 8);
      b.read_se("se_pos");
      b.read_se("se_neg");
      b.read_te("te1", 1);
      b.read_te("te3", 3);
      for (int i = 0; i < 2; ++i) {
        b.begin_array_element("row", i);
        if (i == 0) {
          b.read_uvlc("uvlc");
        } else {
          b.read_su("su", 6);
          b.diag(Severity::Warning, "SYN-TOY-0007", "odd su", {}, true);
        }
        b.end_array_element();
      }
      b.read_ns("ns", 5);
      b.read_f("align", 8, 0);
    }
    {
      auto s = b.scoped("other_struct");
      b.read_le("le", 2);
      b.read_leb128("leb");
      b.read_st("name");
      b.read_bytes("payload", 2);
      b.diag(Severity::Error, "SYN-TOY-0008", "on the struct");
    }
    tree = b.finish().to_flatbuffer();
  }
  const std::vector<std::string> want = {
      "[ovt] ---- Sequence Parameter Set ----  (TOY 7.3.2.2)",
      "[ovt]      0  u3                                                             101 = 5 (FIVE)",
      "[ovt]      3  flag                                                             1 = 1",
      "[ovt]      4  i4                                                            1100 = -4",
      "[ovt]      8  ue                                                           00101 = 4",
      "[ovt]     13  -> Twice = 8",
      "[ovt]     13  se_pos                                                       00100 = 2",
      "[ovt]     18  se_neg                                                         011 = -1",
      "[ovt]     21  te1                                                              0 = 1",
      "[ovt]     22  te3                                                            011 = 2",
      "[ovt]     25  uvlc[0]                                                    0001010 = 9",
      "[ovt]     32  su[1]                                                       111110 = -2",
      "[ovt] !! WARN SYN-TOY-0007 odd su",
      "[ovt]     38  ns                                                              -- = 2",
      "[ovt]     40  align                                                     00000000 = 0",
      "[ovt] ---- other_struct ----  (TOY 1)",
      "[ovt]     48  le                                                0011010000010010 = 4660",
      "[ovt]     64  leb                                               1000000100000001 = 129",
      "[ovt]     80  name                                                            -- = \"O\"",
      "[ovt]     96  payload                                                         -- = abcd",
      "[ovt] !! ERROR SYN-TOY-0008 on the struct",
  };
  CHECK(lines(trace(tree)) == want);
  CHECK(check_bits(trace(tree), unit).first == 13);
}

TEST_CASE("syntax trace: argument checks, a corrupt buffer, deep trees", "[syntax][trace][fast]") {
  ovt_Buffer b{nullptr, 0};
  CHECK(ovt_syntax_trace(nullptr, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_syntax_trace(nullptr, 4, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_syntax_trace(nullptr, 0, &b) == OVT_OK);
  CHECK(b.size == 0);
  release(b);
  const Bytes junk = {0xff, 0xff, 0xff, 0x7f, 1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(ovt_syntax_trace(junk.data(), junk.size(), &b) == OVT_E_CORRUPT);
  CHECK(b.data == nullptr);

  // SyntaxBuilder nests up to 64 levels; the verifier must accept such a tree.
  const Bytes unit(16, 0x55);
  ovt::BitReader br(unit);
  Bytes tree;
  {
    SyntaxBuilder sb(br);
    for (int i = 0; i < 60; ++i) {
      sb.begin_struct("s");
    }
    sb.read_u("leaf", 4);
    tree = sb.finish().to_flatbuffer();
  }
  const auto ls = lines(trace(tree));
  REQUIRE(ls.size() == 61);
  CHECK(ls.back() == "[ovt]      0  leaf                                                          0101 = 5");

  // A caller's buffer cannot make the writer build a code word longer than the element's span: ue(v), se(v),
  // te(v) and uvlc() with an out-of-range value, and a span of 2^32 - 1 bits, print `--` (review of #831).
  for (const auto& [desc, bits] : {std::pair{"ue(v)", 5U}, std::pair{"te(v)", 5U}, std::pair{"uvlc()", 5U},
                                   std::pair{"se(v)", 5U}, std::pair{"uvlc()", 0xFFFFFFFFU}}) {
    CAPTURE(desc, bits);
    flatbuffers::FlatBufferBuilder fbb;
    const ovt::fb::BitSpan span(0, bits);
    fbb.Finish(ovt::fb::CreateSyntaxNodeDirect(fbb, "x", desc, ovt::fb::SyntaxValueKind::UInt,
                                               bits == 5U ? UINT64_MAX : UINT32_MAX, INT64_MIN, false, nullptr, nullptr,
                                               nullptr, nullptr, &span));
    const Bytes buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    const auto out = lines(trace(buf));
    REQUIRE(out.size() == 1);
    CHECK(out[0].find(" -- = ") != std::string::npos);
  }

  // A string read from the stream stays on its line: control characters, '"' and '\\' are escaped.
  const Bytes text = {'a', '\n', '"', '\\', 0x7f, 'b', 0};
  ovt::BitReader tr(text);
  {
    SyntaxBuilder sb(tr);
    sb.read_st("st");
    tree = sb.finish().to_flatbuffer();
  }
  CHECK(lines(trace(tree)) ==
        std::vector<std::string>{
            R"([ovt]      0  st                                                              -- = "a\x0a\"\\\x7fb")"});
}

namespace {

// The units of a committed seed, through the stream API.
struct SeedStream {
  ovt_File file{};
  ovt_Container* container = nullptr;
  ovt_Stream* stream = nullptr;
  Bytes bytes;
  uint32_t units = 0;

  SeedStream(const char* name, const char* hint) {
    std::ifstream in(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / name, std::ios::binary);
    REQUIRE(in.good());
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 1);
    bytes.erase(bytes.begin());  // the fuzz target's selector byte
    file.file = std::make_unique<ovt::io::MemoryFile>(bytes, hint);
    REQUIRE(ovt_container_open(&file, &container) == OVT_OK);
    REQUIRE(ovt_stream_open(container, 0, &stream) == OVT_OK);
    ovt_Buffer b{nullptr, 0};
    REQUIRE(ovt_stream_build_index(stream, nullptr, &b) == OVT_OK);
    static_cast<void>(take(b));
    while (true) {
      ovt_Buffer raw{nullptr, 0};
      if (ovt_stream_unit_bytes(stream, units, &raw, nullptr) != OVT_OK) {
        break;
      }
      static_cast<void>(take(raw));
      ++units;
    }
  }
  SeedStream(const SeedStream&) = delete;
  SeedStream& operator=(const SeedStream&) = delete;
  SeedStream(SeedStream&&) = delete;
  SeedStream& operator=(SeedStream&&) = delete;
  ~SeedStream() {
    ovt_stream_close(stream);
    ovt_container_close(container);
  }

  [[nodiscard]] Bytes tree(uint32_t i) const {
    ovt_Buffer b{nullptr, 0};
    REQUIRE(ovt_stream_unit_syntax(stream, i, &b) == OVT_OK);
    return take(b);
  }
  // The bytes the offsets count in: the RBSP for H.26x, the OBU as stored for AV1 (no RBSP form).
  [[nodiscard]] Bytes unit_bits(uint32_t i) const {
    ovt_Buffer raw{nullptr, 0};
    ovt_Buffer rbsp{nullptr, 0};
    REQUIRE(ovt_stream_unit_bytes(stream, i, &raw, &rbsp) == OVT_OK);
    Bytes r = take(raw);
    Bytes p = take(rbsp);
    return p.empty() ? r : p;
  }
};

}  // namespace

TEST_CASE("syntax trace AT3: every line starts with [ovt], and the rebuilt bits are the unit's bits",
          "[syntax][trace][fast]") {
  for (const auto& [seed, hint] :
       {std::pair{"seed_s_h265_main_bframes.bin", "s.h265"}, std::pair{"seed_s_h265_scaling_list_amp.bin", "s.h265"},
        std::pair{"seed_s_h264_baseline.bin", "s.h264"}, std::pair{"seed_s_h264_high_bpyramid.bin", "s.h264"},
        std::pair{"seed_s_av1_aom_420.bin", "s.obu"}, std::pair{"seed_s_av1_aom_film_grain.bin", "s.obu"}}) {
    CAPTURE(seed);
    const SeedStream s(seed, hint);
    REQUIRE(s.units > 3);
    size_t checked = 0;
    size_t opaque = 0;
    size_t traced = 0;
    for (uint32_t i = 0; i < s.units; ++i) {
      const Bytes tree = s.tree(i);
      if (tree.empty()) {
        continue;
      }
      const std::string text = trace(tree);
      CHECK(text == trace(tree));  // deterministic
      const auto [c, o] = check_bits(text, s.unit_bits(i));
      checked += c;
      opaque += o;
      ++traced;
    }
    CAPTURE(checked, opaque);
    CHECK(traced > 3);
    CHECK(checked > 100);
    CHECK(opaque * 20 < checked);  // ns(n) is the only scalar left at "--"
  }
}
