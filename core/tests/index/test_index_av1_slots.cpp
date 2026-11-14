// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the AV1 reference slot model, set_frame_refs() and the frame / temporal-unit grouping (chapter 7
// §7.3.9, §7.3.10.4, §7.4.3, §7.4.4; WI-1.119).
//
// Acceptance tests:
//  1. av1_index_frame_count_vs_oracle: av1-1-b8-05-mv (hidden ALTREF + show_existing_frame) and av1-1-b8-06-mfmv
//     grouped from the IVF OBUs; FrameInfo count = number of frame headers in FFmpeg 6.1.1 `-bsf:v trace_headers`
//     (5 / 4; the libaom `inspect` tool is not built in the test environment, trace_headers is the §7.7 oracle for
//     the same count), shown frames = `aomdec --rawvideo --summary` "showed frames" (4 / 4). The slot states of
//     av1-1-b8-05-mv are replayed from the trace_headers values in av1_slots_mv_vector_replay.
//  2. av1_set_frame_refs_prop: 1 000 random slot states against a literal transcription of AV1 §7.8 kept in this
//     file, plus three hand-worked examples with documented indices.
//  3. av1_slots_show_existing_key: a shown KEY via show_existing_frame → all 8 dpb_after entries equal
//     show_existing_of and is_rap is true.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "index/av1_frame_boundary.h"
#include "index/av1_slots.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)
using ovt::index::Av1FrameGrouper;
using ovt::index::Av1FrameHeaderValues;
using ovt::index::Av1FrameRecord;
using ovt::index::Av1SlotModel;
using ovt::index::FrameHeaderPeek;
using ovt::index::IndexDiagnostic;
using ovt::index::ObuEvent;
using ovt::index::ObuPlacement;
using ovt::index::Severity;
using Bytes = std::vector<uint8_t>;
using RefIdx = RefFrameIdx;

std::vector<std::string> codes_of(const std::vector<IndexDiagnostic>& diags) {
  std::vector<std::string> out;
  out.reserve(diags.size());
  for (const auto& d : diags) {
    out.emplace_back(d.code);
  }
  return out;
}

ovt::index::IndexDiagnosticCallback collect(std::vector<IndexDiagnostic>& into) {
  return [&into](const IndexDiagnostic& d) { into.push_back(d); };
}

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

// ---- helpers for the slot model ---------------------------------------------------------------------------------

Av1FrameHeaderValues frame(FrameType type, bool show_frame, uint32_t order_hint, uint8_t refresh, RefIdx refs = {},
                           bool showable = false) {
  Av1FrameHeaderValues h;
  h.show_frame = show_frame;
  h.refresh_frame_flags = refresh;
  h.ref_frame_idx = refs;
  h.state.frame_type = type;
  h.state.OrderHint = order_hint;
  h.state.showable_frame = showable;
  h.state.FrameWidth = 352;
  h.state.FrameHeight = 288;
  h.state.UpscaledWidth = 352;
  h.state.RenderWidth = 352;
  h.state.RenderHeight = 288;
  h.state.MiCols = 88;
  h.state.MiRows = 72;
  return h;
}

Av1FrameHeaderValues show_existing(uint8_t slot) {
  Av1FrameHeaderValues h;
  h.show_existing_frame = true;
  h.frame_to_show_map_idx = slot;
  return h;
}

std::array<int64_t, 8> all_slots(int64_t v) {
  std::array<int64_t, 8> a{};
  a.fill(v);
  return a;
}

// ---- helpers for the grouper -----------------------------------------------------------------------------------

ObuEvent obu(ObuType type, std::optional<FrameHeaderPeek> peek = std::nullopt, uint8_t spatial_id = 0) {
  ObuEvent e;
  e.header.obu_type = static_cast<uint8_t>(type);
  e.header.type_name = obu_type_name(e.header.obu_type);
  e.header.spatial_id = spatial_id;
  e.frame_header = peek;
  return e;
}

FrameHeaderPeek peek(FrameType type, bool show_frame) {
  FrameHeaderPeek p;
  p.frame_type = type;
  p.show_frame = show_frame;
  return p;
}

FrameHeaderPeek peek_existing(uint8_t slot) {
  FrameHeaderPeek p;
  p.show_existing_frame = true;
  p.frame_to_show_map_idx = slot;
  return p;
}

