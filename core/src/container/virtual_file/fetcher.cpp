// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/virtual_file/fetcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::vfile {
namespace {

bool starts_with_icase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
         });
}

class LocalFetcher final : public Fetcher {
 public:
  std::optional<uint64_t> size(const std::string& url) override {
    require_local(url);
    std::error_code ec;
    const auto n = std::filesystem::file_size(local_path_of(url), ec);
    if (ec) {
      throw io::IoError(OVT_E_IO, ec.value(), "cannot stat " + url);
    }
    return n;
  }

  std::vector<uint8_t> fetch(const std::string& url, const mpd::ByteRange& range) override {
    require_local(url);
    std::ifstream in(local_path_of(url), std::ios::binary);
    if (!in) {
      throw io::IoError(OVT_E_IO, 0, "cannot open " + url);
    }
    in.seekg(0, std::ios::end);
    const auto total = static_cast<uint64_t>(in.tellg());
    if (range.offset > total) {
      throw io::IoError(OVT_E_IO, 0, "range starts past the end of " + url);
    }
    const uint64_t avail = total - range.offset;
    const uint64_t want = range.length == 0 ? avail : range.length;
    if (want > avail) {
      throw io::IoError(OVT_E_IO, 0, "range runs past the end of " + url);
    }
    std::vector<uint8_t> out(want);
    in.seekg(static_cast<std::streamoff>(range.offset));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want));
    if (static_cast<uint64_t>(in.gcount()) != want) {
      throw io::IoError(OVT_E_IO, 0, "short read of " + url);
    }
    return out;
  }

 private:
  static void require_local(const std::string& url) {
    if (is_http_url(url)) {
      throw io::IoError(OVT_E_IO, 0, "network fetch is not available through the local fetcher: " + url);
    }
  }
};

class DefaultFetcher final : public Fetcher {
 public:
  std::optional<uint64_t> size(const std::string& url) override { return pick(url).size(url); }
  std::vector<uint8_t> fetch(const std::string& url, const mpd::ByteRange& range) override {
    return pick(url).fetch(url, range);
  }

 private:
  Fetcher& pick(const std::string& url) {
    if (!is_http_url(url)) {
      return local_;
    }
    if (offline_requested()) {
      throw io::IoError(OVT_E_IO, 0, "network access is disabled by OVT_OFFLINE=1: " + url);
    }
    if (!http_) {
      http_ = make_http_fetcher();
    }
    return *http_;
  }

  LocalFetcher local_;
  std::unique_ptr<Fetcher> http_;
};

}  // namespace

bool is_http_url(std::string_view url) noexcept {
  return starts_with_icase(url, "http://") || starts_with_icase(url, "https://");
}

std::string local_path_of(std::string_view url) {
  if (starts_with_icase(url, "file://")) {
    return std::string(url.substr(7));
  }
  return std::string(url);
}

bool offline_requested() noexcept {
  const char* v = std::getenv("OVT_OFFLINE");  // NOLINT(concurrency-mt-unsafe): read-only lookup
  return v != nullptr && v[0] != '\0' && std::string_view(v) != "0";
}

std::unique_ptr<Fetcher> make_local_fetcher() {
  return std::make_unique<LocalFetcher>();
}
std::unique_ptr<Fetcher> make_default_fetcher() {
  return std::make_unique<DefaultFetcher>();
}

}  // namespace ovt::container::vfile
