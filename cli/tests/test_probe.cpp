// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.503 part a: `ovt probe` (chapter 12 §12.2.2) run in-process through ovt::cli::run(). The fast cases use a
// fragmented MP4 built here (no corpus media is committed); the [needs-corpus] case runs acceptance test 4 on a corpus
// file. Acceptance tests 2 and 4 are here; 1 and 5 (--fragments, --list-diagnostics) are part b, 3 (--hdr) part c.
// Goldens: cli/tests/golden/probe/ (OVT_UPDATE_GOLDEN=1 rewrites them).

#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app.h"
#include "completions.h"
#include "test_support/corpus.h"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using nlohmann::json;

// ---- a fragmented MP4: ftyp, moov (one avc1 track, empty tables, mvex/trex), then moof+mdat pairs -------------------

void put(Bytes& b, std::uint64_t v, int nbytes) {
  for (int s = (nbytes - 1) * 8; s >= 0; s -= 8) {
    b.push_back(static_cast<std::uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put(b, 8 + payload.size(), 4);
  put_str(b, type);
  return cat({b, payload});
}
Bytes full_box(std::string_view type, std::uint32_t version_flags, const Bytes& payload) {
  Bytes p;
  put(p, version_flags, 4);
  return box(type, cat({p, payload}));
}
Bytes words(std::initializer_list<std::uint64_t> v, int nbytes = 4) {
  Bytes b;
  for (const std::uint64_t x : v) {
    put(b, x, nbytes);
  }
  return b;
}
Bytes matrix() {
  return words({0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000});
}

Bytes moov() {
  const Bytes mvhd = full_box(
      "mvhd", 0, cat({words({0, 0, 1000, 0, 0x10000}), Bytes{1, 0}, Bytes(10, 0), matrix(), Bytes(24, 0), words({2})}));
  const Bytes tkhd =
      full_box("tkhd", 3, cat({words({0, 0, 1, 0, 0}), Bytes(16, 0), matrix(), words({64U << 16U, 32U << 16U})}));
  const Bytes mdhd = full_box("mdhd", 0, cat({words({0, 0, 1000, 0}), words({0x15C7, 0}, 2)}));
  const Bytes hdlr = full_box("hdlr", 0, cat({words({0}), Bytes{'v', 'i', 'd', 'e'}, Bytes(13, 0)}));
  Bytes entry = Bytes(6, 0);
  put(entry, 1, 2);
  entry = cat({entry, Bytes(16, 0), words({64, 32}, 2), words({0x480000, 0x480000, 0}), words({1}, 2), Bytes(32, 0),
               words({0x18, 0xFFFF}, 2), box("avcC", Bytes{1, 0x64, 0x00, 0x1F, 0xFF, 0xE0, 0})});
  const Bytes stbl = box("stbl", cat({full_box("stsd", 0, cat({words({1}), box("avc1", entry)})),
                                      full_box("stts", 0, words({0})), full_box("stsc", 0, words({0})),
                                      full_box("stsz", 0, words({0, 0})), full_box("stco", 0, words({0}))}));
  const Bytes minf = box("minf", cat({full_box("vmhd", 1, Bytes(8, 0)), stbl}));
  const Bytes trak = box("trak", cat({tkhd, box("mdia", cat({mdhd, hdlr, minf}))}));
  const Bytes mvex = box("mvex", full_box("trex", 0, words({1, 1, 100, 0, 0})));
  return box("moov", cat({mvhd, trak, mvex}));
}

// One fragment of `n` samples of 16 bytes and duration 100; the first sample of each fragment is sync.
Bytes fragment(std::uint32_t seq, std::uint64_t base_time, std::uint32_t n) {
  const auto build = [&](std::uint32_t data_offset) {
    Bytes trun_entries;
    for (std::uint32_t i = 0; i < n; ++i) {
      put(trun_entries, 100, 4);
      put(trun_entries, 16, 4);
      put(trun_entries, i == 0 ? 0x02000000 : 0x01010000, 4);
    }
    const Bytes traf =
        box("traf", cat({full_box("tfhd", 0x020000, words({1})), full_box("tfdt", 0x01000000, words({base_time}, 8)),
                         full_box("trun", 0x000701, cat({words({n, data_offset}), trun_entries}))}));
    return box("moof", cat({full_box("mfhd", 0, words({seq})), traf}));
  };
  const Bytes moof = build(static_cast<std::uint32_t>(build(0).size() + 8));
  return cat({moof, box("mdat", Bytes(std::size_t{16} * n, 0xAB))});
}

Bytes fragmented_mp4(int fragments) {
  Bytes b =
      cat({box("ftyp", cat({Bytes{'i', 's', 'o', '6'}, words({0}), Bytes{'i', 's', 'o', '6', 'm', 'p', '4', '1'}})),
           moov()});
  for (int i = 0; i < fragments; ++i) {
    b = cat({b, fragment(static_cast<std::uint32_t>(i + 1), 400U * static_cast<std::uint64_t>(i), 4)});
  }
  return b;
}

// ---- running the CLI -----------------------------------------------------------------------------------------

struct TempFile {
  fs::path path;
  explicit TempFile(const Bytes& bytes) {
    static int n = 0;
    path = fs::temp_directory_path() / ("ovt_probe_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::create_directories(path);
    path /= "a.mp4";
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

Result invoke(std::vector<std::string> args) {
  args.insert(args.begin(), "probe");
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

json run_json(const fs::path& file, std::vector<std::string> extra = {}) {
  extra.insert(extra.begin(), {file.string(), "-f", "json"});
  const Result r = invoke(extra);
  INFO(r.err);
  REQUIRE(r.code == 0);
  return json::parse(r.out);
}

std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> v;
  for (std::size_t at = 0; at < s.size();) {
    const std::size_t nl = s.find('\n', at);
    v.push_back(s.substr(at, nl - at));
    at = nl == std::string::npos ? s.size() : nl + 1;
  }
  return v;
}

void check_golden(const std::string& name, const std::string& actual) {
  const fs::path golden = fs::path(OVT_CLI_TEST_GOLDEN_DIR) / "probe" / name;
  if (std::getenv("OVT_UPDATE_GOLDEN") != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    std::ofstream(golden, std::ios::binary) << actual;
  }
  std::ifstream in(golden, std::ios::binary);
  REQUIRE(in.good());
  const std::string expected((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(actual == expected);
}

// Depth of the deepest box that carries `fields`, 0 when none does.
// NOLINTNEXTLINE(misc-no-recursion): the test files are a few levels deep
int deepest_with_fields(const json& boxes, int depth = 1) {
  int deepest = 0;
  for (const json& b : boxes) {
    if (b.contains("fields")) {
      deepest = std::max(deepest, depth);
    }
    if (b.contains("children")) {
      deepest = std::max(deepest, deepest_with_fields(b.at("children"), depth + 1));
    }
  }
  return deepest;
}

Bytes read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

// ---- acceptance tests ----------------------------------------------------------------------------------------

TEST_CASE("probe AT2: --expand 1 leaves every box below level 1 without fields", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(3));
  const json r = run_json(f.path, {"--expand", "1"}).at("result");
  CHECK(deepest_with_fields(r.at("root")) == 1);
  CHECK(deepest_with_fields(run_json(f.path, {"--expand", "2"}).at("result").at("root")) == 2);
  CHECK(deepest_with_fields(run_json(f.path, {"--expand", "none"}).at("result").at("root")) == 0);
  CHECK(deepest_with_fields(run_json(f.path).at("result").at("root")) > 2);
  check_golden("syn_frag3.expand1.json", r.dump(2) + "\n");
}

TEST_CASE("probe AT4: -f json is byte-identical across runs", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(4));
  for (const std::vector<std::string>& extra :
       {std::vector<std::string>{}, std::vector<std::string>{"--expand", "moov,tfdt"}}) {
    std::vector<std::string> args = {f.path.string(), "-f", "json"};
    args.insert(args.end(), extra.begin(), extra.end());
    const Result a = invoke(args);
    const Result b = invoke(args);
    REQUIRE(a.code == 0);
    CHECK(a.out == b.out);
  }
}

TEST_CASE("probe AT4 on a corpus file", "[cli][probe][needs-corpus]") {
  const fs::path file = ovt::test::corpus_or_skip("syn_h265_main10_pq_hdr_mismatch");
  const Result a = invoke({file.string(), "-f", "json"});
  const Result b = invoke({file.string(), "-f", "json"});
  REQUIRE(a.code == 0);
  CHECK(a.out == b.out);
}

TEST_CASE("probe: text tree line format", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const Result r = invoke({f.path.string(), "-f", "text"});
  REQUIRE(r.code == 0);
  const std::vector<std::string> l = lines(r.out);
  CHECK(l[0] == "ftyp   @0 +24");
  CHECK(l[1].starts_with("  major_brand = \"iso6\" (bits 64..96, ISO/IEC 14496-12"));
  CHECK(r.out.find("\n  mvhd   @") != std::string::npos);
  CHECK(r.out.find("  v0 f=0x3\n") != std::string::npos);  // tkhd
  check_golden("syn_frag2.txt", r.out);
}

TEST_CASE("probe: flattened csv of the tree", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const Result r = invoke({f.path.string(), "-f", "csv", "--expand", "moov,mvhd"});
  REQUIRE(r.code == 0);
  const std::vector<std::string> l = lines(r.out);
  CHECK(l[0] == "depth,path,name,descriptor,value,bit_offset,bit_length,spec_ref");
  CHECK(r.out.find("\n3,moov/mvhd/timescale,timescale,UInt,1000,") != std::string::npos);
  CHECK(r.out.find("\n1,moof[1],moof,box,") != std::string::npos);
  CHECK(r.out.find("moov/trak/tkhd/") == std::string::npos);  // trak is not on the list: no fields below it
  check_golden("syn_frag2.moov_mvhd.csv", r.out);
}

namespace {

// The child chain of `box` named by `path` (null when a link is missing).
json descend(json box, std::initializer_list<const char*> path) {
  for (const char* cc : path) {
    if (!box.contains("children")) {
      return nullptr;
    }
    const json& kids = box.at("children");
    const auto it = std::find_if(kids.begin(), kids.end(), [&](const json& k) { return k.at("fourcc") == cc; });
    if (it == kids.end()) {
      return nullptr;
    }
    box = *it;
  }
  return box;
}

}  // namespace

TEST_CASE("probe: --expand <fourcc list> expands the listed boxes and their ancestors", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const json root = run_json(f.path, {"--expand", "stsd"}).at("result").at("root");
  const json& moov = root.at(1);
  REQUIRE(moov.at("fourcc") == "moov");
  CHECK_FALSE(moov.at("children").at(0).contains("fields"));  // mvhd: not on the path to stsd
  const json box = descend(moov.at("children").at(1), {"mdia", "minf", "stbl", "stsd"});  // from trak
  REQUIRE(box.at("fourcc") == "stsd");
  CHECK(box.contains("fields"));                             // stsd
  CHECK_FALSE(box.at("children").at(0).contains("fields"));  // avc1: shown, not expanded
  CHECK_FALSE(root.at(0).contains("fields"));                // ftyp
}

TEST_CASE("probe: --offset prints one subtree, an unknown offset is exit 2", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const json root = run_json(f.path, {"--offset", "24"}).at("result").at("root");
  REQUIRE(root.size() == 1);
  CHECK(root[0].at("fourcc") == "moov");
  const Result bad = invoke({f.path.string(), "--offset", "3"});
  CHECK(bad.code == 2);
  CHECK(bad.err.find("CLI-0003") != std::string::npos);
}

TEST_CASE("probe: usage errors", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(1));
  CHECK(invoke({"-f", "json"}).code == 2);  // no input
  const Result bad = invoke({f.path.string(), "--expand", "toolong"});
  CHECK(bad.code == 2);
  CHECK(bad.err.find("CLI-0001") != std::string::npos);
  CHECK(invoke({f.path.string(), "--expand", "moov,,trak"}).code == 2);
  CHECK(invoke({f.path.string(), "--hdr"}).code == 2);  // part c
}

TEST_CASE("probe: -f fb writes the envelope and a ContainerRoot record", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const fs::path out = f.path.parent_path() / "out.fb";
  REQUIRE(invoke({f.path.string(), "-f", "fb", "-o", out.string()}).code == 0);
  const Bytes b = read_file(out);
  REQUIRE(b.size() > 32);
  CHECK(std::string_view(reinterpret_cast<const char*>(b.data()), 6) == "OVTFBS");
  CHECK(std::string(b.begin(), b.end()).find("OVTC") != std::string::npos);
}