ObuEvent tile_group(uint32_t tg_end, uint32_t num_tiles) {
  ObuEvent e = obu(ObuType::TileGroup);
  e.tg_end = tg_end;
  e.num_tiles = num_tiles;
  return e;
}

Bytes read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t le32(const Bytes& d, size_t at) {
  return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8U) |
         (static_cast<uint32_t>(d[at + 2]) << 16U) | (static_cast<uint32_t>(d[at + 3]) << 24U);
}

// Walks the IVF frames and splits each payload into OBUs with the §7.3.3 scanner.
void for_each_ivf_obu(const Bytes& file, const std::function<void(std::span<const uint8_t>)>& cb) {
  REQUIRE(file.size() >= 32);
  const size_t header_size = static_cast<size_t>(file[6]) | (static_cast<size_t>(file[7]) << 8U);
  size_t pos = header_size;
  while (pos + 12 <= file.size()) {
    const size_t frame_size = le32(file, pos);
    pos += 12;
    REQUIRE(pos + frame_size <= file.size());
    const std::span<const uint8_t> tu(file.data() + pos, frame_size);
    ovt::syntax::scan_obu_low_overhead(tu, pos, ovt::syntax::kNoContainerSample,
                                       ovt::syntax::ObuSpanKind::LengthDelimited,
                                       [&cb](const ovt::syntax::ScannedUnit& u) { cb(u.bytes); });
    pos += frame_size;
  }
}

struct GroupedStream {
  Av1FrameGrouper grouper;
  std::vector<IndexDiagnostic> diags;
  std::vector<FrameHeaderPeek> frame_headers;  // one per frame start
  std::optional<SequenceHeader> seq;
};

