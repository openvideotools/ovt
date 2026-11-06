// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// gst_parse_oracle: the GStreamer codecparsers oracle of chapter 14 §14.4.2 (WI-0.562).
//
//   gst_parse_oracle --codec h264|h265|av1 [--annexb|--obu|--ivf] <input>
//
// Parses every parameter set, slice header, AV1 sequence header, frame header and tile group of the input with
// GStreamer's codecparsers and prints one JSON object per unit on stdout (JSON Lines, one per input unit):
//
//   {"unit":{"index":0,"offset":4,"type":7},"sps":{...}}
//
// SPEC-ASSUMPTION(#153): JSON Lines, one header-struct document per unit (the spec says both "one object per unit" and
// a single <id>.gstparse.json); arrays of structs print as a struct of arrays.
//
// Every object is a `header-struct` document (tests/oracles/normalize/schema/header-struct.schema.json): one key
// per parsed structure, integers and arrays as values. The members come from tests/oracles/gst_fields.h, generated
// by tools/gen_gst_fields.py; this file names none of them. A unit GStreamer cannot parse prints
// {"unit":{...},"error":{"code":<GstXxxParserResult>}} and the exit status becomes 1.
//
// Exit status: 0 all units parsed, 1 at least one unit failed, 2 usage, 3 the input could not be read.
//
// This file links LGPL GStreamer and is oracle-only: it is built into the oracle image and never shipped.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "gst_fields.h"

typedef struct {
  const uint8_t* data;
  size_t size;
} Input;

typedef struct {
  FILE* out;
  int failures;
} Ctx;

// ---- JSON output over the generated field tables ---------------------------------------------------------------

// Members are naturally aligned inside the GStreamer structures, so a typed load is well defined.
static int64_t read_int(const uint8_t* p, GfKind kind) {
  switch (kind) {
    case GF_U8:
      return *p;
    case GF_I8:
      return *(const int8_t*)p;
    case GF_U16:
      return *(const uint16_t*)(const void*)p;
    case GF_I16:
      return *(const int16_t*)(const void*)p;
    case GF_U32:
      return *(const uint32_t*)(const void*)p;
    case GF_I32:
      return *(const int32_t*)(const void*)p;
    case GF_U64:
      return (int64_t)*(const uint64_t*)(const void*)p;
    case GF_I64:
      return *(const int64_t*)(const void*)p;
  }
  return 0;
}

static void print_value(FILE* out, const uint8_t* p, GfKind kind) {
  if (kind == GF_U64) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    fprintf(out, "%" PRIu64, v);
  } else {
    fprintf(out, "%" PRId64, read_int(p, kind));
  }
}

static void print_array(FILE* out, const uint8_t* base, const GfField* f, int level, size_t first_count) {
  if (level == f->rank) {
    print_value(out, base, f->kind);
    return;
  }
  if (level >= GF_MAX_RANK) {
    return;  // the generator rejects rank > GF_MAX_RANK; this only keeps the compiler's bounds analysis honest
  }
  size_t n = level == 0 ? first_count : f->count[level];
  fputc('[', out);
  for (size_t i = 0; i < n; i++) {
    if (i != 0) {
      fputc(',', out);
    }
    print_array(out, base + (i * f->stride[level]), f, level + 1, first_count);
  }
  fputc(']', out);
}

// Prints the table as a JSON object. Dotted names open and close nested objects; the generator emits the members of
// a nested structure next to each other, so a stack of the current path is enough.
static void print_struct(FILE* out, const void* obj, const GfField* fields, size_t n) {
  const uint8_t* base = obj;
  char open[8][64];  // names of the objects currently open
  int depth = 0;
  fputc('{', out);
  int first_at_level[9] = {1};
  for (size_t i = 0; i < n; i++) {
    const GfField* f = &fields[i];
    char path[256];
    g_strlcpy(path, f->name, sizeof path);
    char* parts[9];
    int np = 0;
    for (char* tok = path; tok != NULL && np < 9;) {
      parts[np++] = tok;
      char* dot = strchr(tok, '.');
      if (dot != NULL) {
        *dot = '\0';
      }
      tok = dot != NULL ? dot + 1 : NULL;
    }
    // Close objects that are not a prefix of this name.
    int common = 0;
    while (common < depth && common < np - 1 && strcmp(open[common], parts[common]) == 0) {
      common++;
    }
    while (depth > common) {
      fputc('}', out);
      depth--;
    }
    for (int k = common; k < np - 1; k++) {
      fprintf(out, "%s\"%s\":{", first_at_level[depth] ? "" : ",", parts[k]);
      g_strlcpy(open[depth], parts[k], sizeof open[depth]);
      depth++;
      first_at_level[depth] = 1;
    }
    fprintf(out, "%s\"%s\":", first_at_level[depth] ? "" : ",", parts[np - 1]);
    first_at_level[depth] = 0;
    // A parent's later siblings need the comma: mark every enclosing level as non-empty.
    for (int k = 0; k < depth; k++) {
      first_at_level[k] = 0;
    }
    size_t first_count = f->rank > 0 ? f->count[0] : 0;
    if (f->rank > 0 && f->limit >= 0) {
      const GfField* lim = &fields[f->limit];
      int64_t v = read_int(base + lim->offset, lim->kind) + f->limit_add;
      first_count = v < 0 ? 0 : ((size_t)v < first_count ? (size_t)v : first_count);
    }
    print_array(out, base + f->offset, f, 0, first_count);
  }
  while (depth > 0) {
    fputc('}', out);
    depth--;
  }
  fputc('}', out);
}

