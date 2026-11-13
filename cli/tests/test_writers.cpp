// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.500 output writers (chapter 12 §12.1.4): json envelope, fb stream, csv, text table.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "output/csv_writer.h"
#include "output/fb_writer.h"
#include "output/json_writer.h"
#include "output/text_writer.h"
#include "ovt_model_generated.h"

namespace {

std::string slurp(std::FILE* f) {
  std::string s;
  std::rewind(f);
  for (int c = std::fgetc(f); c != EOF; c = std::fgetc(f)) {
    s.push_back(static_cast<char>(c));
  }
  static_cast<void>(std::fclose(f));
  return s;
}

ovt::cli::Envelope sample_envelope() {
  ovt::cli::Envelope env;
  env.command = "probe";
  env.input = ovt::cli::EnvelopeInput{"a.mp4", 1234567, {0xab, 0x01}};
  env.diagnostics.push_back(
      {"Warning", "CNT-2001", "stts \"delta\"", std::pair<std::uint64_t, std::uint32_t>{8, 3}, ""});
  env.status = ovt::cli::ExitCode::Corrupt;
  return env;
}

}  // namespace

TEST_CASE("JSON envelope members and order", "[fast][cli]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  REQUIRE(ovt::cli::write_json_envelope(f, false, sample_envelope(), nullptr));
  const std::string s = slurp(f);
  CHECK(s.starts_with("{\"$schema\":\"https://ovt.dev/schema/ovt-cli-1.json\",\"ovt_version\":"));
  const auto j = nlohmann::json::parse(s);
  CHECK(j["schema_version"] == 1);
  CHECK(j["input"]["source_hash"] == "ab01");
  CHECK(j["diagnostics"][0]["span"]["bit_length"] == 3);
  CHECK(j["status"] == "CORRUPT");
}

TEST_CASE("JSON writer number rules", "[fast][cli]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  ovt::cli::JsonWriter w(f, false);
  w.begin_array();
  w.unsigned_integer(9007199254740992ULL);
  w.unsigned_integer(9007199254740993ULL);
  w.integer(-9007199254740993LL);
  w.number(0.1);
  w.number(std::nan(""));
  w.number(-std::numeric_limits<double>::infinity());
  w.end_array();
  REQUIRE(w.finish());
  CHECK(slurp(f) == "[9007199254740992,\"9007199254740993\",\"-9007199254740993\",0.1,\"nan\",\"-inf\"]\n");
}

TEST_CASE("JSON writer pretty layout and escaping", "[fast][cli]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  ovt::cli::JsonWriter w(f, true);
  w.begin_object();
  w.key("a");
  w.begin_array();
  w.end_array();
  w.key("b\n");
  w.string("q\"\x01");
  w.end_object();
  REQUIRE(w.finish());
  CHECK(slurp(f) == "{\n  \"a\": [],\n  \"b\\n\": \"q\\\"\\u0001\"\n}\n");
}

TEST_CASE("fb stream starts with the magic and a size-prefixed OVTE envelope", "[fast][cli]") {
  const auto record = ovt::cli::build_envelope_record(sample_envelope(), 2);
  const std::span<const std::uint8_t> bytes(record.data(), record.size());
  REQUIRE(ovt::cli::is_size_prefixed_record(bytes));
  CHECK(flatbuffers::BufferHasIdentifier(record.data(), "OVTE", true));
  flatbuffers::Verifier verifier(record.data(), record.size());
  REQUIRE(verifier.VerifySizePrefixedBuffer<ovt::fb::CliEnvelope>("OVTE"));
  const auto* env = flatbuffers::GetSizePrefixedRoot<ovt::fb::CliEnvelope>(record.data());
  CHECK(env->record_count() == 2);
  CHECK(env->status()->str() == "CORRUPT");
  CHECK(env->diagnostics()->Get(0)->severity() == ovt::fb::Severity::Warning);
}

TEST_CASE("fb stream writer frames records", "[fast][cli]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  ovt::cli::FbStreamWriter w(f);
  const auto record = ovt::cli::build_envelope_record(sample_envelope(), 0);
  const std::vector<std::uint8_t> junk = {1, 2, 3};
  CHECK_FALSE(w.write_record(junk));
  CHECK(w.write_record({record.data(), record.size()}));
  REQUIRE(w.finish());
  const std::string s = slurp(f);
  CHECK(s.size() == 8 + record.size());
  CHECK(s.substr(0, 8) == std::string("OVTFBS\x01\x00", 8));
  CHECK(ovt::cli::build_envelope_record(sample_envelope(), 0).size() == record.size());  // deterministic size
}

TEST_CASE("CSV quoting and numbers", "[fast][cli]") {
  using ovt::cli::CsvWriter;
  CHECK(CsvWriter::field("plain") == "plain");
  CHECK(CsvWriter::field("a,b") == "\"a,b\"");
  CHECK(CsvWriter::field("say \"hi\"\n") == "\"say \"\"hi\"\"\n\"");
  CHECK(CsvWriter::number(1.0 / 3.0) == "0.333333");
  CHECK(CsvWriter::number(2.5) == "2.5");
  CHECK(CsvWriter::number(-0.0000001) == "0");
}

TEST_CASE("CSV rows end with a newline", "[fast][cli]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  ovt::cli::CsvWriter w(f);
  w.row({"frame", "type"});
  const std::vector<std::string> row = {"0", "I,P"};
  w.row(row);
  REQUIRE(w.finish());
  CHECK(slurp(f) == "frame,type\n0,\"I,P\"\n");
  CHECK(ovt::cli::CsvWriter::boolean(true) == "1");
}

TEST_CASE("Text table aligns columns", "[fast][cli]") {
  ovt::cli::Table t;
  t.add_column("name");
  t.add_column("size", ovt::cli::Align::Right);
  t.add_row({"µs", "1"});
  t.add_row({"longer", "1234"});
  CHECK(t.render(false) == "name    size\nµs         1\nlonger  1234\n");
  CHECK(t.render(true).starts_with("\x1b[1mname\x1b[0m"));
}

TEST_CASE("SI sizes and HH:MM:SS.mmm times", "[fast][cli]") {
  CHECK(ovt::cli::si(999, "B") == "999 B");
  CHECK(ovt::cli::si(12345678, "B") == "12.3 MB");
  CHECK(ovt::cli::si(1500, "b/s") == "1.5 kb/s");
  CHECK(ovt::cli::si(999.95, "B") == "1.0 kB");
  CHECK(ovt::cli::si(999950, "B") == "1.0 MB");
  CHECK(ovt::cli::hms(3723.5) == "01:02:03.500");
  CHECK(ovt::cli::hms(0.0004) == "00:00:00.000");
  CHECK(ovt::cli::hms(-1.25) == "-00:00:01.250");
}
