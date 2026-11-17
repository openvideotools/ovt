// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of WI-1.125 part a (chapter 7 §7.4.5, §7.4.6): lazy EPB stripping, lazy header trees and their on-demand
// re-parse through ovt_stream_unit_syntax() (acceptance test 4), and the job wiring of ovt_stream_build_index():
// progress, cancellation (acceptance test 2) and the partial index of OVT_INDEX_ALLOW_PARTIAL with its resumption
// (acceptance test 3), and the allocation audit of §7.4.6 (part b; the performance test is bench_index_h264_1h_1080p).

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "api/stream_internal.hpp"
#include "index/h264_index.h"
#include "index/lazy_syntax.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt/index.h"
#include "ovt/jobs.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
namespace idx = ovt::index;
namespace syn = ovt::syntax;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// A committed seed of the index_builder fuzz target without its selector byte.
Bytes seed(std::string_view name) {
  std::ifstream in(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / name, std::ios::binary);
  REQUIRE(in.good());
  Bytes b{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(b.size() > 1);
  b.erase(b.begin());
  return b;
}

bool is_vcl(fb::Codec codec, uint8_t type) {
  return codec == fb::Codec::H264 ? idx::h264::is_vcl(type)
                                  : (codec == fb::Codec::H265 ? type < 32 : (type == 3 || type == 4 || type == 6));
}

// The Annex B stream with `pad` opaque bytes appended to every VCL unit's slice data, so that the units exceed a
// lazy-stripping chunk. The pattern holds emulation-prevention bytes and no start code; it ends in a non-zero byte.
Bytes pad_vcl(const Bytes& annexb, fb::Codec codec, size_t pad) {
  static constexpr std::array<uint8_t, 9> kPattern = {0xAA, 0x00, 0x00, 0x03, 0x01, 0x55, 0x00, 0x00, 0x03};
  Bytes out;
  syn::scan_annexb(annexb, 0, syn::kNoContainerSample, [&](const syn::ScannedUnit& u) {
    const auto prefix = std::span<const uint8_t>(annexb).subspan(u.prefix_span.offset, u.prefix_span.length);
    out.insert(out.end(), prefix.begin(), prefix.end());
    out.insert(out.end(), u.bytes.begin(), u.bytes.end());
    const uint8_t type = codec == fb::Codec::H264 ? (u.bytes[0] & 0x1FU) : ((u.bytes[0] >> 1U) & 0x3FU);
    if (is_vcl(codec, type)) {
      for (size_t i = 0; i < pad; ++i) {
        out.push_back(kPattern.at(i % kPattern.size()));
      }
    }
  });
  return out;
}

Bytes repeat(const Bytes& b, size_t times) {
  Bytes out;
  out.reserve(b.size() * times);
  for (size_t i = 0; i < times; ++i) {
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

// Feeds an Annex B / OBU stream into a builder and returns the index.
Bytes build_direct(const Bytes& data, fb::Codec codec, const idx::BuilderOptions& options) {
  idx::StreamIndexBuilder builder(options);
  const auto on_unit = [&builder](const syn::ScannedUnit& u) {
    idx::UnitInput in;
    in.file_offset = u.file_offset;
    in.bytes = u.bytes;
    in.prefix_span = u.prefix_span;
    builder.add_unit(in);
  };
  const auto on_diag = [&builder](const syn::ScanDiagnostic& d) { builder.add_scan_diagnostic(d); };
  if (codec == fb::Codec::AV1) {
    syn::scan_obu_low_overhead(data, 0, syn::kNoContainerSample, syn::ObuSpanKind::Section5Stream, on_unit, on_diag);
  } else {
    syn::scan_annexb(data, 0, syn::kNoContainerSample, on_unit, on_diag);
  }
  const std::array<uint8_t, 32> hash{};
  return builder.finish(hash);
}

const fb::StreamIndex& root(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

std::string_view sv(const flatbuffers::String* s) {
  return s == nullptr ? std::string_view{} : s->string_view();
}

size_t count_code(const fb::StreamIndex& si, std::string_view code) {
  if (si.diagnostics() == nullptr) {
    return 0;
  }
  return static_cast<size_t>(std::ranges::count_if(
      *si.diagnostics(), [code](const fb::Diagnostic* d) { return d->code()->string_view() == code; }));
}

// Structural equality of two SyntaxNode trees (the nested table of an index and a standalone buffer).
bool same_tree(const fb::SyntaxNode& a, const fb::SyntaxNode& b) {
  const auto key = [](const fb::SyntaxNode& n) {
    const bool spanned = n.span() != nullptr;
    return std::tuple(sv(n.name()), sv(n.descriptor()), n.kind(), n.uvalue(), n.ivalue(), n.bvalue(), sv(n.svalue()),
                      sv(n.enum_name()), sv(n.derived()), sv(n.spec_ref()), n.array_index(), spanned,
                      spanned ? n.span()->bit_offset() : 0, spanned ? n.span()->bit_length() : 0U,
                      n.children() == nullptr ? 0U : n.children()->size(),
                      n.diagnostics() == nullptr ? 0U : n.diagnostics()->size());
  };
  if (key(a) != key(b)) {
    return false;
  }
  for (uint32_t i = 0; a.diagnostics() != nullptr && i < a.diagnostics()->size(); ++i) {
    const fb::Diagnostic* x = a.diagnostics()->Get(i);
    const fb::Diagnostic* y = b.diagnostics()->Get(i);
    if (x->severity() != y->severity() || sv(x->code()) != sv(y->code()) || sv(x->message()) != sv(y->message())) {
      return false;
    }
  }
  for (uint32_t i = 0; a.children() != nullptr && i < a.children()->size(); ++i) {
    if (!same_tree(*a.children()->Get(i), *b.children()->Get(i))) {
      return false;
    }
  }
  return true;
}

// The frame bookkeeping of two indices is the same (everything but the header trees).
void check_same_frames(const fb::StreamIndex& a, const fb::StreamIndex& b) {
  REQUIRE(a.units()->size() == b.units()->size());
  REQUIRE(a.frames()->size() == b.frames()->size());
  const auto unit_key = [](const fb::Unit* u) {
    const auto* e = u->epb_positions();
    return std::tuple(u->type(), u->frame()->decode_index(), u->frame()->display_index(), u->file_span()->length(),
                      e == nullptr ? Bytes{} : Bytes(e->begin(), e->end()),
                      u->diagnostics() == nullptr ? 0U : u->diagnostics()->size());
  };
  const auto frame_key = [](const fb::FrameInfo* f) {
    return std::tuple(f->type(), f->poc(), f->is_rap(), f->shown(), f->id()->display_index(), f->size_bytes(),
                      f->slice_count(), f->qp_base(), f->coded_size()->w(),
                      std::vector<uint32_t>(f->units()->begin(), f->units()->end()));
  };
  for (uint32_t i = 0; i < a.units()->size(); ++i) {
    CAPTURE(i);
    CHECK(unit_key(a.units()->Get(i)) == unit_key(b.units()->Get(i)));
  }
  for (uint32_t i = 0; i < a.frames()->size(); ++i) {
    CAPTURE(i);
    CHECK(frame_key(a.frames()->Get(i)) == frame_key(b.frames()->Get(i)));
  }
}

// A container and a stream over in-memory bytes; `name` gives the format sniffer its extension hint.
struct Opened {
  Opened(std::span<const uint8_t> bytes, const std::string& name) {
    file.file = std::make_unique<ovt::io::MemoryFile>(bytes, name);
    REQUIRE(ovt_container_open(&file, &container) == OVT_OK);
    REQUIRE(ovt_stream_open(container, 0, &stream) == OVT_OK);
  }
  ~Opened() {
    ovt_stream_close(stream);
    ovt_container_close(container);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;

  // Builds the index under `job` and returns a validated copy (empty when the status is not OVT_OK).
  Bytes build(ovt_Job* job, ovt_status expected = OVT_OK) const {
    ovt_Buffer buf{nullptr, 0};
    const ovt_status st = ovt_stream_build_index(stream, job, &buf);
    CHECK(st == expected);
    // The module objects are linked without ovt_buffer_free(): a caller-owned buffer is a new[] array.
    const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
    if (st != OVT_OK) {
      CHECK(buf.data == nullptr);
      return {};
    }
    const std::span<const uint8_t> bytes(buf.data, buf.size);
    Bytes index(bytes.begin(), bytes.end());
    for (const auto& v : ovt::model::validate_stream_index(index)) {
      FAIL_CHECK(v.rule << ": " << v.message);
    }
    return index;
  }

  [[nodiscard]] Bytes unit_syntax(uint32_t unit, ovt_status expected = OVT_OK) const {
    ovt_Buffer buf{nullptr, 0};
    CHECK(ovt_stream_unit_syntax(stream, unit, &buf) == expected);
    const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
    return buf.data == nullptr ? Bytes{} : Bytes(buf.data, buf.data + buf.size);
  }

  ovt_File file;
  ovt_Container* container = nullptr;
  ovt_Stream* stream = nullptr;
};

struct Job {
  Job() { REQUIRE(ovt_job_create(&job) == OVT_OK); }
  ~Job() { ovt_job_free(job); }
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  Job(Job&&) = delete;
  Job& operator=(Job&&) = delete;
  ovt_Job* job = nullptr;
};

struct Seed {
  const char* name;
  const char* file;
  fb::Codec codec;
};
constexpr std::array<Seed, 4> kSeeds = {{{"seed_2_h264_bframes.bin", "s.264", fb::Codec::H264},
                                         {"seed_3_h265_open_gop.bin", "s.265", fb::Codec::H265},
                                         {"seed_0_altref_gop.bin", "s.obu", fb::Codec::AV1},
                                         {"seed_4_h264_pic_timing.bin", "s.264", fb::Codec::H264}}};

// Whether `pred` holds for `node` or a descendant.
bool any_node(const fb::SyntaxNode& node, const std::function<bool(const fb::SyntaxNode&)>& pred) {
  return pred(node) || (node.children() != nullptr &&
                        std::ranges::any_of(*node.children(), [&pred](const auto* c) { return any_node(*c, pred); }));
}

bool has_node_diag(const fb::SyntaxNode& tree, std::string_view code) {
  return any_node(tree, [code](const fb::SyntaxNode& n) {
    return n.diagnostics() != nullptr && std::ranges::any_of(*n.diagnostics(), [code](const fb::Diagnostic* d) {
             return d->code()->string_view() == code;
           });
  });
}

}  // namespace

// ---- allocation counter -------------------------------------------------------------------------------------
// The replaced global operator new counts while `g_count_allocations` is set (the audit of §7.4.6: no heap
// allocation per unit when recording is off).
namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<size_t> g_allocations{0};
}  // namespace

void* operator new(size_t n) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(n == 0 ? 1 : n);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}
void operator delete(void* p, size_t /*n*/) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

// ---- lazy EPB stripping ----------------------------------------------------------------------------------------

TEST_CASE("lazy_rbsp: chunked stripping equals rbsp_from_nal", "[index][fast]") {
  uint32_t lcg = 12345;
  const auto next = [&lcg]() {
    lcg = (lcg * 1103515245U) + 12345U;
    return static_cast<uint8_t>(lcg >> 16U);
  };
  for (int round = 0; round < 40; ++round) {
    CAPTURE(round);
    Bytes nal(1 + (next() * 23U % 5000U));
    for (uint8_t& b : nal) {
      const uint8_t r = next();
      b = r < 100 ? 0x00 : (r < 140 ? 0x03 : r);  // many zero runs and 0x03 bytes
    }
    const ovt::Rbsp want = ovt::rbsp_from_nal(nal);
    for (const size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{64}, size_t{2048}}) {
      CAPTURE(chunk);
      idx::LazyRbsp lazy;
      lazy.reset(nal);
      for (size_t limit = chunk; !lazy.complete(); limit *= 2) {  // the index doubles the prefix
        lazy.extend(limit);
        CHECK(lazy.stripped_raw() == std::min(limit, nal.size()));
        CHECK(std::equal(lazy.rbsp().begin(), lazy.rbsp().end(), want.rbsp.begin()));
      }
      CHECK(Bytes(lazy.rbsp().begin(), lazy.rbsp().end()) == want.rbsp);
      CHECK(lazy.epb_positions() == want.epb_positions);
      // A prefix plus the memchr scan of the rest: the same positions, the prefix's RBSP bytes only.
      idx::LazyRbsp partial;
      partial.reset(nal);
      partial.extend(chunk);
      partial.scan_rest();
      CHECK(partial.complete());
      CHECK(partial.epb_positions() == want.epb_positions);
      CHECK(std::equal(partial.rbsp().begin(), partial.rbsp().end(), want.rbsp.begin()));
    }
  }
}

// ---- lazy header trees -----------------------------------------------------------------------------------------

TEST_CASE("lazy trees: VCL units beyond the threshold carry no tree; values and EPBs are unchanged", "[index][fast]") {
  for (const Seed& s : kSeeds) {
    CAPTURE(s.name);
    const Bytes data = s.codec == fb::Codec::AV1 ? seed(s.name) : pad_vcl(seed(s.name), s.codec, 3000);
    idx::BuilderOptions eager;
    eager.codec = s.codec;
    idx::BuilderOptions lazy = eager;
    lazy.lazy_after_frames = 3;
    lazy.lazy_strip_chunk = 4;  // every padded slice header runs past the first prefix: the retry path
    const Bytes a = build_direct(data, s.codec, eager);
    const Bytes b = build_direct(data, s.codec, lazy);
    REQUIRE(root(a).frames()->size() >= 6);
    check_same_frames(root(a), root(b));
    size_t without_tree = 0;
    for (uint32_t i = 0; i < root(b).units()->size(); ++i) {
      CAPTURE(i);
      const fb::Unit* u = root(b).units()->Get(i);
      const bool beyond = is_vcl(s.codec, u->type()) && u->frame()->decode_index() >= 3;
      CHECK((u->header() == nullptr) == beyond);
      without_tree += beyond ? 1 : 0;
    }
    CHECK(without_tree >= 3);
  }
}

TEST_CASE("lazy trees: a value-only parse allocates nothing per unit beyond the records", "[index][fast]") {
  // §7.4.6 audit with a counting allocator: once the builder's vectors have grown, a lazily parsed slice costs no
  // heap allocation apart from the records of the unit and its frame (and the amortised growth of their vectors).
  const Bytes once = pad_vcl(seed("seed_2_h264_bframes.bin"), fb::Codec::H264, 3000);
  const Bytes data = repeat(once, 40);
  std::vector<syn::ScannedUnit> units;  // bytes point into `data`
  syn::scan_annexb(data, 0, syn::kNoContainerSample, [&units](const syn::ScannedUnit& u) { units.push_back(u); });
  REQUIRE(units.size() >= 400);
  idx::BuilderOptions options;
  options.codec = fb::Codec::H264;
  options.lazy_after_frames = 0;
  idx::StreamIndexBuilder builder(options);
  const auto add = [&builder](const syn::ScannedUnit& u) {
    idx::UnitInput in;
    in.file_offset = u.file_offset;
    in.bytes = u.bytes;
    in.prefix_span = u.prefix_span;
    builder.add_unit(in);
  };
  const size_t half = units.size() / 2;
  for (size_t i = 0; i < half; ++i) {
    add(units[i]);
  }
  size_t vcl = 0;
  size_t vcl_allocations = 0;
  size_t other_allocations = 0;
  for (size_t i = half; i < units.size(); ++i) {
    const bool is_slice = is_vcl(fb::Codec::H264, units[i].bytes[0] & 0x1FU);
    g_allocations = 0;
    g_count_allocations = true;
    add(units[i]);
    g_count_allocations = false;
    (is_slice ? vcl_allocations : other_allocations) += g_allocations.load();
    vcl += is_slice ? 1 : 0;
  }
  INFO("allocations: " << vcl_allocations << " for " << vcl << " lazy slices, " << other_allocations << " for "
                       << (units.size() - half - vcl) << " non-VCL units with trees");
  // The budget per slice of this stream: the FrameRec's unit list (every slice starts a picture), the Unit record's
  // EPB list (every padded slice holds EPBs) and the slice header's own MMCO list (the B slices carry one), plus the
  // amortised growth of the units and frames vectors. An allocation inside the lazy path itself (an arena node, a
  // whole-unit RBSP copy, a formatted derived text, the boundary rule list) would add one or more per slice.
  CHECK(vcl_allocations <= (3 * vcl) + 16);
  CHECK(other_allocations > vcl_allocations);  // the recorded trees of the parameter sets do allocate
  const std::array<uint8_t, 32> hash{};
  const Bytes index = builder.finish(hash);
  for (uint32_t i = 0; i < root(index).units()->size(); ++i) {
    const fb::Unit* u = root(index).units()->Get(i);
    CHECK((u->header() == nullptr) == is_vcl(fb::Codec::H264, u->type()));
  }
}

// AT4: Given a unit beyond the lazy threshold, When ovt_stream_unit_syntax() is called, Then its tree equals the
// eagerly recorded tree of the same unit (byte-identical FlatBuffer).
TEST_CASE("ovt_stream_unit_syntax: the re-parsed tree equals the eager tree", "[index][api][fast]") {
  for (const Seed& s : kSeeds) {
    CAPTURE(s.name);
    const Bytes data = seed(s.name);
    Opened eager(data, s.file);
    Opened lazy(data, s.file);
    lazy.stream->lazy_after_frames = 2;
    ovt_Buffer buf{nullptr, 0};
    CHECK(ovt_stream_unit_syntax(lazy.stream, 0, &buf) == OVT_E_INVALID_ARG);  // before build_index
    const Bytes a = eager.build(nullptr);
    const Bytes b = lazy.build(nullptr);
    check_same_frames(root(a), root(b));
    const uint32_t count = root(a).units()->size();
    CHECK(ovt_stream_unit_syntax(lazy.stream, count, &buf) == OVT_E_INVALID_ARG);
    size_t reparsed = 0;
    size_t pic_timing = 0;
    for (uint32_t i = 0; i < count; ++i) {
      CAPTURE(i);
      const fb::Unit* u = root(a).units()->Get(i);
      const Bytes from_lazy = lazy.unit_syntax(i);
      const Bytes from_eager = eager.unit_syntax(i);
      CHECK(from_lazy == from_eager);
      REQUIRE(u->header() != nullptr);
      REQUIRE_FALSE(from_lazy.empty());
      const fb::SyntaxNode& tree = *flatbuffers::GetRoot<fb::SyntaxNode>(from_lazy.data());
      CHECK(same_tree(*u->header(), tree));
      reparsed += root(b).units()->Get(i)->header() == nullptr ? 1U : 0U;
      // An H.264 SEI unit: pic_timing takes the SPS of the previous picture (D.2.2), which the replay must provide.
      if (s.codec == fb::Codec::H264 && u->type() == 6) {
        CHECK_FALSE(has_node_diag(tree, "SYN-H264-0031"));
        pic_timing += any_node(tree, [](const fb::SyntaxNode& n) { return sv(n.name()) == "pic_timing"; }) ? 1U : 0U;
      }
    }
    CHECK(reparsed >= 4);
    if (std::string_view(s.name) == "seed_4_h264_pic_timing.bin") {
      CHECK(pic_timing == 6);  // x264 --nal-hrd vbr: one per frame, buffering_period only at the IDR frames
    }
  }
}

// ---- jobs ----------------------------------------------------------------------------------------------------

namespace {

struct ProgressLog {
  std::atomic<int> calls{0};
  std::atomic<uint64_t> last_done{0};
  std::atomic<uint64_t> total{0};
  std::atomic<bool> stage_ok{true};
  std::atomic<bool> cancel_on_first{false};
  ovt_Job* job = nullptr;
};

void on_progress(const ovt_JobProgress* p, void* user) {
  auto* log = static_cast<ProgressLog*>(user);
  log->calls.fetch_add(1);
  log->last_done = p->done;
  log->total = p->total;
  if (std::string_view(p->stage) != "index") {
    log->stage_ok = false;
  }
  if (log->cancel_on_first) {
    ovt_job_cancel(log->job);
  }
}

}  // namespace

TEST_CASE("ovt_stream_build_index: progress reports and a usable job", "[index][api][fast]") {
  const Bytes data = repeat(seed("seed_2_h264_bframes.bin"), 100);
  Opened o(data, "p.264");
  Job j;
  ProgressLog log;
  ovt_job_set_progress_callback(j.job, on_progress, &log);
  const Bytes index = o.build(j.job);
  CHECK(root(index).frames()->size() == 1200);
  CHECK(log.calls.load() >= 1);
  CHECK(log.stage_ok);
  CHECK(log.total.load() == data.size());
  CHECK(log.last_done.load() <= data.size());  // the callback is throttled; the snapshot below has the final report
  ovt_JobProgress p{};
  p.struct_size = sizeof(p);
  ovt_job_progress(j.job, &p);
  CHECK(p.fraction == 1.0);
  CHECK(ovt_job_is_cancelled(j.job) == 0);
}

// AT2: Given a job cancelled after 100 ms, When ovt_stream_build_index returns, Then OVT_E_CANCELLED within 150 ms.
TEST_CASE("ovt_stream_build_index: cancellation is observed within 150 ms", "[index][api][fast]") {
  const Bytes data = repeat(pad_vcl(seed("seed_2_h264_bframes.bin"), fb::Codec::H264, 3000), 2000);  // ~77 MB
  Opened o(data, "c.264");
  Job j;
  std::atomic<Clock::time_point> cancelled_at{Clock::time_point{}};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancelled_at = Clock::now();
    ovt_job_cancel(j.job);
  });
  ovt_Buffer buf{nullptr, 0};
  const ovt_status st = ovt_stream_build_index(o.stream, j.job, &buf);
  const Clock::time_point returned = Clock::now();
  canceller.join();
  const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
  if (st == OVT_OK) {
    WARN("the build finished before the cancel: the machine is faster than the test stream");
    return;
  }
  CHECK(st == OVT_E_CANCELLED);
  CHECK(buf.data == nullptr);
  const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(returned - cancelled_at.load());
  INFO("cancel latency " << latency.count() << " ms");
  CHECK(latency.count() <= 150);
  CHECK(ovt_stream_index(o.stream).data == nullptr);
  // The stream stays usable afterwards: the OVT_INDEX_ALLOW_PARTIAL test builds again after a cancel.
}

