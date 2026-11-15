// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of PES reassembly, the per-PID SampleTable and the PCR/PTS timing table of the MPEG-TS reader (chapter 08
// §8.6.7, WI-4.206a and WI-4.206b).

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/diagnostics.h"
#include "container/mpegts/pes.hpp"
#include "container/mpegts/psi.hpp"
#include "container/mpegts/timing.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace ts = ovt::container::mpegts;
using ovt::container::DiagCode;

constexpr uint16_t kPmtPid = 0x0100;
constexpr uint16_t kVideoPid = 0x0101;
constexpr uint16_t kAudioPid = 0x0102;

// One 188-byte packet; a payload shorter than the space left is padded with adaptation-field stuffing.
Bytes packet(uint16_t pid, bool pusi, uint8_t cc, std::span<const uint8_t> payload, Bytes af = {}) {
  const size_t room = 184 - (af.empty() ? 0 : 1 + af.size());
  const bool stuff = payload.size() < room;
  const bool has_af = !af.empty() || stuff;
  Bytes out{ts::kSyncByte, static_cast<uint8_t>((pusi ? 0x40U : 0U) | (pid >> 8U)), static_cast<uint8_t>(pid),
            static_cast<uint8_t>(((has_af ? 2U : 0U) | (payload.empty() ? 0U : 1U)) << 4U | (cc & 15U))};
  if (has_af) {
    const size_t len = 183 - payload.size();
    if (len > 0 && af.empty()) {
      af.push_back(0x00);
    }
    af.resize(len, 0xFF);
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), af.begin(), af.end());
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes pcr_af(uint64_t pcr, uint8_t flags = 0) {
  const uint64_t base = pcr / 300;
  const auto ext = static_cast<uint16_t>(pcr % 300);
  return {static_cast<uint8_t>(0x10U | flags),
          static_cast<uint8_t>(base >> 25U),
          static_cast<uint8_t>(base >> 17U),
          static_cast<uint8_t>(base >> 9U),
          static_cast<uint8_t>(base >> 1U),
          static_cast<uint8_t>(((base & 1U) << 7U) | 0x7EU | (ext >> 8U)),
          static_cast<uint8_t>(ext)};
}

Bytes with_crc(Bytes s) {
  const uint32_t crc = ts::crc32_mpeg2(s);
  for (int i = 3; i >= 0; --i) {
    s.push_back(static_cast<uint8_t>(crc >> (8U * static_cast<unsigned>(i))));
  }
  return s;
}

// PAT (program 1 → kPmtPid) and a PMT with the given (pid, stream_type) list, each in one packet.
Bytes psi_packets(uint16_t pcr_pid, const std::vector<std::pair<uint16_t, uint8_t>>& streams) {
  const Bytes pat = with_crc({0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00});
  Bytes pmt{0x02,
            0xB0,
            static_cast<uint8_t>(13 + 5 * streams.size()),
            0x00,
            0x01,
            0xC1,
            0x00,
            0x00,
            static_cast<uint8_t>(0xE0U | (pcr_pid >> 8U)),
            static_cast<uint8_t>(pcr_pid),
            0xF0,
            0x00};
  for (const auto& [pid, type] : streams) {
    pmt.insert(pmt.end(), {type, static_cast<uint8_t>(0xE0U | (pid >> 8U)), static_cast<uint8_t>(pid), 0xF0, 0x00});
  }
  Bytes a{0x00};
  a.insert(a.end(), pat.begin(), pat.end());
  Bytes b{0x00};
  const Bytes pmt_crc = with_crc(pmt);
  b.insert(b.end(), pmt_crc.begin(), pmt_crc.end());
  Bytes out = packet(0x0000, true, 0, a);
  const Bytes p2 = packet(kPmtPid, true, 0, b);
  out.insert(out.end(), p2.begin(), p2.end());
  return out;
}

void put_ts(Bytes& b, uint8_t prefix, int64_t v) {
  const auto u = static_cast<uint64_t>(v);
  b.push_back(static_cast<uint8_t>((uint64_t{prefix} << 4U) | ((u >> 29U) & 0x0EU) | 1U));
  b.push_back(static_cast<uint8_t>(u >> 22U));
  b.push_back(static_cast<uint8_t>(((u >> 14U) & 0xFEU) | 1U));
  b.push_back(static_cast<uint8_t>(u >> 7U));
  b.push_back(static_cast<uint8_t>(((u << 1U) & 0xFEU) | 1U));
}

