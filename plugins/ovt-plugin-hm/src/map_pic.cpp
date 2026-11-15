// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// TComPic slices -> PartitionInfo, HevcFilterParams, HevcRps (chapter 07 §7.6.4, chapter 06 §6.3.4, WI-4.105 c).

#include "map_pic.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "analysis.hpp"
#include "ovt_model_generated.h"

// HM headers (BSD-3-Clause-Clear), outside OVT's warning flags.
#include "TLibCommon/CommonDef.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComPicSym.h"
#include "TLibCommon/TComSlice.h"

namespace ovt::hm {
namespace {

constexpr uint16_t kNoTile = 65535;

uint8_t flag(bool b) {
  return b ? 1 : 0;
}

template <class Builder>
std::vector<uint8_t> finish(flatbuffers::FlatBufferBuilder& fbb, Builder& b) {
  fbb.Finish(b.Finish());
  const flatbuffers::span<const uint8_t> buf = fbb.GetBufferSpan();
  return {buf.begin(), buf.end()};
}

class PicMapper {
 public:
  PicMapper(TComPic& pic, const PicIndex& index, Analysis* out)
      : pic_(pic),
        sym_(*pic.getPicSym()),
        index_(index),
        out_(*out),
        sps_(*pic.getSlice(0)->getSPS()),
        pps_(*pic.getSlice(0)->getPPS()),
        ctu_(static_cast<int>(sps_.getMaxCUWidth())),
        width_(static_cast<int>(sps_.getPicWidthInLumaSamples())),
        height_(static_cast<int>(sps_.getPicHeightInLumaSamples())),
        cols_(sym_.getFrameWidthInCtus()) {}

  PicMaps run(int dpb_fullness) {
    PicMaps maps;
    const UInt n = pic_.getNumberOfCtusInFrame();
    maps.segment.assign(n, 0);
    maps.tile.assign(n, kNoTile);
    maps.last_in_segment.assign(n, 0);
    segments(&maps);
    tiles(&maps);
    out_.filter_params = filter_params();
    out_.ref_state = rps(dpb_fullness);
    return maps;
  }

 private:
  TComPic& pic_;
  TComPicSym& sym_;
  const PicIndex& index_;
  Analysis& out_;
  const TComSPS& sps_;
  const TComPPS& pps_;
  const int ctu_;
  const int width_;
  const int height_;
  const UInt cols_;
  std::vector<TComSlice*> segment_slices_;  // PartitionInfo index -> slice segment

  // The luma rectangle of CTU rs, clipped to the picture.
  [[nodiscard]] fb::Rect ctu_rect(UInt rs) const {
    const auto x = static_cast<int>(rs % cols_) * ctu_;
    const auto y = static_cast<int>(rs / cols_) * ctu_;
    return {x, y, std::min(ctu_, width_ - x), std::min(ctu_, height_ - y)};
  }

  static fb::Rect bounding(const fb::Rect& a, const fb::Rect& b) {
    if (a.w() == 0) {
      return b;
    }
    const int x0 = std::min(a.x(), b.x());
    const int y0 = std::min(a.y(), b.y());
    return {x0, y0, std::max(a.x() + a.w(), b.x() + b.w()) - x0, std::max(a.y() + a.h(), b.y() + b.h()) - y0};
  }

