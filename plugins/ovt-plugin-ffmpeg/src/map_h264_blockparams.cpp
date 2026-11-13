// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Block params -> AvcMb mapping of the FFmpeg analysis plugin (chapter 07 §7.6.3, WI-4.102).

#include "map_h264_blockparams.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/video_enc_params.h>
}

// The OVT patch 0001 (WI-4.100/4.101) adds libavutil/video_block_params.h; a system FFmpeg does not have it.
#if defined(AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS) && __has_include(<libavutil/video_block_params.h>)
#define OVT_FFMPEG_BLOCK_PARAMS 1
extern "C" {
#include <libavutil/video_block_params.h>
}
#else
#define OVT_FFMPEG_BLOCK_PARAMS 0
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ovt::ffmpeg {

namespace {

constexpr uint8_t kNone = 255;

// AVH264MacroblockParams.mb_type_class (the patch's numbering, chapter 07 §7.6.3).
enum MbClass : uint8_t {
  kINxN = 0,
  kI16x16 = 1,
  kIPcm = 2,
  kP16x16 = 3,
  kP16x8 = 4,
  kP8x16 = 5,
  kP8x8 = 6,
  kPSkip = 7,
  kBDirect16x16 = 8,
  kB16x16 = 9,
  kB16x8 = 10,
  kB8x16 = 11,
  kB8x8 = 12,
  kBSkip = 13,
};
constexpr uint8_t kSubDirect = 0x10;  // AVH264MacroblockParams.sub_mb_type bit 4

// Unified AvcMbType values (chapter 06 §6.4) that are not computed from an offset.
constexpr uint8_t kTypeI16x16First = 1;
constexpr uint8_t kTypeIPcm = 25;
constexpr uint8_t kTypePFirst = 26;  // P_L0_16x16; Table 7-13 raw 0
constexpr uint8_t kTypeBFirst = 31;  // B_Direct_16x16; Table 7-14 raw 0
constexpr uint8_t kTypeB8x8 = 53;
constexpr uint8_t kTypePSkip = 54;
constexpr uint8_t kTypeBSkip = 55;
constexpr uint8_t kSubTypeBFirst = 4;  // B_Direct_8x8; Table 7-18 raw 0

#if OVT_FFMPEG_BLOCK_PARAMS

constexpr int16_t kNoMvd = -32768;  // AvcMb.mvd_lX_x/y and MotionVector.mvd_x/y: not signalled
constexpr int32_t kMbSize = 16;
constexpr int32_t kCell = 4;
constexpr uint32_t kFlagBiPred = 1U << 0;  // ovt.fb.MotionVector.flags
constexpr uint32_t kFlagField = 1U << 4;

// H.264 slice_type % 5.
constexpr uint8_t kSliceP = 0;
constexpr uint8_t kSliceB = 1;
constexpr uint8_t kSliceI = 2;

// luma4x4BlkIdx of the 4x4 block at (bx, by) of its macroblock, in 4x4 units (H.264 6.4.3).
int blk_at(int bx, int by) {
  return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2);
}

bool is_intra(int cls) {
  return cls <= kIPcm;
}
bool is_skip(int cls) {
  return cls == kPSkip || cls == kBSkip;
}
bool is_b(int cls) {
  return cls >= kBDirect16x16 && cls <= kBSkip;
}
bool is_p(int cls) {
  return cls >= kP16x16 && cls <= kPSkip;
}

// QPC of Table 8-15 for qPI (QpBdOffsetC excluded, as AvcMb.QPC_cb/cr): identity below 30.
int qpc_of(int qpi) {
  static constexpr std::array<int8_t, 22> kTable = {29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
                                                    36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
  return qpi < 30 ? qpi : kTable.at(static_cast<size_t>(std::min(qpi, 51) - 30));
}

int8_t chroma_qp(int qp_y, int offset, int bd_offset_c) {
  return static_cast<int8_t>(qpc_of(std::clamp(qp_y + offset, -bd_offset_c, 51)));
}

// The partition (in 4x4 units: first luma4x4BlkIdx, width, height) whose mvd a MotionVector rect carries, or -1
// when the rect is not exactly one partition with a signalled mvd for @p list (sub-8x8 shapes are collapsed into
// one 8x8 vector by FFmpeg's export, so their mvds stay in AvcMb only).
int mvd_block_of(const AVH264MacroblockParams& p, const Box& rect, int list) {
  const int cls = p.mb_type_class;
  const int bx = (rect.x % kMbSize) / kCell;
  const int by = (rect.y % kMbSize) / kCell;
  const int part8 = ((by / 2) * 2) + (bx / 2);
  int w = 0;
  int h = 0;
  switch (cls) {
    case kP16x16:
    case kB16x16:
      w = h = kMbSize;
      break;
    case kP16x8:
    case kB16x8:
      w = kMbSize;
      h = kMbSize / 2;
      break;
    case kP8x16:
    case kB8x16:
      w = kMbSize / 2;
      h = kMbSize;
      break;
    case kP8x8:
    case kB8x8:
      if ((p.sub_mb_type[part8] & (kSubDirect | 15)) != 0) {
        return -1;  // direct, or a sub-8x8 shape
      }
      w = h = kMbSize / 2;
      break;
    default:
      return -1;  // intra, skip, B_Direct_16x16: no mvd
  }
  if (rect.w != w || rect.h != h || (p.pred_dir[part8] & (1U << list)) == 0) {
    return -1;
  }
  return blk_at(bx, by);
}

const AVH264MacroblockParams* mb_params(AVVideoBlockParamsHeader* par, unsigned i) {
  return static_cast<const AVH264MacroblockParams*>(av_video_block_params_block(par, i));
}

// The H.264 slice_type of each slice index, inferred from its macroblocks: B with a B macroblock, P with a P one,
// otherwise I. SPEC-ASSUMPTION(#395): an all-intra P or B slice reads as I (the side data carries no slice_type).
std::vector<uint8_t> slice_types(AVVideoBlockParamsHeader* par) {
  std::vector<uint8_t> types;
  for (unsigned i = 0; i < par->nb_blocks; ++i) {
    const AVH264MacroblockParams& p = *mb_params(par, i);
    if (p.mb_type_class == kNone) {
      continue;
    }
    if (types.size() <= p.slice_id) {
      types.resize(size_t{p.slice_id} + 1, kSliceI);
    }
    uint8_t& t = types[p.slice_id];
    if (is_b(p.mb_type_class)) {
      t = kSliceB;
    } else if (is_p(p.mb_type_class) && t != kSliceB) {
      t = kSliceP;
    }
  }
  return types;
}

// mb_type as signalled (7.3.5) from the unified type and the slice type; 255 for skipped macroblocks.
uint8_t raw_mb_type(uint8_t unified, uint8_t slice_type, bool skip) {
  if (skip || unified == kNone) {
    return kNone;
  }
  if (unified < kTypePFirst) {  // an I type: offset 5 in P slices, 23 in B slices (Tables 7-13, 7-14)
    return static_cast<uint8_t>(unified + (slice_type == kSliceP ? 5 : slice_type == kSliceB ? 23 : 0));
  }
  return static_cast<uint8_t>(unified - (unified < kTypeBFirst ? kTypePFirst : kTypeBFirst));
}

bool sub_direct(const AVH264MacroblockParams& p, size_t k) {
  return p.mb_type_class == kB8x8 && (p.sub_mb_type[k] & kSubDirect) != 0;
}

// The intra prediction modes of an I_NxN or I_16x16 macroblock.
void fill_intra(const AVH264MacroblockParams& p, AvcMbInfo* m) {
  const int cls = p.mb_type_class;
  const auto mode = [](int v, int max) { return v >= 0 && v <= max ? static_cast<uint8_t>(v) : kNone; };
  if (cls == kINxN && p.transform_8x8 != 0) {
    for (const int8_t v : p.intra8x8_pred_mode) {
      m->intra8x8_pred_mode.push_back(mode(v, 8));
    }
  } else if (cls == kINxN) {
    for (const int8_t v : p.intra4x4_pred_mode) {
      m->intra4x4_pred_mode.push_back(mode(v, 8));
    }
  }
  if (cls == kI16x16) {
    m->intra16x16_pred_mode = mode(p.intra16x16_pred_mode, 3);
  }
  if (cls == kINxN || cls == kI16x16) {
    m->intra_chroma_pred_mode = mode(p.intra_chroma_pred_mode, 3);
  }
}

// ref_idx, mvd, predFlags and the direct flags of an inter macroblock.
void fill_inter(const AVH264MacroblockParams& p, AvcMbInfo* m) {
  const int cls = p.mb_type_class;
  for (size_t l = 0; l < 2; ++l) {
    const auto bit = static_cast<uint8_t>(1U << l);
    for (size_t k = 0; k < 4; ++k) {
      m->ref_idx.at(l).push_back((p.pred_dir[k] & bit) != 0 ? p.ref_idx[l][k] : int8_t{-1});
    }
    for (size_t blk = 0; blk < 16; ++blk) {
      const size_t k = blk >> 2U;  // the 8x8 of luma4x4BlkIdx blk
      const bool direct = cls == kBDirect16x16 || sub_direct(p, k);
      const bool signalled = (p.pred_dir[k] & bit) != 0 && !m->skip && !direct;
      m->mvd_x.at(l).push_back(signalled ? p.mvd[l][blk][0] : kNoMvd);
      m->mvd_y.at(l).push_back(signalled ? p.mvd[l][blk][1] : kNoMvd);
    }
  }
  for (size_t blk = 0; blk < 16; ++blk) {
    m->pred_flag.push_back(static_cast<uint8_t>(p.pred_dir[blk >> 2U] & 3U));
  }
  if (is_b(cls)) {
    for (size_t k = 0; k < 4; ++k) {
      m->direct_8x8.push_back(cls == kBDirect16x16 || cls == kBSkip || sub_direct(p, k) ? 1 : 0);
    }
  }
}

// The AvcMb fields of one macroblock.
AvcMbInfo mb_info(const AVH264MacroblockParams& p, uint32_t addr, uint8_t slice_type, bool interlaced) {
  AvcMbInfo m;
  m.mb_addr = addr;
  const int cls = p.mb_type_class;
  if (cls > kBSkip) {
    return m;  // not decoded (255), or a class this mapping does not know
  }
  m.decoded = true;
  m.slice_index = p.slice_id;
  const std::array<uint8_t, 4> dir = {p.pred_dir[0], p.pred_dir[1], p.pred_dir[2], p.pred_dir[3]};
  m.mb_type = avc_mb_type(cls, dir, p.intra16x16_pred_mode, p.cbp);
  m.skip = is_skip(cls);
  m.mb_type_raw = raw_mb_type(m.mb_type, slice_type, m.skip);
  m.mb_skip_flag = m.skip ? 1 : 0;
  // SPEC-ASSUMPTION(#395): 0/1 in every macroblock of an interlaced frame (MBAFF and field pictures alike; the side
  // data does not tell them apart, WI-4.110), 255 in progressive frames.
  m.mb_field_decoding_flag = interlaced ? p.mb_field_decoding_flag : kNone;
  // Not signalled for I_16x16, I_PCM and skipped macroblocks (inferred 0).
  const bool t8_present = cls != kI16x16 && cls != kIPcm && !m.skip;
  m.transform_size_8x8_flag = t8_present ? p.transform_8x8 : kNone;
  m.coded_block_pattern = (cls == kIPcm || m.skip) ? kNone : p.cbp;
  m.mb_qp_delta = p.qp_delta;
  if (cls == kP8x8 || cls == kB8x8) {
    for (size_t k = 0; k < 4; ++k) {
      m.sub_mb_type.push_back(avc_sub_mb_type(cls == kB8x8, p.sub_mb_type[k], p.pred_dir[k]));
      m.direct = m.direct || sub_direct(p, k);
    }
  }
  m.direct = m.direct || cls == kBDirect16x16 || cls == kBSkip;
  if (is_intra(cls)) {
    fill_intra(p, &m);
  } else {
    fill_inter(p, &m);
  }
  return m;
}

// QPY and QPC of every macroblock (from the phase-1 QP and the PPS chroma offsets that AVVideoEncParams carries).
void fill_qps(const AVFrame* frame, int bit_depth_chroma, const SideData& side, BlockParams* out) {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
  const bool chroma = desc != nullptr && desc->nb_components >= 3;
  std::array<int, 2> offset{};
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
  if (sd != nullptr && sd->data != nullptr && sd->size >= sizeof(AVVideoEncParams)) {
    const auto* par = reinterpret_cast<const AVVideoEncParams*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
    offset = {par->delta_qp[1][0], par->delta_qp[2][0]};
  }
  const int bd_offset_c = 6 * std::max(bit_depth_chroma - 8, 0);
  if (side.qp_hist.empty()) {
    return;  // no QP side data: QPY / QPC stay unknown (-1)
  }
  for (size_t i = 0; i < out->mbs.size(); ++i) {
    AvcMbInfo& m = out->mbs[i];
    const int qp = side.mbs[i].qp;
    if (!m.decoded) {
      continue;
    }
    m.qp_y = static_cast<int8_t>(qp);
    if (chroma) {
      m.qpc_cb = chroma_qp(qp, offset[0], bd_offset_c);
      m.qpc_cr = chroma_qp(qp, offset[1], bd_offset_c);
    }
  }
}

// The intra_mode_y of the 4x4 block (bx, by) of a macroblock (chapter 06 §6.3.1), -1 when it has none.
int16_t luma_intra_mode(const AvcMbInfo& m, int bx, int by) {
  uint8_t mode = kNone;
  if (!m.intra4x4_pred_mode.empty()) {
    mode = m.intra4x4_pred_mode[static_cast<size_t>(blk_at(bx, by))];
  } else if (!m.intra8x8_pred_mode.empty()) {
    mode = m.intra8x8_pred_mode[(static_cast<size_t>(by / 2) * 2) + static_cast<size_t>(bx / 2)];
  }
  if (mode != kNone) {
    return mode;
  }
  if (m.intra16x16_pred_mode != kNone) {
    return static_cast<int16_t>(16 + m.intra16x16_pred_mode);
  }
  return m.mb_type == kTypeIPcm ? int16_t{32} : int16_t{-1};
}

// The cbf of a cell in 8x8 block part8: luma bit 0 from its CodedBlockPatternLuma bit, chroma bits 1 and 2.
uint8_t cbf_of(const AvcMbInfo& m, size_t part8) {
  if (m.skip) {
    return 0;
  }
  if (m.coded_block_pattern == kNone) {
    return kNone;
  }
  const auto luma = static_cast<uint8_t>((m.coded_block_pattern >> part8) & 1U);
  return static_cast<uint8_t>(luma | ((m.coded_block_pattern >> 4U) != 0 ? 6U : 0U));
}

// The per-cell vectors (partition_id, intra modes, transform size, cbf).
void fill_cells(const SideData& side, int32_t mb_w, BlockParams* out) {
  const size_t cells = static_cast<size_t>(side.cells_w) * side.cells_h;
  out->partition_id.assign(cells, 0);
  out->intra_mode_y.assign(cells, -1);
  out->intra_mode_c.assign(cells, -1);
  out->tx_log2.assign(cells, kNone);
  out->cbf.assign(cells, kNone);
  for (uint32_t cy = 0; cy < side.cells_h; ++cy) {
    for (uint32_t cx = 0; cx < side.cells_w; ++cx) {
      const size_t mb = (size_t{cy / 4} * static_cast<size_t>(mb_w)) + (cx / 4);
      const AvcMbInfo& m = out->mbs[mb];
      if (!m.decoded) {
        continue;
      }
      const size_t cell = (static_cast<size_t>(cy) * side.cells_w) + cx;
      const int bx = static_cast<int>(cx % 4);
      const int by = static_cast<int>(cy % 4);
      const size_t part8 = (static_cast<size_t>(by / 2) * 2) + static_cast<size_t>(bx / 2);
      out->partition_id[cell] = m.slice_index;
      out->intra_mode_y[cell] = luma_intra_mode(m, bx, by);
      if (m.intra_chroma_pred_mode != kNone) {
        out->intra_mode_c[cell] = m.intra_chroma_pred_mode;
      }
      // Chapter 06 §6.3.1: 2 or 3, 255 without a transform (skip, I_PCM). SPEC-ASSUMPTION(#395): I_16x16 is 2 (its
      // AC blocks are 4x4), not the 4 of chapter 07 §7.6.3, which V-027 rejects.
      if (!m.skip && m.mb_type != kTypeIPcm) {
        out->tx_log2[cell] = m.transform_size_8x8_flag == 1 ? 3 : 2;
      }
      out->cbf[cell] = cbf_of(m, part8);
    }
  }
}

void fill_slices(int32_t mb_w, const std::vector<uint8_t>& types, BlockParams* out) {
  for (const AvcMbInfo& m : out->mbs) {
    if (!m.decoded) {
      continue;
    }
    if (out->slices.size() <= m.slice_index) {
      out->slices.resize(size_t{m.slice_index} + 1);
    }
    SliceSpan& s = out->slices[m.slice_index];
    const Box r{static_cast<int32_t>(m.mb_addr % static_cast<uint32_t>(mb_w)) * kMbSize,
                static_cast<int32_t>(m.mb_addr / static_cast<uint32_t>(mb_w)) * kMbSize, kMbSize, kMbSize};
    if (s.mb_count == 0) {
      s.first_mb = m.mb_addr;
      s.rect = r;
    } else {
      const int32_t x1 = std::max(s.rect.x + s.rect.w, r.x + r.w);
      const int32_t y1 = std::max(s.rect.y + s.rect.h, r.y + r.h);
      s.first_mb = std::min(s.first_mb, m.mb_addr);
      s.rect.x = std::min(s.rect.x, r.x);
      s.rect.y = std::min(s.rect.y, r.y);
      s.rect.w = x1 - s.rect.x;
      s.rect.h = y1 - s.rect.y;
    }
    ++s.mb_count;
    s.slice_type = m.slice_index < types.size() ? types[m.slice_index] : kNone;
  }
}

constexpr int kMaxSlots = 32;  // ref_idx is 0..31 (field MBs); refs[] holds both lists

size_t part_of(const MbMotion& mv) {
  return (static_cast<size_t>((mv.rect.y % kMbSize) / 8) * 2) + static_cast<size_t>((mv.rect.x % kMbSize) / 8);
}

// The reference slots per list: one per ref_idx up to the largest one used.
void count_ref_slots(BlockParams* out) {
  std::array<int, 2> n{};
  for (const AvcMbInfo& m : out->mbs) {
    for (size_t l = 0; l < 2; ++l) {
      for (const int8_t r : m.ref_idx.at(l)) {
        n.at(l) = std::max(n.at(l), r + 1);
      }
    }
  }
  out->ref_slots = {static_cast<uint8_t>(std::min(n[0], kMaxSlots)), static_cast<uint8_t>(std::min(n[1], kMaxSlots))};
}

void drop_unused_list_mvs(const BlockParams& bp, SideData* side) {
  // libavcodec exports a vector of a list for every partition of a macroblock that uses the list anywhere
  // (mpegutils.c tests USES_LIST on the macroblock type): the partitions of B_16x8 / B_8x16 / B_8x8 that do not use
  // it get one too. The block params tell them apart; those vectors go, and so does a bi-prediction flag they paired.
  const auto unused = [&bp](const MbMotion& mv) {
    const AvcMbInfo& m = bp.mbs[mv.block_id - 1U];
    return m.decoded && !m.ref_idx.at(mv.list).empty() && m.ref_idx.at(mv.list)[part_of(mv)] < 0;
  };
  if (std::none_of(side->mvs.begin(), side->mvs.end(), unused)) {
    return;
  }
  side->mvs.erase(std::remove_if(side->mvs.begin(), side->mvs.end(), unused), side->mvs.end());
  for (auto run = side->mvs.begin(); run != side->mvs.end();) {  // canonical order: pairs share a block_id run
    const auto run_end =
        std::find_if(run, side->mvs.end(), [run](const MbMotion& o) { return o.block_id != run->block_id; });
    for (auto mv = run; mv != run_end; ++mv) {
      const bool paired =
          std::any_of(run, run_end, [mv](const MbMotion& o) { return o.list != mv->list && o.rect == mv->rect; });
      const uint32_t flags = mv->flags;
      mv->flags = static_cast<uint16_t>(paired ? (flags | kFlagBiPred) : (flags & ~kFlagBiPred));
    }
    run = run_end;
  }
}

// MotionVector.ref (true slots), mvd and field flag.
void assign_slots(AVVideoBlockParamsHeader* par, const std::vector<uint32_t>& record_of, SideData* side,
                  BlockParams* out) {
  for (MbMotion& mv : side->mvs) {
    const size_t mb = mv.block_id - 1U;
    const AvcMbInfo& m = out->mbs[mb];
    if (!m.decoded || m.ref_idx.at(mv.list).empty()) {
      continue;  // keeps the pseudo slot: list 0 -> slot 0, list 1 -> slot ref_slots[0] below
    }
    const int r = std::clamp<int>(m.ref_idx.at(mv.list)[part_of(mv)], 0, kMaxSlots - 1);
    mv.ref = static_cast<int8_t>(mv.list == 0 ? r : out->ref_slots[0] + r);
    const AVH264MacroblockParams& p = *mb_params(par, record_of[mb]);
    if (const int blk = mvd_block_of(p, mv.rect, mv.list); blk >= 0) {
      const auto clamp = [](int v) { return static_cast<int16_t>(std::clamp(v * 4, -32767, 32767)); };
      mv.mvd_x = clamp(p.mvd[mv.list][blk][0]);
      mv.mvd_y = clamp(p.mvd[mv.list][blk][1]);
    }
    const uint32_t flags = mv.flags;
    mv.flags = static_cast<uint16_t>((flags & ~kFlagField) | (m.mb_field_decoding_flag == 1 ? kFlagField : 0U));
  }
  for (const MbMotion& mv : side->mvs) {  // a vector without a decoded record keeps its list's first slot
    if (mv.list == 1 && out->ref_slots[1] == 0) {
      out->ref_slots[1] = 1;
    }
    if (mv.list == 0 && out->ref_slots[0] == 0) {
      out->ref_slots[0] = 1;
    }
  }
  for (MbMotion& mv : side->mvs) {
    const AvcMbInfo& m = out->mbs[mv.block_id - 1U];
    if (!m.decoded || m.ref_idx.at(mv.list).empty()) {
      mv.ref = static_cast<int8_t>(mv.list == 0 ? 0 : out->ref_slots[0]);
    }
  }
}

// The grid's references and the slot usage, from the vectors.
void fill_ref_grids(SideData* side, BlockParams* out) {
  const size_t cells = static_cast<size_t>(side->cells_w) * side->cells_h;
  for (size_t l = 0; l < 2; ++l) {
    side->ref.at(l).assign(cells, -1);
  }
  out->usage_cells.assign(size_t{out->ref_slots[0]} + out->ref_slots[1], 0);
  for (const MbMotion& mv : side->mvs) {
    const auto x_end = std::min<uint32_t>(static_cast<uint32_t>((mv.rect.x + mv.rect.w) / kCell), side->cells_w);
    const auto y_end = std::min<uint32_t>(static_cast<uint32_t>((mv.rect.y + mv.rect.h) / kCell), side->cells_h);
    for (auto cy = static_cast<uint32_t>(mv.rect.y / kCell); cy < y_end; ++cy) {
      for (auto cx = static_cast<uint32_t>(mv.rect.x / kCell); cx < x_end; ++cx) {
        side->ref.at(mv.list)[(static_cast<size_t>(cy) * side->cells_w) + cx] = mv.ref;
      }
    }
  }
  for (size_t l = 0; l < 2; ++l) {
    for (const int8_t r : side->ref.at(l)) {
      if (r >= 0 && static_cast<size_t>(r) < out->usage_cells.size()) {
        ++out->usage_cells[static_cast<size_t>(r)];
      }
    }
  }
}

// MotionVector.ref (true slots), mvd and field flag; then the grid's references and the slot usage from them.
void refine_motion(AVVideoBlockParamsHeader* par, const std::vector<uint32_t>& record_of, SideData* side,
                   BlockParams* out) {
  count_ref_slots(out);
  drop_unused_list_mvs(*out, side);
  assign_slots(par, record_of, side, out);
  fill_ref_grids(side, out);
}

// The record of each macroblock; empty when a record is off the grid or two land on one macroblock.
std::vector<uint32_t> records_by_mb(AVVideoBlockParamsHeader* par, int32_t mb_w, int32_t mb_h) {
  std::vector<uint32_t> record_of(static_cast<size_t>(mb_w) * static_cast<size_t>(mb_h),
                                  std::numeric_limits<uint32_t>::max());
  for (unsigned i = 0; i < par->nb_blocks; ++i) {
    const AVH264MacroblockParams& p = *mb_params(par, i);
    if (p.src_x < 0 || p.src_y < 0 || p.src_x % kMbSize != 0 || p.src_y % kMbSize != 0 || p.src_x / kMbSize >= mb_w ||
        p.src_y / kMbSize >= mb_h) {
      return {};
    }
    const size_t mb =
        (static_cast<size_t>(p.src_y / kMbSize) * static_cast<size_t>(mb_w)) + static_cast<size_t>(p.src_x / kMbSize);
    if (record_of[mb] != std::numeric_limits<uint32_t>::max()) {
      return {};
    }
    record_of[mb] = i;
  }
  return record_of;
}

// Prediction kinds from mb_type replace the phase-1 inference (a vector -> Inter, only a QP -> Intra).
void set_pred_kinds(const BlockParams& bp, SideData* side) {
  for (size_t mb = 0; mb < bp.mbs.size(); ++mb) {
    const AvcMbInfo& m = bp.mbs[mb];
    if (m.decoded) {
      const bool intra = m.mb_type < kTypePFirst;
      side->mbs[mb].pred = m.skip ? kPredSkip : m.mb_type == kTypeIPcm ? kPredPcm : intra ? kPredIntra : kPredInter;
    }
  }
  for (size_t cell = 0; cell < side->pred.size(); ++cell) {
    side->pred[cell] = side->mbs[side->block_id[cell] - 1U].pred;
  }
}

#endif  // OVT_FFMPEG_BLOCK_PARAMS

}  // namespace

uint8_t avc_mb_type(int mb_type_class, const std::array<uint8_t, 4>& pred_dir, int intra16x16_pred_mode, int cbp) {
  switch (mb_type_class) {
    case kINxN:
      return 0;
    case kI16x16: {
      const int chroma = (cbp >> 4) & 3;
      if (intra16x16_pred_mode < 0 || intra16x16_pred_mode > 3 || chroma > 2) {
        return kNone;
      }
      return static_cast<uint8_t>(kTypeI16x16First + intra16x16_pred_mode + (4 * chroma) + ((cbp & 15) != 0 ? 12 : 0));
    }
    case kIPcm:
      return kTypeIPcm;
    case kP16x16:
    case kP16x8:
    case kP8x16:
    case kP8x8:  // SPEC-ASSUMPTION(#395): P_8x8ref0 (raw 4) is reported as P_8x8; the side data does not tell them
                 // apart
      return static_cast<uint8_t>(kTypePFirst + mb_type_class - kP16x16);
    case kPSkip:
      return kTypePSkip;
    case kBDirect16x16:
      return kTypeBFirst;
    case kB16x16:
      return pred_dir[0] >= 1 && pred_dir[0] <= 3 ? static_cast<uint8_t>(kTypeBFirst + pred_dir[0]) : kNone;
    case kB16x8:
    case kB8x16: {
      // Table 7-14: B_<X>_<Y>_16x8 by the directions (1 L0, 2 L1, 3 Bi) of the two partitions; the 8x16 type is one
      // more. Partition 1 is 8x8 block 2 (16x8) or 1 (8x16).
      static const std::map<std::pair<int, int>, uint8_t> kRaw16x8 = {{{1, 1}, 4},  {{2, 2}, 6},  {{1, 2}, 8},
                                                                      {{2, 1}, 10}, {{1, 3}, 12}, {{2, 3}, 14},
                                                                      {{3, 1}, 16}, {{3, 2}, 18}, {{3, 3}, 20}};
      const auto it = kRaw16x8.find({pred_dir[0], mb_type_class == kB16x8 ? pred_dir[2] : pred_dir[1]});
      return it != kRaw16x8.end() ? static_cast<uint8_t>(kTypeBFirst + it->second + (mb_type_class == kB8x16 ? 1 : 0))
                                  : kNone;
    }
    case kB8x8:
      return kTypeB8x8;
    case kBSkip:
      return kTypeBSkip;
    default:
      return kNone;
  }
}

uint8_t avc_sub_mb_type(bool b_slice, int sub_mb_type, int pred_dir) {
  const int shape = sub_mb_type & 15;
  if (shape > 3) {
    return kNone;
  }
  if (!b_slice) {
    return static_cast<uint8_t>(shape);  // Table 7-17: P_L0_8x8, P_L0_8x4, P_L0_4x8, P_L0_4x4
  }
  if ((sub_mb_type & kSubDirect) != 0) {
    return kSubTypeBFirst;  // B_Direct_8x8
  }
  if (pred_dir < 1 || pred_dir > 3) {
    return kNone;
  }
  // Table 7-18: 8x8 raw 1..3 by direction; 8x4 / 4x8 pairs L0 4/5, L1 6/7, Bi 8/9; 4x4 L0 10, L1 11, Bi 12.
  const int raw = shape == 0 ? pred_dir : shape == 3 ? 9 + pred_dir : 2 + (2 * pred_dir) + (shape - 1);
  return static_cast<uint8_t>(kSubTypeBFirst + raw);
}

bool block_params_available() {
  return OVT_FFMPEG_BLOCK_PARAMS != 0;
}

BlockParams map_h264_blockparams(const AVFrame* frame, int bit_depth_chroma, SideData* side, std::vector<Diag>* diags) {
  BlockParams out;
#if OVT_FFMPEG_BLOCK_PARAMS
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_BLOCK_PARAMS);
  if (!side->valid || sd == nullptr || sd->data == nullptr || sd->size < sizeof(AVVideoBlockParamsHeader)) {
    return out;
  }
  auto* par = reinterpret_cast<AVVideoBlockParamsHeader*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast): FFmpeg API
  const int32_t mb_w = static_cast<int32_t>(side->cells_w + 3) / 4;
  const int32_t mb_h = static_cast<int32_t>(side->cells_h + 3) / 4;
  const auto n_mbs = static_cast<size_t>(mb_w) * static_cast<size_t>(mb_h);
  if (par->type != AV_VIDEO_BLOCK_PARAMS_H264 || par->block_size < sizeof(AVH264MacroblockParams) ||
      par->nb_blocks != n_mbs || side->mbs.size() != n_mbs || par->blocks_offset > sd->size ||
      (sd->size - par->blocks_offset) / par->block_size < par->nb_blocks) {
    return out;
  }
  // Each record lands at its macroblock; a record off the grid, or a macroblock without one, voids the side data.
  const std::vector<uint32_t> record_of = records_by_mb(par, mb_w, mb_h);
  if (record_of.empty()) {
    return out;
  }
