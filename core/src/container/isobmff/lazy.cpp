// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF Reader behind ovt_container_open and ovt_container_expand (chapter 08 §8.2.4, WI-1.204). The tree lives
// as mutable BoxNodeT objects; tree() serialises it on demand, so every expand yields a new immutable buffer.

#include "container/isobmff/lazy.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/brands.h"
#include "container/isobmff/fragments.h"
#include "container/isobmff/hdr_summary.h"
#include "container/isobmff/sample_table.h"
#include "container/isobmff/tracks.h"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

bool IsobmffReader::open() {
  // §8.2.4 rule 4: the index location first, so it is known before the top level is walked.
  const std::optional<uint64_t> mfra = locate_mfra(file_);
  WalkResult w = walk(file_, &deferred_);
  if (std::ranges::any_of(w.diagnostics, [](const auto& d) { return d->code == "CNT-0006"; })) {
    return false;
  }
  tree_.format = "isobmff";
  tree_.root = std::move(w.top_level);
  tree_.diagnostics = std::move(w.diagnostics);
  index(tree_.root);
  tree_.tracks = build_tracks(file_, tree_.root);
  has_moof_ = std::ranges::any_of(tree_.root, [](const auto& b) { return b && b->fourcc == "moof"; });
  // The walk confirms the located `mfra`; when the top level disagrees, the walk wins.
  if (mfra && std::ranges::any_of(
                  tree_.root, [&](const auto& b) { return b->file_span->offset() == *mfra && b->fourcc == "mfra"; })) {
    mfra_offset_ = mfra;
  }
  return true;
}

std::vector<uint8_t> IsobmffReader::tree() const {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::ContainerTree::Pack(fbb, &tree_));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

