// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_container_* entry points (chapter 12 §12.3.7, chapter 08 §8.8; WI-1.200). open/close/tree/format/size
// work; everything the later work items own returns OVT_E_UNSUPPORTED. No exception leaves these functions
// (chapter 12 §12.3.1 rule 8).

#include "container/container.hpp"

#include <flatbuffers/flatbuffers.h>

#include <memory>
#include <new>
#include <span>

#include "container/ivf.hpp"
#include "container/rawyuv.hpp"
#include "container/y4m.hpp"
#include "io/ovt_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container {

bool has_reader(Format f) noexcept {
  switch (f) {
    case Format::Isobmff:
    case Format::Ivf:
    case Format::Y4m:
    case Format::AnnexB:
    case Format::Obu:
    case Format::Raw:
      return true;
    default:  // phase 4 formats and unknown
      return false;
  }
}

std::unique_ptr<Reader> make_reader(Format f, const io::RandomAccessFile& file, ovt_status* why) {
  if (why != nullptr) {
    *why = has_reader(f) ? OVT_E_CORRUPT : OVT_E_UNSUPPORTED;  // refined by the format readers
  }
  switch (f) {
    case Format::Ivf:
      return ivf::make_reader(file);
    case Format::Y4m:
      return y4m::make_reader(file, why);
    case Format::Isobmff:
    case Format::AnnexB:
    case Format::Obu:
      return std::make_unique<Reader>();  // stub until the format's reader lands
    case Format::Raw:
      if (why != nullptr) {
        *why = OVT_E_INVALID_ARG;  // headerless: the reader needs a format description (rawyuv.hpp)
      }
      return nullptr;
    default:
      return nullptr;
  }
}

