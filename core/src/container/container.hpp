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
};

// True when a reader exists for `f` in this build (phase 1 formats only).
[[nodiscard]] bool has_reader(Format f) noexcept;
[[nodiscard]] std::unique_ptr<Reader> make_reader(Format f);

}  // namespace ovt::container

struct ovt_Container {
  ovt_File* file = nullptr;  // not owned; outlives the container
  ovt::container::Format format = ovt::container::Format::Unknown;
  std::unique_ptr<ovt::container::Reader> reader;
  std::vector<uint8_t> tree;  // serialised ContainerTree, rebuilt after every expand
};

#endif  // OVT_CONTAINER_CONTAINER_HPP
