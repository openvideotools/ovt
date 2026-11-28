// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A streaming, reflection-based JSON printer for tables of ovt_container.fbs (chapter 12 §12.1.4, chapter 06
// §6.8.1), used by `ovt probe -f json` (WI-1.503). It writes through JsonWriter, so `--expand all` on a file with
// hundreds of thousands of boxes never builds a DOM or one big string.
// SPEC-ASSUMPTION(#100): stands in for ovt_fb_to_json (§12.3.8), which no C entry point exports yet; the rules are
// those of ovt::model::to_json (every scalar written, enums as names, hash-like [ubyte] fields as hex, integers
// beyond ±2^53 as strings). The container schema has no nested-buffer (`X_type`) fields, so that rule is not needed.

#ifndef OVT_CLI_OUTPUT_FB_JSON_H
#define OVT_CLI_OUTPUT_FB_JSON_H

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <functional>
#include <string_view>

#include "output/json_writer.h"

namespace reflection {
struct Field;
}  // namespace reflection

namespace ovt::fb {
struct BoxNode;
}  // namespace ovt::fb

namespace ovt::cli {

// What of a BoxNode is written: its `fields`, and its `children`. `depth` is 1 for a top-level box.
struct BoxShow {
  bool fields = true;
  bool children = true;
};
using BoxFilter = std::function<BoxShow(const fb::BoxNode& box, int depth)>;

class ContainerJson {
 public:
  // `filter` may be empty (everything is written).
  explicit ContainerJson(BoxFilter filter = {});

  // Writes the table at `table` (a root or nested table of type `type`, e.g. "ovt.fb.TrackInfo") as one JSON object.
  // `box_depth` is the depth of `table` when it is a BoxNode. Throws std::invalid_argument for an unknown type.
  void write(JsonWriter& w, const void* table, std::string_view type, int box_depth = 1) const;

  // Convenience for a finished buffer whose root is `type`.
  void write_root(JsonWriter& w, const std::uint8_t* buf, std::string_view type) const;

 private:
  void write_field(JsonWriter& w, const flatbuffers::Table& t, const reflection::Field& f, int box_depth) const;
  void write_vector(JsonWriter& w, const flatbuffers::Table& t, const reflection::Field& f, int box_depth) const;

  BoxFilter filter_;
};

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_FB_JSON_H
