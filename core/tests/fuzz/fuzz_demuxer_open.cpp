// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the libavformat Demuxer and its custom AVIOContext (chapter 08 §8.7, WI-1.230). Byte 0 picks
// the hint_format (so every demuxer OVT opens through §8.7.2 rule 5 is reached, plus libavformat's own probe);
// the rest is the file. Invariants, each an abort when violated: no exception escapes; open() fails only with
// the documented statuses; every packet's stream index is one of streams() and its data is non-null when
// non-empty; read_packet() and seek_to_time() return only documented statuses. Packet reads are capped so one
// input stays fast.

extern "C" {
#include <libavutil/log.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>

#include "demux/demuxer.h"
#include "io/memory_file.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

constexpr std::array<std::string_view, 9> kHints{"", "isobmff", "ivf", "y4m", "annexb", "mpegts", "obu", "av1", "ebml"};
constexpr int kMaxPackets = 256;

bool documented(ovt_status s) {
  return s == OVT_E_CORRUPT || s == OVT_E_UNSUPPORTED || s == OVT_E_IO || s == OVT_E_NOMEM;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  try {
    av_log_set_level(AV_LOG_QUIET);
    const std::span<const uint8_t> in(data, size);
    const std::string_view hint = kHints.at(in[0] % kHints.size());
    auto file = std::make_shared<ovt::io::MemoryFile>(in.subspan(1));
    auto opened = ovt::demux::Demuxer::open(file, hint);
    if (!opened) {
      require(documented(opened.status()));
      return 0;
    }
    auto& d = *opened.value();
    const size_t n_streams = d.streams().size();
    require(n_streams > 0);
    for (int pass = 0; pass < 2; ++pass) {
      for (int i = 0; i < kMaxPackets; ++i) {
        auto r = d.read_packet();
        if (!r) {
          require(documented(r.status()));
          break;
        }
        if (!r.value()) {
          break;
        }
        const auto& p = *r.value();
        require(p.avstream_index >= 0 && static_cast<size_t>(p.avstream_index) < n_streams);
        require(p.data.empty() || p.data.data() != nullptr);
      }
      if (pass == 0) {
        const auto s = d.seek_to_time(0, 0, true);
        require(s.has_value() || documented(s.status()));
      }
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
