// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// hm_cu_dump: the HM getter dumper (chapter 14 §14.4.4, WI-0.563).
//
//   hm_cu_dump -b <input>.bit > <id>.hm-grid.json
//
// Decodes an HEVC Annex B stream with HM's TLibDecoder (the decoder loop is TAppDecTop::decode of HM-18.0 minus
// the output writer) and, after each picture has been reconstructed and loop-filtered, walks every CTU's
// TComDataCU and prints the raw getter value of every 4x4 unit in raster order:
//
//   {"frames": [{"poc": N, "w4": W, "h4": H, "log2_ctu_size": L, "chroma_format": C,
//                "cells": {"getPredictionMode": [...], ...}}, ...]}
//
// Frames are in decode order. The cell keys are the ones tests/oracles/normalize/hm_grid_map.json knows; the
// normaliser (normalize/hm_grid.py) maps them to the CellGrid fields, so this program applies no OVT policy except
// two readings of HM's storage: getCbf_* is the cbf bit at the transform-unit leaf (HM keeps one bit per transform
// depth), and getMv_* / getRefIdx_* are those of the 4x4 unit's own motion field (an unused list has refIdx -1).
//
// HM is BSD-3-Clause-Clear (see its COPYING); this file links its static libraries and is oracle-only: it is built
// into the oracle image by build_hm.sh and is never part of OVT.
//
// Pitfall: TDecTop keeps the picture being finished in a private member; the program reads it through
// `#define private public` around the one HM header that declares it, after every standard header is included.
//
// Exit status: 0 success, 1 unreadable or undecodable stream, 2 usage.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "TLibCommon/CommonDef.h"
#include "TLibCommon/NAL.h"
#include "TLibCommon/TComList.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComSlice.h"
#include "TLibDecoder/AnnexBread.h"
#include "TLibDecoder/NALread.h"

#define private public
#include "TLibDecoder/TDecTop.h"
#undef private

