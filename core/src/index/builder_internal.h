// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Internals of the StreamIndex builder (chapter 7 §7.4.2, WI-1.121): the in-memory unit and frame records, the
// codec-independent frame bookkeeping (S5 open / extend, S7 finish, pending non-VCL units, size accounting) and
// the per-codec dispatch interface (S3, S4, S6). Module-internal, not installed.

#ifndef OVT_INDEX_BUILDER_INTERNAL_H
#define OVT_INDEX_BUILDER_INTERNAL_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt_model_generated.h"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::index {

struct DiagRec {
  syntax::Severity severity = syntax::Severity::Error;
  std::string code;
  std::string message;
  BitSpan span{0, 0};
};

/// One ovt.fb.Unit in the making.
struct UnitRec {
  uint64_t file_offset = 0;
  uint64_t length = 0;
  syntax::ByteRange prefix_span;
  uint8_t type = 0;
  std::string_view type_name;  // static storage (the parsers' *_name tables)
  uint8_t layer_id = 0;
  uint8_t temporal_id = 0;
  std::vector<uint32_t> epb_positions;
  int32_t frame = -1;                          // decode index of the owning frame
  flatbuffers::Offset<fb::SyntaxNode> header;  // the recorded tree inside BuilderCore::index(); 0 = none
  int32_t sample_index = -1;
  std::vector<DiagRec> diagnostics;
  bool parameter_set = false;        // listed in StreamIndex.parameter_sets
  bool sei = false;                  // listed in StreamIndex.sei
  bool temporal_unit_start = false;  // UnitInput.temporal_unit_start (AV1 Annex B, WI-1.124)
  std::optional<SampleTiming> timing;
};

/// One entry of a reference list of a frame's first slice (§7.4.4: the "RefSlot names L0[i] / L1[i]" kept privately
/// for the plugin host and the GOP graph; not serialised).
struct RefSlotRec {
  int32_t decode_index = -1;  // -1: an H.264 non-existing frame
  int64_t poc = 0;            // of the referenced frame, or of the field for a field reference
  fb::PictureStructure structure = fb::PictureStructure::Frame;  // Frame, TopField or BottomField
  bool long_term = false;
};

/// One ovt.fb.FrameInfo in the making, plus what S9 needs and the schema does not carry.
struct FrameRec {
  int32_t decode_index = 0;
  int32_t display_index = -1;
  fb::FrameType type = fb::FrameType::Unknown;
  bool shown = true;
  int32_t show_existing_of = -1;
  fb::PictureStructure structure = fb::PictureStructure::Frame;
  uint8_t temporal_id = 0;
  uint8_t layer_id = 0;
  bool is_rap = false;
  int64_t poc = 0;
  std::optional<SampleTiming> timing;  // container timing of the first unit's sample
  uint64_t size_bytes = 0;
  std::vector<uint32_t> units;
  fb::Rect coded_size{0, 0, 0, 0};
  fb::Rect crop{0, 0, 0, 0};
  uint8_t chroma_format = 1;
  uint8_t bit_depth_luma = 8;
  uint8_t bit_depth_chroma = 8;
  int16_t qp_base = 0;
  std::vector<int32_t> declared_refs;  // §7.4.4; empty where the codec's DPB model is not wired in yet
  std::vector<int32_t> dpb_after;      // AV1: the 8 slots, -1 for an empty slot
  int32_t frame_num = -1;
  uint16_t slice_count = 0;
  // ---- S9 inputs (not serialised) ----
  uint32_t sequence = 0;         // H.26x: ordinal of the coded video sequence the picture belongs to
  int32_t second_field_of = -1;  // H.264: decode index of the first field of the complementary pair
  // ---- §7.4.4 private per-frame references (not serialised) ----
  std::array<std::vector<RefSlotRec>, 2> ref_lists;  // H.264: RefPicList0 / RefPicList1 of the first slice
};

/// The memory of the index under construction: uninitialised blocks, so that only the pages the builder has
/// written are resident (§7.4.6), unlike the zero-filled vectors of a std::vector-backed block. FlatBufferBuilder
/// writes back to front and copies only the bytes in use when it grows. The builder owns the allocator and
/// hands it over with the buffer when the index is released (BuilderCore::index(), S10).
class IndexAllocator final : public flatbuffers::Allocator {
 public:
  uint8_t* allocate(size_t size) override {
    return new uint8_t[size];  // NOLINT(cppcoreguidelines-owning-memory): make_unique would zero-fill (touch) every
                               // page
  }
  void deallocate(uint8_t* p, size_t /*size*/) override {
    delete[] p;  // NOLINT(cppcoreguidelines-owning-memory)
  }
};

/// Elementary-stream timing of chapter 6 §6.2.6, taken from the first parameter set that carries it:
/// pts = display_index * ticks_per_frame in timebase_num / timebase_den seconds.
struct StreamTiming {
  int32_t timebase_num = 1;
  int32_t timebase_den = 1;
  int64_t ticks_per_frame = 1;
};

/// The codec-independent state of §7.4.2 S0: units, frames, the open frame and the pending non-VCL units.
class BuilderCore {
 public:
  /// `index_reserve` bytes of index memory are reserved up front (untouched, so free until written): a build
  /// that outgrows it copies the bytes so far into a larger block once per growth step.
  explicit BuilderCore(size_t index_reserve = kDefaultIndexReserve)
      : index_(index_reserve, new IndexAllocator, /*own_allocator=*/true) {}
  static constexpr size_t kDefaultIndexReserve = size_t{1} << 20U;

