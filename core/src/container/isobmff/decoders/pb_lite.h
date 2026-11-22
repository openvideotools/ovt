// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A minimal protobuf wire-format reader (chapter 08 §8.3.8, WI-1.213): enough to walk the fields of the Widevine
// `WidevinePsshData` message. Every read is bounds-checked; a malformed message ends the walk (`ok()` turns false)
// and never throws. Wire types 3 and 4 (groups) are not used by proto2/proto3 messages of any DRM system and end
// the walk.

#ifndef OVT_CONTAINER_ISOBMFF_DECODERS_PB_LITE_H
#define OVT_CONTAINER_ISOBMFF_DECODERS_PB_LITE_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace ovt::container::isobmff::pb {

enum class WireType : uint8_t { Varint = 0, Fixed64 = 1, LengthDelimited = 2, Fixed32 = 5 };

struct Field {
  uint32_t number = 0;
  WireType wire = WireType::Varint;
  uint64_t value = 0;                // Varint, Fixed32, Fixed64 (little-endian); the length for LengthDelimited
  std::span<const uint8_t> payload;  // the bytes of a LengthDelimited field (empty for the other wire types)
  size_t offset = 0;                 // byte offset of the field's value (after tag and length) in the message
  size_t size = 0;                   // bytes the value covers (varint bytes, 4, 8 or the length)
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> message) noexcept : data_(message) {}

  // Reads the next field into `out`. False at the end of the message and after a malformed field (see ok()).
  [[nodiscard]] bool next(Field& out) noexcept {
    if (pos_ >= data_.size() || !ok_) {
      return false;
    }
    uint64_t tag = 0;
    if (!varint(tag, nullptr) || (tag >> 3U) == 0 || (tag >> 3U) > 0x1FFFFFFFU) {
      return fail();
    }
    out = Field{};
    out.number = static_cast<uint32_t>(tag >> 3U);
    const uint64_t wire = tag & 7U;
    switch (wire) {
      case 0: {
        out.wire = WireType::Varint;
        out.offset = pos_;
        if (!varint(out.value, &out.size)) {
          return fail();
        }
        return true;
      }
      case 1:
      case 5: {
        const size_t n = wire == 1 ? 8U : 4U;
        if (data_.size() - pos_ < n) {
          return fail();
        }
        out.wire = wire == 1 ? WireType::Fixed64 : WireType::Fixed32;
        out.offset = pos_;
        out.size = n;
        for (size_t i = n; i > 0; --i) {
          out.value = (out.value << 8U) | data_[pos_ + i - 1U];
        }
        pos_ += n;
        return true;
      }
      case 2: {
        uint64_t len = 0;
        if (!varint(len, nullptr) || len > data_.size() - pos_) {
          return fail();
        }
        out.wire = WireType::LengthDelimited;
        out.value = len;
        out.offset = pos_;
        out.size = static_cast<size_t>(len);
        out.payload = data_.subspan(pos_, static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
      }
      default:
        return fail();
    }
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] size_t position() const noexcept { return pos_; }

 private:
  bool fail() noexcept {
    ok_ = false;
    return false;
  }

  // A base-128 varint of at most 10 bytes; `used` receives the byte count.
  bool varint(uint64_t& v, size_t* used) noexcept {
    v = 0;
    for (unsigned i = 0; i < 10 && pos_ < data_.size(); ++i) {
      const uint8_t b = data_[pos_++];
      v |= static_cast<uint64_t>(b & 0x7FU) << (7U * i);
      if ((b & 0x80U) == 0) {
        if (used != nullptr) {
          *used = i + 1U;
        }
        return true;
      }
    }
    return false;
  }

  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace ovt::container::isobmff::pb

#endif  // OVT_CONTAINER_ISOBMFF_DECODERS_PB_LITE_H
