// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 index models (chapter 7 §7.3.9, §7.3.10.1, WI-1.111): frame-boundary detection with the ten rules of
// H.264 §7.4.1.2.4, complementary field pairing, and the picture order count state machine for pic_order_cnt_type 0,
// 1 and 2 (H.264 §8.2.1) including memory_management_control_operation 5. They are plain state machines over the
// parsed headers of WI-1.108/1.109 so that WI-1.121 can wire them into the StreamIndex builder. WI-1.122 adds the
// reference model of §7.4.4 (H.264 §8.2.4 reference lists, §8.2.5 marking). Module-internal, not installed.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::index::h264 {

using syntax::Severity;
using syntax::h264::PictureStructure;

struct IndexDiagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
};

// ---- Frame boundary (§7.3.9) --------------------------------------------------------------------------------

/// True for the VCL nal_unit_types 1..5 and the extension slices 20 and 21.
[[nodiscard]] constexpr bool is_vcl(uint32_t nal_unit_type) noexcept {
  return (nal_unit_type >= 1 && nal_unit_type <= 5) || nal_unit_type == 20 || nal_unit_type == 21;
}

enum class UnitRole : uint8_t {
  Preceding,       // AUD, SPS, PPS, SEI, subset SPS, 16..18 (rule 10): belongs to the next picture's access unit
  Prefix,          // prefix NAL (14): belongs to the picture of the base-view slice that follows it
  Following,       // any other non-VCL unit (end of sequence / stream, filler, SPS extension, auxiliary slice, …)
  FirstSlice,      // starts a new primary coded picture
  NextSlice,       // continues the open picture
  RedundantSlice,  // redundant_pic_cnt > 0: belongs to the open picture (flagged in `derived`)
  PartitionBC,     // slice data partition B or C: belongs to the open picture (the picture of its partition A)
  ExtensionSlice,  // nal_unit_type 20 / 21 (MVC non-base view, SVC / 3D-AVC layer): belongs to the open access unit
};

/// The numbers of the §7.3.9 rules that fired, in order: at most ten, held inline so that a slice costs no heap
/// allocation (chapter 7 §7.4.6, WI-1.125). Compares with, and converts to, a std::vector<uint8_t>.
class RuleList {
 public:
  void push_back(uint8_t rule) { rules_.at(size_++) = rule; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] size_t size() const noexcept { return size_; }
  [[nodiscard]] const uint8_t* begin() const noexcept { return rules_.data(); }
  [[nodiscard]] const uint8_t* end() const noexcept { return std::next(begin(), static_cast<std::ptrdiff_t>(size_)); }
  [[nodiscard]] std::vector<uint8_t> to_vector() const { return {begin(), end()}; }
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): the natural view of the list
  operator std::vector<uint8_t>() const { return to_vector(); }
  friend bool operator==(const RuleList& a, const RuleList& b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }
  friend bool operator==(const RuleList& a, const std::vector<uint8_t>& b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }

 private:
  std::array<uint8_t, 10> rules_{};
  size_t size_ = 0;
};

struct BoundaryDecision {
  UnitRole role = UnitRole::Preceding;
  /// True when this unit starts a new access unit, i.e. the open picture ends before it. Set on the first rule-10
  /// unit after a VCL unit, or on a FirstSlice that no such unit preceded. Drivers call ActiveSets::end_picture().
  bool starts_access_unit = false;
  /// Decode index (from 0, counting primary coded pictures, one per field) of the picture a VCL unit belongs to;
  /// -1 for non-VCL units and for a partition B / C or extension slice with no open picture.
  int32_t picture_index = -1;
  /// FirstSlice: the picture's structure (Frame, TopField, BottomField; Mbaff for an MBAFF frame).
  PictureStructure structure = PictureStructure::Frame;
  /// FirstSlice: the decode index of the first field when this field completes a complementary field pair
  /// (`derived` "second field of frame-store F"), else -1.
  int32_t second_field_of = -1;
  /// FirstSlice: the numbers (1..10) of the §7.3.9 rules that started the picture; empty for the first picture and
  /// for the first picture after an end of sequence / stream.
  RuleList rules;
  /// NextSlice: Info SYN-H264-0060 when first_mb_in_slice is 0.
  std::vector<IndexDiagnostic> diagnostics;
};