#ifdef AV_FRAME_FLAG_INTERLACED
  const bool interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
  const bool interlaced = frame->interlaced_frame != 0;  // FFmpeg 6.0
#endif
  const std::vector<uint8_t> types = slice_types(par);
  out.mbs.reserve(n_mbs);
  for (size_t mb = 0; mb < n_mbs; ++mb) {
    const AVH264MacroblockParams& p = *mb_params(par, record_of[mb]);
    const uint8_t type = p.slice_id < types.size() ? types[p.slice_id] : kSliceI;
    out.mbs.push_back(mb_info(p, static_cast<uint32_t>(mb), type, interlaced));
  }
  if (std::none_of(out.mbs.begin(), out.mbs.end(), [](const AvcMbInfo& m) { return m.decoded; })) {
    return BlockParams{};
  }
  out.valid = true;
  fill_qps(frame, bit_depth_chroma, *side, &out);
  fill_cells(*side, mb_w, &out);
  fill_slices(mb_w, types, &out);
  set_pred_kinds(out, side);
  refine_motion(par, record_of, side, &out);
  // The prediction modes are known now: the "n/a" diagnostic of phase 1 no longer applies.
  diags->erase(std::remove_if(diags->begin(), diags->end(), [](const Diag& d) { return d.code == "ANA-H264-0002"; }),
               diags->end());
#else
  (void)frame;
  (void)bit_depth_chroma;
  (void)side;
  (void)diags;
#endif
  return out;
}

}  // namespace ovt::ffmpeg
