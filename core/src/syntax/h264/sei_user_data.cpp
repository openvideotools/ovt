// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 user_data_registered_itu_t_t35() (D.1.6) and user_data_unregistered() (D.1.7): chapter 7 §7.3.4 rows 11 and
// 12, WI-1.110. The identification is done by the shared registries of §7.3.11 (WI-1.120); this file turns their
// results into tree nodes.

#include <fmt/format.h>

#include <cstdint>
#include <string>

#include "ovt/cxx/syntax/h264/sei.hpp"
#include "syntax/common/user_data_registry.h"
#include "syntax/h264/sei_internal.h"

namespace ovt::syntax::h264::detail {

namespace {

// Width in bits of the itu_t_t35_terminal_provider_oriented_code that parse_t35() consumed.
unsigned oriented_code_bits(const T35Result& t, uint64_t bits_after_provider) noexcept {
  if (!t.provider_oriented_code || !t.provider_code) {
    return 0;
  }
  for (const T35Entry& e : kT35Registry) {
    if (t.kind == e.kind && e.country_code == t.country_code && e.provider_code == *t.provider_code) {
      return e.oriented_width == OrientedCode::U32 ? 32 : e.oriented_width == OrientedCode::U16 ? 16 : 0;
    }
  }
  return bits_after_provider >= 16 ? 16 : 0;  // UnknownProvider: parse_t35 peeked 16 bits when they existed
}

}  // namespace

UserDataRegisteredItuTT35 parse_user_data_registered_itu_t_t35(SeiPayloadReader& r) {
  UserDataRegisteredItuTT35 m;
  SyntaxBuilder& b = r.builder();
  // The whole payload goes to the registry first; the fields it consumed are then read again through the builder
  // so that the tree shows them (§7.3.11).
  const std::span<const uint8_t> message = r.reader().bytes(r.start_bit() / 8, r.payload_size());
  const T35Result t = parse_t35(message, T35Carrier::Sei);
  m.itu_t_t35_country_code = static_cast<uint8_t>(r.u("itu_t_t35_country_code", 8));
  if (m.itu_t_t35_country_code == 0xFF) {
    m.itu_t_t35_country_code_extension_byte = static_cast<uint8_t>(r.u("itu_t_t35_country_code_extension_byte", 8));
  }
  if (t.provider_code) {
    m.itu_t_t35_terminal_provider_code = static_cast<uint16_t>(r.u("itu_t_t35_terminal_provider_code", 16));
    b.set_enum(t.enum_name);
    const unsigned bits = oriented_code_bits(t, r.bits_left());
    if (bits > 0) {
      m.itu_t_t35_terminal_provider_oriented_code =
          static_cast<uint32_t>(r.u("itu_t_t35_terminal_provider_oriented_code", bits));
    }
  }
  m.enum_name = t.enum_name;
  m.has_hdr10plus = t.has_hdr10plus;
  m.has_dolby_vision = t.has_dolby_vision;
  m.payload_byte_count = static_cast<uint32_t>(r.bits_left() / 8);
  (void)r.bytes("itu_t_t35_payload_byte", m.payload_byte_count);
  b.derived("t35", fmt::format("enum_name = \"{}\", has_hdr10plus = {}, has_dolby_vision = {}", t.enum_name,
                               t.has_hdr10plus ? 1 : 0, t.has_dolby_vision ? 1 : 0));
  return m;
}

SeiUserDataUnregistered parse_user_data_unregistered_sei(SeiPayloadReader& r) {
  SeiUserDataUnregistered u;
  SyntaxBuilder& b = r.builder();
  const std::span<const uint8_t> message = r.reader().bytes(r.start_bit() / 8, r.payload_size());
  const ovt::syntax::UserDataUnregistered parsed = parse_user_data_unregistered(message);  // BR-0001 below 16 bytes
  (void)r.bytes("uuid_iso_iec_11578", 16);
  b.set_enum(parsed.enum_name);
  b.derived("uuid", parsed.uuid_text);
  u.uuid = parsed.uuid_text;
  u.enum_name = parsed.enum_name;
  u.payload_byte_count = static_cast<uint32_t>(parsed.payload.size());
  (void)r.bytes("user_data_payload_byte", parsed.payload.size());
  if (parsed.text) {
    u.text = parsed.text;
    b.derived("string", *parsed.text);
  }
  for (const EncoderOption& o : parsed.options) {
    b.derived(fmt::format("{}[{}]", parsed.option_prefix, o.name), o.has_value ? o.value : std::string("1"));
  }
  return u;
}

}  // namespace ovt::syntax::h264::detail
