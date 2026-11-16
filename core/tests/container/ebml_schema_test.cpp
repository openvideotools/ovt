// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the generated EBML/Matroska element table (chapter 08 §8.6.7, WI-4.200).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "container/ebml/ebml_schema.h"

namespace {

namespace ebml = ovt::container::ebml;
using ebml::EbmlType;

}  // namespace

TEST_CASE("ebml schema: lookup(Segment) names the root and allows an unknown size", "[container][ebml][fast]") {
  const ebml::EbmlElementDef* segment = ebml::lookup(0x18538067);
  REQUIRE(segment != nullptr);
  CHECK(std::string_view(segment->name) == "Segment");
  CHECK(segment->unknown_size_allowed);
  CHECK(segment->type == EbmlType::Master);
  CHECK(std::string_view(segment->path) == "\\Segment");
  CHECK(segment->min_occurs == 1);
  CHECK(segment->max_occurs == 1);
}

TEST_CASE("ebml schema: ids are unique, sorted and valid EBML ids", "[container][ebml][fast]") {
  const auto all = ebml::ebml_elements();
  REQUIRE(all.size() > 200);
  CHECK(std::is_sorted(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.id < b.id; }));
  std::set<uint32_t> ids;
  std::set<std::string> paths;
  for (const auto& e : all) {
    INFO(e.name);
    CHECK(ids.insert(e.id).second);
    CHECK(paths.insert(e.path).second);
    // The id keeps its length marker (RFC 8794 §5): the marker position equals the byte count, and the value bits
    // are not all ones (reserved).
    int n = 4;
    while (n > 1 && (e.id >> (8 * (n - 1))) == 0) {
      --n;
    }
    const uint32_t first = e.id >> (8 * (n - 1));
    CHECK((first >> (8 - n)) == 1U);
    CHECK((e.id & ((1ULL << (7 * n)) - 1)) != ((1ULL << (7 * n)) - 1));
    CHECK(e.name[0] != '\0');
    CHECK(e.path[0] == '\\');
    CHECK(e.min_occurs >= 0);
    CHECK(e.max_occurs >= -1);
  }
}

TEST_CASE("ebml schema: the EBML header elements come from the header schema", "[container][ebml][fast]") {
  const auto* header = ebml::lookup(0x1A45DFA3);
  REQUIRE(header != nullptr);
  CHECK(std::string_view(header->name) == "EBML");
  CHECK(header->type == EbmlType::Master);
  const auto* doc_type = ebml::lookup(0x4282);
  REQUIRE(doc_type != nullptr);
  CHECK(std::string_view(doc_type->name) == "DocType");
  CHECK(doc_type->type == EbmlType::String);
  const auto* max_id = ebml::lookup(0x42F2);  // restated by the Matroska schema; one entry results
  REQUIRE(max_id != nullptr);
  CHECK(std::string_view(max_id->name) == "EBMLMaxIDLength");
  CHECK(std::string_view(max_id->default_value) == "4");
}

TEST_CASE("ebml schema: global elements keep their path markers", "[container][ebml][fast]") {
  const auto* crc = ebml::lookup(0xBF);
  REQUIRE(crc != nullptr);
  CHECK(std::string_view(crc->name) == "CRC-32");
  CHECK(std::string_view(crc->path) == "\\(1-\\)CRC-32");
  const auto* vd = ebml::lookup(0xEC);
  REQUIRE(vd != nullptr);
  CHECK(std::string_view(vd->name) == "Void");
  CHECK(std::string_view(vd->path) == "\\(-\\)Void");
}

TEST_CASE("ebml schema: types, defaults and occurrence limits are decoded", "[container][ebml][fast]") {
  const auto* cluster = ebml::lookup(0x1F43B675);
  REQUIRE(cluster != nullptr);
  CHECK(cluster->unknown_size_allowed);
  const auto* scale = ebml::lookup(0x2AD7B1);  // TimestampScale
  REQUIRE(scale != nullptr);
  CHECK(scale->type == EbmlType::UInteger);
  CHECK(std::string_view(scale->default_value) == "1000000");
  const auto* seek_head = ebml::lookup(0x114D9B74);
  REQUIRE(seek_head != nullptr);
  CHECK(seek_head->max_occurs == 2);
  const auto* seek = ebml::lookup(0x4DBB);  // no maxOccurs in the schema: unbounded
  REQUIRE(seek != nullptr);
  CHECK(seek->max_occurs == -1);
  CHECK(seek->min_occurs == 1);
  const auto* language = ebml::lookup(0x22B59C);
  REQUIRE(language != nullptr);
  CHECK(std::string_view(language->default_value) == "eng");
  const auto* title = ebml::lookup(0x7BA9);
  REQUIRE(title != nullptr);
  CHECK(title->type == EbmlType::Utf8);
  CHECK(title->default_value == nullptr);
  const auto* date = ebml::lookup(0x4461);  // DateUTC
  REQUIRE(date != nullptr);
  CHECK(date->type == EbmlType::Date);
}

TEST_CASE("ebml schema: only Segment and Cluster allow unknown sizes", "[container][ebml][fast]") {
  std::set<std::string> names;
  for (const auto& e : ebml::ebml_elements()) {
    if (e.unknown_size_allowed) {
      names.insert(e.name);
    }
  }
  const std::set<std::string> expected{"Cluster", "Segment"};
  CHECK(names == expected);
}

TEST_CASE("ebml schema: ids the schemas do not define are not found", "[container][ebml][fast]") {
  CHECK(ebml::lookup(0) == nullptr);
  CHECK(ebml::lookup(0xFFFFFFFF) == nullptr);
  CHECK(ebml::lookup(0x18538068) == nullptr);
  CHECK(ebml::lookup(0x1A45DFA2) == nullptr);
}
