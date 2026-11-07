// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt::analysis::PluginRegistry and the mock plugin (chapter 7 §7.5.1, WI-2.150).

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt_model_generated.h"

extern "C" int ovt_test_abi_header_c_first(void);
extern "C" int ovt_test_abi_header_c_second(void);

namespace {

using ovt::analysis::LoadedPlugin;
using ovt::analysis::PluginRegistry;
using ovt::analysis::PluginRejection;

#if defined(__APPLE__)
const std::string kExt = ".dylib";
#else
const std::string kExt = ".so";
#endif

std::string plugin_dir(const char* sub) {
  return std::string(OVT_MOCK_PLUGIN_ROOT) + "/" + sub;
}

// A registry that searches only `path` (no platform defaults, environment ignored).
PluginRegistry::Options only(std::string path) {
  PluginRegistry::Options o;
  o.ovt_plugin_path = std::move(path);
  o.defaults = std::vector<std::string>{};
  return o;
}

// Collects "<code>|<message>" log lines for the duration of a test.
struct LogCapture {
  std::mutex mutex;
  std::vector<std::string> lines;
  LogCapture() {
    ovt::log::set_level(ovt::log::Level::Trace);
    ovt::log::set_sink([this](ovt::log::Level, std::string_view, std::string_view code, std::string_view msg) {
      const std::lock_guard lock(mutex);
      lines.push_back(std::string(code) + "|" + std::string(msg));
    });
  }
  ~LogCapture() {
    ovt::log::set_sink({});
    ovt::log::set_level(ovt::log::Level::Warn);
  }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  LogCapture(LogCapture&&) = delete;
  LogCapture& operator=(LogCapture&&) = delete;
  bool contains(std::string_view needle) {
    const std::lock_guard lock(mutex);
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string& l) { return l.find(needle) != std::string::npos; });
  }
};

bool has_rejection(const std::vector<PluginRejection>& r, std::string_view code, std::string_view path_part) {
  return std::any_of(r.begin(), r.end(), [code, path_part](const PluginRejection& x) {
    return x.code == code && x.path.find(path_part) != std::string::npos;
  });
}

}  // namespace

TEST_CASE("plugin file names follow libovt_plugin_<id>", "[analysis][plugins][fast]") {
  using ovt::analysis::plugin_id_from_file_name;
  CHECK(plugin_id_from_file_name("libovt_plugin_de265" + kExt) == "de265");
  CHECK(plugin_id_from_file_name("libovt_plugin_a_b9" + kExt) == "a_b9");
  CHECK(plugin_id_from_file_name("libovt_plugin_" + std::string(32, 'a') + kExt));
  for (const std::string& bad :
       {"ovt_plugin_aom" + kExt, "libovt_plugin_" + kExt, "libovt_plugin_Aom" + kExt, "libovt_plugin_a-b" + kExt,
        std::string("libovt_plugin_aom.txt"), "libovt_plugin_" + std::string(33, 'a') + kExt}) {
    CHECK_FALSE(plugin_id_from_file_name(bad));
  }
}

TEST_CASE("search path: added dirs, OVT_PLUGIN_PATH, then defaults", "[analysis][plugins][fast]") {
  using ovt::analysis::resolve_plugin_search_path;
  using V = std::vector<std::string>;
  const V added{"/added"};
  const V defaults{"/d1", "/d2"};
  CHECK(resolve_plugin_search_path(added, nullptr, defaults) == V{"/added", "/d1", "/d2"});
  CHECK(resolve_plugin_search_path(added, "", defaults) == V{"/added", "/d1", "/d2"});
  CHECK(resolve_plugin_search_path(added, "/e1:/e2", defaults) == V{"/added", "/e1", "/e2"});  // replaces defaults
  CHECK(resolve_plugin_search_path({}, "/e1:", defaults) == V{"/e1", "/d1", "/d2"});  // trailing ':' appends them
  CHECK(resolve_plugin_search_path(added, "::/d2::/added:", defaults) == V{"/added", "/d2", "/d1"});  // dedup
  const auto dirs = ovt::analysis::platform_default_plugin_dirs();
  CHECK(std::all_of(dirs.begin(), dirs.end(), [](const auto& d) { return std::filesystem::path(d).is_absolute(); }));
}

