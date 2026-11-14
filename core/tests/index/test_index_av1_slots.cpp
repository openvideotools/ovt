// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the AV1 reference slot model: DecoderState, the reference frame update and loading processes,
// get_relative_dist(), set_frame_refs() and the OrderHints / RefFrameSignBias derivation (chapter 7 §7.3.10.4;
// WI-1.119a). WI-1.119b adds the index-side slot step, the frame / temporal-unit grouping and the corpus tests
// (acceptance tests 1 and 3) to this file.
//
// Acceptance test 2 (av1_set_frame_refs_prop): 1 000 random slot states against a literal transcription of AV1
// §7.8 kept in this file, plus three hand-worked examples with documented indices.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "ovt/cxx/syntax/av1/decoder_state.hpp"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)
using RefIdx = RefFrameIdx;

// ---- literal transcription of AV1 §7.8 set_frame_refs() for acceptance test 2 ----------------------------------

struct SpecState {
  std::array<uint32_t, 8> RefOrderHint{};
  uint32_t OrderHint = 0;
  uint8_t OrderHintBits = 7;
  bool enable_order_hint = true;
  uint8_t last_frame_idx = 0;
  uint8_t gold_frame_idx = 0;
};

int spec_get_relative_dist(const SpecState& s, int a, int b) {
  if (!s.enable_order_hint) {
    return 0;
  }
  int diff = a - b;
  const int m = 1 << (s.OrderHintBits - 1);
  diff = (diff & (m - 1)) - (diff & m);  // NOLINT(hicpp-signed-bitwise)
  return diff;
}

// The three search functions of AV1 §7.8, over shiftedOrderHints[] / usedFrame[] / curFrameHint.
struct Search {
  const std::array<int, 8>& shiftedOrderHints;
  const std::array<int, 8>& usedFrame;
  int curFrameHint;
};
int find_latest_backward(const Search& q) {
  int ref = -1;
  int latestOrderHint = 0;
  for (size_t i = 0; i < 8; i++) {
    const int hint = q.shiftedOrderHints[i];
    if (q.usedFrame[i] == 0 && hint >= q.curFrameHint && (ref < 0 || hint >= latestOrderHint)) {
      ref = static_cast<int>(i);
      latestOrderHint = hint;
    }
  }
  return ref;
}
int find_earliest_backward(const Search& q) {
  int ref = -1;
  int earliestOrderHint = 0;
  for (size_t i = 0; i < 8; i++) {
    const int hint = q.shiftedOrderHints[i];
    if (q.usedFrame[i] == 0 && hint >= q.curFrameHint && (ref < 0 || hint < earliestOrderHint)) {
      ref = static_cast<int>(i);
      earliestOrderHint = hint;
    }
  }
  return ref;
}
int find_latest_forward(const Search& q) {
  int ref = -1;
  int latestOrderHint = 0;
  for (size_t i = 0; i < 8; i++) {
    const int hint = q.shiftedOrderHints[i];
    if (q.usedFrame[i] == 0 && hint < q.curFrameHint && (ref < 0 || hint >= latestOrderHint)) {
      ref = static_cast<int>(i);
      latestOrderHint = hint;
    }
  }
  return ref;
}

