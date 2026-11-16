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

struct GridFixtureOptions {
  uint32_t width = 3840;  // luma samples; 3840 x 2160 is the 4K fixture of §10.9.2 item 4
  uint32_t height = 2160;
  bool tus = false;        // each CU additionally gets four TU children (half its size): exercises TU culling
  bool tiles_2x2 = false;  // four Tile partitions split at the picture centre (slice/tile boundary pass)
};

// The partition-grid fixture of `tools/gen_fixture_fb.py --kind grid` (synthetic_4k_grid.fb, chapter 14 §14.3.3),
// built in C++ so the render tests need no corpus fetch: an H.265 intra picture, CTU 64 in raster order, the CU size
// of CTU (cx, cy) is 64 >> ((cx + cy) % 4), and a CU larger than 8 that crosses the picture edge is split further.
std::vector<uint8_t> make_grid_fixture(const GridFixtureOptions& opts = {});

// WI-2.434: the in-memory twin of `tools/gen_fixture_fb.py --kind mv` (render/tests/fixtures/synthetic_4k_mv.fb for
// w x h = 3840 x 2160): one L0 MotionVector per 4x4 cell, ref 1, in the generator's order (64x64 superblocks in
// raster order, cells in raster order inside each), forming a rotation about the picture centre:
//   mv_x = clamp(-(2 y4 - cells_h) * 2, -512, 512), mv_y = clamp((2 x4 - cells_w) * 2, -512, 512)   (1/16 samples)
// Only the fields the arrow pass reads are written (grid size, pred, mv0, and the MotionVector list); the fixture file
// is 45 MB and not in the fast corpora, so the tests fall back to this when it is absent.
std::vector<uint8_t> make_synthetic_mv(uint32_t width, uint32_t height);

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_FIXTURES_MAKE_SYNTHETIC_H
