// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 index models (chapter 7 §7.3.9, §7.3.10.2, §7.4.4, WI-1.115): frame-boundary detection, POC derivation
// with prevTid0Pic, and the RPS / reference-marking / RefPicList model. They are plain state machines over the
// parsed headers of WI-1.112/1.113 so that WI-1.121/1.123 can wire them into the StreamIndex builder. Only
// base-layer pictures (nuh_layer_id == 0) go into PocState and Dpb; FrameBoundary tracks every layer.
// Module-internal, not installed.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::index::h265 {

using syntax::Severity;

struct IndexDiagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
};

// ---- Frame boundary (§7.3.9) --------------------------------------------------------------------------------

enum class UnitRole : uint8_t {
  Preceding,    // non-VCL unit that precedes the next picture's first VCL unit (AUD, parameter sets, prefix SEI, …)
  Following,    // non-VCL unit that closes the open access unit (suffix SEI, EOS, EOB, FD)
  FirstSlice,   // VCL unit with first_slice_segment_in_pic_flag == 1: starts a picture
  NextSlice,    // VCL unit that continues the open picture of its layer
  OrphanSlice,  // VCL unit with first_slice_segment_in_pic_flag == 0 and no open picture (SYN-H265-0060)
};

struct BoundaryDecision {
  UnitRole role = UnitRole::Preceding;
  /// True when this unit starts a new access unit, i.e. the open picture(s) end before it. Drivers call
  /// ActiveSets::end_picture() when it is set.
  bool starts_access_unit = false;
  /// True when the unit is an EOS NAL (36): the next picture of the layer has NoRaslOutputFlag = 1.
  bool is_eos = false;
  /// Set for OrphanSlice: the SYN-H265-0060 diagnostic to attach to the synthetic frame.
  std::vector<IndexDiagnostic> diagnostics;
};

class FrameBoundary {
 public:
  /// `first_slice_segment_in_pic_flag` is read from the slice header and only used for VCL units. Layers do not
  /// interrupt each other: each nuh_layer_id has its own open-picture flag.
  [[nodiscard]] BoundaryDecision on_unit(const syntax::h265::NalHeader& nal, bool first_slice_segment_in_pic_flag);

 private:
  std::array<bool, 64> picture_open_{};
  bool vcl_seen_in_au_ = false;  // a VCL NAL was seen since the last access-unit start
};

// ---- POC (§7.3.10.2, H.265 §8.3.1) ---------------------------------------------------------------------------

struct PocResult {
  int32_t poc = 0;  // PicOrderCntVal
  int32_t poc_msb = 0;
  bool no_rasl_output_flag = false;  // meaningful for IRAP pictures
  bool is_rap = false;               // IRAP (16..23)
  bool is_rasl = false;
  /// RASL picture whose associated IRAP has NoRaslOutputFlag == 1: not output (§8.1.3). Info SYN-H265-0101 is for the
  /// analysis path only (§7.3.10.2); the index keeps the picture as normal and only reports this flag.
  bool rasl_skipped = false;
  bool updates_prev_tid0 = false;
  /// The MSB left ±2^29 (only a crafted stream gets there) and was clamped so that POC arithmetic cannot overflow.
  bool poc_clamped = false;
};

class PocState {
 public:
  /// The next IRAP gets NoRaslOutputFlag = 1 (called after an EOS NAL; also the initial state).
  void on_eos() noexcept { next_no_rasl_ = true; }
  /// OVT sets HandleCraAsBlaFlag only for a CRA that is the first picture decoded after a seek.
  void set_handle_cra_as_bla(bool v) noexcept { handle_cra_as_bla_ = v; }

  /// Derives the POC of the picture whose first slice segment header is `sh` and advances prevTid0Pic.
  PocResult step(const syntax::h265::NalHeader& nal, const syntax::h265::SliceSegmentHeader& sh,
                 const syntax::h265::Sps& sps);

 private:
  bool next_no_rasl_ = true;  // the first picture in the bitstream
  bool handle_cra_as_bla_ = false;
  bool irap_no_rasl_output_ = false;  // NoRaslOutputFlag of the last IRAP
  int32_t prev_tid0_poc_ = 0;
};

// ---- RPS, marking and RefPicList (§7.4.4, H.265 §8.3.2 and §8.3.4) --------------------------------------------

inline constexpr int32_t kNoRef = -1;

struct DpbPicture {
  int32_t decode_index = 0;
  int32_t poc = 0;
  bool is_long_term = false;
};

/// The five RPS lists of the current picture as decode indices (kNoRef for "no reference picture") and their POCs.
struct RefPicSets {
  struct Entry {
    int32_t poc = 0;
    int32_t decode_index = kNoRef;
  };
  std::vector<Entry> st_curr_before, st_curr_after, st_foll, lt_curr, lt_foll;
};

struct DpbStepResult {
  RefPicSets sets;
  std::array<std::vector<int32_t>, 2> ref_pic_list;  // decode indices; kNoRef entries are "no reference picture"
  std::array<std::vector<int32_t>, 2> ref_pic_list_poc;
  std::vector<int32_t> declared_refs;  // unique decode indices of RefPicList0 then RefPicList1, kNoRef dropped
  std::vector<int32_t> dpb_after;      // ascending decode indices
  std::vector<IndexDiagnostic> diagnostics;
};

class Dpb {
 public:
  /// Runs the §7.4.4 step for the picture `decode_index` whose first slice segment header is `sh`. `poc` comes from
  /// PocState (its no_rasl_output_flag / rasl_skipped decide the marking and the diagnostic severity).
  DpbStepResult step(int32_t decode_index, const syntax::h265::NalHeader& nal,
                     const syntax::h265::SliceSegmentHeader& sh, const syntax::h265::Sps& sps,
                     const syntax::h265::Pps& pps, const PocResult& poc);

  /// Builds RefPicList0/1 of a later slice of the same picture from the picture's `sets`.
  [[nodiscard]] static DpbStepResult build_lists(const RefPicSets& sets, int32_t current_decode_index,
                                                 int32_t current_poc, const syntax::h265::SliceSegmentHeader& sh,
                                                 const syntax::h265::Pps& pps);

  [[nodiscard]] const std::vector<DpbPicture>& pictures() const noexcept { return dpb_; }

 private:
  std::vector<DpbPicture> dpb_;  // reference pictures only
};

}  // namespace ovt::index::h265
