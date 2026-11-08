// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the cache meta.json reader (chapter 06 §6.7.2, chapter 14 §14.7.1; WI-0.091 step 4, listed by
// WI-0.094 as fuzz_cache_meta). The input is the whole document. A CacheError(OVT_E_CORRUPT) is the expected
// answer to a malformed document; any other exception, a crash or a sanitizer report is a finding. A document
// that parses must serialise and parse again to the same record (the writer never produces unreadable text).
// The cache.json parser shares this target: byte 0 == 0xFE routes the rest to parse_cache_json, which must
// never throw (a malformed cache.json is rebuilt, not reported).
return 0;
}
// The option-string parser of ovt_cache_open shares this target: byte 0 == 0xFF routes the rest to it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "cache/cache_json.hpp"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/status.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> input(data, size);
  if (!input.empty() && input[0] == 0xFE) {
    (void)ovt::cache::parse_cache_json(input.subspan(1));
    return 0;
  }
  if (!input.empty() && input[0] == 0xFF) {
    const std::string_view text(reinterpret_cast<const char*>(input.data() + 1), input.size() - 1);
    try {
      (void)ovt::cache::parse_options_json(text);
    } catch (const ovt::cache::CacheError& e) {
      require(e.code == OVT_E_INVALID_ARG);
    }
    return 0;
  }
  try {
    const ovt::cache::Meta meta = ovt::cache::parse_meta(input);
    const std::string text = ovt::cache::serialize_meta(meta);
    require(ovt::cache::parse_meta(text) == meta);
  } catch (const ovt::cache::CacheError& e) {
    require(e.code == OVT_E_CORRUPT);
  }
  return 0;
}
