// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.273 code points (ovt_ColorDescription) → libplacebo pl_color_repr / pl_color_space (chapter 10 §10.3.3,
// WI-1.402). Pure functions; internal to libovt-render (exported only so that the render tests can reach them, as
// output_download.h is). Not a public header.

#ifndef OVT_RENDER_SRC_COLOR_H273_MAP_H
#define OVT_RENDER_SRC_COLOR_H273_MAP_H

#include <libplacebo/colorspace.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "ovt/color.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

enum class H273Field : uint8_t { Primaries, Transfer, Matrix };

// One log line owed by a mapping (§10.10): RND-0020 for a code with no libplacebo mapping, RND-0021 for the
// chromaticity-derived matrices 12 and 13.
struct H273Notice {
  const char* code = "";  // "RND-0020" or "RND-0021"
  H273Field field = H273Field::Primaries;
  uint8_t value = 0;         // the H.273 code met
  const char* assumed = "";  // what it was mapped to, for the message
};

// At most one notice per field.
struct H273Notices {
  std::array<H273Notice, 3> items{};
  size_t count = 0;
  void add(const H273Notice& n) {
    if (count < items.size()) {
      items.at(count++) = n;
    }
  }
};

// The three tables of §10.3.3. `width`/`height` (luma samples) feed libplacebo's guesses for unspecified codes.
// `notices` may be null.
OVT_RENDER_API pl_color_primaries map_primaries(uint8_t code, int width, int height, H273Notices* notices);
OVT_RENDER_API pl_color_transfer map_transfer(uint8_t code, H273Notices* notices);
// `transfer` is the H.273 transfer code: it selects the ICtCp variant for matrix 14.
OVT_RENDER_API pl_color_system map_matrix(uint8_t matrix, uint8_t transfer, int width, int height,
                                          H273Notices* notices);

struct H273Mapping {
  pl_color_repr repr{};    // sys, levels, alpha; bits stays zero (set from the Picture by frame upload, §10.3.1)
  pl_color_space space{};  // primaries, transfer, static HDR metadata (MDCV, CLL)
  H273Notices notices;
};

// The whole of §10.3.3 except Dolby Vision (repr.dovi stays null; §10.3.7 overrides repr.sys when an RPU exists).
OVT_RENDER_API H273Mapping map_color_description(const ovt_ColorDescription& color, int width, int height);

// Hash of every field of the description (padding excluded): the key of the once-per-stream rule.
OVT_RENDER_API uint64_t color_description_hash(const ovt_ColorDescription& color);

// "RND-0020 unknown H.273 code 3 for primaries, assuming BT.709" (message templates of §10.10).
OVT_RENDER_API std::string format_notice(const H273Notice& notice);

// Once-per-stream filter: returns the formatted lines of `mapping.notices` the first time a description with that
// hash is seen, and nothing afterwards.
class OVT_RENDER_API H273NoticeLog {
 public:
  std::vector<std::string> first_seen(const ovt_ColorDescription& color, const H273Mapping& mapping);

 private:
  std::unordered_set<uint64_t> seen_;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_COLOR_H273_MAP_H
