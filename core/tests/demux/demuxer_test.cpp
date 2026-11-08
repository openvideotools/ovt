// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt::demux::Demuxer (chapter 08 §8.7, WI-1.230). The fast cases build their inputs in memory
// (Y4M, IVF and a minimal ISOBMFF file with known sample offsets), so they need neither corpora nor FFmpeg
// tools. The ffprobe parity cases (acceptance test 1) run when ffprobe is on PATH: over the synthetic files
// always, over the corpus files that are present in the corpus cache otherwise skipped.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "demux/demuxer.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"

namespace {

using ovt::demux::Demuxer;
using ovt::demux::Packet;
using Bytes = std::vector<uint8_t>;

// ---- builders ----------------------------------------------------------------------------------------------------

void put(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
void put8(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
}
void put16(Bytes& b, uint32_t v) {
  put8(b, v >> 8U);
  put8(b, v);
}
void put32(Bytes& b, uint32_t v) {
  put16(b, v >> 16U);
  put16(b, v);
}
void put16le(Bytes& b, uint32_t v) {
  put8(b, v);
  put8(b, v >> 8U);
}
void put32le(Bytes& b, uint32_t v) {
  put16le(b, v);
  put16le(b, v >> 16U);
}
void zeros(Bytes& b, size_t n) {
  b.insert(b.end(), n, 0);
}

Bytes box(std::string_view type, const Bytes& payload) {
  Bytes b;
  put32(b, static_cast<uint32_t>(payload.size() + 8));
  put(b, type);
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// A 16x16 4:2:0 8-bit Y4M at `fps` frames per second (1/1 time base for fps == 1). Every frame is a key frame.
struct Y4m {
  Bytes bytes;
  std::vector<uint64_t> frame_offsets;  // offset of each "FRAME" marker
};
Y4m make_y4m(int frames, int fps = 1) {
  Y4m y;
  put(y.bytes, "YUV4MPEG2 W16 H16 F" + std::to_string(fps) + ":1 Ip A1:1 C420jpeg\n");
  for (int i = 0; i < frames; ++i) {
    y.frame_offsets.push_back(y.bytes.size());
    put(y.bytes, "FRAME\n");
    y.bytes.insert(y.bytes.end(), size_t{16 * 16 * 3 / 2}, static_cast<uint8_t>(i));
  }
  return y;
}

// An IVF file with fourcc VP80 and `frames` frames of arbitrary payload (libavformat does not decode to demux).
struct Ivf {
  Bytes bytes;
  std::vector<uint64_t> header_offsets;  // offset of each 12-byte frame header
  std::vector<uint32_t> sizes;
};
Ivf make_ivf(int frames) {
  Ivf v;
  put(v.bytes, "DKIF");
  put16le(v.bytes, 0);   // version
  put16le(v.bytes, 32);  // header size
  put(v.bytes, "VP80");
  put16le(v.bytes, 16);
  put16le(v.bytes, 16);
  put32le(v.bytes, 30);  // rate
  put32le(v.bytes, 1);   // scale
  put32le(v.bytes, static_cast<uint32_t>(frames));
  put32le(v.bytes, 0);
  for (int i = 0; i < frames; ++i) {
    const auto size = static_cast<uint32_t>(10 + i);
    v.header_offsets.push_back(v.bytes.size());
    v.sizes.push_back(size);
    put32le(v.bytes, size);
    put32le(v.bytes, static_cast<uint32_t>(i));  // pts low
    put32le(v.bytes, 0);                         // pts high
    v.bytes.insert(v.bytes.end(), size, static_cast<uint8_t>(0xA0 + i));
  }
  return v;
}

// A minimal ISOBMFF file: ftyp, mdat (all samples in one chunk), moov with one avc1 track (track_ID 7, avcC with
// lengthSizeMinusOne = 3). Samples are 4-byte length-prefixed pseudo NAL units; sync samples are 1 and 6 (1-based).
struct Mp4 {
  Bytes bytes;
  Bytes avcc;
  std::vector<uint64_t> offsets;
  std::vector<uint32_t> sizes;
  std::set<size_t> sync;  // 0-based
  static constexpr uint32_t kTrackId = 7;
  static constexpr uint32_t kTimescale = 25;
};
Mp4 make_mp4(int samples = 10) {
  Mp4 m;
  const Bytes ftyp = [] {
    Bytes p;
    put(p, "isom");
    put32(p, 0x200);
    put(p, "isomavc1");
    return box("ftyp", p);
  }();
  Bytes mdat_payload;
  const uint64_t first = ftyp.size() + 8;
  for (int i = 0; i < samples; ++i) {
    const auto nal = static_cast<uint32_t>(5 + i);
    m.offsets.push_back(first + mdat_payload.size());
    m.sizes.push_back(nal + 4);
    put32(mdat_payload, nal);
    put8(mdat_payload, i % 5 == 0 ? 0x65 : 0x41);  // IDR / non-IDR slice NAL header
    mdat_payload.insert(mdat_payload.end(), nal - 1, static_cast<uint8_t>(0x80 + i));
    if (i % 5 == 0) {
      m.sync.insert(static_cast<size_t>(i));
    }
  }
  const auto n = static_cast<uint32_t>(samples);

  Bytes mvhd;
  put32(mvhd, 0);
  put32(mvhd, 0);
  put32(mvhd, 0);
  put32(mvhd, Mp4::kTimescale);
  put32(mvhd, n);
  put32(mvhd, 0x00010000);
  put16(mvhd, 0x0100);
  zeros(mvhd, 10);
  for (const uint32_t v : {0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U}) {
    put32(mvhd, v);
  }
  zeros(mvhd, 24);
  put32(mvhd, Mp4::kTrackId + 1);

  Bytes tkhd;
  put32(tkhd, 0x00000003);
  put32(tkhd, 0);
  put32(tkhd, 0);
  put32(tkhd, Mp4::kTrackId);
  put32(tkhd, 0);
  put32(tkhd, n);
  zeros(tkhd, 8);
  put16(tkhd, 0);
  put16(tkhd, 0);
  put16(tkhd, 0);
  put16(tkhd, 0);
  for (const uint32_t v : {0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U}) {
    put32(tkhd, v);
  }
  put32(tkhd, 16U << 16U);
  put32(tkhd, 16U << 16U);

  Bytes mdhd;
  put32(mdhd, 0);
  put32(mdhd, 0);
  put32(mdhd, 0);
  put32(mdhd, Mp4::kTimescale);
  put32(mdhd, n);
  put16(mdhd, 0x55C4);  // "und"
  put16(mdhd, 0);

  Bytes hdlr;
  put32(hdlr, 0);
  put32(hdlr, 0);
  put(hdlr, "vide");
  zeros(hdlr, 12);
  put8(hdlr, 0);

  Bytes vmhd;
  put32(vmhd, 1);
  zeros(vmhd, 8);
  Bytes url;
  put32(url, 1);
  Bytes dref;
  put32(dref, 0);
  put32(dref, 1);
  dref = cat({dref, box("url ", url)});

  // avcC: version 1, profile 66, compat 0, level 30, lengthSizeMinusOne 3, one SPS, one PPS (pseudo bytes).
  put(m.avcc, std::string_view("\x01\x42\x00\x1e\xff\xe1", 6));
  put16(m.avcc, 4);
  put(m.avcc, std::string_view("\x67\x42\x00\x1e", 4));
  put8(m.avcc, 1);
  put16(m.avcc, 2);
  put(m.avcc, std::string_view("\x68\xce", 2));

  Bytes avc1;
  zeros(avc1, 6);
  put16(avc1, 1);
  zeros(avc1, 16);
  put16(avc1, 16);
  put16(avc1, 16);
  put32(avc1, 0x00480000);
  put32(avc1, 0x00480000);
  put32(avc1, 0);
  put16(avc1, 1);
  zeros(avc1, 32);
  put16(avc1, 0x0018);
  put16(avc1, 0xFFFF);
  avc1 = cat({avc1, box("avcC", m.avcc)});
  Bytes stsd;
  put32(stsd, 0);
  put32(stsd, 1);
  stsd = cat({stsd, box("avc1", avc1)});

  Bytes stts;
  put32(stts, 0);
  put32(stts, 1);
  put32(stts, n);
  put32(stts, 1);
  Bytes stss;
  put32(stss, 0);
  put32(stss, static_cast<uint32_t>(m.sync.size()));
  for (const size_t s : m.sync) {
    put32(stss, static_cast<uint32_t>(s + 1));
  }
  Bytes stsc;
  put32(stsc, 0);
  put32(stsc, 1);
  put32(stsc, 1);
  put32(stsc, n);
  put32(stsc, 1);
  Bytes stsz;
  put32(stsz, 0);
  put32(stsz, 0);
  put32(stsz, n);
  for (const uint32_t s : m.sizes) {
    put32(stsz, s);
  }
  Bytes stco;
  put32(stco, 0);
  put32(stco, 1);
  put32(stco, static_cast<uint32_t>(first));

  const Bytes stbl = box("stbl", cat({box("stsd", stsd), box("stts", stts), box("stss", stss), box("stsc", stsc),
                                      box("stsz", stsz), box("stco", stco)}));
  const Bytes minf = box("minf", cat({box("vmhd", vmhd), box("dinf", box("dref", dref)), stbl}));
  const Bytes mdia = box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr), minf}));
  const Bytes moov = box("moov", cat({box("mvhd", mvhd), box("trak", cat({box("tkhd", tkhd), mdia}))}));
  m.bytes = cat({ftyp, box("mdat", mdat_payload), moov});
  return m;
}

