// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// display_probe() on Linux and every platform without a probe of its own (chapter 10 §10.4.5; WI-1.407): a stub that
// reports why no probe ran. The Wayland probe (wp_color_manager_v1) replaces it in WI-4.451.

#include <functional>
#include <memory>

#include "ovt/render/display_caps.h"
#include "platform/display_caps_rules.h"

namespace ovt::render {

namespace detail {
std::unique_ptr<DisplayWatcher> watch_display(const DisplayTarget& /*where*/, std::function<void()> /*on_change*/) {
  return nullptr;
}
}  // namespace detail

DisplayCaps display_probe(const DisplayTarget& /*where*/) {
  return detail::unimplemented_caps("not implemented: display probe for this platform arrives with WI-4.451",
                                    "display probe stub");
}

}  // namespace ovt::render
