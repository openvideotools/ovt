// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Smoke and acceptance tests of ovt-plugin-ffmpeg (WI-2.169, chapter 07 §7.6.3), loaded through the host's
// PluginRegistry (WI-2.150) and driven through its ABI only. Synthetic I_PCM streams have known samples, so planes,
// decode_index and slice threading are checked without a corpus. AT1: the 10 `h264-analysis-smoke` JVT streams
// against `ffmpeg -f framemd5` (the in-tree oracle). AT2: ovt_plugin_describe() carries avcodec_configuration().

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <flatbuffers/flatbuffers.h>

#include <dlfcn.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/md5.h>  // MD5 of the core FFmpeg flavour that the host links anyway
}

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"
#include "plugin_test_util.inc"
#include "test_support/corpus.h"

namespace {

using namespace ovt_ffmpeg_test;  // NOLINT(google-build-using-namespace)

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

std::string md5_hex(const std::vector<uint8_t>& bytes) {
  std::array<uint8_t, 16> d{};
  av_md5_sum(d.data(), bytes.data(), bytes.size());
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string hex;
  for (const uint8_t b : d) {
    hex += kHex[b >> 4];
    hex += kHex[b & 15U];
  }
  return hex;
}

// ---- synthetic I_PCM streams ----------------------------------------------------------------------------------------

// ---- driving the plugin -------------------------------------------------------------------------------------------

struct OutFrame {
  int decode_index = -1;
  int64_t pts = 0;
  ovt::fb::FrameType type = ovt::fb::FrameType::Unknown;
  bool is_rap = false;
  std::array<std::vector<uint8_t>, 3> planes;  // coded size
  std::string cropped_md5;  // MD5 of the cropped Y, U, V planes packed back to back, as framemd5 hashes them
  int diagnostics = 0;
};

// Pulls every ready frame, with its planes.
void drain(Instance& in, std::vector<OutFrame>* out) {
  for (;;) {
    ovt_Buffer fb{};
    REQUIRE(in.api.pull(in.dec, kReconMask, &fb) == OVT_OK);
    if (fb.data == nullptr) {
      return;
    }
    const ovt::fb::FrameAnalysis* fa = as_fa(fb);
    REQUIRE(fa->frame() != nullptr);
    REQUIRE(fa->frame()->id() != nullptr);
    CHECK(fa->plugin_id()->str() == "ffmpeg");
    OutFrame f;
    f.decode_index = fa->frame()->id()->decode_index();
    f.pts = fa->frame()->pts();
    f.type = fa->frame()->type();
    f.is_rap = fa->frame()->is_rap();
    f.diagnostics = fa->diagnostics() != nullptr ? static_cast<int>(fa->diagnostics()->size()) : 0;
    const auto* coded = fa->frame()->coded_size();
    const auto* crop = fa->frame()->crop();
    REQUIRE(coded != nullptr);
    REQUIRE(crop != nullptr);
    REQUIRE(fa->grid() != nullptr);
    CHECK(static_cast<int>(fa->grid()->cells_w()) == (coded->w() + 3) / 4);
    CHECK(static_cast<int>(fa->grid()->cells_h()) == (coded->h() + 3) / 4);
    REQUIRE(fa->planes() != nullptr);
    std::vector<uint8_t> cropped;
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      CHECK(p->stage() == ovt::fb::PlaneStage::Reconstruction);
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(in.api.plane(in.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      REQUIRE(p->stride_bytes() == p->width() * p->sample_bytes());
      REQUIRE(size == static_cast<size_t>(p->stride_bytes()) * p->height());
      const auto c = static_cast<size_t>(p->plane());
      REQUIRE(c < 3);
      if (c == 0) {
        CHECK(static_cast<int>(p->width()) == coded->w());
        CHECK(static_cast<int>(p->height()) == coded->h());
      }
      f.planes[c].assign(data, data + size);
      // The conformance window in this plane's sampling grid.
      const int sx = c == 0 || p->width() == static_cast<uint32_t>(coded->w()) ? 0 : 1;
      const int sy = c == 0 || p->height() == static_cast<uint32_t>(coded->h()) ? 0 : 1;
      const size_t bpp = p->sample_bytes();
      const auto x0 = static_cast<size_t>(crop->x() >> sx);
      const auto y0 = static_cast<size_t>(crop->y() >> sy);
      const auto cw = static_cast<size_t>((crop->w() + sx) >> sx);
      const auto ch = static_cast<size_t>((crop->h() + sy) >> sy);
      for (size_t y = y0; y < y0 + ch; ++y) {
        const uint8_t* row = data + (y * p->stride_bytes()) + (x0 * bpp);
        cropped.insert(cropped.end(), row, row + (cw * bpp));
      }
    }
    f.cropped_md5 = md5_hex(cropped);
    out->push_back(std::move(f));
    in.api.release(in.dec, fb);
  }
}

// ---- Annex B input --------------------------------------------------------------------------------------------

struct StreamResult {
  int access_units = 0;
  std::vector<OutFrame> frames;  // pull() order
  int push_errors = 0;
};

// Pushes a stream NAL by NAL the way the host does (chapter 07 §7.5.3): decode_index_hint = the access unit's index,
// where an access unit starts at an AUD/SEI/SPS/PPS after a slice, or at a slice with first_mb_in_slice = 0 after a
// slice (H.264 7.4.1.2.3; the JVT streams of AT1 use neither ASO nor redundant pictures).
StreamResult run_stream(const std::vector<uint8_t>& bytes, int threads) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, threads);
  REQUIRE(in.status == OVT_OK);
  StreamResult r;
  int au = 0;
  bool seen_vcl = false;
  for (const Nal& n : split_annexb(bytes)) {
    if (n.size < 2) {
      continue;
    }
    const int type = n.data[0] & 0x1F;
    const bool vcl = type >= 1 && type <= 5;
    const bool starts_au = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
    if (seen_vcl && ((vcl && (n.data[1] & 0x80) != 0) || starts_au)) {
      ++au;
      seen_vcl = false;
    }
    seen_vcl = seen_vcl || vcl;
    if (api.push(in.dec, n.data, n.size, au, 1000 + au) != OVT_OK) {
      ++r.push_errors;
    }
    drain(in, &r.frames);
  }
  REQUIRE(api.flush(in.dec) == OVT_OK);
  drain(in, &r.frames);
  r.access_units = au + 1;
  return r;
}

