// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output/text_writer.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ovt::cli {

void Table::add_column(std::string header, Align align) {
  columns_.push_back({std::move(header), align});
}

void Table::add_row(std::vector<std::string> cells) {
  cells.resize(columns_.size());
  rows_.push_back(std::move(cells));
}

std::string Table::render(bool color) const {
  std::vector<std::size_t> width(columns_.size());
  for (std::size_t c = 0; c < columns_.size(); ++c) {
    width[c] = display_width(columns_[c].header);
    for (const auto& row : rows_) {
      width[c] = std::max(width[c], display_width(row[c]));
    }
  }
  std::string out;
  const auto emit_line = [&](const auto& cell_of, bool header) {
    std::string line;
    for (std::size_t c = 0; c < columns_.size(); ++c) {
      const std::string_view text = cell_of(c);
      const std::string pad(width[c] - display_width(text), ' ');
      line += c == 0 ? "" : "  ";
      const bool right = columns_[c].align == Align::Right;
      line += right ? pad : "";
      line += header && color ? fmt::format("\x1b[1m{}\x1b[0m", text) : std::string(text);
      line += right ? "" : pad;
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    out += line;
    out += '\n';
  };
  emit_line([&](std::size_t c) -> std::string_view { return columns_[c].header; }, true);
  for (const auto& row : rows_) {
    emit_line([&](std::size_t c) -> std::string_view { return row[c]; }, false);
  }
  return out;
}

bool Table::write(std::FILE* out, bool color) const {
  const std::string text = render(color);
  return std::fwrite(text.data(), 1, text.size(), out) == text.size();
}

std::size_t display_width(std::string_view s) noexcept {
  return static_cast<std::size_t>(
      std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

std::string si(double value, std::string_view unit) {
  static constexpr std::array<std::string_view, 7> kPrefixes = {"", "k", "M", "G", "T", "P", "E"};
  std::size_t i = 0;
  double v = value;
  // Round to the printed precision before choosing the prefix, so 999.95 prints as "1.0 k", not "1000.0".
  const auto shown = [&](double x) { return i == 0 && x == std::floor(x) ? x : std::round(x * 10.0) / 10.0; };
  while (std::fabs(shown(v)) >= 1000.0 && i + 1 < kPrefixes.size()) {
    v /= 1000.0;
    ++i;
  }
  if (i == 0) {
    return v == std::floor(v) ? fmt::format("{:.0f} {}", v, unit) : fmt::format("{:.1f} {}", v, unit);
  }
  return fmt::format("{:.1f} {}{}", v, kPrefixes.at(i), unit);
}

std::string hms(double seconds) {
  const bool negative = seconds < 0;
  const auto total_ms = static_cast<std::int64_t>(std::llround(std::fabs(seconds) * 1000.0));
  const std::int64_t ms = total_ms % 1000;
  const std::int64_t s = (total_ms / 1000) % 60;
  const std::int64_t m = (total_ms / 60000) % 60;
  const std::int64_t h = total_ms / 3600000;
  return fmt::format("{}{:02}:{:02}:{:02}.{:03}", negative ? "-" : "", h, m, s, ms);
}

}  // namespace ovt::cli
