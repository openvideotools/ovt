// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 active_parameter_sets() (D.2.21) and decoded_picture_hash() (D.2.19): chapter 7 §7.3.5 row 14, WI-1.114.
// The picture hash is stored in DecodedPictureHash for the verify job (§7.6.4).

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "syntax/h265/h265_internal.h"
#include "syntax/h265/sei_internal.h"

namespace ovt::syntax::h265::detail {

namespace {

std::string_view hash_type_name(uint32_t hash_type) noexcept {
  switch (hash_type) {
    case 0:
      return "MD5";
    case 1:
      return "CRC";
    case 2:
      return "checksum";
    default:
      return "reserved";
  }
}

uint32_t read_layer_sps_idx(SeiPayloadReader& r, uint32_t i, uint32_t num_sps_ids_minus1) {
  const uint32_t idx = sei_ue_at(r, "layer_sps_idx", static_cast<int>(i));
  (void)check_range(r.builder(), "layer_sps_idx", idx, 0, num_sps_ids_minus1, "H.265 F.14.3.6");
  return idx;
}

}  // namespace

ActiveParameterSets parse_active_parameter_sets(SeiPayloadReader& r, std::span<const Vps* const> vps_by_id) {
  ActiveParameterSets a;
  a.active_video_parameter_set_id = static_cast<uint32_t>(r.u("active_video_parameter_set_id", 4));
  a.self_contained_cvs_flag = r.flag("self_contained_cvs_flag");
  a.no_parameter_set_update_flag = r.flag("no_parameter_set_update_flag");
  a.num_sps_ids_minus1 = r.ue("num_sps_ids_minus1");
  require_range(r.reader(), "num_sps_ids_minus1", a.num_sps_ids_minus1, 0, 15);
  for (uint32_t i = 0; i <= a.num_sps_ids_minus1; ++i) {
    const uint32_t id = sei_ue_at(r, "active_seq_parameter_set_id", static_cast<int>(i));
    (void)check_range(r.builder(), "active_seq_parameter_set_id", id, 0, 15, "H.265 D.3.21");
    a.active_seq_parameter_set_id.push_back(id);
  }
  // for (i = vps_base_layer_internal_flag; i <= MaxLayersMinus1; i++) layer_sps_idx[i], with
  // MaxLayersMinus1 = Min(62, vps_max_layers_minus1) of the named VPS.
  const Vps* vps =
      a.active_video_parameter_set_id < vps_by_id.size() ? vps_by_id[a.active_video_parameter_set_id] : nullptr;
  a.vps_known = vps != nullptr;
  if (vps != nullptr) {
    a.first_layer = vps->vps_base_layer_internal_flag ? 1 : 0;
    const uint32_t max_layers_minus1 = std::min<uint32_t>(62, vps->vps_max_layers_minus1);
    for (uint32_t i = a.first_layer; i <= max_layers_minus1; ++i) {
      a.layer_sps_idx.push_back(read_layer_sps_idx(r, i, a.num_sps_ids_minus1));
    }
  } else if (payload_extension_present(r)) {
    // Without that VPS the loop cannot be sized: read layer_sps_idx while the payload holds more than its trailing
    // bits, indexed from 0, so that the values are not lost.
    r.builder().diag(Severity::Warning, kMissingSetCode,
                     "active_parameter_sets SEI without its VPS; layer_sps_idx count taken from the payload",
                     "H.265 D.2.21");
    for (uint32_t i = 0; i <= 62 && payload_extension_present(r); ++i) {
      a.layer_sps_idx.push_back(read_layer_sps_idx(r, i, a.num_sps_ids_minus1));
    }
  }
  return a;
}

DecodedPictureHash parse_decoded_picture_hash(SeiPayloadReader& r, const Sps& sps) {
  DecodedPictureHash h;
  SyntaxBuilder& b = r.builder();
  h.hash_type = static_cast<uint32_t>(r.u("hash_type", 8));
  h.hash_type_name = hash_type_name(h.hash_type);
  b.set_enum(h.hash_type_name);
  h.num_components = sps.chroma_format_idc == 0 ? 1 : 3;
  if (h.hash_type > 2) {
    // Reserved (D.3.19): the component loop reads nothing, so the bytes become reserved_payload_extension_data
    // with the trailing-bytes Diagnostic of the framing.
    return h;
  }
  for (uint32_t c = 0; c < h.num_components; ++c) {
    const auto idx = static_cast<int>(c);
    switch (h.hash_type) {
      case 0: {
        const SeiArrayElement e(b, "picture_md5", idx);
        const std::span<const uint8_t> md5 = r.bytes("picture_md5", 16);
        std::copy(md5.begin(), md5.end(), h.picture_md5[c].begin());
        break;
      }
      case 1: {
        const SeiArrayElement e(b, "picture_crc", idx);
        h.picture_crc[c] = static_cast<uint32_t>(r.u("picture_crc", 16));
        break;
      }
      default: {
        const SeiArrayElement e(b, "picture_checksum", idx);
        h.picture_checksum[c] = static_cast<uint32_t>(r.u("picture_checksum", 32));
        break;
      }
    }
  }
  return h;
}

}  // namespace ovt::syntax::h265::detail
