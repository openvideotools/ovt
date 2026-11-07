// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.151 acceptance tests: the GOP-local analysis Session (chapter 7 §7.5.3) driven through the mock plugin
// with synthetic StreamIndex buffers ("scripted decode"). A spy function table forwards to the real mock plugin and
// records create/push calls, so the tests see exactly what the host feeds the decoder.

#include <flatbuffers/flatbuffers.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_model_generated.h"
#include "session.hpp"

using ovt::analysis::LoadedPlugin;
using ovt::analysis::PluginRegistry;
using ovt::analysis::Session;

namespace {

// ---- spy over the mock plugin --------------------------------------------------------------------------------

struct Push {
  int32_t hint;
  std::vector<uint8_t> bytes;
};

struct Spy {
  const ovt_AnalysisDecoderApi* real = nullptr;
  int creates = 0;
  int destroys = 0;
  int flushes = 0;
  std::vector<Push> pushes;
  ovt::jobs::Cancel* cancel_on = nullptr;  // requested when the first unit of `cancel_at` is pushed
  int cancel_at = -1;
  void clear_calls() {
    creates = destroys = flushes = 0;
    pushes.clear();
  }
};
Spy g_spy;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): C function table needs global state

ovt_status spy_create(const uint8_t* cp, size_t len, int threads, ovt_AnalysisDecoder** out) noexcept {
  ++g_spy.creates;
  return g_spy.real->create(cp, len, threads, out);
}
void spy_destroy(ovt_AnalysisDecoder* dec) noexcept {
  ++g_spy.destroys;
  g_spy.real->destroy(dec);
}
ovt_status spy_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t hint, int64_t pts) noexcept {
  try {
    g_spy.pushes.push_back({hint, std::vector<uint8_t>(unit, unit + len)});
  } catch (...) {
    return OVT_E_NOMEM;
  }
  if (g_spy.cancel_on != nullptr && hint == g_spy.cancel_at) {
    g_spy.cancel_on->request();
  }
  return g_spy.real->push(dec, unit, len, hint, pts);
}
ovt_status spy_pull(ovt_AnalysisDecoder* dec, uint32_t mask, ovt_Buffer* out) noexcept {
  return g_spy.real->pull(dec, mask, out);
}
ovt_status spy_plane(ovt_AnalysisDecoder* dec, const char* id, const uint8_t** data, size_t* size) noexcept {
  return g_spy.real->plane(dec, id, data, size);
}
void spy_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  g_spy.real->release(dec, fb);
}
ovt_status spy_flush(ovt_AnalysisDecoder* dec) noexcept {
  ++g_spy.flushes;
  return g_spy.real->flush(dec);
}

// The mock plugin `sub/libovt_plugin_<id>` wrapped in the spy.
LoadedPlugin spy_plugin(const char* sub, const char* id) {
  static std::map<std::string, std::unique_ptr<PluginRegistry>> registries;
  auto& reg = registries[sub];
  if (!reg) {
    PluginRegistry::Options o;
    o.ovt_plugin_path = std::string(OVT_MOCK_PLUGIN_ROOT) + "/" + sub;
    o.defaults = std::vector<std::string>{};
    reg = std::make_unique<PluginRegistry>(std::move(o));
  }
  const LoadedPlugin* real = reg->find(id);
  REQUIRE(real != nullptr);
  static ovt_AnalysisDecoderApi api{};
  api = *real->api;
  api.create = spy_create;
  api.destroy = spy_destroy;
  api.push = spy_push;
  api.pull = spy_pull;
  api.plane = spy_plane;
  api.release = spy_release;
  api.flush = spy_flush;
  g_spy = Spy{};
  g_spy.real = real->api;
  LoadedPlugin p = *real;
  p.api = &api;
  return p;
}

// ---- scripted stream --------------------------------------------------------------------------------------------

struct ScriptFrame {
  bool rap = false;
  uint8_t vcl_type = 1;  // nal_unit_type / obu_type of the frame's unit
  int show_existing_of = -1;
  bool shown = true;
  uint8_t first_byte = 0x01;  // 0xFF makes the mock's push return OVT_E_CORRUPT
};

