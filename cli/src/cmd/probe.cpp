// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt probe` (chapter 12 §12.2.2), WI-1.503 part a: the box tree with every decoded field (steps 1, 2, 4, 5 and 6:
// --expand, the expansion driver, the text tree, the flattened CSV and --offset) in json, text, csv and fb. The
// derived-view flags (--tracks, --fragments, --samples, --hdr, ...), --bytes and --list-diagnostics are parts b and c.
//
// The JSON `result` is the ContainerTree (`format`, `root`, `diagnostics`), written box by box (no DOM).

#include "cmd/probe.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app.h"
#include "output/csv_writer.h"
#include "output/fb_json.h"
#include "output/fb_writer.h"
#include "output/json_writer.h"
#include "output/text_writer.h"
#include "output/tree_flatten.h"
#include "ovt/container.h"
#include "ovt/io.h"
#include "ovt/jobs.h"
#include "ovt/status.h"
#include "ovt_container_generated.h"

namespace ovt::cli::cmd {

namespace {

struct FileClose {
  void operator()(ovt_File* f) const noexcept { ovt_file_close(f); }
};
struct ContainerClose {
  void operator()(ovt_Container* c) const noexcept { ovt_container_close(c); }
};
struct JobFree {
  void operator()(ovt_Job* j) const noexcept {
    set_cancel_hook(nullptr, nullptr);
    ovt_job_free(j);
  }
};

std::string_view sv(const flatbuffers::String* s) {
  return s == nullptr ? std::string_view() : s->string_view();
}

Diagnostic diag(std::string severity, std::string code, std::string message) {
  return Diagnostic{std::move(severity), std::move(code), std::move(message), std::nullopt, ""};
}

template <typename T>
bool parse_uint(std::string_view s, T& out) {
  const auto [p, ec] = std::from_chars(s.data(), std::next(s.data(), static_cast<std::ptrdiff_t>(s.size())), out);
  return ec == std::errc() && p == std::next(s.data(), static_cast<std::ptrdiff_t>(s.size())) && !s.empty();
}

// ---- --expand selector (step 1) ------------------------------------------------------------------------------

// SPEC-ASSUMPTION(#829): a top-level box is depth 1; boxes at depth <= N show fields and children. A fourcc list
// expands the listed boxes and their ancestors.
struct Expand {
  enum class Kind : std::uint8_t { All, Depth, List } kind = Kind::All;
  int depth = 0;
  std::set<std::string, std::less<>> fourccs;
};

std::optional<Expand> parse_expand(std::string_view s) {
  Expand e;
  if (s == "all") {
    return e;
  }
  e.kind = Expand::Kind::Depth;
  if (s == "none") {
    return e;
  }
  if (unsigned n = 0; parse_uint(s, n)) {
    e.depth = static_cast<int>(std::min(n, 1000U));
    return e;
  }
  e.kind = Expand::Kind::List;
  for (std::size_t at = 0; at <= s.size();) {
    const std::size_t comma = std::min(s.find(',', at), s.size());
    std::string cc(s.substr(at, comma - at));
    if (cc.empty() || cc.size() > 4) {
      return std::nullopt;
    }
    cc.resize(4, ' ');  // "url" names `url `
    e.fourccs.insert(std::move(cc));
    at = comma + 1;
  }
  return e;
}

// ---- the probe state ---------------------------------------------------------------------------------------------

struct Probe {
  ovt_Container* c = nullptr;
  std::string format;
  std::uint64_t size = 0;
  const fb::ContainerTree* tree = nullptr;  // valid until the next expand
  BoxList boxes;                            // the printed top level (all, or the --offset box)
  Expand expand;
  std::set<const fb::BoxNode*> expanded;  // --expand <fourcc list>: the boxes that show fields and children
};

bool is_unexpanded(const fb::BoxNode& b) {
  return b.diagnostics() != nullptr &&
         std::ranges::any_of(*b.diagnostics(), [](const fb::Diagnostic* d) { return sv(d->code()) == "CNT-0007"; });
}

template <typename F>
// NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxBoxDepth in the reader
void walk(const flatbuffers::Vector<flatbuffers::Offset<fb::BoxNode>>* v, int depth, const F& f) {
  if (v == nullptr) {
    return;
  }
  for (const fb::BoxNode* b : *v) {
    f(*b, depth);
    walk(b->children(), depth + 1, f);
  }
}

void refresh(Probe& p) {
  const ovt_Buffer b = ovt_container_tree(p.c);
  p.tree = b.data == nullptr ? nullptr : flatbuffers::GetRoot<fb::ContainerTree>(b.data);
}

// The offsets of the boxes still carrying CNT-0007 (mdat never) that the depth or fourcc-list selector reaches and
// that were not tried yet. A fourcc list that names a box type not in the tree reaches every lazy box, since the type
// may sit inside one.
std::vector<std::uint64_t> expansion_round(const Probe& p, std::set<std::uint64_t>& tried) {
  std::set<std::string_view> present;
  walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int) { present.insert(sv(b.fourcc())); });
  const bool any_missing = std::ranges::any_of(p.expand.fourccs, [&](const auto& cc) { return !present.contains(cc); });
  std::vector<std::uint64_t> todo;
  walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int depth) {
    if (!is_unexpanded(b) || sv(b.fourcc()) == "mdat" || b.file_span() == nullptr) {
      return;
    }
    const bool wanted = p.expand.kind == Expand::Kind::Depth ? depth <= p.expand.depth
                                                             : any_missing || p.expand.fourccs.contains(sv(b.fourcc()));
    if (wanted && tried.insert(b.file_span()->offset()).second) {
      todo.push_back(b.file_span()->offset());
    }
  });
  return todo;
}

