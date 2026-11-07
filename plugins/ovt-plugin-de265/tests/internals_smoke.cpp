// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Smoke and acceptance tests of libde265 patch 0001, the read-only internals API (WI-2.162, chapter 07 §7.6.2).
// The patched library is built with DE265_INTERNALS=ON (third_party/libde265.cmake). Acceptance test 1 of the WI
// (symbols and object code with DE265_INTERNALS=OFF) is check_internals_abi.py; acceptance test 2 is
// "de265 internals: AMP_A_Samsung_7 CB info" below.

#include <catch2/catch_test_macros.hpp>

#include <libde265/de265.h>
#include <libde265/de265_internals.h>

#include "test_support/corpus.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint8_t kModeIntra = 0;  // libde265 enum PredMode
constexpr std::uint8_t kNotIntra = 255;

/// Called once per decoded picture, in output order, before the picture is released. @p output_index counts the
/// pictures from 0.
using PictureFn = std::function<void(const de265_image& image, int output_index)>;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Decodes the Annex B stream at @p path on the calling thread and passes every picture to @p on_picture. Returns
/// the number of pictures.
int decode_file(const std::filesystem::path& path, const PictureFn& on_picture) {
  const std::vector<std::uint8_t> data = read_file(path);
  de265_decoder_context* ctx = de265_new_decoder();
  REQUIRE(ctx != nullptr);
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, 0);

  int pictures = 0;
  const auto drain = [&] {
    int drained = 0;
    while (const de265_image* image = de265_get_next_picture(ctx)) {
      on_picture(*image, pictures++);
      de265_release_next_picture(ctx);
      ++drained;
    }
    return drained;
  };

  REQUIRE(de265_push_data(ctx, data.data(), static_cast<int>(data.size()), 0, nullptr) == DE265_OK);
  REQUIRE(de265_flush_data(ctx) == DE265_OK);
  // As dec265 does: the last pictures leave the DPB only on further de265_decode() calls, so keep calling while
  // the decoder has more data or still returns pictures.
  for (;;) {
    int more = 0;
    const de265_error err = de265_decode(ctx, &more);
    const int drained = drain();
    INFO("de265_decode: " << de265_get_error_text(err));
    REQUIRE((err == DE265_OK || err == DE265_ERROR_WAITING_FOR_INPUT_DATA));
    if (err != DE265_OK || (more == 0 && drained == 0)) {
      break;
    }
  }
  REQUIRE(de265_get_warning(ctx) == DE265_OK);
  de265_free_decoder(ctx);
  return pictures;
}

struct SpsSizes {
  int ctb_log2 = 0;
  int min_cb_log2 = 0;
  int min_tb_log2 = 0;
  int width_ctbs = 0;
  int height_ctbs = 0;
};

SpsSizes sps_sizes(const de265_image& image) {
  SpsSizes s;
  de265_internals_get_sps_sizes(&image, &s.ctb_log2, &s.min_cb_log2, &s.min_tb_log2, &s.width_ctbs, &s.height_ctbs);
  return s;
}

/// Checks that the CB reported at every MinCb unit of the picture contains that unit and agrees with the CB
/// reported at its top-left unit: the getter is valid at every unit covered by the CB (§7.6.2).
void check_cb_tiling(const de265_image& image) {
  const SpsSizes sps = sps_sizes(image);
  const de265_internals_layout cb = de265_internals_get_CB_info_layout(&image);
  REQUIRE(cb.log2_unit_size == sps.min_cb_log2);
  for (int uy = 0; uy < cb.height_in_units; ++uy) {
    for (int ux = 0; ux < cb.width_in_units; ++ux) {
      de265_internals_cb info{};
      de265_internals_get_CB_info(&image, ux, uy, &info);
      INFO("MinCb (" << ux << "," << uy << ") log2CbSize " << int{info.log2CbSize});
      REQUIRE(info.log2CbSize >= sps.min_cb_log2);
      REQUIRE(info.log2CbSize <= sps.ctb_log2);
      const int shift = info.log2CbSize - cb.log2_unit_size;
      de265_internals_cb origin{};
      de265_internals_get_CB_info(&image, (ux >> shift) << shift, (uy >> shift) << shift, &origin);
      REQUIRE(origin.log2CbSize == info.log2CbSize);
      REQUIRE(origin.PartMode == info.PartMode);
      REQUIRE(origin.PredMode == info.PredMode);
      REQUIRE(origin.ctDepth == info.ctDepth);
      REQUIRE(info.ctDepth == sps.ctb_log2 - info.log2CbSize);
    }
  }
}

