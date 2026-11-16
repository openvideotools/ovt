// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group B: `stsd` and the sample entries (chapter 08 §8.3.2, WI-1.206). `stsd` is a
// ContainerWithPrefix (a FullBox with `entry_count`, then the entries as child boxes). A sample entry is a Box, not a
// FullBox: an eight byte SampleEntry prefix, the fields of its kind (visual, audio, text, metadata, timecode), then
// child boxes, so every entry decoder is a ContainerWithPrefix decoder too and returns the offset of its first child.
//
// Contract (§8.2.9): the walker has already read FullBox version/flags into `node.version` / `node.flags` for the
// boxes that have them, so `br` starts at the first field after them, and the returned children offset counts from
// there. Which entry layout applies depends on the track's handler, so every entry of `stsd` goes through one
// `stsd/*` dispatcher that reads `ctx.handler_type`; the entries whose layout belongs to the fourcc (`tx3g`, `wvtt`,
// `stpp`, `mett`, `metx`, `urim`, `tmcd`) have rows of their own. Derived text goes into `Field.svalue`
// (SPEC-ASSUMPTION(#415), field_builder.h).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

struct Name {
  std::string_view code;
  std::string_view text;
};

template <size_t N>
std::string_view name_of(const std::array<Name, N>& table, std::string_view code) {
  const auto it = std::ranges::find(table, code, &Name::code);
  return it == table.end() ? std::string_view{} : it->text;
}

// ---- codec names ---------------------------------------------------------------------------------------------
// The "codec from fourcc" derivation of §8.3.2. The table also decides the layout of an entry whose track has no
// usable `hdlr` (a damaged file), so it lists every fourcc of the visual and audio rows.
// clang-format off
constexpr auto kVisualCodecs = std::to_array<Name>({
    {"avc1", "H.264"}, {"avc2", "H.264"}, {"avc3", "H.264"},
    {"avc4", "H.264"}, {"dvav", "H.264 (Dolby Vision)"}, {"dva1", "H.264 (Dolby Vision)"},
    {"hev1", "H.265"}, {"hvc1", "H.265"}, {"hev2", "H.265"},
    {"hvc2", "H.265"}, {"hvc3", "H.265"}, {"dvhe", "H.265 (Dolby Vision)"},
    {"dvh1", "H.265 (Dolby Vision)"}, {"av01", "AV1"}, {"dav1", "AV1 (Dolby Vision)"},
    {"vp08", "VP8"}, {"vp09", "VP9"}, {"vvc1", "H.266"},
    {"vvi1", "H.266"}, {"mp4v", "MPEG-4 Visual"}, {"s263", "H.263"},
    {"apch", "Apple ProRes"}, {"apcn", "Apple ProRes"}, {"apcs", "Apple ProRes"}, {"apco", "Apple ProRes"},
    {"ap4h", "Apple ProRes"}, {"ap4x", "Apple ProRes"},
    {"encv", "encrypted video"}, {"resv", "restricted video"},
});

constexpr auto kAudioCodecs = std::to_array<Name>({
    {"mp4a", "MPEG-4 Audio"}, {"Opus", "Opus"}, {"fLaC", "FLAC"},
    {"ac-3", "AC-3"}, {"ec-3", "E-AC-3"}, {"ac-4", "AC-4"},
    {"dtsc", "DTS"}, {"dtsh", "DTS-HD"}, {"dtsl", "DTS-HD Lossless"},
    {"dtse", "DTS Express"}, {"samr", "AMR-NB"}, {"sawb", "AMR-WB"},
    {"alac", "Apple Lossless"}, {"twos", "PCM (big-endian)"}, {"sowt", "PCM (little-endian)"},
    {"lpcm", "PCM"}, {"in24", "PCM 24-bit"}, {"in32", "PCM 32-bit"},
    {"fl32", "PCM float 32-bit"}, {"fl64", "PCM float 64-bit"}, {"ulaw", "G.711 mu-law"},
    {"alaw", "G.711 A-law"}, {"enca", "encrypted audio"},
});

