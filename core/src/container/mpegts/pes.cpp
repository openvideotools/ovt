// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PES header decoding and reassembly (chapter 08 §8.6.7, WI-4.206; ISO/IEC 13818-1 §2.4.3.6–7).

#include "container/mpegts/pes.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "container/mpegts/ts_packets.hpp"
#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

// Stream ids whose PES packets carry no optional header (13818-1 Table 2-21, the first branch of PES_packet()).
bool without_optional_header(uint8_t id) {
  return id == 0xBC || id == 0xBE || id == 0xBF || id == 0xF0 || id == 0xF1 || id == 0xFF || id == 0xF2 || id == 0xF8;
}

// A 33-bit timestamp in the 5-byte '4 bits | 3 bits | marker | 15 bits | marker | 15 bits | marker' layout.
int64_t read_timestamp(std::span<const uint8_t> p, uint8_t prefix, bool& syntax_error) {
  if ((p[0] >> 4U) != prefix || (p[0] & 1U) == 0 || (p[2] & 1U) == 0 || (p[4] & 1U) == 0) {
    syntax_error = true;
  }
  const uint64_t v = (static_cast<uint64_t>((p[0] >> 1U) & 7U) << 30U) | (static_cast<uint64_t>(p[1]) << 22U) |
                     (static_cast<uint64_t>(p[2] >> 1U) << 15U) | (static_cast<uint64_t>(p[3]) << 7U) |
                     (static_cast<uint64_t>(p[4]) >> 1U);
  return static_cast<int64_t>(v);
}

void read_escr(std::span<const uint8_t> p, PesHeader& h) {
  const uint64_t base = (static_cast<uint64_t>((p[0] >> 3U) & 7U) << 30U) | (static_cast<uint64_t>(p[0] & 3U) << 28U) |
                        (static_cast<uint64_t>(p[1]) << 20U) | (static_cast<uint64_t>(p[2] >> 3U) << 15U) |
                        (static_cast<uint64_t>(p[2] & 3U) << 13U) | (static_cast<uint64_t>(p[3]) << 5U) |
                        (static_cast<uint64_t>(p[4]) >> 3U);
  h.escr_base = static_cast<int64_t>(base);
  h.escr_extension = static_cast<uint16_t>(((p[4] & 3U) << 7U) | (p[5] >> 1U));
  if ((p[0] & 4U) == 0 || (p[2] & 4U) == 0 || (p[4] & 4U) == 0 || (p[5] & 1U) == 0) {
    h.syntax_error = true;
  }
}

}  // namespace

PesParse decode_pes_header(std::span<const uint8_t> bytes, PesHeader& out) noexcept {
  out = PesHeader{};
  const size_t have = std::min<size_t>(bytes.size(), 3);
  if (!std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(have), kPesStartCode.begin())) {
    return PesParse::NotPes;
  }
  if (bytes.size() < 6) {
    return PesParse::NeedMore;
  }
  out.stream_id = bytes[3];
  out.packet_length = static_cast<uint16_t>((bytes[4] << 8U) | bytes[5]);
  if (without_optional_header(out.stream_id)) {
    return PesParse::Ok;
  }
  if (bytes.size() < 9) {
    return PesParse::NeedMore;
  }
  out.has_optional_header = true;
  out.syntax_error = (bytes[6] >> 6U) != 2U;
  out.scrambling_control = static_cast<uint8_t>((bytes[6] >> 4U) & 3U);
  out.data_alignment_indicator = (bytes[6] & 0x04U) != 0;
  out.pts_dts_flags = static_cast<uint8_t>(bytes[7] >> 6U);
  out.escr_flag = (bytes[7] & 0x20U) != 0;
  out.header_data_length = bytes[8];
  out.payload_offset = 9U + out.header_data_length;
  if (bytes.size() < out.payload_offset) {
    return PesParse::NeedMore;
  }
  const std::span<const uint8_t> opt = bytes.subspan(9, out.header_data_length);
  size_t pos = 0;
  if (out.pts_dts_flags == 1) {
    out.syntax_error = true;  // '01' is forbidden
  }
  if ((out.pts_dts_flags & 2U) != 0) {
    const bool both = out.pts_dts_flags == 3;
    if (pos + 5 <= opt.size()) {
      out.pts = read_timestamp(opt.subspan(pos, 5), both ? 3 : 2, out.syntax_error);
    } else {
      out.syntax_error = true;
    }
    pos += 5;
    if (both) {
      if (pos + 5 <= opt.size()) {
        out.dts = read_timestamp(opt.subspan(pos, 5), 1, out.syntax_error);
      } else {
        out.syntax_error = true;
      }
      pos += 5;
    }
  }
  if (out.escr_flag) {
    if (pos + 6 <= opt.size()) {
      read_escr(opt.subspan(pos, 6), out);
    } else {
      out.syntax_error = true;
    }
  }
  return PesParse::Ok;
}

