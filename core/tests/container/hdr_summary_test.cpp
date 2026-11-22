// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the HdrMetadata summary (chapter 08 §8.4.6, WI-1.223): box-level collection, the bounded sync-sample probe,
// the classification rules 3.1-3.7, the mismatch rules, the ovt_ColorDescription image and ovt_container_hdr_metadata,
// on hand-assembled files and on the corpus files of the acceptance tests.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/hdr_summary.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/cancel.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/io.h"
#include "ovt/jobs.h"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::HdrKind;

// ---- byte assembly ----

class BitWriter {
 public:
  BitWriter& u(unsigned n, uint64_t v) {
    for (unsigned i = n; i-- > 0;) {
      bits_.push_back(((v >> i) & 1U) != 0);
    }
    return *this;
  }
  BitWriter& ue(uint32_t v) {
    const uint64_t code = uint64_t{v} + 1;
    unsigned len = 0;
    while ((code >> len) > 1) {
      ++len;
    }
    u(len, 0);
    return u(len + 1, code);
  }
  BitWriter& trailing() {
    u(1, 1);
    while (bits_.size() % 8 != 0) {
      u(1, 0);
    }
    return *this;
  }
  [[nodiscard]] Bytes bytes() const {
    Bytes out((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
      if (bits_[i]) {
        out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
      }
    }
    return out;
  }

 private:
  std::vector<bool> bits_;
};

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes be(uint64_t v, unsigned n) {
  Bytes b;
  for (unsigned i = n; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
  return b;
}
Bytes str(std::string_view s) {
  return {s.begin(), s.end()};
}
Bytes zeros(size_t n) {
  return Bytes(n, 0);
}
Bytes box(std::string_view type, const Bytes& payload) {
  return cat({be(8 + payload.size(), 4), str(type), payload});
}

// Inserts emulation prevention bytes after the 2-byte H.265 / 1-byte H.264 header.
Bytes with_epb(const Bytes& rbsp, size_t header) {
  Bytes out(rbsp.begin(), rbsp.begin() + static_cast<std::ptrdiff_t>(header));
  unsigned zeros_seen = 0;
  for (size_t i = header; i < rbsp.size(); ++i) {
    if (zeros_seen >= 2 && rbsp[i] <= 3) {
      out.push_back(3);
      zeros_seen = 0;
    }
    out.push_back(rbsp[i]);
    zeros_seen = rbsp[i] == 0 ? zeros_seen + 1 : 0;
  }
  return out;
}

// ---- H.265 parameter set ----

// A 64x64 SPS with a VUI colour description; `profile_idc` 2 (Main 10) unless given.
Bytes hevc_sps(unsigned cp, unsigned tc, unsigned mc, unsigned profile_idc = 2, unsigned depth_minus8 = 2) {
  BitWriter w;
  w.u(8, 0x42).u(8, 0x01);  // nal_unit_type 33
  w.u(4, 0).u(3, 0).u(1, 1);
  w.u(2, 0).u(1, 0).u(5, profile_idc);
  for (unsigned j = 0; j < 32; ++j) {
    w.u(1, j == profile_idc ? 1 : 0);
  }
  w.u(4, 0b1010).u(43, 0).u(1, 0);  // source flags, constraint bits, inbld
  w.u(8, 90);                       // level 3
  w.ue(0).ue(1).ue(64).ue(64).u(1, 0).ue(depth_minus8).ue(depth_minus8).ue(4);
  w.u(1, 1).ue(4).ue(2).ue(0);
  w.ue(0).ue(1).ue(0).ue(2).ue(1).ue(1);
  w.u(1, 0).u(1, 0).u(1, 0).u(1, 0);  // scaling list, amp, sao, pcm
  w.ue(0).u(1, 0).u(1, 0).u(1, 0);    // st_rps, long term, temporal mvp, strong intra
  w.u(1, 1);                          // vui_parameters_present_flag
  w.u(1, 0).u(1, 0);                  // aspect ratio, overscan
  w.u(1, 1).u(3, 5).u(1, 0).u(1, 1).u(8, cp).u(8, tc).u(8, mc);
  w.u(1, 0).u(1, 0).u(1, 0).u(1, 0).u(1, 0).u(1, 0).u(1, 0);  // chroma loc .. bitstream restriction
  w.u(1, 0);                                                  // sps_extension_present_flag
  w.trailing();
  return with_epb(w.bytes(), 2);
}

// mastering_display_colour_volume() as coded (G, B, R) with BT.2020 primaries, D65, 1000 / 0.005 cd/m².
Bytes mdcv_payload(uint32_t max = 10'000'000, uint32_t min = 50) {
  return cat({be(8500, 2), be(39850, 2), be(6550, 2), be(2300, 2), be(35400, 2), be(14600, 2), be(15635, 2),
              be(16450, 2), be(max, 4), be(min, 4)});
}

Bytes cll_payload(uint16_t cll, uint16_t fall) {
  return cat({be(cll, 2), be(fall, 2)});
}

// ---- boxes ----

Bytes colr(unsigned cp, unsigned tc, unsigned mc, bool full = false) {
  return box("colr", cat({str("nclx"), be(cp, 2), be(tc, 2), be(mc, 2), be(full ? 0x80 : 0, 1)}));
}
Bytes mdcv_box(uint32_t max = 10'000'000, uint32_t min = 50) {
  return box("mdcv", mdcv_payload(max, min));
}
Bytes clli_box(uint16_t cll, uint16_t fall) {
  return box("clli", cll_payload(cll, fall));
}
Bytes dovi_box(std::string_view type, unsigned profile, unsigned compat, bool rpu = true) {
  BitWriter w;
  w.u(8, 1).u(8, 0).u(7, profile).u(6, 6).u(1, rpu ? 1 : 0).u(1, 0).u(1, 1).u(4, compat).u(28, 0).u(32, 0);
  w.u(32, 0).u(32, 0).u(32, 0).u(32, 0);
  return box(type, w.bytes());
}
Bytes hvcc(const Bytes& sps, unsigned profile_idc = 2, unsigned depth_minus8 = 2) {
  BitWriter w;
  w.u(8, 1).u(2, 0).u(1, 0).u(5, profile_idc).u(32, 1U << (31U - profile_idc)).u(48, 0).u(8, 90);
  w.u(4, 0xF).u(12, 0).u(6, 0x3F).u(2, 0).u(6, 0x3F).u(2, 1).u(5, 0x1F).u(3, depth_minus8).u(5, 0x1F);
  w.u(3, depth_minus8).u(16, 0).u(2, 0).u(3, 1).u(1, 1).u(2, 3);
  w.u(8, sps.empty() ? 0 : 1);
  Bytes b = w.bytes();
  if (!sps.empty()) {
    b = cat({b, Bytes{0x80 | 33}, be(1, 2), be(sps.size(), 2), sps});
  }
  return box("hvcC", b);
}
Bytes visual_entry(std::string_view fourcc, const Bytes& children) {
  return box(fourcc, cat({zeros(6), be(1, 2), zeros(16), be(64, 2), be(64, 2), be(0x00480000, 4), be(0x00480000, 4),
                          zeros(4), be(1, 2), zeros(32), be(24, 2), be(0xFFFF, 2), children}));
}

// ---- AV1 ----

Bytes leb(uint64_t v) {
  Bytes b;
  do {
    uint8_t byte = v & 0x7FU;
    v >>= 7U;
    b.push_back(static_cast<uint8_t>(byte | (v != 0 ? 0x80U : 0U)));
  } while (v != 0);
  return b;
}
Bytes obu(unsigned type, const Bytes& payload) {
  return cat({Bytes{static_cast<uint8_t>((type << 3U) | 0x02U)}, leb(payload.size()), payload});
}
// A reduced still-picture sequence header, profile 0, with a colour description.
Bytes av1_seq_header(bool high_bitdepth, unsigned cp, unsigned tc, unsigned mc) {
  BitWriter w;
  w.u(3, 0).u(1, 1).u(1, 1).u(5, 8);      // profile, still_picture, reduced, seq_level_idx
  w.u(4, 5).u(4, 5).u(6, 63).u(6, 63);    // 64x64
  w.u(1, 0).u(1, 0).u(1, 0);              // 128x128, filter_intra, intra_edge
  w.u(1, 0).u(1, 0).u(1, 0);              // superres, cdef, restoration
  w.u(1, high_bitdepth ? 1 : 0).u(1, 0);  // high_bitdepth, mono_chrome
  w.u(1, 1).u(8, cp).u(8, tc).u(8, mc);   // color description
  w.u(1, 0).u(2, 0).u(1, 0);              // color_range, chroma_sample_position, separate_uv_delta_q
  w.u(1, 0);                              // film_grain_params_present
  w.trailing();
  return obu(1, w.bytes());
}
Bytes av1c(bool high_bitdepth, const Bytes& config_obus) {
  const auto b2 = static_cast<uint8_t>((high_bitdepth ? 0x40U : 0U) | 0x0CU);
  return box("av1C", cat({Bytes{0x81, 0x08, b2, 0x00}, config_obus}));
}

// ---- a one-track file ----

Bytes movie(const Bytes& entry, std::string_view handler = "vide") {
  const Bytes hdlr = box("hdlr", cat({zeros(4), zeros(4), str(handler), zeros(12), Bytes{0}}));
  const Bytes stsd = box("stsd", cat({be(0, 4), be(1, 4), entry}));
  const Bytes moov = box("moov", box("trak", box("mdia", cat({hdlr, box("minf", box("stbl", stsd))}))));
  return cat({box("ftyp", cat({str("isom"), zeros(4), str("isom")})), moov});
}

const ovt::fb::BoxNodeT* find(const std::vector<std::unique_ptr<ovt::fb::BoxNodeT>>& nodes, std::string_view cc) {
  for (const auto& n : nodes) {
    if (n->fourcc == cc) {
      return n.get();
    }
    if (const auto* c = find(n->children, cc); c != nullptr) {
      return c;
    }
  }
  return nullptr;
}

struct Run {
  ovt::fb::HdrMetadataT meta;
  ovt_ColorDescription color{};
};

Run summarize(const ovt::io::RandomAccessFile& file) {
  const iso::WalkResult w = iso::walk(file);
  const ovt::fb::BoxNodeT* trak = find(w.top_level, "trak");
  REQUIRE(trak != nullptr);
  Run r;
  r.meta = iso::summarize_hdr(*trak, 1, file);
  r.color = iso::color_from_image(r.meta.color);
  return r;
}

Run summarize(const Bytes& entry, std::string_view handler = "vide") {
  const Bytes bytes = movie(entry, handler);
  const ovt::io::MemoryFile file(bytes);
  return summarize(file);
}

const ovt::fb::HdrSignalT* signal(const Run& r, std::string_view source) {
  for (const auto& s : r.meta.signals) {
    if (s->source == source) {
      return s.get();
    }
  }
  return nullptr;
}

bool has_diag(const Run& r, std::string_view code) {
  return std::any_of(r.meta.diagnostics.begin(), r.meta.diagnostics.end(),
                     [&](const auto& d) { return d->code == code; });
}

// ---- acceptance tests 2 and 3 (corpus) ----

TEST_CASE("AT2: hdr_hlg_bt2020 is HLG", "[container][hdr][needs-corpus][slow]") {
  const auto file = ovt::io::RandomAccessFile::open(ovt::test::corpus_or_skip("hdr_hlg_bt2020").string());
  const Run r = summarize(*file);
  CHECK(r.meta.kind == HdrKind::HLG);
  CHECK(r.color.transfer == 18);
}

TEST_CASE("AT3: syn_h265_main10_pq_hdr_mismatch raises CNT-5001 (colr BT.709 over a BT.2020 SPS)",
          "[container][hdr][needs-corpus]") {
  const auto file =
      ovt::io::RandomAccessFile::open(ovt::test::corpus_or_skip("syn_h265_main10_pq_hdr_mismatch").string());
  const Run r = summarize(*file);
  CHECK(has_diag(r, "CNT-5001"));
  CHECK(r.meta.box_vs_bitstream_mismatch);
  REQUIRE(signal(r, "sps_vui") != nullptr);
  CHECK(r.color.primaries == 1);  // colr wins (§8.4.6 step 5)
  CHECK(r.color.bit_depth == 10);
}

// ---- classification ----

TEST_CASE("colr nclx 9/18/9 is HLG at box level", "[container][hdr]") {
  const Run r = summarize(visual_entry("hvc1", colr(9, 18, 9)));
  CHECK(r.meta.kind == HdrKind::HLG);
  CHECK(r.meta.probed_samples == 0);
  CHECK(r.color.transfer == 18);
  CHECK(r.color.primaries == 9);
  REQUIRE(signal(r, "colr") != nullptr);
  CHECK(signal(r, "colr")->present);
  CHECK(signal(r, "colr")->sample_index == -1);
  REQUIRE(signal(r, "mdcv") != nullptr);
  CHECK_FALSE(signal(r, "mdcv")->present);
  CHECK_FALSE(has_diag(r, "CNT-5007"));
}

TEST_CASE("colr BT.709 over a BT.2020 PQ SPS raises CNT-5001; colr wins in the image", "[container][hdr]") {
  const Run r = summarize(visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(1, 1, 1)})));
  CHECK(has_diag(r, "CNT-5001"));
  CHECK(r.meta.box_vs_bitstream_mismatch);
  CHECK(r.color.primaries == 1);
  CHECK(r.color.transfer == 1);
  CHECK(r.meta.kind == HdrKind::SDR);
  REQUIRE(signal(r, "sps_vui") != nullptr);
  CHECK(signal(r, "sps_vui")->box_offset > 0);

  // Agreeing values: no diagnostic.
  const Run ok = summarize(visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9), mdcv_box()})));
  CHECK_FALSE(ok.meta.box_vs_bitstream_mismatch);
  CHECK(ok.meta.diagnostics.empty());
}

