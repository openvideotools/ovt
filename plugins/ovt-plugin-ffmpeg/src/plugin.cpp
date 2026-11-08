// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libovt_plugin_ffmpeg: the ABI glue of the H.264 analysis plugin (chapter 0 §0.6.2, chapter 07 §7.6.3, WI-2.169).
// It exports ovt_analysis_decoder_v1() and ovt_plugin_describe() (chapter 13 §13.3.3) and nothing else; every entry
// point is noexcept and turns an exception into an error status. It produces FrameAnalysis buffers with FrameInfo,
// the Reconstruction planes and, from AV_FRAME_DATA_MOTION_VECTORS and AV_FRAME_DATA_VIDEO_ENC_PARAMS (WI-2.170,
// map_h264_sidedata.cpp), the macroblock tree, the cell grid, the motion vectors and the pseudo references L0 / L1.
// push() reports on the access unit BEFORE the pushed unit: OVT_E_CORRUPT means that earlier access unit failed to
// decode, the unit just pushed was accepted and must not be pushed again.

#include <flatbuffers/flatbuffers.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "h264_analysis.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"

namespace {

constexpr uint8_t kCodecH264 = 1;    // ovt.fb.Codec.H264
constexpr uint32_t kStageRecon = 0;  // ovt.fb.PlaneStage.Reconstruction
constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};
constexpr int16_t kMvdNotAvailable = -32768;     // MotionVector.mvd_x/y: the side data has no mvd (§7.6.3)
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
  std::string notes = std::string("H.264 analysis via libavcodec ") + av_version_info() +
                      " (motion vectors and QP side data; sub-8x8 MVs, ref_idx and intra modes need the phase-4 "
                      "MB-params patch)";
  std::string describe = std::string("ovt-plugin-ffmpeg ") + version + "; licence " + ffmpeg_spdx() +
                         "; avcodec_configuration(): " + avcodec_configuration();
  ovt_AnalysisCaps caps{};
  Info() {
    caps.struct_size = sizeof(ovt_AnalysisCaps);
    caps.codec = kCodecH264;
    caps.plane_stages = 1U << kStageRecon;
    // §7.6.3: block_id (MB granularity), pred (Intra/Inter only), qp, mv0/mv1 and the list-only ref0/ref1.
    caps.grid_fields =
        OVT_GRID_BLOCK_ID | OVT_GRID_PRED | OVT_GRID_QP | OVT_GRID_MV0 | OVT_GRID_MV1 | OVT_GRID_REF0 | OVT_GRID_REF1;
    caps.has_symbol_accounting = 0;
    caps.bit_exact = 1;
    caps.supports_threads = 1;  // libavcodec frame + slice threads
    caps.id = "ffmpeg";
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
  return g.Finish();
}

// blocks[0] is the virtual root (chapter 06 §6.3.2); blocks[1 + n] is macroblock n in raster order (kind MB, no
// extension table: the side data has no syntax elements).
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
  for (std::size_t i = 0; i < sd.mbs.size(); ++i) {
    const ovt::ffmpeg::MbBlock& mb = sd.mbs[i];
    const ovt::fb::Rect rect(mb.rect.x, mb.rect.y, mb.rect.w, mb.rect.h);
    ovt::fb::BlockBuilder b(fbb);
    b.add_id(1U + static_cast<uint32_t>(i));
    b.add_parent(0);
    b.add_kind(ovt::fb::BlockKind::MB);
    b.add_rect(&rect);
    b.add_level(1);
    b.add_partition_type(kPartitionNotAvailable);
    b.add_pred(static_cast<ovt::fb::PredKind>(mb.pred));
    b.add_qp(mb.qp);
    out->push_back(b.Finish());
  }
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
      mb.add_mvd_x(kMvdNotAvailable);
      mb.add_mvd_y(kMvdNotAvailable);
      mb.add_flags(m.flags);
      mvs.push_back(mb.Finish());
    }
    if (!sd.mvs.empty()) {
      // Two pseudo-slots: the side data names only the direction, not ref_idx (§7.6.3 RefSlot row).
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
    const ovt_status st = ovt::ffmpeg::H264Analysis::create(codec_private, len, threads, &dec->decoder);
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
