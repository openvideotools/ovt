// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Smoke and acceptance tests of ovt-plugin-de265 (WI-2.163, chapter 07 §7.6.2). The plugin is loaded the way the
// analysis host loads it, through ovt::analysis::PluginRegistry (WI-2.150), and driven through its ABI only.
//  - AT1 `de265_md5_vs_sei`: every picture of 20 HEVC_v1 conformance streams comes out of pull(), with its
//    decode index, and its Reconstruction planes match the stream's decoded picture hash SEI (H.265 D.3.19:
//    MD5, CRC or checksum over the whole decoded picture).
//  - AT2: an SCC sequence parameter set makes push() return OVT_E_UNSUPPORTED with DEC-H265-0010
//    "use ovt-plugin-hm".

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, int threads = 0, const uint8_t* cp = nullptr, size_t len = 0)
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

// ---- H.265 D.3.19 decoded picture hash -----------------------------------------------------------------------------

// The hash of one plane as decoded_picture_hash carries it: 16 MD5 bytes, a 16-bit CRC or a 32-bit checksum,
// big-endian as in the SEI. Samples of more than 8 bits are two little-endian bytes (D.3.19 pictureData).
std::vector<uint8_t> plane_hash(int hash_type, const uint8_t* data, uint32_t w, uint32_t h, uint32_t sample_bytes) {
  const auto sample = [&](uint32_t x, uint32_t y) -> uint32_t {
    const uint8_t* p = data + ((static_cast<size_t>(y) * w + x) * sample_bytes);
    return sample_bytes == 2 ? uint32_t{p[0]} | (uint32_t{p[1]} << 8) : uint32_t{p[0]};
  };
  if (hash_type == 0) {
    std::array<uint8_t, 16> d{};
    av_md5_sum(d.data(), data, static_cast<size_t>(w) * h * sample_bytes);
    return {d.begin(), d.end()};
  }
  if (hash_type == 1) {
    uint32_t crc = 0xFFFF;
    const auto feed = [&crc](uint32_t byte) {
      for (int bit = 0; bit < 8; ++bit) {
        const uint32_t msb = (crc >> 15) & 1U;
        const uint32_t val = (byte >> (7 - bit)) & 1U;
        crc = (((crc << 1) + val) & 0xFFFFU) ^ (msb * 0x1021U);
      }
    };
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t s = sample(x, y);
        feed(s & 0xFFU);
        if (sample_bytes == 2) {
          feed(s >> 8);
        }
      }
    }
    for (int bit = 0; bit < 16; ++bit) {
      const uint32_t msb = (crc >> 15) & 1U;
      crc = ((crc << 1) & 0xFFFFU) ^ (msb * 0x1021U);
    }
    return {static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
  }
  uint32_t sum = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t mask = (x & 0xFFU) ^ (y & 0xFFU) ^ (x >> 8) ^ (y >> 8);
      const uint32_t s = sample(x, y);
      sum += (s & 0xFFU) ^ mask;
      if (sample_bytes == 2) {
        sum += (s >> 8) ^ mask;
      }
    }
  }
  return {static_cast<uint8_t>(sum >> 24), static_cast<uint8_t>(sum >> 16), static_cast<uint8_t>(sum >> 8),
          static_cast<uint8_t>(sum)};
}

struct PictureHash {
  int hash_type = -1;
  std::vector<std::vector<uint8_t>> planes;
};

std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && p[i] == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = p[i] == 0 ? zeros + 1 : 0;
    out.push_back(p[i]);
  }
  return out;
}

// One SEI payloadType or payloadSize value: a run of 0xFF bytes plus a last byte (H.265 7.3.5).
bool read_sei_value(const std::vector<uint8_t>& rbsp, size_t* pos, uint32_t* value) {
  *value = 0;
  while (*pos < rbsp.size() && rbsp[*pos] == 0xFF) {
    *value += 255;
    ++*pos;
  }
  if (*pos >= rbsp.size()) {
    return false;
  }
  *value += rbsp[(*pos)++];
  return true;
}

// decoded_picture_hash (payloadType 132) of an SEI NAL unit, if it carries one. Every payloadType read goes to
// @p types, which names what a stream without a hash carries instead.
bool parse_picture_hash(const uint8_t* nal, size_t len, PictureHash* out, std::set<uint32_t>* types) {
  const std::vector<uint8_t> rbsp = unescape(nal + 2, len - 2);
  size_t pos = 0;
  while (pos + 2 <= rbsp.size() && rbsp[pos] != 0x80) {
    uint32_t type = 0;
    uint32_t size = 0;
    if (!read_sei_value(rbsp, &pos, &type) || !read_sei_value(rbsp, &pos, &size) || pos + size > rbsp.size()) {
      return false;
    }
    types->insert(type);
    if (type == 132 && size >= 1) {
      out->hash_type = rbsp[pos];
      const size_t each = out->hash_type == 0 ? 16 : (out->hash_type == 1 ? 2 : 4);
      out->planes.clear();
      for (size_t off = pos + 1; off + each <= pos + size; off += each) {
        out->planes.emplace_back(rbsp.begin() + static_cast<std::ptrdiff_t>(off),
                                 rbsp.begin() + static_cast<std::ptrdiff_t>(off + each));
      }
      return true;
    }
    pos += size;
  }
  return false;
}

