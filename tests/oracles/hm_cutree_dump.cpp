// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// hm_cutree_dump: the CU tree and the per-CTB SAO parameters of every picture, after executeLoopFilters (format
// hm-cutree/1; WI-2.164 acceptance tests 1 and 3). One record per CU in z-order within each CTU (position, size,
// ctDepth, part_mode, pred_mode, skip, PCM, transquant bypass) and, per CTB, the resolved (merged, scaled) SAO type,
// edge class, band position and SaoOffsetVal[1..4] of Y, Cb and Cr.
//
//   hm_cutree_dump -b <input>.bit -s <stream id> -h <input sha256> > <id>.hm-cutree.json
//
// Oracle-only, like hm_cu_dump.cpp, whose decoder loop it shares (HM-18.0, BSD-3-Clause-Clear). It produced
// tests/golden/h265/jctvc_AMP_A_Samsung_7.hm-cutree.json.gz and jctvc_SAO_A_MediaTek_4.hm-cutree.json.gz, linked
// against the stock HM libraries of the oracle image (output gzipped with mtime 0). It is not built by
// build_hm.sh yet.
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "TLibCommon/CommonDef.h"
#include "TLibCommon/NAL.h"
#include "TLibCommon/TComList.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComRom.h"
#include "TLibCommon/TComSlice.h"
#include "TLibDecoder/AnnexBread.h"
#include "TLibDecoder/NALread.h"

#define private public
#include "TLibDecoder/TDecTop.h"
#undef private

namespace {

FILE* g_json = nullptr;
bool g_first_cu = true;

void emit_cu(TComDataCU* cu, unsigned abs, unsigned depth, unsigned max_cu, unsigned width, unsigned height) {
  const unsigned x = cu->getCUPelX() + g_auiRasterToPelX[g_auiZscanToRaster[abs]];
  const unsigned y = cu->getCUPelY() + g_auiRasterToPelY[g_auiZscanToRaster[abs]];
  if (x >= width || y >= height) {
    return;  // wholly outside the picture: HM leaves these parts unset
  }
  const unsigned size = max_cu >> depth;
  if (cu->getDepth(abs) > depth) {
    const unsigned quarter = cu->getPic()->getNumPartitionsInCtu() >> (2 * (depth + 1));
    for (unsigned k = 0; k < 4; ++k) {
      emit_cu(cu, abs + k * quarter, depth + 1, max_cu, width, height);
    }
    return;
  }
  unsigned log2 = 0;
  while ((1u << log2) < size) {
    ++log2;
  }
  std::fprintf(g_json,
               "%s{\"x\":%u,\"y\":%u,\"log2_size\":%u,\"depth\":%d,\"part_mode\":%d,\"pred_mode\":%d,\"skip\":%d,"
               "\"pcm\":%d,\"bypass\":%d}",
               g_first_cu ? "" : ",", x, y, log2, static_cast<int>(cu->getDepth(abs)),
               static_cast<int>(cu->getPartitionSize(abs)), static_cast<int>(cu->getPredictionMode(abs)),
               cu->getSkipFlag(abs) ? 1 : 0, cu->getIPCMFlag(abs) ? 1 : 0, cu->getCUTransquantBypass(abs) ? 1 : 0);
  g_first_cu = false;
}

void emit_sao(const SAOOffset& p, bool last) {
  int type_idx = 0, eo_class = 0, band = 0;
  int off[4] = {0, 0, 0, 0};
  if (p.modeIdc != SAO_MODE_OFF) {
    if (p.typeIdc == SAO_TYPE_START_BO) {
      type_idx = 1;
      band = p.typeAuxInfo;
      for (int k = 0; k < 4; ++k) off[k] = p.offset[(p.typeAuxInfo + k) % NUM_SAO_BO_CLASSES];
    } else {
      type_idx = 2;
      eo_class = p.typeIdc;
      off[0] = p.offset[0];
      off[1] = p.offset[1];
      off[2] = p.offset[3];
      off[3] = p.offset[4];
    }
  }
  std::fprintf(g_json, "{\"type_idx\":%d,\"eo_class\":%d,\"band_position\":%d,\"offsets\":[%d,%d,%d,%d]}%s", type_idx,
               eo_class, band, off[0], off[1], off[2], off[3], last ? "" : ",");
}

void dump_picture(TComPic* pic, int frame) {
  const TComSPS* sps = pic->getSlice(0)->getSPS();
  const unsigned width = sps->getPicWidthInLumaSamples();
  const unsigned height = sps->getPicHeightInLumaSamples();
  const unsigned max_cu = sps->getMaxCUWidth();
  unsigned log2_ctu = 0;
  while ((1u << log2_ctu) < max_cu) ++log2_ctu;
  std::fprintf(g_json, "%s{\"frame\":%d,\"poc\":%d,\"width\":%u,\"height\":%u,\"log2_ctu_size\":%u,\"ctus\":[",
               frame == 0 ? "" : ",", frame, pic->getPOC(), width, height, log2_ctu);
  SAOBlkParam* sao = pic->getPicSym()->getSAOBlkParam();
  for (unsigned rs = 0; rs < pic->getNumberOfCtusInFrame(); ++rs) {
    TComDataCU* cu = pic->getCtu(rs);
    const TComSlice* slice = cu->getSlice();
    std::fprintf(g_json, "%s{\"rs\":%u,\"sao_luma\":%d,\"sao_chroma\":%d,\"sao\":[", rs == 0 ? "" : ",", rs,
                 slice->getSaoEnabledFlag(CHANNEL_TYPE_LUMA) ? 1 : 0,
                 slice->getSaoEnabledFlag(CHANNEL_TYPE_CHROMA) ? 1 : 0);
    for (int c = 0; c < 3; ++c) emit_sao(sao[rs][c], c == 2);
    std::fprintf(g_json, "],\"cus\":[");
    g_first_cu = true;
    emit_cu(cu, 0, 0, max_cu, width, height);
    std::fprintf(g_json, "]}");
  }
  std::fprintf(g_json, "]}");
}

void release_pictures(TComList<TComPic*>* pic_list) {
  if (pic_list == nullptr) return;
  for (TComPic* pic : *pic_list) {
    if (pic->getOutputMark() && pic->getReconMark() && !pic->getSlice(0)->isReferenced()) {
      pic->setReconMark(false);
      pic->getPicYuvRec()->setBorderExtension(false);
    }
    pic->setOutputMark(false);
  }
}

void flush_pictures(TComList<TComPic*>* pic_list, Int* poc_last_display) {
  if (pic_list == nullptr || pic_list->empty()) return;
  release_pictures(pic_list);
  for (TComPic*& pic : *pic_list) {
    pic->destroy();
    delete pic;
    pic = nullptr;
  }
  pic_list->clear();
  *poc_last_display = -MAX_INT;
}

bool is_idr_or_bla(NalUnitType type) {
  return type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || type == NAL_UNIT_CODED_SLICE_IDR_N_LP ||
         type == NAL_UNIT_CODED_SLICE_BLA_N_LP || type == NAL_UNIT_CODED_SLICE_BLA_W_RADL ||
         type == NAL_UNIT_CODED_SLICE_BLA_W_LP;
}

}  // namespace

