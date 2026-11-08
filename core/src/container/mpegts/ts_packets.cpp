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
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

constexpr uint32_t kStrides[] = {188, 192, 204};
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
    return buf_[static_cast<size_t>(off - base_)];
  }

  // Copies up to n bytes starting at off; returns the number copied.
  size_t copy(uint64_t off, size_t n, uint8_t* out) {
    size_t done = 0;
    while (done < n && load(off + done)) {
      const size_t at_block = static_cast<size_t>(off + done - base_);
      const size_t take = std::min(n - done, len_ - at_block);
      std::memcpy(out + done, buf_.data() + at_block, take);
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

std::string hex_pid(uint16_t pid) {
  std::array<char, 8> b{};
  std::snprintf(b.data(), b.size(), "0x%04X", static_cast<unsigned>(pid));
  return b.data();
}

uint64_t read_pcr_base(const uint8_t* p) {
  return (static_cast<uint64_t>(p[0]) << 25U) | (static_cast<uint64_t>(p[1]) << 17U) |
         (static_cast<uint64_t>(p[2]) << 9U) | (static_cast<uint64_t>(p[3]) << 1U) |
         (static_cast<uint64_t>(p[4]) >> 7U);
}

uint16_t read_pcr_ext(const uint8_t* p) {
  return static_cast<uint16_t>(((p[4] & 1U) << 8U) | p[5]);
}

// True when a packet whose sync byte is at `off` fits in the file and every slot of the next kSyncProbe packets that
// fits carries a sync byte; at least two slots are needed.
bool pattern_at(Source& src, uint64_t off, uint64_t stride, uint64_t size) {
  unsigned seen = 0;
  for (unsigned i = 0; i < kSyncProbe; ++i) {
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

void add_diag(File& f, DiagCode code, std::string message, uint64_t offset) {
  if (f.diagnostics.size() < kMaxDiagnostics) {
    f.diagnostics.push_back({code, std::move(message), offset});
  }
}

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
    AdaptationField af;
    if (p.size() < 5) {
      af.truncated = true;
    } else {
      af.length = p[4];
      h.payload_offset = static_cast<uint32_t>(std::min<size_t>(5U + af.length, kPacketBytes));
      const size_t end = std::min<size_t>(5U + af.length, p.size());  // last byte (exclusive) the field may use
      if (5U + af.length > p.size()) {
        af.truncated = true;
      }
      if (af.length > 0 && end > 5) {
        const uint8_t flags = p[5];
        af.discontinuity_indicator = (flags & 0x80U) != 0;
        af.random_access_indicator = (flags & 0x40U) != 0;
        af.elementary_stream_priority_indicator = (flags & 0x20U) != 0;
        af.pcr_flag = (flags & 0x10U) != 0;
        af.opcr_flag = (flags & 0x08U) != 0;
        af.splicing_point_flag = (flags & 0x04U) != 0;
        af.transport_private_data_flag = (flags & 0x02U) != 0;
        af.adaptation_field_extension_flag = (flags & 0x01U) != 0;
        size_t pos = 6;
        if (af.pcr_flag) {
          if (pos + 6 <= end) {
            af.pcr_base = read_pcr_base(&p[pos]);
            af.pcr_extension = read_pcr_ext(&p[pos]);
          } else {
            af.truncated = true;
          }
          pos += 6;
        }
        if (af.opcr_flag) {
          if (pos + 6 <= end) {
            af.opcr_base = read_pcr_base(&p[pos]);
            af.opcr_extension = read_pcr_ext(&p[pos]);
          } else {
            af.truncated = true;
          }
          pos += 6;
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
          if (pos + 1 <= end) {
            af.transport_private_data_length = p[pos];
            if (pos + 1 + af.transport_private_data_length > end) {
              af.truncated = true;
            }
          } else {
            af.truncated = true;
          }
        }
      } else if (af.length > 0) {
        af.truncated = true;
      }
    }
    h.adaptation = af;
  }
  if (h.has_payload() && h.payload_offset < p.size()) {
    h.payload_size = static_cast<uint32_t>(p.size() - h.payload_offset);
  } else {
    h.payload_size = 0;
  }
  return h;
}

std::optional<Geometry> detect_geometry(std::span<const uint8_t> head, uint64_t file_size) {
  for (size_t phase = 0; phase < head.size(); ++phase) {
    if (head[phase] != kSyncByte) {
      continue;
    }
    for (const uint32_t stride : kStrides) {
      unsigned seen = 0;
      bool ok = true;
      for (unsigned i = 0; i < kSyncProbe && ok; ++i) {
        const uint64_t o = phase + static_cast<uint64_t>(i) * stride;
        if (o >= head.size() || o + kPacketBytes > file_size) {
          break;
        }
        ok = head[o] == kSyncByte;
        seen += ok ? 1U : 0U;
      }
      if (ok && seen >= 2) {
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
  if (f.file_size < 2 * kPacketBytes) {
    return std::nullopt;
  }
  std::vector<uint8_t> head(static_cast<size_t>(std::min<uint64_t>(kHeadBytes, f.file_size)));
  head.resize(file.read(0, head.size(), head.data()));
  const auto geometry = detect_geometry(head, f.file_size);
  if (!geometry) {
    return std::nullopt;
  }
  f.geometry = *geometry;
  const uint64_t stride = geometry->stride;
  const uint64_t prefix = geometry->prefix;
  const uint64_t size = f.file_size;
  Source src(file);

  std::vector<PidStats> stats(kPidCount);
  std::vector<CcState> cc(kPidCount);
  for (size_t i = 0; i < kPidCount; ++i) {
    stats[i].pid = static_cast<uint16_t>(i);
  }

  const uint64_t garbage = geometry->first_sync - prefix;
  if (garbage > 0) {  // bytes in front of the first packet: one sync-loss episode
    ++f.sync_loss_events;
    const uint64_t n = std::max<uint64_t>(1, (garbage + stride - 1) / stride);
    add_diag(f, DiagCode::CNT_TS_1001,
             "TS_sync_loss: `" + std::to_string(n) + "` consecutive packets without `0x47` at offset `0`", 0);
  }

  uint64_t pos = geometry->first_sync;
  uint64_t last_good = pos;
  bool in_run = false;
  std::array<uint8_t, kPacketBytes> buf{};
  while (pos + kPacketBytes <= size) {
    if (src.at(pos) == kSyncByte) {
      if (src.copy(pos, buf.size(), buf.data()) != buf.size()) {
        break;
      }
      const auto h = decode_packet(buf);
      if (!h) {
        break;
      }
      if (!in_run) {
        f.runs.push_back({f.packet_count, pos, 0});
        in_run = true;
      }
      ++f.runs.back().count;
      const uint64_t k = f.packet_count;
      PidStats& st = stats[h->pid];
      if (st.packets == 0) {
        st.first_packet = k;
      }
      ++st.packets;
      if (h->has_payload()) {
        ++st.payload_packets;
      }
      if (h->transport_scrambling_control != 0) {
        ++st.scrambled_packets;
      }
      if (h->transport_error_indicator) {
        ++st.transport_errors;
        add_diag(f, DiagCode::CNT_TS_2001,
                 "Transport_error: `transport_error_indicator` set at packet `" + std::to_string(k) + "`", pos);
        // The header of a packet with the error indicator is unreliable: the PID restarts its counter afterwards.
        cc[h->pid] = CcState{};
      } else if (h->pid != kNullPid && h->adaptation_field_control != 0) {
        CcState& c = cc[h->pid];
        const bool discontinuity = h->adaptation && h->adaptation->discontinuity_indicator;
        if (!c.valid || discontinuity) {
          c = {true, false, h->continuity_counter};
        } else {
          const uint8_t expected = h->has_payload() ? static_cast<uint8_t>((c.last + 1U) & 15U) : c.last;
          if (h->continuity_counter == expected) {
            c.last = h->continuity_counter;
            c.dup_used = false;
          } else if (h->has_payload() && h->continuity_counter == c.last && !c.dup_used) {
            c.dup_used = true;  // one duplicate packet is allowed (13818-1 §2.4.3.3)
          } else {
            ++st.continuity_errors;
            add_diag(f, DiagCode::CNT_TS_1006,
                     "Continuity_count_error on PID `" + hex_pid(h->pid) + "` at packet `" + std::to_string(k) +
                         "` (`" + std::to_string(expected) + "`/`" + std::to_string(h->continuity_counter) + "`)",
                     pos);
            c.last = h->continuity_counter;
            c.dup_used = false;
          }
        }
      }
      ++f.packet_count;
      last_good = pos;
      pos += stride;
      continue;
    }

    // The sync byte is missing at the expected position.
    in_run = false;
    const uint64_t next = pos + stride;
    const bool next_fits = next + kPacketBytes <= size;
    const bool after_fits = next + stride + kPacketBytes <= size;
    if (next_fits && src.at(next) == kSyncByte && (!after_fits || src.at(next + stride) == kSyncByte)) {
      ++f.sync_byte_errors;  // isolated: the grid continues after this slot
      add_diag(f, DiagCode::CNT_TS_1002, "Sync_byte_error at packet `" + std::to_string(f.packet_count) + "`", pos);
      pos = next;
      continue;
    }
    ++f.sync_loss_events;
    std::optional<uint64_t> found;
    for (uint64_t q = last_good + 1; q + kPacketBytes <= size; ++q) {
      if (src.at(q) == kSyncByte && pattern_at(src, q, stride, size)) {
        found = q;
        break;
      }
    }
    if (found) {
      const uint64_t skipped = *found > pos ? (*found - pos + stride - 1) / stride : 0;
      add_diag(f, DiagCode::CNT_TS_1001,
               "TS_sync_loss: `" + std::to_string(std::max<uint64_t>(1, skipped)) +
                   "` consecutive packets without `0x47` at offset `" + std::to_string(pos) + "`",
               pos);
      pos = *found;
      continue;
    }
    add_diag(f, DiagCode::CNT_TS_1001,
             "TS_sync_loss: `" + std::to_string(std::max<uint64_t>(1, (size - pos) / stride)) +
                 "` consecutive packets without `0x47` at offset `" + std::to_string(pos) + "`",
             pos);
    break;
  }

  if (f.packet_count == 0) {
    return std::nullopt;
  }
  for (const PidStats& st : stats) {
    if (st.packets > 0) {
      f.pids.push_back(st);
    }
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
    const std::span<const uint8_t> body(raw.data() + prefix, got > prefix ? got - prefix : 0);
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
    fields.push_back(uint_field(fbb, "transport_error_indicator", h.transport_error_indicator, b + 8, 1));
    fields.push_back(uint_field(fbb, "payload_unit_start_indicator", h.payload_unit_start_indicator, b + 9, 1));
    fields.push_back(uint_field(fbb, "transport_priority", h.transport_priority, b + 10, 1));
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
      fields.push_back(uint_field(fbb, "discontinuity_indicator", a.discontinuity_indicator, 8, 1));
      fields.push_back(uint_field(fbb, "random_access_indicator", a.random_access_indicator, 9, 1));
      fields.push_back(
          uint_field(fbb, "elementary_stream_priority_indicator", a.elementary_stream_priority_indicator, 10, 1));
      fields.push_back(uint_field(fbb, "PCR_flag", a.pcr_flag, 11, 1));
      fields.push_back(uint_field(fbb, "OPCR_flag", a.opcr_flag, 12, 1));
      fields.push_back(uint_field(fbb, "splicing_point_flag", a.splicing_point_flag, 13, 1));
      fields.push_back(uint_field(fbb, "transport_private_data_flag", a.transport_private_data_flag, 14, 1));
      fields.push_back(uint_field(fbb, "adaptation_field_extension_flag", a.adaptation_field_extension_flag, 15, 1));
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
