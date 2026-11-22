// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.502: `ovt info` (chapter 12 §12.2.1) run in-process through ovt::cli::run() over synthetic MP4 files built
// here (no corpus media is committed). The golden files hold the `result` member of the JSON envelope.
// SPEC-ASSUMPTION(#783): the colour view and the index summary are unsupported on main, so the tests assert the
// omit-plus-diagnostic path that hands them off to WI-1.223b and WI-1.125.

#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "app.h"
#include "completions.h"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using nlohmann::json;

void put(Bytes& b, std::uint64_t v, int nbytes) {
  for (int s = (nbytes - 1) * 8; s >= 0; s -= 8) {
    b.push_back(s >= 64 ? std::uint8_t{0} : static_cast<std::uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
void put_all(Bytes& b, const Bytes& more) {
  b.insert(b.end(), more.begin(), more.end());
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put(b, 8 + payload.size(), 4);
  put_str(b, type);
  put_all(b, payload);
  return b;
}
Bytes full_box(std::string_view type, std::uint32_t flags, const Bytes& payload) {
  Bytes p;
  put(p, flags, 4);
  put_all(p, payload);
  return box(type, p);
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    put_all(b, p);
  }
  return b;
}

Bytes ftyp() {
  Bytes p;
  put_str(p, "isom");
  put(p, 512, 4);
  put_str(p, "iso6");
  put_str(p, "mp41");
  return box("ftyp", p);
}
Bytes mvhd() {
  Bytes p;
  put(p, 0, 8);
  put(p, 1000, 4);
  put(p, 0, 4);
  put(p, 0x10000, 4);
  put(p, 0x100, 2);
  put(p, 0, 10);
  for (const std::uint32_t m : {0x10000U, 0U, 0U, 0U, 0x10000U, 0U, 0U, 0U, 0x40000000U}) {
    put(p, m, 4);
  }
  put(p, 0, 24);
  put(p, 2, 4);
  return full_box("mvhd", 0, p);
}
Bytes tkhd(std::uint32_t id, std::uint32_t w, std::uint32_t h) {
  Bytes p;
  put(p, 0, 8);
  put(p, id, 4);
  put(p, 0, 4);
  put(p, 1000, 4);
  put(p, 0, 16);
  for (const std::uint32_t m : {0x10000U, 0U, 0U, 0U, 0x10000U, 0U, 0U, 0U, 0x40000000U}) {
    put(p, m, 4);
  }
  put(p, std::uint64_t{w} << 16U, 4);
  put(p, std::uint64_t{h} << 16U, 4);
  return full_box("tkhd", 3, p);
}
Bytes mdhd(std::uint32_t timescale, std::uint32_t duration) {
  Bytes p;
  put(p, 0, 8);
  put(p, timescale, 4);
  put(p, duration, 4);
  put(p, (5U << 10U) | (14U << 5U) | 7U, 2);  // "eng"
  put(p, 0, 2);
  return full_box("mdhd", 0, p);
}
Bytes hdlr(std::string_view type) {
  Bytes p;
  put(p, 0, 4);
  put_str(p, type);
  put(p, 0, 12);
  p.push_back(0);
  return full_box("hdlr", 0, p);
}
Bytes stsz(std::uint32_t size, std::uint32_t count) {
  Bytes p;
  put(p, size, 4);
  put(p, count, 4);
  return full_box("stsz", 0, p);
}
Bytes sample_entry(std::string_view fourcc, bool video, const Bytes& child) {
  Bytes p;
  put(p, 0, 6);
  put(p, 1, 2);
  if (video) {
    put(p, 0, 16);
    put(p, 1920, 2);
    put(p, 1080, 2);
    put(p, 0x480000, 4);
    put(p, 0x480000, 4);
    put(p, 0, 4);
    put(p, 1, 2);
    put(p, 0, 32);
    put(p, 0x18, 2);
    put(p, 0xFFFF, 2);
  } else {
    put(p, 0, 8);
    put(p, 2, 2);
    put(p, 16, 2);
    put(p, 0, 4);
    put(p, std::uint64_t{48000} << 16U, 4);
  }
  put_all(p, child);
  return box(fourcc, p);
}
Bytes stsd(const Bytes& entry) {
  Bytes p;
  put(p, 1, 4);
  put_all(p, entry);
  return full_box("stsd", 0, p);
}
Bytes trak(std::uint32_t id, std::string_view handler, const Bytes& entry, std::uint32_t timescale,
           std::uint32_t duration, std::uint32_t samples) {
  const Bytes stbl = box("stbl", cat({stsd(entry), stsz(4096, samples)}));
  const Bytes minf = box("minf", stbl);
  const Bytes mdia = box("mdia", cat({mdhd(timescale, duration), hdlr(handler), minf}));
  return box("trak", cat({tkhd(id, 1920, 1080), mdia}));
}
Bytes avcc() {
  return box("avcC", Bytes{1, 0x64, 0x00, 0x1F, 0xFF, 0xE0, 0});
}

// 30 frames at 30000/1001 fps (the stsz table is constant size, so a 1-hour file costs no memory).
Bytes build_mp4(bool fast_start, bool with_audio, std::uint32_t video_samples = 30,
                std::uint32_t video_duration = 30030) {
  Bytes moov_payload = mvhd();
  put_all(moov_payload, trak(1, "vide", sample_entry("avc1", true, avcc()), 30000, video_duration, video_samples));
  if (with_audio) {
    put_all(moov_payload, trak(2, "soun", sample_entry("mp4a", false, {}), 48000, 48000, 47));
  }
  const Bytes moov = box("moov", moov_payload);
  const Bytes mdat = box("mdat", Bytes(16, 0));
  return fast_start ? cat({ftyp(), moov, mdat}) : cat({ftyp(), mdat, moov});
}

struct TempFile {
  fs::path path;
  explicit TempFile(const Bytes& bytes, const char* name = "a.mp4") {
    static int n = 0;
    path = fs::temp_directory_path() / ("ovt_info_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::create_directories(path);
    path /= name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // NOLINT
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() {
    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
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

json run_json(const std::vector<std::string>& extra, const TempFile& f) {
  std::vector<std::string> args = {"info", f.path.string(), "-f", "json"};
  args.insert(args.end(), extra.begin(), extra.end());
  const Result r = invoke(args);
  REQUIRE(r.code == 0);
  return json::parse(r.out);
}

bool has_diag(const json& env, std::string_view code, std::string_view needle) {
  const json& diags = env.at("diagnostics");
  return std::any_of(diags.begin(), diags.end(), [&](const json& d) {
    return d.at("code") == code && d.at("message").get<std::string>().find(needle) != std::string::npos;
  });
}

}  // namespace

TEST_CASE("info: json result of a fast-start AVC file matches the golden", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, false));
  const json env = run_json({}, f);
  const std::string actual = env.at("result").dump(2) + "\n";
  const fs::path golden = fs::path(OVT_CLI_TEST_GOLDEN_DIR) / "info" / "syn_avc_fast_start.json";
  if (std::getenv("OVT_UPDATE_GOLDEN") != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    std::ofstream(golden, std::ios::binary) << actual;
  }
  std::ifstream in(golden, std::ios::binary);
  REQUIRE(in.good());
  const std::string expected((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(actual == expected);
}

TEST_CASE("info: container fields and the video track", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, true));
  const json env = run_json({}, f);
  CHECK(env.at("command") == "info");
  CHECK(env.at("input").at("path") == f.path.string());
  const json& r = env.at("result");
  CHECK(r.at("format") == "isobmff");
  CHECK(r.at("brands") == json::array({"isom", "iso6", "mp41"}));
  CHECK(r.at("fast_start") == true);
  CHECK(r.at("fragmented") == false);
  REQUIRE(r.at("tracks").size() == 1);  // video only by default
  const json& t = r.at("tracks")[0];
  CHECK(t.at("handler") == "vide");
  CHECK(t.at("codec") == "H264");
  CHECK(t.at("codec_string") == "avc1.64001F");
  CHECK(t.at("fps") == "30000/1001");
  CHECK(t.at("sample_count") == 30);
  CHECK(t.at("coded_size").at("w") == 1920);
  CHECK(t.at("language") == "eng");
}

TEST_CASE("info: --all-tracks adds the audio track", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, true));
  const json r = run_json({"--all-tracks"}, f).at("result");
  REQUIRE(r.at("tracks").size() == 2);
  CHECK(r.at("tracks")[1].at("handler") == "soun");
}

TEST_CASE("info: fast_start is false when mdat precedes moov", "[cli][info][fast]") {
  const TempFile f(build_mp4(false, false));
  CHECK(run_json({}, f).at("result").at("fast_start") == false);
}

TEST_CASE("info: unsupported colour and index views are omitted with an Info diagnostic", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, false));
  const json plain = run_json({}, f);
  CHECK_FALSE(plain.at("result").at("tracks")[0].contains("color"));
  CHECK_FALSE(plain.at("result").contains("index_summary"));
  CHECK(has_diag(plain, "CLI-0012", "'tracks[].color' omitted"));
  CHECK(plain.at("status") == "OK");
  const json full = run_json({"--full"}, f);
  CHECK_FALSE(full.at("result").contains("index_summary"));
  CHECK(has_diag(full, "CLI-0012", "'index_summary' omitted"));
}

