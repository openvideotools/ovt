// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt::test::verify_flatbuffer / fb_normalize (WI-0.550; schema-aware parts in WI-0.556).

#include <catch2/catch_test_macros.hpp>
#include <flatbuffers/flatbuffers.h>

#include <stdexcept>
#include <vector>

#include "test_support/fb_normalize.h"

namespace {

// One table with a single uint32 field, built without a schema.
std::vector<uint8_t> tiny_buffer(const char* identifier) {
  flatbuffers::FlatBufferBuilder builder;
  const auto start = builder.StartTable();
  builder.AddElement<uint32_t>(4, 42U, 0U);
  const flatbuffers::Offset<void> root(builder.EndTable(start));
  builder.Finish(root, identifier);
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

}  // namespace

TEST_CASE("test_support/flatbuffer: verifies structure and identifier", "[test_support][fast]") {
  const auto plain = tiny_buffer(nullptr);
  CHECK_FALSE(ovt::test::verify_flatbuffer(plain).has_value());
  CHECK(ovt::test::fb_normalize(plain) == plain);

  const auto tagged = tiny_buffer("OVTA");
  CHECK_FALSE(ovt::test::verify_flatbuffer(tagged, "OVTA").has_value());
  CHECK(ovt::test::verify_flatbuffer(tagged, "OVTB").has_value());

  std::vector<uint8_t> garbage(16, 0xFF);
  CHECK(ovt::test::verify_flatbuffer(garbage).has_value());
  CHECK_THROWS_AS(ovt::test::fb_normalize(garbage), std::invalid_argument);
  CHECK(ovt::test::verify_flatbuffer(std::span<const uint8_t>()).has_value());
}
