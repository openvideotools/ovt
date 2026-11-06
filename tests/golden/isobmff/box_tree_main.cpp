// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_golden_box_tree <file.mp4>: prints the `box-tree` canonical JSON (chapter 14 §14.4.0) of an ISOBMFF file.
//
// This is the plumbing proof of WI-0.556 step 4: it does NOT use the OVT container parser (chapter 8, not written
// yet). A minimal box walker builds a fb::ContainerTree, and the same to_canonical() that real golden tests use
// turns it into JSON, so the whole chain FlatBuffer -> canonical JSON -> tolerant compare -> golden file is
// exercised. Once WI-1.2xx lands, the golden test of this file moves to `ovt probe`.

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "ovt_container_generated.h"
#include "test_support/fb_normalize.h"

namespace {

using Bytes = std::vector<uint8_t>;
using BoxOffsets = std::vector<flatbuffers::Offset<ovt::fb::BoxNode>>;

uint32_t be32(const Bytes& b, std::size_t at) {
  return (static_cast<uint32_t>(b[at]) << 24U) | (static_cast<uint32_t>(b[at + 1]) << 16U) |
         (static_cast<uint32_t>(b[at + 2]) << 8U) | static_cast<uint32_t>(b[at + 3]);
}

uint64_t be64(const Bytes& b, std::size_t at) {
  return (static_cast<uint64_t>(be32(b, at)) << 32U) | be32(b, at + 4);
}

std::string fourcc_at(const Bytes& b, std::size_t at) {
  return {b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at) + 4};
}

bool is_container(std::string_view type) {
  return type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" || type == "dinf" ||
         type == "edts" || type == "mvex" || type == "moof" || type == "traf";
}

class Builder {
 public:
  explicit Builder(const Bytes& file) : file_(file) {}

  flatbuffers::Offset<ovt::fb::ContainerTree> tree() {
    const BoxOffsets boxes = walk(0, file_.size());
    const auto root = fbb_.CreateVector(boxes);
    const auto format = fbb_.CreateString("isobmff");
    ovt::fb::ContainerTreeBuilder builder(fbb_);
    builder.add_format(format);
    builder.add_root(root);
    return builder.Finish();
  }

  flatbuffers::FlatBufferBuilder& fbb() { return fbb_; }

 private:
  void add_uint(std::vector<flatbuffers::Offset<ovt::fb::Field>>& fields, const char* name, uint64_t value) {
    const auto n = fbb_.CreateString(name);
    ovt::fb::FieldBuilder f(fbb_);
    f.add_name(n);
    f.add_kind(ovt::fb::SyntaxValueKind::UInt);
    f.add_uvalue(value);
    fields.push_back(f.Finish());
  }

  void add_string(std::vector<flatbuffers::Offset<ovt::fb::Field>>& fields, const char* name,
                  const std::string& value) {
    const auto n = fbb_.CreateString(name);
    const auto v = fbb_.CreateString(value);
    ovt::fb::FieldBuilder f(fbb_);
    f.add_name(n);
    f.add_kind(ovt::fb::SyntaxValueKind::String);
    f.add_svalue(v);
    fields.push_back(f.Finish());
  }

  // Decoded fields of the few box types the fixture has. Offsets are payload offsets (after the header).
  std::vector<flatbuffers::Offset<ovt::fb::Field>> fields_of(const std::string& type, std::size_t payload,
                                                             std::size_t end) {
    std::vector<flatbuffers::Offset<ovt::fb::Field>> fields;
    const bool full = type == "mvhd" || type == "tkhd" || type == "mdhd" || type == "hdlr" || type == "stsz";
    const std::size_t body = full ? payload + 4 : payload;  // after version/flags
    if (full && end >= payload + 4) {
      add_uint(fields, "version", file_[payload]);  // OVT-only extras: the oracle golden does not list them
      add_uint(fields, "flags", be32(file_, payload) & 0xFFFFFFU);
    }
    if (type == "ftyp" && end >= payload + 8) {
      add_string(fields, "major_brand", fourcc_at(file_, payload));
      add_uint(fields, "minor_version", be32(file_, payload + 4));
    } else if (type == "mvhd" && end >= body + 16) {
      add_uint(fields, "timescale", be32(file_, body + 8));
      add_uint(fields, "duration", be32(file_, body + 12));
    } else if (type == "tkhd" && end >= body + 12) {
      add_uint(fields, "track_id", be32(file_, body + 8));
    } else if (type == "mdhd" && end >= body + 16) {
      add_uint(fields, "timescale", be32(file_, body + 8));
      add_uint(fields, "duration", be32(file_, body + 12));
    } else if (type == "hdlr" && end >= body + 12) {
      add_string(fields, "handler_type", fourcc_at(file_, body + 4));
    } else if (type == "stsz" && end >= body + 8) {
      add_uint(fields, "sample_size", be32(file_, body));
      add_uint(fields, "sample_count", be32(file_, body + 4));
    }
    return fields;
  }

  BoxOffsets walk(std::size_t begin, std::size_t end) {
    BoxOffsets boxes;
    std::size_t at = begin;
    while (at + 8 <= end) {
      uint64_t size = be32(file_, at);
      const std::string type = fourcc_at(file_, at + 4);
      std::size_t header = 8;
      if (size == 1 && at + 16 <= end) {
        size = be64(file_, at + 8);
        header = 16;
      } else if (size == 0) {
        size = end - at;
      }
      if (size < header || at + size > end) {
        break;  // truncated or corrupt: stop (the fixture is well formed)
      }
      const std::size_t box_end = at + static_cast<std::size_t>(size);
      BoxOffsets children;
      if (is_container(type)) {
        children = walk(at + header, box_end);
      }
      const auto fields = fields_of(type, at + header, box_end);
      const auto fourcc = fbb_.CreateString(type);
      const auto fields_vec = fbb_.CreateVector(fields);
      const auto children_vec = fbb_.CreateVector(children);
      const ovt::fb::ByteSpan span(at, size);
      ovt::fb::BoxNodeBuilder box(fbb_);
      box.add_fourcc(fourcc);
      box.add_file_span(&span);
      box.add_header_length(static_cast<uint8_t>(header));
      box.add_fields(fields_vec);
      box.add_children(children_vec);
      boxes.push_back(box.Finish());
      at = box_end;
    }
    return boxes;
  }

  const Bytes& file_;
  flatbuffers::FlatBufferBuilder fbb_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ovt_golden_box_tree <file.mp4>\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "ovt_golden_box_tree: cannot open " << argv[1] << "\n";
    return 2;
  }
  const Bytes file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Builder builder(file);
  const auto tree_offset = builder.tree();
  builder.fbb().Finish(tree_offset);
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(builder.fbb().GetBufferPointer());
  std::cout << ovt::test::to_canonical(*tree).dump() << "\n";
  return 0;
}