// A file holding every unit's bytes back to back and the StreamIndex that describes it. Parameter-set units (all
// before frame 0) come first; each frame has one unit {first_byte, decode_index, 0xAB}.
struct Script {
  std::filesystem::path path;
  std::vector<uint8_t> index;
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  uint32_t ps_units = 0;
  ~Script() {
    file.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

std::unique_ptr<Script> make_script(ovt::fb::Codec codec, const std::vector<ScriptFrame>& frames,
                                    const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& param_sets) {
  auto s = std::make_unique<Script>();
  static int counter = 0;
  s->path = std::filesystem::temp_directory_path() /
            ("ovt_session_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".bin");
  std::vector<uint8_t> bytes;
  ovt::fb::StreamIndexT idx;
  idx.codec = codec;
  idx.schema_version = 1;
  idx.source_hash.assign(32, 0x5A);
  auto add_unit = [&](uint8_t type, const std::vector<uint8_t>& payload, int frame) {
    auto u = std::make_unique<ovt::fb::UnitT>();
    u->index = static_cast<uint32_t>(idx.units.size());
    u->file_span = std::make_unique<ovt::fb::ByteSpan>(bytes.size(), payload.size());
    u->type = type;
    u->frame = std::make_unique<ovt::fb::FrameId>(frame, -1);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const uint32_t index = u->index;
    idx.units.push_back(std::move(u));
    return index;
  };
  for (const auto& [type, payload] : param_sets) {
    add_unit(type, payload, -1);
    auto copy = std::make_unique<ovt::fb::UnitT>(*idx.units.back());
    idx.parameter_sets.push_back(std::move(copy));
  }
  s->ps_units = static_cast<uint32_t>(param_sets.size());
  int display = 0;
  for (std::size_t d = 0; d < frames.size(); ++d) {
    const auto& sf = frames[d];
    std::vector<uint8_t> payload{sf.first_byte, static_cast<uint8_t>(d), 0xAB};
    if (codec == ovt::fb::Codec::AV1) {
      // OBU header without obu_has_size_field (Annex B form), then the payload.
      payload = {static_cast<uint8_t>(sf.vcl_type << 3U), sf.first_byte, static_cast<uint8_t>(d), 0xAB};
    }
    const uint32_t u = add_unit(sf.vcl_type, payload, static_cast<int>(d));
    auto f = std::make_unique<ovt::fb::FrameInfoT>();
    f->id = std::make_unique<ovt::fb::FrameId>(static_cast<int>(d), sf.shown ? display++ : -1);
    f->type = sf.rap ? ovt::fb::FrameType::I : ovt::fb::FrameType::P;
    f->is_rap = sf.rap;
    f->shown = sf.shown;
    f->show_existing_of = sf.show_existing_of;
    f->pts = static_cast<int64_t>(d) * 1000;
    f->size_bytes = payload.size();
    f->units = {u};
    f->dpb_after = {0, 0, 0, 0, 0, 0, 0, static_cast<int>(d)};
    idx.frames.push_back(std::move(f));
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &idx));
  s->index.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  {
    std::ofstream out(s->path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(bytes.size()));
  }
  s->file = ovt::io::RandomAccessFile::open(s->path.string());
  return s;
}

// H.264 stream: SPS (7) and PPS (8) up front, RAPs every `gop` frames.
std::unique_ptr<Script> h264_script(int count, int gop) {
  std::vector<ScriptFrame> frames(static_cast<std::size_t>(count));
  for (int d = 0; d < count; d += gop) {
    frames[static_cast<std::size_t>(d)].rap = true;
    frames[static_cast<std::size_t>(d)].vcl_type = 5;
  }
  return make_script(ovt::fb::Codec::H264, frames, {{7, {0x67, 0x42}}, {8, {0x68, 0xCE}}});
}

std::vector<int32_t> hints() {
  std::vector<int32_t> h;
  for (const auto& p : g_spy.pushes) {
    if (std::find(h.begin(), h.end(), p.hint) == h.end()) {
      h.push_back(p.hint);
    }
  }
  return h;
}

const ovt::fb::FrameAnalysis* as_fa(const std::vector<uint8_t>& buf) {
  flatbuffers::Verifier v(buf.data(), buf.size());
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(v));
  return ovt::fb::GetFrameAnalysis(buf.data());
}

bool has_diag(const Session& s, const char* code) {
  return std::any_of(s.diagnostics().begin(), s.diagnostics().end(), [code](const auto& d) { return d.code == code; });
}

}  // namespace

