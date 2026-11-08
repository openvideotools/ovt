// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Inspection-callback smoke program of the libaom build (WI-2.154 step 5, chapter 07 §7.6.1): the bundled libaom
// (third_party/libaom.cmake, CONFIG_INSPECTION=1) calls the AV1_SET_INSPECTION_CALLBACK callback once per decoded
// frame, and the callback can read the frame through the semi-public av1/decoder/inspection.h, which comes from
// LIBAOM_SOURCE_DIR (tp::aom_inspection), not from installed headers. Acceptance test 1 of WI-2.154.

#include <catch2/catch_test_macros.hpp>

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

#include <av1/decoder/inspection.h>

#include "test_support/corpus.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

struct Inspected {
  insp_frame_data fd{};
  int callbacks = 0;
  int mi_rows = 0;
  int mi_cols = 0;
};

// Runs on the decoding thread inside aom_codec_decode (chapter 07 §7.6.1); it only reads the frame.
void on_frame(void* decoder, void* ctx) {
  auto* inspected = static_cast<Inspected*>(ctx);
  ifd_inspect(&inspected->fd, decoder, 0);
  inspected->mi_rows = inspected->fd.mi_rows;
  inspected->mi_cols = inspected->fd.mi_cols;
  ++inspected->callbacks;
}

std::uint32_t le(const std::vector<std::uint8_t>& b, std::size_t at, int bytes) {
  std::uint32_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) {
    v = (v << 8U) | b.at(at + static_cast<std::size_t>(i));
  }
  return v;
}

struct Counts {
  int ivf_frames = 0;
  int decoded = 0;  // aom_codec_decode iterations that decoded a frame (not show_existing_frame)
  int shown = 0;    // frames returned by aom_codec_get_frame
};

// Decodes one IVF frame (a temporal unit) on libaom's inspect path, which is taken when user_priv is an
// Av1DecodeReturn and is the only path that honours the inspection callback (D30). That path decodes one frame per
// call and returns where the next one starts.
void decode_temporal_unit(aom_codec_ctx_t& codec, const std::uint8_t* data, const std::uint8_t* end, Counts& counts) {
  while (data < end) {
    Av1DecodeReturn adr{};
    REQUIRE(aom_codec_decode(&codec, data, static_cast<std::size_t>(end - data), &adr) == AOM_CODEC_OK);
    REQUIRE(adr.buf > data);
    data = adr.buf;
    if (adr.show_existing == 0) {
      ++counts.decoded;
    }
    aom_codec_iter_t iter = nullptr;
    while (aom_codec_get_frame(&codec, &iter) != nullptr) {
      ++counts.shown;
    }
  }
}

Counts decode_ivf(aom_codec_ctx_t& codec, const std::vector<std::uint8_t>& ivf) {
  Counts counts;
  for (std::size_t at = 32; at + 12 <= ivf.size(); ++counts.ivf_frames) {
    const std::size_t size = le(ivf, at, 4);
    REQUIRE(at + 12 + size <= ivf.size());
    const std::uint8_t* data = ivf.data() + at + 12;
    decode_temporal_unit(codec, data, data + size, counts);
    at += 12 + size;
  }
  return counts;
}

struct Result {
  Counts counts;
  int callbacks = 0;
  int mi_rows = 0;
  int mi_cols = 0;
};

// Decodes the whole IVF file with the inspection callback registered.
Result inspect_ivf(const std::vector<std::uint8_t>& ivf) {
  aom_codec_ctx_t codec{};
  aom_codec_dec_cfg_t cfg{};
  cfg.allow_lowbitdepth = 1;
  REQUIRE(aom_codec_dec_init(&codec, aom_codec_av1_dx(), &cfg, 0) == AOM_CODEC_OK);
  Inspected inspected;
  ifd_init(&inspected.fd, static_cast<int>(le(ivf, 12, 2)), static_cast<int>(le(ivf, 14, 2)));
  aom_inspect_init init{on_frame, &inspected};
  REQUIRE(aom_codec_control(&codec, AV1_SET_INSPECTION_CALLBACK, &init) == AOM_CODEC_OK);
  Result result{decode_ivf(codec, ivf), 0, 0, 0};
  REQUIRE(aom_codec_destroy(&codec) == AOM_CODEC_OK);
  ifd_clear(&inspected.fd);
  result.callbacks = inspected.callbacks;
  result.mi_rows = inspected.mi_rows;
  result.mi_cols = inspected.mi_cols;
  return result;
}

std::vector<std::uint8_t> read_ivf(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<std::uint8_t> ivf{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(ivf.size() >= 32);
  REQUIRE(le(ivf, 0, 4) == 0x46494B44U);  // "DKIF"
  return ivf;
}

}  // namespace

TEST_CASE("libaom inspection callback fires once per frame of av1-1-b8-01-size-16x16",
          "[fast][third_party][needs-corpus]") {
  const std::vector<std::uint8_t> ivf = read_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_16x16"));
  const Result r = inspect_ivf(ivf);
  CHECK(r.counts.ivf_frames == static_cast<int>(le(ivf, 24, 4)));
  CHECK(r.counts.shown == 2);  // the vector's .md5 file lists two frames
  CHECK(r.counts.decoded == 2);
  CHECK(r.callbacks == r.counts.decoded);
  CHECK(r.mi_cols == 4);  // 16 / 4: one MI is 4x4 luma samples
  CHECK(r.mi_rows == 4);
}
