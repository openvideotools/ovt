// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the ISOBMFF tree (chapter 08 §8.2.3, §8.2.4 and §8.9.4, WI-1.204). The whole input is the file, read
// through an in-memory RandomAccessFile: ovt_container_open, ovt_container_tree, then ovt_container_expand on every
// box of the tree. Invariants, each an abort when violated: no exception escapes; open returns a documented status;
// every tree buffer verifies; every TrackInfo buffer verifies; expand of an offset taken from the tree returns OVT_OK;
// box spans stay inside the file; an expansion that completes leaves a tree that still verifies; the validation driver
// (WI-1.224) runs over a fresh walk of the input without throwing, and a second run adds nothing; so do the brand
// checks (WI-1.222). The number of expansions is capped so one input stays fast. Every track's SampleTable,
// FragmentTimeline and HdrMetadata verify.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/brands.h"
#include "container/isobmff/edit_list.h"
#include "container/isobmff/sample_table.h"
#include "container/isobmff/validate.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"

namespace {

constexpr size_t kMaxExpansions = 512;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

const ovt::fb::ContainerTree* verified_tree(ovt_Container* c) {
  const ovt_Buffer b = ovt_container_tree(c);
  require(b.data != nullptr && b.size > 0);
  flatbuffers::Verifier v(b.data, b.size);
  require(v.VerifyBuffer<ovt::fb::ContainerTree>(nullptr));
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data);
}

// Offsets of every box, parents before children (a bounded depth keeps this iterative walk small).
void collect(const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::BoxNode>>* nodes, uint64_t file_size,
             std::vector<uint64_t>& out) {
  if (nodes == nullptr) {
    return;
  }
  for (const auto* n : *nodes) {
    require(n->file_span() != nullptr);
    require(n->file_span()->offset() <= file_size && n->file_span()->length() <= file_size - n->file_span()->offset());
    if (out.size() < kMaxExpansions) {
      out.push_back(n->file_span()->offset());
    }
    collect(n->children(), file_size, out);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    ovt_File handle{std::make_unique<ovt::io::MemoryFile>(std::span<const uint8_t>(data, size))};
    ovt_Container* c = nullptr;
    const ovt_status st = ovt_container_open(&handle, &c);
    if (st != OVT_OK) {
      require(c == nullptr && (st == OVT_E_UNSUPPORTED || st == OVT_E_CORRUPT || st == OVT_E_INVALID_ARG));
      return 0;
    }
    std::vector<uint64_t> offsets;
    collect(verified_tree(c)->root(), size, offsets);
    for (const uint64_t off : offsets) {
      require(ovt_container_expand(c, off) == OVT_OK);
    }
    // Expansion may add boxes (a lazy `moof`); every one of those expands too, and the tree stays valid.
    std::vector<uint64_t> after;
    collect(verified_tree(c)->root(), size, after);
    for (const uint64_t off : after) {
      require(ovt_container_expand(c, off) == OVT_OK);
    }
    // WI-1.217: every track of the tree is also served as its own verified TrackInfo buffer.
    const auto* tracks = verified_tree(c)->tracks();
    const int count = ovt_container_track_count(c);
    require(count == (tracks == nullptr ? 0 : static_cast<int>(tracks->size())));
    for (int i = 0; i < count; ++i) {
      ovt_Buffer b{};
      require(ovt_container_track_info(c, i, &b) == OVT_OK);
      flatbuffers::Verifier v(b.data, b.size);
      require(v.VerifyBuffer<ovt::fb::TrackInfo>(nullptr));
      delete[] b.data;  // §12.3.1 rule 3a: new[]-allocated; the fuzz targets do not link status.cpp
      // WI-1.218: the moov SampleTable of every track (whole, then a slice) is a verified buffer too.
      for (const uint32_t first : {0U, 3U}) {
        ovt_Buffer table{};
        require(ovt_container_sample_table(c, i, first, first == 0 ? 0 : 5, &table) == OVT_OK);
        flatbuffers::Verifier sv(table.data, table.size);
        require(sv.VerifyBuffer<ovt::fb::SampleTable>(nullptr));
        delete[] table.data;
      }
      // WI-1.219c: the FragmentTimeline of every track is a verified buffer.
      ovt_Buffer timeline{};
      require(ovt_container_fragments(c, i, nullptr, &timeline) == OVT_OK);
      flatbuffers::Verifier fv(timeline.data, timeline.size);
      require(fv.VerifyBuffer<ovt::fb::FragmentTimeline>(nullptr));
      delete[] timeline.data;
      // WI-1.223: the HdrMetadata of every track (box level and sync-sample probe) verifies, with a 44-byte image.
      ovt_Buffer hdr{};
      ovt_ColorDescription color{};
      require(ovt_container_hdr_metadata(c, i, nullptr, &hdr, &color) == OVT_OK);
      flatbuffers::Verifier hv(hdr.data, hdr.size);
      require(hv.VerifyBuffer<ovt::fb::HdrMetadata>(nullptr));
      require(color.struct_size == sizeof(ovt_ColorDescription));
      delete[] hdr.data;
    }
    ovt_container_close(c);
    // The validation driver over the complete walk: idempotent, whatever the bytes.
    const ovt::io::MemoryFile file{std::span<const uint8_t>(data, size)};
    auto walked = ovt::container::isobmff::walk(file);
    using ovt::container::isobmff::ValidationLevel;
    ovt::container::isobmff::validate(walked.top_level, ValidationLevel::Standard);
    require(ovt::container::isobmff::validate(walked.top_level, ValidationLevel::Deep) == 0);
    // WI-1.220: the EditList of every track verifies, and presenting any sample never throws.
    namespace iso = ovt::container::isobmff;
    for (size_t t = 0; iso::find_trak(walked.top_level, t) != nullptr; ++t) {
      const auto table =
          iso::expand_sample_table(file, *iso::find_trak(walked.top_level, t), ovt::jobs::Cancel::never());
      const iso::EditListBuild built =
          iso::build_edit_list(file, walked.top_level, t, *table, ovt::jobs::Cancel::never());
      const flatbuffers::DetachedBuffer buf = iso::serialise(*built.list);
      flatbuffers::Verifier lv(buf.data(), buf.size());
      require(lv.VerifyBuffer<ovt::fb::EditList>(nullptr));
      int64_t pts = 0;
      for (const uint64_t i : {uint64_t{0}, table->count / 2, table->count}) {
        static_cast<void>(iso::sample_presentation_time(*built.list, *table, i, built.movie_timescale, &pts));
      }
    }
    // The brand checks (WI-1.222) read hvcC, av1C, sidx and the like straight from the file: idempotent as well.
    ovt::container::isobmff::check_brands(file, walked.top_level, ValidationLevel::Standard);
    // Deep adds the sample-byte rows (WI-1.222b): the first run may find more, the second finds nothing new.
    ovt::container::isobmff::check_brands(file, walked.top_level, ValidationLevel::Deep);
    require(ovt::container::isobmff::check_brands(file, walked.top_level, ValidationLevel::Deep) == 0);
  } catch (...) {
    std::abort();
  }
  return 0;
}
