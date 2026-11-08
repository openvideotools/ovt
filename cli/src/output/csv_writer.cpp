// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output/csv_writer.h"

#include <fmt/format.h>

#include <cmath>

namespace ovt::cli {

std::string CsvWriter::field(std::string_view s) {
  if (s.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(s);
  }
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string CsvWriter::number(double v) {
  if (std::isnan(v)) {
    return "nan";
  }
  if (std::isinf(v)) {
    return v > 0 ? "inf" : "-inf";
  }
  std::string s = fmt::format("{:.6f}", v);  // locale independent (no 'L')
  while (s.back() == '0') {
    s.pop_back();
  }
  if (s.back() == '.') {
    s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

void CsvWriter::put(std::string_view s) {
  if (!s.empty() && std::fwrite(s.data(), 1, s.size(), out_) != s.size()) {
    failed_ = true;
  }
}

void CsvWriter::row(std::span<const std::string> fields) {
  bool first = true;
  for (const std::string& f : fields) {
    put(first ? "" : ",");
    put(field(f));
    first = false;
  }
  put("\n");
}

void CsvWriter::row(std::initializer_list<std::string_view> fields) {
  bool first = true;
  for (const std::string_view f : fields) {
    put(first ? "" : ",");
    put(field(f));
    first = false;
  }
  put("\n");
}

bool CsvWriter::finish() {
  return std::fflush(out_) == 0 && !failed_;
}

}  // namespace ovt::cli