// ---- Annex B input ----------------------------------------------------------------------------------------------

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

const ovt::fb::FrameAnalysis* as_fa(const ovt_Buffer& fb) {
  flatbuffers::Verifier verifier(fb.data, fb.size);
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  return ovt::fb::GetFrameAnalysis(fb.data);
}

struct StreamResult {
  int pictures = 0;                                  // pictures in the stream (first_slice_segment_in_pic_flag = 1)
  std::map<int, PictureHash> expected;               // decode index -> SEI hash
  std::map<int, std::array<PictureHash, 3>> actual;  // decode index -> hashes of the plugin's planes, per hash_type
  std::vector<int> output_order;                     // decode indices as pull() returned them
  std::set<uint32_t> sei_types;                      // every SEI payloadType in the stream
  int push_errors = 0;
  int diagnostics = 0;
};

// Pulls every ready frame; hashes its Reconstruction planes the way the stream's SEI for that picture does.
void drain(Instance& in, StreamResult* r) {
  for (;;) {
    ovt_Buffer fb{};
    REQUIRE(in.api.pull(in.dec, kReconMask, &fb) == OVT_OK);
    if (fb.data == nullptr) {
      return;
    }
    const ovt::fb::FrameAnalysis* fa = as_fa(fb);
    REQUIRE(fa->frame() != nullptr);
    REQUIRE(fa->frame()->id() != nullptr);
    const int idx = fa->frame()->id()->decode_index();
    r->output_order.push_back(idx);
    if (fa->diagnostics() != nullptr) {
      r->diagnostics += static_cast<int>(fa->diagnostics()->size());
    }
    CHECK(fa->plugin_id()->str() == "de265");
    const auto* coded = fa->frame()->coded_size();
    REQUIRE(coded != nullptr);
    // A picture can come out before its suffix SEI was read, so all three hash types are kept.
    std::array<PictureHash, 3> got;
    REQUIRE(fa->planes() != nullptr);
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      CHECK(p->stage() == ovt::fb::PlaneStage::Reconstruction);
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(in.api.plane(in.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      REQUIRE(size == static_cast<size_t>(p->stride_bytes()) * p->height());
      REQUIRE(p->stride_bytes() == p->width() * p->sample_bytes());
      if (p->plane() == 0) {
        CHECK(static_cast<int>(p->width()) == coded->w());
        CHECK(static_cast<int>(p->height()) == coded->h());
      }
      for (int type = 0; type < 3; ++type) {
        got[static_cast<size_t>(type)].planes.push_back(
            plane_hash(type, data, p->width(), p->height(), p->sample_bytes()));
      }
    }
    r->actual[idx] = got;
    in.api.release(in.dec, fb);
  }
}

// Pushes the stream unit by unit. With @p pull_each_unit false nothing is pulled before flush(), so decoding runs
// into the plugin's outstanding-frame limit and must resume from pull().
StreamResult run_stream(const std::vector<uint8_t>& bytes, int threads, bool pull_each_unit = true) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, threads);
  REQUIRE(in.status == OVT_OK);
  StreamResult r;
  int pic = -1;
  std::vector<PictureHash> prefix_hashes;  // prefix SEI hashes wait for their picture's first slice
  for (const Nal& n : split_annexb(bytes)) {
    if (n.size < 3) {
      continue;
    }
    const int type = (n.data[0] >> 1) & 0x3F;
    if (type < 32 && (n.data[2] & 0x80) != 0) {
      ++pic;
      for (PictureHash& h : prefix_hashes) {
        r.expected[pic] = std::move(h);
      }
      prefix_hashes.clear();
    }
    PictureHash h;
    if (type == 40 && pic >= 0 && parse_picture_hash(n.data, n.size, &h, &r.sei_types)) {  // SUFFIX_SEI_NUT
      r.expected[pic] = std::move(h);
    } else if (type == 39 && parse_picture_hash(n.data, n.size, &h, &r.sei_types)) {  // PREFIX_SEI_NUT
      prefix_hashes.push_back(std::move(h));
    }
    const ovt_status st = api.push(in.dec, n.data, n.size, std::max(pic, 0), 1000 + std::max(pic, 0));
    if (st != OVT_OK) {
      ++r.push_errors;
    }
    if (pull_each_unit) {
      drain(in, &r);
    }
  }
  REQUIRE(api.flush(in.dec) == OVT_OK);
  drain(in, &r);
  r.pictures = pic + 1;
  return r;
}

