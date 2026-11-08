// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/cache.h (chapter 12 §12.3.14, WI-0.091): acceptance test 4 (the cache-root rule through
// ovt_cache_open(NULL, NULL, &c)), the C round trip of index, frames, planes and reports, caller-owned
// buffers released with ovt_buffer_free, and the status codes of §12.3.14.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ovt/cache.h"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/decode.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"
#include "test_helpers.hpp"

namespace {

namespace fs = std::filesystem;
using ovt::cache::test::FrameSpec;
using ovt::cache::test::hash_of;
using ovt::cache::test::make_frame;
using ovt::cache::test::make_index;
using ovt::cache::test::TempRoot;

/// Sets or clears an environment variable for the test and restores the previous value on destruction.
class EnvGuard {
 public:
  EnvGuard(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name); old != nullptr) {  // NOLINT(concurrency-mt-unsafe): one thread
      old_ = old;
    }
    set(value);
  }
  ~EnvGuard() { set(old_ ? old_->c_str() : nullptr); }
  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;
  EnvGuard(EnvGuard&&) = delete;
  EnvGuard& operator=(EnvGuard&&) = delete;

 private:
  void set(const char* value) const {
    if (value == nullptr) {
      ::unsetenv(name_.c_str());  // NOLINT(concurrency-mt-unsafe)
    } else {
      ::setenv(name_.c_str(), value, 1);  // NOLINT(concurrency-mt-unsafe)
    }
  }
  std::string name_;
  std::optional<std::string> old_;
};

std::vector<std::uint8_t> to_vector(const ovt_Buffer& b) {
  return {b.data, b.data + b.size};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): C span
}

}  // namespace

TEST_CASE("test_cache_c_api: AT4 ovt_cache_open(NULL, NULL) uses ~/.cache/ovt when nothing else is set",
          "[cache][fast]") {
  const TempRoot tmp("c_api_home");
  fs::create_directories(tmp.path());
  const EnvGuard home("HOME", tmp.path().c_str());
  const EnvGuard cache_dir("OVT_CACHE_DIR", nullptr);
  const EnvGuard xdg("XDG_CACHE_HOME", nullptr);
  REQUIRE(ovt_cache_set_dir(nullptr) == OVT_OK);
#if defined(__APPLE__)
  const fs::path expected = tmp.path() / "Library" / "Caches" / "org.openvideotools.ovt";
#else
  const fs::path expected = tmp.path() / ".cache" / "ovt";
#endif
  CHECK(std::string(ovt_cache_dir()) == expected.string());

  ovt_Cache* cache = nullptr;
  REQUIRE(ovt_cache_open(nullptr, nullptr, &cache) == OVT_OK);
  REQUIRE(cache != nullptr);
  CHECK(fs::is_directory(expected));
  ovt_cache_close(cache);
  CHECK(fs::is_regular_file(expected / "cache.json"));

  // OVT_CACHE_DIR wins over the platform rule; ovt_cache_set_dir wins over both.
  const fs::path env_root = tmp.path() / "env-root";
  const EnvGuard env("OVT_CACHE_DIR", env_root.c_str());
  CHECK(std::string(ovt_cache_dir()) == env_root.string());
  const fs::path set_root = tmp.path() / "set-root";
  REQUIRE(ovt_cache_set_dir(set_root.c_str()) == OVT_OK);
  CHECK(std::string(ovt_cache_dir()) == set_root.string());
  REQUIRE(ovt_cache_set_dir(nullptr) == OVT_OK);
}

