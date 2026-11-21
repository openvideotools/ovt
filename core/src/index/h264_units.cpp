// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 dispatch of the StreamIndex builder (chapter 7 §7.4.2 S2–S6, WI-1.121): one NAL unit at a time through
// the §7.3.4 parsers, the parameter-set activation of §7.3.8, the frame-boundary rules of §7.3.9 and the picture
// order count of §7.3.10.1. WI-1.122 adds the DPB model of §7.4.4 (declared_refs, dpb_after). WI-1.125 adds the
// lazy trees of §7.4.5 and the lazy EPB stripping of §7.4.6.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "index/builder_internal.h"
#include "index/frame_classify.h"
#include "index/h264_index.h"
#include "index/lazy_syntax.h"
#include "ovt/cxx/syntax/h264/active_sets.hpp"
#include "ovt/cxx/syntax/h264/aud.hpp"
#include "ovt/cxx/syntax/h264/data_partition.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/prefix_nal.hpp"
#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h264/sps_ext.hpp"
#include "ovt/cxx/syntax/h264/subset_sps.hpp"

namespace ovt::index {

namespace {

using namespace syntax::h264;  // NOLINT(google-build-using-namespace): the §7.3.4 parsers

constexpr std::string_view kDiagNoParameterSets = "SYN-GEN-0102";   // Warning, §7.4.7
constexpr std::string_view kMissingParameterSet = "SYN-H264-0010";  // thrown by parse_slice_header()
constexpr std::string_view kReaderOverrun = "BR-0001";              // a read past the stripped prefix
// SPEC-ASSUMPTION(#736): Info code of §7.4.4 "later slices differ in num_ref_idx or modification ops".
constexpr std::string_view kListsDiffer = "SYN-H264-0107";

// What decides a slice's reference lists apart from the DPB (§7.4.4: the index follows the first slice).
struct ListParams {
  size_t lists = 0;  // 0 for I and SI slices, 1 for P and SP, 2 for B
  std::array<uint32_t, 2> active_minus1{};
  std::array<bool, 2> modified{};
  std::array<std::vector<std::array<uint32_t, 3>>, 2> ops;  // idc, abs_diff_pic_num_minus1, long_term_pic_num

  static ListParams of(const SliceHeader& sh) {
    ListParams p;
    const uint32_t t = sh.slice_type_mod5;
    p.lists = (t == kSliceI || t == kSliceSI) ? 0 : (t == kSliceB ? 2 : 1);
    const RefPicListModification& m = sh.ref_pic_list_modification;
    const std::array<const std::vector<RefPicListModificationOp>*, 2> src{&m.ops_l0, &m.ops_l1};
    const std::array<bool, 2> flag{m.ref_pic_list_modification_flag_l0, m.ref_pic_list_modification_flag_l1};
    const std::array<uint32_t, 2> active{sh.num_ref_idx_l0_active_minus1, sh.num_ref_idx_l1_active_minus1};
    for (size_t x = 0; x < p.lists; ++x) {
      p.active_minus1[x] = active[x];
      p.modified[x] = flag[x];
      for (const RefPicListModificationOp& op : *src[x]) {
        p.ops[x].push_back({op.modification_of_pic_nums_idc, op.abs_diff_pic_num_minus1, op.long_term_pic_num});
      }
    }
    return p;
  }
  friend bool operator==(const ListParams&, const ListParams&) = default;
};

fb::PictureStructure structure_of(h264::Parity p) {
  switch (p) {
    case h264::Parity::Top:
      return fb::PictureStructure::TopField;
    case h264::Parity::Bottom:
      return fb::PictureStructure::BottomField;
    case h264::Parity::Frame:
      break;
  }
  return fb::PictureStructure::Frame;
}

// vui_parameters() timing as the elementary-stream time base of chapter 6 §6.2.6: a frame is two ticks.
std::optional<StreamTiming> stream_timing(const Sps& sps) {
  const VuiParameters& v = sps.vui;
  constexpr auto kMax = static_cast<uint32_t>(INT32_MAX);
  if (!sps.vui_parameters_present_flag || !v.timing_info_present_flag || !v.fixed_frame_rate_flag ||
      v.num_units_in_tick == 0 || v.time_scale == 0 || v.num_units_in_tick > kMax || v.time_scale > kMax) {
    return std::nullopt;
  }
  return StreamTiming{static_cast<int32_t>(v.num_units_in_tick), static_cast<int32_t>(v.time_scale), 2};
}

class H264Dispatch final : public CodecDispatch {
 public:
  void on_unit(BuilderCore& core, uint32_t unit, std::span<const uint8_t> bytes) override {
    // §7.4.5 / §7.4.6 (WI-1.125): the tree is recorded as BuilderCore::record_full_tree says. A slice parsed for its
    // values only has its EPBs stripped lazily, 2 KiB at a time, until its header is in (S2); slice data
    // partitions B and C are placed before they are parsed, so they are stripped whole.
    const uint32_t peek_type = bytes.empty() ? 0U : (bytes[0] & 0x1FU);
    const bool vcl = h264::is_vcl(peek_type);
    // first_mb_in_slice == 0 (ue(v): a leading 1 bit) after the one-byte header, or the three further header bytes
    // of nal_unit_type 20 / 21, is the peek for "starts a picture"; partitions B / C continue one.
    const size_t header_bytes = peek_type == 20 || peek_type == 21 ? 4 : 1;
    const bool starts =
        vcl && peek_type != 3 && peek_type != 4 && bytes.size() > header_bytes && (bytes[header_bytes] & 0x80U) != 0;
    const bool record = core.record_full_tree(unit, vcl, starts);
    const bool lazy = vcl && !record && peek_type != 3 && peek_type != 4 && bytes.size() > core.lazy_strip_chunk;
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
    const NalHeader h = parse_nal_header(br, sb);  // S3
    UnitRec& u = core.units[unit];
    u.type = static_cast<uint8_t>(h.nal_unit_type);
    u.type_name = h.type_name;
    u.layer_id = static_cast<uint8_t>(h.layer_id);
    u.temporal_id = static_cast<uint8_t>(h.temporal_id);
    vcl_ = h264::is_vcl(h.nal_unit_type);
    if (vcl_) {
      slice(core, unit, br, sb, h);
    } else {
      non_vcl(core, unit, br, sb, h, rbsp);
    }
  }