std::string cb_info_of_ctu0(const de265_image& image) {
  const SpsSizes sps = sps_sizes(image);
  const de265_internals_layout cb = de265_internals_get_CB_info_layout(&image);
  const int units = 1 << (sps.ctb_log2 - cb.log2_unit_size);
  std::ostringstream out;
  out << "CTU 0 of POC " << de265_internals_get_picture_poc(&image) << " (CTB " << (1 << sps.ctb_log2) << ", MinCb "
      << (1 << sps.min_cb_log2) << "):\n";
  for (int uy = 0; uy < units && uy < cb.height_in_units; ++uy) {
    for (int ux = 0; ux < units && ux < cb.width_in_units; ++ux) {
      de265_internals_cb info{};
      de265_internals_get_CB_info(&image, ux, uy, &info);
      const int size = 1 << info.log2CbSize;
      const int x = ux << cb.log2_unit_size;
      const int y = uy << cb.log2_unit_size;
      if (x % size != 0 || y % size != 0) {
        continue;  // one line per CB, at its top-left unit
      }
      out << "  CB (" << x << "," << y << ") " << size << "x" << size << " PartMode " << int{info.PartMode}
          << " PredMode " << int{info.PredMode} << " ctDepth " << int{info.ctDepth} << " QP_Y " << int{info.QP_Y}
          << " pcm " << int{info.pcm_flag} << " bypass " << int{info.cu_transquant_bypass} << "\n";
    }
  }
  return out.str();
}

}  // namespace

TEST_CASE("de265 internals: getters tolerate a null image and out-of-range units", "[fast][third_party]") {
  CHECK(de265_internals_get_CB_info_layout(nullptr).width_in_units == 0);
  CHECK(de265_internals_get_deblk_info_layout(nullptr).height_in_units == 0);
  CHECK(de265_internals_get_num_slice_headers(nullptr) == 0);
  CHECK(de265_internals_get_picture_poc(nullptr) == 0);
  CHECK(de265_internals_get_CTB_flags(nullptr, 0, 0) == 0);
  CHECK(de265_internals_get_TU_info(nullptr, -1, 3) == 0);
  CHECK(de265_internals_get_deblk_bS_v(nullptr, 0, 0) == 0);

  de265_internals_cb cb{};
  cb.log2CbSize = 9;
  de265_internals_get_CB_info(nullptr, 0, 0, &cb);
  CHECK(cb.log2CbSize == 0);

  de265_internals_pb pb{};
  de265_internals_get_PB_info(nullptr, 0, 0, &pb);
  CHECK(pb.predFlag[0] == 0);
  CHECK(pb.refIdx[0] == -1);
  CHECK(pb.refIdx[1] == -1);

  std::uint8_t mode_y = 0;
  std::uint8_t mode_c = 0;
  de265_internals_get_intra_modes(nullptr, 0, 0, &mode_y, &mode_c);
  CHECK(mode_y == kNotIntra);
  CHECK(mode_c == kNotIntra);

  de265_internals_slice_header sh{};
  sh.SliceQPY = 7;
  de265_internals_get_slice_header(nullptr, 0, &sh);
  CHECK(sh.SliceQPY == 0);

  int ctb_log2 = 5;
  de265_internals_get_sps_sizes(nullptr, &ctb_log2, nullptr, nullptr, nullptr, nullptr);
  CHECK(ctb_log2 == 0);
}

