// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FieldBuilder (chapter 08 §8.2.8, WI-1.202). See the header for the SPEC-ASSUMPTION about derived text.

#include "container/isobmff/field_builder.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace ovt::container::isobmff {

namespace {

using fb::SyntaxValueKind;

constexpr uint64_t kMaxInlineBytes = 1U << 20U;  // §8.2.8 bytes(): above this only a prefix is kept
constexpr uint64_t kBytesPrefix = 4096;
constexpr uint64_t kSecondsPerDay = 86400;
constexpr uint64_t kDays1904To1970 = 24107;
constexpr uint64_t kMaxDays1904 = 2957003;  // 9999-12-31

// "1.5", "0.0625", "-2.0": at most four decimals, at least one. `raw` is a two's-complement value of at most 32 bits.
std::string format_fixed(int64_t raw, unsigned frac_bits) {
  const bool neg = raw < 0;
  const uint64_t mag = neg ? 0U - static_cast<uint64_t>(raw) : static_cast<uint64_t>(raw);
  const uint64_t scaled = (mag * 10000U + (uint64_t{1} << (frac_bits - 1U))) >> frac_bits;
  std::string frac = std::to_string(scaled % 10000U);
  frac.insert(0, 4U - frac.size(), '0');
  while (frac.size() > 1 && frac.back() == '0') {
    frac.pop_back();
  }
  return std::string(neg && scaled != 0 ? "-" : "") + std::to_string(scaled / 10000U) + "." + frac;
}

std::string two_digits(uint64_t v) {
  return (v < 10 ? "0" : "") + std::to_string(v);
}

// Howard Hinnant's civil_from_days; `days` counts from 1970-01-01.
std::string iso8601(uint64_t seconds_since_1904) {
  const uint64_t days1904 = seconds_since_1904 / kSecondsPerDay;
  if (days1904 > kMaxDays1904) {
    return "out of range";
  }
  const uint64_t sod = seconds_since_1904 % kSecondsPerDay;
  const int64_t z = static_cast<int64_t>(days1904) - static_cast<int64_t>(kDays1904To1970) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<uint64_t>(z - era * 146097);
  const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint64_t mp = (5 * doy + 2) / 153;
  const uint64_t day = doy - (153 * mp + 2) / 5 + 1;
  const uint64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
  std::string y = std::to_string(year);
  y.insert(0, 4U - std::min<size_t>(4, y.size()), '0');
  return y + "-" + two_digits(month) + "-" + two_digits(day) + "T" + two_digits(sod / 3600) + ":" +
         two_digits(sod / 60 % 60) + ":" + two_digits(sod % 60) + "Z";
}

std::string display_fourcc(const std::array<char, 4>& cc) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  const bool printable = std::ranges::all_of(cc, [](char c) { return static_cast<uint8_t>(c) >= 0x20U && c != 0x7F; });
  std::string out;
  for (const char c : cc) {
    const auto b = static_cast<uint8_t>(c);
    if (!printable) {
      out += "\\x";
      out += kHex[b >> 4U];
      out += kHex[b & 0xFU];
    } else if (b < 0x80) {
      out += c;
    } else {  // ISO-8859-1 -> UTF-8 (so "\xA9nam" shows as "©nam")
      out += static_cast<char>(0xC0U | (b >> 6U));
      out += static_cast<char>(0x80U | (b & 0x3FU));
    }
  }
  return out;
}

std::string matrix_derived(const std::array<int32_t, 9>& m) {
  constexpr int32_t kOne = 0x10000;
  constexpr int32_t kW = 0x40000000;
  const auto is = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
    return m[0] == a && m[1] == b && m[3] == c && m[4] == d;
  };
  if (m[2] != 0 || m[5] != 0 || m[8] != kW) {
    return "custom";
  }
  if (m[6] == 0 && m[7] == 0 && is(kOne, 0, 0, kOne)) {
    return "identity";
  }
  if (is(0, kOne, -kOne, 0)) {
    return "rotation 90°";
  }
  if (is(-kOne, 0, 0, -kOne)) {
    return "rotation 180°";
  }
  if (is(0, -kOne, kOne, 0)) {
    return "rotation 270°";
  }
  return "custom";
}

}  // namespace

