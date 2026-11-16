// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The libcurl HTTP(S) fetcher of the virtual file (WI-4.210). Responses are never content-decoded
// (`Accept-Encoding: identity`) so byte ranges and sizes are those of the segment as served.

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "container/virtual_file/fetcher.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

#if defined(OVT_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace ovt::container::vfile {

#if defined(OVT_HAVE_CURL)
namespace {

constexpr uint64_t kMaxBody = 1ULL << 30;  // 1 GiB per fetch: a bound on what a hostile server can make us hold
constexpr long kConnectTimeoutSec = 15;
constexpr long kTotalTimeoutSec = 600;

void ensure_curl_global() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Body {
  std::vector<uint8_t> bytes;
  bool too_big = false;
};

size_t on_body(char* data, size_t size, size_t nmemb, void* user) {
  auto* body = static_cast<Body*>(user);
  const size_t n = size * nmemb;
  if (body->bytes.size() + n > kMaxBody) {
    body->too_big = true;
    return 0;  // aborts the transfer
  }
  body->bytes.insert(body->bytes.end(), reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + n);
  return n;
}

class Handle {
 public:
  Handle() : easy_(curl_easy_init()) {
    if (easy_ == nullptr) {
      throw io::IoError(OVT_E_NOMEM, 0, "curl_easy_init failed");
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    curl_slist_free_all(headers_);
    curl_easy_cleanup(easy_);
  }
  CURL* get() const noexcept { return easy_; }
  void identity_encoding() {
    headers_ = curl_slist_append(headers_, "Accept-Encoding: identity");
    curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers_);
  }

 private:
  CURL* easy_;
  curl_slist* headers_ = nullptr;
};

void common_options(Handle& h, const std::string& url) {
  CURL* c = h.get();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0: the string forms; older libcurl (Ubuntu 22.04's 7.81) has the bitmasks
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, kTotalTimeoutSec);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "ovt-virtual-file");
  h.identity_encoding();
}

long status_of(Handle& h) {
  long code = 0;
  curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &code);
  return code;
}

class HttpFetcher final : public Fetcher {
 public:
  HttpFetcher() { ensure_curl_global(); }

  std::optional<uint64_t> size(const std::string& url) override {
    Handle h;
    common_options(h, url);
    curl_easy_setopt(h.get(), CURLOPT_NOBODY, 1L);
    const CURLcode rc = curl_easy_perform(h.get());
    if (rc != CURLE_OK) {
      throw io::IoError(OVT_E_IO, 0, "HEAD " + url + ": " + curl_easy_strerror(rc));
    }
    const long code = status_of(h);
    if (code < 200 || code >= 300) {
      throw io::IoError(OVT_E_IO, 0, "HEAD " + url + ": HTTP " + std::to_string(code));
    }
    curl_off_t len = -1;
    curl_easy_getinfo(h.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    if (len < 0) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(len);
  }

  std::vector<uint8_t> fetch(const std::string& url, const mpd::ByteRange& range) override {
    Handle h;
    common_options(h, url);
    curl_easy_setopt(h.get(), CURLOPT_FAILONERROR, 0L);
    Body body;
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &body);
    const bool ranged = range.offset != 0 || range.length != 0;
    if (ranged) {
      std::string r = std::to_string(range.offset) + "-";
      if (range.length != 0) {
        r += std::to_string(range.offset + range.length - 1);
      }
      curl_easy_setopt(h.get(), CURLOPT_RANGE, r.c_str());
    }
    const CURLcode rc = curl_easy_perform(h.get());
    if (body.too_big) {
      throw io::IoError(OVT_E_IO, 0, "GET " + url + ": response larger than 1 GiB");
    }
    if (rc != CURLE_OK) {
      throw io::IoError(OVT_E_IO, 0, "GET " + url + ": " + curl_easy_strerror(rc));
    }
    const long code = status_of(h);
    if (code < 200 || code >= 300) {
      throw io::IoError(OVT_E_IO, 0, "GET " + url + ": HTTP " + std::to_string(code));
    }
    if (ranged && code == 200) {
      // The server ignored the Range header and sent the whole resource: cut the range out of it.
      if (range.offset > body.bytes.size()) {
        throw io::IoError(OVT_E_IO, 0, "GET " + url + ": range starts past the end of the resource");
      }
      const uint64_t avail = body.bytes.size() - range.offset;
      const uint64_t want = range.length == 0 ? avail : range.length;
      if (want > avail) {
        throw io::IoError(OVT_E_IO, 0, "GET " + url + ": range runs past the end of the resource");
      }
      return {body.bytes.begin() + static_cast<std::ptrdiff_t>(range.offset),
              body.bytes.begin() + static_cast<std::ptrdiff_t>(range.offset + want)};
    }
    return std::move(body.bytes);
  }
};

}  // namespace

std::unique_ptr<Fetcher> make_http_fetcher() {
  return std::make_unique<HttpFetcher>();
}

#else

std::unique_ptr<Fetcher> make_http_fetcher() {
  throw io::IoError(OVT_E_IO, 0, "this build has no libcurl: http(s) manifests are unavailable");
}

#endif

}  // namespace ovt::container::vfile