  // One parse of the stripped bytes (S3, S4). Returns false when the header ran past a lazily stripped prefix
  // (BR-0001) and `may_extend` allows another attempt; parse() mutates no state before the slice header is in.
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
      if (vcl_ && err.code == kMissingParameterSet) {
        place_unknown(core, unit);
      }
      // Any other unit whose header did not parse belongs to no frame (S4 "continue with next unit").
    }
    core.store_tree(unit, sb.finish(), "nal_unit");
    return true;
  }

  // S4 for a non-VCL unit. It is placed before it is parsed, so that a parse error leaves it in its access unit.
  void non_vcl(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb, const NalHeader& h,
               std::span<const uint8_t> rbsp) {
    const h264::BoundaryDecision bd = boundary_.on_non_vcl(h);
    if (bd.starts_access_unit || (unknown_open_ && bd.role == h264::UnitRole::Preceding)) {
      end_access_unit(core);
    }
    if (bd.role == h264::UnitRole::Following && core.frame_open()) {
      core.extend_frame(unit);
    } else {
      core.add_pending(unit);
    }
    UnitRec& u = core.units[unit];
    switch (h.nal_unit_type) {
      case 6: {
        u.sei = true;
        const ParameterSets& ps = sets_.parameter_sets(set_diags_);
        SeiContext ctx;
        ctx.sps_by_id = ps.sps.entries;
        ctx.active_sps = last_sps_.get();
        ctx.buffering_period_sps_id = buffering_period_sps_id_;
        const SeiRbsp sei = parse_sei_rbsp(br, sb, ctx);
        buffering_period_sps_id_ = ctx.buffering_period_sps_id;
        for (const SeiMessage& m : sei.messages) {
          if (const auto* rp = std::get_if<RecoveryPoint>(&m.payload); rp != nullptr && rp->recovery_frame_cnt == 0) {
            recovery_point_ = rp->exact_match_flag;
          }
        }
        break;
      }
      case 7: {
        u.parameter_set = true;
        Sps sps = parse_sps(br, sb);
        if (!core.timing) {
          core.timing = stream_timing(sps);
        }
        sets_.put_sps(std::move(sps), rbsp, unit, set_diags_);
        break;
      }
      case 8:
        u.parameter_set = true;
        sets_.put_pps(parse_pps(br, sb, sets_.parameter_sets(set_diags_).sps), rbsp, unit, set_diags_);
        break;
      case 9:
        (void)parse_access_unit_delimiter(br, sb);
        break;
      case 10:
        (void)parse_end_of_seq(br, sb);
        break;
      case 11:
        (void)parse_end_of_stream(br, sb);
        break;
      case 12:
        (void)parse_filler_data(br, sb);
        break;
      case 13:
        u.parameter_set = true;
        (void)parse_sps_extension(br, sb);
        break;
      case 14:
        (void)parse_prefix_nal_unit(br, sb, h);
        break;
      case 15:
        u.parameter_set = true;
        sets_.put_subset_sps(parse_subset_sps(br, sb).sps, rbsp, unit, set_diags_);
        break;
      default:
        break;  // opaque (S4 "other")
    }
  }

