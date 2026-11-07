// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Plane views, pictures and plane blobs (chapter 06 §6.6.1, WI-0.087). The C ABI mirror of PlaneView and
// Picture is ovt_PlaneView / ovt_Picture in ovt/decode.h.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_PLANE_HPP
#define OVT_CXX_MODEL_PLANE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ovt/color.h"
#include "ovt_model_generated.h"

namespace ovt::model {

/// Sample encoding of a plane (§6.6.1 rule 2). Little-endian whatever the host.
enum class SampleFormat : std::uint8_t { U8 = 0, U16LE = 1, S16LE = 2, S32LE = 3 };

/// `PlaneView.stage` value of a metric map (§6.6.1); the other values are ovt.fb.PlaneStage.
inline constexpr std::uint8_t kStageMetricMap = 0xFE;

/// Row alignment of plane memory owned by the store, in bytes (§6.6.1, WI-0.087 step 1).
inline constexpr std::uint32_t kPlaneAlignment = 64;

/// A read-only view of one plane (§6.6.1). Does not own its memory.
struct PlaneView {
  const std::uint8_t* data = nullptr;  ///< First byte of row 0; 64-byte aligned when copied into the store.
  std::uint32_t width = 0;             ///< In samples of THIS plane (chroma planes already subsampled).
  std::uint32_t height = 0;            ///< Rows of this plane.
  std::uint32_t stride_bytes = 0;      ///< >= width * sample_bytes; a multiple of 64 when copied by the store.
  std::uint8_t bit_depth = 8;          ///< 8..16 for samples; 16 for S16LE residual; 32 for S32LE coefficients.
  std::uint8_t sample_bytes = 1;       ///< 1, 2 or 4; must match `format`.
  SampleFormat format = SampleFormat::U8;
  std::uint8_t plane = 0;  ///< 0 Y, 1 U, 2 V (3 = alpha, future).
  std::uint8_t stage = 0;  ///< ovt.fb.PlaneStage, or kStageMetricMap.
};

/// What ovt_decoder_next and ovt_analysis_plane hand out (§6.6.1).
struct Picture {
  ovt::fb::FrameId id;
  ovt_ColorDescription color{};
  PlaneView planes[3];             ///< planes[1..2] have width = height = 0 for 4:0:0. NOLINT(*-avoid-c-arrays): §6.6.1
  std::uint8_t chroma_format = 1;  ///< 0..3
  ovt::fb::Rect crop;
  std::shared_ptr<const void> owner;  ///< Keeps the backing memory alive (store entry or decoder frame).
};

/// Thrown when a request cannot fit in the store's hard memory cap; the C API maps it to OVT_E_NOMEM
/// (§6.6.3, WI-0.087 acceptance test 3).
struct NoMemError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Bytes per sample of @p format.
[[nodiscard]] constexpr std::uint8_t sample_bytes_of(SampleFormat format) noexcept {
  switch (format) {
    case SampleFormat::U8:
      return 1;
    case SampleFormat::U16LE:
    case SampleFormat::S16LE:
      return 2;
    case SampleFormat::S32LE:
      return 4;
  }
  return 0;
}

/// `round_up(width * sample_bytes, 64)` (WI-0.087 step 1). Throws std::invalid_argument when the result does
/// not fit in 32 bits.
[[nodiscard]] std::uint32_t aligned_stride(std::uint32_t width, std::uint8_t sample_bytes);

/// Throws std::invalid_argument unless @p view is well formed: sample_bytes matches format, bit_depth fits
/// the format (1..8 for U8, 1..16 for 16-bit, 1..32 for S32LE), stride_bytes >= width * sample_bytes, and
/// data is non-null when the plane is not empty.
void check_plane_view(const PlaneView& view);

// SPEC-ASSUMPTION(#157): WI-0.087 names PlaneBlob without defining it; this is the definition.
/// One immutable plane held by the PictureStore, keyed by its blob_id (§6.6.2). Shared by every holder of a
/// std::shared_ptr to it; the memory is freed with the last one, also after the store evicted it (§6.6.3).
class PlaneBlob {
 public:
  /// Copies @p src into new 64-byte aligned memory with stride aligned_stride(width, sample_bytes); row
  /// padding is zeroed. Throws std::invalid_argument (check_plane_view) or std::bad_alloc.
  [[nodiscard]] static std::shared_ptr<const PlaneBlob> copy_of(std::string blob_id, const PlaneView& src);

  /// Takes @p memory without copying (zero-copy). @p src.data must equal @p memory.get() unless the plane is
  /// empty; @p src's stride is kept as is, and memory is released with delete[]. Throws
  /// std::invalid_argument otherwise.
  [[nodiscard]] static std::shared_ptr<const PlaneBlob> adopt(
      std::string blob_id, const PlaneView& src,
      std::unique_ptr<std::uint8_t[]> memory);  // NOLINT(*-avoid-c-arrays): WI-0.087 interface

  PlaneBlob(const PlaneBlob&) = delete;
  PlaneBlob& operator=(const PlaneBlob&) = delete;
  PlaneBlob(PlaneBlob&&) = delete;
  PlaneBlob& operator=(PlaneBlob&&) = delete;
  ~PlaneBlob();

  [[nodiscard]] const std::string& blob_id() const noexcept { return blob_id_; }
  /// The plane; `view().data` points into memory this blob owns.
  [[nodiscard]] const PlaneView& view() const noexcept { return view_; }
  /// Bytes this blob accounts for in the store: stride_bytes * height.
  [[nodiscard]] std::size_t size_bytes() const noexcept { return size_bytes_; }

 private:
  struct Token {
    explicit Token() = default;
  };

 public:
  /// Use copy_of() or adopt(); the token keeps this constructor private while allowing make_shared.
  PlaneBlob(Token /*unused*/, std::string blob_id, const PlaneView& view, std::uint8_t* memory, bool aligned,
            std::size_t size_bytes) noexcept;

 private:
  std::string blob_id_;
  PlaneView view_;
  std::uint8_t* memory_;
  bool aligned_;  // true: ::operator new(align_val_t{64}); false: new[] (adopted)
  std::size_t size_bytes_;
};

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_PLANE_HPP
