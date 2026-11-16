// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt cache` (chapter 12 §12.2.13, WI-1.509) over the cache C API of ovt/cache.h (§12.3.14).
//
// SPEC-ASSUMPTION(#673): (1) import goes through the RecordIo seam until the C API wrappers exist; (2) a
// FrameAnalysis names its entry through the envelope's input.source_hash or its PlaneRef blob ids (else exit 2); (3)
// every `clear` needs --yes or a TTY answer. (4) Library failures are reported as CLI-0009 diagnostics (no cache code
// is registered for the CLI).

#include "cmd/cache.h"

#include <fmt/format.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

#include "app.h"
#include "output/csv_writer.h"
#include "output/fb_writer.h"
#include "output/json_writer.h"
#include "output/text_writer.h"
#include "ovt/cache.h"
#include "ovt_model_generated.h"

namespace ovt::cli::cmd {

namespace {

namespace fs = std::filesystem;

constexpr double kGiB = 1073741824.0;
constexpr std::size_t kMaxRecordBytes = std::size_t{1} << 30U;

struct BufferGuard {
  ovt_Buffer b{};
  BufferGuard() = default;
  BufferGuard(const BufferGuard&) = delete;
  BufferGuard& operator=(const BufferGuard&) = delete;
  ~BufferGuard() { ovt_buffer_free(&b); }
};

Diagnostic error(std::string message, std::string code = "CLI-0009") {
  return Diagnostic{"Error", std::move(code), std::move(message), std::nullopt, ""};
}

CacheOutcome fail(ovt_status st, std::string what) {
  CacheOutcome o;
  o.code = from_status(st);
  o.diagnostics.push_back(error(fmt::format("{}: {}", what, ovt_status_string(st))));
  return o;
}

std::string gib(std::uint64_t bytes) {
  return fmt::format("{:.2f} GiB", static_cast<double>(bytes) / kGiB);
}

// ---- CacheReport access ---------------------------------------------------------------------------------

struct Report {
  BufferGuard buf;
  const ovt::fb::CacheReport* root = nullptr;
};

ovt_status load_report(Report& r) {
  const ovt_status st = ovt_cache_report(&r.buf.b);
  if (st != OVT_OK) {
    return st;
  }
  r.root = flatbuffers::GetRoot<ovt::fb::CacheReport>(r.buf.b.data);
  return OVT_OK;
}

std::string str(const flatbuffers::String* s) {
  return s != nullptr ? s->str() : std::string();
}

bool selected(const std::set<std::string>* only, const ovt::fb::CacheEntry& e) {
  return only == nullptr || only->contains(str(e.key()));
}

std::size_t distinct_keys(const ovt::fb::CacheReport& r, const std::set<std::string>* only) {
  std::set<std::string> keys;
  if (r.entries() != nullptr) {
    for (const auto* e : *r.entries()) {
      if (selected(only, *e)) {
        keys.insert(str(e->key()));
      }
    }
  }
  return keys.size();
}

// §6.8.1 form of a CacheReport (every scalar written, schema order). Replace with ovt_fb_to_json once it exists.
void write_report_json(JsonWriter& w, const ovt::fb::CacheReport& r, const std::set<std::string>* only) {
  w.begin_object();
  w.key("dir");
  w.string(str(r.dir()));
  w.key("entries");
  w.begin_array();
  std::uint64_t total = 0;
  if (r.entries() != nullptr) {
    for (const auto* e : *r.entries()) {
      if (!selected(only, *e)) {
        continue;
      }
      total += e->size_bytes();
      w.begin_object();
      w.key("key");
      w.string(str(e->key()));
      w.key("kind");
      w.string(str(e->kind()));
      w.key("source_basename");
      w.string(str(e->source_basename()));
      w.key("size_bytes");
      w.unsigned_integer(e->size_bytes());
      w.key("created");
      w.string(str(e->created()));
      w.key("last_used");
      w.string(str(e->last_used()));
      w.end_object();
    }
  }
  w.end_array();
  w.key("total_bytes");
  w.unsigned_integer(only == nullptr ? r.total_bytes() : total);
  w.key("max_bytes");
  w.unsigned_integer(r.max_bytes());
  w.key("hits");
  w.unsigned_integer(r.hits());
  w.key("misses");
  w.unsigned_integer(r.misses());
  w.end_object();
}

// The CacheReport record of the fb stream: size-prefixed, no file identifier (§12.1.4).
flatbuffers::DetachedBuffer report_record(const ovt::fb::CacheReport& r, const std::set<std::string>* only) {
  ovt::fb::CacheReportT t;
  r.UnPackTo(&t);
  if (only != nullptr) {
    std::erase_if(t.entries, [&](const auto& e) { return !only->contains(e->key); });
    t.total_bytes = 0;
    for (const auto& e : t.entries) {
      t.total_bytes += e->size_bytes;
    }
  }
  flatbuffers::FlatBufferBuilder fbb(1024);
  fbb.FinishSizePrefixed(ovt::fb::CacheReport::Pack(fbb, &t));
  return fbb.Release();
}

bool write_fb(const Envelope& env, std::FILE* out, const ovt::fb::CacheReport* report,
              const std::set<std::string>* only) {
  FbStreamWriter w(out);
  const flatbuffers::DetachedBuffer rec = build_envelope_record(env, report != nullptr ? 1U : 0U);
  bool ok = w.write_record(std::span<const std::uint8_t>(rec.data(), rec.size()));
  if (ok && report != nullptr) {
    const flatbuffers::DetachedBuffer r = report_record(*report, only);
    ok = w.write_record(std::span<const std::uint8_t>(r.data(), r.size()));
  }
  return w.finish() && ok;
}

// Envelope plus result in the requested format; `text` writes the text/csv forms.
bool emit(const Globals& g, std::FILE* out, const char* command, const ovt::fb::CacheReport* report,
          const std::set<std::string>* only, const std::vector<Diagnostic>& diags, ExitCode code,
          const std::function<bool()>& text) {
  Envelope env;
  env.command = command;
  env.diagnostics = diags;
  env.status = code;
  if (g.format == Format::Json) {
    return write_json_envelope(out, g.pretty, env, [&](JsonWriter& w) {
      if (report != nullptr) {
        write_report_json(w, *report, only);
      } else {
        w.begin_object();
        w.end_object();
      }
    });
  }
  if (g.format == Format::Fb) {
    return write_fb(env, out, report, only);
  }
  return text();
}

std::optional<UsageIssue> check_format(const Globals& g, std::string_view sub, bool csv) {
  std::vector<Format> ok = {Format::Text, Format::Json, Format::Fb};
  if (csv) {
    ok.push_back(Format::Csv);
  }
  if (std::find(ok.begin(), ok.end(), g.format) != ok.end()) {
    return std::nullopt;
  }
  std::vector<std::string_view> names;
  names.reserve(ok.size());
  for (const Format f : ok) {
    names.push_back(format_name(f));
  }
  std::sort(names.begin(), names.end());
  std::string list;
  for (const auto n : names) {
    list += (list.empty() ? "" : ", ") + std::string(n);
  }
  return UsageIssue{
      "CLI-0002",
      fmt::format("format '{}' is not supported by 'cache {}'; supported: {}", format_name(g.format), sub, list),
      fmt::format("ovt cache {}", sub)};
}

CacheOutcome usage(std::string code, std::string message, std::string command) {
  CacheOutcome o;
  o.code = ExitCode::Usage;
  o.usage = UsageIssue{std::move(code), std::move(message), std::move(command)};
  return o;
}

// ---- Subcommands ------------------------------------------------------------------------------------------

ovt_status apply_max_size(double gib_value) {
  const auto bytes = static_cast<std::uint64_t>(std::max(1.0, std::round(gib_value * kGiB)));
  ovt_status st = ovt_cache_set_max_bytes(bytes);
  if (st != OVT_OK) {
    return st;
  }
  ovt_Cache* cache = nullptr;  // the next open of the default root records the cap in cache.json
  st = ovt_cache_open(nullptr, nullptr, &cache);
  ovt_cache_close(cache);
  return st;
}

bool write_all(std::FILE* out, const std::string& s) {
  return std::fwrite(s.data(), 1, s.size(), out) == s.size();
}

CacheOutcome finish(const Globals& g, std::FILE* out, const char* command, const Report& rep,
                    const std::set<std::string>* only, std::vector<Diagnostic> diags, ExitCode code,
                    const std::function<bool()>& text) {
  CacheOutcome o;
  o.code = code;
  o.write_failed = !emit(g, out, command, rep.root, only, diags, code, text);
  o.diagnostics = std::move(diags);
  return o;
}

CacheOutcome run_list(const Globals& g, std::FILE* out, const CacheEnv& env) {
  if (auto bad = check_format(g, "list", true)) {
    CacheOutcome o;
    o.code = ExitCode::Usage;
    o.usage = std::move(bad);
    return o;
  }
  Report rep;
  if (const ovt_status st = load_report(rep); st != OVT_OK) {
    return fail(st, "cannot read the cache");
  }
  const auto text = [&]() {
    if (g.format == Format::Csv) {
      CsvWriter w(out);
      w.row({"key", "kind", "source_path_basename", "size_bytes", "created", "last_used"});
      if (rep.root->entries() != nullptr) {
        for (const auto* e : *rep.root->entries()) {
          w.row(std::array<std::string, 6>{str(e->key()), str(e->kind()), str(e->source_basename()),
                                           std::to_string(e->size_bytes()), str(e->created()), str(e->last_used())});
        }
      }
      return w.finish();
    }
    Table t;
    t.add_column("key");
    t.add_column("kind");
    t.add_column("source");
    t.add_column("size", Align::Right);
    t.add_column("created");
    t.add_column("last used");
    if (rep.root->entries() != nullptr) {
      for (const auto* e : *rep.root->entries()) {
        t.add_row({str(e->key()).substr(0, 12), str(e->kind()), str(e->source_basename()),
                   si(static_cast<double>(e->size_bytes()), "B"), str(e->created()), str(e->last_used())});
      }
    }
    return t.write(out, env.color);
  };
  return finish(g, out, "cache list", rep, nullptr, {}, ExitCode::Ok, text);
}

CacheOutcome run_stats(const CacheArgs& a, const Globals& g, std::FILE* out) {
  if (auto bad = check_format(g, "stats", false)) {
    CacheOutcome o;
    o.code = ExitCode::Usage;
    o.usage = std::move(bad);
    return o;
  }
  if (a.stats->count("--max-size") > 0) {
    if (const ovt_status st = apply_max_size(a.max_size_gib); st != OVT_OK) {
      return fail(st, "cannot set the cache limit");
    }
  }
  Report rep;
  if (const ovt_status st = load_report(rep); st != OVT_OK) {
    return fail(st, "cannot read the cache");
  }
  const auto text = [&]() {
    const std::string s =
        fmt::format("dir      {}\nentries  {}\nsize     {}\nlimit    {}\nhits     {}\nmisses   {}\n",
                    str(rep.root->dir()), distinct_keys(*rep.root, nullptr), gib(rep.root->total_bytes()),
                    gib(rep.root->max_bytes()), rep.root->hits(), rep.root->misses());
    return write_all(out, s);
  };
  return finish(g, out, "cache stats", rep, nullptr, {}, ExitCode::Ok, text);
}

bool confirm(const CacheEnv& env, const std::string& what) {
  const std::string prompt = fmt::format("Delete {} from the cache? [y/N] ", what);
  static_cast<void>(std::fwrite(prompt.data(), 1, prompt.size(), env.err));
  static_cast<void>(std::fflush(env.err));
  std::array<char, 16> line{};
  if (env.in == nullptr || std::fgets(line.data(), static_cast<int>(line.size()), env.in) == nullptr) {
    return false;
  }
  return line[0] == 'y' || line[0] == 'Y';
}

CacheOutcome run_clear(const CacheArgs& a, const Globals& g, const CacheEnv& env, std::FILE* out) {
  if (auto bad = check_format(g, "clear", false)) {
    CacheOutcome o;
    o.code = ExitCode::Usage;
    o.usage = std::move(bad);
    return o;
  }
  // SPEC-ASSUMPTION(#673): every clear needs --yes or a TTY answer.
  if (!a.yes && !env.stdin_tty) {
    return usage("CLI-0001", "clear needs --yes when stdin is not a terminal", "ovt cache clear");
  }
  std::vector<Diagnostic> diags;
  bool go = true;
  if (!a.yes) {
    std::string what = a.kind.empty() ? "entries" : a.kind + " data";
    if (a.older_than_days > 0) {
      what += fmt::format(" last used {}+ days ago", a.older_than_days);
    }
    go = confirm(env, what);
    if (!go) {
      static_cast<void>(std::fputs("nothing deleted\n", env.err));
    }
  }
  if (a.clear->count("--max-size") > 0) {
    if (const ovt_status st = apply_max_size(a.max_size_gib); st != OVT_OK) {
      return fail(st, "cannot set the cache limit");
    }
  }
  if (go) {
    const ovt_status st = ovt_cache_clear(a.kind.empty() ? nullptr : a.kind.c_str(),
                                          a.source.empty() ? nullptr : a.source.c_str(), a.older_than_days);
    if (st != OVT_OK) {
      return fail(st, "cannot clear the cache");
    }
  }
  Report rep;
  if (const ovt_status st = load_report(rep); st != OVT_OK) {
    return fail(st, "cannot read the cache");
  }
  const auto text = [&]() {
    return write_all(out, fmt::format("{} entries, {} left in the cache\n", distinct_keys(*rep.root, nullptr),
                                      gib(rep.root->total_bytes())));
  };
  return finish(g, out, "cache clear", rep, nullptr, std::move(diags), ExitCode::Ok, text);
}

CacheOutcome run_path(const Globals& g, std::FILE* out) {
  if (auto bad = check_format(g, "path", false)) {
    CacheOutcome o;
    o.code = ExitCode::Usage;
    o.usage = std::move(bad);
    return o;
  }
  Report rep;
  if (const ovt_status st = load_report(rep); st != OVT_OK) {
    return fail(st, "cannot read the cache");
  }
  return finish(g, out, "cache path", rep, nullptr, {}, ExitCode::Ok,
                [&]() { return write_all(out, str(rep.root->dir()) + "\n"); });
}

// ---- import ------------------------------------------------------------------------------------------------

using Hash = std::array<std::uint8_t, 32>;
std::optional<Hash> parse_hash(std::string_view hex);

struct Record {
  RecordKind kind = RecordKind::FrameAnalysis;
  std::vector<std::uint8_t> fb;
  std::string origin;
  std::optional<Hash> hint;  // input.source_hash of the CLI envelope the record came from
};

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (f.bad() || bytes.size() > kMaxRecordBytes) {
    return std::nullopt;
  }
  return bytes;
}

bool inflate_if_zstd(std::vector<std::uint8_t>& bytes) {
  static constexpr std::array<std::uint8_t, 4> kMagic = {0x28, 0xB5, 0x2F, 0xFD};
  if (bytes.size() < 4 || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return true;
  }
  const unsigned long long size = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > kMaxRecordBytes) {
    return false;
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  const std::size_t n = ZSTD_decompress(out.data(), out.size(), bytes.data(), bytes.size());
  if (ZSTD_isError(n) != 0U || n != out.size()) {
    return false;
  }
  bytes = std::move(out);
  return true;
}

std::optional<RecordKind> json_kind(const nlohmann::json& j) {
  if (!j.is_object()) {
    return std::nullopt;
  }
  if (j.contains("frame")) {
    return RecordKind::FrameAnalysis;
  }
  if (j.contains("units") || j.contains("frames") || j.contains("source_hash")) {
    return RecordKind::StreamIndex;
  }
  return std::nullopt;
}

struct LoadError {
  ExitCode code = ExitCode::Ok;
  std::string why;
};

// The records of a JSON document: one object, an array of them, or a CLI envelope whose result is either.
std::optional<LoadError> load_json_records(const std::string& text, const RecordIo& io, const std::string& origin,
                                           std::vector<Record>& out) {
  nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded()) {
    return LoadError{ExitCode::Corrupt, "not valid JSON"};
  }
  bool rewrite = false;
  std::optional<Hash> hint;
  if (j.is_object() && j.contains("result") && j.contains("command") && j.contains("schema_version")) {
    const auto input = j.value("input", nlohmann::json::object());
    if (input.is_object() && input.contains("source_hash") && input["source_hash"].is_string()) {
      hint = parse_hash(input["source_hash"].get<std::string>());
    }
    j = j["result"];  // a CLI envelope: the result holds the record(s)
    rewrite = true;
  }
  std::vector<nlohmann::json> items;
  if (j.is_array()) {
    items.assign(j.begin(), j.end());
    rewrite = true;
  } else {
    items.push_back(j);
  }
  for (const auto& item : items) {
    const auto kind = json_kind(item);
    if (!kind) {
      return LoadError{ExitCode::Unsupported, "JSON is neither a FrameAnalysis nor a StreamIndex"};
    }
    Record r{*kind, {}, origin, hint};
    std::string error;
    const ovt_status st = io.json_to_fb(rewrite ? item.dump() : text, *kind, r.fb, error);
    if (st != OVT_OK) {
      return LoadError{from_status(st), error.empty() ? std::string(ovt_status_string(st)) : error};
    }
    out.push_back(std::move(r));
  }
  return std::nullopt;
}