class FrameBoundary {
 public:
  /// A non-VCL unit (nal_unit_type not in is_vcl()).
  [[nodiscard]] BoundaryDecision on_non_vcl(const syntax::h264::NalHeader& nal);
  /// A slice of nal_unit_type 1, 5 or 2 (partition A; `sh` is its slice header), or an extension slice (20, 21).
  /// `pic_order_cnt_type` is the active SPS's.
  [[nodiscard]] BoundaryDecision on_slice(const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                                          uint32_t pic_order_cnt_type);
  /// A slice data partition B or C (nal_unit_type 3, 4), or an extension slice whose header was not parsed.
  [[nodiscard]] BoundaryDecision on_slice_without_header(const syntax::h264::NalHeader& nal) const;

 private:
  struct Key {
    uint32_t frame_num = 0;
    uint32_t pic_parameter_set_id = 0;
    bool field_pic_flag = false;
    bool bottom_field_flag = false;
    bool reference = false;
    uint32_t pic_order_cnt_type = 0;
    uint32_t pic_order_cnt_lsb = 0;
    int32_t delta_pic_order_cnt_bottom = 0;
    std::array<int32_t, 2> delta_pic_order_cnt{};
    bool idr = false;
    uint32_t idr_pic_id = 0;
  };
  struct LastPicture {
    bool valid = false;
    int32_t index = -1;
    bool field = false;
    bool bottom = false;
    bool reference = false;
    bool idr = false;
    bool paired = false;  // already the second field of a pair, or its first field was paired
    uint32_t frame_num = 0;
  };

  bool picture_open_ = false;
  bool rule10_since_vcl_ = false;  // a rule-10 unit was seen since the last VCL unit of the open picture
  bool au_started_ = false;        // starts_access_unit was already reported for the access unit being collected
  Key prev_{};
  LastPicture last_{};
  int32_t next_index_ = 0;
};

// ---- POC (§7.3.10.1, H.264 §8.2.1) -----------------------------------------------------------------------------

inline constexpr int64_t kPocLimit = int64_t{1} << 30;

struct PocResult {
  PictureStructure structure = PictureStructure::Frame;  // Frame, TopField or BottomField
  bool has_top = false;
  bool has_bottom = false;
  int32_t top_field_order_cnt = 0;     // TopFieldOrderCnt (when has_top)
  int32_t bottom_field_order_cnt = 0;  // BottomFieldOrderCnt (when has_bottom)
  int32_t poc = 0;                     // PicOrderCnt(CurrPic), eq. 8-1: FrameInfo.poc
  // pic_order_cnt_type 0
  int32_t pic_order_cnt_msb = 0;
  int32_t prev_pic_order_cnt_msb = 0;  // the prevPicOrderCntMsb / Lsb this picture was derived from
  int32_t prev_pic_order_cnt_lsb = 0;
  // pic_order_cnt_type 1 and 2
  int32_t frame_num_offset = 0;
  int32_t prev_frame_num_offset = 0;  // the prevFrameNumOffset this picture was derived from
  /// memory_management_control_operation 5: the picture's order counts after the §8.2.1 adjustment
  /// (tempPicOrderCnt subtracted), which later pictures and the DPB see.
  bool mmco5 = false;
  int32_t top_after_mmco5 = 0;
  int32_t bottom_after_mmco5 = 0;
  /// A crafted stream drove an order count beyond ±2^30 and it was clamped so that POC arithmetic cannot overflow.
  bool poc_clamped = false;
};

class PocState {
 public:
  /// Derives the order counts of the picture whose first slice header is `sh` (unit header `nal`, active SPS `sps`)
  /// and advances the prev* state: for type 0 only reference pictures update it (a non-reference picture's lsb is
  /// never remembered); for types 1 and 2 every picture does.
  PocResult step(const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps);

