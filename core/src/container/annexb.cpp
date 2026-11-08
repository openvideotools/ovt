// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Annex B start-code splitter (chapter 08 §8.6.3, WI-1.226).
//
// The scanner is a byte-wise state machine fed with consecutive windows, so a start code that straddles two
// windows needs no carry buffer. Two modes: *outside* a NAL unit (looking for the next start code, counting zero
// bytes and noting the first non-zero byte as garbage) and *inside* one (a NAL unit ends at the first `00 00 00`
// or `00 00 01`; `00 00 02` and `00 00 03` are not boundaries). A NAL unit that ends at EOF drops its trailing
// zero bytes (trailing_zero_8bits).

#include "container/annexb.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "container/diagnostics.h"

namespace ovt::container {
namespace {

constexpr size_t kWindow = size_t{1} << 20U;
constexpr uint64_t kMaxTrailingZeros = 8;

bool needs_four_byte_code(AnnexBCodec codec, uint8_t header) {
  if (codec == AnnexBCodec::H264) {
    const unsigned type = header & 0x1FU;
    return type == 7 || type == 8 || type == 9;  // SPS, PPS, AUD
  }
  if (codec == AnnexBCodec::H265) {
    const unsigned type = (header >> 1U) & 0x3FU;
    return type >= 32 && type <= 35;  // VPS, SPS, PPS, AUD
  }
  return false;
}

class Splitter {
 public:
  Splitter(const NalCallback& on_nal, const FindingSink& sink, AnnexBCodec codec)
      : on_nal_(on_nal), sink_(sink), codec_(codec) {}

  void feed(std::span<const uint8_t> window, uint64_t offset) {
    size_t i = 0;
    const size_t n = window.size();
    while (i < n) {
      uint64_t pos = offset + i;
      if (in_nal_) {
        if (pos == nal_start_) {
          header_ = window[i];
        } else if (zeros_ == 0) {
          // Fast path: jump to the next zero byte; nothing in between can end the NAL unit.
          const void* z = std::memchr(window.data() + i, 0, n - i);
          if (z == nullptr) {
            return;
          }
          i = static_cast<size_t>(static_cast<const uint8_t*>(z) - window.data());
          pos = offset + i;
        }
        const uint8_t b = window[i];
        if (zeros_ >= 2 && b <= 1) {
          // 00 00 00 or 00 00 01 at pos-2: the NAL unit ends before it.
          end_nal(pos - 2);
          region_start_ = pos - 2;
          zeros_ = 2;
          in_nal_ = false;
          continue;  // handle this byte outside a NAL unit
        }
        zeros_ = b == 0 ? zeros_ + 1 : 0;
        ++i;
        continue;
      }
      outside_byte(window[i], pos);
      ++i;
    }
  }

  void finish(uint64_t size) {
    if (in_nal_) {
      end_nal(size - zeros_);
      region_start_ = size - zeros_;
      in_nal_ = false;
      garbage_ = false;
    }
    if (garbage_) {
      report(DiagCode::CNT_ESB_0001, garbage_start_,
             std::to_string(size - garbage_start_) + " bytes of garbage after the last NAL unit");
    } else if (have_prev_ && size - region_start_ > kMaxTrailingZeros) {
      too_many_zeros(region_start_, size - region_start_);
    }
  }

  [[nodiscard]] uint64_t nal_count() const noexcept { return index_; }

 private:
  void report(DiagCode code, uint64_t offset, std::string message) const {
    if (sink_) {
      sink_(Finding{code, offset, std::move(message)});
    }
  }

  void too_many_zeros(uint64_t offset, uint64_t count) const {
    report(DiagCode::CNT_ESB_0002, offset,
           std::to_string(count) + " trailing_zero_8bits after NAL unit " + std::to_string(index_ - 1));
  }