// AV1 §7.8, transcribed. NUM_REF_FRAMES = 8, REFS_PER_FRAME = 7, LAST_FRAME = 1 … ALTREF_FRAME = 7.
std::array<int, 7> spec_set_frame_refs(const SpecState& s) {
  constexpr int LAST_FRAME = 1;
  constexpr int LAST2_FRAME = 2;
  constexpr int LAST3_FRAME = 3;
  constexpr int GOLDEN_FRAME = 4;
  constexpr int BWDREF_FRAME = 5;
  constexpr int ALTREF2_FRAME = 6;
  constexpr int ALTREF_FRAME = 7;
  std::array<int, 7> ref_frame_idx{};
  std::array<int, 8> usedFrame{};
  std::array<int, 8> shiftedOrderHints{};
  for (size_t i = 0; i < 7; i++) {
    ref_frame_idx[i] = -1;
  }
  ref_frame_idx[0] = s.last_frame_idx;  // LAST_FRAME - LAST_FRAME
  ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = s.gold_frame_idx;
  for (size_t i = 0; i < 8; i++) {
    usedFrame[i] = 0;
  }
  usedFrame[s.last_frame_idx] = 1;
  usedFrame[s.gold_frame_idx] = 1;
  const int curFrameHint = 1 << (s.OrderHintBits - 1);
  for (size_t i = 0; i < 8; i++) {
    shiftedOrderHints[i] =
        curFrameHint + spec_get_relative_dist(s, static_cast<int>(s.RefOrderHint[i]), static_cast<int>(s.OrderHint));
  }
  // (lastOrderHint / goldOrderHint conformance checks are diagnostics, not part of the derivation)
  const Search q{shiftedOrderHints, usedFrame, curFrameHint};
  int ref = find_latest_backward(q);
  if (ref >= 0) {
    ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref;
    usedFrame[static_cast<size_t>(ref)] = 1;
  }
  ref = find_earliest_backward(q);
  if (ref >= 0) {
    ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref;
    usedFrame[static_cast<size_t>(ref)] = 1;
  }
  ref = find_earliest_backward(q);
  if (ref >= 0) {
    ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref;
    usedFrame[static_cast<size_t>(ref)] = 1;
  }
  const std::array<int, 5> Ref_Frame_List{LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME};
  for (size_t i = 0; i < 5; i++) {
    const int refFrame = Ref_Frame_List[i];
    const auto k = static_cast<size_t>(refFrame - LAST_FRAME);
    if (ref_frame_idx[k] < 0) {
      ref = find_latest_forward(q);
      if (ref >= 0) {
        ref_frame_idx[k] = ref;
        usedFrame[static_cast<size_t>(ref)] = 1;
      }
    }
  }
  ref = -1;
  int earliestOrderHint = 0;
  for (size_t i = 0; i < 8; i++) {
    const int hint = shiftedOrderHints[i];
    if (ref < 0 || hint < earliestOrderHint) {
      ref = static_cast<int>(i);
      earliestOrderHint = hint;
    }
  }
  for (size_t i = 0; i < 7; i++) {
    if (ref_frame_idx[i] < 0) {
      ref_frame_idx[i] = ref;
    }
  }
  return ref_frame_idx;
}

DecoderState state_from(const SpecState& s) {
  DecoderState st;
  for (int i = 0; i < 8; ++i) {
    st.slot(i).RefOrderHint = s.RefOrderHint[static_cast<size_t>(i)];
  }
  return st;
}

std::array<int64_t, 8> all_slots(int64_t v) {
  std::array<int64_t, 8> a{};
  a.fill(v);
  return a;
}

}  // namespace

// ---- get_relative_dist, update, loading ---------------------------------------------------------------------------

TEST_CASE("av1_get_relative_dist", "[index][av1][fast]") {
  CHECK(get_relative_dist(5, 3, true, 7) == 2);
  CHECK(get_relative_dist(3, 5, true, 7) == -2);
  CHECK(get_relative_dist(0, 127, true, 7) == 1);  // wrap-around: 0 is one after 127
  CHECK(get_relative_dist(127, 0, true, 7) == -1);
  CHECK(get_relative_dist(64, 0, true, 7) == -64);  // exactly half the range is negative
  CHECK(get_relative_dist(63, 0, true, 7) == 63);
  CHECK(get_relative_dist(5, 3, false, 7) == 0);  // disabled
  CHECK(get_relative_dist(5, 3, true, 0) == 0);
  CHECK(get_relative_dist(1, 0, true, 1) == -1);  // OrderHintBits = 1: values 0, 1 and distance -1
  CHECK(get_relative_dist(255, 0, true, 8) == -1);
}

