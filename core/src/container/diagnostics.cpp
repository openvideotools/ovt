// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/diagnostics.h"

#include <array>
#include <cstddef>

namespace ovt::container {
namespace {

constexpr std::array kRegistry = {
#define X(id, code, sev, msg, ref) DiagInfo{code, Severity::sev, msg, ref},
#include "container/diag_codes.inc"
#undef X
};

}  // namespace

std::span<const DiagInfo> all_diagnostics() noexcept {
  return kRegistry;
}

const DiagInfo& diag(DiagCode code) noexcept {
  return kRegistry[static_cast<size_t>(code)];  // DiagCode values are indices into kRegistry
}

const DiagInfo* diag(std::string_view code) noexcept {
  for (const DiagInfo& d : kRegistry) {
    if (d.code == code) {
      return &d;
    }
  }
  return nullptr;
}

}  // namespace ovt::container