// A PES packet with optional PTS / DTS; `bounded` writes PES_packet_length, otherwise 0.
Bytes pes(const Bytes& payload, int64_t pts, int64_t dts = -1, bool bounded = true, uint8_t stream_id = 0xE0) {
  Bytes opt;
  if (pts >= 0) {
    put_ts(opt, dts >= 0 ? 3 : 2, pts);
  }
  if (dts >= 0) {
    put_ts(opt, 1, dts);
  }
  const size_t len = bounded ? 3 + opt.size() + payload.size() : 0;
  Bytes out{0x00,
            0x00,
            0x01,
            stream_id,
            static_cast<uint8_t>(len >> 8U),
            static_cast<uint8_t>(len),
            0x84,
            static_cast<uint8_t>(pts < 0 ? 0x00 : (dts >= 0 ? 0xC0 : 0x80)),
            static_cast<uint8_t>(opt.size())};
  out.insert(out.end(), opt.begin(), opt.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Splits a PES into packets of `pid`; `af0` is the adaptation field of the first packet.
void packetize(Bytes& out, uint16_t pid, uint8_t& cc, const Bytes& p, const Bytes& af0 = {}) {
  size_t pos = 0;
  bool first = true;
  while (pos < p.size()) {
    const Bytes af = first ? af0 : Bytes{};
    const size_t room = 184 - (af.empty() ? 0 : 1 + af.size());
    const size_t take = std::min(room, p.size() - pos);
    const Bytes pk = packet(pid, first, cc, std::span<const uint8_t>(p).subspan(pos, take), af);
    out.insert(out.end(), pk.begin(), pk.end());
    cc = static_cast<uint8_t>((cc + 1U) & 15U);
    pos += take;
    first = false;
  }
}

// An Annex B payload of `n` bytes: AUD, SPS-like and slice-like NAL units, then filler.
Bytes annexb(size_t n, uint8_t seed) {
  Bytes b{0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01, 0x65};
  for (size_t i = b.size(); i < n; ++i) {
    b.push_back(static_cast<uint8_t>(0x80U | ((i * 7U + seed) & 0x3FU)));
  }
  return b;
}

std::optional<ts::File> parse_bytes(const Bytes& b) {
  const ovt::io::MemoryFile file{std::span<const uint8_t>(b)};
  return ts::parse(file);
}

size_t count_code(const ts::File& f, DiagCode code) {
  return static_cast<size_t>(std::count_if(f.diagnostics.begin(), f.diagnostics.end(),
                                           [code](const ts::Diagnostic& d) { return d.code == code; }));
}

const ts::PesStream* stream_of(const ts::File& f, uint16_t pid) {
  const auto it = std::find_if(f.pes.begin(), f.pes.end(), [pid](const ts::PesStream& s) { return s.pid == pid; });
  return it == f.pes.end() ? nullptr : &*it;
}

// PCR-only packets on `pid` every `every` packets (1 ms per packet: 1 504 000 bit/s), null packets between.
Bytes pcr_stream(size_t packets, size_t every, uint16_t pid, const std::vector<std::pair<size_t, int64_t>>& tweak = {},
                 size_t gap_from = SIZE_MAX, size_t gap_to = 0) {
  Bytes out;
  uint8_t null_cc = 0;
  for (size_t k = 0; k < packets; ++k) {
    int64_t pcr = static_cast<int64_t>(k) * 27000;
    for (const auto& [at, delta] : tweak) {
      pcr += at == k ? delta : 0;
    }
    const bool gap = k >= gap_from && k < gap_to;
    const Bytes pk = k % every == 0 && !gap ? packet(pid, false, 0, {}, pcr_af(static_cast<uint64_t>(pcr)))
                                            : packet(ts::kNullPid, false, null_cc, Bytes(184, 0xAB));
    out.insert(out.end(), pk.begin(), pk.end());
  }
  return out;
}

}  // namespace

TEST_CASE("decode_pes_header: timestamps, ESCR and malformed headers", "[container][mpegts][pes][fast]") {
  ts::PesHeader h;
  const Bytes a = pes({0xAA}, 0x1FFFFFFFF, 0x123456789);
  REQUIRE(ts::decode_pes_header(a, h) == ts::PesParse::Ok);
  CHECK(h.stream_id == 0xE0);
  CHECK(h.packet_length == 14);
  CHECK(h.pts == 0x1FFFFFFFF);  // all 33 bits
  CHECK(h.dts == 0x123456789);
  CHECK(h.payload_offset == 19);
  CHECK(h.data_alignment_indicator);
  CHECK_FALSE(h.syntax_error);

  // Too short for the announced header, not a start code, a stream without the optional header.
  CHECK(ts::decode_pes_header(std::span<const uint8_t>(a).first(12), h) == ts::PesParse::NeedMore);
  CHECK(ts::decode_pes_header(std::span<const uint8_t>(a).first(2), h) == ts::PesParse::NeedMore);
  const Bytes not_pes = {0x00, 0x00, 0x02, 0xE0, 0x00, 0x00};
  CHECK(ts::decode_pes_header(not_pes, h) == ts::PesParse::NotPes);
  const Bytes padding = {0x00, 0x00, 0x01, 0xBE, 0x00, 0x02, 0xFF, 0xFF};
  REQUIRE(ts::decode_pes_header(padding, h) == ts::PesParse::Ok);
  CHECK_FALSE(h.has_optional_header);
  CHECK(h.payload_offset == 6);

  // ESCR (base 0x100000001, extension 0x155) after the PTS.
  Bytes e = {0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x80, 0xA0, 11};
  put_ts(e, 2, 90000);
  const uint64_t base = 0x100000001;
  e.insert(
      e.end(),
      {static_cast<uint8_t>(((base >> 27U) & 0x38U) | 0x04U | ((base >> 28U) & 3U)), static_cast<uint8_t>(base >> 20U),
       static_cast<uint8_t>(((base >> 12U) & 0xF8U) | 0x04U | ((base >> 13U) & 3U)), static_cast<uint8_t>(base >> 5U),
       static_cast<uint8_t>(((base << 3U) & 0xF8U) | 0x04U | 0x02U), static_cast<uint8_t>((0x55U << 1U) | 1U)});
  REQUIRE(ts::decode_pes_header(e, h) == ts::PesParse::Ok);
  CHECK(h.pts == 90000);
  CHECK(h.escr_base == static_cast<int64_t>(base));
  CHECK(h.escr_extension == 0x155);
  CHECK_FALSE(h.syntax_error);

  // A broken marker bit and a header_data_length too short for the PTS.
  Bytes bad = pes({}, 1000);
  bad[13] &= 0xFE;
  REQUIRE(ts::decode_pes_header(bad, h) == ts::PesParse::Ok);
  CHECK(h.syntax_error);
  Bytes shrt = {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x80, 0x02, 0x21, 0x00};
  REQUIRE(ts::decode_pes_header(shrt, h) == ts::PesParse::Ok);
  CHECK(h.syntax_error);
  CHECK(h.pts == -1);
}

TEST_CASE("parse: PES reassembly into samples", "[container][mpegts][pes][fast]") {
  Bytes file;
  uint8_t vcc = 0;
  uint8_t acc = 0;
  // A PES on the video PID before the PMT is collected; one on a PID that no PMT names is not.
  packetize(file, kVideoPid, vcc, pes(annexb(100, 9), 1000));
  uint8_t other_cc = 0;
  packetize(file, 0x0300, other_cc, pes(annexb(100, 9), 1000));
  const Bytes psi = psi_packets(kVideoPid, {{kVideoPid, 0x1B}, {kAudioPid, 0x0F}});
  file.insert(file.end(), psi.begin(), psi.end());
  const std::vector<Bytes> payloads = {annexb(500, 1), annexb(150, 2), annexb(1000, 3)};
  packetize(file, kVideoPid, vcc, pes(payloads[0], 9000, 6000, false), pcr_af(0, 0x40));  // unbounded, RAI
  packetize(file, kAudioPid, acc, pes(Bytes(300, 0x11), 9500, -1, true, 0xC0));
  packetize(file, kVideoPid, vcc, pes(payloads[1], 18000, 9000, true));
  // A duplicate of the next packet is dropped from the payload.
  const size_t dup_at = file.size();
  packetize(file, kVideoPid, vcc, pes(payloads[2], 12000, 12000, false));
  Bytes dup(file.begin() + static_cast<std::ptrdiff_t>(dup_at + 188),
            file.begin() + static_cast<std::ptrdiff_t>(dup_at + 376));
  file.insert(file.begin() + static_cast<std::ptrdiff_t>(dup_at + 376), dup.begin(), dup.end());

  const auto f = parse_bytes(file);
  REQUIRE(f);
  REQUIRE(f->pes.size() == 2);
  const ts::PesStream* v = stream_of(*f, kVideoPid);
  REQUIRE(v != nullptr);
  CHECK(v->stream_type == 0x1B);
  REQUIRE(v->packets.size() == 4);
  CHECK(v->packets[0].pts == 1000);
  CHECK(v->packets[0].size == 100);
  const std::vector<ts::PesPacket> vp(v->packets.begin() + 1, v->packets.end());
  CHECK(vp[0].pts == 9000);
  CHECK(vp[0].dts == 6000);
  CHECK(vp[0].random_access);
  CHECK_FALSE(vp[1].random_access);
  CHECK(vp[1].dts == 9000);
  const ovt::io::MemoryFile mf{std::span<const uint8_t>(file)};
  for (size_t i = 0; i < 3; ++i) {
    CAPTURE(i);
    CHECK(vp[i].complete);
    CHECK(vp[i].size == payloads[i].size());
    CHECK(file[vp[i].offset] == 0x00);  // the payload starts with the AUD start code
    CHECK(ts::read_pes_payload(mf, *f, kVideoPid, vp[i]) == payloads[i]);
  }
  // `units` of a sample: the three NAL units of the payload.
  std::vector<ovt::container::NalSpan> nals;
  ts::pes_units(mf, *f, *v, vp[1], [&nals](const ovt::container::NalSpan& n) { nals.push_back(n); });
  REQUIRE(nals.size() == 3);
  CHECK(nals[0].header_byte == 0x09);
  CHECK(nals[2].header_byte == 0x65);
  CHECK(nals[2].file_span.end() == payloads[1].size());

  const ts::PesStream* a = stream_of(*f, kAudioPid);
  REQUIRE(a != nullptr);
  REQUIRE(a->packets.size() == 1);
  CHECK(a->packets[0].stream_id == 0xC0);
  CHECK(a->packets[0].size == 300);
  CHECK(a->packets[0].dts == -1);
}

TEST_CASE("parse: cut and split PES packets", "[container][mpegts][pes][fast]") {
  Bytes file = psi_packets(0x1FFF, {{kVideoPid, 0x24}});
  uint8_t cc = 0;
  // A bounded PES cut short by the next PES, then a continuity error inside an unbounded one.
  Bytes cut = pes(Bytes(400, 0x22), 0);
  cut.resize(250);
  packetize(file, kVideoPid, cc, cut);
  packetize(file, kVideoPid, cc, pes(Bytes(600, 0x33), 3600, -1, false));
  file.erase(file.end() - std::ptrdiff_t{2} * 188, file.end() - 188);  // drop the middle packet of three (CC error)
  // A header split over two packets: 180 bytes of adaptation field leave 3 header bytes in the first packet.
  const Bytes split = pes(Bytes(50, 0x44), 7200);
  const Bytes p1 = packet(kVideoPid, true, cc, std::span<const uint8_t>(split).first(3));
  const Bytes p2 = packet(kVideoPid, false, static_cast<uint8_t>(cc + 1U), std::span<const uint8_t>(split).subspan(3));
  file.insert(file.end(), p1.begin(), p1.end());
  file.insert(file.end(), p2.begin(), p2.end());

  const auto f = parse_bytes(file);
  REQUIRE(f);
  const ts::PesStream* v = stream_of(*f, kVideoPid);
  REQUIRE(v != nullptr);
  REQUIRE(v->packets.size() == 3);
  CHECK_FALSE(v->packets[0].complete);
  CHECK(v->packets[0].size == 250 - 14);
  CHECK_FALSE(v->packets[1].complete);
  CHECK(v->packets[1].size == 600 - 184);
  CHECK(v->packets[2].complete);
  CHECK(v->packets[2].pts == 7200);
  CHECK(v->packets[2].size == 50);
  CHECK(v->packets[2].offset == 8 * 188 + (188 - 61) + (14 - 3));  // inside the second packet of the split PES
  CHECK(count_code(*f, DiagCode::CNT_TS_1006) == 1);
}

TEST_CASE("sample table: timescale, unwrapped DTS, composition offsets and windows", "[container][mpegts][pes][fast]") {
  ts::PesStream s;
  s.pid = kVideoPid;
  const int64_t wrap = int64_t{1} << 33;
  const std::vector<std::pair<int64_t, int64_t>> pd = {
      {wrap - 3600, wrap - 7200}, {3600, -1}, {7200, 3600}, {-1, -1}, {18000, 10800}};
  for (size_t i = 0; i < pd.size(); ++i) {
    ts::PesPacket p;
    p.pts = pd[i].first;
    p.dts = pd[i].second;
    p.offset = 1000 * i;
    p.size = static_cast<uint32_t>(10 + i);
    p.random_access = i == 0;
    s.packets.push_back(p);
  }
  const auto buf = ts::build_sample_table(s, 0, 0);
  const auto* t = flatbuffers::GetRoot<ovt::fb::SampleTable>(buf.data());
  CHECK(t->track_id() == kVideoPid);
  CHECK(t->timescale() == 90000);
  REQUIRE(t->count() == 5);
  const std::vector<int64_t> dts = {wrap - 7200, wrap + 3600, wrap + 3600, wrap + 3600, wrap + 10800};
  const std::vector<int32_t> cts = {3600, 0, 3600, 0, 7200};
  const std::vector<uint32_t> dur = {10800, 0, 0, 7200, 7200};
  for (uint32_t i = 0; i < 5; ++i) {
    CAPTURE(i);
    CHECK(t->dts()->Get(i) == dts[i]);
    CHECK(t->cts_offset()->Get(i) == cts[i]);
    CHECK(t->duration()->Get(i) == dur[i]);
    CHECK(t->offset()->Get(i) == uint64_t{1000} * i);
    CHECK(t->size()->Get(i) == 10 + i);
    CHECK(t->is_sync()->Get(i) == (i == 0));
  }
  // A window carries the same values as the whole table.
  const auto win = ts::build_sample_table(s, 3, 1);
  const auto* w = flatbuffers::GetRoot<ovt::fb::SampleTable>(win.data());
  REQUIRE(w->count() == 1);
  CHECK(w->dts()->Get(0) == dts[3]);
  CHECK(w->duration()->Get(0) == dur[3]);
  CHECK(flatbuffers::GetRoot<ovt::fb::SampleTable>(ts::build_sample_table(s, 9, 0).data())->count() == 0);
}

TEST_CASE("reader: one track per PES stream with its sample table", "[container][mpegts][pes][fast]") {
  Bytes file = psi_packets(0x1FFF, {{kVideoPid, 0x1B}});
  uint8_t cc = 0;
  for (const int64_t pts : {0, 10800, 3600, 7200, 18000}) {
    packetize(file, kVideoPid, cc, pes(Bytes(100, 0x55), pts));
  }
  const ovt::io::MemoryFile mf{std::span<const uint8_t>(file)};
  const auto reader = ts::make_reader(mf);
  REQUIRE(reader != nullptr);
  CHECK(reader->track_count() == 1);
  const auto tree = reader->tree();
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data());
  REQUIRE(t->tracks()->size() == 1);
  CHECK(t->tracks()->Get(0)->track_id() == kVideoPid);
  CHECK(t->tracks()->Get(0)->codec() == ovt::fb::Codec::H264);
  CHECK(t->tracks()->Get(0)->handler() == ovt::fb::HandlerType::Video);
  CHECK(t->tracks()->Get(0)->timescale() == 90000);
  CHECK(t->tracks()->Get(0)->sample_count() == 5);
  const auto table = reader->sample_table(0, 1, 2);
  const auto* st = flatbuffers::GetRoot<ovt::fb::SampleTable>(table.data());
  REQUIRE(st->count() == 2);
  CHECK(st->dts()->Get(0) == 10800);
  CHECK(st->offset()->Get(1) == 4 * 188 + (188 - 114) + 14);  // 114 PES bytes after the stuffing
  CHECK(reader->sample_table(1, 0, 0).empty());
  CHECK(reader->sample_table(-1, 0, 0).empty());
}

