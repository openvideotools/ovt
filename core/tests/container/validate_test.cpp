// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the ISOBMFF validation driver (chapter 08 §8.5, WI-1.224a): CNT-1011 (required boxes), CNT-1012
// (duplicates), CNT-1013 (wrong parent) and the registry listing of `ovt probe --list-diagnostics`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/lazy.h"
#include "container/isobmff/validate.h"
#include "io/memory_file.hpp"

namespace {

using Bytes = std::vector<uint8_t>;
namespace iso = ovt::container::isobmff;
using ovt::fb::BoxNodeT;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}

// A progressive file with every box CNT-1011 asks for. `drop` names one child to leave out (the first match); `dup`
// names one child to write twice; `extra_in_moov` is appended to `moov`.
struct Variant {
  std::string_view drop = {};
  std::string_view dup = {};
  Bytes extra_in_moov = {};
  Bytes extra_top = {};
};

Bytes file_with(const Variant& v) {
  const auto keep = [&](std::string_view name, Bytes b) -> Bytes {
    if (name == v.drop) {
      return {};
    }
    if (name == v.dup) {
      return cat({b, b});
    }
    return b;
  };
  const Bytes stbl = box("stbl", cat({keep("stsd", box("stsd", Bytes(8, 0))), keep("stts", box("stts", Bytes(8, 0))),
                                      keep("stsc", box("stsc", Bytes(8, 0))), keep("stsz", box("stsz", Bytes(12, 0))),
                                      keep("stco", box("stco", Bytes(8, 0)))}));
  const Bytes minf = box("minf", cat({keep("dinf", box("dinf")), keep("stbl", stbl)}));
  const Bytes mdia =
      box("mdia",
          cat({keep("mdhd", box("mdhd", Bytes(24, 0))), keep("hdlr", box("hdlr", Bytes(28, 0))), keep("minf", minf)}));
  const Bytes trak = box("trak", cat({keep("tkhd", box("tkhd", Bytes(84, 0))), keep("mdia", mdia)}));
  const Bytes moov = box("moov", cat({keep("mvhd", box("mvhd", Bytes(100, 0))), trak, v.extra_in_moov}));
  return cat({keep("ftyp", box("ftyp", Bytes(16, 'a'))), moov, box("mdat", Bytes(40, 7)), v.extra_top});
}

iso::WalkResult walk_bytes(const Bytes& data) {
  const ovt::io::MemoryFile f(data);
  return iso::walk(f);
}

using Found = std::vector<std::pair<std::string, std::string>>;  // (code, message) of a code family

void collect(const BoxNodeT& n, std::string_view prefix, Found& out) {
  for (const auto& d : n.diagnostics) {
    if (d->code.starts_with(prefix)) {
      out.emplace_back(d->code, d->message);
    }
  }
  for (const auto& c : n.children) {
    collect(*c, prefix, out);
  }
}

Found findings(const iso::BoxNodes& top, std::string_view prefix = "CNT-101") {
  Found out;
  for (const auto& n : top) {
    collect(*n, prefix, out);
  }
  return out;
}

// CNT-1011..CNT-1013 only: the decoders of the other boxes may find their own faults in the zero payloads.
Found rule_findings(const iso::BoxNodes& top) {
  Found all = findings(top);
  std::erase_if(all,
                [](const auto& f) { return f.first != "CNT-1011" && f.first != "CNT-1012" && f.first != "CNT-1013"; });
  return all;
}

Found validated(const Bytes& data) {
  auto r = walk_bytes(data);
  iso::validate(r.top_level, iso::ValidationLevel::Standard);
  return rule_findings(r.top_level);
}

}  // namespace

TEST_CASE("a file with every required box has no CNT-1011..1013 finding", "[container][isobmff][validate][fast]") {
  CHECK(validated(file_with({})).empty());
}