namespace {

// Serialises an empty ContainerTree for the format (no root boxes, no tracks).
std::vector<uint8_t> empty_tree(Format f) {
  flatbuffers::FlatBufferBuilder fbb;
  const auto name = fbb.CreateString(std::string(format_name(f)));
  fbb.Finish(ovt::fb::CreateContainerTree(fbb, name));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

ovt_status finish_container(ovt_File* file, Format f, std::unique_ptr<Reader> reader, ovt_Container** out) {
  auto tree = reader->tree();
  if (tree.empty()) {
    tree = empty_tree(f);
  }
  *out = new ovt_Container{file, f, std::move(reader), std::move(tree)};
  return OVT_OK;
}

ovt_status make_container(ovt_File* file, Format f, ovt_Container** out) {
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): false positive, the reader moves into the handle
  ovt_status why = OVT_E_UNSUPPORTED;
  auto reader = make_reader(f, *file->file, &why);
  if (!reader) {
    return why;  // OVT_E_CORRUPT: a reader exists, the bytes are not that format
  }
  return finish_container(file, f, std::move(reader), out);
}

ovt_status status_of_current_exception() noexcept {
  try {
    throw;
  } catch (const io::IoError& e) {
    return e.code;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

}  // namespace
}  // namespace ovt::container

extern "C" {

OVT_API ovt_status ovt_container_open(ovt_File* file, ovt_Container** out) {
  if (out != nullptr) {
    *out = nullptr;
  }
  if (file == nullptr || file->file == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    const auto format = ovt::container::sniff(*file->file);
    if (format == ovt::container::Format::Raw) {
      return OVT_E_INVALID_ARG;  // headerless: the geometry is required (open_format), §8.6.6
    }
    return ovt::container::make_container(file, format, out);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

OVT_API ovt_status ovt_container_open_format(ovt_File* file, const char* format, const char* raw_format,
                                             const char* codec_hint, ovt_Container** out) {
  (void)codec_hint;  // used by the Annex B reader (WI-1.226)
  if (out != nullptr) {
    *out = nullptr;
  }
  if (file == nullptr || file->file == nullptr || out == nullptr || format == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  const auto f = ovt::container::format_from_name(format);
  if (!f) {
    return OVT_E_INVALID_ARG;
  }
  if (*f == ovt::container::Format::Raw && (raw_format == nullptr || *raw_format == '\0')) {
    return OVT_E_INVALID_ARG;
  }
  try {
    if (*f == ovt::container::Format::Raw) {
      // §8.6.6 step 2: the explicit `WxH:pix_fmt[:fps]` string. The sidecar and the heuristics are
      // ovt::container::raw::resolve(), which the CLI and the GUI run before they call this function.
      const auto fmt = ovt::container::raw::parse_format_string(raw_format);
      if (!fmt) {
        return OVT_E_INVALID_ARG;
      }
      return ovt::container::finish_container(file, *f, ovt::container::raw::make_reader(*file->file, *fmt), out);
    }
    return ovt::container::make_container(file, *f, out);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

OVT_API ovt_status ovt_container_open_manifest(const char* url_or_path, const char* variant_selector,
                                               ovt_Container** out) {
  (void)url_or_path;
  (void)variant_selector;
  if (out != nullptr) {
    *out = nullptr;
  }
  return out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // phase 4 (WI-4.210)
}

OVT_API const char* ovt_container_format(const ovt_Container* container) {
  // format_name returns views of string literals, so data() is NUL-terminated.
  return container == nullptr ? "unknown" : ovt::container::format_name(container->format).data();
}

OVT_API ovt_Buffer ovt_container_tree(ovt_Container* container) {
  if (container == nullptr) {
    return ovt_Buffer{nullptr, 0};
  }
  return ovt_Buffer{container->tree.data(), container->tree.size()};
}

OVT_API ovt_status ovt_container_expand(ovt_Container* container, uint64_t box_offset) {
  if (container == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  if (!container->reader) {
    return OVT_E_UNSUPPORTED;
  }
  try {
    const ovt_status st = container->reader->expand(box_offset);  // ISOBMFF: WI-1.204
    if (st == OVT_OK) {
      if (auto tree = container->reader->tree(); !tree.empty()) {
        container->tree = std::move(tree);  // buffers are immutable: a new tree after every expand
      }
    }
    return st;
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

OVT_API ovt_status ovt_container_expand_all(ovt_Container* container, ovt_Job* job) {
  (void)job;
  return container == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.204
}

OVT_API int ovt_container_track_count(const ovt_Container* container) {
  return container == nullptr || !container->reader ? 0 : container->reader->track_count();
}

OVT_API ovt_status ovt_container_track_info(ovt_Container* container, int track_index, ovt_Buffer* out) {
  (void)track_index;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.217
}

OVT_API ovt_status ovt_container_sample_table(ovt_Container* container, int track_index, uint32_t first, uint32_t count,
                                              ovt_Buffer* out) {
  (void)track_index;
  (void)first;
  (void)count;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.2xx
}

OVT_API ovt_status ovt_container_fragments(ovt_Container* container, int track_index, ovt_Job* job, ovt_Buffer* out) {
  (void)track_index;
  (void)job;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_encryption(ovt_Container* container, int track_index, uint64_t first, uint64_t count,
                                            ovt_Buffer* out) {
  (void)track_index;
  (void)first;
  (void)count;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_hdr_metadata(ovt_Container* container, int track_index, ovt_Job* job, ovt_Buffer* out,
                                              ovt_ColorDescription* out_color) {
  (void)track_index;
  (void)job;
  (void)out_color;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_items(ovt_Container* container, ovt_Buffer* out) {
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // phase 4
}

OVT_API ovt_status ovt_container_item_bytes(ovt_Container* container, uint32_t item_id, ovt_Buffer* out) {
  (void)item_id;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // phase 4
}

OVT_API ovt_status ovt_container_validate(ovt_Container* container, ovt_Buffer* out) {
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_list_diagnostics(ovt_Buffer* out) {
  // The DiagnosticRegistry table is not in ovt_container.fbs yet (#138); the data source is all_diagnostics().
  return out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_bytes(ovt_Container* container, uint64_t offset, uint32_t len, ovt_Buffer* out) {
  (void)offset;
  (void)len;
  return container == nullptr || out == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.234
}

OVT_API ovt_status ovt_container_box_at(ovt_Container* container, uint64_t offset,
                                        uint64_t* out_box_offset,  // NOLINT(readability-non-const-parameter): stub
                                        int* out_depth) {          // NOLINT(readability-non-const-parameter): stub
  (void)offset;
  return container == nullptr || out_box_offset == nullptr || out_depth == nullptr ? OVT_E_INVALID_ARG
                                                                                   : OVT_E_UNSUPPORTED;
}

OVT_API ovt_status ovt_container_segment_for_offset(ovt_Container* container, uint64_t offset, ovt_Buffer* out_json) {
  (void)offset;
  return container == nullptr || out_json == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API uint64_t ovt_container_size(const ovt_Container* container) {
  if (container == nullptr || container->file == nullptr || container->file->file == nullptr) {
    return 0;
  }
  return container->file->file->size();
}

OVT_API ovt_status ovt_container_stats_json(ovt_Container* container, ovt_Buffer* out_json) {
  return container == nullptr || out_json == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;
}

OVT_API void ovt_container_close(ovt_Container* container) {
  delete container;
}

}  // extern "C"