 private:
  void advance(const PocResult& r, const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, bool reference,
               bool bottom_field);

  // type 0: values of the previous reference picture, already adjusted for its mmco5
  int64_t prev_msb_ = 0;
  int64_t prev_lsb_ = 0;
  // types 1, 2: values of the previous picture
  int64_t prev_frame_num_offset_ = 0;
  uint32_t prev_frame_num_ = 0;
};

// ---- DPB simulation (§7.4.4, H.264 §8.2.4 and §8.2.5), WI-1.122 ----------------------------------------------

/// Field of a frame store: index 0 is the top field, 1 the bottom field.
enum class Parity : uint8_t { Frame, Top, Bottom };

/// One entry of RefPicList0 / RefPicList1.
struct RefPic {
  int32_t decode_index = -1;  // the frame store's decode index (its first field); -1 for a non-existing frame
  Parity parity = Parity::Frame;
  int32_t poc = 0;  // PicOrderCnt of the frame, or of the field for a field entry
  bool long_term = false;
  bool non_existing = false;  // a frame inferred for a frame_num gap (H.264 §8.2.5.2)
};

struct DpbStepResult {
  /// The first slice's lists after modification, truncated to num_ref_idx_lX_active_minus1 + 1. An entry that is
  /// "no reference picture" (list shorter than the active size, or a modification naming a missing picture) is left
  /// out. Empty for I and SI slices (both lists) and for P and SP slices (list 1).
  std::array<std::vector<RefPic>, 2> ref_pic_list;
  std::vector<int32_t> declared_refs;  // unique decode indices of RefPicList0 then RefPicList1, non-existing dropped
  std::vector<int32_t> dpb_after;      // decode indices of the reference frame stores after marking, ascending
  std::vector<IndexDiagnostic> diagnostics;
};

class Dpb {
 public:
  /// Runs the §7.4.4 step for the picture `decode_index` whose first slice header is `sh` (unit header `nal`, active
  /// SPS `sps`, order counts `poc` from PocState). `second_field_of` is the decode index of the first field when the
  /// picture is the second field of a complementary field pair (FrameRec.second_field_of), else -1.
  DpbStepResult step(int32_t decode_index, const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                     const syntax::h264::Sps& sps, const PocResult& poc, int32_t second_field_of);

  /// Frame stores holding at least one field marked "used for reference" (non-existing frames included).
  [[nodiscard]] size_t reference_frames() const noexcept { return stores_.size(); }

  enum class Mark : uint8_t { Unused, Short, Long };
  struct FrameStore {
    int32_t decode_index = -1;  // first field or frame; -1 for a non-existing frame
    bool non_existing = false;
    uint32_t frame_num = 0;  // FrameNum (0 after memory_management_control_operation 5)
    int64_t frame_num_wrap = 0;
    int32_t long_term_frame_idx = -1;
    std::array<bool, 2> has{};  // the field was decoded into this store (top, bottom)
    std::array<Mark, 2> mark{Mark::Unused, Mark::Unused};
    std::array<int32_t, 2> poc{};  // TopFieldOrderCnt, BottomFieldOrderCnt
  };

 private:
  struct CurrentMarking {
    bool mmco5 = false;
    bool long_term = false;
    int32_t long_term_frame_idx = -1;
  };
  void fill_gap(const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, DpbStepResult& out);
  bool trim(size_t limit, size_t keep);
  void apply_mmco(const syntax::h264::MmcoOp& op, const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps,
                  size_t current, CurrentMarking& cm);
  void mark_current(const syntax::h264::SliceHeader& sh, const syntax::h264::Sps& sps, const PocResult& poc,
                    size_t current, DpbStepResult& out);
  void drop_unused();

  std::vector<FrameStore> stores_;        // reference frame stores, in insertion order
  int32_t max_long_term_frame_idx_ = -1;  // MaxLongTermFrameIdx; -1 is "no long-term frame indices"
  bool have_prev_ref_ = false;
  uint32_t prev_ref_frame_num_ = 0;  // PrevRefFrameNum (H.264 §7.4.3)
};

}  // namespace ovt::index::h264