TEST_CASE("HDR10: colr PQ + mdcv + clli, image in R, G, B order with the SPS format", "[container][hdr]") {
  const Bytes bytes =
      movie(visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9), mdcv_box(), clli_box(1000, 400)})));
  const ovt::io::MemoryFile file(bytes);
  const Run r = summarize(file);
  CHECK(r.meta.kind == HdrKind::HDR10);
  CHECK(r.meta.diagnostics.empty());
  CHECK(r.color.struct_size == 44);
  CHECK(r.color.bit_depth == 10);
  CHECK(r.color.chroma_format == 1);
  CHECK(r.color.mdcv_primaries[0][0] == 35400);  // red
  CHECK(r.color.mdcv_primaries[0][1] == 14600);
  CHECK(r.color.mdcv_primaries[1][0] == 8500);  // green
  CHECK(r.color.mdcv_primaries[2][0] == 6550);  // blue
  CHECK(r.color.mdcv_white_point[0] == 15635);
  CHECK(r.color.mdcv_max_luminance == 10'000'000);
  CHECK(r.color.mdcv_min_luminance == 50);
  CHECK(r.color.max_cll == 1000);
  CHECK(r.color.max_fall == 400);
  CHECK(r.color.has_dolby_vision == 0);

  // The serialised form verifies and carries the same image.
  const iso::WalkResult w = iso::walk(file);
  const auto buf = iso::build_hdr_metadata(*find(w.top_level, "trak"), 7, file);
  flatbuffers::Verifier v(buf.data(), buf.size());
  REQUIRE(v.VerifyBuffer<ovt::fb::HdrMetadata>(nullptr));
  const auto* root = flatbuffers::GetRoot<ovt::fb::HdrMetadata>(buf.data());
  CHECK(root->track_id() == 7);
  CHECK(root->kind() == HdrKind::HDR10);
  REQUIRE(root->color() != nullptr);
  CHECK(root->color()->size() == 44);
}