#define EMIT_STRUCT(ctx, key, obj, table)                                       \
  do {                                                                          \
    fprintf((ctx)->out, ",\"%s\":", key);                                       \
    print_struct((ctx)->out, (obj), table, sizeof(table) / sizeof((table)[0])); \
  } while (0)

static void begin_unit(Ctx* ctx, unsigned index, size_t offset, unsigned type) {
  fprintf(ctx->out, "{\"unit\":{\"index\":%u,\"offset\":%zu,\"type\":%u}", index, offset, type);
}

static void end_unit(Ctx* ctx) {
  fputs("}\n", ctx->out);
}

static void unit_error(Ctx* ctx, int code) {
  fprintf(ctx->out, ",\"error\":{\"code\":%d}", code);
  ctx->failures++;
}

// ---- H.264 ------------------------------------------------------------------------------------------------------

static void run_h264(Ctx* ctx, const Input* in) {
  GstH264NalParser* parser = gst_h264_nal_parser_new();
  size_t pos = 0;
  unsigned index = 0;
  while (pos < in->size) {
    GstH264NalUnit nalu;
    GstH264ParserResult r = gst_h264_parser_identify_nalu(parser, in->data, (guint)pos, in->size, &nalu);
    if (r != GST_H264_PARSER_OK && r != GST_H264_PARSER_NO_NAL_END) {
      break;
    }
    begin_unit(ctx, index++, nalu.offset, nalu.type);
    GstH264ParserResult pr = GST_H264_PARSER_OK;
    switch (nalu.type) {
      case GST_H264_NAL_SPS: {
        GstH264SPS sps;
        memset(&sps, 0, sizeof sps);
        pr = gst_h264_parser_parse_sps(parser, &nalu, &sps);
        if (pr == GST_H264_PARSER_OK) {
          EMIT_STRUCT(ctx, "sps", &sps, gst_h264_sps_fields);
        }
        gst_h264_sps_clear(&sps);
        break;
      }
      case GST_H264_NAL_PPS: {
        GstH264PPS pps;
        memset(&pps, 0, sizeof pps);
        pr = gst_h264_parser_parse_pps(parser, &nalu, &pps);
        if (pr == GST_H264_PARSER_OK) {
          EMIT_STRUCT(ctx, "pps", &pps, gst_h264_pps_fields);
        }
        gst_h264_pps_clear(&pps);
        break;
      }
      case GST_H264_NAL_SLICE:
      case GST_H264_NAL_SLICE_IDR: {
        GstH264SliceHdr sh;
        memset(&sh, 0, sizeof sh);
        pr = gst_h264_parser_parse_slice_hdr(parser, &nalu, &sh, TRUE, TRUE);
        if (pr == GST_H264_PARSER_OK) {
          EMIT_STRUCT(ctx, "slice_header", &sh, gst_h264_slice_header_fields);
        }
        break;
      }
      default:
        break;  // SEI, AUD, ... are not header-struct material
    }
    if (pr != GST_H264_PARSER_OK) {
      unit_error(ctx, (int)pr);
    }
    end_unit(ctx);
    if (r == GST_H264_PARSER_NO_NAL_END) {
      break;
    }
    pos = (size_t)nalu.offset + nalu.size;
  }
  gst_h264_nal_parser_free(parser);
}

// ---- H.265 ------------------------------------------------------------------------------------------------------

