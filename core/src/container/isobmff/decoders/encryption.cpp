// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group H: encryption (chapter 08 §8.3.8, WI-1.213): `frma`, `schm`, `tenc`, `pssh` (with the Widevine
// protobuf and PlayReady Object payload decoders) and `senc`. `sinf` and `schi` are containers without
// fields. The CENC per-sample map (§8.4.4, WI-1.221) and the PIFF `uuid` variants (WI-1.216) build on these.
//
// Contract as in movie.cpp: the walker has read FullBox version/flags, `br` starts at the first field.
//
// SPEC-ASSUMPTION(#794): `tenc` also records its IV size in `ctx.enc_for_track` (keyed by track_ID) because the
// `senc` of a `moof` never sees the context of `schi`; a `senc` without any known IV size infers it (8, 16, then 0
// with subsamples) and raises Info CNT-6014.
// SPEC-ASSUMPTION(#795): the Widevine protobuf field numbers follow the open-source definition (policy 6,
// crypto_period_index 7, grouped_license 8, protection_scheme 9).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/decoders/pb_lite.h"
#include "container/isobmff/decoders/pssh_systems.h"
#include "container/isobmff/field_builder.h"
#include "container/isobmff/paged_span.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

using fb::SyntaxValueKind;

constexpr size_t kMaxChildFields = 1024;  // key_id / record / KID children kept per `pssh`
constexpr size_t kMaxXmlChars = 1U << 16U;
constexpr uint8_t kUnknownIvSize = 0xFF;

// Stamps `spec_ref` on every top-level field a decoder adds, also when the decoder throws (CNT-1002).
class SpecRefScope {
 public:
  SpecRefScope(FieldBuilder& fb, std::string_view ref) : fb_(fb), first_(fb.node().fields.size()), ref_(ref) {}
  SpecRefScope(const SpecRefScope&) = delete;
  SpecRefScope& operator=(const SpecRefScope&) = delete;
  SpecRefScope(SpecRefScope&&) = delete;
  SpecRefScope& operator=(SpecRefScope&&) = delete;
  // The string assignment can only fail with bad_alloc, which terminates the process anyway.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~SpecRefScope() {
    auto& fields = fb_.node().fields;
    for (size_t i = first_; i < fields.size(); ++i) {
      if (fields[i] && fields[i]->spec_ref.empty()) {
        fields[i]->spec_ref = ref_;
      }
    }
  }

 private:
  FieldBuilder& fb_;
  size_t first_;
  std::string_view ref_;
};

uint8_t version_of(FieldBuilder& fb) {
  const int v = fb.node().version;
  return v < 0 ? 0 : static_cast<uint8_t>(v);
}

uint32_t flags_of(FieldBuilder& fb) {
  const int32_t f = fb.node().flags;
  return f < 0 ? 0U : static_cast<uint32_t>(f);
}

bool known_version(FieldBuilder& fb, uint8_t max_version) {
  const uint8_t v = version_of(fb);
  if (v <= max_version) {
    return true;
  }
  fb.diag(Severity::Warning, "CNT-1003",
          "unknown " + fb.node().fourcc + " version " + std::to_string(v) + "; fields not decoded");
  return false;
}

void check_trailing(FieldBuilder& fb) {
  if (const uint64_t extra = fb.reader().bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in " + fb.node().fourcc);
  }
}

// A String sibling `<name>.derived`-style summary with an empty span at the end of the last field (#567).
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = 0;
  if (!fields.empty() && fields.back() && fields.back()->span) {
    at = fields.back()->span->bit_offset() + fields.back()->span->bit_length();
  }
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name);
  f->kind = SyntaxValueKind::String;
  f->svalue = std::string(text);
  f->span = std::make_unique<fb::BitSpan>(at, 0);
  fields.push_back(std::move(f));
}

void name_flags(FieldBuilder& fb, std::string_view text) {
  for (auto& f : fb.node().fields) {
    if (f && f->name == "flags") {
      f->svalue = std::string(text);
      return;
    }
  }
}