TEST_CASE("PQ without static metadata, SDR, Unknown and a missing colr", "[container][hdr]") {
  const Run pq = summarize(visual_entry("hvc1", colr(9, 16, 9)));
  CHECK(pq.meta.kind == HdrKind::PQ_NoStatic);
  CHECK(has_diag(pq, "CNT-5012"));
  CHECK(summarize(visual_entry("hvc1", colr(1, 1, 1))).meta.kind == HdrKind::SDR);
  CHECK(summarize(visual_entry("hvc1", colr(9, 2, 9))).meta.kind == HdrKind::Unknown);

  // No colr: the SPS VUI decides, and CNT-5007 / CNT-5008 report what is missing.
  const Run vui = summarize(visual_entry("hvc1", hvcc(hevc_sps(9, 18, 9))));
  CHECK(vui.meta.kind == HdrKind::HLG);
  CHECK(has_diag(vui, "CNT-5007"));
  const Run unspecified = summarize(visual_entry("hvc1", hvcc(hevc_sps(9, 2, 9))));
  CHECK(has_diag(unspecified, "CNT-5008"));
  const Run none = summarize(visual_entry("hvc1", {}));
  CHECK(none.meta.kind == HdrKind::Unknown);
  CHECK(has_diag(none, "CNT-5007"));
}

