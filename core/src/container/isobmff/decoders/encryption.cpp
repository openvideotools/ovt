// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group H: encryption (chapter 08 §8.3.8, WI-1.213a): `frma`, `schm`, `tenc` and `senc`. Part b adds
// `pssh` with the Widevine protobuf and PlayReady Object payload decoders. `sinf` and `schi` are containers without
// fields. The CENC per-sample map (§8.4.4, WI-1.221) and the PIFF `uuid` variants (WI-1.216) build on these.
//
// Contract as in movie.cpp: the walker has read FullBox version/flags, `br` starts at the first field.
//
// SPEC-ASSUMPTION(#794): `tenc` also records its IV size in `ctx.enc_for_track` (keyed by track_ID) because the
// `senc` of a `moof` never sees the context of `schi`; a `senc` without any known IV size infers it (8, 16, then 0
// with subsamples) and raises Info CNT-6011.

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
#include "container/isobmff/decoders/pssh_systems.h"
#include "container/isobmff/field_builder.h"
#include "container/isobmff/paged_span.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

using fb::SyntaxValueKind;

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
    fb.diag(Severity::Info, "CNT-6011", "senc IV size " + std::to_string(iv.size) + " inferred from the box size");
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

}  // namespace

// One registration per §8.3.8 row with fields (`sinf` and `schi` are containers).
OVT_BOX_DECODER("frma", "*", &decode_frma)
OVT_BOX_DECODER("schm", "*", &decode_schm)
OVT_BOX_DECODER("tenc", "*", &decode_tenc)
OVT_BOX_DECODER("senc", "*", &decode_senc)

}  // namespace ovt::container::isobmff