// ---- helpers -----------------------------------------------------------------------------------------------------

std::shared_ptr<ovt::io::RandomAccessFile> memory(const Bytes& b, const std::string& name = "memory") {
  return std::make_shared<ovt::io::MemoryFile>(b, name);
}

std::unique_ptr<Demuxer> open_ok(std::shared_ptr<ovt::io::RandomAccessFile> f, std::string_view hint) {
  auto r = Demuxer::open(std::move(f), hint);
  INFO("open: " << r.message());
  REQUIRE(r.has_value());
  return std::move(r).value();
}

struct Read {
  int avstream_index;
  int64_t pts;
  int64_t file_pos;
  bool keyframe;
  Bytes data;
};

std::vector<Read> read_all(Demuxer& d) {
  std::vector<Read> out;
  for (;;) {
    auto r = d.read_packet();
    INFO("read_packet: " << r.message());
    REQUIRE(r.has_value());
    if (!r.value()) {
      return out;
    }
    const Packet& p = *r.value();
    out.push_back({p.avstream_index, p.pts, p.file_pos, p.keyframe, Bytes(p.data.begin(), p.data.end())});
  }
}

// A scratch file that is removed at scope exit.
struct TempFile {
  std::filesystem::path path;
  TempFile(const Bytes& b, std::string_view ext) {
    static int counter = 0;
    path = std::filesystem::temp_directory_path() /
           ("ovt_demux_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + std::string(ext));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));  // NOLINT
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;
};

// One ffprobe packet line: stream index and pos (-1 for N/A).
struct ProbePacket {
  int stream_index;
  int64_t pos;
};

// Runs ffprobe with the demuxer options the Demuxer passes for `hint` (§8.7.2 rule 4), so both sides demux the
// same way. std::nullopt when ffprobe is missing or rejects the file.
std::optional<std::vector<ProbePacket>> ffprobe_packets(const std::filesystem::path& path, std::string_view hint) {
  std::string opts;
  if (hint == "isobmff") {
    opts = "-advanced_editlist 0 -export_all 1 ";
  } else if (hint == "mpegts") {
    opts = "-fflags +genpts ";
  }
  const std::string cmd = "ffprobe -v error " + opts + "-show_entries packet=stream_index,pos -of csv=p=0 '" +
                          path.string() + "' 2>/dev/null; echo \"rc=$?\"";
  // NOLINTNEXTLINE(cert-env33-c): test-only; the command is fixed and the path comes from the test
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    return std::nullopt;
  }
  std::string out;
  std::array<char, 4096> chunk{};
  while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe.get()) != nullptr) {
    out += chunk.data();
  }
  std::vector<ProbePacket> packets;
  std::istringstream lines(out);
  std::string line;
  bool ok = false;
  while (std::getline(lines, line)) {
    if (line.starts_with("rc=")) {
      ok = line == "rc=0";
      continue;
    }
    const size_t comma = line.find(',');
    if (comma == std::string::npos) {
      continue;
    }
    // "<stream_index>,<pos>", sometimes followed by ",<side data>"; pos is "N/A" when unknown.
    std::string pos = line.substr(comma + 1);
    pos = pos.substr(0, pos.find(','));
    const bool numeric = !pos.empty() && pos.find_first_not_of("0123456789") == std::string::npos;
    packets.push_back({std::stoi(line.substr(0, comma)), numeric ? std::stoll(pos) : -1});
  }
  if (!ok) {
    return std::nullopt;
  }
  return packets;
}

