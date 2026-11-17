// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The StreamIndex builder state machine (chapter 7 §7.4.2, WI-1.121): unit records (S2, S3), the frame
// bookkeeping shared by every codec (S5 open / extend, S7 size accounting, S8), the fallback time base of
// chapter 6 §6.2.6 and the serialisation of the result (S10). Per-codec dispatch (S4, S6) is behind
// CodecDispatch; the display order (S9) is display_order.cpp. WI-1.125 adds the lazy-tree policy of §7.4.5 (the
// dispatch asks BuilderCore::record_full_tree) and the partial index of §7.4.6 (finish_partial).

#include "ovt/cxx/index/stream_index_builder.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "index/builder_internal.h"

namespace ovt::index {

namespace {

using syntax::Severity;

constexpr std::string_view kDiagNoUnits = "SYN-GEN-0101";    // Error, §7.4.7
constexpr std::string_view kDiagNoTiming = "SYN-GEN-0104";   // Info, chapter 6 §6.2.6
constexpr std::string_view kDiagTruncated = "SYN-GEN-0103";  // Warning, §7.4.6 partial index

// Registry severity (§7.9) of a unit scanner finding: BR-0009 and SYN-GEN-0004 are Warnings, the rest Errors.
Severity scan_severity(std::string_view code) noexcept {
  return code == "BR-0009" || code == "SYN-GEN-0004" ? Severity::Warning : Severity::Error;
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::Diagnostic>>> write_diagnostics(
    flatbuffers::FlatBufferBuilder& fbb, const std::vector<DiagRec>& diags) {
  if (diags.empty()) {
    return 0;
  }
  std::vector<flatbuffers::Offset<fb::Diagnostic>> out;
  out.reserve(diags.size());
  for (const DiagRec& d : diags) {
    const auto code = fbb.CreateSharedString(d.code);
    const auto message = fbb.CreateString(d.message);
    const fb::BitSpan span(d.span.bit_offset, d.span.bit_length);
    out.push_back(fb::CreateDiagnostic(fbb, static_cast<fb::Severity>(d.severity), code, message, &span));
  }
  return fbb.CreateVector(out);
}

flatbuffers::Offset<fb::Unit> write_unit(flatbuffers::FlatBufferBuilder& fbb, const UnitRec& u, uint32_t index,
                                         int32_t display_index) {
  const auto type_name = fbb.CreateSharedString(u.type_name.data(), u.type_name.size());
  const auto epb = u.epb_positions.empty() ? 0 : fbb.CreateVector(u.epb_positions);
  const auto header = u.header.empty() ? 0 : u.header.to_flatbuffer(fbb);
  const auto diags = write_diagnostics(fbb, u.diagnostics);
  const fb::ByteSpan file_span(u.file_offset, u.length);
  const fb::ByteSpan prefix_span(u.prefix_span.offset, u.prefix_span.length);
  const fb::FrameId frame(u.frame, display_index);
  fb::UnitBuilder b(fbb);
  b.add_index(index);
  b.add_file_span(&file_span);
  b.add_prefix_span(&prefix_span);
  b.add_type(u.type);
  b.add_type_name(type_name);
  b.add_layer_id(u.layer_id);
  b.add_temporal_id(u.temporal_id);
  b.add_epb_positions(epb);
  b.add_frame(&frame);
  b.add_header(header);
  b.add_sample_index(u.sample_index);
  b.add_diagnostics(diags);
  return b.Finish();
}

flatbuffers::Offset<fb::FrameInfo> write_frame(flatbuffers::FlatBufferBuilder& fbb, const FrameRec& f,
                                               const SampleTiming& t) {
  const auto units = fbb.CreateVector(f.units);
  const auto declared_refs = f.declared_refs.empty() ? 0 : fbb.CreateVector(f.declared_refs);
  const auto dpb_after = f.dpb_after.empty() ? 0 : fbb.CreateVector(f.dpb_after);
  const fb::FrameId id(f.decode_index, f.display_index);
  fb::FrameInfoBuilder b(fbb);
  b.add_id(&id);
  b.add_type(f.type);
  b.add_shown(f.shown);
  b.add_show_existing_of(f.show_existing_of);
  b.add_structure(f.structure);
  b.add_temporal_id(f.temporal_id);
  b.add_layer_id(f.layer_id);
  b.add_is_rap(f.is_rap);
  b.add_poc(f.poc);
  b.add_pts(t.pts);
  b.add_dts(t.dts);
  b.add_timebase_num(t.timebase_num);
  b.add_timebase_den(t.timebase_den);
  b.add_size_bytes(f.size_bytes);
  b.add_units(units);
  b.add_coded_size(&f.coded_size);
  b.add_crop(&f.crop);
  b.add_chroma_format(f.chroma_format);
  b.add_bit_depth_luma(f.bit_depth_luma);
  b.add_bit_depth_chroma(f.bit_depth_chroma);
  b.add_qp_base(f.qp_base);
  b.add_declared_refs(declared_refs);
  b.add_dpb_after(dpb_after);
  b.add_frame_num(f.frame_num);
  b.add_slice_count(f.slice_count);
  return b.Finish();
}

// Chapter 6 §6.2.6: a frame keeps the container timing of its sample; without one, pts counts display slots and
// dts decode slots in the stream's own time base (VUI / timing_info), or in 1/1 with Info SYN-GEN-0104, which the
// caller appends when `fallback_diag` comes back true (this function does not touch the core).
std::vector<SampleTiming> frame_timing(const BuilderCore& core, bool& fallback_diag) {
  std::vector<SampleTiming> out(core.frames.size());
  const bool fallback =
      std::any_of(core.frames.begin(), core.frames.end(), [](const FrameRec& f) { return !f.timing.has_value(); });
  fallback_diag = fallback && !core.timing;
  const StreamTiming st = core.timing.value_or(StreamTiming{});
  // A frame that is never output (display_index -1) takes the pts of the next output frame in decode order.
  // SPEC-ASSUMPTION(#641): §6.2.6 gives no pts for such a frame.
  int64_t shown = 0;
  for (const FrameRec& f : core.frames) {
    shown = std::max<int64_t>(shown, int64_t{f.display_index} + 1);
  }
  int64_t next_pts = shown * st.ticks_per_frame;
  for (size_t i = core.frames.size(); i-- > 0;) {
    const FrameRec& f = core.frames[i];
    if (f.timing) {
      out[i] = *f.timing;
      continue;
    }
    if (f.display_index >= 0) {
      next_pts = int64_t{f.display_index} * st.ticks_per_frame;
    }
    out[i] = SampleTiming{next_pts, int64_t{f.decode_index} * st.ticks_per_frame, st.timebase_num, st.timebase_den};
  }
  return out;
}

}  // namespace

DiagRec diag_from(const ParseError& e) {
  return DiagRec{Severity::Error, e.code, e.what(), BitSpan{e.bit_offset, 0}};
}

// ---- BuilderCore ---------------------------------------------------------------------------------------------

FrameRec& BuilderCore::start_frame(uint32_t unit) {
  finish_frame();
  FrameRec f;
  f.decode_index = next_decode_index_++;
  f.units = std::move(pending_);
  pending_.clear();
  f.units.push_back(unit);
  for (const uint32_t v : f.units) {
    units[v].frame = f.decode_index;
  }
  // "pts/dts/timebase from the container (the first unit's sample)" (S6): the sample of the unit that starts the
  // frame, which for every container is also the sample of the non-VCL units collected before it.
  f.timing = units[unit].timing;
  open_ = std::move(f);
  return *open_;
}

void BuilderCore::extend_frame(uint32_t unit) {
  if (!open_) {
    return;
  }
  for (const uint32_t v : pending_) {
    open_->units.push_back(v);
    units[v].frame = open_->decode_index;
  }
  pending_.clear();
  open_->units.push_back(unit);
  units[unit].frame = open_->decode_index;
}

void BuilderCore::finish_frame() {
  if (!open_) {
    return;
  }
  uint64_t size = 0;
  for (const uint32_t v : open_->units) {
    size += units[v].length + units[v].prefix_span.length;  // S7: prefix bytes are the AU's bytes in the file
  }
  open_->size_bytes = size;
  frames.push_back(std::move(*open_));
  open_.reset();
}

uint32_t BuilderCore::first_open_unit() const noexcept {
  if (!pending_.empty()) {
    return pending_.front();
  }
  if (open_ && !open_->units.empty()) {
    return open_->units.front();
  }
  return static_cast<uint32_t>(units.size());
}

void BuilderCore::unit_diagnostic(uint32_t unit, DiagRec d) {
  diagnostics.push_back(DiagRec{d.severity, d.code, fmt::format("unit {}: {}", unit, d.message), d.span});
  units[unit].diagnostics.push_back(std::move(d));
}

void BuilderCore::index_diagnostic(Severity severity, std::string_view code, std::string message) {
  diagnostics.push_back(DiagRec{severity, std::string(code), std::move(message), BitSpan{0, 0}});
}

// ---- StreamIndexBuilder --------------------------------------------------------------------------------------

struct StreamIndexBuilder::Impl {
  BuilderOptions options;
  BuilderCore core;
  std::unique_ptr<CodecDispatch> dispatch;