std::span<const uint8_t, 16> as_uuid(std::span<const uint8_t> bytes) {
  return bytes.first<16>();
}

// ---- frma (14496-12 §8.12.2) ---------------------------------------------------------------------------------

uint64_t decode_frma(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.12.2");
  fb.fourcc("data_format");
  check_trailing(fb);
  return 0;
}

// ---- schm (14496-12 §8.12.5) ---------------------------------------------------------------------------------

struct SchemeName {
  std::string_view code;
  std::string_view name;
};

constexpr auto kSchemes = std::to_array<SchemeName>({{"cenc", "AES-CTR"},
                                                     {"cbc1", "AES-CBC"},
                                                     {"cens", "AES-CTR with pattern"},
                                                     {"cbcs", "AES-CBC with pattern"},
                                                     {"piff", "PIFF (Smooth Streaming)"},
                                                     {"odkm", "OMA DRM"},
                                                     {"iAEC", "ISMACryp AES-CTR"}});

std::string scheme_version_text(uint32_t v) {
  return std::to_string(v >> 16U) + "." + std::to_string(v & 0xFFFFU);
}

uint64_t decode_schm(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.12.5");
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t flags = flags_of(fb);
  if ((flags & 1U) != 0) {
    name_flags(fb, "scheme_uri-present");
  }
  const auto scheme = fb.fourcc("scheme_type");
  const std::string_view code(scheme.data(), scheme.size());
  if (const auto* it = std::ranges::find(kSchemes, code, &SchemeName::code); it != kSchemes.end()) {
    derived_sibling(fb, "scheme_type.derived", it->name);
  }
  fb.u32("scheme_version");
  fb.derived(scheme_version_text(static_cast<uint32_t>(fb.last_field()->uvalue)));
  if ((flags & 1U) != 0) {
    fb.str_nul("scheme_uri", br.bits_left() / 8U);
  }
  check_trailing(fb);
  return 0;
}

// ---- tenc (23001-7 §8.2) -------------------------------------------------------------------------------------

bool valid_iv_size(uint8_t n) {
  return n == 0 || n == 8 || n == 16;
}

void record_track_encryption(BoxContext& ctx, uint8_t iv_size, bool is_protected) {
  if (ctx.enc_for_track && ctx.track_id != 0) {
    ctx.enc_for_track->try_emplace(ctx.track_id, TrackEncryption{iv_size, is_protected});  // first tenc wins
  }
}

uint64_t decode_tenc(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 23001-7 §8.2");
  if (!known_version(fb, 1)) {
    return 0;
  }
  fb.reserved(8);
  if (version_of(fb) == 0) {
    fb.reserved(8);
  } else {
    const uint64_t crypt = fb.u("default_crypt_byte_block", 4);
    const uint64_t skip = fb.u("default_skip_byte_block", 4);
    derived_sibling(fb, "pattern", std::to_string(crypt) + ":" + std::to_string(skip));
  }
  const uint8_t is_protected = fb.u8("default_isProtected");
  const uint8_t iv_size = fb.u8("default_Per_Sample_IV_Size");
  const bool iv_ok = valid_iv_size(iv_size);
  if (!iv_ok) {
    fb.diag(Severity::Error, "CNT-6003",
            "tenc default_Per_Sample_IV_Size " + std::to_string(iv_size) + " is not 0, 8 or 16");
  }
  const auto kid = fb.bytes("default_KID", 16);
  fb.derived(uuid_text(as_uuid(kid)));
  if (is_protected == 1 && iv_size == 0) {
    const uint8_t n = fb.u8("default_constant_IV_size");
    fb.bytes("default_constant_IV", n);
  }
  if (iv_ok) {
    ctx.per_sample_iv_size = iv_size;
    record_track_encryption(ctx, iv_size, is_protected == 1);
  }
  check_trailing(fb);
  return 0;
}

// ---- senc (23001-7 §7.2) -------------------------------------------------------------------------------------

