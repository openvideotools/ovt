// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MPEG-TS packet sync and packet / adaptation-field decoding (chapter 08 §8.6.7, WI-4.204). Source of truth:
// ISO/IEC 13818-1 §2.4.3 (transport packet, adaptation field) and ETSI TR 101 290 §5.2 (sync and continuity checks).
//
// SPEC-ASSUMPTION(#337): the resync rule, the node names (MPTS / TSPK / ADPT), the continuity-counter rule and the
// M2TS prefix handling below are the answers proposed in that issue.

#include "container/mpegts/ts_packets.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

constexpr std::array<uint32_t, 3> kStrides = {188, 192, 204};
constexpr size_t kHeadBytes = 65536;      // bytes examined for the packet geometry
constexpr size_t kBlockBytes = 1U << 20;  // read-ahead block of the sequential walk
constexpr unsigned kSyncProbe = 5;        // aligned sync bytes required to accept a position

// Cached sequential view of the file; at() returns -1 past the end.
class Source {
 public:
  explicit Source(const io::RandomAccessFile& file) : file_(file), size_(file.size()), buf_(kBlockBytes) {}

  [[nodiscard]] int at(uint64_t off) {
    if (!load(off)) {
      return -1;
    }
    return buf_[off - base_];
  }

  // Copies bytes starting at off into out; returns the number copied.
  size_t copy(uint64_t off, std::span<uint8_t> out) {
    size_t done = 0;
    while (done < out.size() && load(off + done)) {
      const size_t at_block = off + done - base_;
      const size_t take = std::min(out.size() - done, len_ - at_block);
      std::memcpy(out.subspan(done).data(), std::span<const uint8_t>(buf_).subspan(at_block, take).data(), take);
      done += take;
    }
    return done;
  }

 private:
  const io::RandomAccessFile& file_;
  uint64_t size_;
  std::vector<uint8_t> buf_;
  uint64_t base_ = 0;
  size_t len_ = 0;

  bool load(uint64_t off) {
    if (off >= size_) {
      return false;
    }
    if (off >= base_ && off - base_ < len_) {
      return true;
    }
    base_ = off - off % kBlockBytes;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kBlockBytes, size_ - base_));
    len_ = file_.read(base_, want, buf_.data());
    return off - base_ < len_;
  }
};

struct CcState {
  bool valid = false;
  bool dup_used = false;
  uint8_t last = 0;
};

uint64_t flag(bool b) {
  return b ? 1U : 0U;
}

std::string hex_pid(uint16_t pid) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string s = "0x0000";
  for (size_t i = 0; i < 4; ++i) {
    s[5 - i] = digits[(pid >> (4U * i)) & 15U];
  }
  return s;
}

// Reads the 33-bit base and 9-bit extension of a PCR / OPCR from its six bytes.
uint64_t read_pcr_base(std::span<const uint8_t> p) {
  return (static_cast<uint64_t>(p[0]) << 25U) | (static_cast<uint64_t>(p[1]) << 17U) |
         (static_cast<uint64_t>(p[2]) << 9U) | (static_cast<uint64_t>(p[3]) << 1U) |
         (static_cast<uint64_t>(p[4]) >> 7U);
}

uint16_t read_pcr_ext(std::span<const uint8_t> p) {
  return static_cast<uint16_t>(((p[4] & 1U) << 8U) | p[5]);
}

// Reads a six-byte clock reference at `pos` when it fits before `end`; always advances `pos` past it.
void read_clock(std::span<const uint8_t> p, size_t& pos, size_t end, uint64_t& base, uint16_t& ext, bool& truncated) {
  if (pos + 6 <= end) {
    base = read_pcr_base(p.subspan(pos, 6));
    ext = read_pcr_ext(p.subspan(pos, 6));
  } else {
    truncated = true;
  }
  pos += 6;
}