// ---- mismatch rules ----

TEST_CASE("hvcC profile / bit depth differing from the SPS raises CNT-5006", "[container][hdr]") {
  const Run r = summarize(visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9), 1, 0), colr(9, 16, 9)})));
  CHECK(has_diag(r, "CNT-5006"));
  CHECK(r.meta.box_vs_bitstream_mismatch);
}

TEST_CASE("Dolby Vision from the configuration box: CNT-5021, CNT-5022", "[container][hdr]") {
  const Run dv = summarize(visual_entry("hvc1", cat({colr(9, 16, 9), mdcv_box(), dovi_box("dvvC", 8, 1)})));
  CHECK(dv.meta.kind == HdrKind::DolbyVision);
  CHECK(dv.meta.dv_profile == 8);
  CHECK(dv.meta.dv_level == 6);
  CHECK(dv.meta.dv_bl_compat_id == 1);
  CHECK(dv.meta.dv_rpu_present);
  CHECK(dv.meta.dv_bl_present);
  CHECK(dv.color.has_dolby_vision == 1);
  CHECK(dv.color.dv_profile == 8);
  CHECK(dv.color.dv_level == 6);
  CHECK_FALSE(has_diag(dv, "CNT-5011"));  // no probe, no RPU check
  REQUIRE(signal(dv, "dvvC") != nullptr);

  const Run hlg_compat1 = summarize(visual_entry("hvc1", cat({colr(9, 18, 9), dovi_box("dvvC", 8, 1)})));
  CHECK(has_diag(hlg_compat1, "CNT-5021"));
  CHECK(hlg_compat1.meta.box_vs_bitstream_mismatch);

  CHECK(has_diag(summarize(visual_entry("dvh1", cat({colr(9, 16, 9), dovi_box("dvvC", 8, 1)}))), "CNT-5022"));
  const Run p5 = summarize(visual_entry("dvh1", dovi_box("dvcC", 5, 0)));
  CHECK_FALSE(has_diag(p5, "CNT-5022"));
  CHECK(p5.meta.dv_profile == 5);
}

TEST_CASE("AV1: av1C vs its sequence header (CNT-5005) and color_config as the bitstream colour", "[container][hdr]") {
  const Run r = summarize(visual_entry("av01", av1c(false, av1_seq_header(true, 9, 16, 9))));
  CHECK(has_diag(r, "CNT-5005"));
  CHECK(r.meta.box_vs_bitstream_mismatch);
  CHECK(has_diag(r, "CNT-5007"));
  REQUIRE(signal(r, "color_config") != nullptr);
  CHECK(signal(r, "color_config")->box_offset > 0);
  CHECK(r.color.primaries == 9);
  CHECK(r.color.transfer == 16);
  CHECK(r.color.bit_depth == 10);
  CHECK(r.meta.kind == HdrKind::PQ_NoStatic);

  const Run same = summarize(visual_entry("av01", cat({av1c(true, av1_seq_header(true, 9, 18, 9)), colr(9, 18, 9)})));
  CHECK_FALSE(same.meta.box_vs_bitstream_mismatch);
  CHECK(same.meta.kind == HdrKind::HLG);
}

TEST_CASE("vpcC gives the code points without colr; avcC records parse", "[container][hdr]") {
  const Bytes vpcc = box("vpcC", cat({be(0x01000000, 4), Bytes{0, 31, 0xA2, 9, 16, 9, 0, 0}}));
  const Run vp = summarize(visual_entry("vp09", vpcc));
  CHECK(vp.color.primaries == 9);
  CHECK(vp.color.transfer == 16);
  CHECK(vp.color.bit_depth == 10);
  CHECK(vp.meta.kind == HdrKind::PQ_NoStatic);
  REQUIRE(signal(vp, "vpcC") != nullptr);

  const Bytes avcc = box("avcC", Bytes{1, 100, 0, 40, 0xFF, 0xE0, 0, 0xFD, 0xF8, 0xF8, 0});
  const Run avc = summarize(visual_entry("avc1", cat({avcc, colr(1, 1, 1)})));
  CHECK(avc.meta.kind == HdrKind::SDR);
  CHECK(avc.color.bit_depth == 8);
}

TEST_CASE("malformed records are left out; non-video and encrypted tracks", "[container][hdr]") {
  // hvcC claiming an SPS longer than the box, a truncated mdcv: neither throws, the colr still counts.
  const Bytes bad_hvcc = box("hvcC", cat({zeros(22), Bytes{1, 0x80 | 33, 0, 1, 0x7F, 0xFF, 0x42}}));
  Run r;
  REQUIRE_NOTHROW(r = summarize(visual_entry("hvc1", cat({bad_hvcc, colr(9, 16, 9), box("mdcv", Bytes{1, 2})}))));
  CHECK(r.meta.kind == HdrKind::PQ_NoStatic);
  CHECK(signal(r, "sps_vui") == nullptr);

  // encv: the original format comes from frma and the colour boxes still count.
  const Run enc = summarize(visual_entry("encv", cat({colr(9, 18, 9), box("sinf", box("frma", str("hvc1")))})));
  CHECK(enc.meta.kind == HdrKind::HLG);

  const Run audio = summarize(visual_entry("hvc1", {}), "soun");
  CHECK(signal(audio, "colr") == nullptr);
  CHECK_FALSE(has_diag(audio, "CNT-5007"));
}

