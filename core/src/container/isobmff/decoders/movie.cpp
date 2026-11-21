// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group A: file and movie level boxes (chapter 08 §8.3.1, WI-1.205). Each row of §8.3.1 that has
// fields is one OVT_BOX_DECODER registration; the pure containers (`moov`, `trak`, `edts`, `mdia`, `minf`, `dinf`,
// `udta`, `tref`, `trgr`) and the opaque boxes (`free`, `skip`, `wide`, `mdat`) have no field decoder.
//
// Contract (§8.2.9): the walker has already read FullBox version/flags into `node.version` / `node.flags` (and
// their fields), so `br` starts at the first field after them. Decoders read fields in the listed order so spans
// are contiguous, and set the BoxContext members their descendants need (`movie_timescale`, `track_timescale`,
// `track_id`, `handler_type`). Derived text goes into `Field.svalue` (SPEC-ASSUMPTION(#415), field_builder.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

using Fourcc = std::array<char, 4>;

std::string_view sv(const Fourcc& cc) {
  return {cc.data(), cc.size()};
}

// ---- interim name tables -------------------------------------------------------------------------------------
// SPEC-ASSUMPTION(#567, #574): §8.3.1 takes brand and handler descriptions from the generated kBrands / kHandlers
// tables of WI-1.203 (brand_description() / handler_description()). Their descriptions stay empty until the owner
// rules on the MP4RA data licence, so these short tables of the MP4RA entries seen in the container-smoke corpus
// are the fallback; a code in neither has no derived text (no CNT-1031, which needs the full kHandlers table).
struct Name {
  std::string_view code;
  std::string_view text;
};

constexpr auto kBrandNames = std::to_array<Name>({
    {"3g2a", "3GPP2"},
    {"3gp4", "3GPP Release 4"},
    {"3gp5", "3GPP Release 5"},
    {"3gp6", "3GPP Release 6"},
    {"avc1", "Advanced Video Coding extensions"},
    {"av01", "AV1 Image File Format / AV1 in ISOBMFF"},
    {"avif", "AV1 Image File Format"},
    {"cmfc", "CMAF track"},
    {"cmf2", "CMAF track, second structural brand"},
    {"dash", "MPEG-DASH segment"},
    {"dby1", "Dolby extensions"},
    {"heic", "HEVC image"},
    {"hvc1", "HEVC (MIAF)"},
    {"iso2", "ISO Base Media File Format version 2"},
    {"iso3", "ISO Base Media File Format version 3"},
    {"iso4", "ISO Base Media File Format version 4"},
    {"iso5", "ISO Base Media File Format version 5"},
    {"iso6", "ISO Base Media File Format version 6"},
    {"iso7", "ISO Base Media File Format version 7"},
    {"iso8", "ISO Base Media File Format version 8"},
    {"iso9", "ISO Base Media File Format version 9"},
    {"isom", "ISO Base Media File Format"},
    {"M4A ", "Apple iTunes AAC-LC audio"},
    {"M4V ", "Apple iTunes video"},
    {"mif1", "HEIF image"},
    {"msdh", "Media segment"},
    {"msix", "Indexed media segment"},
    {"mp41", "MP4 version 1"},
    {"mp42", "MP4 version 2"},
    {"mp71", "MPEG-7 metadata in MP4"},
    {"qt  ", "QuickTime"},
    {"sims", "Sub-indexed media segment"},
});

constexpr auto kHandlerNames = std::to_array<Name>({
    {"alis", "QuickTime alias data reference"},
    {"auxv", "Auxiliary video"},
    {"clcp", "Closed captions (QuickTime)"},
    {"hint", "Hint track"},
    {"mdir", "iTunes metadata"},
    {"mdta", "QuickTime metadata keys"},
    {"meta", "Timed metadata"},
    {"odsm", "Object descriptor stream"},
    {"pict", "Image item (HEIF)"},
    {"sbtl", "Subtitles (QuickTime)"},
    {"sdsm", "Scene description stream"},
    {"soun", "Audio track"},
    {"subt", "Subtitle track"},
    {"text", "Timed text"},
    {"tmcd", "Timecode (QuickTime)"},
    {"vide", "Video track"},
});