GroupedStream group_ivf(const Bytes& file) {
  GroupedStream g;
  for_each_ivf_obu(file, [&g](std::span<const uint8_t> unit) {
    ovt::BitReader br(unit);
    ovt::syntax::SyntaxBuilder b(br);
    b.set_recording(false);
    ObuEvent e;
    e.header = parse_obu_header(br, b);
    if (e.header.type() == ObuType::SequenceHeader) {
      g.seq = parse_sequence_header(br, b);
    } else if (e.header.type() == ObuType::FrameHeader || e.header.type() == ObuType::Frame) {
      REQUIRE(g.seq.has_value());
      e.frame_header = ovt::index::peek_frame_header(br, *g.seq);
      g.frame_headers.push_back(*e.frame_header);
    }
    const ObuPlacement p = g.grouper.on_obu(e, collect(g.diags));
    if (e.frame_header) {
      CHECK(p.starts_frame);
    }
    (void)b.finish();
  });
  bool truncated = false;
  (void)g.grouper.end_of_stream(truncated, collect(g.diags));
  CHECK_FALSE(truncated);
  return g;
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

// ---- slot model: §7.4.3 / §7.4.4 ---------------------------------------------------------------------------------

TEST_CASE("av1_slots_hidden_frame_and_show_existing", "[index][av1][fast]") {
  Av1SlotModel m;
  m.set_sequence(true, 7);
  std::vector<IndexDiagnostic> diags;
  const auto qp_of = [](int64_t) { return int16_t{42}; };

  // 0: shown KEY, refresh all.
  Av1FrameHeaderValues key = frame(FrameType::Key, true, 0, kAllFrames);
  key.base_q_idx = 100;
  Av1FrameRecord r0 = m.step(key, 0, qp_of, collect(diags));
  CHECK(r0.type == FrameType::Key);
  CHECK(r0.shown);
  CHECK(r0.is_rap);
  CHECK(r0.poc == 0);
  CHECK(r0.qp_base == 100);
  CHECK(r0.declared_refs.empty());
  CHECK(r0.dpb_after == all_slots(0));

  // 1: hidden ALTREF, OrderHint 3, refresh slot 6, refs all slots 0..6.
  Av1FrameRecord r1 =
      m.step(frame(FrameType::Inter, false, 3, 0x40, RefIdx{0, 1, 2, 3, 4, 5, 6}, true), 1, qp_of, collect(diags));
  CHECK_FALSE(r1.shown);
  CHECK(r1.showable_frame);
  CHECK_FALSE(r1.is_rap);
  CHECK(r1.poc == 3);
  CHECK(r1.declared_refs == std::vector<int64_t>{0});
  CHECK(r1.dpb_after == std::array<int64_t, 8>{0, 0, 0, 0, 0, 0, 1, 0});
  CHECK(r1.order_hints.RefFrameSignBias[kLastFrame] == false);

  // 2: shown INTER, OrderHint 1, refresh slot 2, LAST = slot 0, ALTREF = slot 6 (backward).
  Av1FrameRecord r2 =
      m.step(frame(FrameType::Inter, true, 1, 0x04, RefIdx{0, 0, 0, 0, 6, 6, 6}), 2, qp_of, collect(diags));
  CHECK(r2.shown);
  CHECK(r2.declared_refs == std::vector<int64_t>{0, 1});
  CHECK(r2.dpb_after == std::array<int64_t, 8>{0, 0, 2, 0, 0, 0, 1, 0});
  CHECK(r2.order_hints.OrderHints[kAltrefFrame] == 3);
  CHECK(r2.order_hints.RefFrameSignBias[kAltrefFrame]);
  CHECK_FALSE(r2.order_hints.RefFrameSignBias[kLastFrame]);
  CHECK(r2.ref_frame_idx == RefIdx{0, 0, 0, 0, 6, 6, 6});

  // 3: show_existing_frame of slot 6 (the hidden INTER frame): shown, not a RAP, no refresh.
  Av1FrameRecord r3 = m.step(show_existing(6), 3, qp_of, collect(diags));
  CHECK(r3.shown);
  CHECK(r3.show_existing_of == 1);
  CHECK(r3.type == FrameType::Inter);
  CHECK_FALSE(r3.is_rap);
  CHECK(r3.poc == 3);
  CHECK(r3.qp_base == 42);
  CHECK(r3.declared_refs.empty());
  CHECK(r3.dpb_after == r2.dpb_after);  // a show_existing_frame of a non-KEY slot refreshes nothing
  CHECK(r3.ref_frame_idx == RefIdx{-1, -1, -1, -1, -1, -1, -1});
  CHECK(diags.empty());
}

// Acceptance test 3.
TEST_CASE("av1_slots_show_existing_key", "[index][av1][fast]") {
  Av1SlotModel m;
  m.set_sequence(true, 7);
  std::vector<IndexDiagnostic> diags;
  (void)m.step(frame(FrameType::Key, true, 0, kAllFrames), 0, {}, collect(diags));
  (void)m.step(frame(FrameType::Inter, true, 1, 0x01, RefIdx{0, 0, 0, 0, 0, 0, 0}), 1, {}, collect(diags));
  // 2: a hidden KEY frame (forward key frame) into slot 5.
  Av1FrameRecord hidden_key = m.step(frame(FrameType::Key, false, 2, 0x20, {}, true), 2, {}, collect(diags));
  CHECK_FALSE(hidden_key.shown);
  CHECK_FALSE(hidden_key.is_rap);  // §7.4.3: the RAP is the show_existing_frame entry
  CHECK(hidden_key.dpb_after == std::array<int64_t, 8>{1, 0, 0, 0, 0, 2, 0, 0});
  // 3: show_existing_frame of slot 5 → shown KEY: all 8 slots become show_existing_of and is_rap holds.
  Av1FrameRecord shown_key = m.step(show_existing(5), 3, {}, collect(diags));
  CHECK(shown_key.shown);
  CHECK(shown_key.is_rap);
  CHECK(shown_key.type == FrameType::Key);
  CHECK(shown_key.show_existing_of == 2);
  CHECK(shown_key.poc == 2);
  CHECK(shown_key.dpb_after == all_slots(2));
  for (const int64_t d : shown_key.dpb_after) {
    CHECK(d == shown_key.show_existing_of);
  }
  CHECK(diags.empty());
  // The next inter frame references the shown KEY in every slot.
  Av1FrameRecord next =
      m.step(frame(FrameType::Inter, true, 3, 0x01, RefIdx{0, 1, 2, 3, 4, 5, 6}), 4, {}, collect(diags));
  CHECK(next.declared_refs == std::vector<int64_t>{2});
}

TEST_CASE("av1_slots_diagnostics", "[index][av1][fast]") {
  Av1SlotModel m;
  m.set_sequence(true, 7);
  std::vector<IndexDiagnostic> diags;
  // An inter frame before any KEY: every ref_frame_idx points to an empty slot (SYN-AV1-0102 once per index).
  Av1FrameRecord r = m.step(frame(FrameType::Inter, true, 1, 0x01, RefIdx{0, 1, 2, 3, 4, 5, 6}), 0, {}, collect(diags));
  CHECK(r.declared_refs.empty());
  CHECK(codes_of(diags) == std::vector<std::string>(7, "SYN-AV1-0102"));
  CHECK(diags[0].severity == Severity::Error);
  CHECK(r.dpb_after == std::array<int64_t, 8>{0, -1, -1, -1, -1, -1, -1, -1});
  diags.clear();
  // show_existing_frame of an empty slot: SYN-AV1-0013, show_existing_of = -1, not a RAP.
  r = m.step(show_existing(7), 1, {}, collect(diags));
  CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0013"});
  CHECK(r.show_existing_of == -1);
  CHECK_FALSE(r.is_rap);
  diags.clear();
  // INTRA_ONLY with refresh 0xFF: SYN-AV1-0012.
  r = m.step(frame(FrameType::IntraOnly, true, 2, kAllFrames), 2, {}, collect(diags));
  CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0012"});
  CHECK(r.declared_refs.empty());
  CHECK(r.dpb_after == all_slots(2));
  diags.clear();
  // A frame outside the operating point is indexed with shown = false (§7.4.3).
  Av1FrameHeaderValues dropped = frame(FrameType::Inter, true, 3, 0x02, RefIdx{0, 0, 0, 0, 0, 0, 0});
  dropped.in_operating_point = false;
  r = m.step(dropped, 3, {}, collect(diags));
  CHECK_FALSE(r.shown);
  CHECK(r.declared_refs == std::vector<int64_t>{2});
  CHECK(diags.empty());
  // A shown KEY resets RefValid before its own refresh: slots not refreshed would be empty, but refresh is 0xFF.
  r = m.step(frame(FrameType::Key, true, 0, kAllFrames), 4, {}, collect(diags));
  CHECK(r.dpb_after == all_slots(4));
}

TEST_CASE("av1_slots_without_order_hints_poc_is_decode_index", "[index][av1][fast]") {
  Av1SlotModel m;
  m.set_sequence(false, 0);
  std::vector<IndexDiagnostic> diags;
  Av1FrameRecord r0 = m.step(frame(FrameType::Key, true, 0, kAllFrames), 0, {}, collect(diags));
  Av1FrameRecord r1 =
      m.step(frame(FrameType::Inter, true, 0, 0x01, RefIdx{0, 0, 0, 0, 0, 0, 0}), 1, {}, collect(diags));
  CHECK(r0.poc == 0);
  CHECK(r1.poc == 1);
  CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0103", "SYN-AV1-0103"});
  CHECK(diags[0].severity == Severity::Info);
  CHECK(r1.order_hints.RefFrameSignBias == std::array<bool, 8>{});
}

// Replays the frame headers of av1-1-b8-05-mv as FFmpeg 6.1.1 `-bsf:v trace_headers` records them (frame_type,
// show_frame, order_hint, refresh_frame_flags, ref_frame_idx[], show_existing_frame / frame_to_show_map_idx):
//   0: KEY show_frame=1 order_hint=0 (refresh all)
//   1: INTER show_frame=0 showable_frame=1 order_hint=3 refresh_frame_flags=64 ref_frame_idx={0,1,2,3,4,5,6}
//   2: INTER show_frame=1 order_hint=1 refresh_frame_flags=4 ref_frame_idx={0,1,2,3,4,5,6}
//   3: INTER show_frame=1 order_hint=2 refresh_frame_flags=2 ref_frame_idx={2,0,1,3,4,5,6}
//   4: show_existing_frame=1 frame_to_show_map_idx=6
TEST_CASE("av1_slots_mv_vector_replay", "[index][av1][fast]") {
  Av1SlotModel m;
  m.set_sequence(true, 7);
  std::vector<IndexDiagnostic> diags;
  std::vector<Av1FrameRecord> rec;
  rec.push_back(m.step(frame(FrameType::Key, true, 0, kAllFrames), 0, {}, collect(diags)));
  rec.push_back(
      m.step(frame(FrameType::Inter, false, 3, 64, RefIdx{0, 1, 2, 3, 4, 5, 6}, true), 1, {}, collect(diags)));
  rec.push_back(m.step(frame(FrameType::Inter, true, 1, 4, RefIdx{0, 1, 2, 3, 4, 5, 6}), 2, {}, collect(diags)));
  rec.push_back(m.step(frame(FrameType::Inter, true, 2, 2, RefIdx{2, 0, 1, 3, 4, 5, 6}), 3, {}, collect(diags)));
  rec.push_back(m.step(show_existing(6), 4, {}, collect(diags)));
  CHECK(diags.empty());
  CHECK(rec[1].declared_refs == std::vector<int64_t>{0});
  CHECK(rec[1].dpb_after == std::array<int64_t, 8>{0, 0, 0, 0, 0, 0, 1, 0});
  CHECK(rec[2].declared_refs == std::vector<int64_t>{0, 1});
  CHECK(rec[2].order_hints.RefFrameSignBias[kAltrefFrame]);  // slot 6 holds order hint 3 > 1
  CHECK(rec[2].dpb_after == std::array<int64_t, 8>{0, 0, 2, 0, 0, 0, 1, 0});
  CHECK(rec[3].declared_refs == std::vector<int64_t>{2, 0, 1});
  CHECK(rec[3].dpb_after == std::array<int64_t, 8>{0, 3, 2, 0, 0, 0, 1, 0});
  CHECK(rec[4].show_existing_of == 1);
  CHECK(rec[4].poc == 3);
  CHECK(rec[4].shown);
  CHECK_FALSE(rec[4].is_rap);
  CHECK(rec[4].dpb_after == rec[3].dpb_after);
  int shown = 0;
  for (const auto& r : rec) {
    shown += r.shown ? 1 : 0;
  }
  CHECK(shown == 4);  // aomdec: 4 showed frames
}

// ---- frame / temporal-unit grouping (§7.3.9) ----------------------------------------------------------------------

TEST_CASE("av1_frame_grouper_frame_header_and_tile_groups", "[index][av1][fast]") {
  Av1FrameGrouper g;
  std::vector<IndexDiagnostic> diags;
  ObuPlacement p = g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
  CHECK(p.temporal_delimiter);
  p = g.on_obu(obu(ObuType::SequenceHeader), collect(diags));
  CHECK(p.pending_non_vcl);
  p = g.on_obu(obu(ObuType::Metadata), collect(diags));
  CHECK(p.pending_non_vcl);
  CHECK_FALSE(p.in_frame);
  // (c) frame header + two tile groups of a 4-tile frame.
  ObuEvent fh = obu(ObuType::FrameHeader, peek(FrameType::Key, true));
  fh.num_tiles = 4;
  p = g.on_obu(fh, collect(diags));
  CHECK(p.starts_frame);
  CHECK(p.in_frame);
  CHECK_FALSE(p.completes_frame);
  CHECK(p.frame_shown);
  CHECK(g.SeenFrameHeader());
  p = g.on_obu(tile_group(1, 4), collect(diags));
  CHECK(p.in_frame);
  CHECK_FALSE(p.completes_frame);
  CHECK(g.TileNum() == 2);
  p = g.on_obu(obu(ObuType::Metadata), collect(diags));  // intervening metadata belongs to the frame
  CHECK(p.in_frame);
  CHECK_FALSE(p.pending_non_vcl);
  p = g.on_obu(obu(ObuType::RedundantFrameHeader), collect(diags));
  CHECK(p.in_frame);
  p = g.on_obu(tile_group(3, 4), collect(diags));
  CHECK(p.in_frame);
  CHECK(p.completes_frame);
  CHECK_FALSE(g.SeenFrameHeader());
  // (a) one OBU_FRAME is a whole frame; (b) a show_existing_frame header too.
  p = g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
  p = g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, false)), collect(diags));
  CHECK(p.starts_frame);
  CHECK(p.completes_frame);
  CHECK_FALSE(p.frame_shown);
  p = g.on_obu(obu(ObuType::FrameHeader, peek_existing(3)), collect(diags));
  CHECK(p.starts_frame);
  CHECK(p.completes_frame);
  CHECK(p.frame_shown);
  bool truncated = true;
  CHECK_FALSE(g.end_of_stream(truncated, collect(diags)));
  CHECK_FALSE(truncated);
  CHECK(g.frames_started() == 3);
  CHECK(g.frames_completed() == 3);
  CHECK(g.shown_frames() == 2);
  CHECK(g.temporal_units() == 2);
  CHECK(diags.empty());
}

