// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture-hash conformance checks of the analysis host (chapter 7 §7.6.4 item 4, §7.9 `VER-0004`, WI-4.109):
// the H.265 decoded_picture_hash SEI (MD5 / CRC / checksum of D.3.19) and the per-frame MD5 side files of AV1
// conformance streams, compared with a plugin's reconstruction. PictureHashCheck plugs into the Session as the
// opt-in FrameCheck (SessionOptions::frame_check); a Session without one never hashes. Internal to libovt-core.
//
// SPEC-ASSUMPTION(#591): the AV1 side file is read as one MD5 per shown frame in output order (the first 32 hex
// digits of each non-empty, non-'#' line), and the AV1 frame hash is the MD5 of the cropped output planes in
// plane order, 8-bit samples as one byte and deeper samples as two bytes little-endian (aomdec --rawvideo).

#ifndef OVT_SRC_ANALYSIS_HASH_CHECK_HPP
#define OVT_SRC_ANALYSIS_HASH_CHECK_HPP

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/picture_store.hpp"
#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "session.hpp"

namespace ovt::analysis {

using Md5Digest = std::array<uint8_t, 16>;

/// RFC 1321 MD5, incremental.
class Md5 {
 public:
  Md5() noexcept;
  void update(std::span<const uint8_t> data) noexcept;
  [[nodiscard]] Md5Digest finish() noexcept;  // the object must not be used afterwards

 private:
  void block(std::span<const uint8_t, 64> p) noexcept;
  std::array<uint32_t, 4> state_{};
  std::array<uint8_t, 64> buffer_{};
  uint64_t total_ = 0;
};

/// The three D.3.19 hashes of one plane, over the samples as the packing rule lays them out (a sample of a plane
/// with bit_depth <= 8 is one byte, a deeper one two bytes little-endian). `plane` must be U8 or U16LE.
[[nodiscard]] Md5Digest plane_md5(const ovt::model::PlaneView& plane);
[[nodiscard]] uint32_t plane_crc(const ovt::model::PlaneView& plane);
[[nodiscard]] uint32_t plane_checksum(const ovt::model::PlaneView& plane);

enum class HashStatus : uint8_t {
  Match,
  Mismatch,  // `message` is the VER-0004 text
  Skipped,   // nothing was compared; `message` says why (reserved hash_type, cropped or missing planes)
};

struct HashCheckResult {
  HashStatus status = HashStatus::Skipped;
  std::array<bool, 3> component_mismatch{};  // H.265 only: the colour components whose hash differs
  std::string message;
};

/// Compares a decoded_picture_hash SEI with the reconstruction `planes` (Y, then Cb, Cr unless
/// hash.num_components == 1). The hash covers the picture before cropping, so a luma plane whose size is not
/// pic_width x pic_height gives Skipped, never a false mismatch.
[[nodiscard]] HashCheckResult check_decoded_picture_hash(const ovt::syntax::h265::DecodedPictureHash& hash,
                                                         std::span<const ovt::model::PlaneView> planes,
                                                         uint32_t pic_width, uint32_t pic_height,
                                                         std::string_view plugin_id);

/// AV1: MD5 of all planes of one output frame (see SPEC-ASSUMPTION above).
[[nodiscard]] Md5Digest frame_md5(std::span<const ovt::model::PlaneView> planes);
[[nodiscard]] HashCheckResult check_frame_md5(const Md5Digest& expected, std::span<const ovt::model::PlaneView> planes,
                                              std::string_view plugin_id);

/// Reads an AV1 MD5 side file; lines that are blank, start with '#', or do not begin with 32 hex digits are
/// ignored. Throws nothing.
[[nodiscard]] std::vector<Md5Digest> parse_md5_side_file(std::string_view text);

/// FrameCheck that looks the target frame's Reconstruction planes up in a PictureStore and compares them with
/// the expectation registered for its decode index. A frame with no expectation, no Reconstruction PlaneRef or
/// no stored plane is not checked and raises nothing.
class PictureHashCheck final : public FrameCheck {
 public:
  PictureHashCheck(ovt::model::PictureStore& store, std::string plugin_id);

  /// H.265: the frame's decoded_picture_hash SEI and the SPS picture size (before cropping).
  void expect_h265(int decode_index, const ovt::syntax::h265::DecodedPictureHash& hash, uint32_t pic_width,
                   uint32_t pic_height);
  /// AV1: the entry of the side file for this frame.
  void expect_md5(int decode_index, const Md5Digest& digest);

  void check(int decode_index, const ovt::fb::FrameAnalysis& frame, std::vector<HostDiagnostic>& out) override;

 private:
  struct H265Expectation {
    ovt::syntax::h265::DecodedPictureHash hash;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  ovt::model::PictureStore& store_;
  std::string plugin_id_;
  std::map<int, H265Expectation> h265_;
  std::map<int, Md5Digest> md5_;
};

}  // namespace ovt::analysis

#endif  // OVT_SRC_ANALYSIS_HASH_CHECK_HPP
