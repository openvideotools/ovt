// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PQ test pattern and dataspace rules of WI-0.052; see hdr_pattern.h.

#include "hdr_pattern.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ovt::android_shell {

namespace {

// 3 x 5 bitmap digits, one row per entry, bit 2 = left column.
constexpr std::array<std::array<std::uint8_t, 5>, 10> kDigitRows{{
    {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
    {0b111, 0b001, 0b010, 0b010, 0b010},  // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
}};
constexpr int kDigitColumns = 3;
constexpr int kDigitRowsCount = 5;

constexpr double kSdrWhiteNits = 203.0;  // BT.2408 reference white
constexpr double kHlgReferencePeakNits = 1000.0;

std::array<std::uint16_t, 3> grey(OutputEncoding encoding, double nits) {
  const std::uint16_t code = code10(encoding, nits);
  return {code, code, code};
}

// Appends `text` (digits only) in `pixel`-sized blocks, centred on `centre_x`, top at `top`.
void append_label(std::vector<PatternRect>& out, const std::string& text, int centre_x, int top, int pixel,
                  const std::array<std::uint16_t, 3>& code) {
  const int advance = (kDigitColumns + 1) * pixel;
  const int width = static_cast<int>(text.size()) * advance - pixel;
  int left = centre_x - width / 2;
  for (const char ch : text) {
    const auto& rows = kDigitRows.at(static_cast<std::size_t>(ch - '0'));
    for (int row = 0; row < kDigitRowsCount; ++row) {
      for (int column = 0; column < kDigitColumns; ++column) {
        if ((rows.at(static_cast<std::size_t>(row)) & (1U << (kDigitColumns - 1 - column))) != 0) {
          out.push_back({left + column * pixel, top + row * pixel, pixel, pixel, code});
        }
      }
    }
    left += advance;
  }
}

}  // namespace

OutputEncoding choose_encoding(std::span<const std::int32_t> hdr_types) noexcept {
  const auto has = [&](std::int32_t type) {
    return std::find(hdr_types.begin(), hdr_types.end(), type) != hdr_types.end();
  };
  if (has(kHdrTypeHdr10) || has(kHdrTypeHdr10Plus)) {
    return OutputEncoding::Pq10;
  }
  if (has(kHdrTypeHlg)) {
    return OutputEncoding::Hlg10;
  }
  return OutputEncoding::Srgb8;
}

std::int32_t dataspace_for(OutputEncoding encoding) noexcept {
  switch (encoding) {
    case OutputEncoding::Pq10:
      return kStandardBt2020 | kTransferSt2084 | kRangeFull;
    case OutputEncoding::Hlg10:
      return kStandardBt2020 | kTransferHlg | kRangeFull;
    case OutputEncoding::Srgb8:
      break;
  }
  return kStandardBt709 | kTransferSmpte170m | kRangeLimited;
}

const char* dataspace_log_name(OutputEncoding encoding) noexcept {
  switch (encoding) {
    case OutputEncoding::Pq10:
      return "BT2020_PQ";
    case OutputEncoding::Hlg10:
      return "BT2020_HLG";
    case OutputEncoding::Srgb8:
      break;
  }
  return "SRGB";
}

double pq_signal(double nits) noexcept {
  // ST 2084 constants (chapter 10 §10.7.2).
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 4096.0 * 128.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 4096.0 * 32.0;
  constexpr double c3 = 2392.0 / 4096.0 * 32.0;
  const double y = std::clamp(nits, 0.0, 10000.0) / 10000.0;
  const double ym1 = std::pow(y, m1);
  return std::pow((c1 + c2 * ym1) / (1.0 + c3 * ym1), m2);
}

double hlg_signal(double nits) noexcept {
  // BT.2100 HLG: inverse OOTF (achromatic, gamma 1.2 on the 1000 cd/m² display), then the OETF.
  constexpr double a = 0.17883277;
  constexpr double b = 1.0 - 4.0 * a;
  const double c = 0.5 - a * std::log(4.0 * a);
  const double display = std::clamp(nits / kHlgReferencePeakNits, 0.0, 1.0);
  const double scene = std::pow(display, 1.0 / 1.2);
  if (scene <= 1.0 / 12.0) {
    return std::sqrt(3.0 * scene);
  }
  return a * std::log(12.0 * scene - b) + c;
}

double srgb_signal(double nits, double sdr_white_nits) noexcept {
  const double linear = sdr_white_nits > 0.0 ? std::clamp(nits / sdr_white_nits, 0.0, 1.0) : 0.0;
  if (linear <= 0.0031308) {
    return 12.92 * linear;
  }
  return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

std::uint16_t code10(OutputEncoding encoding, double nits) noexcept {
  double signal = 0.0;
  switch (encoding) {
    case OutputEncoding::Pq10:
      signal = pq_signal(nits);
      break;
    case OutputEncoding::Hlg10:
      signal = hlg_signal(nits);
      break;
    case OutputEncoding::Srgb8:
      signal = srgb_signal(nits);
      break;
  }
  return static_cast<std::uint16_t>(std::lround(std::clamp(signal, 0.0, 1.0) * 1023.0));
}

std::vector<PatternRect> build_test_pattern(int width, int height, OutputEncoding encoding) {
  std::vector<PatternRect> out;
  if (width < 64 || height < 64) {
    return out;
  }
  const int left = width * 4 / 100;
  const int usable = width - 2 * left;

  // Top band: grey ramp in equal code steps from 0 to 1023 (the last step absorbs the rounding remainder).
  const int ramp_top = height * 4 / 100;
  const int ramp_height = height * 14 / 100;
  for (int step = 0; step < kRampSteps; ++step) {
    const int x0 = left + usable * step / kRampSteps;
    const int x1 = left + usable * (step + 1) / kRampSteps;
    const auto code = static_cast<std::uint16_t>(step * 1023 / (kRampSteps - 1));
    out.push_back({x0, ramp_top, x1 - x0, ramp_height, {code, code, code}});
  }

  // Middle band: labelled patches.
  const int patches_top = height * 24 / 100;
  const int slot = usable / static_cast<int>(kPatchNits.size());
  const int patch_size = std::max(1, std::min(slot * 2 / 3, height * 30 / 100));
  const int pixel = std::max(1, patch_size / 25);  // digit height = patch / 5
  const auto label_code = grey(encoding, kSdrWhiteNits);
  for (std::size_t i = 0; i < kPatchNits.size(); ++i) {
    const int centre = left + slot * static_cast<int>(i) + slot / 2;
    out.push_back({centre - patch_size / 2, patches_top, patch_size, patch_size, grey(encoding, kPatchNits.at(i))});
    append_label(out, std::to_string(kPatchNits.at(i)), centre, patches_top + patch_size + 2 * pixel, pixel,
                 label_code);
  }

  // Bottom band: colour bars at 203 cd/m² per channel.
  const std::uint16_t on = code10(encoding, kSdrWhiteNits);
  const std::uint16_t off = code10(encoding, 0.0);
  constexpr std::array<std::array<bool, 3>, 7> kBars{{
      {true, true, true},    // white
      {true, true, false},   // yellow
      {false, true, true},   // cyan
      {false, true, false},  // green
      {true, false, true},   // magenta
      {true, false, false},  // red
      {false, false, true},  // blue
  }};
  const int bars_top = height * 72 / 100;
  const int bars_height = height * 22 / 100;
  const int bar_count = static_cast<int>(kBars.size());
  for (int bar = 0; bar < bar_count; ++bar) {
    const int x0 = left + usable * bar / bar_count;
    const int x1 = left + usable * (bar + 1) / bar_count;
    const auto& on_off = kBars.at(static_cast<std::size_t>(bar));
    out.push_back(
        {x0, bars_top, x1 - x0, bars_height, {on_off[0] ? on : off, on_off[1] ? on : off, on_off[2] ? on : off}});
  }
  return out;
}

}  // namespace ovt::android_shell
