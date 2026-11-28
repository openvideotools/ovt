// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.503 parts a and b: `ovt probe` (chapter 12 §12.2.2) run in-process through ovt::cli::run(). The fast cases use
// a fragmented MP4 built here (no corpus media is committed); the [needs-corpus] cases run acceptance tests 1 and 4 on
// the corpus files they name. Acceptance tests 1, 2, 4 and 5 are here; 3 (--hdr) is part c.
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
#include "container/diagnostics.h"
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

// The top-level `moof` boxes of `file`, counted straight from the box headers.
std::size_t count_moofs(const Bytes& file) {
  std::size_t n = 0;
  for (std::size_t at = 0; at + 8 <= file.size();) {
    const std::size_t size = (std::size_t{file[at]} << 24U) | (std::size_t{file[at + 1]} << 16U) |
                             (std::size_t{file[at + 2]} << 8U) | file[at + 3];
    n += std::string_view(reinterpret_cast<const char*>(&file[at + 4]), 4) == "moof" ? 1U : 0U;
    at += std::max<std::size_t>(size, 8);
  }
  return n;
}

}  // namespace

// ---- acceptance tests ----------------------------------------------------------------------------------------

TEST_CASE("probe AT1: --fragments -f csv has one row per moof", "[cli][probe][fast]") {
  const Bytes mp4 = fragmented_mp4(5);
  const TempFile f(mp4);
  const Result r = invoke({f.path.string(), "--fragments", "-f", "csv"});
  REQUIRE(r.code == 0);
  const std::vector<std::string> rows = lines(r.out);
  CHECK(rows.front() ==
        "sequence_number,moof_offset,moof_size,base_data_offset,earliest_pts,duration,sample_count,data_offset,data_"
        "size");
  CHECK(rows.size() - 1 == count_moofs(mp4));
  CHECK(rows.size() - 1 == 5);
  check_golden("syn_frag5.fragments.csv", r.out);
}

TEST_CASE("probe AT1 on syn_mp4_frag: init and media segments joined", "[cli][probe][needs-corpus]") {
  // #829: syn_mp4_frag is a DASH segment set, so the file probed is the init segment followed by the media segments
  // in manifest order.
  const fs::path dir = ovt::test::corpus_or_skip("syn_mp4_frag").parent_path();
  Bytes joined;
  for (const char* name : {"seg_init.mp4", "seg_1.m4s", "seg_2.m4s"}) {
    const Bytes part = read_file(dir / name);
    REQUIRE_FALSE(part.empty());
    joined.insert(joined.end(), part.begin(), part.end());
  }
  const TempFile f(joined);
  const Result r = invoke({f.path.string(), "--fragments", "-f", "csv"});
  REQUIRE(r.code == 0);
  CHECK(lines(r.out).size() - 1 == count_moofs(joined));
}

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

TEST_CASE("probe AT5: --list-diagnostics lists the registry without an input", "[cli][probe][fast]") {
  // #829: the rows come from ovt::container::all_diagnostics() until ovt_container_list_diagnostics returns the
  // DiagnosticRegistry table (#772).
  const Result r = invoke({"--list-diagnostics", "-f", "csv"});
  REQUIRE(r.code == 0);
  CHECK(r.out == ovt::container::diagnostics_csv());
  const auto registry = ovt::container::all_diagnostics();
  const std::vector<std::string> rows = lines(r.out);
  REQUIRE(rows.size() == registry.size() + 1);
  CHECK(rows[1].starts_with(std::string(registry.front().code) + ","));
  CHECK(rows.back().starts_with(std::string(registry.back().code) + ","));
  const json j = json::parse(invoke({"--list-diagnostics", "-f", "json"}).out).at("result");
  CHECK(j.at("family") == "CNT");
  REQUIRE(j.at("entries").size() == registry.size());
  CHECK(j.at("entries").at(0).at("code") == std::string(registry.front().code));
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
  CHECK(invoke({f.path.string(), "--tracks", "--fragments", "-f", "csv"}).code == 2);
  CHECK(invoke({f.path.string(), "--bytes", "0:4", "--tracks"}).code == 2);  // standalone result (#829)
  CHECK(invoke({f.path.string(), "--list-diagnostics", "--bytes", "0:4"}).code == 2);
  CHECK(invoke({f.path.string(), "-t", "4", "--fragments"}).code == 2);
  CHECK(invoke({"--list-diagnostics", "-f", "fb", "-o", (f.path.parent_path() / "x.fb").string()}).code == 2);
}