static void run_h265(Ctx* ctx, const Input* in) {
  GstH265Parser* parser = gst_h265_parser_new();
  size_t pos = 0;
  unsigned index = 0;
  while (pos < in->size) {
    GstH265NalUnit nalu;
    GstH265ParserResult r = gst_h265_parser_identify_nalu(parser, in->data, (guint)pos, in->size, &nalu);
    if (r != GST_H265_PARSER_OK && r != GST_H265_PARSER_NO_NAL_END) {
      break;
    }
    begin_unit(ctx, index++, nalu.offset, nalu.type);
    GstH265ParserResult pr = GST_H265_PARSER_OK;
    if (nalu.type == GST_H265_NAL_VPS) {
      GstH265VPS vps;
      memset(&vps, 0, sizeof vps);
      pr = gst_h265_parser_parse_vps(parser, &nalu, &vps);
      if (pr == GST_H265_PARSER_OK) {
        EMIT_STRUCT(ctx, "vps", &vps, gst_h265_vps_fields);
      }
    } else if (nalu.type == GST_H265_NAL_SPS) {
      GstH265SPS sps;
      memset(&sps, 0, sizeof sps);
      pr = gst_h265_parser_parse_sps(parser, &nalu, &sps, TRUE);
      if (pr == GST_H265_PARSER_OK) {
        EMIT_STRUCT(ctx, "sps", &sps, gst_h265_sps_fields);
      }
    } else if (nalu.type == GST_H265_NAL_PPS) {
      GstH265PPS pps;
      memset(&pps, 0, sizeof pps);
      pr = gst_h265_parser_parse_pps(parser, &nalu, &pps);
      if (pr == GST_H265_PARSER_OK) {
        EMIT_STRUCT(ctx, "pps", &pps, gst_h265_pps_fields);
      }
    } else if (nalu.type <= GST_H265_NAL_SLICE_RASL_R ||
               (nalu.type >= GST_H265_NAL_SLICE_BLA_W_LP && nalu.type <= GST_H265_NAL_SLICE_CRA_NUT)) {
      GstH265SliceHdr sh;
      memset(&sh, 0, sizeof sh);
      pr = gst_h265_parser_parse_slice_hdr(parser, &nalu, &sh);
      if (pr == GST_H265_PARSER_OK) {
        EMIT_STRUCT(ctx, "slice_header", &sh, gst_h265_slice_header_fields);
      }
      gst_h265_slice_hdr_free(&sh);
    }
    if (pr != GST_H265_PARSER_OK) {
      unit_error(ctx, (int)pr);
    }
    end_unit(ctx);
    if (r == GST_H265_PARSER_NO_NAL_END) {
      break;
    }
    pos = (size_t)nalu.offset + nalu.size;
  }
  gst_h265_parser_free(parser);
}

// ---- AV1 --------------------------------------------------------------------------------------------------------

