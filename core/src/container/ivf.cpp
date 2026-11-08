// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The IVF reader (chapter 08 §8.6.2, WI-1.225). Source of truth: libvpx ivfdec.c / ivfenc.c and libaom
// common/ivfdec.c.

#include "container/ivf.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>

#include "container/tree_fb.hpp"

namespace ovt::container::ivf {

namespace {

constexpr uint32_t fourcc_le(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8U |
         static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16U | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24U;
}

constexpr uint32_t kAv1 = fourcc_le('A', 'V', '0', '1');
constexpr uint32_t kVp9 = fourcc_le('V', 'P', '9', '0');
constexpr uint32_t kVp8 = fourcc_le('V', 'P', '8', '0');
constexpr uint32_t kH264 = fourcc_le('H', '2', '6', '4');

constexpr size_t kSyncPrefix = 4096;  // payload bytes read to decide is_sync

uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | p[1] << 8U);
}
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8U | static_cast<uint32_t>(p[2]) << 16U |
         static_cast<uint32_t>(p[3]) << 24U;
}
uint64_t rd64(const uint8_t* p) {
  return static_cast<uint64_t>(rd32(p)) | static_cast<uint64_t>(rd32(p + 4)) << 32U;
}

std::string fourcc_string(uint32_t fourcc) {
  std::string s(4, ' ');
  for (size_t i = 0; i < 4; ++i) {
    const auto c = static_cast<char>((fourcc >> (8U * i)) & 0xFFU);
    s[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  return s;
}

fb::Codec codec_of(uint32_t fourcc) {
  switch (fourcc) {
    case kAv1:
      return fb::Codec::AV1;
    case kVp9:
      return fb::Codec::VP9;
    case kH264:
      return fb::Codec::H264;
    default:  // VP8 and unknown fourccs: ovt.fb.Codec has no VP8
      return fb::Codec::Unknown;
  }
}

// One OBU of an AV1 Section 5 low-overhead stream (spec §5.3): type and the payload bytes present in `p`.
struct Obu {
  unsigned type = 0;
  std::span<const uint8_t> payload;
  uint64_t declared_size = 0;  // payload size as written (or the rest of `p` without a size field)
};

// Reads the OBU at `pos` and advances past its header; nullopt on a forbidden bit or a truncated header.
std::optional<Obu> read_obu(std::span<const uint8_t> p, size_t& pos) noexcept {
  const uint8_t h = p[pos++];
  if ((h & 0x80U) != 0) {  // obu_forbidden_bit
    return std::nullopt;
  }
  Obu obu;
  obu.type = (h >> 3U) & 15U;
  if (((h >> 2U) & 1U) != 0) {  // obu_extension_flag
    if (pos >= p.size()) {
      return std::nullopt;
    }
    ++pos;
  }
  obu.declared_size = p.size() - pos;  // no size field: the OBU runs to the end of the temporal unit
  if (((h >> 1U) & 1U) != 0) {         // obu_has_size_field: leb128
    obu.declared_size = 0;
    bool done = false;
    for (unsigned i = 0; i < 8 && pos < p.size() && !done; ++i) {
      const uint8_t b = p[pos++];
      obu.declared_size |= static_cast<uint64_t>(b & 0x7FU) << (7U * i);
      done = (b & 0x80U) == 0;
    }
    if (!done) {
      return std::nullopt;
    }
  }
  obu.payload = p.subspan(pos, static_cast<size_t>(std::min<uint64_t>(obu.declared_size, p.size() - pos)));
  return obu;
}

// AV1 (spec §5.3, §5.5, §5.9.2): the first frame header decides.
bool av1_starts_with_key_frame(std::span<const uint8_t> p) noexcept {
  size_t pos = 0;
  bool reduced_still_picture_header = false;
  while (pos < p.size()) {
    const auto obu = read_obu(p, pos);
    if (!obu) {
      return false;
    }
    if (obu->type == 1 && !obu->payload.empty()) {  // OBU_SEQUENCE_HEADER: seq_profile(3) still_picture(1) reduced(1)
      reduced_still_picture_header = ((obu->payload[0] >> 3U) & 1U) != 0;
    } else if (obu->type == 3 || obu->type == 6) {  // OBU_FRAME_HEADER, OBU_FRAME
      if (reduced_still_picture_header) {
        return true;  // frame_type is inferred as KEY_FRAME
      }
      if (obu->payload.empty()) {
        return false;
      }
      const bool show_existing_frame = (obu->payload[0] & 0x80U) != 0;
      const unsigned frame_type = (obu->payload[0] >> 5U) & 3U;  // 0 KEY_FRAME
      return !show_existing_frame && frame_type == 0;
    }
    if (obu->declared_size > p.size() - pos) {
      return false;
    }
    pos += static_cast<size_t>(obu->declared_size);
  }
  return false;
}

// VP9 uncompressed header (spec §6.2): frame_marker(2) profile_low(1) profile_high(1) [reserved(1)]
// show_existing_frame(1) frame_type(1), frame_type 0 = KEY_FRAME.
bool vp9_key_frame(std::span<const uint8_t> p) noexcept {
  if (p.empty() || (p[0] >> 6U) != 2) {
    return false;
  }
  const unsigned profile = (((p[0] >> 4U) & 1U) << 1U) | ((p[0] >> 5U) & 1U);
  const unsigned show_existing_bit = profile == 3 ? 2U : 3U;
  if (((p[0] >> show_existing_bit) & 1U) != 0) {
    return false;
  }
  return ((p[0] >> (show_existing_bit - 1U)) & 1U) == 0;
}

// VP8 frame tag (RFC 6386 §9.1): bit 0 of byte 0 is 0 for a key frame.
bool vp8_key_frame(std::span<const uint8_t> p) noexcept {
  return p.size() >= 3 && (p[0] & 1U) == 0;
}

uint64_t gcd64(uint64_t a, uint64_t b) {
  return std::gcd(a, b);
}

class IvfReader final : public Reader {
 public:
  IvfReader(const io::RandomAccessFile& file, File parsed)
      : file_(file), f_(std::move(parsed)), timing_(timing_of(f_.header)), sync_(f_.frames.size(), kUnknown) {
    materialised_ = std::min<size_t>(f_.frames.size(), kChildWindow);
  }

  [[nodiscard]] int track_count() const noexcept override { return 1; }

  [[nodiscard]] std::vector<uint8_t> tree() const override { return build_tree(); }

  ovt_status expand(uint64_t box_offset) override {
    if (box_offset == 0) {  // the IVF root: the next window of FRM children
      materialised_ = std::min<size_t>(f_.frames.size(), materialised_ + kChildWindow);
      return OVT_OK;
    }
    const auto it = std::lower_bound(f_.frames.begin(), f_.frames.end(), box_offset,
                                     [](const Frame& fr, uint64_t o) { return fr.header_offset < o; });
    return it != f_.frames.end() && it->header_offset == box_offset ? OVT_OK : OVT_E_INVALID_ARG;  // FRM: no children
  }

  [[nodiscard]] std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const override {
    if (track_index != 0 || first > f_.frames.size()) {
      return {};
    }
    const size_t n = count == 0 ? f_.frames.size() - first : std::min<size_t>(count, f_.frames.size() - first);
    return build_sample_table(first, n);
  }

 private:
  static constexpr int8_t kUnknown = -1;

  const io::RandomAccessFile& file_;
  File f_;
  Timing timing_;
  mutable std::vector<int8_t> sync_;  // lazily computed is_sync per frame (-1 unknown); the handle is single-threaded
  size_t materialised_ = 0;           // FRM children present in the tree

  [[nodiscard]] int64_t dts_of(const Frame& fr) const noexcept {
    uint64_t v = 0;
    if (__builtin_mul_overflow(fr.timestamp, std::max<uint64_t>(timing_.scale, 1), &v) || v > INT64_MAX) {
      return INT64_MAX;
    }
    return static_cast<int64_t>(v);
  }

  [[nodiscard]] bool is_sync(size_t i) const {
    if (sync_[i] == kUnknown) {
      const Frame& fr = f_.frames[i];
      std::array<uint8_t, kSyncPrefix> buf{};
      const size_t want = std::min<size_t>(fr.size, buf.size());
      const size_t got = want == 0 ? 0 : file_.read(fr.payload_offset(), want, buf.data());
      sync_[i] = frame_is_sync(f_.header.fourcc, std::span<const uint8_t>(buf.data(), got)) ? 1 : 0;
    }
    return sync_[i] == 1;
  }

  [[nodiscard]] uint64_t track_duration() const noexcept {
    if (f_.frames.empty()) {
      return 0;
    }
    uint64_t v = 0;
    const uint64_t scale = std::max<uint64_t>(timing_.scale, 1);
    if (__builtin_mul_overflow(f_.frames.back().timestamp + 1, scale, &v)) {
      return UINT64_MAX;
    }
    return v;
  }

  [[nodiscard]] std::vector<uint8_t> build_sample_table(size_t first, size_t n) const {
    treefb::Fbb fbb;
    std::vector<int64_t> dts(n);
    const std::vector<int32_t> cts_offset(n, 0);
    std::vector<uint32_t> duration(n);
    std::vector<uint64_t> offset(n);
    std::vector<uint32_t> size(n);
    std::vector<uint8_t> sync(n);
    const std::vector<uint16_t> desc(n, 1);
    const std::vector<uint32_t> chunk(n, 0);
    const std::vector<int32_t> fragment(n, -1);
    for (size_t k = 0; k < n; ++k) {
      const size_t i = first + k;
      const Frame& fr = f_.frames[i];
      dts[k] = dts_of(fr);
      // The duration of a sample is the distance to the next timestamp; the last one repeats its predecessor's
      // (a single-frame stream gets one tick of `scale`).
      int64_t next_delta = std::max<int64_t>(timing_.scale, 1);
      if (i + 1 < f_.frames.size()) {
        next_delta = dts_of(f_.frames[i + 1]) - dts[k];
      } else if (i > 0) {
        next_delta = dts[k] - dts_of(f_.frames[i - 1]);
      }
      duration[k] = static_cast<uint32_t>(std::clamp<int64_t>(next_delta, 0, UINT32_MAX));
      offset[k] = fr.payload_offset();
      size[k] = fr.size;
      sync[k] = is_sync(i) ? 1 : 0;
    }
    const auto table = fb::CreateSampleTable(
        fbb, 1, timing_.timescale, n, fbb.CreateVector(dts), fbb.CreateVector(cts_offset), fbb.CreateVector(duration),
        fbb.CreateVector(offset), fbb.CreateVector(size), fbb.CreateVector(sync), 0, fbb.CreateVector(desc),
        fbb.CreateVector(chunk), fbb.CreateVector(fragment));
    fbb.Finish(table);
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
  }

  [[nodiscard]] std::vector<uint8_t> build_tree() const {
    using treefb::string_field;
    using treefb::uint_field;
    treefb::Fbb fbb;
    const Header& h = f_.header;

    // FRM children of the materialised window.
    std::vector<flatbuffers::Offset<fb::BoxNode>> frames;
    frames.reserve(materialised_);
    for (size_t i = 0; i < materialised_; ++i) {
      const Frame& fr = f_.frames[i];
      const std::array<flatbuffers::Offset<fb::Field>, 2> fields = {
          uint_field(fbb, "frame_size", fr.declared_size, 0, 32), uint_field(fbb, "timestamp", fr.timestamp, 32, 64)};
      const fb::ByteSpan span(fr.header_offset, kFrameHeaderSize + static_cast<uint64_t>(fr.size));
      const fb::ByteSpan payload(fr.payload_offset(), fr.size);
      frames.push_back(fb::CreateBoxNode(fbb, fbb.CreateString("FRM "), 0, &span, kFrameHeaderSize, -1, -1,
                                         fbb.CreateVector(fields.data(), fields.size()), &payload));
    }

    const std::string four = fourcc_string(h.fourcc);
    const std::array<flatbuffers::Offset<fb::Field>, 10> header_fields = {
        string_field(fbb, "signature", "DKIF", 0, 32),
        uint_field(fbb, "version", h.version, 32, 16),
        uint_field(fbb, "header_size", h.header_size, 48, 16),
        string_field(fbb, "fourcc", four, 64, 32),
        uint_field(fbb, "width", h.width, 96, 16),
        uint_field(fbb, "height", h.height, 112, 16),
        uint_field(fbb, "timebase_denominator", h.timebase_denominator, 128, 32),
        uint_field(fbb, "timebase_numerator", h.timebase_numerator, 160, 32),
        uint_field(fbb, "frame_count", h.frame_count, 192, 32),
        uint_field(fbb, "unused", h.unused, 224, 32)};

    std::vector<flatbuffers::Offset<fb::Diagnostic>> root_diags;
    if (materialised_ < f_.frames.size()) {  // CNT-0007: lazy children
      root_diags.push_back(treefb::diagnostic(fbb, DiagCode::CNT_0007,
                                              "IVF root at offset `0` has " + std::to_string(materialised_) + " of " +
                                                  std::to_string(f_.frames.size()) +
                                                  " FRM children (call `ovt_container_expand`)",
                                              0));
    }
    const fb::ByteSpan root_span(0, f_.file_size);
    const auto root = fb::CreateBoxNode(fbb, fbb.CreateString("IVF "), 0, &root_span,
                                        static_cast<uint8_t>(std::min<uint64_t>(f_.first_frame_offset, 255)), -1, -1,
                                        fbb.CreateVector(header_fields.data(), header_fields.size()), nullptr,
                                        fbb.CreateVector(frames), 0, 0, fbb.CreateVector(root_diags));

    std::vector<flatbuffers::Offset<fb::Diagnostic>> tree_diags;
    tree_diags.reserve(f_.diagnostics.size());
    for (const Diagnostic& d : f_.diagnostics) {
      tree_diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
    }

    // TrackInfo (§8.6.2): track_id 1, timebase-derived timescale, codec from the fourcc.
    const uint64_t dur = track_duration();
    const double dur_s = timing_.timescale == 0 ? -1.0 : static_cast<double>(dur) / timing_.timescale;
    const auto handler_fourcc = fbb.CreateString("vide");
    const auto entry_fourcc = fbb.CreateString(four);
    const auto original_format = fbb.CreateString(four);
    fb::TrackInfoBuilder tb(fbb);
    tb.add_index(0);
    tb.add_track_id(1);
    tb.add_handler(fb::HandlerType::Video);
    tb.add_handler_fourcc(handler_fourcc);
    tb.add_codec(codec_of(h.fourcc));
    tb.add_sample_entry_fourcc(entry_fourcc);
    tb.add_original_format(original_format);
    tb.add_sample_entry_count(1);
    tb.add_timescale(timing_.timescale);
    tb.add_duration(dur);
    tb.add_duration_seconds(dur_s);
    tb.add_enabled(true);
    tb.add_in_movie(true);
    tb.add_width(h.width);
    tb.add_height(h.height);
    tb.add_coded_width(h.width);
    tb.add_coded_height(h.height);
    tb.add_sample_count(f_.frames.size());
    const auto track = tb.Finish();
    const auto tracks = fbb.CreateVector(&track, 1);
    fbb.Finish(fb::CreateContainerTree(fbb, fbb.CreateString("ivf"), fbb.CreateVector(&root, 1), tracks,
                                       fbb.CreateVector(tree_diags)));
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
  }
};

}  // namespace

Timing timing_of(const Header& h) noexcept {
  if (h.timebase_denominator == 0 || h.timebase_numerator == 0) {
    return Timing{0, 0};  // unusable timebase: timestamps stay raw and the duration in seconds is unknown
  }
  const uint64_t g = gcd64(h.timebase_numerator, h.timebase_denominator);
  return Timing{static_cast<uint32_t>(h.timebase_denominator / g), static_cast<uint32_t>(h.timebase_numerator / g)};
}

bool frame_is_sync(uint32_t fourcc, std::span<const uint8_t> prefix) noexcept {
  switch (fourcc) {
    case kAv1:
      return av1_starts_with_key_frame(prefix);
    case kVp9:
      return vp9_key_frame(prefix);
    case kVp8:
      return vp8_key_frame(prefix);
    default:
      return false;
  }
}

std::optional<File> parse(const io::RandomAccessFile& file) {
  File f;
  f.file_size = file.size();
  std::array<uint8_t, kMinHeaderSize> hdr{};
  if (f.file_size < kMinHeaderSize || file.read(0, hdr.size(), hdr.data()) != hdr.size() ||
      std::memcmp(hdr.data(), "DKIF", 4) != 0) {
    return std::nullopt;
  }
  Header& h = f.header;
  h.version = rd16(&hdr[4]);
  h.header_size = rd16(&hdr[6]);
  h.fourcc = rd32(&hdr[8]);
  h.width = rd16(&hdr[12]);
  h.height = rd16(&hdr[14]);
  h.timebase_denominator = rd32(&hdr[16]);
  h.timebase_numerator = rd32(&hdr[20]);
  h.frame_count = rd32(&hdr[24]);
  h.unused = rd32(&hdr[28]);

  // SPEC-ASSUMPTION(#280): a header_size below 32 would overlap the fixed header; it is reported with
  // CNT-IVF-0001 and the first frame is taken at offset 32, as libvpx does (it ignores the field).
  f.first_frame_offset = std::max<uint32_t>(h.header_size, kMinHeaderSize);
  if (h.header_size != kMinHeaderSize) {
    f.diagnostics.push_back({DiagCode::CNT_IVF_0001,
                             "IVF `header_size` is `" + std::to_string(h.header_size) +
                                 "` (≠ 32); first frame taken at offset `" + std::to_string(f.first_frame_offset) + "`",
                             6});
  }

  uint64_t pos = f.first_frame_offset;
  std::array<uint8_t, kFrameHeaderSize> fh{};
  while (pos < f.file_size) {
    if (f.file_size - pos < kFrameHeaderSize || file.read(pos, fh.size(), fh.data()) != fh.size()) {
      // SPEC-ASSUMPTION(#280): a partial frame header at EOF is reported with CNT-IVF-0003 and ends the walk.
      f.diagnostics.push_back({DiagCode::CNT_IVF_0003,
                               "last IVF frame truncated: only `" + std::to_string(f.file_size - pos) +
                                   "` bytes remain for the 12-byte frame header at offset `" + std::to_string(pos) +
                                   "`",
                               pos});
      break;
    }
    Frame fr;
    fr.header_offset = pos;
    fr.declared_size = rd32(fh.data());
    fr.timestamp = rd64(&fh[4]);
    const uint64_t payload = pos + kFrameHeaderSize;
    const uint64_t remaining = f.file_size - payload;
    if (fr.declared_size > remaining) {
      fr.size = static_cast<uint32_t>(remaining);
      f.frames.push_back(fr);
      f.diagnostics.push_back({DiagCode::CNT_IVF_0003,
                               "last IVF frame truncated: `frame_size` `" + std::to_string(fr.declared_size) +
                                   "` exceeds EOF by `" + std::to_string(fr.declared_size - remaining) +
                                   "` bytes (clamped)",
                               pos});
      break;
    }
    fr.size = fr.declared_size;
    f.frames.push_back(fr);
    pos = payload + fr.size;
  }

  if (h.frame_count != f.frames.size()) {
    f.diagnostics.push_back({DiagCode::CNT_IVF_0002,
                             "IVF header `frame_count` `" + std::to_string(h.frame_count) +
                                 "` differs from the walked count `" + std::to_string(f.frames.size()) + "`",
                             24});
  }
  return f;
}

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file) {
  auto parsed = parse(file);
  if (!parsed) {
    return nullptr;
  }
  return std::make_unique<IvfReader>(file, std::move(*parsed));
}

}  // namespace ovt::container::ivf
