// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libovt_plugin_ffmpeg: the ABI glue of the H.264 analysis plugin (chapter 0 §0.6.2, chapter 07 §7.6.3, WI-2.169).
// It exports ovt_analysis_decoder_v1() and ovt_plugin_describe() (chapter 13 §13.3.3) and nothing else; every entry
// point is noexcept and turns an exception into an error status. It produces FrameAnalysis buffers with FrameInfo,
// the Reconstruction planes and, from AV_FRAME_DATA_MOTION_VECTORS and AV_FRAME_DATA_VIDEO_ENC_PARAMS (WI-2.170,
// map_h264_sidedata.cpp), the macroblock tree, the cell grid, the motion vectors and the pseudo references L0 / L1.
// With the OVT FFmpeg patch 0001 (AV_FRAME_DATA_VIDEO_BLOCK_PARAMS, map_h264_blockparams.cpp, WI-4.102) every
// macroblock also carries its AvcMb extension table, the grid gains partition_id, intra modes, transform size and cbf,
// and refs[] names the reference indices "L0[i]" / "L1[i]" instead of the two pseudo slots; frames without the side
// data (an unpatched FFmpeg) keep the phase-1 mapping.
//
// Compiled with OVT_FFMPEG_PLUGIN_HEVC=1 it is libovt_plugin_ffmpeg_hevc instead (WI-4.107): the ABI carries one codec
// per plugin (caps.codec), so the optional H.265 "fast" path is a second library of the same sources. It drives
// libavcodec's hevc decoder and maps the motion vectors and per-CU QP that patch 0002 exports (map_hevc_sidedata.cpp).
// Its id is not in the registry's H.265 default chain, so it is never the default H.265 plugin (chapter 07 §7.8).
// SPEC-ASSUMPTION(#370): a separate library with its own id, rather than a second codec in libovt_plugin_ffmpeg.

#include <flatbuffers/flatbuffers.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "h264_analysis.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"

namespace {

#ifndef OVT_FFMPEG_PLUGIN_HEVC
#define OVT_FFMPEG_PLUGIN_HEVC 0
#endif
constexpr bool kHevc = OVT_FFMPEG_PLUGIN_HEVC != 0;
constexpr uint8_t kCodec = kHevc ? 2 : 1;  // ovt.fb.Codec.H265 / H264
constexpr ovt::ffmpeg::Codec kDecoder = kHevc ? ovt::ffmpeg::Codec::H265 : ovt::ffmpeg::Codec::H264;
constexpr ovt::fb::BlockKind kBlockKind = kHevc ? ovt::fb::BlockKind::CU : ovt::fb::BlockKind::MB;
constexpr uint32_t kStageRecon = 0;  // ovt.fb.PlaneStage.Reconstruction
constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};
constexpr uint8_t kPartitionNotAvailable = 255;  // Block.partition_type: no mb_type in the side data

}  // namespace

struct ovt_AnalysisDecoder {
  // A FrameAnalysis handed out by pull() and not yet released, with the planes its PlaneRefs name.
  struct Pulled {
    std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
    ovt::ffmpeg::Frame frame;
    std::map<std::string, const ovt::ffmpeg::Plane*> blobs;  // blob_id -> plane of `frame`
  };
  std::unique_ptr<ovt::ffmpeg::H264Analysis> decoder;
  std::map<const uint8_t*, std::unique_ptr<Pulled>> pulled;
};

