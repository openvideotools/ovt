// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.114: H.265 SEI framing, payload names, prefix/suffix placement and the buffering_period, pic_timing,
// recovery_point, active_parameter_sets and decoded_picture_hash payloads (part a), and time_code, the two user-data
// payloads, frame_packing_arrangement, display_orientation and the colour payloads 137, 144, 147 and 148 (part b);
// chapter 7 §7.3.5 rows 13–14.
//
// Fixtures: VPS, SPS and SEI NAL units rebuilt bit for bit from tests/golden/h265/<id>.trace_headers.json.gz (each
// element at its recorded offset and width, plus rbsp_trailing_bits and emulation prevention); expected values are
// the recorded ones, with the golden's unit number in each comment. Acceptance test 1 uses jctvc_VPSID_A_VIDYO_2 for
// the MD5 picture hash, because jctvc_AMP_A_Samsung_7 carries no SEI at all (its ovt-units golden lists NAL types 32,
// 33, 34, 19, 1 and 2 only), and acceptance test 3 uses jctvc_HRD_A_Fujitsu_3 and jctvc_CONFWIN_A_Sony_1, because
// jctvc_VPSID_A_VIDYO_2 carries no active_parameter_sets SEI, and acceptance test 2 uses syn_h265_hdr10plus
// (SPEC-ASSUMPTION #570). Payloads that no golden covers (CRC, multi-layer active_parameter_sets,
// irap_cpb_params_present_flag, time_code, frame_packing_arrangement, display_orientation,
// ambient_viewing_environment) are worked by hand from H.265 (08/2021) Annex D. The [needs-corpus] cases run the same
// checks on the real streams.

#include "ovt/cxx/syntax/h265/sei.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::test::h265::BitWriter;
using namespace ovt::syntax::h265;

using Bytes = std::vector<uint8_t>;

Bytes from_hex(std::string_view hex) {
  Bytes out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

// ---- fixtures rebuilt from the trace_headers goldens ------------------------------------------------------------

// jctvc_HRD_A_Fujitsu_3: VPS 0 (unit 0), SPS 0 (unit 1: NAL + VCL HRD with sub_pic_hrd_params_present_flag 1,
// initial_cpb_removal_delay length 16, au_cpb_removal_delay length 6, dpb_output_delay_du length 13,
// du_cpb_removal_delay_increment length 8, sub_pic_cpb_params_in_pic_timing_sei_flag 1).
constexpr std::string_view kHrdVps = "40010c01ffff0160000003000003000003000003003f970240";
constexpr std::string_view kHrdSps =
    "4201010160000003000003000003000003003fa00d080f1fe5979246d871792493f9e7f3cbffffff3f9fcfcfe778001f80041eb000cdfe603"
    "d88f62333ca5e0092402be014d0049300492015f00a68024990";
constexpr std::string_view kHrdAps = "4e0181010380";                                 // unit 3
constexpr std::string_view kHrdBp = "4e01001180afc8afc8a8aea8aeafc8afc8a8aea8ae80";  // unit 4
constexpr std::string_view kHrdPt0 = "4e01010800000300104008058380";                 // unit 5
constexpr std::string_view kHrdRecovery = "4e010601d080";                            // unit 6
constexpr std::string_view kHrdHash0 = "5001840d0200c0e69b0025fc1b0035073880";       // unit 11 (suffix)
constexpr std::string_view kHrdPt1 = "4e01010800000300110788433080";                 // unit 12

// jctvc_VPSID_A_VIDYO_2: SPS 0 of VPS 4 (unit 2) and the decoded_picture_hash suffix SEIs of the first three pictures
// (units 5, 7, 9).
constexpr std::string_view kVpsidSps =
    "4201440160000003000003000003000003005d0000a00d080f1fe5932e5e491b60817924922d4924917a9248924924bf9fe4ffcff99ffe3"
    "640";
constexpr std::array<std::string_view, 3> kVpsidHash{
    "500184310062f96a0c28c3740b6fdb08f950961e44800e3c37c9052dcde5ebef642e1f4ae4a7037896eeab270ddd2412c2aa3f7b9a80",
    "5003843100711f0fefb746b7c824f408a243a4000ea4a66e6303d9ca1de5b2fa955b860c2405beb89917f4fc3ea27fce1acd1d440f80",
    "5002843100769d06a0207834058a28ebeac3bd59be325365d740aaad8f1aba9303750dd8c07dcf31ee2c12ab54eb59c3016927194080",
};
// picture_md5[0..2][] of the three pictures as hex, from the byte values trace_headers prints (units 5, 7, 9).
constexpr std::array<std::array<std::string_view, 3>, 3> kVpsidMd5{{
    {"62f96a0c28c3740b6fdb08f950961e44", "800e3c37c9052dcde5ebef642e1f4ae4", "a7037896eeab270ddd2412c2aa3f7b9a"},
    {"711f0fefb746b7c824f408a243a4000e", "a4a66e6303d9ca1de5b2fa955b860c24", "05beb89917f4fc3ea27fce1acd1d440f"},
    {"769d06a0207834058a28ebeac3bd59be", "325365d740aaad8f1aba9303750dd8c0", "7dcf31ee2c12ab54eb59c30169271940"},
}};

// jctvc_CONFWIN_A_Sony_1: VPS 0 (unit 1), SPS 0 (unit 2: NAL + VCL HRD without sub-picture parameters,
// initial_cpb_removal_delay length 17, au_cpb_removal_delay / dpb_output_delay length 9, frame_field_info_present_flag
// 1).
constexpr std::string_view kConfVps = "40010c01ffff214000000300100000030000030078350300001f480003a980e8";
constexpr std::string_view kConfSps =
    "420101214000000300100000030000030078a00d080f1a49634d6493222a92f2ee0500001f480003a980f84e084080005b8d80002dc6c00009"
    "89680004c4b557080410";
constexpr std::string_view kConfAps = kHrdAps;                          // unit 4, the same bytes as HRD_A unit 3
constexpr std::string_view kConfBp = "4e01000aa00afc800002bf20000080";  // unit 5
constexpr std::string_view kConfPt0 = "4e010104080000c080";             // unit 6
constexpr std::string_view kConfRecovery = kHrdRecovery;                // unit 7, the same bytes as HRD_A unit 6
constexpr std::string_view kConfPt2 = "4e0101040801004080";             // unit 13

// syn_h265_hdr10plus: content_light_level_info (unit 3), mastering_display_colour_volume (unit 4, with emulation
// prevention) and the HDR10+ user_data_registered_itu_t_t35 (unit 6). syn_pat_hlg_patches: alternative_transfer_
// characteristics (unit 0).
constexpr std::string_view kHdrCll = "4e01900403e8019080";
constexpr std::string_view kHdrMdcv = "4e01891833c286c41d4c0bb884d03e803d13404200989680000003000180";
constexpr std::string_view kHdrT35 =
    "4e010440b5003c0001040140000c81388088b83a9800fa240800282800c8500190c807d1900fa2584e22d09c42f938831a71000042584b"
    "24663353366a0099acdccf9a0080";
constexpr std::string_view kHlgAtc = "4e0193011280";

// ---- plumbing ------------------------------------------------------------------------------------------------------

struct Msg {
  uint32_t type;
  Bytes payload;
};

void put_ff_coded(Bytes& out, uint32_t v) {
  while (v >= 255) {
    out.push_back(0xFF);
    v -= 255;
  }
  out.push_back(static_cast<uint8_t>(v));
}

/// A prefix (39) or suffix (40) SEI NAL unit holding `msgs` and rbsp_trailing_bits(); no emulation prevention.
Bytes sei_nal(const std::vector<Msg>& msgs, bool suffix = false) {
  Bytes nal{static_cast<uint8_t>((suffix ? kSuffixSeiNut : kPrefixSeiNut) << 1U), 0x01};
  for (const Msg& m : msgs) {
    put_ff_coded(nal, m.type);
    put_ff_coded(nal, static_cast<uint32_t>(m.payload.size()));
    nal.insert(nal.end(), m.payload.begin(), m.payload.end());
  }
  nal.push_back(0x80);
  return nal;
}

/// sei_payload() bits: payload_bit_equal_to_one and zero bits up to the byte boundary when not aligned.
Bytes payload_bytes(BitWriter w) {
  if (w.size_bits() % 8 != 0) {
    w.u(1, 1);
    while (w.size_bits() % 8 != 0) {
      w.u(1, 0);
    }
  }
  return w.bytes();
}

struct Parsed {
  SeiRbsp sei;
  SyntaxNodeBuffer tree;
  std::optional<ParseError> error;
};

Parsed parse_sei(const Bytes& nal, SeiContext& ctx, bool recording = true) {
  Parsed p;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    const NalHeader h = parse_nal_header(br, b);
    p.sei = parse_sei_rbsp(br, b, h, ctx);
  } catch (const ParseError& e) {
    p.error = e;
  }
  p.tree = b.finish();
  p.tree.set_root_name("nal_unit");
  return p;
}