TEST_CASE("av1_frame_grouper_diagnostics", "[index][av1][fast]") {
  SECTION("SYN-AV1-0031 tile group without frame header") {
    Av1FrameGrouper g;
    std::vector<IndexDiagnostic> diags;
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    const ObuPlacement p = g.on_obu(tile_group(0, 1), collect(diags));
    CHECK_FALSE(p.in_frame);
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0031"});
    CHECK(diags[0].severity == Severity::Error);
  }
  SECTION("SYN-AV1-0101 truncated frame at a new frame, a temporal delimiter and the end of stream") {
    Av1FrameGrouper g;
    std::vector<IndexDiagnostic> diags;
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    ObuEvent fh = obu(ObuType::FrameHeader, peek(FrameType::Key, true));
    fh.num_tiles = 2;
    (void)g.on_obu(fh, collect(diags));
    (void)g.on_obu(tile_group(0, 2), collect(diags));
    // A new frame header arrives with one tile missing.
    ObuPlacement p = g.on_obu(obu(ObuType::FrameHeader, peek(FrameType::Inter, false)), collect(diags));
    CHECK(p.closes_previous_frame);
    CHECK(p.previous_truncated);
    CHECK(p.starts_frame);
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0101"});
    // The second frame has no tile groups at all when the temporal delimiter arrives (tile count unknown: no
    // SYN-AV1-0101, the frame is closed as complete).
    p = g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    CHECK(p.closes_previous_frame);
    CHECK_FALSE(p.previous_truncated);
    fh.num_tiles = 3;
    (void)g.on_obu(fh, collect(diags));
    bool truncated = false;
    CHECK(g.end_of_stream(truncated, collect(diags)));
    CHECK(truncated);
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0101", "SYN-AV1-0101"});
    CHECK(g.frames_completed() == 3);
  }
  SECTION("SYN-AV1-0030 temporal unit with zero or two shown frames per spatial layer") {
    Av1FrameGrouper g;
    std::vector<IndexDiagnostic> diags;
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Key, true)), collect(diags));
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, true)), collect(diags));  // second shown frame
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0030"});
    CHECK(diags[0].severity == Severity::Warning);
    CHECK(diags[0].message.find("2 shown frames") != std::string::npos);
    diags.clear();
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, false)), collect(diags));  // only a hidden frame
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0030"});
    CHECK(diags[0].message.find("0 shown frames") != std::string::npos);
    diags.clear();
    // Two spatial layers with one shown frame each are fine; a hidden frame plus its show_existing too.
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, true), 0), collect(diags));
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, true), 1), collect(diags));
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Inter, false)), collect(diags));
    (void)g.on_obu(obu(ObuType::FrameHeader, peek_existing(0)), collect(diags));
    bool truncated = false;
    (void)g.end_of_stream(truncated, collect(diags));
    CHECK(diags.empty());
    CHECK(g.temporal_units() == 4);
  }
  SECTION("SYN-AV1-0032 Annex B temporal_unit_size against temporal delimiters") {
    Av1FrameGrouper g(true);
    std::vector<IndexDiagnostic> diags;
    ObuEvent td = obu(ObuType::TemporalDelimiter);
    td.annexb_temporal_unit_start = true;
    (void)g.on_obu(td, collect(diags));
    (void)g.on_obu(obu(ObuType::Frame, peek(FrameType::Key, true)), collect(diags));
    CHECK(diags.empty());
    ObuEvent frame_at_tu_start = obu(ObuType::Frame, peek(FrameType::Inter, true));
    frame_at_tu_start.annexb_temporal_unit_start = true;  // a temporal unit that does not begin with a TD
    (void)g.on_obu(frame_at_tu_start, collect(diags));
    (void)g.on_obu(obu(ObuType::TemporalDelimiter), collect(diags));  // a TD inside a temporal unit
    // The Annex B check runs before the temporal unit is closed, so the second SYN-AV1-0032 precedes the
    // SYN-AV1-0030 (two shown frames) of the unit it ends.
    CHECK(codes_of(diags) == std::vector<std::string>{"SYN-AV1-0032", "SYN-AV1-0032", "SYN-AV1-0030"});
  }
}