// Loads the records of one file; problems become diagnostics (Error: the file is unusable).
ExitCode load_records(const fs::path& p, const RecordIo& io, std::vector<Record>& out, std::vector<Diagnostic>& diags,
                      bool strict) {
  const auto result = [&](const LoadError& e) {
    diags.push_back(Diagnostic{strict ? "Error" : "Warning", "CLI-0009",
                               fmt::format("input '{}': {}", p.string(), e.why), std::nullopt, ""});
    return e.code;
  };
  auto bytes = read_file(p);
  if (!bytes) {
    return result({ExitCode::Io, "cannot be read"});
  }
  if (!inflate_if_zstd(*bytes)) {
    return result({ExitCode::Corrupt, "not a valid zstd frame"});
  }
  const auto first = std::find_if(bytes->begin(), bytes->end(), [](std::uint8_t c) { return std::isspace(c) == 0; });
  if (first != bytes->end() && (*first == '{' || *first == '[')) {
    const auto err = load_json_records(std::string(bytes->begin(), bytes->end()), io, p.string(), out);
    return err ? result(*err) : ExitCode::Ok;
  }
  const std::string id = bytes->size() >= 8 ? std::string(bytes->begin() + 4, bytes->begin() + 8) : std::string();
  if (id == "OVTA" || id == "OVTI") {
    out.push_back(Record{id == "OVTA" ? RecordKind::FrameAnalysis : RecordKind::StreamIndex, std::move(*bytes),
                         p.string(), std::nullopt});
    return ExitCode::Ok;
  }
  return result({ExitCode::Unsupported, "not a recognised record (.json, .fa, .fa.zst, .index.fb)"});
}

