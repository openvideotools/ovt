// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libaom `inspect`-compatible JSON exporter (chapter 06 §6.8.3, WI-0.090): the per-frame JSON that libaom
// v3.12.1 `examples/inspect.c` prints, so that xiph aomanalyzer and scripts written against `inspect` load OVT
// output. The CLI (`ovt analyze --format aom-inspect`, WI-2.521) calls this function, so the format is
// implemented once. C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_AOM_INSPECT_JSON_HPP
#define OVT_CXX_MODEL_AOM_INSPECT_JSON_HPP

#include <iosfwd>
#include <span>
#include <string>

#include "ovt_model_generated.h"

namespace ovt::model {

/// Options of write_aom_inspect_json.
struct AomJsonOptions {
  /// `decode_index` of the first exported record when a sub-range is exported; written into `"configString"`
  /// (aomanalyzer expects `"frame"` to count from 0, so the original position is recorded there).
  int decode_index_offset = 0;
  /// The OVT version written into `"configString"`; empty = the product version (ovt_version_string()). Golden tests
  /// pin it so that a version bump does not change their expected bytes.
  // SPEC-ASSUMPTION(#269): the §6.11 interface names only decode_index_offset; this member is added.
  std::string ovt_version;
};

/// Writes @p frames_in_decode_order (AV1 FrameAnalysis records; null pointers are skipped) to @p out in the
/// byte format of libaom v3.12.1 `inspect` (§6.8.3): `[`, one object per frame each followed by `,`, `null`,
/// `]`. Each object holds the per-MI layers (with the `*Map` objects of `inspect.c`), the accounting layer when
/// the record has `symbols`, and the frame-level keys. The output is a deterministic function of the inputs.
/// Throws std::invalid_argument for a record that is not AV1 or lacks its frame info or cell grid, or whose
/// grid does not match its block tree.
void write_aom_inspect_json(std::ostream& out, std::span<const fb::FrameAnalysis* const> frames_in_decode_order,
                            const AomJsonOptions& options = {});

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_AOM_INSPECT_JSON_HPP
