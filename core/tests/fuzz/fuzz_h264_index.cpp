// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.264 index chain (chapter 7 §7.3.8–§7.3.10.1, chapter 14 §14.7.1, WI-1.111): an Annex B byte
// stream goes through the unit scan, the NAL header / SPS / subset SPS / PPS / slice parsers, ActiveSets (including the
// PPS re-parse), FrameBoundary and PocState. Units whose parse throws ovt::ParseError are skipped as the index does.
// Invariants, each an abort when violated: only ovt::ParseError escapes; two runs over the same bytes give the same
// pictures (determinism, §0.7.5); picture indices count up from 0; every order count stays within ±kPocLimit.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "index/h264_index.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/active_sets.hpp"
#include "ovt/cxx/syntax/h264/data_partition.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

namespace h264 = ovt::syntax::h264;
namespace idx = ovt::index::h264;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

struct Pic {
  int32_t index = 0;
  int32_t poc = 0;
  int32_t second_field_of = -1;
  bool operator==(const Pic&) const = default;
};

class Chain {
 public:
  void add(std::span<const uint8_t> bytes) {
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(bytes);
    ovt::BitReader br(rbsp.rbsp);
    ovt::syntax::SyntaxBuilder b(br);
    b.set_recording(false);
    try {
      step(br, b, rbsp, unit_);
    } catch (const ovt::ParseError&) {
      // the index keeps the unit without a parsed header
    }
    (void)b.finish();
    ++unit_;
  }

  std::vector<Pic> pics;

 private:
  void step(ovt::BitReader& br, ovt::syntax::SyntaxBuilder& b, const ovt::Rbsp& rbsp, uint64_t unit) {
    const h264::NalHeader h = h264::parse_nal_header(br, b);
    const uint32_t t = h.nal_unit_type;
    if (!idx::is_vcl(t)) {
      if (fb_.on_non_vcl(h).starts_access_unit) {
        sets_.end_picture();
      }
      if (t == 7) {
        sets_.put_sps(h264::parse_sps(br, b), rbsp.rbsp, unit, diags_);
      } else if (t == 8) {
        sets_.put_pps(h264::parse_pps(br, b, sets_.parameter_sets(diags_).sps), rbsp.rbsp, unit, diags_);
      } else if (t == 15) {
        sets_.put_subset_sps(h264::parse_seq_parameter_set_data(br, b), rbsp.rbsp, unit, diags_);
      }
      return;
    }
    if (t == 3 || t == 4) {
      (void)fb_.on_slice_without_header(h);
      return;
    }
    const h264::ParameterSets& ps = sets_.parameter_sets(diags_);
    std::optional<h264::SliceHeader> sh;
    if (t == 2) {
      sh = h264::parse_slice_data_partition_a(br, b, h, ps).slice_header;
    } else {
      sh = h264::parse_slice_layer(br, b, h, ps);
    }
    if (!sh) {
      (void)fb_.on_slice_without_header(h);
      return;
    }
    const h264::Pps* pps = ps.pps.find(sh->pic_parameter_set_id);
    const h264::Sps* sps =
        pps != nullptr ? (sh->uses_subset_sps ? ps.subset_sps : ps.sps).find(pps->seq_parameter_set_id) : nullptr;
    require(sps != nullptr);  // parse_slice_header() throws SYN-H264-0010 otherwise
    const idx::BoundaryDecision bd = fb_.on_slice(h, *sh, sps->pic_order_cnt_type);
    const bool first = bd.role == idx::UnitRole::FirstSlice;
    const auto act = sets_.activate(h, *sh, first, diags_);
    require(act.has_value());
    if (!first) {
      return;
    }
    require(bd.picture_index == static_cast<int32_t>(pics.size()));
    require(bd.second_field_of < bd.picture_index);
    const idx::PocResult r = poc_.step(h, *sh, *act->sps);
    require(r.poc >= -idx::kPocLimit && r.poc <= idx::kPocLimit);
    require(r.top_field_order_cnt >= -idx::kPocLimit && r.top_field_order_cnt <= idx::kPocLimit);
    require(r.bottom_field_order_cnt >= -idx::kPocLimit && r.bottom_field_order_cnt <= idx::kPocLimit);
    pics.push_back({bd.picture_index, r.poc, bd.second_field_of});
  }

  h264::ActiveSets sets_;
  idx::FrameBoundary fb_;
  idx::PocState poc_;
  std::vector<h264::SetDiagnostic> diags_;
  uint64_t unit_ = 0;
};

std::vector<Pic> run(std::span<const uint8_t> data) {
  Chain c;
  ovt::syntax::scan_annexb(data, 0, ovt::syntax::kNoContainerSample, [&c](const ovt::syntax::ScannedUnit& u) {
    if (!u.bytes.empty()) {
      c.add(u.bytes);
    }
  });
  return c.pics;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> in(data, size);
  const std::vector<Pic> first = run(in);
  const std::vector<Pic> again = run(in);
  require(first == again);
  return 0;
}
