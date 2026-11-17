// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stub renderer switch of chapter 14 §14.10.2 / §14.10.3 (WI-1.465): with OVT_RENDER_STUB=1 in the environment
// the Picture panel's PictureHost creates no libovt-render surface and draws only its background colour, so scenes and
// interaction scripts that test chrome (layout, panels, models) do not depend on a GPU or on libplacebo's output.
// Renderer scenes leave the variable unset and run on lavapipe.

#ifndef OVT_APP_RENDER_STUB_H
#define OVT_APP_RENDER_STUB_H

namespace ovt::app {

// True when OVT_RENDER_STUB is set to "1". Read once per process (the render thread calls it too).
[[nodiscard]] bool render_stub_enabled();

}  // namespace ovt::app

#endif  // OVT_APP_RENDER_STUB_H
