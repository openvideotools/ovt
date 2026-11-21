// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 DPB simulation for the StreamIndex (chapter 7 §7.4.4, WI-1.122): frame stores and picture numbers
// (H.264 §8.2.4.1), the initial reference picture lists for frames and fields (§8.2.4.2.1–8.2.4.2.5), their
// modification (§8.2.4.3), the frame_num gap process (§8.2.5.2), the sliding window (§8.2.5.3) and the adaptive
// memory control operations 1–6 (§8.2.5.4). Only reference marking is modelled: pictures that wait for output only
// are not stores here.

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "index/h264_index.h"

namespace ovt::index::h264 {

namespace {

using syntax::h264::kSliceB;
using syntax::h264::kSliceI;
using syntax::h264::kSliceSI;
using syntax::h264::NalHeader;
using syntax::h264::RefPicListModificationOp;
using syntax::h264::SliceHeader;
using syntax::h264::Sps;
using Mark = Dpb::Mark;
using Store = Dpb::FrameStore;

constexpr const char* kGapInserted = "SYN-H264-0102";  // Info
constexpr const char* kGap = "SYN-H264-0103";          // Warning
constexpr const char* kShortList = "SYN-H264-0104";    // Warning
constexpr const char* kMissing = "SYN-H264-0105";      // Error
constexpr const char* kOverflow = "SYN-H264-0106";     // Error

// max_num_ref_frames is at most 16 (H.264 A.3.1 MaxDpbFrames), so at most 16 inferred frames of a gap survive the
// sliding window; inferring the earlier ones changes nothing (they are evicted again, oldest first).
constexpr uint32_t kMaxGapFrames = 16;

constexpr size_t field(Parity p) noexcept {
  return p == Parity::Bottom ? 1 : 0;
}

constexpr Parity opposite(Parity p) noexcept {
  return p == Parity::Top ? Parity::Bottom : Parity::Top;
}

bool field_is(const Store& s, Parity p, Mark m) noexcept {
  return s.has[field(p)] && s.mark[field(p)] == m;
}

bool any_is(const Store& s, Mark m) noexcept {
  return field_is(s, Parity::Top, m) || field_is(s, Parity::Bottom, m);
}

bool both_are(const Store& s, Mark m) noexcept {
  return field_is(s, Parity::Top, m) && field_is(s, Parity::Bottom, m);
}

bool is_reference(const Store& s) noexcept {
  return any_is(s, Mark::Short) || any_is(s, Mark::Long);
}

void unmark(Store& s) noexcept {
  s.mark = {Mark::Unused, Mark::Unused};
}

// PicOrderCnt of a frame or complementary field pair (eq. 8-1): the smaller field order count.
int32_t frame_poc(const Store& s) noexcept {
  return std::min(s.poc[0], s.poc[1]);
}

// §8.2.4.2.4: when decoding a field, the PicOrderCnt of an entry of refFrameList0ShortTerm considers only its
// fields marked "used for short-term reference".
int32_t short_term_poc(const Store& s) noexcept {
  const bool top = field_is(s, Parity::Top, Mark::Short);
  const bool bottom = field_is(s, Parity::Bottom, Mark::Short);
  if (top && bottom) {
    return frame_poc(s);
  }
  return top ? s.poc[0] : s.poc[1];
}

void wrap_frame_nums(std::vector<Store>& stores, uint32_t frame_num, uint32_t max_frame_num) {
  for (Store& s : stores) {
    s.frame_num_wrap = s.frame_num > frame_num ? int64_t{s.frame_num} - max_frame_num : int64_t{s.frame_num};
  }
}

// The current picture as the list construction and marking see it (§8.2.4.1).
struct Current {
  bool field = false;
  Parity parity = Parity::Frame;
  int64_t curr_pic_num = 0;
  int64_t max_pic_num = 0;
  int32_t poc = 0;
};

using Ref = ListRef;
using List = std::vector<Ref>;

// Stable sort of the store indices `v` by `less`. std::stable_sort allocates a temporary buffer on every call, which
// the index hot path (§7.4.6) cannot afford; the vectors here hold at most max_num_ref_frames entries (16) in a
// conforming stream, so a stable insertion sort does it without memory. A corrupt stream may hold many more
// long-term frame stores: past kInsertionSortMax the allocating library sort takes over.
constexpr size_t kInsertionSortMax = 64;

template <typename Less>
void stable_sort_indices(std::vector<int32_t>& v, Less less) {
  if (v.size() > kInsertionSortMax) {
    std::stable_sort(v.begin(), v.end(), less);
    return;
  }
  for (size_t i = 1; i < v.size(); ++i) {
    const int32_t x = v[i];
    size_t j = i;
    for (; j > 0 && less(x, v[j - 1]); --j) {
      v[j] = v[j - 1];
    }
    v[j] = x;
  }
}

int64_t pic_num(const Store& s, Parity p, const Current& c) noexcept {
  return c.field ? 2 * s.frame_num_wrap + (p == c.parity ? 1 : 0) : s.frame_num_wrap;
}

int64_t long_term_pic_num(const Store& s, Parity p, const Current& c) noexcept {
  const int64_t idx = s.long_term_frame_idx;
  return c.field ? 2 * idx + (p == c.parity ? 1 : 0) : idx;
}

// The picture whose PicNum (short-term) or LongTermPicNum (long-term) is `num`: a frame when decoding a frame,
// else a field. "No reference picture" when there is none.
Ref find_pic(const std::vector<Store>& stores, const Current& c, Mark m, int64_t num) {
  const auto number = [&](const Store& s, Parity p) {
    return m == Mark::Short ? pic_num(s, p, c) : long_term_pic_num(s, p, c);
  };
  for (size_t i = 0; i < stores.size(); ++i) {
    const Store& s = stores[i];
    if (!c.field && both_are(s, m) && number(s, Parity::Frame) == num) {
      return Ref{static_cast<int32_t>(i), Parity::Frame};
    }
    for (const Parity p : {Parity::Top, Parity::Bottom}) {
      if (c.field && field_is(s, p, m) && number(s, p) == num) {
        return Ref{static_cast<int32_t>(i), p};
      }
    }
  }
  return Ref{};
}

// eq. 8-34 to 8-36: picNumLXNoWrap from the predictor (which it updates), then picNumLX.
int64_t next_pic_num(const RefPicListModificationOp& op, const Current& c, int64_t& pred) {
  const int64_t abs_diff = int64_t{op.abs_diff_pic_num_minus1} + 1;
  int64_t no_wrap = op.modification_of_pic_nums_idc == 0 ? pred - abs_diff : pred + abs_diff;
  if (no_wrap < 0) {
    no_wrap += c.max_pic_num;
  } else if (no_wrap >= c.max_pic_num) {
    no_wrap -= c.max_pic_num;
  }
  pred = no_wrap;
  return no_wrap > c.curr_pic_num ? no_wrap - c.max_pic_num : no_wrap;
}

void set_mark(Store& s, Parity p, Mark m) noexcept {
  if (p == Parity::Frame) {
    s.mark = {m, m};
  } else {
    s.mark[field(p)] = m;
  }
}

void drop_long_term(Store& s) noexcept {
  for (Mark& f : s.mark) {
    f = f == Mark::Long ? Mark::Unused : f;
  }
  s.long_term_frame_idx = -1;
}

Current current_of(const SliceHeader& sh, const Sps& sps, int32_t poc) {
  const uint32_t max_frame_num = std::max<uint32_t>(sps.MaxFrameNum, 1);
  Current c;
  c.field = sh.field_pic_flag;
  c.parity = !sh.field_pic_flag ? Parity::Frame : (sh.bottom_field_flag ? Parity::Bottom : Parity::Top);
  c.max_pic_num = c.field ? 2 * int64_t{max_frame_num} : int64_t{max_frame_num};
  c.curr_pic_num = c.field ? 2 * int64_t{sh.frame_num} + 1 : int64_t{sh.frame_num};
  c.poc = poc;
  return c;
}

RefPic to_ref_pic(const Store& s, Parity p) {
  RefPic e;
  e.decode_index = s.non_existing ? -1 : s.decode_index;
  e.parity = p;
  e.poc = p == Parity::Frame ? frame_poc(s) : s.poc[field(p)];
  e.long_term = s.mark[field(p)] == Mark::Long;
  e.non_existing = s.non_existing;
  return e;
}

class ListBuilder {
 public:
  ListBuilder(const std::vector<Store>& stores, const Current& cur) : stores_(stores), cur_(cur) {}

