// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Synthetic FrameAnalysis for the overlay tests (WI-2.430). Deterministic closed-form cell values so a test can
// compute the expected texel of any cell; the same generator is reused by the later overlay work items (WI-2.431...).

#ifndef OVT_RENDER_TESTS_FIXTURES_MAKE_SYNTHETIC_H
#define OVT_RENDER_TESTS_FIXTURES_MAKE_SYNTHETIC_H

#include <cstdint>
#include <vector>

namespace ovt::render::test {

struct SyntheticOptions {
  uint32_t cells_w = 80;  // 80 x 64 = 5120 cells: above the builder's threshold for the multi-threaded path
  uint32_t cells_h = 64;
  bool av1 = false;        // plugin_id / filter_params_type say AV1 (QP default range 0..255)
  bool drop_pred = false;  // CellGrid.pred empty (plugin did not provide it)
  bool short_qp = false;   // CellGrid.qp one entry short (malformed)
};

// Cell (cx, cy), with i = cy * cells_w + cx:
//   qp          = (7 cx + 3 cy) % 52, except cell (0, 0) = -1 (not applicable)
//   pred        = Skip at (7, 5), Intra at (1, 0), Inter elsewhere
//   ref0        = cx % 3 - 1  (-1, 0, 1);  ref1 = 1 where cx % 5 == 0, else -1
//   mv0         = (4 cx - 100, cy)  in 1/16 sample units
//   bits_x8     = 8 * cx * cy
//   tx_log2_w/h = 2 + cx % 4 / 2 + 2 * (cy % 2 == 0 ? 0 : 0) (4x4 .. 8x8), 0xFF at (0, 0)
//   block_id    = 0 for cx < cells_w / 2, else 1; block 1 is skip + merge
// Partitions: two tiles side by side, a slice segment and a tile group (the last two are not spatial boundaries).
std::vector<uint8_t> make_synthetic(const SyntheticOptions& opts = {});

int expected_qp(uint32_t cx, uint32_t cy);

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_FIXTURES_MAKE_SYNTHETIC_H