/// Parameter sets of one stream, with the tables a SeiContext points at.
struct Env {
  std::optional<Vps> vps;
  std::optional<Sps> sps;
  std::array<const Vps*, 16> vps_table{};
  std::array<const Sps*, 16> sps_table{};
  SeiContext ctx;

  Env(const Bytes& vps_nal, const Bytes& sps_nal) {
    if (!vps_nal.empty()) {
      vps = ovt::test::h265::parse_nal(
          vps_nal, [](BitReader& br, SyntaxBuilder& b, const NalHeader&) { return parse_vps(br, b); });
      vps_table[vps->vps_video_parameter_set_id] = &*vps;
    }
    sps = ovt::test::h265::parse_nal(
        sps_nal, [](BitReader& br, SyntaxBuilder& b, const NalHeader& h) { return parse_sps(br, b, h.nuh_layer_id); });
    sps_table[sps->sps_seq_parameter_set_id] = &*sps;
    ctx.vps_by_id = vps_table;
    ctx.sps_by_id = sps_table;
    ctx.active_sps = &*sps;
  }
  Env(std::string_view vps_hex, std::string_view sps_hex) : Env(from_hex(vps_hex), from_hex(sps_hex)) {}
};

const SyntaxNodeData* find_node(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name && !n->array_element && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const auto* hit = find_node(c, name, array_index)) {
      return hit;
    }
  }
  return nullptr;
}

void collect_diags(const SyntaxNodeData* n, std::vector<std::string>& codes) {
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    codes.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    collect_diags(c, codes);
  }
}

std::vector<std::string> diag_codes(const Parsed& p) {
  std::vector<std::string> codes;
  if (p.tree.root() != nullptr) {
    collect_diags(p.tree.root(), codes);
  }
  return codes;
}

template <class T>
const T& only_payload(const Parsed& p) {
  REQUIRE(p.sei.messages.size() == 1);
  REQUIRE(p.sei.messages[0].parsed);
  REQUIRE(std::holds_alternative<T>(p.sei.messages[0].payload));
  return std::get<T>(p.sei.messages[0].payload);
}

// ---- shared checks (also run on the corpus streams) ----------------------------------------------------------------