  // Indices of the stores that `keep` selects into `out`, in insertion order (stable sorts then give a
  // deterministic order for equal keys, which a conforming stream does not have).
  template <typename Pred>
  void select(Pred keep, std::vector<int32_t>& out) const {
    out.clear();
    for (size_t i = 0; i < stores_.size(); ++i) {
      if (keep(stores_[i])) {
        out.push_back(static_cast<int32_t>(i));
      }
    }
  }

  // Appends the frames of `order` as frame entries.
  static void frames(const std::vector<int32_t>& order, List& out) {
    for (const int32_t i : order) {
      out.push_back(Ref{i, Parity::Frame});
    }
  }

  // §8.2.4.2.5: appends fields taken alternately from the ordered frame list, same parity first; when one parity
  // runs out, the remaining fields of the other parity follow in list order.
  void alternate(const std::vector<int32_t>& order, Mark m, List& out) const {
    const Parity same = cur_.parity;
    const Parity other = opposite(same);
    size_t i_same = 0;
    size_t i_other = 0;
    const size_t n = order.size();
    while (i_same < n || i_other < n) {
      for (; i_same < n; ++i_same) {
        if (field_is(store(order[i_same]), same, m)) {
          out.push_back(Ref{order[i_same], same});
          ++i_same;
          break;
        }
      }
      for (; i_other < n; ++i_other) {
        if (field_is(store(order[i_other]), other, m)) {
          out.push_back(Ref{order[i_other], other});
          ++i_other;
          break;
        }
      }
    }
  }

