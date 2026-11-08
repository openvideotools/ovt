// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `text` output helpers (chapter 12 §12.1.4): fixed-width tables, SI sizes with one decimal
// ("12.3 MB"), times as HH:MM:SS.mmm. Human output only; never parsed by tools.

#ifndef OVT_CLI_OUTPUT_TEXT_WRITER_H
#define OVT_CLI_OUTPUT_TEXT_WRITER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::cli {

enum class Align : std::uint8_t { Left, Right };

class Table {
 public:
  void add_column(std::string header, Align align = Align::Left);
  // Missing cells are blank, extra cells are dropped.
  void add_row(std::vector<std::string> cells);
  // Columns are separated by two spaces; the header is bold when `color` is set. No trailing spaces.
  [[nodiscard]] std::string render(bool color) const;
  bool write(std::FILE* out, bool color) const;

 private:
  struct Column {
    std::string header;
    Align align;
  };
  std::vector<Column> columns_;
  std::vector<std::vector<std::string>> rows_;
};

// Display width of UTF-8 text in code points (continuation bytes do not count).
[[nodiscard]] std::size_t display_width(std::string_view s) noexcept;
// SI prefix with one decimal from 1000 up: si(1234567, "B") == "1.2 MB", si(999, "B") == "999 B".
[[nodiscard]] std::string si(double value, std::string_view unit);
// 3723.5 -> "01:02:03.500"; negative values get a leading '-'.
[[nodiscard]] std::string hms(double seconds);

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_TEXT_WRITER_H
