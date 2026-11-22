// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// HdrMetadata summary (chapter 08 §8.4.6, WI-1.223a: steps 1, 3, 4 and 5; the sync-sample probe of step 2 is
// WI-1.223b). Step 1 reads the boxes of the first sample entry again from the file (they are a few dozen bytes each)
// rather than the decoded fields, so the summary does not depend on how the group C decoders name their fields; the
// decoders have already reported what is malformed in them. The parameter sets of avcC / hvcC and the sequence header
// of av1C go to the typed parsers of chapter 7 with recording off. Steps 3-5 classify, compare and fill the
// ovt_ColorDescription image.

#include "container/isobmff/hdr_summary.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "demux/unit_split.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::container::isobmff {

static_assert(sizeof(ovt_ColorDescription) == 44, "HdrMetadata.color is the 44-byte ovt_ColorDescription image");

namespace {

using fb::BoxNodeT;

constexpr size_t kMaxConfigBytes = size_t{64} * 1024U;  // a configuration / colour box larger than this is not read
constexpr uint8_t kUnspecified = 2;                     // H.273 "unspecified" for primaries, transfer and matrix
constexpr uint8_t kTransferPq = 16;
constexpr uint8_t kTransferHlg = 18;

bool is_dolby_entry(std::string_view fourcc) {
  return fourcc == "dvh1" || fourcc == "dvhe" || fourcc == "dvav" || fourcc == "dva1" || fourcc == "dav1";
}

// ---- values collected from boxes and units ------------------------------------------------------------------

struct Triple {
  uint8_t primaries = kUnspecified;
  uint8_t transfer = kUnspecified;
  uint8_t matrix = kUnspecified;
  bool full_range = false;
  friend bool operator==(const Triple& a, const Triple& b) {
    return a.primaries == b.primaries && a.transfer == b.transfer && a.matrix == b.matrix;
  }
};

std::string text(const Triple& t) {
  return fmt::format("{}/{}/{}", t.primaries, t.transfer, t.matrix);
}

// Static mastering display metadata in ovt_ColorDescription order (R, G, B), 0.00002 and 0.0001 cd/m² units.
struct Mdcv {
  std::array<uint16_t, 3> x{};
  std::array<uint16_t, 3> y{};
  uint16_t wx = 0;
  uint16_t wy = 0;
  uint32_t max = 0;
  uint32_t min = 0;
  friend bool operator==(const Mdcv&, const Mdcv&) = default;
};

// Orders three primaries R, G, B by their chromaticity (green has the largest y, red the largest x of the other
// two), never by their position: the SEI and the box code G, B, R, AV1 codes R, G, B and some muxers reorder.
Mdcv make_mdcv(const std::array<uint32_t, 3>& px, const std::array<uint32_t, 3>& py, uint32_t wx, uint32_t wy,
               uint32_t max, uint32_t min) {
  size_t g = 0;
  for (size_t i = 1; i < 3; ++i) {
    g = py.at(i) > py.at(g) ? i : g;
  }
  size_t r = (g + 1) % 3;
  if (const size_t other = (g + 2) % 3; px.at(other) > px.at(r)) {
    r = other;
  }
  const size_t b = 3 - g - r;
  Mdcv m;
  const std::array<size_t, 3> order{r, g, b};
  for (size_t i = 0; i < 3; ++i) {
    m.x.at(i) = static_cast<uint16_t>(px.at(order.at(i)));
    m.y.at(i) = static_cast<uint16_t>(py.at(order.at(i)));
  }
  m.wx = static_cast<uint16_t>(wx);
  m.wy = static_cast<uint16_t>(wy);
  m.max = max;
  m.min = min;
  return m;
}

std::string text(const Mdcv& m) {
  const auto c = [](uint32_t v) { return fmt::format("{:.4f}", v * 0.00002); };
  return fmt::format("R ({}, {}) G ({}, {}) B ({}, {}) WP ({}, {}) max {:.4f} min {:.4f} cd/m²", c(m.x[0]), c(m.y[0]),
                     c(m.x[1]), c(m.y[1]), c(m.x[2]), c(m.y[2]), c(m.wx), c(m.wy), m.max * 0.0001, m.min * 0.0001);
}

struct Cll {
  uint16_t max_cll = 0;
  uint16_t max_fall = 0;
  friend bool operator==(const Cll&, const Cll&) = default;
};

std::string text(const Cll& c) {
  return fmt::format("MaxCLL {} cd/m², MaxFALL {} cd/m²", c.max_cll, c.max_fall);
}

struct Dovi {
  std::string fourcc;  // dvcC / dvvC / dvwC
  uint64_t offset = 0;
  uint8_t profile = 0;
  uint8_t level = 0;
  bool rpu = false;
  bool el = false;
  bool bl = false;
  uint8_t compat = 0;
};

// The colour and format a parameter set / sequence header declares (SPS VUI, AV1 color_config).
struct StreamColour {
  std::string source;  // "sps_vui" | "color_config"
  bool description_present = false;
  Triple triple;
  uint8_t chroma_siting = 0;
  uint8_t chroma_format = 1;
  uint8_t bit_depth = 8;
  uint32_t profile_idc = 0;  // H.264 profile_idc, H.265 general_profile_idc
  // AV1 color_config (CNT-5005)
  bool high_bitdepth = false;
  bool twelve_bit = false;
  bool mono_chrome = false;
  bool subsampling_x = true;
  bool subsampling_y = true;
};

struct ConfigRecord {  // avcC / hvcC / av1C / vpcC header values
  std::string fourcc;
  uint64_t offset = 0;
  uint32_t profile_idc = 0;
  std::optional<uint8_t> bit_depth_luma_minus8;  // avcC: only with the profile_idc 100+ extension
  uint8_t chroma_format = 1;
  // av1C
  bool high_bitdepth = false;
  bool twelve_bit = false;
  bool mono_chrome = false;
  bool subsampling_x = true;
  bool subsampling_y = true;
  // vpcC
  std::optional<Triple> vp_triple;
  uint8_t vp_bit_depth = 0;
};

// ---- small readers -------------------------------------------------------------------------------------------

const BoxNodeT* child(const BoxNodeT& n, std::string_view fourcc) {
  for (const auto& c : n.children) {
    if (c && c->fourcc == fourcc) {
      return c.get();
    }
  }
  return nullptr;
}

const BoxNodeT* descend(const BoxNodeT& n, std::initializer_list<std::string_view> path) {
  const BoxNodeT* at = &n;
  for (const std::string_view p : path) {
    at = child(*at, p);
    if (at == nullptr) {
      return nullptr;
    }
  }
  return at;
}

uint64_t offset_of(const BoxNodeT& n) {
  return n.file_span ? n.file_span->offset() : 0;
}

// The payload of a box after its header (and after version/flags for a FullBox the walker read), capped.
std::vector<uint8_t> payload(const io::RandomAccessFile& file, const BoxNodeT& n) {
  if (!n.file_span) {
    return {};
  }
  const uint64_t skip = uint64_t{n.header_length} + (n.version >= 0 ? 4U : 0U);
  if (n.file_span->length() <= skip) {
    return {};
  }
  const uint64_t len = std::min<uint64_t>(n.file_span->length() - skip, kMaxConfigBytes);
  std::vector<uint8_t> bytes(static_cast<size_t>(len));
  bytes.resize(file.read(n.file_span->offset() + skip, bytes.size(), bytes.data()));
  return bytes;
}

std::span<const uint8_t> sub(std::span<const uint8_t> s, size_t at, size_t len) {
  if (at > s.size() || len > s.size() - at) {
    throw ParseError("BR-0001", "length field past the end of the configuration record", at * 8U);
  }
  return s.subspan(at, len);
}

// ---- the parsers of chapter 7, recording off -----------------------------------------------------------------

std::optional<StreamColour> sps_colour_h265(std::span<const uint8_t> nal) {
  const Rbsp rbsp = rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  syntax::SyntaxBuilder b(br);
  b.set_recording(false);
  const syntax::h265::NalHeader h = syntax::h265::parse_nal_header(br, b);
  if (h.nal_unit_type != 33) {
    return std::nullopt;
  }
  const syntax::h265::Sps sps = syntax::h265::parse_sps(br, b, h.nuh_layer_id);
  StreamColour s;
  s.source = "sps_vui";
  s.profile_idc = sps.profile_tier_level.general.profile_idc;
  s.chroma_format = sps.chroma_format_idc;
  s.bit_depth = static_cast<uint8_t>(sps.bit_depth_luma_minus8 + 8U);
  if (sps.vui) {
    s.description_present = sps.vui->colour_description_present_flag;
    s.triple = {sps.vui->colour_primaries, sps.vui->transfer_characteristics, sps.vui->matrix_coeffs,
                sps.vui->video_full_range_flag};
    s.chroma_siting = sps.vui->color.chroma_siting;
  }
  return s;
}

std::optional<StreamColour> sps_colour_h264(std::span<const uint8_t> nal) {
  const Rbsp rbsp = rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  syntax::SyntaxBuilder b(br);
  b.set_recording(false);
  const syntax::h264::NalHeader h = syntax::h264::parse_nal_header(br, b);
  if (h.nal_unit_type != 7) {
    return std::nullopt;
  }
  const syntax::h264::Sps sps = syntax::h264::parse_sps(br, b);
  StreamColour s;
  s.source = "sps_vui";
  s.profile_idc = sps.profile_idc;
  s.chroma_format = static_cast<uint8_t>(sps.chroma_format_idc);
  s.bit_depth = static_cast<uint8_t>(sps.bit_depth_luma_minus8 + 8U);
  if (sps.vui_parameters_present_flag) {
    const syntax::h264::VuiParameters& v = sps.vui;
    s.description_present = v.colour_description_present_flag;
    s.triple = {static_cast<uint8_t>(v.colour_primaries), static_cast<uint8_t>(v.transfer_characteristics),
                static_cast<uint8_t>(v.matrix_coefficients), v.video_full_range_flag};
    s.chroma_siting = sps.color_description.chroma_siting;
  }
  return s;
}

StreamColour colour_of(const syntax::av1::SequenceHeader& seq) {
  const syntax::av1::ColorConfig& c = seq.color_config;
  const ovt_ColorDescription d = syntax::av1::color_description(seq);
  StreamColour s;
  s.source = "color_config";
  s.description_present = c.color_description_present_flag;
  s.triple = {c.color_primaries, c.transfer_characteristics, c.matrix_coefficients, c.color_range};
  s.chroma_siting = d.chroma_siting;
  s.chroma_format = c.chroma_format;
  s.bit_depth = c.BitDepth;
  s.high_bitdepth = c.high_bitdepth;
  s.twelve_bit = c.twelve_bit;
  s.mono_chrome = c.mono_chrome;
  s.subsampling_x = c.subsampling_x;
  s.subsampling_y = c.subsampling_y;
  return s;
}

// ---- the summary under construction ------------------------------------------------------------------------

class Summary {
 public:
  explicit Summary(const io::RandomAccessFile& file) : file_(file) {}

