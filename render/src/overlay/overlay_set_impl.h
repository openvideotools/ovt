// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Definition of the opaque OverlaySet (chapter 10 §10.2.5). Internal to libovt-render. WI-1.400 needs a complete
// type so that VideoSurface::set_overlays() can own one; the overlay work items add the GPU buffers here.

#ifndef OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
#define OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H

#include "ovt/render/overlay_set.h"

namespace ovt::render {

class OverlaySet {
 public:
  OverlaySet() = default;
  OverlaySet(const OverlaySet&) = delete;
  OverlaySet& operator=(const OverlaySet&) = delete;
  OverlaySet(OverlaySet&&) = delete;
  OverlaySet& operator=(OverlaySet&&) = delete;
  ~OverlaySet() = default;
};

}  // namespace ovt::render

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_SET_IMPL_H
