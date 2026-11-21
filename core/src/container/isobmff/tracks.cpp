// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// TrackInfo from the decoded `moov` (chapter 08 §8.1.3, WI-1.217). Every value comes from the BoxNode tree the box
// walk and the hand decoders built; the few payloads no decoder covers (`frma`, `colr` for the AV1 codec string) are
// read straight from the file.
//
// SPEC-ASSUMPTION(#768): (1) `sample_count` counts the `moov` tables only; the `trun` part is added by WI-1.219.
// (2) Without a `btrt`, `bitrate_avg_bps` is derived only when `stsz` has one common sample size, and
// `bitrate_max_bps` stays 0 until the SampleTable exists (WI-1.218). (3) A `dvcC`/`dvvC` entry gets the Dolby Vision
// codec string (`dvh1.08.06`) instead of the HEVC form.

#include "container/isobmff/tracks.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include "container/diagnostics.h"
#include "container/isobmff/decoders/codec_config.h"

namespace ovt::container::isobmff {

namespace {

using Bytes = std::vector<uint8_t>;
using Node = fb::BoxNodeT;

const Node* child(const Node& n, std::string_view fourcc) {
  for (const auto& c : n.children) {
    if (c && c->fourcc == fourcc) {
      return c.get();
    }
  }
  return nullptr;
}

const fb::FieldT* field(const Node& n, std::string_view name) {
  for (const auto& f : n.fields) {
    if (f && f->name == name) {
      return f.get();
    }
  }
  return nullptr;
}

uint64_t uval(const Node& n, std::string_view name, uint64_t dflt = 0) {
  const fb::FieldT* f = field(n, name);
  return f != nullptr ? f->uvalue : dflt;
}

std::string sval(const Node& n, std::string_view name) {
  const fb::FieldT* f = field(n, name);
  return f != nullptr ? f->svalue : std::string{};
}

// A fourcc field's four characters (`uvalue` keeps the raw big-endian value).
std::string fourcc_text(uint32_t v) {
  std::string s(4, ' ');
  for (size_t i = 0; i < 4; ++i) {
    s[i] = static_cast<char>((v >> (24U - 8U * i)) & 0xFFU);
  }
  return s;
}

// Up to `max` bytes of the payload of `n` (after its header), or fewer when the box or file is shorter.
Bytes payload_bytes(const io::RandomAccessFile& file, const Node& n, size_t max) {
  if (!n.file_span) {
    return {};
  }
  const uint64_t begin = n.file_span->offset() + n.header_length;
  const uint64_t end = n.file_span->offset() + n.file_span->length();
  if (end <= begin) {
    return {};
  }
  Bytes b(static_cast<size_t>(std::min<uint64_t>(max, end - begin)));
  b.resize(file.read(begin, b.size(), b.data()));
  return b;
}

uint32_t be32(const Bytes& b, size_t at) {
  return (uint32_t{b[at]} << 24U) | (uint32_t{b[at + 1]} << 16U) | (uint32_t{b[at + 2]} << 8U) | b[at + 3];
}

std::string hex_upper(uint64_t v) {
  return fmt::format("{:X}", v);
}

std::string dec2(unsigned v) {
  return fmt::format("{:02}", v);
}

const Bytes* config_bytes(const std::vector<std::unique_ptr<fb::CodecPrivateT>>& cp, std::string_view fourcc) {
  for (const auto& c : cp) {
    if (c && c->fourcc == fourcc) {
      return &c->bytes;
    }
  }
  return nullptr;
}

// ---- esds (14496-1 §7.2.6.5, §7.2.6.6) ---------------------------------------------------------------------------

struct EsInfo {
  std::optional<uint8_t> oti;
  std::optional<unsigned> aot;  // AudioSpecificConfig audioObjectType
  std::optional<double> sample_rate;
  std::optional<unsigned> channels;
};

constexpr std::array<unsigned, 13> kAacRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                22050, 16000, 12000, 11025, 8000,  7350};

// Reads one descriptor header at `pos`: the tag and the size (up to four 7-bit groups).
bool read_descriptor(const Bytes& b, size_t& pos, uint8_t& tag, size_t& size) {
  if (pos >= b.size()) {
    return false;
  }
  tag = b[pos++];
  size = 0;
  for (int i = 0; i < 4; ++i) {
    if (pos >= b.size()) {
      return false;
    }
    const uint8_t c = b[pos++];
    size = (size << 7U) | (c & 0x7FU);
    if ((c & 0x80U) == 0) {
      return true;
    }
  }
  return true;
}

void parse_asc(std::span<const uint8_t> asc, EsInfo& out) {
  size_t bit = 0;
  const auto get = [&](unsigned n) -> std::optional<uint32_t> {
    if (bit + n > asc.size() * 8U) {
      return std::nullopt;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i, ++bit) {
      v = (v << 1U) | ((asc[bit / 8U] >> (7U - bit % 8U)) & 1U);
    }
    return v;
  };
  auto aot = get(5);
  if (!aot) {
    return;
  }
  if (*aot == 31) {
    const auto ext = get(6);
    if (!ext) {
      return;
    }
    aot = 32 + *ext;
  }
  out.aot = *aot;
  const auto idx = get(4);
  if (!idx) {
    return;
  }
  if (*idx == 15) {
    if (const auto r = get(24)) {
      out.sample_rate = *r;
    }
  } else if (*idx < kAacRates.size()) {
    out.sample_rate = kAacRates.at(*idx);
  }
  if (const auto ch = get(4)) {
    out.channels = *ch;
  }
}

EsInfo parse_esds(const Bytes& b) {
  EsInfo info;
  size_t pos = 4;  // version and flags
  uint8_t tag = 0;
  size_t size = 0;
  if (!read_descriptor(b, pos, tag, size) || tag != 0x03 || pos + 3 > b.size()) {
    return info;
  }
  const uint8_t flags = b[pos + 2];
  pos += 3;
  if ((flags & 0x80U) != 0) {
    pos += 2;
  }
  if ((flags & 0x40U) != 0 && pos < b.size()) {
    pos += 1U + b[pos];
  }
  if ((flags & 0x20U) != 0) {
    pos += 2;
  }
  if (!read_descriptor(b, pos, tag, size) || tag != 0x04 || pos + 13 > b.size()) {
    return info;
  }
  info.oti = b[pos];
  pos += 13;
  if (read_descriptor(b, pos, tag, size) && tag == 0x05 && pos < b.size()) {
    parse_asc(std::span<const uint8_t>(b).subspan(pos, std::min(size, b.size() - pos)), info);
  }
  return info;
}

// ---- codec strings (§8.1.3) ----------------------------------------------------------------------------------

std::string avc_string(const std::string& fourcc, const Bytes& avcc) {
  if (avcc.size() < 4) {
    return fourcc;
  }
  return fourcc + "." + fmt::format("{:02X}{:02X}{:02X}", avcc[1], avcc[2], avcc[3]);
}

uint32_t reverse_bits(uint32_t v) {
  uint32_t r = 0;
  for (int i = 0; i < 32; ++i) {
    r = (r << 1U) | ((v >> static_cast<unsigned>(i)) & 1U);
  }
  return r;
}

std::string hevc_string(const std::string& fourcc, const Bytes& hvcc) {
  if (hvcc.size() < 13) {
    return fourcc;
  }
  std::string s = fourcc + ".";
  const unsigned space = hvcc[1] >> 6U;
  if (space != 0) {
    s += static_cast<char>('A' + space - 1U);
  }
  s += std::to_string(hvcc[1] & 0x1FU) + "." + hex_upper(reverse_bits(be32(hvcc, 2))) + "." +
       ((hvcc[1] & 0x20U) != 0 ? "H" : "L") + std::to_string(hvcc[12]);
  size_t last = 6;  // constraint bytes 0..5 at hvcc[6..11]; trailing zero bytes are omitted
  while (last > 0 && hvcc[6 + last - 1] == 0) {
    --last;
  }
  for (size_t i = 0; i < last; ++i) {
    s += "." + hex_upper(hvcc[6 + i]);
  }
  return s;
}

struct Nclx {
  unsigned primaries = 1;
  unsigned transfer = 1;
  unsigned matrix = 1;
  unsigned full_range = 0;
};

std::string av1_string(const std::string& fourcc, const Bytes& av1c, const Nclx& colr) {
  if (av1c.size() < 4) {
    return fourcc;
  }
  const unsigned profile = av1c[1] >> 5U;
  const unsigned level = av1c[1] & 0x1FU;
  const unsigned tier = av1c[2] >> 7U;
  const bool high = ((av1c[2] >> 6U) & 1U) != 0;
  const bool twelve = ((av1c[2] >> 5U) & 1U) != 0;
  const unsigned mono = (av1c[2] >> 4U) & 1U;
  const unsigned subx = (av1c[2] >> 3U) & 1U;
  const unsigned suby = (av1c[2] >> 2U) & 1U;
  const unsigned csp = av1c[2] & 3U;
  const unsigned depth = high ? (twelve ? 12U : 10U) : 8U;
  std::string s =
      fourcc + "." + std::to_string(profile) + "." + dec2(level) + (tier != 0 ? "H" : "M") + "." + dec2(depth);
  const std::string chroma = std::to_string(subx) + std::to_string(suby) + std::to_string(csp);
  const bool defaults = mono == 0 && chroma == "110" && colr.primaries == 1 && colr.transfer == 1 && colr.matrix == 1 &&
                        colr.full_range == 0;
  if (!defaults) {
    s += "." + std::to_string(mono) + "." + chroma + "." + dec2(colr.primaries) + "." + dec2(colr.transfer) + "." +
         dec2(colr.matrix) + "." + std::to_string(colr.full_range);
  }
  return s;
}

std::string vp9_string(const std::string& fourcc, const Bytes& vpcc) {
  if (vpcc.size() < 7) {
    return fourcc;
  }
  return fourcc + "." + dec2(vpcc[4]) + "." + dec2(vpcc[5]) + "." + dec2(vpcc[6] >> 4U);
}

std::string dolby_string(const std::string& fourcc, const Bytes& dv) {
  if (dv.size() < 4) {
    return fourcc;
  }
  return fourcc + "." + dec2(dv[2] >> 1U) + "." + dec2(((dv[2] & 1U) << 5U) | (dv[3] >> 3U));
}

// ---- mappings --------------------------------------------------------------------------------------------------

fb::HandlerType handler_of(std::string_view h) {
  if (h == "vide") {
    return fb::HandlerType::Video;
  }
  if (h == "soun") {
    return fb::HandlerType::Audio;
  }
  if (h == "sbtl" || h == "subt") {
    return fb::HandlerType::Subtitle;
  }
  if (h == "text") {
    return fb::HandlerType::Text;
  }
  if (h == "meta") {
    return fb::HandlerType::Metadata;
  }
  if (h == "hint") {
    return fb::HandlerType::Hint;
  }
  if (h == "tmcd") {
    return fb::HandlerType::Timecode;
  }
  if (h == "auxv") {
    return fb::HandlerType::Auxiliary;
  }
  return fb::HandlerType::Unknown;
}

bool is_one_of(std::string_view v, std::initializer_list<std::string_view> set) {
  return std::ranges::find(set, v) != set.end();
}

fb::Codec codec_of(std::string_view format, const EsInfo* es) {
  if (is_one_of(format, {"avc1", "avc2", "avc3", "avc4", "dvav", "dva1"})) {
    return fb::Codec::H264;
  }
  if (is_one_of(format, {"hev1", "hvc1", "hev2", "hvc2", "dvhe", "dvh1"})) {
    return fb::Codec::H265;
  }
  if (is_one_of(format, {"av01", "dav1"})) {
    return fb::Codec::AV1;
  }
  if (format == "vp09") {
    return fb::Codec::VP9;
  }
  if (is_one_of(format, {"vvc1", "vvi1"})) {
    return fb::Codec::H266;
  }
  if (format == "mp4v" && es != nullptr && es->oti && (*es->oti == 0x60 || *es->oti == 0x61)) {
    return fb::Codec::MPEG2;
  }
  return fb::Codec::Unknown;
}

// 16.16 matrix entries a, b, c, d -> clockwise rotation, 0 when it is not one of the four rotations.
uint16_t rotation_of(const std::vector<int32_t>& m) {
  if (m.size() != 9) {
    return 0;
  }
  constexpr int32_t kOne = 0x10000;
  const std::array<int32_t, 4> abcd = {m[0], m[1], m[3], m[4]};
  if (abcd == std::array<int32_t, 4>{kOne, 0, 0, kOne}) {
    return 0;
  }
  if (abcd == std::array<int32_t, 4>{0, kOne, -kOne, 0}) {
    return 90;
  }
  if (abcd == std::array<int32_t, 4>{-kOne, 0, 0, -kOne}) {
    return 180;
  }
  if (abcd == std::array<int32_t, 4>{0, -kOne, kOne, 0}) {
    return 270;
  }
  return 0;
}

Nclx nclx_of(const io::RandomAccessFile& file, const Node& entry) {
  Nclx out;
  const Node* colr = child(entry, "colr");
  if (colr == nullptr) {
    return out;
  }
  const Bytes p = payload_bytes(file, *colr, 11);
  if (p.size() >= 11 && std::string_view(reinterpret_cast<const char*>(p.data()), 4) == "nclx") {
    out.primaries = (unsigned{p[4]} << 8U) | p[5];
    out.transfer = (unsigned{p[6]} << 8U) | p[7];
    out.matrix = (unsigned{p[8]} << 8U) | p[9];
    out.full_range = p[10] >> 7U;
  }
  return out;
}

std::unique_ptr<fb::DiagnosticT> make_diag(std::string_view code, std::string message) {
  auto d = std::make_unique<fb::DiagnosticT>();
  d->code = std::string(code);
  d->message = std::move(message);
  if (const DiagInfo* info = diag(code)) {
    d->severity = static_cast<fb::Severity>(info->severity);
    d->spec_ref = std::string(info->spec_ref);
  }
  return d;
}

// ---- one track -------------------------------------------------------------------------------------------------

void fill_mdia_headers(const Node& mdia, fb::TrackInfoT& t) {
  if (const Node* mdhd = child(mdia, "mdhd")) {
    t.timescale = static_cast<uint32_t>(uval(*mdhd, "timescale"));
    t.duration = uval(*mdhd, "duration");
    const uint64_t unknown = mdhd->version == 1 ? UINT64_MAX : UINT32_MAX;
    t.duration_seconds =
        t.duration == unknown || t.timescale == 0 ? -1.0 : static_cast<double>(t.duration) / t.timescale;
    t.language = sval(*mdhd, "language");
  }
  if (const Node* elng = child(mdia, "elng")) {
    if (const std::string ext = sval(*elng, "extended_language"); !ext.empty()) {
      t.language = ext;
    }
  }
  if (const Node* hdlr = child(mdia, "hdlr")) {
    t.handler_fourcc = sval(*hdlr, "handler_type");
    t.handler = handler_of(t.handler_fourcc);
  }
}

// Sets sample_count; returns the common sample size of `stsz` (0 when sizes vary or there is no `stsz`).
uint64_t fill_sample_count(const Node& stbl, fb::TrackInfoT& t) {
  if (const Node* stsz = child(stbl, "stsz")) {
    t.sample_count = uval(*stsz, "sample_count");
    return uval(*stsz, "sample_size");
  }
  if (const Node* stz2 = child(stbl, "stz2")) {
    t.sample_count = uval(*stz2, "sample_count");
  }
  return 0;
}

// original_format and is_encrypted: `sinf` present, the name from `sinf/frma`.
void fill_protection(const io::RandomAccessFile& file, const Node& entry, fb::TrackInfoT& t) {
  t.sample_entry_fourcc = entry.fourcc;
  t.original_format = entry.fourcc;
  const Node* sinf = child(entry, "sinf");
  t.is_encrypted = sinf != nullptr;
  const Node* frma = sinf != nullptr ? child(*sinf, "frma") : nullptr;
  if (frma == nullptr) {
    return;
  }
  const Bytes p = payload_bytes(file, *frma, 4);
  if (p.size() == 4) {
    t.original_format = fourcc_text(be32(p, 0));
  }
}

void fill_visual(const Node& entry, fb::TrackInfoT& t) {
  t.coded_width = static_cast<uint32_t>(uval(entry, "width"));
  t.coded_height = static_cast<uint32_t>(uval(entry, "height"));
  if (const Node* pasp = child(entry, "pasp")) {
    t.pixel_aspect = {static_cast<uint32_t>(uval(*pasp, "hSpacing")), static_cast<uint32_t>(uval(*pasp, "vSpacing"))};
  }
  if (const Node* clap = child(entry, "clap")) {
    for (const char* name : {"cleanApertureWidthN", "cleanApertureWidthD", "cleanApertureHeightN",
                             "cleanApertureHeightD", "horizOffN", "horizOffD", "vertOffN", "vertOffD"}) {
      t.clean_aperture.push_back(static_cast<uint32_t>(uval(*clap, name)));
    }
  }
}

const fb::FieldT* first_field(const Node& n, std::initializer_list<std::string_view> names) {
  for (const std::string_view name : names) {
    if (const fb::FieldT* f = field(n, name)) {
      return f;
    }
  }
  return nullptr;
}

double sample_rate_of(const Node& entry, const fb::TrackInfoT& t, const EsInfo& es) {
  if (const fb::FieldT* rate = first_field(entry, {"samplerate", "sample_rate"});
      rate != nullptr && rate->ivalue != 0) {
    return static_cast<double>(rate->ivalue) / 65536.0;
  }
  if (es.sample_rate) {
    return *es.sample_rate;
  }
  const Bytes* dops = config_bytes(t.codec_private, "dOps");
  return dops != nullptr && dops->size() >= 8 ? static_cast<double>(be32(*dops, 4)) : 0.0;
}

void fill_audio(const Node& entry, const EsInfo& es, fb::TrackInfoT& t) {
  const fb::FieldT* channels = first_field(entry, {"channelcount", "channels"});
  const fb::FieldT* size = first_field(entry, {"samplesize", "sample_size"});
  t.channel_count = channels != nullptr ? static_cast<uint32_t>(channels->uvalue) : 0;
  t.sample_size = size != nullptr ? static_cast<uint32_t>(size->uvalue) : 0;
  t.sample_rate = sample_rate_of(entry, t, es);
  if (t.channel_count == 0 && es.channels) {
    t.channel_count = *es.channels;
  }
}

std::string mp4a_string(uint8_t oti, const std::optional<unsigned>& aot) {
  std::string s = "mp4a." + fmt::format("{:02X}", oti);
  if (oti == 0x40 && aot.has_value()) {
    s += "." + std::to_string(aot.value());
  }
  return s;
}

// RFC 6381 string of the first sample entry (§8.1.3).
std::string codec_string_of(const io::RandomAccessFile& file, const Node& entry, const EsInfo& es,
                            const fb::TrackInfoT& t) {
  const std::string& fmt_name = t.original_format;
  const auto& cp = t.codec_private;
  const Bytes* dv = config_bytes(cp, "dvvC");
  dv = dv != nullptr ? dv : config_bytes(cp, "dvcC");
  if (dv != nullptr && is_one_of(fmt_name, {"dvav", "dva1", "dvhe", "dvh1", "dav1"})) {
    return dolby_string(fmt_name, *dv);
  }
  const auto with = [&](std::string_view box_type, auto&& make) {
    const Bytes* c = config_bytes(cp, box_type);
    return c != nullptr ? make(*c) : fmt_name;
  };
  switch (t.codec) {
    case fb::Codec::H264:
      return with("avcC", [&](const Bytes& c) { return avc_string(fmt_name, c); });
    case fb::Codec::H265:
      return with("hvcC", [&](const Bytes& c) { return hevc_string(fmt_name, c); });
    case fb::Codec::AV1:
      return with("av1C", [&](const Bytes& c) { return av1_string(fmt_name, c, nclx_of(file, entry)); });
    case fb::Codec::VP9:
      return with("vpcC", [&](const Bytes& c) { return vp9_string(fmt_name, c); });
    default:
      break;
  }
  return fmt_name == "mp4a" && es.oti.has_value() ? mp4a_string(es.oti.value(), es.aot) : fmt_name;
}

// btrt when present, else the average of a constant-size stsz (SPEC-ASSUMPTION(#768) above).
void fill_bitrate(const Node& entry, uint64_t constant_size, fb::TrackInfoT& t) {
  if (const Node* btrt = child(entry, "btrt")) {
    t.bitrate_max_bps = static_cast<uint32_t>(uval(*btrt, "maxBitrate"));
    t.bitrate_avg_bps = static_cast<uint32_t>(uval(*btrt, "avgBitrate"));
    return;
  }
  if (constant_size != 0 && t.duration_seconds > 0) {
    const double bps =
        static_cast<double>(constant_size) * static_cast<double>(t.sample_count) * 8.0 / t.duration_seconds;
    t.bitrate_avg_bps = bps >= static_cast<double>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(std::llround(bps));
  }
}

void fill_first_entry(const io::RandomAccessFile& file, const Node& entry, uint64_t constant_size, fb::TrackInfoT& t) {
  fill_protection(file, entry, t);
  for (auto& cp : codec_private_of(file, entry)) {
    t.codec_private.push_back(std::make_unique<fb::CodecPrivateT>(std::move(cp)));
  }
  const Bytes* esds = config_bytes(t.codec_private, "esds");
  const EsInfo es = esds != nullptr ? parse_esds(*esds) : EsInfo{};
  t.codec = codec_of(t.original_format, &es);
  fill_visual(entry, t);
  if (t.handler == fb::HandlerType::Audio) {
    fill_audio(entry, es, t);
  }
  t.codec_string = codec_string_of(file, entry, es, t);
  fill_bitrate(entry, constant_size, t);
}

void fill_media(const io::RandomAccessFile& file, const Node& mdia, fb::TrackInfoT& t) {
  fill_mdia_headers(mdia, t);
  const Node* minf = child(mdia, "minf");
  const Node* stbl = minf != nullptr ? child(*minf, "stbl") : nullptr;
  if (stbl == nullptr) {
    return;
  }
  const uint64_t constant_size = fill_sample_count(*stbl, t);
  const Node* stsd = child(*stbl, "stsd");
  if (stsd == nullptr) {
    return;
  }
  t.sample_entry_count = static_cast<uint32_t>(uval(*stsd, "entry_count"));
  if (!stsd->children.empty() && stsd->children.front()) {
    fill_first_entry(file, *stsd->children.front(), constant_size, t);
  }
}

void fill_tkhd(const Node& tkhd, fb::TrackInfoT& t) {
  t.track_id = static_cast<uint32_t>(uval(tkhd, "track_ID"));
  t.tkhd_duration = uval(tkhd, "duration");
  const int32_t flags = tkhd.flags < 0 ? 0 : tkhd.flags;
  t.enabled = (flags & 1) != 0;
  t.in_movie = (flags & 2) != 0;
  t.in_preview = (flags & 4) != 0;
  const auto fixed = [&](std::string_view name) {
    const fb::FieldT* f = field(tkhd, name);
    return f != nullptr ? static_cast<double>(f->ivalue) / 65536.0 : 0.0;
  };
  t.width = fixed("width");
  t.height = fixed("height");
  if (const fb::FieldT* m = field(tkhd, "matrix"); m != nullptr && m->children.size() == 9) {
    for (const auto& c : m->children) {
      t.matrix.push_back(static_cast<int32_t>(c->ivalue));
    }
  }
  t.rotation = rotation_of(t.matrix);
}

void fill_references(const Node& tref, fb::TrackInfoT& t) {
  for (const auto& r : tref.children) {
    if (!r) {
      continue;
    }
    auto ref = std::make_unique<fb::TrackReferenceT>();
    ref->type = r->fourcc;
    if (const fb::FieldT* ids = field(*r, "track_IDs")) {
      for (const auto& id : ids->children) {
        ref->track_ids.push_back(static_cast<uint32_t>(id->uvalue));
      }
    }
    t.references.push_back(std::move(ref));
  }
}

std::unordered_set<uint32_t> trex_track_ids(const Node& moov) {
  std::unordered_set<uint32_t> ids;
  if (const Node* mvex = child(moov, "mvex")) {
    for (const auto& c : mvex->children) {
      if (c && c->fourcc == "trex") {
        ids.insert(static_cast<uint32_t>(uval(*c, "track_ID")));
      }
    }
  }
  return ids;
}

std::unique_ptr<fb::TrackInfoT> build_track(const io::RandomAccessFile& file, const Node& trak, uint32_t index) {
  auto t = std::make_unique<fb::TrackInfoT>();
  t->index = index;
  t->trak_offset = trak.file_span ? trak.file_span->offset() : 0;
  t->avstream_index = -1;
  t->duration_seconds = -1.0;
  if (const Node* tkhd = child(trak, "tkhd")) {
    fill_tkhd(*tkhd, *t);
  }
  if (const Node* edts = child(trak, "edts")) {
    t->edit_list_present = child(*edts, "elst") != nullptr;
  }
  if (const Node* tref = child(trak, "tref")) {
    fill_references(*tref, *t);
  }
  if (const Node* mdia = child(trak, "mdia")) {
    fill_media(file, *mdia, *t);
  }
  return t;
}

}  // namespace