  // One PartitionInfo per slice segment, in decode order (HM's slice index). A dependent segment's parent is the
  // independent segment that starts its slice.
  void segments(PicMaps* maps) {
    std::map<const TComSlice*, uint16_t> index_of;
    int16_t slice_start = -1;
    for (UInt i = 0; i < pic_.getNumAllocatedSlice(); ++i) {
      TComSlice* s = pic_.getSlice(static_cast<Int>(i));
      const bool dependent = s->getDependentSliceSegmentFlag();
      PartRec p;
      p.kind = dependent ? fb::PartitionKind::SliceSegment : fb::PartitionKind::Slice;
      if (!dependent) {
        slice_start = static_cast<int16_t>(i);
      }
      p.parent = dependent ? slice_start : int16_t{-1};
      p.slice_type = static_cast<uint8_t>(s->getSliceType());  // B 0, P 1, I 2 (7.4.7.1)
      p.qp = static_cast<int16_t>(s->getSliceQp());
      // HM's decoder never sets TComSlice::m_iSliceQpDelta, so slice_qp_delta is recovered from SliceQpY (7-54).
      p.qp_delta = static_cast<int16_t>(s->getSliceQp() - 26 - s->getPPS()->getPicInitQPMinus26());
      p.deblocking_disable = flag(s->getDeblockingFilterDisable());
      p.beta_offset_div2 = static_cast<int8_t>(s->getDeblockingFilterBetaOffsetDiv2());
      p.tc_offset_div2 = static_cast<int8_t>(s->getDeblockingFilterTcOffsetDiv2());
      p.loop_filter_across_slices = flag(s->getLFCrossSliceBoundaryFlag());
      p.sao_luma = flag(s->getSaoEnabledFlag(CHANNEL_TYPE_LUMA));
      p.sao_chroma = flag(s->getSaoEnabledFlag(CHANNEL_TYPE_CHROMA));
      p.num_entry_points = static_cast<uint16_t>(s->getNumberOfSubstreamSizes());
      p.cabac_init = s->getSliceType() == I_SLICE ? uint8_t{255} : flag(s->getCabacInitFlag());
      p.max_num_merge_cand =
          s->getSliceType() == I_SLICE ? uint8_t{255} : static_cast<uint8_t>(s->getMaxNumMergeCand());
      index_of.emplace(s, static_cast<uint16_t>(i));
      segment_slices_.push_back(s);
      out_.partitions.push_back(p);
    }
    // CTUs in tile scan: membership, address, count and bounding box of each segment.
    const UInt n = pic_.getNumberOfCtusInFrame();
    for (UInt ts = 0; ts < n; ++ts) {
      const UInt rs = sym_.getCtuTsToRsAddrMap(static_cast<Int>(ts));
      const auto it = index_of.find(pic_.getCtu(rs)->getSlice());
      if (it == index_of.end()) {
        continue;
      }
      PartRec& p = out_.partitions[it->second];
      if (p.block_count++ == 0) {
        p.first_block_addr = rs;  // slice_segment_address, CTB raster scan
      }
      p.rect = bounding(p.rect, ctu_rect(rs));
      maps->segment[rs] = it->second;
      const UInt next = ts + 1 < n ? sym_.getCtuTsToRsAddrMap(static_cast<Int>(ts + 1)) : n;
      maps->last_in_segment[rs] = flag(next == n || pic_.getCtu(next)->getSlice() != it->first);
    }
  }

  // With more than one tile, one PartitionInfo per tile after the slice segments (as the de265 plugin does, #461).
  void tiles(PicMaps* maps) {
    const auto count = static_cast<UInt>(sym_.getNumTiles());
    if (count <= 1) {
      return;
    }
    const auto tile_cols = static_cast<UInt>(pps_.getNumTileColumnsMinus1() + 1);
    const auto first = static_cast<uint16_t>(out_.partitions.size());
    for (UInt t = 0; t < count; ++t) {
      const TComTile& tile = *sym_.getTComTile(t);
      const UInt rs0 = tile.getFirstCtuRsAddr();
      const fb::Rect a = ctu_rect(rs0);
      const fb::Rect b = ctu_rect(rs0 + ((tile.getTileHeightInCtus() - 1) * cols_) + tile.getTileWidthInCtus() - 1);
      PartRec p;
      p.kind = fb::PartitionKind::Tile;
      p.rect = bounding(a, b);
      p.first_block_addr = rs0;
      p.block_count = tile.getTileWidthInCtus() * tile.getTileHeightInCtus();
      p.tile_row = static_cast<uint16_t>(t / tile_cols);
      p.tile_col = static_cast<uint16_t>(t % tile_cols);
      out_.partitions.push_back(p);
    }
    for (UInt rs = 0; rs < pic_.getNumberOfCtusInFrame(); ++rs) {
      maps->tile[rs] = static_cast<uint16_t>(first + sym_.getTileIdxMap(static_cast<Int>(rs)));
    }
  }