  // One byte outside a NAL unit: a zero, the `01` of a start code, or garbage.
  void outside_byte(uint8_t b, uint64_t pos) {
    if (b == 0) {
      ++zeros_;
    } else if (b == 1 && zeros_ >= 2) {
      start_code(pos);
    } else {
      if (!garbage_) {
        garbage_ = true;
        garbage_start_ = pos;
      }
      zeros_ = 0;
    }
  }

  // The start code's final `01` is at `pos`; zeros_ >= 2 zero bytes precede it in this region.
  void start_code(uint64_t pos) {
    sc_bytes_ = zeros_ >= 3 ? 4 : 3;
    prefix_start_ = pos + 1 - sc_bytes_;
    if (garbage_) {
      report(DiagCode::CNT_ESB_0001, garbage_start_,
             std::to_string(prefix_start_ - garbage_start_) + " bytes of garbage before the start code at offset " +
                 std::to_string(prefix_start_));
    } else if (have_prev_ && prefix_start_ - region_start_ > kMaxTrailingZeros) {
      too_many_zeros(region_start_, prefix_start_ - region_start_);
    }
    garbage_ = false;
    in_nal_ = true;
    nal_start_ = pos + 1;
    zeros_ = 0;
  }

  void end_nal(uint64_t nal_end) {
    have_prev_ = true;
    const uint64_t length = nal_end - nal_start_;
    if (length == 0) {
      report(DiagCode::CNT_ESB_0003, nal_start_ - 3,
             "empty NAL unit after start code at offset " + std::to_string(nal_start_ - 3));
      return;
    }
    if ((header_ & 0x80U) != 0) {
      report(DiagCode::CNT_ESB_0005, nal_start_,
             "forbidden_zero_bit set in NAL header at offset " + std::to_string(nal_start_));
    }
    if (sc_bytes_ == 3 && needs_four_byte_code(codec_, header_)) {
      report(DiagCode::CNT_ESB_0004, prefix_start_,
             "3-byte start code at offset " + std::to_string(prefix_start_) + " before a parameter set or AUD");
    }
    const NalSpan span{index_, FileSpan{prefix_start_, nal_start_ - prefix_start_}, FileSpan{nal_start_, length},
                       sc_bytes_, header_};
    ++index_;
    if (on_nal_) {
      on_nal_(span);
    }
  }

  const NalCallback& on_nal_;
  const FindingSink& sink_;
  AnnexBCodec codec_;

  bool in_nal_ = false;
  uint64_t zeros_ = 0;         // consecutive zero bytes ending at the previous byte, within the current region/NAL unit
  uint64_t region_start_ = 0;  // outside a NAL unit: where the region (after the previous NAL unit) begins
  bool garbage_ = false;
  uint64_t garbage_start_ = 0;
  bool have_prev_ = false;  // a NAL unit (possibly empty) ended before this region
  uint64_t index_ = 0;

  uint64_t prefix_start_ = 0;
  uint64_t nal_start_ = 0;
  uint8_t sc_bytes_ = 3;
  uint8_t header_ = 0;
};

}  // namespace

AnnexBSummary annexb_split(std::span<const uint8_t> data, uint64_t base_offset, const NalCallback& on_nal,
                           const FindingSink& findings, AnnexBCodec codec) {
  Splitter s(on_nal, findings, codec);
  s.feed(data, base_offset);
  s.finish(base_offset + data.size());
  return AnnexBSummary{s.nal_count()};
}

AnnexBSummary annexb_split(const io::RandomAccessFile& file, const NalCallback& on_nal, const FindingSink& findings,
                           AnnexBCodec codec) {
  Splitter s(on_nal, findings, codec);
  file.advise(0, file.size(), io::RandomAccessFile::Access::Sequential);
  std::vector<uint8_t> buf(kWindow);
  uint64_t offset = 0;
  while (offset < file.size()) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kWindow, file.size() - offset));
    const size_t got = file.read(offset, want, buf.data());
    if (got == 0) {
      break;
    }
    s.feed(std::span<const uint8_t>(buf.data(), got), offset);
    offset += got;
  }
  s.finish(offset);
  return AnnexBSummary{s.nal_count()};
}

}  // namespace ovt::container
