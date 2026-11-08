// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_plugin_ffmpeg_framemd5 <file.264>: `ffmpeg -i <file.264> -f framemd5 -` in-tree (WI-2.169 AT1 oracle, so CI
// needs no ffmpeg executable). h264 parser + decoder, default cropping; prints "<packet index> <md5>" per output
// frame in output order, where the packet index (in pts) is the access unit that started the frame and the MD5 is
// over the cropped picture packed with alignment 1, as framemd5 hashes rawvideo. Exit 0 on success, 1 on error.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
}

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

// Prints every frame the decoder has ready; false on a decoder error.
bool print_frames(AVCodecContext* ctx, AVFrame* frame) {
  constexpr std::array<char, 17> kHex = {"0123456789abcdef"};
  for (;;) {
    const int err = avcodec_receive_frame(ctx, frame);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
      return true;
    }
    const auto fmt = static_cast<AVPixelFormat>(frame->format);
    const int size = err < 0 ? err : av_image_get_buffer_size(fmt, frame->width, frame->height, 1);
    if (size < 0) {
      return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    av_image_copy_to_buffer(buf.data(), size, frame->data,  // NOLINT(*-array-to-pointer-decay): FFmpeg API
                            frame->linesize, fmt, frame->width, frame->height, 1);  // NOLINT(*-array-to-pointer-decay)
    std::array<uint8_t, 16> md5{};
    av_md5_sum(md5.data(), buf.data(), buf.size());
    std::cout << frame->pts << ' ';
    for (const uint8_t b : md5) {
      std::cout << kHex.at(b >> 4) << kHex.at(b & 15U);
    }
    std::cout << '\n';
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
  bool ok = parser != nullptr && ctx != nullptr && pkt != nullptr && frame != nullptr &&
            avcodec_open2(ctx, codec, nullptr) >= 0;
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
  const std::vector<const char*> args(argv, argv + argc);  // NOLINT(*-pointer-arithmetic)
  if (args.size() != 2) {
    std::cerr << "usage: ovt_plugin_ffmpeg_framemd5 <file.264>\n";
    return 1;
  }
  std::ifstream in(args[1], std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << args[1] << '\n';
    return 1;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const size_t data_size = bytes.size();
  bytes.resize(data_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  return run(bytes, data_size) ? 0 : 1;
}