  // HevcFilterParams: PPS/SPS deblocking and SAO controls plus the per-slice-segment vectors.
  std::vector<uint8_t> filter_params() {
    std::vector<uint8_t> disabled;
    std::vector<int8_t> beta;
    std::vector<int8_t> tc;
    std::vector<uint8_t> sao_luma;
    std::vector<uint8_t> sao_chroma;
    for (const PartRec& p : out_.partitions) {
      if (p.kind == fb::PartitionKind::Tile) {
        continue;
      }
      disabled.push_back(p.deblocking_disable);
      beta.push_back(p.beta_offset_div2);
      tc.push_back(p.tc_offset_div2);
      sao_luma.push_back(p.sao_luma);
      sao_chroma.push_back(p.sao_chroma);
    }
    flatbuffers::FlatBufferBuilder fbb(256);
    const auto disabled_v = fbb.CreateVector(disabled);
    const auto beta_v = fbb.CreateVector(beta);
    const auto tc_v = fbb.CreateVector(tc);
    const auto sao_luma_v = fbb.CreateVector(sao_luma);
    const auto sao_chroma_v = fbb.CreateVector(sao_chroma);
    fb::HevcFilterParamsBuilder h(fbb);
    h.add_pps_deblocking_filter_disabled_flag(flag(pps_.getPPSDeblockingFilterDisabledFlag()));
    h.add_pps_beta_offset_div2(static_cast<int8_t>(pps_.getDeblockingFilterBetaOffsetDiv2()));
    h.add_pps_tc_offset_div2(static_cast<int8_t>(pps_.getDeblockingFilterTcOffsetDiv2()));
    h.add_deblocking_filter_override_enabled_flag(flag(pps_.getDeblockingFilterOverrideEnabledFlag()));
    h.add_slice_deblocking_filter_disabled_flag(disabled_v);
    h.add_slice_beta_offset_div2(beta_v);
    h.add_slice_tc_offset_div2(tc_v);
    h.add_pps_loop_filter_across_slices_enabled_flag(flag(pps_.getLoopFilterAcrossSlicesEnabledFlag()));
    h.add_loop_filter_across_tiles_enabled_flag(flag(pps_.getLoopFilterAcrossTilesEnabledFlag()));
    h.add_pcm_loop_filter_disabled_flag(flag(sps_.getUsePCM() && sps_.getPCMFilterDisableFlag()));
    h.add_sample_adaptive_offset_enabled_flag(flag(sps_.getUseSAO()));
    h.add_slice_sao_luma_flag(sao_luma_v);
    h.add_slice_sao_chroma_flag(sao_chroma_v);
    return finish(fbb, h);
  }

  // The decode index of the reference picture with POC @p poc HM still holds (LSB match for a long-term entry
  // signalled without MSB), or -1 ("no reference picture").
  [[nodiscard]] const TComPic* find_ref(int poc, bool lsb_only) const {
    const int mask = (1 << sps_.getBitsForPOC()) - 1;
    for (const auto& [p, idx] : index_) {
      if (p == &pic_ || !p->getReconMark() || !p->getSlice(0)->isReferenced()) {
        continue;
      }
      if (lsb_only ? (p->getPOC() & mask) == (poc & mask) : p->getPOC() == poc) {
        return p;
      }
    }
    return nullptr;
  }

  flatbuffers::Offset<fb::HevcRpsPic> rps_pic(flatbuffers::FlatBufferBuilder& fbb, const TComPic* p, int poc, bool used,
                                              bool long_term, uint8_t msb_present) const {
    const auto found = p != nullptr ? index_.find(p) : index_.end();
    const fb::FrameId frame(found != index_.end() ? found->second : -1, -1);
    fb::HevcRpsPicBuilder b(fbb);
    b.add_frame(&frame);
    b.add_PicOrderCntVal(p != nullptr ? p->getPOC() : poc);
    b.add_used_by_curr_pic(used);
    b.add_is_long_term(long_term);
    b.add_delta_poc_msb_present_flag(msb_present);
    return b.Finish();
  }

  using PicVec = std::vector<flatbuffers::Offset<fb::HevcRpsPic>>;