// Step 2: expansion driver. `all` is ovt_container_expand_all; a depth or a fourcc list expands, round by round, what
// expansion_round() reaches, re-reading the tree once per round (not once per box).
ovt_status drive_expansion(Probe& p, ovt_Job* job) {
  if (p.expand.kind == Expand::Kind::All) {
    const ovt_status st = ovt_container_expand_all(p.c, job);
    refresh(p);
    return st == OVT_E_UNSUPPORTED ? OVT_OK : st;  // formats without lazy boxes
  }
  std::set<std::uint64_t> tried;
  for (refresh(p); p.tree != nullptr; refresh(p)) {
    const std::vector<std::uint64_t> todo = expansion_round(p, tried);
    if (todo.empty()) {
      break;
    }
    for (const std::uint64_t off : todo) {
      if (ovt_job_is_cancelled(job) != 0) {
        return OVT_E_CANCELLED;
      }
      static_cast<void>(ovt_container_expand(p.c, off));  // a box that cannot expand keeps its CNT-0007
    }
  }
  return OVT_OK;
}

// The boxes a fourcc list expands: the listed types and their ancestors.
// NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxBoxDepth in the reader
bool mark_expanded(Probe& p, const fb::BoxNode& b) {
  bool any = p.expand.fourccs.contains(sv(b.fourcc()));
  if (b.children() != nullptr) {
    for (const fb::BoxNode* c : *b.children()) {
      any = mark_expanded(p, *c) || any;
    }
  }
  if (any) {
    p.expanded.insert(&b);
  }
  return any;
}

BoxFilter make_filter(const Probe& p) {
  switch (p.expand.kind) {
    case Expand::Kind::All:
      return {};
    case Expand::Kind::Depth:
      return [d = p.expand.depth](const fb::BoxNode&, int depth) { return BoxShow{depth <= d, depth <= d}; };
    case Expand::Kind::List:
      break;
  }
  return [&p](const fb::BoxNode& b, int) {
    const bool on = p.expanded.contains(&b);
    return BoxShow{on, on};
  };
}

const fb::BoxNode* find_offset(const Probe& p, std::uint64_t off) {
  const fb::BoxNode* found = nullptr;
  walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int) {
    if (found == nullptr && b.file_span() != nullptr && b.file_span()->offset() == off) {
      found = &b;
    }
  });
  return found;
}