// Acceptance test 1.
TEST_CASE("OVT_PLUGIN_PATH pointing at the mock plugin lists it with its caps", "[analysis][plugins][fast]") {
  const std::string dir = plugin_dir("mock");
  // The registry reads $OVT_PLUGIN_PATH at scan time, like the process singleton.
  REQUIRE(::setenv("OVT_PLUGIN_PATH", dir.c_str(), 1) == 0);  // NOLINT(concurrency-mt-unsafe): single-threaded
  PluginRegistry reg;
  CHECK(reg.search_paths() == std::vector<std::string>{dir});  // no trailing ':', so no platform defaults
  const ovt_status st = reg.rescan();
  ::unsetenv("OVT_PLUGIN_PATH");  // NOLINT(concurrency-mt-unsafe)
  REQUIRE(st == OVT_OK);

  const auto plugins = reg.plugins();
  REQUIRE(plugins.size() == 1);
  const LoadedPlugin* p = plugins[0];
  CHECK((p->id == "mock" && p->path == dir + "/libovt_plugin_mock" + kExt && p->handle != nullptr));
  CHECK(p->api->abi_version == OVT_ANALYSIS_ABI_VERSION);
  CHECK(p->caps->struct_size == sizeof(ovt_AnalysisCaps));
  CHECK(p->caps->codec == static_cast<uint8_t>(ovt::fb::Codec::H264));
  CHECK((std::string(p->caps->version) == "1.0.0" && std::string(p->caps->license) == "LGPL-2.1-or-later"));
  CHECK((p->caps->grid_fields == (OVT_GRID_QP | OVT_GRID_BITS_X8) && p->caps->plane_stages == 0U));
  CHECK((reg.find("mock") == p && reg.find("aom") == nullptr));
  CHECK(reg.rejections().empty());
}

// Acceptance test 2.
TEST_CASE("a plugin exporting abi_version 2 is rejected and logged", "[analysis][plugins][fast]") {
  LogCapture log;
  PluginRegistry reg(only(plugin_dir("bad_abi")));
  CHECK(reg.plugins().empty());
  CHECK(has_rejection(reg.rejections(), "ANA-PLG-0001", "libovt_plugin_abi2"));
  CHECK(log.contains("ANA-PLG-0001|plugin "));
  CHECK(log.contains(" rejected: abi_version 2, host expects 1"));
}

// Acceptance test 3.
TEST_CASE("two directories with the same plugin id: the first wins", "[analysis][plugins][fast]") {
  LogCapture log;
  PluginRegistry first(only(plugin_dir("mock") + ":" + plugin_dir("mock_dup")));
  REQUIRE(first.plugins().size() == 1);
  CHECK(std::string(first.find("mock")->caps->version) == "1.0.0");
  CHECK(has_rejection(first.rejections(), "ANA-PLG-0002", "/mock_dup/"));
  CHECK(log.contains("ANA-PLG-0002|plugin "));

  PluginRegistry second(only(plugin_dir("mock_dup") + ":" + plugin_dir("mock")));
  REQUIRE(second.plugins().size() == 1);
  CHECK(std::string(second.find("mock")->caps->version) == "2.0.0-duplicate");
}

TEST_CASE("tables and caps that fail validation are rejected", "[analysis][plugins][fast]") {
  const std::array<std::pair<const char*, const char*>, 4> cases = {{
      {"bad_id", "differs from the file name id"},
      {"bad_codec", "is not an ovt.fb.Codec"},
      {"bad_size", "ovt_AnalysisDecoderApi.struct_size 16 < "},
      {"bad_null", "function table has a NULL entry"},
  }};
  for (const auto& [dir, reason] : cases) {
    PluginRegistry reg(only(plugin_dir(dir)));
    CHECK(reg.plugins().empty());
    const auto r = reg.rejections();
    REQUIRE(r.size() == 1);
    CHECK(r[0].message.find(reason) != std::string::npos);
  }
}

TEST_CASE("scan is idempotent; rescan picks up new directories", "[analysis][plugins][fast]") {
  PluginRegistry reg(only(plugin_dir("mock")));
  REQUIRE(reg.scan() == OVT_OK);
  REQUIRE(reg.scan() == OVT_OK);
  const LoadedPlugin* first = reg.find("mock");
  REQUIRE(reg.add_search_path(plugin_dir("hevc")) == OVT_OK);
  CHECK(reg.add_search_path("") == OVT_E_INVALID_ARG);
  REQUIRE(reg.scan() == OVT_OK);  // a no-op until rescan()
  CHECK(reg.plugins().size() == 1);
  REQUIRE(reg.rescan() == OVT_OK);
  CHECK(reg.plugins().size() == 2);
  CHECK(reg.find("mock") == first);  // already-loaded plugins stay loaded, same object
  CHECK(reg.find("mockhevc") != nullptr);
  CHECK(reg.rejections().empty());  // the loaded mock is not reloaded as a duplicate of itself
}