uint32_t be16(std::span<const uint8_t> b, size_t at) {
  return (static_cast<uint32_t>(b[at]) << 8U) | b[at + 1U];
}

constexpr uint32_t kSencOverride = 0x1;
constexpr uint32_t kSencSubsamples = 0x2;

// Does a payload of `count` entries with `iv` IV bytes (and subsamples when `sub`) end exactly at `rest`'s end?
bool senc_fits(std::span<const uint8_t> rest, uint32_t count, unsigned iv, bool sub) {
  const size_t min_entry = iv + (sub ? 2U : 0U);
  if (min_entry == 0) {
    return rest.empty();
  }
  if (count > rest.size() / min_entry) {
    return false;
  }
  size_t pos = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (rest.size() - pos < min_entry) {
      return false;
    }
    pos += iv;
    if (sub) {
      const size_t need = size_t{6} * be16(rest, pos);
      pos += 2;
      if (rest.size() - pos < need) {
        return false;
      }
      pos += need;
    }
  }
  return pos == rest.size();
}

struct IvChoice {
  uint8_t size = kUnknownIvSize;
  std::string source;
};

// §8.3.8: the IV size from the box override, the context, the track's `tenc`; else inferred from the payload.
IvChoice choose_iv_size(const BoxContext& ctx, uint8_t override_size, bool has_override, std::span<const uint8_t> rest,
                        bool window_complete, uint32_t count, bool sub) {
  if (has_override) {
    return {override_size, "senc override"};
  }
  if (ctx.per_sample_iv_size != kUnknownIvSize) {
    return {ctx.per_sample_iv_size, "tenc (context)"};
  }
  if (ctx.enc_for_track) {
    if (const auto it = ctx.enc_for_track->find(ctx.track_id);
        it != ctx.enc_for_track->end() && it->second.per_sample_iv_size != kUnknownIvSize) {
      return {it->second.per_sample_iv_size, "tenc (track " + std::to_string(ctx.track_id) + ")"};
    }
  }
  if (window_complete) {
    for (const unsigned iv : {8U, 16U, 0U}) {
      if ((iv != 0 || sub) && senc_fits(rest, count, iv, sub)) {
        return {static_cast<uint8_t>(iv), "inferred"};
      }
    }
  }
  return {};
}

struct SubsampleTotals {
  uint64_t entries = 0;
  uint64_t clear = 0;
  uint64_t protected_bytes = 0;
};

// The subsample pairs of one sample (`shown` = recorded as fields); returns {clear, protected} byte totals.
std::pair<uint64_t, uint64_t> read_subsamples(BitReader& br, FieldBuilder& fb, bool shown, uint64_t& entries) {
  const uint32_t n = shown ? fb.u16("subsample_count") : static_cast<uint32_t>(br.u(16));
  if (shown) {
    fb.array("subsamples", n);
  }
  uint64_t clear = 0;
  uint64_t prot = 0;
  for (uint32_t j = 0; j < n; ++j) {
    const bool item = shown && FieldBuilder::materialise(j, n);
    if (item) {
      fb.array_element("subsample", j);
    }
    clear += item ? fb.u16("BytesOfClearData") : br.u(16);
    prot += item ? fb.u32("BytesOfProtectedData") : br.u(32);
    if (item) {
      fb.end();
    }
  }
  entries += n;
  if (shown) {
    fb.end();
  }
  return {clear, prot};
}

// One sample of the senc: IV, then (with subsamples) the count and the (clear, protected) pairs.
void read_senc_sample(BitReader& br, FieldBuilder& fb, uint32_t index, uint32_t count, unsigned iv_size, bool sub,
                      SubsampleTotals& totals) {
  const bool shown = FieldBuilder::materialise(index, count);
  if (shown) {
    fb.array_element("sample", index);
  }
  if (iv_size > 0) {
    if (shown) {
      fb.bytes("InitializationVector", iv_size);
    } else {
      br.skip_bytes(iv_size);
    }
  }
  uint64_t clear = 0;
  uint64_t prot = 0;
  if (sub) {
    std::tie(clear, prot) = read_subsamples(br, fb, shown, totals.entries);
  }
  totals.clear += clear;
  totals.protected_bytes += prot;
  if (shown) {
    fb.end();
    if (sub) {
      fb.derived("clear " + std::to_string(clear) + " + protected " + std::to_string(prot) + " = " +
                 std::to_string(clear + prot) + " bytes");
    }
  }
}