  // The five RPS lists of 8.3.2: StCurrBefore, StCurrAfter, StFoll, LtCurr, LtFoll.
  void rps_lists(flatbuffers::FlatBufferBuilder& fbb, TComSlice& s, std::array<PicVec, 5>* lists) const {
    const TComReferencePictureSet* rps = s.getRPS();
    if (rps == nullptr) {
      return;
    }
    const int neg = rps->getNumberOfNegativePictures();
    const int st = neg + rps->getNumberOfPositivePictures();
    for (int i = 0; i < rps->getNumberOfPictures(); ++i) {
      const bool used = rps->getUsed(i) != 0;
      if (i < st) {
        if (rps->getDeltaPOC(i) == 0) {
          // HM's inter RPS prediction keeps a predicted entry whose dPoc is 0 (TDecCavlc::parseShortTermRefPicSet
          // counts it as positive); equations 7-61 and 7-62 drop it, since it would be the current picture.
          continue;
        }
        const int poc = s.getPOC() + rps->getDeltaPOC(i);
        const std::size_t list = !used ? 2 : (i < neg ? 0 : 1);
        lists->at(list).push_back(rps_pic(fbb, find_ref(poc, false), poc, used, false, 255));
      } else {
        const bool msb = rps->getCheckLTMSBPresent(i);
        const int poc = rps->getPOC(i);
        lists->at(used ? 3 : 4).push_back(rps_pic(fbb, find_ref(poc, !msb), poc, used, true, flag(msb)));
      }
    }
  }

  // HevcRps of the first slice segment (8.3.2 / 8.3.4).
  std::vector<uint8_t> rps(int dpb_fullness) {
    TComSlice& s = *pic_.getSlice(0);
    flatbuffers::FlatBufferBuilder fbb(512);
    std::array<PicVec, 5> sets;
    rps_lists(fbb, s, &sets);
    std::array<PicVec, 2> lists;
    for (int l = 0; l < 2; ++l) {
      const auto list = static_cast<RefPicList>(l);
      for (Int i = 0; i < s.getNumRefIdx(list); ++i) {
        const TComPic* p = s.getRefPic(list, i);
        lists.at(static_cast<std::size_t>(l))
            .push_back(rps_pic(fbb, p, s.getRefPOC(list, i), true, p != nullptr && p->getIsLongTerm(), 255));
      }
    }
    std::array<flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::HevcRpsPic>>>, 7> v;
    for (std::size_t k = 0; k < 5; ++k) {
      v.at(k) = fbb.CreateVector(sets.at(k));
    }
    v[5] = fbb.CreateVector(lists[0]);
    v[6] = fbb.CreateVector(lists[1]);
    const UInt tid = sps_.getMaxTLayers() - 1;
    fb::HevcRpsBuilder h(fbb);
    h.add_PicOrderCntVal(s.getPOC());
    if (!s.getIdrPicFlag()) {
      const bool from_sps = s.getRPSidx() >= 0;
      h.add_short_term_ref_pic_set_sps_flag(flag(from_sps));
      if (from_sps) {
        h.add_short_term_ref_pic_set_idx(static_cast<uint8_t>(s.getRPSidx()));
      }
    }
    h.add_NumPicTotalCurr(static_cast<uint8_t>(s.getNumRpsCurrTempList()));
    h.add_RefPicSetStCurrBefore(v[0]);
    h.add_RefPicSetStCurrAfter(v[1]);
    h.add_RefPicSetStFoll(v[2]);
    h.add_RefPicSetLtCurr(v[3]);
    h.add_RefPicSetLtFoll(v[4]);
    h.add_RefPicList0(v[5]);
    h.add_RefPicList1(v[6]);
    if (s.getEnableTMVPFlag() && s.getSliceType() != I_SLICE) {
      h.add_collocated_from_l0_flag(s.getSliceType() == B_SLICE ? flag(s.getColFromL0Flag()) : uint8_t{1});
      h.add_collocated_ref_idx(static_cast<uint8_t>(s.getColRefIdx()));
    }
    if (s.isIRAP()) {
      h.add_NoRaslOutputFlag(flag(s.getNoRaslOutputFlag()));
    }
    h.add_pic_output_flag(flag(s.getPicOutputFlag()));
    h.add_dpb_fullness_after(static_cast<uint8_t>(std::clamp(dpb_fullness, 0, 255)));
    h.add_sps_max_dec_pic_buffering_minus1(static_cast<uint8_t>(sps_.getMaxDecPicBuffering(tid) - 1));
    h.add_sps_max_num_reorder_pics(static_cast<uint8_t>(sps_.getNumReorderPics(tid)));
    h.add_sps_max_latency_increase_plus1(sps_.getMaxLatencyIncreasePlus1(tid));
    return finish(fbb, h);
  }
};

}  // namespace

PicMaps map_pic(TComPic& pic, const PicIndex& index, int dpb_fullness, Analysis* out) {
  return PicMapper(pic, index, out).run(dpb_fullness);
}

}  // namespace ovt::hm