// `ovt_plugin_ffmpeg_framemd5 <file>`: "<access unit index> <md5>" per output frame, in output order.
std::vector<std::pair<int, std::string>> framemd5_oracle(const std::filesystem::path& file) {
  const std::string cmd = std::string("\"") + OVT_FFMPEG_FRAMEMD5 + "\" \"" + file.string() + "\"";
  FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): the oracle executable of this build
  REQUIRE(p != nullptr);
  std::string text;
  std::array<char, 4096> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(pclose(p) == 0);
  std::vector<std::pair<int, std::string>> out;
  std::istringstream lines(text);
  int index = 0;
  std::string md5;
  while (lines >> index >> md5) {
    out.emplace_back(index, md5);
  }
  return out;
}

// AT1 for one stream: one FrameAnalysis per output frame, each access unit at most once, with the decode index and
// cropped-picture MD5 of `ffmpeg -f framemd5` (the oracle), in output order.
void md5_vs_framemd5(const std::string& id, int threads) {
  CAPTURE(id, threads);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::vector<std::pair<int, std::string>> want = framemd5_oracle(path);
  REQUIRE(!want.empty());
  const StreamResult r = run_stream(read_file(path), threads);
  CHECK(r.push_errors == 0);
  REQUIRE(r.frames.size() == want.size());
  std::set<int> seen;
  int mismatches = 0;
  for (size_t i = 0; i < want.size(); ++i) {
    const OutFrame& f = r.frames[i];
    CHECK(seen.insert(f.decode_index).second);
    CHECK((f.decode_index >= 0 && f.decode_index < r.access_units));
    CHECK(f.pts == 1000 + f.decode_index);
    if (f.decode_index != want[i].first || f.cropped_md5 != want[i].second) {
      ++mismatches;
      UNSCOPED_INFO("output frame " << i << ": decode index " << f.decode_index << " md5 " << f.cropped_md5
                                    << ", framemd5 access unit " << want[i].first << " md5 " << want[i].second);
    }
  }
  CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("ffmpeg plugin: caps and ABI table", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  CHECK(api.struct_size == sizeof(ovt_AnalysisDecoderApi));
  CHECK(api.abi_version == OVT_ANALYSIS_ABI_VERSION);
  const ovt_AnalysisCaps* caps = api.caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->struct_size == sizeof(ovt_AnalysisCaps));
  CHECK(caps->codec == 1);  // ovt.fb.Codec.H264
  CHECK(std::string_view(caps->id) == "ffmpeg");
  CHECK(std::string_view(caps->version).starts_with("0.1.0+ffmpeg."));
  const std::string_view lic(caps->license);
  CHECK((lic == "LGPL-2.1-or-later" || lic == "GPL-2.0-or-later" || lic == "LGPL-3.0-or-later" ||
         lic == "GPL-3.0-or-later"));
  CHECK(caps->plane_stages == kReconMask);
  const uint32_t phase1 = OVT_GRID_BLOCK_ID | OVT_GRID_PRED | OVT_GRID_QP | OVT_GRID_MV0 | OVT_GRID_MV1 |
                          OVT_GRID_REF0 | OVT_GRID_REF1;  // WI-2.170, §7.6.3 caps row
  const uint32_t phase4 = OVT_GRID_PARTITION_ID | OVT_GRID_INTRA_MODE_Y | OVT_GRID_INTRA_MODE_C | OVT_GRID_TX_LOG2 |
                          OVT_GRID_CBF;  // WI-4.102: "with the phase-4 patch: bits 1, 4, 5, 8, 9, 10, 12"
#if !OVT_FFMPEG_SYSTEM
  CHECK(caps->grid_fields == (phase1 | phase4));  // the plugin flavour always carries patch 0001
#else
  CHECK((caps->grid_fields == phase1 || caps->grid_fields == (phase1 | phase4)));
#endif
  CHECK(caps->has_symbol_accounting == 0);
  CHECK(caps->bit_exact == 1);
  CHECK(caps->supports_threads == 1);
  CHECK(caps->notes != nullptr);
}

