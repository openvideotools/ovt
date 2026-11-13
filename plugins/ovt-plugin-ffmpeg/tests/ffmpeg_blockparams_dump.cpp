// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_plugin_ffmpeg_blockparams <file.264> <threads>: prints the AV_FRAME_DATA_VIDEO_BLOCK_PARAMS side data of the
// patched FFmpeg (plugins/ovt-plugin-ffmpeg/patches, WI-4.101) for every output frame, keyed by the index of the
// packet that started the frame (decoding order). threads > 1 selects frame threading. Per frame:
//   F <idx> <mb_width> <mb_height> <top_field_first>
// then one line per macroblock, in raster order:
//   B <src_x> <src_y> <class> <slice_id> <sub_mb_type x4> <pred_dir x4> <ref_idx l0 x4> <ref_idx l1 x4>
//     <intra4x4 x16> <intra8x8 x4> <intra16x16> <chroma> <cbp> <transform_8x8> <field> <qp_delta> <mvd l0 x32>
//     <mvd l1 x32>
// Exit 0 on success, 1 on error.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/video_block_params.h>
}

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

template <typename T, size_t N>
void put(const T (&values)[N]) {  // NOLINT(*-avoid-c-arrays): the fields of the C struct
  for (const T v : values) {
    std::cout << ' ' << int{v};
  }
}

void print_block(AVH264MacroblockParams& b) {
  std::cout << "B " << b.src_x << ' ' << b.src_y << ' ' << int{b.mb_type_class} << ' ' << int{b.slice_id};
  put(b.sub_mb_type);
  put(b.pred_dir);
  put(b.ref_idx[0]);
  put(b.ref_idx[1]);
  put(b.intra4x4_pred_mode);
  put(b.intra8x8_pred_mode);
  std::cout << ' ' << int{b.intra16x16_pred_mode} << ' ' << int{b.intra_chroma_pred_mode} << ' ' << int{b.cbp} << ' '
            << int{b.transform_8x8} << ' ' << int{b.mb_field_decoding_flag} << ' ' << int{b.qp_delta};
  for (auto& list : b.mvd) {
    for (auto& mvd : list) {
      put(mvd);
    }
  }
  std::cout << '\n';
}

void print_side_data(const AVFrame* frame) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_BLOCK_PARAMS);
  if (sd == nullptr) {
    std::cout << "F " << frame->pts << " 0 0 0\n";
    return;
  }
  auto* par = reinterpret_cast<AVVideoBlockParamsHeader*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
  const int tff = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0 ? 1 : 0;
  std::cout << "F " << frame->pts << ' ' << par->plane_w << ' ' << par->plane_h << ' ' << tff << '\n';
  if (par->type != AV_VIDEO_BLOCK_PARAMS_H264) {
    return;
  }
  for (unsigned i = 0; i < par->nb_blocks; ++i) {
    print_block(*static_cast<AVH264MacroblockParams*>(av_video_block_params_block(par, i)));
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

bool run(std::vector<uint8_t>& bytes, size_t data_size, int threads) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
  AVCodecContext* ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = parser != nullptr && ctx != nullptr && pkt != nullptr && frame != nullptr;
  if (ok) {
    ctx->export_side_data = AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS;
    ctx->thread_count = threads;
    ctx->thread_type = threads > 1 ? FF_THREAD_FRAME : 0;
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
    std::ifstream in(args.size() == 3 ? args[1] : "", std::ios::binary);
    const int threads = args.size() == 3 ? std::stoi(args[2]) : 0;
    if (args.size() != 3 || !in || threads < 1) {
      std::cerr << "usage: ovt_plugin_ffmpeg_blockparams <readable file.264> <threads >= 1>\n";
      return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t data_size = bytes.size();
    bytes.resize(data_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    return run(bytes, data_size, threads) ? 0 : 1;
  } catch (...) {
    return 1;
  }
}
