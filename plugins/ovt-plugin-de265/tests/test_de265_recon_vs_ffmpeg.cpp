// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// WI-2.582 acceptance test 2 (chapter 14 §14.4.4, chapter 16 §16.4): the PlaneStage::Reconstruction planes that
// ovt-plugin-de265 returns are bit-exact with the FFmpeg hevc software decode of the same stream, picture by picture in
// output order, on the `jctvc` Main / Main 10 fast subset (every stream that has an HM grid golden; the same ids as
// test_de265_map.cpp). Unlike test_de265_plugin_smoke.cpp (AT1 of WI-2.163: decoded picture hash SEI), this needs no
// SEI in the stream, so it also covers RPS_A, RPS_F, POC_A, CONFWIN_A, MAXBINS_A and STRUCT_B, which carry none (#255).
//
// The plugin is loaded the way the analysis host loads it (ovt::analysis::PluginRegistry) and driven through its ABI.
// FFmpeg is the core flavour the host links anyway (libavformat's raw `hevc` demuxer and the software decoder, one
// thread, no cropping so that the picture is the decoded picture of H.265 D.3.19, which the plugin's planes are too).
// Each picture is compared as one MD5 per plane, so a failure names the picture and the plane.
//
// SLIST_A_Sony_5 is left out: libde265 1.0.15 mis-decodes it (#254, the same exclusion as the SEI test). RAP_B_Bossen_2
// is left out too: on CI FFmpeg outputs a picture (output index 69 of 80) that no libde265 picture equals, after the
// end-of-sequence CRA with RASL pictures, and without the stream here which of the two decoders is right is open
// (#589); the HM grid test still covers its decode.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/md5.h>
#include <libavutil/pixdesc.h>
}

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

using Md5 = std::array<uint8_t, 16>;

// One decoded picture: its size, bytes per sample and the MD5 of each plane's samples (rows packed, no padding).
struct Picture {
  int width = 0;
  int height = 0;
  int sample_bytes = 0;
  std::vector<Md5> planes;

  bool operator==(const Picture& o) const {
    return width == o.width && height == o.height && sample_bytes == o.sample_bytes && planes == o.planes;
  }
};

std::string describe(const Picture& p) {
  return std::to_string(p.width) + "x" + std::to_string(p.height) + ", " + std::to_string(p.sample_bytes) +
         " byte samples, " + std::to_string(p.planes.size()) + " planes";
}

struct FormatContextDeleter {
  void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecContextDeleter {
  void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
  void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};

Md5 md5_of(const uint8_t* data, size_t size) {
  Md5 d{};
  av_md5_sum(d.data(), data, size);
  return d;
}

// One FFmpeg frame as a Picture. The rows of a plane are packed first because the frame's lines are padded.
Picture picture_of(const AVFrame& f) {
  const auto fmt = static_cast<AVPixelFormat>(f.format);
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
  REQUIRE(desc != nullptr);
  REQUIRE((desc->flags & AV_PIX_FMT_FLAG_PLANAR) != 0);
  Picture p;
  p.width = f.width;
  p.height = f.height;
  p.sample_bytes = desc->comp[0].step;
  for (int plane = 0; plane < av_pix_fmt_count_planes(fmt); ++plane) {
    const int shift_w = (plane == 1 || plane == 2) ? desc->log2_chroma_w : 0;
    const int shift_h = (plane == 1 || plane == 2) ? desc->log2_chroma_h : 0;
    const auto w = static_cast<size_t>(AV_CEIL_RSHIFT(f.width, shift_w));
    const auto h = static_cast<size_t>(AV_CEIL_RSHIFT(f.height, shift_h));
    const size_t row = w * static_cast<size_t>(p.sample_bytes);
    std::vector<uint8_t> packed(row * h);
    for (size_t y = 0; y < h; ++y) {
      const uint8_t* src = f.data[plane] + static_cast<ptrdiff_t>(y) * f.linesize[plane];  // NOLINT(*-pro-bounds-*)
      std::copy_n(src, row, packed.begin() + static_cast<std::ptrdiff_t>(y * row));
    }
    p.planes.push_back(md5_of(packed.data(), packed.size()));
  }
  return p;
}

// Every picture the FFmpeg hevc software decoder outputs for the Annex B stream @p path, in output order.
std::vector<Picture> ffmpeg_pictures(const std::filesystem::path& path) {
  AVFormatContext* raw = nullptr;
  const AVInputFormat* hevc = av_find_input_format("hevc");
  REQUIRE(hevc != nullptr);
  REQUIRE(avformat_open_input(&raw, path.string().c_str(), hevc, nullptr) == 0);
  std::unique_ptr<AVFormatContext, FormatContextDeleter> fmt(raw);
  REQUIRE(avformat_find_stream_info(fmt.get(), nullptr) >= 0);
  const int stream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  REQUIRE(stream >= 0);
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecContextDeleter> ctx(avcodec_alloc_context3(codec));
  REQUIRE(ctx != nullptr);
  REQUIRE(avcodec_parameters_to_context(ctx.get(), fmt->streams[stream]->codecpar) >= 0);  // NOLINT(*-bounds-*)
  ctx->thread_count = 1;
  ctx->apply_cropping = 0;  // the decoded picture, not the conformance window (H.265 D.3.19)
  REQUIRE(avcodec_open2(ctx.get(), codec, nullptr) == 0);
  std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  std::vector<Picture> out;
  const auto receive = [&] {
    while (avcodec_receive_frame(ctx.get(), frame.get()) == 0) {
      out.push_back(picture_of(*frame));
      av_frame_unref(frame.get());
    }
  };
  while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
    if (pkt->stream_index == stream) {
      REQUIRE(avcodec_send_packet(ctx.get(), pkt.get()) >= 0);
      receive();
    }
    av_packet_unref(pkt.get());
  }
  REQUIRE(avcodec_send_packet(ctx.get(), nullptr) >= 0);  // drain
  receive();
  return out;
}