constexpr auto kReferenceNames = std::to_array<Name>({
    {"auxl", "auxiliary media for the referenced track"},
    {"cdsc", "content describes"},
    {"chap", "chapter list (QuickTime)"},
    {"font", "font for the referenced track"},
    {"hind", "hint dependency"},
    {"hint", "hint track for the referenced media"},
    {"sbas", "scalable base"},
    {"scal", "scalable extractor reference"},
    {"subt", "subtitle for the referenced track"},
    {"tmcd", "timecode (QuickTime)"},
    {"vdep", "auxiliary depth video"},
    {"vplx", "auxiliary parallax video"},
});

template <size_t N>
std::string_view name_of(const std::array<Name, N>& table, std::string_view code) {
  const auto it = std::ranges::find(table, code, &Name::code);
  return it == table.end() ? std::string_view{} : it->text;
}

// The registry's description of a brand / handler, else the interim table's.
std::string_view brand_text(std::string_view code) {
  const std::string_view d = brand_description(code);
  return d.empty() ? name_of(kBrandNames, code) : d;
}

std::string_view handler_text(std::string_view code) {
  const std::string_view d = handler_description(code);
  return d.empty() ? name_of(kHandlerNames, code) : d;
}

// ---- shared helpers ------------------------------------------------------------------------------------------

