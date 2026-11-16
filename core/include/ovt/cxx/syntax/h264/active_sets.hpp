// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 parameter-set activation tracking (chapter 7 §7.3.8, WI-1.111): the sps[32], pps[256] and subset_sps[32]
// tables of one stream with the "replace before a picture / identical within a picture" rule (H.264 §7.4.1.2.1),
// the re-parse of a PPS that was parsed before its SPS, and the Table 7-2 scaling-list fall-back resolved at
// activation. It owns the ParameterSets that parse_slice_header() takes (issue #529). C++ helper header (D22):
// semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_ACTIVE_SETS_HPP
#define OVT_CXX_SYNTAX_H264_ACTIVE_SETS_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// A diagnostic raised by the activation rules; the caller attaches it to the unit or frame it concerns.
struct SetDiagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
};

/// The scaling matrices in effect for a picture after the Table 7-2 fall-back rules (H.264 §7.4.2.1.1, §7.4.2.2),
/// in the zig-zag order of ScalingList::list. Index i is the scaling-list index of Table 7-2: 0..5 are the 4x4
/// lists (Y/Cb/Cr intra, Y/Cb/Cr inter), 6..11 the 8x8 lists (Y intra, Y inter, Cb intra, Cb inter, Cr intra,
/// Cr inter). All entries are 16 for a flat matrix.
struct ScalingMatrices {
  std::array<std::array<uint8_t, 16>, 6> list4x4{};
  std::array<std::array<uint8_t, 64>, 6> list8x8{};
  bool flat = true;  // neither the SPS nor the PPS carries a matrix (Flat_4x4_16 / Flat_8x8_16)
};

/// Resolves Table 7-2 for the PPS `pps` activated with the SPS `sps` (fall-back rule A for the SPS lists and for the
/// PPS lists when the SPS has none, rule B for the PPS lists otherwise).
[[nodiscard]] ScalingMatrices resolve_scaling_matrices(const Sps& sps, const Pps& pps);

/// What a slice activates (§7.3.8): its PPS and the PPS's SPS (or subset SPS for nal_unit_type 20 / 21). The shared
/// pointers keep both alive after a later parameter set replaces them.
struct Activation {
  std::shared_ptr<const Pps> pps;
  std::shared_ptr<const Sps> sps;  // seq_parameter_set_data() of the subset SPS when `subset`
  bool subset = false;
  std::shared_ptr<const ScalingMatrices> scaling;  // resolved once per (PPS, SPS) generation pair
  uint64_t pps_unit = 0;                           // Unit.index each was received in (FrameInfo active_pps_unit)
  uint64_t sps_unit = 0;                           // (FrameInfo active_sps_unit)
  uint64_t pps_generation = 0;
  uint64_t sps_generation = 0;
};

/// The H.264 half of `ovt::syntax::ActiveSets`: sps[32], pps[256] and subset_sps[32], each holding the latest
/// received parsed struct, its Unit.index and a generation counter.
///
/// Typical use per NAL unit, in decode order:
///   * SPS / subset SPS / PPS NAL: parse it (a PPS against parameter_sets().sps), then put_sps() / put_subset_sps() /
///     put_pps() with the unit's RBSP, NAL header byte(s) included (kept for the identity check and the re-parse).
///   * Before parsing a slice header: parse_slice_header(.., sets.parameter_sets(diags)).
///   * After it: activate(), which opens the picture when the frame-boundary detector says the slice starts one.
///   * When the frame-boundary detector starts a new access unit: end_picture().
class ActiveSets {
 public:
  static constexpr unsigned kMaxSps = 32;
  static constexpr unsigned kMaxPps = 256;

  /// A repeat with identical RBSP bytes only updates the entry's Unit.index (its generation stays). A PPS received
  /// while an open picture uses the same id must repeat that picture's content (§7.4.1.2.1): a copy with different
  /// bytes is kept aside, replaces the entry at end_picture() and appends Error SYN-H264-0021 to `diags`.
  void put_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);
  /// `sps` is the seq_parameter_set_data() of a subset_seq_parameter_set_rbsp() (nal_unit_type 15).
  void put_subset_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);
  void put_pps(Pps pps, std::span<const uint8_t> rbsp, uint64_t unit_index, std::vector<SetDiagnostic>& diags);

  /// The tables parse_slice_header() takes. A PPS that was parsed provisionally (before its SPS), or against an SPS
  /// that has since been replaced, is re-parsed here against the current SPS (§7.3.8; its generation changes); a
  /// re-parse that throws keeps the earlier struct and appends the ParseError to `diags`. Pointers stay valid until
  /// the next put_*(), parameter_sets() or end_picture() call.
  [[nodiscard]] const ParameterSets& parameter_sets(std::vector<SetDiagnostic>& diags);

  /// Resolves the parameter sets of the slice `sh` of a unit with header `nal` (uses sh.pic_parameter_set_id and,
  /// for nal_unit_type 20 / 21, the subset SPS table). When `starts_picture` is set it first calls end_picture() and
  /// then opens the picture. Returns nullopt with Error SYN-H264-0010 in `diags` when the PPS or its SPS is missing
  /// (parse_slice_header() throws the same code first, so a driver that parsed the slice never sees it).
  [[nodiscard]] std::optional<Activation> activate(const NalHeader& nal, const SliceHeader& sh, bool starts_picture,
                                                   std::vector<SetDiagnostic>& diags);

  /// Closes the open picture and applies the parameter sets that were held back while it was open.
  void end_picture();

  [[nodiscard]] bool picture_open() const noexcept { return picture_open_; }
  [[nodiscard]] std::shared_ptr<const Sps> sps(unsigned id) const;
  [[nodiscard]] std::shared_ptr<const Sps> subset_sps(unsigned id) const;
  [[nodiscard]] std::shared_ptr<const Pps> pps(unsigned id) const;
  /// Generation counter of the entry (0 when empty); it changes whenever the entry is replaced or re-parsed.
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
    uint64_t parsed_with_sps_generation = 0;  // 0: provisional (no SPS when parsed)
    std::shared_ptr<const ScalingMatrices> scaling;
    uint64_t scaling_pps_generation = 0;
    uint64_t scaling_sps_generation = 0;
  };

  void put_seq(std::array<Entry<Sps>, kMaxSps>& table, std::array<Entry<Sps>, kMaxSps>& pending, Sps sps,
               std::span<const uint8_t> rbsp, uint64_t unit_index, int active_id);
  void reparse_stale_pps(std::vector<SetDiagnostic>& diags);
  void rebuild_tables();

  std::array<Entry<Sps>, kMaxSps> sps_{};
  std::array<Entry<Sps>, kMaxSps> subset_sps_{};
  std::array<PpsEntry, kMaxPps> pps_{};
  std::array<Entry<Sps>, kMaxSps> sps_pending_{};
  std::array<Entry<Sps>, kMaxSps> subset_sps_pending_{};
  std::array<PpsEntry, kMaxPps> pps_pending_{};
  ParameterSets tables_{};
  bool tables_dirty_ = true;
  bool picture_open_ = false;
  int active_pps_ = -1;
  int active_sps_ = -1;
  int active_subset_sps_ = -1;
  uint64_t next_generation_ = 1;
};

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_ACTIVE_SETS_HPP
