// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// First Catch2 test (WI-0.002): the version entry points of chapter 12 §12.3.2.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include "ovt/version.h"

TEST_CASE("version entry points", "[version][fast]") {
  SECTION("ovt_version matches ovt_version_string") {
    int major = -1;
    int minor = -1;
    int patch = -1;
    ovt_version(&major, &minor, &patch);
    CHECK(major >= 0);
    CHECK(minor >= 0);
    CHECK(patch >= 0);

    const char* text = ovt_version_string();
    REQUIRE(text != nullptr);
    const std::string expected =
        std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    CHECK(std::string(text).rfind(expected, 0) == 0);  // a suffix such as "-dev" may follow
  }

  SECTION("ovt_version accepts null out-pointers") {
    ovt_version(nullptr, nullptr, nullptr);
    int minor = -1;
    ovt_version(nullptr, &minor, nullptr);
    CHECK(minor >= 0);
  }

  SECTION("ovt_check_version accepts its own header version only") {
    CHECK(ovt_check_version(OVT_API_VERSION) == OVT_OK);
    constexpr uint32_t next_major = static_cast<uint32_t>(OVT_API_VERSION_MAJOR + 1) << 16U;
    CHECK(ovt_check_version(next_major) == OVT_E_UNSUPPORTED);
  }

  SECTION("ovt_build_info_json is a JSON object") {
    const char* json = ovt_build_info_json();
    REQUIRE(json != nullptr);
    const std::size_t n = std::strlen(json);
    REQUIRE(n >= 2);
    CHECK(json[0] == '{');
    CHECK(json[n - 1] == '}');
    CHECK(std::string(json).find("\"platform\"") != std::string::npos);
  }
}