  // S4 "VCL" and S5.
  void slice(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb, const NalHeader& h) {
    const uint32_t t = h.nal_unit_type;
    if (t == 3 || t == 4) {
      (void)boundary_.on_slice_without_header(h);
      join(core, unit);
      if (open_sps_ && open_pps_) {
        (void)parse_slice_data_partition_bc(br, sb, h, *open_sps_, *open_pps_);
      }
      return;
    }
    const ParameterSets& ps = sets_.parameter_sets(set_diags_);
    std::optional<SliceHeader> sh;
    if (t == 2) {
      sh = parse_slice_data_partition_a(br, sb, h, ps).slice_header;
    } else {
      sh = parse_slice_layer(br, sb, h, ps);
    }
    const Pps* pps = sh ? ps.pps.find(sh->pic_parameter_set_id) : nullptr;
    const Sps* sps =
        pps == nullptr ? nullptr : (t == 20 || t == 21 ? ps.subset_sps : ps.sps).find(pps->seq_parameter_set_id);
    if (!sh || sps == nullptr) {  // an SVC / 3D-AVC layer slice (kept opaque until WI-4.111)
      (void)boundary_.on_slice_without_header(h);
      join(core, unit);
      return;
    }
    h264::BoundaryDecision bd = boundary_.on_slice(h, *sh, sps->pic_order_cnt_type);
    for (h264::IndexDiagnostic& d : bd.diagnostics) {
      core.unit_diagnostic(unit, DiagRec{d.severity, std::move(d.code), std::move(d.message), BitSpan{0, 0}});
    }
    const bool first = bd.role == h264::UnitRole::FirstSlice;
    const std::optional<Activation> act = sets_.activate(h, *sh, first, set_diags_);
    if (!act) {
      return;  // parse_slice_header() found the sets, so this does not happen; the unit stays outside any frame
    }
    if (!first) {
      join(core, unit);
      if (bd.role == h264::UnitRole::NextSlice) {
        vote(core, *sh);
        check_lists(core, unit, *sh);
      }
      return;
    }
    open_picture(core, unit, h, *sh, *act, bd);
  }

  // S5 "new frame" and S6 for the picture that the slice `sh` starts.
  void open_picture(BuilderCore& core, uint32_t unit, const NalHeader& h, const SliceHeader& sh, const Activation& act,
                    const h264::BoundaryDecision& bd) {
    FrameRec& f = core.start_frame(unit);  // S5: closes the previous picture
    unknown_open_ = false;
    open_sps_ = act.sps;
    open_pps_ = act.pps;
    last_sps_ = act.sps;
    const h264::PocResult poc = poc_.step(h, sh, *act.sps);
    classify_h264(f, h, sh, *act.sps, bd, poc);  // S6
    f.slice_count = 1;
    // S9 sorts by POC inside a coded video sequence. An IDR picture starts one; a picture with
    // memory_management_control_operation 5 ends one, because the order counts restart after it (H.264 §8.2.1).
    const bool second_field = bd.second_field_of >= 0 && bd.second_field_of == last_picture_;
    if (second_field) {
      f.second_field_of = last_frame_;
    } else if ((sh.IdrPicFlag || mmco5_) && f.decode_index > 0) {
      ++sequence_;
    }
    f.sequence = sequence_;
    mmco5_ = poc.mmco5 || (second_field && mmco5_);
    last_picture_ = bd.picture_index;
    last_frame_ = f.decode_index;
    // §7.4.4: declared_refs and dpb_after from the first slice.
    h264::DpbStepResult refs = dpb_.step(f.decode_index, h, sh, *act.sps, poc, f.second_field_of);
    f.declared_refs = std::move(refs.declared_refs);
    f.dpb_after = std::move(refs.dpb_after);
    for (size_t x = 0; x < refs.ref_pic_list.size(); ++x) {
      for (const h264::RefPic& r : refs.ref_pic_list[x]) {
        f.ref_lists[x].push_back(RefSlotRec{r.decode_index, r.poc, structure_of(r.parity), r.long_term});
      }
    }
    for (h264::IndexDiagnostic& d : refs.diagnostics) {
      core.unit_diagnostic(unit, DiagRec{d.severity, std::move(d.code), std::move(d.message), BitSpan{0, 0}});
    }
    first_lists_ = ListParams::of(sh);
    lists_differ_ = false;
    // Recovery point RAP (S6): a recovery_point SEI with recovery_frame_cnt 0 preceded this picture.
    if (recovery_point_ && (*recovery_point_ || sh.slice_type_mod5 == kSliceI || sh.slice_type_mod5 == kSliceSI)) {
      f.is_rap = true;
    }
    recovery_point_.reset();
  }

