// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::Result<T> (chapter 7 §7.1.6, WI-1.512).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

#include "ovt/cxx/result.hpp"

TEST_CASE("Result holds a value or a status", "[result][fast]") {
  auto ok = ovt::Result<std::string>::ok("abc");
  CHECK(ok.has_value());
  CHECK(ok.status() == OVT_OK);
  CHECK(ok.message().empty());
  CHECK(ok.value() == "abc");

  const auto err = ovt::Result<int>::error(OVT_E_CORRUPT, "bad");
  CHECK_FALSE(err);
  CHECK(err.status() == OVT_E_CORRUPT);
  CHECK(err.message() == "bad");
}

TEST_CASE("Result moves out move-only values and never reports an error as OK", "[result][fast]") {
  auto r = ovt::Result<std::unique_ptr<int>>::ok(std::make_unique<int>(7));
  const std::unique_ptr<int> p = std::move(r).value();
  CHECK(*p == 7);
  CHECK(ovt::Result<int>::error(OVT_OK, "x").status() == OVT_E_INTERNAL);
  CHECK(ovt::Result<void>::error(OVT_OK, "x").status() == OVT_E_INTERNAL);
}

TEST_CASE("Result of void", "[result][fast]") {
  CHECK(ovt::Result<void>::ok());
  const auto e = ovt::Result<void>::error(OVT_E_IO, "io");
  CHECK(e.status() == OVT_E_IO);
  CHECK(e.message() == "io");
}
