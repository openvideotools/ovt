// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Block label strings of chapter 10 §10.5.5 (WI-2.433 step 2): one string per leaf block for OverlayConfig.label,
// built on the CPU by OverlayBuilder and laid out as glyphs by the label pass (WI-2.433b). Internal to libovt-render.

#ifndef OVT_RENDER_SRC_OVERLAY_LABEL_TEXT_H
#define OVT_RENDER_SRC_OVERLAY_LABEL_TEXT_H

#include <cstdint>
#include <string>

#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

enum class LabelCodec : uint8_t { Unknown, H264, H265, Av1 };

// Short codec-numbered names (§10.5.5: "DC", "PLANAR", "ANG 26", "D45_PRED"; "DCT2", "DST7", "ADST_FLIPADST"). The
// §6.3.1 tables of ovt/cxx/model/labels.hpp spell the long forms ("INTRA_ANGULAR26") and are not exported by
// libovt_core; these are the forms that fit in a block. A value without a name prints as its number.
OVT_RENDER_API std::string intra_mode_label(LabelCodec codec, int value);
OVT_RENDER_API std::string tx_type_label(LabelCodec codec, int value);

// Fills set.block_labels for `content` from the leaf blocks of `fa` (blocks without children), in block order.
// SPEC-ASSUMPTION(#648): §10.5.5 puts labels at "the current cycle level", which OverlayConfig does not carry; the
// leaf level (the blocks CellGrid.block_id names) is used.
void build_block_labels(const ovt::fb::FrameAnalysis& fa, LabelContent content, LabelCodec codec, OverlaySet& set);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_LABEL_TEXT_H
