// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/index.h entry points (chapter 12 §12.3.8, WI-1.121): ovt_stream_open() / ovt_stream_close(),
// ovt_stream_codec(), ovt_stream_build_index(), ovt_stream_index() and ovt_stream_rap_before(). The stream reads
// its units straight from the container's file: an Annex B byte stream (H.264, H.265), a Section 5 or Annex B OBU
// stream, or IVF frames (AV1). WI-1.125 runs the build under its ovt_Job (§7.4.6: cancellation, progress, the partial
// index of OVT_INDEX_ALLOW_PARTIAL) and adds ovt_stream_unit_syntax() (§7.4.5); WI-1.130 adds ovt_stream_unit_bytes().
// The other functions of the block return OVT_E_UNSUPPORTED or -1 until their work items land (the WI-1.121 Produces
// line of chapter 7, issue #642).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "api/buffer.hpp"
#include "api/stream_internal.hpp"
#include "container/byte_service.h"
#include "container/container.hpp"
#include "container/format.hpp"
#include "container/ivf.hpp"
#include "container/obu.hpp"
#include "index/lazy_syntax.h"
#include "index/source_hash.h"
#include "io/ovt_file.hpp"
#include "jobs/job_internal.hpp"
#include "ovt/cxx/cancel.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt/index.h"
#include "ovt_model_generated.h"

