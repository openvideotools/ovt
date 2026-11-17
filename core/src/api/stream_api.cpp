// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/index.h entry points (chapter 12 §12.3.8, WI-1.121): ovt_stream_open() / ovt_stream_close(),
// ovt_stream_codec(), ovt_stream_build_index(), ovt_stream_index() and ovt_stream_rap_before(). The stream reads
// its units straight from the container's file: an Annex B byte stream (H.264, H.265), a Section 5 or Annex B OBU
// stream, or IVF frames (AV1). The other functions of the block return OVT_E_UNSUPPORTED or -1 until their work
// items land (the WI-1.121 Produces line of chapter 7, issue #642).

#include <algorithm>
#include <array>
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
#include "container/container.hpp"
#include "container/format.hpp"
#include "container/ivf.hpp"
#include "container/obu.hpp"
#include "index/rap_map.h"
#include "index/source_hash.h"
#include "io/ovt_file.hpp"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt/index.h"
#include "ovt_model_generated.h"

namespace {

enum class Framing : uint8_t { AnnexB, ObuSection5, ObuAnnexB, Ivf };

constexpr uint32_t kFourccAv01 = 0x31305641;        // "AV01", first byte in the low byte
constexpr size_t kSniffWindow = size_t{64} << 10U;  // parameter sets are looked for in the first 64 KiB

}  // namespace

struct ovt_Stream {
  ovt_Container* container = nullptr;  // not owned; outlives the stream
  ovt::fb::Codec codec = ovt::fb::Codec::Unknown;
  Framing framing = Framing::AnnexB;
  std::vector<uint8_t> index;  // the last built StreamIndex (ovt_stream_index())
  ovt::index::RapMap raps;     // §7.4.2 S11
  bool built = false;
};

namespace {

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
  return in;
}

// IVF: one sample per frame, pts = dts = the frame's timestamp in scale / rate seconds (chapter 6 §6.2.6). A
// frame cut off by the end of the file is IO-0001; the OBUs that are complete are still indexed.
void feed_ivf(const RandomAccessFile& file, idx::StreamIndexBuilder& builder) {
  const auto ivf = ovt::container::ivf::parse(file);
  if (!ivf) {
    return;
  }
  constexpr auto kMax = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  const ovt::container::ivf::Header& h = ivf->header;
  const bool timed = h.timebase_numerator != 0 && h.timebase_denominator != 0 && h.timebase_numerator <= kMax &&
                     h.timebase_denominator <= kMax;
  const auto on_diag = [&builder](const syn::ScanDiagnostic& d) { builder.add_scan_diagnostic(d); };
  for (size_t i = 0; i < ivf->frames.size(); ++i) {
    const ovt::container::ivf::Frame& fr = ivf->frames[i];
    const auto ts = static_cast<int64_t>(std::min<uint64_t>(fr.timestamp, std::numeric_limits<int64_t>::max()));
    const idx::SampleTiming timing{ts, ts, static_cast<int32_t>(h.timebase_numerator),
                                   static_cast<int32_t>(h.timebase_denominator)};
    if (fr.size < fr.declared_size) {
      builder.add_scan_diagnostic({"IO-0001",
                                   "unexpected end of file at offset " + std::to_string(file.size()) + " (" +
                                       std::to_string(fr.declared_size) + " bytes requested)",
                                   fr.header_offset});
    }
    const auto payload = file.view(fr.payload_offset(), fr.size);
    syn::scan_obu_low_overhead(
        payload.bytes, fr.payload_offset(), static_cast<int64_t>(i), syn::ObuSpanKind::LengthDelimited,
        [&builder, &timing, timed](const syn::ScannedUnit& u) {
          builder.add_unit(input_of(u, timed ? &timing : nullptr));
        },
        on_diag);
  }
}

// §7.4.2 S1–S10 over the stream's units.
std::vector<uint8_t> build(const ovt_Stream& stream) {
  const RandomAccessFile& file = *stream.container->file->file;
  idx::StreamIndexBuilder builder(idx::BuilderOptions{stream.codec});
  const auto on_unit = [&builder](const syn::ScannedUnit& u) { builder.add_unit(input_of(u, nullptr)); };
  const auto on_diag = [&builder](const syn::ScanDiagnostic& d) { builder.add_scan_diagnostic(d); };
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
      feed_ivf(file, builder);
      break;
  }
  return builder.finish(idx::source_hash(file));
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

OVT_API ovt_status ovt_stream_build_index(ovt_Stream* stream, ovt_Job* job, ovt_Buffer* out_index) {
  (void)job;  // cancellation, progress and the partial index are WI-1.125's
  if (out_index != nullptr) {
    *out_index = ovt_Buffer{nullptr, 0};
  }
  if (stream == nullptr || out_index == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    std::vector<uint8_t> index = build(*stream);
    const ovt_status st = ovt::api::make_buffer(index.data(), index.size(), out_index);
    if (st != OVT_OK) {
      return st;
    }
    idx::RapMap raps(*flatbuffers::GetRoot<fb::StreamIndex>(index.data()));
    stream->index = std::move(index);
    stream->raps = std::move(raps);
    stream->built = true;
    return OVT_OK;
  } catch (...) {
    return status_of_current_exception();
  }
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
  (void)unit_index;
  (void)out_raw;
  (void)out_rbsp;
  return stream == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.130
}

OVT_API ovt_status ovt_stream_unit_syntax(ovt_Stream* stream, uint32_t unit_index, ovt_Buffer* out) {
  (void)unit_index;
  return stream == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.130
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
