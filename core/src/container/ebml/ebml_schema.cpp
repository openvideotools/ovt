// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Lookup over the generated EBML element table (chapter 08 §8.6.7, WI-4.200).

#include "container/ebml/ebml_schema.h"

#include <algorithm>

namespace ovt::container::ebml {

const EbmlElementDef* lookup(uint32_t id) noexcept {
  const auto all = ebml_elements();
  const auto it =
      std::lower_bound(all.begin(), all.end(), id, [](const EbmlElementDef& e, uint32_t v) { return e.id < v; });
  return it != all.end() && it->id == id ? &*it : nullptr;
}

}  // namespace ovt::container::ebml