  std::vector<UnitRec> units;
  std::vector<FrameRec> frames;        // finished frames, decode order
  std::vector<DiagRec> diagnostics;    // StreamIndex.diagnostics
  std::optional<StreamTiming> timing;  // set by the codec dispatch
  uint32_t lazy_after_frames = 10000;  // BuilderOptions.lazy_after_frames
  int64_t record_only_unit = -1;       // BuilderOptions.record_only_unit
  size_t lazy_strip_chunk = 2048;      // BuilderOptions.lazy_strip_chunk

  /// §7.4.5 record_full_tree(u): whether the dispatch records the header tree of `unit`, which the codec peeked to
  /// be a VCL unit (`vcl`) that starts a new picture (`starts_frame`) or not. The frames finished so far plus the
  /// open one, if the unit closes it, give the decode index the unit's frame will have, so "VCL units of the first N
  /// frames" is exact for every unit whose peek is right (a picture started by a slice that does not look like a
  /// first slice is off by one frame).
  [[nodiscard]] bool record_full_tree(uint32_t unit, bool vcl, bool starts_frame) const noexcept {
    if (record_only_unit >= 0) {
      return int64_t{unit} == record_only_unit;
    }
    if (!vcl) {
      return true;
    }
    const size_t frame = frames.size() + ((starts_frame && open_) ? 1 : 0);
    return frame < lazy_after_frames;
  }
  /// The first unit that is not part of a finished frame: the first pending non-VCL unit, else the first unit of
  /// the open frame, else units.size(). Everything before it belongs to complete frames or to no frame.
  [[nodiscard]] uint32_t first_open_unit() const noexcept;
  /// The parsed tree of `unit`, written into index() at once so that its arena is freed while the stream is fed
  /// (§7.4.6: the memory of the index is its bytes, not its trees). An empty tree stores nothing.
  void store_tree(uint32_t unit, syntax::SyntaxNodeBuffer tree, std::string_view root_name);
  /// The index under construction. The recorded trees are already in it (UnitRec::header is their offset);
  /// S10 appends the records, finishes it and releases the buffer, after which the core is spent.
  [[nodiscard]] flatbuffers::FlatBufferBuilder& index() noexcept { return index_; }
  [[nodiscard]] size_t index_bytes() const noexcept { return index_.GetSize(); }
  /// A copy of `unit`'s recorded tree in `dst` (0 when none is recorded), in the same order and with the same
  /// string sharing as index() holds it.
  [[nodiscard]] flatbuffers::Offset<fb::SyntaxNode> copy_tree(flatbuffers::FlatBufferBuilder& dst, uint32_t unit) const;
  /// `unit`'s recorded tree as a standalone SyntaxNode FlatBuffer; empty when none is recorded.
  [[nodiscard]] std::vector<uint8_t> tree_bytes(uint32_t unit) const;

  [[nodiscard]] bool frame_open() const noexcept { return open_.has_value(); }
  /// The open frame, or null when none is open.
  [[nodiscard]] FrameRec* open_frame() noexcept { return open_ ? &*open_ : nullptr; }

  /// S4: a non-VCL unit that precedes the next frame's first VCL unit.
  void add_pending(uint32_t unit) { pending_.push_back(unit); }
  /// S5 (new frame): finishes the open frame, then opens a frame owning the pending units and `unit`.
  FrameRec& start_frame(uint32_t unit);
  /// S5 (same frame): `unit` joins the open frame, after any pending units (they precede it in the stream, e.g.
  /// the H.264 prefix NAL of a later slice). No-op without an open frame.
  void extend_frame(uint32_t unit);
  /// S7: size accounting and frames.push_back(). No-op without an open frame.
  void finish_frame();

  /// Unit-level diagnostic, mirrored into the index diagnostics with a "unit N: " prefix (S4 catch).
  void unit_diagnostic(uint32_t unit, DiagRec d);
  void index_diagnostic(syntax::Severity severity, std::string_view code, std::string message);

 private:
  std::optional<FrameRec> open_;
  std::vector<uint32_t> pending_;
  int32_t next_decode_index_ = 0;
  flatbuffers::FlatBufferBuilder index_;
};

/// §7.4.2 S3, S4 and S6 for one codec.
class CodecDispatch {
 public:
  CodecDispatch() = default;
  CodecDispatch(const CodecDispatch&) = delete;
  CodecDispatch& operator=(const CodecDispatch&) = delete;
  CodecDispatch(CodecDispatch&&) = delete;
  CodecDispatch& operator=(CodecDispatch&&) = delete;
  virtual ~CodecDispatch() = default;
  /// Parses units[unit] from `bytes` (S2–S4), fills its type fields and header tree and places it (S5). Parse
  /// errors are caught inside and become diagnostics.
  virtual void on_unit(BuilderCore& core, uint32_t unit, std::span<const uint8_t> bytes) = 0;
  /// S8: closes whatever is open.
  virtual void end_of_stream(BuilderCore& core) = 0;
};

/// `operating_point` and `annexb` are BuilderOptions.av1_operating_point / av1_annexb (WI-1.124).
[[nodiscard]] std::unique_ptr<CodecDispatch> make_av1_dispatch(uint8_t operating_point = 0, bool annexb = false);
[[nodiscard]] std::unique_ptr<CodecDispatch> make_h264_dispatch();
[[nodiscard]] std::unique_ptr<CodecDispatch> make_h265_dispatch();

/// S9: fills FrameRec.display_index and returns StreamIndex.display_order (display_order.cpp).
[[nodiscard]] std::vector<int32_t> assign_display_order(fb::Codec codec, std::vector<FrameRec>& frames);

/// Converts a ParseError into the Diagnostic of the S4 catch clause.
[[nodiscard]] DiagRec diag_from(const ParseError& e);

}  // namespace ovt::index

#endif  // OVT_INDEX_BUILDER_INTERNAL_H