FieldBuilder::FieldBuilder(fb::BoxNodeT& node, uint64_t box_file_offset, BitReader& br)
    : FieldBuilder(node, box_file_offset, br, static_cast<uint64_t>(node.header_length) * 8U) {}

FieldBuilder::FieldBuilder(fb::BoxNodeT& node, uint64_t box_file_offset, BitReader& br, uint64_t payload_bit_base)
    : node_(&node), box_off_(box_file_offset), br_(&br), base_(payload_bit_base), origin_(br.bit_pos()) {}

std::vector<std::unique_ptr<fb::FieldT>>& FieldBuilder::sink() {
  return stack_.empty() ? node_->fields : stack_.back()->children;
}

// Appends a field that spans [start_pos, br.bit_pos()) of the reader.
fb::FieldT& FieldBuilder::add(std::string_view name, SyntaxValueKind kind, uint64_t start_pos) {
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name);
  f->kind = kind;
  const uint64_t len = br_->bit_pos() - start_pos;
  f->span = std::make_unique<fb::BitSpan>(rel(start_pos), static_cast<uint32_t>(std::min<uint64_t>(len, UINT32_MAX)));
  last_ = f.get();
  sink().push_back(std::move(f));
  return *last_;
}

fb::FieldT& FieldBuilder::integer(std::string_view name, SyntaxValueKind kind, unsigned nbits) {
  const uint64_t start = br_->bit_pos();
  if (kind == SyntaxValueKind::Int) {
    const int64_t v = br_->i(nbits);
    fb::FieldT& f = add(name, kind, start);
    f.ivalue = v;
    return f;
  }
  const uint64_t v = br_->u(nbits);
  fb::FieldT& f = add(name, kind, start);
  f.uvalue = v;
  return f;
}

uint64_t FieldBuilder::u(std::string_view name, unsigned nbits, std::string_view /*descriptor*/) {
  return integer(name, SyntaxValueKind::UInt, nbits).uvalue;
}
int64_t FieldBuilder::s(std::string_view name, unsigned nbits) {
  return integer(name, SyntaxValueKind::Int, nbits).ivalue;
}
uint8_t FieldBuilder::u8(std::string_view name) {
  return static_cast<uint8_t>(u(name, 8));
}
uint16_t FieldBuilder::u16(std::string_view name) {
  return static_cast<uint16_t>(u(name, 16));
}
uint32_t FieldBuilder::u24(std::string_view name) {
  return static_cast<uint32_t>(u(name, 24));
}
uint32_t FieldBuilder::u32(std::string_view name) {
  return static_cast<uint32_t>(u(name, 32));
}
uint64_t FieldBuilder::u64(std::string_view name) {
  return u(name, 64);
}
int16_t FieldBuilder::i16(std::string_view name) {
  return static_cast<int16_t>(s(name, 16));
}
int32_t FieldBuilder::i32(std::string_view name) {
  return static_cast<int32_t>(s(name, 32));
}
int64_t FieldBuilder::i64(std::string_view name) {
  return s(name, 64);
}
uint64_t FieldBuilder::u32or64(std::string_view name, bool wide) {
  return u(name, wide ? 64U : 32U);
}
int64_t FieldBuilder::i32or64(std::string_view name, bool wide) {
  return s(name, wide ? 64U : 32U);
}

double FieldBuilder::fixed(std::string_view name, unsigned nbits, unsigned frac_bits, bool is_signed) {
  const uint64_t start = br_->bit_pos();
  const int64_t raw = is_signed ? br_->i(nbits) : static_cast<int64_t>(br_->u(nbits));
  fb::FieldT& f = add(name, SyntaxValueKind::Fixed, start);
  f.ivalue = raw;
  f.svalue = format_fixed(raw, frac_bits);
  return static_cast<double>(raw) / static_cast<double>(uint64_t{1} << frac_bits);
}

double FieldBuilder::fixed16_16(std::string_view name, bool is_signed) {
  return fixed(name, 32, 16, is_signed);
}
double FieldBuilder::fixed8_8(std::string_view name, bool is_signed) {
  return fixed(name, 16, 8, is_signed);
}
double FieldBuilder::fixed2_30(std::string_view name) {
  return fixed(name, 32, 30, true);
}