// AT1 checks of one stream: every picture out exactly once by decode index, every hash equal to its SEI.
void check_stream(const StreamResult& r) {
  CHECK(r.push_errors == 0);
  REQUIRE(r.pictures > 0);
  // Every picture came out exactly once, identified by its decode index (not by the output order).
  std::set<int> seen(r.output_order.begin(), r.output_order.end());
  CHECK(seen.size() == r.output_order.size());
  CHECK(static_cast<int>(seen.size()) == r.pictures);
  REQUIRE(!seen.empty());
  CHECK(*seen.begin() == 0);
  CHECK(*seen.rbegin() == r.pictures - 1);
  // Every picture's planes match its decoded picture hash SEI.
  for (const uint32_t t : r.sei_types) {
    UNSCOPED_INFO("SEI payloadType " << t);
  }
  CHECK(static_cast<int>(r.expected.size()) == r.pictures);
  int mismatches = 0;
  for (const auto& [idx, want] : r.expected) {
    const auto it = r.actual.find(idx);
    if (it == r.actual.end() || want.hash_type < 0 || want.hash_type > 2 ||
        it->second[static_cast<size_t>(want.hash_type)].planes != want.planes) {
      ++mismatches;
      UNSCOPED_INFO("hash mismatch at decode index " << idx);
    }
  }
  CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("de265 plugin: caps and ABI table", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  CHECK(api.struct_size == sizeof(ovt_AnalysisDecoderApi));
  CHECK(api.abi_version == OVT_ANALYSIS_ABI_VERSION);
  const ovt_AnalysisCaps* caps = api.caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->struct_size == sizeof(ovt_AnalysisCaps));
  CHECK(caps->codec == 2);  // ovt.fb.Codec.H265
  CHECK(std::string_view(caps->id) == "de265");
  CHECK(std::string_view(caps->license) == "LGPL-3.0-or-later");
  CHECK(caps->plane_stages == kReconMask);
  CHECK(caps->grid_fields == 0U);  // WI-2.164 announces the grid vectors it fills
  CHECK(caps->bit_exact == 1);
  CHECK(caps->supports_threads == 1);
}

TEST_CASE("de265 plugin: SCC, RExt and enhancement-layer units are refused with DEC-H265-0010", "[fast][third_party]") {
  // AT2. SPS NAL header (nal_unit_type 33, layer 0, tid 1), sps_video_parameter_set_id 0, max_sub_layers_minus1 0,
  // temporal_id_nesting 1, then general_profile_space 0, tier 0, general_profile_idc (9 = SCC, 4 = RExt).
  struct Case {
    const char* name;
    std::vector<uint8_t> unit;
  };
  const auto c = GENERATE(Case{"SCC SPS", {0x42, 0x01, 0x01, 0x09, 0x00, 0x00, 0x00, 0x40, 0x90, 0x00}},
                          Case{"RExt SPS", {0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x90, 0x00}},
                          Case{"layer 1 slice", {0x02, 0x09, 0x80, 0x00}});
  CAPTURE(c.name);
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, c.unit.data(), c.unit.size(), 7, 42) == OVT_E_UNSUPPORTED);
  ovt_Buffer fb{};
  REQUIRE(api.pull(in.dec, kReconMask, &fb) == OVT_OK);
  REQUIRE(fb.data != nullptr);
  const ovt::fb::FrameAnalysis* fa = as_fa(fb);
  REQUIRE(fa->frame() != nullptr);
  CHECK(fa->frame()->id()->decode_index() == 7);
  CHECK((fa->planes() == nullptr || fa->planes()->begin() == fa->planes()->end()));
  REQUIRE(fa->diagnostics() != nullptr);
  bool found = false;
  for (const ovt::fb::Diagnostic* d : *fa->diagnostics()) {
    if (d->code()->str() == "DEC-H265-0010") {
      CHECK(d->message()->str() == "use ovt-plugin-hm");
      found = true;
    }
  }
  CHECK(found);
  api.release(in.dec, fb);
  REQUIRE(api.pull(in.dec, kReconMask, &fb) == OVT_OK);
  CHECK(fb.data == nullptr);
}

TEST_CASE("de265 plugin: a Main SPS passes the profile check", "[fast][third_party]") {
  // general_profile_idc 1 with a truncated SPS: libde265 may reject it, but never as unsupported.
  const std::vector<uint8_t> sps = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0x90, 0x00};
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, sps.data(), sps.size(), 0, 0) != OVT_E_UNSUPPORTED);
}