namespace {

// The SPDX id of the linked libavcodec (GPL with OVT_FFMPEG_GPL or some system builds), from avcodec_license().
const char* ffmpeg_spdx() {
  const char* lic = avcodec_license();
  const bool lgpl = std::strstr(lic, "LGPL") != nullptr;
  const bool v3 = std::strstr(lic, "version 3") != nullptr;
  if (lgpl) {
    return v3 ? "LGPL-3.0-or-later" : "LGPL-2.1-or-later";
  }
  return v3 ? "GPL-3.0-or-later" : "GPL-2.0-or-later";
}

// Strings and caps are built once from the linked FFmpeg (static locals: thread-safe initialisation).
struct Info {
  std::string version = std::string("0.1.0+ffmpeg.") + av_version_info();
  std::string notes =
      kHevc ? std::string("H.265 motion vectors and per-CU QP via libavcodec ") + av_version_info() +
                  " (needs the OVT patch 0002; no partitions, intra modes, transform tree or ref_idx; optional, never "
                  "the default H.265 plugin)"
      : ovt::ffmpeg::block_params_available()
          ? std::string("H.264 analysis via libavcodec ") + av_version_info() +
                " with the OVT block-params patch (mb_type, sub_mb_type, ref_idx, intra modes, cbp, mb_qp_delta, "
                "mvd; sub-8x8 motion vectors are collapsed to 8x8)"
          : std::string("H.264 analysis via libavcodec ") + av_version_info() +
                " (motion vectors and QP side data; sub-8x8 MVs, ref_idx and intra modes need the phase-4 "
                "MB-params patch)";
  std::string describe = std::string(kHevc ? "ovt-plugin-ffmpeg_hevc " : "ovt-plugin-ffmpeg ") + version +
                         "; licence " + ffmpeg_spdx() + "; avcodec_configuration(): " + avcodec_configuration();
  ovt_AnalysisCaps caps{};
  Info() {
    caps.struct_size = sizeof(ovt_AnalysisCaps);
    caps.codec = kCodec;
    caps.plane_stages = 1U << kStageRecon;
    caps.grid_fields =
        OVT_GRID_BLOCK_ID | OVT_GRID_PRED | OVT_GRID_QP | OVT_GRID_MV0 | OVT_GRID_MV1 | OVT_GRID_REF0 | OVT_GRID_REF1;
    if (!kHevc && ovt::ffmpeg::block_params_available()) {  // §7.6.3: bits 1, 4, 5, 10 and 12 with the patch
      caps.grid_fields |=
          OVT_GRID_PARTITION_ID | OVT_GRID_INTRA_MODE_Y | OVT_GRID_INTRA_MODE_C | OVT_GRID_TX_LOG2 | OVT_GRID_CBF;
    }
    caps.has_symbol_accounting = 0;
    caps.bit_exact = 1;
    caps.supports_threads = 1;  // libavcodec slice threads (SPEC-ASSUMPTION(#302))
    caps.id = kHevc ? "ffmpeg_hevc" : "ffmpeg";
    caps.version = version.c_str();
    caps.license = ffmpeg_spdx();
    caps.notes = notes.c_str();
  }
};

const Info& info() {
  static const Info i;
  return i;
}

// The plugin-local blob id "<decode_index>/recon/<plane>", as in ovt-plugin-de265. SPEC-ASSUMPTION(#250): the host
// prefixes the stream's source hash to make the chapter 06 §6.6.2 blob_id.
std::string blob_id(int32_t decode_index, int plane) {
  return std::to_string(decode_index) + "/recon/" + kPlaneNames.at(static_cast<std::size_t>(plane));
}

// CellGrid: the vectors named in caps.grid_fields; the others stay empty ("not provided by this plugin").
flatbuffers::Offset<ovt::fb::CellGrid> build_grid(flatbuffers::FlatBufferBuilder& fbb, const ovt::ffmpeg::Frame& f) {
  const ovt::ffmpeg::SideData& sd = f.side;
  if (!sd.valid) {
    return ovt::fb::CreateCellGrid(fbb, static_cast<uint32_t>((f.coded_w + 3) / 4),
                                   static_cast<uint32_t>((f.coded_h + 3) / 4));
  }
  const auto block_id = fbb.CreateVector(sd.block_id);
  const auto pred = fbb.CreateVector(sd.pred);
  const auto qp = fbb.CreateVector(sd.qp);
  const auto mv0_x = fbb.CreateVector(sd.mv_x[0]);
  const auto mv0_y = fbb.CreateVector(sd.mv_y[0]);
  const auto mv1_x = fbb.CreateVector(sd.mv_x[1]);
  const auto mv1_y = fbb.CreateVector(sd.mv_y[1]);
  const auto ref0 = fbb.CreateVector(sd.ref[0]);
  const auto ref1 = fbb.CreateVector(sd.ref[1]);
  const ovt::ffmpeg::BlockParams& bp = f.blocks;
  flatbuffers::Offset<flatbuffers::Vector<uint16_t>> partition_id;
  flatbuffers::Offset<flatbuffers::Vector<int16_t>> intra_mode_y;
  flatbuffers::Offset<flatbuffers::Vector<int16_t>> intra_mode_c;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> tx_log2_w;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> tx_log2_h;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> cbf;
  if (bp.valid) {
    partition_id = fbb.CreateVector(bp.partition_id);
    intra_mode_y = fbb.CreateVector(bp.intra_mode_y);
    intra_mode_c = fbb.CreateVector(bp.intra_mode_c);
    tx_log2_w = fbb.CreateVector(bp.tx_log2);
    tx_log2_h = fbb.CreateVector(bp.tx_log2);
    cbf = fbb.CreateVector(bp.cbf);
  }
  ovt::fb::CellGridBuilder g(fbb);
  g.add_cells_w(sd.cells_w);
  g.add_cells_h(sd.cells_h);
  g.add_block_id(block_id);
  g.add_pred(pred);
  g.add_qp(qp);
  g.add_mv0_x(mv0_x);
  g.add_mv0_y(mv0_y);
  g.add_mv1_x(mv1_x);
  g.add_mv1_y(mv1_y);
  g.add_ref0(ref0);
  g.add_ref1(ref1);
  if (bp.valid) {
    g.add_partition_id(partition_id);
    g.add_intra_mode_y(intra_mode_y);
    g.add_intra_mode_c(intra_mode_c);
    g.add_tx_log2_w(tx_log2_w);
    g.add_tx_log2_h(tx_log2_h);
    g.add_cbf(cbf);
  }
  return g.Finish();
}

template <typename E>
flatbuffers::Offset<flatbuffers::Vector<E>> enum_vector(flatbuffers::FlatBufferBuilder& fbb,
                                                        const std::vector<uint8_t>& v) {
  std::vector<E> out(v.size());
  std::transform(v.begin(), v.end(), out.begin(), [](uint8_t x) { return static_cast<E>(x); });
  return fbb.CreateVector(out);
}

// The nested AvcMb buffer of one macroblock (chapter 06 §6.4, Block.ext); empty vectors are left out.
std::vector<uint8_t> avc_mb_ext(flatbuffers::FlatBufferBuilder& fbb, const ovt::ffmpeg::AvcMbInfo& m) {
  fbb.Clear();
  const auto opt = [&fbb](const auto& v) { return v.empty() ? 0 : fbb.CreateVector(v).o; };
  const auto sub = m.sub_mb_type.empty() ? 0 : enum_vector<ovt::fb::AvcSubMbType>(fbb, m.sub_mb_type).o;
  const auto i4 =
      m.intra4x4_pred_mode.empty() ? 0 : enum_vector<ovt::fb::AvcIntra4x4PredMode>(fbb, m.intra4x4_pred_mode).o;
  const auto i8 =
      m.intra8x8_pred_mode.empty() ? 0 : enum_vector<ovt::fb::AvcIntra4x4PredMode>(fbb, m.intra8x8_pred_mode).o;
  const flatbuffers::uoffset_t ref0 = opt(m.ref_idx[0]);
  const flatbuffers::uoffset_t ref1 = opt(m.ref_idx[1]);
  const flatbuffers::uoffset_t mvd0x = opt(m.mvd_x[0]);
  const flatbuffers::uoffset_t mvd0y = opt(m.mvd_y[0]);
  const flatbuffers::uoffset_t mvd1x = opt(m.mvd_x[1]);
  const flatbuffers::uoffset_t mvd1y = opt(m.mvd_y[1]);
  const flatbuffers::uoffset_t pred_flag = opt(m.pred_flag);
  const flatbuffers::uoffset_t direct = opt(m.direct_8x8);
  ovt::fb::AvcMbBuilder b(fbb);
  b.add_mb_addr(m.mb_addr);
  b.add_mb_type_raw(m.mb_type_raw);
  b.add_mb_type(static_cast<ovt::fb::AvcMbType>(m.mb_type));
  b.add_mb_skip_flag(m.mb_skip_flag);
  b.add_mb_field_decoding_flag(m.mb_field_decoding_flag);
  b.add_transform_size_8x8_flag(m.transform_size_8x8_flag);
  b.add_coded_block_pattern(m.coded_block_pattern);
  b.add_mb_qp_delta(m.mb_qp_delta);
  b.add_QPY(m.qp_y);
  b.add_QPC_cb(m.qpc_cb);
  b.add_QPC_cr(m.qpc_cr);
  b.add_sub_mb_type(sub);
  b.add_intra4x4_pred_mode(i4);
  b.add_intra8x8_pred_mode(i8);
  b.add_Intra16x16PredMode(static_cast<ovt::fb::AvcIntra16x16PredMode>(m.intra16x16_pred_mode));
  b.add_intra_chroma_pred_mode(static_cast<ovt::fb::AvcIntraChromaPredMode>(m.intra_chroma_pred_mode));
  b.add_ref_idx_l0(ref0);
  b.add_ref_idx_l1(ref1);
  b.add_mvd_l0_x(mvd0x);
  b.add_mvd_l0_y(mvd0y);
  b.add_mvd_l1_x(mvd1x);
  b.add_mvd_l1_y(mvd1y);
  b.add_pred_flag(pred_flag);
  b.add_direct_8x8(direct);
  b.add_slice_index(m.slice_index);
  fbb.Finish(b.Finish());
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

// blocks[0] is the virtual root (chapter 06 §6.3.2); blocks[1 + n] is macroblock n in raster order (H.265: coding unit
// n in the order of the side data).
void build_blocks(flatbuffers::FlatBufferBuilder& fbb, const ovt::ffmpeg::Frame& f,
                  std::vector<flatbuffers::Offset<ovt::fb::Block>>* out) {
  const ovt::ffmpeg::SideData& sd = f.side;
  std::vector<uint32_t> children(sd.mbs.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    children[i] = 1U + static_cast<uint32_t>(i);
  }
  const auto children_v = fbb.CreateVector(children);
  const ovt::fb::Rect coded(0, 0, f.coded_w, f.coded_h);
  ovt::fb::BlockBuilder root(fbb);
  root.add_id(0);
  root.add_children(children_v);
  root.add_kind(ovt::fb::BlockKind::Other);
  root.add_rect(&coded);
  root.add_partition_type(kPartitionNotAvailable);
  out->push_back(root.Finish());
  const ovt::ffmpeg::BlockParams& bp = f.blocks;
  flatbuffers::FlatBufferBuilder ext_fbb(256);
  for (std::size_t i = 0; i < sd.mbs.size(); ++i) {
    const ovt::ffmpeg::MbBlock& mb = sd.mbs[i];
    const ovt::fb::Rect rect(mb.rect.x, mb.rect.y, mb.rect.w, mb.rect.h);
    const ovt::ffmpeg::AvcMbInfo* avc = bp.valid ? &bp.mbs[i] : nullptr;
    flatbuffers::Offset<flatbuffers::String> ext_type;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> ext;
    if (avc != nullptr) {
      ext_type = fbb.CreateString("AvcMb");
      ext = fbb.CreateVector(avc_mb_ext(ext_fbb, *avc));
    }
    ovt::fb::BlockBuilder b(fbb);
    b.add_id(1U + static_cast<uint32_t>(i));
    b.add_parent(0);
    b.add_kind(kBlockKind);
    b.add_rect(&rect);
    b.add_level(1);
    b.add_partition_type(avc != nullptr ? avc->mb_type : kPartitionNotAvailable);
    b.add_pred(static_cast<ovt::fb::PredKind>(mb.pred));
    b.add_qp(mb.qp);
    if (avc != nullptr) {
      b.add_skip(avc->skip);
      b.add_merge_or_direct(avc->direct);
      b.add_ext_type(ext_type);
      b.add_ext(ext);
    }
    out->push_back(b.Finish());
  }
}

// refs[] with the block params: "L0[i]" for each reference index of list 0 used in the frame, then "L1[i]".
// RefSlot.frame stays -1: the host resolves the reference lists (§7.5.3 A6, issue #253). SPEC-ASSUMPTION(#395): the
// slots name the indices; slices whose lists differ (chapter 06 §6.3.1 "S<slice>:L0[i]") are not told apart because
// the side data carries no reference lists.
std::vector<flatbuffers::Offset<ovt::fb::RefSlot>> ref_slots(flatbuffers::FlatBufferBuilder& fbb,
                                                             const ovt::ffmpeg::BlockParams& bp) {
  std::vector<flatbuffers::Offset<ovt::fb::RefSlot>> refs;
  const ovt::fb::FrameId unknown(-1, -1);
  for (uint8_t l = 0; l < 2; ++l) {
    for (uint8_t i = 0; i < bp.ref_slots.at(l); ++i) {
      const auto index = static_cast<uint8_t>(refs.size());
      const auto name = fbb.CreateString((l == 0 ? "L0[" : "L1[") + std::to_string(i) + "]");
      ovt::fb::RefSlotBuilder rb(fbb);
      rb.add_index(index);
      rb.add_frame(&unknown);
      rb.add_name(name);
      rb.add_usage_cells(index < bp.usage_cells.size() ? bp.usage_cells[index] : 0);
      refs.push_back(rb.Finish());
    }
  }
  return refs;
}

// FrameAnalysis.partitions with the block params: one Slice per slice index. The host fills unit_index, file_span
// and the header from its index (§7.5.3 A6); the plugin knows the macroblocks only.
std::vector<flatbuffers::Offset<ovt::fb::PartitionInfo>> slice_partitions(flatbuffers::FlatBufferBuilder& fbb,
                                                                          const ovt::ffmpeg::BlockParams& bp) {
  std::vector<flatbuffers::Offset<ovt::fb::PartitionInfo>> parts;
  for (std::size_t i = 0; i < bp.slices.size(); ++i) {
    const ovt::ffmpeg::SliceSpan& s = bp.slices[i];
    const ovt::fb::Rect rect(s.rect.x, s.rect.y, s.rect.w, s.rect.h);
    ovt::fb::PartitionInfoBuilder p(fbb);
    p.add_kind(ovt::fb::PartitionKind::Slice);
    p.add_index(static_cast<uint16_t>(i));
    p.add_rect(&rect);
    p.add_first_block_addr(s.first_mb);
    p.add_block_count(s.mb_count);
    p.add_slice_type(s.slice_type);
    parts.push_back(p.Finish());
  }
  return parts;
}

std::unique_ptr<ovt_AnalysisDecoder::Pulled> build(ovt::ffmpeg::Frame frame, uint32_t plane_stage_mask) {
  auto out = std::make_unique<ovt_AnalysisDecoder::Pulled>();
  out->frame = std::move(frame);
  const ovt::ffmpeg::Frame& f = out->frame;
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);

  std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags;
  diags.reserve(f.diagnostics.size());
  for (const ovt::ffmpeg::Diag& d : f.diagnostics) {
    diags.push_back(ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(d.severity), d.code.c_str(),
                                                    d.message.c_str()));
  }
  const auto diags_v = fbb->CreateVector(diags);