constexpr auto kOtherEntries = std::to_array<Name>({
    {"tx3g", "3GPP timed text"}, {"wvtt", "WebVTT"}, {"stpp", "XML subtitles (TTML/IMSC)"},
    {"mett", "text metadata"}, {"metx", "XML metadata"}, {"urim", "URI metadata"},
    {"tmcd", "QuickTime timecode"}, {"mp4s", "MPEG-4 systems stream"}, {"rtp ", "RTP hint"},
});
// clang-format on

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

constexpr std::string_view kRefStsd = "ISO/IEC 14496-12:2022 §8.5.2";
constexpr std::string_view kRefEntry = "ISO/IEC 14496-12:2022 §8.5.2.2";
constexpr std::string_view kRefVisual = "ISO/IEC 14496-12:2022 §12.1.3";
constexpr std::string_view kRefAudio = "ISO/IEC 14496-12:2022 §12.2.3";
constexpr std::string_view kRefAudioQt = "QuickTime File Format, Sound Sample Description";
constexpr std::string_view kRefMetadata = "ISO/IEC 14496-12:2022 §12.3.3";

uint8_t version_of(FieldBuilder& fb) {
  const int v = fb.node().version;
  return v < 0 ? 0 : static_cast<uint8_t>(v);
}

// CNT-1005: bytes after the last field of a box whose layout has no trailing data.
void check_trailing(FieldBuilder& fb) {
  if (const uint64_t extra = fb.reader().bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in " + fb.node().fourcc);
  }
}

// SPEC-ASSUMPTION(#567): derived text for a field the box has no source field for, §8.3 notation: a String sibling
// `<name>.derived` with an empty span right after the previous field (same convention as decoders/movie.cpp).
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = static_cast<uint64_t>(fb.node().header_length) * 8U;
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

// "29.97", "48000", "23.976": at most `places` decimals, trailing zeros dropped.
std::string number_text(double v, unsigned places) {
  std::string s = std::to_string(v);
  const size_t dot = s.find('.');
  if (dot == std::string::npos) {
    return s;
  }
  s.resize(std::min(s.size(), dot + 1U + places));
  while (!s.empty() && s.back() == '0') {
    s.pop_back();
  }
  if (!s.empty() && s.back() == '.') {
    s.pop_back();
  }
  return s;
}

struct FlagName {
  uint32_t bit;
  std::string_view name;
};

template <size_t N>
std::string flag_text(uint32_t flags, const std::array<FlagName, N>& names) {
  std::string text;
  for (const FlagName& n : names) {
    if ((flags & n.bit) != 0) {
      text += (text.empty() ? "" : "|") + std::string(n.name);
    }
  }
  return text;
}

// A `pre_defined` field that should hold `expected`. Encoders write other values there (and QuickTime files use the
// same bytes for version, vendor and quality), so a mismatch is CNT-1007 Info and never a failure; QuickTime files
// are not checked because for them the bytes are defined fields.
void check_pre_defined(const BoxContext& ctx, FieldBuilder& fb, uint64_t value, uint64_t expected) {
  if (value != expected && !ctx.quicktime) {
    fb.diag(
        Severity::Info, "CNT-1007",
        "pre_defined has unexpected value " + std::to_string(value) + " (expected " + std::to_string(expected) + ")");
  }
}

// A reserved run, quiet in QuickTime files where the same bytes are version/vendor/revision fields.
void reserved_run(const BoxContext& ctx, FieldBuilder& fb, unsigned nbits) {
  if (ctx.quicktime) {
    fb.u("reserved", nbits);
  } else {
    fb.reserved(nbits);
  }
}

// ---- stsd (14496-12 §8.5.2) ----------------------------------------------------------------------------------

uint64_t decode_stsd(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefStsd);
  const uint8_t version = version_of(fb);
  if (version > 1) {
    // §8.2.9 step 3: a reader MUST ignore an unknown `stsd` version. The entries stay undecoded, so no child offset.
    fb.diag(Severity::Warning, "CNT-1003", "unknown stsd version " + std::to_string(version) + "; entries not decoded");
    return br.bits_left() / 8U;
  }
  ctx.stsd_version = version;
  fb.u32("entry_count");
  return fb.bytes_consumed();
}

// ---- the SampleEntry prefix ----------------------------------------------------------------------------------