TEST_CASE("corpus: ts_h264_dvb PES count and timestamps", "[container][mpegts][pes][needs-corpus][fast]") {
  // Reference: ffprobe 6.1.1 `-show_packets` gives 916 packets of 323 bytes on PID 0x42C at the positions of the
  // PES start packets; it prints no PTS because libavformat drops teletext PTS without a PCR (this excerpt has
  // none), so the PTS values come from an independent decode. See docs/acceptance/wi/WI-4.206.md.
  const auto path = ovt::test::corpus_or_skip("ts_h264_dvb");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  REQUIRE(f->pes.size() == 1);
  const ts::PesStream& s = f->pes[0];
  CHECK(s.pid == 0x42C);
  REQUIRE(s.packets.size() == 916);
  uint64_t pos_sum = 0;
  int64_t pts_sum = 0;
  for (const ts::PesPacket& p : s.packets) {
    CHECK(p.size == 323);
    CHECK(p.dts == -1);
    pos_sum += f->packet_offset(p.first_packet);
    pts_sum += p.pts;
  }
  CHECK(pos_sum == 170894444);
  CHECK(s.packets.front().pts == 3856608233);
  CHECK(s.packets.back().pts == 3859902233);
  CHECK(pts_sum == 3534161793428);
}

TEST_CASE("corpus: ts_test_025 PTS/DTS per PES equal the ffprobe golden",
          "[container][mpegts][pes][needs-corpus][fast]") {
  // AT1 with B-frames: tests/golden/mpegts/ts_test_025.ffprobe-packets.json is ffprobe 6.1.1 `-show_packets` (PID,
  // PTS, DTS and position of every packet) of tsduck-test input/test-025.ts (MPEG-2 video with B-frames and MPEG-1
  // audio). ffprobe prints a missing DTS as the PTS; the packet sizes are left out because the MPEG-2 video parser
  // moves a few bytes between pictures.
  const auto path = ovt::test::corpus_or_skip("ts_test_025");
  std::ifstream in(std::string(OVT_TEST_SOURCE_DIR) + "/tests/golden/mpegts/ts_test_025.ffprobe-packets.json");
  REQUIRE(in);
  const auto golden = nlohmann::json::parse(in)["packets"];
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  struct Row {
    uint64_t pos;
    uint16_t pid;
    int64_t pts;
    int64_t dts;
  };
  std::vector<Row> ours;
  for (const ts::PesStream& s : f->pes) {
    for (const ts::PesPacket& p : s.packets) {
      ours.push_back({f->packet_offset(p.first_packet), s.pid, p.pts, p.dts >= 0 ? p.dts : p.pts});
    }
  }
  std::sort(ours.begin(), ours.end(), [](const Row& a, const Row& b) { return a.pos < b.pos; });
  REQUIRE(ours.size() == golden.size());
  size_t with_dts = 0;
  for (size_t i = 0; i < ours.size(); ++i) {
    CAPTURE(i);
    CHECK(ours[i].pos == golden[i]["pos"].get<uint64_t>());
    CHECK(ours[i].pid == golden[i]["pid"].get<uint16_t>());
    CHECK(ours[i].pts == golden[i]["pts"].get<int64_t>());
    CHECK(ours[i].dts == golden[i]["dts"].get<int64_t>());
    with_dts += ours[i].dts != ours[i].pts ? 1U : 0U;
  }
  CHECK(with_dts == 25);  // the B-frame reordering is exercised
}

