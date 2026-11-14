// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 parameter-set activation tracking (chapter 7 §7.3.8, WI-1.115). See ovt/cxx/syntax/h265/active_sets.hpp.

#include "ovt/cxx/syntax/h265/active_sets.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace ovt::syntax::h265 {

namespace {

// SPEC-ASSUMPTION(#418): chapter 7 §7.3.8 states the "identical within a picture" rule for H.265 (§7.4.2.4.2) but
// registers no H.265 diagnostic for it (H.264 has SYN-H264-0021) and none for a missing VPS (which it calls a
// Warning). These two codes are the proposed ones.
constexpr const char* kChangedInPicture = "SYN-H265-0021";
constexpr const char* kMissingVps = "SYN-H265-0022";
constexpr const char* kMissingPps = "SYN-H265-0011";
constexpr const char* kNoParameterSets = "SYN-GEN-0102";

bool same_bytes(const std::vector<uint8_t>& a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// The shared owner of an ActivePps: it keeps the PPS and SPS the tables were derived from alive.
struct ActiveHolder {
  std::shared_ptr<const Pps> pps;
  std::shared_ptr<const Sps> sps;
  ActivePps active;
};

}  // namespace

void ActiveSets::put_vps(Vps vps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                         std::vector<SetDiagnostic>& diags) {
  const unsigned id = vps.vps_video_parameter_set_id;
  if (id >= kMaxVps) {
    return;
  }
  const bool in_use = picture_open_ && active_vps_ == static_cast<int>(id);
  if (in_use && vps_[id].value && !same_bytes(vps_[id].rbsp, rbsp)) {
    diags.push_back({Severity::Error, kChangedInPicture, fmt::format("VPS {} changed within a picture", id)});
    vps_pending_[id] = {std::make_shared<const Vps>(std::move(vps)), std::vector<uint8_t>(rbsp.begin(), rbsp.end()),
                        unit_index, next_generation_++};
    return;
  }
  if (in_use && vps_[id].value) {
    return;  // a repeat of the active VPS: nothing changes (the entry keeps its generation)
  }
  vps_[id] = {std::make_shared<const Vps>(std::move(vps)), std::vector<uint8_t>(rbsp.begin(), rbsp.end()), unit_index,
              next_generation_++};
  vps_pending_[id] = {};
}

void ActiveSets::put_sps(Sps sps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                         std::vector<SetDiagnostic>& diags) {
  const unsigned id = sps.sps_seq_parameter_set_id;
  if (id >= kMaxSps) {
    return;
  }
  const bool in_use = picture_open_ && active_sps_ == static_cast<int>(id);
  if (in_use && sps_[id].value && !same_bytes(sps_[id].rbsp, rbsp)) {
    diags.push_back({Severity::Error, kChangedInPicture, fmt::format("SPS {} changed within a picture", id)});
    sps_pending_[id] = {std::make_shared<const Sps>(std::move(sps)), std::vector<uint8_t>(rbsp.begin(), rbsp.end()),
                        unit_index, next_generation_++};
    return;
  }
  if (in_use && sps_[id].value) {
    return;
  }
  sps_[id] = {std::make_shared<const Sps>(std::move(sps)), std::vector<uint8_t>(rbsp.begin(), rbsp.end()), unit_index,
              next_generation_++};
  sps_pending_[id] = {};
  table_dirty_ = true;
}

void ActiveSets::put_pps(Pps pps, std::span<const uint8_t> rbsp, uint64_t unit_index,
                         std::vector<SetDiagnostic>& diags) {
  const unsigned id = pps.pps_pic_parameter_set_id;
  if (id >= kMaxPps) {
    return;
  }
  const bool in_use = picture_open_ && active_pps_ == static_cast<int>(id);
  if (in_use && pps_[id].value && !same_bytes(pps_[id].rbsp, rbsp)) {
    diags.push_back({Severity::Error, kChangedInPicture, fmt::format("PPS {} changed within a picture", id)});
    pps_pending_[id].value = std::make_shared<const Pps>(std::move(pps));
    pps_pending_[id].rbsp.assign(rbsp.begin(), rbsp.end());
    pps_pending_[id].unit_index = unit_index;
    pps_pending_[id].generation = next_generation_++;
    return;
  }
  if (in_use && pps_[id].value) {
    return;
  }
  pps_[id].value = std::make_shared<const Pps>(std::move(pps));
  pps_[id].rbsp.assign(rbsp.begin(), rbsp.end());
  pps_[id].unit_index = unit_index;
  pps_[id].generation = next_generation_++;
  pps_pending_[id] = {};
  table_dirty_ = true;
}

void ActiveSets::rebuild_table() {
  for (unsigned id = 0; id < kMaxPps; ++id) {
    PpsEntry& e = pps_[id];
    table_.entries[id] = nullptr;
    if (!e.value) {
      continue;
    }
    const unsigned sps_id = e.value->pps_seq_parameter_set_id;
    if (sps_id >= kMaxSps || !sps_[sps_id].value) {
      e.active.reset();
      continue;
    }
    const Entry<Sps>& s = sps_[sps_id];
    if (!e.active || e.built_pps_generation != e.generation || e.built_sps_generation != s.generation) {
      auto holder = std::make_shared<ActiveHolder>();
      holder->pps = e.value;
      holder->sps = s.value;
      holder->active = activate_pps(*e.value, *s.value);
      e.active = std::shared_ptr<const ActivePps>(holder, &holder->active);
      e.built_pps_generation = e.generation;
      e.built_sps_generation = s.generation;
    }
    table_.entries[id] = e.active.get();
  }
  table_dirty_ = false;
}

const PpsTable& ActiveSets::pps_table() {
  if (table_dirty_) {
    rebuild_table();
  }
  return table_;
}

std::optional<Activation> ActiveSets::activate(uint32_t pps_id, bool first_slice_segment_in_pic_flag,
                                               std::vector<SetDiagnostic>& diags) {
  if (first_slice_segment_in_pic_flag) {
    end_picture();
  }
  if (pps_id >= kMaxPps || !pps_[pps_id].value) {
    diags.push_back({Severity::Error, kMissingPps, fmt::format("slice references missing PPS {}", pps_id)});
    return std::nullopt;
  }
  (void)pps_table();
  const PpsEntry& pe = pps_[pps_id];
  const unsigned sps_id = pe.value->pps_seq_parameter_set_id;
  if (!pe.active) {
    diags.push_back(
        {Severity::Warning, kNoParameterSets,
         fmt::format("no parameter sets before first VCL unit (PPS {} refers to missing SPS {})", pps_id, sps_id)});
    return std::nullopt;
  }
  const Entry<Sps>& se = sps_[sps_id];
  Activation a;
  a.active = pe.active;
  a.pps = pe.value;
  a.sps = se.value;
  a.pps_unit = pe.unit_index;
  a.sps_unit = se.unit_index;
  const unsigned vps_id = se.value->sps_video_parameter_set_id;
  if (vps_id < kMaxVps && vps_[vps_id].value) {
    a.vps = vps_[vps_id].value;
    a.vps_unit = vps_[vps_id].unit_index;
  } else if (vps_warned_for_sps_generation_[sps_id] != se.generation) {
    vps_warned_for_sps_generation_[sps_id] = se.generation;
    diags.push_back({Severity::Warning, kMissingVps, fmt::format("SPS {} refers to missing VPS {}", sps_id, vps_id)});
  }
  if (first_slice_segment_in_pic_flag || !picture_open_) {
    picture_open_ = true;
    active_pps_ = static_cast<int>(pps_id);
    active_sps_ = static_cast<int>(sps_id);
    active_vps_ = a.vps ? static_cast<int>(vps_id) : -1;
  }
  return a;
}

void ActiveSets::end_picture() {
  picture_open_ = false;
  active_pps_ = active_sps_ = active_vps_ = -1;
  for (unsigned i = 0; i < kMaxVps; ++i) {
    if (vps_pending_[i].value) {
      vps_[i] = std::move(vps_pending_[i]);
      vps_pending_[i] = {};
    }
  }
  for (unsigned i = 0; i < kMaxSps; ++i) {
    if (sps_pending_[i].value) {
      sps_[i] = std::move(sps_pending_[i]);
      sps_pending_[i] = {};
      table_dirty_ = true;
    }
  }
  for (unsigned i = 0; i < kMaxPps; ++i) {
    if (pps_pending_[i].value) {
      pps_[i].value = std::move(pps_pending_[i].value);
      pps_[i].rbsp = std::move(pps_pending_[i].rbsp);
      pps_[i].unit_index = pps_pending_[i].unit_index;
      pps_[i].generation = pps_pending_[i].generation;
      pps_pending_[i] = {};
      table_dirty_ = true;
    }
  }
}

std::shared_ptr<const Vps> ActiveSets::vps(unsigned id) const {
  return id < kMaxVps ? vps_[id].value : nullptr;
}
std::shared_ptr<const Sps> ActiveSets::sps(unsigned id) const {
  return id < kMaxSps ? sps_[id].value : nullptr;
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

}  // namespace ovt::syntax::h265
