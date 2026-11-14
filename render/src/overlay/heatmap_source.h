// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Heatmap value extraction and normalisation per HeatmapSource (chapter 10 §10.5.5). WI-2.430.

#ifndef OVT_RENDER_SRC_OVERLAY_HEATMAP_SOURCE_H
#define OVT_RENDER_SRC_OVERLAY_HEATMAP_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ovt/render/overlay_set.h"

namespace ovt::render::detail {

// Runs fn(begin, end) over [0, count) split into `chunks` contiguous ranges on worker threads (inline when count is
// small). Ranges are disjoint, so a fn that writes only inside its
// range is deterministic.
void parallel_ranges(size_t count, unsigned chunks, const std::function<void(size_t, size_t)>& fn);

struct HeatmapResult {
  std::vector<uint16_t> values;  // cells entries, normalised to 0..65535
  std::vector<uint8_t> valid;    // 0 = not applicable
  float lo = 0, hi = 0;          // NOLINT(readability-isolate-declaration): range applied, source domain
};

// Fills `out` for cfg.heatmap from the CellGrid. `is_av1` selects the QP default range (0..255 instead of 0..51).
// `custom` (cells entries) feeds Custom and MetricMap. Vectors of the wrong length are ignored and noted in `diag`.
// Returns false (out left empty, a note in diag) when the source has no data.
bool build_heatmap(const ovt::fb::CellGrid& grid, bool is_av1, const OverlayConfig& cfg, const uint16_t* custom,
                   HeatmapResult& out, std::vector<std::string>& diag);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_HEATMAP_SOURCE_H
