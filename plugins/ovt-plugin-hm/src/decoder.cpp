// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// HM driving for ovt-plugin-hm (chapter 07 §7.6.4 "Decoder driving", WI-4.105), checked against HM-18.0's
// TAppDecTop::decode()/xFlushOutput() and TDecTop; hm_cu_dump.cpp and tests/hm_smoke.cpp follow the same loop.

#include "decoder.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <new>
#include <set>
#include <utility>

// HM's headers (system includes: HM's warnings are not OVT's, WI-4.104).
#include "TLibCommon/CommonDef.h"
#include "TLibCommon/NAL.h"
#include "TLibCommon/TComChromaFormat.h"
#include "TLibCommon/TComDataCU.h"
#include "TLibCommon/TComList.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComPicYuv.h"
#include "TLibCommon/TComRom.h"
#include "TLibCommon/TComSlice.h"
#include "TLibDecoder/NALread.h"
#include "TLibDecoder/TDecTop.h"

namespace ovt::hm {
namespace {

constexpr uint8_t kInfo = 0;  // ovt.fb.Severity values
constexpr uint8_t kError = 2;

// Diagnostics kept for the next frame; more are counted in one summary line.
constexpr std::size_t kMaxPendingDiags = 64;

constexpr int kNalVclMax = 31;
constexpr int kNalEos = 36;

// HM keeps decoder state in globals: TDecTop::init() allocates g_scanOrder (initROM), deletePicBuffer() frees it
// (destroyROM), and TDecCu::create() rebuilds the z-scan tables for the CTU size of each picture it starts. So every
// call into HM runs under one mutex, one set of scan-order tables lives while any instance does, and the z-scan
// tables are rebuilt for the calling instance's CTU size before HM resumes it.

std::mutex g_hm_mutex;
int g_live_instances = 0;              // guarded by g_hm_mutex
const void* g_tables_owner = nullptr;  // the instance the z-scan tables were last built for; guarded by g_hm_mutex

using ScanOrder = decltype(g_scanOrder);

void save_scan_order(ScanOrder* out) {
  std::memcpy(out, &g_scanOrder, sizeof(g_scanOrder));
}

void restore_scan_order(const ScanOrder& saved) {
  std::memcpy(&g_scanOrder, &saved, sizeof(g_scanOrder));
}

bool is_idr_or_bla(NalUnitType type) {
  return type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || type == NAL_UNIT_CODED_SLICE_IDR_N_LP ||
         type == NAL_UNIT_CODED_SLICE_BLA_N_LP || type == NAL_UNIT_CODED_SLICE_BLA_W_RADL ||
         type == NAL_UNIT_CODED_SLICE_BLA_W_LP;
}

// TAppDecTop releases a picture once written; the plugin releases it once copied (as hm_cu_dump and hm_smoke do).
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
void flush_pictures(TComList<TComPic*>* pic_list, Int* poc_last_display, PicIndex* index) {
  index->clear();
  if (pic_list == nullptr || pic_list->empty()) {
    return;
  }
  release_pictures(pic_list);
  for (TComPic*& pic : *pic_list) {
    pic->destroy();
    delete pic;  // NOLINT(cppcoreguidelines-owning-memory): HM's picture list owns raw pointers
    pic = nullptr;
  }
  pic_list->clear();
  *poc_last_display = -MAX_INT;
}

// FrameInfo.type is the slice majority (ovt_model.fbs); a tie goes to the type with more prediction.
uint8_t majority_type(const std::array<int, 3>& per_type) {
  const int b = per_type[B_SLICE];
  const int p = per_type[P_SLICE];
  const int i = per_type[I_SLICE];
  if (b + p + i == 0) {
    return 7;
  }
  return (b >= p && b >= i) ? 2 : (p >= i ? 1 : 0);
}

// Copies one component of the reconstruction: 8-bit pictures packed to one byte per sample (V-023), deeper ones as
// two little-endian bytes. HM's Pel is a 32-bit Int with RExt__HIGH_BIT_DEPTH_SUPPORT=1 (SPEC-ASSUMPTION(#442)).
void copy_plane(const TComPicYuv& rec, ComponentID comp, int bit_depth, Plane* out) {
  const int w = rec.getWidth(comp);
  const int h = rec.getHeight(comp);
  const int stride = rec.getStride(comp);
  const Pel* origin = rec.getAddr(comp);
  if (origin == nullptr || w <= 0 || h <= 0 || stride < w) {
    return;
  }
  const std::span<const Pel> src(
      origin, (static_cast<std::size_t>(stride) * static_cast<std::size_t>(h - 1)) + static_cast<std::size_t>(w));
  const bool narrow = bit_depth <= 8;
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->bit_depth = static_cast<uint8_t>(bit_depth);
  out->sample_bytes = narrow ? 1 : 2;
  out->data.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * out->sample_bytes);
  auto dst = out->data.begin();
  for (std::size_t y = 0; y < static_cast<std::size_t>(h); ++y) {
    for (const Pel sample : src.subspan(y * static_cast<std::size_t>(stride), static_cast<std::size_t>(w))) {
      const auto v = static_cast<uint32_t>(sample);
      *dst++ = static_cast<uint8_t>(v);
      if (!narrow) {
        *dst++ = static_cast<uint8_t>(v >> 8);
      }
    }
  }
}

}  // namespace