  // The frames (or fields) of `order` under the current picture's structure.
  void append(const std::vector<int32_t>& order, Mark m, List& out) const {
    if (cur_.field) {
      alternate(order, m, out);
    } else {
      frames(order, out);
    }
  }

  // §8.2.4.2.1 / 8.2.4.2.2 (P and SP slices) into `out`.
  void p_list(ListScratch& sc, List& out) const {
    const bool fld = cur_.field;
    select([fld](const Store& s) { return fld ? any_is(s, Mark::Short) : both_are(s, Mark::Short); }, sc.st);
    stable_sort_indices(sc.st, [&](int32_t a, int32_t b) { return store(a).frame_num_wrap > store(b).frame_num_wrap; });
    select([fld](const Store& s) { return fld ? any_is(s, Mark::Long) : both_are(s, Mark::Long); }, sc.lt);
    sort_long_term(sc.lt);
    out.clear();
    append(sc.st, Mark::Short, out);
    append(sc.lt, Mark::Long, out);
  }

  // §8.2.4.2.3 / 8.2.4.2.4 (B slices) into `out`. Non-existing frames have no order count and are left out.
  void b_lists(ListScratch& sc, std::array<List, 2>& out) const {
    const bool fld = cur_.field;
    select(
        [fld](const Store& s) { return !s.non_existing && (fld ? any_is(s, Mark::Short) : both_are(s, Mark::Short)); },
        sc.st);
    const auto poc_of = [&](int32_t i) { return fld ? short_term_poc(store(i)) : frame_poc(store(i)); };
    // Frames: "less than" (a reference frame never has the current POC); fields: "less than or equal" (the first
    // field of the current frame may).
    const auto before = [&](int32_t i) { return fld ? poc_of(i) <= cur_.poc : poc_of(i) < cur_.poc; };
    sc.lo.clear();
    sc.hi.clear();
    for (const int32_t i : sc.st) {
      (before(i) ? sc.lo : sc.hi).push_back(i);
    }
    stable_sort_indices(sc.lo, [&](int32_t a, int32_t b) { return poc_of(a) > poc_of(b); });
    stable_sort_indices(sc.hi, [&](int32_t a, int32_t b) { return poc_of(a) < poc_of(b); });
    sc.order0.assign(sc.lo.begin(), sc.lo.end());
    sc.order0.insert(sc.order0.end(), sc.hi.begin(), sc.hi.end());
    sc.order1.assign(sc.hi.begin(), sc.hi.end());
    sc.order1.insert(sc.order1.end(), sc.lo.begin(), sc.lo.end());
    select([fld](const Store& s) { return fld ? any_is(s, Mark::Long) : both_are(s, Mark::Long); }, sc.lt);
    sort_long_term(sc.lt);
    const std::array<const std::vector<int32_t>*, 2> orders{&sc.order0, &sc.order1};
    for (size_t x = 0; x < 2; ++x) {
      out[x].clear();
      append(*orders[x], Mark::Short, out[x]);
      append(sc.lt, Mark::Long, out[x]);
    }
    // When RefPicList1 has more than one entry and equals RefPicList0, its first two entries are switched.
    if (out[1].size() > 1 && out[1] == out[0]) {
      std::swap(out[1][0], out[1][1]);
    }
  }