TEST_CASE("each required box that is missing raises CNT-1011 on its parent", "[container][isobmff][validate][fast]") {
  struct Row {
    std::string_view drop;
    std::string_view parent;
  };
  const Row rows[] = {{"mvhd", "moov"}, {"tkhd", "trak"}, {"mdia", "trak"}, {"mdhd", "mdia"}, {"hdlr", "mdia"},
                      {"minf", "mdia"}, {"stbl", "minf"}, {"dinf", "minf"}, {"stsd", "stbl"}, {"stts", "stbl"},
                      {"stsc", "stbl"}, {"stsz", "stbl"}, {"stco", "stbl"}};
  for (const Row& row : rows) {
    CAPTURE(row.drop);
    const Found f = validated(file_with({.drop = row.drop}));
    // Dropping `mdia` or `minf` also drops the boxes below it; those are not reported (nothing to be inside).
    REQUIRE(f.size() == 1);
    CHECK(f[0].first == "CNT-1011");
    CHECK(f[0].second.find(row.drop) != std::string::npos);
    CHECK(f[0].second.find(row.parent) != std::string::npos);
  }
}

TEST_CASE("stz2 satisfies the stsz rule and co64 the stco rule", "[container][isobmff][validate][fast]") {
  // A file with stz2 and co64 in place of stsz and stco.
  const Bytes stbl = box("stbl", cat({box("stsd", Bytes(8, 0)), box("stts", Bytes(8, 0)), box("stsc", Bytes(8, 0)),
                                      box("stz2", Bytes(12, 0)), box("co64", Bytes(8, 0))}));
  const Bytes minf = box("minf", cat({box("dinf"), stbl}));
  const Bytes mdia = box("mdia", cat({box("mdhd", Bytes(24, 0)), box("hdlr", Bytes(28, 0)), minf}));
  const Bytes moov =
      box("moov", cat({box("mvhd", Bytes(100, 0)), box("trak", cat({box("tkhd", Bytes(84, 0)), mdia}))}));
  CHECK(validated(cat({box("ftyp", Bytes(16, 'a')), moov})).empty());
}

TEST_CASE("a second single-instance box raises CNT-1012 on the second", "[container][isobmff][validate][fast]") {
  for (const std::string_view dup : {"mvhd", "tkhd", "mdhd", "hdlr", "stsd", "stts", "stsz", "stco", "ftyp"}) {
    CAPTURE(dup);
    const Found f = validated(file_with({.dup = dup}));
    REQUIRE(f.size() == 1);
    CHECK(f[0].first == "CNT-1012");
    CHECK(f[0].second.find(dup) != std::string::npos);
  }
}

TEST_CASE("a third copy raises CNT-1012 once, not twice", "[container][isobmff][validate][fast]") {
  const Bytes mvhd = box("mvhd", Bytes(100, 0));
  auto r = walk_bytes(box("moov", cat({mvhd, mvhd, mvhd})));
  iso::validate(r.top_level, iso::ValidationLevel::Standard);
  const Found f = findings(r.top_level, "CNT-1012");
  CHECK(f.size() == 1);
}

TEST_CASE("a box in a parent the registry does not allow raises CNT-1013 on the box",
          "[container][isobmff][validate][fast]") {
  const Found elst = validated(file_with({.extra_in_moov = box("elst", Bytes(8, 0))}));
  REQUIRE(elst.size() == 1);
  CHECK(elst[0].first == "CNT-1013");
  CHECK(elst[0].second.find("elst") != std::string::npos);
  CHECK(elst[0].second.find("moov") != std::string::npos);
  // At the top level the parent is "file": `stss` belongs in `stbl` only.
  const Found top = validated(file_with({.extra_top = box("stss", Bytes(8, 0))}));
  REQUIRE(top.size() == 1);
  CHECK(top[0].first == "CNT-1013");
}

TEST_CASE("boxes the registry has no parent for are not reported", "[container][isobmff][validate][fast]") {
  // `free` and an unregistered fourcc have no allowed_parents; `uuid` is skipped.
  Bytes uuid_box = box("uuid", Bytes(16, 1));
  CHECK(validated(file_with({.extra_in_moov = cat({box("free", Bytes(4, 0)), box("zzzz", Bytes(4, 0)), uuid_box}),
                             .extra_top = box("free", Bytes(4, 0))}))
            .empty());
}