TEST_CASE("default_for: override, built-in order, then any plugin of the codec", "[analysis][plugins][fast]") {
  PluginRegistry reg(only(plugin_dir("mock") + ":" + plugin_dir("hevc")));
  const auto h264 = static_cast<uint8_t>(ovt::fb::Codec::H264);
  // No ffmpeg/de265/aom registered: the first plugin of the codec, or none.
  REQUIRE(reg.default_for(h264) != nullptr);
  CHECK(reg.default_for(h264)->id == "mock");
  CHECK(reg.default_for(static_cast<uint8_t>(ovt::fb::Codec::H265))->id == "mockhevc");
  CHECK(reg.default_for(static_cast<uint8_t>(ovt::fb::Codec::AV1)) == nullptr);
  // An override naming a plugin of another codec, or one not loaded, is ignored.
  REQUIRE(reg.set_default(h264, "mockhevc") == OVT_OK);
  CHECK(reg.default_for(h264)->id == "mock");
  REQUIRE(reg.set_default(h264, "notloaded") == OVT_OK);
  CHECK(reg.default_for(h264)->id == "mock");
  CHECK(reg.set_default(h264, "Bad-Id") == OVT_E_INVALID_ARG);
  CHECK(reg.set_default(h264, "") == OVT_OK);
}

TEST_CASE("concurrent scans register each plugin once", "[analysis][plugins][fast]") {
  PluginRegistry reg(only(plugin_dir("mock") + ":" + plugin_dir("hevc")));
  std::atomic<int> found{0};
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&reg, &found] {
      if (reg.find("mock") != nullptr && reg.default_for(static_cast<uint8_t>(ovt::fb::Codec::H265)) != nullptr) {
        found.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  CHECK(found.load() == 4);
  CHECK(reg.plugins().size() == 2);
}

TEST_CASE("the mock plugin turns units into synthetic FrameAnalysis buffers", "[analysis][plugins][fast]") {
  PluginRegistry reg(only(plugin_dir("mock")));
  REQUIRE(reg.find("mock") != nullptr);
  const ovt_AnalysisDecoderApi* api = reg.find("mock")->api;
  ovt_AnalysisDecoder* dec = nullptr;
  const std::array<uint8_t, 4> avcc = {1, 2, 3, 4};
  REQUIRE(api->create(avcc.data(), avcc.size(), 1, &dec) == OVT_OK);
  ovt_Buffer fb{};
  REQUIRE(api->pull(dec, 0, &fb) == OVT_OK);
  CHECK(fb.data == nullptr);  // nothing pushed yet

  const std::vector<uint8_t> a(10, 0x41);
  const std::array<uint8_t, 2> corrupt = {0xFF, 0x00};
  REQUIRE(api->push(dec, a.data(), 10, 45, 4500) == OVT_OK);
  REQUIRE(api->push(dec, a.data(), 7, 45, 4500) == OVT_OK);
  CHECK(api->push(dec, corrupt.data(), corrupt.size(), 45, 4500) == OVT_E_CORRUPT);
  REQUIRE(api->push(dec, a.data(), 3, 46, 4600) == OVT_OK);

  REQUIRE(api->pull(dec, 0, &fb) == OVT_OK);
  REQUIRE(fb.data != nullptr);
  flatbuffers::Verifier v(fb.data, fb.size);
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(v));
  const ovt::fb::FrameAnalysis* fa = ovt::fb::GetFrameAnalysis(fb.data);
  CHECK(fa->frame()->id()->decode_index() == 45);  // filled from decode_index_hint (§0.6.2 rule)
  CHECK(fa->frame()->pts() == 4500);
  CHECK((fa->plugin_id()->str() == "mock" && fa->plugin_version()->str() == "1.0.0"));
  const ovt::fb::CellGrid* g = fa->grid();
  CHECK(g->cells_w() == 2);  // 1 + 17 % 16
  CHECK(g->cells_h() == 2);  // two units
  REQUIRE(g->qp()->size() == 4);
  uint64_t bits = 0;
  for (const uint32_t x : *g->bits_x8()) {
    bits += x;
  }
  CHECK(bits == uint64_t{17} * 8);
  CHECK(g->mv0_x() == nullptr);  // not announced in grid_fields, so empty
  api->release(dec, fb);

  REQUIRE(api->pull(dec, 0, &fb) == OVT_OK);
  REQUIRE(fb.data != nullptr);
  CHECK(ovt::fb::GetFrameAnalysis(fb.data)->frame()->id()->decode_index() == 46);
  REQUIRE(api->pull(dec, 0, &fb) == OVT_OK);  // the frame of 46 is not released: destroy() frees it
  CHECK(fb.data == nullptr);
  CHECK(api->flush(dec) == OVT_OK);
  const uint8_t* data = nullptr;
  size_t size = 1;
  CHECK((api->plane(dec, "none", &data, &size) == OVT_E_INVALID_ARG && size == 0));
  api->destroy(dec);
}

TEST_CASE("the ABI header compiles as C11 in both include orders", "[analysis][plugins][fast]") {
  CHECK(ovt_test_abi_header_c_first() == static_cast<int>(sizeof(ovt_AnalysisCaps)) + 1);
  CHECK(ovt_test_abi_header_c_second() == 2);
  static_assert(offsetof(ovt_AnalysisCaps, struct_size) == 0, "D4: struct_size first");
  static_assert(offsetof(ovt_AnalysisDecoderApi, struct_size) == 0, "D4: struct_size first");
}