  // §8.2.4.3.1 / 8.2.4.3.2 on `list` (num_ref_idx_lX_active_minus1 + 2 entries, the last a scratch slot).
  void modify(List& list, const std::vector<RefPicListModificationOp>& ops, DpbStepResult& out) const {
    const size_t active = list.size() - 1;
    int64_t pred = cur_.curr_pic_num;
    size_t ref_idx = 0;
    for (const RefPicListModificationOp& op : ops) {
      const uint32_t idc = op.modification_of_pic_nums_idc;
      if (idc > 2 || ref_idx > active) {
        continue;  // 3 ends the loop (not stored); 4 and 5 are MVC inter-view operations (WI-4.111)
      }
      const int64_t num = idc == 2 ? int64_t{op.long_term_pic_num} : next_pic_num(op, cur_, pred);
      const Ref pic = find_pic(stores_, cur_, idc == 2 ? Mark::Long : Mark::Short, num);
      if (pic.store < 0) {
        out.diagnostics.push_back(
            {Severity::Error, kMissing, "ref_pic_list_modification references a missing picture"});
      }
      // eq. 8-37 / 8-38: the picture goes to refIdxLX, the entries from there move up by one (the last falls off)
      // and the later copy of the picture is removed.
      list.insert(list.begin() + static_cast<std::ptrdiff_t>(ref_idx), pic);
      list.pop_back();
      ++ref_idx;
      size_t n = ref_idx;
      for (size_t c = ref_idx; c < list.size(); ++c) {
        if (pic.store < 0 || list[c] != pic) {
          list[n++] = list[c];
        }
      }
    }
  }

 private:
  [[nodiscard]] const Store& store(int32_t i) const { return stores_[static_cast<size_t>(i)]; }

  void sort_long_term(std::vector<int32_t>& v) const {
    stable_sort_indices(
        v, [&](int32_t a, int32_t b) { return store(a).long_term_frame_idx < store(b).long_term_frame_idx; });
  }

