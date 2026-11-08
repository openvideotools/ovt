// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// meta.json reader and writer (chapter 06 §6.7.2, WI-0.091 step 4). The reader applies the rules of
// docs/spec/cache-meta-1.schema.json by hand: required members, types, the `^[0-9a-f]{64}$` patterns, the
// codec enum, `minimum: 0` and the date-time format. Unknown members are ignored.

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/status.h"

namespace ovt::cache {

namespace {

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

constexpr std::array<std::string_view, 7> kCodecs = {"H264", "H265", "AV1", "VP9", "H266", "MPEG2", "Unknown"};

[[noreturn]] void bad(std::string_view where, std::string_view what) {
  std::string msg = "cache: meta.json: ";
  msg.append(where).append(": ").append(what);
  throw CacheError(OVT_E_CORRUPT, std::move(msg));
}

bool is_digits(std::string_view s, std::size_t from, std::size_t count) noexcept {
  if (s.size() < from + count) {
    return false;
  }
  for (std::size_t i = from; i < from + count; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

// The required member `name` of `obj`, or a schema error.
const json& required(const json& obj, std::string_view where, const char* name) {
  const auto it = obj.find(name);
  if (it == obj.end()) {
    bad(where, std::string("missing required member '") + name + "'");
  }
  return *it;
}

std::string get_string(const json& obj, std::string_view where, const char* name) {
  const json& v = required(obj, where, name);
  if (!v.is_string()) {
    bad(where, std::string("'") + name + "' is not a string");
  }
  return v.get<std::string>();
}

std::uint64_t get_uint(const json& obj, std::string_view where, const char* name) {
  const json& v = required(obj, where, name);
  if (!v.is_number_integer()) {
    bad(where, std::string("'") + name + "' is not an integer");
  }
  if (v.is_number_unsigned()) {
    return v.get<std::uint64_t>();
  }
  const auto s = v.get<std::int64_t>();
  if (s < 0) {
    bad(where, std::string("'") + name + "' is negative");
  }
  return static_cast<std::uint64_t>(s);
}

std::optional<std::string> opt_string(const json& obj, std::string_view where, const char* name) {
  const auto it = obj.find(name);
  if (it == obj.end()) {
    return std::nullopt;
  }
  if (!it->is_string()) {
    bad(where, std::string("'") + name + "' is not a string");
  }
  return it->get<std::string>();
}

std::optional<std::uint64_t> opt_uint(const json& obj, std::string_view where, const char* name) {
  if (obj.find(name) == obj.end()) {
    return std::nullopt;
  }
  return get_uint(obj, where, name);
}

std::optional<bool> opt_bool(const json& obj, std::string_view where, const char* name) {
  const auto it = obj.find(name);
  if (it == obj.end()) {
    return std::nullopt;
  }
  if (!it->is_boolean()) {
    bad(where, std::string("'") + name + "' is not a boolean");
  }
  return it->get<bool>();
}

void check_hex64(std::string_view where, const char* name, std::string_view value) {
  if (!is_hex64(value)) {
    bad(where, std::string("'") + name + "' does not match ^[0-9a-f]{64}$");
  }
}

void check_date_time(std::string_view where, const char* name, std::string_view value) {
  if (!is_iso8601_date_time(value)) {
    bad(where, std::string("'") + name + "' is not an RFC 3339 date-time");
  }
}

SchemaRev parse_schema_rev(const json& schemas, const char* name) {
  const std::string where = std::string("schemas.") + name;
  const json& rev = required(schemas, "schemas", name);
  if (!rev.is_object()) {
    bad(where, "not an object");
  }
  SchemaRev out;
  const std::uint64_t major = get_uint(rev, where, "major");
  if (major < 1 || major > std::numeric_limits<std::uint32_t>::max()) {
    bad(where, "'major' is outside 1..2^32-1");
  }
  out.major = static_cast<std::uint32_t>(major);
  out.sha256 = get_string(rev, where, "sha256");
  check_hex64(where, "sha256", out.sha256);
  return out;
}

ordered schema_rev_json(const SchemaRev& rev) {
  ordered o;
  o["major"] = rev.major;
  o["sha256"] = rev.sha256;
  return o;
}

}  // namespace

bool is_hex64(std::string_view text) noexcept {
  if (text.size() != 64) {
    return false;
  }
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool is_iso8601_date_time(std::string_view s) noexcept {
  // YYYY-MM-DDThh:mm:ss[.frac](Z|z|±hh:mm); the separators 'T'/'t' as RFC 3339 allows.
  if (s.size() < 20 || !is_digits(s, 0, 4) || s[4] != '-' || !is_digits(s, 5, 2) || s[7] != '-' ||
      !is_digits(s, 8, 2) || (s[10] != 'T' && s[10] != 't') || !is_digits(s, 11, 2) || s[13] != ':' ||
      !is_digits(s, 14, 2) || s[16] != ':' || !is_digits(s, 17, 2)) {
    return false;
  }
  std::size_t i = 19;
  if (s[i] == '.') {
    const std::size_t start = ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
    }
    if (i == start) {
      return false;
    }
  }
  if (i >= s.size()) {
    return false;
  }
  if (s[i] == 'Z' || s[i] == 'z') {
    return i + 1 == s.size();
  }
  if (s[i] != '+' && s[i] != '-') {
    return false;
  }
  return s.size() == i + 6 && is_digits(s, i + 1, 2) && s[i + 3] == ':' && is_digits(s, i + 4, 2);
}

std::string format_iso8601_utc(std::chrono::system_clock::time_point when) {
  const std::time_t t = std::chrono::system_clock::to_time_t(when);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::array<char, 32> buf{};
  const std::size_t n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf.data(), n);
}

Meta parse_meta(std::span<const std::uint8_t> bytes) {
  return parse_meta(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Meta parse_meta(std::string_view text) {
  const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/false);
  if (doc.is_discarded()) {
    bad("document", "not valid JSON");
  }
  if (!doc.is_object()) {
    bad("document", "not a JSON object");
  }
  const std::string_view top = "document";
  if (get_uint(doc, top, "cache_format") != kCacheFormat) {
    bad(top, "'cache_format' is not 1");
  }
  Meta m;
  m.source_hash = get_string(doc, top, "source_hash");
  check_hex64(top, "source_hash", m.source_hash);
  m.source_name = opt_string(doc, top, "source_name");
  m.source_size = opt_uint(doc, top, "source_size");
  m.created = get_string(doc, top, "created");
  check_date_time(top, "created", m.created);
  m.last_used = opt_string(doc, top, "last_used");
  if (m.last_used) {
    check_date_time(top, "last_used", *m.last_used);
  }
  m.ovt_version = get_string(doc, top, "ovt_version");
  const json& schemas = required(doc, top, "schemas");
  if (!schemas.is_object()) {
    bad(top, "'schemas' is not an object");
  }
  m.ovt_model = parse_schema_rev(schemas, "ovt_model");
  m.ovt_container = parse_schema_rev(schemas, "ovt_container");
  m.ovt_metrics = parse_schema_rev(schemas, "ovt_metrics");
  m.codec = get_string(doc, top, "codec");
  bool known = false;
  for (const std::string_view c : kCodecs) {
    known = known || c == m.codec;
  }
  if (!known) {
    bad(top, "'codec' is not one of H264, H265, AV1, VP9, H266, MPEG2, Unknown");
  }
  m.frame_count = get_uint(doc, top, "frame_count");
  m.index_complete = opt_bool(doc, top, "index_complete");
  const json& plugins = required(doc, top, "plugins");
  if (!plugins.is_array()) {
    bad(top, "'plugins' is not an array");
  }
  std::size_t index = 0;
  for (const json& p : plugins) {
    const std::string where = "plugins[" + std::to_string(index++) + "]";
    if (!p.is_object()) {
      bad(where, "not an object");
    }
    PluginFrames pf;
    pf.plugin_id = get_string(p, where, "plugin_id");
    pf.plugin_version = get_string(p, where, "plugin_version");
    if (const auto mask = opt_uint(p, where, "plane_stage_mask")) {
      if (*mask > std::numeric_limits<std::uint32_t>::max()) {
        bad(where, "'plane_stage_mask' exceeds 2^32-1");
      }
      pf.plane_stage_mask = static_cast<std::uint32_t>(*mask);
    }
    pf.frames = get_uint(p, where, "frames");
    m.plugins.push_back(std::move(pf));
  }
  m.planes_cached = opt_bool(doc, top, "planes_cached");
  return m;
}

std::string serialize_meta(const Meta& m) {
  // The members that have a pattern or enum are checked here, so that a record this writer produced always
  // parses (the counts and booleans cannot be wrong by construction).
  auto invalid = [](std::string_view what) {
    throw CacheError(OVT_E_INVALID_ARG, std::string("cache: meta.json: ") + std::string(what));
  };
  if (!is_hex64(m.source_hash)) {
    invalid("source_hash is not 64 lowercase hex digits");
  }
  if (!is_iso8601_date_time(m.created) || (m.last_used && !is_iso8601_date_time(*m.last_used))) {
    invalid("created / last_used is not an RFC 3339 date-time");
  }
  for (const SchemaRev* rev : {&m.ovt_model, &m.ovt_container, &m.ovt_metrics}) {
    if (rev->major < 1 || !is_hex64(rev->sha256)) {
      invalid("schemas.*: major < 1 or sha256 not 64 lowercase hex digits");
    }
  }
  bool known = false;
  for (const std::string_view c : kCodecs) {
    known = known || c == m.codec;
  }
  if (!known) {
    invalid("codec is not in the enum");
  }

  ordered o;
  o["cache_format"] = kCacheFormat;
  o["source_hash"] = m.source_hash;
  if (m.source_name) {
    o["source_name"] = *m.source_name;
  }
  if (m.source_size) {
    o["source_size"] = *m.source_size;
  }
  o["created"] = m.created;
  if (m.last_used) {
    o["last_used"] = *m.last_used;
  }
  o["ovt_version"] = m.ovt_version;
  ordered schemas;
  schemas["ovt_model"] = schema_rev_json(m.ovt_model);
  schemas["ovt_container"] = schema_rev_json(m.ovt_container);
  schemas["ovt_metrics"] = schema_rev_json(m.ovt_metrics);
  o["schemas"] = std::move(schemas);
  o["codec"] = m.codec;
  o["frame_count"] = m.frame_count;
  if (m.index_complete) {
    o["index_complete"] = *m.index_complete;
  }
  ordered plugins = ordered::array();
  for (const PluginFrames& p : m.plugins) {
    ordered pj;
    pj["plugin_id"] = p.plugin_id;
    pj["plugin_version"] = p.plugin_version;
    if (p.plane_stage_mask) {
      pj["plane_stage_mask"] = *p.plane_stage_mask;
    }
    pj["frames"] = p.frames;
    plugins.push_back(std::move(pj));
  }
  o["plugins"] = std::move(plugins);
  if (m.planes_cached) {
    o["planes_cached"] = *m.planes_cached;
  }
  return o.dump(2) + "\n";
}

}  // namespace ovt::cache