bool have_ffprobe() {
  // NOLINTNEXTLINE(cert-env33-c,concurrency-mt-unsafe): test-only, fixed command
  return std::system("ffprobe -version >/dev/null 2>&1") == 0;
}

// Acceptance test 1 for one file: packet counts per stream equal ffprobe's, and so does every file_pos.
// `tolerant`: MPEG-TS / Annex B, where ±1 packet is accepted and positions are compared only on equal counts
// (§8.7.1 "Oracle parity").
void check_parity(const std::filesystem::path& path, std::string_view hint, bool tolerant) {
  const auto probe = ffprobe_packets(path, hint);
  if (!probe) {
    WARN("ffprobe rejected " << path << "; not compared");
    return;
  }
  auto d = open_ok(std::shared_ptr<ovt::io::RandomAccessFile>(ovt::io::RandomAccessFile::open(path.string())), hint);
  const auto got = read_all(*d);
  if (!tolerant) {
    REQUIRE(got.size() == probe->size());
    for (size_t i = 0; i < got.size(); ++i) {
      INFO("packet " << i);
      CHECK(got[i].avstream_index == (*probe)[i].stream_index);
      CHECK(got[i].file_pos == (*probe)[i].pos);
    }
    return;
  }
  const auto diff = static_cast<int64_t>(got.size()) - static_cast<int64_t>(probe->size());
  CHECK(diff >= -1);
  CHECK(diff <= 1);
  if (diff == 0) {
    for (size_t i = 0; i < got.size(); ++i) {
      INFO("packet " << i);
      CHECK(got[i].file_pos == (*probe)[i].pos);
    }
  }
}

