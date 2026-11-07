// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.087: the C ABI view types of ovt/decode.h and ovt/color.h (chapter 12 §12.3.9, backbone §0.5.6).

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "ovt/color.h"
#include "ovt/decode.h"

// Layout pins: changing any of these breaks the C ABI (§12.3.1 rule 7: fields are only appended).
static_assert(sizeof(ovt_ColorDescription) == 44, "backbone §0.5.6 fixes sizeof(ovt_ColorDescription) = 44");
static_assert(offsetof(ovt_ColorDescription, mdcv_primaries) == 12);
static_assert(offsetof(ovt_ColorDescription, mdcv_max_luminance) == 28);
static_assert(offsetof(ovt_ColorDescription, max_cll) == 36);
static_assert(offsetof(ovt_ColorDescription, dv_level) == 43);
static_assert(sizeof(ovt_FrameId) == 8);
static_assert(offsetof(ovt_PlaneView, struct_size) == 0);
static_assert(offsetof(ovt_PlaneView, bit_depth) == offsetof(ovt_PlaneView, stride_bytes) + 4);
static_assert(offsetof(ovt_PlaneView, reserved) == offsetof(ovt_PlaneView, bit_depth) + 5);
static_assert(offsetof(ovt_Picture, struct_size) == 0);
static_assert(offsetof(ovt_Picture, planes) == alignof(ovt_PlaneView));
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(ovt_PlaneView) == 40);
static_assert(offsetof(ovt_PlaneView, data) == 8);
static_assert(offsetof(ovt_Picture, chroma_format) == 128);
static_assert(offsetof(ovt_Picture, color) == 132);
static_assert(offsetof(ovt_Picture, id) == 176);
static_assert(offsetof(ovt_Picture, pts) == 184);
static_assert(offsetof(ovt_Picture, crop_x) == 200);
static_assert(offsetof(ovt_Picture, hw_decoded) == 216);
static_assert(offsetof(ovt_Picture, opaque) == 224);
static_assert(sizeof(ovt_Picture) == 232);
#endif

TEST_CASE("ovt/decode.h view types have the chapter 12 layout", "[model][c_api]") {
  // The static_asserts above are the test; this case registers the file with CTest and checks that a
  // zero-initialised picture describes absent planes, as §12.3.9 requires for 4:0:0.
  ovt_Picture picture{};
  picture.struct_size = sizeof(ovt_Picture);
  CHECK(picture.planes[1].struct_size == 0);
  CHECK(picture.planes[2].data == nullptr);
  CHECK(picture.opaque == nullptr);
}