__attribute__((weak)) bool g_md5_mismatch = false;

int main(int argc, char** argv) {
  const char* path = nullptr;
  const char* stream = "";
  const char* sha = "";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "-b") == 0)
      path = argv[i + 1];
    else if (std::strcmp(argv[i], "-s") == 0)
      stream = argv[i + 1];
    else if (std::strcmp(argv[i], "-h") == 0)
      sha = argv[i + 1];
  }
  if (path == nullptr) {
    std::fprintf(stderr, "usage: hm_cutree_dump -b <bit> -s <id> -h <sha256>\n");
    return 2;
  }
  std::ifstream bitstream_file(path, std::ifstream::in | std::ifstream::binary);
  if (!bitstream_file) return 1;
  std::fflush(stdout);
  const int json_fd = dup(STDOUT_FILENO);
  g_json = json_fd >= 0 ? fdopen(json_fd, "w") : nullptr;
  if (g_json == nullptr || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 1;

  TDecTop dec;
  dec.create();
  dec.init();
  InputByteStream bytestream(bitstream_file);
  std::fprintf(g_json,
               "{\"format\":\"hm-cutree/1\",\"stream\":\"%s\",\"input_sha256\":\"%s\",\"hm\":{\"version\":\"HM-18.0\","
               "\"commit\":\"fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce\"},\"frames\":[",
               stream, sha);
  int frame = 0;
  bool loop_filtered = false;
  int poc = 0;
  TComList<TComPic*>* pic_list = nullptr;
  Int skip_frame = 0;
  Int poc_last_display = -MAX_INT;
  while (!!bitstream_file) {
    std::streampos location = bitstream_file.tellg();
    AnnexBStats stats = AnnexBStats();
    InputNALUnit nalu;
    byteStreamNALUnit(bytestream, nalu.getBitstream().getFifo(), stats);
    bool new_picture = false;
    if (!nalu.getBitstream().getFifo().empty()) {
      read(nalu);
      new_picture = dec.decode(nalu, skip_frame, poc_last_display);
      if (new_picture) {
        bitstream_file.clear();
        bitstream_file.seekg(location - std::streamoff(3));
        bytestream.reset();
      }
    }
    if ((new_picture || !bitstream_file || nalu.m_nalUnitType == NAL_UNIT_EOS) && !dec.getFirstSliceInSequence()) {
      if (!loop_filtered || bitstream_file) {
        dec.executeLoopFilters(poc, pic_list);
        if (dec.m_pcPic != nullptr) {
          dump_picture(dec.m_pcPic, frame++);
        }
        release_pictures(pic_list);
      }
      loop_filtered = (nalu.m_nalUnitType == NAL_UNIT_EOS);
      if (nalu.m_nalUnitType == NAL_UNIT_EOS) dec.setFirstSliceInSequence(true);
    } else if ((new_picture || !bitstream_file || nalu.m_nalUnitType == NAL_UNIT_EOS) &&
               dec.getFirstSliceInSequence()) {
      dec.setFirstSliceInPicture(true);
    }
    if (pic_list != nullptr) {
      if ((new_picture || nalu.m_nalUnitType == NAL_UNIT_CODED_SLICE_CRA) && dec.getNoOutputPriorPicsFlag()) {
        dec.checkNoOutputPriorPics(pic_list);
        dec.setNoOutputPriorPicsFlag(false);
      }
      if (new_picture && is_idr_or_bla(nalu.m_nalUnitType)) flush_pictures(pic_list, &poc_last_display);
      if (nalu.m_nalUnitType == NAL_UNIT_EOS) dec.setFirstSliceInPicture(false);
    }
  }
  flush_pictures(pic_list, &poc_last_display);
  std::fprintf(g_json, "]}\n");
  dec.deletePicBuffer();
  std::fflush(stdout);
  const bool write_error = std::ferror(g_json) != 0;
  return (std::fclose(g_json) == 0 && !write_error) ? 0 : 1;
}
