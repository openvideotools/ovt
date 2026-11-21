// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt_container_* entry points (chapter 12 §12.3.7, chapter 08 §8.8; WI-1.200). open/close/tree/format/size
// work; everything the later work items own returns OVT_E_UNSUPPORTED. No exception leaves these functions
// (chapter 12 §12.3.1 rule 8).

#include "container/container.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "api/buffer.hpp"
#include "container/diagnostics.h"
#include "container/isobmff/lazy.h"
#include "container/ivf.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "container/mxf_placeholder.hpp"
#include "container/rawyuv.hpp"
#include "container/y4m.hpp"
#include "io/ovt_file.hpp"
#include "jobs/job_internal.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
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
    case Format::Mpegts:
    case Format::Mxf:  // libavformat-backed placeholder (WI-4.211)
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
    case Format::Mpegts:
      return mpegts::make_reader(file);
    case Format::Y4m:
      return y4m::make_reader(file, why);
    case Format::Mxf:
      return mxf::make_reader(file, why);
    case Format::Isobmff:
      return isobmff::make_reader(file, why);
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
  *out = new ovt_Container{file, f, std::move(reader), std::move(tree), {}, false};
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

}  // namespace

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
  try {
    if (*f == ovt::container::Format::Raw && raw_format == nullptr) {
      // §8.6.6 step 2: NULL runs the sidecar, then the heuristics, so a sidecar's `header_bytes` reaches the C API.
      // Other than exactly one candidate is CNT-RAW-0002 and OVT_E_INVALID_ARG; the ranked list is
      // ovt::container::raw::resolve()'s, which the CLI and the GUI call.
      const auto res = ovt::container::raw::resolve(*file->file);
      if (!res.format) {
        return res.status == OVT_OK ? OVT_E_INVALID_ARG : res.status;
      }
      return ovt::container::finish_container(file, *f, ovt::container::raw::make_reader(*file->file, *res.format),
                                              out);
    }
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
  try {
    const std::lock_guard lock(container->bytes.tree_mutex());
    ovt::container::refresh_tree(*container);
    return ovt_Buffer{container->tree.data(), container->tree.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};  // only std::bad_alloc while serialising
  }
}