struct Decoder::Impl {
  TDecTop dec;
  bool created = false;
  TComList<TComPic*>* pic_list = nullptr;
  Int skip_frame = 0;
  Int poc_last_display = -MAX_INT;
  bool loop_filtered = false;
  bool flushed = false;
  // CTU geometry of the last picture this instance started, for restore_tables().
  UInt max_depth = 0;
  UInt max_cu_w = 0;
  UInt max_cu_h = 0;

  // The picture HM is decoding (m_pcPic) and the decode index hint and pts of the unit that started it. HM finishes
  // pictures one at a time in decode order, so one record replaces the POC-keyed side map of §7.6.4.
  const TComPic* cur_pic = nullptr;
  const TComSlice* cur_slice0 = nullptr;
  int32_t cur_index = -1;
  int64_t cur_pts = 0;
  int32_t next_index = 0;  // decode index when the host gave none
  PicIndex pic_index;      // decode index of every exported picture HM still holds
  std::deque<Frame> frames;
  std::vector<Diag> pending_diags;  // attached to the next frame that comes out

  void add_diag(uint8_t severity, std::string code, std::string message) {
    if (pending_diags.size() < kMaxPendingDiags) {
      pending_diags.push_back(Diag{severity, std::move(code), std::move(message)});
    } else if (pending_diags.size() == kMaxPendingDiags) {
      pending_diags.push_back(Diag{kInfo, "", "further HM diagnostics dropped"});
    }
  }

  // Called with g_hm_mutex held before HM resumes this instance's picture.
  void restore_tables() {
    if (g_tables_owner != this && max_depth != 0) {
      UInt* cursor = &g_auiZscanToRaster[0];
      initZscanToRaster(static_cast<Int>(max_depth), 1, 0, cursor);
      initRasterToZscan(max_cu_w, max_cu_h, max_depth);
      initRasterToPelXY(max_cu_w, max_cu_h, max_depth);
    }
    g_tables_owner = this;
  }

  // After HM consumed a slice: a picture's first slice swaps a new slice object into its slot 0
  // (TDecTop::xActivateParameterSets); a later slice, or one HM skipped (RASL, TFD), does not.
  void note_slice(int32_t hint, int64_t host_pts) {
    TComPic* pic = dec.getPcPic();
    if (pic == nullptr || (pic == cur_pic && pic->getSlice(0) == cur_slice0)) {
      return;
    }
    cur_pic = pic;
    cur_slice0 = pic->getSlice(0);
    cur_index = hint >= 0 ? hint : next_index;
    cur_pts = host_pts;
    const TComSPS* sps = cur_slice0->getSPS();
    max_depth = sps->getMaxTotalCUDepth() + 1;
    max_cu_w = sps->getMaxCUWidth();
    max_cu_h = sps->getMaxCUHeight();
  }

