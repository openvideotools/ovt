// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The adapter from ovt::fb::StreamIndex to the SelectionIndex interface of SelectionModel (chapter 11 §11.4.1;
// WI-1.455a, as agreed in spec-question #219 for WI-1.452).
//
// StreamIndexHandle owns the bytes of one FlatBuffer StreamIndex (it is a table without file identifier, so the
// bytes are verified as a table, §6.1.1) and keeps them alive for every view over them. StreamIndexAdapter answers
// the five lookups of SelectionIndex from it: the unit and frame tables are read in place, and two small tables are
// built once at construction (the sample spans and a byte-offset → unit map), each O(units).

#ifndef OVT_APP_MODELS_STREAM_INDEX_ADAPTER_H
#define OVT_APP_MODELS_STREAM_INDEX_ADAPTER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "models/selection_model.h"
#include "ovt_model_generated.h"

namespace ovt::app {

class StreamIndexHandle {
 public:
  // Takes the bytes of a serialized StreamIndex. Returns nullptr when they do not verify as one.
  [[nodiscard]] static std::shared_ptr<const StreamIndexHandle> adopt(std::vector<std::uint8_t> bytes);

  [[nodiscard]] const fb::StreamIndex& index() const noexcept { return *root_; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

  StreamIndexHandle(const StreamIndexHandle&) = delete;
  StreamIndexHandle& operator=(const StreamIndexHandle&) = delete;
  StreamIndexHandle(StreamIndexHandle&&) = delete;
  StreamIndexHandle& operator=(StreamIndexHandle&&) = delete;
  ~StreamIndexHandle() = default;

 private:
  struct Private {};

 public:
  StreamIndexHandle(Private, std::vector<std::uint8_t> bytes);

 private:
  std::vector<std::uint8_t> bytes_;
  const fb::StreamIndex* root_ = nullptr;
};

class StreamIndexAdapter final : public SelectionIndex {
 public:
  explicit StreamIndexAdapter(std::shared_ptr<const StreamIndexHandle> handle);

  [[nodiscard]] std::optional<SelectionUnit> unit(int unit_index) const override;
  [[nodiscard]] std::optional<FrameId> resolve_frame(FrameId frame) const override;
  [[nodiscard]] int first_vcl_unit(FrameId frame) const override;
  [[nodiscard]] std::optional<SelectionSample> sample(int sample_index) const override;
  [[nodiscard]] int unit_at_byte(std::uint64_t file_offset) const override;

 private:
  // The decode index of a frame given with either index, or -1.
  [[nodiscard]] int decode_index_of(FrameId frame) const;

  struct ByteEntry {
    std::uint64_t begin = 0;  // start of the prefix (start code / length field), else of the payload
    std::uint64_t end = 0;    // end of the payload
    int unit = -1;
  };

  std::shared_ptr<const StreamIndexHandle> handle_;
  std::vector<SelectionSample> samples_;  // by sample index; span.length == 0: no such sample
  std::vector<ByteEntry> by_byte_;        // sorted by begin
  bool frames_dense_ = true;              // frames()[i].id.decode_index == i for every i
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_STREAM_INDEX_ADAPTER_H
