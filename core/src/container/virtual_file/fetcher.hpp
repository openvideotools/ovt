// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Where the virtual file gets segment bytes (chapter 08 §8.6.7, WI-4.210): a local path or file:// URL is read from
// disk, http:// and https:// go through libcurl on every platform (no Android HttpURLConnection, for determinism).
// OVT_OFFLINE=1 makes every network fetch fail, so tests can never reach the network by accident.

#ifndef OVT_CONTAINER_VIRTUAL_FILE_FETCHER_HPP
#define OVT_CONTAINER_VIRTUAL_FILE_FETCHER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "container/virtual_file/mpd.hpp"

namespace ovt::container::vfile {

class Fetcher {
 public:
  Fetcher() = default;
  Fetcher(const Fetcher&) = delete;
  Fetcher& operator=(const Fetcher&) = delete;
  Fetcher(Fetcher&&) = delete;
  Fetcher& operator=(Fetcher&&) = delete;
  virtual ~Fetcher() = default;

  // The size of the whole resource when it is cheap to learn (stat, HEAD + Content-Length); nullopt otherwise.
  // Throws io::IoError when the resource does not exist.
  [[nodiscard]] virtual std::optional<uint64_t> size(const std::string& url) = 0;
  // `range.length` bytes at `range.offset`, or from `range.offset` to the end when length is 0. Throws io::IoError
  // (OVT_E_IO) on a missing resource, a network failure or a refused fetch.
  [[nodiscard]] virtual std::vector<uint8_t> fetch(const std::string& url, const mpd::ByteRange& range) = 0;
};

// True for http:// and https:// (case-insensitive scheme).
[[nodiscard]] bool is_http_url(std::string_view url) noexcept;
// `file:///a/b` -> `/a/b`; anything else unchanged.
[[nodiscard]] std::string local_path_of(std::string_view url);
// True when OVT_OFFLINE is set to a non-empty value other than "0".
[[nodiscard]] bool offline_requested() noexcept;

// Reads local files only; http(s) URLs throw.
[[nodiscard]] std::unique_ptr<Fetcher> make_local_fetcher();
// Local files plus libcurl for http(s); honours OVT_OFFLINE.
[[nodiscard]] std::unique_ptr<Fetcher> make_default_fetcher();

// Implemented in http_fetcher.cpp. Throws io::IoError(OVT_E_UNSUPPORTED-like message) when the build has no libcurl.
[[nodiscard]] std::unique_ptr<Fetcher> make_http_fetcher();

}  // namespace ovt::container::vfile

#endif  // OVT_CONTAINER_VIRTUAL_FILE_FETCHER_HPP