  // S9 and S10 over units[0, unit_count) and every finished frame. `extra` diagnostics follow the core's. Leaves
  // the core as it is apart from FrameRec.display_index (S9), so a partial index can be followed by more units.
  [[nodiscard]] std::vector<uint8_t> serialise(std::span<const uint8_t, 32> source_hash, uint32_t unit_count,
                                               std::vector<DiagRec> extra);
};

StreamIndexBuilder::StreamIndexBuilder(const BuilderOptions& options) : impl_(std::make_unique<Impl>()) {
  impl_->options = options;
  impl_->core.lazy_after_frames = options.lazy_after_frames;
  impl_->core.record_only_unit = options.record_only_unit;
  impl_->core.lazy_strip_chunk = options.lazy_strip_chunk;
  switch (options.codec) {
    case fb::Codec::AV1:
      impl_->dispatch = make_av1_dispatch(options.av1_operating_point, options.av1_annexb);
      break;
    case fb::Codec::H264:
      impl_->dispatch = make_h264_dispatch();
      break;
    case fb::Codec::H265:
      impl_->dispatch = make_h265_dispatch();
      break;
    default:
      throw UnsupportedCodec(
          fmt::format("StreamIndexBuilder: no dispatch for codec {}", static_cast<unsigned>(options.codec)));
  }
}

StreamIndexBuilder::~StreamIndexBuilder() = default;

void StreamIndexBuilder::add_unit(const UnitInput& u) {
  BuilderCore& core = impl_->core;
  if (core.units.size() >= std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("StreamIndexBuilder: more than 2^32 - 1 units");  // Unit.index is 32-bit
  }
  const auto index = static_cast<uint32_t>(core.units.size());
  UnitRec rec;
  rec.file_offset = u.file_offset;
  rec.length = u.bytes.size();
  rec.prefix_span = u.prefix_span;
  rec.sample_index = u.sample_index >= 0 && u.sample_index <= std::numeric_limits<int32_t>::max()
                         ? static_cast<int32_t>(u.sample_index)
                         : -1;
  if (u.timing != nullptr) {
    rec.timing = *u.timing;
  }
  rec.temporal_unit_start = u.temporal_unit_start;
  core.units.push_back(std::move(rec));
  impl_->dispatch->on_unit(core, index, u.bytes);
}

void StreamIndexBuilder::add_scan_diagnostic(const syntax::ScanDiagnostic& d) {
  impl_->core.index_diagnostic(scan_severity(d.code), d.code,
                               fmt::format("{} (file offset {})", d.message, d.file_offset));
}

std::vector<uint8_t> StreamIndexBuilder::finish(std::span<const uint8_t, 32> source_hash) {
  BuilderCore& core = impl_->core;
  impl_->dispatch->end_of_stream(core);  // S8
  core.finish_frame();
  if (core.units.empty()) {
    core.index_diagnostic(Severity::Error, kDiagNoUnits, "no video units found");
  }
  return impl_->serialise(source_hash, static_cast<uint32_t>(core.units.size()), {});
}

std::vector<uint8_t> StreamIndexBuilder::finish_partial(std::span<const uint8_t, 32> source_hash) {
  const uint32_t cut = impl_->core.first_open_unit();
  std::vector<DiagRec> extra;
  extra.push_back(DiagRec{Severity::Warning, std::string(kDiagTruncated),
                          fmt::format("index truncated at unit {}", cut), BitSpan{0, 0}});
  return impl_->serialise(source_hash, cut, std::move(extra));
}

uint32_t StreamIndexBuilder::unit_count() const noexcept {
  return static_cast<uint32_t>(impl_->core.units.size());
}

std::vector<uint8_t> StreamIndexBuilder::unit_syntax(uint32_t unit) const {
  const std::vector<UnitRec>& units = impl_->core.units;
  if (unit >= units.size()) {
    return {};
  }
  return units[unit].header.to_flatbuffer();
}

std::vector<uint8_t> StreamIndexBuilder::Impl::serialise(std::span<const uint8_t, 32> source_hash, uint32_t unit_count,
                                                         std::vector<DiagRec> extra) {
  const std::vector<int32_t> display_order = assign_display_order(options.codec, core.frames);  // S9
  bool fallback_timing = false;
  const std::vector<SampleTiming> timing = frame_timing(core, fallback_timing);
  std::vector<DiagRec> diagnostics = core.diagnostics;
  if (fallback_timing) {
    diagnostics.push_back(DiagRec{Severity::Info, std::string(kDiagNoTiming),
                                  "no timing information; frame-count time base", BitSpan{0, 0}});
  }
  diagnostics.insert(diagnostics.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));