std::string_view codec_of(std::string_view fourcc) {
  if (const std::string_view d = name_of(kVisualCodecs, fourcc); !d.empty()) {
    return d;
  }
  if (const std::string_view d = name_of(kAudioCodecs, fourcc); !d.empty()) {
    return d;
  }
  return name_of(kOtherEntries, fourcc);
}

// reserved u8[6], data_reference_index u16; sets `ctx.sample_entry` and the codec text.
void sample_entry_prefix(BoxContext& ctx, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefEntry);
  const std::string& type = fb.node().fourcc;
  ctx.sample_entry = {};
  if (type.size() == 4) {
    std::ranges::copy(type, ctx.sample_entry.begin());
  }
  fb.reserved(48);
  fb.u16("data_reference_index");
  std::string codec(codec_of(type));
  if (type.starts_with("enc") && codec.empty()) {
    codec = "encrypted";
  }
  if (type.starts_with("enc") || type == "resv") {
    codec += (codec.empty() ? "" : "; ") + std::string("original format in sinf/frma");
  }
  if (!codec.empty()) {
    derived_sibling(fb, "codec", codec);
  }
}

// ---- VisualSampleEntry (14496-12 §12.1.3) --------------------------------------------------------------------

std::string_view depth_text(uint16_t depth) {
  switch (depth) {
    case 0x18:
      return "24 colour";
    case 0x20:
      return "32 colour+alpha";
    case 0x28:
      return "40 greyscale";
    default:
      return {};
  }
}