  std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>> planes;
  if ((plane_stage_mask & (1U << kStageRecon)) != 0) {
    for (int c = 0; c < 3; ++c) {
      const ovt::ffmpeg::Plane& p = f.planes[static_cast<std::size_t>(c)];
      if (p.data.empty()) {
        continue;
      }
      std::string id = blob_id(f.decode_index, c);
      planes.push_back(ovt::fb::CreatePlaneRefDirect(*fbb, ovt::fb::PlaneStage::Reconstruction, static_cast<uint8_t>(c),
                                                     p.width, p.height, p.width * p.sample_bytes, p.bit_depth,
                                                     p.sample_bytes, /*is_signed=*/false, id.c_str()));
      out->blobs.emplace(std::move(id), &p);
    }
  }
  const auto planes_v = fbb->CreateVector(planes);
  const ovt::ffmpeg::SideData& sd = f.side;
  const auto grid_off = build_grid(*fbb, f);
  std::vector<flatbuffers::Offset<ovt::fb::Block>> blocks;
  std::vector<flatbuffers::Offset<ovt::fb::MotionVector>> mvs;
  std::vector<flatbuffers::Offset<ovt::fb::RefSlot>> refs;
  if (sd.valid) {
    build_blocks(*fbb, f, &blocks);
    for (const ovt::ffmpeg::MbMotion& m : sd.mvs) {
      const ovt::fb::Rect rect(m.rect.x, m.rect.y, m.rect.w, m.rect.h);
      ovt::fb::MotionVectorBuilder mb(*fbb);
      mb.add_block_id(m.block_id);
      mb.add_list(m.list);
      mb.add_ref(m.ref);
      mb.add_rect(&rect);
      mb.add_mv_x(m.mv_x);
      mb.add_mv_y(m.mv_y);
      mb.add_mvd_x(m.mvd_x);
      mb.add_mvd_y(m.mvd_y);
      mb.add_flags(m.flags);
      mvs.push_back(mb.Finish());
    }
    if (f.blocks.valid) {
      refs = ref_slots(*fbb, f.blocks);
    } else if (!sd.mvs.empty()) {
      const ovt::fb::FrameId unknown(-1, -1);
      for (uint8_t l = 0; l < 2; ++l) {
        const auto name = fbb->CreateString(l == 0 ? "L0" : "L1");
        ovt::fb::RefSlotBuilder rb(*fbb);
        rb.add_index(l);
        rb.add_frame(&unknown);
        rb.add_name(name);
        rb.add_usage_cells(sd.usage_cells.at(l));
        refs.push_back(rb.Finish());
      }
    }
  }
  const auto blocks_v = fbb->CreateVector(blocks);
  const auto mvs_v = fbb->CreateVector(mvs);
  const auto refs_v = fbb->CreateVector(refs);
  const auto partitions_v = fbb->CreateVector(slice_partitions(*fbb, f.blocks));
  const auto qp_hist_v = fbb->CreateVector(sd.qp_hist);