// ---- the plugin --------------------------------------------------------------------------------------------------

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

std::vector<std::pair<size_t, size_t>> annexb_nals(const std::vector<uint8_t>& s) {
  std::vector<std::pair<size_t, size_t>> nals;  // offset, length
  size_t start = SIZE_MAX;
  const auto close = [&](size_t end) {
    while (end > start && s[end - 1] == 0) {
      --end;  // trailing_zero_8bits and the leading zero of a 4-byte start code
    }
    if (end > start) {
      nals.emplace_back(start, end - start);
    }
  };
  for (size_t i = 0; i + 3 <= s.size();) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        close(i);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX) {
    close(s.size());
  }
  return nals;
}

// The Reconstruction planes of one pull()ed FrameAnalysis, hashed the way picture_of() does.
Picture plugin_picture(const ovt_AnalysisDecoderApi& api, ovt_AnalysisDecoder* dec, const ovt::fb::FrameAnalysis& fa) {
  Picture p;
  REQUIRE(fa.planes() != nullptr);
  for (const ovt::fb::PlaneRef* ref : *fa.planes()) {
    REQUIRE(ref->stage() == ovt::fb::PlaneStage::Reconstruction);
    const uint8_t* data = nullptr;
    size_t size = 0;
    REQUIRE(api.plane(dec, ref->blob_id()->c_str(), &data, &size) == OVT_OK);
    REQUIRE(size == static_cast<size_t>(ref->stride_bytes()) * ref->height());
    REQUIRE(ref->stride_bytes() == ref->width() * ref->sample_bytes());  // rows are packed
    if (ref->plane() == 0) {
      p.width = static_cast<int>(ref->width());
      p.height = static_cast<int>(ref->height());
      p.sample_bytes = static_cast<int>(ref->sample_bytes());
    }
    p.planes.push_back(md5_of(data, size));
  }
  return p;
}

// Every picture the plugin outputs for @p bytes, in the order pull() returns them.
std::vector<Picture> plugin_pictures(const std::vector<uint8_t>& bytes, int threads) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  ovt_AnalysisDecoder* dec = nullptr;
  REQUIRE(api.create(nullptr, 0, threads, &dec) == OVT_OK);
  std::vector<Picture> out;
  const auto pull = [&] {
    for (;;) {
      ovt_Buffer buf{};
      REQUIRE(api.pull(dec, kReconMask, &buf) == OVT_OK);
      if (buf.data == nullptr) {
        return;
      }
      flatbuffers::Verifier verifier(buf.data, buf.size);
      REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
      out.push_back(plugin_picture(api, dec, *ovt::fb::GetFrameAnalysis(buf.data)));
      api.release(dec, buf);
    }
  };
  int pic = -1;
  for (const auto& [offset, length] : annexb_nals(bytes)) {
    if (length < 3) {
      continue;
    }
    const uint8_t* nal = bytes.data() + offset;  // NOLINT(*-pro-bounds-pointer-arithmetic)
    const int type = (nal[0] >> 1) & 0x3F;
    if (type < 32 && (nal[2] & 0x80) != 0) {  // first_slice_segment_in_pic_flag opens the next picture
      ++pic;
    }
    // An error here (an unsupported or corrupt unit) would show as a missing or different picture below.
    (void)api.push(dec, nal, length, std::max(pic, 0), 1000 + std::max(pic, 0));
    pull();
  }
  REQUIRE(api.flush(dec) == OVT_OK);
  pull();
  api.destroy(dec);
  return out;
}

