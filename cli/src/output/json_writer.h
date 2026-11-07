// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming JSON writer and the `json` envelope (chapter 12 §12.1.4). Streaming, so that later commands
// never build a DOM for hundreds of MB of output (WI-1.503/WI-1.505 notes). Numbers never go through the
// C locale: fmt, whose shortest round-trip output equals
// std::to_chars (libc++ has no floating-point to_chars before macOS 13.3; the deployment target is 13.0).

#ifndef OVT_CLI_OUTPUT_JSON_WRITER_H
#define OVT_CLI_OUTPUT_JSON_WRITER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "output/envelope.h"

namespace ovt::cli {

class JsonWriter {
 public:
  // `out` is not owned. pretty = 2-space indentation, otherwise a single line.
  JsonWriter(std::FILE* out, bool pretty) : out_(out), pretty_(pretty) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);

  void string(std::string_view s);
  void boolean(bool b);
  void null();
  // |v| > 2^53 is written as a decimal string (§12.1.4 printer rule for long/ulong).
  void integer(std::int64_t v);
  void unsigned_integer(std::uint64_t v);
  // Shortest round-trip form; NaN and ±Inf as the strings "nan", "inf", "-inf".
  void number(double v);
  // An already serialised JSON value, e.g. the text of ovt_fb_to_json (WI-0.088).
  void raw(std::string_view json);

  // Ends the document with '\n'. Returns false if any write failed.
  bool finish();

 private:
  void before_value();
  void put(std::string_view s);
  void newline_indent();

  std::FILE* out_;
  bool pretty_;
  bool after_key_ = false;
  bool failed_ = false;
  struct Level {
    bool is_array;
    bool empty;
  };
  std::vector<Level> stack_;
};

// JSON string escaping (RFC 8259): quotes, backslash and control characters; UTF-8 passes through.
[[nodiscard]] std::string json_quote(std::string_view s);

void write_diagnostic(JsonWriter& w, const Diagnostic& d);

}  // namespace ovt::cli

#endif  // OVT_CLI_OUTPUT_JSON_WRITER_H