  void export_picture(TComPic* pic);
  void finish_picture(bool eos);
  void manage_buffer(NalUnitType type, bool new_picture);
  ovt_status decode_unit(std::span<const uint8_t> unit, int32_t hint, int64_t host_pts);
};

void Decoder::Impl::export_picture(TComPic* pic) {
  Frame f;
  const int poc = pic->getPOC();
  f.decode_index = pic == cur_pic ? cur_index : next_index;
  f.pts = pic == cur_pic ? cur_pts : 0;
  next_index = f.decode_index + 1;

  const TComSlice* first = pic->getSlice(0);
  const TComSPS* sps = first->getSPS();
  const ChromaFormat chroma = sps->getChromaFormatIdc();
  f.poc = poc;
  f.shown = first->getPicOutputFlag();
  f.is_rap = first->getRapPicFlag();
  f.temporal_id = static_cast<uint8_t>(first->getTLayer());
  f.layer_id = 0;  // HM decodes the base layer only (TDecTop::decode drops nuh_layer_id > 0)
  f.coded_w = static_cast<int32_t>(sps->getPicWidthInLumaSamples());
  f.coded_h = static_cast<int32_t>(sps->getPicHeightInLumaSamples());
  const Window& conf = sps->getConformanceWindow();
  const int unit_x = TComSPS::getWinUnitX(chroma);
  const int unit_y = TComSPS::getWinUnitY(chroma);
  f.crop_x = conf.getWindowLeftOffset() * unit_x;
  f.crop_y = conf.getWindowTopOffset() * unit_y;
  f.crop_w = f.coded_w - ((conf.getWindowLeftOffset() + conf.getWindowRightOffset()) * unit_x);
  f.crop_h = f.coded_h - ((conf.getWindowTopOffset() + conf.getWindowBottomOffset()) * unit_y);
  f.chroma_format = static_cast<uint8_t>(chroma);  // CHROMA_400/420/422/444 = 0..3
  f.bit_depth_luma = static_cast<uint8_t>(sps->getBitDepth(CHANNEL_TYPE_LUMA));
  f.bit_depth_chroma = chroma == CHROMA_400 ? 0 : static_cast<uint8_t>(sps->getBitDepth(CHANNEL_TYPE_CHROMA));
  f.qp_base = static_cast<int16_t>(first->getSliceQp());

  // Slice segments in CTU order: every segment has its own TComSlice (dependent ones copy the header).
  std::set<const TComSlice*> slices;
  std::array<int, 3> per_type{};
  for (UInt addr = 0; addr < pic->getNumberOfCtusInFrame(); ++addr) {
    const TComSlice* s = pic->getCtu(addr)->getSlice();
    if (s != nullptr && slices.insert(s).second && s->getSliceType() >= B_SLICE && s->getSliceType() <= I_SLICE) {
      ++per_type[static_cast<std::size_t>(s->getSliceType())];
    }
  }
  f.slice_count = static_cast<uint16_t>(std::min<std::size_t>(slices.size(), 0xFFFF));
  f.frame_type = majority_type(per_type);

  const TComPicYuv* rec = pic->getPicYuvRec();
  for (UInt c = 0; c < getNumberValidComponents(chroma); ++c) {
    const auto comp = static_cast<ComponentID>(c);
    copy_plane(*rec, comp, sps->getBitDepth(toChannelType(comp)), &f.planes[c]);
  }
  map_cu(*pic, pic_index, &f.analysis);
  pic_index[pic] = f.decode_index;
  f.diagnostics = std::move(pending_diags);
  pending_diags.clear();
  frames.push_back(std::move(f));
}

