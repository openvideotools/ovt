// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one hook the box walk calls into the group D decoders (chapter 08 §8.3.4, WI-1.209).

#ifndef OVT_CONTAINER_ISOBMFF_SAMPLE_TABLES_H
#define OVT_CONTAINER_ISOBMFF_SAMPLE_TABLES_H

#include <memory>
#include <vector>

#include "container/isobmff/box_context.h"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// Called when the children of a box have been walked; `ctx` is the sibling context they were walked in. For the
// children of an `stbl` it runs the checks that need two boxes whatever their order (the "second pass" of WI-1.209
// step 3): CNT-2005 (`stts` vs `stsz`/`stz2`), CNT-2008 (`stco` and `co64`), CNT-2009 (`stss`/`stps` past the sample
// count), CNT-2010 (`sdtp` length), CNT-2011 (`ctts` vs sample count), CNT-2019 (`stss` lists every sample).
void finish_sample_tables(std::vector<std::unique_ptr<fb::BoxNodeT>>& children, const BoxContext& ctx);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_SAMPLE_TABLES_H
