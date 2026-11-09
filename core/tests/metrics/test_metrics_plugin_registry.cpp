// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300 step 3: ovt::metrics::PluginRegistry with the example plugin (acceptance test 3 at the registry level)
// and its rejection cases (VQ-PLUG-0001, duplicate ids, both entry points), static registration and the
// descriptor JSON.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metrics/host/plugin_registry.h"
#include "ovt/abi/metric_v1.h"
#include "ovt/cxx/log.hpp"
#include "ovt_metrics_generated.h"

namespace {

using ovt::metrics::MetricPlugin;
using ovt::metrics::PluginRegistry;
using ovt::metrics::PluginRejection;

#if defined(__APPLE__)
const std::string kExt = ".dylib";
#else
const std::string kExt = ".so";
#endif

std::string plugin_dir(const char* sub) {
  return std::string(OVT_EXAMPLE_METRIC_ROOT) + "/" + sub;
}

// A registry that searches only `metric_path` (no platform defaults, no $OVT_PLUGIN_PATH).
PluginRegistry::Options only(std::string metric_path) {
  PluginRegistry::Options o;
  o.ovt_metric_path = std::move(metric_path);
  o.ovt_plugin_path = std::string();
  o.defaults = std::vector<std::string>{};
  return o;
}

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

bool rejected(const PluginRegistry& reg, const std::string& path_part, std::string_view code) {
  const auto rejections = reg.rejections();
  return std::any_of(rejections.begin(), rejections.end(), [&](const PluginRejection& r) {
    return r.path.find(path_part) != std::string::npos && r.code == code;
  });
}

}  // namespace

TEST_CASE("metric plugin file names follow libovt_metric_<id>", "[metrics][plugin][fast]") {
  using ovt::metrics::metric_plugin_id_from_file_name;
  CHECK(metric_plugin_id_from_file_name("libovt_metric_example" + kExt) == "example");
  CHECK(metric_plugin_id_from_file_name("libovt_metric_ssim_x264" + kExt) == "ssim_x264");
  CHECK_FALSE(metric_plugin_id_from_file_name("libovt_plugin_mock" + kExt).has_value());
  CHECK_FALSE(metric_plugin_id_from_file_name("libovt_metric_" + kExt).has_value());
  CHECK_FALSE(metric_plugin_id_from_file_name("libovt_metric_Example" + kExt).has_value());
  CHECK_FALSE(metric_plugin_id_from_file_name("libovt_metric_example.txt").has_value());
}