// A picture is complete when the next one's first slice arrives (decode() returns true; the unit is fed again), at an
// end of sequence, or at flush().
void Decoder::Impl::finish_picture(bool eos) {
  if (dec.getFirstSliceInSequence()) {
    dec.setFirstSliceInPicture(true);
    return;
  }
  if (!loop_filtered || !flushed) {
    Int poc = 0;
    TComPic* pic = dec.getPcPic();
    dec.executeLoopFilters(poc, pic_list);
    if (pic != nullptr) {
      export_picture(pic);
    }
    release_pictures(pic_list);
  }
  loop_filtered = eos;
  if (eos) {
    dec.setFirstSliceInSequence(true);
  }
}

// The picture-buffer steps of TAppDecTop::decode that follow the loop filter, minus the writes.
void Decoder::Impl::manage_buffer(NalUnitType type, bool new_picture) {
  if (pic_list == nullptr) {
    return;
  }
  if ((new_picture || type == NAL_UNIT_CODED_SLICE_CRA) && dec.getNoOutputPriorPicsFlag()) {
    dec.checkNoOutputPriorPics(pic_list);
    dec.setNoOutputPriorPicsFlag(false);
  }
  if (new_picture && is_idr_or_bla(type)) {
    flush_pictures(pic_list, &poc_last_display, &pic_index);
    cur_pic = nullptr;  // destroyed; HM's m_pcPic dangles until the IDR or BLA is fed again
    cur_slice0 = nullptr;
  }
  if (type == NAL_UNIT_EOS) {
    dec.setFirstSliceInPicture(false);
  }
}

ovt_status Decoder::Impl::decode_unit(std::span<const uint8_t> unit, int32_t hint, int64_t host_pts) {
  restore_tables();
  // At most twice: a unit that starts a new picture is fed again once the previous picture is finished.
  for (int pass = 0; pass < 2; ++pass) {
    InputNALUnit nalu;
    nalu.getBitstream().getFifo().assign(unit.begin(), unit.end());
    read(nalu);  // NAL header and emulation-prevention removal, as HM's byteStreamNALUnit + read() do
    const bool new_picture = dec.decode(nalu, skip_frame, poc_last_display);
    const NalUnitType type = nalu.m_nalUnitType;
    if (!new_picture && static_cast<int>(type) <= kNalVclMax && nalu.m_nuhLayerId == 0) {
      note_slice(hint, host_pts);
    }
    const bool eos = static_cast<int>(type) == kNalEos;
    if (new_picture || eos) {
      finish_picture(eos);
    }
    manage_buffer(type, new_picture);
    if (!new_picture) {
      return OVT_OK;
    }
  }
  add_diag(kError, "DEC-H265-0001", "HM: a unit started a new picture twice");
  return OVT_E_CORRUPT;
}

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {
  const std::lock_guard<std::mutex> lock(g_hm_mutex);
  impl_->dec.create();
  if (g_live_instances > 0) {
    // init() allocates a second set of scan-order tables over the live one: free the new set, keep the old.
    ScanOrder saved;
    save_scan_order(&saved);
    impl_->dec.init();
    destroyROM();
    restore_scan_order(saved);
  } else {
    impl_->dec.init();
  }
  // The hash check is OVT's verify job (WI-4.106); HM would only print it.
  impl_->dec.setDecodedPictureHashSEIEnabled(0);
  impl_->created = true;
  ++g_live_instances;
}

Decoder::~Decoder() {
  if (!impl_ || !impl_->created) {
    return;
  }
  const std::lock_guard<std::mutex> lock(g_hm_mutex);
  flush_pictures(impl_->pic_list, &impl_->poc_last_display, &impl_->pic_index);
  if (g_live_instances > 1) {
    // deletePicBuffer() ends with destroyROM(): hide the shared tables from it, so the other instances keep them.
    ScanOrder saved;
    save_scan_order(&saved);
    std::memset(&g_scanOrder, 0, sizeof(g_scanOrder));
    impl_->dec.deletePicBuffer();
    restore_scan_order(saved);
  } else {
    impl_->dec.deletePicBuffer();
  }
  impl_->dec.destroy();
  if (g_tables_owner == impl_.get()) {
    g_tables_owner = nullptr;
  }
  --g_live_instances;
}