uint64_t decode_senc(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 23001-7 §7.2");
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t flags = flags_of(fb);
  const bool sub = (flags & kSencSubsamples) != 0;
  if (flags != 0) {
    name_flags(fb, std::string((flags & kSencOverride) != 0 ? "override-track-encryption" : "") +
                       ((flags & kSencOverride) != 0 && sub ? "|" : "") + (sub ? "use-subsample-encryption" : ""));
  }
  uint8_t override_size = 0;
  if ((flags & kSencOverride) != 0) {
    fb.u("AlgorithmID", 24);
    override_size = fb.u8("IV_size");
    const auto kid = fb.bytes("KID", 16);
    fb.derived(uuid_text(as_uuid(kid)));
  }
  const uint32_t count = fb.u32("sample_count");
  const std::span<const uint8_t> rest = br.remaining_bytes();
  const bool window_complete = ctx.payload_pages == nullptr || ctx.payload_pages->size() <= br.size_bits() / 8U;
  const IvChoice iv =
      choose_iv_size(ctx, override_size, (flags & kSencOverride) != 0, rest, window_complete, count, sub);
  if (count == 0) {
    check_trailing(fb);
    return 0;
  }
  if (iv.size == kUnknownIvSize) {
    fb.diag(Severity::Error, "CNT-6001",
            window_complete ? "senc IV size is unknown (no tenc) and neither 8 nor 16 bytes fit the payload exactly"
                            : "senc IV size is unknown (no tenc) and the payload is larger than the decode window");
    return 0;
  }
  derived_sibling(fb, "Per_Sample_IV_Size", std::to_string(iv.size) + " (" + iv.source + ")");
  if (iv.source == "inferred") {
    fb.diag(Severity::Info, "CNT-6014", "senc IV size " + std::to_string(iv.size) + " inferred from the box size");
  }
  if (iv.size == 0 && !sub) {
    return 0;  // constant IV, no subsamples: the entries carry no bytes
  }
  SubsampleTotals totals;
  fb.array("samples", count);
  for (uint32_t i = 0; i < count; ++i) {
    read_senc_sample(br, fb, i, count, iv.size, sub, totals);
  }
  fb.end();
  if (sub) {
    derived_sibling(fb, "subsample_totals",
                    std::to_string(totals.entries) + " subsamples, clear " + std::to_string(totals.clear) +
                        " bytes, protected " + std::to_string(totals.protected_bytes) + " bytes");
  }
  if (window_complete) {
    check_trailing(fb);
  }
  return 0;
}

// ---- child fields of a decoded `pssh` Data (spans are bits from the box start) -------------------------------

constexpr size_t kMaxChildBytes = 4096;

fb::FieldT& add_child(fb::FieldT& parent, uint64_t base_bit, std::string_view name, SyntaxValueKind kind,
                      size_t byte_offset, size_t byte_length) {
  auto c = std::make_unique<fb::FieldT>();
  c->name = std::string(name);
  c->kind = kind;
  const uint64_t bits = std::min<uint64_t>(uint64_t{byte_length} * 8U, UINT32_MAX);
  c->span = std::make_unique<fb::BitSpan>(base_bit + uint64_t{byte_offset} * 8U, static_cast<uint32_t>(bits));
  fb::FieldT& ref = *c;
  parent.children.push_back(std::move(c));
  return ref;
}

void set_bytes(fb::FieldT& f, std::span<const uint8_t> bytes) {
  const size_t kept = std::min(bytes.size(), kMaxChildBytes);
  f.bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kept));
  if (kept < bytes.size()) {
    f.uvalue = bytes.size();
    f.svalue = std::to_string(bytes.size()) + " bytes, first " + std::to_string(kept) + " shown";
  }
}