  const std::vector<Store>& stores_;
  const Current& cur_;
};

// §8.2.4.2 initial lists and §8.2.4.3 modification of the first slice.
void build_lists(const std::vector<Store>& stores, const Current& cur, const SliceHeader& sh, ListScratch& sc,
                 DpbStepResult& out) {
  const ListBuilder lb(stores, cur);
  std::array<List, 2>& lists = sc.lists;
  const uint32_t t = sh.slice_type_mod5;
  const size_t num_lists = (t == kSliceI || t == kSliceSI) ? 0 : (t == kSliceB ? 2 : 1);
  if (num_lists == 1) {
    lb.p_list(sc, lists[0]);
  } else if (num_lists == 2) {
    lb.b_lists(sc, lists);
  }
  const syntax::h264::RefPicListModification& mod = sh.ref_pic_list_modification;
  const std::array<uint32_t, 2> active_minus1{sh.num_ref_idx_l0_active_minus1, sh.num_ref_idx_l1_active_minus1};
  const std::array<const std::vector<RefPicListModificationOp>*, 2> ops{&mod.ops_l0, &mod.ops_l1};
  const std::array<bool, 2> modify{mod.ref_pic_list_modification_flag_l0, mod.ref_pic_list_modification_flag_l1};
  for (size_t x = 0; x < num_lists; ++x) {
    const size_t active = std::min<size_t>(size_t{active_minus1[x]} + 1, syntax::h264::kMaxRefIdx);
    List& list = lists[x];
    list.reserve(syntax::h264::kMaxRefIdx + 2);  // the insert of eq. 8-37 needs one entry beyond the scratch slot
    list.resize(active + 1);  // truncated, padded with "no reference picture", plus the scratch slot of eq. 8-37
    if (modify[x] && !mod.mvc) {
      lb.modify(list, *ops[x], out);
    }
    list.resize(active);
    std::erase_if(list, [](const Ref& r) { return r.store < 0; });
    // SPEC-ASSUMPTION(#736): SYN-H264-0104 judges the final list. A short initial list that the modification fills
    // (x264 --weightp repeats a reference this way) is complete.
    if (list.size() < active) {
      out.diagnostics.push_back(
          {Severity::Warning, kShortList,
           fmt::format("reference list shorter than num_ref_idx_l{}_active ({} < {})", x, list.size(), active)});
    }
    for (const Ref& r : list) {
      const RefPic e = to_ref_pic(stores[static_cast<size_t>(r.store)], r.parity);
      out.ref_pic_list[x].push_back(e);
      if (e.decode_index >= 0 && std::ranges::find(out.declared_refs, e.decode_index) == out.declared_refs.end()) {
        out.declared_refs.push_back(e.decode_index);
      }
    }
  }
}

}  // namespace

const DpbStepResult& Dpb::step(int32_t decode_index, const NalHeader& nal, const SliceHeader& sh, const Sps& sps,
                               const PocResult& poc, int32_t second_field_of) {
  DpbStepResult& out = result_;
  for (std::vector<RefPic>& l : out.ref_pic_list) {
    l.clear();
  }
  out.declared_refs.clear();
  out.dpb_after.clear();
  out.diagnostics.clear();
  const Current cur = current_of(sh, sps, poc.poc);
  // The frame store of the first field when this picture completes a complementary field pair.
  int32_t pair_store = -1;
  for (size_t i = 0; cur.field && second_field_of >= 0 && i < stores_.size(); ++i) {
    if (stores_[i].decode_index == second_field_of && !stores_[i].has[field(cur.parity)]) {
      pair_store = static_cast<int32_t>(i);
    }
  }
  if (sh.IdrPicFlag && pair_store < 0) {
    stores_.clear();
    have_prev_ref_ = false;
  } else if (!sh.IdrPicFlag) {
    fill_gap(sh, sps, out);
  }
  wrap_frame_nums(stores_, sh.frame_num, std::max<uint32_t>(sps.MaxFrameNum, 1));
  build_lists(stores_, cur, sh, scratch_, out);

  // §8.2.5 marking, for reference pictures only.
  if (nal.nal_ref_idc != 0) {
    if (pair_store < 0) {
      Store s;  // the current picture's new store, holding no field yet
      s.decode_index = second_field_of >= 0 ? second_field_of : decode_index;
      stores_.push_back(s);
      pair_store = static_cast<int32_t>(stores_.size() - 1);
    }
    mark_current(sh, sps, poc, static_cast<size_t>(pair_store), out);
  }
  drop_unused();
  for (const Store& s : stores_) {
    if (s.decode_index >= 0) {
      out.dpb_after.push_back(s.decode_index);
    }
  }
  std::ranges::sort(out.dpb_after);
  out.dpb_after.erase(std::unique(out.dpb_after.begin(), out.dpb_after.end()), out.dpb_after.end());
  return out;
}

// §8.2.5.2: frame_num gaps. Each missing frame_num value becomes a "non-existing" short-term frame, marked through
// the sliding window.
void Dpb::fill_gap(const SliceHeader& sh, const Sps& sps, DpbStepResult& out) {
  const uint32_t max_frame_num = std::max<uint32_t>(sps.MaxFrameNum, 1);
  if (!have_prev_ref_ || sh.frame_num == prev_ref_frame_num_ ||
      sh.frame_num == (prev_ref_frame_num_ + 1) % max_frame_num) {
    return;
  }
  if (!sps.gaps_in_frame_num_value_allowed_flag) {
    out.diagnostics.push_back({Severity::Warning, kGap, "frame_num gap"});
    return;
  }
  const uint32_t first = (prev_ref_frame_num_ + 1) % max_frame_num;
  const uint32_t count = (sh.frame_num + max_frame_num - first) % max_frame_num;
  const uint32_t last = (sh.frame_num + max_frame_num - 1) % max_frame_num;
  out.diagnostics.push_back({Severity::Info, kGapInserted,
                             fmt::format("non-existing frames inserted for frame_num gap ({}..{})", first, last)});
  for (uint32_t k = count > kMaxGapFrames ? count - kMaxGapFrames : 0; k < count; ++k) {
    const uint32_t v = (first + k) % max_frame_num;
    wrap_frame_nums(stores_, v, max_frame_num);
    (void)trim(std::max<uint32_t>(sps.max_num_ref_frames, 1) - 1, SIZE_MAX);
    drop_unused();
    Store s;
    s.non_existing = true;
    s.frame_num = v;
    s.has = {true, true};
    s.mark = {Mark::Short, Mark::Short};
    stores_.push_back(s);
    prev_ref_frame_num_ = v;
  }
}

// Unmarks the short-term frame store with the smallest FrameNumWrap (stores_[keep] excepted) while more than `limit`
// frame stores are references: the sliding window of §8.2.5.3 with limit Max(max_num_ref_frames, 1) - 1, the
// overflow rule of §7.4.4 with the limit itself. FrameNumWrap must be current. Returns whether it unmarked any.
bool Dpb::trim(size_t limit, size_t keep) {
  for (bool any = false;; any = true) {
    size_t refs = 0;
    Store* oldest = nullptr;
    for (size_t i = 0; i < stores_.size(); ++i) {
      Store& s = stores_[i];
      refs += is_reference(s) ? 1U : 0U;
      if (i != keep && any_is(s, Mark::Short) && (oldest == nullptr || s.frame_num_wrap < oldest->frame_num_wrap)) {
        oldest = &s;
      }
    }
    if (refs <= limit || oldest == nullptr) {
      return any;
    }
    unmark(*oldest);
  }
}

// One memory_management_control_operation (§8.2.5.4) of the picture whose store is stores_[current].
void Dpb::apply_mmco(const syntax::h264::MmcoOp& op, const SliceHeader& sh, const Sps& sps, size_t current,
                     CurrentMarking& cm) {
  const Current cur = current_of(sh, sps, 0);
  const int64_t pic_num_x = cur.curr_pic_num - (int64_t{op.difference_of_pic_nums_minus1} + 1);
  const auto idx = static_cast<int32_t>(op.long_term_frame_idx);
  // A LongTermFrameIdx moves: the frame store that held it, unless it is `keep`, stops being long-term.
  const auto release = [&](size_t keep) {
    for (size_t i = 0; i < stores_.size(); ++i) {
      if (i != keep && stores_[i].long_term_frame_idx == idx && any_is(stores_[i], Mark::Long)) {
        unmark(stores_[i]);
        stores_[i].long_term_frame_idx = -1;
      }
    }
  };
  switch (op.memory_management_control_operation) {
    case 1:
    case 2: {
      const bool lt = op.memory_management_control_operation == 2;
      const Ref r = find_pic(stores_, cur, lt ? Mark::Long : Mark::Short, lt ? op.long_term_pic_num : pic_num_x);
      if (r.store >= 0) {
        set_mark(stores_[static_cast<size_t>(r.store)], r.parity, Mark::Unused);
      }
      break;
    }
    case 3: {
      const Ref r = find_pic(stores_, cur, Mark::Short, pic_num_x);
      if (r.store < 0) {
        break;
      }
      release(static_cast<size_t>(r.store));
      Store& s = stores_[static_cast<size_t>(r.store)];
      if (s.long_term_frame_idx != idx) {
        drop_long_term(s);  // the other field cannot keep a different index
      }
      s.long_term_frame_idx = idx;
      set_mark(s, r.parity, Mark::Long);
      break;
    }
    case 4:
      max_long_term_frame_idx_ = static_cast<int32_t>(op.max_long_term_frame_idx_plus1) - 1;
      for (Store& s : stores_) {
        if (any_is(s, Mark::Long) && s.long_term_frame_idx > max_long_term_frame_idx_) {
          drop_long_term(s);
        }
      }
      break;
    case 5:
      for (Store& s : stores_) {
        unmark(s);
      }
      max_long_term_frame_idx_ = -1;
      cm.mmco5 = true;
      break;
    case 6:
      release(current);
      cm.long_term = true;
      cm.long_term_frame_idx = idx;
      break;
    default:
      break;
  }
}

// §8.2.5.1: IDR marking, the sliding window or the memory management control operations, then the current picture
// into stores_[current] (the first field's store for a second field, else a new empty store).
void Dpb::mark_current(const SliceHeader& sh, const Sps& sps, const PocResult& poc, size_t current,
                       DpbStepResult& out) {
  const Current cur = current_of(sh, sps, poc.poc);
  const uint32_t max_frame_num = std::max<uint32_t>(sps.MaxFrameNum, 1);
  const size_t max_refs = std::max<uint32_t>(sps.max_num_ref_frames, 1);
  const syntax::h264::DecRefPicMarking& m = sh.dec_ref_pic_marking;
  CurrentMarking cm;
  if (sh.IdrPicFlag) {
    cm.long_term = m.long_term_reference_flag;
    cm.long_term_frame_idx = m.long_term_reference_flag ? 0 : -1;
    max_long_term_frame_idx_ = m.long_term_reference_flag ? 0 : -1;
  } else if (m.adaptive_ref_pic_marking_mode_flag) {
    for (const syntax::h264::MmcoOp& op : m.mmco) {
      apply_mmco(op, sh, sps, current, cm);
    }
  } else if (!field_is(stores_[current], opposite(cur.parity), Mark::Short) || !cur.field) {
    // The second field of a pair whose first field is a short-term reference does not run the window (§8.2.5.3).
    (void)trim(max_refs - 1, current);
  }

  Store& s = stores_[current];
  s.frame_num = cm.mmco5 ? 0 : sh.frame_num;
  if (cm.long_term) {
    s.long_term_frame_idx = cm.long_term_frame_idx;
  }
  const std::array<int32_t, 2> pocs{cm.mmco5 ? poc.top_after_mmco5 : poc.top_field_order_cnt,
                                    cm.mmco5 ? poc.bottom_after_mmco5 : poc.bottom_field_order_cnt};
  for (const size_t f : {size_t{0}, size_t{1}}) {
    if (!cur.field || f == field(cur.parity)) {
      s.has[f] = true;
      s.mark[f] = cm.long_term ? Mark::Long : Mark::Short;
      s.poc[f] = pocs[f];
    }
  }
  have_prev_ref_ = true;
  prev_ref_frame_num_ = s.frame_num;

  // More reference frames than max_num_ref_frames (only after adaptive marking or long-term pictures): the
  // oldest short-term frame stores other than the current one are dropped.
  wrap_frame_nums(stores_, s.frame_num, max_frame_num);
  if (trim(max_refs, current)) {
    out.diagnostics.push_back({Severity::Error, kOverflow, "DPB overflow"});
  }
}

void Dpb::drop_unused() {
  std::erase_if(stores_, [](const Store& s) { return !is_reference(s); });
}

}  // namespace ovt::index::h264
