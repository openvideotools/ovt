// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 RPS derivation, reference marking and RefPicList construction (chapter 7 §7.4.4, H.265 §8.3.2 and §8.3.4),
// WI-1.115.

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>

#include "h265_index.h"

namespace ovt::index::h265 {

namespace {

using syntax::h265::SliceSegmentHeader;
using Entries = std::vector<RefPicSets::Entry>;

constexpr const char* kMissingRef = "SYN-H265-0102";

bool contains(const std::vector<int32_t>& v, int32_t x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// The reference pictures of the DPB with the marks one RPS derivation sets on them.
struct Lookup {
  const std::vector<DpbPicture>& dpb;
  std::vector<bool> used;       // named by one of the five lists: stays a reference picture
  std::vector<bool> make_long;  // named by a long-term list: becomes long-term
  explicit Lookup(const std::vector<DpbPicture>& d) : dpb(d), used(d.size(), false), make_long(d.size(), false) {}
};

// A "no reference picture" entry in a Foll list, or in the RPS of a CRA/BLA (§8.3.3 generates those), is legal.
void note_missing(DpbStepResult& out, const syntax::h265::NalHeader& nal, const PocResult& poc, int32_t missing_poc,
                  bool curr) {
  if (!curr || nal.is_irap) {
    return;
  }
  const Severity sev = poc.rasl_skipped ? Severity::Info : Severity::Error;
  out.diagnostics.push_back({sev, kMissingRef, fmt::format("missing reference picture POC {}", missing_poc)});
}

// Long-term entries (eq. 8-5): the full POC with delta_poc_msb_present_flag, else the LSBs.
void derive_long_term(Lookup& lk, const syntax::h265::NalHeader& nal, const SliceSegmentHeader& sh,
                      const syntax::h265::Sps& sps, const PocResult& poc, DpbStepResult& out) {
  const int64_t max_lsb = sps.MaxPicOrderCntLsb;
  const int64_t cur_poc = poc.poc;
  const uint32_t num_lt = sh.num_long_term_sps + sh.num_long_term_pics;
  for (uint32_t i = 0; i < num_lt && i < syntax::h265::kMaxLongTerm; ++i) {
    int64_t poc_lt = sh.PocLsbLt[i];
    const bool msb = sh.delta_poc_msb_present_flag[i];
    if (msb) {
      poc_lt += cur_poc - static_cast<int64_t>(sh.DeltaPocMsbCycleLt[i]) * max_lsb - (cur_poc & (max_lsb - 1));
    }
    const bool curr = sh.UsedByCurrPicLt[i];
    RefPicSets::Entry e{static_cast<int32_t>(poc_lt), kNoRef};
    for (size_t k = 0; k < lk.dpb.size() && e.decode_index == kNoRef; ++k) {
      const int64_t cand = msb ? lk.dpb[k].poc : (lk.dpb[k].poc & (max_lsb - 1));
      if (cand == poc_lt) {
        e.decode_index = lk.dpb[k].decode_index;
        e.poc = lk.dpb[k].poc;  // an LSB-only entry reports the full POC of the picture it matched
        lk.used[k] = true;
        lk.make_long[k] = true;
      }
    }
    if (e.decode_index == kNoRef) {
      note_missing(out, nal, poc, e.poc, curr);
    }
    (curr ? out.sets.lt_curr : out.sets.lt_foll).push_back(e);
  }
}

// One short-term entry (eqs. 8-6, 8-7): short-term reference pictures only, i.e. not the ones just taken as long-term.
void find_short_term(Lookup& lk, const syntax::h265::NalHeader& nal, const PocResult& poc, int32_t want, bool curr,
                     Entries& list, DpbStepResult& out) {
  RefPicSets::Entry e{want, kNoRef};
  for (size_t k = 0; k < lk.dpb.size() && e.decode_index == kNoRef; ++k) {
    if (!lk.dpb[k].is_long_term && !lk.make_long[k] && lk.dpb[k].poc == want) {
      e.decode_index = lk.dpb[k].decode_index;
      lk.used[k] = true;
    }
  }
  if (e.decode_index == kNoRef) {
    note_missing(out, nal, poc, want, curr);
  }
  list.push_back(e);
}

void derive_short_term(Lookup& lk, const syntax::h265::NalHeader& nal, const SliceSegmentHeader& sh,
                       const syntax::h265::Sps& sps, const PocResult& poc, DpbStepResult& out) {
  const syntax::h265::StRefPicSet* rps = syntax::h265::current_st_ref_pic_set(sh, sps);
  if (rps == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < rps->NumNegativePics; ++i) {
    const bool curr = rps->UsedByCurrPicS0[i];
    find_short_term(lk, nal, poc, poc.poc + rps->DeltaPocS0[i], curr, curr ? out.sets.st_curr_before : out.sets.st_foll,
                    out);
  }
  for (uint32_t i = 0; i < rps->NumPositivePics; ++i) {
    const bool curr = rps->UsedByCurrPicS1[i];
    find_short_term(lk, nal, poc, poc.poc + rps->DeltaPocS1[i], curr, curr ? out.sets.st_curr_after : out.sets.st_foll,
                    out);
  }
}

// RefPicListX of §8.3.4 (eqs. 8-8..8-10) for list `x`; empty when no picture can fill it.
void build_one_list(const RefPicSets& sets, unsigned x, int32_t current_decode_index, int32_t current_poc,
                    const SliceSegmentHeader& sh, bool curr_pic_ref, DpbStepResult& out) {
  const uint32_t active = (x == 0 ? sh.num_ref_idx_l0_active_minus1 : sh.num_ref_idx_l1_active_minus1) + 1U;
  const Entries& first = x == 0 ? sets.st_curr_before : sets.st_curr_after;
  const Entries& second = x == 0 ? sets.st_curr_after : sets.st_curr_before;
  Entries base;
  base.insert(base.end(), first.begin(), first.end());
  base.insert(base.end(), second.begin(), second.end());
  base.insert(base.end(), sets.lt_curr.begin(), sets.lt_curr.end());
  if (curr_pic_ref) {
    base.push_back({current_poc, current_decode_index});
  }
  if (base.empty()) {
    out.diagnostics.push_back(
        {Severity::Error, kMissingRef, fmt::format("RefPicList{} requested but NumPicTotalCurr is 0", x)});
    return;
  }
  // RefPicListTemp is the base list repeated up to NumRpsCurrTempList = Max(num_ref_idx_active, NumPicTotalCurr).
  const size_t temp_size = std::max<size_t>(active, std::max<size_t>(sh.NumPicTotalCurr, base.size()));
  Entries temp;
  temp.reserve(temp_size);
  for (size_t i = 0; i < temp_size; ++i) {
    temp.push_back(base[i % base.size()]);
  }
  const auto& mod = sh.ref_pic_lists_modification;
  const bool modified =
      mod && (x == 0 ? mod->ref_pic_list_modification_flag_l0 : mod->ref_pic_list_modification_flag_l1);
  auto& list = out.ref_pic_list[x];
  auto& list_poc = out.ref_pic_list_poc[x];
  for (uint32_t r = 0; r < active; ++r) {
    size_t src = r;
    if (modified) {
      src = x == 0 ? mod->list_entry_l0[r] : mod->list_entry_l1[r];
    }
    const RefPicSets::Entry& e = temp[std::min(src, temp.size() - 1)];
    list.push_back(e.decode_index);
    list_poc.push_back(e.poc);
  }
  // SCC: the current picture takes the last entry of an unmodified list that is shorter than the temp list.
  if (curr_pic_ref && !modified && temp_size > active) {
    list.back() = current_decode_index;
    list_poc.back() = current_poc;
  }
}

}  // namespace

DpbStepResult Dpb::step(int32_t decode_index, const syntax::h265::NalHeader& nal, const SliceSegmentHeader& sh,
                        const syntax::h265::Sps& sps, const syntax::h265::Pps& pps, const PocResult& poc) {
  DpbStepResult out;
  if (nal.is_irap && poc.no_rasl_output_flag) {
    dpb_.clear();  // all reference pictures marked unused
  }
  if (nal.is_idr) {
    out.dpb_after = {decode_index};
    dpb_ = {{decode_index, poc.poc, false}};
    return out;
  }

  // The lookups only see reference pictures; `dpb_` holds nothing else. Long-term entries go first (§8.3.2).
  Lookup lk(dpb_);
  derive_long_term(lk, nal, sh, sps, poc, out);
  derive_short_term(lk, nal, sh, sps, poc, out);

  // Marking: Lt entries become long-term, pictures in none of the five sets become unused.
  std::vector<DpbPicture> next;
  for (size_t k = 0; k < dpb_.size(); ++k) {
    if (lk.used[k]) {
      DpbPicture p = dpb_[k];
      p.is_long_term = p.is_long_term || lk.make_long[k];
      next.push_back(p);
    }
  }
  next.push_back({decode_index, poc.poc, false});
  dpb_ = std::move(next);
  for (const DpbPicture& p : dpb_) {
    out.dpb_after.push_back(p.decode_index);
  }
  std::sort(out.dpb_after.begin(), out.dpb_after.end());

  DpbStepResult lists = build_lists(out.sets, decode_index, poc.poc, sh, pps);
  out.ref_pic_list = std::move(lists.ref_pic_list);
  out.ref_pic_list_poc = std::move(lists.ref_pic_list_poc);
  out.declared_refs = std::move(lists.declared_refs);
  for (IndexDiagnostic& d : lists.diagnostics) {
    out.diagnostics.push_back(std::move(d));
  }
  return out;
}

DpbStepResult Dpb::build_lists(const RefPicSets& sets, int32_t current_decode_index, int32_t current_poc,
                               const SliceSegmentHeader& sh, const syntax::h265::Pps& pps) {
  DpbStepResult out;
  out.sets = sets;
  if (sh.slice_type == syntax::h265::kSliceI) {
    return out;
  }
  const bool curr_pic_ref = pps.scc_extension.pps_curr_pic_ref_enabled_flag;
  const unsigned num_lists = sh.slice_type == syntax::h265::kSliceB ? 2U : 1U;
  for (unsigned x = 0; x < num_lists; ++x) {
    build_one_list(sets, x, current_decode_index, current_poc, sh, curr_pic_ref, out);
  }
  for (const auto& list : out.ref_pic_list) {
    for (const int32_t idx : list) {
      if (idx != kNoRef && !contains(out.declared_refs, idx)) {
        out.declared_refs.push_back(idx);
      }
    }
  }
  return out;
}

}  // namespace ovt::index::h265