TEST_CASE("ffmpeg plugin: ovt_plugin_describe carries avcodec_configuration", "[fast][third_party]") {
  // AT2. The describe string is the plugin's `ffmpeg -version` equivalent; the CI log keeps it.
  using DescribeFn = const char* (*)();
  void* sym = dlsym(plugin().handle, "ovt_plugin_describe");
  REQUIRE(sym != nullptr);
  const auto describe = reinterpret_cast<DescribeFn>(sym);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  const char* text = describe();
  REQUIRE(text != nullptr);
  const std::string s(text);
  std::printf("ovt_plugin_describe(): %s\n", s.c_str());
  CHECK(s.find("avcodec_configuration(): ") != std::string::npos);
#if !OVT_FFMPEG_SYSTEM
  CHECK(s.find("--enable-decoder='h264") != std::string::npos);  // configure quotes values with a comma
#if defined(__ANDROID__) && OVT_FFMPEG_HW_DECODE
  CHECK(s.find("--enable-mediacodec") != std::string::npos);
#endif
#endif
}

TEST_CASE("ffmpeg plugin: I_PCM pictures come out with their decode index and exact samples", "[fast][third_party]") {
  const int threads = GENERATE(1, 4);  // 4: slice threads must keep the decode index (§7.6.3 pitfall)
  CAPTURE(threads);
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, threads);
  REQUIRE(in.status == OVT_OK);
  const std::vector<uint8_t> sps = pcm_sps();
  const std::vector<uint8_t> pps = pcm_pps();
  constexpr int kPictures = 12;
  std::vector<OutFrame> out;
  for (int pic = 0; pic < kPictures; ++pic) {
    const int hint = 100 + pic;  // decode indices need not start at 0
    if (pic == 0) {
      REQUIRE(api.push(in.dec, sps.data(), sps.size(), hint, 5000) == OVT_OK);
      REQUIRE(api.push(in.dec, pps.data(), pps.size(), hint, 5000) == OVT_OK);
    }
    const std::vector<uint8_t> idr = pcm_idr(pic);
    REQUIRE(api.push(in.dec, idr.data(), idr.size(), hint, 5000 + (pic * 40)) == OVT_OK);
    drain(in, &out);
  }
  REQUIRE(api.flush(in.dec) == OVT_OK);
  drain(in, &out);
  REQUIRE(out.size() == kPictures);
  for (int pic = 0; pic < kPictures; ++pic) {
    const OutFrame& f = out[static_cast<size_t>(pic)];
    CAPTURE(pic);
    CHECK(f.decode_index == 100 + pic);  // IDR pictures are output in decode order
    CHECK(f.pts == 5000 + (pic * 40));
    CHECK(f.type == ovt::fb::FrameType::I);
    CHECK(f.is_rap);
    // ANA-H264-0002 when intra is inferred (phase 1); with the block params I_PCM is known (the decoder reports no
    // error either way).
    CHECK(f.diagnostics == (has_block_params() ? 0 : 1));
    for (int c = 0; c < 3; ++c) {
      CHECK(f.planes[static_cast<size_t>(c)] == pcm_plane(pic, c));
    }
  }
}

