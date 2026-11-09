// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// ovt_plugin_hm_smoke: the smoke program of the HM build (chapter 07 §7.6.4, WI-4.104).
//
//   ovt_plugin_hm_smoke <stream>.bit
//
// Decodes an HEVC Annex B stream with HM's TDecTop (the decoder loop of TAppDecTop::decode in HM-18.0, minus the
// output writer) and prints one line per picture, in decode order:
//
//   ovt_plugin_hm_smoke: POC 3 MD5 OK
//
// The hash is the picture's decoded picture hash SEI (MD5, CRC or checksum), which HM checks itself in
// TDecGop::filterPicture. Exit status 0 when at least one picture was decoded, every picture carried a hash SEI that
// matched, and the POCs are 0..N with none missing or repeated (WI-4.104 acceptance test 1); 1 otherwise; 2 on a usage
// error. HM's own status lines go to stdout as well.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "TLibCommon/CommonDef.h"
#include "TLibCommon/NAL.h"
#include "TLibCommon/SEI.h"
#include "TLibCommon/TComList.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComSlice.h"
#include "TLibDecoder/AnnexBread.h"
#include "TLibDecoder/NALread.h"
#include "TLibDecoder/TDecTop.h"

namespace {

const char* hash_name(HashType type) {
  switch (type) {
    case HASHTYPE_MD5:
      return "MD5";
    case HASHTYPE_CRC:
      return "CRC";
    case HASHTYPE_CHECKSUM:
      return "checksum";
    default:
      return "hash";
  }
}

struct Picture {
  int poc = 0;
  bool hashed = false;
  bool matched = false;
};

// Records the picture HM has just loop-filtered: TDecGop::filterPicture compared its reconstruction with its hash SEI
// and counted a mismatch in getNumberOfChecksumErrorsDetected().
void record(TDecTop& dec, unsigned errors_before, std::vector<Picture>& pictures) {
  TComPic* pic = dec.getPcPic();
  if (pic == nullptr) {
    return;
  }
  Picture out;
  out.poc = pic->getPOC();
  SEIMessages hashes = getSeisByType(pic->getSEIs(), SEI::DECODED_PICTURE_HASH);
  const char* type = "hash";
  if (!hashes.empty()) {
    out.hashed = true;
    type = hash_name(static_cast<const SEIDecodedPictureHash*>(hashes.front())->method);
  }
  out.matched = out.hashed && dec.getNumberOfChecksumErrorsDetected() == errors_before;
  std::cout << "\novt_plugin_hm_smoke: POC " << out.poc << " " << type << " "
            << (!out.hashed ? "MISSING" : (out.matched ? "OK" : "MISMATCH")) << std::endl;  // flush: HM uses printf
  pictures.push_back(out);
}

// TAppDecTop releases a picture once it has written it; nothing is written here, so every picture is released as soon
// as it has been loop-filtered (the same steps as tests/oracles/hm_cu_dump.cpp).
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

// TAppDecTop::xFlushOutput minus the writes, at an IDR or BLA picture and at the end of the stream.
void flush_pictures(TComList<TComPic*>* pic_list, Int* poc_last_display) {
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
  *poc_last_display = -MAX_INT;
}

bool is_idr_or_bla(NalUnitType type) {
  return type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || type == NAL_UNIT_CODED_SLICE_IDR_N_LP ||
         type == NAL_UNIT_CODED_SLICE_BLA_N_LP || type == NAL_UNIT_CODED_SLICE_BLA_W_RADL ||
         type == NAL_UNIT_CODED_SLICE_BLA_W_LP;
}

int verdict(const std::vector<Picture>& pictures) {
  if (pictures.empty()) {
    std::cerr << "ovt_plugin_hm_smoke: no picture decoded\n";
    return 1;
  }
  bool ok = true;
  for (const Picture& pic : pictures) {
    if (!pic.matched) {
      std::cerr << "ovt_plugin_hm_smoke: POC " << pic.poc << ": hash SEI " << (pic.hashed ? "mismatch" : "missing")
                << "\n";
      ok = false;
    }
  }
  std::vector<int> pocs;
  pocs.reserve(pictures.size());
  for (const Picture& pic : pictures) {
    pocs.push_back(pic.poc);
  }
  std::sort(pocs.begin(), pocs.end());
  for (std::size_t i = 0; i < pocs.size(); ++i) {
    if (pocs[i] != static_cast<int>(i)) {
      std::cerr << "ovt_plugin_hm_smoke: the POCs are not 0.." << pocs.size() - 1 << " (position " << i << " holds "
                << pocs[i] << ")\n";
      ok = false;
      break;
    }
  }
  std::cout << "ovt_plugin_hm_smoke: " << pictures.size() << " pictures, POC 0.." << pocs.back() << ", "
            << (ok ? "all hashes OK" : "FAILED") << "\n";
  return ok ? 0 : 1;
}

// The decoder loop of TAppDecTop::decode (HM-18.0), minus the output writer. A picture is complete when the first
// slice of the next one arrives (decode() returns true and the NAL unit is read again) or at the end of the stream or
// sequence.
class Decoder {
 public:
  explicit Decoder(std::ifstream& file) : file_(file), bytestream_(file) {
    dec_.create();
    dec_.init();
    dec_.setDecodedPictureHashSEIEnabled(1);  // TAppDecoder's default (--SEIDecodedPictureHash=1)
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;
  ~Decoder() {
    flush_pictures(pic_list_, &poc_last_display_);
    dec_.deletePicBuffer();
    dec_.destroy();
  }

  std::vector<Picture> run() {
    while (!!file_) {
      step();
    }
    return std::move(pictures_);
  }

 private:
  void step() {
    const std::streampos location = file_.tellg();
    AnnexBStats stats = AnnexBStats();
    InputNALUnit nalu;
    byteStreamNALUnit(bytestream_, nalu.getBitstream().getFifo(), stats);

    bool new_picture = false;
    if (!nalu.getBitstream().getFifo().empty()) {
      read(nalu);
      new_picture = dec_.decode(nalu, skip_frame_, poc_last_display_);
      if (new_picture) {
        file_.clear();
        // location points to the current NAL unit payload[1] because the Annex B parser reads three extra bytes
        file_.seekg(location - std::streamoff(3));
        bytestream_.reset();
      }
    }
    const bool eos = nalu.m_nalUnitType == NAL_UNIT_EOS;
    if (new_picture || !file_ || eos) {
      finish_picture(eos);
    }
    manage_buffer(nalu.m_nalUnitType, new_picture);
  }

  void finish_picture(bool eos) {
    if (dec_.getFirstSliceInSequence()) {
      dec_.setFirstSliceInPicture(true);
      return;
    }
    if (!loop_filtered_ || !!file_) {
      const unsigned errors_before = dec_.getNumberOfChecksumErrorsDetected();
      int poc = 0;
      dec_.executeLoopFilters(poc, pic_list_);
      record(dec_, errors_before, pictures_);
      release_pictures(pic_list_);
    }
    loop_filtered_ = eos;
    if (eos) {
      dec_.setFirstSliceInSequence(true);
    }
  }

  void manage_buffer(NalUnitType type, bool new_picture) {
    if (pic_list_ == nullptr) {
      return;
    }
    if ((new_picture || type == NAL_UNIT_CODED_SLICE_CRA) && dec_.getNoOutputPriorPicsFlag()) {
      dec_.checkNoOutputPriorPics(pic_list_);
      dec_.setNoOutputPriorPicsFlag(false);
    }
    if (new_picture && is_idr_or_bla(type)) {
      flush_pictures(pic_list_, &poc_last_display_);
    }
    if (type == NAL_UNIT_EOS) {
      dec_.setFirstSliceInPicture(false);
    }
  }

  std::ifstream& file_;
  InputByteStream bytestream_;
  TDecTop dec_;
  std::vector<Picture> pictures_;
  bool loop_filtered_ = false;
  TComList<TComPic*>* pic_list_ = nullptr;
  Int skip_frame_ = 0;
  Int poc_last_display_ = -MAX_INT;
};

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  if (args.size() != 2) {
    std::cerr << "usage: ovt_plugin_hm_smoke <stream>.bit\n";
    return 2;
  }
  std::ifstream file(args[1], std::ifstream::in | std::ifstream::binary);
  if (!file) {
    std::cerr << "ovt_plugin_hm_smoke: cannot open " << args[1] << "\n";
    return 1;
  }
  std::vector<Picture> pictures;
  {
    Decoder decoder(file);
    pictures = decoder.run();
  }
  return verdict(pictures);
}
