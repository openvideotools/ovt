// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/diagnostics.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace ovt::container {
namespace {

constexpr std::array kRegistry = {
#define X(id, code, sev, msg, ref) DiagInfo{code, Severity::sev, msg, ref},
#include "container/diag_codes.inc"
#undef X
};

constexpr std::array<std::string_view, 4> kSeverityNames = {"Info", "Warning", "Error", "Fatal"};

void append_csv_field(std::string& out, std::string_view field) {
  if (field.find_first_of(",\"\n\r") == std::string_view::npos) {
    out += field;
    return;
  }
  out += '"';
  for (const char c : field) {
    if (c == '"') {
      out += '"';
    }
    out += c;
  }
  out += '"';
}

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

std::string diagnostics_csv() {
  std::string out = "code,severity,message,spec_ref\n";
  for (const DiagInfo& d : kRegistry) {
    append_csv_field(out, d.code);
    out += ',';
    out += kSeverityNames[static_cast<size_t>(d.severity)];
    out += ',';
    append_csv_field(out, d.message);
    out += ',';
    append_csv_field(out, d.spec_ref);
    out += '\n';
  }
  return out;
}

}  // namespace ovt::container
