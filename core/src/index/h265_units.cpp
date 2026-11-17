// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 dispatch of the StreamIndex builder (chapter 7 §7.4.2 S2–S6, WI-1.121): one NAL unit at a time through
// the §7.3.5 parsers, the parameter-set activation of §7.3.8, the frame-boundary rule of §7.3.9 and the picture
// order count of §7.3.10.2. The RPS / DPB model joins with WI-1.123. WI-1.125 adds the lazy trees of §7.4.5 and
// the lazy EPB stripping of §7.4.6.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "index/builder_internal.h"
#include "index/frame_classify.h"
#include "index/h265_index.h"
#include "index/lazy_syntax.h"
#include "ovt/cxx/syntax/h265/active_sets.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"

namespace ovt::index {

namespace {

using namespace syntax::h265;  // NOLINT(google-build-using-namespace): the §7.3.5 parsers

constexpr std::string_view kDiagNoParameterSets = "SYN-GEN-0102";  // Warning, §7.4.7
constexpr std::string_view kMissingPps = "SYN-H265-0011";          // thrown by parse_slice_segment_header()
constexpr std::string_view kReaderOverrun = "BR-0001";             // a read past the stripped prefix

// vui_parameters() timing as the elementary-stream time base of chapter 6 §6.2.6: one clock tick per picture, or
// elemental_duration_in_tc_minus1 + 1 ticks when the VUI HRD fixes the picture rate.
std::optional<StreamTiming> stream_timing(const Sps& sps) {
  if (!sps.vui || !sps.vui->vui_timing_info_present_flag) {
    return std::nullopt;
  }
  const VuiParameters& v = *sps.vui;
  constexpr auto kMax = static_cast<uint32_t>(INT32_MAX);
  if (v.vui_num_units_in_tick == 0 || v.vui_time_scale == 0 || v.vui_num_units_in_tick > kMax ||
      v.vui_time_scale > kMax) {
    return std::nullopt;
  }
  return StreamTiming{static_cast<int32_t>(v.vui_num_units_in_tick), static_cast<int32_t>(v.vui_time_scale),
                      h265_ticks_per_picture(v)};
}

class H265Dispatch final : public CodecDispatch {
 public:
  void on_unit(BuilderCore& core, uint32_t unit, std::span<const uint8_t> bytes) override {
    // §7.4.5 / §7.4.6 (WI-1.125): the tree is recorded as BuilderCore::record_full_tree says; a slice segment parsed
    // for its values only has its EPBs stripped lazily, 2 KiB at a time, until its header is in (S2).
    const uint32_t peek_type = bytes.empty() ? 0U : ((bytes[0] >> 1U) & 0x3FU);
    const bool vcl = peek_type < 32;
    const bool starts = vcl && bytes.size() > 2 && (bytes[2] & 0x80U) != 0;  // first_slice_segment_in_pic_flag
    const bool record = core.record_full_tree(unit, vcl, starts);
    const bool lazy = vcl && !record && bytes.size() > core.lazy_strip_chunk;
    rbsp_.reset(bytes);
    for (size_t limit = lazy ? core.lazy_strip_chunk : bytes.size();; limit = std::min(limit * 2, bytes.size())) {
      rbsp_.extend(limit);
      if (attempt(core, unit, record, lazy && limit < bytes.size())) {
        break;
      }
      set_diags_.clear();  // the next attempt reports them again
    }
    rbsp_.scan_rest();
    for (SetDiagnostic& d : set_diags_) {
      core.unit_diagnostic(unit, DiagRec{d.severity, std::move(d.code), std::move(d.message), BitSpan{0, 0}});
    }
    set_diags_.clear();
    if (!rbsp_.epb_positions().empty()) {
      core.units[unit].epb_positions = rbsp_.epb_positions();
    }
  }

  void end_of_stream(BuilderCore& core) override {
    core.finish_frame();  // S8
  }

