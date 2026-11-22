// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt info` (chapter 12 §12.2.1, WI-1.502). Reads container headers only: the TrackInfo views of
// ovt_container_track_info, the top-level box order (fast start), the ftyp brands and the colour view.
//
// SPEC-ASSUMPTION(#783, pass 27): (1) while ovt_container_hdr_metadata (WI-1.223b) or ovt_stream_index_summary
// (WI-1.125) answers anything but OVT_OK, `tracks[].color` / `index_summary` are omitted and one Info diagnostic
// (CLI-0012,
// "'{field}' omitted: {reason}") names the field; the exit status stays 0. (2) chroma_format and the bit depths are not
// in TrackInfo, so they are omitted until the index provides them. (3) input.source_hash is empty: the hash is
// module-internal.

#include "cmd/info.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app.h"
#include "output/fb_writer.h"
#include "output/json_writer.h"
#include "output/text_writer.h"
#include "ovt/color.h"
#include "ovt/container.h"
#include "ovt/index.h"
#include "ovt/io.h"
#include "ovt/status.h"
#include "ovt_container_generated.h"

namespace ovt::cli::cmd {

namespace {

constexpr std::uint32_t kMaxFtypBytes = 4096;

struct FileClose {
  void operator()(ovt_File* f) const noexcept { ovt_file_close(f); }
};
struct ContainerClose {
  void operator()(ovt_Container* c) const noexcept { ovt_container_close(c); }
};
struct StreamClose {
  void operator()(ovt_Stream* s) const noexcept { ovt_stream_close(s); }
};

// A caller-owned ovt_Buffer copied into a vector and released.
std::vector<std::uint8_t> take(ovt_Buffer& b) {
  std::vector<std::uint8_t> v;
  if (b.data != nullptr) {
    v.assign(b.data, std::next(b.data, static_cast<std::ptrdiff_t>(b.size)));
  }
  ovt_buffer_free(&b);
  return v;
}

struct Color {
  ovt_ColorDescription c{};
};

struct Track {
  std::vector<std::uint8_t> buf;
  std::optional<Color> color;

