// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Smoke and acceptance tests of ovt-plugin-ffmpeg (WI-2.169, chapter 07 §7.6.3), loaded through the host's
// PluginRegistry (WI-2.150) and driven through its ABI only. Synthetic I_PCM streams have known samples, so planes,
// decode_index and frame threading are checked without a corpus. AT1: the 10 `h264-analysis-smoke` JVT streams
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
#include "test_support/corpus.h"

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

const ovt::analysis::LoadedPlugin& plugin() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_FFMPEG_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* p = registry.find("ffmpeg");
  REQUIRE(p != nullptr);
  return *p;
}

const ovt_AnalysisDecoderApi& plugin_api() {
  return *plugin().api;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, int threads = 1, const uint8_t* cp = nullptr, size_t len = 0)
      : api(a) {
    status = api.create(cp, len, threads, &dec);
  }
  ~Instance() {
    if (dec != nullptr) {
      api.destroy(dec);
    }
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  const ovt_AnalysisDecoderApi& api;
  ovt_AnalysisDecoder* dec = nullptr;
  ovt_status status = OVT_E_INTERNAL;
};

const ovt::fb::FrameAnalysis* as_fa(const ovt_Buffer& fb) {
  flatbuffers::Verifier verifier(fb.data, fb.size);
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  return ovt::fb::GetFrameAnalysis(fb.data);
}

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

class BitWriter {
 public:
  BitWriter& bits(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      bit((value >> i) & 1U);
    }
    return *this;
  }
  BitWriter& ue(uint32_t v) {  // Exp-Golomb, H.264 9.1
    const uint32_t x = v + 1;
    int len = 0;
    while ((x >> len) > 1) {
      ++len;
    }
    return bits(0, len).bits(x, len + 1);
  }
  BitWriter& align_zero() {
    while (n_ != 0) {
      bit(0);
    }
    return *this;
  }
  BitWriter& trailing() { return bits(1, 1).align_zero(); }  // rbsp_trailing_bits
  void byte(uint8_t b) { bits(b, 8); }
  std::vector<uint8_t> take() { return std::move(out_); }

 private:
  void bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | b);
    if (++n_ == 8) {
      out_.push_back(cur_);
      cur_ = 0;
      n_ = 0;
    }
  }
  std::vector<uint8_t> out_;
  uint8_t cur_ = 0;
  int n_ = 0;
};

// Constrained Baseline, level 1.0, 32x16 luma (two macroblocks), POC type 2, no VUI.
std::vector<uint8_t> pcm_sps() {
  BitWriter w;
  w.bits(0x6742C00AU, 32);       // SPS NAL (nal_ref_idc 3), profile_idc 66, constraint_set0/1, level_idc 10
  w.ue(0).ue(0).ue(2).ue(1);     // sps id, log2_max_frame_num_minus4, pic_order_cnt_type 2, max_num_ref_frames
  w.bits(0, 1).ue(1).ue(0);      // no frame_num gaps; 2x1 macroblocks
  w.bits(0b1100, 4).trailing();  // frame_mbs_only, direct_8x8_inference, no cropping, no VUI
  return w.take();
}

std::vector<uint8_t> pcm_pps() {
  BitWriter w;
  w.bits(0x68, 8).ue(0).ue(0);  // PPS NAL, pps id, sps id
  w.bits(0, 2).ue(0);           // CAVLC, no bottom_field_pic_order_in_frame_present, one slice group
  w.ue(0).ue(0).bits(0, 3);     // one reference per list, no weighted prediction
  w.ue(0).ue(0).ue(0);          // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset: se(0)
  w.bits(0b100, 3).trailing();  // deblocking_filter_control_present; no constrained intra, no redundant_pic_cnt
  return w.take();
}

// The sample value of picture @p pic at (x, y) of plane @p c; never 0, so no emulation prevention is needed.
uint8_t pcm_sample(int pic, int c, int x, int y) {
  return static_cast<uint8_t>(16 + ((pic * 37 + c * 61 + x * 5 + y * 11) % 224));
}

