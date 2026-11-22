// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF container reader (chapter 08 §8.2.4, WI-1.204): the eager box walk with header-only `moof`s, the
// `mfro`/`mfra` index location and `ovt_container_expand`.

#ifndef OVT_CONTAINER_ISOBMFF_LAZY_H
#define OVT_CONTAINER_ISOBMFF_LAZY_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include "container/container.hpp"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/validate.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

class IsobmffReader final : public Reader {
 public:
  explicit IsobmffReader(const io::RandomAccessFile& file) : file_(file) {}

  // False when the file is not ISOBMFF at all (CNT-0006). Walks the top level (§8.2.4 rules 1-5).
  [[nodiscard]] bool open();

  [[nodiscard]] std::vector<uint8_t> tree() const override;
  [[nodiscard]] int track_count() const noexcept override { return static_cast<int>(tree_.tracks.size()); }
  [[nodiscard]] std::vector<uint8_t> track_info(int track_index) const override;
  // SampleTable FlatBuffer (§8.4.1 moov part, WI-1.218) of `count` samples from `first` (0 = all). The track's table is
  // expanded once and kept; an unknown track yields an empty vector.
  [[nodiscard]] std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const override;
  // §8.2.4 rule 6: OVT_E_INVALID_ARG when `box_offset` is no box; OK and a no-op for a box that is complete.
  ovt_status expand(uint64_t box_offset) override;
  // Bumped by each expand() that decodes a deferred box; a no-op expand leaves it, and the tree, unchanged.
  [[nodiscard]] std::optional<uint64_t> tree_revision() const noexcept override { return revision_; }

  // The `mfra` located through `mfro` (§8.2.4 rule 4) and confirmed by the walk; nullopt otherwise.
  [[nodiscard]] std::optional<uint64_t> mfra_offset() const noexcept { return mfra_offset_; }
  // Runs the validation driver (§8.5, WI-1.224) over the tree as it is now and adds its findings to the boxes. A
  // `moof` still waiting for expand() is not validated. Returns the number of diagnostics added; the tree revision
  // moves when that is not zero. Idempotent.
  size_t validate(ValidationLevel level);
  // Number of boxes still waiting for expand().
  [[nodiscard]] size_t unexpanded() const noexcept { return deferred_.size(); }

 private:
  void index(const BoxNodes& nodes);

  const io::RandomAccessFile& file_;
  fb::ContainerTreeT tree_;
  std::vector<DeferredBox> deferred_;
  std::unordered_set<uint64_t> offsets_;  // file offset of every box in the tree
  std::optional<uint64_t> mfra_offset_;
  uint64_t revision_ = 0;
  mutable std::mutex tables_mutex_;
  mutable std::map<size_t, std::shared_ptr<const fb::SampleTableT>> tables_;  // by trak ordinal
};

// nullptr with *why = OVT_E_CORRUPT when the first bytes are not a box header (CNT-0006). Throws io::IoError on an I/O
// failure. `file` must outlive the reader.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_LAZY_H