TEST_CASE("a subtree with a walk error is not checked for missing boxes", "[container][isobmff][validate][fast]") {
  Bytes data = file_with({});
  // Cut the file inside the moov: its children end early and a CNT-0108 sits on the cut box.
  data.resize(data.size() - 120);
  auto r = walk_bytes(data);
  iso::validate(r.top_level, iso::ValidationLevel::Standard);
  CHECK(findings(r.top_level, "CNT-1011").empty());
  CHECK_FALSE(findings(r.top_level, "CNT-0108").empty());
}

TEST_CASE("validate is idempotent and counts what it adds", "[container][isobmff][validate][fast]") {
  auto r = walk_bytes(file_with({.drop = "stsd", .extra_in_moov = box("elst", Bytes(8, 0))}));
  const size_t first = iso::validate(r.top_level, iso::ValidationLevel::Standard);
  CHECK(first == 2);
  CHECK(iso::validate(r.top_level, iso::ValidationLevel::Standard) == 0);
  CHECK(iso::validate(r.top_level, iso::ValidationLevel::Quick) == 0);
}

TEST_CASE("a diagnostic added by the driver carries the registry severity and the parent's header span",
          "[container][isobmff][validate][fast]") {
  auto r = walk_bytes(file_with({.drop = "mvhd"}));
  iso::validate(r.top_level, iso::ValidationLevel::Standard);
  const BoxNodeT& moov = *r.top_level[1];
  const auto it = std::ranges::find_if(moov.diagnostics, [](const auto& d) { return d->code == "CNT-1011"; });
  REQUIRE(it != moov.diagnostics.end());
  CHECK((*it)->severity == ovt::fb::Severity::Error);
  CHECK((*it)->span->bit_offset() == 0);
  CHECK((*it)->span->bit_length() == 64);
}

TEST_CASE("the reader validates the tree it holds and bumps the revision", "[container][isobmff][validate][fast]") {
  const Bytes data = file_with({.drop = "hdlr"});
  const ovt::io::MemoryFile f(data);
  iso::IsobmffReader reader(f);
  REQUIRE(reader.open());
  const uint64_t before = reader.tree_revision().value_or(0);
  CHECK(reader.validate(iso::ValidationLevel::Standard) == 1);
  CHECK(reader.tree_revision().value_or(0) > before);
  CHECK(reader.validate(iso::ValidationLevel::Standard) == 0);
}

TEST_CASE("the diagnostic registry lists every code the reader can emit", "[container][isobmff][validate][fast]") {
  for (const std::string_view code : {"CNT-1003", "CNT-1005", "CNT-1011", "CNT-1012", "CNT-1013", "CNT-1014",
                                      "CNT-1015", "CNT-1024", "CNT-1032", "CNT-4001", "CNT-4002", "CNT-4003"}) {
    CAPTURE(code);
    CHECK(ovt::container::diag(code) != nullptr);
  }
  CHECK(ovt::container::diag("CNT-1013")->severity == ovt::container::Severity::Warning);
  CHECK(ovt::container::diag("CNT-1014")->severity == ovt::container::Severity::Error);
}

TEST_CASE("--list-diagnostics CSV has one line per code with severity and spec reference",
          "[container][validate][list_diagnostics][fast]") {
  const std::string csv = ovt::container::diagnostics_csv();
  // Split into records: a quoted field may hold a comma, but no registry text holds a newline.
  std::istringstream in(csv);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  const auto all = ovt::container::all_diagnostics();
  REQUIRE(lines.size() == all.size() + 1);
  CHECK(lines[0] == "code,severity,message,spec_ref");
  std::set<std::string> codes;
  for (size_t i = 0; i < all.size(); ++i) {
    const std::string& l = lines[i + 1];
    constexpr std::array<std::string_view, 4> names = {"Info", "Warning", "Error", "Fatal"};
    const std::string_view severity = names[static_cast<size_t>(all[i].severity)];
    CHECK(l.starts_with(std::string(all[i].code) + "," + std::string(severity) + ","));
    CHECK(codes.insert(std::string(all[i].code)).second);
  }
  // Quoting: a field with a comma is wrapped in quotes and a quote inside it is doubled.
  CHECK(csv.find(",\"required box missing:") != std::string::npos);
  CHECK(csv.find("\"14496-12 §8.x \"\"Mandatory: Yes\"\"\"") != std::string::npos);
}
