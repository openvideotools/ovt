// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ovt_Container handle (chapter 08 §8.1, WI-1.200). Per-format readers (WI-1.201 onwards) plug in behind
// ovt::container::Reader; until then every supported format gets the empty-tree stub reader.

#ifndef OVT_CONTAINER_CONTAINER_HPP
#define OVT_CONTAINER_CONTAINER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "container/byte_service.h"
#include "container/format.hpp"
#include "ovt/container.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"

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
  // Materialises lazy children of the node at `box_offset`; the handle re-reads tree() after OVT_OK when
  // tree_revision() changed (or is nullopt).
  virtual ovt_status expand(uint64_t box_offset) {
    (void)box_offset;
    return OVT_E_UNSUPPORTED;
  }
  // A counter expand() bumps whenever it changes the tree, so the handle skips re-serialising the whole tree after a
  // no-op expand (an O(tree) cost per call). nullopt: the reader does not track it and every OVT_OK re-reads tree().
  [[nodiscard]] virtual std::optional<uint64_t> tree_revision() const noexcept { return std::nullopt; }
  // RawVideoFormat FlatBuffer (Y4M and raw readers); empty otherwise.
  [[nodiscard]] virtual std::vector<uint8_t> raw_format() const { return {}; }
  // SampleTable FlatBuffer of `count` samples from `first` (count 0 = all); empty when unsupported.
  [[nodiscard]] virtual std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const {
    (void)track_index;
    (void)first;
    (void)count;
    return {};
  }
  // TrackInfo FlatBuffer of one track (chapter 08 §8.1.3); empty when the reader has no such track.
  [[nodiscard]] virtual std::vector<uint8_t> track_info(int track_index) const {
    (void)track_index;
    return {};
  }
  // HdrMetadata FlatBuffer of one track (chapter 08 §8.4.6); empty when unsupported. Throws ovt::Cancelled when
  // `cancel` is requested and io::IoError on an I/O failure.
  [[nodiscard]] virtual std::vector<uint8_t> hdr_metadata(int track_index, jobs::Cancel& cancel) const {
    (void)track_index;
    (void)cancel;
    return {};
  }
  // TsTiming FlatBuffer (MPEG-TS reader); empty otherwise.
  [[nodiscard]] virtual std::vector<uint8_t> ts_timing() const { return {}; }
  // TsReport FlatBuffer (TR 101 290 priority 1 and 2 checks, MPEG-TS reader); empty otherwise.
  [[nodiscard]] virtual std::vector<uint8_t> ts_report() const { return {}; }
};

// True when a reader exists for `f` in this build (phase 1 formats, MPEG-TS and the MXF placeholder).
[[nodiscard]] bool has_reader(Format f) noexcept;
// nullptr when there is no reader for `f` (has_reader false) or `file` is not a valid `f` stream; then *why (if
// given) says OVT_E_UNSUPPORTED or OVT_E_CORRUPT (the libavformat-backed MXF placeholder may also report
// OVT_E_IO or OVT_E_NOMEM).
[[nodiscard]] std::unique_ptr<Reader> make_reader(Format f, const io::RandomAccessFile& file,
                                                  ovt_status* why = nullptr);

// Maps the exception being handled to the status a C entry point returns (IoError code, OVT_E_NOMEM, OVT_E_INTERNAL).
// Call from inside a catch block.
ovt_status status_of_current_exception() noexcept;

}  // namespace ovt::container

struct ovt_Container;
namespace ovt::container {
// Re-serialises the handle's tree when an expand marked it stale; a no-op otherwise. The caller holds
// bytes.tree_mutex(). Throws what Reader::tree() throws (std::bad_alloc).
void refresh_tree(ovt_Container& c);
// ovt_container_expand without the C boundary: expands the box at `box_offset` under the tree lock and marks the tree
// stale when the reader changed it. OVT_E_UNSUPPORTED without a reader. May throw (std::bad_alloc).
ovt_status expand_box(ovt_Container& c, uint64_t box_offset);
// Body of ovt_container_expand_all (WI-1.235): expands every box except `mdat` payloads, checking `cancel` before each
// box (throws ovt::Cancelled) and reporting progress to `sink`. OVT_OK once no unexpanded box is left.
ovt_status expand_all(ovt_Container& c, jobs::Cancel& cancel, const jobs::Pool::ProgressSink& sink);
}  // namespace ovt::container

struct ovt_Container {
  ovt_File* file = nullptr;  // not owned; outlives the container
  ovt::container::Format format = ovt::container::Format::Unknown;
  std::unique_ptr<ovt::container::Reader> reader;
  std::vector<uint8_t> tree;          // serialised ContainerTree; rebuilt on the next read after an expand changed it
  ovt::container::ByteService bytes;  // page cache and tree lock of the thread-safe byte service (§8.8)
  // An expand changed the reader's tree since `tree` was serialised. Guarded by bytes.tree_mutex(), like `tree`.
  // Serialising lazily keeps a run of expands linear: each one used to re-serialise the whole tree.
  bool tree_stale = false;
};

#endif  // OVT_CONTAINER_CONTAINER_HPP