 private:
  void parse(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb,
             std::span<const uint8_t> rbsp) {
    vcl_ = false;
    header_ = parse_nal_header(br, sb);  // S3
    const NalHeader& h = header_;
    UnitRec& u = core.units[unit];
    u.type = h.nal_unit_type;
    u.type_name = nal_unit_type_name(h.nal_unit_type);
    u.layer_id = h.nuh_layer_id;
    u.temporal_id = h.TemporalId;
    vcl_ = h.is_vcl;
    // first_slice_segment_in_pic_flag is the first bit after the two header bytes, whatever the parameter sets say.
    first_flag_ = vcl_ && rbsp.size() > 2 && (rbsp[2] & 0x80U) != 0;
    if (!vcl_) {
      non_vcl(core, unit, br, sb, rbsp);
    } else if (h.nuh_layer_id != 0) {
      // A picture of an enhancement layer gets its own FrameInfo (§7.3.9); its slice segment header needs the
      // multilayer parameter sets, which phase 1 keeps opaque (SYN-H265-0004), so the picture stays unclassified.
      place_unknown(core, unit, false);
    } else {
      slice(core, unit, br, sb);
    }
  }

  // One parse of the stripped bytes (S3, S4). Returns false when the header ran past a lazily stripped prefix
  // (BR-0001) and `may_extend` allows another attempt; parse() mutates no state before a slice segment header is in.
  bool attempt(BuilderCore& core, uint32_t unit, bool record, bool may_extend) {
    BitReader br(rbsp_.rbsp());
    syntax::SyntaxBuilder sb(br);
    sb.set_recording(record);
    try {
      parse(core, unit, br, sb, rbsp_.rbsp());
    } catch (const ParseError& err) {
      if (may_extend && err.code == kReaderOverrun) {
        return false;
      }
      core.unit_diagnostic(unit, diag_from(err));
      if (vcl_ && err.code == kMissingPps) {
        if (!warned_no_sets_) {
          warned_no_sets_ = true;
          core.index_diagnostic(syntax::Severity::Warning, kDiagNoParameterSets,
                                "no parameter sets before first VCL unit");
        }
        place_unknown(core, unit, true);
      }
      // Any other unit whose header did not parse belongs to no frame (S4 "continue with next unit").
    }
    core.store_tree(unit, sb.finish(), "nal_unit");
    return true;
  }

  // S4 for a non-VCL unit. It is placed before it is parsed, so that a parse error leaves it in its access unit.
  void non_vcl(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb,
               std::span<const uint8_t> rbsp) {
    const NalHeader& h = header_;
    const h265::BoundaryDecision bd = boundary_.on_unit(h, false);
    if (bd.starts_access_unit) {
      sets_.end_picture();
      core.finish_frame();
      buffering_period_sps_id_.reset();
    }
    if (bd.is_eos) {
      poc_.on_eos();
    }
    if (bd.role == h265::UnitRole::Following && core.frame_open()) {
      core.extend_frame(unit);
    } else {
      core.add_pending(unit);
    }
    UnitRec& u = core.units[unit];
    const bool base = h.nuh_layer_id == 0;
    switch (h.nal_unit_type) {
      case kVpsNut:
        u.parameter_set = true;
        sets_.put_vps(parse_vps(br, sb), rbsp, unit, set_diags_);
        break;
      case kSpsNut: {
        u.parameter_set = true;
        Sps sps = parse_sps(br, sb, h.nuh_layer_id);
        if (base) {
          if (!core.timing) {
            core.timing = stream_timing(sps);
          }
          sets_.put_sps(std::move(sps), rbsp, unit, set_diags_);
        }
        break;
      }
      case kPpsNut: {
        u.parameter_set = true;
        Pps pps = parse_pps(br, sb);
        if (base) {
          sets_.put_pps(std::move(pps), rbsp, unit, set_diags_);
        }
        break;
      }
      case kPrefixSeiNut:
      case kSuffixSeiNut: {
        u.sei = true;
        std::array<std::shared_ptr<const Vps>, ActiveSets::kMaxVps> vps_keep;
        std::array<std::shared_ptr<const Sps>, ActiveSets::kMaxSps> sps_keep;
        std::array<const Vps*, ActiveSets::kMaxVps> vps{};
        std::array<const Sps*, ActiveSets::kMaxSps> sps{};
        for (unsigned i = 0; i < ActiveSets::kMaxVps; ++i) {
          vps_keep.at(i) = sets_.vps(i);
          vps.at(i) = vps_keep.at(i).get();
        }
        for (unsigned i = 0; i < ActiveSets::kMaxSps; ++i) {
          sps_keep.at(i) = sets_.sps(i);
          sps.at(i) = sps_keep.at(i).get();
        }
        SeiContext ctx;
        ctx.vps_by_id = vps;
        ctx.sps_by_id = sps;
        ctx.active_sps = last_sps_.get();
        ctx.buffering_period_sps_id = buffering_period_sps_id_;
        (void)parse_sei_rbsp(br, sb, h, ctx);
        buffering_period_sps_id_ = ctx.buffering_period_sps_id;
        break;
      }
      default:
        break;  // AUD, EOS, EOB, filler data and the reserved / unspecified types stay opaque (S4 "other")
    }
  }