std::vector<uint8_t> IsobmffReader::track_info(int track_index) const {
  if (track_index < 0 || static_cast<size_t>(track_index) >= tree_.tracks.size()) {
    return {};
  }
  const auto ordinal = static_cast<size_t>(track_index);
  const fb::TrackInfoT* info = tree_.tracks[ordinal].get();
  std::optional<fb::TrackInfoT> with_fragments;
  if (has_moof_) {
    const std::lock_guard lock(tables_mutex_);
    with_fragments.emplace(*info);
    with_fragments->sample_count += trun_sample_count(fragment_build(ordinal, nullptr, jobs::Cancel::never()).timeline);
    info = &*with_fragments;
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::TrackInfo::Pack(fbb, info));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

std::shared_ptr<const fb::SampleTableT> IsobmffReader::table(size_t ordinal, jobs::Cancel& cancel) const {
  const std::lock_guard lock(tables_mutex_);  // held over the expansion: a second caller waits instead of repeating it
  auto it = tables_.find(ordinal);
  if (it == tables_.end()) {
    const fb::BoxNodeT* trak = find_trak(tree_.root, ordinal);
    if (trak == nullptr) {
      return nullptr;
    }
    std::unique_ptr<fb::SampleTableT> built = expand_sample_table(file_, *trak, cancel);
    if (has_moof_) {  // §8.4.1 fragment part
      FragmentBuild& f = fragment_build(ordinal, built.get(), cancel);
      append_fragment_samples(*built, f.timeline, std::move(f.samples));
    }
    it = tables_.emplace(ordinal, std::move(built)).first;
  }
  return it->second;
}

std::vector<uint8_t> IsobmffReader::sample_table(int track_index, uint32_t first, uint32_t count) const {
  if (track_index < 0 || static_cast<size_t>(track_index) >= tree_.tracks.size()) {
    return {};
  }
  const std::shared_ptr<const fb::SampleTableT> t = table(static_cast<size_t>(track_index), jobs::Cancel::never());
  if (!t) {
    return {};
  }
  const flatbuffers::DetachedBuffer buf = serialise(*slice_sample_table(*t, first, count));
  const std::span<const uint8_t> bytes(buf.data(), buf.size());
  return {bytes.begin(), bytes.end()};
}

std::vector<uint8_t> IsobmffReader::hdr_metadata(int track_index, jobs::Cancel& cancel) const {
  if (track_index < 0 || static_cast<size_t>(track_index) >= tree_.tracks.size()) {
    return {};
  }
  const auto ordinal = static_cast<size_t>(track_index);
  const fb::BoxNodeT* trak = find_trak(tree_.root, ordinal);
  const std::shared_ptr<const fb::SampleTableT> t = table(ordinal, cancel);
  if (trak == nullptr || !t) {
    return {};
  }
  const flatbuffers::DetachedBuffer buf =
      build_hdr_metadata(*trak, tree_.tracks[ordinal]->track_id, *t, file_, HdrProbeOptions{}, cancel);
  const std::span<const uint8_t> bytes(buf.data(), buf.size());
  return {bytes.begin(), bytes.end()};
}

std::vector<uint8_t> IsobmffReader::fragments(int track_index, jobs::Cancel& cancel) const {
  if (track_index < 0 || static_cast<size_t>(track_index) >= tree_.tracks.size()) {
    return {};
  }
  const std::lock_guard lock(tables_mutex_);
  const flatbuffers::DetachedBuffer buf =
      serialise(fragment_build(static_cast<size_t>(track_index), nullptr, cancel).timeline);
  const std::span<const uint8_t> bytes(buf.data(), buf.size());
  return {bytes.begin(), bytes.end()};
}

FragmentBuild& IsobmffReader::fragment_build(size_t ordinal, const fb::SampleTableT* moov_table,
                                             jobs::Cancel& cancel) const {
  if (const auto it = fragments_.find(ordinal); it != fragments_.end()) {
    return *it->second;
  }
  // The continuation point needs the moov samples: the kept table (its leading moov part), else a fresh expansion.
  std::unique_ptr<fb::SampleTableT> expanded;
  if (moov_table == nullptr) {
    if (const auto t = tables_.find(ordinal); t != tables_.end()) {
      moov_table = t->second.get();
    } else if (const fb::BoxNodeT* trak = find_trak(tree_.root, ordinal); trak != nullptr) {
      expanded = expand_sample_table(file_, *trak, cancel);
      moov_table = expanded.get();
    }
  }
  const fb::SampleTableT empty;
  const FragmentTrackInput in =
      fragment_track_input(file_, tree_.root, *tree_.tracks[ordinal], moov_table != nullptr ? *moov_table : empty);
  auto build = std::make_unique<FragmentBuild>(build_fragments(file_, tree_.root, in, cancel));
  return *fragments_.emplace(ordinal, std::move(build)).first->second;
}

size_t IsobmffReader::validate(ValidationLevel level) {
  size_t added = isobmff::validate(tree_.root, level);
  added += check_brands(file_, tree_.root, level);
  if (added > 0) {
    ++revision_;
  }
  return added;
}

ovt_status IsobmffReader::expand(uint64_t box_offset) {
  if (!offsets_.contains(box_offset)) {
    return OVT_E_INVALID_ARG;
  }
  const auto it = std::ranges::find_if(deferred_, [&](const DeferredBox& d) { return d.offset == box_offset; });
  if (it == deferred_.end()) {
    return OVT_OK;
  }
  const auto node =
      std::ranges::find_if(tree_.root, [&](const auto& b) { return b->file_span->offset() == box_offset; });
  expand_node(file_, **node, it->ctx);
  index((*node)->children);
  deferred_.erase(it);
  ++revision_;
  return OVT_OK;
}

// Depth is bounded by kMaxBoxDepth.
// NOLINTNEXTLINE(misc-no-recursion)
void IsobmffReader::index(const BoxNodes& nodes) {
  for (const auto& n : nodes) {
    offsets_.insert(n->file_span->offset());
    index(n->children);
  }
}

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why) {
  auto r = std::make_unique<IsobmffReader>(file);
  if (!r->open()) {
    if (why != nullptr) {
      *why = OVT_E_CORRUPT;
    }
    return nullptr;
  }
  return r;
}

}  // namespace ovt::container::isobmff
