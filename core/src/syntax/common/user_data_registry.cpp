// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ITU-T T.35 dispatcher, user_data_unregistered registry, Dolby Vision markers (chapter 7 §7.3.11).

#include "syntax/common/user_data_registry.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ovt::syntax {

namespace {

// SPEC-ASSUMPTION(#89): 7.3.11 says the provider code is read "only for country code 0xB5", yet its
// own table has a row for country 0x26 (China, HDR Vivid) keyed on a provider code. The provider code is
// therefore read for 0xB5 and for every country that has a registry row.
bool country_has_provider_field(uint8_t country) {
  if (country == kT35CountryUs) {
    return true;
  }
  return std::any_of(kT35Registry.begin(), kT35Registry.end(),
                     [country](const T35Entry& e) { return e.country_code == country; });
}

// Does `entry` match the bytes at the reader's position? Never advances the reader.
bool entry_matches(const T35Entry& entry, const BitReader& br) {
  uint64_t pos = 0;  // bits looked at so far
  switch (entry.oriented_width) {
    case OrientedCode::None:
      break;
    case OrientedCode::U16:
      if (br.bits_left() < 16 || br.peek_bits(16) != entry.oriented_code) {
        return false;
      }
      pos = 16;
      break;
    case OrientedCode::U32:
      if (br.bits_left() < 32 || br.peek_bits(32) != entry.oriented_code) {
        return false;
      }
      pos = 32;
      break;
  }
  BitReader look = br;  // a copy: peeking further than 64 bits needs a second cursor
  look.skip_bits(pos);
  switch (entry.kind) {
    case T35Kind::Hdr10Plus: {
      if (look.bits_left() < 16) {
        return false;
      }
      const uint64_t id_and_version = look.peek_bits(16);
      const uint64_t id = id_and_version >> 8;
      const uint64_t version = id_and_version & 0xFF;
      // SPEC-ASSUMPTION(#89): versions other than 0 and 1 are reserved (7.3.11) and fall
      // back to the generic "T.35 provider" opaque entry instead of being parsed as ST 2094-40.
      return id == kHdr10PlusApplicationIdentifier && version <= 1;
    }
    case T35Kind::AtscGa94:
      return look.bits_left() >= 32 && look.peek_bits(32) == kGa94UserIdentifier;
    default:
      return true;
  }
}

}  // namespace

std::span<const uint8_t> strip_av1_trailing_bits(std::span<const uint8_t> payload) noexcept {
  size_t n = payload.size();
  while (n > 0 && payload[n - 1] == 0) {
    --n;
  }
  if (n > 0 && payload[n - 1] == 0x80) {
    return payload.first(n - 1);
  }
  return payload;
}

T35Result parse_t35(std::span<const uint8_t> payload, T35Carrier carrier) {
  BitReader br(payload);
  T35Result r;
  r.country_code = static_cast<uint8_t>(br.u(8));
  if (r.country_code == 0xFF) {
    r.country_code_extension_byte = static_cast<uint8_t>(br.u(8));
  }
  if (!country_has_provider_field(r.country_code)) {
    r.kind = T35Kind::UnknownCountry;
    r.enum_name = fmt::format("T.35 country 0x{:02X}", r.country_code);
    r.payload = br.remaining_bytes();
    return r;
  }
  const auto provider = static_cast<uint16_t>(br.u(16));
  r.provider_code = provider;

  for (const T35Entry& e : kT35Registry) {
    if (e.country_code != r.country_code || e.provider_code != provider || !entry_matches(e, br)) {
      continue;
    }
    r.kind = e.kind;
    r.enum_name = std::string(e.enum_name);
    if (e.oriented_width != OrientedCode::None) {
      r.provider_oriented_code = static_cast<uint32_t>(br.u(e.oriented_width == OrientedCode::U16 ? 16 : 32));
    }
    r.payload = br.remaining_bytes();
    switch (e.kind) {
      case T35Kind::Hdr10Plus: {
        // AV1 carries the OBU's trailing bits inside the payload: strip them before the ST 2094-40 parse.
        const std::span<const uint8_t> body =
            carrier == T35Carrier::Av1Metadata ? strip_av1_trailing_bits(r.payload) : r.payload;
        BitReader body_reader(body);
        r.hdr10plus = parse_hdr10plus(body_reader);
        r.has_hdr10plus = true;
        break;
      }
      case T35Kind::AtscGa94:
        r.atsc = parse_atsc_cc(br);
        break;
      case T35Kind::DolbyVisionAv1:
        r.has_dolby_vision = true;
        break;
      default:
        break;
    }
    return r;
  }

  // Provider not in the registry (or an entry whose further match failed): opaque.
  r.kind = T35Kind::UnknownProvider;
  const uint32_t oriented = br.bits_left() >= 16 ? static_cast<uint32_t>(br.u(16)) : 0;
  r.provider_oriented_code = oriented;
  r.enum_name = fmt::format("T.35 provider 0x{:04X} / 0x{:04X}", provider, oriented);
  r.payload = br.remaining_bytes();
  return r;
}