// HRD_A unit 4 (buffering_period) and unit 5 (pic_timing) of the first access unit.
void check_hrd_a_first_au(const Parsed& bp_unit, const Parsed& pt_unit) {
  const auto& bp = only_payload<BufferingPeriod>(bp_unit);
  CHECK(bp.bp_seq_parameter_set_id == 0);
  CHECK_FALSE(bp.irap_cpb_params_present_flag);  // not sent: sub_pic_hrd_params_present_flag is 1
  CHECK(find_node(bp_unit.tree.root(), "irap_cpb_params_present_flag") == nullptr);
  CHECK_FALSE(bp.concatenation_flag);
  CHECK(bp.au_cpb_removal_delay_delta_minus1 == 0);
  CHECK(bp.alt_present);
  REQUIRE(bp.nal.size() == 1);
  REQUIRE(bp.vcl.size() == 1);
  for (const InitialCpbRemoval& e : {bp.nal[0], bp.vcl[0]}) {
    CHECK(e.initial_cpb_removal_delay == 45000);
    CHECK(e.initial_cpb_removal_offset == 45000);
    CHECK(e.initial_alt_cpb_removal_delay == 43182);
    CHECK(e.initial_alt_cpb_removal_offset == 43182);
  }
  CHECK_FALSE(bp.use_alt_cpb_params_flag.has_value());
  const SyntaxNodeData* delay = find_node(bp_unit.tree.root(), "nal_initial_cpb_removal_delay", 0);
  REQUIRE(delay != nullptr);
  CHECK(delay->span.bit_offset == 40);  // trace_headers: nal_initial_cpb_removal_delay[0] at bit 40, 16 bits
  CHECK(delay->span.bit_length == 16);
  CHECK(diag_codes(bp_unit).empty());

  const auto& pt = only_payload<PicTiming>(pt_unit);
  CHECK_FALSE(pt.frame_field_info_present);
  CHECK(pt.cpb_dpb_delays_present);
  CHECK(pt.au_cpb_removal_delay_minus1 == 0);
  CHECK(pt.pic_dpb_output_delay == 0);
  CHECK(pt.pic_dpb_output_du_delay == 0);
  CHECK(pt.du_info_present);
  CHECK(pt.num_decoding_units_minus1 == 3);
  CHECK_FALSE(pt.du_common_cpb_removal_delay_flag);
  CHECK(pt.num_nalus_in_du_minus1 == std::vector<uint32_t>{3, 0, 0, 0});
  CHECK(pt.du_cpb_removal_delay_increment_minus1 == std::vector<uint32_t>{0, 0, 96});
  const SyntaxNodeData* du = find_node(pt_unit.tree.root(), "du_cpb_removal_delay_increment_minus1", 2);
  REQUIRE(du != nullptr);
  CHECK(du->span.bit_offset == 86);  // trace_headers: du_cpb_removal_delay_increment_minus1[2] at bit 86
  CHECK(diag_codes(pt_unit).empty());
}

void check_vpsid_a_hash(const Parsed& p, size_t picture) {
  const auto& h = only_payload<DecodedPictureHash>(p);
  CHECK(p.sei.suffix);
  CHECK(h.hash_type == 0);
  CHECK(h.hash_type_name == "MD5");
  CHECK(h.num_components == 3);
  for (size_t c = 0; c < 3; ++c) {
    CHECK(Bytes(h.picture_md5[c].begin(), h.picture_md5[c].end()) == from_hex(kVpsidMd5[picture][c]));
  }
  CHECK(diag_codes(p).empty());
}

}  // namespace

// ---- acceptance test 1: picture hash and HRD timing ----------------------------------------------------------------

TEST_CASE("h265_sei_hrd_a_timing_and_recovery_point", "[syntax][h265][sei][fast]") {
  Env env(kHrdVps, kHrdSps);
  const auto bp = parse_sei(from_hex(kHrdBp), env.ctx);
  CHECK(env.ctx.buffering_period_sps_id == 0U);
  const auto pt = parse_sei(from_hex(kHrdPt0), env.ctx);
  check_hrd_a_first_au(bp, pt);
  const auto pt1 = parse_sei(from_hex(kHrdPt1), env.ctx);  // unit 12
  const auto& t = only_payload<PicTiming>(pt1);
  CHECK(t.num_decoding_units_minus1 == 3);
  CHECK(t.num_nalus_in_du_minus1 == std::vector<uint32_t>{0, 0, 0, 0});
  CHECK(t.du_cpb_removal_delay_increment_minus1 == std::vector<uint32_t>{7, 16, 12});
  const auto rp = parse_sei(from_hex(kHrdRecovery), env.ctx);  // unit 6
  const auto& r = only_payload<RecoveryPoint>(rp);
  CHECK(r.recovery_poc_cnt == 0);
  CHECK(r.exact_match_flag);
  CHECK_FALSE(r.broken_link_flag);
  CHECK(diag_codes(rp).empty());
}

TEST_CASE("h265_sei_hrd_a_checksum_hash", "[syntax][h265][sei][fast]") {
  Env env(kHrdVps, kHrdSps);
  const auto p = parse_sei(from_hex(kHrdHash0), env.ctx);  // unit 11
  const auto& h = only_payload<DecodedPictureHash>(p);
  CHECK(h.hash_type == 2);
  CHECK(h.hash_type_name == "checksum");
  CHECK(h.picture_checksum == std::array<uint32_t, 3>{12641947, 2489371, 3475256});
  CHECK(p.sei.messages[0].allowed);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h265_sei_vpsid_a_md5_first_three_pictures", "[syntax][h265][sei][fast]") {
  Env env(Bytes{}, from_hex(kVpsidSps));
  for (size_t i = 0; i < kVpsidHash.size(); ++i) {
    check_vpsid_a_hash(parse_sei(from_hex(kVpsidHash[i]), env.ctx), i);
  }
  const auto p = parse_sei(from_hex(kVpsidHash[0]), env.ctx);
  const SyntaxNodeData* md5 = find_node(p.tree.root(), "picture_md5", 2);
  REQUIRE(md5 != nullptr);
  CHECK(md5->span.bit_offset == 296);  // trace_headers: picture_md5[2][0] at bit 296
  CHECK(md5->span.bit_length == 128);
}

// ---- acceptance test 3: active_parameter_sets ----------------------------------------------------------------------

TEST_CASE("h265_sei_active_parameter_sets_name_the_stream_sets", "[syntax][h265][sei][fast]") {
  for (const auto& [vps, sps, aps] :
       {std::array{kHrdVps, kHrdSps, kHrdAps}, std::array{kConfVps, kConfSps, kConfAps}}) {
    Env env(vps, sps);
    const auto p = parse_sei(from_hex(aps), env.ctx);
    const auto& a = only_payload<ActiveParameterSets>(p);
    CHECK(a.active_video_parameter_set_id == env.vps->vps_video_parameter_set_id);
    REQUIRE(a.active_seq_parameter_set_id.size() == 1);
    CHECK(a.active_seq_parameter_set_id[0] == env.sps->sps_seq_parameter_set_id);
    CHECK_FALSE(a.self_contained_cvs_flag);
    CHECK_FALSE(a.no_parameter_set_update_flag);
    CHECK(a.vps_known);
    CHECK(a.layer_sps_idx.empty());  // one layer, base layer internal
    CHECK(diag_codes(p).empty());
  }
}

