// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt probe` (chapter 12 §12.2.2), WI-1.503 parts a and b: the box tree with every decoded field (steps 1, 2, 4, 5
// and 6: --expand, the expansion driver, the text tree, the flattened CSV and --offset), the --tracks and --fragments
// views, --bytes and --list-diagnostics, in json, text, csv and fb. The other views (--samples, --hdr, ...) are part c.
//
// JSON `result` (#829): with no view flag, the ContainerTree (`format`, `root`, `diagnostics`), written box by box (no
// DOM); with a view flag, the image of the ContainerRoot that -f fb writes (`tree` always, with `format`; `tree.root`
// only with --boxes; `tree.tracks` with --tracks; the view vectors of the flags given). --bytes (`{offset, length,
// data}`) and --list-diagnostics (the DiagnosticRegistry table) are standalone results.

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
#include "container/diagnostics.h"
#include "container/hex_dump.h"
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

constexpr std::uint32_t kMaxBytes = 1U << 20U;  // --bytes LEN limit (ovt_container_bytes)

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

std::vector<std::uint8_t> take(ovt_Buffer& b) {
  std::vector<std::uint8_t> v;
  if (b.data != nullptr) {
    v.assign(b.data, std::next(b.data, static_cast<std::ptrdiff_t>(b.size)));
  }
  ovt_buffer_free(&b);
  return v;
}

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

// SPEC-ASSUMPTION(#829): a view the core cannot produce is left out with one CLI-0012 Info naming it.
Diagnostic not_available(std::string_view view, std::string_view reason) {
  return diag("Info", "CLI-0012", fmt::format("{} is not available: {}", view, reason));
}

std::optional<std::pair<std::uint64_t, std::uint32_t>> parse_bytes(std::string_view s) {
  const std::size_t colon = s.find(':');
  std::uint64_t off = 0;
  std::uint32_t len = 0;
  if (colon == std::string_view::npos || !parse_uint(s.substr(0, colon), off) ||
      !parse_uint(s.substr(colon + 1), len) || len > kMaxBytes) {
    return std::nullopt;
  }
  return std::pair{off, len};
}

std::string base64(std::span<const std::uint8_t> b) {
  static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((b.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < b.size(); i += 3) {
    const std::uint32_t n = (std::uint32_t{b[i]} << 16U) | (i + 1 < b.size() ? std::uint32_t{b[i + 1]} << 8U : 0U) |
                            (i + 2 < b.size() ? std::uint32_t{b[i + 2]} : 0U);
    out += kAlphabet[(n >> 18U) & 63U];
    out += kAlphabet[(n >> 12U) & 63U];
    out += i + 1 < b.size() ? kAlphabet[(n >> 6U) & 63U] : '=';
    out += i + 2 < b.size() ? kAlphabet[n & 63U] : '=';
  }
  return out;
}

bool view_flag(const ProbeArgs& a) {
  return a.tracks || a.fragments;
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
  std::optional<std::uint64_t> offset;            // --offset
  std::set<const fb::BoxNode*> expanded;          // --expand <fourcc list>: the boxes that show fields and children
  bool show_boxes = true;                         // tree.root: always without a view flag, else only with --boxes
  std::vector<std::vector<std::uint8_t>> tracks;  // TrackInfo buffers (every view flag reads them)
  int track = -1;                                 // the track of --fragments
  std::vector<std::uint8_t> fragments;
  std::vector<std::uint8_t> bytes;
  std::uint64_t bytes_offset = 0;
};

const fb::TrackInfo& info(const std::vector<std::uint8_t>& buf) {
  return *flatbuffers::GetRoot<fb::TrackInfo>(buf.data());
}

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

bool contains(const fb::ByteSpan& s, std::uint64_t off) {
  return s.offset() <= off && off - s.offset() < s.length();
}

// With --offset (#835): the selected box is depth 1. Returns the depth of a box relative to the selected one (0 for a
// box outside it); a strict ancestor of the selected offset is depth 0 but always expanded, so the box can be found.
struct OffsetScope {
  std::optional<std::uint64_t> offset;
  int base = 0;                        // file depth of the selected box, 0 while it is not in the tree yet
  const fb::ByteSpan* span = nullptr;  // its file span

  OffsetScope(const Probe& p, std::optional<std::uint64_t> off) : offset(off) {
    if (offset) {
      walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int depth) {
        if (base == 0 && b.file_span() != nullptr && b.file_span()->offset() == *offset) {
          base = depth;
          span = b.file_span();
        }
      });
    }
  }
  [[nodiscard]] bool ancestor(const fb::BoxNode& b) const {
    return offset && contains(*b.file_span(), *offset) && b.file_span()->offset() != *offset;
  }
  [[nodiscard]] int depth(const fb::BoxNode& b, int file_depth) const {
    if (!offset) {
      return file_depth;
    }
    return span != nullptr && contains(*span, b.file_span()->offset()) ? file_depth - base + 1 : 0;
  }
};

