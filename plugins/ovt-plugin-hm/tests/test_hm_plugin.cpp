// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Tests of ovt-plugin-hm (WI-4.105, chapter 07 §7.6.4), loaded through ovt::analysis::PluginRegistry like the host
// does. AT1 `hm_md5_vs_sei`: every picture of every fetchable HEVC_v1 stream matches its decoded picture hash SEI.
// AT3: an RExt 4:4:4 12-bit stream yields 12-bit 4:4:4 FrameAnalysis records. AT2 needs part b and WI-4.106.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "TLibCommon/CommonDef.h"  // HM's typedefs, which its MD5.h needs
#include "libmd5/MD5.h"            // HM's public-domain MD5 (Colin Plumb), compiled into tp::hm_common
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_HM_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("hm");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, const uint8_t* cp = nullptr, size_t len = 0) : api(a) {
    status = api.create(cp, len, 0, &dec);
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
// big-endian as in the SEI. The plane bytes are already in D.3.19 order: samples of more than 8 bits are two
// little-endian bytes.
std::vector<uint8_t> plane_hash(int hash_type, const uint8_t* data, uint32_t w, uint32_t h, uint32_t sample_bytes) {
  std::vector<uint8_t> bytes(data, data + (static_cast<size_t>(w) * h * sample_bytes));
  if (hash_type == 0) {
    MD5 md5;
    md5.update(bytes.data(), static_cast<unsigned>(bytes.size()));
    std::array<uint8_t, 16> d{};
    md5.finalize(d.data());
    return {d.begin(), d.end()};
  }
  uint32_t crc = 0xFFFF;
  uint32_t sum = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t msb = (crc >> 15) & 1U;
      crc = (((crc << 1) + ((bytes[i] >> (7 - bit)) & 1U)) & 0xFFFFU) ^ (msb * 0x1021U);
    }
    const auto x = static_cast<uint32_t>((i / sample_bytes) % w);
    const auto y = static_cast<uint32_t>((i / sample_bytes) / w);
    sum += bytes[i] ^ ((x & 0xFFU) ^ (y & 0xFFU) ^ (x >> 8) ^ (y >> 8));
  }
  if (hash_type == 2) {
    return {static_cast<uint8_t>(sum >> 24), static_cast<uint8_t>(sum >> 16), static_cast<uint8_t>(sum >> 8),
            static_cast<uint8_t>(sum)};
  }
  for (int bit = 0; bit < 16; ++bit) {
    const uint32_t msb = (crc >> 15) & 1U;
    crc = ((crc << 1) & 0xFFFFU) ^ (msb * 0x1021U);
  }
  return {static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
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

// decoded_picture_hash (payloadType 132) of an SEI NAL unit, if it carries one.
bool parse_picture_hash(const uint8_t* nal, size_t len, PictureHash* out) {
  const std::vector<uint8_t> rbsp = unescape(nal + 2, len - 2);
  size_t pos = 0;
  while (pos + 2 <= rbsp.size() && rbsp[pos] != 0x80) {
    uint32_t type = 0;
    uint32_t size = 0;
    if (!read_sei_value(rbsp, &pos, &type) || !read_sei_value(rbsp, &pos, &size) || pos + size > rbsp.size()) {
      return false;
    }
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

// ---- Driving the plugin ----------------------------------------------------------------------------------------

struct PictureOut {
  std::array<PictureHash, 3> hashes;  // hashes of the plugin's planes, per hash_type
  int64_t poc = 0;
  int64_t pts = 0;
  uint8_t chroma_format = 0;
  uint8_t bit_depth_luma = 0;
  uint8_t bit_depth_chroma = 0;
  std::vector<uint8_t> plane_bit_depths;
  std::vector<uint8_t> plane_sample_bytes;
};

struct StreamResult {
  int pictures = 0;                     // pictures in the stream (first_slice_segment_in_pic_flag = 1)
  std::set<int> skipped;                // RASL pictures of an IRAP with NoRaslOutputFlag = 1: never decoded
  std::map<int, PictureHash> expected;  // decode index -> SEI hash
  std::map<int, PictureOut> actual;     // decode index -> what pull() returned
  std::vector<int> pull_order;          // decode indices as pull() returned them
  int push_errors = 0;
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
    CHECK(fa->plugin_id()->str() == "hm");
    if (fa->planes() == nullptr || fa->planes()->empty()) {
      in.api.release(in.dec, fb);  // a diagnostics-only record
      continue;
    }
    const int idx = fa->frame()->id()->decode_index();
    r->pull_order.push_back(idx);
    const auto* coded = fa->frame()->coded_size();
    REQUIRE(coded != nullptr);
    PictureOut out;
    out.poc = fa->frame()->poc();
    out.pts = fa->frame()->pts();
    out.chroma_format = fa->frame()->chroma_format();
    out.bit_depth_luma = fa->frame()->bit_depth_luma();
    out.bit_depth_chroma = fa->frame()->bit_depth_chroma();
    REQUIRE(fa->grid() != nullptr);
    CHECK(static_cast<int>(fa->grid()->cells_w()) == (coded->w() + 3) / 4);
    CHECK(static_cast<int>(fa->grid()->cells_h()) == (coded->h() + 3) / 4);
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      CHECK(p->stage() == ovt::fb::PlaneStage::Reconstruction);
      // V-023: one byte per sample exactly for unsigned 8-bit planes.
      CHECK((p->sample_bytes() == 1) == (p->bit_depth() == 8 && !p->is_signed()));
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(in.api.plane(in.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      REQUIRE(size == static_cast<size_t>(p->stride_bytes()) * p->height());
      REQUIRE(p->stride_bytes() == p->width() * p->sample_bytes());
      if (p->plane() == 0) {
        CHECK(static_cast<int>(p->width()) == coded->w());
        CHECK(static_cast<int>(p->height()) == coded->h());
      }
      out.plane_bit_depths.push_back(p->bit_depth());
      out.plane_sample_bytes.push_back(p->sample_bytes());
      for (int type = 0; type < 3; ++type) {
        out.hashes[static_cast<size_t>(type)].planes.push_back(
            plane_hash(type, data, p->width(), p->height(), p->sample_bytes()));
      }
    }
    CHECK(!r->actual.contains(idx));
    r->actual[idx] = std::move(out);
    in.api.release(in.dec, fb);
  }
}

// Feeds a stream unit by unit (decode index hints counted at first_slice_segment_in_pic_flag) and records each
// picture's SEI hash. RASL pictures of an IRAP with NoRaslOutputFlag = 1 are never decoded (H.265 8.1.3): `skipped`.
class StreamFeeder {
 public:
  StreamFeeder(Instance& in, const std::vector<uint8_t>& bytes) : in_(in), nals_(split_annexb(bytes)) {}

  [[nodiscard]] bool done() const { return next_ >= nals_.size(); }

  // Pushes the next NAL unit and pulls what is ready.
  void step() {
    const Nal& n = nals_[next_++];
    if (n.size < 3) {
      return;
    }
    const int type = (n.data[0] >> 1) & 0x3F;
    const int layer = ((n.data[0] & 0x01) << 5) | (n.data[1] >> 3);
    if (type == 36) {  // EOS_NUT
      after_eos_ = true;
    }
    if (type < 32 && layer == 0 && (n.data[2] & 0x80) != 0) {
      ++pic_;
      if (type >= 16 && type <= 23) {  // IRAP
        skip_rasl_ = type != 21 || pic_ == 0 || after_eos_;
        after_eos_ = false;
      }
      if ((type == 8 || type == 9) && skip_rasl_) {
        r_.skipped.insert(pic_);
      }
      for (PictureHash& h : prefix_hashes_) {
        r_.expected[pic_] = std::move(h);
      }
      prefix_hashes_.clear();
    }
    PictureHash h;
    if (type == 40 && pic_ >= 0 && parse_picture_hash(n.data, n.size, &h)) {  // SUFFIX_SEI_NUT
      r_.expected[pic_] = std::move(h);
    } else if (type == 39 && parse_picture_hash(n.data, n.size, &h)) {  // PREFIX_SEI_NUT
      prefix_hashes_.push_back(std::move(h));
    }
    const int hint = std::max(pic_, 0);
    if (in_.api.push(in_.dec, n.data, n.size, hint, 1000 + hint) != OVT_OK) {
      ++r_.push_errors;
    }
    drain(in_, &r_);
  }

  StreamResult finish() {
    REQUIRE(in_.api.flush(in_.dec) == OVT_OK);
    drain(in_, &r_);
    for (const int s : r_.skipped) {
      r_.expected.erase(s);
    }
    r_.pictures = pic_ + 1;
    return std::move(r_);
  }

 private:
  Instance& in_;
  std::vector<Nal> nals_;
  size_t next_ = 0;
  int pic_ = -1;
  bool skip_rasl_ = false;
  bool after_eos_ = false;
  std::vector<PictureHash> prefix_hashes_;  // prefix SEI hashes wait for their picture's first slice
  StreamResult r_;
};

StreamResult run_stream(const std::vector<uint8_t>& bytes) {
  Instance in(plugin_api());
  REQUIRE(in.status == OVT_OK);
  StreamFeeder feeder(in, bytes);
  while (!feeder.done()) {
    feeder.step();
  }
  return feeder.finish();
}

// AT1 checks of one stream: every decodable picture out exactly once by decode index, in decode order, with the
// host's pts; every hash equal to its SEI.
void check_stream(const StreamResult& r, bool expect_hashes) {
  CHECK(r.push_errors == 0);
  REQUIRE(r.pictures > 0);
  std::set<int> want;
  for (int i = 0; i < r.pictures; ++i) {
    if (!r.skipped.contains(i)) {
      want.insert(i);
    }
  }
  std::set<int> got;
  for (const auto& [idx, out] : r.actual) {
    got.insert(idx);
    CHECK(out.pts == 1000 + idx);
  }
  CHECK(got == want);
  CHECK(std::is_sorted(r.pull_order.begin(), r.pull_order.end()));  // HM's pictures come out in decode order
  if (expect_hashes) {
    CHECK(!r.expected.empty());
  }
  int mismatches = 0;
  for (const auto& [idx, sei] : r.expected) {
    const auto it = r.actual.find(idx);
    if (it == r.actual.end() || sei.hash_type < 0 || sei.hash_type > 2 ||
        it->second.hashes[static_cast<size_t>(sei.hash_type)].planes != sei.planes) {
      ++mismatches;
      UNSCOPED_INFO("hash mismatch at decode index " << idx);
    }
  }
  INFO(r.expected.size() << " of " << want.size() << " decoded pictures carry a decoded picture hash SEI");
  CHECK(mismatches == 0);
}

// AT1: every HEVC_v1 entry of tests/corpora/manifest.json with a fetchable, verified file (source jctvc, codec h265,
// no rext/scc feature). These carry no SEI at all (issue #255), so for them AT1 checks only that every picture comes
// out: RPS_A, RPS_F, POC_A, AMP_A, AMP_D, CONFWIN_A, MAXBINS_A, STRUCT_B, TILES_B.
struct At1Stream {
  std::string_view id;
  bool hashes;
};
constexpr std::array<At1Stream, 40> kAt1Streams = {{
    {"jctvc_AMP_A_Samsung_7", false},
    {"jctvc_AMP_D_Hisilicon_3", false},
    {"jctvc_CAINIT_A_SHARP_4", true},
    {"jctvc_CONFWIN_A_Sony_1", false},
    {"jctvc_DSLICE_A_HHI_5", true},
    {"jctvc_ENTP_A_QUALCOMM_1", true},
    {"jctvc_EXT_A_ericsson_4", true},
    {"jctvc_FILLER_A_Sony_1", true},
    {"jctvc_HRD_A_Fujitsu_3", true},
    {"jctvc_MAXBINS_A_TI_5", false},
    {"jctvc_MERGE_B_TI_3", true},
    {"jctvc_NUT_A_ericsson_5", true},
    {"jctvc_OPFLAG_A_Qualcomm_1", true},
    {"jctvc_PICSIZE_A_Bossen_1", true},
    {"jctvc_PMERGE_A_TI_3", true},
    {"jctvc_POC_A_Bossen_3", false},
    {"jctvc_PPS_A_qualcomm_7", true},
    {"jctvc_RAP_A_docomo_6", true},
    {"jctvc_RPS_A_docomo_5", false},
    {"jctvc_SAO_A_MediaTek_4", true},
    {"jctvc_SDH_A_Orange_4", true},
    {"jctvc_SLIST_A_Sony_5", true},
    {"jctvc_STRUCT_B_Samsung_7", false},
    {"jctvc_TILES_B_Cisco_1", false},
    {"jctvc_TSCL_A_VIDYO_5", true},
    {"jctvc_WP_A_Toshiba_3", true},
    {"jctvc_WPP_A_ericsson_MAIN10_2", true},
    {"jctvc_VPSID_A_VIDYO_2", true},
    {"jctvc_WPP_A_ericsson_MAIN_2", true},
    {"jctvc_RAP_B_Bossen_2", true},
    {"jctvc_TSKIP_A_MS_3", true},
    {"jctvc_MVCLIP_A_qualcomm_3", true},
    {"jctvc_MVEDGE_A_qualcomm_3", true},
    {"jctvc_NoOutPrior_A_Qualcomm_1", true},
    {"jctvc_ipcm_A_NEC_3", true},
    {"jctvc_RPS_B_qualcomm_5", true},
    {"jctvc_RPS_C_ericsson_5", true},
    {"jctvc_RPS_D_ericsson_6", true},
    {"jctvc_RPS_E_qualcomm_5", true},
    {"jctvc_RPS_F_docomo_2", false},
}};

}  // namespace

TEST_CASE("hm plugin: caps and ABI table", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  CHECK(api.struct_size == sizeof(ovt_AnalysisDecoderApi));
  CHECK(api.abi_version == OVT_ANALYSIS_ABI_VERSION);
  const ovt_AnalysisCaps* caps = api.caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->struct_size == sizeof(ovt_AnalysisCaps));
  CHECK(caps->codec == 2);  // ovt.fb.Codec.H265
  CHECK(std::string_view(caps->id) == "hm");
  CHECK(std::string_view(caps->license) == "BSD-3-Clause");
  CHECK(caps->plane_stages == kReconMask);
  CHECK(caps->grid_fields == 0U);  // WI-4.105 part b announces the grid vectors it fills
  CHECK(caps->has_symbol_accounting == 0);
  CHECK(caps->bit_exact == 1);
  CHECK(caps->supports_threads == 0);
}

TEST_CASE("hm plugin: malformed hvcC, units and arguments are rejected", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const std::array<uint8_t, 4> short_hvcc = {1, 2, 3, 4};
  ovt_AnalysisDecoder* dec = nullptr;
  CHECK(api.create(short_hvcc.data(), short_hvcc.size(), 0, &dec) == OVT_E_CORRUPT);
  CHECK(dec == nullptr);
  // 22 configuration bytes, one array claiming one 0x40-byte NAL unit that is not there.
  std::vector<uint8_t> truncated(22, 0);
  truncated.insert(truncated.end(), {1, 0x21, 0x00, 0x01, 0x00, 0x40, 0x42});
  CHECK(api.create(truncated.data(), truncated.size(), 0, &dec) == OVT_E_CORRUPT);
  CHECK(api.create(nullptr, 0, 0, nullptr) == OVT_E_INVALID_ARG);

  Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, nullptr, 0, 0, 0) == OVT_E_INVALID_ARG);
  const std::array<uint8_t, 1> one = {0x40};
  CHECK(api.push(in.dec, one.data(), one.size(), 0, 0) == OVT_E_CORRUPT);
  const std::array<uint8_t, 3> forbidden_bit = {0xC0, 0x01, 0x0C};
  CHECK(api.push(in.dec, forbidden_bit.data(), forbidden_bit.size(), 0, 0) == OVT_E_CORRUPT);
  const uint8_t* data = nullptr;
  size_t size = 0;
  CHECK(api.plane(in.dec, "0/recon/y", &data, &size) == OVT_E_INVALID_ARG);
  // The rejected units' diagnostics come out after the end of the stream, in a diagnostics-only record.
  REQUIRE(api.flush(in.dec) == OVT_OK);
  ovt_Buffer fb{};
  REQUIRE(api.pull(in.dec, kReconMask, &fb) == OVT_OK);
  REQUIRE(fb.data != nullptr);
  const ovt::fb::FrameAnalysis* fa = as_fa(fb);
  REQUIRE(fa->diagnostics() != nullptr);
  CHECK(fa->diagnostics()->size() == 2);
  CHECK(fa->grid() == nullptr);
  api.release(in.dec, fb);
  CHECK(api.push(in.dec, one.data(), one.size(), 0, 0) == OVT_E_INVALID_ARG);  // after flush()
}