TEST_CASE("test_cache_c_api: index, frames, planes and reports round-trip through the C API", "[cache][fast]") {
  const TempRoot tmp("c_api_roundtrip");
  const ovt::cache::SourceHash h = hash_of(0x42);

  ovt_Cache* cache = nullptr;
  REQUIRE(ovt_cache_open(tmp.path().c_str(), "{\"max_bytes\": 1048576}", &cache) == OVT_OK);
  ovt_CacheEntry* entry = nullptr;
  REQUIRE(ovt_cache_entry(cache, h.data(), &entry) == OVT_OK);
  REQUIRE(entry != nullptr);

  const std::vector<std::uint8_t> index = make_index(h, 2, ovt::fb::Codec::H265);
  REQUIRE(ovt_cache_put_index(entry, index.data(), index.size()) == OVT_OK);
  FrameSpec spec;
  spec.decode_index = 0;
  spec.with_recon_y_ref = true;
  const std::vector<std::uint8_t> frame0 = make_frame(spec);
  spec.decode_index = 1;
  spec.with_recon_y_ref = false;
  const std::vector<std::uint8_t> frame1 = make_frame(spec);
  REQUIRE(ovt_cache_put_frame(entry, 0, frame0.data(), frame0.size()) == OVT_OK);
  REQUIRE(ovt_cache_put_frame(entry, 1, frame1.data(), frame1.size()) == OVT_OK);

  // A 64 x 32 8-bit plane with a padded stride of 80.
  std::vector<std::uint8_t> samples(static_cast<std::size_t>(80U * 32U));
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<std::uint8_t>(i * 7U);
  }
  ovt_PlaneView view{};
  view.struct_size = sizeof(ovt_PlaneView);
  view.data = samples.data();
  view.width = 64;
  view.height = 32;
  view.stride_bytes = 80;
  view.bit_depth = 8;
  view.sample_bytes = 1;
  REQUIRE(ovt_cache_put_plane(entry, 0, static_cast<std::uint8_t>(ovt::fb::PlaneStage::Reconstruction), 0, &view) ==
          OVT_OK);
  // A report is an opaque FlatBuffer; any well-formed one does (§6.7.1 reports/<kind>.fb.zst).
  const std::vector<std::uint8_t> report = make_index(hash_of(0x71), 1);
  REQUIRE(ovt_cache_put_report(entry, "metrics", report.data(), report.size()) == OVT_OK);
  ovt_cache_entry_release(entry);
  ovt_cache_close(cache);

  // Reopen read-only and read everything back.
  REQUIRE(ovt_cache_open(tmp.path().c_str(), "{\"read_only\": true}", &cache) == OVT_OK);
  REQUIRE(ovt_cache_entry(cache, h.data(), &entry) == OVT_OK);
  ovt_Buffer buf{};
  REQUIRE(ovt_cache_get_index(entry, &buf) == OVT_OK);
  CHECK(to_vector(buf) == index);
  ovt_buffer_free(&buf);
  CHECK(buf.data == nullptr);
  CHECK(buf.size == 0);
  ovt_buffer_free(&buf);  // a zeroed buffer is a no-op
  REQUIRE(ovt_cache_get_frame(entry, 1, &buf) == OVT_OK);
  CHECK(to_vector(buf) == frame1);
  ovt_buffer_free(&buf);

  ovt_Buffer storage{};
  ovt_PlaneView got{};
  REQUIRE(ovt_cache_get_plane(entry, 0, static_cast<std::uint8_t>(ovt::fb::PlaneStage::Reconstruction), 0, &storage,
                              &got) == OVT_OK);
  CHECK(got.struct_size == sizeof(ovt_PlaneView));
  CHECK(got.width == 64);
  CHECK(got.height == 32);
  CHECK(got.stride_bytes == 64);
  CHECK(got.sample_bytes == 1);
  CHECK(got.data == storage.data);
  REQUIRE(storage.size == static_cast<std::size_t>(64U * 32U));
  bool rows_match = true;
  for (std::uint32_t row = 0; row < 32 && rows_match; ++row) {
    rows_match = std::memcmp(&samples[row * 80U], &storage.data[row * 64U], 64) == 0;  // NOLINT
  }
  CHECK(rows_match);
  ovt_buffer_free(&storage);

  REQUIRE(ovt_cache_get_report(entry, "metrics", &buf) == OVT_OK);
  CHECK(to_vector(buf) == report);
  ovt_buffer_free(&buf);

  // Writes on the read-only cache are refused; absent records are OVT_E_IO.
  CHECK(ovt_cache_put_frame(entry, 1, frame1.data(), frame1.size()) == OVT_E_UNSUPPORTED);
  CHECK(ovt_cache_get_frame(entry, 7, &buf) == OVT_E_IO);
  CHECK(buf.data == nullptr);
  CHECK(ovt_cache_get_report(entry, "hdr", &buf) == OVT_E_IO);
  CHECK(ovt_cache_get_plane(entry, 1, static_cast<std::uint8_t>(ovt::fb::PlaneStage::Reconstruction), 0, &storage,
                            &got) == OVT_E_IO);
  ovt_cache_entry_release(entry);
  ovt_cache_close(cache);
}

