// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared by the per-unit fuzz targets of WI-1.129 (fuzz_h264_units, fuzz_h265_units, fuzz_av1_obus): the abort
// helper, the splitter of the canned contexts of fuzz_unit_contexts.inc, a fingerprint of a syntax tree and the
// driver that runs one input against the contexts.
//
// The driver runs the unit against every context (the coverage pass) and must finish that pass in under one second
// (WI-1.129 step 3: catches quadratic loops). One context, the input size modulo the context count (so most
// mutations keep it), runs with recording on, the others with recording off, which is several times cheaper. That
// context then runs twice more on freshly built contexts: with recording on it must give the same tree, with
// recording off it must succeed or fail alike, with the same ParseError code, and on success end at the same bit
// (§0.7.5 determinism; "reset per input", WI-1.129 pitfalls). The tree is compared by fingerprint, not by JSON:
// to_json() costs ten times the parse in the sanitizer build and would starve the 60 s smoke run.

#ifndef OVT_TESTS_FUZZ_FUZZ_UNITS_COMMON_H
#define OVT_TESTS_FUZZ_FUZZ_UNITS_COMMON_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt_model_generated.h"
#include "syntax/common/parameter_set.h"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt_fuzz {

inline void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

/// The NAL units of an H.26x context: 4-byte start codes, as tools/fuzz_seed.py --contexts writes them.
inline std::vector<std::span<const uint8_t>> split_context(std::span<const unsigned char> bytes) {
  std::vector<std::span<const uint8_t>> units;
  size_t begin = 4;
  for (size_t i = 4; i + 4 <= bytes.size(); ++i) {
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
      units.push_back(bytes.subspan(begin, i - begin));
      begin = i + 4;
    }
  }
  units.push_back(bytes.subspan(begin));
  return units;
}

class Fingerprint {
 public:
  void add(uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
      h_ = (h_ ^ ((v >> (8 * i)) & 0xFFU)) * 0x100000001B3ULL;  // FNV-1a
    }
  }
  void add(std::string_view s) noexcept {
    add(s.size());
    for (const char c : s) {
      add(static_cast<uint64_t>(static_cast<unsigned char>(c)));
    }
  }
  void add(const ovt::syntax::SyntaxNodeData* n) noexcept {  // NOLINT(misc-no-recursion): the tree is finite
    for (; n != nullptr; n = n->next_sibling) {
      add(n->name);
      add(n->derived);
      add(n->svalue);
      add(n->enum_name);
      add(static_cast<uint64_t>(n->kind));
      add(n->uvalue ^ static_cast<uint64_t>(n->ivalue) ^ (n->bvalue ? 1U : 0U));
      add(n->span.bit_offset);
      add(n->span.bit_length);
      add(n->bytes.size());
      for (const ovt::syntax::DiagnosticData* d = n->first_diag; d != nullptr; d = d->next) {
        add(d->code);
        add(static_cast<uint64_t>(d->severity));
      }
      add(n->first_child);
      add(uint64_t{0xC105ED});
    }
  }
  [[nodiscard]] uint64_t value() const noexcept { return h_; }

 private:
  uint64_t h_ = 0xCBF29CE484222325ULL;
};

inline uint64_t fingerprint(const ovt::syntax::SyntaxNodeBuffer& tree) {
  Fingerprint f;
  f.add(tree.root());
  return f.value();
}

struct Result {
  bool ok = false;       // no ParseError
  std::string error;     // ParseError::code otherwise
  uint64_t end_bit = 0;  // reader position after the parsers (compared only when ok: after a throw it may differ)
  uint64_t tree = 0;     // fingerprint of the tree (recording on only)

  [[nodiscard]] bool same_outcome(const Result& o) const {
    return ok == o.ok && error == o.error && (!ok || end_bit == o.end_bit);
  }
};

/// `run(context, recording)` parses the input once against a freshly built context.
template <class Run>
void drive(std::span<const std::span<const unsigned char>> contexts, size_t input_size, Run run) {
  const size_t pick = input_size % contexts.size();
  const auto start = std::chrono::steady_clock::now();
  std::vector<Result> first;
  first.reserve(contexts.size());
  for (size_t i = 0; i < contexts.size(); ++i) {
    first.push_back(run(contexts[i], i == pick));
  }
  require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  const Result again = run(contexts[pick], true);
  const Result quiet = run(contexts[pick], false);
  require(again.same_outcome(first[pick]) && again.end_bit == first[pick].end_bit && again.tree == first[pick].tree);
  require(quiet.same_outcome(first[pick]));
}

/// The configuration-record entry point (syntax/common/parameter_set.h, WI-1.207), which takes one unit too: the
/// context's units go in first through one ParameterSetContext, then the input. It never throws; two runs must give
/// the same tree.
inline void check_parameter_set(ovt::fb::Codec codec, std::span<const std::span<const uint8_t>> context_units,
                                std::span<const uint8_t> input) {
  const auto once = [&] {
    ovt::syntax::ParameterSetContext ctx;
    for (const std::span<const uint8_t> unit : context_units) {
      (void)ovt::syntax::parse_parameter_set(codec, unit, &ctx);
    }
    const ovt::syntax::ParameterSetResult r = ovt::syntax::parse_parameter_set(codec, input, &ctx);
    return fingerprint(r.tree) ^ (r.complete ? 1U : 0U) ^ (static_cast<uint64_t>(r.unit_type) << 1U);
  };
  const uint64_t first = once();
  const uint64_t again = once();
  require(first == again);
}

}  // namespace ovt_fuzz

#endif  // OVT_TESTS_FUZZ_FUZZ_UNITS_COMMON_H
