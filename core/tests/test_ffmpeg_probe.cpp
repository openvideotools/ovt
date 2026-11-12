// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the FFmpeg two-flavour build (WI-0.012, chapter 13 §13.3.3). One source, two executables, because the
// two flavours define the same av* symbols and cannot share a process outside the plugin:
//   ovt_tests_ffmpeg_probe         links tp::ffmpeg_core   (OVT_FFMPEG_PLUGIN_FLAVOUR=0)
//   ovt_tests_ffmpeg_plugin_probe  links tp::ffmpeg_plugin (OVT_FFMPEG_PLUGIN_FLAVOUR=1)
// Both print avcodec_configuration() so a CI log or an acceptance record shows the configure line of the build.
//
// Compile definitions (core/tests/CMakeLists.txt): OVT_FFMPEG_PLUGIN_FLAVOUR, OVT_FFMPEG_SYSTEM
// (OVT_SYSTEM_FFMPEG=ON: no flavour assertions), OVT_PATENT_CODECS_IN_CORE (ON by default), OVT_ENABLE_HW_DECODE,
// OVT_FFMPEG_DAV1D.

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
#if !OVT_FFMPEG_PLUGIN_FLAVOUR && !OVT_FFMPEG_SYSTEM
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#endif
}

#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#if !OVT_FFMPEG_PLUGIN_FLAVOUR && !OVT_FFMPEG_SYSTEM
#include <catch2/catch_message.hpp>

#include <filesystem>

#include "test_support/corpus.h"
#endif

// CTest registers the test cases of both executables in one namespace, so the names carry the flavour.
#if OVT_FFMPEG_PLUGIN_FLAVOUR
#define OVT_PROBE "ffmpeg plugin probe: "
#else
#define OVT_PROBE "ffmpeg core probe: "
#endif

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string configuration() {
  const char* conf = avcodec_configuration();
  return conf != nullptr ? std::string(conf) : std::string();
}

bool has_parser_for(AVCodecID id) {
  void* it = nullptr;
  while (const AVCodecParser* parser = av_parser_iterate(&it)) {
    const auto& ids = parser->codec_ids;  // int[7], zero-padded
    for (const int codec_id : std::to_array(ids)) {
      if (codec_id == id) {
        return true;
      }
    }
  }
  return false;
}

std::set<std::string> decoder_names() {
  std::set<std::string> names;
  void* it = nullptr;
  while (const AVCodec* codec = av_codec_iterate(&it)) {
    if (av_codec_is_decoder(codec) != 0) {
      names.emplace(codec->name);
    }
  }
  return names;
}

}  // namespace

TEST_CASE(OVT_PROBE "avcodec_configuration is printed and names the flavour", "[fast][third_party]") {
  const std::string conf = configuration();
  // Printed on purpose: acceptance test 3 of WI-0.012 reads it from the test log.
  std::printf("%s avcodec_configuration(): %s\n", OVT_FFMPEG_PLUGIN_FLAVOUR ? "plugin" : "core", conf.c_str());
  std::printf("libavcodec %s\n", av_version_info());
  REQUIRE_FALSE(conf.empty());
#if !OVT_FFMPEG_SYSTEM
  CHECK(contains(conf, "--disable-encoders"));
  CHECK(contains(conf, "--disable-autodetect"));
#if OVT_FFMPEG_PLUGIN_FLAVOUR
  CHECK(contains(conf, "--enable-static"));
  // configure quotes values that contain a comma: --enable-decoder='h264,hevc'.
  CHECK(contains(conf, "--enable-decoder='h264,hevc"));
#else
  CHECK(contains(conf, "--enable-shared"));
  CHECK(contains(conf, "--build-suffix=_ovt"));
#endif
#if OVT_ENABLE_HW_DECODE && defined(__APPLE__)
  CHECK(contains(conf, "--enable-videotoolbox"));
#endif
#if OVT_ENABLE_HW_DECODE && defined(__ANDROID__)
  CHECK(contains(conf, "--enable-mediacodec"));
#endif
#if OVT_ENABLE_HW_DECODE && defined(__linux__) && !defined(__ANDROID__)
  CHECK(contains(conf, "--enable-vaapi"));
#endif
#endif
}

TEST_CASE(OVT_PROBE "h264/hevc parsers are present in every flavour", "[fast][third_party]") {
  // Parsers are not decoders; the raw h264/hevc and mpegts demuxers need them to split access units (§13.3.3).
  CHECK(has_parser_for(AV_CODEC_ID_H264));
  CHECK(has_parser_for(AV_CODEC_ID_HEVC));
#if !OVT_FFMPEG_PLUGIN_FLAVOUR
  CHECK(has_parser_for(AV_CODEC_ID_AV1));
  CHECK(has_parser_for(AV_CODEC_ID_VP9));
#endif
}

#if !OVT_FFMPEG_SYSTEM
#if OVT_FFMPEG_PLUGIN_FLAVOUR

TEST_CASE(OVT_PROBE "the plugin flavour decodes exactly AVC and HEVC", "[fast][third_party]") {
  CHECK(avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr);
  CHECK(avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr);
  CHECK(avcodec_find_decoder(AV_CODEC_ID_AV1) == nullptr);
  CHECK(avcodec_find_decoder(AV_CODEC_ID_VP9) == nullptr);
  std::set<std::string> expected{"h264", "hevc"};
#if OVT_ENABLE_HW_DECODE && defined(__ANDROID__)
  expected.insert({"h264_mediacodec", "hevc_mediacodec"});
#endif
  CHECK(decoder_names() == expected);
}

#else  // core flavour