TEST_CASE("ffmpeg plugin: avcC parameter sets are sent with the first access unit", "[fast][third_party]") {
  const std::vector<uint8_t> sps = pcm_sps();
  const std::vector<uint8_t> pps = pcm_pps();
  std::vector<uint8_t> avcc = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1};
  avcc.push_back(static_cast<uint8_t>(sps.size() >> 8));
  avcc.push_back(static_cast<uint8_t>(sps.size()));
  avcc.insert(avcc.end(), sps.begin(), sps.end());
  avcc.push_back(1);
  avcc.push_back(static_cast<uint8_t>(pps.size() >> 8));
  avcc.push_back(static_cast<uint8_t>(pps.size()));
  avcc.insert(avcc.end(), pps.begin(), pps.end());
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, 1, avcc.data(), avcc.size());
  REQUIRE(in.status == OVT_OK);
  const std::vector<uint8_t> idr = pcm_idr(3);
  REQUIRE(api.push(in.dec, idr.data(), idr.size(), 0, 0) == OVT_OK);
  REQUIRE(api.flush(in.dec) == OVT_OK);
  std::vector<OutFrame> out;
  drain(in, &out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].planes[0] == pcm_plane(3, 0));
}

TEST_CASE("ffmpeg plugin: malformed avcC and arguments are rejected", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  ovt_AnalysisDecoder* dec = nullptr;
  const std::array<uint8_t, 4> short_avcc = {1, 66, 0, 10};
  CHECK(api.create(short_avcc.data(), short_avcc.size(), 1, &dec) == OVT_E_CORRUPT);
  // One SPS of 0x40 bytes announced, three present.
  const std::vector<uint8_t> truncated = {1, 66, 0, 10, 0xFF, 0xE1, 0x00, 0x40, 0x67, 0x42, 0x00};
  CHECK(api.create(truncated.data(), truncated.size(), 1, &dec) == OVT_E_CORRUPT);
  CHECK(dec == nullptr);
  CHECK(api.create(nullptr, 0, 1, nullptr) == OVT_E_INVALID_ARG);
  const Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, nullptr, 0, 0, 0) == OVT_E_INVALID_ARG);
  const uint8_t* data = nullptr;
  size_t size = 0;
  CHECK(api.plane(in.dec, "0/recon/y", &data, &size) == OVT_E_INVALID_ARG);
  ovt_Buffer fb{};
  REQUIRE(api.pull(in.dec, kReconMask, &fb) == OVT_OK);
  CHECK(fb.data == nullptr);
  REQUIRE(api.flush(in.dec) == OVT_OK);
  const std::vector<uint8_t> pps = pcm_pps();
  CHECK(api.push(in.dec, pps.data(), pps.size(), 1, 0) == OVT_E_INVALID_ARG);  // never pushed to after flush
}

TEST_CASE("ffmpeg plugin: garbage units never crash the plugin", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const Instance in(api, 2);
  REQUIRE(in.status == OVT_OK);
  uint32_t state = 0x2169U;  // xorshift32: deterministic garbage
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  const std::vector<uint8_t> sps = pcm_sps();
  const std::vector<uint8_t> pps = pcm_pps();
  REQUIRE(api.push(in.dec, sps.data(), sps.size(), 0, 0) == OVT_OK);
  REQUIRE(api.push(in.dec, pps.data(), pps.size(), 0, 0) == OVT_OK);
  for (int i = 0; i < 400; ++i) {
    std::vector<uint8_t> unit(1 + (next() % 200));
    for (uint8_t& b : unit) {
      b = static_cast<uint8_t>(next());
    }
    unit[0] = static_cast<uint8_t>((unit[0] & 0x7FU) | 0x60U);  // forbidden_zero_bit 0, nal_ref_idc 3
    const ovt_status st = api.push(in.dec, unit.data(), unit.size(), i / 3, i);
    CHECK((st == OVT_OK || st == OVT_E_CORRUPT));
    ovt_Buffer fb{};
    while (api.pull(in.dec, kReconMask, &fb) == OVT_OK && fb.data != nullptr) {
      (void)as_fa(fb);
      api.release(in.dec, fb);
    }
  }
  const ovt_status st = api.flush(in.dec);
  CHECK((st == OVT_OK || st == OVT_E_CORRUPT));
}

TEST_CASE("ffmpeg plugin: ffmpeg_plugin_md5_vs_framemd5 on the fast JVT streams", "[fast][third_party][needs-corpus]") {
  const std::string id = GENERATE(as<std::string>{}, "jvt_BA1_Sony_D", "jvt_BA_MW_D", "jvt_CABA1_SVA_B",
                                  "jvt_CVWP1_TOSHIBA_E", "jvt_MR1_BT_A", "jvt_CI1_FT_B", "jvt_NRF_MW_E");
  md5_vs_framemd5(id, GENERATE(1, 4));
}

TEST_CASE("ffmpeg plugin: ffmpeg_plugin_md5_vs_framemd5 on the slow JVT streams", "[slow][third_party][needs-corpus]") {
  const std::string id = GENERATE(as<std::string>{}, "jvt_CABA3_TOSHIBA_E", "jvt_CAMA1_Sony_C", "jvt_CAPM3_Sony_D");
  md5_vs_framemd5(id, GENERATE(1, 4));
}
