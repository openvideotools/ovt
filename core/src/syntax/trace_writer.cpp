// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_syntax_trace (chapter 12 §12.3.8, WI-1.504): the `--trace` text of §12.2.3.1 from an ovt.fb.SyntaxNode
// FlatBuffer. The buffer carries no unit bytes, so the bit string of each element is rebuilt from its descriptor,
// value and span (SPEC-ASSUMPTION(#822)).

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/buffer.hpp"
#include "ovt/status.h"
#include "ovt/syntax.h"
#include "ovt_model_generated.h"

namespace ovt::syntax {

namespace {

namespace fb = ovt::fb;

constexpr std::size_t kNameWidth = 50;
constexpr std::size_t kBitsWidth = 16;
constexpr std::size_t kMaxBytesShown = 32;

// Titles of the structure header lines, after FFmpeg's trace_headers wording; other structures print their name.
constexpr std::array<std::pair<std::string_view, std::string_view>, 24> kTitles = {{
    {"nal_unit_header", "NAL unit header"},
    {"video_parameter_set_rbsp", "Video Parameter Set"},
    {"seq_parameter_set_rbsp", "Sequence Parameter Set"},
    {"seq_parameter_set_data", "Sequence Parameter Set"},
    {"subset_seq_parameter_set_rbsp", "Subset Sequence Parameter Set"},
    {"pic_parameter_set_rbsp", "Picture Parameter Set"},
    {"access_unit_delimiter_rbsp", "Access Unit Delimiter"},
    {"sei_rbsp", "Supplemental Enhancement Information"},
    {"sei_message", "SEI message"},
    {"slice_segment_header", "Slice Segment Header"},
    {"slice_header", "Slice Header"},
    {"profile_tier_level", "Profile, tier, level"},
    {"vui_parameters", "VUI Parameters"},
    {"hrd_parameters", "HRD Parameters"},
    {"st_ref_pic_set", "Short-term reference picture set"},
    {"pred_weight_table", "Prediction weight table"},
    {"obu_header", "OBU header"},
    {"sequence_header_obu", "Sequence Header"},
    {"frame_header_obu", "Frame Header"},
    {"uncompressed_header", "Uncompressed Header"},
    {"metadata_obu", "Metadata"},
    {"tile_group_obu", "Tile Group"},
    {"color_config", "Color config"},
    {"timing_info", "Timing info"},
}};

std::string_view title_of(std::string_view name) {
  for (const auto& [n, t] : kTitles) {
    if (n == name) {
      return t;
    }
  }
  return name;
}

std::string_view sv(const flatbuffers::String* s) {
  return s == nullptr ? std::string_view{} : std::string_view(s->data(), s->size());
}

// A buffer string as one line of trace text: control characters, a backslash and a double quote are escaped,
// so that a string read from the stream cannot break the one-element-per-line layout.
std::string text(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20U || u == 0x7FU) {
      fmt::format_to(std::back_inserter(out), "\\x{:02x}", u);
    } else if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}

std::string_view severity_word(fb::Severity s) {
  switch (s) {
    case fb::Severity::Info:
      return "INFO";
    case fb::Severity::Warning:
      return "WARN";
    case fb::Severity::Error:
      return "ERROR";
    case fb::Severity::Fatal:
      return "FATAL";
  }
  return "INFO";
}

// The low `n` bits of `v`, most significant first.
constexpr uint32_t kMaxRebuiltBits = 4096;

std::string binary(uint64_t v, uint32_t n) {
  std::string s;
  s.reserve(n);
  for (uint32_t k = n; k > 0; --k) {
    s += (k - 1 < 64 && ((v >> (k - 1)) & 1U) != 0) ? '1' : '0';
  }
  return s;
}

// Exp-Golomb code word of `code_num` (ue(v), uvlc()); empty when it is longer than `max_len` bits.
std::string exp_golomb(uint64_t code_num, uint32_t max_len) {
  const uint64_t x = code_num + 1;  // 0 when code_num is 2^64 - 1, which no reader returns
  uint32_t len = 0;
  for (uint64_t t = x; t != 0; t >>= 1U) {
    ++len;
  }
  // The code word is 2 * len - 1 bits; one longer than the element's span cannot be the bits as read, and a buffer
  // from the caller must not make the writer allocate more than the span (SPEC-ASSUMPTION(#822)).
  if (len == 0 || 2 * len - 1 > max_len) {
    return {};
  }
  return std::string(len - 1, '0') + binary(x, len);
}

bool starts_with(std::string_view s, std::string_view p) {
  return s.substr(0, p.size()) == p;
}

// le(n) bytes, least significant first; empty when the length is not whole bytes.
std::string le_bits(uint64_t u, uint32_t len) {
  std::string bits;
  for (uint32_t i = 0; len % 8 == 0 && len <= 64 && i < len / 8; ++i) {
    bits += binary(u >> (8 * i), 8);
  }
  return bits;
}

// leb128() groups. At most 8 (table 7.2.4); the 8th group's continuation bit is read and ignored (BR-0009), so it is
// not known from the value: a full 8-group code is left empty and prints `--`.
std::string leb128_bits(uint64_t u, uint32_t len) {
  std::string bits;
  const uint32_t count = len % 8 == 0 && len < 64 ? len / 8 : 0;
  for (uint32_t i = 0; i < count; ++i) {
    bits += (i + 1 < count ? '1' : '0') + binary((u >> (7 * i)) & 0x7FU, 7);
  }
  return bits;
}

// The code word of a variable-length descriptor (ue, se, te, uvlc); empty for any other descriptor.
std::string vlc_bits(std::string_view d, const fb::SyntaxNode& n, uint64_t u, uint32_t len) {
  if (d == "uvlc()" && u == UINT32_MAX) {
    // 32 or more leading zeros: the value bits are not read (AV1 §4.10.3), only the zeros and the one bit.
    return std::string(len - 1, '0') + '1';
  }
  if (d == "ue(v)" || d == "uvlc()") {
    return exp_golomb(u, len);
  }
  if (d == "se(v)") {
    const int64_t v = n.ivalue();
    return exp_golomb(v > 0 ? 2 * static_cast<uint64_t>(v) - 1 : 2 * (0 - static_cast<uint64_t>(v)), len);
  }
  if (d == "te(v)") {
    return len == 1 ? std::string(1, u == 0 ? '1' : '0') : exp_golomb(u, len);
  }
  return {};
}

// The bits of one element as read, rebuilt from its descriptor and value; nullopt when that is not possible.
std::optional<std::string> rebuild_bits(const fb::SyntaxNode& n) {
  const std::string_view d = sv(n.descriptor());
  const uint32_t len = n.span() == nullptr ? 0U : n.span()->bit_length();
  const fb::SyntaxValueKind kind = n.kind();
  // kMaxRebuiltBits bounds what a caller's buffer can make the writer allocate (a capped uvlc() is the only code
  // longer than 64 bits).
  if (kind == fb::SyntaxValueKind::Bytes || kind == fb::SyntaxValueKind::String || len == 0 || len > kMaxRebuiltBits) {
    return std::nullopt;
  }
  uint64_t u = n.uvalue();
  if (kind == fb::SyntaxValueKind::Bool) {
    u = n.bvalue() ? 1U : 0U;
  } else if (kind == fb::SyntaxValueKind::Int || kind == fb::SyntaxValueKind::Fixed) {
    u = static_cast<uint64_t>(n.ivalue());
  }
  std::string bits;
  if (starts_with(d, "u(") || starts_with(d, "f(") || starts_with(d, "b(") || starts_with(d, "i(") ||
      starts_with(d, "su(")) {
    bits = len <= 64 ? binary(u, len) : std::string();
  } else if (starts_with(d, "le(")) {
    bits = le_bits(u, len);
  } else if (d == "leb128()") {
    bits = leb128_bits(u, len);
  } else {
    bits = vlc_bits(d, n, u, len);
  }
  return bits.size() == len ? std::optional<std::string>(std::move(bits)) : std::nullopt;
}

class Trace {
 public:
  explicit Trace(std::string& out) : out_(out) {}

  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by the FlatBuffers verifier (max depth 64)
  void walk(const fb::SyntaxNode& n, bool root) {
    const auto* children = n.children();
    const bool container = children != nullptr && !children->empty();
    if (!container) {
      element(n);
    } else {
      const bool array_element = n.kind() == fb::SyntaxValueKind::None && n.array_index() >= 0;
      if (array_element) {
        indices_.push_back(n.array_index());
      } else if (!root) {
        const std::string_view ref = sv(n.spec_ref());
        out_ += fmt::format("[ovt] ---- {} ----", text(title_of(sv(n.name()))));
        if (!ref.empty()) {
          out_ += fmt::format("  ({})", text(ref));
        }
        out_ += '\n';
      }
      for (const fb::SyntaxNode* c : *children) {
        walk(*c, false);
      }
      if (array_element) {
        indices_.pop_back();
      }
    }
    diagnostics(n);
  }