  // libavcodec exports no display_index, POC, slice data or picture structure: the host index has them (§7.5.3 A6).
  const ovt::fb::FrameId id(f.decode_index, -1);
  const ovt::fb::Rect coded(0, 0, f.coded_w, f.coded_h);
  const ovt::fb::Rect crop(f.crop_x, f.crop_y, f.crop_w, f.crop_h);
  ovt::fb::FrameInfoBuilder fi(*fbb);
  fi.add_id(&id);
  fi.add_pts(f.pts);
  fi.add_type(static_cast<ovt::fb::FrameType>(f.frame_type));
  fi.add_shown(true);  // libavcodec outputs only pictures that are output (C.4.5.3 bumping)
  fi.add_is_rap(f.is_rap);
  fi.add_coded_size(&coded);
  fi.add_crop(&crop);
  fi.add_chroma_format(f.chroma_format);
  fi.add_bit_depth_luma(f.bit_depth_luma);
  fi.add_bit_depth_chroma(f.bit_depth_chroma);
  const auto info_off = fi.Finish();

  const auto plugin_id = fbb->CreateString(info().caps.id);
  const auto plugin_version = fbb->CreateString(info().caps.version);
  ovt::fb::FrameAnalysisBuilder fa(*fbb);
  fa.add_schema_version(1);
  fa.add_plugin_id(plugin_id);
  fa.add_plugin_version(plugin_version);
  fa.add_frame(info_off);
  fa.add_grid(grid_off);
  fa.add_blocks(blocks_v);
  fa.add_mvs(mvs_v);
  fa.add_refs(refs_v);
  fa.add_partitions(partitions_v);
  fa.add_planes(planes_v);
  fa.add_qp_hist(qp_hist_v);
  fa.add_diagnostics(diags_v);
  ovt::fb::FinishFrameAnalysisBuffer(*fbb, fa.Finish());
  out->fbb = std::move(fbb);
  return out;
}

