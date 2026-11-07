// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::Cancelled (chapter 7 §7.1.6, WI-1.512). C++ helper header (D22): no ABI promise, never included from
// a C header, not installed.

#ifndef OVT_CXX_CANCEL_HPP
#define OVT_CXX_CANCEL_HPP

#include <stdexcept>

namespace ovt {

// The one exception type besides ParseError (chapter 5 §5.7). Thrown only by
// jobs::Cancel::throw_if_requested(); caught at the job boundary (the Pool task wrapper and the C API entry
// points) and mapped to OVT_E_CANCELLED. Never crosses the C ABI.
struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("cancelled") {}
};

}  // namespace ovt

#endif  // OVT_CXX_CANCEL_HPP