TEST_CASE("timing: PCR repetition, discontinuity and accuracy checks", "[container][mpegts][timing][fast]") {
  SECTION("a clean 20 ms PCR raises nothing") {
    const auto f = parse_bytes(pcr_stream(1000, 20, 0x0200));
    REQUIRE(f);
    REQUIRE(f->timing.pcr.size() == 1);
    CHECK(f->timing.pcr[0].points.size() == 50);
    CHECK(f->pcr_pid == 0x0200);
    CHECK(count_code(*f, DiagCode::CNT_TS_2003) == 0);
    CHECK(count_code(*f, DiagCode::CNT_TS_2004) == 0);
    CHECK(count_code(*f, DiagCode::CNT_TS_2005) == 0);
  }
  SECTION("a 1 s gap of missing PCRs is a repetition error") {
    const auto f = parse_bytes(pcr_stream(2000, 20, 0x0200, {}, 500, 1500));
    REQUIRE(f);
    CHECK(count_code(*f, DiagCode::CNT_TS_2003) == 1);
    CHECK(count_code(*f, DiagCode::CNT_TS_2004) == 1);  // TR 101 290 2.3b: also more than 100 ms
    CHECK(f->timing.pcr[0].repetition_errors == 1);
    const auto it = std::find_if(f->diagnostics.begin(), f->diagnostics.end(),
                                 [](const ts::Diagnostic& d) { return d.code == DiagCode::CNT_TS_2003; });
    CHECK(it->message == "PCR_repetition_error: `1020` ms between PCRs on PID `0x0200` (> 40 ms)");
    CHECK(it->offset == uint64_t{1500} * 188);
  }
  SECTION("a jump back is a discontinuity error unless discontinuity_indicator is set") {
    const auto f = parse_bytes(pcr_stream(200, 20, 0x0200, {{100, -27000 * 90}, {120, -27000 * 90}}));
    REQUIRE(f);
    CHECK(count_code(*f, DiagCode::CNT_TS_2004) == 2);  // back at 100, and forward again at 140
    Bytes b = pcr_stream(
        200, 20, 0x0200,
        {{100, -27000 * 90}, {120, -27000 * 90}, {140, -27000 * 90}, {160, -27000 * 90}, {180, -27000 * 90}});
    b[100 * 188 + 5] |= 0x80U;  // discontinuity_indicator
    const auto g = parse_bytes(b);
    REQUIRE(g);
    CHECK(count_code(*g, DiagCode::CNT_TS_2004) == 0);
    CHECK(count_code(*g, DiagCode::CNT_TS_2003) == 0);
  }
  SECTION("a PCR 740 ns off the line through its neighbours") {
    const auto f = parse_bytes(pcr_stream(400, 20, 0x0200, {{200, 20}}));
    REQUIRE(f);
    REQUIRE(count_code(*f, DiagCode::CNT_TS_2005) == 1);
    const auto it = std::find_if(f->diagnostics.begin(), f->diagnostics.end(),
                                 [](const ts::Diagnostic& d) { return d.code == DiagCode::CNT_TS_2005; });
    CHECK(it->message == "PCR_accuracy_error: `741` ns deviation on PID `0x0200` (> ±500 ns)");
    CHECK(it->offset == uint64_t{200} * 188);
  }
}