// AT3: Given OVT_INDEX_ALLOW_PARTIAL and a cancel, When the partial index is returned, Then it contains only
// complete frames and SYN-GEN-0103; the next build resumes and equals an uninterrupted one.
TEST_CASE("OVT_INDEX_ALLOW_PARTIAL: a cancelled build returns the complete frames and resumes", "[index][api][fast]") {
  // A forbidden byte pattern (scan diagnostic SYN-GEN-0004) ends every copy's last NAL unit: the resumed build
  // rescans the file from its start and must not record the diagnostics of the skipped units again.
  Bytes copy = seed("seed_2_h264_bframes.bin");
  copy.insert(copy.end(), {0x00, 0x00, 0x02, 0xAA});
  const Bytes data = repeat(copy, 300);
  Opened whole(data, "w.264");
  const Bytes full = whole.build(nullptr);
  const uint32_t all_units = root(full).units()->size();
  REQUIRE(root(full).frames()->size() == 3600);
  REQUIRE(count_code(root(full), "SYN-GEN-0004") == 300);

  Opened o(data, "p.264");
  CHECK(ovt_stream_set_index_flags(nullptr, 0) == OVT_E_INVALID_ARG);
  CHECK(ovt_stream_set_index_flags(o.stream, 2) == OVT_E_INVALID_ARG);  // not an ovt_IndexFlags bit
  REQUIRE(ovt_stream_set_index_flags(o.stream, OVT_INDEX_ALLOW_PARTIAL) == OVT_OK);
  Job j;
  ProgressLog log;
  log.job = j.job;
  log.cancel_on_first = true;  // the first progress report (unit 256 at the latest) cancels the job
  ovt_job_set_progress_callback(j.job, on_progress, &log);
  const Bytes partial = o.build(j.job);
  REQUIRE_FALSE(partial.empty());
  const fb::StreamIndex& si = root(partial);
  CHECK(1 == count_code(si, "SYN-GEN-0103"));
  const uint32_t kept = si.units()->size();
  CHECK(kept > 0);
  CHECK(kept < all_units);
  CHECK_FALSE(si.frames()->empty());
  CHECK(si.frames()->size() < 3600);
  bool truncated_message = false;
  for (const fb::Diagnostic* d : *si.diagnostics()) {
    if (d->code()->string_view() == "SYN-GEN-0103") {
      CHECK(d->severity() == fb::Severity::Warning);
      truncated_message = d->message()->string_view() == "index truncated at unit " + std::to_string(kept);
    }
  }
  CHECK(truncated_message);
  // Only complete frames: every frame's units are in the index, every indexed unit's frame exists, and the frames
  // equal the first frames of the uninterrupted index.
  for (uint32_t f = 0; f < si.frames()->size(); ++f) {
    CAPTURE(f);
    const fb::FrameInfo* fi = si.frames()->Get(f);
    for (const uint32_t u : *fi->units()) {
      CHECK(u < kept);
    }
    CHECK(fi->size_bytes() == root(full).frames()->Get(f)->size_bytes());
    CHECK(fi->type() == root(full).frames()->Get(f)->type());
  }
  for (uint32_t u = 0; u < kept; ++u) {
    const int32_t f = si.units()->Get(u)->frame()->decode_index();
    CHECK(f < static_cast<int32_t>(si.frames()->size()));
  }
  CHECK(count_code(si, "SYN-GEN-0004") >= 1);
  CHECK(count_code(si, "SYN-GEN-0004") < 300);

  // The resumed build (a fresh or reset job) returns the whole index, byte for byte.
  ovt_job_set_progress_callback(j.job, nullptr, nullptr);
  ovt_job_reset(j.job);
  const Bytes resumed = o.build(j.job);
  CHECK(count_code(root(resumed), "SYN-GEN-0004") == 300);
  CHECK(resumed == full);
  CHECK(0 == count_code(root(resumed), "SYN-GEN-0103"));
  // And a further build starts over, with the same result.
  CHECK(o.build(nullptr) == full);
  // Without the flag the same cancel yields OVT_E_CANCELLED and keeps the last index.
  REQUIRE(ovt_stream_set_index_flags(o.stream, 0) == OVT_OK);
  ovt_job_reset(j.job);
  ovt_job_set_progress_callback(j.job, on_progress, &log);
  CHECK(o.build(j.job, OVT_E_CANCELLED).empty());
  CHECK(ovt_stream_index(o.stream).size == full.size());
}