TEST_CASE("probe: a view flag makes the result the ContainerRoot image", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(3));
  const json r = run_json(f.path, {"--tracks", "--fragments"}).at("result");
  CHECK(r.at("schema_version") == 1);
  CHECK(r.at("tree").at("format") == "isobmff");
  CHECK_FALSE(r.at("tree").contains("root"));  // only with --boxes
  CHECK(r.at("tree").at("tracks").at(0).at("handler") == "Video");
  CHECK(r.at("fragment_timelines").at(0).at("fragments").size() == 3);
  CHECK(r.at("is_fragmented") == true);
  const json boxes = run_json(f.path, {"--fragments", "--boxes", "--expand", "1"}).at("result");
  CHECK(deepest_with_fields(boxes.at("tree").at("root")) == 1);
  CHECK_FALSE(boxes.at("tree").contains("tracks"));
  const Result tracks = invoke({f.path.string(), "--tracks", "-f", "csv"});
  REQUIRE(tracks.code == 0);
  CHECK(lines(tracks.out).at(0) ==
        "index,handler,codec,codec_string,coded_w,coded_h,crop_w,crop_h,chroma_format,"
        "bit_depth_luma,bit_depth_chroma,fps,sample_count,bitrate_bps,language,encrypted");
  CHECK(lines(tracks.out).at(1).starts_with("0,vide,H264,"));
}

TEST_CASE("probe: --bytes prints the hex layout, and base64 in json", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(1));
  const Result r = invoke({f.path.string(), "--bytes", "0:24", "-f", "text"});
  REQUIRE(r.code == 0);
  CHECK(r.out ==
        "00000000  00 00 00 18 66 74 79 70  69 73 6f 36 00 00 00 00  |....ftypiso6....|\n"
        "00000010  69 73 6f 36 6d 70 34 31                           |iso6mp41|\n");
  const json b = run_json(f.path, {"--bytes", "4:4"}).at("result");
  CHECK(b == json{{"offset", 4}, {"length", 4}, {"data", "ZnR5cA=="}});
  CHECK(invoke({f.path.string(), "--bytes", "0:1048577"}).code == 2);
}

TEST_CASE("probe: -f fb writes the envelope and a ContainerRoot record", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(2));
  const fs::path out = f.path.parent_path() / "out.fb";
  REQUIRE(invoke({f.path.string(), "--tracks", "--fragments", "-f", "fb", "-o", out.string()}).code == 0);
  const Bytes b = read_file(out);
  REQUIRE(b.size() > 32);
  CHECK(std::string_view(reinterpret_cast<const char*>(b.data()), 6) == "OVTFBS");
  CHECK(std::string(b.begin(), b.end()).find("OVTC") != std::string::npos);
}

TEST_CASE("probe: json keys follow the schema field order (§6.8.1)", "[cli][probe][fast]") {
  // The goldens are compared after a parse, so the key order is checked on the raw text here.
  const TempFile f(fragmented_mp4(1));
  const auto in_order = [](const std::string& s, std::initializer_list<std::string_view> keys) {
    std::size_t at = 0;
    for (const std::string_view k : keys) {
      at = s.find("\"" + std::string(k) + "\":", at);
      if (at == std::string::npos) {
        return false;
      }
    }
    return true;
  };
  const Result tree = invoke({f.path.string(), "-f", "json", "--expand", "1"});
  REQUIRE(tree.code == 0);
  CHECK(in_order(tree.out, {"format", "root", "fourcc", "file_span", "header_length"}));
  const Result root = invoke({f.path.string(), "-f", "json", "--tracks"});
  REQUIRE(root.code == 0);
  CHECK(in_order(root.out, {"schema_version", "tree", "format", "tracks", "file_size", "file_name", "is_fragmented"}));
}

namespace {

// The file offsets of the last moof of `f` and of its traf, as text for --offset.
std::pair<std::string, std::string> last_moof_and_traf(const fs::path& f) {
  const json all = run_json(f).at("result").at("root");
  const json& moof = all.at(all.size() - 2);  // the last moof (then its mdat)
  const json& traf = moof.at("children").at(1);
  CHECK(moof.at("fourcc") == "moof");
  CHECK(traf.at("fourcc") == "traf");
  const auto at = [](const json& b) { return std::to_string(b.at("file_span").at("offset").get<std::uint64_t>()); };
  return {at(moof), at(traf)};
}

}  // namespace

// #835: with --offset the selected box is depth 1. More than 64 fragments leave the moofs unexpanded (CNT-0007)
// until asked.
TEST_CASE("probe: with --offset, --expand N counts from the selected box", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(70));
  const auto [moof, traf] = last_moof_and_traf(f.path);
  const json sel = run_json(f.path, {"--offset", moof, "--expand", "2"}).at("result").at("root");
  REQUIRE(sel.size() == 1);
  CHECK(deepest_with_fields(sel) == 2);  // moof and its children (mfhd, traf), not the traf's children
  const json inner = run_json(f.path, {"--offset", traf, "--expand", "2"}).at("result").at("root");
  REQUIRE(inner.size() == 1);
  CHECK(deepest_with_fields(inner) == 2);  // traf is a container: its children (tfhd, tfdt, trun) carry the fields
}

TEST_CASE("probe: --offset inside a lazy moof expands the moof on the way", "[cli][probe][fast]") {
  const TempFile f(fragmented_mp4(70));
  const std::string traf = last_moof_and_traf(f.path).second;
  const json bare = run_json(f.path, {"--offset", traf, "--expand", "none"}).at("result").at("root");
  REQUIRE(bare.size() == 1);
  CHECK(bare[0].at("fourcc") == "traf");
  CHECK(deepest_with_fields(bare) == 0);
}