TEST_CASE("AT1: frame T then T+1 reuses the warm decoder", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  auto s = h264_script(90, 30);
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;

  REQUIRE(session.frame(45, 0, nullptr, out) == OVT_OK);
  CHECK(as_fa(out)->frame()->id()->decode_index() == 45);
  CHECK(g_spy.creates == 1);
  // Cold start at RAP 30: the active SPS and PPS, then frames 30..45.
  REQUIRE(g_spy.pushes.size() == 2 + 16);
  CHECK(g_spy.pushes[0].bytes == std::vector<uint8_t>{0x67, 0x42});
  CHECK(g_spy.pushes[1].bytes == std::vector<uint8_t>{0x68, 0xCE});
  CHECK(g_spy.pushes[0].hint == 30);
  CHECK(g_spy.pushes.back().hint == 45);

  g_spy.clear_calls();
  REQUIRE(session.frame(46, 0, nullptr, out) == OVT_OK);
  CHECK(as_fa(out)->frame()->id()->decode_index() == 46);
  CHECK(g_spy.creates == 0);
  REQUIRE(g_spy.pushes.size() == 1);
  CHECK(g_spy.pushes[0].hint == 46);
  CHECK(g_spy.pushes[0].bytes == std::vector<uint8_t>{0x01, 46, 0xAB});
  CHECK(g_spy.flushes == 0);  // never flush when the target appeared
}

TEST_CASE("frame T-1 after T recreates the decoder from the RAP", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  auto s = h264_script(90, 30);
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  REQUIRE(session.frame(46, 0, nullptr, out) == OVT_OK);
  g_spy.clear_calls();
  REQUIRE(session.frame(45, 0, nullptr, out) == OVT_OK);
  CHECK(g_spy.creates == 1);
  CHECK(g_spy.destroys == 1);
  CHECK(hints() == std::vector<int32_t>{30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45});
  CHECK(as_fa(out)->frame()->id()->decode_index() == 45);
  CHECK(as_fa(out)->frame()->pts() == 45000);  // filled from the index (A6)
  CHECK(as_fa(out)->frame()->id()->display_index() == 45);
}

TEST_CASE("AT2: cancellation between frames keeps the decoder for reuse", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  auto s = h264_script(60, 30);
  Session session(*s->file, s->index, plugin, {});
  ovt::jobs::Cancel cancel;
  g_spy.cancel_on = &cancel;
  g_spy.cancel_at = 20;  // the job is cancelled while frame 20 of the 30-frame GOP decodes
  std::vector<uint8_t> out;
  CHECK(session.frame(29, 0, &cancel, out) == OVT_E_CANCELLED);
  CHECK(out.empty());
  CHECK(session.has_decoder());
  CHECK(session.last_decoded() == 20);

  g_spy.clear_calls();
  g_spy.cancel_on = nullptr;
  cancel.reset();
  REQUIRE(session.frame(25, 0, &cancel, out) == OVT_OK);
  CHECK(g_spy.creates == 0);
  CHECK(hints() == std::vector<int32_t>{21, 22, 23, 24, 25});
  CHECK(as_fa(out)->frame()->id()->decode_index() == 25);
}

TEST_CASE("AT3: 32 consecutive OVT_E_CORRUPT pushes abort with a Diagnostic", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  std::vector<ScriptFrame> frames(40);
  frames[0].rap = true;
  for (std::size_t d = 1; d < frames.size(); ++d) {
    frames[d].first_byte = 0xFF;
  }
  auto s = make_script(ovt::fb::Codec::H264, frames, {});
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  CHECK(session.frame(39, 0, nullptr, out) == OVT_E_CORRUPT);
  CHECK(has_diag(session, "ANA-0003"));
  CHECK(has_diag(session, "ANA-0002"));
  CHECK(g_spy.pushes.size() == 1 + 32);  // frame 0, then 32 failures; frame 33 onwards never pushed
  CHECK_FALSE(session.has_decoder());
}