 private:
  void element(const fb::SyntaxNode& n) {
    const uint32_t len = n.span() == nullptr ? 0U : n.span()->bit_length();
    if (n.kind() == fb::SyntaxValueKind::None) {
      // A derived variable without bits of its own (e.g. "CtbSizeY = 64").
      if (const std::string_view dv = sv(n.derived()); !dv.empty()) {
        const uint64_t at = n.span() == nullptr ? 0U : n.span()->bit_offset();
        out_ += fmt::format("[ovt] {:>6}  -> {}\n", at, text(dv));
      }
      return;
    }
    if (len == 0) {
      return;
    }
    std::string name = text(sv(n.name()));
    if (!indices_.empty()) {
      for (const int i : indices_) {
        name += fmt::format("[{}]", i);
      }
    } else if (n.array_index() >= 0) {
      name += fmt::format("[{}]", n.array_index());
    }
    const std::string bits = rebuild_bits(n).value_or("--");
    std::string line = fmt::format("[ovt] {:>6}  {:<{}}", n.span()->bit_offset(), name, kNameWidth);
    if (name.size() >= kNameWidth) {
      line += ' ';
    }
    line += fmt::format("{:>{}} = ", bits, kBitsWidth);
    line += value(n);
    if (const std::string_view e = sv(n.enum_name()); !e.empty()) {
      line += fmt::format(" ({})", text(e));
    }
    if (const std::string_view dv = sv(n.derived()); !dv.empty()) {
      line += fmt::format(" -> {}", text(dv));
    }
    out_ += line;
    out_ += '\n';
  }