namespace {

// One getter column: the name used in the JSON and one value per 4x4 unit of the picture.
struct Column {
  const char* name;
  std::vector<int> cells;
};

enum ColumnId {
  kPredictionMode,
  kPartitionSize,
  kDepth,
  kSkipFlag,
  kMergeFlag,
  kMergeIndex,
  kIntraDirLuma,
  kIntraDirChroma,
  kQp,
  kCbfY,
  kCbfCb,
  kCbfCr,
  kTransformIdx,
  kTransformSkipY,
  kMvL0X,
  kMvL0Y,
  kMvL1X,
  kMvL1Y,
  kRefIdxL0,
  kRefIdxL1,
  kMvdL0X,
  kMvdL0Y,
  kMvdL1X,
  kMvdL1Y,
  kInterDir,
  kMvpIdxL0,
  kMvpIdxL1,
  kIpcmFlag,
  kTransquantBypass,
  kColumnCount
};

const char* const kNames[kColumnCount] = {"getPredictionMode",
                                          "getPartitionSize",
                                          "getDepth",
                                          "getSkipFlag",
                                          "getMergeFlag",
                                          "getMergeIndex",
                                          "getIntraDir_luma",
                                          "getIntraDir_chroma",
                                          "getQP",
                                          "getCbf_Y",
                                          "getCbf_Cb",
                                          "getCbf_Cr",
                                          "getTransformIdx",
                                          "getTransformSkip_Y",
                                          "getMv_L0_x",
                                          "getMv_L0_y",
                                          "getMv_L1_x",
                                          "getMv_L1_y",
                                          "getRefIdx_L0",
                                          "getRefIdx_L1",
                                          "getMvd_L0_x",
                                          "getMvd_L0_y",
                                          "getMvd_L1_x",
                                          "getMvd_L1_y",
                                          "getInterDir",
                                          "getMVPIdx_L0",
                                          "getMVPIdx_L1",
                                          "getIPCMFlag",
                                          "getCUTransquantBypass"};

int ilog2(unsigned v) {
  int n = 0;
  while ((1u << (n + 1)) <= v) {
    ++n;
  }
  return n;
}

// z-scan index of a 4x4 unit inside a CTU -> (x, y) in 4x4 units: the even bits are x, the odd bits are y.
void deinterleave(unsigned z, unsigned* x, unsigned* y) {
  unsigned xx = 0;
  unsigned yy = 0;
  for (unsigned bit = 0; bit < 8; ++bit) {
    xx |= ((z >> (2 * bit)) & 1u) << bit;
    yy |= ((z >> (2 * bit + 1)) & 1u) << bit;
  }
  *x = xx;
  *y = yy;
}

// The cbf of one component at the transform-unit leaf of unit `idx` (HM stores one bit per transform depth).
// In 4:2:0 and 4:2:2 a 4x4 luma transform has no chroma block of its own: the chroma cbf is at the parent depth.
int leaf_cbf(TComDataCU* cu, unsigned idx, ComponentID comp, int log2_tu, ChromaFormat chroma) {
  const unsigned tr_idx = cu->getTransformIdx(idx);
  if (comp == COMPONENT_Y) {
    return cu->getCbf(idx, comp, tr_idx);
  }
  if (chroma == CHROMA_400) {
    return 0;
  }
  unsigned depth = tr_idx;
  if (log2_tu == 2 && chroma != CHROMA_444 && depth > 0) {
    depth -= 1;
  }
  return cu->getCbf(idx, comp, depth);
}

// Appends the cells of one CTU to the picture-wide columns.
void dump_ctu(TComDataCU* cu, unsigned ctu_x4, unsigned ctu_y4, unsigned w4, unsigned h4, unsigned log2_ctu,
              ChromaFormat chroma, std::vector<Column>* cols) {
  const unsigned side = 1u << (log2_ctu - 2);
  const unsigned total = cu->getTotalNumPart();
  if (total != side * side) {
    std::fprintf(stderr, "hm_cu_dump: %u partitions in a CTU of %u x %u units\n", total, side, side);
    std::exit(1);
  }
  for (unsigned z = 0; z < total; ++z) {
    unsigned mx = 0;
    unsigned my = 0;
    deinterleave(z, &mx, &my);
    const unsigned x4 = ctu_x4 + mx;
    const unsigned y4 = ctu_y4 + my;
    if (x4 >= w4 || y4 >= h4) {
      continue;  // beyond the picture edge: HM leaves these units unset
    }
    const size_t at = static_cast<size_t>(y4) * w4 + x4;
    auto set = [&](ColumnId id, int value) { (*cols)[id].cells[at] = value; };

    const int log2_tu =
        static_cast<int>(log2_ctu) - static_cast<int>(cu->getDepth(z)) - static_cast<int>(cu->getTransformIdx(z));
    set(kPredictionMode, cu->getPredictionMode(z));
    set(kPartitionSize, cu->getPartitionSize(z));
    set(kDepth, cu->getDepth(z));
    set(kSkipFlag, cu->getSkipFlag(z));
    set(kMergeFlag, cu->getMergeFlag(z));
    set(kMergeIndex, cu->getMergeIndex(z));
    set(kIntraDirLuma, cu->getIntraDir(CHANNEL_TYPE_LUMA, z));
    set(kIntraDirChroma, cu->getIntraDir(CHANNEL_TYPE_CHROMA, z));
    set(kQp, cu->getQP(z));
    set(kCbfY, leaf_cbf(cu, z, COMPONENT_Y, log2_tu, chroma));
    set(kCbfCb, leaf_cbf(cu, z, COMPONENT_Cb, log2_tu, chroma));
    set(kCbfCr, leaf_cbf(cu, z, COMPONENT_Cr, log2_tu, chroma));
    set(kTransformIdx, cu->getTransformIdx(z));
    set(kTransformSkipY, cu->getTransformSkip(z, COMPONENT_Y));
    const TComCUMvField* l0 = cu->getCUMvField(REF_PIC_LIST_0);
    const TComCUMvField* l1 = cu->getCUMvField(REF_PIC_LIST_1);
    set(kMvL0X, l0->getMv(z).getHor());
    set(kMvL0Y, l0->getMv(z).getVer());
    set(kMvL1X, l1->getMv(z).getHor());
    set(kMvL1Y, l1->getMv(z).getVer());
    set(kRefIdxL0, l0->getRefIdx(z));
    set(kRefIdxL1, l1->getRefIdx(z));
    set(kMvdL0X, l0->getMvd(z).getHor());
    set(kMvdL0Y, l0->getMvd(z).getVer());
    set(kMvdL1X, l1->getMvd(z).getHor());
    set(kMvdL1Y, l1->getMvd(z).getVer());
    set(kInterDir, cu->getInterDir(z));
    set(kMvpIdxL0, cu->getMVPIdx(REF_PIC_LIST_0, z));
    set(kMvpIdxL1, cu->getMVPIdx(REF_PIC_LIST_1, z));
    set(kIpcmFlag, cu->getIPCMFlag(z));
    set(kTransquantBypass, cu->getCUTransquantBypass(z));
  }
}

// Prints one picture as a JSON object (no trailing newline).
void dump_picture(TComPic* pic, bool first) {
  auto sps = pic->getSlice(0)->getSPS();
  const unsigned width = sps->getPicWidthInLumaSamples();
  const unsigned height = sps->getPicHeightInLumaSamples();
  const unsigned ctu_size = sps->getMaxCUWidth();
  const unsigned log2_ctu = static_cast<unsigned>(ilog2(ctu_size));
  const unsigned w4 = (width + 3) / 4;
  const unsigned h4 = (height + 3) / 4;
  const unsigned ctus_x = (width + ctu_size - 1) / ctu_size;
  const ChromaFormat chroma = sps->getChromaFormatIdc();

  std::vector<Column> cols(kColumnCount);
  for (int i = 0; i < kColumnCount; ++i) {
    cols[i].name = kNames[i];
    cols[i].cells.assign(static_cast<size_t>(w4) * h4, 0);
  }
  for (unsigned addr = 0; addr < pic->getNumberOfCtusInFrame(); ++addr) {
    const unsigned cx = addr % ctus_x;
    const unsigned cy = addr / ctus_x;
    dump_ctu(pic->getCtu(addr), cx * (ctu_size / 4), cy * (ctu_size / 4), w4, h4, log2_ctu, chroma, &cols);
  }

  std::printf(
      "%s\n  {\"poc\": %d, \"w4\": %u, \"h4\": %u, \"log2_ctu_size\": %u, \"chroma_format\": %d,\n   \"cells\": {\n",
      first ? "" : ",", pic->getPOC(), w4, h4, log2_ctu, static_cast<int>(chroma));
  for (int i = 0; i < kColumnCount; ++i) {
    std::printf("    \"%s\": [", cols[i].name);
    for (size_t n = 0; n < cols[i].cells.size(); ++n) {
      std::printf("%s%d", n == 0 ? "" : ",", cols[i].cells[n]);
    }
    std::printf("]%s\n", i + 1 == kColumnCount ? "" : ",");
  }
  std::printf("   }}");
}

// TAppDecTop releases a picture once it has written it (xWriteOutput, xFlushOutput): the picture is no longer needed
// for output and, unless it is still a reference, no longer holds a reconstruction. Nothing is written here, so every
// picture is released as soon as it has been dumped.
void release_pictures(TComList<TComPic*>* pic_list) {
  if (pic_list == nullptr) {
    return;
  }
  for (TComPic* pic : *pic_list) {
    if (pic->getOutputMark() && pic->getReconMark() && !pic->getSlice(0)->isReferenced()) {
      pic->setReconMark(false);
      pic->getPicYuvRec()->setBorderExtension(false);
    }
    pic->setOutputMark(false);
  }
}

// TAppDecTop::xFlushOutput minus the writes: release every picture, then destroy them and empty the decoded picture
// buffer. TAppDecTop flushes at an IDR or BLA picture and at the end of the stream; without the flush at an IDR the
// pictures of the previous coded video sequence stay marked as reconstructed with POCs above the IDR's, and HM's
// TComSlice::checkLeadingPictureRestrictions asserts (rpcPic->getPOC() < this->getPOC()).
void flush_pictures(TComList<TComPic*>* pic_list) {
  if (pic_list == nullptr || pic_list->empty()) {
    return;
  }
  release_pictures(pic_list);
  for (TComPic*& pic : *pic_list) {
    pic->destroy();
    delete pic;
    pic = nullptr;
  }
  pic_list->clear();
}

bool is_idr_or_bla(NalUnitType type) {
  return type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || type == NAL_UNIT_CODED_SLICE_IDR_N_LP ||
         type == NAL_UNIT_CODED_SLICE_BLA_N_LP || type == NAL_UNIT_CODED_SLICE_BLA_W_RADL ||
         type == NAL_UNIT_CODED_SLICE_BLA_W_LP;
}

}  // namespace

