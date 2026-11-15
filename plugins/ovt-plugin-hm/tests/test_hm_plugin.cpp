// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Tests of ovt-plugin-hm (WI-4.105, chapter 07 §7.6.4), loaded through ovt::analysis::PluginRegistry like the host
// does. AT1 `hm_md5_vs_sei`: every picture of every fetchable HEVC_v1 stream matches its decoded picture hash SEI.
// AT3: an RExt 4:4:4 12-bit stream yields 12-bit 4:4:4 FrameAnalysis records. AT2 needs part b and WI-4.106.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <flatbuffers/flatbuffers.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TLibCommon/CommonDef.h"  // HM's typedefs, which its MD5.h needs
#include "libmd5/MD5.h"            // HM's public-domain MD5 (Colin Plumb), compiled into tp::hm_common
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/model/tree.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/active_sets.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_HM_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("hm");
  REQUIRE(plugin != nullptr);
  return *plugin->api;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, const uint8_t* cp = nullptr, size_t len = 0) : api(a) {
    status = api.create(cp, len, 0, &dec);
  }
  ~Instance() {
    if (dec != nullptr) {
      api.destroy(dec);
    }
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  const ovt_AnalysisDecoderApi& api;
  ovt_AnalysisDecoder* dec = nullptr;
  ovt_status status = OVT_E_INTERNAL;
};

// ---- H.265 D.3.19 decoded picture hash -----------------------------------------------------------------------------

// The hash of one plane as decoded_picture_hash carries it: 16 MD5 bytes, a 16-bit CRC or a 32-bit checksum,
// big-endian as in the SEI. The plane bytes are already in D.3.19 order: samples of more than 8 bits are two
// little-endian bytes.
std::vector<uint8_t> plane_hash(int hash_type, const uint8_t* data, uint32_t w, uint32_t h, uint32_t sample_bytes) {
  std::vector<uint8_t> bytes(data, data + (static_cast<size_t>(w) * h * sample_bytes));
  if (hash_type == 0) {
    MD5 md5;
    md5.update(bytes.data(), static_cast<unsigned>(bytes.size()));
    std::array<uint8_t, 16> d{};
    md5.finalize(d.data());
    return {d.begin(), d.end()};
  }
  uint32_t crc = 0xFFFF;
  uint32_t sum = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t msb = (crc >> 15) & 1U;
      crc = (((crc << 1) + ((bytes[i] >> (7 - bit)) & 1U)) & 0xFFFFU) ^ (msb * 0x1021U);
    }
    const auto x = static_cast<uint32_t>((i / sample_bytes) % w);
    const auto y = static_cast<uint32_t>((i / sample_bytes) / w);
    sum += bytes[i] ^ ((x & 0xFFU) ^ (y & 0xFFU) ^ (x >> 8) ^ (y >> 8));
  }
  if (hash_type == 2) {
    return {static_cast<uint8_t>(sum >> 24), static_cast<uint8_t>(sum >> 16), static_cast<uint8_t>(sum >> 8),
            static_cast<uint8_t>(sum)};
  }
  for (int bit = 0; bit < 16; ++bit) {
    const uint32_t msb = (crc >> 15) & 1U;
    crc = ((crc << 1) & 0xFFFFU) ^ (msb * 0x1021U);
  }
  return {static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
}

struct PictureHash {
  int hash_type = -1;
  std::vector<std::vector<uint8_t>> planes;
};

std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && p[i] == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = p[i] == 0 ? zeros + 1 : 0;
    out.push_back(p[i]);
  }
  return out;
}

// One SEI payloadType or payloadSize value: a run of 0xFF bytes plus a last byte (H.265 7.3.5).
bool read_sei_value(const std::vector<uint8_t>& rbsp, size_t* pos, uint32_t* value) {
  *value = 0;
  while (*pos < rbsp.size() && rbsp[*pos] == 0xFF) {
    *value += 255;
    ++*pos;
  }
  if (*pos >= rbsp.size()) {
    return false;
  }
  *value += rbsp[(*pos)++];
  return true;
}

// decoded_picture_hash (payloadType 132) of an SEI NAL unit, if it carries one.
bool parse_picture_hash(const uint8_t* nal, size_t len, PictureHash* out) {
  const std::vector<uint8_t> rbsp = unescape(nal + 2, len - 2);
  size_t pos = 0;
  while (pos + 2 <= rbsp.size() && rbsp[pos] != 0x80) {
    uint32_t type = 0;
    uint32_t size = 0;
    if (!read_sei_value(rbsp, &pos, &type) || !read_sei_value(rbsp, &pos, &size) || pos + size > rbsp.size()) {
      return false;
    }
    if (type == 132 && size >= 1) {
      out->hash_type = rbsp[pos];
      const size_t each = out->hash_type == 0 ? 16 : (out->hash_type == 1 ? 2 : 4);
      out->planes.clear();
      for (size_t off = pos + 1; off + each <= pos + size; off += each) {
        out->planes.emplace_back(rbsp.begin() + static_cast<std::ptrdiff_t>(off),
                                 rbsp.begin() + static_cast<std::ptrdiff_t>(off + each));
      }
      return true;
    }
    pos += size;
  }
  return false;
}

// ---- Annex B input ----------------------------------------------------------------------------------------------

struct Nal {
  const uint8_t* data;
  size_t size;
};

std::vector<Nal> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<Nal> nals;
  size_t i = 0;
  size_t start = SIZE_MAX;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) {
          --end;  // trailing_zero_8bits and the leading zero of a 4-byte start code
        }
        nals.push_back({s.data() + start, end - start});
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < s.size()) {
    nals.push_back({s.data() + start, s.size() - start});
  }
  return nals;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const ovt::fb::FrameAnalysis* as_fa(const ovt_Buffer& fb) {
  flatbuffers::Verifier verifier(fb.data, fb.size);
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  return ovt::fb::GetFrameAnalysis(fb.data);
}

// ---- Driving the plugin ----------------------------------------------------------------------------------------

struct PictureOut {
  std::array<PictureHash, 3> hashes;  // hashes of the plugin's planes, per hash_type
  int64_t poc = 0;
  int64_t pts = 0;
  uint8_t chroma_format = 0;
  uint8_t bit_depth_luma = 0;
  uint8_t bit_depth_chroma = 0;
  std::vector<uint8_t> plane_bit_depths;
  std::vector<uint8_t> plane_sample_bytes;
  std::shared_ptr<const struct GridOut> grid;  // when StreamResult::keep_grids
  std::shared_ptr<const struct PicOut> pic;    // partitions, filter parameters and RPS
};

// The CellGrid of one picture, with ref0/ref1 resolved to the refIdx of their RefSlot ("L0[i]", "S<n>:L1[i]") and,
// per cell, the cell index of the top-left of its CU (Block of grid.block_id).
struct GridOut {
  uint32_t w4 = 0;
  uint32_t h4 = 0;
  std::map<std::string, std::vector<int>> fields;
  std::vector<std::size_t> cu_origin;
  bool mv_clamp_reported = false;  // FrameAnalysis.diagnostics holds MDL-GEN-0003
};

// The picture-level data of one FrameAnalysis: partitions (slice segments, then tiles), the per-slice-segment
// HevcFilterParams vectors and HevcRps, plus the cells' partition_id and HevcCtu partition fields per CTU.
struct PicOut {
  struct Part {
    ovt::fb::PartitionKind kind{};
    int parent = -1;
    int slice_type = 255;
    ovt::fb::Rect rect;
    uint32_t first_block_addr = 0;
    uint32_t block_count = 0;
    int qp = 0, qp_delta = 0, deblocking_disable = 0, beta = 0, tc = 0, lf_across = 0, sao_luma = 0, sao_chroma = 0;
    int num_entry_points = 0, cabac_init = 0, max_merge = 0, tile_row = 0, tile_col = 0;
  };
  std::vector<Part> parts;
  std::vector<int> filter_disabled, filter_beta, filter_tc, filter_sao_luma, filter_sao_chroma;
  bool has_filter = false;
  bool has_rps = false;
  int num_pic_total_curr = 0, sps_flag = 0, sps_idx = 0, col_l0 = 0, col_idx = 0, output_flag = 0;
  int dpb_fullness = 0, max_dec_pic_buffering = 0;  // dpb_fullness_after, sps_max_dec_pic_buffering_minus1 + 1
  std::array<std::vector<int>, 3> st_deltas;        // PicOrderCntVal - current POC of StCurrBefore, StCurrAfter, StFoll
  std::array<std::size_t, 7> rps_sizes{};           // StCurrBefore, StCurrAfter, StFoll, LtCurr, LtFoll, L0, L1
  std::array<std::vector<int>, 2> list_frames;      // RefPicList0/1 decode indices
  std::array<std::vector<int>, 2> slot_frames;      // decode index of RefSlot "L0[i]" / "L1[i]"
  // Per CTU in tile scan: HevcCtu slice_segment_idx, tile_idx, CtbAddrInRs, the partition_id of its first cell, and
  // the CTU's x and y in luma samples.
  std::vector<std::array<uint32_t, 6>> ctus;
};