// A RandomAccessFile whose reads throw IoError past `limit` bytes.
class FailingFile final : public ovt::io::RandomAccessFile {
 public:
  FailingFile(Bytes b, uint64_t limit)
      : RandomAccessFile(ovt::io::FileInfo{b.size(), 0, "failing"}), inner_(b), limit_(limit) {}
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset + len > limit_) {
      throw ovt::io::IoError(OVT_E_IO, 5, "injected failure");
    }
    return inner_.read(offset, len, dst);
  }
  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override { return inner_.view(offset, len); }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*access*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }

 private:
  ovt::io::MemoryFile inner_;
  uint64_t limit_;
};

}  // namespace

// ---- mapping tables ------------------------------------------------------------------------------------------------

TEST_CASE("demux: AVCodecID maps to ovt::fb::Codec", "[demux][fast]") {
  using ovt::demux::codec_from_av;
  using ovt::fb::Codec;
  CHECK(codec_from_av(AV_CODEC_ID_H264) == Codec::H264);
  CHECK(codec_from_av(AV_CODEC_ID_HEVC) == Codec::H265);
  CHECK(codec_from_av(AV_CODEC_ID_AV1) == Codec::AV1);
  CHECK(codec_from_av(AV_CODEC_ID_VP9) == Codec::VP9);
  CHECK(codec_from_av(AV_CODEC_ID_VVC) == Codec::H266);
  CHECK(codec_from_av(AV_CODEC_ID_MPEG2VIDEO) == Codec::MPEG2);
  CHECK(codec_from_av(AV_CODEC_ID_VP8) == Codec::Unknown);
  CHECK(codec_from_av(AV_CODEC_ID_AAC) == Codec::Unknown);
}

TEST_CASE("demux: hint_format maps to the libavformat demuxer names", "[demux][fast]") {
  using ovt::demux::libavformat_name;
  CHECK(libavformat_name("isobmff") == "mov,mp4,m4a,3gp,3g2,mj2");
  CHECK(libavformat_name("ivf") == "ivf");
  CHECK(libavformat_name("y4m") == "yuv4mpegpipe");
  CHECK(libavformat_name("obu") == "obu");
  CHECK(libavformat_name("av1") == "av1");
  CHECK(libavformat_name("ebml") == "matroska,webm");
  CHECK(libavformat_name("mpegts") == "mpegts");
  CHECK(libavformat_name("annexb") == "");
  CHECK(libavformat_name("") == "");
  CHECK_FALSE(libavformat_name("raw").has_value());
  CHECK_FALSE(libavformat_name("bogus").has_value());
}