const ovt_AnalysisCaps* ff_caps() noexcept {
  try {
    return &info().caps;
  } catch (...) {
    return nullptr;  // out of memory while building the strings; the registry rejects the plugin
  }
}

// Runs @p body, turning an exception into OVT_E_NOMEM / OVT_E_PLUGIN: no exception crosses the ABI.
template <typename F>
ovt_status guarded(F&& body) noexcept {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status ff_create(const uint8_t* codec_private, size_t len, int threads, ovt_AnalysisDecoder** out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  return guarded([&] {
    auto dec = std::make_unique<ovt_AnalysisDecoder>();
    const ovt_status st = ovt::ffmpeg::H264Analysis::create(kDecoder, codec_private, len, threads, &dec->decoder);
    if (st == OVT_OK) {
      *out = dec.release();
    }
    return st;
  });
}

void ff_destroy(ovt_AnalysisDecoder* dec) noexcept {
  delete dec;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status ff_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t decode_index_hint,
                   int64_t pts) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return dec->decoder->push(unit, len, decode_index_hint, pts); });
}

ovt_status ff_pull(ovt_AnalysisDecoder* dec, uint32_t plane_stage_mask, ovt_Buffer* out_fb) noexcept {
  if (dec == nullptr || out_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out_fb = ovt_Buffer{nullptr, 0};
  return guarded([&] {
    ovt::ffmpeg::Frame frame;
    if (!dec->decoder->pop(&frame, (plane_stage_mask & (1U << kStageRecon)) != 0)) {
      return OVT_OK;
    }
    auto pulled = build(std::move(frame), plane_stage_mask);
    const ovt_Buffer fb{pulled->fbb->GetBufferPointer(), pulled->fbb->GetSize()};
    dec->pulled.emplace(fb.data, std::move(pulled));
    *out_fb = fb;  // only once the buffer is recorded, so a failure leaves *out_fb empty
    return OVT_OK;
  });
}

ovt_status ff_plane(ovt_AnalysisDecoder* dec, const char* blob_id, const uint8_t** data, size_t* size) noexcept {
  if (dec == nullptr || blob_id == nullptr || data == nullptr || size == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *data = nullptr;
  *size = 0;
  return guarded([&] {
    for (const auto& [ptr, pulled] : dec->pulled) {
      if (const auto it = pulled->blobs.find(blob_id); it != pulled->blobs.end()) {
        *data = it->second->data.data();
        *size = it->second->data.size();
        return OVT_OK;
      }
    }
    return OVT_E_INVALID_ARG;  // unknown, or its FrameAnalysis was already released
  });
}

void ff_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  if (dec != nullptr) {
    dec->pulled.erase(fb.data);
  }
}

ovt_status ff_flush(ovt_AnalysisDecoder* dec) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return dec->decoder->flush(); });
}

const ovt_AnalysisDecoderApi kApi = {
    sizeof(ovt_AnalysisDecoderApi),
    OVT_ANALYSIS_ABI_VERSION,
    ff_caps,
    ff_create,
    ff_destroy,
    ff_push,
    ff_pull,
    ff_plane,
    ff_release,
    ff_flush,
};

}  // namespace

extern "C" OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void) {
  return &kApi;
}

// The plugin's version, licence and avcodec_configuration() line for `ovt plugins` and logs (chapter 13 §13.3.3
// names it as the plugin's second export; WI-2.169 acceptance test 2 reads the configure line from it).
// SPEC-ASSUMPTION(#261): `const char* ovt_plugin_describe(void)`, a NUL-terminated UTF-8 string owned by the plugin
// and valid until it is unloaded, or NULL when it could not be built.
extern "C" OVT_PLUGIN_EXPORT const char* ovt_plugin_describe(void) {
  try {
    return info().describe.c_str();
  } catch (...) {
    return nullptr;
  }
}