// ---- json ----------------------------------------------------------------------------------------------------

void write_json_result(JsonWriter& w, const Probe& p) {
  const ContainerJson printer(make_filter(p));
  w.begin_object();
  w.key("format");
  w.string(p.format);
  w.key("root");
  w.begin_array();
  for (const fb::BoxNode* b : p.boxes) {
    printer.write(w, b, "ovt.fb.BoxNode", 1);
  }
  w.end_array();
  if (p.tree != nullptr && p.tree->diagnostics() != nullptr && !p.tree->diagnostics()->empty()) {
    w.key("diagnostics");
    w.begin_array();
    for (const fb::Diagnostic* d : *p.tree->diagnostics()) {
      printer.write(w, d, "ovt.fb.Diagnostic");
    }
    w.end_array();
  }
  w.end_object();
}

// ---- csv and text (step 5) -----------------------------------------------------------------------------------

bool write_csv(std::FILE* out, const Probe& p) {
  CsvWriter csv(out);
  flatten_box_tree(csv, p.boxes, make_filter(p));
  return csv.finish();
}

bool write_text(std::FILE* out, const Probe& p) {
  const std::string s = render_box_tree(p.boxes, make_filter(p));
  return std::fwrite(s.data(), 1, s.size(), out) == s.size();
}

// ---- fb ------------------------------------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxBoxDepth in the reader
void prune(fb::BoxNodeT& b, const fb::BoxNode& src, int depth, const BoxFilter& filter) {
  const BoxShow show = filter ? filter(src, depth) : BoxShow{};
  if (!show.fields) {
    b.fields.clear();
  }
  if (!show.children) {
    b.children.clear();
    return;
  }
  for (flatbuffers::uoffset_t i = 0; i < b.children.size() && src.children() != nullptr; ++i) {
    prune(*b.children[i], *src.children()->Get(i), depth + 1, filter);
  }
}

flatbuffers::DetachedBuffer build_root(const Probe& p, const std::string& path) {
  fb::ContainerRootT root;
  root.schema_version = 1;
  root.tree = std::make_unique<fb::ContainerTreeT>();
  root.tree->format = p.format;
  const BoxFilter filter = make_filter(p);
  for (const fb::BoxNode* b : p.boxes) {
    root.tree->root.emplace_back(b->UnPack());
    prune(*root.tree->root.back(), *b, 1, filter);
  }
  root.file_size = p.size;
  const std::size_t slash = path.find_last_of("/\\");
  root.file_name = slash == std::string::npos ? path : path.substr(slash + 1);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.FinishSizePrefixed(fb::ContainerRoot::Pack(fbb, &root), "OVTC");
  return fbb.Release();
}

bool emit(const Globals& g, std::FILE* out, const Envelope& env, const Probe& p) {
  if (g.format == Format::Json) {
    return write_json_envelope(out, g.pretty, env, [&](JsonWriter& w) { write_json_result(w, p); });
  }
  if (g.format == Format::Fb) {
    FbStreamWriter w(out);
    const flatbuffers::DetachedBuffer rec = build_envelope_record(env, 1U);
    const flatbuffers::DetachedBuffer root = build_root(p, env.input ? env.input->path : std::string());
    const bool ok = w.write_record(std::span<const std::uint8_t>(rec.data(), rec.size())) &&
                    w.write_record(std::span<const std::uint8_t>(root.data(), root.size()));
    return w.finish() && ok;
  }
  if (g.format == Format::Csv) {
    return write_csv(out, p);
  }
  return write_text(out, p);
}

ExitCode open_failure(ovt_status st, const std::string& path, std::vector<Diagnostic>& diags) {
  const std::string reason = st == OVT_E_IO            ? "cannot be read"
                             : st == OVT_E_UNSUPPORTED ? "format not recognised"
                             : st == OVT_E_CORRUPT     ? "is corrupt"
                                                       : std::string(ovt_status_string(st));
  diags.push_back(diag("Error", "CLI-0009", fmt::format("input '{}': {}", path, reason)));
  return from_status(st);
}

}  // namespace