// ---- open / streams / read_packet ----------------------------------------------------------------------------------

TEST_CASE("demux: ISOBMFF packets carry the sample offsets, bytes and sync flags", "[demux][fast]") {
  const Mp4 m = make_mp4();
  for (const std::string_view hint : {"isobmff", ""}) {
    INFO("hint '" << hint << "'");
    auto d = open_ok(memory(m.bytes), hint);
    REQUIRE(d->streams().size() == 1);
    const auto& s = d->streams()[0];
    CHECK(s.avstream_index == 0);
    CHECK(s.container_track_id == static_cast<int>(Mp4::kTrackId));
    CHECK(s.codec == ovt::fb::Codec::H264);
    CHECK(s.media_type == AVMEDIA_TYPE_VIDEO);
    CHECK(s.time_base.num == 1);
    CHECK(s.time_base.den == static_cast<int>(Mp4::kTimescale));
    CHECK(s.nb_frames == static_cast<int64_t>(m.sizes.size()));
    CHECK(s.codec_private_fourcc == "avcC");
    CHECK(s.codec_private == m.avcc);
    CHECK(s.nal_length_size == 4);
    CHECK(s.length_prefixed);

    const auto got = read_all(*d);
    REQUIRE(got.size() == m.sizes.size());
    for (size_t i = 0; i < got.size(); ++i) {
      INFO("sample " << i);
      CHECK(got[i].file_pos == static_cast<int64_t>(m.offsets[i]));
      CHECK(got[i].keyframe == m.sync.contains(i));
      // The container's bytes, unchanged: no bitstream filter (§8.7.3).
      const Bytes expected(m.bytes.begin() + static_cast<std::ptrdiff_t>(m.offsets[i]),
                           m.bytes.begin() + static_cast<std::ptrdiff_t>(m.offsets[i] + m.sizes[i]));
      CHECK(got[i].data == expected);
    }
    CHECK(d->raw() != nullptr);
  }
}

TEST_CASE("demux: IVF packets are the frame payloads, positioned at the frame headers", "[demux][fast]") {
  const Ivf v = make_ivf(8);
  auto d = open_ok(memory(v.bytes), "ivf");
  REQUIRE(d->streams().size() == 1);
  CHECK(d->streams()[0].codec_private_fourcc == "none");
  CHECK(d->streams()[0].container_track_id == -1);
  const auto got = read_all(*d);
  REQUIRE(got.size() == v.sizes.size());
  for (size_t i = 0; i < got.size(); ++i) {
    INFO("frame " << i);
    // libavformat's ivfdec reports pos as the offset of the 12-byte frame header.
    CHECK(got[i].file_pos == static_cast<int64_t>(v.header_offsets[i]));
    CHECK(got[i].pts == static_cast<int64_t>(i));
    CHECK(got[i].data == Bytes(v.sizes[i], static_cast<uint8_t>(0xA0 + i)));
  }
}

TEST_CASE("demux: Y4M frames are key-frame packets", "[demux][fast]") {
  const Y4m y = make_y4m(5);
  auto d = open_ok(memory(y.bytes), "y4m");
  REQUIRE(d->streams().size() == 1);
  CHECK(d->streams()[0].codec == ovt::fb::Codec::Unknown);
  const auto got = read_all(*d);
  REQUIRE(got.size() == 5);
  for (size_t i = 0; i < got.size(); ++i) {
    INFO("frame " << i);
    CHECK(got[i].keyframe);
    CHECK(got[i].data.size() == size_t{16 * 16 * 3 / 2});
    CHECK(got[i].data.front() == static_cast<uint8_t>(i));
  }
}

TEST_CASE("demux: select_streams discards the unselected streams", "[demux][fast]") {
  const Mp4 m = make_mp4();
  auto d = open_ok(memory(m.bytes), "isobmff");
  d->select_streams({});
  CHECK(read_all(*d).empty());

  auto d2 = open_ok(memory(m.bytes), "isobmff");
  const std::array<int, 3> sel{0, 5, -1};  // out-of-range indices are ignored
  d2->select_streams(sel);
  CHECK(read_all(*d2).size() == m.sizes.size());
}

// ---- seek_to_time (acceptance test 2) ------------------------------------------------------------------------------