TEST_CASE("h265_sei_active_parameter_sets_layer_sps_idx", "[syntax][h265][sei][fast]") {
  Vps vps;
  vps.vps_video_parameter_set_id = 3;
  vps.vps_base_layer_internal_flag = false;
  vps.vps_max_layers_minus1 = 2;
  std::array<const Vps*, 16> table{};
  table[3] = &vps;
  SeiContext ctx;
  ctx.vps_by_id = table;
  BitWriter w;
  w.u(4, 3);  // active_video_parameter_set_id
  w.u(1, 1);  // self_contained_cvs_flag
  w.u(1, 0);  // no_parameter_set_update_flag
  w.ue(1);    // num_sps_ids_minus1
  w.ue(2);    // active_seq_parameter_set_id[0]
  w.ue(5);    // active_seq_parameter_set_id[1]
  w.ue(1);    // layer_sps_idx[0] (vps_base_layer_internal_flag 0: the loop starts at 0)
  w.ue(0);    // layer_sps_idx[1]
  w.ue(1);    // layer_sps_idx[2]
  const auto p = parse_sei(sei_nal({{129, payload_bytes(w)}}), ctx);
  const auto& a = only_payload<ActiveParameterSets>(p);
  CHECK(a.active_seq_parameter_set_id == std::vector<uint32_t>{2, 5});
  CHECK(a.first_layer == 0);
  CHECK(a.layer_sps_idx == std::vector<uint32_t>{1, 0, 1});
  CHECK(find_node(p.tree.root(), "layer_sps_idx", 2) != nullptr);
  CHECK(diag_codes(p).empty());

  // Without the VPS the loop count comes from the payload itself, with a Warning.
  SeiContext no_vps;
  const auto q = parse_sei(sei_nal({{129, payload_bytes(w)}}), no_vps);
  const auto& b = only_payload<ActiveParameterSets>(q);
  CHECK_FALSE(b.vps_known);
  CHECK(b.layer_sps_idx == std::vector<uint32_t>{1, 0, 1});
  CHECK(diag_codes(q) == std::vector<std::string>{"SYN-H265-0051"});
}

// ---- CONFWIN_A: frame-field info and a NAL + VCL HRD without sub-picture parameters ---------------------------------

TEST_CASE("h265_sei_confwin_a_timing", "[syntax][h265][sei][fast]") {
  Env env(kConfVps, kConfSps);
  const auto bp_unit = parse_sei(from_hex(kConfBp), env.ctx);  // unit 5
  const auto& bp = only_payload<BufferingPeriod>(bp_unit);
  CHECK_FALSE(bp.irap_cpb_params_present_flag);
  CHECK(bp.concatenation_flag);
  CHECK_FALSE(bp.alt_present);
  REQUIRE(bp.nal.size() == 1);
  REQUIRE(bp.vcl.size() == 1);
  CHECK(bp.nal[0].initial_cpb_removal_delay == 90000);
  CHECK(bp.nal[0].initial_cpb_removal_offset == 0);
  CHECK(bp.vcl[0].initial_cpb_removal_delay == 90000);
  CHECK(diag_codes(bp_unit).empty());

  struct Expected {
    std::string_view hex;
    uint32_t au_cpb_removal_delay_minus1;
    uint32_t pic_dpb_output_delay;
  };
  for (const Expected& e : {Expected{kConfPt0, 0, 1}, Expected{kConfPt2, 1, 0}}) {  // units 6 and 13
    const auto p = parse_sei(from_hex(e.hex), env.ctx);
    const auto& t = only_payload<PicTiming>(p);
    CHECK(t.frame_field_info_present);
    CHECK(t.pic_struct == 0);
    CHECK(t.pic_struct_name == "frame");
    CHECK(t.source_scan_type == 2);
    CHECK_FALSE(t.duplicate_flag);
    CHECK(t.au_cpb_removal_delay_minus1 == e.au_cpb_removal_delay_minus1);
    CHECK(t.pic_dpb_output_delay == e.pic_dpb_output_delay);
    CHECK_FALSE(t.du_info_present);
    CHECK(diag_codes(p).empty());
  }
  const auto rp = parse_sei(from_hex(kConfRecovery), env.ctx);  // unit 7
  CHECK(only_payload<RecoveryPoint>(rp).exact_match_flag);
}

// ---- hand-worked payloads (H.265 (08/2021) Annex D) -----------------------------------------------------------------

TEST_CASE("h265_sei_buffering_period_irap_params_and_extension", "[syntax][h265][sei][fast]") {
  Env env(kConfVps, kConfSps);  // 17-bit initial delays, 9-bit au_cpb_removal / dpb_output delays, no sub-pic
  BitWriter w;
  w.ue(0);                                  // bp_seq_parameter_set_id
  w.u(1, 1);                                // irap_cpb_params_present_flag
  w.u(9, 300);                              // cpb_delay_offset
  w.u(9, 7);                                // dpb_delay_offset
  w.u(1, 0);                                // concatenation_flag
  w.u(9, 2);                                // au_cpb_removal_delay_delta_minus1
  for (uint64_t hrd = 0; hrd < 2; ++hrd) {  // NAL then VCL, one CPB each, with the alt fields
    w.u(17, 1000 + hrd);
    w.u(17, 2000 + hrd);
    w.u(17, 3000 + hrd);
    w.u(17, 4000 + hrd);
  }
  w.u(1, 1);  // use_alt_cpb_params_flag (payload_extension_present())
  const auto p = parse_sei(sei_nal({{0, payload_bytes(w)}}), env.ctx);
  const auto& bp = only_payload<BufferingPeriod>(p);
  CHECK(bp.irap_cpb_params_present_flag);
  CHECK(bp.cpb_delay_offset == 300);
  CHECK(bp.dpb_delay_offset == 7);
  CHECK(bp.au_cpb_removal_delay_delta_minus1 == 2);
  CHECK(bp.alt_present);
  REQUIRE(bp.vcl.size() == 1);
  CHECK(bp.nal[0].initial_alt_cpb_removal_offset == 4000);
  CHECK(bp.vcl[0].initial_cpb_removal_delay == 1001);
  CHECK(bp.vcl[0].initial_alt_cpb_removal_delay == 3001);
  CHECK(bp.use_alt_cpb_params_flag == true);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h265_sei_decoded_picture_hash_crc_and_monochrome", "[syntax][h265][sei][fast]") {
  Env env(kConfVps, kConfSps);
  BitWriter crc;
  crc.u(8, 1);
  crc.u(16, 0xBEEF);
  crc.u(16, 0x1234);
  crc.u(16, 0x0001);
  const auto p = parse_sei(sei_nal({{132, payload_bytes(crc)}}, true), env.ctx);
  CHECK(only_payload<DecodedPictureHash>(p).picture_crc == std::array<uint32_t, 3>{0xBEEF, 0x1234, 0x0001});

  Sps mono = *env.sps;
  mono.chroma_format_idc = 0;
  SeiContext ctx;
  ctx.active_sps = &mono;
  BitWriter one;
  one.u(8, 2);
  one.u(32, 0xDEADBEEF);
  const auto q = parse_sei(sei_nal({{132, payload_bytes(one)}}, true), ctx);
  const auto& h = only_payload<DecodedPictureHash>(q);
  CHECK(h.num_components == 1);
  CHECK(h.picture_checksum[0] == 0xDEADBEEF);
  CHECK(diag_codes(q).empty());

  // A reserved hash_type reads no component: the rest is reserved_payload_extension_data.
  const auto r = parse_sei(sei_nal({{132, {7, 1, 2, 3}}}, true), env.ctx);
  CHECK(std::get<DecodedPictureHash>(r.sei.messages[0].payload).hash_type_name == "reserved");
  CHECK(diag_codes(r) == std::vector<std::string>{"SYN-H265-0052"});
  CHECK(find_node(r.tree.root(), "reserved_payload_extension_data") != nullptr);
}

