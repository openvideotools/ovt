// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ovt_Container handle (chapter 08 §8.1, WI-1.200). Per-format readers (WI-1.201 onwards) plug in behind
// ovt::container::Reader; until then every supported format gets the empty-tree stub reader.

#ifndef OVT_CONTAINER_CONTAINER_HPP
#define OVT_CONTAINER_CONTAINER_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "container/format.hpp"
#include "ovt/container.h"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container {

// A format reader. The stub implements nothing beyond the defaults.
class Reader {
 public:
  Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;
  virtual ~Reader() = default;
  [[nodiscard]] virtual int track_count() const noexcept { return 0; }
  // The serialised ContainerTree of the current state; empty when the reader builds none (the handle then
  // keeps its empty tree).
  [[nodiscard]] virtual std::vector<uint8_t> tree() const { return {}; }
  // Materialises lazy children of the node at `box_offset`; the handle re-reads tree() after OVT_OK.
  virtual ovt_status expand(uint64_t box_offset) {
    (void)box_offset;
    return OVT_E_UNSUPPORTED;
  }
  // SampleTable FlatBuffer of `count` samples from `first` (count 0 = all); empty when unsupported.
  [[nodiscard]] virtual std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const {
    (void)track_index;
    (void)first;
    (void)count;
    return {};
  }
};

// True when a reader exists for `f` in this build (phase 1 formats only).
[[nodiscard]] bool has_reader(Format f) noexcept;
// nullptr when there is no reader for `f` (has_reader false) or `file` is not a valid `f` stream.
[[nodiscard]] std::unique_ptr<Reader> make_reader(Format f, const io::RandomAccessFile& file);

}  // namespace ovt::container

struct ovt_Container {
  ovt_File* file = nullptr;  // not owned; outlives the container
  ovt::container::Format format = ovt::container::Format::Unknown;
  std::unique_ptr<ovt::container::Reader> reader;
  std::vector<uint8_t> tree;  // serialised ContainerTree, rebuilt after every expand
};

#endif  // OVT_CONTAINER_CONTAINER_HPP