void rps_of(const ovt::fb::HevcRps& r, PicOut* out) {
  out->has_rps = true;
  out->num_pic_total_curr = r.NumPicTotalCurr();
  out->sps_flag = r.short_term_ref_pic_set_sps_flag();
  out->sps_idx = r.short_term_ref_pic_set_idx();
  out->col_l0 = r.collocated_from_l0_flag();
  out->col_idx = r.collocated_ref_idx();
  out->output_flag = r.pic_output_flag();
  out->dpb_fullness = r.dpb_fullness_after();
  out->max_dec_pic_buffering = r.sps_max_dec_pic_buffering_minus1() + 1;
  int set = 0;
  for (const auto* v : {r.RefPicSetStCurrBefore(), r.RefPicSetStCurrAfter(), r.RefPicSetStFoll()}) {
    std::vector<int>& d = out->st_deltas.at(static_cast<std::size_t>(set++));
    if (v != nullptr) {
      for (const ovt::fb::HevcRpsPic* e : *v) {
        d.push_back(e->PicOrderCntVal() - r.PicOrderCntVal());
      }
    }
    std::sort(d.begin(), d.end());
  }
  const auto size = [](const auto* v) { return v == nullptr ? std::size_t{0} : std::size_t{v->size()}; };
  out->rps_sizes = {size(r.RefPicSetStCurrBefore()),
                    size(r.RefPicSetStCurrAfter()),
                    size(r.RefPicSetStFoll()),
                    size(r.RefPicSetLtCurr()),
                    size(r.RefPicSetLtFoll()),
                    size(r.RefPicList0()),
                    size(r.RefPicList1())};
  for (int l = 0; l < 2; ++l) {
    if (const auto* v = l == 0 ? r.RefPicList0() : r.RefPicList1()) {
      for (const ovt::fb::HevcRpsPic* e : *v) {
        out->list_frames.at(static_cast<std::size_t>(l)).push_back(e->frame()->decode_index());
      }
    }
  }
}

std::shared_ptr<const PicOut> pic_of(const ovt::fb::FrameAnalysis& fa) {
  auto out = std::make_shared<PicOut>();
  if (fa.partitions() != nullptr) {
    for (const ovt::fb::PartitionInfo* p : *fa.partitions()) {
      out->parts.push_back({p->kind(), p->parent(), p->slice_type(), *p->rect(), p->first_block_addr(),
                            p->block_count(), p->qp(), p->qp_delta(), p->deblocking_disable(),
                            p->deblock_alpha_or_beta_offset_div2(), p->deblock_beta_or_tc_offset_div2(),
                            p->loop_filter_across_slices(), p->sao_luma(), p->sao_chroma(), p->num_entry_points(),
                            p->cabac_init(), p->max_num_merge_cand(), p->tile_row(), p->tile_col()});
    }
  }
  if (const auto* f = ovt::model::nested_as<ovt::fb::HevcFilterParams>(fa.filter_params(), fa.filter_params_type())) {
    out->has_filter = true;
    const auto copy = [](const auto* v, std::vector<int>* dst) {
      if (v != nullptr) {
        dst->assign(v->begin(), v->end());
      }
    };
    copy(f->slice_deblocking_filter_disabled_flag(), &out->filter_disabled);
    copy(f->slice_beta_offset_div2(), &out->filter_beta);
    copy(f->slice_tc_offset_div2(), &out->filter_tc);
    copy(f->slice_sao_luma_flag(), &out->filter_sao_luma);
    copy(f->slice_sao_chroma_flag(), &out->filter_sao_chroma);
  }
  if (const auto* r = ovt::model::nested_as<ovt::fb::HevcRps>(fa.frame()->ref_state(), fa.frame()->ref_state_type())) {
    rps_of(*r, out.get());
  }
  if (fa.refs() != nullptr) {
    for (const ovt::fb::RefSlot* r : *fa.refs()) {
      const std::string name = r->name()->str();
      if (name.starts_with("L0[") || name.starts_with("L1[")) {
        out->slot_frames.at(name[1] == '0' ? 0 : 1).push_back(r->frame()->decode_index());
      }
    }
  }
  const ovt::fb::CellGrid& g = *fa.grid();
  for (const uint32_t id : *fa.blocks()->Get(0)->children()) {
    const ovt::fb::Block& b = *fa.blocks()->Get(id);
    const auto* ctu = ovt::model::ext_as<ovt::fb::HevcCtu>(b);
    REQUIRE(ctu != nullptr);
    const uint32_t cell =
        (static_cast<uint32_t>(b.rect()->y() / 4) * g.cells_w()) + static_cast<uint32_t>(b.rect()->x() / 4);
    out->ctus.push_back({ctu->slice_segment_idx(), ctu->tile_idx(), ctu->CtbAddrInRs(), g.partition_id()->Get(cell),
                         static_cast<uint32_t>(b.rect()->x()), static_cast<uint32_t>(b.rect()->y())});
  }
  return out;
}

int ref_idx(const ovt::fb::FrameAnalysis& fa, int slot) {
  if (slot < 0 || fa.refs() == nullptr || static_cast<uint32_t>(slot) >= fa.refs()->size()) {
    return -1;
  }
  const std::string name = fa.refs()->Get(static_cast<uint32_t>(slot))->name()->str();
  const auto open = name.rfind('[');
  return open == std::string::npos ? -1 : std::stoi(name.substr(open + 1));
}

std::shared_ptr<const GridOut> grid_of(const ovt::fb::FrameAnalysis& fa) {
  auto out = std::make_shared<GridOut>();
  const ovt::fb::CellGrid& g = *fa.grid();
  out->w4 = g.cells_w();
  out->h4 = g.cells_h();
  const auto copy = [&](const char* name, const auto* v, auto map) {
    std::vector<int>& dst = out->fields[name];
    if (v != nullptr) {
      for (const auto x : *v) {
        dst.push_back(map(static_cast<int>(x)));
      }
    }
  };
  const auto same = [](int v) { return v; };
  copy("pred", g.pred(), same);
  copy("qp", g.qp(), same);
  copy("intra_mode_y", g.intra_mode_y(), same);
  copy("intra_mode_c", g.intra_mode_c(), same);
  copy("mv0_x", g.mv0_x(), same);
  copy("mv0_y", g.mv0_y(), same);
  copy("mv1_x", g.mv1_x(), same);
  copy("mv1_y", g.mv1_y(), same);
  copy("ref0", g.ref0(), [&](int v) { return ref_idx(fa, v); });
  copy("ref1", g.ref1(), [&](int v) { return ref_idx(fa, v); });
  copy("tx_log2_w", g.tx_log2_w(), same);
  copy("cbf", g.cbf(), same);
  if (fa.diagnostics() != nullptr) {
    for (const ovt::fb::Diagnostic* d : *fa.diagnostics()) {
      out->mv_clamp_reported = out->mv_clamp_reported || (d->code() != nullptr && d->code()->str() == "MDL-GEN-0003");
    }
  }
  for (const uint32_t id : *g.block_id()) {
    const ovt::fb::Rect* r = fa.blocks()->Get(id)->rect();
    out->cu_origin.push_back((static_cast<std::size_t>(r->y() / 4) * out->w4) + static_cast<std::size_t>(r->x() / 4));
  }
  return out;
}

