// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF validation driver (chapter 08 §8.5, WI-1.224a): the structural CNT-1011..CNT-1013 rules that need the
// whole box tree. CNT-1014 and CNT-1015 are raised by the hand decoders when they see the payload; the driver does not
// repeat them. The cross-track and sample-level rules (CNT-2016, CNT-2017, the Deep level) are WI-1.224b.

#ifndef OVT_CONTAINER_ISOBMFF_VALIDATE_H
#define OVT_CONTAINER_ISOBMFF_VALIDATE_H

#include <cstddef>
#include <cstdint>

#include "container/isobmff/box_walk.h"

namespace ovt::container::isobmff {

// `quick` runs the structural rules only (§8.5 groups 0 and 1). `standard` adds every rule that needs no sample reads.
// `deep` also reads samples; until WI-1.224b it runs the same rules as `standard`.
enum class ValidationLevel : uint8_t { Quick, Standard, Deep };

// Required, duplicate and wrong-parent box rules over `top_level` (the file-order top-level boxes of a walk). Findings
// are appended to the diagnostics of the box they describe (a missing child goes to its parent, with the span of the
// parent's header). Idempotent: a finding already on the node, with the same code and message, is not added again.
// A subtree with a walk error (CNT-0102..CNT-0110, CNT-0111) is not checked for missing boxes: its children are cut
// short, so a missing box there is a consequence, not a finding. Returns the number of diagnostics added.
size_t validate(BoxNodes& top_level, ValidationLevel level);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_VALIDATE_H