  void boxes(const BoxNodeT& trak);
  fb::HdrMetadataT finish(uint32_t track_id);

 private:
  // step 1
  void colr(const BoxNodeT& n);
  void mdcv(const BoxNodeT& n);
  void clli(const BoxNodeT& n);
  void dovi(const BoxNodeT& n);
  void avcc(const BoxNodeT& n);
  void hvcc(const BoxNodeT& n);
  void av1c(const BoxNodeT& n);
  void vpcc(const BoxNodeT& n);
  void stream_colour(StreamColour s, int64_t box_offset, int64_t sample, int64_t unit);
  void sample_entry(const BoxNodeT& entry);
  void entry_child(const BoxNodeT& c);
  // steps 3-5
  void classify(Triple t);
  void mismatches(const Triple& t);
  bool colour_mismatch();
  bool static_mismatch();
  bool config_mismatch();
  void fill_format(ovt_ColorDescription& c) const;
  void fill_static(ovt_ColorDescription& c) const;
  [[nodiscard]] std::vector<uint8_t> colour_image(const Triple& t) const;

  void signal(std::string_view source, int64_t box_offset, int64_t sample, int64_t unit, bool present,
              std::string summary);
  void diag(DiagCode code, std::string message, uint64_t file_offset);

  const io::RandomAccessFile& file_;
  fb::HdrMetadataT out_;
  std::set<std::string, std::less<>> seen_;