TEST_CASE("demux: seek_to_time(30 s, backward) lands on a key frame at or before 30 s", "[demux][fast]") {
  const Y4m y = make_y4m(60, 1);  // time base 1/1: pts == seconds
  auto d = open_ok(memory(y.bytes), "y4m");
  REQUIRE(d->streams()[0].time_base.num == 1);
  REQUIRE(d->streams()[0].time_base.den == 1);
  // Read a little first so the seek really moves backwards in the file.
  for (int i = 0; i < 45; ++i) {
    REQUIRE(d->read_packet().has_value());
  }
  const auto st = d->seek_to_time(0, 30, true);
  INFO(st.message());
  REQUIRE(st.has_value());
  auto r = d->read_packet();
  REQUIRE(r.has_value());
  REQUIRE(r.value().has_value());
  CHECK(r.value()->keyframe);
  CHECK(r.value()->pts <= 30);
  // libavformat's yuv4_read_seek steps one frame back for AVSEEK_FLAG_BACKWARD, so 29 is what it returns.
  CHECK(r.value()->pts >= 29);
  CHECK(r.value()->data.front() == static_cast<uint8_t>(r.value()->pts));

  CHECK(d->seek_to_time(3, 0).status() == OVT_E_INVALID_ARG);
  CHECK(d->seek_to_time(-1, 0).status() == OVT_E_INVALID_ARG);
}

TEST_CASE("demux: seek_to_time backward on ISOBMFF returns the preceding sync sample", "[demux][fast]") {
  const Mp4 m = make_mp4();
  auto d = open_ok(memory(m.bytes), "isobmff");
  REQUIRE(d->seek_to_time(0, 8, true).has_value());  // samples 5..9 follow the sync sample 5
  auto r = d->read_packet();
  REQUIRE(r.has_value());
  REQUIRE(r.value().has_value());
  CHECK(r.value()->keyframe);
  CHECK(r.value()->pts == 5);
  CHECK(r.value()->file_pos == static_cast<int64_t>(m.offsets[5]));
}

// ---- errors (acceptance test 3) ------------------------------------------------------------------------------------

TEST_CASE("demux: a 0-byte file is rejected without leaking", "[demux][fast]") {
  // LSan (linux-x64-asan-ubsan) checks the leak half of acceptance test 3.
  for (const std::string_view hint : {"", "isobmff", "ivf", "y4m", "annexb", "mpegts", "obu", "ebml", "h264", "av1"}) {
    INFO("hint '" << hint << "'");
    const auto r = Demuxer::open(memory({}), hint);
    REQUIRE_FALSE(r.has_value());
    CHECK((r.status() == OVT_E_CORRUPT || r.status() == OVT_E_UNSUPPORTED));
  }
}

TEST_CASE("demux: bad arguments and truncated or garbage input fail cleanly", "[demux][fast]") {
  CHECK(Demuxer::open(nullptr, "").status() == OVT_E_INVALID_ARG);
  CHECK(Demuxer::open(memory(make_y4m(1).bytes), "bogus").status() == OVT_E_INVALID_ARG);
  CHECK(Demuxer::open(memory(make_y4m(1).bytes), "raw").status() == OVT_E_UNSUPPORTED);

  Bytes garbage(4096);
  for (size_t i = 0; i < garbage.size(); ++i) {
    garbage[i] = static_cast<uint8_t>((i * 7919U) >> 3U);
  }
  const auto g = Demuxer::open(memory(garbage), "isobmff");
  CHECK_FALSE(g.has_value());

  // An ISOBMFF file cut inside moov.
  Mp4 m = make_mp4();
  m.bytes.resize(m.bytes.size() - 40);
  const auto t = Demuxer::open(memory(m.bytes), "isobmff");
  CHECK_FALSE(t.has_value());
}

TEST_CASE("demux: a failing RandomAccessFile maps to OVT_E_IO", "[demux][fast]") {
  // 4000 frames of 390 bytes: larger than the 256 KiB AVIO buffer, so a late failure happens after open.
  const Y4m y = make_y4m(4000);
  // Fails while libavformat reads the header.
  const auto r = Demuxer::open(std::make_shared<FailingFile>(y.bytes, 4), "y4m");
  CHECK(r.status() == OVT_E_IO);
  // Fails while packets are read.
  auto d = open_ok(std::make_shared<FailingFile>(y.bytes, y.frame_offsets[3000]), "y4m");
  ovt_status last = OVT_OK;
  for (int i = 0; i < 4001 && last == OVT_OK; ++i) {
    auto p = d->read_packet();
    last = p.status();
    REQUIRE((last != OVT_OK || p.value().has_value()));
  }
  CHECK(last == OVT_E_IO);
}