TEST_CASE("fewer than 32 corrupt pushes do not abort the session", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  std::vector<ScriptFrame> frames(40);
  frames[0].rap = true;
  for (std::size_t d = 1; d <= 31; ++d) {
    frames[d].first_byte = 0xFF;
  }
  auto s = make_script(ovt::fb::Codec::H264, frames, {});
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  REQUIRE(session.frame(32, 0, nullptr, out) == OVT_OK);
  CHECK(has_diag(session, "ANA-0002"));
  CHECK_FALSE(has_diag(session, "ANA-0003"));
  // Frame 31 produced nothing: asking for it flushes, finds nothing and destroys the decoder.
  REQUIRE(session.frame(39, 0, nullptr, out) == OVT_OK);
  CHECK(session.frame(31, 0, nullptr, out) == OVT_E_CORRUPT);
  CHECK(has_diag(session, "ANA-0004"));
  CHECK_FALSE(session.has_decoder());
}

TEST_CASE("AT4: an AV1 show_existing_frame target decodes only up to the shown frame", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("av1", "mockav1");
  // 0 KEY, 1 hidden ALTREF, 2 INTER, 3 show_existing of 1, 4 INTER. OBU type 6 = OBU_FRAME.
  std::vector<ScriptFrame> frames(5);
  for (auto& f : frames) {
    f.vcl_type = 6;
  }
  frames[0].rap = true;
  frames[1].shown = false;
  frames[3].show_existing_of = 1;
  frames[3].vcl_type = 3;  // OBU_FRAME_HEADER
  auto s = make_script(ovt::fb::Codec::AV1, frames, {{1, {0x08, 0x00, 0x00}}});
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  REQUIRE(session.frame(3, 0, nullptr, out) == OVT_OK);
  const auto* fa = as_fa(out);
  CHECK(fa->frame()->show_existing_of() == 1);
  CHECK(fa->frame()->id()->decode_index() == 3);
  CHECK(fa->frame()->shown());
  CHECK(hints() == std::vector<int32_t>{0, 1});  // nothing beyond the referenced frame
  // Annex B OBUs reach the plugin with obu_has_size_field = 1 and obu_size inserted (A4).
  REQUIRE(g_spy.pushes.size() == 3);  // sequence header, frame 0, frame 1
  CHECK(g_spy.pushes[0].bytes == std::vector<uint8_t>{0x0A, 0x02, 0x00, 0x00});
  CHECK(g_spy.pushes[2].bytes == std::vector<uint8_t>{0x32, 0x03, 0x01, 0x01, 0xAB});
}

TEST_CASE("av1_obu_with_size_field rewrites Annex B OBUs only", "[analysis][session][fast]") {
  using ovt::analysis::av1_obu_with_size_field;
  const std::vector<uint8_t> with_size{0x32, 0x01, 0x07};
  CHECK(av1_obu_with_size_field(with_size) == with_size);
  // Extension header kept, 200-byte payload gets a two-byte leb128 size.
  std::vector<uint8_t> ext{0x34, 0x28};
  ext.resize(2 + 200, 0x11);
  const auto rewritten = av1_obu_with_size_field(ext);
  REQUIRE(rewritten.size() == 2 + 2 + 200);
  CHECK(rewritten[0] == 0x36);
  CHECK(rewritten[1] == 0x28);
  CHECK(rewritten[2] == (0x80 | (200 & 0x7F)));
  CHECK(rewritten[3] == 200 >> 7);
}

TEST_CASE("H.265 RASL pictures after a CRA start are skipped", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("hevc", "mockhevc");
  // 0 IDR, 1..9 TRAIL, 10 CRA, 11 and 12 RASL_N, 13 TRAIL.
  std::vector<ScriptFrame> frames(14);
  frames[0] = {.rap = true, .vcl_type = 19};
  frames[10] = {.rap = true, .vcl_type = 21};
  frames[11].vcl_type = 8;
  frames[12].vcl_type = 8;
  auto s = make_script(ovt::fb::Codec::H265, frames, {{32, {0x40, 0x01}}, {33, {0x42, 0x01}}, {34, {0x44, 0x01}}});
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  REQUIRE(session.frame(13, 0, nullptr, out) == OVT_OK);
  CHECK(hints() == std::vector<int32_t>{10, 13});

  CHECK(session.frame(12, 0, nullptr, out) == OVT_E_UNSUPPORTED);
  CHECK(has_diag(session, "ANA-0006"));

  g_spy.clear_calls();
  REQUIRE(session.frame(12, 0, nullptr, out, {.from_previous_rap = true}) == OVT_OK);
  CHECK(hints().front() == 0);
  CHECK(hints().back() == 12);
  CHECK(as_fa(out)->frame()->id()->decode_index() == 12);
}

