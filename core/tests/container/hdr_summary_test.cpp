// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the HdrMetadata summary (chapter 08 §8.4.6, WI-1.223a): box-level collection, the classification rules
// 3.1-3.7, the box-level mismatch rules and the ovt_ColorDescription image, on hand-assembled files and on the corpus
// files of acceptance tests 2 and 3. Acceptance tests 1 and 4 need the sync-sample probe (WI-1.223b).

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/hdr_summary.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
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

TEST_CASE("AT2: hdr_hlg_bt2020 is HLG", "[container][hdr][needs-corpus]") {
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

}  // namespace