// ---- user_data_unregistered --------------------------------------------------------------------------

std::string uuid_to_string(const Uuid& uuid) {
  std::string s;
  s.reserve(36);
  for (size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      s.push_back('-');
    }
    s += fmt::format("{:02X}", uuid[i]);
  }
  return s;
}

std::vector<EncoderOption> split_encoder_options(std::string_view text) {
  std::vector<EncoderOption> out;
  const size_t marker = text.find("options:");
  if (marker == std::string_view::npos) {
    return out;
  }
  text.remove_prefix(marker + std::strlen("options:"));
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
      ++i;
    }
    const size_t start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r') {
      ++i;
    }
    if (i == start) {
      break;
    }
    const std::string_view token = text.substr(start, i - start);
    const size_t eq = token.find('=');
    EncoderOption opt;
    if (eq == std::string_view::npos) {
      opt.name = std::string(token);
    } else {
      opt.name = std::string(token.substr(0, eq));
      opt.value = std::string(token.substr(eq + 1));
      opt.has_value = true;
    }
    out.push_back(std::move(opt));
  }
  return out;
}

UserDataUnregistered parse_user_data_unregistered(std::span<const uint8_t> payload) {
  BitReader br(payload);
  UserDataUnregistered r;
  const std::span<const uint8_t> id = br.bytes(0, r.uuid.size());  // BR-0001 when shorter than 16 bytes
  std::copy(id.begin(), id.end(), r.uuid.begin());
  br.skip_bytes(r.uuid.size());
  r.uuid_text = uuid_to_string(r.uuid);
  r.payload = br.remaining_bytes();

  const auto* const it =
      std::find_if(kUuidRegistry.begin(), kUuidRegistry.end(), [&](const UuidEntry& e) { return e.uuid == r.uuid; });
  if (it != kUuidRegistry.end()) {
    r.known = true;
    r.enum_name = std::string(it->enum_name);
    r.option_prefix = it->option_prefix;
  } else {
    r.enum_name = fmt::format("user data (UUID {})", r.uuid_text);
  }

  // Printable-ASCII rendering; the NUL terminator that x264/x265 append is not part of the text.
  std::span<const uint8_t> body = r.payload;
  while (!body.empty() && body.back() == 0) {
    body = body.first(body.size() - 1);
  }
  const bool printable =
      !body.empty() && std::all_of(body.begin(), body.end(), [](uint8_t c) { return c >= 0x20 && c <= 0x7E; });
  if (printable) {
    r.text = std::string(reinterpret_cast<const char*>(body.data()), body.size());
    if (r.known) {
      r.options = split_encoder_options(*r.text);
    }
  }
  return r;
}

// ---- Dolby Vision markers ----------------------------------------------------------------------------

std::optional<DolbyVisionMarker> classify_hevc_unspecified_nal(uint8_t nal_unit_type, uint8_t nuh_layer_id) noexcept {
  if (nal_unit_type == 62 && nuh_layer_id == 0) {
    return DolbyVisionMarker{"Dolby Vision RPU", true};
  }
  if (nal_unit_type == 63) {
    return DolbyVisionMarker{"Dolby Vision EL", true};
  }
  return std::nullopt;
}

std::optional<DolbyVisionMarker> classify_h264_nal28(bool container_signals_dolby_vision) noexcept {
  if (container_signals_dolby_vision) {
    return DolbyVisionMarker{"Dolby Vision RPU", true};
  }
  return std::nullopt;
}

}  // namespace ovt::syntax
