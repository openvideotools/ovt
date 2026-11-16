// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_analysis_de265_gop60_1080p: GOP analysis time of ovt-plugin-de265 through the host (chapter 14 §14.9.3,
// chapter 07 WI-2.168). One iteration is what the GUI does on a cold scrub to the end of a GOP: a fresh
// ovt::analysis::Session (no cache) asks for the FrameAnalysis of decode index 59 with the Reconstruction plane, so the
// host creates a decoder at the RAP (decode index 0), pushes all 60 pictures and post-processes the target (§7.5.3).
//
//   bench_analysis_de265_gop60_1080p                     syn_h265_gop60_1080p, host default threads   (<= 1 500 ms
//                                                        on m1-air-8gb, <= 5 000 ms on pixel-tablet)
//   bench_de265_gop60_threads/<stream>/<n>               thread sweep, no budget (WI-2.168 step 2); <stream> is
//                                                        `wpp` (syn_h265_gop60_1080p) or `nowpp`
//                                                        (syn_h265_gop60_1080p_nowpp), <n> libde265 worker threads
//
// SPEC-ASSUMPTION(#596): syn_h265_gop60_1080p_nowpp is a recipe-26 entry added for WI-2.168 step 1.
// Input: the corpus cache ($OVT_CORPORA_DIR, default build/corpora) entries syn_h265_gop60_1080p (recipe 26, x265
// with its default WPP) and syn_h265_gop60_1080p_nowpp (the same with --no-wpp). Neither stream has tiles. A stream
// that is not in the cache is not registered, so a partial cache runs the rest; the binary prints which were left out.
//
// The StreamIndex the Session needs is built here from the Annex B units (scan_annexb), since the H.265 StreamIndex
// builder (WI-1.121/WI-1.123) is not available yet: a picture starts at a VCL NAL unit with
// first_slice_segment_in_pic_flag, IRAP pictures are the RAPs, VPS/SPS/PPS are the parameter sets, and every other
// unit goes with the picture it precedes (a suffix SEI with the picture before it). The host pushes the same bytes in
// the same order as it would with the real index, so the measured time is the decode and post-processing time.
//
// The time is wall-clock (`real_time`): libde265 decodes on worker threads. Iterations are fixed (the run name then
// carries `/iterations:N`, which tools/perf_compare.py strips) instead of UseRealTime(), whose `/real_time` suffix
// would no longer match the budget row's name.

#include <benchmark/benchmark.h>
#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"
#include "ovt_perf.hpp"
#include "session.hpp"

namespace {

constexpr int kTarget = 59;           // last picture of the first 60-picture GOP
constexpr uint32_t kReconMask = 1U;   // ovt.fb.PlaneStage.Reconstruction
constexpr uint8_t kFirstNonVcl = 32;  // H.265 Table 7-1: VPS_NUT
constexpr uint8_t kVps = 32;
constexpr uint8_t kPps = 34;
constexpr uint8_t kSuffixSei = 40;
constexpr uint8_t kFirstIrap = 16;  // BLA_W_LP
constexpr uint8_t kLastIrap = 23;   // RSV_IRAP_VCL23
constexpr std::array<int, 4> kSweepThreads{1, 2, 4, 8};

struct Stream {
  std::string id;
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  std::vector<uint8_t> index;  // StreamIndex FlatBuffer, not size-prefixed
};

std::filesystem::path corpora_dir() {
  const char* env = std::getenv("OVT_CORPORA_DIR");  // NOLINT(concurrency-mt-unsafe): read before threads start
  return env != nullptr && *env != '\0' ? std::filesystem::path(env) : std::filesystem::path("build/corpora");
}

// The StreamIndex of an H.265 Annex B file (see the header comment for the rules).
std::vector<uint8_t> index_annexb(const ovt::io::RandomAccessFile& file) {
  ovt::fb::StreamIndexT idx;
  idx.codec = ovt::fb::Codec::H265;
  idx.schema_version = 1;
  idx.source_hash.assign(32, 0);
  std::vector<uint32_t> pending;  // units waiting for the next picture
  int display = 0;
  ovt::syntax::scan_annexb(file, [&](const ovt::syntax::ScannedUnit& su) {
    if (su.bytes.size() < 2) {
      return;
    }
    const auto type = static_cast<uint8_t>((su.bytes[0] >> 1U) & 0x3FU);
    const auto u = static_cast<uint32_t>(idx.units.size());
    auto unit = std::make_unique<ovt::fb::UnitT>();
    unit->index = u;
    unit->file_span = std::make_unique<ovt::fb::ByteSpan>(su.file_offset, su.bytes.size());
    unit->prefix_span = std::make_unique<ovt::fb::ByteSpan>(su.prefix_span.offset, su.prefix_span.length);
    unit->type = type;
    unit->frame = std::make_unique<ovt::fb::FrameId>(-1, -1);
    if (type >= kVps && type <= kPps) {
      auto ps = std::make_unique<ovt::fb::UnitT>(*unit);
      ps->file_span = std::make_unique<ovt::fb::ByteSpan>(*unit->file_span);
      ps->prefix_span = std::make_unique<ovt::fb::ByteSpan>(*unit->prefix_span);
      ps->frame = std::make_unique<ovt::fb::FrameId>(-1, -1);
      idx.parameter_sets.push_back(std::move(ps));
    }
    const bool first_slice = type < kFirstNonVcl && su.bytes.size() > 2 && (su.bytes[2] & 0x80U) != 0;
    if (first_slice) {
      const auto d = static_cast<int32_t>(idx.frames.size());
      auto f = std::make_unique<ovt::fb::FrameInfoT>();
      f->id = std::make_unique<ovt::fb::FrameId>(d, display++);
      f->is_rap = type >= kFirstIrap && type <= kLastIrap;
      f->type = f->is_rap ? ovt::fb::FrameType::I : ovt::fb::FrameType::P;
      f->shown = true;
      f->pts = d;
      f->units = std::move(pending);
      pending.clear();
      idx.frames.push_back(std::move(f));
    }
    const bool own_picture = (type < kFirstNonVcl || type == kSuffixSei) && !idx.frames.empty();
    if (own_picture) {
      auto& f = *idx.frames.back();
      f.units.push_back(u);
      f.size_bytes += su.bytes.size();
      unit->frame = std::make_unique<ovt::fb::FrameId>(*f.id);
    } else if (!idx.frames.empty() || type < kVps || type > kPps) {
      pending.push_back(u);  // parameter sets before picture 0 reach the decoder through parameter_sets only
    }
    idx.units.push_back(std::move(unit));
  });
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &idx));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

