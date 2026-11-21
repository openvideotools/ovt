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

// C++ twin of `tools/gen_fixture_fb.py --kind grid` (synthetic_4k_grid.fb, §14.3.3; no corpus fetch): HEVC intra,
// CTU 64 in raster order, CU size of CTU (cx, cy) 64 >> ((cx + cy) % 4), CUs > 8 crossing the picture edge split.
std::vector<uint8_t> make_grid_fixture(const GridFixtureOptions& opts = {});

// WI-2.434: the in-memory twin of `tools/gen_fixture_fb.py --kind mv` (render/tests/fixtures/synthetic_4k_mv.fb for
// w x h = 3840 x 2160): one L0 MotionVector per 4x4 cell, ref 1, in the generator's order (64x64 superblocks in
// raster order, cells in raster order inside each), forming a rotation about the picture centre:
//   mv_x = clamp(-(2 y4 - cells_h) * 2, -512, 512), mv_y = clamp((2 x4 - cells_w) * 2, -512, 512)   (1/16 samples)
// Only the fields the arrow pass reads are written (grid size, pred, mv0, and the MotionVector list); the fixture file
// is 45 MB and not in the fast corpora, so the tests fall back to this when it is absent.
std::vector<uint8_t> make_synthetic_mv(uint32_t width, uint32_t height);

// WI-2.436: a 128 x 96 HEVC frame (32 x 24 cells) for the deblocking, reference-index and selection passes:
//   blocks      = 0 root, 1-4 CTUs 64 x 64 in raster order (clipped to the picture), 5-8 the 32 x 32 CUs of CTU 1;
//                 block_id of a cell = the CU (5-8) inside CTU 1, else the CTU
//   deblock_bs_v = 2 where cx % 8 == 0, 1 where cx % 2 == 0 (cx > 0), else 0, and 0 for rows 8 <= cy < 12 (a gap);
//   deblock_bs_h = 2 where cy % 8 == 0, 1 where cy % 2 == 0 (cy > 0), else 0
//   ref0 = (cx / 4 + 2 * (cy / 4)) % 8 for cy < 20, else -1;  ref1 = (cx / 4) % 8 for cy >= 20, else -1
//   mvs  = [0] L0 at (0, 0, 16, 16), [1] L0 at (64, 32, 16, 16), [2] L1 at (96, 64, 32, 32)
std::vector<uint8_t> make_misc_fixture();

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_FIXTURES_MAKE_SYNTHETIC_H
