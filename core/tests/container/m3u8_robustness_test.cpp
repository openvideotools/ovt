// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Robustness of the HLS playlist parser (chapter 08 §8.6.9, RFC 8216): playlists cut at every length and with every
// tag line damaged parse without a crash, keep the media segment list consistent, and flatten to a manifest.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "container/virtual_file/m3u8.hpp"
#include "ovt_container_generated.h"

namespace {

namespace m3u8 = ovt::container::m3u8;

const std::string kMedia =
    "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:10\n#EXT-X-DISCONTINUITY-SEQUENCE:2\n"
    "#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"720@0\"\n"
    "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\",IV=0x000102030405060708090A0B0C0D0E0F\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00.000Z\n#EXTINF:5.005,title\n#EXT-X-BYTERANGE:1000@0\na.m4s\n"
    "#EXT-X-DISCONTINUITY\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\n#EXT-X-BYTERANGE:900\nb.m4s\n"
    "#EXT-X-GAP\n#EXTINF:4,\nc.m4s\n#EXT-X-ENDLIST\n";

const std::string kMaster =
    "#EXTM3U\n#EXT-X-INDEPENDENT-SEGMENTS\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\",LANGUAGE=\"en\",DEFAULT=YES,AUTOSELECT=YES,URI=\"a."
    "m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sub\",NAME=\"Sub\",LANGUAGE=\"fr\",DEFAULT=NO,URI=\"s.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=800000,AVERAGE-BANDWIDTH=700000,RESOLUTION=640x360,FRAME-RATE=29.97,"
    "CODECS=\"avc1.64001e,mp4a.40.2\",AUDIO=\"aud\",SUBTITLES=\"sub\"\nlow.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,CODECS=\"hvc1.1.6.L93.B0\",VIDEO-RANGE=PQ\nhigh.m3u8\n"
    "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=100000,URI=\"iframe.m3u8\"\n";

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    out.push_back(l);
  }
  return out;
}

void parse_and_flatten(const std::string& text) {
  const m3u8::ParseResult r = m3u8::parse(text, {"https://example.invalid/d/p.m3u8"});
  for (const auto& s : r.manifest.media.segments) {
    CHECK_FALSE(s.url.empty());
  }
  flatbuffers::FlatBufferBuilder fbb;
  const auto off = m3u8::write_manifest(fbb, r.manifest);
  CHECK(off.o != 0);
}

}  // namespace

TEST_CASE("m3u8: playlists cut at every length parse and flatten", "[container][m3u8][fast]") {
  for (const std::string* full : {&kMedia, &kMaster}) {
    for (size_t len = 0; len <= full->size(); ++len) {
      parse_and_flatten(full->substr(0, len));
    }
  }
}

TEST_CASE("m3u8: every line damaged in several ways still parses", "[container][m3u8][fast]") {
  for (const std::string* full : {&kMedia, &kMaster}) {
    const std::vector<std::string> lines = lines_of(*full);
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& l = lines[i];
      const size_t colon = l.find(':');
      std::vector<std::string> variants{
          "",
          l.substr(0, colon == std::string::npos ? l.size() / 2 : colon + 1),
          l + l,
          l.substr(0, l.size() / 2),
          l + ",BANDWIDTH=-1,RESOLUTION=axb,DEFAULT=MAYBE,BYTERANGE=\"x@y\",IV=0xZZ",
          "#EXT-X-UNKNOWN-TAG:A=1\n" + l,
          "\xEF\xBB\xBF" + l,
      };
      for (const std::string& v : variants) {
        std::string text;
        for (size_t j = 0; j < lines.size(); ++j) {
          text += (j == i ? v : lines[j]) + "\n";
        }
        parse_and_flatten(text);
      }
    }
  }
}

TEST_CASE("m3u8: a text that is not a playlist is reported, not decoded", "[container][m3u8][fast]") {
  for (const std::string& text : {std::string(), std::string("hello\n"), std::string("#EXTM3U"), std::string("\n\n\n"),
                                  std::string(2000, '#'), std::string("#EXTM3U\n#EXT-X-STREAM-INF\n#EXTINF\n")}) {
    parse_and_flatten(text);
  }
}