TEST_CASE(OVT_PROBE "the core flavour decodes the pool-covered codecs unless OVT_PATENT_CODECS_IN_CORE=OFF",
          "[fast][third_party]") {
  // AVC and HEVC, plus the other pool-covered codecs of POL-PAT-2: VVC, AAC/HE-AAC, MPEG-4 Part 2 (with its
  // Microsoft variants MS-MPEG4 v1-v3 and WMV7/8) and VC-1. ON (the default since the owner decision of 2026-11-12,
  // ADR-015) keeps all of them in the core; OFF is the patent-pool-free split build.
  constexpr std::array pool_ids{AV_CODEC_ID_H264,      AV_CODEC_ID_HEVC,  AV_CODEC_ID_VVC,       AV_CODEC_ID_AAC,
                                AV_CODEC_ID_AAC_LATM,  AV_CODEC_ID_MPEG4, AV_CODEC_ID_MSMPEG4V1, AV_CODEC_ID_MSMPEG4V2,
                                AV_CODEC_ID_MSMPEG4V3, AV_CODEC_ID_WMV1,  AV_CODEC_ID_WMV2,      AV_CODEC_ID_VC1,
                                AV_CODEC_ID_WMV3};
#if OVT_PATENT_CODECS_IN_CORE
  for (const AVCodecID id : pool_ids) {
    INFO("codec " << avcodec_get_name(id));
    CHECK(avcodec_find_decoder(id) != nullptr);
  }
  // Nothing is disabled by name in the default core: its configure line carries no --disable-decoder/--disable-hwaccel.
  CHECK_FALSE(contains(configuration(), "--disable-decoder"));
  CHECK_FALSE(contains(configuration(), "--disable-hwaccel"));
  const std::set<std::string> names = decoder_names();
  CHECK(names.contains("h264"));
  CHECK(names.contains("hevc"));
#if OVT_ENABLE_HW_DECODE && defined(__ANDROID__)
  CHECK(names.contains("h264_mediacodec"));
  CHECK(names.contains("hevc_mediacodec"));
#endif
#else
  for (const AVCodecID id : pool_ids) {
    INFO("codec " << avcodec_get_name(id));
    CHECK(avcodec_find_decoder(id) == nullptr);
  }
  const std::set<std::string> names = decoder_names();
  // clang-format off
  constexpr std::array pool_decoders{
      "h264", "hevc", "h264_mediacodec", "hevc_mediacodec", "vvc", "aac", "aac_fixed", "aac_latm", "aac_at",
      "aac_mediacodec", "mpeg4", "mpeg4_mediacodec", "msmpeg4v1", "msmpeg4v2", "msmpeg4v3", "wmv1", "wmv2", "vc1",
      "vc1image", "wmv3", "wmv3image"};
  // clang-format on
  for (const char* name : pool_decoders) {
    INFO("decoder " << name);
    CHECK_FALSE(names.contains(name));
  }
#endif
  // The core keeps every other decoder; VP9 is a native FFmpeg decoder in every build.
  CHECK(avcodec_find_decoder(AV_CODEC_ID_VP9) != nullptr);
}

TEST_CASE(OVT_PROBE "the core flavour has the platform AV1 decoders", "[fast][third_party]") {
#if OVT_FFMPEG_DAV1D
  CHECK(avcodec_find_decoder_by_name("libdav1d") != nullptr);
  CHECK(contains(configuration(), "--enable-libdav1d"));
#endif
#if OVT_ENABLE_HW_DECODE && defined(__ANDROID__)
  // Acceptance test 4 of WI-0.012.
  CHECK(avcodec_find_decoder_by_name("av1_mediacodec") != nullptr);
#endif
  CHECK(avcodec_find_decoder(AV_CODEC_ID_AV1) != nullptr);  // FFmpeg's own av1 (hwaccel front end) at least
}

namespace {

// What demuxing a file found: the first video stream's codec id and its packet count (-1: not opened).
struct DemuxSummary {
  AVCodecID video_codec = AV_CODEC_ID_NONE;
  int video_packets = -1;
};

DemuxSummary demux(const std::filesystem::path& path) {
  DemuxSummary summary;
  AVFormatContext* ctx = nullptr;
  if (avformat_open_input(&ctx, path.string().c_str(), nullptr, nullptr) != 0) {
    return summary;
  }
  const int video = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  AVPacket* packet = av_packet_alloc();
  if (video >= 0 && packet != nullptr) {
    summary.video_codec = ctx->streams[video]->codecpar->codec_id;  // NOLINT(*-pointer-arithmetic): FFmpeg array
    summary.video_packets = 0;
    while (av_read_frame(ctx, packet) >= 0) {
      summary.video_packets += packet->stream_index == video ? 1 : 0;
      av_packet_unref(packet);
    }
  }
  av_packet_free(&packet);
  avformat_close_input(&ctx);
  return summary;
}

}  // namespace

TEST_CASE(OVT_PROBE "avformat opens an MP4 of the corpus", "[fast][third_party][needs-corpus]") {
  // Acceptance test 1 of WI-0.012. Demuxing needs no decoder. (The manifest lists this file as h264; FFmpeg reports
  // MPEG-4 Part 2 video, so the test checks only that a video stream with a known codec id is demuxed.)
  const std::filesystem::path path = ovt::test::corpus_or_skip("mpegff_a1_foreman_qcif");
  const DemuxSummary summary = demux(path);
  CHECK(summary.video_codec != AV_CODEC_ID_NONE);
  CHECK(summary.video_packets > 0);
}

#endif  // OVT_FFMPEG_PLUGIN_FLAVOUR
#endif  // !OVT_FFMPEG_SYSTEM
