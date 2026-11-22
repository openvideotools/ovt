// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Brand and profile checks (chapter 08 §8.4.5, WI-1.222a): the `ftyp`/`styp` brands of a file are matched against the
// rows of `kBrandChecks[]`, and the sample-entry rows run for every `stsd` entry. Findings are CNT-7xxx and CNT-8010
// diagnostics on the box they describe. They are advisory: nothing here changes how a file opens.

#ifndef OVT_CONTAINER_ISOBMFF_BRANDS_H
#define OVT_CONTAINER_ISOBMFF_BRANDS_H

#include <cstddef>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/validate.h"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::isobmff {

struct BrandOptions {
  // The file is a segment of an HLS playlist (the caller opened it through a playlist): enables CNT-7040 and CNT-7041.
  bool hls_context = false;
};

// Runs the brand rows at `level` (Quick runs none: §8.5 group 7 needs more than the structural pass). Brand rows that
// read sample bytes (CNT-7004, CNT-7008, CNT-7009) belong to the Deep level and are WI-1.222b. A top-level `moof` that
// is not expanded yet (CNT-0007) is skipped. Reads at most 1 MiB of any one box. Findings are appended to the
// diagnostics of the box they describe, never twice (add_finding). Returns the number of diagnostics added. Throws
// io::IoError only when a file read fails for a reason other than a short file.
size_t check_brands(const io::RandomAccessFile& file, BoxNodes& top_level, ValidationLevel level,
                    const BrandOptions& options = {});

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_BRANDS_H