// ---- part b: user data, picture and colour payloads ------------------------------------------------------------

// Acceptance test 2 (SPEC-ASSUMPTION #570: syn_h265_hdr10plus, an x265 encode with --master-display
// G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1) --max-cll 1000,400 --dhdr10-info): the
// MDCV, CLL and HDR10+ SEIs rebuilt from its trace_headers golden (units 3, 4 and 6); syn_h265_main10_pq_hdr carries
// the same bytes in its units 3 and 4.
TEST_CASE("h265_sei_hdr10plus_colour_volume_and_t35", "[syntax][h265][sei][fast]") {
  SeiContext ctx;
  const auto cll_unit = parse_sei(from_hex(kHdrCll), ctx);
  const auto& cll = only_payload<ContentLightLevelInfo>(cll_unit);
  CHECK(cll.max_content_light_level == 1000);
  CHECK(cll.max_pic_average_light_level == 400);
  CHECK(find_node(cll_unit.tree.root(), "content_light_level")->derived == "MaxCLL 1000 cd/m², MaxFALL 400 cd/m²");

  const auto mdcv_unit = parse_sei(from_hex(kHdrMdcv), ctx);
  const auto& m = only_payload<MasteringDisplayColourVolume>(mdcv_unit);
  CHECK(m.display_primaries_x == std::array<uint32_t, 3>{13250, 7500, 34000});  // G, B, R as coded
  CHECK(m.display_primaries_y == std::array<uint32_t, 3>{34500, 3000, 16000});
  CHECK(m.mdcv_primaries_x == std::array<uint32_t, 3>{34000, 13250, 7500});  // R, G, B
  CHECK(m.mdcv_primaries_y == std::array<uint32_t, 3>{16000, 34500, 3000});
  CHECK(m.white_point_x == 15635);
  CHECK(m.white_point_y == 16450);
  CHECK(m.max_display_mastering_luminance == 10000000);
  CHECK(m.min_display_mastering_luminance == 1);
  CHECK(m.text ==
        "R (0.680, 0.320) G (0.265, 0.690) B (0.150, 0.060) WP (0.3127, 0.3290) max 1000 cd/m² min 0.0001 cd/m²");
  CHECK(find_node(mdcv_unit.tree.root(), "mastering_display")->derived == m.text);

  const auto t35_unit = parse_sei(from_hex(kHdrT35), ctx);
  const auto& t = only_payload<UserDataRegisteredItuTT35>(t35_unit);
  CHECK(t.itu_t_t35_country_code == 181);
  CHECK(t.itu_t_t35_terminal_provider_code == 0x003C);
  CHECK(t.itu_t_t35_terminal_provider_oriented_code == 0x0001);
  CHECK(t.enum_name == "HDR10+ (SMPTE ST 2094-40)");
  CHECK(t.has_hdr10plus);
  CHECK_FALSE(t.has_dolby_vision);
  CHECK(t.payload_byte_count == 59);  // 64 bytes less country code, provider code and oriented code
  // Hand-decoded from payload bytes 5.. of the golden: application_identifier 4 (AT2), version 1, one window,
  // targeted_system_display_maximum_luminance 400, maxscl 40000 / 35000 / 30000.
  CHECK(t.hdr10plus_application_identifier == 4);
  CHECK(t.hdr10plus_application_version == 1);
  CHECK(t.hdr10plus_num_windows == 1);
  CHECK(t.hdr10plus_targeted_system_display_maximum_luminance == 400);
  CHECK(t.hdr10plus_maxscl == std::array<uint32_t, 3>{40000, 35000, 30000});
  CHECK(find_node(t35_unit.tree.root(), "itu_t_t35_terminal_provider_code")->enum_name == t.enum_name);
  for (const Parsed* p : {&cll_unit, &mdcv_unit, &t35_unit}) {
    CHECK(diag_codes(*p).empty());
  }
}