// WI-2.162 step 3: decode WPP_A_ericsson_MAIN_2 and print the CB info of CTU 0.
TEST_CASE("de265 internals: WPP_A_ericsson_MAIN_2 smoke", "[fast][third_party][needs-corpus]") {
  const std::filesystem::path stream = ovt::test::corpus_or_skip("jctvc_WPP_A_ericsson_MAIN_2");
  bool printed = false;
  const int pictures = decode_file(stream, [&](const de265_image& image, int output_index) {
    const SpsSizes sps = sps_sizes(image);
    const de265_internals_layout ctb = de265_internals_get_CTB_info_layout(&image);
    REQUIRE(ctb.log2_unit_size == sps.ctb_log2);
    REQUIRE(ctb.width_in_units == sps.width_ctbs);
    REQUIRE(ctb.height_in_units == sps.height_ctbs);
    REQUIRE(de265_internals_get_num_slice_headers(&image) >= 1);
    for (int y = 0; y < ctb.height_in_units; ++y) {
      for (int x = 0; x < ctb.width_in_units; ++x) {
        REQUIRE(int{de265_internals_get_CTB_slice_header_index(&image, x, y)} <
                de265_internals_get_num_slice_headers(&image));
      }
    }
    check_cb_tiling(image);
    if (output_index == 0) {
      CHECK(de265_internals_get_picture_poc(&image) == 0);
      const std::string text = cb_info_of_ctu0(image);
      WARN(text);  // the smoke output of WI-2.162 step 3
      CHECK(text.find("CB (0,0)") != std::string::npos);
      printed = true;
    }
  });
  CHECK(pictures > 0);
  CHECK(printed);
  WARN("decoded " << pictures << " pictures");
}

// WI-2.162 acceptance test 2.
// SPEC-ASSUMPTION(#215): frame 0 of AMP_A_Samsung_7 is its IDR picture, and AMP (asymmetric partitions) exists
// only in inter CUs, so the AMP part of the test runs over every picture of the stream; the log2CbSize part runs on
// frame 0 (and, through check_cb_tiling, on every picture).
TEST_CASE("de265 internals: AMP_A_Samsung_7 CB info", "[fast][third_party][needs-corpus]") {
  const std::filesystem::path stream = ovt::test::corpus_or_skip("jctvc_AMP_A_Samsung_7");
  std::set<int> part_modes;
  std::set<int> frame0_sizes;
  const int pictures = decode_file(stream, [&](const de265_image& image, int output_index) {
    check_cb_tiling(image);
    const de265_internals_layout cb = de265_internals_get_CB_info_layout(&image);
    for (int uy = 0; uy < cb.height_in_units; ++uy) {
      for (int ux = 0; ux < cb.width_in_units; ++ux) {
        de265_internals_cb info{};
        de265_internals_get_CB_info(&image, ux, uy, &info);
        part_modes.insert(info.PartMode);
        if (output_index == 0) {
          frame0_sizes.insert(info.log2CbSize);
        }
      }
    }
  });
  REQUIRE(pictures > 1);
  REQUIRE(!frame0_sizes.empty());
  CHECK(*frame0_sizes.begin() >= 3);
  CHECK(*frame0_sizes.rbegin() <= 6);
  for (const int amp : {4, 5, 6, 7}) {
    INFO("PartMode " << amp);
    CHECK(part_modes.count(amp) == 1);
  }
}

// deblk_bS_v (the one decoding-code change of patch 0001): the horizontal-edge pass of libde265 visits only every
// second 4x4 row, so on the other rows the stock deblk_info byte still holds the vertical edge's bS, which must equal
// deblk_bS_v. Intra pictures have bS 2 on every vertical edge of the 8x8 grid.
TEST_CASE("de265 internals: deblk_bS_v keeps the vertical-edge bS", "[fast][third_party][needs-corpus]") {
  const std::filesystem::path stream = ovt::test::corpus_or_skip("jctvc_WPP_A_ericsson_MAIN_2");
  int nonzero_inter = 0;
  decode_file(stream, [&](const de265_image& image, int /*output_index*/) {
    const de265_internals_layout d = de265_internals_get_deblk_info_layout(&image);
    REQUIRE(d.log2_unit_size == 2);
    const de265_internals_layout ctb = de265_internals_get_CTB_info_layout(&image);
    for (int y4 = 0; y4 < d.height_in_units; ++y4) {
      for (int x4 = 0; x4 < d.width_in_units; ++x4) {
        const std::uint8_t flags = de265_internals_get_deblk_flags(&image, x4, y4);
        const std::uint8_t bs_v = de265_internals_get_deblk_bS_v(&image, x4, y4);
        INFO("4x4 (" << x4 << "," << y4 << ") flags " << int{flags} << " bS_v " << int{bs_v});
        REQUIRE(bs_v <= 2);
        const bool vertical_edge =
            (flags & (DE265_INTERNALS_DEBLOCK_FLAG_VERTI | DE265_INTERNALS_DEBLOCK_PB_EDGE_VERTI)) != 0;
        if (!vertical_edge || x4 % 2 != 0) {
          REQUIRE(bs_v == 0);
          continue;
        }
        const int shift = ctb.log2_unit_size - 2;
        const int slice = de265_internals_get_CTB_slice_header_index(&image, x4 >> shift, y4 >> shift);
        de265_internals_slice_header sh{};
        de265_internals_get_slice_header(&image, slice, &sh);
        if (y4 % 2 != 0) {
          REQUIRE(bs_v == (flags & DE265_INTERNALS_DEBLOCK_BS_MASK));
        }
        de265_internals_cb q{};
        const int cb_shift = de265_internals_get_CB_info_layout(&image).log2_unit_size - 2;
        de265_internals_get_CB_info(&image, x4 >> cb_shift, y4 >> cb_shift, &q);
        if (sh.slice_type == 2 /* I */) {
          REQUIRE(bs_v == 2);
        } else if (q.PredMode != kModeIntra && bs_v != 0) {
          ++nonzero_inter;
        }
      }
    }
  });
  CHECK(nonzero_inter > 0);
}

