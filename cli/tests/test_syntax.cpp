// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.504a: `ovt syntax --list/--trace` (chapter 12 §12.2.3, §12.2.3.1), run in-process through
// ovt::cli::run() over the committed encoder-made seeds of the index_builder fuzz target (no corpus media is
// committed). A seed is the stream with one selector byte in front.

#include <unistd.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "completions.h"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr const char* kHevc = "seed_s_h265_main_bframes.bin";
constexpr const char* kAvc = "seed_s_h264_baseline.bin";
constexpr const char* kAv1 = "seed_s_av1_aom_420.bin";

// A seed written without its selector byte to a temporary file with the extension the sniffer expects.
struct Stream {
  fs::path path;
  Stream(const char* seed, const char* ext) {
    std::ifstream in(fs::path(OVT_CLI_TEST_SEEDS_DIR) / seed, std::ios::binary);
    REQUIRE(in.good());
    Bytes b{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(b.size() > 1);
    write(Bytes(b.begin() + 1, b.end()), ext);
  }
  Stream(const Bytes& bytes, const char* ext) { write(bytes, ext); }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  Stream(Stream&&) = delete;
  Stream& operator=(Stream&&) = delete;
  ~Stream() {
    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
  }
  [[nodiscard]] Bytes bytes() const {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

 private:
  void write(const Bytes& bytes, const char* ext) {
    static int n = 0;
    path = fs::temp_directory_path() / ("ovt_syntax_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::create_directories(path);
    path /= std::string("s.") + ext;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

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

Result invoke(const std::vector<std::string>& args) {
  std::FILE* out = std::tmpfile();
  std::FILE* err = std::tmpfile();
  REQUIRE((out != nullptr && err != nullptr));
  ovt::cli::Context ctx;
  ctx.out = out;
  ctx.err = err;
  ctx.completions = ovt::cli::embedded_completions();
  ctx.getenv = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  Result r;
  r.code = ovt::cli::run(args, ctx);
  r.out = slurp(out);
  r.err = slurp(err);
  static_cast<void>(std::fclose(out));
  static_cast<void>(std::fclose(err));
  return r;
}

Result syntax(const Stream& s, std::vector<std::string> extra) {
  std::vector<std::string> args = {"syntax", s.path.string()};
  args.insert(args.end(), extra.begin(), extra.end());
  return invoke(args);
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    out.push_back(l);
  }
  return out;
}

}  // namespace

TEST_CASE("syntax AT3: every --trace line starts with [ovt]", "[cli][syntax][fast]") {
  for (const auto& [seed, ext] : {std::pair{kHevc, "h265"}, std::pair{kAvc, "h264"}, std::pair{kAv1, "obu"}}) {
    const Stream s(seed, ext);
    const Result r = syntax(s, {"--trace"});
    CAPTURE(seed, r.err);
    REQUIRE(r.code == 0);
    const auto ls = lines(r.out);
    CHECK(ls.size() > 50);
    for (const std::string& l : ls) {
      CHECK(l.rfind("[ovt]", 0) == 0);
    }
  }
}

TEST_CASE("syntax trace: column layout, enum names and unit line", "[cli][syntax][fast]") {
  const Stream s(kHevc, "h265");
  const Result r = syntax(s, {"--trace"});
  REQUIRE(r.code == 0);
  const auto ls = lines(r.out);
  const auto unit2 =
      std::find(ls.begin(), ls.end(), "[ovt] Unit 2  type 34 (PPS_NUT)  offset 0x0000004C  length 7  temporal_id 0");
  REQUIRE(unit2 != ls.end());
  REQUIRE(std::next(unit2) != ls.end());
  CHECK(*std::next(unit2) == "[ovt] ---- NAL unit header ----  (H.265 7.3.1.2)");
  CHECK(std::find(ls.begin(), ls.end(),
                  "[ovt]      1  nal_unit_type                                               100010 = 34 (PPS_NUT)") !=
        ls.end());
  CHECK(std::find(ls.begin(), ls.end(),
                  "[ovt]     31  diff_cu_qp_delta_depth                                         010 = 1") != ls.end());
  CHECK(std::find(ls.begin(), ls.end(), "[ovt] ---- Picture Parameter Set ----  (H.265 7.3.2.3.1)") != ls.end());
}

TEST_CASE("syntax usage errors", "[cli][syntax][fast]") {
  const Stream s(kHevc, "h265");
  Result r = syntax(s, {});  // WI-1.504a: --trace is required until part b adds the unit list
  CHECK(r.code == 2);
  CHECK(r.err.find("CLI-0001") != std::string::npos);
  r = syntax(s, {"--trace", "-f", "json"});
  CHECK(r.code == 2);
  CHECK(r.err.find("CLI-0006") != std::string::npos);
  r = invoke({"syntax", "/nonexistent/x.h265", "--trace"});
  CHECK(r.code == 3);
  CHECK(r.err.find("CLI-0009") != std::string::npos);
}

TEST_CASE("syntax --trace prints the diagnostics of a damaged unit", "[cli][syntax][fast]") {
  Bytes cut = Stream(kHevc, "h265").bytes();
  cut.resize(50);  // inside the SPS
  const Stream t(cut, "h265");
  const Result trace = syntax(t, {"--trace"});
  REQUIRE(trace.code == 0);
  CHECK(trace.out.find("[ovt] !! ERROR BR-0001 ") != std::string::npos);
  for (const std::string& l : lines(trace.out)) {
    CHECK(l.rfind("[ovt]", 0) == 0);
  }
}

TEST_CASE("syntax determinism", "[cli][syntax][fast]") {
  const Stream s(kAv1, "obu");
  const Result ta = syntax(s, {"--trace"});
  REQUIRE(ta.code == 0);
  CHECK(ta.out == syntax(s, {"--trace"}).out);
}