TEST_CASE("info: -f csv is a usage error", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, false));
  const Result r = invoke({"info", f.path.string(), "-f", "csv"});
  CHECK(r.code == 2);
  CHECK(r.err.find("CLI-0002") != std::string::npos);
  CHECK(r.out.empty());
}

TEST_CASE("info: a missing file exits 3 and an unknown format exits 4", "[cli][info][fast]") {
  const Result missing = invoke({"info", "/nonexistent/ovt_info_test.mp4"});
  CHECK(missing.code == 3);
  CHECK(missing.err.find("CLI-0009") != std::string::npos);
  const TempFile junk(Bytes(64, 0x5A), "junk.bin");
  CHECK(invoke({"info", junk.path.string()}).code == 4);
}

TEST_CASE("info: text and fb output, and determinism", "[cli][info][fast]") {
  const TempFile f(build_mp4(true, false));
  const Result text = invoke({"info", f.path.string(), "-f", "text"});
  CHECK(text.code == 0);
  CHECK(text.out.find("avc1.64001F") != std::string::npos);
  const Result fb = invoke({"info", f.path.string(), "-f", "fb"});
  CHECK(fb.code == 0);
  CHECK(fb.out.rfind("OVTFBS", 0) == 0);
  CHECK(invoke({"info", f.path.string(), "-f", "json"}).out == invoke({"info", f.path.string(), "-f", "json"}).out);
}

TEST_CASE("info: a 1-hour header-only file reports quickly", "[cli][info][fast]") {
  // 216000 frames at 60 fps: a constant-size stsz costs no memory. The 200 ms budget of §12.2.1 is recorded on the
  // reference machine in docs/acceptance/wi/WI-1.502.md; here only a generous sanity bound is asserted.
  const TempFile f(build_mp4(true, false, 216000, 3600U * 30000U));
  const auto t0 = std::chrono::steady_clock::now();
  const json r = run_json({}, f).at("result");
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.at("tracks")[0].at("sample_count") == 216000);
  CHECK(ms < 2000.0);
}