struct StreamResult {
  int pictures = 0;                     // pictures in the stream (first_slice_segment_in_pic_flag = 1)
  std::set<int> skipped;                // RASL pictures of an IRAP with NoRaslOutputFlag = 1: never decoded
  std::map<int, PictureHash> expected;  // decode index -> SEI hash
  std::map<int, PictureOut> actual;     // decode index -> what pull() returned
  std::vector<int> pull_order;          // decode indices as pull() returned them
  int push_errors = 0;
  std::vector<std::string> violations;  // chapter 06 §6.9 validator findings (first few)
  bool keep_grids = false;
  // 4:2:2 HevcTu.cbf_cb / cbf_cr (ovt_model.fbs: two entries per chroma-coding node, one per vertical half).
  int tu422_two_entry_nodes = 0;   // nodes that carry two entries per component
  int tu422_halves_differ = 0;     // ... of which the two halves of a component differ (so they are not the OR twice)
  int tu422_parent_two_entry = 0;  // ... of which 8x8 nodes split into 4x4 luma TUs
  int tu422_parent_differ = 0;     // ... of those, with differing halves
  std::vector<std::string> tu422_errors;  // first few mismatches
};

// Checks the chroma cbf entries of every HevcTu of a 4:2:2 picture: their count per 7.3.8.8 (none at a 4x4 luma
// node, two where the node codes its chroma TBs: a leaf, or an 8x8 node split into 4x4 luma TUs; one otherwise),
// and that the OR of a node's two entries equals the chroma cbf bit of every grid cell the node covers (HM keeps
// the halves and their OR at different transform depths, so a wrong depth or half shows up as a mismatch).
// The cell index of the first grid cell inside @p rect whose chroma cbf bit @p c differs from @p any.
std::optional<uint32_t> grid_cbf_mismatch(const ovt::fb::CellGrid& g, const ovt::fb::Rect& rect, int c, int any) {
  const int w4 = static_cast<int>(g.cells_w());
  const int x1 = std::min((rect.x() + rect.w()) / 4, w4);
  const int y1 = std::min((rect.y() + rect.h()) / 4, static_cast<int>(g.cells_h()));
  for (int y4 = rect.y() / 4; y4 < y1; ++y4) {
    for (int x4 = rect.x() / 4; x4 < x1; ++x4) {
      const auto i = static_cast<uint32_t>((y4 * w4) + x4);
      if (((g.cbf()->Get(i) >> c) & 1) != any) {
        return i;
      }
    }
  }
  return std::nullopt;
}

// One component's chroma cbf entries of a 4:2:2 TU node: "" when they hold @p want entries and, for two, their OR
// matches the grid; sets *differ when the two entries differ.
std::string check_chroma_cbf(const ovt::fb::CellGrid& g, const ovt::fb::Rect& rect, int c,
                             const flatbuffers::Vector<uint8_t>* v, std::size_t want, bool* differ) {
  const std::size_t n = v == nullptr ? 0 : v->size();
  if (n != want) {
    return std::to_string(n) + " entries, want " + std::to_string(want);
  }
  if (n != 2) {
    return "";
  }
  *differ = *differ || v->Get(0) != v->Get(1);
  const auto bad = grid_cbf_mismatch(g, rect, c, (v->Get(0) | v->Get(1)) != 0 ? 1 : 0);
  return bad ? "OR of the halves differs from grid cbf at cell " + std::to_string(*bad) : "";
}

// Checks the chroma cbf entries of every HevcTu of a 4:2:2 picture: their count per 7.3.8.8 (none at a 4x4 luma
// node, two where the node codes its chroma TBs: a leaf, or an 8x8 node split into 4x4 luma TUs; one otherwise),
// and that the OR of a node's two entries equals the chroma cbf bit of every grid cell the node covers (HM keeps
// the halves and their OR at different transform depths, so a wrong depth or half shows up as a mismatch).
void check_tu_node(const ovt::fb::FrameAnalysis& fa, const ovt::fb::Block& b, const ovt::fb::HevcTu& tu,
                   StreamResult* r) {
  const int l2 = tu.log2TrafoSize();
  const bool split = tu.split_transform_flag() == 1;
  const std::size_t want = l2 == 2 ? 0 : ((!split || l2 == 3) ? 2 : 1);
  bool differ = false;
  for (int c = 1; c <= 2; ++c) {
    const std::string error =
        check_chroma_cbf(*fa.grid(), *b.rect(), c, c == 1 ? tu.cbf_cb() : tu.cbf_cr(), want, &differ);
    if (!error.empty() && r->tu422_errors.size() < 8) {
      std::string where = "decode index ";
      where += std::to_string(fa.frame()->id()->decode_index());
      where += " TU at " + std::to_string(b.rect()->x());
      where += "," + std::to_string(b.rect()->y());
      where += " log2 " + std::to_string(l2);
      where += " component " + std::to_string(c);
      where += ": ";
      where += error;
      r->tu422_errors.push_back(std::move(where));
    }
  }
  if (want == 2) {
    ++r->tu422_two_entry_nodes;
    r->tu422_halves_differ += differ ? 1 : 0;
    r->tu422_parent_two_entry += split ? 1 : 0;
    r->tu422_parent_differ += split && differ ? 1 : 0;
  }
}

void check_tu_cbf_422(const ovt::fb::FrameAnalysis& fa, StreamResult* r) {
  for (const ovt::fb::Block* b : *fa.blocks()) {
    if (const auto* tu = ovt::model::ext_as<ovt::fb::HevcTu>(*b)) {
      check_tu_node(fa, *b, *tu, r);
    }
  }
}

// Validator findings on what the host completes (A6): the timebase, PartitionInfo.unit_index and the source-hash
// prefix of blob ids.
bool host_fills(const ovt::model::Violation& v) {
  static const std::regex unit_index(R"(partitions\[[0-9]+\]\.unit_index)");
  return (v.rule == "V-002" && v.path == "frame.timebase_den") ||
         (v.rule == "V-017" && std::regex_match(v.path, unit_index)) ||
         (v.rule == "V-023" && v.path.ends_with(".blob_id") && v.message.find("source-hash") != std::string::npos);
}