std::string printable_text(std::span<const uint8_t> bytes) {
  for (const uint8_t b : bytes) {
    if (b < 0x20 || b > 0x7E) {
      return {};
    }
  }
  return {bytes.begin(), bytes.end()};
}

// ---- Widevine WidevinePsshData (protobuf) --------------------------------------------------------------------

std::string_view widevine_algorithm(uint64_t v) {
  switch (v) {
    case 0:
      return "UNENCRYPTED";
    case 1:
      return "AESCTR";
    default:
      return "unknown";
  }
}

std::string fourcc_text(uint32_t v) {
  std::string s(4, ' ');
  for (size_t i = 0; i < 4; ++i) {
    s[i] = static_cast<char>((v >> (24U - 8U * i)) & 0xFFU);
  }
  return s;
}

struct WidevineState {
  size_t key_ids = 0;
  size_t children = 0;
};

// One protobuf field of WidevinePsshData; false when it is not a known field of the expected wire type.
bool widevine_field(fb::FieldT& data, uint64_t base, const pb::Field& f, WidevineState& st) {
  using enum pb::WireType;
  const auto at = [&](std::string_view name, SyntaxValueKind kind) -> fb::FieldT& {
    return add_child(data, base, name, kind, f.offset, f.size);
  };
  switch (f.number) {
    case 1:
      if (f.wire != Varint) {
        return false;
      }
      at("algorithm", SyntaxValueKind::Enum).uvalue = f.value;
      data.children.back()->svalue = std::string(widevine_algorithm(f.value));
      return true;
    case 2:
      if (f.wire != LengthDelimited) {
        return false;
      }
      if (st.key_ids++ < kMaxChildFields) {
        fb::FieldT& c = at("key_id[" + std::to_string(st.key_ids - 1U) + "]", SyntaxValueKind::Bytes);
        set_bytes(c, f.payload);
        if (f.payload.size() == 16) {
          c.svalue = uuid_text(as_uuid(f.payload));
        }
      }
      return true;
    case 3:
    case 6:
      if (f.wire != LengthDelimited) {
        return false;
      }
      at(f.number == 3 ? "provider" : "policy", SyntaxValueKind::String).svalue =
          std::string(f.payload.begin(), f.payload.end());
      return true;
    case 4:
    case 8: {
      if (f.wire != LengthDelimited) {
        return false;
      }
      fb::FieldT& c = at(f.number == 4 ? "content_id" : "grouped_license", SyntaxValueKind::Bytes);
      set_bytes(c, f.payload);
      if (const std::string text = printable_text(f.payload); !text.empty() && f.number == 4) {
        c.svalue = text;
      }
      return true;
    }
    case 7:
      if (f.wire != Varint) {
        return false;
      }
      at("crypto_period_index", SyntaxValueKind::UInt).uvalue = f.value;
      return true;
    case 9:
      if (f.wire != Varint) {
        return false;
      }
      {
        fb::FieldT& c = at("protection_scheme", SyntaxValueKind::String);
        c.uvalue = f.value;
        c.svalue = fourcc_text(static_cast<uint32_t>(f.value));
      }
      return true;
    default:
      return false;
  }
}

// Decodes `data` (the bytes of the `Data` field, which starts at `base` bits in the box) into children of `data_field`.
void decode_widevine(FieldBuilder& fb, fb::FieldT& data_field, std::span<const uint8_t> data) {
  const uint64_t base = data_field.span->bit_offset();
  pb::Reader r(data);
  pb::Field f;
  WidevineState st;
  while (r.next(f)) {
    if (widevine_field(data_field, base, f, st)) {
      continue;
    }
    if (st.children++ >= kMaxChildFields) {
      continue;
    }
    fb::FieldT& c =
        add_child(data_field, base, "field_" + std::to_string(f.number), SyntaxValueKind::UInt, f.offset, f.size);
    if (f.wire == pb::WireType::LengthDelimited) {
      c.kind = SyntaxValueKind::Bytes;
      set_bytes(c, f.payload);
    } else {
      c.uvalue = f.value;
    }
  }
  if (st.key_ids > kMaxChildFields) {
    add_child(data_field, base, "key_id.count", SyntaxValueKind::UInt, 0, 0).uvalue = st.key_ids;
  }
  if (!r.ok()) {
    fb.diag(Severity::Info, "CNT-1005",
            "Widevine pssh Data is not valid protobuf from byte " + std::to_string(r.position()) + "; " +
                std::to_string(data.size() - r.position()) + " bytes are not decoded");
  }
}