// Parses the OBUs of one chunk (a whole OBU stream, or one IVF frame).
static void av1_chunk(Ctx* ctx, GstAV1Parser* parser, const uint8_t* data, size_t size, size_t base, unsigned* index) {
  size_t pos = 0;
  while (pos < size) {
    GstAV1OBU obu;
    guint32 consumed = 0;
    GstAV1ParserResult r = gst_av1_parser_identify_one_obu(parser, data + pos, (guint32)(size - pos), &obu, &consumed);
    if (r != GST_AV1_PARSER_OK && r != GST_AV1_PARSER_DROP) {
      begin_unit(ctx, (*index)++, base + pos, 0);
      unit_error(ctx, (int)r);
      end_unit(ctx);
      return;  // cannot resynchronise inside an OBU stream
    }
    begin_unit(ctx, (*index)++, base + pos, obu.obu_type);
    EMIT_STRUCT(ctx, "obu_header", &obu.header, gst_av1_obu_header_fields);
    GstAV1ParserResult pr = r;
    if (r == GST_AV1_PARSER_OK) {
      switch (obu.obu_type) {
        case GST_AV1_OBU_SEQUENCE_HEADER: {
          GstAV1SequenceHeaderOBU seq;
          memset(&seq, 0, sizeof seq);
          pr = gst_av1_parser_parse_sequence_header_obu(parser, &obu, &seq);
          if (pr == GST_AV1_PARSER_OK) {
            EMIT_STRUCT(ctx, "sequence_header", &seq, gst_av1_sequence_header_fields);
          }
          break;
        }
        case GST_AV1_OBU_FRAME_HEADER:
        case GST_AV1_OBU_REDUNDANT_FRAME_HEADER: {
          GstAV1FrameHeaderOBU fh;
          memset(&fh, 0, sizeof fh);
          pr = gst_av1_parser_parse_frame_header_obu(parser, &obu, &fh);
          if (pr == GST_AV1_PARSER_OK) {
            EMIT_STRUCT(ctx, "frame_header", &fh, gst_av1_frame_header_fields);
            gst_av1_parser_reference_frame_update(parser, &fh);
          }
          break;
        }
        case GST_AV1_OBU_FRAME: {
          GstAV1FrameOBU frame;
          memset(&frame, 0, sizeof frame);
          pr = gst_av1_parser_parse_frame_obu(parser, &obu, &frame);
          if (pr == GST_AV1_PARSER_OK) {
            EMIT_STRUCT(ctx, "frame_header", &frame.frame_header, gst_av1_frame_header_fields);
            EMIT_STRUCT(ctx, "tile_group", &frame.tile_group, gst_av1_tile_group_fields);
            gst_av1_parser_reference_frame_update(parser, &frame.frame_header);
          }
          break;
        }
        case GST_AV1_OBU_TILE_GROUP: {
          GstAV1TileGroupOBU tg;
          memset(&tg, 0, sizeof tg);
          pr = gst_av1_parser_parse_tile_group_obu(parser, &obu, &tg);
          if (pr == GST_AV1_PARSER_OK) {
            EMIT_STRUCT(ctx, "tile_group", &tg, gst_av1_tile_group_fields);
          }
          break;
        }
        default:
          break;  // temporal delimiter, metadata, padding, tile list
      }
    }
    if (pr != GST_AV1_PARSER_OK && pr != GST_AV1_PARSER_DROP) {
      unit_error(ctx, (int)pr);
    }
    end_unit(ctx);
    if (consumed == 0) {
      return;
    }
    pos += consumed;
  }
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void run_av1(Ctx* ctx, const Input* in, int ivf) {
  GstAV1Parser* parser = gst_av1_parser_new();
  unsigned index = 0;
  if (!ivf) {
    av1_chunk(ctx, parser, in->data, in->size, 0, &index);
  } else if (in->size >= 32 && memcmp(in->data, "DKIF", 4) == 0) {
    size_t pos = (size_t)(in->data[6] | (in->data[7] << 8));  // header length, normally 32
    while (pos + 12 <= in->size) {
      size_t frame_size = le32(in->data + pos);
      pos += 12;
      if (frame_size > in->size - pos) {
        frame_size = in->size - pos;
      }
      av1_chunk(ctx, parser, in->data + pos, frame_size, pos, &index);
      pos += frame_size;
    }
  } else {
    fprintf(stderr, "gst_parse_oracle: not an IVF file (no DKIF signature)\n");
    ctx->failures++;
  }
  gst_av1_parser_free(parser);
}

// ---- main -------------------------------------------------------------------------------------------------------

static int usage(void) {
  fputs(
      "usage: gst_parse_oracle --codec h264|h265|av1 [--annexb|--obu|--ivf] <input>\n"
      "  h264/h265 read Annex B (--annexb, the default); av1 reads an OBU stream (--obu) or IVF (--ivf);\n"
      "  av1 input starting with DKIF is taken as IVF when no format is given.\n",
      stderr);
  return 2;
}

int main(int argc, char** argv) {
  const char* codec = NULL;
  const char* format = NULL;
  const char* path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
      codec = argv[++i];
    } else if (strcmp(argv[i], "--annexb") == 0 || strcmp(argv[i], "--obu") == 0 || strcmp(argv[i], "--ivf") == 0) {
      format = argv[i] + 2;
    } else if (argv[i][0] != '-' && path == NULL) {
      path = argv[i];
    } else {
      return usage();
    }
  }
  if (codec == NULL || path == NULL) {
    return usage();
  }
  int is_h264 = strcmp(codec, "h264") == 0;
  int is_h265 = strcmp(codec, "h265") == 0;
  int is_av1 = strcmp(codec, "av1") == 0;
  if (!is_h264 && !is_h265 && !is_av1) {
    return usage();
  }
  if (format != NULL && ((!is_av1 && strcmp(format, "annexb") != 0) || (is_av1 && strcmp(format, "annexb") == 0))) {
    fprintf(stderr, "gst_parse_oracle: --%s does not apply to %s\n", format, codec);
    return 2;
  }

  gchar* contents = NULL;
  gsize length = 0;
  GError* error = NULL;
  if (!g_file_get_contents(path, &contents, &length, &error)) {
    fprintf(stderr, "gst_parse_oracle: %s\n", error->message);
    g_error_free(error);
    return 3;
  }
  Input in = {(const uint8_t*)contents, length};
  Ctx ctx = {stdout, 0};
  if (is_h264) {
    run_h264(&ctx, &in);
  } else if (is_h265) {
    run_h265(&ctx, &in);
  } else {
    int ivf = format != NULL ? strcmp(format, "ivf") == 0 : (length >= 4 && memcmp(contents, "DKIF", 4) == 0);
    run_av1(&ctx, &in, ivf);
  }
  g_free(contents);
  return ctx.failures != 0 ? 1 : 0;
}