// Older HM releases (16.0 .. 16.15) define this decode-hash flag in TAppDecTop.cpp, which is not linked here, and
// reference it from TDecGop.cpp. A weak definition keeps the program linkable against them; newer releases do not
// reference the symbol and ignore it.
__attribute__((weak)) bool g_md5_mismatch = false;

int main(int argc, char** argv) {
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc && path == nullptr) {
      path = argv[++i];
    } else {
      std::fprintf(stderr, "usage: hm_cu_dump -b <input>.bit > <id>.hm-grid.json\n");
      return 2;
    }
  }
  if (path == nullptr) {
    std::fprintf(stderr, "usage: hm_cu_dump -b <input>.bit > <id>.hm-grid.json\n");
    return 2;
  }
  std::ifstream bitstream_file(path, std::ifstream::in | std::ifstream::binary);
  if (!bitstream_file) {
    std::fprintf(stderr, "hm_cu_dump: cannot open %s\n", path);
    return 1;
  }

  TDecTop dec;
  dec.create();
  dec.init();
  InputByteStream bytestream(bitstream_file);

  std::printf("{\"frames\": [");
  bool first = true;
  bool loop_filtered = false;
  int poc = 0;
  TComList<TComPic*>* pic_list = nullptr;
  Int skip_frame = 0;
  Int poc_last_display = -MAX_INT;

  // The decoder loop of TAppDecTop::decode (HM-18.0). A picture is complete when the first slice of the next one
  // arrives (decode() returns true and the NAL unit is read again) or at the end of the stream or sequence.
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
        // location points to the current NAL unit payload[1] because the Annex B parser reads three extra bytes
        bitstream_file.seekg(location - std::streamoff(3));
        bytestream.reset();
      }
    } else if (!!bitstream_file) {
      std::fprintf(stderr, "hm_cu_dump: warning: empty NAL unit\n");
    }

    if ((new_picture || !bitstream_file || nalu.m_nalUnitType == NAL_UNIT_EOS) && !dec.getFirstSliceInSequence()) {
      if (!loop_filtered || bitstream_file) {
        dec.executeLoopFilters(poc, pic_list);
        if (dec.m_pcPic != nullptr) {
          dump_picture(dec.m_pcPic, first);
          first = false;
        }
        // Nothing is written to a file: release every dumped picture where TAppDecTop would write it.
        release_pictures(pic_list);
      }
      loop_filtered = (nalu.m_nalUnitType == NAL_UNIT_EOS);
      if (nalu.m_nalUnitType == NAL_UNIT_EOS) {
        dec.setFirstSliceInSequence(true);
      }
    } else if ((new_picture || !bitstream_file || nalu.m_nalUnitType == NAL_UNIT_EOS) &&
               dec.getFirstSliceInSequence()) {
      dec.setFirstSliceInPicture(true);
    }

    // The picture-buffer steps of TAppDecTop::decode that follow the loop filter, minus the writes.
    if (pic_list != nullptr) {
      if ((new_picture || nalu.m_nalUnitType == NAL_UNIT_CODED_SLICE_CRA) && dec.getNoOutputPriorPicsFlag()) {
        dec.checkNoOutputPriorPics(pic_list);
        dec.setNoOutputPriorPicsFlag(false);
      }
      if (new_picture && is_idr_or_bla(nalu.m_nalUnitType)) {
        flush_pictures(pic_list);
      }
      if (nalu.m_nalUnitType == NAL_UNIT_EOS) {
        dec.setFirstSliceInPicture(false);
      }
    }
  }

  flush_pictures(pic_list);
  std::printf("\n]}\n");
  dec.deletePicBuffer();
  return std::fflush(stdout) == 0 ? 0 : 1;
}