// `compressorname`: a counted string in a field of 32 bytes, always.
void compressorname(FieldBuilder& fb) {
  const std::span<const uint8_t> raw = fb.bytes("compressorname", 32);
  const size_t declared = raw[0];
  if (declared > 31) {
    fb.diag(Severity::Info, "CNT-1007", "compressorname length " + std::to_string(declared) + " exceeds 31");
  }
  const size_t len = std::min<size_t>(declared, 31);
  std::string text;
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(raw[1 + i]);
    text += (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  // The field is a String as the goldens print it (mp4box.js); its bytes stay in `bytes`.
  if (!fb.node().fields.empty() && fb.node().fields.back()) {
    fb.node().fields.back()->kind = fb::SyntaxValueKind::String;
    fb.node().fields.back()->svalue = text;
  }
}

uint64_t decode_visual_fields(BoxContext& ctx, FieldBuilder& fb) {
  {
    const SpecRefScope scope(fb, kRefVisual);
    check_pre_defined(ctx, fb, fb.u16("pre_defined"), 0);
    fb.reserved(16);
    fb.array("pre_defined_2", 3);
    uint64_t any = 0;
    for (unsigned i = 0; i < 3; ++i) {
      any |= fb.u32("pre_defined_2[" + std::to_string(i) + "]");
    }
    fb.end();
    check_pre_defined(ctx, fb, any, 0);
    fb.u16("width");
    fb.u16("height");
    fb.fixed16_16("horizresolution", false);
    fb.fixed16_16("vertresolution", false);
    reserved_run(ctx, fb, 32);
    fb.u16("frame_count");
    compressorname(fb);
    const uint16_t depth = fb.u16("depth");
    if (const std::string_view d = depth_text(depth); !d.empty()) {
      fb.derived(d);
    }
    const int16_t pre = fb.i16("pre_defined_3");
    if (pre != -1 && !ctx.quicktime) {
      fb.diag(Severity::Info, "CNT-1007", "pre_defined has unexpected value " + std::to_string(pre) + " (expected -1)");
    }
  }
  return fb.bytes_consumed();
}

// ---- AudioSampleEntry (14496-12 §12.2.3, QuickTime "Sound Sample Description") -------------------------------

std::string channel_text(uint64_t channels) {
  switch (channels) {
    case 1:
      return "mono";
    case 2:
      return "stereo";
    default:
      return std::to_string(channels) + " channels";
  }
}

constexpr size_t kQtV1Fixed = 36;  // bytes after the SampleEntry prefix, before the child boxes
constexpr size_t kQtV2Fixed = 56;

// Does a `wave` box appear among the child boxes that follow `fixed` bytes of the QuickTime layout? Part of R-AUDIO:
// a version 1 or 2 layout in a file the context does not call QuickTime is still QuickTime when it has a `wave`.
bool has_wave_child(std::span<const uint8_t> rest, size_t fixed) {
  size_t pos = fixed;
  for (unsigned boxes = 0; boxes < 64 && pos + 8 <= rest.size(); ++boxes) {
    const uint64_t size = (uint64_t{rest[pos]} << 24U) | (uint64_t{rest[pos + 1]} << 16U) |
                          (uint64_t{rest[pos + 2]} << 8U) | uint64_t{rest[pos + 3]};
    if (rest[pos + 4] == 'w' && rest[pos + 5] == 'a' && rest[pos + 6] == 'v' && rest[pos + 7] == 'e') {
      return true;
    }
    if (size < 8) {
      return false;  // largesize, to-end-of-file or garbage: stop looking
    }
    pos += static_cast<size_t>(size);
  }
  return false;
}

// channelcount .. samplerate, shared by the ISO v0 and V1 layouts.
void iso_audio_tail(const BoxContext& ctx, FieldBuilder& fb) {
  const uint16_t channels = fb.u16("channelcount");
  fb.derived(channel_text(channels));
  fb.u16("samplesize");
  check_pre_defined(ctx, fb, fb.u16("pre_defined"), 0);
  reserved_run(ctx, fb, 16);
  const double rate = fb.fixed16_16("samplerate", false);
  fb.derived(number_text(rate, 3) + " Hz");
}

uint64_t decode_audio_fields(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  // R-AUDIO (§8.3.2): peek the u16 right after the SampleEntry prefix.
  const std::span<const uint8_t> rest = br.remaining_bytes();
  const unsigned peek = rest.size() >= 2 ? (unsigned{rest[0]} << 8U) | rest[1] : 0U;
  const bool iso_v1 = ctx.stsd_version == 1;
  const bool qt_layout = !iso_v1 && (peek == 1 || peek == 2) &&
                         (ctx.quicktime || has_wave_child(rest, peek == 1 ? kQtV1Fixed : kQtV2Fixed));
  const SpecRefScope scope(fb, qt_layout ? kRefAudioQt : kRefAudio);
  if (iso_v1) {
    const uint16_t entry_version = fb.u16("entry_version");
    if (entry_version != 1) {
      fb.diag(Severity::Info, "CNT-1007", "entry_version " + std::to_string(entry_version) + " in an stsd version 1");
    }
    fb.reserved(48);
    iso_audio_tail(ctx, fb);
  } else if (qt_layout) {
    fb.u16("version");
    fb.u16("revision_level");
    fb.fourcc("vendor");
    derived_sibling(fb, "layout",
                    peek == 1 ? "QuickTime sound description version 1" : "QuickTime sound description version 2");
    if (peek == 1) {
      const uint16_t channels = fb.u16("channels");
      fb.derived(channel_text(channels));
      fb.u16("sample_size");
      fb.i16("compression_id");
      fb.u16("packet_size");
      const double rate = fb.fixed16_16("sample_rate", false);
      fb.derived(number_text(rate, 3) + " Hz");
      fb.u32("samples_per_packet");
      fb.u32("bytes_per_packet");
      fb.u32("bytes_per_frame");
      fb.u32("bytes_per_sample");
    } else {
      fb.u16("always3");
      fb.u16("always16");
      fb.i16("alwaysMinus2");
      fb.u16("always0");
      fb.u32("always65536");
      fb.u32("sizeOfStructOnly");
      const uint64_t bits = fb.u64("audioSampleRate");
      fb.derived(number_text(std::bit_cast<double>(bits), 3) + " Hz");
      const uint32_t channels = fb.u32("numAudioChannels");
      fb.derived(channel_text(channels));
      fb.u32("always7F000000");
      fb.u32("constBitsPerChannel");
      fb.u32("formatSpecificFlags");
      fb.u32("constBytesPerAudioPacket");
      fb.u32("constLPCMFramesPerAudioPacket");
    }
  } else {
    reserved_run(ctx, fb, 64);
    iso_audio_tail(ctx, fb);
  }
  return fb.bytes_consumed();
}

// ---- the stsd/* dispatcher -----------------------------------------------------------------------------------

enum class Layout : uint8_t { Visual, Audio, Plain };

// The handler decides (§8.3.2: "decoded by ctx.handler_type"); a track without a usable handler falls back on the
// codec tables, and every other entry is the SampleEntry prefix alone (`mp4s`, `rtp `, the entries of `hint` tracks).
Layout layout_for(const BoxContext& ctx, std::string_view fourcc) {
  const std::string_view handler(ctx.handler_type.data(), ctx.handler_type.size());
  if (handler == "vide") {
    return Layout::Visual;
  }
  if (handler == "soun") {
    return Layout::Audio;
  }
  if (handler.front() == '\0') {
    if (!name_of(kVisualCodecs, fourcc).empty()) {
      return Layout::Visual;
    }
    if (!name_of(kAudioCodecs, fourcc).empty()) {
      return Layout::Audio;
    }
  }
  return Layout::Plain;
}

uint64_t decode_sample_entry(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  switch (layout_for(ctx, fb.node().fourcc)) {
    case Layout::Visual:
      return decode_visual_fields(ctx, fb);
    case Layout::Audio:
      return decode_audio_fields(ctx, br, fb);
    case Layout::Plain:
      break;
  }
  return fb.bytes_consumed();
}

// ---- tx3g and ftab (3GPP TS 26.245 §5.16, §5.17) -------------------------------------------------------------

// SPEC-ASSUMPTION(#592): the display flag names and bit values are those of 3GPP TS 26.245 §5.16 as the
// published tools use them; the scroll direction (mask 0x180) is reported as a number.
constexpr auto kTx3gFlags = std::to_array<FlagName>({
    {0x00000020, "scroll_in"},
    {0x00000040, "scroll_out"},
    {0x00000800, "continuous_karaoke"},
    {0x00020000, "write_text_vertically"},
    {0x00040000, "fill_text_region"},
});

void rgba(FieldBuilder& fb, std::string_view name) {
  fb.array(name, 4);
  for (const char* c : {"r", "g", "b", "a"}) {
    fb.u8(c);
  }
  fb.end();
}

uint64_t decode_tx3g(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  const SpecRefScope scope(fb, "3GPP TS 26.245 §5.16");
  const uint32_t flags = fb.u32("displayFlags");
  std::string text = flag_text(flags, kTx3gFlags);
  if ((flags & 0x180U) != 0) {
    text += (text.empty() ? "" : "|") + std::string("scroll_direction=") + std::to_string((flags >> 7U) & 3U);
  }
  fb.derived(text);
  fb.u("horizontal_justification", 8);
  fb.u("vertical_justification", 8);
  rgba(fb, "background_color_rgba");
  fb.group("BoxRecord");
  fb.i16("top");
  fb.i16("left");
  fb.i16("bottom");
  fb.i16("right");
  fb.end();
  fb.group("StyleRecord");
  fb.u16("startChar");
  fb.u16("endChar");
  fb.u16("font_ID");
  fb.u8("face_style_flags");
  fb.u8("font_size");
  rgba(fb, "text_color_rgba");
  fb.end();
  return fb.bytes_consumed();
}

uint64_t decode_ftab(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "3GPP TS 26.245 §5.16");
  const uint16_t count = fb.u16("entry_count");
  fb.array("entries", count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!FieldBuilder::materialise(i, count)) {
      br.skip_bytes(2);
      br.skip_bytes(br.u(8));
      continue;
    }
    fb.array_element("entry", i);
    fb.u16("font_ID");
    const uint8_t len = fb.u8("font_name_length");
    fb.str_fixed("font", len);
    fb.end();
  }
  fb.end();
  check_trailing(fb);
  return 0;
}