TEST_CASE("timing: PTS_error and the TsTiming table", "[container][mpegts][timing][fast]") {
  Bytes file = psi_packets(0x0200, {{kVideoPid, 0x1B}});
  uint8_t cc = 0;
  // Reordered PTS (no error), then a 900 ms step (one CNT-TS-2006).
  for (const int64_t pts : {0, 10800, 3600, 7200, 10800 + 81000}) {
    packetize(file, kVideoPid, cc, pes(Bytes(100, 0x55), pts));
  }
  const Bytes pcr = pcr_stream(100, 20, 0x0200);
  file.insert(file.end(), pcr.begin(), pcr.end());
  const auto f = parse_bytes(file);
  REQUIRE(f);
  REQUIRE(count_code(*f, DiagCode::CNT_TS_2006) == 1);
  CHECK(f->timing.pts_errors == 1);
  const auto it = std::find_if(f->diagnostics.begin(), f->diagnostics.end(),
                               [](const ts::Diagnostic& d) { return d.code == DiagCode::CNT_TS_2006; });
  CHECK(it->message == "PTS_error: `900` ms between PTS on PID `0x0101` (> 700 ms)");

  const ovt::io::MemoryFile mf{std::span<const uint8_t>(file)};
  const auto reader = ts::make_reader(mf);
  REQUIRE(reader != nullptr);
  CHECK(reader->track_count() == 1);
  const auto tree = reader->tree();
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data());
  REQUIRE(t->tracks()->size() == 1);
  CHECK(t->tracks()->Get(0)->track_id() == kVideoPid);
  CHECK(t->tracks()->Get(0)->codec() == ovt::fb::Codec::H264);
  CHECK(t->tracks()->Get(0)->handler() == ovt::fb::HandlerType::Video);
  CHECK(t->tracks()->Get(0)->sample_count() == 5);
  CHECK(!reader->sample_table(0, 0, 0).empty());
  CHECK(reader->sample_table(1, 0, 0).empty());

  const auto timing = reader->ts_timing();
  const auto* tt = flatbuffers::GetRoot<ovt::fb::TsTiming>(timing.data());
  CHECK(tt->pcr_pid() == 0x0200);
  CHECK(tt->packet_size() == 188);
  REQUIRE(tt->pcr()->size() == 5);
  CHECK(tt->pcr()->Get(1)->pcr() == uint64_t{20} * 27000);
  REQUIRE(tt->pts_dts()->size() == 5);
  CHECK(tt->pts_dts()->Get(1)->pts() == 10800);
  CHECK(tt->pts_dts()->Get(1)->dts() == -1);
  CHECK(tt->pts_dts()->Get(1)->packet_index() == 3);
}