TEST_CASE("h265_sei_alternative_transfer_characteristics_hlg", "[syntax][h265][sei][fast]") {
  SeiContext ctx;
  const auto p = parse_sei(from_hex(kHlgAtc), ctx);
  CHECK(only_payload<AlternativeTransferCharacteristics>(p).preferred_transfer_characteristics == 18);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h265_sei_user_data_unregistered_x265", "[syntax][h265][sei][fast]") {
  // The UUID and the start of the option string of syn_h265_hdr10plus unit 5, shortened (the golden is 2 391 bytes).
  Bytes payload = from_hex("2ca2de09b51747dbbb55a4fe7fc2fc4e");
  const std::string text =
      "x265 (build 215) - 4.1:[Linux][GCC 13.3.0][64 bit] 10bit - H.265/HEVC codec - options: "
      "cpuid=1111039 frame-threads=1 wpp bitdepth=10";
  payload.insert(payload.end(), text.begin(), text.end());
  payload.push_back(0);
  SeiContext ctx;
  const auto p = parse_sei(sei_nal({{5, payload}}), ctx);
  const auto& u = only_payload<SeiUserDataUnregistered>(p);
  CHECK(u.uuid == "2CA2DE09-B517-47DB-BB55-A4FE7FC2FC4E");
  CHECK(u.enum_name == "x265 options");
  CHECK(u.text == text);
  CHECK(u.payload_byte_count == text.size() + 1);
  CHECK(find_node(p.tree.root(), "uuid_iso_iec_11578")->enum_name == "x265 options");
  CHECK(find_node(p.tree.root(), "x265_option[wpp]")->derived == "1");
  CHECK(find_node(p.tree.root(), "x265_option[bitdepth]")->derived == "10");
  CHECK(diag_codes(p).empty());
  // Shorter than its UUID: BR-0001, and the next message still parses.
  const auto q = parse_sei(sei_nal({{5, Bytes(10, 0x11)}, {144, {0x03, 0xE8, 0x01, 0x90}}}), ctx);
  REQUIRE(q.sei.messages.size() == 2);
  CHECK_FALSE(q.sei.messages[0].parsed);
  CHECK(q.sei.messages[1].parsed);
  CHECK(diag_codes(q) == std::vector<std::string>{"BR-0001"});
}

TEST_CASE("h265_sei_frame_packing_arrangement", "[syntax][h265][sei][fast]") {
  BitWriter w;
  w.ue(2);    // frame_packing_arrangement_id
  w.u(1, 0);  // cancel_flag
  w.u(7, 3);  // type: side by side
  w.u(1, 0);  // quincunx_sampling_flag
  w.u(6, 1);  // content_interpretation_type
  w.u(1, 1);  // spatial_flipping_flag
  w.u(1, 0);  // frame0_flipped_flag
  w.u(1, 0);  // field_views_flag
  w.u(1, 1);  // current_frame_is_frame0_flag
  w.u(1, 0);  // frame0_self_contained_flag
  w.u(1, 1);  // frame1_self_contained_flag
  w.u(4, 1);  // frame0_grid_position_x
  w.u(4, 2);  // frame0_grid_position_y
  w.u(4, 3);  // frame1_grid_position_x
  w.u(4, 4);  // frame1_grid_position_y
  w.u(8, 0);  // reserved_byte
  w.u(1, 1);  // persistence_flag
  w.u(1, 1);  // upsampled_aspect_ratio_flag
  SeiContext ctx;
  const auto p = parse_sei(sei_nal({{45, payload_bytes(w)}}), ctx);
  const auto& f = only_payload<FramePackingArrangement>(p);
  CHECK(f.frame_packing_arrangement_id == 2);
  CHECK(f.type == 3);
  CHECK(f.type_name == "side by side");
  CHECK(f.content_interpretation_type == 1);
  CHECK(f.spatial_flipping_flag);
  CHECK(f.current_frame_is_frame0_flag);
  CHECK(f.frame1_self_contained_flag);
  CHECK(f.grid_position == std::array<uint32_t, 4>{1, 2, 3, 4});
  CHECK(f.persistence_flag);
  CHECK(f.upsampled_aspect_ratio_flag);
  CHECK(diag_codes(p).empty());
  // Cancelled, then frame sequential (no grid positions).
  BitWriter c;
  c.ue(0);
  c.u(1, 1);  // cancel_flag
  c.u(1, 0);  // upsampled_aspect_ratio_flag
  BitWriter s;
  s.ue(1);
  s.u(1, 0);
  s.u(7, 5);  // frame sequential
  s.u(1, 0);
  s.u(6, 0);
  s.u(6, 0);  // the six flags
  s.u(8, 0);  // reserved_byte
  s.u(1, 0);  // persistence_flag
  s.u(1, 0);  // upsampled_aspect_ratio_flag
  const auto q = parse_sei(sei_nal({{45, payload_bytes(c)}, {45, payload_bytes(s)}}), ctx);
  REQUIRE(q.sei.messages.size() == 2);
  CHECK(std::get<FramePackingArrangement>(q.sei.messages[0].payload).cancel_flag);
  const auto& seq = std::get<FramePackingArrangement>(q.sei.messages[1].payload);
  CHECK(seq.type_name == "frame sequential");
  CHECK(find_node(q.tree.root(), "frame0_grid_position_x") == nullptr);
  CHECK(diag_codes(q).empty());
}