  // A VCL unit that continues the open picture; without one it belongs to no frame.
  static void join(BuilderCore& core, uint32_t unit) {
    if (FrameRec* f = core.open_frame(); f != nullptr) {
      core.extend_frame(unit);
      ++f->slice_count;
    }
  }

  // S5 "majority vote": any B slice makes the picture B, else any P slice makes it P, else it is I.
  static void vote(BuilderCore& core, const SliceHeader& sh) {
    if (FrameRec* f = core.open_frame(); f != nullptr) {
      f->type = merge_frame_type(f->type, h264_slice_frame_type(sh));
    }
  }

  // SPEC-ASSUMPTION(#736): Info SYN-H264-0107 once per picture when a later slice's lists would differ.
  void check_lists(BuilderCore& core, uint32_t unit, const SliceHeader& sh) {
    if (lists_differ_ || !first_lists_ || ListParams::of(sh) == *first_lists_) {
      return;
    }
    lists_differ_ = true;
    core.unit_diagnostic(unit, DiagRec{syntax::Severity::Info, std::string(kListsDiffer),
                                       "slice reference lists differ from the first slice; declared_refs follow the "
                                       "first slice",
                                       BitSpan{0, 0}});
  }

  // SYN-GEN-0102: a slice whose parameter sets are missing is indexed as a frame of type Unknown. Its header is
  // not known, so the §7.3.9 rules cannot run: the frame ends at the next unit that may only precede a picture.
  void place_unknown(BuilderCore& core, uint32_t unit) {
    if (!warned_no_sets_) {
      warned_no_sets_ = true;
      core.index_diagnostic(syntax::Severity::Warning, kDiagNoParameterSets, "no parameter sets before first VCL unit");
    }
    if (unknown_open_ && core.frame_open()) {
      join(core, unit);
      return;
    }
    FrameRec& f = core.start_frame(unit);
    f.type = fb::FrameType::Unknown;
    f.slice_count = 1;
    f.sequence = sequence_;
    unknown_open_ = true;
    first_lists_.reset();
    open_sps_.reset();
    open_pps_.reset();
  }

  void end_access_unit(BuilderCore& core) {
    sets_.end_picture();
    core.finish_frame();
    unknown_open_ = false;
    open_sps_.reset();
    open_pps_.reset();
    buffering_period_sps_id_.reset();
  }

  LazyRbsp rbsp_;  // the unit being parsed, EPBs stripped (S2)
  ActiveSets sets_;
  h264::FrameBoundary boundary_;
  h264::PocState poc_;
  h264::Dpb dpb_;
  std::optional<ListParams> first_lists_;  // of the open picture's first slice
  bool lists_differ_ = false;              // SYN-H264-0107 already raised for the open picture
  std::vector<SetDiagnostic> set_diags_;
  std::shared_ptr<const Sps> open_sps_;  // the open picture's sets (partitions B and C)
  std::shared_ptr<const Pps> open_pps_;
  std::shared_ptr<const Sps> last_sps_;  // SPS of the previous picture (pic_timing SEI, D.2.2)
  std::optional<uint32_t> buffering_period_sps_id_;
  std::optional<bool> recovery_point_;  // exact_match_flag of a pending recovery point with recovery_frame_cnt 0
  uint32_t sequence_ = 0;
  int32_t last_picture_ = -1;  // FrameBoundary's index of the previous picture, and its decode index
  int32_t last_frame_ = -1;
  bool mmco5_ = false;  // the previous picture carried memory_management_control_operation 5
  bool vcl_ = false;    // the unit being parsed is a VCL unit
  bool unknown_open_ = false;
  bool warned_no_sets_ = false;
};

}  // namespace

std::unique_ptr<CodecDispatch> make_h264_dispatch() {
  return std::make_unique<H264Dispatch>();
}

}  // namespace ovt::index