// ---- step 2: the sync-sample probe ----

struct Sei {
  unsigned type;
  Bytes payload;
};

Bytes sei_body(std::initializer_list<Sei> messages) {
  Bytes b;
  for (const Sei& m : messages) {
    b.push_back(static_cast<uint8_t>(m.type));
    b.push_back(static_cast<uint8_t>(m.payload.size()));
    b.insert(b.end(), m.payload.begin(), m.payload.end());
  }
  b.push_back(0x80);
  return b;
}
Bytes hevc_sei(std::initializer_list<Sei> messages) {
  return with_epb(cat({Bytes{0x4E, 0x01}, sei_body(messages)}), 2);
}
Bytes avc_sei(std::initializer_list<Sei> messages) {
  return with_epb(cat({Bytes{0x06}, sei_body(messages)}), 1);
}

// An HDR10+ T.35 message with one processing window (ST 2094-40, application_version 1).
Bytes hdr10plus_payload() {
  BitWriter w;
  w.u(8, 0xB5).u(16, 0x003C).u(16, 0x0001);
  w.u(8, 4).u(8, 1).u(2, 1).u(27, 4000).u(1, 0);
  w.u(17, 1000).u(17, 2000).u(17, 3000).u(17, 1500);
  w.u(4, 3).u(7, 1).u(17, 100).u(7, 50).u(17, 900).u(7, 99).u(17, 2800);
  w.u(10, 25).u(1, 0).u(1, 0).u(1, 0);  // fraction_bright_pixels, no peak grid, no tone mapping, no saturation
  return w.bytes();
}

const Bytes kRpu{0x7C, 0x01, 0x19, 0x08, 0x09, 0x00};  // nal_unit_type 62, nuh_layer_id 0
const Bytes kIdr{0x26, 0x01, 0xAF, 0x09, 0x40};        // nal_unit_type 19

Bytes length_prefixed(std::initializer_list<Bytes> units) {
  Bytes b;
  for (const Bytes& u : units) {
    b = cat({b, be(u.size(), 4), u});
  }
  return b;
}

// A one-track file whose samples follow it in an mdat, and the SampleTable the probe reads them through.
struct Probed {
  Bytes bytes;
  ovt::fb::SampleTableT table;
};

Probed probed(const Bytes& entry, const std::vector<Bytes>& samples, std::vector<bool> sync = {}) {
  Probed p;
  p.bytes = movie(entry);
  Bytes mdat;
  uint64_t at = p.bytes.size() + 8;
  for (const Bytes& s : samples) {
    p.table.offset.push_back(at);
    p.table.size.push_back(static_cast<uint32_t>(s.size()));
    at += s.size();
    mdat = cat({mdat, s});
  }
  p.bytes = cat({p.bytes, box("mdat", mdat)});
  p.table.count = samples.size();
  p.table.is_sync = std::move(sync);
  return p;
}

Run summarize(const ovt::io::RandomAccessFile& file, const ovt::fb::SampleTableT& table,
              const iso::HdrProbeOptions& options = {}, const ovt::jobs::Cancel& cancel = ovt::jobs::Cancel::never()) {
  const iso::WalkResult w = iso::walk(file);
  const ovt::fb::BoxNodeT* trak = find(w.top_level, "trak");
  REQUIRE(trak != nullptr);
  Run r;
  r.meta = iso::summarize_hdr(*trak, 1, table, file, options, cancel);
  r.color = iso::color_from_image(r.meta.color);
  return r;
}

Run summarize(const Probed& p, const iso::HdrProbeOptions& options = {}) {
  const ovt::io::MemoryFile file(p.bytes);
  return summarize(file, p.table, options);
}

TEST_CASE("AT1 shape: Dolby Vision 8.1 with HDR10+ T.35 and an RPU in sample 0", "[container][hdr]") {
  const Bytes entry = visual_entry(
      "hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9), mdcv_box(), clli_box(1000, 400), dovi_box("dvvC", 8, 1)}));
  const Bytes s0 = length_prefixed({hevc_sei({{4, hdr10plus_payload()}}), kRpu, kIdr});
  const Run r = summarize(probed(entry, {s0, length_prefixed({kRpu, kIdr})}));
  CHECK(r.meta.kind == HdrKind::DolbyVision);
  CHECK(r.meta.dv_profile == 8);
  CHECK(r.meta.dv_rpu_present);
  CHECK(r.color.has_hdr10plus == 1);
  CHECK(r.color.has_dolby_vision == 1);
  CHECK(r.meta.hdr10plus_application_version == 1);
  CHECK(r.meta.hdr10plus_num_windows == 1);
  CHECK(r.meta.probed_samples == 2);
  const auto* rpu = signal(r, "nal_62_rpu");
  REQUIRE(rpu != nullptr);
  CHECK(rpu->sample_index == 0);
  CHECK(rpu->unit_index == 1);
  CHECK(rpu->box_offset == -1);
  const auto* t35 = signal(r, "sei_t35_hdr10plus");
  REQUIRE(t35 != nullptr);
  CHECK(t35->sample_index == 0);
  CHECK(t35->unit_index == 0);
  CHECK_FALSE(has_diag(r, "CNT-5010"));
  CHECK_FALSE(has_diag(r, "CNT-5011"));
  CHECK_FALSE(r.meta.box_vs_bitstream_mismatch);

  // Without the DV box the same samples are HDR10+.
  const Run plus = summarize(probed(visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9)})),
                                    {length_prefixed({hevc_sei({{4, hdr10plus_payload()}}), kIdr})}));
  CHECK(plus.meta.kind == HdrKind::HDR10Plus);
}