std::optional<Stream> load(const std::string& id) {
  const std::filesystem::path path = corpora_dir() / id / (id + ".265");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return std::nullopt;
  }
  Stream s;
  s.id = id;
  s.file = ovt::io::RandomAccessFile::open(path.string());
  s.index = index_annexb(*s.file);
  const auto* root = flatbuffers::GetRoot<ovt::fb::StreamIndex>(s.index.data());
  if (root->frames() == nullptr || root->frames()->size() <= static_cast<uint32_t>(kTarget) ||
      !root->frames()->Get(0)->is_rap()) {
    throw std::runtime_error(id + ": needs a RAP at decode index 0 and at least 60 pictures");
  }
  return s;
}

const ovt::analysis::LoadedPlugin& de265_plugin() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  if (plugin == nullptr) {
    throw std::runtime_error("ovt-plugin-de265 not found in " OVT_DE265_PLUGIN_DIR);
  }
  return *plugin;
}

// One cold GOP analysis per iteration with `threads` libde265 worker threads; 0 = the host default
// (SessionOptions::threads = 0: min(4, hardware threads), 2 on Android, §7.5.3 A3).
void run_gop(benchmark::State& state, const Stream& stream, int threads) {
  const ovt::analysis::LoadedPlugin& plugin = de265_plugin();
  std::vector<uint8_t> out;
  for (auto _ : state) {
    ovt::analysis::SessionOptions options;
    options.threads = threads;
    ovt::analysis::Session session(*stream.file, stream.index, plugin, options);
    const ovt_status st = session.frame(kTarget, kReconMask, nullptr, out);
    if (st != OVT_OK) {
      state.SkipWithError("frame(59) failed with status " + std::to_string(static_cast<int>(st)));
      return;
    }
    benchmark::DoNotOptimize(out.data());
  }
  state.SetLabel(stream.id);
  state.counters["frames"] = kTarget + 1;
}

int iterations() {
  return std::max(1, static_cast<int>(ovt::perf::min_time_seconds()));
}

benchmark::internal::Benchmark* configure(benchmark::internal::Benchmark* b) {
  return b->Unit(benchmark::kMillisecond)
      ->Iterations(iterations())
      ->Repetitions(ovt::perf::repetitions())
      ->ReportAggregatesOnly(true);
}

int run(int argc, char** argv) {
  std::vector<std::unique_ptr<Stream>> streams;
  for (const char* id : {"syn_h265_gop60_1080p", "syn_h265_gop60_1080p_nowpp"}) {
    std::optional<Stream> s = load(id);
    if (!s) {
      (void)std::fprintf(stderr,
                         "bench_analysis_de265_gop60_1080p: %s not in the corpus cache %s, its rows are skipped\n", id,
                         corpora_dir().string().c_str());
      streams.push_back(nullptr);
      continue;
    }
    streams.push_back(std::make_unique<Stream>(std::move(*s)));
  }
  if (const Stream* wpp = streams[0].get(); wpp != nullptr) {
    configure(benchmark::RegisterBenchmark("bench_analysis_de265_gop60_1080p",
                                           [wpp](benchmark::State& st) { run_gop(st, *wpp, 0); }));
  }
  const std::array<const char*, 2> names{"wpp", "nowpp"};
  for (std::size_t k = 0; k < streams.size(); ++k) {
    const Stream* s = streams[k].get();
    if (s == nullptr) {
      continue;
    }
    for (const int n : kSweepThreads) {
      const std::string name = std::string("bench_de265_gop60_threads/") + names.at(k) + "/" + std::to_string(n);
      configure(benchmark::RegisterBenchmark(name, [s, n](benchmark::State& st) { run_gop(st, *s, n); }));
    }
  }
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    (void)std::fprintf(stderr, "bench_analysis_de265_gop60_1080p: %s\n", e.what());
    return 2;
  }
}
