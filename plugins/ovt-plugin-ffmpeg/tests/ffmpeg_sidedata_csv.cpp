// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_plugin_ffmpeg_sidedata <file.264>: the in-tree `ffmpeg_mvs` of chapter 14 §14.4.3a (WI-2.170 oracle, so CI
// needs no oracle image). h264 parser + decoder with export_side_data = mvs + venc_params. Per output frame, in
// output order, it prints one row per AVMotionVector,
//   M <packet index> <source> <w> <h> <src_x> <src_y> <dst_x> <dst_y> <flags> <motion_x> <motion_y> <motion_scale>
// and one per AVVideoBlockParams,
//   Q <packet index> <src_x> <src_y> <w> <h> <qp> <delta_qp>
// where the packet index (in pts) is the access unit that started the frame, as in ovt_plugin_ffmpeg_framemd5.
// Rows are raw side-data fields: the conversion to OVT units is the test's job. Exit 0 on success, 1 on error.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
}

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

void print_side_data(const AVFrame* frame) {
  if (const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS); sd != nullptr) {
    const auto* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
    for (size_t i = 0; i < sd->size / sizeof(AVMotionVector); ++i) {
      const AVMotionVector& m = mvs[i];  // NOLINT(*-pro-bounds-pointer-arithmetic)
      std::cout << "M " << frame->pts << ' ' << int{m.source} << ' ' << int{m.w} << ' ' << int{m.h} << ' ' << m.src_x
                << ' ' << m.src_y << ' ' << m.dst_x << ' ' << m.dst_y << ' ' << m.flags << ' ' << m.motion_x << ' '
                << m.motion_y << ' ' << m.motion_scale << '\n';
    }
  }
  if (const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS); sd != nullptr) {
    auto* par = reinterpret_cast<AVVideoEncParams*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
    for (unsigned i = 0; i < par->nb_blocks; ++i) {
      const AVVideoBlockParams* b = av_video_enc_params_block(par, i);
      std::cout << "Q " << frame->pts << ' ' << b->src_x << ' ' << b->src_y << ' ' << b->w << ' ' << b->h << ' '
                << par->qp << ' ' << b->delta_qp << '\n';
    }
  }
}

bool print_frames(AVCodecContext* ctx, AVFrame* frame) {
  for (;;) {
    const int err = avcodec_receive_frame(ctx, frame);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
      return true;
    }
    if (err < 0) {
      return false;
    }
    print_side_data(frame);
    av_frame_unref(frame);
  }
}

// Sends one packet, reading frames out first when the decoder asks for it; decode errors are ignored, as ffmpeg does.
bool decode(AVCodecContext* ctx, const AVPacket* pkt, AVFrame* frame) {
  if (avcodec_send_packet(ctx, pkt) == AVERROR(EAGAIN)) {
    if (!print_frames(ctx, frame)) {
      return false;
    }
    avcodec_send_packet(ctx, pkt);
  }
  return print_frames(ctx, frame);
}

bool run(std::vector<uint8_t>& bytes, size_t data_size) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
  AVCodecContext* ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = parser != nullptr && ctx != nullptr && pkt != nullptr && frame != nullptr;
  if (ok) {
    ctx->export_side_data = AV_CODEC_EXPORT_DATA_MVS | AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
    ok = avcodec_open2(ctx, codec, nullptr) >= 0;
  }
  int64_t index = 0;
  size_t pos = 0;
  bool eof = false;
  while (ok && !eof) {
    uint8_t* out = nullptr;
    int out_size = 0;
    const size_t remaining = data_size - pos;
    eof = remaining == 0;  // a zero-length call makes the parser return its last access unit
    pos += static_cast<size_t>(av_parser_parse2(parser, ctx, &out, &out_size, eof ? nullptr : &bytes.at(pos),
                                                static_cast<int>(remaining), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0));
    if (out_size > 0) {
      pkt->data = out;
      pkt->size = out_size;
      pkt->pts = index++;
      ok = decode(ctx, pkt, frame);
    }
  }
  ok = ok && decode(ctx, nullptr, frame);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  av_parser_close(parser);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::vector<const char*> args(argv, argv + argc);  // NOLINT(*-pointer-arithmetic)
    std::ifstream in(args.size() == 2 ? args[1] : "", std::ios::binary);
    if (args.size() != 2 || !in) {
      std::cerr << "usage: ovt_plugin_ffmpeg_sidedata <readable file.264>\n";
      return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t data_size = bytes.size();
    bytes.resize(data_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    return run(bytes, data_size) ? 0 : 1;
  } catch (...) {
    return 1;
  }
}