TEST_CASE("av1_peek_frame_header", "[index][av1][fast]") {
  SequenceHeader seq;
  SECTION("show_existing_frame") {
    const Bytes bytes{0b1110'0000};  // show_existing_frame 1, frame_to_show_map_idx 110 = 6
    ovt::BitReader br(bytes);
    const FrameHeaderPeek p = ovt::index::peek_frame_header(br, seq);
    CHECK(p.show_existing_frame);
    CHECK(p.frame_to_show_map_idx == 6);
    CHECK(p.show_frame);
    CHECK(br.bit_pos() == 4);
  }
  SECTION("hidden inter frame") {
    const Bytes bytes{0b0010'0000};  // show_existing 0, frame_type 01, show_frame 0
    ovt::BitReader br(bytes);
    const FrameHeaderPeek p = ovt::index::peek_frame_header(br, seq);
    CHECK_FALSE(p.show_existing_frame);
    CHECK(p.frame_type == FrameType::Inter);
    CHECK_FALSE(p.show_frame);
    CHECK(br.bit_pos() == 4);
  }
  SECTION("reduced_still_picture_header infers a shown KEY frame without reading") {
    seq.reduced_still_picture_header = true;
    const Bytes bytes{0xFF};
    ovt::BitReader br(bytes);
    const FrameHeaderPeek p = ovt::index::peek_frame_header(br, seq);
    CHECK(p.frame_type == FrameType::Key);
    CHECK(p.show_frame);
    CHECK(br.bit_pos() == 0);
  }
  SECTION("truncated OBU throws BR-0001") {
    const Bytes bytes{};
    ovt::BitReader br(bytes);
    CHECK_THROWS_AS(ovt::index::peek_frame_header(br, seq), ovt::ParseError);
  }
}

// ---- acceptance test 1: frame counts on the libaom vectors --------------------------------------------------------

TEST_CASE("av1_index_frame_count_vs_oracle", "[index][av1][fast][needs-corpus]") {
  SECTION("av1-1-b8-05-mv: 5 frames (1 hidden, 1 show_existing), 4 shown, 4 temporal units") {
    const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv");
    const GroupedStream g = group_ivf(read_file(path));
    // trace_headers: KEY shown; INTER hidden (showable); INTER shown; INTER shown; show_existing_frame slot 6.
    CHECK(g.grouper.frames_started() == 5);
    CHECK(g.grouper.frames_completed() == 5);
    CHECK(g.grouper.shown_frames() == 4);  // aomdec --summary: 4 showed frames
    CHECK(g.grouper.temporal_units() == 4);
    REQUIRE(g.frame_headers.size() == 5);
    CHECK(g.frame_headers[0].frame_type == FrameType::Key);
    CHECK(g.frame_headers[0].show_frame);
    CHECK_FALSE(g.frame_headers[1].show_frame);
    CHECK(g.frame_headers[1].frame_type == FrameType::Inter);
    CHECK(g.frame_headers[4].show_existing_frame);
    CHECK(g.frame_headers[4].frame_to_show_map_idx == 6);
    CHECK(codes_of(g.diags).empty());
  }
  SECTION("av1-1-b8-06-mfmv: 4 frames, 4 shown, 4 temporal units") {
    const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_06_mfmv");
    const GroupedStream g = group_ivf(read_file(path));
    CHECK(g.grouper.frames_started() == 4);
    CHECK(g.grouper.frames_completed() == 4);
    CHECK(g.grouper.shown_frames() == 4);  // aomdec --summary: 4 showed frames
    CHECK(g.grouper.temporal_units() == 4);
    CHECK(codes_of(g.diags).empty());
  }
}
