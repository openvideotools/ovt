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

std::unique_ptr<Reader> make_reader(Format f) {
  return has_reader(f) ? std::make_unique<Reader>() : nullptr;
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

ovt_status make_container(ovt_File* file, Format f, ovt_Container** out) {
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): false positive, the reader moves into the handle
  auto reader = make_reader(f);
  if (!reader) {
    return OVT_E_UNSUPPORTED;
  }
  auto tree = empty_tree(f);
  *out = new ovt_Container{file, f, std::move(reader), std::move(tree)};
  return OVT_OK;
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
  (void)box_offset;
  return container == nullptr ? OVT_E_INVALID_ARG : OVT_E_UNSUPPORTED;  // WI-1.204
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