// ---- wvtt, vttC, vlab (14496-30 §7.5) ------------------------------------------------------------------------

uint64_t decode_wvtt(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  return fb.bytes_consumed();
}

uint64_t decode_vttc(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-30 §7.5");
  fb.str_fixed("config", static_cast<uint32_t>(br.bits_left() / 8U));
  return 0;
}

uint64_t decode_vlab(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "ISO/IEC 14496-30 §7.5");
  fb.str_fixed("source_label", static_cast<uint32_t>(br.bits_left() / 8U));
  return 0;
}

// ---- stpp (14496-30 §6.3), mett, metx, urim (14496-12 §12.3.3) -----------------------------------------------

uint64_t decode_stpp(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  const SpecRefScope scope(fb, "ISO/IEC 14496-30 §6.3");
  fb.str_nul("namespace", br.bits_left() / 8U);
  fb.str_nul("schema_location", br.bits_left() / 8U);
  fb.str_nul("auxiliary_mime_types", br.bits_left() / 8U);
  return fb.bytes_consumed();
}

uint64_t decode_mett(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  const SpecRefScope scope(fb, kRefMetadata);
  fb.str_nul("content_encoding", br.bits_left() / 8U);
  fb.str_nul("mime_format", br.bits_left() / 8U);
  return fb.bytes_consumed();
}

