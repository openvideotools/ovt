// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal SHA-256 (FIPS 180-4) for the WI-0.093 fixtures: gen_fixtures writes the `.sha256` files and
// test_model_roundtrip checks them. Test-only; production code hashes through the platform's vetted library.

#ifndef OVT_TESTS_GOLDEN_SHA256_HPP
#define OVT_TESTS_GOLDEN_SHA256_HPP

#include <cstdint>
#include <span>
#include <string>

namespace ovt::golden {

/// Lower-case hex SHA-256 of @p data (64 characters).
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> data);

}  // namespace ovt::golden

#endif
