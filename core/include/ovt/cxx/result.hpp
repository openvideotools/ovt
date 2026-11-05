// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::Result<T>: a status and a value, without exceptions at module boundaries (chapter 7 §7.1.6, §0.1.7;
// WI-1.512). C++ helper header (D22): no ABI promise, never included from a C header, not installed.

#ifndef OVT_CXX_RESULT_HPP
#define OVT_CXX_RESULT_HPP

#include <cassert>
#include <optional>
#include <string>
#include <utility>

#include "ovt/status.h"

namespace ovt {

template <class T>
class Result {
 public:
  static Result ok(T v) { return Result(std::move(v)); }
  // An error Result carries a status other than OVT_OK; passing OVT_OK is a programming error and becomes
  // OVT_E_INTERNAL, so has_value() never lies about value() being present.
  static Result error(ovt_status s, std::string msg) { return Result(s, std::move(msg)); }

  [[nodiscard]] bool has_value() const noexcept { return status_ == OVT_OK; }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] ovt_status status() const noexcept { return status_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }  // empty when ok

  // Precondition: has_value().
  T& value() & {
    assert(has_value());
    return *value_;
  }
  const T& value() const& {
    assert(has_value());
    return *value_;
  }
  T&& value() && {
    assert(has_value());
    return std::move(*value_);
  }

 private:
  explicit Result(T v) : status_(OVT_OK), value_(std::move(v)) {}
  Result(ovt_status s, std::string msg) : status_(s == OVT_OK ? OVT_E_INTERNAL : s), message_(std::move(msg)) {}

  ovt_status status_;
  std::string message_;
  std::optional<T> value_;
};

template <>
class Result<void> {
 public:
  static Result ok() { return Result(OVT_OK, {}); }
  static Result error(ovt_status s, std::string msg) {
    return Result(s == OVT_OK ? OVT_E_INTERNAL : s, std::move(msg));
  }

  [[nodiscard]] bool has_value() const noexcept { return status_ == OVT_OK; }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] ovt_status status() const noexcept { return status_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }  // empty when ok

 private:
  Result(ovt_status s, std::string msg) : status_(s), message_(std::move(msg)) {}

  ovt_status status_;
  std::string message_;
};

}  // namespace ovt

#endif  // OVT_CXX_RESULT_HPP