TEST_CASE("SEI 137 / 144 differing from mdcv / clli raise CNT-5003 / CNT-5004; SEI alone gives HDR10",
          "[container][hdr]") {
  const Bytes entry =
      visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9), mdcv_box(), clli_box(1000, 400)}));
  const Bytes s0 =
      length_prefixed({hevc_sei({{137, mdcv_payload(40'000'000, 50)}, {144, cll_payload(4000, 400)}}), kIdr});
  const Run r = summarize(probed(entry, {s0}));
  CHECK(has_diag(r, "CNT-5003"));
  CHECK(has_diag(r, "CNT-5004"));
  CHECK(r.meta.box_vs_bitstream_mismatch);
  CHECK(r.color.mdcv_max_luminance == 10'000'000);  // the boxes win
  CHECK(r.color.max_cll == 1000);
  REQUIRE(signal(r, "sei_137") != nullptr);
  CHECK(signal(r, "sei_137")->sample_index == 0);
  REQUIRE(signal(r, "sei_144") != nullptr);

  const Bytes same = length_prefixed({hevc_sei({{137, mdcv_payload()}, {144, cll_payload(1000, 400)}}), kIdr});
  const Run ok = summarize(probed(entry, {same}));
  CHECK_FALSE(ok.meta.box_vs_bitstream_mismatch);
  CHECK(ok.meta.diagnostics.empty());

  // H.264: SEI only, an in-band SPS-less stream; the SEI values fill the image.
  const Bytes avcc = box("avcC", Bytes{1, 100, 0, 40, 0xFF, 0xE0, 0, 0xFD, 0xF8, 0xF8, 0});
  const Run avc =
      summarize(probed(visual_entry("avc1", cat({avcc, colr(9, 16, 9)})),
                       {length_prefixed({avc_sei({{137, mdcv_payload()}, {144, cll_payload(800, 200)}})})}));
  CHECK(avc.meta.kind == HdrKind::HDR10);
  CHECK(avc.color.max_cll == 800);
  CHECK(avc.color.mdcv_max_luminance == 10'000'000);
}

TEST_CASE("Dolby Vision from the bitstream: CNT-5010 and CNT-5011", "[container][hdr]") {
  const Run no_box = summarize(probed(visual_entry("hvc1", colr(9, 16, 9)), {length_prefixed({kRpu, kIdr})}));
  CHECK(no_box.meta.kind == HdrKind::DolbyVision);
  CHECK(has_diag(no_box, "CNT-5010"));
  CHECK(no_box.meta.dv_rpu_present);

  const Run no_rpu =
      summarize(probed(visual_entry("hvc1", cat({colr(9, 16, 9), dovi_box("dvvC", 8, 1)})), {length_prefixed({kIdr})}));
  CHECK(no_rpu.meta.kind == HdrKind::DolbyVision);
  CHECK(has_diag(no_rpu, "CNT-5011"));
}

TEST_CASE("AV1 metadata OBUs: HDR_CLL and HDR10+ T.35", "[container][hdr]") {
  const Bytes entry = visual_entry("av01", cat({av1c(true, av1_seq_header(true, 9, 16, 9)), colr(9, 16, 9)}));
  const Bytes cll = obu(5, cat({leb(1), cll_payload(600, 150), Bytes{0x80}}));
  const Bytes t35 = obu(5, cat({leb(4), hdr10plus_payload(), Bytes{0x80}}));
  const Run r = summarize(probed(entry, {cat({cll, t35, obu(6, Bytes{0x10, 0x00})})}));
  CHECK(r.color.max_cll == 600);
  REQUIRE(signal(r, "obu_metadata_1") != nullptr);
  REQUIRE(signal(r, "obu_metadata_4") != nullptr);
  CHECK(signal(r, "obu_metadata_4")->unit_index == 1);
  CHECK(r.meta.kind == HdrKind::HDR10Plus);
}

TEST_CASE("the probe takes sync samples only, up to max_sync_samples and max_bytes, and stops on cancel",
          "[container][hdr]") {
  const Bytes entry = visual_entry("hvc1", cat({colr(9, 16, 9), dovi_box("dvvC", 8, 1)}));
  const Probed p = probed(entry, {length_prefixed({kIdr}), length_prefixed({kRpu, kIdr}), length_prefixed({kIdr})},
                          {true, false, true});
  const Run r = summarize(p);
  CHECK(r.meta.probed_samples == 2);
  CHECK(has_diag(r, "CNT-5011"));  // the RPU sits in the non-sync sample 1

  iso::HdrProbeOptions one;
  one.max_sync_samples = 1;
  CHECK(summarize(p, one).meta.probed_samples == 1);

  iso::HdrProbeOptions small;
  small.max_bytes = 10;  // the first sample is 9 bytes, the second sync sample would take the total to 18
  CHECK(summarize(p, small).meta.probed_samples == 1);
  small.allow_slow_scan = true;
  CHECK(summarize(p, small).meta.probed_samples == 2);

  const ovt::io::MemoryFile file(p.bytes);
  ovt::jobs::Cancel cancel;
  cancel.request();
  CHECK_THROWS_AS(summarize(file, p.table, {}, cancel), ovt::Cancelled);
}