  // S10. Units are written once; parameter_sets and sei refer to the same tables.
  flatbuffers::FlatBufferBuilder fbb(size_t{1} << 16U);
  std::vector<flatbuffers::Offset<fb::Unit>> units;
  std::vector<flatbuffers::Offset<fb::Unit>> parameter_sets;
  std::vector<flatbuffers::Offset<fb::Unit>> sei;
  units.reserve(unit_count);
  for (uint32_t i = 0; i < unit_count; ++i) {
    const UnitRec& u = core.units[i];
    const int32_t display_index = u.frame >= 0 ? core.frames[static_cast<size_t>(u.frame)].display_index : -1;
    units.push_back(write_unit(fbb, u, i, display_index));
    if (u.parameter_set) {
      parameter_sets.push_back(units.back());
    }
    if (u.sei) {
      sei.push_back(units.back());
    }
  }
  std::vector<flatbuffers::Offset<fb::FrameInfo>> frames;
  frames.reserve(core.frames.size());
  for (size_t i = 0; i < core.frames.size(); ++i) {
    frames.push_back(write_frame(fbb, core.frames[i], timing[i]));
  }
  const auto hash_off = fbb.CreateVector(source_hash.data(), source_hash.size());
  const auto units_off = fbb.CreateVector(units);
  const auto frames_off = fbb.CreateVector(frames);
  const auto order_off = fbb.CreateVector(display_order);
  const auto ps_off = fbb.CreateVector(parameter_sets);
  const auto sei_off = fbb.CreateVector(sei);
  const auto diags_off = write_diagnostics(fbb, diagnostics);
  fb::StreamIndexBuilder b(fbb);
  b.add_codec(options.codec);
  b.add_schema_version(1);
  b.add_source_hash(hash_off);
  b.add_units(units_off);
  b.add_frames(frames_off);
  b.add_display_order(order_off);
  b.add_parameter_sets(ps_off);
  b.add_sei(sei_off);
  b.add_diagnostics(diags_off);
  fbb.Finish(b.Finish());
  const std::span<const uint8_t> out(fbb.GetBufferPointer(), fbb.GetSize());
  return {out.begin(), out.end()};
}

}  // namespace ovt::index