// An IDR picture of two I_PCM macroblocks whose samples are pcm_sample(pic, ...).
std::vector<uint8_t> pcm_idr(int pic) {
  BitWriter w;
  w.bits(0x65, 8).ue(0).ue(7).ue(0);  // IDR NAL (nal_ref_idc 3), first_mb_in_slice 0, slice_type I (all), pps id
  w.bits(0, 4).ue(static_cast<uint32_t>(pic % 2));  // frame_num; idr_pic_id differs between consecutive IDRs
  w.bits(0, 2).ue(0).ue(1);  // no_output_of_prior_pics, not long-term, slice_qp_delta se(0), deblocking off
  for (int mb = 0; mb < 2; ++mb) {
    w.ue(25).align_zero();  // mb_type I_PCM, pcm_alignment_zero_bit
    for (int c = 0; c < 3; ++c) {
      const int n = c == 0 ? 16 : 8;
      for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
          w.byte(pcm_sample(pic, c, (mb * n) + x, y));
        }
      }
    }
  }
  w.trailing();
  return w.take();
}

std::vector<uint8_t> pcm_plane(int pic, int c) {
  const int w = c == 0 ? 32 : 16;
  const int h = c == 0 ? 16 : 8;
  std::vector<uint8_t> out;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      out.push_back(pcm_sample(pic, c, x, y));
    }
  }
  return out;
}

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

struct Nal {
  const uint8_t* data;
  size_t size;
};

std::vector<Nal> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<Nal> nals;
  size_t i = 0;
  size_t start = SIZE_MAX;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) {
          --end;  // trailing_zero_8bits and the leading zero of a 4-byte start code
        }
        nals.push_back({s.data() + start, end - start});
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < s.size()) {
    nals.push_back({s.data() + start, s.size() - start});
  }
  return nals;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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
  CHECK(caps->grid_fields == 0U);  // WI-2.170 announces the grid vectors it fills
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
  const int threads = GENERATE(1, 4);  // 4: frame threading must keep frame->opaque (§7.6.3 pitfall)
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
    CHECK(f.diagnostics == 0);
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
  Instance in(api);
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
  Instance in(api, 2);
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

TEST_CASE("ffmpeg plugin: ffmpeg_plugin_md5_vs_framemd5 on the h264-analysis-smoke JVT streams",
          "[fast][third_party][needs-corpus]") {
  // AT1. The 10 streams of the h264-analysis-smoke tag (chapter 14 §14.2): CAVLC and CABAC, MBAFF, weighted
  // prediction, multiple and long-term references, 8x8 transforms, interlace and non-reference frames.
  const std::string id = GENERATE(as<std::string>{}, "jvt_BA1_Sony_D", "jvt_BA_MW_D", "jvt_CABA1_SVA_B",
                                  "jvt_CABA3_TOSHIBA_E", "jvt_CAMA1_Sony_C", "jvt_CVWP1_TOSHIBA_E", "jvt_MR1_BT_A",
                                  "jvt_CAPM3_Sony_D", "jvt_CI1_FT_B", "jvt_NRF_MW_E");
  const int threads = GENERATE(1, 4);
  CAPTURE(id, threads);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::vector<std::pair<int, std::string>> want = framemd5_oracle(path);
  REQUIRE(!want.empty());
  const StreamResult r = run_stream(read_file(path), threads);
  CHECK(r.push_errors == 0);
  // One FrameAnalysis per output frame, each access unit at most once, identified by its decode index.
  REQUIRE(r.frames.size() == want.size());
  std::set<int> seen;
  int mismatches = 0;
  for (size_t i = 0; i < want.size(); ++i) {
    const OutFrame& f = r.frames[i];
    CHECK(seen.insert(f.decode_index).second);
    CHECK(f.decode_index >= 0);
    CHECK(f.decode_index < r.access_units);
    CHECK(f.pts == 1000 + f.decode_index);
    if (f.decode_index != want[i].first || f.cropped_md5 != want[i].second) {
      ++mismatches;
      UNSCOPED_INFO("output frame " << i << ": decode index " << f.decode_index << " md5 " << f.cropped_md5
                                    << ", framemd5 access unit " << want[i].first << " md5 " << want[i].second);
    }
  }
  CHECK(mismatches == 0);
}