// ---- Android-style pread wrapper (step 5)
// ----------------------------------------------------------------------------

TEST_CASE("demux: the pread backend and an adopted descriptor demux identically", "[demux][fast]") {
  const Mp4 m = make_mp4();
  const TempFile tmp(m.bytes, ".mp4");
  const auto expected = [&] {
    auto d = open_ok(memory(m.bytes), "isobmff");
    return read_all(*d);
  }();

  std::shared_ptr<ovt::io::RandomAccessFile> by_path =
      ovt::io::RandomAccessFile::open(tmp.path.string(), ovt::io::OpenMode::Pread);
  REQUIRE_FALSE(by_path->is_mapped());
  auto d1 = open_ok(by_path, "isobmff");
  const auto got1 = read_all(*d1);

  // What Android does with a ParcelFileDescriptor: adopt the fd, read through pread(2) (§8.7.2 rule 6).
  const int fd = ::open(tmp.path.c_str(), O_RDONLY | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  REQUIRE(fd >= 0);
  std::shared_ptr<ovt::io::RandomAccessFile> by_fd =
      ovt::io::RandomAccessFile::open_fd(fd, m.bytes.size(), true, "fd:test", ovt::io::OpenMode::Pread);
  auto d2 = open_ok(by_fd, "");
  const auto got2 = read_all(*d2);

  REQUIRE(got1.size() == expected.size());
  REQUIRE(got2.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(got1[i].file_pos == expected[i].file_pos);
    CHECK(got1[i].data == expected[i].data);
    CHECK(got2[i].file_pos == expected[i].file_pos);
    CHECK(got2[i].data == expected[i].data);
  }
}

// ---- ffprobe parity (acceptance test 1)
// ------------------------------------------------------------------------------

TEST_CASE("demux: packet counts and positions equal ffprobe on the synthetic files", "[demux][ffprobe][fast]") {
  if (!have_ffprobe()) {
    SKIP("ffprobe is not on PATH");
  }
  SECTION("isobmff") {
    const TempFile f(make_mp4().bytes, ".mp4");
    check_parity(f.path, "isobmff", false);
  }
  SECTION("ivf") {
    const TempFile f(make_ivf(8).bytes, ".ivf");
    check_parity(f.path, "ivf", false);
  }
  SECTION("y4m") {
    const TempFile f(make_y4m(12).bytes, ".y4m");
    check_parity(f.path, "y4m", false);
  }
}

TEST_CASE("demux: packet counts and positions equal ffprobe on every corpus MP4/IVF/ES/Y4M",
          "[demux][ffprobe][needs-corpus]") {
  if (!have_ffprobe()) {
    SKIP("ffprobe is not on PATH");
  }
  std::ifstream in(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  int compared = 0;
  for (const auto& entry : manifest.at("entries")) {
    const auto containers = entry.value("containers", std::vector<std::string>{});
    if (containers.size() != 1) {
      continue;
    }
    const std::string& c = containers.front();
    std::string hint;
    if (c == "isobmff" || c == "mp4") {
      hint = "isobmff";
    } else if (c == "ivf" || c == "y4m" || c == "annexb" || c == "mpegts") {
      hint = c;
    } else if (c == "obu") {
      hint = "";  // Section 5 ("obu") or Annex B ("av1"): libavformat probes, as ffprobe does
    } else {
      continue;
    }
    const std::string id = entry.at("id");
    const auto path = ovt::test::corpus_path(id);
    if (!path) {
      CHECK_FALSE(ovt::test::require_corpora());
      continue;
    }
    // Files the manifest marks as broken on purpose are not part of the parity set.
    if (id.find("_broken_") != std::string::npos) {
      continue;
    }
    INFO("corpus " << id);
    check_parity(*path, hint, c == "annexb" || c == "mpegts");
    ++compared;
  }
  if (compared == 0) {
    SKIP("no corpus MP4/IVF/ES/Y4M file is present");
  }
}
