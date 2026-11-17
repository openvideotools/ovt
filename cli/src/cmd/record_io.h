// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between `ovt cache import` and the record codecs it needs (chapter 12 §12.2.13, WI-1.509 step 4):
// JSON -> FlatBuffer (`ovt_json_to_fb`) and the §6.9 validation rules (`ovt_validate_*`, §12.3.8).
// SPEC-ASSUMPTION(#673): neither C function exists in core/include yet, so the seam is filled by the C API
// wrappers once they land; until then `unavailable_record_io()` makes `import` exit 4 with a clear message.

#ifndef OVT_CLI_CMD_RECORD_IO_H
#define OVT_CLI_CMD_RECORD_IO_H

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "output/envelope.h"
#include "ovt/status.h"

namespace ovt::cli::cmd {

enum class RecordKind : std::uint8_t { FrameAnalysis, StreamIndex };

[[nodiscard]] constexpr std::string_view root_type_name(RecordKind k) noexcept {
  return k == RecordKind::FrameAnalysis ? "ovt.fb.FrameAnalysis" : "ovt.fb.StreamIndex";
}

struct RecordIo {
  // JSON text -> canonical FlatBuffer (plain, with file identifier). On failure returns a status and sets error.
  std::function<ovt_status(std::string_view json, RecordKind kind, std::vector<std::uint8_t>& fb, std::string& error)>
      json_to_fb;
  // §6.9 rules. Violations are appended to out as diagnostics (severity Error/Warning); OVT_OK with no Error
  // diagnostic means valid. expected_hash is null or 32 bytes.
  std::function<ovt_status(RecordKind kind, std::span<const std::uint8_t> fb, const std::uint8_t* expected_hash,
                           std::vector<Diagnostic>& out)>
      validate;
  [[nodiscard]] bool available() const { return json_to_fb && validate; }
};

}  // namespace ovt::cli::cmd

#endif  // OVT_CLI_CMD_RECORD_IO_H
