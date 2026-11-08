// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `csv` output (chapter 12 §12.1.4): RFC 4180, comma separator, "\n" line ending, header row first,
// UTF-8 without BOM, numbers with '.' and at most 6 fractional digits, booleans 0/1, enums as names.

#ifndef OVT_CLI_OUTPUT_CSV_WRITER_H
#define OVT_CLI_OUTPUT_CSV_WRITER_H

#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace ovt::cli {

class CsvWriter {
 public:
  explicit CsvWriter(std::FILE* out) : out_(out) {}
  void row(std::span<const std::string> fields);
  void row(std::initializer_list<std::string_view> fields);
  // Returns false if any write failed.
  bool finish();

  // A field quoted when it contains a comma, a double quote, CR or LF; quotes are doubled.
  [[nodiscard]] static std::string field(std::string_view s);
  // At most 6 fractional digits, trailing zeros removed, never an exponent; NaN/±Inf as nan, inf, -inf.
  [[nodiscard]] static std::string number(double v);
  [[nodiscard]] static std::string_view boolean(bool b) { return b ? "1" : "0"; }

 private:
  void put(std::string_view s);

  std::FILE* out_;
  bool failed_ = false;
};

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_CSV_WRITER_H