TEST_CASE("hm plugin: hm_md5_vs_sei on the HEVC_v1 conformance streams", "[fast][third_party][needs-corpus]") {
  // AT1 is all streams or nothing: without any of them (no corpora fetched) the test skips; once one is present,
  // a missing one fails, so a partial corpus can never report AT1 as passed or skipped.
  std::size_t present = 0;
  for (const At1Stream& s : kAt1Streams) {
    present += ovt::test::corpus_path(s.id).has_value() ? 1U : 0U;
  }
  if (present == 0) {
    (void)ovt::test::corpus_or_skip(kAt1Streams.front().id);  // skips, or fails under OVT_REQUIRE_CORPORA=1
  }
  INFO(present << "/" << kAt1Streams.size() << " AT1 streams present");
  const At1Stream s = GENERATE(from_range(kAt1Streams));
  CAPTURE(s.id);
  const std::optional<std::filesystem::path> path = ovt::test::corpus_path(s.id);
  REQUIRE(path.has_value());
  check_stream(run_stream(read_file(*path)), s.hashes);
}

TEST_CASE("hm plugin: two instances decode interleaved", "[fast][third_party][needs-corpus]") {
  // HM keeps its scan-order and z-scan tables in globals (decoder.cpp): two live instances, fed alternately, must
  // both still match their hashes, and destroying one must leave the other's tables intact.
  const std::filesystem::path a_path = ovt::test::corpus_or_skip("jctvc_WPP_A_ericsson_MAIN10_2");
  const std::filesystem::path b_path = ovt::test::corpus_or_skip("jctvc_SAO_A_MediaTek_4");
  const std::vector<uint8_t> a_bytes = read_file(a_path);
  const std::vector<uint8_t> b_bytes = read_file(b_path);
  const ovt_AnalysisDecoderApi& api = plugin_api();
  StreamResult b_result;
  {
    Instance a(api);
    REQUIRE(a.status == OVT_OK);
    StreamFeeder fa(a, a_bytes);
    {
      Instance b(api);
      REQUIRE(b.status == OVT_OK);
      StreamFeeder fb(b, b_bytes);
      while (!fb.done()) {
        fb.step();
        if (!fa.done()) {
          fa.step();
        }
      }
      b_result = fb.finish();
    }  // b destroyed while a is mid-stream
    while (!fa.done()) {
      fa.step();
    }
    check_stream(fa.finish(), true);
  }
  check_stream(b_result, true);
}

TEST_CASE("hm plugin: RExt 4:4:4 12-bit yields a FrameAnalysis per picture", "[fast][third_party][needs-corpus]") {
  // AT3. The primary file of syn_h265_422_444_rext (chapter 14 §14.2, recipe 11) is the x265 main444-12 stream.
  const std::filesystem::path path = ovt::test::corpus_or_skip("syn_h265_422_444_rext");
  const StreamResult r = run_stream(read_file(path));
  check_stream(r, false);
  REQUIRE(!r.actual.empty());
  for (const auto& [idx, out] : r.actual) {
    CAPTURE(idx);
    CHECK(out.chroma_format == 3);
    CHECK(out.bit_depth_luma == 12);
    CHECK(out.bit_depth_chroma == 12);
    CHECK(out.plane_bit_depths == std::vector<uint8_t>{12, 12, 12});
    CHECK(out.plane_sample_bytes == std::vector<uint8_t>{2, 2, 2});
  }
}