std::array<char, 4> FieldBuilder::fourcc(std::string_view name) {
  const uint64_t start = br_->bit_pos();
  const uint64_t v = br_->u(32);
  std::array<char, 4> cc{};
  for (size_t i = 0; i < 4; ++i) {
    cc[i] = static_cast<char>((v >> (24U - 8U * i)) & 0xFFU);
  }
  fb::FieldT& f = add(name, SyntaxValueKind::String, start);
  f.uvalue = v;
  f.svalue = display_fourcc(cc);
  return cc;
}

std::string FieldBuilder::str_nul(std::string_view name, uint64_t max_bytes) {
  const uint64_t start = br_->bit_pos();
  std::string s;
  uint64_t n = 0;
  while (n < max_bytes && br_->bits_left() >= 8) {
    const auto c = static_cast<char>(br_->u(8));
    ++n;
    if (c == '\0') {
      break;
    }
    s += c;
  }
  add(name, SyntaxValueKind::String, start).svalue = s;
  return s;
}

std::string FieldBuilder::str_fixed(std::string_view name, uint32_t nbytes) {
  const uint64_t start = br_->bit_pos();
  std::string s;
  s.reserve(std::min<uint64_t>(nbytes, br_->bits_left() / 8U));
  for (uint32_t i = 0; i < nbytes; ++i) {
    s += static_cast<char>(br_->u(8));  // BR-0001 when the payload is shorter
  }
  s.resize(s.find('\0') == std::string::npos ? s.size() : s.find('\0'));
  add(name, SyntaxValueKind::String, start).svalue = s;
  return s;
}

std::span<const uint8_t> FieldBuilder::bytes(std::string_view name, uint64_t n) {
  const uint64_t start = br_->bit_pos();
  const std::span<const uint8_t> rest = br_->remaining_bytes();  // BR-0004 when not byte aligned
  if (n > rest.size()) {
    throw ParseError("BR-0001", "read of " + std::to_string(n) + " bytes past the end of the box", start);
  }
  const std::span<const uint8_t> view = rest.first(static_cast<size_t>(n));
  br_->skip_bytes(n);
  fb::FieldT& f = add(name, SyntaxValueKind::Bytes, start);
  const uint64_t kept = n <= kMaxInlineBytes ? n : kBytesPrefix;
  f.bytes.assign(view.begin(), view.begin() + static_cast<std::ptrdiff_t>(kept));
  if (kept < n) {
    f.uvalue = n;
    f.svalue = std::to_string(n) + " bytes, first " + std::to_string(kept) + " shown";
  }
  return view;
}

std::string FieldBuilder::lang_packed(std::string_view name) {
  const uint64_t start = br_->bit_pos();
  const uint64_t v = br_->u(16);
  std::string s;
  bool valid = (v >> 15U) == 0;
  for (const unsigned shift : {10U, 5U, 0U}) {
    const uint64_t c = (v >> shift) & 0x1FU;
    valid = valid && c >= 1 && c <= 26;
    s += static_cast<char>(0x60U + c);
  }
  if (!valid) {
    s = "und";  // the raw packed value stays in uvalue
  }
  fb::FieldT& f = add(name, SyntaxValueKind::String, start);
  f.uvalue = v;
  f.svalue = s;
  return s;
}

void FieldBuilder::reserved(unsigned nbits, uint64_t expected) {
  const uint64_t start = br_->bit_pos();
  uint64_t value = 0;
  for (unsigned left = nbits; left > 0;) {
    const unsigned chunk = std::min(left, 64U);
    value |= br_->u(chunk);
    left -= chunk;
  }
  fb::FieldT& f = add("reserved", SyntaxValueKind::UInt, start);
  f.uvalue = value;
  if (value != expected) {
    push_diag(
        Severity::Info, "CNT-1007",
        "reserved field has unexpected value " + std::to_string(value) + " (expected " + std::to_string(expected) + ")",
        f.span->bit_offset(), f.span->bit_length());
  }
}

std::string FieldBuilder::datetime1904(std::string_view name, bool wide) {
  const uint64_t start = br_->bit_pos();
  const uint64_t v = br_->u(wide ? 64U : 32U);
  fb::FieldT& f = add(name, SyntaxValueKind::UInt, start);
  f.uvalue = v;
  f.svalue = iso8601(v);
  return f.svalue;
}