ovt_status Decoder::create(const uint8_t* codec_private, size_t len, std::unique_ptr<Decoder>* out) {
  auto dec = std::make_unique<Decoder>();  // std::bad_alloc becomes OVT_E_NOMEM in plugin.cpp
  if (codec_private != nullptr && len > 0) {
    const ovt_status st = dec->push_hvcc(std::span<const uint8_t>(codec_private, len));
    if (st != OVT_OK) {
      return st;
    }
  }
  *out = std::move(dec);
  return OVT_OK;
}

// hvcC (ISO/IEC 14496-15 §8.3.3.1): 22 bytes, numOfArrays, per array 1 byte, numNalus u(16), (u(16) length, NAL)*.
ovt_status Decoder::push_hvcc(std::span<const uint8_t> hvcc) {
  if (hvcc.size() < 23) {
    return OVT_E_CORRUPT;
  }
  std::size_t pos = 22;
  const int arrays = hvcc[pos++];
  for (int a = 0; a < arrays; ++a) {
    if (hvcc.size() - pos < 3) {
      return OVT_E_CORRUPT;
    }
    const std::size_t count = (std::size_t{hvcc[pos + 1]} << 8) | hvcc[pos + 2];
    pos += 3;
    for (std::size_t k = 0; k < count; ++k) {
      if (hvcc.size() - pos < 2) {
        return OVT_E_CORRUPT;
      }
      const std::size_t size = (std::size_t{hvcc[pos]} << 8) | hvcc[pos + 1];
      pos += 2;
      if (hvcc.size() - pos < size) {
        return OVT_E_CORRUPT;
      }
      const std::span<const uint8_t> nal = hvcc.subspan(pos, size);
      const ovt_status st = push(nal.data(), nal.size(), -1, 0);
      if (st != OVT_OK) {
        return st;
      }
      pos += size;
    }
  }
  return OVT_OK;
}

ovt_status Decoder::push(const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts) {
  if (impl_->flushed || unit == nullptr || len == 0 || len > static_cast<std::size_t>(INT_MAX)) {
    return OVT_E_INVALID_ARG;
  }
  // A NAL unit header is two bytes and forbidden_zero_bit is 0 (H.265 7.3.1.2); HM asserts on either.
  // SPEC-ASSUMPTION(#442): beyond this header check the plugin cannot catch HM's exit()/assert() on malformed
  // slice data without patching HM; ovt-plugin-hm is an oracle for conformant streams.
  const std::span<const uint8_t> bytes(unit, len);
  if (bytes.size() < 2 || (bytes[0] & 0x80) != 0) {
    impl_->add_diag(kError, "DEC-H265-0001", "HM: malformed NAL unit header");
    return OVT_E_CORRUPT;
  }
  const std::lock_guard<std::mutex> lock(g_hm_mutex);
  return impl_->decode_unit(bytes, decode_index_hint, pts);
}

ovt_status Decoder::flush() {
  const std::lock_guard<std::mutex> lock(g_hm_mutex);
  Impl& d = *impl_;
  if (d.flushed) {
    return OVT_OK;
  }
  d.flushed = true;
  d.restore_tables();
  d.finish_picture(false);  // TAppDecTop at the end of the bitstream file
  flush_pictures(d.pic_list, &d.poc_last_display, &d.pic_index);
  if (!d.pending_diags.empty()) {
    // Diagnostics after the last picture travel in a diagnostics-only FrameAnalysis (chapter 07 §7.6, #249).
    Frame f;
    f.picture = false;
    f.decode_index = d.next_index;
    f.diagnostics = std::move(d.pending_diags);
    d.pending_diags.clear();
    d.frames.push_back(std::move(f));
  }
  return OVT_OK;
}

bool Decoder::pop(Frame* out) {
  if (impl_->frames.empty()) {
    return false;
  }
  *out = std::move(impl_->frames.front());
  impl_->frames.pop_front();
  return true;
}

}  // namespace ovt::hm