  static std::string value(const fb::SyntaxNode& n) {
    switch (n.kind()) {
      case fb::SyntaxValueKind::UInt:
      case fb::SyntaxValueKind::Enum:
        return fmt::format("{}", n.uvalue());
      case fb::SyntaxValueKind::Int:
      case fb::SyntaxValueKind::Fixed:
        return fmt::format("{}", n.ivalue());
      case fb::SyntaxValueKind::Bool:
        return n.bvalue() ? "1" : "0";
      case fb::SyntaxValueKind::String:
        return fmt::format("\"{}\"", text(sv(n.svalue())));
      case fb::SyntaxValueKind::Bytes: {
        std::string s;
        const auto* b = n.bytes();
        const std::size_t count = b == nullptr ? 0 : b->size();
        for (std::size_t i = 0; i < count && i < kMaxBytesShown; ++i) {
          fmt::format_to(std::back_inserter(s), "{:02x}", b->Get(static_cast<flatbuffers::uoffset_t>(i)));
        }
        if (count > kMaxBytesShown) {
          s += "...";
        }
        return s.empty() ? std::string("--") : s;
      }
      case fb::SyntaxValueKind::None:
        break;
    }
    return {};
  }

  void diagnostics(const fb::SyntaxNode& n) {
    if (n.diagnostics() == nullptr) {
      return;
    }
    for (const fb::Diagnostic* d : *n.diagnostics()) {
      out_ +=
          fmt::format("[ovt] !! {} {} {}\n", severity_word(d->severity()), text(sv(d->code())), text(sv(d->message())));
    }
  }

  std::string& out_;
  std::vector<int> indices_;
};

// The §12.2.3.1 lines of a verified SyntaxNode tree.
std::string trace_text(const fb::SyntaxNode& root) {
  std::string out;
  Trace(out).walk(root, true);
  return out;
}

}  // namespace

}  // namespace ovt::syntax

OVT_API ovt_status ovt_syntax_trace(const uint8_t* syntax_node_fb, size_t len, ovt_Buffer* out_text) {
  if (out_text != nullptr) {
    *out_text = ovt_Buffer{nullptr, 0};
  }
  if (out_text == nullptr || (syntax_node_fb == nullptr && len != 0)) {
    return OVT_E_INVALID_ARG;
  }
  if (len == 0) {
    return ovt::api::make_buffer(std::string_view{}, out_text);
  }
  try {
    flatbuffers::Verifier verifier(syntax_node_fb, len);
    if (!verifier.VerifyBuffer<ovt::fb::SyntaxNode>(nullptr)) {
      return OVT_E_CORRUPT;
    }
    const std::string text = ovt::syntax::trace_text(*flatbuffers::GetRoot<ovt::fb::SyntaxNode>(syntax_node_fb));
    return ovt::api::make_buffer(text, out_text);
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}
