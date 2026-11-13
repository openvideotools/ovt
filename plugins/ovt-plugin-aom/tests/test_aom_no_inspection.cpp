// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// WI-2.155 acceptance test 2 (chapter 07 §7.6.1): with a libaom built without inspection (the stub of
// aom_no_inspection_stub.c), create() fails with OVT_E_PLUGIN and "libaom built without CONFIG_INSPECTION".

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "decoder.hpp"

TEST_CASE("aom plugin AT2: libaom without CONFIG_INSPECTION", "[fast][third_party]") {
  std::unique_ptr<ovt::aom::Decoder> dec;
  std::string error;
  CHECK(ovt::aom::Decoder::create(1, &dec, &error) == OVT_E_PLUGIN);
  CHECK(error == "libaom built without CONFIG_INSPECTION");
  CHECK(dec == nullptr);
}
