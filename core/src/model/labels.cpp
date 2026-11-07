// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// label_for (chapter 06 §6.3.1, WI-0.084). Field tables use the generated EnumName* functions where §6.3.1
// numbers a value with a schema enum, and the constexpr string tables below where it does not. Enum-name
// lookups ("AvcMbType", …) read the embedded binary schema (WI-0.083), so every enum of ovt_model.fbs is
// covered without a hand-kept list.

#include "ovt/cxx/model/labels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <flatbuffers/reflection.h>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::model {

namespace {

// HEVC IntraPredModeY / IntraPredModeC, H.265 Table 8-1: 0 INTRA_PLANAR, 1 INTRA_DC, 2..34 INTRA_ANGULAR2..34.
constexpr std::array<std::string_view, 35> kHevcIntraModes = {
    "INTRA_PLANAR",    "INTRA_DC",        "INTRA_ANGULAR2",  "INTRA_ANGULAR3",  "INTRA_ANGULAR4",  "INTRA_ANGULAR5",
    "INTRA_ANGULAR6",  "INTRA_ANGULAR7",  "INTRA_ANGULAR8",  "INTRA_ANGULAR9",  "INTRA_ANGULAR10", "INTRA_ANGULAR11",
    "INTRA_ANGULAR12", "INTRA_ANGULAR13", "INTRA_ANGULAR14", "INTRA_ANGULAR15", "INTRA_ANGULAR16", "INTRA_ANGULAR17",
    "INTRA_ANGULAR18", "INTRA_ANGULAR19", "INTRA_ANGULAR20", "INTRA_ANGULAR21", "INTRA_ANGULAR22", "INTRA_ANGULAR23",
    "INTRA_ANGULAR24", "INTRA_ANGULAR25", "INTRA_ANGULAR26", "INTRA_ANGULAR27", "INTRA_ANGULAR28", "INTRA_ANGULAR29",
    "INTRA_ANGULAR30", "INTRA_ANGULAR31", "INTRA_ANGULAR32", "INTRA_ANGULAR33", "INTRA_ANGULAR34",
};

// §6.3.1 tx_type tables (values 0..3). The H.264 and HEVC numbering is OVT's own; the strings name the
// transform the value stands for.
constexpr std::array<std::string_view, 4> kAvcTxTypes = {"DCT_4x4", "DCT_8x8", "DCT_4x4_DC_HADAMARD", "PCM"};
constexpr std::array<std::string_view, 4> kHevcTxTypes = {"DCT_II", "DST_VII", "TRANSFORM_SKIP", "TRANSQUANT_BYPASS"};

// AV1 cdef_idx 0..7 (§6.3.1 filter_idx); 0xFE stands for cdef_idx = -1 (not signalled).
constexpr std::array<std::string_view, 8> kAv1CdefIdx = {"cdef_idx=0", "cdef_idx=1", "cdef_idx=2", "cdef_idx=3",
                                                         "cdef_idx=4", "cdef_idx=5", "cdef_idx=6", "cdef_idx=7"};
constexpr std::int32_t kAv1CdefNotSignalled = 0xFE;

// intra_mode_y sentinels of §6.3.1.
constexpr std::int32_t kAvcIntra16x16Base = 16;  // 16 + Intra16x16PredMode
constexpr std::int32_t kAvcIntraPcm = 32;
constexpr std::int32_t kHevcIntraPcm = 64;
constexpr std::int32_t kHevcIntraPalette = 65;
constexpr std::int32_t kAv1IntraBc = 32;
constexpr std::int32_t kAv1IntraPalette = 33;
constexpr std::int32_t kAv1UvCfl = 13;  // UV_CFL_PRED

constexpr std::array<std::string_view, 9> kFields = {"pred",        "intra_mode_y", "intra_mode_c",
                                                     "tx_type",     "filter_idx",   "mb_type",
                                                     "sub_mb_type", "part_mode",    "partition"};

template <std::size_t N>
std::string_view from_table(const std::array<std::string_view, N>& table, std::int32_t value) noexcept {
  return (value >= 0 && static_cast<std::size_t>(value) < N) ? table.at(static_cast<std::size_t>(value))
                                                             : std::string_view{};
}

// EnumName<E> for value in [lo, hi]; empty outside (or when the generated function has no name for it).
template <class E, class F>
std::string_view enum_name(F name_of, std::int32_t value, std::int32_t lo, std::int32_t hi) noexcept {
  if (value < lo || value > hi) {
    return {};
  }
  return name_of(static_cast<E>(value));
}

std::string_view intra_mode_y(fb::Codec codec, std::int32_t v) noexcept {
  switch (codec) {
    case fb::Codec::H264:
      if (v >= 0 && v <= static_cast<std::int32_t>(fb::AvcIntra4x4PredMode::Horizontal_Up)) {
        return fb::EnumNameAvcIntra4x4PredMode(static_cast<fb::AvcIntra4x4PredMode>(v));
      }
      if (v >= kAvcIntra16x16Base &&
          v <= kAvcIntra16x16Base + static_cast<std::int32_t>(fb::AvcIntra16x16PredMode::I16_Plane)) {
        return fb::EnumNameAvcIntra16x16PredMode(static_cast<fb::AvcIntra16x16PredMode>(v - kAvcIntra16x16Base));
      }
      return v == kAvcIntraPcm ? "I_PCM" : std::string_view{};
    case fb::Codec::H265:
      if (v == kHevcIntraPcm) {
        return "PCM";
      }
      if (v == kHevcIntraPalette) {
        return "PALETTE";
      }
      return from_table(kHevcIntraModes, v);
    case fb::Codec::AV1:
      if (v == kAv1IntraBc) {
        return "INTRABC";
      }
      if (v == kAv1IntraPalette) {
        return "PALETTE";
      }
      return enum_name<fb::Av1YMode>(fb::EnumNameAv1YMode, v, 0, static_cast<std::int32_t>(fb::Av1YMode::PAETH_PRED));
    default:
      return {};
  }
}

std::string_view intra_mode_c(fb::Codec codec, std::int32_t v) noexcept {
  switch (codec) {
    case fb::Codec::H264:
      return enum_name<fb::AvcIntraChromaPredMode>(fb::EnumNameAvcIntraChromaPredMode, v, 0,
                                                   static_cast<std::int32_t>(fb::AvcIntraChromaPredMode::Chroma_Plane));
    case fb::Codec::H265:
      return from_table(kHevcIntraModes, v);
    case fb::Codec::AV1:
      if (v == kAv1UvCfl) {
        return "UV_CFL_PRED";
      }
      return enum_name<fb::Av1YMode>(fb::EnumNameAv1YMode, v, 0, static_cast<std::int32_t>(fb::Av1YMode::PAETH_PRED));
    default:
      return {};
  }
}

std::string_view tx_type(fb::Codec codec, std::int32_t v) noexcept {
  switch (codec) {
    case fb::Codec::H264:
      return from_table(kAvcTxTypes, v);
    case fb::Codec::H265:
      return from_table(kHevcTxTypes, v);
    case fb::Codec::AV1:
      return enum_name<fb::Av1TxType>(fb::EnumNameAv1TxType, v, 0, static_cast<std::int32_t>(fb::Av1TxType::WHT_WHT));
    default:
      return {};
  }
}

std::string_view filter_idx(fb::Codec codec, std::int32_t v) noexcept {
  switch (codec) {
    case fb::Codec::H265:
      return enum_name<fb::HevcSaoType>(fb::EnumNameHevcSaoType, v, 0,
                                        static_cast<std::int32_t>(fb::HevcSaoType::SAO_EDGE));
    case fb::Codec::AV1:
      return v == kAv1CdefNotSignalled ? std::string_view{"cdef_idx=-1"} : from_table(kAv1CdefIdx, v);
    default:
      return {};  // H.264 stores 0 (§6.3.1): nothing to label
  }
}

// The label of @p value in enum @p name (unqualified) of ovt_model.fbs, from the embedded binary schema.
std::string_view schema_enum_label(std::string_view name, std::int32_t value) {
  const auto bytes = bfbs(SchemaId::Model);
  if (bytes.empty() || name.empty()) {
    return {};
  }
  const reflection::Schema* schema = reflection::GetSchema(bytes.data());
  if (schema == nullptr || schema->enums() == nullptr) {
    return {};
  }
  const std::string qualified = "ovt.fb." + std::string(name);
  const reflection::Enum* e = schema->enums()->LookupByKey(qualified.c_str());
  if (e == nullptr || e->is_union() || e->values() == nullptr) {
    return {};
  }
  const reflection::EnumVal* ev = e->values()->LookupByKey(std::int64_t{value});
  if (ev == nullptr || ev->name() == nullptr) {
    return {};
  }
  return {ev->name()->c_str(), ev->name()->size()};
}

}  // namespace