// The optional fields after the flag byte of an adaptation field (13818-1 §2.4.3.4); `end` is the exclusive end of
// the bytes the field may use.
void decode_optional_fields(std::span<const uint8_t> p, size_t end, AdaptationField& af) {
  size_t pos = 6;
  if (af.pcr_flag) {
    read_clock(p, pos, end, af.pcr_base, af.pcr_extension, af.truncated);
  }
  if (af.opcr_flag) {
    read_clock(p, pos, end, af.opcr_base, af.opcr_extension, af.truncated);
  }
  if (af.splicing_point_flag) {
    if (pos + 1 <= end) {
      af.splice_countdown = static_cast<int8_t>(p[pos]);
    } else {
      af.truncated = true;
    }
    pos += 1;
  }
  if (af.transport_private_data_flag) {
    if (pos + 1 > end) {
      af.truncated = true;
    } else {
      af.transport_private_data_length = p[pos];
      af.truncated = af.truncated || pos + 1 + af.transport_private_data_length > end;
    }
  }
}

// Decodes the adaptation field of `p` (whole packet, at most 188 bytes) and sets the payload offset.
AdaptationField decode_adaptation(std::span<const uint8_t> p, PacketHeader& h) {
  AdaptationField af;
  if (p.size() < 5) {
    af.truncated = true;
    return af;
  }
  af.length = p[4];
  h.payload_offset = static_cast<uint32_t>(std::min<size_t>(5U + af.length, kPacketBytes));
  const size_t end = std::min<size_t>(5U + af.length, p.size());
  af.truncated = 5U + af.length > p.size();
  if (af.length == 0) {
    return af;
  }
  if (end <= 5) {
    af.truncated = true;
    return af;
  }
  const uint8_t flags = p[5];
  af.discontinuity_indicator = (flags & 0x80U) != 0;
  af.random_access_indicator = (flags & 0x40U) != 0;
  af.elementary_stream_priority_indicator = (flags & 0x20U) != 0;
  af.pcr_flag = (flags & 0x10U) != 0;
  af.opcr_flag = (flags & 0x08U) != 0;
  af.splicing_point_flag = (flags & 0x04U) != 0;
  af.transport_private_data_flag = (flags & 0x02U) != 0;
  af.adaptation_field_extension_flag = (flags & 0x01U) != 0;
  decode_optional_fields(p, end, af);
  return af;
}

// True when a packet whose sync byte is at `off` fits in the file and every slot of the next kSyncProbe packets that
// fits carries a sync byte; at least two slots are needed.
bool pattern_at(Source& src, uint64_t off, uint64_t stride, uint64_t size) {
  unsigned seen = 0;
  for (uint64_t i = 0; i < kSyncProbe; ++i) {
    const uint64_t o = off + i * stride;
    if (o + kPacketBytes > size) {
      break;
    }
    if (src.at(o) != kSyncByte) {
      return false;
    }
    ++seen;
  }
  return seen >= 2;
}

// The same probe on the in-memory head of the file.
bool head_pattern(std::span<const uint8_t> head, uint64_t phase, uint64_t stride, uint64_t file_size) {
  unsigned seen = 0;
  for (uint64_t i = 0; i < kSyncProbe; ++i) {
    const uint64_t o = phase + i * stride;
    if (o >= head.size() || o + kPacketBytes > file_size) {
      break;
    }
    if (head[static_cast<size_t>(o)] != kSyncByte) {
      return false;
    }
    ++seen;
  }
  return seen >= 2;
}

void add_diag(File& f, DiagCode code, std::string message, uint64_t offset) {
  if (f.diagnostics.size() < kMaxDiagnostics) {
    f.diagnostics.push_back({code, std::move(message), offset});
  }
}

// The sequential walk of one file: sync, resync, statistics and diagnostics (§8.6.7).
class Walker {
 public:
  Walker(const io::RandomAccessFile& file, File& f)
      : src_(file),
        f_(f),
        stride_(f.geometry.stride),
        prefix_(f.geometry.prefix),
        size_(f.file_size),
        stats_(kPidCount),
        cc_(kPidCount),
        pos_(f.geometry.first_sync),
        last_good_(f.geometry.first_sync) {
    for (size_t i = 0; i < kPidCount; ++i) {
      stats_[i].pid = static_cast<uint16_t>(i);
    }
  }

