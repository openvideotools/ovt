// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `fb` output: an OVT FlatBuffer stream (chapter 12 §12.1.4) = the 8-byte magic "OVTFBS\x01\x00",
// then size-prefixed FlatBuffers, each with its 4-byte file_identifier. The first record is always a
// CliEnvelope ("OVTE", chapter 06 §6.3).

#ifndef OVT_CLI_OUTPUT_FB_WRITER_H
#define OVT_CLI_OUTPUT_FB_WRITER_H

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "output/envelope.h"

namespace ovt::cli {

inline constexpr std::array<std::uint8_t, 8> kFbStreamMagic = {'O', 'V', 'T', 'F', 'B', 'S', 0x01, 0x00};
inline constexpr std::string_view kEnvelopeIdentifier = "OVTE";
// WI-1.500 acceptance test 3: printed when `--format fb` would go to a terminal.
inline constexpr std::string_view kRefuseBinaryOnTty = "refusing to write binary to a terminal; use -o";

// The CliEnvelope record, finished with FinishSizePrefixed and identifier "OVTE".
[[nodiscard]] flatbuffers::DetachedBuffer build_envelope_record(const Envelope& env, std::uint32_t record_count);

// True when `record` is one size-prefixed FlatBuffer: the 4-byte prefix equals the remaining length and a
// 4-character identifier follows the root offset.
[[nodiscard]] bool is_size_prefixed_record(std::span<const std::uint8_t> record) noexcept;

class FbStreamWriter {
 public:
  explicit FbStreamWriter(std::FILE* out) : out_(out) {}
  // Writes the magic before the first record. Refuses (returns false) a buffer that is not a size-prefixed
  // record. Returns false on a write error.
  bool write_record(std::span<const std::uint8_t> record);
  // Writes just the magic if no record was written; flushes.
  bool finish();

 private:
  bool write_bytes(std::span<const std::uint8_t> bytes);

  std::FILE* out_;
  bool magic_written_ = false;
  bool failed_ = false;
};

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_FB_WRITER_H