uint64_t decode_metx(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  const SpecRefScope scope(fb, kRefMetadata);
  fb.str_nul("content_encoding", br.bits_left() / 8U);
  fb.str_nul("namespace", br.bits_left() / 8U);
  fb.str_nul("schema_location", br.bits_left() / 8U);
  return fb.bytes_consumed();
}

uint64_t decode_urim(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  return fb.bytes_consumed();
}

uint64_t decode_txtc(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefMetadata);
  fb.str_nul("text_config", br.bits_left() / 8U);
  return 0;
}

uint64_t decode_uri(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefMetadata);
  fb.str_nul("theURI", br.bits_left() / 8U);
  return 0;
}

uint64_t decode_urii(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefMetadata);
  fb.bytes("uri_initialization_data", br.bits_left() / 8U);
  return 0;
}

// ---- tmcd and its `name` (QuickTime File Format, "Timecode Sample Description") ------------------------------

constexpr auto kTmcdFlags = std::to_array<FlagName>({
    {0x1, "drop_frame"},
    {0x2, "24_hour_max"},
    {0x4, "negative_times_ok"},
    {0x8, "counter"},
});

uint64_t decode_tmcd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  sample_entry_prefix(ctx, fb);
  const SpecRefScope scope(fb, "QuickTime File Format, Timecode Sample Description");
  reserved_run(ctx, fb, 32);
  const uint32_t flags = fb.u32("flags");
  fb.derived(flag_text(flags, kTmcdFlags));
  const uint32_t scale = fb.u32("time_scale");
  const uint32_t duration = fb.u32("frame_duration");
  if (duration != 0) {
    derived_sibling(fb, "frame_rate",
                    number_text(static_cast<double>(scale) / static_cast<double>(duration), 3) + " fps");
  }
  fb.u8("number_of_frames");
  reserved_run(ctx, fb, 8);
  return fb.bytes_consumed();
}

uint64_t decode_tmcd_name(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "QuickTime File Format, Timecode Sample Description");
  const uint16_t size = fb.u16("text_size");
  fb.u16("language");
  fb.str_fixed("text", size);
  check_trailing(fb);
  return 0;
}

}  // namespace

// One registration per §8.3.2 row with fields.
OVT_BOX_DECODER("stsd", "*", &decode_stsd)
OVT_BOX_DECODER("stsd/*", "*/stsd", &decode_sample_entry)
OVT_BOX_DECODER("tx3g", "*/stsd", &decode_tx3g)
OVT_BOX_DECODER("ftab", "*/stsd/tx3g", &decode_ftab)
OVT_BOX_DECODER("wvtt", "*/stsd", &decode_wvtt)
OVT_BOX_DECODER("vttC", "*/stsd/wvtt", &decode_vttc)
OVT_BOX_DECODER("vlab", "*/stsd/wvtt", &decode_vlab)
OVT_BOX_DECODER("stpp", "*/stsd", &decode_stpp)
OVT_BOX_DECODER("mett", "*/stsd", &decode_mett)
OVT_BOX_DECODER("txtC", "*/stsd/mett", &decode_txtc)
OVT_BOX_DECODER("metx", "*/stsd", &decode_metx)
OVT_BOX_DECODER("urim", "*/stsd", &decode_urim)
OVT_BOX_DECODER("uri ", "*/stsd/urim", &decode_uri)
OVT_BOX_DECODER("uriI", "*/stsd/urim", &decode_urii)
OVT_BOX_DECODER("tmcd", "*/stsd", &decode_tmcd)
OVT_BOX_DECODER("name", "*/stsd/tmcd", &decode_tmcd_name)

}  // namespace ovt::container::isobmff