  void run() {
    report_leading_garbage();
    while (pos_ + kPacketBytes <= size_) {
      if (src_.at(pos_) == kSyncByte) {
        if (!take_packet()) {
          break;
        }
        continue;
      }
      in_run_ = false;  // the sync byte is missing at the expected position
      if (!skip_isolated() && !resync()) {
        break;
      }
    }
    uint64_t scrambled_elsewhere = 0;
    for (const PidStats& st : stats_) {
      if (st.packets > 0) {
        f_.pids.push_back(st);
      }
      if (st.pid != kPatPid && st.pid != kCatPid) {
        scrambled_elsewhere += st.scrambled_packets;
      }
    }
    psi_.finish([this](uint16_t pid) { return stats_[pid].packets; }, scrambled_elsewhere);
    f_.psi = std::move(psi_.tables());
    for (const PsiDiagnostic& d : f_.psi.diagnostics) {  // the PSI diagnostics join the file list (same cap)
      add_diag(f_, d.code, d.message, d.offset);
    }
  }

 private:
  Source src_;
  File& f_;
  uint64_t stride_;
  uint64_t prefix_;
  uint64_t size_;
  std::vector<PidStats> stats_;
  std::vector<CcState> cc_;
  PsiParser psi_;
  uint64_t pos_;        // file offset of the next sync byte slot
  uint64_t last_good_;  // sync offset of the last packet taken
  bool in_run_ = false;

  void report_leading_garbage() {
    const uint64_t garbage = f_.geometry.first_sync - prefix_;
    if (garbage == 0) {
      return;
    }
    ++f_.sync_loss_events;
    const uint64_t n = std::max<uint64_t>(1, (garbage + stride_ - 1) / stride_);
    add_diag(f_, DiagCode::CNT_TS_1001,
             "TS_sync_loss: `" + std::to_string(n) + "` consecutive packets without `0x47` at offset `0`", 0);
  }

  // Decodes the packet at pos_ (its sync byte is present), updates the statistics and moves to the next slot.
  bool take_packet() {
    std::array<uint8_t, kPacketBytes> buf{};
    if (src_.copy(pos_, buf) != buf.size()) {
      return false;
    }
    const auto h = decode_packet(buf);
    if (!h) {
      return false;
    }
    if (!in_run_) {
      f_.runs.push_back({f_.packet_count, pos_, 0});
      in_run_ = true;
    }
    ++f_.runs.back().count;
    account(*h);
    if (psi_.wants(h->pid) && h->has_payload() && !h->transport_error_indicator) {
      psi_.feed(h->pid, h->payload_unit_start_indicator, h->transport_scrambling_control != 0,
                std::span<const uint8_t>(buf).subspan(h->payload_offset, h->payload_size), f_.packet_count,
                pos_ + h->payload_offset);
    }
    ++f_.packet_count;
    last_good_ = pos_;
    pos_ += stride_;
    return true;
  }

  void account(const PacketHeader& h) {
    const uint64_t k = f_.packet_count;
    PidStats& st = stats_[h.pid];
    if (st.packets == 0) {
      st.first_packet = k;
    }
    ++st.packets;
    st.payload_packets += h.has_payload() ? 1U : 0U;
    st.scrambled_packets += h.transport_scrambling_control != 0 ? 1U : 0U;
    if (h.transport_error_indicator) {
      ++st.transport_errors;
      add_diag(f_, DiagCode::CNT_TS_2001,
               "Transport_error: `transport_error_indicator` set at packet `" + std::to_string(k) + "`", pos_);
      // The header of a packet with the error indicator is unreliable: the PID restarts its counter afterwards.
      cc_[h.pid] = CcState{};
    } else if (h.pid != kNullPid && h.adaptation_field_control != 0) {
      check_continuity(h, st, k);
    }
  }

  // Continuity counter rule (13818-1 §2.4.3.3; SPEC-ASSUMPTION(#337)): the counter of the last payload packet is
  // kept; a payload packet carries the next value, one duplicate is allowed, an adaptation-only packet repeats it.
  void check_continuity(const PacketHeader& h, PidStats& st, uint64_t k) {
    CcState& c = cc_[h.pid];
    const bool discontinuity = h.adaptation && h.adaptation->discontinuity_indicator;
    if (!c.valid || discontinuity) {
      c = {true, false, h.continuity_counter};
      return;
    }
    const uint8_t expected = h.has_payload() ? static_cast<uint8_t>((c.last + 1U) & 15U) : c.last;
    if (h.continuity_counter == expected) {
      c.last = h.continuity_counter;
      c.dup_used = false;
    } else if (h.has_payload() && h.continuity_counter == c.last && !c.dup_used) {
      c.dup_used = true;  // one duplicate packet is allowed
    } else {
      ++st.continuity_errors;
      add_diag(f_, DiagCode::CNT_TS_1006,
               "Continuity_count_error on PID `" + hex_pid(h.pid) + "` at packet `" + std::to_string(k) + "` (`" +
                   std::to_string(expected) + "`/`" + std::to_string(h.continuity_counter) + "`)",
               pos_);
      c.last = h.continuity_counter;
      c.dup_used = false;
      psi_.discontinuity(h.pid);  // a section that was being collected is incomplete
    }
  }

