// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// YUView statistics CSV exporter and importer (chapter 06 §6.8.2, WI-0.089). The exporter writes the fixed
// OVT type table (`OVT_PredMode` … `OVT_BlockGrid_<Kind>`); the importer reads such a file back into a
// grid-only FrameAnalysis. The CLI (`ovt analyze --format yuview`, WI-2.521) and the Python bindings call
// these functions, so the format is implemented once.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_YUVIEW_CSV_HPP
#define OVT_CXX_MODEL_YUVIEW_CSV_HPP

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::model {

/// Options of write_yuview_csv (chapter 12 §12.2.8 `--yuview-types`, `--yuview-poc-column`).
struct YuviewOptions {
  /// The §6.8.2 type ids to write; empty = every type the frames provide.
  std::vector<std::uint16_t> types;
  /// false: the POC column is `FrameId.display_index` (the default); true: `FrameInfo.poc`.
  bool poc_column_is_poc = false;
  /// The `seq-specs` basename (usually the YUV file name without extension). Must not contain `;` or a line
  /// break; such characters are replaced by `_`.
  std::string basename;
  /// Frames per second for `seq-specs`; 0 = unknown.
  double fps = 0.0;
};

/// Writes one YUView statistics CSV (§6.8.2) for @p frames_in_display_order to @p out: the header (version,
/// `seq-specs` with the first frame's `coded_size`, the type blocks) and the data rows sorted by POC, type id,
/// y, x. Null pointers, frames without `FrameInfo`, hidden frames (`shown == false`) and field pictures are
/// skipped. @p index supplies the codec (QP range, partition-type value set). The output is a deterministic
/// function of the inputs.
void write_yuview_csv(std::ostream& out, const fb::StreamIndex& index,
                      std::span<const fb::FrameAnalysis* const> frames_in_display_order, const YuviewOptions& options);

/// Reads a YUView statistics CSV and returns a finished FrameAnalysis buffer (file identifier `OVTA`, not
/// size-prefixed) for the rows whose POC column equals @p poc: `plugin_id = "yuview-csv-import"` and a
/// CellGrid sized from `seq-specs`, holding the vectors of the `OVT_*` types that map to a CellGrid field
/// (sentinel where no row covers a cell). Foreign type names, OVT types without a grid field and malformed
/// lines are reported as Diagnostics in the buffer; the function never throws on bad input.
[[nodiscard]] std::vector<std::uint8_t> read_yuview_csv(std::istream& in, int poc);

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_YUVIEW_CSV_HPP