// Stamps `spec_ref` on every top-level field a decoder adds, also when the decoder throws (CNT-1002).
class SpecRefScope {
 public:
  SpecRefScope(FieldBuilder& fb, std::string_view ref) : fb_(fb), first_(fb.node().fields.size()), ref_(ref) {}
  SpecRefScope(const SpecRefScope&) = delete;
  SpecRefScope& operator=(const SpecRefScope&) = delete;
  SpecRefScope(SpecRefScope&&) = delete;
  SpecRefScope& operator=(SpecRefScope&&) = delete;
  ~SpecRefScope() {
    auto& fields = fb_.node().fields;
    for (size_t i = first_; i < fields.size(); ++i) {
      if (fields[i] && fields[i]->spec_ref.empty()) {
        fields[i]->spec_ref = std::string(ref_);
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
  return v < 0 ? 0 : static_cast<uint8_t>(v);  // -1: the walker read no version (treated as version 0)
}

uint32_t flags_of(FieldBuilder& fb) {
  const int32_t f = fb.node().flags;
  return f < 0 ? 0U : static_cast<uint32_t>(f);
}

// CNT-1003 for a version the decoder does not know; the payload is then left undecoded.
bool known_version(FieldBuilder& fb, uint8_t max_version) {
  const uint8_t v = version_of(fb);
  if (v <= max_version) {
    return true;
  }
  fb.diag(Severity::Warning, "CNT-1003",
          "unknown " + fb.node().fourcc + " version " + std::to_string(v) + "; fields not decoded");
  return false;
}

// CNT-1015: a fixed-size box (§8.5 list) with bytes after its last field.
void check_fixed_size_end(FieldBuilder& fb) {
  const uint64_t extra = fb.reader().bits_left() / 8U;
  if (extra > 0) {
    fb.diag(Severity::Warning, "CNT-1015",
            fb.node().fourcc + " is " + std::to_string(extra) + " bytes larger than its fields");
  }
}

// CNT-1032 for a zero timescale.
void check_timescale(FieldBuilder& fb, uint32_t timescale) {
  if (timescale == 0) {
    fb.diag(Severity::Warning, "CNT-1032", fb.node().fourcc + ".timescale is 0");
  }
}

struct FlagName {
  uint32_t bit;
  std::string_view name;
};

// The walker records `flags` as a field of its own; its svalue gets the names of the set bits ("" when none).
template <size_t N>
void name_flags(FieldBuilder& fb, const std::array<FlagName, N>& names) {
  const uint32_t flags = flags_of(fb);
  std::string text;
  for (const FlagName& n : names) {
    if ((flags & n.bit) != 0) {
      text += (text.empty() ? "" : "|") + std::string(n.name);
    }
  }
  for (auto& f : fb.node().fields) {
    if (f && f->name == "flags") {
      f->svalue = text;
      return;
    }
  }
}

// `media_time` in the track timescale -> "x.xxx s"; the text when it cannot be expressed.
std::string media_time_text(int64_t t, uint32_t timescale) {
  if (t == -1) {
    return "empty edit";
  }
  if (t < 0) {
    return "invalid (negative)";
  }
  if (timescale == 0) {
    return "track timescale unknown";
  }
  const auto v = static_cast<uint64_t>(t);
  uint64_t whole = v / timescale;
  uint64_t ms = ((v % timescale) * 1000U + timescale / 2U) / timescale;
  if (ms == 1000) {
    ++whole;
    ms = 0;
  }
  std::string frac = std::to_string(ms);
  frac.insert(0, 3U - frac.size(), '0');
  return std::to_string(whole) + "." + frac + " s";
}

// SPEC-ASSUMPTION(#567): derived text for a String field (fourcc): §8.3 notation, a String sibling `<field>.derived`
// with an empty span right after the source field. Only for top-level fields (FieldBuilder::derived() is a no-op on
// String fields).
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = 0;
  if (!fields.empty() && fields.back() && fields.back()->span) {
    at = fields.back()->span->bit_offset() + fields.back()->span->bit_length();
  }
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name) + ".derived";
  f->kind = fb::SyntaxValueKind::String;
  f->svalue = std::string(text);
  f->span = std::make_unique<fb::BitSpan>(at, 0);
  fields.push_back(std::move(f));
}

// CNT-1005: bytes after the last field of a box whose layout has no trailing data.
void check_trailing(FieldBuilder& fb) {
  if (const uint64_t extra = fb.reader().bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in " + fb.node().fourcc);
  }
}

// ---- ftyp / styp (14496-12 §4.3, §8.16.2) ---------------------------------------------------------------------

uint64_t decode_file_type(BoxContext& ctx, BitReader& br, FieldBuilder& fb, std::string_view ref) {
  const SpecRefScope scope(fb, ref);
  const Fourcc major = fb.fourcc("major_brand");
  if (const std::string_view d = brand_text(sv(major)); !d.empty()) {
    derived_sibling(fb, "major_brand", d);
  }
  fb.u32("minor_version");
  const uint64_t count = br.bits_left() / 32U;
  std::string compat;
  fb.array("compatible_brands", count);
  for (uint64_t i = 0; i < count; ++i) {
    const Fourcc cc = fb.fourcc("compatible_brands[" + std::to_string(i) + "]");
    if (const std::string_view d = brand_text(sv(cc)); !d.empty()) {
      compat += (compat.empty() ? "" : "; ") + std::string(sv(cc)) + ": " + std::string(d);
    }
  }
  fb.end();
  if (!compat.empty()) {
    derived_sibling(fb, "compatible_brands", compat);
  }
  check_trailing(fb);  // fewer than 4 bytes left: not a whole brand
  if (fb.node().fourcc == "ftyp") {
    ctx.quicktime = sv(major) == "qt  ";
  }
  return 0;
}

uint64_t decode_ftyp(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_file_type(ctx, br, fb, "ISO/IEC 14496-12:2022 §4.3");
}

uint64_t decode_styp(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_file_type(ctx, br, fb, "ISO/IEC 14496-12:2022 §8.16.2");
}

// ---- mvhd (14496-12 §8.2.2) ----------------------------------------------------------------------------------

uint64_t decode_mvhd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.2.2");
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool wide = version_of(fb) == 1;
  fb.datetime1904("creation_time", wide);
  fb.datetime1904("modification_time", wide);
  const uint32_t timescale = fb.u32("timescale");
  check_timescale(fb, timescale);
  ctx.movie_timescale = timescale;
  fb.duration("duration", wide, timescale);
  fb.fixed16_16("rate");
  fb.fixed8_8("volume");
  fb.reserved(16);
  fb.reserved(64);
  fb.matrix("matrix");
  fb.array("pre_defined", 6);
  for (unsigned i = 0; i < 6; ++i) {
    fb.u32("pre_defined[" + std::to_string(i) + "]");
  }
  fb.end();
  fb.u32("next_track_ID");
  check_fixed_size_end(fb);
  return 0;
}