  // S4 "VCL" and S5 for a base-layer slice segment.
  void slice(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb) {
    const NalHeader& h = header_;
    const SliceSegmentHeader sh =
        parse_slice_segment_header(br, sb, h, sets_.pps_table(), prev_independent_ ? &*prev_independent_ : nullptr);
    h265::BoundaryDecision bd = boundary_.on_unit(h, sh.first_slice_segment_in_pic_flag);
    report(core, unit, bd);
    if (!sh.dependent_slice_segment_flag) {
      prev_independent_ = sh;
    }
    if (bd.role == h265::UnitRole::NextSlice) {
      if (FrameRec* f = core.open_frame(); f != nullptr) {
        core.extend_frame(unit);
        ++f->slice_count;
        if (f->type != fb::FrameType::Unknown) {
          f->type = merge_frame_type(f->type, h265_slice_frame_type(sh));  // S5 "majority vote"
        }
      }
      return;
    }
    FrameRec& f = core.start_frame(unit);  // S5: closes the previous picture
    f.slice_count = 1;
    f.temporal_id = h.TemporalId;
    f.sequence = sequence_;
    f.type = fb::FrameType::Unknown;
    if (bd.role != h265::UnitRole::FirstSlice) {
      return;  // the synthetic frame of SYN-H265-0060
    }
    const std::optional<Activation> act = sets_.activate(sh.slice_pic_parameter_set_id, true, set_diags_);
    if (!act) {
      return;  // parse_slice_segment_header() found the sets, so this does not happen
    }
    last_sps_ = act->sps;
    const h265::PocResult poc = poc_.step(h, sh, *act->sps);
    // S9 sorts by POC inside a coded video sequence, which an IRAP picture with NoRaslOutputFlag = 1 starts.
    if (poc.is_rap && poc.no_rasl_output_flag && f.decode_index > 0) {
      f.sequence = ++sequence_;
    }
    classify_h265(f, h, sh, *act->sps, poc);  // S6
  }

  // A VCL unit whose slice segment header is not parsed: the first-segment flag alone places it. `base` is false
  // for a picture of an enhancement layer, which the base-layer display order does not contain.
  void place_unknown(BuilderCore& core, uint32_t unit, bool base) {
    const NalHeader& h = header_;
    h265::BoundaryDecision bd = boundary_.on_unit(h, first_flag_);
    report(core, unit, bd);
    if (bd.role == h265::UnitRole::NextSlice) {
      if (FrameRec* f = core.open_frame(); f != nullptr) {
        core.extend_frame(unit);
        ++f->slice_count;
      }
      return;
    }
    FrameRec& f = core.start_frame(unit);
    f.type = fb::FrameType::Unknown;
    f.slice_count = 1;
    f.temporal_id = h.TemporalId;
    f.layer_id = h.nuh_layer_id;
    f.sequence = sequence_;
    f.shown = base;
  }

  static void report(BuilderCore& core, uint32_t unit, h265::BoundaryDecision& bd) {
    for (h265::IndexDiagnostic& d : bd.diagnostics) {
      core.unit_diagnostic(unit, DiagRec{d.severity, std::move(d.code), std::move(d.message), BitSpan{0, 0}});
    }
  }

  LazyRbsp rbsp_;  // the unit being parsed, EPBs stripped (S2)
  ActiveSets sets_;
  h265::FrameBoundary boundary_;
  h265::PocState poc_;
  std::vector<SetDiagnostic> set_diags_;
  NalHeader header_;                                    // of the unit being parsed
  std::optional<SliceSegmentHeader> prev_independent_;  // §7.4.7.1: what a dependent slice segment copies from
  std::shared_ptr<const Sps> last_sps_;                 // SPS of the current or previous picture (SEI context)
  std::optional<uint32_t> buffering_period_sps_id_;
  uint32_t sequence_ = 0;
  bool vcl_ = false;         // the unit being parsed is a VCL unit
  bool first_flag_ = false;  // its first_slice_segment_in_pic_flag
  bool warned_no_sets_ = false;
};

}  // namespace

std::unique_ptr<CodecDispatch> make_h265_dispatch() {
  return std::make_unique<H265Dispatch>();
}

}  // namespace ovt::index