TEST_CASE("h265_sei_display_orientation_ambient_viewing_time_code", "[syntax][h265][sei][fast]") {
  BitWriter d;
  d.u(1, 0);        // display_orientation_cancel_flag
  d.u(1, 1);        // hor_flip
  d.u(1, 0);        // ver_flip
  d.u(16, 0x4000);  // anticlockwise_rotation: 90 degrees
  d.u(1, 1);        // display_orientation_persistence_flag
  BitWriter a;
  a.u(32, 3141592);  // ambient_illuminance: 314.1592 lux
  a.u(16, 15635);    // ambient_light_x (D65)
  a.u(16, 16450);    // ambient_light_y
  BitWriter t;
  t.u(2, 3);  // num_clock_ts
  // [0] full timestamp, n_frames 300 needs the ninth bit; time_offset_length 5, time_offset_value -3
  t.u(1, 1);
  t.u(1, 0);     // units_field_based_flag
  t.u(5, 4);     // counting_type
  t.u(1, 1);     // full_timestamp_flag
  t.u(1, 0);     // discontinuity_flag
  t.u(1, 1);     // cnt_dropped_flag
  t.u(9, 300);   // n_frames
  t.u(6, 59);    // seconds_value
  t.u(6, 30);    // minutes_value
  t.u(5, 23);    // hours_value
  t.u(5, 5);     // time_offset_length
  t.u(5, 0x1D);  // time_offset_value i(5) = -3
  // [1] no timestamp
  t.u(1, 0);
  // [2] seconds and minutes only, no time offset
  t.u(1, 1);
  t.u(1, 1);   // units_field_based_flag
  t.u(5, 0);   // counting_type
  t.u(1, 0);   // full_timestamp_flag
  t.u(1, 1);   // discontinuity_flag
  t.u(1, 0);   // cnt_dropped_flag
  t.u(9, 12);  // n_frames
  t.u(1, 1);   // seconds_flag
  t.u(6, 5);   // seconds_value
  t.u(1, 1);   // minutes_flag
  t.u(6, 2);   // minutes_value
  t.u(1, 0);   // hours_flag
  t.u(5, 0);   // time_offset_length
  SeiContext ctx;
  const auto p = parse_sei(sei_nal({{47, payload_bytes(d)}, {148, payload_bytes(a)}, {136, payload_bytes(t)}}), ctx);
  REQUIRE(p.sei.messages.size() == 3);
  CHECK(diag_codes(p).empty());
  const auto& o = std::get<DisplayOrientation>(p.sei.messages[0].payload);
  CHECK(o.hor_flip);
  CHECK_FALSE(o.ver_flip);
  CHECK(o.anticlockwise_rotation == 0x4000);
  CHECK(o.degrees == 90.0);
  CHECK(o.persistence_flag);
  CHECK(find_node(p.tree.root(), "rotation_degrees")->derived == "90 degrees anticlockwise");
  const auto& e = std::get<AmbientViewingEnvironment>(p.sei.messages[1].payload);
  CHECK(e.ambient_illuminance == 3141592);
  CHECK(e.ambient_light_x == 15635);
  CHECK(e.ambient_light_y == 16450);
  CHECK(find_node(p.tree.root(), "ambient_viewing_environment", -1)->first_child != nullptr);
  const auto& tc = std::get<TimeCode>(p.sei.messages[2].payload);
  REQUIRE(tc.num_clock_ts == 3);
  REQUIRE(tc.clock.size() == 3);
  CHECK(tc.clock[0].n_frames == 300);
  CHECK(tc.clock[0].counting_type == 4);
  CHECK(tc.clock[0].cnt_dropped_flag);
  CHECK(tc.clock[0].time_offset_value == -3);
  CHECK(tc.clock[0].text == "23:30:59:300");
  CHECK_FALSE(tc.clock[1].clock_timestamp_flag);
  CHECK(tc.clock[1].text.empty());
  CHECK(tc.clock[2].units_field_based_flag);
  CHECK(tc.clock[2].seconds_value == 5);
  CHECK(tc.clock[2].minutes_value == 2);
  CHECK_FALSE(tc.clock[2].hours_flag);
  CHECK(tc.clock[2].text == "00:02:05:12");
  // A cancelled display orientation is one bit.
  const auto c = parse_sei(sei_nal({{47, {0xC0}}}), ctx);
  CHECK(only_payload<DisplayOrientation>(c).cancel_flag);
  CHECK(diag_codes(c).empty());
}

// ---- framing, names, placement
// ---------------------------------------------------------------------------------------

TEST_CASE("h265_sei_payload_names", "[syntax][h265][sei][fast]") {
  CHECK(sei_payload_name(0) == "buffering_period");
  CHECK(sei_payload_name(132) == "decoded_picture_hash");
  CHECK(sei_payload_name(137) == "mastering_display_colour_volume");
  CHECK(sei_payload_name(148) == "ambient_viewing_environment");
  CHECK(sei_payload_name(205) == "shutter_interval_info");
  CHECK(sei_payload_name(7) == "reserved_sei_message");
  CHECK(sei_payload_name(153) == "reserved_sei_message");
  CHECK(sei_payload_name(206) == "reserved_sei_message");
  CHECK(sei_payload_allowed(132, true));
  CHECK_FALSE(sei_payload_allowed(132, false));
  CHECK(sei_payload_allowed(5, true));
  CHECK(sei_payload_allowed(5, false));
  CHECK_FALSE(sei_payload_allowed(0, true));
  CHECK(sei_payload_allowed(146, true));
  CHECK_FALSE(sei_payload_allowed(146, false));
}

TEST_CASE("h265_sei_misplaced_payloads_warn_and_still_parse", "[syntax][h265][sei][fast]") {
  Env env(kHrdVps, kHrdSps);
  // decoded_picture_hash in a prefix SEI
  auto hash = from_hex(kHrdHash0);
  hash[0] = static_cast<uint8_t>(kPrefixSeiNut << 1U);
  const auto p = parse_sei(hash, env.ctx);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H265-0050"});
  CHECK_FALSE(p.sei.messages[0].allowed);
  CHECK(only_payload<DecodedPictureHash>(p).hash_type == 2);
  // recovery_point in a suffix SEI; user_data_unregistered and a reserved type are fine in both
  const auto q = parse_sei(sei_nal({{6, {0xD0}}, {5, Bytes(16, 0x11)}, {250, {1, 2}}}, true), env.ctx);
  REQUIRE(q.sei.messages.size() == 3);
  CHECK(diag_codes(q) == std::vector<std::string>{"SYN-H265-0050"});
  CHECK(std::holds_alternative<RecoveryPoint>(q.sei.messages[0].payload));
  CHECK(q.sei.messages[1].allowed);
  CHECK(q.sei.messages[2].name == "reserved_sei_message");
  CHECK(q.sei.messages[2].allowed);
  CHECK(find_node(q.tree.root(), "reserved_sei_message") != nullptr);
}