TEST_CASE("av1_reference_frame_update_and_loading", "[index][av1][fast]") {
  DecoderState st;
  FrameState cur;
  cur.frame_type = FrameType::Inter;
  cur.OrderHint = 9;
  cur.FrameWidth = 640;
  cur.FrameHeight = 360;
  cur.UpscaledWidth = 640;
  cur.showable_frame = true;
  cur.OrderHints[kLastFrame] = 8;
  cur.gm_params[kLastFrame][0] = 77;
  cur.film_grain_params.apply_grain = true;
  cur.film_grain_params.grain_seed = 4321;
  std::vector<StateDiagnostic> diags;
  reference_frame_update(st, cur, 0b1000'0101, 12, [&diags](const StateDiagnostic& d) { diags.push_back(d); });
  CHECK(diags.empty());
  for (int i = 0; i < 8; ++i) {
    const bool refreshed = i == 0 || i == 2 || i == 7;
    CHECK(st.slot(i).RefValid == refreshed);
    if (refreshed) {
      CHECK(st.slot(i).RefOrderHint == 9);
      CHECK(st.slot(i).RefFrameWidth == 640);
      CHECK(st.slot(i).RefFrameType == FrameType::Inter);
      CHECK(st.slot(i).RefShowableFrame);
      CHECK(st.slot(i).SavedOrderHints[kLastFrame] == 8);
      CHECK(st.slot(i).SavedGmParams[kLastFrame][0] == 77);
      CHECK(st.slot(i).SavedFilmGrainParams.grain_seed == 4321);
      CHECK(st.slot(i).slot_decode_index == 12);
    }
  }
  CHECK(st.slot_decode_indices() == std::array<int64_t, 8>{12, -1, 12, -1, -1, -1, -1, 12});

  // Loading restores exactly the saved frame state.
  FrameState loaded;
  CHECK(reference_frame_loading(st, 2, loaded));
  CHECK(loaded == cur);
  // An empty slot is SYN-AV1-0013 and returns false.
  CHECK_FALSE(reference_frame_loading(st, 3, loaded, [&diags](const StateDiagnostic& d) { diags.push_back(d); }));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == "SYN-AV1-0013");

  // INTRA_ONLY with refresh_frame_flags 0xFF is SYN-AV1-0012; the update still runs.
  diags.clear();
  cur.frame_type = FrameType::IntraOnly;
  reference_frame_update(st, cur, kAllFrames, 13, [&diags](const StateDiagnostic& d) { diags.push_back(d); });
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].code == "SYN-AV1-0012");
  CHECK(st.slot_decode_indices() == all_slots(13));

  // The shown-KEY reset clears RefValid and RefOrderHint for every slot (WI-1.119 notes).
  st.reset_for_shown_key_frame();
  for (int i = 0; i < 8; ++i) {
    CHECK_FALSE(st.slot(i).RefValid);
    CHECK(st.slot(i).RefOrderHint == 0);
  }
  CHECK(st.slot_decode_indices() == all_slots(-1));
}

TEST_CASE("av1_ref_frame_names", "[index][av1][fast]") {
  CHECK(ref_frame_name(kLastFrame) == "LAST");
  CHECK(ref_frame_name(kAltrefFrame) == "ALTREF");
  CHECK(ref_frame_name(kIntraFrame) == "INTRA");
  CHECK(ref_frame_name(kNoneFrame) == "NONE");
  CHECK(frame_type_name(FrameType::IntraOnly) == "INTRA_ONLY_FRAME");
  CHECK(identity_gm_params()[kLastFrame] == std::array<int32_t, 6>{0, 0, 1 << 16, 0, 0, 1 << 16});
}

// ---- acceptance test 2: set_frame_refs -----------------------------------------------------------------------------

