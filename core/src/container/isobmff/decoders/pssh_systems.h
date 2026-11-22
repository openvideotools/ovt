// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `pssh` SystemID table (chapter 08 §8.3.8, WI-1.213): the DRM and content-protection systems of the DASH-IF
// content protection registry (https://dashif.org/identifiers/content_protection/) and the UUID text helpers the
// encryption decoders share.

#ifndef OVT_CONTAINER_ISOBMFF_DECODERS_PSSH_SYSTEMS_H
#define OVT_CONTAINER_ISOBMFF_DECODERS_PSSH_SYSTEMS_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ovt::container::isobmff {

struct PsshSystem {
  std::array<uint8_t, 16> id;  // the SystemID bytes in file order (RFC 4122 big-endian UUID)
  std::string_view name;
};

// Every system §8.3.8 lists, in the order of the table. Defined in pssh_systems.cpp.
[[nodiscard]] std::span<const PsshSystem> pssh_systems() noexcept;

// The name of the system, or an empty view when the SystemID is not in the table (the decoder then raises CNT-6009).
[[nodiscard]] std::string_view pssh_system_name(std::span<const uint8_t, 16> system_id) noexcept;

// "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed" for 16 bytes in file order.
[[nodiscard]] std::string uuid_text(std::span<const uint8_t, 16> bytes);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_DECODERS_PSSH_SYSTEMS_H