TEST_CASE("h265_sei_payloads_without_a_parser_keep_their_bytes", "[syntax][h265][sei][fast]") {
  SeiContext ctx;
  const auto p = parse_sei(sei_nal({{2, {1, 2, 3}}, {300, Bytes(300, 0x55)}}), ctx);
  REQUIRE(p.sei.messages.size() == 2);
  CHECK(p.sei.messages[0].name == "pan_scan_rect");
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(p.sei.messages[1].payload_type == 300);
  CHECK(p.sei.messages[1].payload_size == 300);
  const SyntaxNodeData* bytes = find_node(p.tree.root(), "pan_scan_rect");
  REQUIRE(bytes != nullptr);
  CHECK(bytes->span.bit_length == 24);
  CHECK(p.sei.complete);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h265_sei_missing_parameter_sets", "[syntax][h265][sei][fast]") {
  SeiContext ctx;  // no VPS, no SPS
  const auto p = parse_sei(from_hex(kHrdBp), ctx);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H265-0051"});
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(find_node(p.tree.root(), "buffering_period")->span.bit_length == 17 * 8);
  CHECK_FALSE(ctx.buffering_period_sps_id.has_value());
  CHECK(diag_codes(parse_sei(from_hex(kHrdPt0), ctx)) == std::vector<std::string>{"SYN-H265-0051"});
  CHECK(diag_codes(parse_sei(from_hex(kHrdHash0), ctx)) == std::vector<std::string>{"SYN-H265-0051"});
  // recovery_point needs no SPS (only for its range check)
  CHECK(diag_codes(parse_sei(from_hex(kHrdRecovery), ctx)).empty());
}

TEST_CASE("h265_sei_pic_timing_uses_the_buffering_period_sps", "[syntax][h265][sei][fast]") {
  Env hrd(kHrdVps, kHrdSps);
  Env conf(kConfVps, kConfSps);
  // The previous picture used CONFWIN's SPS 0 layout, but this access unit's buffering_period names SPS 1 = HRD_A's.
  Sps hrd_as_1 = *hrd.sps;
  std::array<const Sps*, 16> table{};
  table[0] = &*conf.sps;
  table[1] = &hrd_as_1;
  SeiContext ctx;
  ctx.sps_by_id = table;
  ctx.active_sps = &*conf.sps;
  ctx.buffering_period_sps_id = 1;
  const auto p = parse_sei(from_hex(kHrdPt0), ctx);
  CHECK(only_payload<PicTiming>(p).du_cpb_removal_delay_increment_minus1 == std::vector<uint32_t>{0, 0, 96});
  // A suffix SEI belongs to the current picture: it ignores the buffering_period's SPS.
  ctx.active_sps = &*hrd.sps;
  ctx.buffering_period_sps_id = 0;
  const auto q = parse_sei(from_hex(kHrdHash0), ctx);
  CHECK(only_payload<DecodedPictureHash>(q).num_components == 3);
}

TEST_CASE("h265_sei_malformed_payload_does_not_reach_the_next_message", "[syntax][h265][sei][fast]") {
  Env env(kHrdVps, kHrdSps);
  BitWriter w;  // pic_timing whose num_decoding_units_minus1 exceeds PicSizeInCtbsY - 1
  w.u(6, 0);
  w.u(6, 0);
  w.u(13, 0);
  w.ue(100000);
  const auto p = parse_sei(sei_nal({{1, payload_bytes(w)}, {6, {0xD0}}}), env.ctx);
  REQUIRE(p.sei.messages.size() == 2);
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H265-0007"});
  CHECK(std::get<RecoveryPoint>(p.sei.messages[1].payload).exact_match_flag);

  // A payload that ends early: the read that crosses payloadSize fails, not the next message's bytes.
  const auto q = parse_sei(sei_nal({{129, {0x00}}, {6, {0xD0}}}), env.ctx);
  REQUIRE(q.sei.messages.size() == 2);
  CHECK(diag_codes(q) == std::vector<std::string>{"BR-0001"});
  CHECK(q.sei.messages[1].parsed);
}

TEST_CASE("h265_sei_recording_off_and_determinism", "[syntax][h265][sei][fast]") {
  Env on(kHrdVps, kHrdSps);
  Env off(kHrdVps, kHrdSps);
  const auto a = parse_sei(from_hex(kHrdPt0), on.ctx, true);
  const auto b = parse_sei(from_hex(kHrdPt0), off.ctx, false);
  CHECK(b.tree.empty());
  CHECK(only_payload<PicTiming>(a).du_cpb_removal_delay_increment_minus1 ==
        only_payload<PicTiming>(b).du_cpb_removal_delay_increment_minus1);
  // determinism (§0.7.5): the same bytes give the same tree
  const auto c = parse_sei(from_hex(kHrdBp), on.ctx);
  const auto d = parse_sei(from_hex(kHrdBp), on.ctx);
  CHECK(c.tree.to_flatbuffer() == d.tree.to_flatbuffer());
  CHECK(ovt::syntax::to_json(c.tree) == ovt::syntax::to_json(d.tree));
}

// ---- corpus ---------------------------------------------------------------------------------------------------------

namespace {

/// The NAL units of an Annex B stream, in order, up to `limit`.
std::vector<Bytes> first_units(std::string_view id, size_t limit) {
  const auto path = ovt::test::corpus_or_skip(id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  std::vector<Bytes> units;
  ovt::syntax::scan_annexb(*file, [&](const ovt::syntax::ScannedUnit& u) {
    if (units.size() < limit && u.bytes.size() >= 2) {
      units.emplace_back(u.bytes.begin(), u.bytes.end());
    }
  });
  return units;
}

}  // namespace

TEST_CASE("h265_sei_corpus_hrd_a_fujitsu_3", "[syntax][h265][sei][fast][needs-corpus]") {
  const auto units = first_units("jctvc_HRD_A_Fujitsu_3", 12);
  REQUIRE(units.size() == 12);
  Env env(units[0], units[1]);
  (void)parse_sei(units[3], env.ctx);
  const auto bp = parse_sei(units[4], env.ctx);
  const auto pt = parse_sei(units[5], env.ctx);
  check_hrd_a_first_au(bp, pt);
  const auto hash = parse_sei(units[11], env.ctx);
  CHECK(only_payload<DecodedPictureHash>(hash).picture_checksum[0] == 12641947);
}

TEST_CASE("h265_sei_corpus_vpsid_a_vidyo_2", "[syntax][h265][sei][fast][needs-corpus]") {
  const auto units = first_units("jctvc_VPSID_A_VIDYO_2", 10);
  REQUIRE(units.size() == 10);
  Env env(Bytes{}, units[2]);
  check_vpsid_a_hash(parse_sei(units[5], env.ctx), 0);
  check_vpsid_a_hash(parse_sei(units[7], env.ctx), 1);
  check_vpsid_a_hash(parse_sei(units[9], env.ctx), 2);
}