void compare_with_ffmpeg(std::string_view id) {
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  INFO("stream " << id);
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(!bytes.empty());
  const std::vector<Picture> ref = ffmpeg_pictures(path);
  const std::vector<Picture> got = plugin_pictures(bytes, 1);
  REQUIRE(!ref.empty());
  INFO("FFmpeg output " << ref.size() << " pictures, ovt-plugin-de265 " << got.size());
  // libde265 outputs pictures that H.265 C.5.2.2 / 8.3.1 do not output, which FFmpeg drops: the pictures cut by
  // no_output_of_prior_pics_flag (NoOutPrior_A). On that
  // stream every FFmpeg picture must still equal a plugin picture, in order, and the plugin has the
  // extra ones (#590); everywhere else the two outputs must be the same sequence.
  std::vector<size_t> match(ref.size());
  if (id == "jctvc_NoOutPrior_A_Qualcomm_1") {
    REQUIRE(got.size() > ref.size());
    size_t g = 0;
    for (size_t i = 0; i < ref.size(); ++i, ++g) {
      while (g < got.size() && !(got[g] == ref[i])) {
        ++g;
      }
      INFO("FFmpeg picture " << i << " has no equal plugin picture after plugin picture " << (g == got.size() ? 0 : g));
      REQUIRE(g < got.size());
      match[i] = g;
    }
  } else {
    REQUIRE(got.size() == ref.size());
    for (size_t i = 0; i < ref.size(); ++i) {
      match[i] = i;
    }
  }
  for (size_t i = 0; i < ref.size(); ++i) {
    const Picture& a = got[match[i]];
    INFO("picture " << i << " (output order)");
    REQUIRE(a.width == ref[i].width);
    REQUIRE(a.height == ref[i].height);
    REQUIRE(a.sample_bytes == ref[i].sample_bytes);
    REQUIRE(a.planes.size() == ref[i].planes.size());
    for (size_t plane = 0; plane < ref[i].planes.size(); ++plane) {
      INFO("plane " << plane << " of " << describe(ref[i]));
      CHECK(a.planes[plane] == ref[i].planes[plane]);
    }
  }
  // Frame threads and slice threads must not change a sample.
  const std::vector<Picture> threaded = plugin_pictures(bytes, 4);
  REQUIRE(threaded.size() == got.size());
  for (size_t i = 0; i < got.size(); ++i) {
    INFO("picture " << i << " with 4 threads");
    CHECK(threaded[i] == got[i]);
  }
}

// The `jctvc` Main / Main 10 fast subset: every stream with an HM grid golden (tests/golden/h265/<id>.hm-grid.json.gz,
// which is what test_de265_map.cpp compares) and AMP_A (hm-cutree golden), less SLIST_A_Sony_5 (#254) and
// MVCLIP_A_qualcomm_3 (nightly).
constexpr std::array<std::string_view, 30> kStreams = {
    "jctvc_AMP_A_Samsung_7",         "jctvc_CAINIT_A_SHARP_4",
    "jctvc_CONFWIN_A_Sony_1",        "jctvc_EXT_A_ericsson_4",
    "jctvc_FILLER_A_Sony_1",         "jctvc_HRD_A_Fujitsu_3",
    "jctvc_MAXBINS_A_TI_5",          "jctvc_MERGE_B_TI_3",
    "jctvc_MVEDGE_A_qualcomm_3",     "jctvc_NUT_A_ericsson_5",
    "jctvc_NoOutPrior_A_Qualcomm_1", "jctvc_OPFLAG_A_Qualcomm_1",
    "jctvc_PMERGE_A_TI_3",           "jctvc_POC_A_Bossen_3",
    "jctvc_PPS_A_qualcomm_7",        "jctvc_RAP_A_docomo_6",
    "jctvc_RPS_A_docomo_5",          "jctvc_RPS_B_qualcomm_5",
    "jctvc_RPS_C_ericsson_5",        "jctvc_RPS_D_ericsson_6",
    "jctvc_RPS_E_qualcomm_5",        "jctvc_RPS_F_docomo_2",
    "jctvc_SAO_A_MediaTek_4",        "jctvc_STRUCT_B_Samsung_7",
    "jctvc_TSCL_A_VIDYO_5",          "jctvc_VPSID_A_VIDYO_2",
    "jctvc_WPP_A_ericsson_MAIN10_2", "jctvc_WPP_A_ericsson_MAIN_2",
    "jctvc_WP_A_Toshiba_3",          "jctvc_ipcm_A_NEC_3",
};

}  // namespace

TEST_CASE("de265 plugin: de265_recon_vs_ffmpeg on the jctvc fast subset", "[fast][third_party][needs-corpus][golden]") {
  const std::string_view id = GENERATE(from_range(kStreams));
  compare_with_ffmpeg(id);
}

TEST_CASE("de265 plugin: de265_recon_vs_ffmpeg on MVCLIP_A_qualcomm_3",
          "[nightly][third_party][needs-corpus][golden]") {
  compare_with_ffmpeg("jctvc_MVCLIP_A_qualcomm_3");
}