// ---- PlayReady Object (PlayReady Header Specification) -------------------------------------------------------

uint32_t le16(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1U]) << 8U);
}

uint32_t le32(std::span<const uint8_t> b, size_t at) {
  return le16(b, at) | (le16(b, at + 2U) << 16U);
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80U) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800U) {
    out += static_cast<char>(0xC0U | (cp >> 6U));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  } else if (cp < 0x10000U) {
    out += static_cast<char>(0xE0U | (cp >> 12U));
    out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  } else {
    out += static_cast<char>(0xF0U | (cp >> 18U));
    out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  }
}

// UTF-16LE (with an optional BOM) to UTF-8; a lone surrogate becomes U+FFFD; at most kMaxXmlChars code units.
std::string utf16le_to_utf8(std::span<const uint8_t> b) {
  std::string out;
  size_t i = 0;
  if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
    i = 2;
  }
  const size_t end = std::min(b.size() & ~size_t{1}, i + 2U * kMaxXmlChars);
  while (i < end) {
    uint32_t cp = le16(b, i);
    i += 2;
    if (cp >= 0xD800U && cp < 0xDC00U && i < end && le16(b, i) >= 0xDC00U && le16(b, i) < 0xE000U) {
      cp = 0x10000U + ((cp - 0xD800U) << 10U) + (le16(b, i) - 0xDC00U);
      i += 2;
    } else if (cp >= 0xD800U && cp < 0xE000U) {
      cp = 0xFFFDU;
    }
    append_utf8(out, cp);
  }
  return out;
}

// Standard base64 (whitespace and padding tolerated); empty on any other character.
std::vector<uint8_t> base64_decode(std::string_view s) {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  unsigned bits = 0;
  for (const char c : s) {
    uint32_t v = 0;
    if (c >= 'A' && c <= 'Z') {
      v = static_cast<uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      v = static_cast<uint32_t>(c - 'a') + 26U;
    } else if (c >= '0' && c <= '9') {
      v = static_cast<uint32_t>(c - '0') + 52U;
    } else if (c == '+') {
      v = 62;
    } else if (c == '/') {
      v = 63;
    } else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    } else {
      return {};
    }
    acc = (acc << 6U) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFU));
    }
  }
  return out;
}

// The text between `<tag>` and `</tag>`, or empty.
std::string_view element_text(std::string_view xml, std::string_view tag, size_t from = 0) {
  const std::string open = "<" + std::string(tag) + ">";
  const std::string close = "</" + std::string(tag) + ">";
  const size_t a = xml.find(open, from);
  if (a == std::string_view::npos) {
    return {};
  }
  const size_t b = xml.find(close, a + open.size());
  return b == std::string_view::npos ? std::string_view{} : xml.substr(a + open.size(), b - a - open.size());
}