std::vector<std::unique_ptr<fb::TrackInfoT>> build_tracks(const io::RandomAccessFile& file,
                                                          const std::vector<std::unique_ptr<fb::BoxNodeT>>& top_level) {
  std::vector<std::unique_ptr<fb::TrackInfoT>> out;
  const auto moov_it = std::ranges::find_if(top_level, [](const auto& b) { return b && b->fourcc == "moov"; });
  if (moov_it == top_level.end()) {
    return out;
  }
  const Node& moov = **moov_it;
  const Node* mvhd = child(moov, "mvhd");
  const auto movie_timescale = static_cast<uint32_t>(mvhd != nullptr ? uval(*mvhd, "timescale") : 0);
  const std::unordered_set<uint32_t> fragmented_ids = trex_track_ids(moov);
  std::unordered_set<uint32_t> seen_ids;
  for (const auto& trak : moov.children) {
    if (!trak || trak->fourcc != "trak") {
      continue;
    }
    auto t = build_track(file, *trak, static_cast<uint32_t>(out.size()));
    t->movie_timescale = movie_timescale;
    t->is_fragmented = fragmented_ids.contains(t->track_id);
    if (!seen_ids.insert(t->track_id).second) {
      t->diagnostics.push_back(make_diag("CNT-1012", "duplicate track_ID " + std::to_string(t->track_id)));
    }
    out.push_back(std::move(t));
  }
  return out;
}

}  // namespace ovt::container::isobmff
