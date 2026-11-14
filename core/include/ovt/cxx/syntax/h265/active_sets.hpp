// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 parameter-set activation tracking (chapter 7 §7.3.8, WI-1.115): the VPS/SPS/PPS tables of one stream with
// the "replace before a picture / identical within a picture" rule (H.265 §7.4.2.4.2), and the ActivePps tables
// (colBd, CtbAddrRsToTs, …) that need the SPS. It also owns the PpsTable that parse_slice_segment_header() takes
// (issue #391). C++ helper header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H265_ACTIVE_SETS_HPP
#define OVT_CXX_SYNTAX_H265_ACTIVE_SETS_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// A diagnostic raised by the activation rules; the caller attaches it to the unit or frame it concerns.
struct SetDiagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
};

/// What a slice segment activates (§7.3.8): the PPS with its tile tables, its SPS and, when present in the stream,
/// the SPS's VPS. The shared pointers keep the three alive even after a later parameter set replaces them.
struct Activation {
  std::shared_ptr<const ActivePps> active;  // tile tables; active->pps and active->sps point at *pps and *sps
  std::shared_ptr<const Pps> pps;
  std::shared_ptr<const Sps> sps;
  std::shared_ptr<const Vps> vps;  // null when the stream has no VPS (a Warning, not an error)
  uint64_t pps_unit = 0;           // Unit.index each was received in
  uint64_t sps_unit = 0;
  uint64_t vps_unit = 0;  // 0 when `vps` is null
};

/// The H.265 half of `ovt::syntax::ActiveSets`: vps[16], sps[16], pps[64], each holding the latest received parsed
/// struct, its Unit.index and a generation counter.
///
/// Typical use per unit, in decode order:
///   * VPS/SPS/PPS NAL: parse it, then put_vps()/put_sps()/put_pps() with the RBSP bytes (for the identity check).
///   * Before parsing a slice segment header: parse_slice_segment_header(.., sets.pps_table(), ..).
///   * After it: activate(), which opens the picture when first_slice_segment_in_pic_flag is 1.
///   * When the frame-boundary detector starts a new access unit: end_picture().
class ActiveSets {
 public:
  static constexpr unsigned kMaxVps = 16;
  static constexpr unsigned kMaxSps = 16;
  static constexpr unsigned kMaxPps = 64;

  /// A parameter set received while a picture that uses the same id is open must repeat that picture's content
  /// (§7.4.2.4.2). A copy with different RBSP bytes is kept aside and replaces the entry at end_picture(); the
  /// call appends Error SYN-H265-0021 to `diags`.
  void put_vps(Vps vps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);
  void put_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);
  void put_pps(Pps pps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);

  /// The table parse_slice_segment_header() takes: one ActivePps per stored PPS whose SPS has been received
  /// (a PPS that arrived before its SPS is activated lazily once the SPS is there). Pointers stay valid until the
  /// next put_*() or end_picture() call.
  [[nodiscard]] const PpsTable& pps_table();

  /// Resolves the parameter sets of a slice segment of PPS `pps_id`. When `first_slice_segment_in_pic_flag` is set it
  /// first calls end_picture() and then opens the picture. Returns nullopt with Error SYN-H265-0011 (PPS missing)
  /// or Warning SYN-GEN-0102 (the PPS's SPS missing) in `diags`. A missing VPS is a Warning SYN-H265-0022, once
  /// per SPS generation.
  [[nodiscard]] std::optional<Activation> activate(uint32_t pps_id, bool first_slice_segment_in_pic_flag,
                                                   std::vector<SetDiagnostic>& diags);

  /// Closes the open picture and applies the parameter sets that were held back while it was open.
  void end_picture();

  [[nodiscard]] bool picture_open() const noexcept { return picture_open_; }
  [[nodiscard]] std::shared_ptr<const Vps> vps(unsigned id) const;
  [[nodiscard]] std::shared_ptr<const Sps> sps(unsigned id) const;
  [[nodiscard]] std::shared_ptr<const Pps> pps(unsigned id) const;
  /// Generation counter of the entry (0 when empty); it changes whenever the entry is replaced.
  [[nodiscard]] uint64_t sps_generation(unsigned id) const;
  [[nodiscard]] uint64_t pps_generation(unsigned id) const;

 private:
  template <class T>
  struct Entry {
    std::shared_ptr<const T> value;
    std::vector<uint8_t> rbsp;
    uint64_t unit_index = 0;
    uint64_t generation = 0;
  };
  struct PpsEntry : Entry<Pps> {
    std::shared_ptr<const ActivePps> active;  // built lazily; aliases `holder`
    uint64_t built_pps_generation = 0;
    uint64_t built_sps_generation = 0;
  };

  void rebuild_table();

  std::array<Entry<Vps>, kMaxVps> vps_{};
  std::array<Entry<Sps>, kMaxSps> sps_{};
  std::array<PpsEntry, kMaxPps> pps_{};
  std::array<Entry<Vps>, kMaxVps> vps_pending_{};
  std::array<Entry<Sps>, kMaxSps> sps_pending_{};
  std::array<PpsEntry, kMaxPps> pps_pending_{};
  PpsTable table_{};
  bool table_dirty_ = true;
  bool picture_open_ = false;
  int active_pps_ = -1;
  int active_sps_ = -1;
  int active_vps_ = -1;
  uint64_t next_generation_ = 1;
  std::array<uint64_t, kMaxSps> vps_warned_for_sps_generation_{};
};

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_ACTIVE_SETS_HPP