// ---- tkhd (14496-12 §8.3.2) ----------------------------------------------------------------------------------

constexpr auto kTkhdFlags = std::to_array<FlagName>({
    {0x1, "track_enabled"},
    {0x2, "track_in_movie"},
    {0x4, "track_in_preview"},
    {0x8, "track_size_is_aspect_ratio"},
});

uint64_t decode_tkhd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.3.2");
  name_flags(fb, kTkhdFlags);
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool wide = version_of(fb) == 1;
  fb.datetime1904("creation_time", wide);
  fb.datetime1904("modification_time", wide);
  ctx.track_id = fb.u32("track_ID");
  fb.reserved(32);
  fb.duration("duration", wide, ctx.movie_timescale);  // all ones: "unknown"
  fb.reserved(64);
  fb.i16("layer");
  fb.i16("alternate_group");
  fb.fixed8_8("volume");
  fb.reserved(16);
  fb.matrix("matrix");
  if (fb.node().fields.back()->svalue == "custom") {
    fb.diag(Severity::Info, "CNT-1024", "tkhd.matrix is neither identity nor a 90°/180°/270° rotation");
  }
  fb.fixed16_16("width", false);
  fb.fixed16_16("height", false);
  check_fixed_size_end(fb);
  return 0;
}

// ---- tref children (14496-12 §8.3.3), trgr children (§8.3.4) ---------------------------------------------------

uint64_t decode_track_reference(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.3.3");
  const uint64_t count = br.bits_left() / 32U;
  fb.array("track_IDs", count);
  const auto c32 = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
  for (uint64_t i = 0; i < count; ++i) {
    const auto idx = static_cast<uint32_t>(std::min<uint64_t>(i, UINT32_MAX));
    if (FieldBuilder::materialise(idx, c32)) {
      fb.u32("track_IDs[" + std::to_string(i) + "]");
    } else {
      br.skip_bytes(4);
    }
  }
  fb.end();
  if (const std::string_view d = name_of(kReferenceNames, fb.node().fourcc); !d.empty()) {
    fb.derived(d);
  }
  check_trailing(fb);
  return 0;
}

uint64_t decode_track_group(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.3.4");
  fb.u32("track_group_id");
  return 0;  // type-specific data may follow; it is not decoded
}

// ---- elst (14496-12 §8.6.6) ----------------------------------------------------------------------------------

uint64_t decode_elst(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.6.6");
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool wide = version_of(fb) == 1;
  const uint32_t entry_bytes = wide ? 20U : 12U;
  const uint32_t count = fb.u32("entry_count");
  uint32_t readable = count;
  if (const uint64_t fit = br.bits_left() / 8U / entry_bytes; fit < count) {
    fb.diag(Severity::Error, "CNT-1014",
            "elst.entry_count " + std::to_string(count) + " needs " + std::to_string(uint64_t{count} * entry_bytes) +
                " bytes; " + std::to_string(fit) + " entries fit in the box");
    readable = static_cast<uint32_t>(fit);  // fit < count <= UINT32_MAX
  }
  fb.array("entries", readable);  // the entries present; entry_count keeps the declared number
  for (uint32_t i = 0; i < readable; ++i) {
    if (!FieldBuilder::materialise(i, readable)) {
      br.skip_bytes(entry_bytes);
      continue;
    }
    fb.array_element("entry", i);
    fb.duration("segment_duration", wide, ctx.movie_timescale);  // movie timescale
    const int64_t media_time = fb.i32or64("media_time", wide);
    fb.derived(media_time_text(media_time, ctx.track_timescale));  // track timescale
    if (fb.i16("media_rate_integer") == 0) {
      fb.derived("dwell");
    }
    fb.i16("media_rate_fraction");
    fb.end();
  }
  fb.end();
  if (readable == count) {
    check_trailing(fb);
  }
  return 0;
}

