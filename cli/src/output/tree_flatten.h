// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The text tree and the flattened CSV of a ContainerTree box list (chapter 12 §12.1.4 and §12.2.2, WI-1.503).
//
// Text: one line per box, `fourcc @offset +size` plus `  vN f=0x…` for a FullBox, indented two spaces per depth; one
// line per decoded field, `name = value ("derived") (bits a..b, spec_ref)`, one level deeper; box diagnostics as
// `! Severity CODE: message`. The bit range is relative to the box start (Field.span, §0.5.5).
// CSV: the §12.1.4 tree columns `depth,path,name,descriptor,value,bit_offset,bit_length,spec_ref`. A box row has the
// descriptor `box`, its size as value and its absolute file span in bits; a field row has its SyntaxValueKind name as
// descriptor and the box-relative span. `path` joins names with `/`; a box that has same-fourcc siblings is written
// `fourcc[i]` (i counts those siblings from 0); array fields are already named `name[i]` by the decoders.

#ifndef OVT_CLI_OUTPUT_TREE_FLATTEN_H
#define OVT_CLI_OUTPUT_TREE_FLATTEN_H

#include <flatbuffers/flatbuffers.h>

#include <string>
#include <vector>

#include "output/csv_writer.h"
#include "output/fb_json.h"
#include "ovt_container_generated.h"

namespace ovt::cli {

using BoxList = std::vector<const fb::BoxNode*>;

// The text tree of `boxes` (top-level, depth 1) under `filter` (empty = everything).
[[nodiscard]] std::string render_box_tree(const BoxList& boxes, const BoxFilter& filter);

// Writes the header row and one row per box and field of `boxes` under `filter`.
void flatten_box_tree(CsvWriter& csv, const BoxList& boxes, const BoxFilter& filter);

// The text of one field value: the number, the quoted string or the hex bytes (text form shortens long bytes).
[[nodiscard]] std::string field_value(const fb::Field& f, bool for_text);

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_TREE_FLATTEN_H