TEST_CASE("test_cache_c_api: NULL handles, bad buffers and bad options map to OVT_E_INVALID_ARG", "[cache][fast]") {
  const TempRoot tmp("c_api_errors");
  ovt_Cache* cache = nullptr;
  CHECK(ovt_cache_open(tmp.path().c_str(), "[1, 2]", &cache) == OVT_E_INVALID_ARG);
  CHECK(cache == nullptr);
  CHECK(ovt_cache_open(tmp.path().c_str(), "{\"max_bytes\": -1}", &cache) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_open(tmp.path().c_str(), nullptr, nullptr) == OVT_E_INVALID_ARG);
  REQUIRE(ovt_cache_open(tmp.path().c_str(), nullptr, &cache) == OVT_OK);

  const ovt::cache::SourceHash h = hash_of(1);
  ovt_CacheEntry* entry = nullptr;
  CHECK(ovt_cache_entry(nullptr, h.data(), &entry) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_entry(cache, nullptr, &entry) == OVT_E_INVALID_ARG);
  CHECK(entry == nullptr);
  REQUIRE(ovt_cache_entry(cache, h.data(), &entry) == OVT_OK);

  const std::vector<std::uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  CHECK(ovt_cache_put_index(entry, garbage.data(), garbage.size()) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_put_index(entry, nullptr, 0) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_put_frame(entry, 0, garbage.data(), garbage.size()) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_put_index(nullptr, garbage.data(), garbage.size()) == OVT_E_INVALID_ARG);
  const std::vector<std::uint8_t> other_index = make_index(hash_of(2), 1);
  CHECK(ovt_cache_put_index(entry, other_index.data(), other_index.size()) == OVT_E_INVALID_ARG);
  ovt_Buffer buf{};
  CHECK(ovt_cache_get_index(entry, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_get_index(entry, &buf) == OVT_E_IO);  // no index yet
  CHECK(ovt_cache_put_report(entry, "bad kind!", garbage.data(), garbage.size()) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_get_report(entry, nullptr, &buf) == OVT_E_INVALID_ARG);

  ovt_PlaneView view{};
  view.struct_size = sizeof(ovt_PlaneView);
  view.data = garbage.data();
  view.width = 4;
  view.height = 4;
  view.stride_bytes = 2;  // below width * sample_bytes
  view.sample_bytes = 1;
  view.bit_depth = 8;
  CHECK(ovt_cache_put_plane(entry, 0, 0, 0, &view) == OVT_E_INVALID_ARG);
  view.stride_bytes = 4;
  CHECK(ovt_cache_put_plane(entry, 0, 0, 9, &view) == OVT_E_INVALID_ARG);  // plane outside 0..3
  view.struct_size = 1;
  CHECK(ovt_cache_put_plane(entry, 0, 0, 0, &view) == OVT_E_INVALID_ARG);
  CHECK(ovt_cache_put_plane(entry, 0, 0, 0, nullptr) == OVT_E_INVALID_ARG);

  ovt_cache_entry_release(entry);
  ovt_cache_entry_release(nullptr);
  ovt_cache_close(cache);
  ovt_cache_close(nullptr);
}

TEST_CASE("test_cache_c_api: process-wide enabled flag and size cap", "[cache][fast]") {
  CHECK(ovt_cache_enabled() == 1);
  CHECK(ovt_cache_set_enabled(0) == OVT_OK);
  CHECK(ovt_cache_enabled() == 0);
  CHECK(ovt_cache_set_enabled(1) == OVT_OK);
  CHECK(ovt_cache_max_bytes() == ovt::cache::kDefaultMaxBytes);
  CHECK(ovt_cache_set_max_bytes(123) == OVT_OK);
  CHECK(ovt_cache_max_bytes() == 123);
  CHECK(ovt_cache_set_max_bytes(0) == OVT_OK);
  CHECK(ovt_cache_max_bytes() == ovt::cache::kDefaultMaxBytes);
}
