// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// TrackInfo of every `trak` of a parsed `moov` (chapter 08 §8.1.3, WI-1.217).

#ifndef OVT_CONTAINER_ISOBMFF_TRACKS_H
#define OVT_CONTAINER_ISOBMFF_TRACKS_H

#include <memory>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// One TrackInfo per `trak` of the first top-level `moov`, in `trak` order; empty without a `moov`. Reads only the
// decoded tree plus a few payload bytes of boxes no decoder covers (`frma`). Throws io::IoError only when the
// file read of such a payload fails with something other than a short file.
[[nodiscard]] std::vector<std::unique_ptr<fb::TrackInfoT>> build_tracks(
    const io::RandomAccessFile& file, const std::vector<std::unique_ptr<fb::BoxNodeT>>& top_level);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_TRACKS_H