// Pulls every ready frame; hashes its Reconstruction planes the way the stream's SEI for that picture does.
void drain(Instance& in, StreamResult* r) {
  for (;;) {
    ovt_Buffer fb{};
    REQUIRE(in.api.pull(in.dec, kReconMask, &fb) == OVT_OK);
    if (fb.data == nullptr) {
      return;
    }
    const ovt::fb::FrameAnalysis* fa = as_fa(fb);
    for (const auto& v : ovt::model::validate_frame_analysis(std::span<const uint8_t>(fb.data, fb.size))) {
      if (!host_fills(v) && r->violations.size() < 8) {
        r->violations.push_back(v.rule + " " + v.path + ": " + v.message);
      }
    }
    REQUIRE(fa->frame() != nullptr);
    REQUIRE(fa->frame()->id() != nullptr);
    CHECK(fa->plugin_id()->str() == "hm");
    if (fa->planes() == nullptr || fa->planes()->empty()) {
      in.api.release(in.dec, fb);  // a diagnostics-only record
      continue;
    }
    const int idx = fa->frame()->id()->decode_index();
    r->pull_order.push_back(idx);
    const auto* coded = fa->frame()->coded_size();
    REQUIRE(coded != nullptr);
    PictureOut out;
    out.poc = fa->frame()->poc();
    out.pts = fa->frame()->pts();
    out.chroma_format = fa->frame()->chroma_format();
    out.bit_depth_luma = fa->frame()->bit_depth_luma();
    out.bit_depth_chroma = fa->frame()->bit_depth_chroma();
    REQUIRE(fa->grid() != nullptr);
    CHECK(static_cast<int>(fa->grid()->cells_w()) == (coded->w() + 3) / 4);
    CHECK(static_cast<int>(fa->grid()->cells_h()) == (coded->h() + 3) / 4);
    for (const ovt::fb::PlaneRef* p : *fa->planes()) {
      CHECK(p->stage() == ovt::fb::PlaneStage::Reconstruction);
      // V-023: one byte per sample exactly for unsigned 8-bit planes.
      CHECK((p->sample_bytes() == 1) == (p->bit_depth() == 8 && !p->is_signed()));
      const uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(in.api.plane(in.dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
      REQUIRE(size == static_cast<size_t>(p->stride_bytes()) * p->height());
      REQUIRE(p->stride_bytes() == p->width() * p->sample_bytes());
      if (p->plane() == 0) {
        CHECK(static_cast<int>(p->width()) == coded->w());
        CHECK(static_cast<int>(p->height()) == coded->h());
      }
      out.plane_bit_depths.push_back(p->bit_depth());
      out.plane_sample_bytes.push_back(p->sample_bytes());
      for (int type = 0; type < 3; ++type) {
        out.hashes[static_cast<size_t>(type)].planes.push_back(
            plane_hash(type, data, p->width(), p->height(), p->sample_bytes()));
      }
    }
    if (r->keep_grids) {
      out.grid = grid_of(*fa);
    }
    out.pic = pic_of(*fa);
    if (out.chroma_format == 2) {
      check_tu_cbf_422(*fa, r);
    }
    CHECK(!r->actual.contains(idx));
    r->actual[idx] = std::move(out);
    in.api.release(in.dec, fb);
  }
}

// Feeds a stream unit by unit (decode index hints counted at first_slice_segment_in_pic_flag) and records each
// picture's SEI hash. RASL pictures of an IRAP with NoRaslOutputFlag = 1 are never decoded (H.265 8.1.3): `skipped`.
class StreamFeeder {
 public:
  StreamFeeder(Instance& in, const std::vector<uint8_t>& bytes) : in_(in), nals_(split_annexb(bytes)) {}

  [[nodiscard]] bool done() const { return next_ >= nals_.size(); }
  void keep_grids(bool keep) { r_.keep_grids = keep; }

  // Pushes the next NAL unit and pulls what is ready.
  void step() {
    const Nal& n = nals_[next_++];
    if (n.size < 2) {
      return;
    }
    const int type = (n.data[0] >> 1) & 0x3F;
    const int layer = ((n.data[0] & 0x01) << 5) | (n.data[1] >> 3);
    if (type == 36) {  // EOS_NUT
      after_eos_ = true;
    }
    if (type < 32 && layer == 0 && n.size > 2 && (n.data[2] & 0x80) != 0) {
      ++pic_;
      if (type >= 16 && type <= 23) {  // IRAP
        skip_rasl_ = type != 21 || pic_ == 0 || after_eos_;
        after_eos_ = false;
      }
      if ((type == 8 || type == 9) && skip_rasl_) {
        r_.skipped.insert(pic_);
      }
      for (PictureHash& h : prefix_hashes_) {
        r_.expected[pic_] = std::move(h);
      }
      prefix_hashes_.clear();
    }
    PictureHash h;
    if (type == 40 && pic_ >= 0 && parse_picture_hash(n.data, n.size, &h)) {  // SUFFIX_SEI_NUT
      r_.expected[pic_] = std::move(h);
    } else if (type == 39 && parse_picture_hash(n.data, n.size, &h)) {  // PREFIX_SEI_NUT
      prefix_hashes_.push_back(std::move(h));
    }
    const int hint = std::max(pic_, 0);
    if (in_.api.push(in_.dec, n.data, n.size, hint, 1000 + hint) != OVT_OK) {
      ++r_.push_errors;
    }
    drain(in_, &r_);
  }

  StreamResult finish() {
    REQUIRE(in_.api.flush(in_.dec) == OVT_OK);
    drain(in_, &r_);
    for (const int s : r_.skipped) {
      r_.expected.erase(s);
    }
    r_.pictures = pic_ + 1;
    return std::move(r_);
  }

 private:
  Instance& in_;
  std::vector<Nal> nals_;
  size_t next_ = 0;
  int pic_ = -1;
  bool skip_rasl_ = false;
  bool after_eos_ = false;
  std::vector<PictureHash> prefix_hashes_;  // prefix SEI hashes wait for their picture's first slice
  StreamResult r_;
};

StreamResult run_stream(const std::vector<uint8_t>& bytes, bool keep_grids = false) {
  Instance in(plugin_api());
  REQUIRE(in.status == OVT_OK);
  StreamFeeder feeder(in, bytes);
  feeder.keep_grids(keep_grids);
  while (!feeder.done()) {
    feeder.step();
  }
  return feeder.finish();
}

// AT1 checks of one stream: every decodable picture out exactly once by decode index, in decode order, with the
// host's pts; every hash equal to its SEI.
void check_stream(const StreamResult& r, bool expect_hashes) {
  CHECK(r.push_errors == 0);
  for (const std::string& v : r.violations) {
    UNSCOPED_INFO(v);
  }
  CHECK(r.violations.empty());
  REQUIRE(r.pictures > 0);
  std::set<int> want;
  for (int i = 0; i < r.pictures; ++i) {
    if (!r.skipped.contains(i)) {
      want.insert(i);
    }
  }
  std::set<int> got;
  for (const auto& [idx, out] : r.actual) {
    got.insert(idx);
    CHECK(out.pts == 1000 + idx);
  }
  CHECK(got == want);
  CHECK(std::is_sorted(r.pull_order.begin(), r.pull_order.end()));  // HM's pictures come out in decode order
  if (expect_hashes) {
    CHECK(!r.expected.empty());
  }
  int mismatches = 0;
  for (const auto& [idx, sei] : r.expected) {
    const auto it = r.actual.find(idx);
    if (it == r.actual.end() || sei.hash_type < 0 || sei.hash_type > 2 ||
        it->second.hashes[static_cast<size_t>(sei.hash_type)].planes != sei.planes) {
      ++mismatches;
      UNSCOPED_INFO("hash mismatch at decode index " << idx);
    }
  }
  INFO(r.expected.size() << " of " << want.size() << " decoded pictures carry a decoded picture hash SEI");
  CHECK(mismatches == 0);
}

// AT1: every HEVC_v1 entry of tests/corpora/manifest.json with a fetchable, verified file (source jctvc, codec h265,
// no rext/scc feature). These carry no picture hash SEI (#255), so for them AT1 checks only that every picture comes
// out: RPS_A, RPS_F, POC_A, AMP_A, AMP_D, CONFWIN_A, MAXBINS_A, STRUCT_B, TILES_B, RAP_A, FILLER_A.
struct At1Stream {
  std::string_view id;
  bool hashes;
};
constexpr std::array<At1Stream, 40> kAt1Streams = {{
    {"jctvc_AMP_A_Samsung_7", false},
    {"jctvc_AMP_D_Hisilicon_3", false},
    {"jctvc_CAINIT_A_SHARP_4", true},
    {"jctvc_CONFWIN_A_Sony_1", false},
    {"jctvc_DSLICE_A_HHI_5", true},
    {"jctvc_ENTP_A_QUALCOMM_1", true},
    {"jctvc_EXT_A_ericsson_4", true},
    {"jctvc_FILLER_A_Sony_1", false},
    {"jctvc_HRD_A_Fujitsu_3", true},
    {"jctvc_MAXBINS_A_TI_5", false},
    {"jctvc_MERGE_B_TI_3", true},
    {"jctvc_NUT_A_ericsson_5", true},
    {"jctvc_OPFLAG_A_Qualcomm_1", true},
    {"jctvc_PICSIZE_A_Bossen_1", true},
    {"jctvc_PMERGE_A_TI_3", true},
    {"jctvc_POC_A_Bossen_3", false},
    {"jctvc_PPS_A_qualcomm_7", true},
    {"jctvc_RAP_A_docomo_6", false},
    {"jctvc_RPS_A_docomo_5", false},
    {"jctvc_SAO_A_MediaTek_4", true},
    {"jctvc_SDH_A_Orange_4", true},
    {"jctvc_SLIST_A_Sony_5", true},
    {"jctvc_STRUCT_B_Samsung_7", false},
    {"jctvc_TILES_B_Cisco_1", false},
    {"jctvc_TSCL_A_VIDYO_5", true},
    {"jctvc_WP_A_Toshiba_3", true},
    {"jctvc_WPP_A_ericsson_MAIN10_2", true},
    {"jctvc_VPSID_A_VIDYO_2", true},
    {"jctvc_WPP_A_ericsson_MAIN_2", true},
    {"jctvc_RAP_B_Bossen_2", true},
    {"jctvc_TSKIP_A_MS_3", true},
    {"jctvc_MVCLIP_A_qualcomm_3", true},
    {"jctvc_MVEDGE_A_qualcomm_3", true},
    {"jctvc_NoOutPrior_A_Qualcomm_1", true},
    {"jctvc_ipcm_A_NEC_3", true},
    {"jctvc_RPS_B_qualcomm_5", true},
    {"jctvc_RPS_C_ericsson_5", true},
    {"jctvc_RPS_D_ericsson_6", true},
    {"jctvc_RPS_E_qualcomm_5", true},
    {"jctvc_RPS_F_docomo_2", false},
}};

// ---- Picture-level data against the slice segment headers -------------------------------------------------------

namespace h265 = ovt::syntax::h265;

// The layer-0 slice segment headers of one picture, parsed by the core H.265 parser, with its tile and CTB counts.
struct HeaderPic {
  std::vector<h265::SliceSegmentHeader> segs;
  std::size_t tiles = 1;
  uint32_t ctbs = 0;
  bool idr = false;      // IDR_W_RADL or IDR_N_LP: the slice headers carry no RPS syntax
  bool bla = false;      // BLA_W_LP, BLA_W_RADL or BLA_N_LP
  h265::StRefPicSet st;  // the short-term RPS of the picture (the SPS's or the slice header's own)
};

// Stores a VPS, SPS or PPS NAL unit in @p sets.
void put_parameter_set(const h265::NalHeader& h, ovt::BitReader& br, ovt::syntax::SyntaxBuilder& b,
                       std::span<const uint8_t> rbsp, uint64_t unit, h265::ActiveSets* sets,
                       std::vector<h265::SetDiagnostic>* diags) {
  if (h.nal_unit_type < h265::kVpsNut || h.nal_unit_type > h265::kPpsNut) {
    return;
  }
  sets->end_picture();  // conforming streams change a set's content only between pictures
  if (h.nal_unit_type == h265::kVpsNut) {
    sets->put_vps(h265::parse_vps(br, b), rbsp, unit, *diags);
  } else if (h.nal_unit_type == h265::kSpsNut) {
    sets->put_sps(h265::parse_sps(br, b, h.nuh_layer_id), rbsp, unit, *diags);
  } else {
    sets->put_pps(h265::parse_pps(br, b), rbsp, unit, *diags);
  }
}

// The picture-level fields of a HeaderPic, from its first slice segment.
HeaderPic header_pic(const h265::NalHeader& h, const h265::SliceSegmentHeader& sh, const h265::Activation& act) {
  HeaderPic out;
  out.tiles = (act.active->colBd.size() - 1) * (act.active->rowBd.size() - 1);
  out.ctbs = act.sps->PicSizeInCtbsY;
  out.idr = h.nal_unit_type == h265::kIdrWRadl || h.nal_unit_type == h265::kIdrNLp;
  out.bla = h.nal_unit_type >= h265::kBlaWLp && h.nal_unit_type <= h265::kBlaNLp;
  if (!out.idr) {
    out.st = sh.short_term_ref_pic_set_sps_flag ? act.sps->st_ref_pic_set.at(sh.short_term_ref_pic_set_idx)
                                                : sh.st_ref_pic_set;
  }
  return out;
}

// Parses every layer-0 slice segment header of @p bytes, keyed by the picture counter StreamFeeder hints with.
std::map<int, HeaderPic> headers_of(const std::vector<uint8_t>& bytes) {
  std::map<int, HeaderPic> out;
  h265::ActiveSets sets;
  std::vector<h265::SetDiagnostic> diags;
  std::optional<h265::SliceSegmentHeader> prev_independent;
  int pic = -1;
  uint64_t unit = 0;
  for (const Nal& n : split_annexb(bytes)) {
    if (n.size < 3) {
      continue;
    }
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(n.data, n.size));
    ovt::BitReader br(rbsp.rbsp);
    ovt::syntax::SyntaxBuilder b(br);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    ++unit;
    if (h.nuh_layer_id != 0) {
      continue;
    }
    put_parameter_set(h, br, b, rbsp.rbsp, unit, &sets, &diags);
    if (!h.is_vcl) {
      continue;
    }
    h265::SliceSegmentHeader sh =
        h265::parse_slice_segment_header(br, b, h, sets.pps_table(), prev_independent ? &*prev_independent : nullptr);
    const auto act = sets.activate(sh.slice_pic_parameter_set_id, sh.first_slice_segment_in_pic_flag, diags);
    REQUIRE(act.has_value());
    if (!sh.dependent_slice_segment_flag) {
      prev_independent = sh;
    }
    if (sh.first_slice_segment_in_pic_flag) {
      out[++pic] = header_pic(h, sh, *act);
    }
    if (pic >= 0) {
      out[pic].segs.push_back(std::move(sh));
    }
  }
  return out;
}

// Accumulates mismatches as "field: got X, want Y" lines (the first few).
struct Mismatches {
  std::vector<std::string> lines;
  std::size_t count = 0;
  void check(int idx, const std::string& what, long long got, long long want) {
    if (got != want && ++count <= 16) {
      lines.push_back("decode index " + std::to_string(idx) + " " + what + ": got " + std::to_string(got) + ", want " +
                      std::to_string(want));
    }
  }
};

// One slice segment's PartitionInfo and HevcFilterParams entries against its header.
void check_segment(int idx, std::size_t i, const PicOut& p, const h265::SliceSegmentHeader& sh, int independent,
                   Mismatches* m) {
  const PicOut::Part& part = p.parts[i];
  const std::string s = "segment " + std::to_string(i) + " ";
  const bool intra = sh.slice_type == h265::kSliceI;
  m->check(idx, s + "kind", static_cast<int>(part.kind),
           static_cast<int>(sh.dependent_slice_segment_flag ? ovt::fb::PartitionKind::SliceSegment
                                                            : ovt::fb::PartitionKind::Slice));
  m->check(idx, s + "parent", part.parent, sh.dependent_slice_segment_flag ? independent : -1);
  m->check(idx, s + "slice_type", part.slice_type, sh.slice_type);
  m->check(idx, s + "first_block_addr", part.first_block_addr, sh.slice_segment_address);
  m->check(idx, s + "qp", part.qp, sh.SliceQpY);
  m->check(idx, s + "qp_delta", part.qp_delta, sh.slice_qp_delta);
  m->check(idx, s + "deblocking_disable", part.deblocking_disable, sh.slice_deblocking_filter_disabled_flag ? 1 : 0);
  m->check(idx, s + "beta_offset_div2", part.beta, sh.slice_beta_offset_div2);
  m->check(idx, s + "tc_offset_div2", part.tc, sh.slice_tc_offset_div2);
  m->check(idx, s + "loop_filter_across_slices", part.lf_across,
           sh.slice_loop_filter_across_slices_enabled_flag ? 1 : 0);
  m->check(idx, s + "sao_luma", part.sao_luma, sh.slice_sao_luma_flag ? 1 : 0);
  m->check(idx, s + "sao_chroma", part.sao_chroma, sh.slice_sao_chroma_flag ? 1 : 0);
  m->check(idx, s + "num_entry_points", part.num_entry_points, sh.num_entry_point_offsets);
  m->check(idx, s + "cabac_init", part.cabac_init, intra ? 255 : (sh.cabac_init_flag ? 1 : 0));
  m->check(idx, s + "max_num_merge_cand", part.max_merge, intra ? 255 : sh.MaxNumMergeCand);
  m->check(idx, s + "filter deblocking_disabled", p.filter_disabled.at(i), part.deblocking_disable);
  m->check(idx, s + "filter beta_offset_div2", p.filter_beta.at(i), part.beta);
  m->check(idx, s + "filter tc_offset_div2", p.filter_tc.at(i), part.tc);
  m->check(idx, s + "filter sao_luma", p.filter_sao_luma.at(i), part.sao_luma);
  m->check(idx, s + "filter sao_chroma", p.filter_sao_chroma.at(i), part.sao_chroma);
}

// The expected PicOrderCntVal differences of StCurrBefore, StCurrAfter and StFoll (7-61, 7-62), each sorted.
std::array<std::vector<int>, 3> expected_st_deltas(const HeaderPic& want) {
  std::array<std::vector<int>, 3> deltas;
  // SPEC-ASSUMPTION(#553): HM drops a BLA picture's RPS, so the HM plugin reports its lists empty.
  const h265::StRefPicSet st = want.bla ? h265::StRefPicSet{} : want.st;
  for (uint32_t i = 0; i < st.NumNegativePics; ++i) {
    deltas.at(st.UsedByCurrPicS0.at(i) ? 0 : 2).push_back(st.DeltaPocS0.at(i));
  }
  for (uint32_t i = 0; i < st.NumPositivePics; ++i) {
    deltas.at(st.UsedByCurrPicS1.at(i) ? 1 : 2).push_back(st.DeltaPocS1.at(i));
  }
  for (std::vector<int>& d : deltas) {
    std::sort(d.begin(), d.end());
  }
  return deltas;
}

std::string join_ints(const std::vector<int>& v) {
  std::string out;
  for (const int d : v) {
    out += (out.empty() ? "" : ",") + std::to_string(d);
  }
  return "{" + out + "}";
}

// The PicOrderCntVal differences of the short-term RPS lists against the parser's DeltaPocS0/S1, and
// dpb_fullness_after within [1 + the "Curr" references, sps_max_dec_pic_buffering_minus1 + 1], 1 at an IDR or BLA.
void check_st_rps_and_fullness(int idx, const PicOut& p, const HeaderPic& want, Mismatches* m) {
  const std::array<std::vector<int>, 3> deltas = expected_st_deltas(want);
  static constexpr std::array<const char*, 3> kNames = {"StCurrBefore", "StCurrAfter", "StFoll"};
  for (std::size_t l = 0; l < 3; ++l) {
    if (deltas.at(l) != p.st_deltas.at(l)) {
      m->check(idx,
               std::string("RefPicSet") + kNames.at(l) + " POC deltas " + join_ints(p.st_deltas.at(l)) +
                   " differ from st_ref_pic_set " + join_ints(deltas.at(l)),
               1, 0);
    }
  }
  const std::size_t curr = p.rps_sizes[0] + p.rps_sizes[1] + p.rps_sizes[3];
  m->check(idx, "dpb_fullness_after >= 1 + Curr references", std::cmp_greater_equal(p.dpb_fullness, 1 + curr) ? 1 : 0,
           1);
  m->check(idx, "dpb_fullness_after <= sps_max_dec_pic_buffering_minus1 + 1",
           p.dpb_fullness <= p.max_dec_pic_buffering ? 1 : 0, 1);
  if (want.bla) {
    m->check(idx, "BLA long-term entries", p.rps_sizes[3] + p.rps_sizes[4] == 0 ? 0 : 1, 0);
  }
  if (want.idr || want.bla) {
    m->check(idx, "dpb_fullness_after at an IDR or BLA", p.dpb_fullness, 1);
  }
}

// HevcRps against the first slice segment header.
void check_rps(int idx, const PicOut& p, const h265::SliceSegmentHeader& sh, bool idr, Mismatches* m) {
  m->check(idx, "has ref_state", p.has_rps ? 1 : 0, 1);
  m->check(idx, "NumPicTotalCurr", p.num_pic_total_curr, sh.NumPicTotalCurr);
  m->check(idx, "StCurrBefore + StCurrAfter + LtCurr",
           static_cast<long long>(p.rps_sizes[0]) + static_cast<long long>(p.rps_sizes[1]) +
               static_cast<long long>(p.rps_sizes[3]),
           sh.NumPicTotalCurr);
  m->check(idx, "short_term_ref_pic_set_sps_flag", p.sps_flag,
           idr ? 255 : (sh.short_term_ref_pic_set_sps_flag ? 1 : 0));
  m->check(idx, "short_term_ref_pic_set_idx", p.sps_idx,
           !idr && sh.short_term_ref_pic_set_sps_flag ? sh.short_term_ref_pic_set_idx : 255);
  m->check(idx, "pic_output_flag", p.output_flag, sh.pic_output_flag ? 1 : 0);
  const bool p_or_b = sh.slice_type != h265::kSliceI;
  const bool b = sh.slice_type == h265::kSliceB;
  m->check(idx, "RefPicList0 size", static_cast<long long>(p.rps_sizes[5]),
           p_or_b ? sh.num_ref_idx_l0_active_minus1 + 1 : 0);
  m->check(idx, "RefPicList1 size", static_cast<long long>(p.rps_sizes[6]),
           b ? sh.num_ref_idx_l1_active_minus1 + 1 : 0);
  const bool tmvp = sh.slice_temporal_mvp_enabled_flag && p_or_b;
  m->check(idx, "collocated_from_l0_flag", p.col_l0, tmvp ? (sh.collocated_from_l0_flag ? 1 : 0) : 255);
  m->check(idx, "collocated_ref_idx", p.col_idx, tmvp ? sh.collocated_ref_idx : 255);
  for (std::size_t l = 0; l < 2; ++l) {
    if (p.list_frames.at(l) != p.slot_frames.at(l)) {
      m->check(idx, "RefPicList" + std::to_string(l) + " differs from RefSlots L" + std::to_string(l), 1, 0);
    }
  }
}

// The CTUs' HevcCtu fields and cells against the partitions: each CTU is in exactly one segment, the one its cells'
// partition_id names, and (with tiles) inside the Tile partition its tile_idx names.
void check_ctus(int idx, const PicOut& p, std::size_t segments, std::size_t tiles, Mismatches* m) {
  std::vector<std::size_t> count(segments, 0);
  for (const auto& c : p.ctus) {
    m->check(idx, "CTU " + std::to_string(c[2]) + " partition_id", c[3], c[0]);
    if (c[0] >= segments) {
      m->check(idx, "CTU " + std::to_string(c[2]) + " slice_segment_idx in range", 0, 1);
      continue;
    }
    ++count[c[0]];
    if (tiles <= 1) {
      m->check(idx, "CTU " + std::to_string(c[2]) + " tile_idx", c[1], 65535);
      continue;
    }
    const bool in_range = c[1] >= segments && c[1] < p.parts.size();
    const ovt::fb::Rect* r = in_range ? &p.parts[c[1]].rect : nullptr;
    const bool inside = r != nullptr && std::cmp_greater_equal(c[4], r->x()) && std::cmp_less(c[4], r->x() + r->w()) &&
                        std::cmp_greater_equal(c[5], r->y()) && std::cmp_less(c[5], r->y() + r->h());
    m->check(idx, "CTU " + std::to_string(c[2]) + " inside its tile", inside ? 1 : 0, 1);
  }
  for (std::size_t i = 0; i < segments; ++i) {
    m->check(idx, "segment " + std::to_string(i) + " block_count", p.parts[i].block_count,
             static_cast<long long>(count[i]));
  }
}

// One decoded picture's PicOut against its slice segment headers.
void check_picture(int idx, const PicOut& p, const HeaderPic& want, Mismatches* m) {
  const std::size_t segments = want.segs.size();
  const std::size_t tile_parts = want.tiles > 1 ? want.tiles : 0;
  m->check(idx, "partitions", static_cast<long long>(p.parts.size()),
           static_cast<long long>(segments) + static_cast<long long>(tile_parts));
  m->check(idx, "CTUs", static_cast<long long>(p.ctus.size()), want.ctbs);
  m->check(idx, "has filter_params", p.has_filter ? 1 : 0, 1);
  if (p.parts.size() != segments + tile_parts || p.filter_disabled.size() != segments ||
      p.filter_beta.size() != segments || p.filter_tc.size() != segments || p.filter_sao_luma.size() != segments ||
      p.filter_sao_chroma.size() != segments) {
    m->check(idx, "filter_params vectors sized per segment", 0, 1);
    return;
  }
  int independent = -1;
  for (std::size_t i = 0; i < segments; ++i) {
    if (!want.segs[i].dependent_slice_segment_flag) {
      independent = static_cast<int>(i);
    }
    check_segment(idx, i, p, want.segs[i], independent, m);
  }
  for (std::size_t t = 0; t < tile_parts; ++t) {
    m->check(idx, "partition " + std::to_string(segments + t) + " kind", static_cast<int>(p.parts[segments + t].kind),
             static_cast<int>(ovt::fb::PartitionKind::Tile));
  }
  check_ctus(idx, p, segments, want.tiles, m);
  check_rps(idx, p, want.segs.front(), want.idr, m);
  check_st_rps_and_fullness(idx, p, want, m);
}

// Streams with dependent segments, entry points, tiles, WPP, several slices, explicit and SPS RPSs, long-term
// pictures, PPS changes, SAO, cabac_init_flag and merge candidates.
constexpr std::array<std::string_view, 16> kPictureLevelStreams = {
    "jctvc_DSLICE_A_HHI_5",   "jctvc_ENTP_A_QUALCOMM_1",  "jctvc_TILES_B_Cisco_1", "jctvc_WPP_A_ericsson_MAIN_2",
    "jctvc_SLIST_A_Sony_5",   "jctvc_STRUCT_B_Samsung_7", "jctvc_RPS_A_docomo_5",  "jctvc_RPS_B_qualcomm_5",
    "jctvc_RPS_E_qualcomm_5", "jctvc_RPS_F_docomo_2",     "jctvc_POC_A_Bossen_3",  "jctvc_PPS_A_qualcomm_7",
    "jctvc_SAO_A_MediaTek_4", "jctvc_CAINIT_A_SHARP_4",   "jctvc_MERGE_B_TI_3",    "jctvc_NUT_A_ericsson_5",
};

}  // namespace