// ---- mdhd (14496-12 §8.4.2) ----------------------------------------------------------------------------------

uint64_t decode_mdhd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.4.2");
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool wide = version_of(fb) == 1;
  fb.datetime1904("creation_time", wide);
  fb.datetime1904("modification_time", wide);
  const uint32_t timescale = fb.u32("timescale");
  check_timescale(fb, timescale);
  ctx.track_timescale = timescale;
  if (ctx.trex_for_track && ctx.track_id != 0) {  // SPEC-ASSUMPTION(#670): tfdt derives seconds from it
    (*ctx.trex_for_track)[ctx.track_id].timescale = timescale;
  }
  fb.duration("duration", wide, timescale);
  fb.lang_packed("language");  // pad u(1) + language u(5)[3]; 0x7FFF and invalid codes read "und"
  fb.u16("pre_defined");
  check_fixed_size_end(fb);
  return 0;
}

// ---- hdlr (14496-12 §8.4.3) ----------------------------------------------------------------------------------

uint64_t decode_hdlr(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.4.3");
  fb.u32("pre_defined");
  const Fourcc handler = fb.fourcc("handler_type");
  if (const std::string_view d = handler_text(sv(handler)); !d.empty()) {
    derived_sibling(fb, "handler_type", d);
  }
  if (ctx.parent_is("mdia")) {
    ctx.handler_type = handler;  // a `meta` handler does not describe the track
  }
  fb.reserved(96);
  const uint64_t left = br.bits_left() / 8U;
  // SPEC-ASSUMPTION(#567): QuickTime writes a counted (Pascal) string, recorded as `name_length` then `name`.
  if (ctx.quicktime && left > 0 && br.remaining_bytes()[0] == left - 1) {
    const uint8_t len = fb.u8("name_length");
    fb.str_fixed("name", len);
  } else {
    fb.str_nul("name", left);
    check_trailing(fb);
  }
  return 0;
}

// ---- elng (14496-12 §8.4.6) ----------------------------------------------------------------------------------

uint64_t decode_elng(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.4.6");
  fb.str_nul("extended_language", br.bits_left() / 8U);
  check_trailing(fb);
  return 0;
}

// ---- media information headers (14496-12 §12.1.2, §12.2.2, §12.4.2, §8.4.5.2, §12.6.2) -------------------------

std::string_view graphicsmode_name(uint16_t mode) {
  switch (mode) {
    case 0x0000:
      return "copy";
    case 0x0020:
      return "blend";
    case 0x0024:
      return "transparent";
    case 0x0040:
      return "dither copy";
    case 0x0100:
      return "straight alpha";
    case 0x0101:
      return "premul white alpha";
    case 0x0102:
      return "premul black alpha";
    case 0x0103:
      return "composition (dither copy)";
    case 0x0104:
      return "straight alpha blend";
    default:
      return {};
  }
}

uint64_t decode_vmhd(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §12.1.2");
  if (const std::string_view n = graphicsmode_name(fb.u16("graphicsmode")); !n.empty()) {
    fb.derived(n);
  }
  fb.array("opcolor", 3);
  for (unsigned i = 0; i < 3; ++i) {
    fb.u16("opcolor[" + std::to_string(i) + "]");
  }
  fb.end();
  check_fixed_size_end(fb);
  return 0;
}

