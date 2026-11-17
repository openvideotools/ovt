// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Lazy header trees (chapter 7 §7.4.5) and lazy emulation-prevention stripping (§7.4.6), WI-1.125. Module-internal.
//
// LazyRbsp strips the EPBs of one H.26x NAL unit in chunks: the index parses a slice header from the first 2 KiB
// and strips more only when the parse runs past them, while Unit.epb_positions still lists every EPB of the unit
// (a memchr scan of the rest, no copy). reparse_unit_syntax() rebuilds the full tree of one unit of a finished
// StreamIndex on demand: it replays the units the target depends on (parameter sets, the earlier units of its
// frame, and for AV1 everything since the last key frame) with recording off and records only the target, so the
// tree is byte-identical to the one an eager run records (acceptance test 4).

#ifndef OVT_INDEX_LAZY_SYNTAX_H
#define OVT_INDEX_LAZY_SYNTAX_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_model_generated.h"

namespace ovt::index {

/// Chunked rbsp_from_nal() (chapter 7 §7.2.5) with reusable storage: one instance per dispatch, no allocation per
/// unit once its buffers have grown.
class LazyRbsp {
 public:
  /// Forgets the previous unit (capacity is kept). `nal` must stay valid until the next reset().
  void reset(std::span<const uint8_t> nal) noexcept;
  /// Strips raw bytes [stripped_raw(), min(raw_limit, size)) and appends them to rbsp(); their EPB positions join
  /// epb_positions(). Throws std::length_error for a NAL unit over 4 GiB (positions are 32-bit).
  void extend(size_t raw_limit);
  /// Appends the EPB positions of the raw bytes not yet stripped (ascending, after those of the prefix) without
  /// stripping them; rbsp() is unchanged. After this, extend() must not be called again for the unit.
  void scan_rest();

  [[nodiscard]] std::span<const uint8_t> rbsp() const noexcept { return rbsp_; }
  [[nodiscard]] const std::vector<uint32_t>& epb_positions() const noexcept { return epb_; }
  /// Raw bytes covered by rbsp().
  [[nodiscard]] size_t stripped_raw() const noexcept { return stripped_; }
  [[nodiscard]] bool complete() const noexcept { return stripped_ == nal_.size(); }

 private:
  std::span<const uint8_t> nal_;
  std::vector<uint8_t> rbsp_;
  std::vector<uint32_t> epb_;
  size_t stripped_ = 0;
  unsigned zeros_ = 0;  // §7.2.5 loop state at stripped_: zero bytes since the last non-zero byte or EPB
};

/// Re-parses unit `unit_index` of `index` (which must have been built from `file`) and returns its header tree
/// as a standalone SyntaxNode FlatBuffer; empty when the unit records nothing (no parse at all). `unit_index`
/// must be below index.units()->size(). May throw ovt::io::IoError when the file no longer holds the unit.
[[nodiscard]] std::vector<uint8_t> reparse_unit_syntax(const fb::StreamIndex& index, const io::RandomAccessFile& file,
                                                       uint32_t unit_index);

}  // namespace ovt::index

#endif  // OVT_INDEX_LAZY_SYNTAX_H