TEST_CASE("hm plugin: malformed hvcC, units and arguments are rejected", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const std::array<uint8_t, 4> short_hvcc = {1, 2, 3, 4};
  ovt_AnalysisDecoder* dec = nullptr;
  CHECK(api.create(short_hvcc.data(), short_hvcc.size(), 0, &dec) == OVT_E_CORRUPT);
  CHECK(dec == nullptr);
  // 22 configuration bytes, one array claiming one 0x40-byte NAL unit that is not there.
  std::vector<uint8_t> truncated(22, 0);
  truncated.insert(truncated.end(), {1, 0x21, 0x00, 0x01, 0x00, 0x40, 0x42});
  CHECK(api.create(truncated.data(), truncated.size(), 0, &dec) == OVT_E_CORRUPT);
  CHECK(api.create(nullptr, 0, 0, nullptr) == OVT_E_INVALID_ARG);

  const Instance in(api);
  REQUIRE(in.status == OVT_OK);
  CHECK(api.push(in.dec, nullptr, 0, 0, 0) == OVT_E_INVALID_ARG);
  const std::array<uint8_t, 1> one = {0x40};
  CHECK(api.push(in.dec, one.data(), one.size(), 0, 0) == OVT_E_CORRUPT);
  const std::array<uint8_t, 3> forbidden_bit = {0xC0, 0x01, 0x0C};
  CHECK(api.push(in.dec, forbidden_bit.data(), forbidden_bit.size(), 0, 0) == OVT_E_CORRUPT);
  const uint8_t* data = nullptr;
  size_t size = 0;
  CHECK(api.plane(in.dec, "0/recon/y", &data, &size) == OVT_E_INVALID_ARG);
  // The rejected units' diagnostics come out after the end of the stream, in a diagnostics-only record.
  REQUIRE(api.flush(in.dec) == OVT_OK);
  ovt_Buffer fb{};
  REQUIRE(api.pull(in.dec, kReconMask, &fb) == OVT_OK);
  REQUIRE(fb.data != nullptr);
  const ovt::fb::FrameAnalysis* fa = as_fa(fb);
  REQUIRE(fa->diagnostics() != nullptr);
  CHECK(fa->diagnostics()->size() == 2);
  CHECK(fa->grid() == nullptr);
  api.release(in.dec, fb);
  CHECK(api.push(in.dec, one.data(), one.size(), 0, 0) == OVT_E_INVALID_ARG);  // after flush()
}