PesAssembler::PesAssembler() : states_(kPidCount) {
  for (size_t i = 0; i < kPidCount; ++i) {
    states_[i].stream.pid = static_cast<uint16_t>(i);
  }
}

void PesAssembler::close(State& s, bool cut) {
  if (!s.active) {
    return;
  }
  if (!s.in_header) {  // a PES whose header never completed is not a sample
    s.cur.complete = s.cur.complete && (!cut || !s.bounded || s.cur.size >= s.expected);
    s.stream.packets.push_back(s.cur);
  }
  s.active = false;
  s.in_header = false;
  s.header.clear();
}

const PesPacket* PesAssembler::feed(uint16_t pid, bool payload_unit_start, bool random_access,
                                    uint8_t continuity_counter, std::span<const uint8_t> payload, uint64_t packet_index,
                                    uint64_t payload_offset) {
  State& s = states_[pid & 0x1FFFU];
  if (payload_unit_start && (!s.active || !s.dup_ok || continuity_counter != s.last_cc)) {
    close(s, true);
    s.active = true;
    s.in_header = true;
    s.cur = PesPacket{};
    s.cur.first_packet = packet_index;
    s.cur.last_packet = packet_index;
    s.cur.random_access = random_access;
    s.last_cc = continuity_counter;
    s.dup_ok = true;
    return take_header(s, payload, payload_offset, packet_index);
  }
  if (!s.active) {
    return nullptr;  // before the first PES start, or after a bounded PES was complete
  }
  if (s.dup_ok && continuity_counter == s.last_cc) {  // includes a repeated start packet
    s.dup_ok = false;                                 // a duplicate packet repeats the previous payload
    return nullptr;
  }
  s.last_cc = continuity_counter;
  s.dup_ok = true;
  if (s.in_header) {
    return take_header(s, payload, payload_offset, packet_index);
  }
  take_payload(s, payload, packet_index);
  return nullptr;
}

const PesPacket* PesAssembler::take_header(State& s, std::span<const uint8_t> payload, uint64_t payload_offset,
                                           uint64_t packet_index) {
  const size_t before = s.header.size();
  const size_t take = std::min(payload.size(), kMaxPesHeader - before);
  s.header.insert(s.header.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(take));
  PesHeader h;
  const PesParse r = decode_pes_header(s.header, h);
  if (r == PesParse::NotPes) {
    s.active = false;  // not a PES (for instance sections on a private PID): ignored until the next start
    s.in_header = false;
    s.header.clear();
    return nullptr;
  }
  if (r == PesParse::NeedMore) {
    return nullptr;
  }
  s.in_header = false;
  s.header.clear();
  s.cur.stream_id = h.stream_id;
  s.cur.pts = h.pts;
  s.cur.dts = h.dts;
  s.bounded = h.packet_length != 0;
  s.expected = h.packet_length + 6U > h.payload_offset ? h.packet_length + 6U - h.payload_offset : 0;
  const size_t used = h.payload_offset - before;  // header bytes inside this packet's payload
  s.cur.offset = payload_offset + used;
  take_payload(s, payload.subspan(std::min(used, payload.size())), packet_index);
  return &s.cur;
}

void PesAssembler::take_payload(State& s, std::span<const uint8_t> bytes, uint64_t packet_index) {
  uint64_t n = bytes.size();
  if (s.bounded) {
    n = std::min<uint64_t>(n, s.expected - s.cur.size);
  }
  if (n > 0) {
    s.cur.size = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, s.cur.size + n));
    s.cur.last_packet = packet_index;
  }
  if (s.bounded && s.cur.size >= s.expected) {
    close(s, false);
  }
}

void PesAssembler::damage(uint16_t pid) {
  State& s = states_[pid & 0x1FFFU];
  if (s.active) {
    s.cur.complete = false;
    s.dup_ok = false;
  }
}

std::vector<PesStream> PesAssembler::finish() {
  std::vector<PesStream> out;
  for (State& s : states_) {
    if (s.active && !s.in_header && s.bounded && s.cur.size < s.expected) {
      s.cur.complete = false;
    }
    close(s, false);
    if (!s.stream.packets.empty()) {
      out.push_back(std::move(s.stream));
    }
  }
  return out;
}

