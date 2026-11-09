// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ovt_plugin_de265_grid_dump: the libde265 oracle of WI-4.107 acceptance test 1 (chapter 07 §7.8). It decodes an Annex
// B H.265 stream with the patched libde265 (patch 0001, the read-only internals API) and prints, for every picture in
// output order, the prediction mode, QP_Y and motion vectors of each 4x4 luma cell of the coded picture:
//
//     picture <output index> <poc> <cells_w> <cells_h>
//     <pred> <qp> <pf0> <mv0_x> <mv0_y> <pf1> <mv1_x> <mv1_y>      (one line per cell, raster order)
//
// pred is 1 intra, 2 inter (skip included), 0 not decoded; vectors are in quarter samples and 0 when the list is
// unused. test_ffmpeg_hevc_sidedata.cpp compares the ffmpeg_hevc plugin with it. A separate executable because libde265
// is LGPL-3.0-or-later and the plugin tests are LGPL-2.1-or-later (chapter 15 §15.4.2).
//
//     ovt_plugin_de265_grid_dump <stream.265>

#include <libde265/de265.h>
#include <libde265/de265_internals.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

void print_picture(const de265_image* img, int index) {
  const de265_internals_layout pb = de265_internals_get_PB_info_layout(img);
  const de265_internals_layout cb = de265_internals_get_CB_info_layout(img);
  std::printf("picture %d %d %d %d\n", index, de265_internals_get_picture_poc(img), pb.width_in_units,
              pb.height_in_units);
  const int cb_shift = cb.log2_unit_size - pb.log2_unit_size;
  for (int y = 0; y < pb.height_in_units; ++y) {
    for (int x = 0; x < pb.width_in_units; ++x) {
      de265_internals_cb c{};
      de265_internals_get_CB_info(img, x >> cb_shift, y >> cb_shift, &c);
      de265_internals_pb p{};
      de265_internals_get_PB_info(img, x, y, &p);
      int pred = 0;
      if (c.log2CbSize != 0) {
        pred = c.PredMode == 0 ? 1 : 2;
      }
      std::printf("%d %d %d %d %d %d %d %d\n", pred, int{c.QP_Y}, int{p.predFlag[0]}, int{p.mv_x[0]}, int{p.mv_y[0]},
                  int{p.predFlag[1]}, int{p.mv_x[1]}, int{p.mv_y[1]});
    }
  }
}

// The NAL units of an Annex B byte stream, without start codes and trailing zero bytes.
std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<std::vector<uint8_t>> nals;
  size_t start = SIZE_MAX;
  const auto close = [&](size_t end) {
    while (end > start && s[end - 1] == 0) {
      --end;
    }
    if (end > start) {
      nals.emplace_back(s.begin() + static_cast<std::ptrdiff_t>(start), s.begin() + static_cast<std::ptrdiff_t>(end));
    }
  };
  for (size_t i = 0; i + 3 <= s.size();) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        close(i);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX) {
    close(s.size());
  }
  return nals;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    (void)std::fprintf(stderr, "usage: ovt_plugin_de265_grid_dump <stream.265>\n");
    return 2;
  }
  const char* path = argv[1];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): main's argument vector
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    (void)std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }
  const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  de265_decoder_context* ctx = de265_new_decoder();
  if (ctx == nullptr) {
    return 1;
  }
  de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, 0);
  int pictures = 0;
  const auto drain = [&] {
    int drained = 0;
    while (const de265_image* img = de265_get_next_picture(ctx)) {
      print_picture(img, pictures++);
      de265_release_next_picture(ctx);
      ++drained;
    }
    return drained;
  };
  int rc = 0;
  // Decodes what is queued, as dec265 does: keep calling de265_decode() while it has more to do or returns pictures.
  const auto decode = [&] {
    for (;;) {
      int more = 0;
      const de265_error err = de265_decode(ctx, &more);
      const int drained = drain();
      if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
        (void)std::fprintf(stderr, "de265_decode: %s\n", de265_get_error_text(err));
        rc = 1;
      }
      if (err != DE265_OK || (more == 0 && drained == 0)) {
        return;
      }
    }
  };
  // One NAL unit per call, decoding in between, as the de265 plugin does.
  for (const std::vector<uint8_t>& nal : split_annexb(data)) {
    if (de265_push_NAL(ctx, nal.data(), static_cast<int>(nal.size()), 0, nullptr) != DE265_OK) {
      rc = 1;
    }
    decode();
  }
  if (de265_flush_data(ctx) != DE265_OK) {
    rc = 1;
  }
  decode();
  de265_free_decoder(ctx);
  return rc;
}
