// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.509: `ovt cache` list, stats, clear, path and import (chapter 12 §12.2.13), run in-process through
// ovt::cli::run() against a throw-away cache directory. `import` uses a fake RecordIo: the C API it will use
// (ovt_json_to_fb, ovt_validate_*) is not in the library yet (SPEC-ASSUMPTION #673).

#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "app.h"
#include "cmd/record_io.h"
#include "completions.h"
#include "ovt/cache.h"
#include "ovt_model_generated.h"

namespace {

namespace fs = std::filesystem;
using ovt::cli::cmd::RecordKind;

std::string slurp(std::FILE* f) {
  std::string s;
  std::rewind(f);
  for (int c = std::fgetc(f); c != EOF; c = std::fgetc(f)) {
    s.push_back(static_cast<char>(c));
  }
  return s;
}

struct Result {
  int code = -1;
  std::string out;
  std::string err;
};

Result invoke(std::vector<std::string> args, const fs::path& dir, const ovt::cli::cmd::RecordIo* io = nullptr,
              bool stdin_tty = false, const char* stdin_text = "") {
  std::FILE* out = std::tmpfile();
  std::FILE* err = std::tmpfile();
  std::FILE* in = std::tmpfile();
  REQUIRE((out != nullptr && err != nullptr && in != nullptr));
  static_cast<void>(std::fputs(stdin_text, in));
  std::rewind(in);
  ovt::cli::Context ctx;
  ctx.out = out;
  ctx.err = err;
  ctx.in = in;
  ctx.stdin_tty = stdin_tty;
  ctx.record_io = io;
  ctx.completions = ovt::cli::embedded_completions();
  ctx.getenv = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  args.insert(args.end(), {"--cache-dir", dir.string()});
  Result r;
  r.code = ovt::cli::run(args, ctx);
  r.out = slurp(out);
  r.err = slurp(err);
  for (std::FILE* f : {out, err, in}) {
    static_cast<void>(std::fclose(f));
  }
  return r;
}

struct TempDir {
  fs::path path;
  TempDir() {
    path =
        fs::temp_directory_path() / ("ovt_cache_cmd_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  static int& counter() {
    static int n = 0;
    return n;
  }
};

constexpr std::uint8_t kHashByte = 0x5a;

std::vector<std::uint8_t> stream_index(std::uint8_t hash_byte) {
  flatbuffers::FlatBufferBuilder fbb;
  const std::vector<std::uint8_t> hash(32, hash_byte);
  fbb.Finish(ovt::fb::CreateStreamIndexDirect(fbb, ovt::fb::Codec::H265, 1, &hash), "OVTI");
  const std::span<const std::uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

std::vector<std::uint8_t> frame_analysis(int decode_index) {
  flatbuffers::FlatBufferBuilder fbb;
  const ovt::fb::FrameId id(decode_index, decode_index);
  const auto info = ovt::fb::CreateFrameInfo(fbb, &id);
  fbb.Finish(ovt::fb::CreateFrameAnalysisDirect(fbb, 1, "fake", "1", info), "OVTA");
  const std::span<const std::uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

// Seeds one entry holding an index through the C API.
void seed_index(const fs::path& dir, std::uint8_t hash_byte) {
  REQUIRE(ovt_cache_set_dir(dir.c_str()) == OVT_OK);
  ovt_Cache* cache = nullptr;
  REQUIRE(ovt_cache_open(nullptr, nullptr, &cache) == OVT_OK);
  const std::array<std::uint8_t, 32> hash = [&] {
    std::array<std::uint8_t, 32> h{};
    h.fill(hash_byte);
    return h;
  }();
  ovt_CacheEntry* entry = nullptr;
  REQUIRE(ovt_cache_entry(cache, hash.data(), &entry) == OVT_OK);
  const auto idx = stream_index(hash_byte);
  REQUIRE(ovt_cache_put_index(entry, idx.data(), idx.size()) == OVT_OK);
  ovt_cache_entry_release(entry);
  ovt_cache_close(cache);
}

ovt::cli::cmd::RecordIo fake_io(bool valid) {
  ovt::cli::cmd::RecordIo io;
  io.json_to_fb = [](std::string_view json, RecordKind kind, std::vector<std::uint8_t>& fb, std::string&) {
    const auto j = nlohmann::json::parse(json);
    fb = kind == RecordKind::FrameAnalysis ? frame_analysis(j["frame"]["id"]["decode_index"].get<int>())
                                           : stream_index(kHashByte);
    return OVT_OK;
  };
  io.validate = [valid](RecordKind, std::span<const std::uint8_t>, const std::uint8_t*,
                        std::vector<ovt::cli::Diagnostic>& out) {
    if (!valid) {
      out.push_back({"Error", "MDL-VAL-0001", "V-012 broken", std::nullopt, ""});
    }
    return OVT_OK;
  };
  return io;
}

std::string hash_hex(std::uint8_t b) {
  std::string s;
  for (int i = 0; i < 32; ++i) {
    s += fmt::format("{:02x}", b);
  }
  return s;
}

std::string envelope_json(int decode_index) {
  return R"({"schema_version":1,"command":"analyze","input":{"path":"a.mp4","size_bytes":1,"source_hash":")" +
         hash_hex(kHashByte) + R"("},"result":[{"frame":{"id":{"decode_index":)" + std::to_string(decode_index) +
         R"(}}}],"diagnostics":[],"status":"OK"})";
}

}  // namespace

TEST_CASE("cache list -f csv has one index row per indexed file (AT1)", "[fast][cli][cache]") {
  const TempDir d;
  seed_index(d.path, kHashByte);
  const Result r = invoke({"cache", "list", "-f", "csv"}, d.path);
  REQUIRE(r.code == 0);
  CHECK(r.out.starts_with("key,kind,source_path_basename,size_bytes,created,last_used\n"));
  CHECK(r.out.find(hash_hex(kHashByte) + ",index,") != std::string::npos);
  CHECK(std::count(r.out.begin(), r.out.end(), '\n') == 2);
}

TEST_CASE("cache clear --yes empties the cache and stats shows 0 entries (AT2)", "[fast][cli][cache]") {
  const TempDir d;
  seed_index(d.path, kHashByte);
  const Result before = invoke({"cache", "stats", "--json"}, d.path);
  REQUIRE(before.code == 0);
  CHECK(nlohmann::json::parse(before.out)["result"]["entries"].size() == 1);
  const Result clear = invoke({"cache", "clear", "--yes", "--json"}, d.path);
  REQUIRE(clear.code == 0);
  const Result after = invoke({"cache", "stats", "--json"}, d.path);
  REQUIRE(after.code == 0);
  const auto j = nlohmann::json::parse(after.out);
  CHECK(j["command"] == "cache stats");
  CHECK(j["result"]["entries"].empty());
  CHECK(j["result"]["total_bytes"] == 0);
  const Result text = invoke({"cache", "stats", "--text"}, d.path);
  CHECK(text.out.find("entries  0") != std::string::npos);
}

TEST_CASE("cache clear needs --yes or a terminal answer", "[fast][cli][cache]") {
  const TempDir d;
  seed_index(d.path, kHashByte);
  const Result refused = invoke({"cache", "clear"}, d.path);
  CHECK(refused.code == 2);
  CHECK(refused.err.find("clear needs --yes") != std::string::npos);
  CHECK(invoke({"cache", "stats", "--json"}, d.path).out.find(hash_hex(kHashByte)) != std::string::npos);

  const Result no = invoke({"cache", "clear"}, d.path, nullptr, true, "n\n");
  CHECK(no.code == 0);
  CHECK(invoke({"cache", "stats", "--json"}, d.path).out.find(hash_hex(kHashByte)) != std::string::npos);

  const Result yes = invoke({"cache", "clear"}, d.path, nullptr, true, "y\n");
  CHECK(yes.code == 0);
  CHECK(invoke({"cache", "stats", "--json"}, d.path).out.find(hash_hex(kHashByte)) == std::string::npos);
}

TEST_CASE("a declined clear leaves the cache and its cap alone, even with --max-size", "[fast][cli][cache]") {
  const TempDir d;
  seed_index(d.path, kHashByte);
  const Result no = invoke({"cache", "clear", "--max-size", "0.000001"}, d.path, nullptr, true, "n\n");
  CHECK(no.code == 0);
  const auto j = nlohmann::json::parse(invoke({"cache", "stats", "--json"}, d.path).out);
  CHECK(j["result"]["entries"].size() == 1);
  CHECK(j["result"]["max_bytes"] == 20ULL * 1073741824ULL);
}

TEST_CASE("cache clear --kind and --source", "[fast][cli][cache]") {
  const TempDir d;
  seed_index(d.path, kHashByte);
  const Result bad = invoke({"cache", "clear", "--yes", "--kind", "bogus"}, d.path);
  CHECK(bad.code == 2);
  const Result src = invoke({"cache", "clear", "--yes", "--source", "x.mp4"}, d.path);
  CHECK(src.code == 4);  // OVT_E_UNSUPPORTED until the source hasher exists
  const Result kind = invoke({"cache", "clear", "--yes", "--kind", "index"}, d.path);
  CHECK(kind.code == 0);
}

TEST_CASE("cache path and --max-size", "[fast][cli][cache]") {
  const TempDir d;
  const Result p = invoke({"cache", "path", "--text"}, d.path);
  REQUIRE(p.code == 0);
  CHECK(p.out.find(d.path.string()) == 0);
  const Result s = invoke({"cache", "stats", "--max-size", "3", "--json"}, d.path);
  REQUIRE(s.code == 0);
  CHECK(nlohmann::json::parse(s.out)["result"]["max_bytes"] == 3ULL * 1073741824ULL);
  CHECK(invoke({"cache", "stats", "--max-size", "-1"}, d.path).code == 2);
}

TEST_CASE("cache formats per subcommand", "[fast][cli][cache]") {
  const TempDir d;
  const Result r = invoke({"cache", "stats", "-f", "csv"}, d.path);
  CHECK(r.code == 2);
  CHECK(r.err.find("CLI-0002") != std::string::npos);
  CHECK(invoke({"cache", "list", "--flatbuffers", "-o", (d.path / "l.fb").string()}, d.path).code == 0);
  CHECK(invoke({"cache"}, d.path).code == 2);
}

TEST_CASE("cache import without the record codecs exits 4", "[fast][cli][cache]") {
  const TempDir d;
  std::ofstream(d.path / "a.json") << "{}";
  const Result r = invoke({"cache", "import", (d.path / "a.json").string()}, d.path);
  CHECK(r.code == 4);
}

TEST_CASE("cache import writes a validated FrameAnalysis (AT3 shape)", "[fast][cli][cache]") {
  const TempDir d;
  const fs::path cache = d.path / "cache";
  const fs::path in = d.path / "tiny.json";
  std::ofstream(in) << envelope_json(7);
  const auto io = fake_io(true);
  const Result r = invoke({"cache", "import", in.string(), "--json"}, cache, &io);
  REQUIRE(r.code == 0);
  const auto j = nlohmann::json::parse(r.out);
  CHECK(j["command"] == "cache import");

  REQUIRE(ovt_cache_set_dir(cache.c_str()) == OVT_OK);
  ovt_Cache* c = nullptr;
  REQUIRE(ovt_cache_open(nullptr, nullptr, &c) == OVT_OK);
  std::array<std::uint8_t, 32> h{};
  h.fill(kHashByte);
  ovt_CacheEntry* e = nullptr;
  REQUIRE(ovt_cache_entry(c, h.data(), &e) == OVT_OK);
  ovt_Buffer b{};
  REQUIRE(ovt_cache_get_frame(e, 7, &b) == OVT_OK);
  const auto want = frame_analysis(7);
  CHECK(std::vector<std::uint8_t>(std::span<const std::uint8_t>(b.data, b.size).begin(),
                                  std::span<const std::uint8_t>(b.data, b.size).end()) == want);
  ovt_buffer_free(&b);
  ovt_cache_entry_release(e);
  ovt_cache_close(c);
}

TEST_CASE("cache import never writes a record that fails validation (exit 5)", "[fast][cli][cache]") {
  const TempDir d;
  const fs::path cache = d.path / "cache";
  const fs::path in = d.path / "tiny.json";
  std::ofstream(in) << envelope_json(1);
  const auto io = fake_io(false);
  const Result r = invoke({"cache", "import", in.string(), "--json"}, cache, &io);
  CHECK(r.code == 5);
  CHECK(nlohmann::json::parse(r.out)["diagnostics"][0]["code"] == "MDL-VAL-0001");
  CHECK(nlohmann::json::parse(invoke({"cache", "stats", "--json"}, cache).out)["result"]["entries"].empty());
}

TEST_CASE("cache import takes the hash from the envelope; a bare record names none (exit 2); directories",
          "[fast][cli][cache]") {
  const TempDir d;
  const fs::path cache = d.path / "cache";
  const fs::path dir = d.path / "records";
  fs::create_directories(dir);
  std::ofstream(dir / "a.json") << envelope_json(1);
  std::ofstream(dir / "b.txt") << "junk";
  const auto io = fake_io(true);
  const Result ok = invoke({"cache", "import", dir.string(), "--json"}, cache, &io);
  CHECK(ok.code == 0);  // the unrecognised file is skipped with a warning
  CHECK(ok.out.find("Warning") != std::string::npos);
  CHECK(nlohmann::json::parse(ok.out)["result"]["entries"][0]["kind"] == "analysis");
  std::ofstream(dir / "bare.json") << R"({"frame":{"id":{"decode_index":2}}})";
  const Result bare = invoke({"cache", "import", dir.string()}, cache, &io);
  CHECK(bare.code == 2);
  CHECK(bare.err.find("names no source") != std::string::npos);
  CHECK(invoke({"cache", "import", dir.string(), "--source-hash", "zz"}, cache, &io).code == 2);  // no such flag
}

TEST_CASE("cache import rejects a FrameAnalysis without frame or id (exit 5)", "[fast][cli][cache]") {
  const TempDir d;
  const fs::path in = d.path / "bare.json";
  std::ofstream(in) << R"({"schema_version":1,"command":"analyze","input":{"source_hash":")" + hash_hex(kHashByte) +
                           R"("},"result":[{"frame":{"id":{"decode_index":-1}}}]})";
  ovt::cli::cmd::RecordIo io = fake_io(true);
  io.json_to_fb = [](std::string_view, RecordKind, std::vector<std::uint8_t>& fb, std::string&) {
    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(ovt::fb::CreateFrameAnalysisDirect(fbb, 1, "fake", "1", 0), "OVTA");  // no frame table
    const std::span<const std::uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    fb.assign(bytes.begin(), bytes.end());
    return OVT_OK;
  };
  const Result r = invoke({"cache", "import", in.string(), "--json"}, d.path / "c", &io);
  CHECK(r.code == 5);
  CHECK(nlohmann::json::parse(invoke({"cache", "stats", "--json"}, d.path / "c").out)["result"]["entries"].empty());
}

TEST_CASE("cache is in the completion scripts", "[fast][cli][cache]") {
  const TempDir d;
  for (const char* shell : {"bash", "zsh", "fish"}) {
    const Result r = invoke({"completion", shell}, d.path);
    CHECK(r.code == 0);
    CHECK(r.out.find("import") != std::string::npos);
    CHECK(r.out.find("older-than") != std::string::npos);
  }
}