TEST_CASE("hm plugin: hm_md5_vs_sei on the HEVC_v1 conformance streams", "[fast][third_party][needs-corpus]") {
  // AT1 is all streams or nothing: without any of them (no corpora fetched) the test skips; once one is present,
  // a missing one fails, so a partial corpus can never report AT1 as passed or skipped.
  std::size_t present = 0;
  for (const At1Stream& s : kAt1Streams) {
    present += ovt::test::corpus_path(s.id).has_value() ? 1U : 0U;
  }
  if (present == 0) {
    (void)ovt::test::corpus_or_skip(kAt1Streams.front().id);  // skips, or fails under OVT_REQUIRE_CORPORA=1
  }
  INFO(present << "/" << kAt1Streams.size() << " AT1 streams present");
  const At1Stream s = GENERATE(from_range(kAt1Streams));
  CAPTURE(s.id);
  const std::optional<std::filesystem::path> path = ovt::test::corpus_path(s.id);
  REQUIRE(path.has_value());
  check_stream(run_stream(read_file(*path)), s.hashes);
}

TEST_CASE("hm plugin: partitions, filter parameters and RPS match the slice headers",
          "[fast][third_party][needs-corpus]") {
  const std::string_view id = GENERATE(from_range(kPictureLevelStreams));
  CAPTURE(id);
  const std::vector<uint8_t> bytes = read_file(ovt::test::corpus_or_skip(id));
  const StreamResult r = run_stream(bytes);
  check_stream(r, false);
  const std::map<int, HeaderPic> headers = headers_of(bytes);
  REQUIRE(static_cast<int>(headers.size()) == r.pictures);
  Mismatches m;
  std::size_t segments = 0;
  std::size_t dependent = 0;
  std::size_t tiled = 0;
  std::size_t long_term = 0;
  for (const auto& [idx, out] : r.actual) {
    REQUIRE(out.pic != nullptr);
    const HeaderPic& want = headers.at(idx);
    check_picture(idx, *out.pic, want, &m);
    segments += want.segs.size();
    dependent += static_cast<std::size_t>(std::count_if(
        want.segs.begin(), want.segs.end(), [](const auto& sh) { return sh.dependent_slice_segment_flag; }));
    tiled += want.tiles > 1 ? 1U : 0U;
    long_term += out.pic->rps_sizes[3] + out.pic->rps_sizes[4] > 0 ? 1U : 0U;
  }
  for (const std::string& line : m.lines) {
    UNSCOPED_INFO(line);
  }
  INFO(r.actual.size() << " pictures, " << segments << " slice segments (" << dependent << " dependent), " << tiled
                       << " with tiles, " << long_term << " with long-term references");
  CHECK(m.count == 0);
}