std::vector<uint8_t> read_pes_payload(const io::RandomAccessFile& file, const File& f, uint16_t pid,
                                      const PesPacket& pes) {
  std::vector<uint8_t> out;
  out.reserve(pes.size);
  bool dup_ok = false;
  uint8_t last_cc = 0;
  for (uint64_t k = pes.first_packet; k <= pes.last_packet && k < f.packet_count && out.size() < pes.size; ++k) {
    if (k < f.timing.packet_pid.size() && f.timing.packet_pid[k] != pid) {
      continue;  // another PID: no read needed
    }
    std::array<uint8_t, kPacketBytes> buf{};
    const uint64_t sync = f.sync_offset(k);
    const size_t got = file.read(sync, buf.size(), buf.data());
    const auto h = decode_packet(std::span<const uint8_t>(buf.data(), got));
    if (!h || h->pid != pid || !h->has_payload() || h->transport_error_indicator ||
        h->transport_scrambling_control != 0) {
      continue;
    }
    if (k != pes.first_packet && dup_ok && h->continuity_counter == last_cc) {
      dup_ok = false;  // the duplicate the assembler dropped
      continue;
    }
    dup_ok = true;
    last_cc = h->continuity_counter;
    const uint64_t start = sync + h->payload_offset;
    const uint64_t end = start + h->payload_size;
    if (end <= pes.offset) {
      continue;  // header bytes only
    }
    const auto from = static_cast<uint32_t>(std::max(start, pes.offset) - start);
    const size_t take = std::min<size_t>(h->payload_size - from, pes.size - out.size());
    const auto chunk = std::span<const uint8_t>(buf).subspan(h->payload_offset + from, take);
    out.insert(out.end(), chunk.begin(), chunk.end());
  }
  return out;
}

AnnexBSummary pes_units(const io::RandomAccessFile& file, const File& f, const PesStream& stream, const PesPacket& pes,
                        const NalCallback& on_nal) {
  AnnexBCodec codec = AnnexBCodec::None;
  if (stream.stream_type == 0x1B) {
    codec = AnnexBCodec::H264;
  } else if (stream.stream_type == 0x24) {
    codec = AnnexBCodec::H265;
  }
  const std::vector<uint8_t> payload = read_pes_payload(file, f, stream.pid, pes);
  return annexb_split(payload, 0, on_nal, {}, codec);
}

namespace {

// a - b of two 33-bit timestamps as the nearer signed distance.
int64_t timestamp_diff(int64_t a, int64_t b) {
  const auto d = static_cast<int64_t>((static_cast<uint64_t>(a) - static_cast<uint64_t>(b)) & (kTimestampWrap - 1));
  return d >= static_cast<int64_t>(kTimestampWrap / 2) ? d - static_cast<int64_t>(kTimestampWrap) : d;
}

}  // namespace

std::vector<uint8_t> build_sample_table(const PesStream& stream, uint32_t first, uint32_t count) {
  const size_t total = stream.packets.size();
  const size_t begin = std::min<size_t>(first, total);
  const size_t n = count == 0 ? total - begin : std::min<size_t>(count, total - begin);
  const size_t upto = std::min(total, begin + n + 1);  // one more for the last duration

  // Decode times unwrapped from the first PES, so a window gives the same values as the whole table.
  std::vector<int64_t> dts_all(upto, 0);
  std::vector<int32_t> cts_all(upto, 0);
  int64_t cur = 0;
  int64_t prev_raw = -1;
  for (size_t i = 0; i < upto; ++i) {
    const PesPacket& p = stream.packets[i];
    const int64_t raw = p.dts >= 0 ? p.dts : p.pts;
    if (raw >= 0) {
      cur = prev_raw < 0 ? raw : cur + timestamp_diff(raw, prev_raw);
      prev_raw = raw;
      if (p.pts >= 0) {
        cts_all[i] = static_cast<int32_t>(timestamp_diff(p.pts, raw));
      }
    }
    dts_all[i] = cur;
  }

  treefb::Fbb fbb;
  std::vector<int64_t> dts(n);
  std::vector<int32_t> cts_offset(n);
  std::vector<uint32_t> duration(n);
  std::vector<uint64_t> offset(n);
  std::vector<uint32_t> size(n);
  std::vector<uint8_t> sync(n);
  const std::vector<uint16_t> desc(n, 1);
  const std::vector<uint32_t> chunk(n, 0);
  const std::vector<int32_t> fragment(n, -1);
  for (size_t k = 0; k < n; ++k) {
    const size_t i = begin + k;
    const PesPacket& p = stream.packets[i];
    dts[k] = dts_all[i];
    cts_offset[k] = cts_all[i];
    int64_t d = 0;  // distance to the next decode time; the last sample repeats its predecessor's
    if (i + 1 < upto) {
      d = dts_all[i + 1] - dts_all[i];
    } else if (i > 0) {
      d = dts_all[i] - dts_all[i - 1];
    }
    duration[k] = static_cast<uint32_t>(std::clamp<int64_t>(d, 0, UINT32_MAX));
    offset[k] = p.offset;
    size[k] = p.size;
    sync[k] = p.random_access ? 1 : 0;
  }
  const auto table = fb::CreateSampleTable(fbb, stream.pid, 90000, n, fbb.CreateVector(dts),
                                           fbb.CreateVector(cts_offset), fbb.CreateVector(duration),
                                           fbb.CreateVector(offset), fbb.CreateVector(size), fbb.CreateVector(sync), 0,
                                           fbb.CreateVector(desc), fbb.CreateVector(chunk), fbb.CreateVector(fragment));
  fbb.Finish(table);
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

}  // namespace ovt::container::mpegts