TEST_CASE("the registry loads libovt_metric_example and lists id example", "[metrics][plugin][fast]") {
  // Acceptance test 3 (the C API form is in test_metrics_c_api.cpp).
  LogCapture log;
  PluginRegistry reg(only(plugin_dir("example")));
  const std::vector<const MetricPlugin*> plugins = reg.plugins();
  REQUIRE(plugins.size() == 1);
  const MetricPlugin* p = plugins.front();
  CHECK(p->id == "example");
  CHECK(p->path.find("libovt_metric_example" + kExt) != std::string::npos);
  CHECK(p->handle != nullptr);
  REQUIRE(p->api != nullptr);
  CHECK(p->api->abi_version == OVT_METRIC_ABI_VERSION);
  REQUIRE(p->descriptor() != nullptr);
  CHECK(p->descriptor()->kind() == ovt::fb::MetricKind::FullReference);
  CHECK(p->descriptor()->domain() == ovt::fb::WorkingDomain::CodeValue);
  CHECK(reg.find("example") == p);
  CHECK(reg.find("nosuch") == nullptr);
  CHECK(reg.rejections().empty());
  CHECK(log.contains("metric example 1.0.0"));

  SECTION("the descriptor JSON is an array with the schema's fields in order") {
    const auto doc = nlohmann::json::parse(reg.descriptors_json());
    REQUIRE(doc.is_array());
    REQUIRE(doc.size() == 1);
    const auto& d = doc.at(0);
    CHECK(d.at("id") == "example");
    CHECK(d.at("kind") == "FullReference");
    CHECK(d.at("domain") == "CodeValue");
    CHECK(d.at("outputs") == nlohmann::json::array({"score"}));
    CHECK(d.at("license") == "LGPL-2.1-or-later");
    CHECK(d.at("temporal_unit_frames") == 1);
    CHECK(d.at("higher_is_better") == true);
    // Field order of ovt_metrics.fbs: id first, version last.
    const std::string text = reg.descriptors_json();
    CHECK(text.rfind("[{\"id\":\"example\"", 0) == 0);
    CHECK(text.find("\"version\":\"1.0.0\"}]") == text.size() - std::string("\"version\":\"1.0.0\"}]").size());
  }

  SECTION("the plugin's entry points work through the table") {
    ovt_Metric* m = nullptr;
    REQUIRE(p->api->create("{}", &m) == OVT_OK);
    REQUIRE(m != nullptr);
    CHECK(p->api->create("{\"unknown\":1}", &m) == OVT_E_INVALID_ARG);  // rule 6
    ovt_MetricPlanes planes{};
    planes.struct_size = sizeof(planes);
    ovt_Buffer out{};
    CHECK(p->api->score(m, &planes, &planes, ovt_FrameId{5, 5}, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    const auto* score = flatbuffers::GetRoot<ovt::fb::FrameScore>(out.data);
    REQUIRE(score->values() != nullptr);
    CHECK(score->values()->Get(0) == 5.0);
    CHECK(p->api->finish(m, &out) == OVT_OK);
    CHECK(out.data == nullptr);
    p->api->destroy(m);
  }
}

TEST_CASE("the registry rejects broken plugins with VQ-PLUG-0001 and keeps going", "[metrics][plugin][fast]") {
  LogCapture log;
  const std::string path = plugin_dir("bad_abi") + ":" + plugin_dir("bad_id") + ":" + plugin_dir("bad_size") + ":" +
                           plugin_dir("bad_null") + ":" + plugin_dir("bad_desc") + ":" + plugin_dir("bad_both") + ":" +
                           plugin_dir("bad_utf8") + ":" + plugin_dir("example");
  PluginRegistry reg(only(path));
  const auto plugins = reg.plugins();
  REQUIRE(plugins.size() == 1);
  CHECK(plugins.front()->id == "example");
  CHECK(rejected(reg, "libovt_metric_abi2", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_wrongid", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_small", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_nullscore", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_baddesc", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_both", "VQ-PLUG-0001"));
  CHECK(rejected(reg, "libovt_metric_badutf8", "VQ-PLUG-0001"));
  CHECK(reg.rejections().size() == 7);
  // One bad plugin must not take the listing down with it (nlohmann::json::dump() throws on invalid UTF-8).
  const std::string listed = reg.descriptors_json();
  CHECK(listed.find("\"example\"") != std::string::npos);
  CHECK(listed.find("badutf8") == std::string::npos);
  CHECK(log.contains("VQ-PLUG-0001|metric plugin"));
  CHECK(log.contains("abi_version 2, host expects 1"));
  CHECK(log.contains("differs from the file name id"));
  CHECK(log.contains("exports both"));
  CHECK(log.contains("not valid UTF-8"));
}

TEST_CASE("the first directory holding an id wins", "[metrics][plugin][fast]") {
  PluginRegistry reg(only(plugin_dir("example_dup") + ":" + plugin_dir("example")));
  const auto plugins = reg.plugins();
  REQUIRE(plugins.size() == 1);
  CHECK(plugins.front()->path.find("/example_dup/") != std::string::npos);
  REQUIRE(reg.rejections().size() == 1);
  CHECK(reg.rejections().front().code.empty());  // a duplicate is not VQ-PLUG-0001
  CHECK(reg.rejections().front().path.find("/example/") != std::string::npos);
  // rescan() keeps what is loaded and does not load the duplicate either.
  CHECK(reg.rescan() == OVT_OK);
  CHECK(reg.plugins().size() == 1);
}

TEST_CASE("search path order: added, $OVT_METRIC_PATH, $OVT_PLUGIN_PATH, defaults", "[metrics][plugin][fast]") {
  PluginRegistry::Options o;
  o.ovt_metric_path = std::string("/m1:/m2");
  o.ovt_plugin_path = std::string("/p1:");  // trailing ':' appends the defaults
  o.defaults = std::vector<std::string>{"/d1"};
  PluginRegistry reg(o);
  REQUIRE(reg.add_search_path("/a1") == OVT_OK);
  CHECK(reg.add_search_path("") == OVT_E_INVALID_ARG);
  CHECK(reg.search_paths() == std::vector<std::string>{"/a1", "/m1", "/m2", "/p1", "/d1"});
  PluginRegistry::Options replaced = o;
  replaced.ovt_plugin_path = std::string("/p1");  // no trailing ':' replaces the defaults
  CHECK(PluginRegistry(replaced).search_paths() == std::vector<std::string>{"/m1", "/m2", "/p1"});
  // An empty, unreadable or missing directory just yields nothing.
  PluginRegistry empty(only("/nonexistent/ovt/metrics"));
  CHECK(empty.plugins().empty());
  CHECK(empty.rejections().empty());
}

namespace {

const std::vector<uint8_t>& builtin_descriptor_bytes() {
  static const std::vector<uint8_t> bytes = [] {
    flatbuffers::FlatBufferBuilder fbb;
    const auto id = fbb.CreateString("builtin_test");
    ovt::fb::MetricDescriptorBuilder d(fbb);
    d.add_id(id);
    d.add_kind(ovt::fb::MetricKind::NoReference);
    d.add_domain(ovt::fb::WorkingDomain::PU21);
    fbb.Finish(d.Finish());
    return std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }();
  return bytes;
}

ovt_Buffer builtin_descriptor() noexcept {
  try {
    const auto& b = builtin_descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}
ovt_status builtin_create(const char* /*options*/, ovt_Metric** out) noexcept {
  *out = nullptr;
  return OVT_E_UNSUPPORTED;
}
void builtin_destroy(ovt_Metric* /*metric*/) noexcept {}
ovt_status builtin_score(ovt_Metric* /*metric*/, const ovt_MetricPlanes* /*reference*/,
                         const ovt_MetricPlanes* /*distorted*/, ovt_FrameId /*frame*/, ovt_Buffer* /*out*/) noexcept {
  return OVT_E_UNSUPPORTED;
}
ovt_status builtin_finish(ovt_Metric* /*metric*/, ovt_Buffer* /*out*/) noexcept {
  return OVT_E_UNSUPPORTED;
}
const ovt_MetricApi kBuiltin = {sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, builtin_descriptor, builtin_create,
                                builtin_destroy,       builtin_score,          builtin_finish};

}  // namespace

TEST_CASE("static registration of in-tree metrics", "[metrics][plugin][fast]") {
  PluginRegistry reg(only(plugin_dir("example")));
  REQUIRE(reg.register_builtin(&kBuiltin, "(built-in)") == OVT_OK);
  CHECK(reg.register_builtin(&kBuiltin, "(built-in)") == OVT_E_PLUGIN);  // duplicate id
  CHECK(reg.register_builtin(nullptr, "(built-in)") == OVT_E_INVALID_ARG);
  ovt_MetricApi bad = kBuiltin;
  bad.abi_version = 7;
  CHECK(reg.register_builtin(&bad, "(built-in bad)") == OVT_E_INVALID_ARG);
  const auto plugins = reg.plugins();  // scans the example plugin too; sorted by id
  REQUIRE(plugins.size() == 2);
  CHECK(plugins[0]->id == "builtin_test");
  CHECK(plugins[0]->path.empty());
  CHECK(plugins[0]->origin == "(built-in)");
  CHECK(plugins[0]->handle == nullptr);
  CHECK(plugins[1]->id == "example");
  const auto doc = nlohmann::json::parse(reg.descriptors_json());
  REQUIRE(doc.size() == 2);
  CHECK(doc.at(0).at("id") == "builtin_test");
  CHECK(doc.at(0).at("kind") == "NoReference");
  CHECK(doc.at(0).at("domain") == "PU21");
  CHECK(doc.at(0).at("display_name").get<std::string>().empty());
  CHECK(doc.at(0).at("outputs") == nlohmann::json::array());
}

TEST_CASE("validate_metric_api checks the table before the descriptor", "[metrics][plugin][fast]") {
  using ovt::metrics::validate_metric_api;
  std::string error;
  CHECK_FALSE(validate_metric_api(nullptr, nullptr, error).has_value());
  ovt_MetricApi api = kBuiltin;
  api.struct_size = 4;  // too small to hold abi_version
  CHECK_FALSE(validate_metric_api(&api, nullptr, error).has_value());
  CHECK(error.find("struct_size") != std::string::npos);
  api = kBuiltin;
  api.finish = nullptr;
  CHECK_FALSE(validate_metric_api(&api, nullptr, error).has_value());
  CHECK(error.find("NULL") != std::string::npos);
  api = kBuiltin;
  const std::string other = "other";
  CHECK_FALSE(validate_metric_api(&api, &other, error).has_value());
  CHECK(error.find("differs from the file name id") != std::string::npos);
  const std::string same = "builtin_test";
  const auto ok = validate_metric_api(&api, &same, error);
  REQUIRE(ok.has_value());
  CHECK_FALSE(ok->empty());
}
