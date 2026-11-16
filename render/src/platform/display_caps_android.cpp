// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// display_probe() on Android (chapter 10 §10.4.5; WI-1.407): a stub that reports why no probe ran. The JNI-fed probe
// (Display.Mode.supportedHdrTypes, HdrSdrRatioChangedListener) replaces it in WI-4.458.

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
  return detail::unimplemented_caps("not implemented: Android display probe arrives with WI-4.458",
                                    "Android display probe stub");
}

}  // namespace ovt::render