  // An isolated bad sync byte (the next one or two slots are fine) is CNT-TS-1002 and the grid continues.
  bool skip_isolated() {
    const uint64_t next = pos_ + stride_;
    const bool next_fits = next + kPacketBytes <= size_;
    const bool after_fits = next + stride_ + kPacketBytes <= size_;
    if (!next_fits || src_.at(next) != kSyncByte || (after_fits && src_.at(next + stride_) != kSyncByte)) {
      return false;
    }
    ++f_.sync_byte_errors;
    add_diag(f_, DiagCode::CNT_TS_1002, "Sync_byte_error at packet `" + std::to_string(f_.packet_count) + "`", pos_);
    pos_ = next;
    return true;
  }

  // Sync loss: reports it once and looks for the next position with up to five aligned sync bytes, starting just
  // after the last good sync byte. Returns false when there is none (the rest of the file is lost).
  bool resync() {
    ++f_.sync_loss_events;
    for (uint64_t q = last_good_ + 1; q + kPacketBytes <= size_; ++q) {
      if (src_.at(q) == kSyncByte && pattern_at(src_, q, stride_, size_)) {
        const uint64_t skipped = q > pos_ ? (q - pos_ + stride_ - 1) / stride_ : 0;
        report_loss(std::max<uint64_t>(1, skipped));
        pos_ = q;
        return true;
      }
    }
    report_loss(std::max<uint64_t>(1, (size_ - pos_) / stride_));
    return false;
  }

  void report_loss(uint64_t packets) {
    add_diag(f_, DiagCode::CNT_TS_1001,
             "TS_sync_loss: `" + std::to_string(packets) + "` consecutive packets without `0x47` at offset `" +
                 std::to_string(pos_) + "`",
             pos_);
  }
};

}  // namespace

std::optional<PacketHeader> decode_packet(std::span<const uint8_t> packet) noexcept {
  if (packet.size() < 4 || packet[0] != kSyncByte) {
    return std::nullopt;
  }
  const std::span<const uint8_t> p = packet.first(std::min<size_t>(packet.size(), kPacketBytes));
  PacketHeader h;
  h.transport_error_indicator = (p[1] & 0x80U) != 0;
  h.payload_unit_start_indicator = (p[1] & 0x40U) != 0;
  h.transport_priority = (p[1] & 0x20U) != 0;
  h.pid = static_cast<uint16_t>(((p[1] & 0x1FU) << 8U) | p[2]);
  h.transport_scrambling_control = static_cast<uint8_t>(p[3] >> 6U);
  h.adaptation_field_control = static_cast<uint8_t>((p[3] >> 4U) & 3U);
  h.continuity_counter = static_cast<uint8_t>(p[3] & 15U);
  h.payload_offset = 4;
  if ((h.adaptation_field_control & 2U) != 0) {
    h.adaptation = decode_adaptation(p, h);
  }
  h.payload_size =
      h.has_payload() && h.payload_offset < p.size() ? static_cast<uint32_t>(p.size() - h.payload_offset) : 0;
  return h;
}

std::optional<Geometry> detect_geometry(std::span<const uint8_t> head, uint64_t file_size) {
  for (size_t phase = 0; phase < head.size(); ++phase) {
    if (head[phase] != kSyncByte) {
      continue;
    }
    for (const uint32_t stride : kStrides) {
      if (head_pattern(head, phase, stride, file_size)) {
        Geometry g;
        g.stride = stride;
        g.first_sync = phase;
        g.prefix = (stride == 192 && phase == kM2tsPrefix) ? kM2tsPrefix : 0;
        return g;
      }
    }
  }
  return std::nullopt;
}

