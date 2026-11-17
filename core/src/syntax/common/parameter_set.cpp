// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// parse_parameter_set(): the dispatcher of parameter_set.h (chapter 08 §8.3.3 step 5, WI-1.207) over the H.264
// (WI-1.108, 1.109c, 1.110), H.265 (WI-1.112, 1.114) and AV1 (WI-1.116) parsers.

#include "syntax/common/parameter_set.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/av1/metadata.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h264/sps_ext.hpp"
#include "ovt/cxx/syntax/h264/subset_sps.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax {

struct ParameterSetContext::Impl {
  std::array<std::unique_ptr<h264::Sps>, 32> h264_sps;
  std::array<std::unique_ptr<h265::Vps>, 16> h265_vps;
  std::array<std::unique_ptr<h265::Sps>, 16> h265_sps;
  const h265::Sps* h265_last_sps = nullptr;
};

ParameterSetContext::ParameterSetContext() : impl_(std::make_unique<Impl>()) {}
ParameterSetContext::~ParameterSetContext() = default;
ParameterSetContext::ParameterSetContext(ParameterSetContext&&) noexcept = default;
ParameterSetContext& ParameterSetContext::operator=(ParameterSetContext&&) noexcept = default;

namespace {

template <class T, size_t N>
void keep(std::array<std::unique_ptr<T>, N>& table, uint32_t id, T value) {
  if (id < N) {
    table[id] = std::make_unique<T>(std::move(value));
  }
}

template <class T, size_t N>
std::array<const T*, N> views(const std::array<std::unique_ptr<T>, N>& table) {
  std::array<const T*, N> out{};
  for (size_t i = 0; i < N; ++i) {
    out[i] = table[i].get();
  }
  return out;
}

// Returns true when the unit type has a parser. Throws ParseError from the parsers.
bool parse_h264(BitReader& br, SyntaxBuilder& b, ParameterSetContext::Impl& ps, uint32_t& type) {
  const h264::NalHeader h = h264::parse_nal_header(br, b);
  type = h.nal_unit_type;
  switch (h.nal_unit_type) {
    case 7: {
      h264::Sps sps = h264::parse_sps(br, b);
      const uint32_t id = sps.seq_parameter_set_id;
      keep(ps.h264_sps, id, std::move(sps));
      return true;
    }
    case 8: {
      h264::SpsTable table;
      table.entries = views(ps.h264_sps);
      (void)h264::parse_pps(br, b, table);
      return true;
    }
    case 13:
      (void)h264::parse_sps_extension(br, b);
      return true;
    case 15:
      (void)h264::parse_subset_sps(br, b);
      return true;
    default:
      return false;
  }
}

bool parse_h265(BitReader& br, SyntaxBuilder& b, ParameterSetContext::Impl& ps, uint32_t& type) {
  const h265::NalHeader h = h265::parse_nal_header(br, b);
  type = h.nal_unit_type;
  switch (h.nal_unit_type) {
    case 32: {
      h265::Vps vps = h265::parse_vps(br, b);
      const uint32_t id = vps.vps_video_parameter_set_id;
      keep(ps.h265_vps, id, std::move(vps));
      return true;
    }
    case 33: {
      h265::Sps sps = h265::parse_sps(br, b, h.nuh_layer_id);
      const uint32_t id = sps.sps_seq_parameter_set_id;
      keep(ps.h265_sps, id, std::move(sps));
      if (id < ps.h265_sps.size()) {
        ps.h265_last_sps = ps.h265_sps[id].get();
      }
      return true;
    }
    case 34:
      (void)h265::parse_pps(br, b);
      return true;
    case 39:
    case 40: {
      const auto vps = views(ps.h265_vps);
      const auto sps = views(ps.h265_sps);
      h265::SeiContext sei;
      sei.vps_by_id = vps;
      sei.sps_by_id = sps;
      sei.active_sps = ps.h265_last_sps;
      (void)h265::parse_sei_rbsp(br, b, h, sei);
      return true;
    }
    default:
      return false;
  }
}

bool parse_av1(BitReader& br, SyntaxBuilder& b, uint32_t& type) {
  const av1::ObuHeader h = av1::parse_obu_header(br, b);
  type = h.obu_type;
  switch (h.type()) {
    case av1::ObuType::SequenceHeader:
      (void)av1::parse_sequence_header(br, b);
      return true;
    case av1::ObuType::Metadata:
      (void)av1::parse_metadata(br, b);
      return true;
    default:
      return false;
  }
}

}  // namespace

ParameterSetResult parse_parameter_set(fb::Codec codec, std::span<const uint8_t> bytes, ParameterSetContext* ctx) {
  ParameterSetResult r;
  if (codec != fb::Codec::H264 && codec != fb::Codec::H265 && codec != fb::Codec::AV1) {
    return r;
  }
  ParameterSetContext local;
  ParameterSetContext::Impl& ps = (ctx != nullptr ? *ctx : local).impl();
  Rbsp rbsp;
  std::span<const uint8_t> data = bytes;
  if (codec != fb::Codec::AV1) {
    rbsp = rbsp_from_nal(bytes);
    data = rbsp.rbsp;
    r.epb_positions = std::move(rbsp.epb_positions);
  }
  BitReader br(data);
  SyntaxBuilder b(br);
  try {
    switch (codec) {
      case fb::Codec::H264:
        r.has_parser = parse_h264(br, b, ps, r.unit_type);
        break;
      case fb::Codec::H265:
        r.has_parser = parse_h265(br, b, ps, r.unit_type);
        break;
      default:
        r.has_parser = parse_av1(br, b, r.unit_type);
        break;
    }
    r.complete = r.has_parser;
  } catch (const ParseError& e) {
    b.diag(Severity::Error, e.code, e.what());
    r.complete = false;
  }
  r.tree = b.finish();
  r.tree.set_root_name(codec == fb::Codec::AV1 ? "open_bitstream_unit" : "nal_unit");
  return r;
}

}  // namespace ovt::syntax