TEST_CASE("malformed samples end quietly; encrypted tracks are not probed", "[container][hdr]") {
  // An SEI whose payload size runs past the NAL, a NAL length past the end of its sample, a sample past the end of the
  // file.
  const Bytes entry = visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9)}));
  Probed p =
      probed(entry, {length_prefixed({Bytes{0x4E, 0x01, 137, 60, 0x01, 0x80}, Bytes{0x7C}}), cat({be(1000, 4), kIdr})});
  p.table.offset.push_back(p.bytes.size() + 100);
  p.table.size.push_back(64);
  p.table.count = 3;
  Run r;
  REQUIRE_NOTHROW(r = summarize(p));
  CHECK(r.meta.kind == HdrKind::PQ_NoStatic);
  CHECK(r.meta.probed_samples == 3);

  const Bytes sinf = box("sinf", box("frma", str("hvc1")));
  const Run enc = summarize(probed(visual_entry("encv", cat({colr(9, 16, 9), sinf})), {length_prefixed({kRpu})}));
  CHECK(enc.meta.probed_samples == 0);
  CHECK(enc.meta.kind == HdrKind::PQ_NoStatic);
}

// ---- ovt_container_hdr_metadata ----

// A sparse file: the listed pieces are real, every other byte below `size` reads as zero. Counts the bytes served
// (RandomAccessFile has no bytes_read(), #630).
class SparseFile final : public ovt::io::RandomAccessFile {
 public:
  SparseFile(uint64_t size, std::map<uint64_t, Bytes> pieces)
      : RandomAccessFile(ovt::io::FileInfo{size, 0, "sparse"}), pieces_(std::move(pieces)) {}

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= size()) {
      return 0;
    }
    const auto n = static_cast<size_t>(std::min<uint64_t>(len, size() - offset));
    const std::span<uint8_t> out(dst, n);
    std::fill(out.begin(), out.end(), uint8_t{0});
    for (const auto& [at, bytes] : pieces_) {
      const uint64_t lo = std::max<uint64_t>(at, offset);
      const uint64_t hi = std::min<uint64_t>(at + bytes.size(), offset + n);
      if (lo < hi) {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(lo - at), hi - lo,
                    out.begin() + static_cast<std::ptrdiff_t>(lo - offset));
      }
    }
    bytes_read_ += n;
    return n;
  }
  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override {
    auto buf = std::make_shared<Bytes>(len);
    buf->resize(read(offset, len, buf->data()));
    return Holder{buf, std::span<const uint8_t>(*buf)};
  }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*a*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }
  [[nodiscard]] uint64_t bytes_read() const noexcept { return bytes_read_; }

 private:
  std::map<uint64_t, Bytes> pieces_;
  mutable std::atomic<uint64_t> bytes_read_{0};
};

Bytes full_box(std::string_view type, const Bytes& payload) {
  return box(type, cat({zeros(4), payload}));
}

// A complete one-track movie (tkhd, mdhd, hdlr, stbl) whose `count` samples of `size` bytes sit at `offsets`.
Bytes complete_moov(const Bytes& entry, const std::vector<uint64_t>& offsets, uint32_t size) {
  Bytes co64 = be(offsets.size(), 4);
  for (const uint64_t o : offsets) {
    co64 = cat({co64, be(o, 8)});
  }
  const Bytes stbl =
      cat({full_box("stsd", cat({be(1, 4), entry})), full_box("stts", cat({be(1, 4), be(offsets.size(), 4), be(1, 4)})),
           full_box("stsc", cat({be(1, 4), be(1, 4), be(1, 4), be(1, 4)})),
           full_box("stsz", cat({be(size, 4), be(offsets.size(), 4)})), full_box("co64", co64)});
  const Bytes tkhd = full_box("tkhd", cat({zeros(8), be(1, 4), zeros(4 + 4 + 8 + 8 + 36 + 8)}));
  const Bytes mdhd = full_box("mdhd", cat({zeros(8), be(1000, 4), zeros(4), be(0x55C4, 2), zeros(2)}));
  const Bytes hdlr = full_box("hdlr", cat({zeros(4), str("vide"), zeros(12), Bytes{0}}));
  return box("moov", box("trak", cat({tkhd, box("mdia", cat({mdhd, hdlr, box("minf", box("stbl", stbl))}))})));
}

struct Opened {
  ovt_File handle;
  ovt_Container* container = nullptr;
  explicit Opened(std::unique_ptr<ovt::io::RandomAccessFile> f) : handle{std::move(f)} {
    REQUIRE(ovt_container_open(&handle, &container) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;
  ~Opened() { ovt_container_close(container); }
};

// ovt_container_hdr_metadata on track `index`: the verified HdrMetadata and the colour struct.
Run hdr_metadata(ovt_Container* c, int index = 0) {
  ovt_Buffer b{};
  Run r;
  REQUIRE(ovt_container_hdr_metadata(c, index, nullptr, &b, &r.color) == OVT_OK);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::HdrMetadata>(nullptr));
  flatbuffers::GetRoot<ovt::fb::HdrMetadata>(b.data)->UnPackTo(&r.meta);
  delete[] b.data;  // §12.3.1 rule 3a
  return r;
}

Run hdr_metadata_of(std::string_view corpus_id) {
  const std::string path = ovt::test::corpus_or_skip(corpus_id).string();
  ovt_File* f = nullptr;
  REQUIRE(ovt_file_open(path.c_str(), &f) == OVT_OK);
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(f, &c) == OVT_OK);
  const Run r = hdr_metadata(c);
  ovt_container_close(c);
  ovt_file_close(f);
  return r;
}