OVT_API ovt_status ovt_container_expand(ovt_Container* container, uint64_t box_offset) {
  if (container == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    return ovt::container::expand_box(*container, box_offset);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
}

OVT_API ovt_status ovt_container_expand_all(ovt_Container* container, ovt_Job* job) {
  if (container == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  if (!container->reader) {
    return OVT_E_UNSUPPORTED;
  }
  // jobs::run maps ovt::Cancelled to OVT_E_CANCELLED and std::bad_alloc to OVT_E_NOMEM; the reader's own statuses
  // keep their codes.
  return ovt::jobs::run(job, "expand", [&](ovt::jobs::Cancel& cancel, const ovt::jobs::Pool::ProgressSink& sink) {
    return ovt::container::expand_all(*container, cancel, sink);
  });
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
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  try {
    // One DiagnosticRegistry entry per diag_codes.inc row, in file order (WI-1.235 step 2, #763). The message is
    // the template and `span` stays unset: the registry describes codes, not findings. No file identifier (§6.1.1).
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> entries;
    const std::span<const ovt::container::DiagInfo> all = ovt::container::all_diagnostics();
    entries.reserve(all.size());
    for (const ovt::container::DiagInfo& d : all) {
      const auto code = fbb.CreateString(d.code.data(), d.code.size());
      const auto message = fbb.CreateString(d.message.data(), d.message.size());
      const auto spec_ref = fbb.CreateString(d.spec_ref.data(), d.spec_ref.size());
      ovt::fb::DiagnosticBuilder b(fbb);
      b.add_severity(static_cast<ovt::fb::Severity>(d.severity));
      b.add_code(code);
      b.add_message(message);
      b.add_spec_ref(spec_ref);
      entries.push_back(b.Finish());
    }
    const auto family = fbb.CreateString("CNT");
    const auto vec = fbb.CreateVector(entries);
    fbb.Finish(ovt::fb::CreateDiagnosticRegistry(fbb, family, vec));
    return ovt::api::make_buffer(fbb.GetBufferPointer(), fbb.GetSize(), out);
  } catch (...) {
    return ovt::container::status_of_current_exception();
  }
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

OVT_API void ovt_container_close(ovt_Container* container) {
  delete container;
}

}  // extern "C"

namespace ovt::container {

ovt_status expand_box(ovt_Container& c, uint64_t box_offset) {
  if (!c.reader) {
    return OVT_E_UNSUPPORTED;
  }
  // Held across the reader's expand: ovt_container_box_at (thread-safe, §8.8) may serialise the reader's tree.
  const std::lock_guard lock(c.bytes.tree_mutex());
  const std::optional<uint64_t> before = c.reader->tree_revision();
  const ovt_status st = c.reader->expand(box_offset);  // ISOBMFF: WI-1.204
  // The tree is re-serialised on its next read, not here: a run of expands (fuzz_isobmff_tree expands every box)
  // re-serialised the whole tree each time, quadratic in the number of boxes. A no-op expand leaves it as it is.
  if (st == OVT_OK && (!before || c.reader->tree_revision() != before)) {
    c.tree_stale = true;
  }
  return st;
}

namespace {

// True when `n` carries the CNT-0007 "not yet expanded" diagnostic.
bool is_unexpanded(const fb::BoxNode& n) {
  if (n.diagnostics() == nullptr) {
    return false;
  }
  return std::ranges::any_of(*n.diagnostics(), [](const fb::Diagnostic* d) {
    return d->code() != nullptr && d->code()->string_view() == "CNT-0007";
  });
}

// Offsets of every box of the current tree that is still unexpanded (carries CNT-0007), `mdat` excluded: its payload is
// never walked (§8.2.4). Parents come before children; the walk is iterative so a deep tree cannot exhaust the stack.
std::vector<uint64_t> unexpanded_offsets(ovt_Container& c) {
  using Nodes = flatbuffers::Vector<flatbuffers::Offset<fb::BoxNode>>;
  std::vector<uint64_t> out;
  const std::lock_guard lock(c.bytes.tree_mutex());
  refresh_tree(c);
  if (c.tree.empty()) {
    return out;
  }
  const auto* tree = flatbuffers::GetRoot<fb::ContainerTree>(c.tree.data());
  std::vector<const Nodes*> stack;
  if (tree->root() != nullptr) {
    stack.push_back(tree->root());
  }
  while (!stack.empty()) {
    const Nodes* nodes = stack.back();
    stack.pop_back();
    for (const fb::BoxNode* n : *nodes) {
      if (n->fourcc() != nullptr && n->fourcc()->string_view() == "mdat") {
        continue;
      }
      if (n->file_span() != nullptr && is_unexpanded(*n)) {
        out.push_back(n->file_span()->offset());
      }
      if (n->children() != nullptr) {
        stack.push_back(n->children());
      }
    }
  }
  return out;
}

}  // namespace

ovt_status expand_all(ovt_Container& c, jobs::Cancel& cancel, const jobs::Pool::ProgressSink& sink) {
  // Expanding a box can reveal new unexpanded boxes (a lazy IVF/Y4M root materialises its children in batches), so the
  // walk repeats until a pass finds none. A pass that leaves the tree as it was (a reader that cannot expand, or one
  // that keeps the marker) ends the loop instead of spinning.
  uint64_t done = 0;
  std::vector<uint64_t> previous;
  for (;;) {
    const std::vector<uint64_t> todo = unexpanded_offsets(c);
    if (todo.empty() || todo == previous) {
      return OVT_OK;
    }
    for (const uint64_t offset : todo) {
      cancel.throw_if_requested();  // one box per check: a single expand is far below 50 ms
      const ovt_status st = expand_box(c, offset);
      if (st != OVT_OK) {
        return st;
      }
      sink(jobs::Progress{++done, 0, "expand"});
    }
    previous = todo;
  }
}

void refresh_tree(ovt_Container& c) {
  if (!c.tree_stale) {
    return;
  }
  if (auto tree = c.reader->tree(); !tree.empty()) {
    c.tree = std::move(tree);  // buffers are immutable: a new one after every change
  }
  c.tree_stale = false;
}

}  // namespace ovt::container