TEST_CASE("hm plugin: RExt 4:4:4 12-bit yields a FrameAnalysis per picture", "[fast][third_party][needs-corpus]") {
  // AT3. The primary file of syn_h265_422_444_rext (chapter 14 §14.2, recipe 11) is the x265 main444-12 stream.
  const std::filesystem::path path = ovt::test::corpus_or_skip("syn_h265_422_444_rext");
  const StreamResult r = run_stream(read_file(path));
  check_stream(r, false);
  REQUIRE(!r.actual.empty());
  for (const auto& [idx, out] : r.actual) {
    CAPTURE(idx);
    CHECK(out.chroma_format == 3);
    CHECK(out.bit_depth_luma == 12);
    CHECK(out.bit_depth_chroma == 12);
    CHECK(out.plane_bit_depths == std::vector<uint8_t>{12, 12, 12});
    CHECK(out.plane_sample_bytes == std::vector<uint8_t>{2, 2, 2});
  }
}

TEST_CASE("hm plugin: 4:2:2 HevcTu chroma cbf has one entry per chroma TB", "[fast][third_party][needs-corpus]") {
  // syn_h265_422_444_rext (recipe 11) also holds the x265 main422-10 stream next to its primary file.
  const std::filesystem::path path =
      ovt::test::corpus_or_skip("syn_h265_422_444_rext").parent_path() / "syn_h265_422_10bit.265";
  REQUIRE(std::filesystem::exists(path));
  const StreamResult r = run_stream(read_file(path));
  check_stream(r, false);
  std::string errors;
  for (const std::string& e : r.tu422_errors) {
    errors += e + "\n";
  }
  INFO(errors);
  CHECK(r.tu422_errors.empty());
  INFO("two-entry nodes " << r.tu422_two_entry_nodes << ", halves differ " << r.tu422_halves_differ << ", 8x8 parents "
                          << r.tu422_parent_two_entry);
  CHECK(r.tu422_two_entry_nodes > 0);
  // Reading the OR's depth would give the same flag twice; the halves must differ somewhere in the stream.
  CHECK(r.tu422_halves_differ > 0);
  CHECK(r.tu422_parent_differ > 0);  // HM keeps those halves one depth lower than at a leaf
}