  std::string entry_;  // sample entry fourcc (original format for encv/resv)
  bool video_ = false;
  std::optional<Triple> colr_;
  bool colr_nclc_ = false;
  uint64_t colr_offset_ = 0;
  std::optional<Mdcv> box_mdcv_;
  std::optional<Cll> box_clli_;
  std::optional<Dovi> dovi_;
  std::optional<ConfigRecord> config_;
  std::optional<StreamColour> stream_;
  // Bitstream findings of the sync-sample probe (step 2, WI-1.223b); unset until it lands, so the rules that read
  // them (3.1 RPU, 3.2 HDR10+, 3.3 SEI 137, CNT-5003/5004/5010/5011) see box values only.
  std::optional<Mdcv> sei_mdcv_;
  std::optional<Cll> sei_cll_;
  bool hdr10plus_ = false;
  bool rpu_ = false;
  bool el_ = false;
};

void Summary::signal(std::string_view source, int64_t box_offset, int64_t sample, int64_t unit, bool present,
                     std::string summary) {
  auto s = std::make_unique<fb::HdrSignalT>();
  s->source = std::string(source);
  s->box_offset = box_offset;
  s->sample_index = sample;
  s->unit_index = unit;
  s->present = present;
  s->summary = std::move(summary);
  out_.signals.push_back(std::move(s));
}

void Summary::diag(DiagCode code, std::string message, uint64_t file_offset) {
  const DiagInfo& info = container::diag(code);
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = static_cast<fb::Severity>(info.severity);
  d->code = std::string(info.code);
  d->message = std::move(message);
  // SPEC-ASSUMPTION(#770): HdrMetadata has no node to be relative to, so the span is the absolute file position of
  // the box the finding is about (0 for the track as a whole).
  d->span = std::make_unique<fb::BitSpan>(file_offset * 8U, 0U);
  d->spec_ref = std::string(info.spec_ref);
  out_.diagnostics.push_back(std::move(d));
}

// ---- step 1: box level -----------------------------------------------------------------------------------------

void Summary::boxes(const BoxNodeT& trak) {
  if (const BoxNodeT* hdlr = descend(trak, {"mdia", "hdlr"}); hdlr != nullptr) {
    const std::vector<uint8_t> p = payload(file_, *hdlr);
    video_ =
        p.size() >= 8 && std::string_view(reinterpret_cast<const char*>(p.data()), p.size()).substr(4, 4) == "vide";
  }
  const BoxNodeT* stsd = descend(trak, {"mdia", "minf", "stbl", "stsd"});
  if (stsd == nullptr || stsd->children.empty() || !stsd->children.front()) {
    return;
  }
  sample_entry(*stsd->children.front());
  if (video_) {
    for (const std::string_view s : {"colr", "mdcv", "clli"}) {
      if (!seen_.contains(s)) {
        signal(s, -1, -1, -1, false, "absent");
      }
    }
  }
}

void Summary::sample_entry(const BoxNodeT& entry) {
  entry_ = entry.fourcc;
  if (entry_ == "encv" || entry_ == "resv") {
    if (const BoxNodeT* frma = descend(entry, {"sinf", "frma"}); frma != nullptr) {
      const std::vector<uint8_t> p = payload(file_, *frma);
      if (p.size() >= 4) {
        entry_.assign(reinterpret_cast<const char*>(p.data()), 4);
      }
    }
  }
  for (const auto& c : entry.children) {
    if (c) {
      entry_child(*c);
    }
  }
}

void Summary::entry_child(const BoxNodeT& c) {
  using Handler = void (Summary::*)(const BoxNodeT&);
  static constexpr std::array<std::pair<std::string_view, Handler>, 10> kHandlers{{
      {"colr", &Summary::colr},
      {"mdcv", &Summary::mdcv},
      {"clli", &Summary::clli},
      {"dvcC", &Summary::dovi},
      {"dvvC", &Summary::dovi},
      {"dvwC", &Summary::dovi},
      {"avcC", &Summary::avcc},
      {"hvcC", &Summary::hvcc},
      {"av1C", &Summary::av1c},
      {"vpcC", &Summary::vpcc},
  }};
  for (const auto& [fourcc, handler] : kHandlers) {
    if (c.fourcc != fourcc) {
      continue;
    }
    try {
      (this->*handler)(c);
    } catch (const ParseError&) {
      // Malformed or truncated: the group C decoder reported it on the box; the summary ignores the box.
    }
  }
}

void Summary::colr(const BoxNodeT& n) {
  if (colr_) {
    return;  // the first nclx/nclc colr counts; an ICC colr after it does not change the triple
  }
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  const uint64_t type = br.u(32);
  constexpr uint32_t kNclx = 0x6E636C78;
  constexpr uint32_t kNclc = 0x6E636C63;
  std::string summary;
  if (type == kNclx || type == kNclc) {
    Triple t;
    t.primaries = static_cast<uint8_t>(br.u(16));
    t.transfer = static_cast<uint8_t>(br.u(16));
    t.matrix = static_cast<uint8_t>(br.u(16));
    colr_nclc_ = type == kNclc;
    if (!colr_nclc_) {
      t.full_range = br.u(1) != 0;
    }
    colr_ = t;
    colr_offset_ = offset_of(n);
    summary = fmt::format("{} {}{}", colr_nclc_ ? "nclc" : "nclx", text(t),
                          colr_nclc_ ? "" : (t.full_range ? " full range" : " limited range"));
  } else {
    summary = "ICC profile (no code points)";
  }
  if (seen_.emplace("colr").second) {
    signal("colr", static_cast<int64_t>(offset_of(n)), -1, -1, true, std::move(summary));
  }
}

void Summary::mdcv(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  std::array<uint32_t, 3> x{};
  std::array<uint32_t, 3> y{};
  for (size_t i = 0; i < 3; ++i) {
    x.at(i) = static_cast<uint32_t>(br.u(16));
    y.at(i) = static_cast<uint32_t>(br.u(16));
  }
  const auto wx = static_cast<uint32_t>(br.u(16));
  const auto wy = static_cast<uint32_t>(br.u(16));
  const auto max = static_cast<uint32_t>(br.u(32));
  const auto min = static_cast<uint32_t>(br.u(32));
  box_mdcv_ = make_mdcv(x, y, wx, wy, max, min);
  seen_.emplace("mdcv");
  signal("mdcv", static_cast<int64_t>(offset_of(n)), -1, -1, true, text(*box_mdcv_));
}

void Summary::clli(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  Cll c;
  c.max_cll = static_cast<uint16_t>(br.u(16));
  c.max_fall = static_cast<uint16_t>(br.u(16));
  box_clli_ = c;
  seen_.emplace("clli");
  signal("clli", static_cast<int64_t>(offset_of(n)), -1, -1, true, text(c));
}

void Summary::dovi(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  Dovi d;
  d.fourcc = n.fourcc;
  d.offset = offset_of(n);
  br.skip_bits(16);  // dv_version_major, dv_version_minor
  d.profile = static_cast<uint8_t>(br.u(7));
  d.level = static_cast<uint8_t>(br.u(6));
  d.rpu = br.u(1) != 0;
  d.el = br.u(1) != 0;
  d.bl = br.u(1) != 0;
  d.compat = static_cast<uint8_t>(br.u(4));
  signal(n.fourcc, static_cast<int64_t>(d.offset), -1, -1, true,
         fmt::format("profile {} level {}, rpu {} el {} bl {}, bl_signal_compatibility_id {}", d.profile, d.level,
                     d.rpu ? 1 : 0, d.el ? 1 : 0, d.bl ? 1 : 0, d.compat));
  if (!dovi_) {
    dovi_ = std::move(d);
  }
}

void Summary::stream_colour(StreamColour s, int64_t box_offset, int64_t sample, int64_t unit) {
  if (stream_) {
    return;
  }
  std::string summary = s.description_present
                            ? text(s.triple) + (s.triple.full_range ? " full range" : " limited range")
                            : std::string("no colour description");
  summary += fmt::format(", {}-bit, chroma_format {}", s.bit_depth, s.chroma_format);
  seen_.emplace(s.source);
  signal(s.source, box_offset, sample, unit, true, std::move(summary));
  stream_ = std::move(s);
}

void Summary::avcc(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  const std::span<const uint8_t> s(p);
  BitReader br(s);
  ConfigRecord r;
  r.fourcc = "avcC";
  r.offset = offset_of(n);
  br.skip_bits(8);  // configurationVersion
  r.profile_idc = static_cast<uint32_t>(br.u(8));
  br.skip_bits(8 + 8 + 6);  // profile_compatibility, AVCLevelIndication, reserved
  br.skip_bits(2 + 3);      // lengthSizeMinusOne, reserved
  const auto num_sps = static_cast<unsigned>(br.u(5));
  config_ = r;
  for (unsigned i = 0; i < num_sps; ++i) {
    const size_t len = br.u(16);
    const auto nal = sub(s, br.bit_pos() / 8U, len);
    br.skip_bytes(len);
    if (auto c = sps_colour_h264(nal); c && !stream_) {
      stream_colour(std::move(*c), static_cast<int64_t>(r.offset), -1, -1);
    }
  }
  const auto num_pps = static_cast<unsigned>(br.u(8));
  for (unsigned i = 0; i < num_pps; ++i) {
    br.skip_bytes(br.u(16));
  }
  if (br.bits_left() >= 32 && r.profile_idc != 66 && r.profile_idc != 77 && r.profile_idc != 88) {
    br.skip_bits(6);
    config_->chroma_format = static_cast<uint8_t>(br.u(2));
    br.skip_bits(5);
    config_->bit_depth_luma_minus8 = static_cast<uint8_t>(br.u(3));
  }
}

void Summary::hvcc(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  const std::span<const uint8_t> s(p);
  BitReader br(s);
  ConfigRecord r;
  r.fourcc = "hvcC";
  r.offset = offset_of(n);
  br.skip_bits(8 + 2 + 1);  // configurationVersion, general_profile_space, general_tier_flag
  r.profile_idc = static_cast<uint32_t>(br.u(5));
  br.skip_bits(32 + 48 + 8 + 4 + 12 + 6 + 2 + 6);  // compatibility, constraints, level, segmentation, parallelism
  r.chroma_format = static_cast<uint8_t>(br.u(2));
  br.skip_bits(5);
  r.bit_depth_luma_minus8 = static_cast<uint8_t>(br.u(3));
  br.skip_bits(5 + 3 + 16 + 2 + 3 + 1 + 2);  // bitDepthChromaMinus8 .. lengthSizeMinusOne
  config_ = r;
  const auto arrays = static_cast<unsigned>(br.u(8));
  for (unsigned a = 0; a < arrays; ++a) {
    br.skip_bits(2);
    const auto type = static_cast<unsigned>(br.u(6));
    const auto count = static_cast<unsigned>(br.u(16));
    for (unsigned i = 0; i < count; ++i) {
      const size_t len = br.u(16);
      const auto nal = sub(s, br.bit_pos() / 8U, len);
      br.skip_bytes(len);
      if (type == 33 && !stream_) {
        if (auto c = sps_colour_h265(nal); c) {
          stream_colour(std::move(*c), static_cast<int64_t>(r.offset), -1, -1);
        }
      }
    }
  }
}

void Summary::av1c(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  ConfigRecord r;
  r.fourcc = "av1C";
  r.offset = offset_of(n);
  br.skip_bits(1 + 7);  // marker, version
  r.profile_idc = static_cast<uint32_t>(br.u(3));
  br.skip_bits(5 + 1);  // seq_level_idx_0, seq_tier_0
  r.high_bitdepth = br.u(1) != 0;
  r.twelve_bit = br.u(1) != 0;
  r.mono_chrome = br.u(1) != 0;
  r.subsampling_x = br.u(1) != 0;
  r.subsampling_y = br.u(1) != 0;
  br.skip_bits(2 + 3 + 1 + 4);  // chroma_sample_position, reserved, initial_presentation_delay
  r.chroma_format = r.mono_chrome ? 0 : (r.subsampling_x ? (r.subsampling_y ? 1 : 2) : 3);
  config_ = r;
  seen_.emplace("av1C");
  signal("av1C", static_cast<int64_t>(r.offset), -1, -1, true,
         fmt::format("{}-bit, chroma_format {}", r.twelve_bit ? 12 : (r.high_bitdepth ? 10 : 8), r.chroma_format));
  const std::span<const uint8_t> obus = br.remaining_bytes();
  std::vector<uint8_t> scratch;
  const demux::SplitResult split = demux::split_units(obus, demux::Framing::Av1Obu, 0, scratch);
  for (size_t i = 0; i < split.units.size() && !stream_; ++i) {
    const std::span<const uint8_t> obu = demux::unit_bytes(obus, split, scratch, i);
    if (obu.empty() ||
        static_cast<syntax::av1::ObuType>((obu[0] >> 3U) & 0xFU) != syntax::av1::ObuType::SequenceHeader) {
      continue;
    }
    BitReader obr(obu);
    syntax::SyntaxBuilder b(obr);
    b.set_recording(false);
    (void)syntax::av1::parse_obu_header(obr, b);
    stream_colour(colour_of(syntax::av1::parse_sequence_header(obr, b)), static_cast<int64_t>(r.offset), -1, -1);
  }
}

void Summary::vpcc(const BoxNodeT& n) {
  const std::vector<uint8_t> p = payload(file_, n);
  BitReader br(p);
  ConfigRecord r;
  r.fourcc = "vpcC";
  r.offset = offset_of(n);
  r.profile_idc = static_cast<uint32_t>(br.u(8));
  br.skip_bits(8);  // level
  r.vp_bit_depth = static_cast<uint8_t>(br.u(4));
  const auto chroma = static_cast<unsigned>(br.u(3));
  r.chroma_format = chroma <= 1 ? 1 : static_cast<uint8_t>(chroma);
  Triple t;
  t.full_range = br.u(1) != 0;
  t.primaries = static_cast<uint8_t>(br.u(8));
  t.transfer = static_cast<uint8_t>(br.u(8));
  t.matrix = static_cast<uint8_t>(br.u(8));
  r.vp_triple = t;
  config_ = r;
  seen_.emplace("vpcC");
  signal("vpcC", static_cast<int64_t>(r.offset), -1, -1, true,
         fmt::format("{}{}, {}-bit", text(t), t.full_range ? " full range" : " limited range", r.vp_bit_depth));
}

// ---- steps 3-5 ---------------------------------------------------------------------------------------------------

void Summary::classify(Triple t) {
  using fb::HdrKind;
  if (dovi_ || rpu_) {
    out_.kind = HdrKind::DolbyVision;
    if (rpu_ && !dovi_) {
      diag(DiagCode::CNT_5010, "Dolby Vision RPU in bitstream but no DV configuration box", 0);
    }
    if (dovi_ && !rpu_ && out_.probed_samples > 0) {
      diag(DiagCode::CNT_5011,
           fmt::format("{} present but no RPU NAL found in {} sync samples", dovi_->fourcc, out_.probed_samples),
           dovi_->offset);
    }
  } else if (t.transfer == kTransferPq && hdr10plus_) {
    out_.kind = HdrKind::HDR10Plus;
  } else if (t.transfer == kTransferPq && (box_mdcv_ || sei_mdcv_)) {
    out_.kind = HdrKind::HDR10;
  } else if (t.transfer == kTransferPq) {
    out_.kind = HdrKind::PQ_NoStatic;
    diag(DiagCode::CNT_5012, "PQ without static metadata", colr_ ? colr_offset_ : 0);
  } else if (t.transfer == kTransferHlg) {
    out_.kind = HdrKind::HLG;
  } else if (t.transfer == 1 || t.transfer == 6 || (t.transfer >= 13 && t.transfer <= 15) || t.primaries == 1 ||
             t.primaries == 5 || t.primaries == 6) {
    out_.kind = HdrKind::SDR;
  } else {
    out_.kind = HdrKind::Unknown;
  }
}

bool Summary::colour_mismatch() {
  if (!colr_ || !stream_ || !stream_->description_present || *colr_ == stream_->triple) {
    return false;
  }
  diag(DiagCode::CNT_5001,
       fmt::format("colr {} {} differs from {} {} (decoders use the bitstream value)", colr_nclc_ ? "nclc" : "nclx",
                   text(*colr_), stream_->source == "sps_vui" ? "SPS VUI" : "AV1 color_config", text(stream_->triple)),
       colr_offset_);
  return true;
}

bool Summary::static_mismatch() {
  bool mismatch = false;
  if (box_mdcv_ && sei_mdcv_ && !(*box_mdcv_ == *sei_mdcv_)) {
    diag(DiagCode::CNT_5003, fmt::format("mdcv {} differs from SEI 137 {}", text(*box_mdcv_), text(*sei_mdcv_)), 0);
    mismatch = true;
  }
  if (box_clli_ && sei_cll_ && !(*box_clli_ == *sei_cll_)) {
    diag(DiagCode::CNT_5004, fmt::format("clli {} differs from SEI 144 {}", text(*box_clli_), text(*sei_cll_)), 0);
    mismatch = true;
  }
  return mismatch;
}

bool Summary::config_mismatch() {
  if (!config_ || !stream_) {
    return false;
  }
  const ConfigRecord& r = *config_;
  const StreamColour& s = *stream_;
  const auto bit = [](bool b) { return b ? 1 : 0; };
  if (r.fourcc == "av1C" && s.source == "color_config" &&
      (r.high_bitdepth != s.high_bitdepth || r.twelve_bit != s.twelve_bit || r.mono_chrome != s.mono_chrome ||
       r.subsampling_x != s.subsampling_x || r.subsampling_y != s.subsampling_y)) {
    diag(DiagCode::CNT_5005,
         fmt::format("av1C high_bitdepth {} twelve_bit {} monochrome {} subsampling {}{} differs from the sequence "
                     "header ({} {} {} {}{})",
                     bit(r.high_bitdepth), bit(r.twelve_bit), bit(r.mono_chrome), bit(r.subsampling_x),
                     bit(r.subsampling_y), bit(s.high_bitdepth), bit(s.twelve_bit), bit(s.mono_chrome),
                     bit(s.subsampling_x), bit(s.subsampling_y)),
         r.offset);
    return true;
  }
  if ((r.fourcc != "hvcC" && r.fourcc != "avcC") || s.source != "sps_vui") {
    return false;
  }
  const bool depth = r.bit_depth_luma_minus8 && uint32_t{*r.bit_depth_luma_minus8} + 8U != s.bit_depth;
  if (r.profile_idc == s.profile_idc && !depth) {
    return false;
  }
  diag(DiagCode::CNT_5006,
       fmt::format("{} profile {} bit depth {} differs from the SPS (profile {} bit depth {})", r.fourcc, r.profile_idc,
                   r.bit_depth_luma_minus8 ? std::to_string(*r.bit_depth_luma_minus8 + 8U) : "-", s.profile_idc,
                   s.bit_depth),
       r.offset);
  return true;
}

void Summary::mismatches(const Triple& t) {
  const bool colour = colour_mismatch();
  const bool statics = static_mismatch();
  const bool config = config_mismatch();
  const bool dovi = dovi_ && dovi_->compat == 1 && t.transfer != kTransferPq;
  if (dovi) {
    diag(DiagCode::CNT_5021,
         fmt::format("dv_bl_signal_compatibility_id 1 (HDR10) but transfer_characteristics {}", t.transfer),
         dovi_->offset);
  }
  out_.box_vs_bitstream_mismatch = colour || statics || config || dovi;
}

void Summary::fill_format(ovt_ColorDescription& c) const {
  if (stream_) {
    c.chroma_siting = stream_->chroma_siting;
    c.chroma_format = stream_->chroma_format;
    c.bit_depth = stream_->bit_depth;
  } else if (config_) {
    c.chroma_format = config_->chroma_format;
    if (config_->fourcc == "vpcC") {
      c.bit_depth = config_->vp_bit_depth;
    } else if (config_->fourcc == "av1C") {
      c.bit_depth = config_->twelve_bit ? 12 : (config_->high_bitdepth ? 10 : 8);
    } else {
      c.bit_depth = static_cast<uint8_t>(config_->bit_depth_luma_minus8.value_or(0) + 8U);
    }
  }
}

void Summary::fill_static(ovt_ColorDescription& c) const {
  if (const std::optional<Mdcv>& m = box_mdcv_ ? box_mdcv_ : sei_mdcv_; m) {
    for (size_t i = 0; i < 3; ++i) {
      c.mdcv_primaries[i][0] = m->x.at(i);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      c.mdcv_primaries[i][1] = m->y.at(i);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
    c.mdcv_white_point[0] = m->wx;
    c.mdcv_white_point[1] = m->wy;
    c.mdcv_max_luminance = m->max;
    c.mdcv_min_luminance = m->min;
  }
  if (const std::optional<Cll>& l = box_clli_ ? box_clli_ : sei_cll_; l) {
    c.max_cll = l->max_cll;
    c.max_fall = l->max_fall;
  }
}

std::vector<uint8_t> Summary::colour_image(const Triple& t) const {
  ovt_ColorDescription c{};
  c.struct_size = sizeof(ovt_ColorDescription);
  c.primaries = t.primaries;
  c.transfer = t.transfer;
  c.matrix = t.matrix;
  c.full_range = t.full_range ? 1 : 0;
  fill_format(c);
  fill_static(c);
  c.has_hdr10plus = hdr10plus_ ? 1 : 0;
  c.has_dolby_vision = out_.kind == fb::HdrKind::DolbyVision ? 1 : 0;
  if (c.has_dolby_vision != 0 && dovi_) {
    c.dv_profile = dovi_->profile;
    c.dv_level = dovi_->level;
  }
  std::vector<uint8_t> image(sizeof(ovt_ColorDescription));
  std::memcpy(image.data(), &c, image.size());  // little-endian hosts only (§0.5.6, like RawVideoFormat.color)
  return image;
}

fb::HdrMetadataT Summary::finish(uint32_t track_id) {
  out_.track_id = track_id;
  // Step 5: colr wins for the code points (libavformat / Apple behaviour), then vpcC, then the bitstream.
  Triple t;
  if (colr_) {
    t = *colr_;
    if (colr_nclc_ && stream_) {
      t.full_range = stream_->triple.full_range;  // nclc carries no range flag
    }
  } else if (config_ && config_->vp_triple) {
    t = *config_->vp_triple;
  } else if (stream_ && stream_->description_present) {
    t = stream_->triple;
  } else if (stream_) {
    t.full_range = stream_->triple.full_range;
  }
  classify(t);
  mismatches(t);
  if (video_ && !colr_) {
    diag(DiagCode::CNT_5007, "colr absent on a video track (SDR assumed)", 0);
  }
  const uint8_t depth = stream_ ? stream_->bit_depth : 0;
  if (t.transfer == kUnspecified && t.primaries == 9 && depth == 10) {
    diag(DiagCode::CNT_5008, "transfer_characteristics 2 (unspecified) with 10-bit BT.2020 content", 0);
  }
  if (dovi_) {
    out_.dv_profile = dovi_->profile;
    out_.dv_level = dovi_->level;
    out_.dv_bl_compat_id = dovi_->compat;
    out_.dv_bl_present = dovi_->bl;
    // SPEC-ASSUMPTION(#770): §8.4.6 gives CNT-5022 no trigger; a cross-compatible base layer in a DV-only entry.
    if (is_dolby_entry(entry_) && dovi_->compat != 0) {
      diag(DiagCode::CNT_5022,
           fmt::format("sample entry {} (not cross-compatible) carries a profile with bl_signal_compatibility_id {}; "
                       "a cross-compatible stream is signalled as hvc1/avc1/av01 + {}",
                       entry_, dovi_->compat, dovi_->fourcc),
           dovi_->offset);
    }
  }
  out_.dv_rpu_present = (dovi_ && dovi_->rpu) || rpu_;
  out_.dv_el_present = (dovi_ && dovi_->el) || el_;
  out_.color = colour_image(t);
  return std::move(out_);
}

}  // namespace

fb::HdrMetadataT summarize_hdr(const fb::BoxNodeT& trak, uint32_t track_id, const io::RandomAccessFile& file) {
  Summary s(file);
  s.boxes(trak);
  return s.finish(track_id);
}

flatbuffers::DetachedBuffer build_hdr_metadata(const fb::BoxNodeT& trak, uint32_t track_id,
                                               const io::RandomAccessFile& file) {
  const fb::HdrMetadataT m = summarize_hdr(trak, track_id, file);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::HdrMetadata::Pack(fbb, &m));
  return fbb.Release();
}

ovt_ColorDescription color_from_image(const std::vector<uint8_t>& image) noexcept {
  ovt_ColorDescription c{};
  if (image.size() == sizeof(ovt_ColorDescription)) {
    std::memcpy(&c, image.data(), sizeof c);
  }
  c.struct_size = sizeof(ovt_ColorDescription);
  return c;
}

}  // namespace ovt::container::isobmff