TEST_CASE("timing: bitrate per PID in 10/100/250/500 ms windows", "[container][mpegts][timing][fast]") {
  // 1 000 packets at 1 ms each: PID 0x0200 every 20th packet, the null PID otherwise.
  const Bytes b = pcr_stream(1000, 20, 0x0200);
  const auto f = parse_bytes(b);
  REQUIRE(f);
  const auto rates = ts::compute_bitrates(f->timing.packet_pid, f->timing.pcr[0].points, 188);
  REQUIRE(rates.size() == 8);
  CHECK(rates[0].pid == 0x0200);
  CHECK(rates[0].window_ms == 10);
  CHECK(rates[0].bps.size() == 100);
  CHECK(rates[1].window_ms == 100);
  REQUIRE(rates[1].bps.size() == 10);
  CHECK(std::all_of(rates[1].bps.begin(), rates[1].bps.end(), [](uint32_t v) { return v == 75200; }));
  CHECK(rates[5].pid == ts::kNullPid);
  CHECK(rates[5].window_ms == 100);
  CHECK(std::all_of(rates[5].bps.begin(), rates[5].bps.end(), [](uint32_t v) { return v == 1428800; }));
  CHECK(rates[3].window_ms == 500);
  CHECK(rates[3].bps.size() == 2);
  // Fewer than two PCRs: no time axis.
  CHECK(ts::compute_bitrates(f->timing.packet_pid, {f->timing.pcr[0].points[0]}, 188).empty());
}

TEST_CASE("corpus: syn_ts_cc_errors has a 1 s PCR gap", "[container][mpegts][timing][needs-corpus][fast]") {
  // Recipe 33 removes the PCRs of one second: one PCR interval of more than 1 s on the video PID.
  const auto path = ovt::test::corpus_or_skip("syn_ts_cc_errors");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  REQUIRE(f->timing.pcr.size() == 1);
  CHECK(f->pcr_pid == 0x0100);
  // ffmpeg's VBR muxer writes a PCR every 100 ms, so shorter repetition errors are expected too.
  CHECK(std::any_of(f->diagnostics.begin(), f->diagnostics.end(), [](const ts::Diagnostic& d) {
    constexpr std::string_view head = "PCR_repetition_error: `";
    return d.code == DiagCode::CNT_TS_2003 && d.message.starts_with(head) &&
           std::stoul(d.message.substr(head.size())) >= 1000;
  }));
}