uint64_t FieldBuilder::duration(std::string_view name, bool wide, uint32_t timescale) {
  const uint64_t start = br_->bit_pos();
  const uint64_t v = br_->u(wide ? 64U : 32U);
  fb::FieldT& f = add(name, SyntaxValueKind::UInt, start);
  f.uvalue = v;
  const uint64_t unknown = wide ? UINT64_MAX : UINT32_MAX;
  if (v == unknown) {
    f.svalue = "unknown";
  } else if (timescale == 0) {
    f.svalue = "unknown timescale";
  } else {
    uint64_t whole = v / timescale;
    uint64_t ms = ((v % timescale) * 1000U + timescale / 2U) / timescale;
    if (ms == 1000) {
      ++whole;
      ms = 0;
    }
    std::string frac = std::to_string(ms);
    frac.insert(0, 3U - frac.size(), '0');
    f.svalue = std::to_string(whole) + "." + frac + " s";
  }
  return v;
}

void FieldBuilder::matrix(std::string_view name) {
  group(name);
  std::array<int32_t, 9> m{};
  for (size_t i = 0; i < m.size(); ++i) {
    const bool wide_frac = i == 2 || i == 5 || i == 8;  // u, v, w are 2.30
    const std::string child = std::string(name) + "[" + std::to_string(i) + "]";
    if (wide_frac) {
      fixed2_30(child);
    } else {
      fixed16_16(child);
    }
    m[i] = static_cast<int32_t>(last_->ivalue);
  }
  end();
  derived(matrix_derived(m));
}

FieldBuilder& FieldBuilder::group(std::string_view name) {
  const uint64_t start = br_->bit_pos();
  add(name, SyntaxValueKind::None, start);
  stack_.push_back(last_);
  stack_start_.push_back(start);
  return *this;
}

FieldBuilder& FieldBuilder::array(std::string_view name, uint64_t count) {
  group(name);
  last_->uvalue = count;
  if (count > 1024) {
    last_->svalue = "see Sample Table view";
  }
  return *this;
}

FieldBuilder& FieldBuilder::array_element(std::string_view name, uint32_t index) {
  return group(std::string(name) + "[" + std::to_string(index) + "]");
}

void FieldBuilder::end() {
  if (stack_.empty()) {
    return;
  }
  fb::FieldT* g = stack_.back();
  const uint64_t len = br_->bit_pos() - stack_start_.back();
  g->span =
      std::make_unique<fb::BitSpan>(g->span->bit_offset(), static_cast<uint32_t>(std::min<uint64_t>(len, UINT32_MAX)));
  stack_.pop_back();
  stack_start_.pop_back();
  last_ = g;
}

void FieldBuilder::derived(std::string_view text) {
  if (last_ != nullptr && last_->kind != SyntaxValueKind::String) {
    last_->svalue = std::string(text);
  }
}

void FieldBuilder::enum_name(std::string_view text) {
  if (last_ != nullptr) {
    last_->kind = SyntaxValueKind::Enum;
    last_->svalue = std::string(text);
  }
}

void FieldBuilder::spec_ref(std::string_view text) {
  if (last_ != nullptr) {
    last_->spec_ref = std::string(text);
  }
}

void FieldBuilder::push_diag(Severity sev, std::string_view code, std::string_view msg, uint64_t bit_offset,
                             uint32_t bit_length) {
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = static_cast<fb::Severity>(sev);
  d->code = std::string(code);
  d->message = std::string(msg);
  d->span = std::make_unique<fb::BitSpan>(bit_offset, bit_length);
  if (const DiagInfo* info = container::diag(code); info != nullptr) {
    d->spec_ref = std::string(info->spec_ref);
  }
  node_->diagnostics.push_back(std::move(d));
}

void FieldBuilder::diag(Severity sev, std::string_view code, std::string_view msg) {
  if (last_ != nullptr) {
    push_diag(sev, code, msg, last_->span->bit_offset(), last_->span->bit_length());
  } else {
    push_diag(sev, code, msg, rel(br_->bit_pos()), 0);
  }
}

uint64_t FieldBuilder::bytes_consumed() const {
  return (br_->bit_pos() - origin_) / 8U;
}

void FieldBuilder::report_truncated(const ParseError& e) {
  while (!stack_.empty()) {
    end();
  }
  push_diag(Severity::Error, "CNT-1002", "truncated " + node_->fourcc + ": " + e.what(), rel(e.bit_offset), 0);
}

}  // namespace ovt::container::isobmff