namespace {
struct FakeCache final : ovt::analysis::SessionCache {
  std::map<uint32_t, ovt::analysis::CachedAnalysis> entries;
  std::optional<ovt::analysis::CachedAnalysis> lookup(uint32_t decode_index) noexcept override {
    auto it = entries.find(decode_index);
    return it == entries.end() ? std::nullopt : std::optional(it->second);
  }
  void store(uint32_t decode_index, std::span<const uint8_t> fa, uint32_t stage_mask) noexcept override {
    entries[decode_index] = {{fa.begin(), fa.end()}, stage_mask};
  }
};
}  // namespace

TEST_CASE("cache seam: intermediate frames are stored, a covering hit skips decoding", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  auto s = h264_script(30, 30);
  FakeCache cache;
  ovt::analysis::SessionOptions options;
  options.cache = &cache;
  Session session(*s->file, s->index, plugin, std::move(options));
  std::vector<uint8_t> first;
  REQUIRE(session.frame(5, 1U, nullptr, first) == OVT_OK);  // Reconstruction requested
  CHECK(cache.entries.size() == 6);
  CHECK(cache.entries.at(3).stage_mask == 0);
  CHECK(cache.entries.at(5).stage_mask == 1U);
  // The mock exports no planes: ANA-0001 in the result and in the call's diagnostics.
  const auto* fa = as_fa(first);
  REQUIRE(fa->diagnostics() != nullptr);
  CHECK(fa->diagnostics()->Get(0)->code()->str() == "ANA-0001");
  CHECK(has_diag(session, "ANA-0001"));
  // qp_hist computed by the host (A6): 256 bins summing to the cell count.
  REQUIRE(fa->qp_hist() != nullptr);
  CHECK(fa->qp_hist()->size() == 256);
  uint32_t sum = 0;
  for (const uint32_t b : *fa->qp_hist()) {
    sum += b;
  }
  CHECK(sum == fa->grid()->cells_w() * fa->grid()->cells_h());

  g_spy.clear_calls();
  std::vector<uint8_t> second;
  REQUIRE(session.frame(5, 1U, nullptr, second) == OVT_OK);
  CHECK(g_spy.pushes.empty());
  CHECK(second == first);
}

TEST_CASE("invalid requests", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  std::vector<ScriptFrame> frames(3);  // no RAP at all
  auto s = make_script(ovt::fb::Codec::H264, frames, {});
  Session session(*s->file, s->index, plugin, {});
  std::vector<uint8_t> out;
  CHECK(session.frame(-1, 0, nullptr, out) == OVT_E_INVALID_ARG);
  CHECK(session.frame(3, 0, nullptr, out) == OVT_E_INVALID_ARG);
  CHECK(session.frame(1, 0, nullptr, out) == OVT_E_UNSUPPORTED);
  CHECK(has_diag(session, "ANA-0005"));
  const std::vector<uint8_t> junk{1, 2, 3};
  CHECK_THROWS_AS(Session(*s->file, junk, plugin, {}), std::invalid_argument);
}

TEST_CASE("frames are deterministic across sessions", "[analysis][session][fast]") {
  const auto plugin = spy_plugin("mock", "mock");
  auto s = h264_script(60, 30);
  std::vector<uint8_t> a;
  std::vector<uint8_t> b;
  {
    Session session(*s->file, s->index, plugin, {});
    REQUIRE(session.frame(40, 0, nullptr, a) == OVT_OK);
  }
  {
    Session session(*s->file, s->index, plugin, {});
    REQUIRE(session.frame(40, 0, nullptr, b) == OVT_OK);
  }
  CHECK(a == b);
}