  [[nodiscard]] const fb::TrackInfo& info() const { return *flatbuffers::GetRoot<fb::TrackInfo>(buf.data()); }
};

struct Info {
  std::string format;
  std::vector<std::string> brands;
  double duration_s = 0;
  std::uint64_t size = 0;
  bool fragmented = false;
  bool encrypted = false;
  bool fast_start = false;
  std::vector<Track> tracks;  // the tracks to report (video only unless --all-tracks)
  bool index_summary_requested = false;
};

Diagnostic make_diag(std::string severity, std::string code, std::string message) {
  return Diagnostic{std::move(severity), std::move(code), std::move(message), std::nullopt, ""};
}

std::string str(const flatbuffers::String* s) {
  return s == nullptr ? std::string() : s->str();
}

// ---- container probing -------------------------------------------------------------------------------------

std::vector<std::string> parse_brands(const std::vector<std::uint8_t>& ftyp) {
  std::vector<std::string> out;
  // size(4) type(4) major(4) minor(4) compatible(4 each)
  if (ftyp.size() < 16) {
    return out;
  }
  const auto brand = [&](std::size_t at) {
    return std::string(ftyp.begin() + static_cast<std::ptrdiff_t>(at),
                       ftyp.begin() + static_cast<std::ptrdiff_t>(at + 4));
  };
  out.push_back(brand(8));
  for (std::size_t at = 16; at + 4 <= ftyp.size(); at += 4) {
    out.push_back(brand(at));
  }
  return out;
}

void read_top_level(ovt_Container* c, Info& info) {
  const ovt_Buffer tree_buf = ovt_container_tree(c);
  if (tree_buf.data == nullptr) {
    return;
  }
  const auto* tree = flatbuffers::GetRoot<fb::ContainerTree>(tree_buf.data);
  if (tree->root() == nullptr) {
    return;
  }
  std::optional<std::size_t> moov;
  std::optional<std::size_t> mdat;
  std::size_t pos = 0;
  for (const fb::BoxNode* box : *tree->root()) {
    const std::string cc = str(box->fourcc());
    if (cc == "ftyp" && info.brands.empty() && box->file_span() != nullptr) {
      const auto len = static_cast<std::uint32_t>(std::min<std::uint64_t>(box->file_span()->length(), kMaxFtypBytes));
      ovt_Buffer b{nullptr, 0};
      if (ovt_container_bytes(c, box->file_span()->offset(), len, &b) == OVT_OK) {
        info.brands = parse_brands(take(b));
      }
    } else if (cc == "moov" && !moov) {
      moov = pos;
    } else if (cc == "mdat" && !mdat) {
      mdat = pos;
    }
    ++pos;
  }
  info.fast_start = moov && (!mdat || *moov < *mdat);
}

// The colour view of one track; false when the view is unavailable (reported by the caller).
bool read_color(ovt_Container* c, int index, Color& out) {
  ovt_Buffer b{nullptr, 0};
  out.c = ovt_ColorDescription{};
  const ovt_status st = ovt_container_hdr_metadata(c, index, nullptr, &b, &out.c);
  static_cast<void>(take(b));
  return st == OVT_OK;
}

void collect_tracks(ovt_Container* c, const InfoArgs& args, Info& info, std::vector<Diagnostic>& diags) {
  const int count = ovt_container_track_count(c);
  bool color_missing = false;
  for (int i = 0; i < count; ++i) {
    ovt_Buffer b{nullptr, 0};
    if (ovt_container_track_info(c, i, &b) != OVT_OK) {
      continue;
    }
    Track t;
    t.buf = take(b);
    const fb::TrackInfo& ti = t.info();
    info.duration_s = std::max(info.duration_s, ti.duration_seconds());
    info.fragmented = info.fragmented || ti.is_fragmented();
    info.encrypted = info.encrypted || ti.is_encrypted();
    const bool video = ti.handler() == fb::HandlerType::Video;
    if (!video && !args.all_tracks) {
      continue;
    }
    if (video) {
      Color col;
      if (read_color(c, i, col)) {
        t.color = col;
      } else {
        color_missing = true;
      }
    }
    info.tracks.push_back(std::move(t));
  }
  if (color_missing) {
    diags.push_back(make_diag("Info", "CLI-0012",
                              "'tracks[].color' omitted: the HDR metadata view is not supported by this build"));
  }
}

void request_index_summary(ovt_Container* c, const Info& info, std::vector<Diagnostic>& diags) {
  std::string reason = "the index summary view is not supported by this build";
  if (!info.tracks.empty()) {
    ovt_Stream* raw = nullptr;
    if (ovt_stream_open(c, static_cast<int>(info.tracks.front().info().index()), &raw) == OVT_OK) {
      const std::unique_ptr<ovt_Stream, StreamClose> stream(raw);
      ovt_Buffer b{nullptr, 0};
      if (ovt_stream_index_summary(stream.get(), &b) == OVT_OK) {
        // SPEC-ASSUMPTION(#783): the IndexSummary record is mapped to JSON when WI-1.125 lands.
        static_cast<void>(take(b));
      }
    }
  } else {
    reason = "no video track to index";
  }
  diags.push_back(make_diag("Info", "CLI-0012", "'index_summary' omitted: " + reason));
}

// ---- rendering ---------------------------------------------------------------------------------------------

struct Rect {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t w = 0;
  std::int64_t h = 0;
};

double ratio(const flatbuffers::Vector<std::uint32_t>& v, flatbuffers::uoffset_t n) {
  return v.Get(n + 1) == 0 ? 0.0 : static_cast<double>(v.Get(n)) / static_cast<double>(v.Get(n + 1));
}

Rect coded_rect(const fb::TrackInfo& t) {
  return {0, 0, t.coded_width(), t.coded_height()};
}

// `clap` (cleanApertureWidth/Height/HorizOff/VertOff as fractions) mapped onto the coded size.
Rect crop_rect(const fb::TrackInfo& t) {
  const Rect coded = coded_rect(t);
  const auto* clap = t.clean_aperture();
  if (clap == nullptr || clap->size() < 8) {
    return coded;
  }
  const double w = ratio(*clap, 0);
  const double h = ratio(*clap, 2);
  const double cx = static_cast<double>(coded.w) / 2.0 + ratio(*clap, 4);
  const double cy = static_cast<double>(coded.h) / 2.0 + ratio(*clap, 6);
  return {std::llround(cx - w / 2.0), std::llround(cy - h / 2.0), std::llround(w), std::llround(h)};
}

// sample_count / duration as a reduced fraction of timescale units; empty when unknown.
std::string fps_of(const fb::TrackInfo& t) {
  if (t.sample_count() == 0 || t.duration() == 0 || t.timescale() == 0) {
    return "";
  }
  const std::uint64_t num = t.sample_count() * t.timescale();
  const std::uint64_t den = t.duration();
  const std::uint64_t g = std::gcd(num, den);
  const std::uint64_t n = num / g;
  const std::uint64_t d = den / g;
  return d == 1 ? std::to_string(n) : fmt::format("{}/{}", n, d);
}

void write_rect(JsonWriter& w, std::string_view name, const Rect& r) {
  w.key(name);
  w.begin_object();
  w.key("x");
  w.integer(r.x);
  w.key("y");
  w.integer(r.y);
  w.key("w");
  w.integer(r.w);
  w.key("h");
  w.integer(r.h);
  w.end_object();
}

void write_color(JsonWriter& w, const ovt_ColorDescription& c) {
  w.key("color");
  w.begin_object();
  w.key("primaries");
  w.unsigned_integer(c.primaries);
  w.key("transfer");
  w.unsigned_integer(c.transfer);
  w.key("matrix");
  w.unsigned_integer(c.matrix);
  w.key("full_range");
  w.unsigned_integer(c.full_range);
  w.key("max_cll");
  w.unsigned_integer(c.max_cll);
  w.key("max_fall");
  w.unsigned_integer(c.max_fall);
  w.key("mdcv_max_luminance");
  w.unsigned_integer(c.mdcv_max_luminance);
  w.key("has_hdr10plus");
  w.boolean(c.has_hdr10plus != 0);
  w.key("has_dolby_vision");
  w.boolean(c.has_dolby_vision != 0);
  w.end_object();
}

void write_track(JsonWriter& w, const Track& tr) {
  const fb::TrackInfo& t = tr.info();
  w.begin_object();
  w.key("index");
  w.unsigned_integer(t.index());
  w.key("handler");
  w.string(str(t.handler_fourcc()));
  w.key("codec");
  w.string(fb::EnumNameCodec(t.codec()));
  w.key("codec_string");
  w.string(str(t.codec_string()));
  write_rect(w, "coded_size", coded_rect(t));
  write_rect(w, "crop", crop_rect(t));
  if (const std::string fps = fps_of(t); !fps.empty()) {
    w.key("fps");
    w.string(fps);
  }
  w.key("sample_count");
  w.unsigned_integer(t.sample_count());
  w.key("bitrate_bps");
  w.unsigned_integer(t.bitrate_avg_bps());
  w.key("language");
  w.string(str(t.language()));
  if (tr.color) {
    write_color(w, tr.color->c);
  }
  w.end_object();
}

void write_json_result(JsonWriter& w, const Info& info) {
  w.begin_object();
  w.key("format");
  w.string(info.format);
  w.key("brands");
  w.begin_array();
  for (const std::string& b : info.brands) {
    w.string(b);
  }
  w.end_array();
  w.key("duration_s");
  w.number(info.duration_s);
  w.key("size_bytes");
  w.unsigned_integer(info.size);
  w.key("fragmented");
  w.boolean(info.fragmented);
  w.key("encrypted");
  w.boolean(info.encrypted);
  w.key("fast_start");
  w.boolean(info.fast_start);
  w.key("tracks");
  w.begin_array();
  for (const Track& t : info.tracks) {
    write_track(w, t);
  }
  w.end_array();
  w.end_object();
}

bool put(std::FILE* out, const std::string& s) {
  return std::fwrite(s.data(), 1, s.size(), out) == s.size();
}

bool write_text(std::FILE* out, const Info& info, bool color) {
  std::string flags;
  for (const auto& [on, name] : {std::pair{info.fragmented, "fragmented"}, std::pair{info.encrypted, "encrypted"},
                                 std::pair{info.fast_start, "fast start"}}) {
    if (on) {
      flags += (flags.empty() ? "" : ", ") + std::string(name);
    }
  }
  std::string brands;
  for (const std::string& b : info.brands) {
    brands += (brands.empty() ? "" : " ") + b;
  }
  const std::string head = fmt::format("format    {}\nbrands    {}\nduration  {}\nsize      {}\nflags     {}\n",
                                       info.format, brands.empty() ? "-" : brands, hms(info.duration_s),
                                       si(static_cast<double>(info.size), "B"), flags.empty() ? "-" : flags);
  Table table;
  for (const char* h : {"#", "handler", "codec", "codec string"}) {
    table.add_column(h);
  }
  for (const char* h : {"size", "fps", "samples", "bitrate"}) {
    table.add_column(h, Align::Right);
  }
  table.add_column("lang");
  table.add_column("transfer");
  for (const Track& tr : info.tracks) {
    const fb::TrackInfo& t = tr.info();
    const std::string fps = fps_of(t);
    table.add_row({std::to_string(t.index()), str(t.handler_fourcc()), fb::EnumNameCodec(t.codec()),
                   str(t.codec_string()), fmt::format("{}x{}", t.coded_width(), t.coded_height()),
                   fps.empty() ? "-" : fps, std::to_string(t.sample_count()),
                   t.bitrate_avg_bps() == 0 ? "-" : si(static_cast<double>(t.bitrate_avg_bps()), "bps"),
                   str(t.language()), tr.color ? std::to_string(tr.color->c.transfer) : "-"});
  }
  return put(out, head + (info.tracks.empty() ? "" : "\n")) && (info.tracks.empty() || table.write(out, color));
}

flatbuffers::DetachedBuffer build_root(const Info& info, const std::string& path) {
  fb::ContainerRootT root;
  root.schema_version = 1;
  root.tree = std::make_unique<fb::ContainerTreeT>();
  root.tree->format = info.format;
  for (const Track& t : info.tracks) {
    root.tree->tracks.emplace_back(t.info().UnPack());
  }
  root.file_size = info.size;
  const std::size_t slash = path.find_last_of("/\\");
  root.file_name = slash == std::string::npos ? path : path.substr(slash + 1);
  root.brands = info.brands;
  root.is_fragmented = info.fragmented;
  root.is_encrypted = info.encrypted;
  flatbuffers::FlatBufferBuilder fbb;
  fbb.FinishSizePrefixed(fb::ContainerRoot::Pack(fbb, &root), "OVTC");
  return fbb.Release();
}

bool emit(const Globals& g, std::FILE* out, const Envelope& env, const Info& info, bool color) {
  if (g.format == Format::Json) {
    return write_json_envelope(out, g.pretty, env, [&](JsonWriter& w) { write_json_result(w, info); });
  }
  if (g.format == Format::Fb) {
    FbStreamWriter w(out);
    const flatbuffers::DetachedBuffer rec = build_envelope_record(env, 1U);
    const flatbuffers::DetachedBuffer root = build_root(info, env.input ? env.input->path : std::string());
    const bool ok = w.write_record(std::span<const std::uint8_t>(rec.data(), rec.size())) &&
                    w.write_record(std::span<const std::uint8_t>(root.data(), root.size()));
    return w.finish() && ok;
  }
  return write_text(out, info, color);
}

ExitCode open_failure(ovt_status st, const std::string& path, std::vector<Diagnostic>& diags) {
  const std::string reason = st == OVT_E_IO            ? "cannot be read"
                             : st == OVT_E_UNSUPPORTED ? "format not recognised"
                             : st == OVT_E_CORRUPT     ? "is corrupt"
                                                       : std::string(ovt_status_string(st));
  diags.push_back(make_diag("Error", "CLI-0009", fmt::format("input '{}': {}", path, reason)));
  return from_status(st);
}

}  // namespace

void add_info_command(Cli& cli, InfoArgs& args) {
  args.app = cli.app().add_subcommand("info", "Container summary: tracks, codecs, HDR flags (no pixel decoding)");
  args.app->add_option("file", args.file, "Input file")->required();
  args.app->add_flag("--full", args.full, "Also build the StreamIndex (uses the cache) for exact frame statistics");
  args.app->add_flag("--all-tracks", args.all_tracks, "Include audio, subtitle and metadata tracks");
}

InfoOutcome run_info(const InfoArgs& args, const Globals& g, std::FILE* out) {
  InfoOutcome o;
  ovt_File* file_raw = nullptr;
  if (const ovt_status st = ovt_file_open(args.file.c_str(), &file_raw); st != OVT_OK) {
    o.code = open_failure(st, args.file, o.diagnostics);
    return o;
  }
  const std::unique_ptr<ovt_File, FileClose> file(file_raw);
  ovt_Container* c_raw = nullptr;
  if (const ovt_status st = ovt_container_open(file.get(), &c_raw); st != OVT_OK) {
    o.code = open_failure(st, args.file, o.diagnostics);
    return o;
  }
  const std::unique_ptr<ovt_Container, ContainerClose> container(c_raw);

  Info info;
  const char* format = ovt_container_format(container.get());
  info.format = format == nullptr ? "" : format;
  info.size = ovt_container_size(container.get());
  read_top_level(container.get(), info);
  collect_tracks(container.get(), args, info, o.diagnostics);
  if (args.full) {
    request_index_summary(container.get(), info, o.diagnostics);
  }

  Envelope env;
  env.command = "info";
  env.input = EnvelopeInput{args.file, info.size, {}};
  env.diagnostics = o.diagnostics;
  env.status = ExitCode::Ok;
  o.write_failed = !emit(g, out, env, info, g.color == ColorMode::Always);
  return o;
}

}  // namespace ovt::cli::cmd