TEST_CASE("AT4: a 2 GB file is probed with at most 16 MiB read", "[container][hdr]") {
  constexpr uint64_t kGiB = uint64_t{1} << 30U;
  constexpr uint32_t kSample = 5U << 20U;  // three 5 MiB sync samples fit the 16 MiB default, a fourth does not
  const Bytes ftyp = box("ftyp", cat({str("isom"), zeros(4), str("isom")}));
  const Bytes entry = visual_entry("hvc1", cat({hvcc(hevc_sps(9, 16, 9)), colr(9, 16, 9), dovi_box("dvvC", 8, 1)}));
  const uint64_t moov_size = complete_moov(entry, std::vector<uint64_t>(8, 0), kSample).size();
  const uint64_t mdat_at = ftyp.size() + moov_size;
  std::vector<uint64_t> offsets;
  offsets.reserve(8);
  for (uint64_t i = 0; i < 8; ++i) {
    offsets.push_back(mdat_at + 16 + i * (kGiB / 4));
  }
  const Bytes mdat_header = cat({be(1, 4), str("mdat"), be(2 * kGiB, 8)});
  const Bytes s0 = length_prefixed({hevc_sei({{4, hdr10plus_payload()}}), kRpu, kIdr});
  auto file = std::make_unique<SparseFile>(
      mdat_at + 2 * kGiB,
      std::map<uint64_t, Bytes>{
          {0, cat({ftyp, complete_moov(entry, offsets, kSample)})}, {mdat_at, mdat_header}, {offsets[0], s0}});
  const SparseFile* raw = file.get();
  Opened o(std::move(file));
  REQUIRE(ovt_container_track_count(o.container) == 1);
  const uint64_t before = raw->bytes_read();
  const Run r = hdr_metadata(o.container);
  CHECK(raw->bytes_read() - before <= 16U << 20U);
  CHECK(r.meta.probed_samples == 3);
  CHECK(r.meta.track_id == 1);
  CHECK(r.meta.kind == HdrKind::DolbyVision);
  REQUIRE(signal(r, "nal_62_rpu") != nullptr);
  CHECK(signal(r, "nal_62_rpu")->sample_index == 0);
  CHECK(r.color.has_hdr10plus == 1);
  CHECK(r.color.struct_size == sizeof(ovt_ColorDescription));
}

TEST_CASE("ovt_container_hdr_metadata: arguments, other formats and cancellation", "[container][hdr][c_api]") {
  ovt_Buffer b{};
  CHECK(ovt_container_hdr_metadata(nullptr, 0, nullptr, &b, nullptr) == OVT_E_INVALID_ARG);
  const Bytes entry = visual_entry("hvc1", colr(9, 18, 9));
  Opened o(std::make_unique<ovt::io::MemoryFile>(
      cat({box("ftyp", cat({str("isom"), zeros(4), str("isom")})), complete_moov(entry, {}, 0)})));
  CHECK(ovt_container_hdr_metadata(o.container, 0, nullptr, nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_hdr_metadata(o.container, 1, nullptr, &b, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_hdr_metadata(o.container, -1, nullptr, &b, nullptr) == OVT_E_INVALID_ARG);
  CHECK(b.data == nullptr);
  const Run r = hdr_metadata(o.container);  // out_color filled; no samples, so nothing is probed
  CHECK(r.meta.kind == HdrKind::HLG);
  CHECK(r.color.transfer == 18);
  CHECK(r.meta.probed_samples == 0);
  REQUIRE(ovt_container_hdr_metadata(o.container, 0, nullptr, &b, nullptr) == OVT_OK);  // out_color may be NULL
  delete[] b.data;

  ovt_Job* job = nullptr;
  REQUIRE(ovt_job_create(&job) == OVT_OK);
  ovt_job_cancel(job);
  CHECK(ovt_container_hdr_metadata(o.container, 0, job, &b, nullptr) == OVT_E_CANCELLED);
  ovt_job_free(job);
}

TEST_CASE("AT1: hdr_hdr10plus_dv81 is Dolby Vision 8.1 with HDR10+", "[container][hdr][needs-corpus]") {
  const Run r = hdr_metadata_of("hdr_hdr10plus_dv81");
  CHECK(r.meta.kind == HdrKind::DolbyVision);
  CHECK(r.meta.dv_profile == 8);
  CHECK(r.color.has_hdr10plus == 1);
  const auto* rpu = signal(r, "nal_62_rpu");
  REQUIRE(rpu != nullptr);
  CHECK(rpu->sample_index == 0);
  const auto* t35 = signal(r, "sei_t35_hdr10plus");
  REQUIRE(t35 != nullptr);
  CHECK(t35->sample_index == 0);
}

TEST_CASE("FR-CNT-102: syn_h265_main10_pq_hdr is HDR10 with MaxCLL 1000 / MaxFALL 400",
          "[container][hdr][needs-corpus]") {
  const Run r = hdr_metadata_of("syn_h265_main10_pq_hdr");
  CHECK(r.meta.kind == HdrKind::HDR10);
  CHECK(r.color.max_cll == 1000);
  CHECK(r.color.max_fall == 400);
  CHECK(r.color.transfer == 16);
}

}  // namespace
