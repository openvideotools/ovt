// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the MPEG-TS packet layer (chapter 8 §8.6.7 and §8.9.4, WI-4.204). The whole input is the file.
// Invariants, each an abort when violated: no exception escapes; the walk is deterministic; every counted packet
// lies inside the file on the stride grid of its run; the per-PID packet counts add up to the packet count; the
// diagnostics list stays capped; the tree can be built and expanding the root until nothing changes terminates;
// decode_packet never reads past the span it is given.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"

namespace {

namespace ts = ovt::container::mpegts;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    // decode_packet on every prefix length of the head of the input (the sanitizers check the bounds).
    for (size_t n = 0; n <= 200 && n <= size; ++n) {
      (void)ts::decode_packet(std::span<const uint8_t>(data, n));
    }

    const ovt::io::MemoryFile file(std::span<const uint8_t>(data, size));
    const auto a = ts::parse(file);
    const auto b = ts::parse(file);
    require(a.has_value() == b.has_value());
    if (!a) {
      return 0;
    }
    require(a->packet_count == b->packet_count && a->diagnostics.size() == b->diagnostics.size());
    require(a->diagnostics.size() <= ts::kMaxDiagnostics);
    require(a->packet_count > 0 && !a->runs.empty());

    uint64_t next_index = 0;
    uint64_t last_sync = 0;
    for (const auto& r : a->runs) {
      require(r.first_packet == next_index && r.count > 0);
      require(r.sync_offset >= a->geometry.prefix);
      require(r.sync_offset + (r.count - 1) * a->geometry.stride + ts::kPacketBytes <= size);
      require(next_index == 0 || r.sync_offset > last_sync);
      next_index += r.count;
      last_sync = r.sync_offset + (r.count - 1) * a->geometry.stride;
    }
    require(next_index == a->packet_count);
    require(data[a->sync_offset(0)] == ts::kSyncByte && data[a->sync_offset(a->packet_count - 1)] == ts::kSyncByte);
    require(a->packet_offset(0) + a->geometry.prefix == a->sync_offset(0));

    uint64_t total = 0;
    for (const auto& p : a->pids) {
      require(p.packets > 0 && p.pid < ts::kPidCount);
      require(p.payload_packets <= p.packets && p.scrambled_packets <= p.packets && p.transport_errors <= p.packets);
      total += p.packets;
    }
    require(total == a->packet_count);

    auto reader = ts::make_reader(file);
    require(reader != nullptr);
    for (size_t i = 0; i <= a->packet_count / ts::kChildWindow + 1; ++i) {
      require(reader->expand(0) == OVT_OK);
    }
    const auto tree = reader->tree();
    require(!tree.empty());
    const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data());
    // Root 0 is the packet tree; a second root `PSIT` carries the PSI/SI sections (WI-4.205).
    require(t->root()->size() == (a->psi.section_count > 0 ? 2U : 1U));
    require(t->root()->Get(0)->children()->size() == a->packet_count);
    require(a->psi.sections.size() <= ts::kMaxSections && a->psi.programs.size() <= 1024);
    require(a->psi.section_count == b->psi.section_count && a->psi.crc_errors == b->psi.crc_errors);
    require(reader->expand(a->packet_offset(0)) == OVT_OK);
  } catch (...) {
    std::abort();
  }
  return 0;
}