// Every key ID of a WRMHEADER: `<KID>base64</KID>` (4.0.0.0) and `<KID ... VALUE="base64"/>` (4.1.0.0 and later).
std::vector<std::string> wrm_kids(std::string_view xml) {
  std::vector<std::string> kids;
  size_t pos = 0;
  while (kids.size() < kMaxChildFields && (pos = xml.find("<KID", pos)) != std::string_view::npos) {
    pos += 4;
    if (pos >= xml.size()) {
      break;
    }
    if (xml[pos] == '>') {
      const size_t close = xml.find("</KID>", pos);
      if (close != std::string_view::npos) {
        kids.emplace_back(xml.substr(pos + 1U, close - pos - 1U));
      }
    } else if (xml[pos] == ' ') {
      const size_t tag_end = xml.find('>', pos);
      const std::string_view tag =
          xml.substr(pos, tag_end == std::string_view::npos ? std::string_view::npos : tag_end - pos);
      if (const size_t v = tag.find("VALUE=\""); v != std::string_view::npos) {
        const size_t q = tag.find('"', v + 7U);
        if (q != std::string_view::npos) {
          kids.emplace_back(tag.substr(v + 7U, q - v - 7U));
        }
      }
    }
  }
  return kids;
}

// A PlayReady key ID is a GUID (first three groups little-endian); the CENC KID is the same 16 bytes big-endian.
std::string playready_kid_text(const std::vector<uint8_t>& guid) {
  std::array<uint8_t, 16> k{};
  std::copy(guid.begin(), guid.end(), k.begin());
  std::swap(k[0], k[3]);
  std::swap(k[1], k[2]);
  std::swap(k[4], k[5]);
  std::swap(k[6], k[7]);
  return uuid_text(k);
}

void decode_wrmheader(fb::FieldT& record, uint64_t base, size_t offset, size_t length, std::string_view xml) {
  size_t n = 0;
  for (const std::string& b64 : wrm_kids(xml)) {
    const std::vector<uint8_t> raw = base64_decode(b64);
    if (raw.size() != 16) {
      continue;
    }
    add_child(record, base, "KID[" + std::to_string(n++) + "]", SyntaxValueKind::String, offset, length).svalue =
        playready_kid_text(raw) + " (base64 " + b64 + ")";
  }
  if (const std::string_view url = element_text(xml, "LA_URL"); !url.empty()) {
    add_child(record, base, "LA_URL", SyntaxValueKind::String, offset, length).svalue = std::string(url);
  }
}

std::string_view playready_record_name(uint32_t type) {
  switch (type) {
    case 1:
      return "rights management header (WRMHEADER)";
    case 2:
      return "device ID";
    case 3:
      return "embedded license store";
    default:
      return {};
  }
}

void decode_playready_record(fb::FieldT& data_field, uint64_t base, std::span<const uint8_t> data, size_t at,
                             size_t index, uint32_t type, uint32_t len) {
  fb::FieldT& rec =
      add_child(data_field, base, "record[" + std::to_string(index) + "]", SyntaxValueKind::None, at, 4U + len);
  add_child(rec, base, "record_type", SyntaxValueKind::UInt, at, 2).uvalue = type;
  if (const std::string_view name = playready_record_name(type); !name.empty()) {
    rec.children.back()->svalue = std::string(name);
  }
  add_child(rec, base, "record_length", SyntaxValueKind::UInt, at + 2U, 2).uvalue = len;
  const std::span<const uint8_t> value = data.subspan(at + 4U, len);
  if (type == 1) {
    const std::string xml = utf16le_to_utf8(value);
    add_child(rec, base, "record_value", SyntaxValueKind::String, at + 4U, len).svalue = xml;
    decode_wrmheader(rec, base, at + 4U, len, xml);
  } else {
    set_bytes(add_child(rec, base, "record_value", SyntaxValueKind::Bytes, at + 4U, len), value);
  }
}

void decode_playready(FieldBuilder& fb, fb::FieldT& data_field, std::span<const uint8_t> data) {
  const uint64_t base = data_field.span->bit_offset();
  if (data.size() < 6) {
    fb.diag(Severity::Info, "CNT-1005", "PlayReady pssh Data is shorter than its 6-byte object header");
    return;
  }
  add_child(data_field, base, "length", SyntaxValueKind::UInt, 0, 4).uvalue = le32(data, 0);
  const uint32_t count = le16(data, 4);
  add_child(data_field, base, "record_count", SyntaxValueKind::UInt, 4, 2).uvalue = count;
  size_t at = 6;
  for (uint32_t i = 0; i < count && i < kMaxChildFields; ++i) {
    if (data.size() - at < 4U) {
      fb.diag(Severity::Info, "CNT-1005", "PlayReady object ends inside record " + std::to_string(i));
      return;
    }
    const uint32_t type = le16(data, at);
    const uint32_t len = le16(data, at + 2U);
    if (len > data.size() - at - 4U) {
      fb.diag(Severity::Info, "CNT-1005", "PlayReady record " + std::to_string(i) + " is longer than the object");
      return;
    }
    decode_playready_record(data_field, base, data, at, i, type, len);
    at += 4U + len;
  }
}

