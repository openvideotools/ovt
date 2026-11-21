// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Decoded picture hashes per H.265 §D.3.19 (decoded_picture_hash SEI): MD5, CRC and checksum of one decoded
// picture, per colour component, computed over the decoded picture BEFORE cropping (the planes a plugin exports
// at coded size) with the D.3.19 sample packing: a sample of BitDepth <= 8 is one byte, a wider sample two bytes
// little-endian. WI-4.106 (verify mode, chapter 7 §7.6.4) raises VER-0004 from these; WI-4.543 (`ovt verify
// --hash-sei`) and WI-4.109 (hash check during analysis) reuse them. Internal to libovt-core.
//
// The MD5 is OVT's own implementation of RFC 1321 (no third-party code); HM's and libde265's MD5 are not linked.

#ifndef OVT_SRC_DECODE_PICTURE_HASH_HPP
#define OVT_SRC_DECODE_PICTURE_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"

namespace ovt::decode {

/// hash_type of the decoded_picture_hash SEI (D.3.19).
enum class PictureHashKind : std::uint8_t { Md5 = 0, Crc = 1, Checksum = 2 };

/// RFC 1321 MD5, incremental.
class Md5 {
 public:
  Md5() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  /// The digest of everything updated so far; the object must not be updated afterwards.
  [[nodiscard]] std::array<std::uint8_t, 16> finish() noexcept;

 private:
  void block(std::span<const std::uint8_t, 64> chunk) noexcept;
  std::array<std::uint32_t, 4> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
};

/// The hash of one colour component. Only the member named by the kind is meaningful.
struct ComponentHash {
  std::array<std::uint8_t, 16> md5{};
  std::uint16_t crc = 0;
  std::uint32_t checksum = 0;
};

/// The hash of one decoded picture (D.3.19): `num_components` is 1 for 4:0:0, else 3.
struct PictureHash {
  PictureHashKind kind = PictureHashKind::Md5;
  std::uint32_t num_components = 0;
  std::array<ComponentHash, 3> components{};
};

/// Appends the D.3.19 `pictureData` byte sequence of @p plane to @p out: raster order, one byte per sample when
/// plane.bit_depth <= 8, else two bytes little-endian. Samples are read with plane.sample_bytes (1 or 2; the low
/// 16 bits of a 4-byte sample).
void append_picture_data(const ovt::model::PlaneView& plane, std::vector<std::uint8_t>& out);

/// D.3.19 MD5 of one component.
[[nodiscard]] std::array<std::uint8_t, 16> plane_md5(const ovt::model::PlaneView& plane);
/// D.3.19 CRC of one component (CRC-16/CCITT polynomial 0x1021, initial 0xFFFF, two zero bytes appended).
[[nodiscard]] std::uint16_t plane_crc(const ovt::model::PlaneView& plane);
/// D.3.19 checksum of one component (sum of the data bytes xor the (x, y) mask, modulo 2^32).
[[nodiscard]] std::uint32_t plane_checksum(const ovt::model::PlaneView& plane);

/// The picture hash of @p kind over @p planes (1 or 3 PlaneViews, Y then Cb then Cr, at coded size).
/// Throws std::invalid_argument when planes is empty or has more than 3 entries.
[[nodiscard]] PictureHash compute_picture_hash(PictureHashKind kind, std::span<const ovt::model::PlaneView> planes);

/// The D.3.19 kind of a parsed SEI, or std::nullopt for a reserved hash_type.
[[nodiscard]] std::optional<PictureHashKind> kind_of(const ovt::syntax::h265::DecodedPictureHash& sei) noexcept;

/// The hash of a parsed decoded_picture_hash SEI in the PictureHash form. Throws std::invalid_argument for a
/// reserved hash_type.
[[nodiscard]] PictureHash from_sei(const ovt::syntax::h265::DecodedPictureHash& sei);

/// Same kind, same component count and equal hash of every component.
[[nodiscard]] bool operator==(const PictureHash& a, const PictureHash& b) noexcept;
[[nodiscard]] inline bool operator!=(const PictureHash& a, const PictureHash& b) noexcept {
  return !(a == b);
}

/// Text form for VerifyFrame.declared_hash / computed_hash: "md5:<32 hex>/<32 hex>/<32 hex>",
/// "crc:<4 hex>/...", "checksum:<8 hex>/..." with one entry per component, lowercase.
[[nodiscard]] std::string to_string(const PictureHash& hash);

}  // namespace ovt::decode

#endif  // OVT_SRC_DECODE_PICTURE_HASH_HPP