TEST_CASE("de265 plugin: malformed hvcC and arguments are rejected", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const std::array<uint8_t, 4> short_hvcc = {1, 2, 3, 4};
  ovt_AnalysisDecoder* dec = nullptr;
  CHECK(api.create(short_hvcc.data(), short_hvcc.size(), 0, &dec) == OVT_E_CORRUPT);
  CHECK(dec == nullptr);
  // 22 configuration bytes, one array claiming one 0x40-byte NAL unit that is not there.
  std::vector<uint8_t> truncated(22, 0);
  truncated.insert(truncated.end(), {1, 0x21, 0x00, 0x01, 0x00, 0x40, 0x42});
  CHECK(api.create(truncated.data(), truncated.size(), 0, &dec) == OVT_E_CORRUPT);
  Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, nullptr, 0, 0, 0) == OVT_E_INVALID_ARG);
  const uint8_t* data = nullptr;
  size_t size = 0;
  CHECK(api.plane(in.dec, "0/recon/y", &data, &size) == OVT_E_INVALID_ARG);
}

TEST_CASE("de265 plugin: garbage units never crash the plugin", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, 2);
  REQUIRE(in.status == OVT_OK);
  uint32_t state = 0x12345678;  // xorshift32: deterministic garbage
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (int i = 0; i < 400; ++i) {
    std::vector<uint8_t> unit(2 + (next() % 200));
    for (uint8_t& b : unit) {
      b = static_cast<uint8_t>(next());
    }
    unit[0] = static_cast<uint8_t>((((next() % 41) << 1)) & 0x7E);  // forbidden_zero_bit 0, types 0..40, layer 0
    unit[1] = 0x01;
    // A random SPS/VPS can name an unsupported profile: OVT_E_UNSUPPORTED is a clean refusal too.
    const ovt_status st = api.push(in.dec, unit.data(), unit.size(), i, i);
    CHECK((st == OVT_OK || st == OVT_E_CORRUPT || st == OVT_E_UNSUPPORTED));
    ovt_Buffer fb{};
    while (api.pull(in.dec, kReconMask, &fb) == OVT_OK && fb.data != nullptr) {
      (void)as_fa(fb);
      api.release(in.dec, fb);
    }
  }
  const ovt_status st = api.flush(in.dec);
  CHECK((st == OVT_OK || st == OVT_E_CORRUPT));
}

TEST_CASE("de265 plugin: de265_md5_vs_sei on 20 HEVC_v1 streams", "[fast][third_party][needs-corpus]") {
  // AT1. Main and Main 10 streams that carry a decoded picture hash SEI for every picture and output every picture
  // (no pic_output_flag = 0, no skipped RASL or no_output_of_prior_pics), covering PCM, merge, temporal layers, MV
  // clipping, SAO, RPS, long-term refs, HRD, CABAC init types, multiple PPS, scaling lists, dependent slices, WPP
  // and 10-bit. RPS_A, RPS_F, POC_A, AMP_A, AMP_D, CONFWIN_A, MAXBINS_A, STRUCT_B and TILES_B carry no SEI at all,
  // so they cannot serve here (issue #254).
  const std::string id = GENERATE(
      as<std::string>{}, "jctvc_ipcm_A_NEC_3", "jctvc_PMERGE_A_TI_3", "jctvc_MERGE_B_TI_3", "jctvc_VPSID_A_VIDYO_2",
      "jctvc_TSCL_A_VIDYO_5", "jctvc_MVEDGE_A_qualcomm_3", "jctvc_SAO_A_MediaTek_4", "jctvc_RPS_C_ericsson_5",
      "jctvc_RPS_D_ericsson_6", "jctvc_RPS_E_qualcomm_5", "jctvc_MVCLIP_A_qualcomm_3", "jctvc_HRD_A_Fujitsu_3",
      "jctvc_RPS_B_qualcomm_5", "jctvc_CAINIT_A_SHARP_4", "jctvc_LTRPSPS_A_FJ_1", "jctvc_PPS_A_qualcomm_7",
      "jctvc_SLIST_A_Sony_5", "jctvc_DSLICE_A_HHI_5", "jctvc_WPP_A_ericsson_MAIN_2", "jctvc_WPP_A_ericsson_MAIN10_2");
  CAPTURE(id);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  check_stream(run_stream(read_file(path), 2));
}

TEST_CASE("de265 plugin: decoding pauses at the outstanding-frame limit and resumes on pull",
          "[fast][third_party][needs-corpus]") {
  // SAO_A_MediaTek_4 has more pictures than the limit of 16; nothing is pulled until the stream is flushed.
  const std::filesystem::path path = ovt::test::corpus_or_skip("jctvc_SAO_A_MediaTek_4");
  const StreamResult r = run_stream(read_file(path), 0, /*pull_each_unit=*/false);
  REQUIRE(r.pictures > 16);
  check_stream(r);
}