uint64_t File::sync_offset(uint64_t index) const noexcept {
  const auto it =
      std::upper_bound(runs.begin(), runs.end(), index, [](uint64_t i, const Run& r) { return i < r.first_packet; });
  const Run& r = *std::prev(it);
  return r.sync_offset + (index - r.first_packet) * geometry.stride;
}

uint64_t File::packet_offset(uint64_t index) const noexcept {
  return sync_offset(index) - geometry.prefix;
}

std::optional<File> parse(const io::RandomAccessFile& file) {
  File f;
  f.file_size = file.size();
  if (f.file_size < 2U * uint64_t{kPacketBytes}) {
    return std::nullopt;
  }
  std::vector<uint8_t> head(static_cast<size_t>(std::min<uint64_t>(kHeadBytes, f.file_size)));
  head.resize(file.read(0, head.size(), head.data()));
  const auto geometry = detect_geometry(head, f.file_size);
  if (!geometry) {
    return std::nullopt;
  }
  f.geometry = *geometry;
  Walker(file, f).run();
  if (f.packet_count == 0) {
    return std::nullopt;
  }
  return f;
}

namespace {

class MpegtsReader final : public Reader {
 public:
  MpegtsReader(const io::RandomAccessFile& file, File parsed) : file_(file), f_(std::move(parsed)) {
    materialised_ = std::min<uint64_t>(f_.packet_count, kChildWindow);
  }

  [[nodiscard]] std::vector<uint8_t> tree() const override { return build_tree(); }

  ovt_status expand(uint64_t box_offset) override {
    if (box_offset == 0) {  // the root: the next window of TSPK children
      materialised_ = std::min<uint64_t>(f_.packet_count, materialised_ + kChildWindow);
      return OVT_OK;
    }
    return is_packet_start(box_offset) ? OVT_OK : OVT_E_INVALID_ARG;  // TSPK: nothing lazy below it
  }

 private:
  const io::RandomAccessFile& file_;
  File f_;
  uint64_t materialised_ = 0;

  [[nodiscard]] bool is_packet_start(uint64_t off) const {
    const uint64_t prefix = f_.geometry.prefix;
    const auto it = std::upper_bound(f_.runs.begin(), f_.runs.end(), off,
                                     [prefix](uint64_t o, const Run& r) { return o < r.sync_offset - prefix; });
    if (it == f_.runs.begin()) {
      return false;
    }
    const Run& r = *std::prev(it);
    const uint64_t d = off - (r.sync_offset - prefix);
    return d % f_.geometry.stride == 0 && d / f_.geometry.stride < r.count;
  }

  using Fbb = treefb::Fbb;

  [[nodiscard]] flatbuffers::Offset<fb::BoxNode> packet_node(Fbb& fbb, uint64_t index) const {
    using treefb::uint_field;
    const uint64_t prefix = f_.geometry.prefix;
    const uint64_t stride = f_.geometry.stride;
    const uint64_t sync = f_.sync_offset(index);
    const uint64_t start = sync - prefix;
    std::array<uint8_t, kM2tsPrefix + kPacketBytes> raw{};
    const size_t got = file_.read(start, prefix + kPacketBytes, raw.data());
    const std::span<const uint8_t> body =
        std::span<const uint8_t>(raw).subspan(prefix, got > prefix ? got - prefix : 0);
    const auto decoded = decode_packet(body);
    const PacketHeader h = decoded.value_or(PacketHeader{});
    const uint64_t b = prefix * 8U;  // bit offset of the sync byte inside the node

    std::vector<flatbuffers::Offset<fb::Field>> fields;
    if (prefix != 0) {
      const uint32_t ts = (static_cast<uint32_t>(raw[0]) << 24U) | (static_cast<uint32_t>(raw[1]) << 16U) |
                          (static_cast<uint32_t>(raw[2]) << 8U) | raw[3];
      fields.push_back(uint_field(fbb, "copy_permission_indicator", ts >> 30U, 0, 2));
      fields.push_back(uint_field(fbb, "arrival_time_stamp", ts & 0x3FFFFFFFU, 2, 30));
    }
    fields.push_back(uint_field(fbb, "sync_byte", kSyncByte, b, 8));
    fields.push_back(uint_field(fbb, "transport_error_indicator", flag(h.transport_error_indicator), b + 8, 1));
    fields.push_back(uint_field(fbb, "payload_unit_start_indicator", flag(h.payload_unit_start_indicator), b + 9, 1));
    fields.push_back(uint_field(fbb, "transport_priority", flag(h.transport_priority), b + 10, 1));
    fields.push_back(uint_field(fbb, "PID", h.pid, b + 11, 13));
    fields.push_back(uint_field(fbb, "transport_scrambling_control", h.transport_scrambling_control, b + 24, 2));
    fields.push_back(uint_field(fbb, "adaptation_field_control", h.adaptation_field_control, b + 26, 2));
    fields.push_back(uint_field(fbb, "continuity_counter", h.continuity_counter, b + 28, 4));

    std::vector<flatbuffers::Offset<fb::BoxNode>> children;
    if (h.adaptation) {
      children.push_back(adaptation_node(fbb, *h.adaptation, sync + 4,
                                         std::min<uint64_t>(f_.file_size - (sync + 4), 1U + h.adaptation->length)));
    }

    const uint64_t length = std::min<uint64_t>(stride, f_.file_size - start);
    const fb::ByteSpan span(start, length);
    const fb::ByteSpan payload(sync + h.payload_offset, h.payload_size);
    return fb::CreateBoxNode(fbb, fbb.CreateString("TSPK"), 0, &span, static_cast<uint8_t>(prefix + 4), -1, -1,
                             fbb.CreateVector(fields), h.payload_size > 0 ? &payload : nullptr,
                             fbb.CreateVector(children), fbb.CreateString("ISO/IEC 13818-1 §2.4.3.2"),
                             fbb.CreateString("transport_packet"));
  }

