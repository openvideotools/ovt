// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ovt syntax` (chapter 12 §12.2.3, WI-1.504a). Builds the StreamIndex of one track through ovt/index.h and prints
// the trace text of §12.2.3.1:
// the `[ovt] Unit` line of each unit, then ovt_syntax_trace() of its SyntaxNode tree (ovt_stream_unit_syntax: the
// tree of a slice is parsed with the parameter sets active at that unit), then the unit's own diagnostics.
//
// WI-1.504 is split (§16.2.6): part a (this file) ships --trace, which it requires; part b adds the unit list
// (--list, the default), the header trees and the selectors (--unit, --frame, --type, --sei, --parameter-sets,
// --all-units, --depth) with their text and json output; part c adds --hex, --filter,
// --fields, --diagnostics-only, csv and fb output, the `units` alias and the trace_headers golden comparison.

#include "cmd/syntax.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app.h"
#include "ovt/container.h"
#include "ovt/index.h"
#include "ovt/io.h"
#include "ovt/status.h"
#include "ovt/syntax.h"
#include "ovt_container_generated.h"
#include "ovt_model_generated.h"

namespace ovt::cli::cmd {

namespace {

using Bytes = std::vector<std::uint8_t>;

struct FileClose {
  void operator()(ovt_File* f) const noexcept { ovt_file_close(f); }
};
struct ContainerClose {
  void operator()(ovt_Container* c) const noexcept { ovt_container_close(c); }
};
struct StreamClose {
  void operator()(ovt_Stream* s) const noexcept { ovt_stream_close(s); }
};

Bytes take(ovt_Buffer& b) {
  Bytes v;
  if (b.data != nullptr) {
    v.assign(b.data, std::next(b.data, static_cast<std::ptrdiff_t>(b.size)));
  }
  ovt_buffer_free(&b);
  return v;
}

std::string_view sv(const flatbuffers::String* s) {
  return s == nullptr ? std::string_view{} : std::string_view(s->data(), s->size());
}

Diagnostic make_diag(std::string severity, std::string code, std::string message) {
  return Diagnostic{std::move(severity), std::move(code), std::move(message), std::nullopt, ""};
}

// ---- text ----------------------------------------------------------------------------------------------------

std::string unit_line(const fb::Unit& u) {
  const std::uint64_t off = u.file_span() == nullptr ? 0 : u.file_span()->offset();
  const std::uint64_t len = u.file_span() == nullptr ? 0 : u.file_span()->length();
  return fmt::format("Unit {}  type {} ({})  offset 0x{:08X}  length {}  temporal_id {}", u.index(), u.type(),
                     sv(u.type_name()), off, len, u.temporal_id());
}

std::string_view severity_word(fb::Severity s) {
  switch (s) {
    case fb::Severity::Info:
      return "INFO";
    case fb::Severity::Warning:
      return "WARN";
    case fb::Severity::Error:
      return "ERROR";
    case fb::Severity::Fatal:
      return "FATAL";
  }
  return "INFO";
}

bool put(std::FILE* out, const std::string& s) {
  return s.empty() || std::fwrite(s.data(), 1, s.size(), out) == s.size();
}

// --trace: the unit line, then ovt_syntax_trace() of the tree, then the unit's own diagnostics.
bool write_trace(std::FILE* out, ovt_Stream* stream, const fb::StreamIndex& index, std::vector<Diagnostic>& diags) {
  const flatbuffers::uoffset_t count = index.units() == nullptr ? 0U : index.units()->size();
  for (flatbuffers::uoffset_t i = 0; i < count; ++i) {
    const fb::Unit& u = *index.units()->Get(i);
    std::string text = "[ovt] " + unit_line(u) + "\n";
    ovt_Buffer hb{nullptr, 0};
    const ovt_status st = ovt_stream_unit_syntax(stream, i, &hb);
    const Bytes header = take(hb);
    if (st != OVT_OK) {
      diags.push_back(make_diag("Warning", "CLI-0009",
                                fmt::format("unit {}: header not available ({})", i, ovt_status_string(st))));
    } else if (!header.empty()) {
      ovt_Buffer b{nullptr, 0};
      if (ovt_syntax_trace(header.data(), header.size(), &b) == OVT_OK) {
        const Bytes t = take(b);
        text.append(t.begin(), t.end());
      } else {
        diags.push_back(make_diag("Warning", "CLI-0009", fmt::format("unit {}: no trace text", i)));
      }
    }
    // Unit diagnostics that the tree does not already carry (e.g. a read past the end of the unit).
    if (u.diagnostics() != nullptr) {
      for (const fb::Diagnostic* d : *u.diagnostics()) {
        const std::string line =
            fmt::format("[ovt] !! {} {} {}\n", severity_word(d->severity()), sv(d->code()), sv(d->message()));
        if (text.find(line) == std::string::npos) {
          text += line;
        }
      }
    }
    if (!put(out, text)) {
      return false;
    }
  }
  return true;
}

// ---- opening -------------------------------------------------------------------------------------------------

ExitCode open_failure(ovt_status st, const std::string& path, std::vector<Diagnostic>& diags) {
  const std::string reason = st == OVT_E_IO            ? "cannot be read"
                             : st == OVT_E_UNSUPPORTED ? "no unit map for this format or codec"
                             : st == OVT_E_CORRUPT     ? "is corrupt"
                                                       : std::string(ovt_status_string(st));
  diags.push_back(make_diag("Error", "CLI-0009", fmt::format("input '{}': {}", path, reason)));
  return from_status(st);
}

int first_video_track(ovt_Container* c) {
  const int count = ovt_container_track_count(c);
  for (int i = 0; i < count; ++i) {
    ovt_Buffer b{nullptr, 0};
    if (ovt_container_track_info(c, i, &b) != OVT_OK) {
      ovt_buffer_free(&b);
      continue;
    }
    const Bytes info = take(b);
    if (flatbuffers::GetRoot<fb::TrackInfo>(info.data())->handler() == fb::HandlerType::Video) {
      return i;
    }
  }
  return 0;
}

}  // namespace

void add_syntax_command(Cli& cli, SyntaxArgs& args) {
  args.app = cli.app().add_subcommand("syntax", "Bitstream syntax: unit list, header trees with bit spans, trace");
  CLI::App& a = *args.app;
  a.add_option("file", args.file, "Input file")->required();
  a.add_option("-t,--track", args.track, "Track index as listed by `ovt info` (default: first video track)");
  // WI-1.504a: --trace is required until part b adds the unit list (the default) and the header trees.
  a.add_flag("--trace", args.trace, "Print the trace_headers-like text (implies --format text)")->required();
}

SyntaxOutcome run_syntax(const SyntaxArgs& args, std::FILE* out) {
  SyntaxOutcome o;
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
  ovt_Stream* s_raw = nullptr;
  const int track = args.track >= 0 ? args.track : first_video_track(container.get());
  if (const ovt_status st = ovt_stream_open(container.get(), track, &s_raw); st != OVT_OK) {
    o.code = open_failure(st, args.file, o.diagnostics);
    return o;
  }
  const std::unique_ptr<ovt_Stream, StreamClose> stream(s_raw);
  ovt_Buffer ib{nullptr, 0};
  if (const ovt_status st = ovt_stream_build_index(stream.get(), nullptr, &ib); st != OVT_OK) {
    static_cast<void>(take(ib));
    o.code = open_failure(st, args.file, o.diagnostics);
    return o;
  }
  const Bytes index_buf = take(ib);
  const fb::StreamIndex& index = *flatbuffers::GetRoot<fb::StreamIndex>(index_buf.data());

  o.write_failed = !write_trace(out, stream.get(), index, o.diagnostics);
  return o;
}

}  // namespace ovt::cli::cmd