std::span<const std::string_view> label_fields() noexcept {
  return kFields;
}

std::string_view label_for(std::string_view field, fb::Codec codec, std::int32_t value) {
  if (field == "pred") {
    return enum_name<fb::PredKind>(fb::EnumNamePredKind, value, 0, static_cast<std::int32_t>(fb::PredKind::MAX));
  }
  if (field == "intra_mode_y") {
    return intra_mode_y(codec, value);
  }
  if (field == "intra_mode_c") {
    return intra_mode_c(codec, value);
  }
  if (field == "tx_type") {
    return tx_type(codec, value);
  }
  if (field == "filter_idx") {
    return filter_idx(codec, value);
  }
  // Block.partition_type, per BlockKind (§6.3.1): the unified enums of the schema, valid for one codec each.
  if (field == "mb_type") {
    return codec == fb::Codec::H264 ? schema_enum_label("AvcMbType", value) : std::string_view{};
  }
  if (field == "sub_mb_type") {
    return codec == fb::Codec::H264 ? schema_enum_label("AvcSubMbType", value) : std::string_view{};
  }
  if (field == "part_mode") {
    return codec == fb::Codec::H265 ? schema_enum_label("HevcPartMode", value) : std::string_view{};
  }
  if (field == "partition") {
    return codec == fb::Codec::AV1 ? schema_enum_label("Av1Partition", value) : std::string_view{};
  }
  return schema_enum_label(field, value);
}

}  // namespace ovt::model