  [[nodiscard]] static flatbuffers::Offset<fb::BoxNode> adaptation_node(Fbb& fbb, const AdaptationField& a,
                                                                        uint64_t offset, uint64_t length) {
    using treefb::uint_field;
    std::vector<flatbuffers::Offset<fb::Field>> fields;
    fields.push_back(uint_field(fbb, "adaptation_field_length", a.length, 0, 8));
    if (a.length > 0) {
      fields.push_back(uint_field(fbb, "discontinuity_indicator", flag(a.discontinuity_indicator), 8, 1));
      fields.push_back(uint_field(fbb, "random_access_indicator", flag(a.random_access_indicator), 9, 1));
      fields.push_back(
          uint_field(fbb, "elementary_stream_priority_indicator", flag(a.elementary_stream_priority_indicator), 10, 1));
      fields.push_back(uint_field(fbb, "PCR_flag", flag(a.pcr_flag), 11, 1));
      fields.push_back(uint_field(fbb, "OPCR_flag", flag(a.opcr_flag), 12, 1));
      fields.push_back(uint_field(fbb, "splicing_point_flag", flag(a.splicing_point_flag), 13, 1));
      fields.push_back(uint_field(fbb, "transport_private_data_flag", flag(a.transport_private_data_flag), 14, 1));
      fields.push_back(
          uint_field(fbb, "adaptation_field_extension_flag", flag(a.adaptation_field_extension_flag), 15, 1));
      uint64_t bit = 16;
      const uint64_t limit = std::min<uint64_t>(8U * (1U + static_cast<uint64_t>(a.length)), 8U * length);
      if (a.pcr_flag && bit + 48 <= limit) {
        fields.push_back(uint_field(fbb, "program_clock_reference_base", a.pcr_base, bit, 33));
        fields.push_back(uint_field(fbb, "program_clock_reference_extension", a.pcr_extension, bit + 39, 9));
        fields.push_back(uint_field(fbb, "PCR_27MHz", a.pcr(), bit, 48));
        bit += 48;
      } else if (a.pcr_flag) {
        bit += 48;
      }
      if (a.opcr_flag && bit + 48 <= limit) {
        fields.push_back(uint_field(fbb, "original_program_clock_reference_base", a.opcr_base, bit, 33));
        fields.push_back(uint_field(fbb, "original_program_clock_reference_extension", a.opcr_extension, bit + 39, 9));
        bit += 48;
      } else if (a.opcr_flag) {
        bit += 48;
      }
      if (a.splicing_point_flag && bit + 8 <= limit) {
        fields.push_back(treefb::uint_field(fbb, "splice_countdown", static_cast<uint8_t>(a.splice_countdown), bit, 8));
        bit += 8;
      }
      if (a.transport_private_data_flag && bit + 8 <= limit) {
        fields.push_back(uint_field(fbb, "transport_private_data_length", a.transport_private_data_length, bit, 8));
      }
    }
    const fb::ByteSpan span(offset, length);
    return fb::CreateBoxNode(fbb, fbb.CreateString("ADPT"), 0, &span,
                             static_cast<uint8_t>(std::min<uint64_t>(length, 2)), -1, -1, fbb.CreateVector(fields),
                             nullptr, 0, fbb.CreateString("ISO/IEC 13818-1 §2.4.3.4"),
                             fbb.CreateString("adaptation_field"));
  }