TEST_CASE("de265 internals: PB motion, intra modes and slice headers agree", "[fast][third_party][needs-corpus]") {
  const std::filesystem::path stream = ovt::test::corpus_or_skip("jctvc_WPP_A_ericsson_MAIN_2");
  int inter_pbs = 0;
  int intra_blocks = 0;
  decode_file(stream, [&](const de265_image& image, int /*output_index*/) {
    const int poc = de265_internals_get_picture_poc(&image);
    const de265_internals_layout pb = de265_internals_get_PB_info_layout(&image);
    const de265_internals_layout intra = de265_internals_get_intra_info_layout(&image);
    const de265_internals_layout tu = de265_internals_get_TU_info_layout(&image);
    REQUIRE(pb.log2_unit_size == 2);
    REQUIRE(intra.log2_unit_size == 2);
    REQUIRE(tu.log2_unit_size == 2);
    const int cb_shift = de265_internals_get_CB_info_layout(&image).log2_unit_size - 2;
    const int ctb_shift = de265_internals_get_CTB_info_layout(&image).log2_unit_size - 2;
    const de265_internals_layout d = de265_internals_get_deblk_info_layout(&image);
    for (int y4 = 0; y4 < d.height_in_units; ++y4) {
      for (int x4 = 0; x4 < d.width_in_units; ++x4) {
        de265_internals_cb cb{};
        de265_internals_get_CB_info(&image, x4 >> cb_shift, y4 >> cb_shift, &cb);
        de265_internals_pb motion{};
        de265_internals_get_PB_info(&image, x4, y4, &motion);
        std::uint8_t mode_y = 0;
        std::uint8_t mode_c = 0;
        de265_internals_get_intra_modes(&image, x4, y4, &mode_y, &mode_c);
        INFO("POC " << poc << " 4x4 (" << x4 << "," << y4 << ") PredMode " << int{cb.PredMode});
        if (cb.PredMode == kModeIntra) {
          ++intra_blocks;
          REQUIRE(mode_y <= 34);
          REQUIRE(mode_c <= 34);
          REQUIRE(motion.predFlag[0] == 0);
          REQUIRE(motion.predFlag[1] == 0);
          REQUIRE(motion.refIdx[0] == -1);
          continue;
        }
        REQUIRE(mode_y == kNotIntra);
        REQUIRE((motion.predFlag[0] != 0 || motion.predFlag[1] != 0));
        ++inter_pbs;
        de265_internals_slice_header sh{};
        de265_internals_get_slice_header(
            &image, de265_internals_get_CTB_slice_header_index(&image, x4 >> ctb_shift, y4 >> ctb_shift), &sh);
        REQUIRE(sh.slice_type != 2);
        for (int list = 0; list < 2; ++list) {
          if (motion.predFlag[list] == 0) {
            REQUIRE(motion.refIdx[list] == -1);
            continue;
          }
          const int active = list == 0 ? sh.num_ref_idx_l0_active : sh.num_ref_idx_l1_active;
          REQUIRE(motion.refIdx[list] >= 0);
          REQUIRE(motion.refIdx[list] < active);
          REQUIRE(motion.refPOC[list] == sh.RefPicList_POC[list][motion.refIdx[list]]);
          REQUIRE(motion.refPOC[list] != poc);
        }
      }
    }
  });
  CHECK(inter_pbs > 0);
  CHECK(intra_blocks > 0);
}