uint64_t decode_smhd(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §12.2.2");
  fb.fixed8_8("balance");
  fb.reserved(16);
  check_fixed_size_end(fb);
  return 0;
}

uint64_t decode_hmhd(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §12.4.2");
  fb.u16("maxPDUsize");
  fb.u16("avgPDUsize");
  fb.u32("maxbitrate");
  fb.u32("avgbitrate");
  fb.reserved(32);
  check_trailing(fb);
  return 0;
}

uint64_t decode_empty_header(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  check_trailing(fb);  // nmhd / sthd: FullBox with no fields
  return 0;
}

// ---- dref, url , urn  (14496-12 §8.7.2) --------------------------------------------------------------------------

uint64_t decode_dref(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.7.2");
  fb.u32("entry_count");
  return fb.bytes_consumed();  // ContainerWithPrefix: the entries are child boxes
}

constexpr auto kUrlFlags = std::to_array<FlagName>({{0x1, "self-contained"}});

uint64_t decode_url(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.7.2");
  name_flags(fb, kUrlFlags);
  if ((flags_of(fb) & 1U) == 0) {
    fb.str_nul("location", br.bits_left() / 8U);
  }
  check_trailing(fb);
  return 0;
}

uint64_t decode_urn(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.7.2");
  fb.str_nul("name", br.bits_left() / 8U);
  if (br.bits_left() >= 8) {
    fb.str_nul("location", br.bits_left() / 8U);
  }
  check_trailing(fb);
  return 0;
}

// ---- pdin (14496-12 §8.1.3) ----------------------------------------------------------------------------------

uint64_t decode_pdin(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-12:2022 §8.1.3");
  const uint64_t count = br.bits_left() / 64U;
  const auto c32 = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
  fb.array("entries", count);
  for (uint64_t i = 0; i < count; ++i) {
    const auto idx = static_cast<uint32_t>(std::min<uint64_t>(i, UINT32_MAX));
    if (!FieldBuilder::materialise(idx, c32)) {
      br.skip_bytes(8);
      continue;
    }
    fb.array_element("entry", idx);
    fb.u32("rate");
    fb.u32("initial_delay");
    fb.end();
  }
  fb.end();
  check_trailing(fb);
  return 0;
}

}  // namespace

// One registration per §8.3.1 row with fields. Containers and opaque boxes have no decoder.
OVT_BOX_DECODER("ftyp", "*", &decode_ftyp)
OVT_BOX_DECODER("styp", "*", &decode_styp)
OVT_BOX_DECODER("mvhd", "*", &decode_mvhd)
OVT_BOX_DECODER("tkhd", "*", &decode_tkhd)
OVT_BOX_DECODER("tref/*", "*/tref", &decode_track_reference)
OVT_BOX_DECODER("trgr/*", "*/trgr", &decode_track_group)
OVT_BOX_DECODER("elst", "*", &decode_elst)
OVT_BOX_DECODER("mdhd", "*", &decode_mdhd)
OVT_BOX_DECODER("hdlr", "*", &decode_hdlr)
OVT_BOX_DECODER("elng", "*", &decode_elng)
OVT_BOX_DECODER("vmhd", "*", &decode_vmhd)
OVT_BOX_DECODER("smhd", "*", &decode_smhd)
OVT_BOX_DECODER("hmhd", "*", &decode_hmhd)
OVT_BOX_DECODER("nmhd", "*", &decode_empty_header)
OVT_BOX_DECODER("sthd", "*", &decode_empty_header)
OVT_BOX_DECODER("dref", "*", &decode_dref)
OVT_BOX_DECODER("url ", "*", &decode_url)
OVT_BOX_DECODER("urn ", "*", &decode_urn)
OVT_BOX_DECODER("pdin", "*", &decode_pdin)

}  // namespace ovt::container::isobmff
