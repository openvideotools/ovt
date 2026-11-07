// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ffmpeg_mvs: motion vectors and block QPs exported by libavcodec (chapter 14 §14.4.3a, WI-0.563).
//
//   ffmpeg_mvs [--qp <qp.csv>] <input>
//
// `ffprobe` prints only the side-data *type* of AV_FRAME_DATA_MOTION_VECTORS, never the vectors, so this small
// program decodes the first video stream of <input> and writes, to stdout, one CSV row per AVMotionVector:
//
//   framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale
//
// `framenum` counts decoded frames from 1, as FFmpeg's doc/examples/extract_mvs.c does (the program is derived from
// that example, LGPL-2.1-or-later); `flags` is printed as 0x%PRIx64. `normalize/ffmpeg_mvs.py` turns the rows into
// block-grid motion vectors. With --qp the program also exports AV_FRAME_DATA_VIDEO_ENC_PARAMS and writes one row
// per AVVideoBlockParams to <qp.csv>:
//
//   framenum,src_x,src_y,w,h,qp,delta_qp
//
// `qp` is AVVideoEncParams.qp; FFmpeg's qp + delta_qp includes QpBdOffsetY, which the normaliser removes.
// Oracle only: it links the oracle image's FFmpeg and is never part of OVT.
//
// SPEC-ASSUMPTION(#181): the second CSV of §14.4.3a is named on the command line (--qp <file>) because stdout
// carries the motion-vector CSV; the spec gives the file name `<id>.ffmpeg-qp.csv` but no option syntax.
//
// Exit status: 0 success (a stream without motion vectors, such as an intra-only one, writes only the header);
// 1 decode or I/O error; 2 usage.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>

static int usage(void) {
  fprintf(stderr, "usage: ffmpeg_mvs [--qp <qp.csv>] <input>\n");
  return 2;
}

// Writes the rows of one decoded frame. Returns 0, or -1 on a write error.
static int write_frame(const AVFrame* frame, long framenum, FILE* qp_csv) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
  if (sd != NULL) {
    const AVMotionVector* mvs = (const AVMotionVector*)sd->data;
    const size_t count = sd->size / sizeof(*mvs);
    for (size_t i = 0; i < count; ++i) {
      const AVMotionVector* mv = &mvs[i];
      if (printf("%ld,%d,%d,%d,%d,%d,%d,%d,0x%" PRIx64 ",%d,%d,%d\n", framenum, (int)mv->source, (int)mv->w, (int)mv->h,
                 (int)mv->src_x, (int)mv->src_y, (int)mv->dst_x, (int)mv->dst_y, (uint64_t)mv->flags, (int)mv->motion_x,
                 (int)mv->motion_y, (int)mv->motion_scale) < 0) {
        return -1;
      }
    }
  }
  if (qp_csv != NULL) {
    const AVFrameSideData* qd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
    if (qd != NULL) {
      AVVideoEncParams* par = (AVVideoEncParams*)qd->data;  // the accessor takes a non-const pointer
      for (unsigned int i = 0; i < par->nb_blocks; ++i) {
        const AVVideoBlockParams* b = av_video_enc_params_block(par, i);
        if (fprintf(qp_csv, "%ld,%d,%d,%d,%d,%d,%d\n", framenum, b->src_x, b->src_y, b->w, b->h, (int)par->qp,
                    (int)b->delta_qp) < 0) {
          return -1;
        }
      }
    }
  }
  return 0;
}

// Sends one packet (NULL flushes) and writes every frame that comes out. Returns 0, or a negative AVERROR / -1.
static int decode_packet(AVCodecContext* ctx, const AVPacket* pkt, AVFrame* frame, long* framenum, FILE* qp_csv) {
  int ret = avcodec_send_packet(ctx, pkt);
  if (ret < 0) {
    return ret;
  }
  for (;;) {
    ret = avcodec_receive_frame(ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return 0;
    }
    if (ret < 0) {
      return ret;
    }
    *framenum += 1;
    const int wrote = write_frame(frame, *framenum, qp_csv);
    av_frame_unref(frame);
    if (wrote < 0) {
      return -1;
    }
  }
}

int main(int argc, char** argv) {
  const char* qp_path = NULL;
  const char* input = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--qp") == 0 && i + 1 < argc) {
      qp_path = argv[++i];
    } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      return usage();
    } else if (input == NULL) {
      input = argv[i];
    } else {
      return usage();
    }
  }
  if (input == NULL) {
    return usage();
  }

  int status = 1;
  AVFormatContext* fmt = NULL;
  AVCodecContext* ctx = NULL;
  AVPacket* pkt = NULL;
  AVFrame* frame = NULL;
  FILE* qp_csv = NULL;
  long framenum = 0;

  if (avformat_open_input(&fmt, input, NULL, NULL) < 0) {
    fprintf(stderr, "ffmpeg_mvs: cannot open %s\n", input);
    return 1;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    fprintf(stderr, "ffmpeg_mvs: no stream information in %s\n", input);
    goto done;
  }
  const AVCodec* dec = NULL;
  const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
  if (stream < 0 || dec == NULL) {
    fprintf(stderr, "ffmpeg_mvs: no decodable video stream in %s\n", input);
    goto done;
  }
  ctx = avcodec_alloc_context3(dec);
  if (ctx == NULL || avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) < 0) {
    fprintf(stderr, "ffmpeg_mvs: cannot set up the %s decoder\n", dec->name);
    goto done;
  }
  // The export flags must be set before avcodec_open2. AV_CODEC_EXPORT_DATA_MVS is the non-deprecated spelling of
  // AV_CODEC_FLAG2_EXPORT_MVS (flags2 +export_mvs) and works in every FFmpeg since 4.0.
  ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
  if (qp_path != NULL) {
    ctx->export_side_data |= AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
    qp_csv = fopen(qp_path, "w");
    if (qp_csv == NULL) {
      fprintf(stderr, "ffmpeg_mvs: cannot write %s\n", qp_path);
      goto done;
    }
    fprintf(qp_csv, "framenum,src_x,src_y,w,h,qp,delta_qp\n");
  }
  // One thread: the side data is identical for any thread count, but the order of frames is not guaranteed
  // to be easy to reason about with frame threading, and the oracle must be deterministic.
  ctx->thread_count = 1;
  if (avcodec_open2(ctx, dec, NULL) < 0) {
    fprintf(stderr, "ffmpeg_mvs: cannot open the %s decoder\n", dec->name);
    goto done;
  }

  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  if (pkt == NULL || frame == NULL) {
    fprintf(stderr, "ffmpeg_mvs: out of memory\n");
    goto done;
  }
  printf("framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale\n");

  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == stream && decode_packet(ctx, pkt, frame, &framenum, qp_csv) < 0) {
      fprintf(stderr, "ffmpeg_mvs: decode error after frame %ld\n", framenum);
      av_packet_unref(pkt);
      goto done;
    }
    av_packet_unref(pkt);
  }
  if (decode_packet(ctx, NULL, frame, &framenum, qp_csv) < 0) {
    fprintf(stderr, "ffmpeg_mvs: decode error while flushing after frame %ld\n", framenum);
    goto done;
  }
  status = 0;

done:
  if (qp_csv != NULL && fclose(qp_csv) != 0) {
    status = 1;
  }
  if (fflush(stdout) != 0) {
    status = 1;
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  return status;
}