TEST_CASE("av1_set_frame_refs_prop", "[index][av1][fast]") {
  SECTION("hand-worked example: three backward and two forward references") {
    // OrderHintBits 7 → curFrameHint 64; OrderHint 8. Slots' RefOrderHint {7, 4, 12, 10, 16, 5, 6, 2} shift to
    // {63, 60, 68, 66, 72, 61, 62, 58}. last_frame_idx 0, gold_frame_idx 1. ALTREF = latest backward 72 → slot 4;
    // BWDREF = earliest backward 66 → slot 3; ALTREF2 = next earliest backward 68 → slot 2; LAST2 = latest
    // forward 62 → slot 6; LAST3 = next latest forward 61 → slot 5. Slot 7 (58) stays unused.
    SpecState s;
    s.RefOrderHint = {7, 4, 12, 10, 16, 5, 6, 2};
    s.OrderHint = 8;
    s.last_frame_idx = 0;
    s.gold_frame_idx = 1;
    const RefIdx got = set_frame_refs(state_from(s), s.OrderHint, s.last_frame_idx, s.gold_frame_idx, true, 7);
    CHECK(got == RefIdx{0, 6, 5, 1, 3, 2, 4});
    CHECK(got == spec_set_frame_refs(s));
  }
  SECTION("hand-worked example: no backward reference") {
    // RefOrderHint {7, 4, 3, 2, 1, 0, 6, 5}, OrderHint 8: all shifted hints < 64. BWDREF/ALTREF2/ALTREF stay
    // unset after the backward searches and are filled by latest-forward in list order LAST2 (62 → 6), LAST3
    // (61 → 7), BWDREF (59 → 2), ALTREF2 (58 → 3), ALTREF (57 → 4).
    SpecState s;
    s.RefOrderHint = {7, 4, 3, 2, 1, 0, 6, 5};
    s.OrderHint = 8;
    s.gold_frame_idx = 1;
    const RefIdx got = set_frame_refs(state_from(s), s.OrderHint, 0, 1, true, 7);
    CHECK(got == RefIdx{0, 6, 7, 1, 2, 3, 4});
    CHECK(got == spec_set_frame_refs(s));
  }
  SECTION("hand-worked example: fallback to the earliest frame") {
    // RefOrderHint {7, 4, 12, 10, 16, 9, 11, 13}, OrderHint 8: every slot but LAST and GOLDEN is backward.
    // ALTREF 72 → 4, BWDREF 65 → 5, ALTREF2 66 → 3; LAST2 and LAST3 find no forward frame and fall back to the
    // slot with the smallest shifted hint, 60 → slot 1 (GOLDEN).
    SpecState s;
    s.RefOrderHint = {7, 4, 12, 10, 16, 9, 11, 13};
    s.OrderHint = 8;
    s.gold_frame_idx = 1;
    const RefIdx got = set_frame_refs(state_from(s), s.OrderHint, 0, 1, true, 7);
    CHECK(got == RefIdx{0, 1, 1, 1, 5, 3, 4});
    CHECK(got == spec_set_frame_refs(s));
  }
  SECTION("SYN-AV1-0014 when LAST or GOLDEN is not before the current frame") {
    SpecState s;
    s.RefOrderHint = {9, 4, 12, 10, 16, 5, 6, 2};  // slot 0 (LAST) is after OrderHint 8
    s.OrderHint = 8;
    s.gold_frame_idx = 1;
    std::vector<StateDiagnostic> diags;
    const RefIdx got =
        set_frame_refs(state_from(s), 8, 0, 1, true, 7, [&diags](const StateDiagnostic& d) { diags.push_back(d); });
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == "SYN-AV1-0014");
    CHECK(got == spec_set_frame_refs(s));
    // GOLDEN equal to the current hint (shifted == curFrameHint) is flagged too.
    diags.clear();
    s.RefOrderHint = {7, 8, 12, 10, 16, 5, 6, 2};
    (void)set_frame_refs(state_from(s), 8, 0, 1, true, 7, [&diags](const StateDiagnostic& d) { diags.push_back(d); });
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].code == "SYN-AV1-0014");
  }
  SECTION("1000 random slot states equal the literal transcription") {
    std::mt19937 rng(0x1119);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed, deterministic (§14.1.3)
    int diagnostics = 0;
    for (int n = 0; n < 1000; ++n) {
      SpecState s;
      s.OrderHintBits = static_cast<uint8_t>(1 + rng() % 8);
      const uint32_t range = uint32_t{1} << s.OrderHintBits;
      for (auto& h : s.RefOrderHint) {
        h = static_cast<uint32_t>(rng() % range);
      }
      s.OrderHint = static_cast<uint32_t>(rng() % range);
      s.last_frame_idx = static_cast<uint8_t>(rng() % 8);
      s.gold_frame_idx = static_cast<uint8_t>(rng() % 8);
      DecoderState st = state_from(s);
      for (int i = 0; i < 8; ++i) {
        st.slot(i).RefValid = (rng() % 4) != 0;  // RefValid does not enter set_frame_refs (AV1 §7.8)
      }
      const RefIdx got = set_frame_refs(st, s.OrderHint, s.last_frame_idx, s.gold_frame_idx, true, s.OrderHintBits,
                                        [&diagnostics](const StateDiagnostic&) { ++diagnostics; });
      INFO("case " << n << ": OrderHintBits " << int{s.OrderHintBits} << " OrderHint " << s.OrderHint);
      REQUIRE(got == spec_set_frame_refs(s));
      for (const int idx : got) {
        REQUIRE(idx >= 0);
        REQUIRE(idx < 8);
      }
    }
    CHECK(diagnostics > 0);  // random states do hit the LAST/GOLDEN conformance rule
  }
}

TEST_CASE("av1_compute_order_hints_and_sign_bias", "[index][av1][fast]") {
  SpecState s;
  s.RefOrderHint = {7, 4, 12, 10, 16, 5, 6, 2};
  const DecoderState st = state_from(s);
  const RefIdx refs{0, 6, 5, 1, 3, 2, 4};
  const RefOrderHints oh = compute_order_hints(st, refs, 8, true, 7);
  CHECK(oh.OrderHints == std::array<uint32_t, 8>{0, 7, 6, 5, 4, 10, 12, 16});
  CHECK(oh.RefFrameSignBias == std::array<bool, 8>{false, false, false, false, false, true, true, true});
  const RefOrderHints off = compute_order_hints(st, refs, 8, false, 0);
  CHECK(off.RefFrameSignBias == std::array<bool, 8>{});
}