  [[nodiscard]] std::vector<uint8_t> build_tree() const {
    using treefb::uint_field;
    Fbb fbb;
    std::vector<flatbuffers::Offset<fb::BoxNode>> packets;
    packets.reserve(static_cast<size_t>(materialised_));
    for (uint64_t i = 0; i < materialised_; ++i) {
      packets.push_back(packet_node(fbb, i));
    }

    // Derived, span-less fields of the root (§8.6.7): geometry, totals and per-PID statistics.
    const auto plain = [&fbb](const char* name, uint64_t value) {
      return fb::CreateField(fbb, fbb.CreateString(name), fb::SyntaxValueKind::UInt, value);
    };
    std::vector<flatbuffers::Offset<fb::Field>> fields = {
        plain("packet_size", f_.geometry.stride),
        plain("packet_count", f_.packet_count),
        plain("first_sync_offset", f_.geometry.first_sync),
        plain("m2ts_timestamp_prefix", f_.geometry.prefix != 0 ? 1 : 0),
        plain("sync_loss_events", f_.sync_loss_events),
        plain("sync_byte_errors", f_.sync_byte_errors),
        plain("pid_count", f_.pids.size())};
    for (const PidStats& p : f_.pids) {
      const std::array<flatbuffers::Offset<fb::Field>, 6> sub = {plain("packets", p.packets),
                                                                 plain("payload_packets", p.payload_packets),
                                                                 plain("scrambled_packets", p.scrambled_packets),
                                                                 plain("transport_errors", p.transport_errors),
                                                                 plain("continuity_errors", p.continuity_errors),
                                                                 plain("first_packet", p.first_packet)};
      fields.push_back(fb::CreateField(fbb, fbb.CreateString("pid"), fb::SyntaxValueKind::UInt, p.pid, 0, 0, 0, nullptr,
                                       0, fbb.CreateVector(sub.data(), sub.size())));
    }

    std::vector<flatbuffers::Offset<fb::Diagnostic>> root_diags;
    if (materialised_ < f_.packet_count) {  // CNT-0007: lazy children
      root_diags.push_back(treefb::diagnostic(fbb, DiagCode::CNT_0007,
                                              "MPEG-TS root at offset `0` has " + std::to_string(materialised_) +
                                                  " of " + std::to_string(f_.packet_count) +
                                                  " TSPK children (call `ovt_container_expand`)",
                                              0));
    }
    const fb::ByteSpan root_span(0, f_.file_size);
    const auto root =
        fb::CreateBoxNode(fbb, fbb.CreateString("MPTS"), 0, &root_span, 0, -1, -1, fbb.CreateVector(fields), nullptr,
                          fbb.CreateVector(packets), fbb.CreateString("ISO/IEC 13818-1 §2.4.3"),
                          fbb.CreateString("MPEG transport stream"), fbb.CreateVector(root_diags));

    std::vector<flatbuffers::Offset<fb::Diagnostic>> tree_diags;
    tree_diags.reserve(f_.diagnostics.size());
    for (const Diagnostic& d : f_.diagnostics) {
      tree_diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
    }
    fbb.Finish(fb::CreateContainerTree(fbb, fbb.CreateString("mpegts"), fbb.CreateVector(&root, 1), 0,
                                       fbb.CreateVector(tree_diags)));
    const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    return {bytes.begin(), bytes.end()};
  }
};

}  // namespace

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file) {
  auto parsed = parse(file);
  if (!parsed) {
    return nullptr;
  }
  return std::make_unique<MpegtsReader>(file, std::move(*parsed));
}

}  // namespace ovt::container::mpegts