namespace {

using ovt::api::Framing;

constexpr uint32_t kFourccAv01 = 0x31305641;        // "AV01", first byte in the low byte
constexpr size_t kSniffWindow = size_t{64} << 10U;  // parameter sets are looked for in the first 64 KiB

constexpr uint32_t kCheckEveryUnits = 256;            // §7.4.6: check_cancel every 256 units ...
constexpr std::chrono::milliseconds kCheckEvery{50};  // ... or 50 ms
constexpr uint32_t kClockEveryUnits = 16;             // how often the 50 ms rule looks at the clock
constexpr uint32_t kKnownIndexFlags = OVT_INDEX_ALLOW_PARTIAL;

namespace fb = ovt::fb;
namespace idx = ovt::index;
namespace syn = ovt::syntax;
using ovt::io::RandomAccessFile;

ovt_status status_of_current_exception() noexcept {
  try {
    throw;
  } catch (const ovt::io::IoError& e) {
    return e.code;
  } catch (const idx::UnsupportedCodec&) {
    return OVT_E_UNSUPPORTED;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

// H.264 or H.265: the format sniffer looks at the first NAL unit only and often cannot tell (§8.6.1 step 8), so
// the stream decides by the first parameter set. An H.264 SPS header byte (nal_unit_type 7) and the first header
// byte of an H.265 VPS / SPS / PPS of layer 0 never coincide.
fb::Codec annexb_codec(const RandomAccessFile& file) {
  const ovt::container::AnnexBCodec guess = ovt::container::sniff_detail(file).annexb_codec;
  if (guess == ovt::container::AnnexBCodec::H264) {
    return fb::Codec::H264;
  }
  if (guess == ovt::container::AnnexBCodec::H265) {
    return fb::Codec::H265;
  }
  fb::Codec codec = fb::Codec::Unknown;
  const auto head = file.view(0, static_cast<size_t>(std::min<uint64_t>(file.size(), kSniffWindow)));
  syn::scan_annexb(head.bytes, 0, syn::kNoContainerSample, [&codec](const syn::ScannedUnit& u) {
    if (codec != fb::Codec::Unknown || u.bytes.size() < 2) {
      return;
    }
    const uint8_t b0 = u.bytes[0];
    const uint8_t b1 = u.bytes[1];
    if ((b0 == 0x40 || b0 == 0x42 || b0 == 0x44) && (b1 & 0xF8U) == 0 && (b1 & 0x07U) != 0) {
      codec = fb::Codec::H265;
    } else if ((b0 & 0x9FU) == 0x07) {
      codec = fb::Codec::H264;
    }
  });
  return codec;
}

// Resolves the stream's codec and framing; OVT_E_UNSUPPORTED for a format without a unit map.
ovt_status resolve(const ovt_Container& container, ovt_Stream& stream) {
  const RandomAccessFile& file = *container.file->file;
  switch (container.format) {
    case ovt::container::Format::AnnexB:
      stream.framing = Framing::AnnexB;
      stream.codec = annexb_codec(file);
      return stream.codec == fb::Codec::Unknown ? OVT_E_UNSUPPORTED : OVT_OK;
    case ovt::container::Format::Obu: {
      const auto head = file.view(0, static_cast<size_t>(std::min<uint64_t>(file.size(), kSniffWindow)));
      const auto detected = ovt::container::obu_detect(head.bytes, file.size());
      if (detected.framing == ovt::container::ObuFraming::NotAv1) {
        return OVT_E_UNSUPPORTED;
      }
      stream.framing =
          detected.framing == ovt::container::ObuFraming::AnnexB ? Framing::ObuAnnexB : Framing::ObuSection5;
      stream.codec = fb::Codec::AV1;
      return OVT_OK;
    }
    case ovt::container::Format::Ivf: {
      const auto ivf = ovt::container::ivf::parse(file);
      if (!ivf || ivf->header.fourcc != kFourccAv01) {
        return OVT_E_UNSUPPORTED;  // VP8 / VP9 have no unit parser
      }
      stream.framing = Framing::Ivf;
      stream.codec = fb::Codec::AV1;
      return OVT_OK;
    }
    default:
      return OVT_E_UNSUPPORTED;  // ISOBMFF and the others: with the work item that maps samples to units
  }
}

idx::UnitInput input_of(const syn::ScannedUnit& u, const idx::SampleTiming* timing) {
  idx::UnitInput in;
  in.file_offset = u.file_offset;
  in.bytes = u.bytes;
  in.prefix_span = u.prefix_span;
  in.sample_index = u.container_sample_index;
  in.timing = timing;
  in.temporal_unit_start = u.temporal_unit_start;
  return in;
}

// The §7.4.6 loop control around the builder: skips the units an earlier partial build already indexed, checks
// the cancel token every 256 units or 50 ms and reports progress in bytes of the file.
class Driver {
 public:
  Driver(idx::StreamIndexBuilder& builder, uint32_t skip, uint64_t total, ovt::jobs::Cancel& cancel,
         const ovt::jobs::Pool::ProgressSink& sink)
      : builder_(builder), skip_(skip), total_(total), cancel_(cancel), sink_(sink), last_(Clock::now()) {}

  void on_unit(const syn::ScannedUnit& u, const idx::SampleTiming* timing) {
    if (skip_ > 0) {
      --skip_;
      return;
    }
    builder_.add_unit(input_of(u, timing));
    ++count_;
    if (count_ % kCheckEveryUnits == 0 || (count_ % kClockEveryUnits == 0 && Clock::now() - last_ >= kCheckEvery)) {
      check(u.file_offset);
    }
  }
  // A scan diagnostic. The cancelled pass reported every diagnostic up to its last unit (the throw happens inside
  // on_unit, after add_unit), so the resumed pass drops those of the units it skips.
  void on_diag(const syn::ScanDiagnostic& d) {
    if (skip_ == 0) {
      builder_.add_scan_diagnostic(d);
    }
  }

  // After the scan: the last chance to observe a cancellation before the index is finished.
  void end() {
    scan_complete_ = true;
    cancel_.throw_if_requested();
    sink_(ovt::jobs::Progress{total_, total_, "index"});
  }

  // Whether the scan ran to the end (a cancellation observed in end() leaves nothing to rescan).
  [[nodiscard]] bool scan_complete() const noexcept { return scan_complete_; }

 private:
  using Clock = std::chrono::steady_clock;

  void check(uint64_t done) {
    cancel_.throw_if_requested();
    sink_(ovt::jobs::Progress{done, total_, "index"});
    last_ = Clock::now();
  }

  idx::StreamIndexBuilder& builder_;
  uint32_t skip_;
  uint64_t total_;
  ovt::jobs::Cancel& cancel_;
  const ovt::jobs::Pool::ProgressSink& sink_;
  Clock::time_point last_;
  uint32_t count_ = 0;
  bool scan_complete_ = false;
};

// IVF: one sample per frame, pts = dts = the frame's timestamp in scale / rate seconds (chapter 6 §6.2.6). A
// frame cut off by the end of the file is IO-0001; the OBUs that are complete are still indexed.
void feed_ivf(const RandomAccessFile& file, Driver& driver) {
  const auto ivf = ovt::container::ivf::parse(file);
  if (!ivf) {
    return;
  }
  constexpr auto kMax = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  const ovt::container::ivf::Header& h = ivf->header;
  const bool timed = h.timebase_numerator != 0 && h.timebase_denominator != 0 && h.timebase_numerator <= kMax &&
                     h.timebase_denominator <= kMax;
  const auto on_diag = [&driver](const syn::ScanDiagnostic& d) { driver.on_diag(d); };
  for (size_t i = 0; i < ivf->frames.size(); ++i) {
    const ovt::container::ivf::Frame& fr = ivf->frames[i];
    const auto ts = static_cast<int64_t>(std::min<uint64_t>(fr.timestamp, std::numeric_limits<int64_t>::max()));
    const idx::SampleTiming timing{ts, ts, static_cast<int32_t>(h.timebase_numerator),
                                   static_cast<int32_t>(h.timebase_denominator)};
    if (fr.size < fr.declared_size) {
      driver.on_diag({"IO-0001",
                      "unexpected end of file at offset " + std::to_string(file.size()) + " (" +
                          std::to_string(fr.declared_size) + " bytes requested)",
                      fr.header_offset});
    }
    const auto payload = file.view(fr.payload_offset(), fr.size);
    syn::scan_obu_low_overhead(
        payload.bytes, fr.payload_offset(), static_cast<int64_t>(i), syn::ObuSpanKind::LengthDelimited,
        [&driver, &timing, timed](const syn::ScannedUnit& u) { driver.on_unit(u, timed ? &timing : nullptr); },
        on_diag);
  }
}

// §7.4.2 S1–S10 over the stream's units, under `cancel`. A cancellation throws ovt::Cancelled unless
// OVT_INDEX_ALLOW_PARTIAL is set: then the partial index comes back and the builder stays on the stream for the
// next call to resume from (§7.4.6). A build that runs to the end drops any such state.
std::vector<uint8_t> build(ovt_Stream& stream, ovt::jobs::Cancel& cancel, const ovt::jobs::Pool::ProgressSink& sink) {
  const RandomAccessFile& file = *stream.container->file->file;
  std::unique_ptr<ovt::api::ResumeState> resume = std::move(stream.resume);
  if (!resume) {
    resume = std::make_unique<ovt::api::ResumeState>();
    idx::BuilderOptions options;
    options.codec = stream.codec;
    options.lazy_after_frames = stream.lazy_after_frames;
    options.av1_annexb = stream.framing == Framing::ObuAnnexB;
    resume->builder = std::make_unique<idx::StreamIndexBuilder>(options);
  }
  idx::StreamIndexBuilder& builder = *resume->builder;
  Driver driver(builder, resume->units_fed, file.size(), cancel, sink);
  const auto on_unit = [&driver](const syn::ScannedUnit& u) { driver.on_unit(u, nullptr); };
  const auto on_diag = [&driver](const syn::ScanDiagnostic& d) { driver.on_diag(d); };
  try {
    if (resume->scan_complete) {
      driver.end();
      return builder.finish(idx::source_hash(file));
    }
    switch (stream.framing) {
      case Framing::AnnexB:
        syn::scan_annexb(file, on_unit, on_diag);
        break;
      case Framing::ObuSection5:
      case Framing::ObuAnnexB: {
        // One view of the whole stream: an OBU stream beyond what RandomAccessFile can view (2 GiB) is an IoError.
        const auto all = file.view(0, static_cast<size_t>(std::min<uint64_t>(file.size(), SIZE_MAX)));
        if (stream.framing == Framing::ObuAnnexB) {
          syn::scan_obu_annexb(all.bytes, 0, syn::kNoContainerSample, on_unit, on_diag);
        } else {
          syn::scan_obu_low_overhead(all.bytes, 0, syn::kNoContainerSample, syn::ObuSpanKind::Section5Stream, on_unit,
                                     on_diag);
        }
        break;
      }
      case Framing::Ivf:
        feed_ivf(file, driver);
        break;
    }
    driver.end();
  } catch (const ovt::Cancelled&) {
    if ((stream.index_flags & OVT_INDEX_ALLOW_PARTIAL) == 0) {
      throw;
    }
    std::vector<uint8_t> partial = builder.finish_partial(idx::source_hash(file));
    resume->units_fed = builder.unit_count();
    resume->scan_complete = driver.scan_complete();
    stream.resume = std::move(resume);
    return partial;
  }
  return builder.finish(idx::source_hash(file));
}

// Installs a freshly built index on the stream and hands the caller its copy.
ovt_status install(ovt_Stream& stream, std::vector<uint8_t> index, ovt_Buffer* out_index) {
  const ovt_status st = ovt::api::make_buffer(index.data(), index.size(), out_index);
  if (st != OVT_OK) {
    return st;
  }
  idx::RapMap raps(*flatbuffers::GetRoot<fb::StreamIndex>(index.data()));
  stream.index = std::move(index);
  stream.raps = std::move(raps);
  stream.built = true;
  return OVT_OK;
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_stream_open(ovt_Container* container, int track_index, ovt_Stream** out) {
  if (out != nullptr) {
    *out = nullptr;
  }
  if (container == nullptr || out == nullptr || container->file == nullptr || container->file->file == nullptr ||
      track_index != 0) {
    return OVT_E_INVALID_ARG;
  }
  try {
    auto stream = std::make_unique<ovt_Stream>();
    stream->container = container;
    const ovt_status st = resolve(*container, *stream);
    if (st != OVT_OK) {
      return st;
    }
    *out = stream.release();
    return OVT_OK;
  } catch (...) {
    return status_of_current_exception();
  }
}

OVT_API uint8_t ovt_stream_codec(const ovt_Stream* stream) {
  return stream == nullptr ? uint8_t{0} : static_cast<uint8_t>(stream->codec);
}

OVT_API ovt_status ovt_stream_set_index_flags(ovt_Stream* stream, uint32_t flags) {
  if (stream == nullptr || (flags & ~kKnownIndexFlags) != 0) {
    return OVT_E_INVALID_ARG;
  }
  stream->index_flags = flags;
  return OVT_OK;
}

OVT_API ovt_status ovt_stream_build_index(ovt_Stream* stream, ovt_Job* job, ovt_Buffer* out_index) {
  if (out_index != nullptr) {
    *out_index = ovt_Buffer{nullptr, 0};
  }
  if (stream == nullptr || out_index == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  // jobs::run maps ovt::Cancelled to OVT_E_CANCELLED, std::bad_alloc to OVT_E_NOMEM and anything else to
  // OVT_E_INTERNAL; the stream's own errors keep their codes.
  return ovt::jobs::run(job, "index", [&](ovt::jobs::Cancel& cancel, const ovt::jobs::Pool::ProgressSink& sink) {
    try {
      return install(*stream, build(*stream, cancel, sink), out_index);
    } catch (const ovt::io::IoError& e) {
      return e.code;
    } catch (const idx::UnsupportedCodec&) {
      return OVT_E_UNSUPPORTED;
    }
  });
}

OVT_API ovt_Buffer ovt_stream_index(ovt_Stream* stream) {
  if (stream == nullptr || !stream->built) {
    return ovt_Buffer{nullptr, 0};
  }
  return ovt_Buffer{stream->index.data(), stream->index.size()};
}

OVT_API ovt_status ovt_stream_index_summary(ovt_Stream* stream, ovt_Buffer* out) {
  return stream == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.125
}

OVT_API ovt_status ovt_stream_unit_bytes(ovt_Stream* stream, uint32_t unit_index, ovt_Buffer* out_raw,
                                         ovt_Buffer* out_rbsp) {
  if (out_raw != nullptr) {
    *out_raw = ovt_Buffer{nullptr, 0};
  }
  if (out_rbsp != nullptr) {
    *out_rbsp = ovt_Buffer{nullptr, 0};
  }
  if (stream == nullptr || !stream->built) {
    return OVT_E_INVALID_ARG;
  }
  try {
    const fb::StreamIndex& index = *flatbuffers::GetRoot<fb::StreamIndex>(stream->index.data());
    if (index.units() == nullptr || unit_index >= index.units()->size()) {
      return OVT_E_INVALID_ARG;
    }
    const fb::ByteSpan* span = index.units()->Get(unit_index)->file_span();
    if (span == nullptr) {
      return OVT_E_CORRUPT;
    }
    ovt::container::UnitCodec codec = ovt::container::UnitCodec::Other;
    switch (stream->codec) {
      case fb::Codec::H264:
        codec = ovt::container::UnitCodec::H264;
        break;
      case fb::Codec::H265:
        codec = ovt::container::UnitCodec::H265;
        break;
      case fb::Codec::AV1:
        codec = ovt::container::UnitCodec::Av1;
        break;
      default:
        break;
    }
    ovt::container::PageCache& cache = stream->container->bytes.cache(*stream->container->file->file);
    // On failure read_unit_bytes leaves the buffers empty; nothing is retained by the stream (D4).
    return ovt::container::read_unit_bytes(cache, codec, {span->offset(), span->length()}, out_raw, out_rbsp);
  } catch (...) {
    return status_of_current_exception();
  }
}

OVT_API ovt_status ovt_stream_unit_syntax(ovt_Stream* stream, uint32_t unit_index, ovt_Buffer* out) {
  if (out != nullptr) {
    *out = ovt_Buffer{nullptr, 0};
  }
  if (stream == nullptr || out == nullptr || !stream->built) {
    return OVT_E_INVALID_ARG;
  }
  try {
    const fb::StreamIndex& index = *flatbuffers::GetRoot<fb::StreamIndex>(stream->index.data());
    if (index.units() == nullptr || unit_index >= index.units()->size()) {
      return OVT_E_INVALID_ARG;
    }
    const std::vector<uint8_t> tree = idx::reparse_unit_syntax(index, *stream->container->file->file, unit_index);
    return tree.empty() ? OVT_OK : ovt::api::make_buffer(tree.data(), tree.size(), out);
  } catch (...) {
    return status_of_current_exception();
  }
}

OVT_API ovt_status ovt_stream_frame_info(ovt_Stream* stream, int decode_index, ovt_Buffer* out) {
  (void)decode_index;
  return stream == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.125
}

OVT_API int ovt_stream_frame_count(const ovt_Stream* stream) {
  (void)stream;
  return -1;  // WI-1.125
}

OVT_API int ovt_stream_display_to_decode(const ovt_Stream* stream, int display_index) {
  (void)stream;
  (void)display_index;
  return -1;  // WI-1.125
}

OVT_API int ovt_stream_decode_to_display(const ovt_Stream* stream, int decode_index) {
  (void)stream;
  (void)decode_index;
  return -1;  // WI-1.125
}

OVT_API int ovt_stream_rap_before(const ovt_Stream* stream, int decode_index) {
  return stream == nullptr || !stream->built ? -1 : stream->raps.rap_before(decode_index);
}

OVT_API ovt_status ovt_stream_color(ovt_Stream* stream, ovt_ColorDescription* out) {
  return stream == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // chapter 10
}

OVT_API ovt_status ovt_stream_extract(ovt_Stream* stream, const uint32_t* unit_indices, size_t count,
                                      const char* to_format, ovt_Buffer* out_bytes) {
  (void)unit_indices;
  (void)count;
  (void)to_format;
  return stream == nullptr || out_bytes == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.130
}

OVT_API ovt_status ovt_stream_min_cvs_units(ovt_Stream* stream, int decode_index, int with_sei, ovt_Buffer* out) {
  (void)decode_index;
  (void)with_sei;
  return stream == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.130
}

OVT_API void ovt_stream_close(ovt_Stream* stream) {
  delete stream;  // NOLINT(cppcoreguidelines-owning-memory): the handle was released to the C caller
}

}  // extern "C"
