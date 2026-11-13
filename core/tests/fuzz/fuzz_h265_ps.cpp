// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.265 header parsers (chapter 7 §7.3.5 rows 1–9, chapter 14 §14.7.1, WI-1.112). The low
// three bits of the first input byte select the structure parsed after nal_unit_header(): 0 profile_tier_level()
// (bits 3–5: max_num_sub_layers_minus1), 1 hrd_parameters() (bit 3: commonInfPresentFlag, bits 4–6: sub-layers),
// 2 vui_parameters() (bits 3–5: sub-layers, bits 6–7: ChromaArrayType), 3 scaling_list_data(), 4 st_ref_pic_set()
// (bits 3–7: num_short_term_ref_pic_sets, each earlier set parsed from the same bytes), 5 VPS, 6 SPS, 7 PPS; with
// bit 3 set on mode 7 the input is an SPS (second byte = length) followed by a PPS, which is activated against it.
// The remaining bytes are the NAL unit (header included) and go through rbsp_from_nal, so emulation prevention is
// exercised too. Invariants, each an abort when violated: only ovt::ParseError escapes a parser; the builder always
// finishes into a tree; a second parse of the same bytes yields the same JSON (determinism, §0.7.5); activate_pps
// never throws and its tile tables are empty or a permutation of the picture's CTBs.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
#include "ovt/cxx/syntax/h265/scaling_list.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
namespace h265 = ovt::syntax::h265;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

struct Parsed {
  std::optional<h265::Sps> sps;
  std::optional<h265::Pps> pps;
  bool ok = false;
  std::string json;
};

void parse_structure(BitReader& br, SyntaxBuilder& b, uint8_t sel, Parsed& out) {
  switch (sel & 7U) {
    case 0:
      (void)h265::parse_profile_tier_level(br, b, true, (sel >> 3) & 7U);
      break;
    case 1:
      (void)h265::parse_hrd_parameters(br, b, (sel & 8U) != 0, (sel >> 4) & 7U);
      break;
    case 2:
      (void)h265::parse_vui_parameters(br, b, (sel >> 3) & 7U, (sel >> 6) & 3U);
      break;
    case 3:
      (void)h265::parse_scaling_list_data(br, b);
      break;
    case 4: {
      const uint32_t num_sets = sel >> 3;
      std::vector<h265::StRefPicSet> sets;
      for (uint32_t i = 0; i <= num_sets; ++i) {
        sets.push_back(h265::parse_st_ref_pic_set(br, b, i, num_sets, sets, 15));
      }
      break;
    }
    case 5:
      (void)h265::parse_vps(br, b);
      break;
    case 6:
      out.sps = h265::parse_sps(br, b);
      break;
    default:
      out.pps = h265::parse_pps(br, b);
      break;
  }
}

// Parses one NAL unit as the selected structure. Any exception other than ParseError aborts.
Parsed parse_one(std::span<const uint8_t> nal, uint8_t sel) {
  Parsed out;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  try {
    (void)h265::parse_nal_header(br, b);
    parse_structure(br, b, sel, out);
    out.ok = true;
  } catch (const ovt::ParseError&) {
    out.ok = false;
  }
  SyntaxNodeBuffer tree = b.finish();
  out.json = ovt::syntax::to_json(tree);
  return out;
}

void check_activation(const h265::Pps& pps, const h265::Sps& sps) {
  const h265::ActivePps a = h265::activate_pps(pps, sps);
  require(a.CtbAddrRsToTs.size() == a.CtbAddrTsToRs.size());
  require(a.TileId.size() == a.CtbAddrRsToTs.size());
  if (a.CtbAddrRsToTs.empty()) {
    return;
  }
  for (size_t rs = 0; rs < a.CtbAddrRsToTs.size(); ++rs) {
    const uint32_t ts = a.CtbAddrRsToTs[rs];
    require(ts < a.CtbAddrTsToRs.size() && a.CtbAddrTsToRs[ts] == rs);
  }
  require(!a.colBd.empty() && !a.rowBd.empty());
  require(std::is_sorted(a.colBd.begin(), a.colBd.end()) && std::is_sorted(a.rowBd.begin(), a.rowBd.end()));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const std::span<const uint8_t> all(data, size);
  const uint8_t sel = all[0];
  if ((sel & 0xFU) != 0xFU) {
    const std::span<const uint8_t> nal = all.subspan(1);
    const Parsed first = parse_one(nal, sel);
    const Parsed again = parse_one(nal, sel);
    require(first.ok == again.ok && first.json == again.json);
    return 0;
  }
  const size_t sps_len = std::min<size_t>(all[1], size - 2);
  const Parsed sps = parse_one(all.subspan(2, sps_len), 6);
  const Parsed pps = parse_one(all.subspan(2 + sps_len), 7);
  if (sps.ok && pps.ok) {
    check_activation(*pps.pps, *sps.sps);
  }
  return 0;
}
