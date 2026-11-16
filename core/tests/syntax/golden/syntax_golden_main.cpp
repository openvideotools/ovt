// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_syntax_golden: one test of the header-parser golden suite (chapter 14 §14.4.1, WI-1.572).
//
//   ovt_syntax_golden_run --codec h264|h265|av1 --oracle trace_headers|gstparse --golden <file[.gz]>
//                         --names <cbs_name_map.json> --ignore <trace_headers_ignore.txt> --map <gst_map.json>
//                         [--pictures <n>] (--corpus <id> | --input <file>) [--dump <file>]
//
// Parses the corpus file with OVT's header parsers (cut to the first <n> pictures, the cut the golden was made from)
// and compares OVT's trees with the golden: trace_headers element by element (§14.4.1), gstparse member values
// (§14.4.2). --dump writes OVT's syntax elements as JSON for debugging.
// SPEC-ASSUMPTION(#603): this driver stands in for `ovt syntax` (WI-1.504) until it exists.
// Exit: 0 equal, 1 different, 2 usage or input error, 77 skipped (corpus not fetched and OVT_REQUIRE_CORPORA unset).

#include <fmt/format.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "syntax_golden.h"
#include "test_support/corpus.h"

namespace sg = ovt::test::syntax_golden;

namespace {

// gen_h26x.py CONTAINER_PACKETS: packets copied out of a container before the stream is cut.
constexpr unsigned kContainerPackets = 16;

struct Args {
  std::string codec;
  std::string oracle;
  std::string golden;
  std::string names;
  std::string ignore;
  std::string map;
  std::string corpus;
  std::string input;
  std::string dump;
  unsigned pictures = 0;
};

int usage(std::string_view why) {
  std::cerr << "ovt_syntax_golden_run: " << why
            << "\nusage: ovt_syntax_golden_run --codec h264|h265|av1 --oracle trace_headers|gstparse --golden <file> "
               "--names <json> --ignore <txt> [--map <json>] [--pictures <n>] (--corpus <id> | --input <file>) "
               "[--dump <file>]\n";
  return 2;
}

std::optional<Args> parse_args(std::span<char*> argv) {
  Args a;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argv.size()) {
      return std::nullopt;
    }
    const std::string value = argv[++i];
    if (arg == "--codec") {
      a.codec = value;
    } else if (arg == "--oracle") {
      a.oracle = value;
    } else if (arg == "--golden") {
      a.golden = value;
    } else if (arg == "--names") {
      a.names = value;
    } else if (arg == "--ignore") {
      a.ignore = value;
    } else if (arg == "--map") {
      a.map = value;
    } else if (arg == "--corpus") {
      a.corpus = value;
    } else if (arg == "--input") {
      a.input = value;
    } else if (arg == "--dump") {
      a.dump = value;
    } else if (arg == "--pictures") {
      a.pictures = static_cast<unsigned>(std::stoul(value));
    } else {
      return std::nullopt;
    }
  }
  return a;
}

nlohmann::json to_json(const std::vector<sg::Element>& elements) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& e : elements) {
    out.push_back({{"unit", e.unit},
                   {"name", e.name},
                   {"bit_offset", e.bit_offset},
                   {"bit_length", e.bit_length},
                   {"value", e.value}});
  }
  return out;
}

// The units of the stream as the oracle saw them, parsed by OVT.
std::vector<sg::ParsedUnit> parse_input(const std::filesystem::path& input, sg::Codec codec, unsigned pictures) {
  const std::string text = sg::read_text(input);
  std::vector<uint8_t> stream(text.begin(), text.end());
  if (!sg::looks_like_annexb(stream)) {
    stream = sg::h264_container_to_annexb(input, kContainerPackets);
  }
  const auto all = sg::annexb_units(stream);
  const auto units = sg::cut_to_pictures(all, codec, pictures);
  return sg::parse_h264(units);
}

void print_parse_errors(const std::vector<sg::ParsedUnit>& parsed) {
  for (const auto& u : parsed) {
    if (!u.parse_error) {
      continue;
    }
    for (const auto& d : u.diagnostics) {
      std::cout << fmt::format("unit {} (type {}): parse error {} at bit {}\n", u.index, u.type, d.code, d.bit_offset);
    }
  }
}

int run(const Args& a) {
  const auto codec = sg::codec_from_name(a.codec);
  if (!codec) {
    return usage("unknown --codec " + a.codec);
  }
  const bool trace = a.oracle == "trace_headers";
  if (!trace && a.oracle != "gstparse") {
    return usage("unknown --oracle " + a.oracle);
  }
  if (a.golden.empty() || a.names.empty() || a.ignore.empty() || (!trace && a.map.empty()) ||
      (a.corpus.empty() == a.input.empty())) {
    return usage("missing argument");
  }
  if (*codec != sg::Codec::kH264) {
    return usage("codec not supported yet: " + a.codec);
  }
  std::filesystem::path input = a.input;
  if (!a.corpus.empty()) {
    const auto path = ovt::test::corpus_path(a.corpus);
    if (!path) {
      std::cerr << "corpus file missing: " << a.corpus << " (run tools/fetch_corpora.py)\n";
      return ovt::test::require_corpora() ? 1 : 77;
    }
    input = *path;
  }

  const auto parsed = parse_input(input, *codec, a.pictures);
  const auto ovt = sg::ovt_elements(parsed, sg::read_list(a.ignore), sg::read_known(a.names, *codec));
  if (!a.dump.empty()) {
    std::ofstream(a.dump) << to_json(ovt).dump(1) << "\n";
  }
  const auto golden_doc = nlohmann::json::parse(sg::read_text(a.golden));
  std::vector<std::string> diffs;
  if (trace) {
    const auto golden = sg::golden_elements(golden_doc, sg::read_rename(a.names, *codec));
    diffs = sg::compare_elements(golden, ovt);
    std::cout << fmt::format("{}: {} units parsed, {} golden elements, {} OVT elements\n", input.filename().string(),
                             parsed.size(), golden.size(), ovt.size());
  } else {
    diffs = sg::gst_compare(golden_doc, parsed, sg::read_gst_map(a.map, *codec));
    std::cout << fmt::format("{}: {} units parsed, {} golden units\n", input.filename().string(), parsed.size(),
                             golden_doc.size());
  }
  print_parse_errors(parsed);
  if (diffs.empty()) {
    std::cout << (trace ? "syntax elements equal\n" : "header values equal\n");
    return 0;
  }
  for (const auto& line : diffs) {
    std::cout << line << "\n";
  }
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = parse_args(std::span<char*>(argv, static_cast<size_t>(argc)));
  if (!args) {
    return usage("bad arguments");
  }
  try {
    return run(*args);
  } catch (const std::exception& e) {
    std::cerr << "ovt_syntax_golden_run: " << e.what() << "\n";
    return 2;
  }
}
