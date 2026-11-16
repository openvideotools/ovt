// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 parameter-set activation tracking (chapter 7 §7.3.8, WI-1.111). See ovt/cxx/syntax/h264/active_sets.hpp.

#include "ovt/cxx/syntax/h264/active_sets.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::syntax::h264 {

namespace {

// Registered in chapter 7 §7.9.
constexpr const char* kMissingSets = "SYN-H264-0010";
constexpr const char* kChangedInPicture = "SYN-H264-0021";

bool same_bytes(const std::vector<uint8_t>& a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Table 7-3 and Table 7-4 (H.264 §7.4.2.1.1), zig-zag order like ScalingList::list.
constexpr std::array<uint8_t, 16> kDefault4x4Intra = {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42};
constexpr std::array<uint8_t, 16> kDefault4x4Inter = {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34};
constexpr std::array<uint8_t, 64> kDefault8x8Intra = {6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
                                                      23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
                                                      27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
                                                      31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
constexpr std::array<uint8_t, 64> kDefault8x8Inter = {9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
                                                      21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
                                                      24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
                                                      27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

// One list of one level (SPS or PPS) of Table 7-2. `fallback_list` is what a list that is not present takes: the
// default of its class for lists 0, 3, 6, 7 under rule A, the sequence-level list under rule B, or the previous list
// of its class.
template <size_t N>
void resolve_list(const ScalingList& l, const std::array<uint8_t, N>& default_list,
                  const std::array<uint8_t, N>& fallback_list, std::array<uint8_t, N>& out) {
  if (l.present && !l.UseDefaultScalingMatrixFlag) {
    std::copy_n(l.list.begin(), N, out.begin());
  } else if (l.present) {
    out = default_list;
  } else {
    out = fallback_list;
  }
}

// One level (SPS or PPS) of Table 7-2. `fallback` is the list set a rule-B fall-back takes lists 0, 3, 6 and 7 from
// (the SPS level); nullptr selects rule A (the default matrices).
ScalingMatrices resolve_level(const std::array<ScalingList, 12>& lists, const ScalingMatrices* fallback) {
  ScalingMatrices m;
  m.flat = false;
  for (unsigned i = 0; i < 6; ++i) {
    const auto& def = i < 3 ? kDefault4x4Intra : kDefault4x4Inter;
    const bool first_of_class = i == 0 || i == 3;
    const auto& fb = !first_of_class ? m.list4x4[i - 1] : fallback != nullptr ? fallback->list4x4[i] : def;
    resolve_list(lists[i], def, fb, m.list4x4[i]);
  }
  for (unsigned j = 0; j < 6; ++j) {  // Table 7-2 index i = 6 + j; Cb from Y, Cr from Cb, same intra/inter class
    const auto& def = (j % 2) == 0 ? kDefault8x8Intra : kDefault8x8Inter;
    const auto& fb = j >= 2 ? m.list8x8[j - 2] : fallback != nullptr ? fallback->list8x8[j] : def;
    resolve_list(lists[6 + j], def, fb, m.list8x8[j]);
  }
  return m;
}

ScalingMatrices flat_matrices() {
  ScalingMatrices m;
  for (auto& l : m.list4x4) {
    l.fill(16);
  }
  for (auto& l : m.list8x8) {
    l.fill(16);
  }
  m.flat = true;
  return m;
}

}  // namespace

ScalingMatrices resolve_scaling_matrices(const Sps& sps, const Pps& pps) {
  const ScalingMatrices seq =
      sps.seq_scaling_matrix_present_flag ? resolve_level(sps.scaling_list, nullptr) : flat_matrices();
  if (!pps.pic_scaling_matrix_present_flag) {
    return seq;
  }
  // Rule A when the SPS has no matrix, rule B (lists 0, 3, 6, 7 from the sequence level) otherwise.
  return resolve_level(pps.scaling_list, sps.seq_scaling_matrix_present_flag ? &seq : nullptr);
}

void ActiveSets::put_seq(std::array<Entry<Sps>, kMaxSps>& table, std::array<Entry<Sps>, kMaxSps>& pending, Sps sps,
                         std::span<const uint8_t> rbsp, uint64_t unit_index, int active_id) {
  const unsigned id = sps.seq_parameter_set_id;
  if (id >= kMaxSps) {
    return;
  }
  Entry<Sps>& e = table[id];
  if (e.value && same_bytes(e.rbsp, rbsp)) {
    e.unit_index = unit_index;  // a repeat: the entry keeps its generation
    pending[id] = {};
    return;
  }
  Entry<Sps> fresh{std::make_shared<const Sps>(std::move(sps)), std::vector<uint8_t>(rbsp.begin(), rbsp.end()),
                   unit_index, next_generation_++};
  // SPEC-ASSUMPTION(#568): an SPS whose content changes while a picture that uses it is open is held back to the
  // next picture like a PPS (§7.4.1.2.1 forbids it); no diagnostic code is registered for the SPS case.
  if (picture_open_ && active_id == static_cast<int>(id) && e.value) {
    pending[id] = std::move(fresh);
    return;
  }
  e = std::move(fresh);
  pending[id] = {};
  tables_dirty_ = true;
}

void ActiveSets::put_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                         std::vector<SetDiagnostic>& /*diags*/) {
  put_seq(sps_, sps_pending_, std::move(sps), rbsp, unit_index, active_sps_);
}

void ActiveSets::put_subset_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                                std::vector<SetDiagnostic>& /*diags*/) {
  put_seq(subset_sps_, subset_sps_pending_, std::move(sps), rbsp, unit_index, active_subset_sps_);
}

void ActiveSets::put_pps(Pps pps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                         std::vector<SetDiagnostic>& diags) {
  const unsigned id = pps.pic_parameter_set_id;
  if (id >= kMaxPps) {
    return;
  }
  PpsEntry& e = pps_[id];
  if (e.value && same_bytes(e.rbsp, rbsp)) {
    e.unit_index = unit_index;
    pps_pending_[id] = {};
    return;
  }
  // The caller parsed the PPS against parameter_sets().sps, i.e. the current entry of its SPS (or none).
  const unsigned sps_id = pps.seq_parameter_set_id;
  const uint64_t parsed_with =
      (pps.provisional || sps_id >= kMaxSps || !sps_[sps_id].value) ? 0 : sps_[sps_id].generation;
  PpsEntry fresh;
  fresh.value = std::make_shared<const Pps>(std::move(pps));
  fresh.rbsp.assign(rbsp.begin(), rbsp.end());
  fresh.unit_index = unit_index;
  fresh.generation = next_generation_++;
  fresh.parsed_with_sps_generation = parsed_with;
  if (picture_open_ && active_pps_ == static_cast<int>(id) && e.value) {
    diags.push_back({Severity::Error, kChangedInPicture, fmt::format("PPS {} changed within a picture", id)});
    pps_pending_[id] = std::move(fresh);
    return;
  }
  e = std::move(fresh);
  pps_pending_[id] = {};
  tables_dirty_ = true;
}

void ActiveSets::reparse_stale_pps(std::vector<SetDiagnostic>& diags) {
  for (unsigned id = 0; id < kMaxPps; ++id) {
    PpsEntry& e = pps_[id];
    if (!e.value) {
      continue;
    }
    const unsigned sps_id = e.value->seq_parameter_set_id;
    if (sps_id >= kMaxSps) {
      continue;
    }
    // The SPS the PPS refers to; a PPS used only by MVC views may refer to a subset SPS (H.7.4.1.2.1).
    const Entry<Sps>& s = sps_[sps_id].value ? sps_[sps_id] : subset_sps_[sps_id];
    if (!s.value || e.parsed_with_sps_generation == s.generation) {
      continue;
    }
    // Never re-parse the PPS of the open picture: its content is fixed until end_picture().
    if (picture_open_ && active_pps_ == static_cast<int>(id)) {
      continue;
    }
    SpsTable one;
    one.entries[sps_id] = s.value.get();
    BitReader br(e.rbsp);
    SyntaxBuilder b(br);
    b.set_recording(false);
    try {
      (void)parse_nal_header(br, b);
      Pps again = parse_pps(br, b, one);
      e.value = std::make_shared<const Pps>(std::move(again));
      e.generation = next_generation_++;
    } catch (const ParseError& err) {
      diags.push_back({Severity::Error, err.code, err.what()});
    }
    (void)b.finish();
    e.parsed_with_sps_generation = s.generation;  // one attempt per SPS generation
    tables_dirty_ = true;
  }
}

void ActiveSets::rebuild_tables() {
  for (unsigned id = 0; id < kMaxSps; ++id) {
    tables_.sps.entries[id] = sps_[id].value.get();
    tables_.subset_sps.entries[id] = subset_sps_[id].value.get();
  }
  for (unsigned id = 0; id < kMaxPps; ++id) {
    tables_.pps.entries[id] = pps_[id].value.get();
  }
  tables_dirty_ = false;
}

const ParameterSets& ActiveSets::parameter_sets(std::vector<SetDiagnostic>& diags) {
  reparse_stale_pps(diags);
  if (tables_dirty_) {
    rebuild_tables();
  }
  return tables_;
}

std::optional<Activation> ActiveSets::activate(const NalHeader& nal, const SliceHeader& sh, bool starts_picture,
                                               std::vector<SetDiagnostic>& diags) {
  if (starts_picture) {
    end_picture();
  }
  const uint32_t pps_id = sh.pic_parameter_set_id;
  if (pps_id >= kMaxPps || !pps_[pps_id].value) {
    diags.push_back({Severity::Error, kMissingSets, fmt::format("slice references missing PPS {}", pps_id)});
    return std::nullopt;
  }
  if (!picture_open_ || active_pps_ != static_cast<int>(pps_id)) {
    reparse_stale_pps(diags);
  }
  PpsEntry& pe = pps_[pps_id];
  const unsigned sps_id = pe.value->seq_parameter_set_id;
  const bool subset = nal.nal_unit_type == 20 || nal.nal_unit_type == 21;
  const auto& table = subset ? subset_sps_ : sps_;
  if (sps_id >= kMaxSps || !table[sps_id].value) {
    diags.push_back(
        {Severity::Error, kMissingSets,
         fmt::format("slice references PPS {} whose {}SPS {} is missing", pps_id, subset ? "subset " : "", sps_id)});
    return std::nullopt;
  }
  const Entry<Sps>& se = table[sps_id];
  if (!pe.scaling || pe.scaling_pps_generation != pe.generation || pe.scaling_sps_generation != se.generation) {
    pe.scaling = std::make_shared<const ScalingMatrices>(resolve_scaling_matrices(*se.value, *pe.value));
    pe.scaling_pps_generation = pe.generation;
    pe.scaling_sps_generation = se.generation;
  }
  Activation a;
  a.pps = pe.value;
  a.sps = se.value;
  a.subset = subset;
  a.scaling = pe.scaling;
  a.pps_unit = pe.unit_index;
  a.sps_unit = se.unit_index;
  a.pps_generation = pe.generation;
  a.sps_generation = se.generation;
  if (!picture_open_) {
    picture_open_ = true;
    active_pps_ = static_cast<int>(pps_id);
  }
  if (subset) {
    active_subset_sps_ = static_cast<int>(sps_id);
  } else {
    active_sps_ = static_cast<int>(sps_id);
  }
  return a;
}

void ActiveSets::end_picture() {
  picture_open_ = false;
  active_pps_ = active_sps_ = active_subset_sps_ = -1;
  for (unsigned i = 0; i < kMaxSps; ++i) {
    if (sps_pending_[i].value) {
      sps_[i] = std::move(sps_pending_[i]);
      sps_pending_[i] = {};
      tables_dirty_ = true;
    }
    if (subset_sps_pending_[i].value) {
      subset_sps_[i] = std::move(subset_sps_pending_[i]);
      subset_sps_pending_[i] = {};
      tables_dirty_ = true;
    }
  }
  for (unsigned i = 0; i < kMaxPps; ++i) {
    if (pps_pending_[i].value) {
      pps_[i] = std::move(pps_pending_[i]);
      pps_pending_[i] = {};
      tables_dirty_ = true;
    }
  }
}

std::shared_ptr<const Sps> ActiveSets::sps(unsigned id) const {
  return id < kMaxSps ? sps_[id].value : nullptr;
}
std::shared_ptr<const Sps> ActiveSets::subset_sps(unsigned id) const {
  return id < kMaxSps ? subset_sps_[id].value : nullptr;
}
std::shared_ptr<const Pps> ActiveSets::pps(unsigned id) const {
  return id < kMaxPps ? pps_[id].value : nullptr;
}
uint64_t ActiveSets::sps_generation(unsigned id) const {
  return id < kMaxSps ? sps_[id].generation : 0;
}
uint64_t ActiveSets::pps_generation(unsigned id) const {
  return id < kMaxPps ? pps_[id].generation : 0;
}

}  // namespace ovt::syntax::h264