namespace {

// ---- part b: the CellGrid against hm_cu_dump --------------------------------------------------------------------

// The hm-grid goldens: tests/oracles/hm_cu_dump.cpp output normalised by tests/oracles/normalize/hm_grid.py.
constexpr std::array<std::string_view, 32> kGridGoldens = {
    "jctvc_CAINIT_A_SHARP_4",
    "jctvc_CONFWIN_A_Sony_1",
    "jctvc_EXT_A_ericsson_4",
    "jctvc_FILLER_A_Sony_1",
    "jctvc_HRD_A_Fujitsu_3",
    "jctvc_MAXBINS_A_TI_5",
    "jctvc_MERGE_B_TI_3",
    "jctvc_MVCLIP_A_qualcomm_3",
    "jctvc_MVEDGE_A_qualcomm_3",
    "jctvc_NUT_A_ericsson_5",
    "jctvc_NoOutPrior_A_Qualcomm_1",
    "jctvc_OPFLAG_A_Qualcomm_1",
    "jctvc_PMERGE_A_TI_3",
    "jctvc_POC_A_Bossen_3",
    "jctvc_PPS_A_qualcomm_7",
    "jctvc_RAP_A_docomo_6",
    "jctvc_RAP_B_Bossen_2",
    "jctvc_RPS_A_docomo_5",
    "jctvc_RPS_B_qualcomm_5",
    "jctvc_RPS_C_ericsson_5",
    "jctvc_RPS_D_ericsson_6",
    "jctvc_RPS_E_qualcomm_5",
    "jctvc_RPS_F_docomo_2",
    "jctvc_SAO_A_MediaTek_4",
    "jctvc_SLIST_A_Sony_5",
    "jctvc_STRUCT_B_Samsung_7",
    "jctvc_TSCL_A_VIDYO_5",
    "jctvc_VPSID_A_VIDYO_2",
    "jctvc_WPP_A_ericsson_MAIN10_2",
    "jctvc_WPP_A_ericsson_MAIN_2",
    "jctvc_WP_A_Toshiba_3",
    "jctvc_ipcm_A_NEC_3",
};

// The text of a .gz golden (gzip is on every desktop CI image, as for the vmaf goldens).
std::string gunzip(const std::filesystem::path& path) {
  const std::string cmd = "gzip -dc '" + path.string() + "'";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),  // NOLINT(cert-env33-c): fixed command
                                             pclose);
  REQUIRE(pipe != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe.get())) > 0) {
    text.append(buf.data(), n);
  }
  return text;
}

constexpr int kPredInter = 2;
constexpr int kPredSkip = 3;
constexpr int kPredPcm = 5;
constexpr int kTxNone = 0xFF;
constexpr int kDmChroma = 36;  // HM's DM_CHROMA_IDX, kept raw by hm_cu_dump

constexpr int kNeutralMvMax = 32767;

bool is_mv(const std::string& name) {
  return name.starts_with("mv");
}

// The value the plugin must report for cell i of field `name`, given the golden's raw readings (chapter 06 §6.3.1):
// PCM luma mode 64; chroma DM resolved to the luma mode of the CU's first partition; no transform (0xFF) for skip,
// PCM and inter CUs without a transform tree (rqt_root_cbf = 0, so cbf 0); MVs (already x4) clamped to ±32767
// (chapter 06 §6.2.3).
int expected_cell(const std::string& name, const nlohmann::json& want, const GridOut& got, std::size_t i) {
  const auto w = [&](const char* f) { return want.at(f).at(i).get<int>(); };
  if (name == "intra_mode_y") {
    return w("pred") == kPredPcm ? 64 : w(name.c_str());
  }
  if (name == "intra_mode_c") {
    const int c = w(name.c_str());
    return c == kDmChroma ? want.at("intra_mode_y").at(got.cu_origin[i]).get<int>() : c;
  }
  if (is_mv(name)) {
    return std::clamp(w(name.c_str()), -kNeutralMvMax, kNeutralMvMax);
  }
  if (name == "tx_log2_w" && got.fields.at(name)[i] == kTxNone) {
    const int pred = w("pred");
    if (pred == kPredSkip || pred == kPredPcm || (pred == kPredInter && w("cbf") == 0)) {
      return kTxNone;
    }
  }
  return w(name.c_str());
}

void check_grids(std::string_view id, const StreamResult& r) {
  const auto golden =
      nlohmann::json::parse(gunzip(std::filesystem::path(OVT_HM_GOLDEN_DIR) / (std::string(id) + ".hm-grid.json.gz")));
  const auto& frames = golden.at("frames");
  // The goldens hold the first decoded pictures of each stream, in decode order. Their "frame" counts decoded
  // pictures only, while the plugin's decode index also counts the leading pictures HM skips, so the two are paired
  // in order and checked by POC.
  REQUIRE(!frames.empty());
  REQUIRE(frames.size() <= r.actual.size());
  auto it = r.actual.begin();
  for (const nlohmann::json& g : frames) {
    const auto& [idx, out] = *it++;
    INFO(id << " golden frame " << g.at("frame").get<int>() << ", decode index " << idx);
    REQUIRE(g.at("poc").get<int64_t>() == out.poc);
    REQUIRE(out.grid != nullptr);
    REQUIRE(g.at("w4") == out.grid->w4);
    REQUIRE(g.at("h4") == out.grid->h4);
    bool clamp_needed = false;
    for (const auto& [name, values] : g.at("fields").items()) {
      if (name == "tx_log2_h") {
        continue;  // equal to tx_log2_w on both sides
      }
      REQUIRE(out.grid->fields.contains(name));
      const std::vector<int>& got = out.grid->fields.at(name);
      std::size_t bad = 0;
      std::size_t first = 0;
      int first_want = 0;
      for (std::size_t i = 0; i < values.size(); ++i) {
        clamp_needed = clamp_needed || (is_mv(name) && std::abs(values.at(i).get<int>()) > kNeutralMvMax);
        const int want = expected_cell(name, g.at("fields"), *out.grid, i);
        if (got[i] != want && bad++ == 0) {
          first = i;
          first_want = want;
        }
      }
      INFO(id << " decode index " << idx << " field " << name << ": " << bad << " cells differ, first at cell " << first
              << " (x4 " << first % out.grid->w4 << ", y4 " << first / out.grid->w4 << "): got "
              << (bad == 0 ? 0 : got[first]) << ", want " << first_want);
      CHECK(bad == 0);
    }
    // A clamp is reported once per frame with MDL-GEN-0003 (chapter 06 §6.2.3), and only then.
    CHECK(out.grid->mv_clamp_reported == clamp_needed);
  }
}

}  // namespace

TEST_CASE("hm plugin: CellGrid matches hm_cu_dump on the hm-grid goldens", "[fast][third_party][needs-corpus]") {
  for (const std::string_view id : kGridGoldens) {
    const auto path = ovt::test::corpus_or_skip(id);
    INFO(id);
    const StreamResult r = run_stream(read_file(path), true);
    CHECK(r.violations.empty());
    check_grids(id, r);
  }
}

TEST_CASE("hm plugin: caps and ABI table", "[fast][third_party]") {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  CHECK(api.struct_size == sizeof(ovt_AnalysisDecoderApi));
  CHECK(api.abi_version == OVT_ANALYSIS_ABI_VERSION);
  const ovt_AnalysisCaps* caps = api.caps();
  REQUIRE(caps != nullptr);
  CHECK(caps->struct_size == sizeof(ovt_AnalysisCaps));
  CHECK(caps->codec == 2);  // ovt.fb.Codec.H265
  CHECK(std::string_view(caps->id) == "hm");
  CHECK(std::string_view(caps->license) == "BSD-3-Clause");
  CHECK(caps->plane_stages == kReconMask);
  // Chapter 07 §7.6.4: every grid vector but bits_x8 and deblock bS; partition_id waits for PartitionInfo.
  CHECK(caps->grid_fields == (0xFFFFU & ~(OVT_GRID_BITS_X8 | OVT_GRID_DEBLOCK_BS)));
  CHECK(caps->has_symbol_accounting == 0);
  CHECK(caps->bit_exact == 1);
  CHECK(caps->supports_threads == 0);
}

TEST_CASE("hm plugin: two instances decode interleaved", "[fast][third_party][needs-corpus]") {
  // HM keeps its scan-order and z-scan tables in globals (decoder.cpp): two live instances, fed alternately, must
  // both still match their hashes, and destroying one must leave the other's tables intact.
  const std::filesystem::path a_path = ovt::test::corpus_or_skip("jctvc_WPP_A_ericsson_MAIN10_2");
  const std::filesystem::path b_path = ovt::test::corpus_or_skip("jctvc_SAO_A_MediaTek_4");
  const std::vector<uint8_t> a_bytes = read_file(a_path);
  const std::vector<uint8_t> b_bytes = read_file(b_path);
  const ovt_AnalysisDecoderApi& api = plugin_api();
  StreamResult b_result;
  {
    Instance a(api);
    REQUIRE(a.status == OVT_OK);
    StreamFeeder fa(a, a_bytes);
    {
      Instance b(api);
      REQUIRE(b.status == OVT_OK);
      StreamFeeder fb(b, b_bytes);
      while (!fb.done()) {
        fb.step();
        if (!fa.done()) {
          fa.step();
        }
      }
      b_result = fb.finish();
    }  // b destroyed while a is mid-stream
    while (!fa.done()) {
      fa.step();
    }
    check_stream(fa.finish(), true);
  }
  check_stream(b_result, true);
}