std::optional<Hash> parse_hash(std::string_view hex) {
  if (hex.size() != 64) {
    return std::nullopt;
  }
  Hash h{};
  for (std::size_t i = 0; i < 32; ++i) {
    const auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    const int hi = nib(hex[2 * i]);
    const int lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    h[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return h;
}

std::string to_hex_string(const Hash& h) {
  std::string s;
  for (const std::uint8_t b : h) {
    s += fmt::format("{:02x}", b);
  }
  return s;
}

// The hash a record belongs to: StreamIndex.source_hash, else the envelope's input.source_hash, else the prefix
// of a PlaneRef blob id.
std::optional<Hash> record_hash(const Record& r, bool& conflict) {
  const std::optional<Hash>& given = r.hint;
  conflict = false;
  if (r.kind == RecordKind::StreamIndex) {
    const auto* si = flatbuffers::GetRoot<ovt::fb::StreamIndex>(r.fb.data());
    if (si->source_hash() == nullptr || si->source_hash()->size() != 32) {
      return given;
    }
    Hash h{};
    std::copy(si->source_hash()->begin(), si->source_hash()->end(), h.begin());
    conflict = given && *given != h;
    return h;
  }
  if (given) {
    return given;
  }
  const auto* fa = flatbuffers::GetRoot<ovt::fb::FrameAnalysis>(r.fb.data());
  if (fa->planes() != nullptr) {
    for (const auto* p : *fa->planes()) {
      if (p->blob_id() != nullptr) {
        const std::string id = p->blob_id()->str();
        if (const auto h = parse_hash(std::string_view(id).substr(0, std::min<std::size_t>(64, id.size())))) {
          return h;
        }
      }
    }
  }
  return std::nullopt;
}

bool verifies(const Record& r) {
  flatbuffers::Verifier v(r.fb.data(), r.fb.size());
  return r.kind == RecordKind::FrameAnalysis ? ovt::fb::VerifyFrameAnalysisBuffer(v)
                                             : v.VerifyBuffer<ovt::fb::StreamIndex>("OVTI");
}

ovt_status store(const Record& r, const Hash& h) {
  ovt_Cache* cache = nullptr;
  ovt_status st = ovt_cache_open(nullptr, nullptr, &cache);
  if (st != OVT_OK) {
    return st;
  }
  ovt_CacheEntry* entry = nullptr;
  st = ovt_cache_entry(cache, h.data(), &entry);
  if (st == OVT_OK) {
    if (r.kind == RecordKind::StreamIndex) {
      st = ovt_cache_put_index(entry, r.fb.data(), r.fb.size());
    } else {
      const auto* fa = flatbuffers::GetRoot<ovt::fb::FrameAnalysis>(r.fb.data());
      st = ovt_cache_put_frame(entry, static_cast<std::uint32_t>(fa->frame()->id()->decode_index()), r.fb.data(),
                               r.fb.size());
    }
  }
  ovt_cache_entry_release(entry);
  ovt_cache_close(cache);
  return st;
}

struct ImportState {
  ExitCode code = ExitCode::Ok;
  std::set<std::string> touched;
  std::size_t imported = 0;
  std::optional<UsageIssue> usage;
};

// Validates and stores one record; failures go to diags and st.code, a usage problem to st.usage.
void import_record(const Record& r, const RecordIo& io, std::vector<Diagnostic>& diags, ImportState& st) {
  if (!verifies(r)) {
    diags.push_back(error(fmt::format("input '{}': record does not verify", r.origin)));
    st.code = ExitCode::Corrupt;
    return;
  }
  bool conflict = false;
  const auto hash = record_hash(r, conflict);
  if (conflict || !hash) {
    st.usage =
        UsageIssue{"CLI-0001",
                   conflict ? "the envelope's input.source_hash differs from the StreamIndex's source_hash"
                            : fmt::format("'{}' names no source (use the JSON of `ovt analyze -f json`, which carries "
                                          "input.source_hash)",
                                          r.origin),
                   "ovt cache import"};
    return;
  }
  const std::size_t first = diags.size();
  const ovt_status vs = io.validate(r.kind, r.fb, r.kind == RecordKind::FrameAnalysis ? hash->data() : nullptr, diags);
  const bool errors = std::any_of(diags.begin() + static_cast<std::ptrdiff_t>(first), diags.end(),
                                  [](const Diagnostic& d) { return d.severity == "Error" || d.severity == "Fatal"; });
  if (vs != OVT_OK || errors) {  // never write a record that fails the §6.9 rules
    st.code = vs != OVT_OK ? from_status(vs) : ExitCode::Corrupt;
    return;
  }
  if (const ovt_status ss = store(r, *hash); ss != OVT_OK) {
    diags.push_back(error(fmt::format("input '{}': cannot store: {}", r.origin, ovt_status_string(ss))));
    st.code = from_status(ss);
    return;
  }
  st.touched.insert(to_hex_string(*hash));
  ++st.imported;
}

std::vector<fs::path> import_files(const fs::path& root, bool& is_dir) {
  std::error_code ec;
  is_dir = fs::is_directory(root, ec);
  std::vector<fs::path> files;
  if (!is_dir) {
    files.push_back(root);
    return files;
  }
  for (const auto& de : fs::directory_iterator(root, ec)) {
    if (de.is_regular_file()) {
      files.push_back(de.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

CacheOutcome run_import(const CacheArgs& a, const Globals& g, const CacheEnv& env, std::FILE* out) {
  CacheOutcome o;
  if (g.format != Format::Text && g.format != Format::Json && g.format != Format::Fb) {
    o.code = ExitCode::Usage;
    o.usage = check_format(g, "import", false);
    return o;
  }
  if (env.io == nullptr || !env.io->available()) {
    o.code = ExitCode::Unsupported;
    o.diagnostics.push_back(
        error("import needs ovt_json_to_fb and ovt_validate_* (not in this library build yet, #673)"));
    return o;
  }
  bool is_dir = false;
  const std::vector<fs::path> files = import_files(fs::path(a.import_path), is_dir);
  ImportState st;
  ProgressReporter progress(env.err, env.stderr_tty, "import", "files");
  std::size_t done = 0;
  for (const fs::path& f : files) {
    std::vector<Record> records;
    // A file that is no record is an error for a named file, a skipped warning inside a directory.
    const ExitCode lc = load_records(f, *env.io, records, o.diagnostics, !is_dir);
    if (lc != ExitCode::Ok && !is_dir && st.code == ExitCode::Ok) {
      st.code = lc;
    }
    for (const Record& r : records) {
      import_record(r, *env.io, o.diagnostics, st);
      if (st.usage) {
        o.code = ExitCode::Usage;
        o.usage = std::move(st.usage);
        return o;
      }
    }
    ++done;
    if (g.progress && files.size() > 1) {
      progress.update(static_cast<double>(done) / static_cast<double>(files.size()), done, files.size());
    }
    if (cancel_requested()) {
      st.code = ExitCode::Cancelled;
      break;
    }
  }
  progress.finish();
  Report rep;
  if (const ovt_status ls = load_report(rep); ls != OVT_OK) {
    return fail(ls, "cannot read the cache");
  }
  const auto text = [&]() {
    return write_all(out, fmt::format("imported {} record(s) into {} cache entr{}\n", st.imported, st.touched.size(),
                                      st.touched.size() == 1 ? "y" : "ies"));
  };
  // fb: the CliEnvelope only (§12.2.0 table); JSON: the report restricted to the touched entries.
  const ovt::fb::CacheReport* shown = g.format == Format::Fb ? nullptr : rep.root;
  o.code = st.code;
  o.write_failed = !emit(g, out, "cache import", shown, &st.touched, o.diagnostics, st.code, text);
  return o;
}

}  // namespace

void add_cache_command(Cli& cli, CacheArgs& a) {
  CLI::App& root = cli.app();
  a.app = root.add_subcommand("cache", "List, inspect, clear and import the on-disk analysis cache");
  a.app->require_subcommand(1);
  a.app->fallthrough();
  a.list = a.app->add_subcommand(
      "list",
      "One row per (entry, kind). Timestamps come from cache metadata, so the output is exempt from the "
      "byte-identical determinism check (FR-CLI-004)");
  a.stats = a.app->add_subcommand("stats", "Totals, entry count, limit and hit/miss counters");
  a.stats->add_option("--max-size", a.max_size_gib, "Set the cache limit in GiB (persistent)")
      ->check(CLI::PositiveNumber);
  a.clear = a.app->add_subcommand("clear", "Delete cache entries (needs --yes unless asked on a terminal)");
  cli.add_choice(*a.clear, "--kind", a.kind, "Delete only this kind of data", {"index", "analysis", "metrics"});
  a.clear->add_option("--older-than", a.older_than_days, "Only entries last used at least this many days ago")
      ->check(CLI::NonNegativeNumber);
  a.clear->add_option("--source", a.source, "Only the entry of this source file");
  a.clear->add_flag("-y,--yes", a.yes, "Do not ask for confirmation");
  a.clear->add_option("--max-size", a.max_size_gib, "Set the cache limit in GiB (persistent)")
      ->check(CLI::PositiveNumber);
  a.path = a.app->add_subcommand("path", "Print the resolved cache directory");
  a.import = a.app->add_subcommand(
      "import",
      "Import .json exports, .fa/.fa.zst FrameAnalysis, .index.fb StreamIndex records or a directory of them");
  a.import->add_option("path", a.import_path, "File or directory to import")->required();
  for (CLI::App* sub : {a.list, a.stats, a.clear, a.path, a.import}) {
    sub->fallthrough();
  }
}

CacheOutcome run_cache(const CacheArgs& a, const Globals& g, const CacheEnv& env, std::FILE* out) {
  ovt_status st = ovt_cache_set_dir(g.cache_dir.c_str());
  if (st == OVT_OK) {
    st = ovt_cache_set_enabled(1);
  }
  if (st != OVT_OK) {
    return fail(st, "cannot select the cache");
  }
  if (a.list->parsed()) {
    return run_list(g, out, env);
  }
  if (a.stats->parsed()) {
    return run_stats(a, g, out);
  }
  if (a.clear->parsed()) {
    return run_clear(a, g, env, out);
  }
  if (a.path->parsed()) {
    return run_path(g, out);
  }
  return run_import(a, g, env, out);
}

}  // namespace ovt::cli::cmd