// ---- pssh (23001-7 §8.1) -------------------------------------------------------------------------------------

constexpr std::array<uint8_t, 16> kWidevineId = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                                 0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
constexpr std::array<uint8_t, 16> kPlayReadyId = {0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
                                                  0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95};

// Reads KID_count and the KIDs of a version 1 box; false (CNT-6004 raised) when the count does not fit the box.
bool read_pssh_kids(BitReader& br, FieldBuilder& fb, uint64_t& kid_count) {
  const uint32_t count = fb.u32("KID_count");
  kid_count = count;
  if (count > br.bits_left() / 8U / 16U) {
    fb.diag(Severity::Error, "CNT-6004", "pssh KID_count " + std::to_string(count) + " does not fit the box");
    return false;
  }
  fb.array("KIDs", count);
  for (uint32_t i = 0; i < count; ++i) {
    if (FieldBuilder::materialise(i, count)) {
      const auto kid = fb.bytes("KID[" + std::to_string(i) + "]", 16);
      fb.derived(uuid_text(as_uuid(kid)));
    } else {
      br.skip_bytes(16);
    }
  }
  fb.end();
  return true;
}

uint64_t decode_pssh(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 23001-7 §8.1");
  if (!known_version(fb, 1)) {
    return 0;
  }
  const auto system = fb.bytes("SystemID", 16);
  const std::string_view name = pssh_system_name(as_uuid(system));
  fb.derived(name.empty() ? uuid_text(as_uuid(system)) : std::string(name));
  if (name.empty()) {
    fb.diag(Severity::Warning, "CNT-6009", "pssh SystemID " + uuid_text(as_uuid(system)) + " is not a known system");
  }
  uint64_t kid_count = 0;
  const bool v1 = version_of(fb) == 1;
  if (v1 && !read_pssh_kids(br, fb, kid_count)) {
    return 0;
  }
  const uint32_t data_size = fb.u32("DataSize");
  const uint64_t available = br.bits_left() / 8U;
  const auto data = fb.bytes("Data", std::min<uint64_t>(data_size, available));
  const uint64_t header = fb.node().header_length;
  const uint64_t expected = header + 4U + 16U + (v1 ? 4U + 16U * kid_count : 0U) + 4U + data_size;
  if (fb.node().file_span && expected != fb.node().file_span->length()) {
    fb.diag(Severity::Error, "CNT-6004",
            "pssh DataSize " + std::to_string(data_size) + " implies a box of " + std::to_string(expected) +
                " bytes, the box has " + std::to_string(fb.node().file_span->length()));
  }
  fb::FieldT* data_field = fb.last_field();
  if (data_field != nullptr && data_field->span != nullptr && !data.empty()) {
    if (std::ranges::equal(system, kWidevineId)) {
      decode_widevine(fb, *data_field, data);
    } else if (std::ranges::equal(system, kPlayReadyId)) {
      decode_playready(fb, *data_field, data);
    }
  }
  return 0;
}

}  // namespace

// One registration per §8.3.8 row with fields (`sinf` and `schi` are containers).
OVT_BOX_DECODER("frma", "*", &decode_frma)
OVT_BOX_DECODER("schm", "*", &decode_schm)
OVT_BOX_DECODER("tenc", "*", &decode_tenc)
OVT_BOX_DECODER("pssh", "*", &decode_pssh)
OVT_BOX_DECODER("senc", "*", &decode_senc)

}  // namespace ovt::container::isobmff