// A cancellation observed after the last unit (the scan ran to its end): the resumed build finishes without a
// second scan. The stream holds exactly 256 units, so the first progress report, which cancels, comes with the last.
TEST_CASE("OVT_INDEX_ALLOW_PARTIAL: a cancellation after the last unit resumes to the same index",
          "[index][api][fast]") {
  const Bytes copy = seed("seed_2_h264_bframes.bin");
  const uint32_t per_copy =
      root(build_direct(copy, fb::Codec::H264, idx::BuilderOptions{fb::Codec::H264})).units()->size();
  REQUIRE((per_copy > 0 && per_copy <= 256));
  Bytes data = repeat(copy, 256 / per_copy);
  for (uint32_t i = 0; i < 256 % per_copy; ++i) {
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0});  // access unit delimiter
  }
  Opened o(data, "p.264");
  REQUIRE(ovt_stream_set_index_flags(o.stream, OVT_INDEX_ALLOW_PARTIAL) == OVT_OK);
  Job j;
  ProgressLog log;
  log.job = j.job;
  log.cancel_on_first = true;
  ovt_job_set_progress_callback(j.job, on_progress, &log);
  const Bytes partial = o.build(j.job);
  REQUIRE(1 == count_code(root(partial), "SYN-GEN-0103"));
  ovt_job_set_progress_callback(j.job, nullptr, nullptr);
  ovt_job_reset(j.job);
  const Bytes resumed = o.build(j.job);
  REQUIRE(root(resumed).units()->size() == 256);
  CHECK(0 == count_code(root(resumed), "SYN-GEN-0103"));
  CHECK(resumed == o.build(nullptr));  // a build that starts over
}