void add_probe_command(Cli& cli, ProbeArgs& args) {
  CLI::App* app = cli.app().add_subcommand("probe", "Container inspection: box tree, fields and derived views");
  args.app = app;
  app->add_option("file", args.file, "Input file")->required();
  app->add_option("--expand", args.expand, "Boxes to expand: all, none, a depth N, or a fourcc list (moov,trak,stsd)");
  app->add_flag("--boxes", args.boxes, "Include the box tree (the default)");
  app->add_option("--offset", args.offset, "Print only the box at this file offset and its subtree");
  app->add_flag("--tree", args.tree, "Alias of --boxes --expand all")->group("Aliases");
}

std::optional<UsageIssue> check_probe_args(const ProbeArgs& a) {
  if (!a.tree && !parse_expand(a.expand)) {
    return UsageIssue{"CLI-0001", fmt::format("invalid value '{}' for --expand", a.expand), "ovt probe"};
  }
  return std::nullopt;
}

ProbeOutcome run_probe(const ProbeArgs& a, const Globals& g, std::FILE* out) {
  ProbeOutcome o;
  Envelope env;
  env.command = "probe";
  env.status = ExitCode::Ok;
  Probe p;
  ovt_File* file_raw = nullptr;
  if (const ovt_status st = ovt_file_open(a.file.c_str(), &file_raw); st != OVT_OK) {
    o.code = open_failure(st, a.file, o.diagnostics);
    return o;
  }
  const std::unique_ptr<ovt_File, FileClose> file(file_raw);
  ovt_Container* c_raw = nullptr;
  if (const ovt_status st = ovt_container_open(file.get(), &c_raw); st != OVT_OK) {
    o.code = open_failure(st, a.file, o.diagnostics);
    return o;
  }
  const std::unique_ptr<ovt_Container, ContainerClose> container(c_raw);
  p.c = container.get();
  const char* format = ovt_container_format(p.c);
  p.format = format == nullptr ? "" : format;
  p.size = ovt_container_size(p.c);
  env.input = EnvelopeInput{a.file, p.size, {}};

  ovt_Job* job_raw = nullptr;
  if (ovt_job_create(&job_raw) != OVT_OK) {
    o.code = ExitCode::Error;
    return o;
  }
  const std::unique_ptr<ovt_Job, JobFree> job(job_raw);
  set_cancel_hook([](void* j) { ovt_job_cancel(static_cast<ovt_Job*>(j)); }, job.get());
  const auto fail = [&](ovt_status st) {
    o.code = st == OVT_E_CANCELLED ? ExitCode::Cancelled : from_status(st);
    o.diagnostics.push_back(diag("Error", "CLI-0009", fmt::format("input '{}': {}", a.file, ovt_status_string(st))));
    return o;
  };

  p.expand = a.tree ? Expand{} : parse_expand(a.expand).value_or(Expand{});  // checked by check_probe_args
  if (const ovt_status st = drive_expansion(p, job.get()); st != OVT_OK) {
    return fail(st);
  }
  if (p.tree == nullptr) {
    return fail(OVT_E_NOMEM);
  }
  if (p.tree->root() != nullptr) {
    p.boxes.assign(p.tree->root()->begin(), p.tree->root()->end());
  }
  if (a.offset) {
    const fb::BoxNode* b = find_offset(p, *a.offset);
    if (b == nullptr) {
      o.usage = UsageIssue{"CLI-0003", fmt::format("bad selector '{}': no box starts at this offset", *a.offset),
                           "ovt probe"};
      o.code = ExitCode::Usage;
      return o;
    }
    p.boxes = {b};
  }
  if (p.expand.kind == Expand::Kind::List) {
    for (const fb::BoxNode* b : p.boxes) {
      mark_expanded(p, *b);
    }
  }
  env.diagnostics = o.diagnostics;
  o.write_failed = !emit(g, out, env, p);
  return o;
}

}  // namespace ovt::cli::cmd