// The offsets of the boxes still carrying CNT-0007 (mdat never) that the depth or fourcc-list selector reaches and
// that were not tried yet. A fourcc list that names a box type not in the tree reaches every lazy box, since the type
// may sit inside one. With --offset, the ancestors of the selected box and the boxes below it count.
std::vector<std::uint64_t> expansion_round(const Probe& p, std::set<std::uint64_t>& tried) {
  std::set<std::string_view> present;
  walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int) { present.insert(sv(b.fourcc())); });
  const bool any_missing = std::ranges::any_of(p.expand.fourccs, [&](const auto& cc) { return !present.contains(cc); });
  const OffsetScope scope(p, p.offset);
  std::vector<std::uint64_t> todo;
  walk(p.tree->root(), 1, [&](const fb::BoxNode& b, int file_depth) {
    if (!is_unexpanded(b) || sv(b.fourcc()) == "mdat" || b.file_span() == nullptr) {
      return;
    }
    const int depth = scope.depth(b, file_depth);
    const bool wanted =
        scope.ancestor(b) || (depth > 0 && (p.expand.kind == Expand::Kind::Depth
                                                ? depth <= p.expand.depth
                                                : any_missing || p.expand.fourccs.contains(sv(b.fourcc()))));
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

// ---- derived views (step 3) ----------------------------------------------------------------------------------

// Reads the tracks and the views asked for. An unavailable view adds a CLI-0012 and stays empty.
ovt_status read_views(const ProbeArgs& a, Probe& p, ovt_Job* job, std::vector<Diagnostic>& diags) {
  for (int i = 0; i < ovt_container_track_count(p.c); ++i) {
    ovt_Buffer b{nullptr, 0};
    if (ovt_container_track_info(p.c, i, &b) == OVT_OK) {
      p.tracks.push_back(take(b));
    }
  }
  if (!a.fragments) {
    return OVT_OK;
  }
  if (p.tracks.empty()) {
    diags.push_back(not_available("fragment_timelines", "the file has no tracks"));
    return OVT_OK;
  }
  p.track = a.track.value_or(-1);
  if (p.track < 0) {
    const auto video =
        std::ranges::find_if(p.tracks, [](const auto& t) { return info(t).handler() == fb::HandlerType::Video; });
    p.track = video == p.tracks.end() ? 0 : static_cast<int>(std::distance(p.tracks.begin(), video));
  }
  ovt_Buffer b{nullptr, 0};
  const ovt_status st = ovt_container_fragments(p.c, p.track, job, &b);
  p.fragments = take(b);
  if (st == OVT_E_CANCELLED) {
    return st;
  }
  if (st != OVT_OK) {
    p.fragments.clear();
    diags.push_back(
        not_available("fragment_timelines", fmt::format("ovt_container_fragments returned {}", ovt_status_string(st))));
  }
  return OVT_OK;
}

// ---- json ----------------------------------------------------------------------------------------------------

void write_json_registry(JsonWriter& w) {
  w.begin_object();
  w.key("family");
  w.string("CNT");
  w.key("entries");
  w.begin_array();
  for (const container::DiagInfo& d : container::all_diagnostics()) {
    w.begin_object();  // the Diagnostic table's key order; `span` is unset
    w.key("severity");
    w.string(fb::EnumNameSeverity(static_cast<fb::Severity>(d.severity)));
    w.key("code");
    w.string(d.code);
    w.key("message");
    w.string(d.message);
    w.key("spec_ref");
    w.string(d.spec_ref);
    w.end_object();
  }
  w.end_array();
  w.end_object();
}

void write_json_bytes(JsonWriter& w, const Probe& p) {
  w.begin_object();
  w.key("offset");
  w.unsigned_integer(p.bytes_offset);
  w.key("length");
  w.unsigned_integer(p.bytes.size());
  w.key("data");
  w.string(base64(p.bytes));
  w.end_object();
}

void write_json_tree(JsonWriter& w, const Probe& p) {
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

struct Rows {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
};

Rows track_rows(const Probe& p) {
  Rows r{{"index", "handler", "codec", "codec_string", "coded_w", "coded_h", "timescale", "duration", "sample_count",
          "bitrate_bps", "language", "fragmented", "encrypted"},
         {}};
  for (const auto& buf : p.tracks) {
    const fb::TrackInfo& t = info(buf);
    r.rows.push_back({std::to_string(t.index()), std::string(sv(t.handler_fourcc())), fb::EnumNameCodec(t.codec()),
                      std::string(sv(t.codec_string())), std::to_string(t.coded_width()),
                      std::to_string(t.coded_height()), std::to_string(t.timescale()), std::to_string(t.duration()),
                      std::to_string(t.sample_count()), std::to_string(t.bitrate_avg_bps()),
                      std::string(sv(t.language())), std::string(CsvWriter::boolean(t.is_fragmented())),
                      std::string(CsvWriter::boolean(t.is_encrypted()))});
  }
  return r;
}

// One row per moof (AT1). earliest_pts is the first traf's decode start (tfdt, else the computed one).
Rows fragment_rows(const Probe& p) {
  Rows r{{"sequence_number", "moof_offset", "moof_size", "base_data_offset", "earliest_pts", "duration", "sample_count",
          "data_offset", "data_size"},
         {}};
  const auto* tl = p.fragments.empty() ? nullptr : flatbuffers::GetRoot<fb::FragmentTimeline>(p.fragments.data());
  if (tl == nullptr || tl->fragments() == nullptr) {
    return r;
  }
  for (const fb::FragmentInfo* f : *tl->fragments()) {
    std::uint64_t duration = 0;
    std::uint64_t count = 0;
    const fb::TrafInfo* first = f->trafs() == nullptr || f->trafs()->empty() ? nullptr : f->trafs()->Get(0);
    for (flatbuffers::uoffset_t i = 0; f->trafs() != nullptr && i < f->trafs()->size(); ++i) {
      const auto* truns = f->trafs()->Get(i)->truns();
      for (flatbuffers::uoffset_t j = 0; truns != nullptr && j < truns->size(); ++j) {
        duration += truns->Get(j)->duration_sum();
        count += truns->Get(j)->sample_count();
      }
    }
    r.rows.push_back(
        {std::to_string(f->sequence_number()), std::to_string(f->moof_offset()), std::to_string(f->moof_size()),
         first == nullptr ? "" : std::to_string(first->base_data_offset()),
         first == nullptr ? "" : std::to_string(first->has_tfdt() ? first->tfdt() : first->computed_dts_start()),
         std::to_string(duration), std::to_string(count), std::to_string(f->mdat_offset()),
         std::to_string(f->mdat_size())});
  }
  return r;
}

Rows registry_rows() {
  Rows r{{"code", "severity", "message", "spec_ref"}, {}};
  for (const container::DiagInfo& d : container::all_diagnostics()) {
    r.rows.push_back({std::string(d.code), fb::EnumNameSeverity(static_cast<fb::Severity>(d.severity)),
                      std::string(d.message), std::string(d.spec_ref)});
  }
  return r;
}

// The one table a `-f csv` run prints (check_probe_args allows at most one), else nullopt for the flattened tree.
std::optional<Rows> csv_table(const ProbeArgs& a, const Probe& p) {
  if (a.list_diagnostics) {
    return registry_rows();
  }
  if (a.tracks) {
    return track_rows(p);
  }
  if (a.fragments) {
    return fragment_rows(p);
  }
  return std::nullopt;
}

bool write_csv(std::FILE* out, const ProbeArgs& a, const Probe& p) {
  CsvWriter csv(out);
  if (const auto t = csv_table(a, p)) {
    csv.row(t->header);
    for (const auto& row : t->rows) {
      csv.row(row);
    }
  } else {
    flatten_box_tree(csv, p.boxes, make_filter(p));
  }
  return csv.finish();
}

std::string table_text(const Rows& r, bool color) {
  Table t;
  for (const std::string& h : r.header) {
    t.add_column(h);
  }
  for (const auto& row : r.rows) {
    t.add_row(row);
  }
  return t.render(color);
}

bool write_text(std::FILE* out, const ProbeArgs& a, const Probe& p, bool color) {
  std::string s;
  if (a.list_diagnostics) {
    s = table_text(registry_rows(), color);
  } else if (!a.bytes.empty()) {
    s = container::format_hex_dump(p.bytes_offset, p.bytes);
  } else {
    if (p.show_boxes) {
      s = render_box_tree(p.boxes, make_filter(p));
    }
    const auto section = [&](std::string_view title, const Rows& r) {
      s += fmt::format("{}{}\n{}", s.empty() ? "" : "\n", title, table_text(r, color));
    };
    if (a.tracks) {
      section("tracks", track_rows(p));
    }
    if (a.fragments) {
      section(fmt::format("fragments (track {})", p.track), fragment_rows(p));
    }
  }
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

flatbuffers::DetachedBuffer build_root(const ProbeArgs& a, const Probe& p, const std::string& path, bool prefixed) {
  fb::ContainerRootT root;
  root.schema_version = 1;
  root.tree = std::make_unique<fb::ContainerTreeT>();
  root.tree->format = p.format;
  const BoxFilter filter = make_filter(p);
  if (p.show_boxes) {
    for (const fb::BoxNode* b : p.boxes) {
      root.tree->root.emplace_back(b->UnPack());
      prune(*root.tree->root.back(), *b, 1, filter);
    }
  }
  if (p.tree != nullptr && p.tree->diagnostics() != nullptr) {
    for (const fb::Diagnostic* d : *p.tree->diagnostics()) {
      root.tree->diagnostics.emplace_back(d->UnPack());
    }
  }
  for (const auto& t : p.tracks) {
    root.is_fragmented = root.is_fragmented || info(t).is_fragmented();
    root.is_encrypted = root.is_encrypted || info(t).is_encrypted();
    if (a.tracks) {
      root.tree->tracks.emplace_back(info(t).UnPack());
    }
  }
  if (!p.fragments.empty()) {
    root.fragment_timelines.emplace_back(flatbuffers::GetRoot<fb::FragmentTimeline>(p.fragments.data())->UnPack());
  }
  root.file_size = p.size;
  const std::size_t slash = path.find_last_of("/\\");
  root.file_name = slash == std::string::npos ? path : path.substr(slash + 1);
  flatbuffers::FlatBufferBuilder fbb;
  if (prefixed) {
    fbb.FinishSizePrefixed(fb::ContainerRoot::Pack(fbb, &root), "OVTC");
  } else {
    fbb.Finish(fb::ContainerRoot::Pack(fbb, &root), "OVTC");
  }
  return fbb.Release();
}

void write_json_result(JsonWriter& w, const ProbeArgs& a, const Probe& p, const std::string& path) {
  if (a.list_diagnostics) {
    write_json_registry(w);
  } else if (!a.bytes.empty()) {
    write_json_bytes(w, p);
  } else if (!view_flag(a)) {
    write_json_tree(w, p);
  } else {
    // The ContainerRoot image (#829): the same table -f fb writes, already pruned by the --expand filter.
    const flatbuffers::DetachedBuffer root = build_root(a, p, path, false);
    ContainerJson().write_root(w, root.data(), "ovt.fb.ContainerRoot");
  }
}

bool emit(const ProbeArgs& a, const Globals& g, std::FILE* out, const Envelope& env, const Probe& p) {
  const std::string path = env.input ? env.input->path : std::string();
  if (g.format == Format::Json) {
    return write_json_envelope(out, g.pretty, env, [&](JsonWriter& w) { write_json_result(w, a, p, path); });
  }
  if (g.format == Format::Fb) {
    FbStreamWriter w(out);
    const flatbuffers::DetachedBuffer rec = build_envelope_record(env, 1U);
    const flatbuffers::DetachedBuffer root = build_root(a, p, path, true);
    const bool ok = w.write_record(std::span<const std::uint8_t>(rec.data(), rec.size())) &&
                    w.write_record(std::span<const std::uint8_t>(root.data(), root.size()));
    return w.finish() && ok;
  }
  if (g.format == Format::Csv) {
    return write_csv(out, a, p);
  }
  return write_text(out, a, p, g.color == ColorMode::Always);
}

// The printed top level (all boxes, or the --offset box) and the fourcc-list marks; a usage issue (exit 2) for an
// --offset with no box or a --track past the last track.
std::optional<UsageIssue> select_boxes(const ProbeArgs& a, Probe& p) {
  if (p.tree->root() != nullptr) {
    p.boxes.assign(p.tree->root()->begin(), p.tree->root()->end());
  }
  if (a.offset) {
    const fb::BoxNode* b = find_offset(p, *a.offset);
    if (b == nullptr) {
      return UsageIssue{"CLI-0003", fmt::format("bad selector '{}': no box starts at this offset", *a.offset),
                        "ovt probe"};
    }
    p.boxes = {b};
  }
  if (p.expand.kind == Expand::Kind::List) {
    for (const fb::BoxNode* b : p.boxes) {
      mark_expanded(p, *b);
    }
  }
  if (const int n = ovt_container_track_count(p.c); a.track && *a.track >= n) {
    return UsageIssue{"CLI-0003", fmt::format("bad selector '{}': no such track ({} in the file)", *a.track, n),
                      "ovt probe"};
  }
  return std::nullopt;
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
  app->add_option("file", args.file, "Input file (not needed with --list-diagnostics)");
  app->add_option("--expand", args.expand, "Boxes to expand: all, none, a depth N, or a fourcc list (moov,trak,stsd)");
  app->add_flag("--boxes", args.boxes, "Include the box tree (the default unless a view is asked for)");
  app->add_flag("--tracks", args.tracks, "Include tracks[] (TrackInfo)");
  app->add_flag("--fragments", args.fragments, "Include the FragmentTimeline of --track (one row per moof)");
  app->add_option("--offset", args.offset, "Print only the box at this file offset and its subtree");
  app->add_option("--bytes", args.bytes, "Hex dump of LEN bytes at OFFSET (OFFSET:LEN, LEN <= 1 MiB)");
  app->add_flag("--list-diagnostics", args.list_diagnostics, "Print the container diagnostic registry and exit");
  app->add_option("-t,--track", args.track, "Track index for --fragments (default: the first video track)");
  app->add_flag("--tree", args.tree, "Alias of --boxes --expand all")->group("Aliases");
}

std::optional<UsageIssue> check_probe_args(const ProbeArgs& a, const Globals& g) {
  const auto usage = [](std::string code, std::string msg) {
    return UsageIssue{std::move(code), std::move(msg), "ovt probe"};
  };
  const auto exclusive = [&](std::string_view x, std::string_view y) {
    return usage("CLI-0006", fmt::format("options {} and {} are mutually exclusive", x, y));
  };
  if (a.file.empty() && !a.list_diagnostics) {
    return usage("CLI-0001", "the input file is required for 'ovt probe'");
  }
  if (!a.tree && !parse_expand(a.expand)) {
    return usage("CLI-0001", fmt::format("invalid value '{}' for --expand", a.expand));
  }
  if (!a.bytes.empty() && !parse_bytes(a.bytes)) {
    return usage("CLI-0001", fmt::format("invalid value '{}' for --bytes (OFFSET:LEN, LEN <= {})", a.bytes, kMaxBytes));
  }
  if (a.track && *a.track < 0) {
    return usage("CLI-0003", fmt::format("bad selector '{}': negative track index", *a.track));
  }
  // #829: --bytes and --list-diagnostics are standalone results.
  const std::string_view standalone = a.list_diagnostics ? "--list-diagnostics" : !a.bytes.empty() ? "--bytes" : "";
  if (a.list_diagnostics && !a.bytes.empty()) {
    return exclusive("--list-diagnostics", "--bytes");
  }
  if (!standalone.empty() && view_flag(a)) {
    return exclusive(standalone, a.tracks ? "--tracks" : "--fragments");
  }
  if (!standalone.empty() && g.format == Format::Fb) {
    return exclusive(standalone, "--format fb");
  }
  if (g.format == Format::Csv && a.tracks && a.fragments) {
    return exclusive("--tracks", "--fragments with --format csv");
  }
  if (g.format == Format::Csv && !a.bytes.empty()) {
    return exclusive("--bytes", "--format csv");
  }
  return std::nullopt;
}

ProbeOutcome run_probe(const ProbeArgs& a, const Globals& g, std::FILE* out) {
  ProbeOutcome o;
  Envelope env;
  env.command = "probe";
  env.status = ExitCode::Ok;
  Probe p;
  if (a.list_diagnostics) {
    o.write_failed = !emit(a, g, out, env, p);
    return o;
  }
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

  if (!a.bytes.empty()) {
    const auto [off, len] = parse_bytes(a.bytes).value_or(std::pair<std::uint64_t, std::uint32_t>{});
    ovt_Buffer b{nullptr, 0};
    if (const ovt_status st = ovt_container_bytes(p.c, off, len, &b); st != OVT_OK) {
      return fail(st);
    }
    p.bytes = take(b);
    p.bytes_offset = off;
    o.write_failed = !emit(a, g, out, env, p);
    return o;
  }

  p.expand = a.tree ? Expand{} : parse_expand(a.expand).value_or(Expand{});  // checked by check_probe_args
  p.offset = a.offset;
  p.show_boxes = a.boxes || a.tree || a.offset || !view_flag(a);
  if (const ovt_status st = drive_expansion(p, job.get()); st != OVT_OK) {
    return fail(st);
  }
  if (p.tree == nullptr) {
    return fail(OVT_E_NOMEM);
  }
  if (auto issue = select_boxes(a, p)) {
    o.usage = std::move(issue);
    o.code = ExitCode::Usage;
    return o;
  }
  if (view_flag(a)) {
    if (const ovt_status st = read_views(a, p, job.get(), o.diagnostics); st != OVT_OK) {
      return fail(st);
    }
  }
  env.diagnostics = o.diagnostics;
  o.write_failed = !emit(a, g, out, env, p);
  return o;
}

}  // namespace ovt::cli::cmd
